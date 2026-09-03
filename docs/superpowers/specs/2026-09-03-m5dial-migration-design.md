# PaceKeeper on M5Stack Dial v1.1 — design

Date: 2026-09-03. Status: approved for planning.
Companion audit: `doc/AUDIT_2026-09-03.md`. BLE behaviour reference: `doc/Q1_BLE_NOTES.md`.

## 1. Goals

- Replace the ESP32 DevKit with an M5Stack Dial v1.1 as the single BLE bridge to the DeerRun Q1 Classic Pro.
- Local control without Home Assistant: tap the screen to start/pause/stop, turn the dial to change speed, live stats on the round display.
- Standalone first: BLE and local controls work with no WiFi. WiFi, MQTT and OTA connect in the background and retry forever.
- Keep the Home Assistant MQTT integration and entity set unchanged. The HA device identity (`pacekeeper-bridge`) is MAC independent, so the Dial takes over the existing HA device.
- Preserve every Q1 BLE behaviour documented in `doc/Q1_BLE_NOTES.md` (kick-phase backoff, supervision-timeout drop, zombie detection, GATT cache reuse, 200 ms keepalive).
- Keep the DevKit build working throughout, so every refactor step is testable against known-good hardware before the port.

## 2. Non-goals

- RFID, RTC, battery operation, Strava changes, new HA entities.
- ESPHome. Its BLE client cannot do the keepalive and kick-phase handling the Q1 needs.
- Running the DevKit and Dial at the same time. Only one BLE central can hold the Q1.

## 3. Hardware facts used by this design

| Item | Value |
|---|---|
| MCU | StampS3A, ESP32-S3FN8, dual core 240 MHz, 8 MB flash, no PSRAM |
| Display | 1.28" round 240x240, GC9A01 SPI (RS G4, MOSI G5, SCK G6, CS G7, RST G8, BL G9) |
| Touch | FT3267, I2C SDA G11 / SCL G12 / INT G14 |
| Encoder | A G41, B G40. 16 detents and 64 pulses per revolution, so 4 pulses per detent. **Confirmed on hardware 2026-09-03: `Encoder.read()` changes by 4 per click, clockwise is positive.** No push switch |
| Button | WAKE = BtnA on GPIO42 (side of body). RST also on body |
| Buzzer | G3 (M5Unified `Speaker`) |
| Power | USB-C 5 V or rear screw terminal 6 to 36 V. HOLD pin GPIO46 |
| Library | `m5stack/M5Dial` 1.0.3 on `M5Unified` + `M5GFX`. `M5Dial.begin(cfg, enableEncoder=true, enableRFID=false)` |

## 4. Architecture

```
            +------------------+          +-------------------+
 BLE  <---> | TreadmillHandler | -------> |  SessionTracker   | ---> TreadMillData snapshot
            |  (link layer)    | packets  | (+TreadmillState) |        (mutex guarded)
            +------------------+          +-------------------+
                     ^                              |
                     | commands                     v  observers
            +------------------+          +--------+----------+
            | TreadmillController| <----- | MqttView | DialUi |
            |  (intent layer)  |  input   +--------+----------+
            +------------------+               ^         ^
                                               |         |
                                          NetManager   M5Dial (encoder, touch, BtnA, display)
```

### 4.1 Unchanged
- `ba05protocol`: packet build and parse.
- `TreadmillState`: NVS totals and calibration curve.

### 4.2 `TreadmillHandler` (BLE link layer only)
Keeps: client lifecycle, `connectToDevice()`, connection params, early keepalive, subscribe, `onConnect`/`onDisconnect` with all reason-code and backoff logic, keepalive scheduling, `sendCommand()` with cooldown and zombie detection, idle-disconnect and pause-timeout timers, intentional-drop flags, pending-command execution after cooldown.

Changes:
- Parsing result and connection events are handed to `SessionTracker` instead of being accounted inside the handler.
- All session-accounting members move out (`m_connectionBase*`, `m_paused*`, `m_justResumedFromPause`, `m_resumeDistanceM`, `m_sessionActive` becomes a tracker query).
- `m_lastData` is replaced by a `SnapshotStore`: a `TreadMillData` plus a FreeRTOS mutex with `write(const TreadMillData&)` and `read() -> TreadMillData`. NimBLE task writes, main loop reads. Lock hold time is a struct copy.
- No NVS writes from BLE callbacks. `onDisconnect` and `notifyCallback` record a `PendingCommit` (delta values) that `handle()` flushes on the main loop.
- Timers keep living here because they decide when to drop the link, but they are armed and disarmed by tracker events (`sessionStarted`, `sessionEnded`, `paused`, `settledIdle`).

