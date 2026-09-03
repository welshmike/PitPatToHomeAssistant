# Plan 2: Dial port and DialUi (spec Phase B + Phase C)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Gate (revised 2026-09-03):** no DevKit is available, so the Phase A checklist runs on the Dial after Task 1. If BLE misbehaves there, build tag `devkit-baseline-2026-09-03` for `m5stack-stamps3` as a control to separate refactor bugs from S3 port bugs.

**Goal:** Run the refactored firmware on the M5Stack Dial v1.1, then add `DialUi`: tap = start/pause/resume, long-press = stop, side button = stop, rotate = speed (settle then send, rotate-from-stopped starts), screens per spec §4.6, idle dimming, buzzer feedback, OTA for the Dial.

**Architecture:** spec `docs/superpowers/specs/2026-09-03-m5dial-migration-design.md` §4.6, §5. `DialUi` is a loop-task `ISnapshotObserver`; it reads inputs via M5Dial, calls `TreadmillController`, and renders into one full-screen `M5Canvas`. Pure input/formatting logic lives in Arduino-free classes with native tests.

**Confirmed hardware facts:** `Encoder.read()` moves 4 per detent, clockwise positive. Touch, BtnA and buzzer work through `M5Dial` 1.0.3 (see `spike/m5dial-hello/src/main.cpp` for compiling API usage: `M5Dial.begin(cfg, true, false)`, `M5Dial.update()`, `Encoder.read()`, `Touch.getDetail().isPressed()`, `BtnA.wasClicked()`, `Speaker.tone(freq, ms)`, `M5Canvas` with `setColorDepth(16)`, `createSprite(240,240)`, `setTextDatum(middle_center)`, `drawString(str, x, y, &fonts::FontN)`, `pushSprite(0,0)`).

**Operational constraint:** only one device may run at a time (same BLE peer, same HA device identity `pacekeeper-bridge`). Power off the DevKit before flashing the Dial.

**Verification for every task:** `pio test -e native`, `pio run -e devkit-usb`, `pio run -e dial-usb` all pass; report dial flash % and, once DialUi exists, free heap after `createSprite` from the serial log when Mike flashes.

**Model guidance:** pure-logic + tests (Tasks 2, 4) → opus for design, sonnet acceptable for the formatter task; M5GFX rendering (Tasks 5, 6) → opus; config/docs (Tasks 1, 7, 8) → sonnet.

---

## Task 1: DialUi skeleton and Phase B smoke firmware

**Files:** new `src/DialUi.h`, `src/DialUi.cpp`, edit `src/main.cpp`, `src/board.h` (only if needed).

- [ ] `DialUi : public ISnapshotObserver`, compiled only under `#if HAS_DIAL_UI`. API: `void begin();` (M5Dial init, canvas create with 16 bpp, log `ESP.getFreeHeap()` before/after, fall back to direct `M5Dial.Display` drawing if `createSprite` fails), `void tick(uint32_t nowMs);` (`M5Dial.update()`, render at most every 50 ms), `onSnapshot` stores a copy, `onTargetSpeed` stores target + pending, `onNetStatus` stores status.
- [ ] Smoke render only: three status dots along the top (BLE = snapshot status != DISCONNECTED; WiFi = status ≥ WIFI_UP; MQTT = MQTT_UP), the status name in the centre, speed feedback and distance below in `Font2`. No inputs yet.
- [ ] `main.cpp`: under `#if HAS_DIAL_UI` construct `DialUi dialUi(controller)`, call `dialUi.begin()` FIRST in `setup()` (M5Unified must init the display and I2C before anything else), `controller.addObserver(dialUi)`, and `dialUi.tick(now)` after `controller.tick(now)` in `loop()`.
- [ ] M5Unified config: `auto cfg = M5.config(); cfg.serial_baudrate = 115200; cfg.internal_imu = false; cfg.internal_rtc = false;` then `M5Dial.begin(cfg, true, false)`. Keep `Serial.begin` in `main.cpp` guarded `#if !HAS_DIAL_UI` (M5Unified opens Serial itself).
- [ ] Commit: `Add DialUi skeleton with status dots; Dial smoke firmware`.

**Hardware check for Mike (Phase B):** power off the DevKit, flash `dial-usb`, `pio device monitor -e dial-usb`. Expect: screen shows dots + "disconnected"; press Connect BLE in HA; kick phase settles (~2 min, beeps normal); HA shows the same device and entities as before; start/pause/stop/speed from HA work; live speed/distance appear on the Dial screen. Report the "free heap after sprite" log line and whether the S3 needed more or fewer kick cycles than the DevKit.

---

## Task 2: `DialInput` pure logic (TDD)

**Files:** new `src/DialInput.h`, `src/DialInput.cpp`, new `test/test_dial_input/test_main.cpp`, edit `platformio.ini` native `build_src_filter` (+`DialInput.cpp`).

Arduino-free. Inputs are fed each tick; outputs are events.

