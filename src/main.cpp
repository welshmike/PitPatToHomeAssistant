#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
#include "esp_log.h"
#include <Arduino.h>
#include <NimBLEDevice.h>

// watch dog
#include <esp_task_wdt.h>

#include "config.h"
#include <esp_coexist.h>
#include "platform.h"
#include "utils.h"
#include "Commands.h"
#include "TreadmillHandler.h"
#include "TreadmillController.h"
#include "NetManager.h"
#include "NetTask.h"
#include "RemoteLog.h"
#include "mqttview.h"
#include "board.h"
#include "TimeService.h"
#if HAS_DIAL_UI
#include "DialUi.h"
#endif

// -DUSE_ESP_IDF_LOG (spec 4.14) makes log_x() expand to
// ESP_LOG_LEVEL_LOCAL(..., TAG, ...); esp32-hal-log.h has no default TAG.
static const char *TAG = "App";

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

// NTP/RTC wall clock for the Dial's clock card (compiles to a no-op RTC on
// the DevKit — see TimeService.h).
TimeService timeService;

#if HAS_DIAL_UI
// DialUi binds netTask.flights()/netTask.lights() (references to NetTask's
// own service members — safe here, before netTask.begin() runs in setup()
// below; see NetTask::flights()/lights() and FlightsService's class comment)
// and keeps netTask itself only to enqueue outgoing light commands.
DialUi dialUi(controller, timeService, netTask);
#endif

// Last network state pushed to the views; the display will read this too.
NetStatus g_lastNetStatus = NetStatus::WIFI_DOWN;

// BLE connect hold (spec 4.15). WiFi and BLE share one antenna: a connect that
// lands on top of WiFi association or MQTT's discovery/resync burst loses
// service discovery, and the disconnect() that follows sends the Q1 into its
// kicking phase. So background connects wait for the air to be quiet.
//   - the first kPostBootHoldMs after boot, while WiFi/MQTT are still coming up;
//   - kPostMqttHoldMs after each transition into MQTT_UP (the discovery burst).
// Never held when WiFi is simply down (standalone Dial) or after the boot
// window, so a Dial with no network still connects within seconds.
constexpr uint32_t kPostMqttHoldMs = 6000;
constexpr uint32_t kPostBootHoldMs = 30000;

// A loop() iteration longer than this is logged (spec 4.16). Orders of
// magnitude above a normal pass (single-digit ms) and far below the 300 s task
// watchdog, so a freeze leaves evidence long before it becomes a reset. The one
// routine pass that can trip it is a connect whose service discovery runs all 8
// retries (2 s inside treadmill.handle()), which is worth seeing anyway.
constexpr uint32_t kLoopStallMs = 3000; // above the 2 s worst-case GATT discovery loop, so only real freezes log

// millis() of the last transition into MQTT_UP; 0 while not MQTT_UP.
uint32_t g_mqttUpSinceMs = 0;

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

    case CmdType::SET_FLIGHTS_AUTO_SHOW:
      treadmill.setFlightsAutoShow(cmd.b);
#if HAS_DIAL_UI
      dialUi.setFlightsAutoShow(cmd.b);
#endif
      enqueue(PubType::FLIGHTS_AUTO_SHOW, cmd.b);
      break;

    case CmdType::SET_CALENDAR_NUDGE:
      treadmill.setCalendarNudge(cmd.b);
      // Task 7: dialUi.setCalendarNudge(cmd.b);
      enqueue(PubType::CALENDAR_NUDGE, cmd.b);
      break;

    case CmdType::SET_CALENDAR_LEAD:
      treadmill.setCalendarLeadMin(cmd.u16);
      // Task 7: dialUi.setCalendarLeadMin(cmd.u16);
      enqueue(PubType::CALENDAR_LEAD, false, cmd.u16);
      break;

    case CmdType::SET_CALENDAR_STAY:
      treadmill.setCalendarStayMin(cmd.u16);
      // Task 7: dialUi.setCalendarStayMin(cmd.u16);
      enqueue(PubType::CALENDAR_STAY, false, cmd.u16);
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
  // Very first statement (spec 4.14): from here on every log line — ours, the
  // IDF's — is also queued for MQTT, so the boot story is captured even though
  // nothing has opened Serial yet. The net task publishes the queue once MQTT
  // is up; the line below therefore reaches the broker even if its serial copy
  // is lost to the not-yet-initialised port.
  RemoteLog::begin();
  log_w("boot: reset=%s build=%s", RemoteLog::resetReasonName(), RemoteLog::buildStamp());