### 4.3 `SessionTracker` (new)
Pure logic, no Arduino dependencies beyond `millis()` injected as a parameter, so it compiles under the `native` test env.

Responsibilities:
- Connection baselines (distance, calories, duration) captured on the first full packet per connection; reset to zero when the belt odometer is seen below the baseline.
- `m_sessionActive` ground truth (speed > 0.001 while RUNNING).
- Pause snapshot and deferred commit; resume detection and spurious post-resume STOPPED suppression.
- The single `computeDelta(current, base)` used by live totals, pause snapshot, session end and disconnect commit.
- Emits events to the handler: `sessionStarted`, `sessionEnded(summary)`, `paused(snapshot)`, `resumed`, `settledIdle` (first 0x2F STOPPED with no active session).
- Produces the outgoing `TreadMillData` (live fields, totals, session summary, `sessionComplete`).

### 4.4 `TreadmillController` (new, intent layer)
The only thing MQTT and the Dial call.

| Method | Behaviour |
|---|---|
| `start()` | If stopped: send start at `START_SPEED`. If disconnected: connect with pending START. Optimistic status COUNTDOWN. |
| `pause()` | If running: send pause. Optimistic PAUSED. |
| `resume()` | If paused: send speed = last commanded speed. |
| `stop()` | Always allowed. Optimistic STOPPED. |
| `toggleStartPause()` | stopped→start, running→pause, paused→resume, countdown→stop. Used by screen tap. |
| `setSpeedMph(f)` | Clamp to [0.6, 3.8]. If running or paused: send speed. If stopped or disconnected: start at that speed (pending SET_SPEED when disconnected). Optimistic `speedCmd`. |
| `nudgeSpeed(clicks)` | Adjusts a target by 0.1 mph per click, clamps, publishes a `TargetSpeedChanged(target, deadlineMs)` observer event, and after `SPEED_SETTLE_MS` (400 ms) with no further clicks calls `setSpeedMph(target)`. |
| `requestConnect()` / `requestDisconnect()` | Wraps `toggleConnection()` safety logic. |

Constants live in `platform.h`: `SPEED_MIN_MPH 0.6`, `SPEED_MAX_MPH 3.8`, `SPEED_STEP_MPH 0.1`, `SPEED_RAW_PER_MPH 1600`, `SPEED_SETTLE_MS 400`, `START_SPEED_RAW 994`.

Observers implement `onSnapshot(const TreadMillData&)`, `onTargetSpeed(float mph)`, `onNetStatus(NetStatus)`. `MqttView` and `DialUi` register at boot. The controller republishes the snapshot after every command so optimistic state reaches both views once.

Speed encoding stays mph x 1600 raw, cap 6080 raw (3.8 mph), stop below 100 raw, exactly as today.

### 4.5 `NetManager` (new) and the network task
**Amended 2026-09-03 after Task 7 review:** `NetManager`, `MqttView`, `PubSubClient` and OTA run on a dedicated FreeRTOS task (`NetTask`), not on the loop task. `PubSubClient::connect()` can block 3 s (TCP) + 5 s (CONNACK) and `WiFiClient::write()` up to 10 s once the send buffer saturates after AP loss, both longer than the 6 s BLE supervision timeout. The loop task talks to the net task through two FreeRTOS queues: commands in (MQTT → controller) and publish items out (snapshots and settings → MQTT). See Plan 1 Task 9 for the exact structs.

State machine ticked from the net task: `WIFI_DOWN → WIFI_CONNECTING → WIFI_UP → MQTT_CONNECTING → MQTT_UP`. Backoff 1 s, 2 s, 5 s, 10 s, capped at 30 s. No forced reboot on WiFi loss. `WiFi.begin()` is called once after BLE init and never blocks. OTA and mDNS start on `WIFI_UP`. MQTT subscriptions, discovery configs and retained states are (re)published on each `MQTT_UP` and on HA birth message, as today. Exposes `status()` for the display.

