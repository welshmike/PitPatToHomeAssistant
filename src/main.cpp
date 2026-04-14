#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
#include "esp_log.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <LittleFS.h>

#include <WiFi.h>
#include <mdns.h>
// watch dog
#include <esp_task_wdt.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <MqttDevice.h>

// #include <WiFiUdp.h>
#include <ArduinoOTA.h>

#include "config.h"
#include "platform.h"
#include "TreadmillHandler.h"
#include "mqttview.h"

const uint WATCHDOG_TIMEOUT_S = 300;
const uint WIFI_DISCONNECT_FORCED_RESTART_S = 60;

WiFiClient net;
PubSubClient client(net);
MqttView g_mqttView(&client);

bool g_wifiConnected = false;
bool g_mqttConnected = false;
unsigned long g_lastWifiConnect = 0;

String g_bssid = "";

TreadmillHandler treadmill;

// Recovery topic — allows restoring NVS totals after accidental "Erase Flash".
// Topic: pacekeeper-{mac}/restore-totals/set
// Payload: JSON e.g. {"dist_km":12.34,"steps":15000,"calories":200,"duration_sec":5400}
// Publish from HA Developer Tools → MQTT. All fields are optional (omit to keep current).
char g_restoreTotalsTopic[64];

bool connectToMqtt()
{
  if (client.connected())
  {
    return true;
  }

  log_i("Connecting to MQTT...");
  if (strlen(MQTT_USER) == 0)
  {
    if (!client.connect(composeClientID().c_str()))
    {
      return false;
    }
  }
  else
  {
    if (!client.connect(composeClientID().c_str(), MQTT_USER, MQTT_PASS))
    {
      return false;
    }
  }

  client.subscribe(g_mqttView.getSpeed().getCommandTopic(), 1);
  client.subscribe(g_mqttView.getStartButton().getCommandTopic(), 1);
  client.subscribe(g_mqttView.getPauseButton().getCommandTopic(), 1);
  client.subscribe(g_mqttView.getStopButton().getCommandTopic(), 1);
  client.subscribe(g_mqttView.getAutoReconnectSwitch().getCommandTopic(), 1);
  client.subscribe(g_mqttView.getStepLengthNumber().getCommandTopic(), 1);
  client.subscribe(g_mqttView.getConnectSwitch().getCommandTopic(), 1);
  client.subscribe(g_mqttView.getCalibrate20StepsButton().getCommandTopic(), 1);

  // Recovery topic — build once and subscribe. Topic is logged at INFO so you can
  // find it in the serial monitor if you need to publish a restore payload.
  snprintf(g_restoreTotalsTopic, sizeof(g_restoreTotalsTopic),
           "%s/restore-totals/set", composeClientID().c_str());
  client.subscribe(g_restoreTotalsTopic);
  log_i("Restore-totals topic: %s", g_restoreTotalsTopic);

  client.subscribe(HOMEASSISTANT_STATUS_TOPIC);
  client.subscribe(HOMEASSISTANT_STATUS_TOPIC_ALT);

  g_mqttView.publishAllConfigs();
  delay(200); // give mqtt broker some time to process all config messages
  g_mqttView.publishState(treadmill.getLastData());
  g_mqttView.publishAutoReconnectSetting(treadmill.getAutoReconnect());
  g_mqttView.publishStepLengthSetting(treadmill.getStepLength());

  return true;
}

bool connectToWifi()
{
  return WiFi.status() == WL_CONNECTED;
}

void handleSpeedCommand(byte *payload, unsigned int length)
{
  float data = atof((char *)payload);
  // HA slider sends mph. Device encoding: mph * 1600.
  uint16_t speed = (uint16_t)(data * 1600.0f);
  log_i("Setting speed to %.2f mph (raw=%u)", data, speed);

  if (speed <= 100)  // below ~0.1 km/h → stop
  {
    treadmill.stop();
    return;
  }
  if (speed > 6080)  // cap at 3.8 mph (device max)
  {
    speed = 6080;
  }
  treadmill.setSpeed(speed);
  // Immediately reflect the new commanded speed in HA — don't wait for the
  // next BLE notification from the belt (can be 3-5s). speed_feedback still
  // lags until the belt confirms, but the command value updates instantly.
  TreadMillData updated = treadmill.getLastData();
  updated.speedCmd = roundf((speed / 1600.0f) * 10) / 10.0f;
  g_mqttView.publishState(updated);
}