```cpp
struct DialEvents { int detents = 0; bool tap = false; bool longPress = false; float holdProgress = 0; bool wake = false; bool btnStop = false; };
class DialInput {
public:
    // pulsesPerDetent = 4; tapMaxMs = 600; tapMaxMovePx = 20; holdMs = 1000; dimAfterMs = 120000; offAfterMs = 600000
    DialEvents tick(long encoderCount, bool touchDown, int touchX, int touchY, bool btnClicked, uint32_t nowMs);
    enum class Backlight { FULL, DIM, OFF }; Backlight backlight() const;
    void noteActivity(uint32_t nowMs);  // called by UI on belt activity so a running walk never dims
};
```

Rules:
- Detents: accumulate `encoderCount - lastCount`; emit `delta / 4` (integer, sign preserved), keep the remainder.
- Tap: touch down then up within 600 ms with movement under 20 px, and no long-press fired.
- Long press: touch held 1000 ms without exceeding 20 px movement → `longPress` once; `holdProgress` ramps 0→1 during the hold (for the ring animation), resets on release.
- Wake gating: if `backlight() != FULL` when any input arrives, emit only `wake = true` and swallow that input (detents, tap, long press, button). Backlight returns to FULL.
- Dimming: FULL → DIM after 120 s without activity, DIM → OFF after 600 s total. `noteActivity` resets.
- `btnStop` passes BtnA clicks through (subject to wake gating).

- [ ] Tests: 4 pulses → 1 detent, 6 pulses → 1 detent then 2 more pulses → second detent, negative direction; tap inside/outside 600 ms; drag > 20 px is neither tap nor long press; long press fires once at 1000 ms with progress 0.5 at 500 ms; input while DIM yields wake only; dim at 120 s, off at 600 s, activity resets; wraparound-safe time maths (start at `UINT32_MAX - 1000`).
- [ ] Commit: `Add DialInput: encoder detents, tap/long-press, wake gating and dimming (native tests)`.

---

## Task 3: Wire inputs to the controller and buzzer

**Files:** `src/DialUi.h/.cpp`, `src/TreadmillData.h` (add `DIAL_SPEED_OVERLAY_MS 1500`), `platformio.ini` (`-DDIAL_SOUND=1` in `[dial]`).

- [ ] In `DialUi::tick`: read `M5Dial.Encoder.read()`, `Touch.getDetail()` (`isPressed()`, `x`, `y`), `BtnA.wasClicked()`, feed `DialInput`. Map events: `tap` → `controller.toggleStartPause()`; `longPress` → `controller.stop()`; `btnStop` → `controller.stop()`; `detents != 0` → `controller.nudgeSpeed(detents, nowMs)`; `wake` → brightness only.
- [ ] Brightness: FULL 255 (configurable constant), DIM 50, OFF 0 via `M5Dial.Display.setBrightness()`. Call `input.noteActivity()` whenever snapshot status is RUNNING or COUNTDOWN so a walk never dims.
- [ ] Buzzer (`#if DIAL_SOUND`): tap accepted `tone(2000, 40)`; stop `tone(1500, 60)` twice 80 ms apart (use a tiny non-blocking scheduler, no `delay`); refused command (controller returned nothing new: compare snapshot status before/after `toggleStartPause`) `tone(400, 120)`.
- [ ] Commit: `DialUi: tap/hold/rotate/button drive the controller; brightness and buzzer`.

---

## Task 4: Display formatting helpers (TDD, pure)

**Files:** new `src/DialFormat.h/.cpp`, new `test/test_dial_format/test_main.cpp`, `platformio.ini` native filter.

- [ ] `formatDuration(uint32_t sec, char* out, size_t n)` → `mm:ss` under one hour, `h:mm:ss` from 3600 s. `formatDistanceKm(float, ...)` → two decimals. `formatSteps(uint32_t, ...)` → plain integer with no separators. `speedToAngle(float mph)` → degrees 0..300 mapping `[0, SPEED_MAX_MPH]` onto a 300° arc starting at 120° (gap at the bottom). Clamp.
- [ ] Tests for each boundary (59:59 → 1:00:00, 0 → 00:00, 3.8 → 300°, negative → 0°).
- [ ] Commit: `Add DialFormat helpers with native tests`.

---

## Task 5: Running / Paused screens and speed overlay

**Files:** `src/DialUi.cpp` (render functions), `src/DialUi.h`.