### 4.6 `DialUi` (new, Dial env only)
Input:
- Encoder: `readAndReset()` every loop, divided by 4 to get detents, sign = clockwise positive. Calls `controller.nudgeSpeed(detents)`.
- Touch: tap (press and release under 600 ms, movement under 20 px) → `toggleStartPause()`. Long press (held 1000 ms) → `stop()`, with a filling ring drawn during the hold so the user sees it coming.
- BtnA (side button): `stop()`. Backup and emergency stop.
- Any input while the screen is dimmed or off only wakes the screen; the input itself is discarded. Prevents a brush of the dial starting the belt in the dark.

Render, 20 Hz max, into a full-screen M5GFX sprite then pushed, to avoid flicker:

| Screen | Trigger | Content |
|---|---|---|
| Disconnected | status DISCONNECTED, no connect in progress | Last session summary (time, distance, steps). Hint "tap or turn to start". |
| Connecting | connect requested, kick phase | "Connecting… attempt N". Note "belt beeps are normal". Cancel on long press. |
| Countdown | status COUNTDOWN | The BA05 packet does not carry the countdown number, so show a pulsing "STARTING" with "tap to cancel". |
| Running | status RUNNING | Time big (mm:ss, h:mm:ss over an hour) in the centre. Speed ring 0 to 3.8 mph around the edge with small numeric speed at the ring end. Row under time: distance km and steps. |
| Paused | status PAUSED, or STOPPED while tracker says paused | Same as Running, dimmed, "PAUSED" pulsing, "tap to resume, hold to stop". |
| Speed overlay | after `onTargetSpeed` | Target speed replaces the time for 1500 ms after the last click, ring shows target. Reverts automatically. |

Every screen has three status dots on the top edge: BLE (on when connected), WiFi, MQTT.

**Amended 2026-09-03 after Task 8 doc pass:** as built, the Paused hint text reads
"tap resume - hold stop" (not "tap to resume, hold to stop"), and the three status dots are
drawn at full brightness even on the Paused screen, unlike the rest of that screen's elements,
which are all halved. See `src/DialUi.cpp` (`drawRunning()`, `drawStatusDots()`).

**Amended 2026-09-03 after final review:** `currentScreen()` now checks `controller.isConnecting()`
before the COUNTDOWN test, not after — `TreadmillHandler::start()` while disconnected queues the
command and the controller optimistically publishes COUNTDOWN right away, so without this reorder
the Dial showed "STARTING / tap to cancel" for a belt that wasn't actually counting down, and its
tap-to-cancel went through `stop()`, which the disconnected link refuses (nothing was cleared).
Connecting now wins that race, and its cancel gesture is tap **or** hold (not hold alone) —
`handleInput()` checks `currentScreen() == CONNECTING` for both `ev.tap` and `ev.longPress` and
routes either one to `requestDisconnect()` instead of `toggleStartPause()`/`stop()`. The Starting
screen is consequently reachable only once actually connected and the belt itself reports
COUNTDOWN. See `src/DialUi.cpp` (`currentScreen()`, `handleInput()`).

Idle power: after 2 min in Disconnected or Stopped, brightness drops to 20 %. After 10 min the backlight goes off. Any input wakes at full brightness.

Buzzer: one short click on an accepted tap, two on stop, a low buzz when a command is refused (post-connect cooldown, disconnect blocked while belt active). Compile-time `DIAL_SOUND` flag defaults on.

### 4.7 `board.h`
Selected by build flag `-DBOARD_DEVKIT` or `-DBOARD_M5DIAL`. Provides `HAS_STATUS_LED`, `LED_BLE_PIN`, `HAS_DIAL_UI`, and serial settings. `main.cpp` contains no pin numbers.

## 5. Threading and timing

- Loop task has no long `delay()`. Order per pass: watchdog reset, `handler.handle()`, drain command queue into `controller`, `controller.tick()` (speed settle timer), `dialUi.tick()` (Dial only), `delay(1)`.
- Net task: `net.tick()`, drain publish queue into `MqttView`, `vTaskDelay(10 ms)`. Only this task touches sockets.
- NimBLE task: `notifyCallback` parses, calls `tracker.onPacket()`, writes the snapshot under the mutex, sets `m_newDataAvailable`. No MQTT, no NVS, no display from this task.
- `onDisconnect` runs on the NimBLE task: records pending commit, sets flags. Same rules.
- `MqttView::m_publishingConfigs` guard is removed; all publishes are already on one task.
- Watchdog stays at 300 s on the main loop task.

