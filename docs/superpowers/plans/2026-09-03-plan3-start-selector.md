# Plan 3: Default start speed + start selector

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Checkbox steps.

**Goal:** spec §4.7. Configurable start speed (HA + NVS, default 1.0 mph) used by every start path; a Dial selector screen (knob or swipe, 0.2 mph steps) between tap and start; long press starts at the default.

**Verification for every task:** `pio test -e native`, `pio run -e devkit-usb`, `pio run -e dial-usb`; no new `src/` warnings. Commit per task.

**Constraints:** `doc/Q1_BLE_NOTES.md` BLE behaviour unchanged (reply-per-notification heartbeat, reconnect logic). Speed numbers only from `TreadmillData.h`. No `String`/heap per frame in `DialUi`. All new pure logic Arduino-free with Unity tests.

---

## Task 1: `startSpeedMph` setting through handler, controller, MQTT

**Files:** `src/TreadmillData.h` (add `static constexpr float START_SPEED_DEFAULT_MPH = 1.0f; static constexpr float SELECTOR_STEP_MPH = 0.2f; static constexpr uint32_t SELECTOR_TIMEOUT_MS = 20000;`), `src/TreadmillHandler.h/.cpp`, `src/ITreadmillLink.h`, `src/TreadmillController.h/.cpp`, `src/Commands.h`, `src/NetTask.cpp`, `src/main.cpp`, `src/mqttview.h/.cpp`, `test/test_controller/test_main.cpp`.

- [ ] Handler: `uint16_t m_startSpeedTenths` loaded in `begin()` from `pk_cfg`/`start` (default 10), saved in `saveSettings()`. `setStartSpeedMph(float)` (clamp, round to tenths, save), `float getStartSpeedMph() const`. `uint16_t startSpeedRaw() const` = `lroundf(mph*SPEED_RAW_PER_MPH)`.
- [ ] `ITreadmillLink`: replace `bool start()` with `bool startAtRaw(uint16_t raw)` (queues `PendingCmd::START` with `m_pendingSpeed = raw` when disconnected) and add `uint16_t startSpeedRaw() const`. Handler `start()` becomes `startAtRaw(startSpeedRaw())`. Remove `START_SPEED_RAW` uses except as a floor in `resume()` (keep the constant).
- [ ] Controller: `start()` → `m_link.startAtRaw(m_link.startSpeedRaw())`; new `startAt(float mph)` → clamp, `startAtRaw(rawFromMph(mph))`, optimistic COUNTDOWN + `speedCmd`. `toggleStartPause()` unchanged (calls `start()`). Pass-throughs `startSpeedMph()`.
- [ ] MQTT: `MqttNumber m_startSpeed("start-speed", "Start Speed")` config category, min `SPEED_MIN_MPH`, max `SPEED_MAX_MPH`, step 0.1, unit mph, BOX mode, icon `mdi:speedometer-slow`. Subscribe, `CmdType::SET_START_SPEED` (float), `PubType::START_SPEED`, published on connect/resync and after set. `drainCommands()` routes to `treadmill.setStartSpeedMph()` then enqueues publish.
- [ ] Tests: FakeLink gains `startSpeedRaw` field and records `START_AT(raw)`; `start()` sends the link's start raw; `startAt(2.0)` sends 3200; `startAt` clamps; queued-while-disconnected still returns true and publishes COUNTDOWN. Update existing tests that asserted `START_SPEED_RAW`.
- [ ] Commit: `Configurable start speed (HA number + NVS); all start paths use it`.

## Task 2: Swipe detection in `DialInput` (TDD)

**Files:** `src/DialInput.h/.cpp`, `test/test_dial_input/test_main.cpp`.

- [ ] `DialEvents` gains `int swipe` (−1 left, +1 right, 0 none). Constant `SWIPE_MIN_PX = 40`. During a touch, when `|dx| ≥ 40 && |dx| > |dy|` from the touch-down point, emit `swipe = sign(dx)` once for that touch and mark the gesture consumed (no tap, no long press, `holdProgress` 0 afterwards). Wake gating applies (a swipe on a dimmed screen only wakes).
- [ ] Tests: right swipe emits +1 once; left −1; diagonal with |dy| ≥ |dx| does not swipe (and is a drag: no tap); swipe then release → no tap; swipe while DIM → wake only; a 39 px move is not a swipe and, if released quickly, still counts as a drag (not a tap) because it exceeds `TAP_MAX_MOVE_PX`.
- [ ] Commit: `DialInput: horizontal swipe detection`.

## Task 3: `SpeedSelector` pure state (TDD)

**Files:** new `src/SpeedSelector.h/.cpp`, new `test/test_speed_selector/test_main.cpp`, `platformio.ini` native filter.

- [ ] API: `void open(float defaultMph, uint32_t nowMs)`, `bool isOpen() const`, `void step(int n, uint32_t nowMs)` (±`SELECTOR_STEP_MPH` per n, clamp `[SPEED_MIN_MPH, SPEED_MAX_MPH]`, round to 0.1, refresh activity), `float value() const`, `bool tick(uint32_t nowMs)` (returns true and closes when `SELECTOR_TIMEOUT_MS` elapsed since last activity), `void close()`.
- [ ] Tests: opens at default; +3 → +0.6; clamps both ends; timeout closes at exactly 20 000 ms and not at 19 999; activity resets timeout; wraparound-safe; `close()` idempotent.
- [ ] Commit: `Add SpeedSelector state with native tests`.

## Task 4: Selector screen and input wiring in `DialUi`

**Files:** `src/DialUi.h/.cpp`.

- [ ] New `Screen::SELECTOR`, chosen when `m_selector.isOpen()` and the belt is not RUNNING/paused/COUNTDOWN and not connecting (any of those closes the selector).
- [ ] Input on Disconnected: tap → `m_selector.open(m_controller.startSpeedMph(), nowMs)` + accept beep; long press → `m_controller.start()` (default speed). On Selector: knob detents → `step(detents)`; `swipe` → `step(swipe)`; tap → `m_controller.startAt(m_selector.value())`, close; long press → `m_controller.start()`, close; side button → close (cancel), no belt command; `tick()` timeout → close. Hold-progress arc still drawn.
- [ ] Render: ring track + amber value arc at `value()`, centre `Font7` value (1 dp) with `mph` caption (`Font2`, y=140), title `START SPEED` `Font2` at y=60, hints `tap to start` y=196 and `hold: default` y=214 (`Font2`). Dots shown. Frame key includes `selectorOpen` and `int(value*10)`.
- [ ] The knob must not call `nudgeSpeed` while the selector is open (selector consumes detents). Rotate is otherwise still ignored when not running.
- [ ] Commit: `DialUi: start-speed selector screen (knob or swipe, 0.2 mph steps)`.

## Task 5: Docs

**Files:** `README.md`, `doc/AUDIT_2026-09-03.md`.

- [ ] README controls: tap on Disconnected opens the selector; knob or swipe 0.2 mph; tap starts; hold starts at default; side button cancels; 20 s timeout; "Start Speed" HA entity. Phase C checklist: add items for the selector (open, step by knob, step by swipe, confirm, default via hold, cancel, timeout) and for the HA Start button using the configured speed.
- [ ] Commit: `Docs: start speed setting and selector`.