#if HAS_DIAL_UI
  // Must run first after that: M5Unified owns display/I2C/Serial init on the Dial.
  dialUi.begin();
#endif

  // Seeds the system clock from the Dial's RTC (no-op on the DevKit, which
  // has none) so the clock card reads correctly before WiFi/NTP.
  timeService.begin();

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

#if HAS_DIAL_UI
  // treadmill.begin() has just loaded the persisted settings from NVS, so the
  // Dial's auto-show state machine starts from the stored value (spec 4.11)
  // rather than its own default — HA gets the same value from fullResync().
  dialUi.setFlightsAutoShow(treadmill.getFlightsAutoShow());
#endif

  log_i("Starting BLE Client...");
  NimBLEDevice::init("PaceKeeper");
  // WiFi and BLE share the radio; the belt heartbeat matters more than
  // background HTTPS. Bias the coexistence arbiter towards Bluetooth.
  esp_coex_preference_set(ESP_COEX_PREFER_BT);

  controller.addObserver(g_publishObserver);
#if HAS_DIAL_UI
  controller.addObserver(dialUi);
#endif

  // Starts the net task, which brings up WiFi/MQTT/OTA on its own.
  netTask.begin(composeClientID().c_str());
}

void loop()
{
  // Stall detector (spec 4.16). The task watchdog only fires after
  // WATCHDOG_TIMEOUT_S (300 s), by which time the Dial is already resetting;
  // a loop that takes seconds rather than milliseconds is the early symptom,
  // and this is the log line that survives in the RTC ring if the next one
  // never comes. Wrap-safe: unsigned subtraction of two millis() readings.
  const uint32_t loopStartMs = millis();

  // reset watchdog, important to be called once each loop.
  esp_task_wdt_reset();
  {
    // Heap diagnostics every 15 s: free/min-free heap plus BLE link state.
    static uint32_t s_lastHeapLog = 0;
    const uint32_t nowDiag = millis();
    if ((uint32_t)(nowDiag - s_lastHeapLog) >= 15000)
    {
      s_lastHeapLog = nowDiag;
      log_i("Heap: free=%u minFree=%u largestBlock=%u ble=%d",
            (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
            (unsigned)ESP.getMaxAllocHeap(), (int)treadmill.isConnected());
    }
  }

  const uint32_t now = millis();

  // BLE first, always — keepalives must never stall.
  if (treadmill.handle())
  {
    controller.publish();
  }

  drainCommands();
  controller.tick(now);
  timeService.tick(now);
#if HAS_DIAL_UI
  dialUi.tick(now);
#endif

  const NetStatus netStatus = netTask.status();
  if (netStatus != g_lastNetStatus)
  {
    // Fires once, on the transition into WIFI_UP or better (not on every
    // later state change, e.g. WIFI_UP -> MQTT_CONNECTING) — onWifiUp()
    // itself is idempotent too, so this is a belt-and-braces guard.
    if (netStatus >= NetStatus::WIFI_UP && g_lastNetStatus < NetStatus::WIFI_UP)
    {
      timeService.onWifiUp();
    }
    // Timestamp the entry into MQTT_UP so the post-MQTT hold below can run out.
    // millis() can be 0 for one tick after boot; 1 is close enough and keeps 0
    // meaning "not up".
    g_mqttUpSinceMs = (netStatus == NetStatus::MQTT_UP) ? (now == 0 ? 1 : now) : 0;
    g_lastNetStatus = netStatus;
    controller.publishNetStatus(netStatus);
    log_i("Net status %d: heap=%u minFree=%u", (int)netStatus,
          (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
  }

  // Cheap every loop — the handler logs only when the flag actually changes.
  // Latched rather than compared against millis() so the boot window cannot
  // re-open when millis() wraps after 49.7 days (Plan 11 review).
  static bool bootWindowOver = false;
  if (!bootWindowOver && now >= kPostBootHoldMs) bootWindowOver = true;
  const bool bringingUp = (netStatus == NetStatus::WIFI_CONNECTING ||
                           netStatus == NetStatus::MQTT_CONNECTING);
  const bool connectHold =
      (!bootWindowOver && bringingUp) ||
      (netStatus == NetStatus::MQTT_UP && g_mqttUpSinceMs != 0 &&
       now - g_mqttUpSinceMs < kPostMqttHoldMs);
  treadmill.setConnectHold(connectHold);

  delay(1); // yield to the idle task

  const uint32_t loopMs = millis() - loopStartMs;
  if (loopMs > kLoopStallMs)
  {
    log_w("loop stall %lu ms", (unsigned long)loopMs);
  }
}