## 6. Build and toolchain

- `platformio.ini` envs: `devkit-usb`, `devkit-ota` (today's `usb`/`ota`, renamed), `dial-usb`, `dial-ota`, `native` (unit tests).
- Platform: pin `espressif32` to a version with `m5stack-stamps3`. First try `6.13.0` (Arduino-ESP32 2.0.17, known good with NimBLE-Arduino 2.5.1 and M5Unified). If the Dial libraries require Arduino 3.x, move to `7.1.0`. The spike task decides and records the outcome in the plan.
- Dial `lib_deps` add `m5stack/M5Dial@^1.0.3`, `m5stack/M5Unified`, `m5stack/M5GFX`. Board `m5stack-stamps3`. USB CDC on boot for serial logs.
- Dial partition table `partitions_8mb.csv`: nvs 0x5000, otadata 0x2000, app0 3 MB, app1 3 MB, spiffs remainder, coredump 64 KB. DevKit keeps `partitions.csv`.
- NVS namespaces and keys unchanged. A fresh Dial has empty totals; restore them with the existing `restore-totals` and `restore-calibration` MQTT topics from HA sensor history.
- `config.h` gains nothing new. `TARGET_ADDRESS`, WiFi and MQTT settings are the same.

## 7. Testing

Automated, run by subagents on every task:
- `native` env with Unity: `SessionTracker` (baselines, odometer reset, pause/resume, spurious STOPPED suppression, mid-session disconnect commit, delta never negative), `TreadmillController` (clamps, toggle state table, nudge settle timer, pending command when disconnected), `BA05Protocol` (packet build checksum, parse of recorded 0x2F/0x34/20-byte samples from `treadmill_log.txt`).
- `pio run -e devkit-usb` and `pio run -e dial-usb` must both succeed after every task. Flash usage reported.

Hardware, by Mike, at the end of each phase (checklist in the plan):
- Phase A (DevKit refactor): unplug WiFi router for 30 s mid-walk, belt must keep running. Start/pause/stop/speed from HA unchanged. Session totals match a manual calculation.
- Phase B (Dial hello): display, encoder detents, touch tap/long press, BtnA, buzzer, serial over USB-C.
- Phase C (Dial full): all Dial screens; tap/turn/hold behaviours; kick phase completes and connects; HA still sees the same device and entities; OTA works.

## 8. Migration phases

- **Phase 0, spike (parallel with A):** toolchain bump, `dial-usb` env, `M5Dial` hello world showing encoder count and touch coordinates. Decides the platform version.
- **Phase A, refactor on DevKit:** `board.h`; `NetManager` and non-blocking `loop()`; `SnapshotStore` mutex and pending commits; `SessionTracker` extraction with unit tests; `TreadmillController` with unit tests; `MqttView` as observer; `main.cpp` reduced to wiring. Commit uncommitted baseline first.
- **Phase B, port:** build the refactored firmware for `dial-usb` with `DialUi` stubbed to status dots only. Verify BLE against the Q1 on the S3 radio.
- **Phase C, Dial UI:** screens, inputs, overlay, dimming, buzzer. Then `dial-ota` and README update.

## 9. Risks

- **S3 radio timing vs the Q1's ~300 ms kick window.** The DevKit needed the early keepalive and GATT cache reuse to survive it; the S3 should be at least as fast, but this is only provable on hardware in Phase B.
- **Shared internal I2C.** Touch, RTC and RFID share G11/G12. Touch polling from the main loop is fine; nothing else touches that bus in this design.
- **Toolchain jump.** Arduino-ESP32 2.0.17 → possibly 3.x changes the watchdog API (already handled behind `ESP_ARDUINO_VERSION_MAJOR`), `mdns.h`, and ArduinoOTA behaviour. The spike surfaces these before the app code depends on them.
- **Accidental start.** Mitigated by: wake-only input when dimmed, the belt's own countdown shown on screen, tap cancels during countdown, and minimum speed 0.6 mph.
