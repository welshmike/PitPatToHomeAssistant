#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
#include "esp_log.h"
#include <Arduino.h>
#include <NimBLEDevice.h>

#include <WiFi.h>
// watch dog
#include <esp_task_wdt.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <MqttDevice.h>

#include "config.h"
#include "platform.h"
#include "TreadmillHandler.h"
#include "NetManager.h"
#include "mqttview.h"

const uint WATCHDOG_TIMEOUT_S = 300;

// Networking is standalone-first: NetManager connects WiFi/MQTT in the
// background and retries forever, so BLE and local control keep running
// through any network outage.
NetManager net(DEFAULT_STA_WIFI_SSID, DEFAULT_STA_WIFI_PASS,
               MQTT_SERVER, MQTT_PORT, MQTT_USER, MQTT_PASS);
MqttView g_mqttView(&net.mqtt());

TreadmillHandler treadmill;

// Recovery topic — allows restoring NVS totals after accidental "Erase Flash".
// Topic: pacekeeper-{mac}/restore-totals/set
// Payload: JSON e.g. {"dist_km":12.34,"steps":15000,"calories":200,"duration_sec":5400}
// Publish from HA Developer Tools → MQTT. All fields are optional (omit to keep current).
char g_restoreTotalsTopic[64];

// Recovery topic — allows restoring calibration points after "Erase Flash".
// Topic: pacekeeper-{mac}/restore-calibration/set
// Payload: JSON array e.g. [{"mph":1.0,"spm":68.8},{"mph":1.5,"spm":82.0}]
char g_restoreCalibTopic[64];


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
  bool isConnected = treadmill.isConnected();
  bool wantConnect = command.equalsIgnoreCase(g_mqttView.getConnectSwitch().getOnState());

  if (wantConnect == isConnected)
  {
    // Already in the requested state — re-publish to keep HA in sync.
    g_mqttView.publishState(treadmill.getLastData());
    return;
  }

  bool actionTaken = treadmill.toggleConnection();
  g_mqttView.publishAutoReconnectSetting(treadmill.getAutoReconnect());
  if (isConnected && actionTaken)
  {
    // Disconnect initiated — publish optimistically now so HA doesn't wait for the callback.
    TreadMillData disconnected = treadmill.getLastData();
    disconnected.status = TreadMillData::DISCONNECTED;
    g_mqttView.publishState(disconnected);
  }
  else if (isConnected && !actionTaken)
  {
    // Blocked by safety check (belt is active) — snap switch back to ON.
    log_w("Connect switch OFF blocked — re-publishing current state to correct HA");
    g_mqttView.publishState(treadmill.getLastData());
  }
  // If !isConnected: connect requested, BLE callback will publish ON when established.
}

void handleIdleDisconnectCommand(byte *payload, unsigned int length)
{
  String command = String((char *)payload).substring(0, length);
  command.trim();
  uint16_t mins = (uint16_t)command.toInt();
  log_i("Idle disconnect setting received: %u min", mins);
  treadmill.setIdleDisconnectMins(mins);
  g_mqttView.publishIdleDisconnectSetting(mins);
}

void handlePauseTimeoutCommand(byte *payload, unsigned int length)
{
  String command = String((char *)payload).substring(0, length);
  command.trim();
  uint16_t mins = (uint16_t)command.toInt();
  log_i("Pause timeout setting received: %u min", mins);
  treadmill.setPauseTimeoutMins(mins);
  g_mqttView.publishPauseTimeoutSetting(mins);
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

void handleRestoreCalibrationCommand(byte *payload, unsigned int length)
{
  String payloadStr = String((char *)payload).substring(0, length);
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payloadStr);
  if (err)
  {
    log_e("restore-calibration: JSON parse failed (%s) — payload: %s",
          err.c_str(), payloadStr.c_str());
    return;
  }
  JsonArray arr = doc.as<JsonArray>();
  if (arr.isNull() || arr.size() == 0 || arr.size() > 10)
  {
    log_e("restore-calibration: expected JSON array of 1–10 points, got: %s", payloadStr.c_str());
    return;
  }
  CalibrationPoint pts[10];
  uint8_t n = 0;
  for (JsonObject obj : arr)
  {
    pts[n].speedMph = obj["mph"];
    pts[n].spm      = obj["spm"];
    n++;
  }
  treadmill.restoreCalibrationPoints(pts, n);
  g_mqttView.publishCalibrationPoints(treadmill.getCalibrationPoints(), treadmill.getCalibrationPointCount());
  log_i("Calibration restored: %u points written to NVS.", n);
}