Layout on 240x240 (centre 120,120):
- Speed ring: `fillArc(120,120, 118, 108, 120, 120 + speedToAngle(speedFeedback), colour)` over a dark track arc; small numeric speed (`Font4`, e.g. `2.3`) at the ring's end angle position or fixed at (120, 200) — pick fixed for stability, label `mph` in `Font2`.
- Centre: time big (`Font7`, `formatDuration(durationSec)`), or during the overlay window the target speed (`Font7`, one decimal) with a `mph` caption; overlay lasts `DIAL_SPEED_OVERLAY_MS` after the last `onTargetSpeed(pending=true)`; while pending, ring shows the target in a distinct colour.
- Row under time: distance km (2 dp) left, steps right, `Font4`, captions `Font2`.
- Paused: same layout at reduced colours, `PAUSED` pulsing (1 Hz) above the time, hint `tap resume · hold stop` at the bottom. "Paused" = `controller`'s link `isPaused()` (expose via controller `bool isPaused() const` delegating to the link) OR status PAUSED.
- Status dots: three 8 px circles at y = 14, x = 96/120/144: BLE blue, WiFi green, MQTT green; dim grey when off.
- Long-press progress: draw an outer thin arc growing with `holdProgress` in red.
- Render only when something changed or 250 ms elapsed (pulse animation), max 20 Hz.
- [ ] Commit: `DialUi: running and paused screens, speed overlay, hold progress`.

---

## Task 6: Disconnected / Connecting / Starting screens

**Files:** `src/DialUi.cpp`, `src/TreadmillHandler.h/.cpp` (expose `bool isConnecting() const` = `m_doConnect && m_autoReconnect`, and `uint16_t connectAttempts() const` counting `connectToDevice()` calls, reset when `requestConnect()` is called), `src/TreadmillController.h/.cpp` (pass-throughs `isConnecting()`, `connectAttempts()`), `ITreadmillLink` (add the two getters; update `FakeLink` in tests).

- [ ] Disconnected: last session summary (`sessionDurationSec`, `sessionDistanceKm`, `sessionSteps`) in the same row style, `tap or turn to start` hint, dots on top.
- [ ] Connecting: `Connecting…` with `attempt N`, `belt beeps are normal`, `hold to cancel` (long press → `controller.requestDisconnect()`).
- [ ] Starting (status COUNTDOWN): pulsing `STARTING`, `tap to cancel`.
- [ ] Screen selection order: COUNTDOWN → Starting; connected & (RUNNING or paused) → Running/Paused; connecting → Connecting; else Disconnected.
- [ ] Commit: `DialUi: disconnected, connecting and starting screens`.

---

## Task 7: Dial OTA hostname and build hygiene

**Files:** `src/NetManager.cpp`, `platformio.ini`, `src/board.h`.

- [ ] `board.h`: `#define OTA_HOSTNAME "pacekeeper-dial"` for M5DIAL only; `NetManager::startOtaOnce()` calls `ArduinoOTA.setHostname(OTA_HOSTNAME)` when defined (DevKit keeps its default `esp32-<chipid>` so `devkit-ota` still works).
- [ ] Confirm `dial-ota` `upload_port = pacekeeper-dial.local`.
- [ ] Commit: `Dial OTA hostname pacekeeper-dial`.

---

## Task 8: Docs and Phase C checklist

**Files:** `README.md`, `doc/AUDIT_2026-09-03.md`, `docs/superpowers/specs/...` (only if behaviour was adjusted during implementation).

- [ ] README: "M5Stack Dial" section — wiring-free (USB-C or 6–36 V terminal), controls (tap/hold/rotate/side button), what the screen shows, flashing (`dial-usb` first, then `dial-ota`), one-device-at-a-time note.
- [ ] Phase C hardware checklist appended to the audit doc: each screen appears in the right state; tap starts from disconnected (connect + start), tap pauses, tap resumes at the previous speed, hold stops, side button stops; rotate 5 clicks → one speed command +0.5 mph, rotate from stopped starts at the dialed speed; overlay reverts after 1.5 s; dimming at 2 min, off at 10 min, first touch only wakes; buzzer tones; HA entities unchanged; `dial-ota` upload works; kick phase settles; mid-walk AP off 30 s and broker kill 60 s — belt keeps running.
- [ ] Commit: `Docs: M5Stack Dial section and Phase C checklist`.

---

## Backlog from Plan 1 to fold in where touched
See the end of `2026-09-03-plan1-devkit-refactor.md`. Do in this plan: `MQTT_CONNECTING` observable from `NetTask::status()` (Task 1 or 6, needed for the dots), dedupe the second snapshot publish on `onTargetSpeed(false)` (Task 3), split `ITreadmillLink.h` (Task 6 when the interface grows). Leave the rest unless a task touches that code.

## Backlog from the Plan 2 final review (follow-up, not blocking)
- Tap "refused" heuristic compares status/speedCmd before and after; a failed GATT write (status → DISCONNECTED) beeps "accepted", and a resume to the same speed beeps "refused". Make the controller return an explicit accepted/refused result instead.
- Dial-initiated cancel/stop change `m_autoReconnect` without echoing `PubType::AUTO_RECONNECT`, so HA's Auto Reconnect switch drifts (the MQTT STOP path has the same gap).
- `kHintY = 222` may clip Font2 hints at the round bezel; check on hardware and move up if needed.
- Skip rendering while the backlight is OFF.
- `pulsePhase` in the frame key forces a 2 Hz redraw on screens with nothing pulsing.
- Verify on hardware that the 115 KB canvas plus NimBLE init leaves enough heap (log lines exist).
