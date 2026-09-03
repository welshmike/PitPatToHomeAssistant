# Plan 1: Toolchain bump + DevKit refactor (spec Phase 0 + Phase A)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (or superpowers:executing-plans) to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restructure the firmware on the ESP32 DevKit so a second command source (the Dial) and a display can be added without touching the Q1 BLE logic, and so BLE keeps running with no WiFi. Also get the project building for the M5Stack Dial (ESP32-S3) so S3 surprises surface now.

**Architecture:** See `docs/superpowers/specs/2026-09-03-m5dial-migration-design.md` §4 to §7. In short: `TreadmillHandler` becomes BLE link only; `SessionTracker` owns accounting; `TreadmillController` is the single command entry point; `NetManager` makes WiFi/MQTT non-blocking; `SnapshotStore` guards shared state; `TreadmillState` defers NVS writes to the main loop; `board.h` isolates pins.

**Tech stack:** PlatformIO, `espressif32@6.13.0` (Arduino-ESP32 2.0.17 for both DevKit and S3), NimBLE-Arduino 2.5.1, ArduinoJson 7, PubSubClient 2.8, mqttdisco, Unity tests on the `native` platform (Apple clang on this Mac).

**Hard rule for every task:** `doc/Q1_BLE_NOTES.md` behaviour is preserved. Code may move between files; timings, reason-code handling, backoffs and flag semantics may not change.

**Verification for every task** (unless the task says otherwise):
```bash
~/Library/Python/3.9/bin/pio run -e devkit-usb
~/Library/Python/3.9/bin/pio run -e dial-usb
~/Library/Python/3.9/bin/pio test -e native
```
All three must pass. Report flash usage for `devkit-usb`. Commit at the end of each task with the message given.

**Model guidance for subagents:** mechanical moves and config (Tasks 0, 1, 3, 4, 6, 9) → `sonnet`; extractions that need judgement about behaviour (Tasks 2, 5, 7, 8, 10) → `opus`. Spec-compliance reviews → `sonnet`; code-quality review of Tasks 5, 7, 8 → `opus`.

---

## Task 0: Commit the baseline

**Files:** all currently modified/untracked files; `.gitignore`.

- [ ] `git rm --cached` nothing; instead `rm Archive.zip` and add `Archive.zip` to `.gitignore` under "# Build artifacts".
- [ ] `git add platformio.ini partitions.csv doc/Q1_BLE_NOTES.md src/ .gitignore`.
- [ ] Verify build: `pio run -e usb` (old env name still valid in this task).
- [ ] Commit: `Baseline before M5Dial refactor: idle/pause timers, pending commands, calibration restore, usb/ota envs`.

Tag it: `git tag devkit-baseline-2026-09-03`.

---

## Task 1: Toolchain bump and build environments

**Files:** `platformio.ini`, new `partitions_8mb.csv`, `src/main.cpp` (only if the new core breaks an include).

- [ ] Replace `platformio.ini` with:

```ini
; Flash DevKit via USB:   pio run -e devkit-usb -t upload
; Flash DevKit via OTA:   pio run -e devkit-ota -t upload
; Flash Dial via USB:     pio run -e dial-usb -t upload
; Unit tests (host):      pio test -e native
; Monitor:                pio device monitor -e <env>

[platformio]
default_envs = devkit-usb

[common]
platform = espressif32@6.13.0
framework = arduino
monitor_speed = 115200
build_flags =
    -DCORE_DEBUG_LEVEL=3
    -DCONFIG_NIMBLE_CPP_LOG_LEVEL=2
    -Wno-cpp
lib_deps =
    h2zero/NimBLE-Arduino@^2.5.1
    bblanchon/ArduinoJson@^7.4.2
    knolleary/PubSubClient@^2.8
    https://github.com/peteh/mqttdisco.git

[devkit]
extends = common
board = esp32dev
board_build.partitions = partitions.csv
monitor_port = /dev/cu.usbserial-0001
monitor_rts = 0
monitor_dtr = 0
build_flags = ${common.build_flags} -DBOARD_DEVKIT

[env:devkit-usb]
extends = devkit
upload_speed = 115200

[env:devkit-ota]
extends = devkit
upload_protocol = espota
upload_port = esp32-ece33445ef18.local

[dial]
extends = common
board = m5stack-stamps3
board_build.partitions = partitions_8mb.csv
build_flags = ${common.build_flags}
    -DBOARD_M5DIAL
    -DARDUINO_USB_MODE=1
    -DARDUINO_USB_CDC_ON_BOOT=1
lib_deps = ${common.lib_deps}
    m5stack/M5Dial@^1.0.3
    m5stack/M5Unified
    m5stack/M5GFX

[env:dial-usb]
extends = dial

[env:dial-ota]
extends = dial
upload_protocol = espota
upload_port = pacekeeper-dial.local

[env:native]
platform = native
test_framework = unity
build_flags = -std=gnu++17 -DNATIVE_TEST
build_src_filter = +<ba05protocol.cpp> +<SessionTracker.cpp> +<TreadmillController.cpp>
test_build_src = yes
```

  Note: `SessionTracker.cpp` and `TreadmillController.cpp` do not exist yet. Until Task 5/8 create them, PlatformIO ignores missing filter entries, so `pio test -e native` will only fail if there are no tests; that is fine until Task 4 adds the first test.