void handleButtonCommand(byte *payload, unsigned int length, const char* action)
{
  String command = String((char *)payload).substring(0, length);
  command.trim();
  if (command.equalsIgnoreCase("press"))
  {
    if (strcmp(action, "start") == 0) {
      log_i("Start command received");
      treadmill.start();
      TreadMillData updated = treadmill.getLastData();
      updated.status = TreadMillData::COUNTDOWN;
      g_mqttView.publishState(updated);
    } else if (strcmp(action, "pause") == 0) {
      log_i("Pause command received");
      treadmill.pause();
      TreadMillData updated = treadmill.getLastData();
      updated.status = TreadMillData::PAUSED;
      g_mqttView.publishState(updated);
    } else if (strcmp(action, "stop") == 0) {
      log_i("Stop command received");
      treadmill.stop();
      TreadMillData updated = treadmill.getLastData();
      updated.status = TreadMillData::STOPPED;
      g_mqttView.publishState(updated);
    }
  }
}

void handleAutoReconnectCommand(byte *payload, unsigned int length)
{
  String command = String((char *)payload).substring(0, length);
  command.trim();
  log_i("Auto Reconnect command received: %s", command.c_str());
  if (command.equalsIgnoreCase(g_mqttView.getAutoReconnectSwitch().getOnState()))
  {
    treadmill.setAutoReconnect(true);
    g_mqttView.publishAutoReconnectSetting(true);
  }
  else if (command.equalsIgnoreCase(g_mqttView.getAutoReconnectSwitch().getOffState()))
  {
    treadmill.setAutoReconnect(false);
    g_mqttView.publishAutoReconnectSetting(false);
  }
}

void handleConnectSwitchCommand(byte *payload, unsigned int length)
{
  String command = String((char *)payload).substring(0, length);
  command.trim();
  bool wasConnected = treadmill.isConnected();
  bool actionTaken = treadmill.toggleConnection();
  // Sync auto-reconnect switch in HA to reflect the new state
  g_mqttView.publishAutoReconnectSetting(treadmill.getAutoReconnect());
  if (wasConnected && actionTaken)
  {
    // Disconnect initiated — onDisconnect() will set DISCONNECTED + m_newDataAvailable,
    // but publish optimistically now so HA doesn't wait for that callback (~42ms delay)
    TreadMillData disconnected = treadmill.getLastData();
    disconnected.status = TreadMillData::DISCONNECTED;
    g_mqttView.publishState(disconnected);
  }
  else if (wasConnected && !actionTaken)
  {
    // Blocked by safety check (belt is active) —
    // re-publish current state so HA snaps the switch back to ON
    log_w("Connect switch OFF blocked — re-publishing current state to correct HA");
    g_mqttView.publishState(treadmill.getLastData());
  }
  // If !wasConnected: connect requested, BLE callback will publish ON when established
}

void handleStepLengthCommand(byte *payload, unsigned int length)
{
  float stepM = atof((char *)payload);
  if (stepM >= 0.10f && stepM <= 0.80f)
  {
    log_i("Setting step length to %.2f m", stepM);
    treadmill.setStepLength(stepM);
    g_mqttView.publishStepLengthSetting(stepM);
  }
  else
  {
    log_w("Step length %.2f out of range [0.10, 0.80] - ignoring", stepM);
  }
}

void handleCalibrate20StepsCommand(byte *payload, unsigned int length)
{
  log_i("Calibration 20 Steps button pressed via MQTT");
  treadmill.toggleCalibration();
  g_mqttView.publishCalibrationPoints(treadmill.getCalibrationPoints(), treadmill.getCalibrationPointCount());
}

