#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
#include "esp_log.h"
#include <Arduino.h>
#include <NimBLEDevice.h>

// watch dog
#include <esp_task_wdt.h>

#include "config.h"
#include "platform.h"
#include "utils.h"
#include "Commands.h"
#include "TreadmillHandler.h"
#include "TreadmillController.h"
#include "NetManager.h"
#include "NetTask.h"
#include "mqttview.h"
#include "board.h"
#if HAS_DIAL_UI
#include "DialUi.h"
#endif

const uint WATCHDOG_TIMEOUT_S = 300;

// Two tasks, two queues.
//
//   loop task (core 1): BLE keepalives, the controller, command execution.
//                       Never touches PubSubClient, WiFi or ArduinoOTA.
//   net task  (core 0): NetManager (WiFi/MQTT/OTA) and every MqttView publish.
//
// Networking is standalone-first: NetManager retries forever in the background,
// so BLE and local control keep running through any network outage — and now a
// blocking socket call can no longer stall the belt's 6 s supervision timeout.
NetManager net(DEFAULT_STA_WIFI_SSID, DEFAULT_STA_WIFI_PASS,
               MQTT_SERVER, MQTT_PORT, MQTT_USER, MQTT_PASS);
MqttView g_mqttView(&net.mqtt());

TreadmillHandler treadmill;
TreadmillController controller(treadmill);

NetTask netTask(net, g_mqttView, treadmill);
PublishQueueObserver g_publishObserver(netTask, treadmill);

#if HAS_DIAL_UI
DialUi dialUi(controller);
#endif

// Last network state pushed to the views; the display will read this too.
NetStatus g_lastNetStatus = NetStatus::WIFI_DOWN;

// Hands one settings publish to the net task. Snapshots go through the
// controller's observer instead.
static void enqueue(PubType type, bool b = false, uint16_t u16 = 0, uint8_t u8 = 0, float f = 0)
{
  PublishItem item;
  item.type = type;
  item.b    = b;
  item.u16  = u16;
  item.u8   = u8;
  item.f    = f;
  netTask.enqueuePublish(item);
}

// Executes commands parsed by the net task. Runs on the loop task, so every
// treadmill/controller call here is on the same task as treadmill.handle().
static void drainCommands()
{
  Command cmd;
  while (netTask.receiveCommand(cmd))
  {
    switch (cmd.type)
    {
    case CmdType::NONE:
      break;

    case CmdType::START:
      controller.start();
      break;

    case CmdType::PAUSE:
      controller.pause();
      break;

    case CmdType::STOP:
      controller.stop();
      break;

    case CmdType::SET_SPEED_MPH:
      controller.setSpeedMph(cmd.f);
      break;

    case CmdType::CONNECT:
      controller.requestConnect();
      // Connecting re-arms auto-reconnect inside the link — echo it so HA's
      // switch matches.
      enqueue(PubType::AUTO_RECONNECT, treadmill.getAutoReconnect());
      break;

    case CmdType::DISCONNECT:
      // A refused disconnect (belt still active) re-notifies observers with the
      // real snapshot, which snaps HA's switch back to ON.
      controller.requestDisconnect();
      enqueue(PubType::AUTO_RECONNECT, treadmill.getAutoReconnect());
      break;

    case CmdType::SET_AUTO_RECONNECT:
      treadmill.setAutoReconnect(cmd.b);
      enqueue(PubType::AUTO_RECONNECT, cmd.b);
      break;

    case CmdType::SET_IDLE_MINS:
      treadmill.setIdleDisconnectMins(cmd.u16);
      enqueue(PubType::IDLE_MINS, false, cmd.u16);
      break;

    case CmdType::SET_PAUSE_MINS:
      treadmill.setPauseTimeoutMins(cmd.u16);
      enqueue(PubType::PAUSE_MINS, false, cmd.u16);
      break;

    case CmdType::SET_START_SPEED:
      treadmill.setStartSpeedMph(cmd.f);
      enqueue(PubType::START_SPEED, false, 0, 0, treadmill.getStartSpeedMph());
      break;

    case CmdType::TOGGLE_CALIBRATION:
      treadmill.toggleCalibration();
      enqueue(PubType::CALIB_COUNT, false, 0, treadmill.getCalibrationPointCount());
      break;

    case CmdType::RESTORE_TOTALS:
    {
      // Fields the payload omitted keep their current NVS value, so individual
      // totals can be restored without disturbing the others.
      const float    distKm      = cmd.totals.has[0] ? cmd.totals.distKm      : treadmill.getTotalDistanceKm();
      const uint32_t steps       = cmd.totals.has[1] ? cmd.totals.steps       : treadmill.getTotalSteps();
      const uint32_t calories    = cmd.totals.has[2] ? cmd.totals.calories    : treadmill.getTotalCalories();
      const uint32_t durationSec = cmd.totals.has[3] ? cmd.totals.durationSec : treadmill.getTotalDurationSec();
      treadmill.restoreTotals(distKm, steps, calories, durationSec);
      controller.publish();
      log_i("Totals restored and published to MQTT.");
      break;
    }

    case CmdType::RESTORE_CALIBRATION:
      treadmill.restoreCalibrationPoints(cmd.calib.pts, cmd.calib.n);
      enqueue(PubType::CALIB_COUNT, false, 0, treadmill.getCalibrationPointCount());
      log_i("Calibration restored: %u points written to NVS.", cmd.calib.n);
      break;

    case CmdType::HA_ONLINE:
      enqueue(PubType::FULL_RESYNC);
      break;
    }
  }
}

void setup()
{
#if HAS_DIAL_UI
  // Must run first: M5Unified owns display/I2C/Serial init on the Dial.
  dialUi.begin();
#endif

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
  esp_task_wdt_add(NULL); // watch the loop task only — the net task is allowed
                          // to block on a socket for as long as it takes

#if !HAS_DIAL_UI
  // On the Dial, M5Unified opens Serial itself inside dialUi.begin() above.
  Serial.begin(115200);
#endif

#if HAS_STATUS_LED
  pinMode(LED_BLE_PIN, OUTPUT);
  digitalWrite(LED_BLE_PIN, LOW); // active-high: off until BLE connects
#endif

  // BLE first: the belt must work with no network at all.
  NimBLEAddress targetAddress(std::string(TARGET_ADDRESS), BLE_ADDR_PUBLIC);
  treadmill.begin(targetAddress);

  log_i("Starting BLE Client...");
  NimBLEDevice::init("PaceKeeper");

  controller.addObserver(g_publishObserver);
#if HAS_DIAL_UI
  controller.addObserver(dialUi);
#endif

  // Starts the net task, which brings up WiFi/MQTT/OTA on its own.
  netTask.begin(composeClientID().c_str());
}

void loop()
{
  // reset watchdog, important to be called once each loop.
  esp_task_wdt_reset();

  const uint32_t now = millis();

  // BLE first, always — keepalives must never stall.
  if (treadmill.handle())
  {
    controller.publish();
  }

  drainCommands();
  controller.tick(now);
#if HAS_DIAL_UI
  dialUi.tick(now);
#endif

  const NetStatus netStatus = netTask.status();
  if (netStatus != g_lastNetStatus)
  {
    g_lastNetStatus = netStatus;
    controller.publishNetStatus(netStatus);
  }

  delay(1); // yield to the idle task
}