- [ ] Create `partitions_8mb.csv`:

```csv
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x5000,
otadata,  data, ota,     0xe000,   0x2000,
app0,     app,  ota_0,   0x10000,  0x300000,
app1,     app,  ota_1,   0x310000, 0x300000,
spiffs,   data, spiffs,  0x610000, 0x1E0000,
coredump, data, coredump,0x7F0000, 0x10000,
```

- [ ] Remove `#include <mdns.h>` from `src/main.cpp` (unused; ArduinoOTA brings ESPmDNS).
- [ ] Build `devkit-usb`, then `dial-usb`. Expect the first build to download the platform (a few minutes). Fix any Arduino 2.0.17 compile break minimally and note it in the commit body.
- [ ] Commit: `Pin espressif32 6.13.0, add dial-usb/dial-ota/native envs and 8MB partition table`.

---

## Task 2: Spike — M5Dial hello world (separate project)

**Files:** `spike/m5dial-hello/platformio.ini`, `spike/m5dial-hello/src/main.cpp`.

Purpose: prove the S3 toolchain, encoder, touch, BtnA, buzzer and USB serial on the real Dial before the app depends on them. Build only; Mike flashes.

- [ ] `spike/m5dial-hello/platformio.ini`:

```ini
[env:dial]
platform = espressif32@6.13.0
board = m5stack-stamps3
framework = arduino
monitor_speed = 115200
build_flags = -DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1
lib_deps =
    m5stack/M5Dial@^1.0.3
    m5stack/M5Unified
    m5stack/M5GFX
```

- [ ] `src/main.cpp`: `M5Dial.begin(cfg, true, false)`; each loop: `M5Dial.update()`; read `M5Dial.Encoder.read()`; `M5Dial.Touch.getDetail()` for state and x/y; `M5Dial.BtnA.wasClicked()` beeps `M5Dial.Speaker.tone(2000, 50)`; draw encoder count large in the centre, touch x/y and BtnA state below, using `M5Dial.Display.setTextDatum(middle_center)` and a full-screen `M5Canvas` sprite pushed once per frame at 20 Hz. Print the same values to `Serial` every 500 ms.
- [ ] Build: `cd spike/m5dial-hello && ~/Library/Python/3.9/bin/pio run`. Record flash size and any library warnings in the commit body.
- [ ] Commit: `Add M5Dial hello-world spike for hardware bring-up`.

**Hardware check for Mike:** flash with `pio run -t upload` from `spike/m5dial-hello`, confirm: encoder count changes by 4 per detent (record actual pulses per detent), clockwise direction sign, touch coordinates, BtnA click beeps, serial output over USB-C.

---

## Task 3: `board.h` and Arduino-free `TreadmillData.h`

**Files:** new `src/board.h`, new `src/TreadmillData.h`, edit `src/platform.h`, `src/main.cpp`, `src/TreadmillHandler.h`, `src/ba05protocol.h`, `src/ba05protocol.cpp`.

- [ ] `src/board.h`:

```cpp
#pragma once
// Per-target pins and features. Selected by -DBOARD_DEVKIT or -DBOARD_M5DIAL.
#if defined(BOARD_M5DIAL)
  #define BOARD_NAME        "m5dial"
  #define HAS_STATUS_LED    0
  #define HAS_DIAL_UI       1
#elif defined(BOARD_DEVKIT) || !defined(NATIVE_TEST)
  #define BOARD_NAME        "devkit"
  #define HAS_STATUS_LED    1
  #define LED_BLE_PIN       2      // built-in blue LED, active-high
  #define HAS_DIAL_UI       0
#else
  #define BOARD_NAME        "native"
  #define HAS_STATUS_LED    0
  #define HAS_DIAL_UI       0
#endif
```

- [ ] Move `class TreadMillData` from `platform.h` into `src/TreadmillData.h` with only `#include <stdint.h>`. Add the speed constants there too (they are pure numbers):

```cpp
static constexpr float    SPEED_MIN_MPH     = 0.6f;
static constexpr float    SPEED_MAX_MPH     = 3.8f;
static constexpr float    SPEED_STEP_MPH    = 0.1f;
static constexpr uint16_t SPEED_RAW_PER_MPH = 1600;
static constexpr uint16_t SPEED_RAW_MAX     = 6080;   // 3.8 mph
static constexpr uint16_t SPEED_RAW_STOP_BELOW = 100; // < ~0.1 km/h => stop
static constexpr uint16_t START_SPEED_RAW   = 994;    // ~1.0 km/h
static constexpr uint32_t SPEED_SETTLE_MS   = 400;
```

- [ ] `platform.h`: `#include "board.h"` and `#include "TreadmillData.h"`; delete the `LED_BLE_PIN` define and the `TreadMillData` class body. Keep UUIDs, `STEP_LENGTH_M`, version, HA topic externs.
- [ ] `ba05protocol.h`: include `TreadmillData.h` instead of `platform.h`. `ba05protocol.cpp`: remove `#include "esp_log.h"` (unused). Confirm the file now has no Arduino dependency.
- [ ] `main.cpp` and `TreadmillHandler.h`: wrap every `pinMode`/`digitalWrite(LED_BLE_PIN...)` in `#if HAS_STATUS_LED`. In `TreadmillHandler`, replace `START_SPEED` with `START_SPEED_RAW`.
- [ ] Build all, commit: `Add board.h and Arduino-free TreadmillData.h; centralise speed constants`.

---

## Task 4: First native test — BA05 protocol

**Files:** new `test/test_protocol/test_main.cpp`.

- [ ] Tests (Unity):
  - `makeKeepalive(seq)` produces `4D 00 seq 05 6A 05 FD F8 43`.
  - `makePacket(994, 0x01, 0x0C, 7)` has header `4D 00 07 17 6A 17`, speed bytes `03 E2`, byte 12 = 0x01, byte 16 = 0x0C, byte 26 = 0x43, and byte 25 equals XOR of bytes 5..24.
  - Parse the recorded 57-byte 0x34 packet from `treadmill_log.txt` line 345 (`4D 04 09 34 68 34 00 12 C0 12 C0 00 00 00 83 ...`): `valid`, `packetType==0x34`, `speedFeedback` ≈ 0x12C0/1600 = 3.0, `distanceM == 0x0083`, status is RUNNING or STOPPED per byte 28 flags (assert whatever the parser returns for flags byte 0x7A: RUNNING, since speed != 0).
  - A synthetic 50-byte 0x2F packet with flags byte 45 = 0x18 parses as COUNTDOWN; 0x10 as PAUSED; 0x00 as STOPPED.
  - A 20-byte packet parses `speedFeedback` from bytes 9..10 and is `valid`.
  - A 10-byte packet is `!valid`.
- [ ] `pio test -e native` passes. The first run installs the native platform.
- [ ] Commit: `Add native Unity tests for BA05 protocol`.

---

## Task 5: Extract `SessionTracker` (TDD)

**Files:** new `src/SessionTracker.h`, `src/SessionTracker.cpp`, new `test/test_session_tracker/test_main.cpp`, edit `src/TreadmillState.h/.cpp`, `src/TreadmillHandler.h/.cpp`.

### 5a. Interfaces

- [ ] `TreadmillState` implements a pure interface and defers NVS:

```cpp
// in TreadmillData.h (pure)
struct SessionDelta { float distKm = 0; uint32_t steps = 0, calories = 0, durationSec = 0; };

class ITotalsStore {
public:
    virtual float    totalDistanceKm()  const = 0;
    virtual uint32_t totalSteps()       const = 0;
    virtual uint32_t totalCalories()    const = 0;
    virtual uint32_t totalDurationSec() const = 0;
    virtual float    stepLengthM(float speedMph) const = 0;
    virtual void     addSession(const SessionDelta&) = 0;   // in-memory now, NVS later
    virtual ~ITotalsStore() = default;
};

class ISessionEvents {
public:
    virtual void onSessionStarted() = 0;   // belt moving: cancel idle timer
    virtual void onSessionEnded()   = 0;   // clean STOPPED: autoReconnect=false, arm idle timer
    virtual void onPaused()         = 0;   // arm pause timeout
    virtual void onResumed()        = 0;   // cancel pause timeout
    virtual void onSettledIdle()    = 0;   // first 0x2F STOPPED with no session: arm idle timer
    virtual ~ISessionEvents() = default;
};
```

  `TreadmillState : public ITotalsStore`. `addSession()` updates members and sets `m_dirty = true`. New `void flush()` writes NVS if dirty (called from `TreadmillHandler::handle()` on the main loop). Existing `saveTotalsToNVS` becomes private. `restoreTotals` still writes immediately.

- [ ] `SessionTracker` public API:

```cpp
class SessionTracker {
public:
    SessionTracker(ITotalsStore& totals, ISessionEvents& events);
    void onConnected();                                  // reset per-connection state
    TreadMillData onPacket(const BA05Protocol::ParsedData& p,
                           const TreadMillData& prev, uint32_t nowMs);
    void onDisconnected(const TreadMillData& last);      // mid-session / paused commit
    void onPauseCommand();                               // sets paused flag
    bool onStopCommand();                                // commits paused session if any; true if committed
    TreadMillData onPauseTimeout(const TreadMillData& last); // commit paused snapshot, return summary snapshot
    bool sessionActive() const; bool isPaused() const;
private:
    SessionDelta computeDelta(float distKm, uint32_t cal, uint32_t dur, float speedMph) const;
    // moved from TreadmillHandler: m_connectionBase*, m_sessionActive, m_isPaused,
    // m_paused*, m_justResumedFromPause, m_resumeDistanceM, m_lastPacketType, m_firstPacketAfterConnect
};
```

  `onPacket` reproduces today's `notifyCallback` body from "if (length >= 50 ...)" down to the totals block, calling `events.*` where the handler used to arm/disarm timers or set `m_autoReconnect=false`. Logging via a `TRACKER_LOG*` macro that maps to `log_i/log_w` on Arduino and `printf` under `NATIVE_TEST`.

### 5b. Tests first

- [ ] Write `test/test_session_tracker/test_main.cpp` with a `FakeTotals` (step length 0.5 m, records `addSession` calls) and `FakeEvents` (counts each event). Cases:
  1. First packet captures baseline: connect, packet STOPPED dist=500 m; then RUNNING speed 2.0 dist=700 → live `totalDistanceKm == totals + 0.2`, `onSessionStarted` once.
  2. Odometer reset: baseline 500, then RUNNING dist=50 → bases zeroed, live delta 0.05.
  3. Clean stop commits delta once, sets `sessionComplete`, zeroes live fields, fires `onSessionEnded`.
  4. Pause: `onPauseCommand`, then STOPPED with dist>0 → no commit, `onPaused` fired, totals held at snapshot. Then RUNNING dist>0 → `onResumed`, still no commit. Then STOPPED with same dist → suppressed (no commit). Then RUNNING dist larger, STOPPED → one commit of full delta.
  5. Pause then belt reset: paused, then RUNNING dist=0 → commit of paused snapshot, `sessionComplete`.
  6. Mid-session disconnect: RUNNING dist=1200 from base 200, `onDisconnected` → one commit of 1.0 km.
  7. `onStopCommand` while paused commits snapshot and returns true; while not paused returns false and commits nothing.
  8. `onSettledIdle` fires exactly once on first 0x2F STOPPED with no session, not on 0x34.
  9. Delta is never negative in any of the above (assert in FakeTotals).
- [ ] Run `pio test -e native`; tests fail to compile (no tracker yet).

### 5c. Implement and wire