void handleHAStatus(byte *payload, unsigned int length)
{
  if (strncmp((char *)payload, "online", length) == 0)
  {
    g_mqttView.publishAllConfigs();
    delay(200); // give mqtt broker some time to process all config messages
    g_mqttView.publishState(treadmill.getLastData());
    g_mqttView.publishAutoReconnectSetting(treadmill.getAutoReconnect());
    g_mqttView.publishIdleDisconnectSetting(treadmill.getIdleDisconnectMins());
    g_mqttView.publishPauseTimeoutSetting(treadmill.getPauseTimeoutMins());
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
  } else if (strcmp(topic, g_mqttView.getCalibrate20StepsButton().getCommandTopic()) == 0) {
    handleCalibrate20StepsCommand(payload, length);
  } else if (strcmp(topic, g_mqttView.getIdleDisconnectNumber().getCommandTopic()) == 0) {
    handleIdleDisconnectCommand(payload, length);
  } else if (strcmp(topic, g_mqttView.getPauseTimeoutNumber().getCommandTopic()) == 0) {
    handlePauseTimeoutCommand(payload, length);
  } else if (strcmp(topic, g_restoreTotalsTopic) == 0) {
    handleRestoreTotalsCommand(payload, length);
  } else if (strcmp(topic, g_restoreCalibTopic) == 0) {
    handleRestoreCalibrationCommand(payload, length);
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

#if HAS_STATUS_LED
  pinMode(LED_BLE_PIN, OUTPUT);
  digitalWrite(LED_BLE_PIN, LOW); // active-high: off until BLE connects
#endif

  // BLE first: the belt must work with no network at all.
  NimBLEAddress targetAddress(std::string(TARGET_ADDRESS), BLE_ADDR_PUBLIC);
  treadmill.begin(targetAddress);

  treadmill.setCallback([](const TreadMillData &data)
                        {
    log_d("Speed: %.2f km/h, Distance: %.2f %d", data.speedCmd, data.distanceKm, data.status);
    // Silently drop state updates while MQTT is down — publishing would fail
    // (and log an error) every 200ms during an outage.
    if (net.mqttUp())
    {
      g_mqttView.publishState(data);
    } });

  log_i("Starting BLE Client...");
  NimBLEDevice::init("PaceKeeper");

  net.setMqttCallback(callback);

  // Everything that has to happen each time the MQTT session comes up.
  net.onMqttConnected([]()
                      {
    PubSubClient &client = net.mqtt();

    char configUrl[256];
    snprintf(configUrl, sizeof(configUrl), "http://%s/", WiFi.localIP().toString().c_str());
    g_mqttView.getDevice().setConfigurationUrl(configUrl);

    client.subscribe(g_mqttView.getSpeed().getCommandTopic(), 1);
    client.subscribe(g_mqttView.getStartButton().getCommandTopic(), 1);
    client.subscribe(g_mqttView.getPauseButton().getCommandTopic(), 1);
    client.subscribe(g_mqttView.getStopButton().getCommandTopic(), 1);
    client.subscribe(g_mqttView.getAutoReconnectSwitch().getCommandTopic(), 1);
    client.subscribe(g_mqttView.getConnectSwitch().getCommandTopic(), 1);
    client.subscribe(g_mqttView.getCalibrate20StepsButton().getCommandTopic(), 1);
    client.subscribe(g_mqttView.getIdleDisconnectNumber().getCommandTopic(), 1);
    client.subscribe(g_mqttView.getPauseTimeoutNumber().getCommandTopic(), 1);

    // Recovery topic — build once and subscribe. Topic is logged at INFO so you can
    // find it in the serial monitor if you need to publish a restore payload.
    snprintf(g_restoreTotalsTopic, sizeof(g_restoreTotalsTopic),
             "%s/restore-totals/set", composeClientID().c_str());
    client.subscribe(g_restoreTotalsTopic);
    log_i("Restore-totals topic: %s", g_restoreTotalsTopic);

    snprintf(g_restoreCalibTopic, sizeof(g_restoreCalibTopic),
             "%s/restore-calibration/set", composeClientID().c_str());
    client.subscribe(g_restoreCalibTopic);
    log_i("Restore-calibration topic: %s", g_restoreCalibTopic);

    client.subscribe(HOMEASSISTANT_STATUS_TOPIC);
    client.subscribe(HOMEASSISTANT_STATUS_TOPIC_ALT);

    g_mqttView.publishAllConfigs();
    delay(200); // give mqtt broker some time to process all config messages
    g_mqttView.publishState(treadmill.getLastData());
    g_mqttView.publishAutoReconnectSetting(treadmill.getAutoReconnect());
    g_mqttView.publishIdleDisconnectSetting(treadmill.getIdleDisconnectMins());
    g_mqttView.publishPauseTimeoutSetting(treadmill.getPauseTimeoutMins()); });

  // Non-blocking: WiFi/MQTT come up in the background, with backoff, forever.
  net.begin(composeClientID().c_str());
}

void loop()
{
  // reset watchdog, important to be called once each loop.
  esp_task_wdt_reset();

  const uint32_t now = millis();
  treadmill.handle(); // BLE first, always — keepalives must never stall
  net.tick(now);
  delay(1); // yield to the idle task
}