void handleRestoreTotalsCommand(byte *payload, unsigned int length)
{
  String payloadStr = String((char *)payload).substring(0, length);
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payloadStr);
  if (err)
  {
    log_e("restore-totals: JSON parse failed (%s) — payload: %s",
          err.c_str(), payloadStr.c_str());
  }
  else
  {
    // Read each field; fall back to the current NVS value if the field is absent
    // so you can restore individual fields without affecting others.
    float    distKm      = doc["dist_km"]      | treadmill.getTotalDistanceKm();
    uint32_t steps       = doc["steps"]        | treadmill.getTotalSteps();
    uint32_t calories    = doc["calories"]      | treadmill.getTotalCalories();
    uint32_t durationSec = doc["duration_sec"] | treadmill.getTotalDurationSec();

    treadmill.restoreTotals(distKm, steps, calories, durationSec);

    // Publish immediately so HA sensors reflect the restored values right away
    g_mqttView.publishState(treadmill.getLastData());
    log_i("Totals restored and published to MQTT.");
  }
}

void handleHAStatus(byte *payload, unsigned int length)
{
  if (strncmp((char *)payload, "online", length) == 0)
  {
    g_mqttView.publishAllConfigs();
    delay(200); // give mqtt broker some time to process all config messages
    g_mqttView.publishState(treadmill.getLastData());
    g_mqttView.publishAutoReconnectSetting(treadmill.getAutoReconnect());
    g_mqttView.publishStepLengthSetting(treadmill.getStepLength());
    g_mqttView.publishCalibrationPoints(treadmill.getCalibrationPoints(), treadmill.getCalibrationPointCount());
  }
}

void callback(char *topic, byte *payload, unsigned int length)
{
  log_i("Message arrived [%s]", topic);
  for (unsigned int i = 0; i < length; i++)
  {
    Serial.print((char)payload[i]);
  }
  Serial.println();

  if (strcmp(topic, g_mqttView.getSpeed().getCommandTopic()) == 0) {
    handleSpeedCommand(payload, length);
  } else if (strcmp(topic, g_mqttView.getStartButton().getCommandTopic()) == 0) {
    handleButtonCommand(payload, length, "start");
  } else if (strcmp(topic, g_mqttView.getPauseButton().getCommandTopic()) == 0) {
    handleButtonCommand(payload, length, "pause");
  } else if (strcmp(topic, g_mqttView.getStopButton().getCommandTopic()) == 0) {
    handleButtonCommand(payload, length, "stop");
  } else if (strcmp(topic, g_mqttView.getAutoReconnectSwitch().getCommandTopic()) == 0) {
    handleAutoReconnectCommand(payload, length);
  } else if (strcmp(topic, g_mqttView.getConnectSwitch().getCommandTopic()) == 0) {
    handleConnectSwitchCommand(payload, length);
  } else if (strcmp(topic, g_mqttView.getStepLengthNumber().getCommandTopic()) == 0) {
    handleStepLengthCommand(payload, length);
  } else if (strcmp(topic, g_mqttView.getCalibrate20StepsButton().getCommandTopic()) == 0) {
    handleCalibrate20StepsCommand(payload, length);
  } else if (strcmp(topic, g_restoreTotalsTopic) == 0) {
    handleRestoreTotalsCommand(payload, length);
  } else if (strcmp(topic, HOMEASSISTANT_STATUS_TOPIC) == 0 ||
             strcmp(topic, HOMEASSISTANT_STATUS_TOPIC_ALT) == 0) {
    handleHAStatus(payload, length);
  }
}