- [ ] Implement `SessionTracker.cpp` by moving code from `TreadmillHandler::notifyCallback`, `onDisconnect`, `stop()`, `pause()`, and the pause-timeout block in `handle()`.
- [ ] `TreadmillHandler : public NimBLEClientCallbacks, public ISessionEvents`. Members: `TreadmillState m_state; SessionTracker m_tracker{m_state, *this};`. Implement the five event methods with exactly the timer/flag code that lived at those points. `notifyCallback` becomes: parse → `data = m_tracker.onPacket(parsed, m_lastData, millis())` → store → flag. `handle()` gains `m_state.flush()` at the end.
- [ ] Remove the moved members from `TreadmillHandler.h`. `toggleCalibration` uses `m_lastData.speedFeedback` as before.
- [ ] All three builds plus tests pass. Commit: `Extract SessionTracker from TreadmillHandler; defer NVS totals writes to main loop`.

---

## Task 6: `SnapshotStore` (mutex-guarded shared state)

**Files:** new `src/SnapshotStore.h`, edit `src/TreadmillHandler.h/.cpp`.

- [ ] `SnapshotStore.h`:

```cpp
#pragma once
#include "TreadmillData.h"
#ifdef NATIVE_TEST
class SnapshotStore {
public:
    void write(const TreadMillData& d) { m_data = d; }
    TreadMillData read() const { return m_data; }
    template<typename F> void modify(F f) { f(m_data); }
private: TreadMillData m_data;
};
#else
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
class SnapshotStore {
public:
    SnapshotStore() { m_mtx = xSemaphoreCreateMutex(); }
    void write(const TreadMillData& d) { lock(); m_data = d; unlock(); }
    TreadMillData read() const { lock(); TreadMillData c = m_data; unlock(); return c; }
    template<typename F> void modify(F f) { lock(); f(m_data); unlock(); }
private:
    void lock() const { xSemaphoreTake(m_mtx, portMAX_DELAY); }
    void unlock() const { xSemaphoreGive(m_mtx); }
    mutable SemaphoreHandle_t m_mtx; TreadMillData m_data;
};
#endif
```

- [ ] Replace `TreadMillData m_lastData` in `TreadmillHandler` with `SnapshotStore m_snapshot`. `getLastData()` → `m_snapshot.read()`. Every `m_lastData.x = y` becomes a `modify([&](auto& d){ ... })`. `notifyCallback` reads `prev = m_snapshot.read()` once, calls the tracker, then `write()`s once.
- [ ] Build all, commit: `Guard shared treadmill snapshot with a FreeRTOS mutex`.

---

## Task 7: `NetManager` and a non-blocking main loop

**Files:** new `src/NetManager.h`, `src/NetManager.cpp`, edit `src/main.cpp`.

- [ ] `NetManager` API:

```cpp
enum class NetStatus : uint8_t { WIFI_DOWN, WIFI_CONNECTING, WIFI_UP, MQTT_CONNECTING, MQTT_UP };
class NetManager {
public:
    NetManager(const char* ssid, const char* pass, const char* mqttHost, uint16_t mqttPort,
               const char* mqttUser, const char* mqttPass);
    void begin(const char* hostname);            // WiFi.mode(STA), scan/sort methods, WiFi.begin(); non-blocking
    void tick(uint32_t nowMs);                   // state machine, ArduinoOTA.handle(), client.loop()
    NetStatus status() const;
    PubSubClient& mqtt();
    void onMqttConnected(std::function<void()> cb);   // subscribe + publish configs/states
    void setMqttCallback(MQTT_CALLBACK_SIGNATURE);
    bool wifiUp() const; bool mqttUp() const;
};
```

  Backoff on failure: 1 s, 2 s, 5 s, 10 s, 30 s cap, reset on success. OTA and mDNS start once on first `WIFI_UP`. No `ESP.restart()` anywhere. MQTT `connect()` is blocking in PubSubClient (socket timeout default 15 s); set `client.setSocketTimeout(5)` and only attempt when the backoff allows, so a dead broker costs at most 5 s per 30 s. Log every state transition at `log_i`.

- [ ] `main.cpp` `setup()`: watchdog, Serial, `#if HAS_STATUS_LED` LED, `treadmill.begin()`, `NimBLEDevice::init`, then `net.begin(composeClientID().c_str())`. Nothing blocks. Move the OTA callbacks into `NetManager`.
- [ ] `loop()`:

```cpp
void loop() {
    esp_task_wdt_reset();
    const uint32_t now = millis();
    treadmill.handle();      // BLE first, always
    net.tick(now);
    delay(1);                // yield to idle task; no 100 ms sleep
}
```

  The old "publish counters on MQTT reconnect" behaviour becomes the `onMqttConnected` callback (subscriptions + `publishAllConfigs` + retained states, exactly the list in today's `connectToMqtt`).

- [ ] Build all, commit: `Add NetManager: non-blocking WiFi/MQTT with backoff; BLE handled every loop`.

**Hardware check for Mike (DevKit):** start a walk from HA, then power off the WiFi AP for 30 s. Belt must keep running; HA catches up when WiFi returns. Boot with WiFi off: BLE must connect to the belt and HA must appear once WiFi is back.

---

## Task 8: `TreadmillController` intent layer (TDD)

**Files:** new `src/TreadmillController.h/.cpp`, new `test/test_controller/test_main.cpp`, edit `src/TreadmillHandler.h`.

- [ ] Link interface implemented by `TreadmillHandler`:

```cpp
class ITreadmillLink {
public:
    virtual bool isConnected() const = 0;
    virtual void start() = 0;                 // start at START_SPEED_RAW (queues if disconnected)
    virtual void pause() = 0;
    virtual void stop() = 0;
    virtual void setSpeedRaw(uint16_t raw) = 0; // queues if disconnected
    virtual bool requestConnect() = 0;        // today's toggleConnection() connect branch
    virtual bool requestDisconnect() = 0;     // today's toggleConnection() disconnect branch (safety check inside)
    virtual TreadMillData snapshot() const = 0;
    virtual void publishOptimistic(const TreadMillData&) = 0; // write snapshot + flag new data
    virtual ~ITreadmillLink() = default;
};
```

- [ ] Observer interface and controller:

```cpp
class ISnapshotObserver {
public:
    virtual void onSnapshot(const TreadMillData&) = 0;
    virtual void onTargetSpeed(float mph, bool pending) = 0;  // pending=true while settle timer runs
    virtual void onNetStatus(NetStatus) {}
    virtual ~ISnapshotObserver() = default;
};

class TreadmillController {
public:
    explicit TreadmillController(ITreadmillLink& link);
    void addObserver(ISnapshotObserver&);
    void start(); void pause(); void resume(); void stop();
    void toggleStartPause();                 // stopped/disconnected→start, running→pause, paused→resume, countdown→stop
    void setSpeedMph(float mph);             // clamp [0.6,3.8]; <0.55 => stop (HA slider at 0 still stops)
    void nudgeSpeed(int clicks, uint32_t nowMs);
    void tick(uint32_t nowMs);               // fires setSpeedMph(target) after SPEED_SETTLE_MS
    void requestConnect(); void requestDisconnect();
    void publish();                          // push link.snapshot() to observers (called by main loop when new data)
    float targetSpeedMph() const;
};
```

  Rules: `nudgeSpeed` seeds the target from `speedCmd` if not pending (or from `SPEED_MIN_MPH` when stopped), adds `clicks * SPEED_STEP_MPH`, clamps, notifies `onTargetSpeed(target, true)`, sets deadline `now + SPEED_SETTLE_MS`. `tick` fires `setSpeedMph(target)` once at the deadline and notifies `onTargetSpeed(target, false)`. `setSpeedMph` when status is STOPPED or DISCONNECTED calls `link.setSpeedRaw(raw)` (the link already starts the belt at that speed and queues when disconnected). Every command applies the same optimistic status the MQTT handlers apply today (COUNTDOWN / PAUSED / STOPPED / `speedCmd` rounded to 0.1) via `link.publishOptimistic`, then `publish()`.

- [ ] Tests with a `FakeLink` recording calls: clamps at both ends; slider 0.0 → `stop()`; `toggleStartPause` table for all five statuses; three nudges within 400 ms produce one `setSpeedRaw(round((2.0+0.3)*1600))`; nudge while DISCONNECTED calls `setSpeedRaw` (queued start) after settle; observers receive exactly one `onSnapshot` per command.
- [ ] Implement, build all, tests pass. Commit: `Add TreadmillController intent layer with settle-then-send speed nudges`.

---

## Task 9: MQTT on its own FreeRTOS task; MqttView as observer; `main.cpp` reduced to wiring

**Files:** new `src/NetTask.h`, `src/NetTask.cpp`, new `src/Commands.h`, edit `src/main.cpp`, `src/mqttview.h/.cpp`, `src/NetManager.h/.cpp`.

**Why revised:** the Task 7 review showed `PubSubClient::connect()` (3 s TCP + 5 s CONNACK spin) and `WiFiClient::write()` (10 x 1 s retry once the TCP send buffer saturates after AP loss) block the loop task for longer than the 6 s BLE supervision timeout. Rate limiting does not close the gap. The only robust fix is to run all networking on a separate task so the loop task (BLE, controller, later the display) is never blocked by a socket. Spec §4.5 and §5 are amended accordingly.

### 9a. Threading model

- **Loop task** (Arduino `loop()`): `treadmill.handle()`, drain the command queue into `controller`, `controller.tick()`, `delay(1)`. Never touches `PubSubClient`, `WiFi`, or `ArduinoOTA`.
- **Net task** (`NetTask`, FreeRTOS task, 8 KB stack, priority 1, pinned to core 0 on dual-core targets, `tskNO_AFFINITY` otherwise): loops `net.tick(millis())`, drains the publish queue into `MqttView`, `vTaskDelay(pdMS_TO_TICKS(10))`. Owns `NetManager`, `MqttView`, `PubSubClient`, OTA. The MQTT receive callback runs on this task and may only parse and enqueue.
- **Queues** (`src/Commands.h`):

```cpp
enum class CmdType : uint8_t { START, PAUSE, STOP, SET_SPEED_MPH, CONNECT, DISCONNECT,
    SET_AUTO_RECONNECT, SET_IDLE_MINS, SET_PAUSE_MINS, TOGGLE_CALIBRATION,
    RESTORE_TOTALS, RESTORE_CALIBRATION, HA_ONLINE };
struct Command {
    CmdType type;
    float    f = 0;            // SET_SPEED_MPH
    bool     b = false;        // SET_AUTO_RECONNECT
    uint16_t u16 = 0;          // SET_IDLE_MINS / SET_PAUSE_MINS
    struct { float distKm; uint32_t steps, calories, durationSec; bool has[4]; } totals; // RESTORE_TOTALS
    struct { CalibrationPoint pts[10]; uint8_t n; } calib;                                // RESTORE_CALIBRATION
};
enum class PubType : uint8_t { SNAPSHOT, AUTO_RECONNECT, IDLE_MINS, PAUSE_MINS, CALIB_COUNT, FULL_RESYNC };
struct PublishItem { PubType type; TreadMillData snap; bool b; uint16_t u16; uint8_t u8; };
```

  Command queue: net → loop, depth 8, `xQueueSend` with zero wait; on full, log and drop. Publish queue: loop → net, depth 8; `SNAPSHOT` items are sent with zero wait and dropped when full (the next snapshot supersedes), settings items use a 50 ms wait so they are not lost. `FULL_RESYNC` is enqueued by the loop task in response to `HA_ONLINE` and on demand; the net task handles it by running the same list that today's `onMqttConnected` runs (configs, delay 200, state, settings, calibration count).

- `TreadMillData` and `CalibrationPoint` are PODs, so queue copies are plain `memcpy`. `sizeof(Command)` is about 120 bytes and `sizeof(PublishItem)` about 100 bytes; both fine for 8-deep queues.

### 9b. Wiring

- `MqttView : public ISnapshotObserver` is NOT how it is wired now (it would run on the wrong task). Instead a tiny `PublishQueueObserver : public ISnapshotObserver` (in `main.cpp` or `NetTask.h`) implements `onSnapshot` by enqueueing a `SNAPSHOT` item and `onTargetSpeed` by doing nothing while pending and enqueueing a snapshot when the settle fires. `MqttView` keeps its `publishState` API and gains nothing; remove `m_publishingConfigs` and its comments.
- `NetManager::onMqttConnected` callback: subscriptions (same nine plus two restore topics plus both HA status topics) and then a `FULL_RESYNC`. It runs on the net task, so it may call `MqttView` directly.
- MQTT receive `callback(topic, payload, len)` runs on the net task: match topic exactly as today, parse into a `Command` using a bounded copy helper:

```cpp
static bool payloadToBuf(const byte* payload, unsigned int len, char* buf, size_t bufSize) {
    if (len >= bufSize) return false;
    memcpy(buf, payload, len); buf[len] = '\0'; return true;
}
```
  Speed uses a 32-byte buffer and `strtof`; button payloads compare against `"press"`; switch payloads against the entity's on/off strings; numbers via `strtoul`; restore payloads via `deserializeJson(doc, payload, len)` (length-aware, no copy). Then `xQueueSend`.
- Loop task `drainCommands()` executes each `Command` against `controller` / `treadmill` exactly as the old handlers did, then enqueues the corresponding `PublishItem` (settings) or lets the controller's observer notification carry the snapshot. `HA_ONLINE` → enqueue `FULL_RESYNC`.
- `treadmill.setCallback(...)` is deleted; instead `handle()` returns `bool newData`, and `loop()` calls `controller.publish()` when true. (`controller.publish()` notifies observers with `link.snapshot()`.)
- `NetTask::begin()` creates both queues and the task; `setup()` calls it after `NimBLEDevice::init`. `NetTask` exposes `NetStatus status()` (an atomic copy updated by the net task each tick) so the future display can read it from the loop task.
- Speed constants: the last literals (`1600.0f`, `6080`, `3.8f` in `mqttview.cpp` `setMax`) come from `TreadmillData.h`.

### 9c. Verification

- [ ] `pio test -e native` (controller/tracker/protocol suites unchanged), `pio run -e devkit-usb`, `pio run -e dial-usb`.
- [ ] Static check: `grep -n "client\.\|g_mqttView\.\|WiFi\.\|ArduinoOTA" src/main.cpp` returns only the `setup()` wiring lines, none in `loop()` or `drainCommands()`.
- [ ] Commit: `Run MQTT/OTA on a dedicated task with command and publish queues; MqttView fed by observer`.

**Hardware check for Mike (DevKit):** same as Task 7's plus: with a walk running, `docker stop` (or equivalently kill) the MQTT broker for 60 s, then restart it. Belt must keep running; HA entities recover within one backoff period. Then power off the AP for 30 s mid-walk; belt keeps running.

---

## Task 10: Handler cleanup, docs, Phase A hardware checklist

**Files:** `src/TreadmillHandler.h/.cpp`, `doc/Q1_BLE_NOTES.md`, `README.md`.

- [ ] `TreadmillHandler.h`: move the inline `onDisconnect` body to the `.cpp`; header should be declarations plus short inline getters. Confirm the remaining members are all BLE-link concerns (client, characteristics, connect flags, backoff, timers, keepalive, cooldown, pending command, intentional drop). Anything else goes to tracker or controller.
- [ ] Grep for `m_lastData` (should be gone), `LED_BLE_PIN` outside `board.h`/`#if HAS_STATUS_LED`, `1600.0f` / `6080` / `3.8f` literals outside `TreadmillData.h` (should be gone).
- [ ] `doc/Q1_BLE_NOTES.md`: update code references (`SessionTracker::onPacket` for `m_sessionActive`, `TreadmillState::flush()` for NVS, `NetManager`). No behaviour text changes.
- [ ] `README.md`: build/flash commands use the new env names; mention `pio test -e native`.
- [ ] Build all, tests pass. Commit: `Tidy TreadmillHandler to BLE link concerns; update docs for new module layout`.

**Phase A hardware checklist for Mike (DevKit, real belt):**
1. Flash `devkit-usb`. HA shows the same PaceKeeper device and entities; retained totals unchanged.
2. Start / pause / resume / stop from HA. Session summary and totals match a manual delta.
3. Set speed from HA slider; stop via slider at 0.
4. Mid-walk WiFi outage 30 s: belt keeps running, HA catches up.
5. Boot with WiFi off: belt connects; HA appears when WiFi returns.
6. Idle disconnect and pause timeout still fire (set to 1 min each for the test).
7. Kick phase from cold connect settles in about 2 min as before.

When all seven pass, tag `phase-a-verified` and start Plan 2 (Dial port + UI).

---

## Out of scope for this plan
`DialUi`, screens, encoder/touch wiring, dimming, buzzer, `dial-ota` hostname, README hardware section for the Dial. These are Plan 2 and depend on the Task 2 spike results and the Phase A hardware checklist.