void setup()
{
  // initialize watchdog
  // ESP-IDF 5.x (Arduino-ESP32 3.x) changed the WDT API to use a config struct
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t wdt_config = {
      .timeout_ms = WATCHDOG_TIMEOUT_S * 1000,
      .idle_core_mask = 0,
      .trigger_panic = true,
  };
  esp_task_wdt_reconfigure(&wdt_config);
#else
  esp_task_wdt_init(WATCHDOG_TIMEOUT_S, true); // enable panic so ESP32 restarts
#endif
  esp_task_wdt_add(NULL);                      // add current thread to WDT watch

  Serial.begin(115200);

  // Initialize LittleFS for packet logging
  if (LittleFS.begin(true)) {  // format on first use
    log_i("LittleFS mounted successfully");
    // Create CSV header for packet log
    File logFile = LittleFS.open("/packets.csv", "w");
    if (logFile) {
      logFile.println("millis,length,type,b7,b8,b9,b10,b14,b23,b24,b25,b26,b27,b28");
      logFile.close();
      log_i("Packet log initialized");
    }
  } else {
    log_e("LittleFS mount failed");
  }

  WiFi.setHostname(composeClientID().c_str());
  WiFi.mode(WIFI_STA);

  // select the AP with the strongest signal
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);

  WiFi.begin(DEFAULT_STA_WIFI_SSID, DEFAULT_STA_WIFI_PASS);

  log_i("Connecting to wifi...");
  while (!connectToWifi())
  {
    log_d(".");
    delay(500);
  }
  g_wifiConnected = true;
  g_lastWifiConnect = millis();
  log_i("Connected to SSID: %s", DEFAULT_STA_WIFI_SSID);
  log_i("IP address: %s", WiFi.localIP().toString().c_str());

  char configUrl[256];
  snprintf(configUrl, sizeof(configUrl), "http://%s/", WiFi.localIP().toString().c_str());
  g_mqttView.getDevice().setConfigurationUrl(configUrl);
  client.setBufferSize(1024);
  client.setServer(MQTT_SERVER, MQTT_PORT);
  client.setCallback(callback);

  NimBLEAddress targetAddress(std::string(TARGET_ADDRESS), BLE_ADDR_PUBLIC);
  treadmill.begin(targetAddress);

  treadmill.setCallback([](const TreadMillData &data)
                        {
    log_d("Speed: %.2f km/h, Distance: %.2f %d", data.speedCmd, data.distanceKm, data.status);
    g_mqttView.publishState(data); });

  log_i("Starting BLE Client...");
  NimBLEDevice::init("PaceKeeper");

  ArduinoOTA.onStart([]()
                     {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else { // U_FS
      type = "filesystem";
    }

    // NOTE: if updating FS this would be the place to unmount FS using FS.end()
    log_i("Start updating %s", type.c_str()); });
  ArduinoOTA.onEnd([]()
                   { log_i("End"); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
                        {
// reset watchdog during update
#ifdef ESP32
            esp_task_wdt_reset();
#endif
        log_i("Progress: %u%%\r", (progress / (total / 100))); });
  ArduinoOTA.onError([](ota_error_t error)
                     {
    log_e("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      log_e("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      log_e("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      log_e("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      log_e("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      log_e("End Failed");
    } });
  ArduinoOTA.begin();
}

void loop()
{
  // reset watchdog, important to be called once each loop.
  esp_task_wdt_reset();

  bool wifiConnected = connectToWifi();
  if (!wifiConnected)
  {
    if (g_wifiConnected)
    {
    }
    if (millis() - g_lastWifiConnect > WIFI_DISCONNECT_FORCED_RESTART_S * 1000)
    {
      log_w("Wifi could not connect in time, will force a restart");
      ESP.restart();
    }
    g_wifiConnected = false;
    g_mqttConnected = false;
    delay(1000);
    return;
  }
  g_wifiConnected = true;
  g_lastWifiConnect = millis();

  ArduinoOTA.handle();

  bool mqttConnected = connectToMqtt();
  if (!mqttConnected)
  {
    if (g_mqttConnected)
    {
      // we switched to disconnected
    }
    g_mqttConnected = false;
    delay(1000);
    return;
  }
  if (!g_mqttConnected)
  {
    // now we are successfully reconnected and publish our counters
    g_bssid = WiFi.BSSIDstr();
    // g_mqttView.publishDiagnostics(g_settings, g_bssid.c_str());
  }
  g_mqttConnected = true;

  client.loop();
  treadmill.handle();

  // Handle serial commands for packet log
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd == "dump") {
      File logFile = LittleFS.open("/packets.csv", "r");
      if (logFile) {
        Serial.println("=== Packet Log ===");
        while (logFile.available()) {
          Serial.write(logFile.read());
        }
        logFile.close();
        Serial.println("=== End Log ===");
      } else {
        Serial.println("No log file found");
      }
    }
    else if (cmd == "clear") {
      File logFile = LittleFS.open("/packets.csv", "w");
      if (logFile) {
        logFile.println("millis,length,type,b7,b8,b9,b10,b14,b23,b24,b25,b26,b27,b28");
        logFile.close();
        Serial.println("Log cleared");
      }
    }
  }

  // Notifications are handled in the callback
  delay(100);
}
