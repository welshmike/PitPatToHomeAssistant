# Plan 4: Card framework, knob navigation, analogue clock

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Checkbox steps.

**Goal:** spec §4.8, sub-project 1. Cards ring (Treadmill, Clock), knob rotates cards when the belt is idle, side button goes home to Treadmill, boot into Clock, dim-but-never-off, analogue clock with NTP + RTC.

**Verification for every task:** `pio test -e native`, `pio run -e devkit-usb`, `pio run -e dial-usb`; no new `src/` warnings. Commit per task.

**Constraints:** `doc/Q1_BLE_NOTES.md` BLE behaviour unchanged. Speed numbers only from `TreadmillData.h`. `DialUi` has no `delay`/per-frame heap; wrap-safe time. Pure logic Arduino-free with Unity tests. Treadmill behaviour (selector, start, pause, stop, kick handling) unchanged.

---

## Task 1: `TimeService` — NTP, timezone, RTC

**Files:** new `src/TimeService.h/.cpp`, `src/config.h.example` (+ `TIMEZONE_TZ`), `src/main.cpp`, `src/DialUi.cpp` (M5 config), `platformio.ini` (nothing unless needed).

- [ ] `config.h.example`: `#define TIMEZONE_TZ "GMT0BST,M3.5.0/1,M10.5.0"` (Europe/London) with a comment pointing at the POSIX TZ format. `config.h` is gitignored; document that existing users must add the line (fallback `#ifndef TIMEZONE_TZ` to the London string in `TimeService.h` so builds do not break).
- [ ] `TimeService` (device-only, Arduino): `begin()` (on Dial: read BM8563 via `M5.Rtc` if `isEnabled()` and the date is plausible (year ≥ 2024) → `settimeofday`; set `m_source = RTC`); `void onWifiUp()` → `configTzTime(TIMEZONE_TZ, "pool.ntp.org", "time.google.com")`; `tick(nowMs)`: once `getLocalTime` succeeds after WiFi up, write the RTC once (`M5.Rtc.setDateTime`) and set `m_source = NTP`; `bool valid() const`; `bool localTime(struct tm&) const`. On the DevKit (no RTC) the RTC parts compile out under `HAS_DIAL_UI`.
- [ ] Dial: set `cfg.internal_rtc = true` in `DialUi::begin()` so M5Unified initialises the RTC.
- [ ] `main.cpp`: `timeService.begin()` after `dialUi.begin()`; call `onWifiUp()` when `NetStatus` first reaches `WIFI_UP` (reuse the existing net-status change detection in `loop()`); `tick(now)` each loop. Log `Time: source=RTC|NTP local=YYYY-MM-DD HH:MM:SS` once per source change.
- [ ] Commit: `Add TimeService: NTP with POSIX TZ, RTC sync and boot read on the Dial`.

## Task 2: Card ring and knob navigation

**Files:** new `src/CardRing.h` (pure), new `test/test_card_ring/test_main.cpp`, `src/DialUi.h/.cpp`, `src/DialInput.h/.cpp` + tests, `platformio.ini` native filter (header-only ring needs no .cpp; if you add one, add it).

- [ ] `enum class CardId : uint8_t { TREADMILL, CLOCK, COUNT };` and `class CardRing { CardId current() const; void next(); void prev(); void set(CardId); }` wrapping around. Tests: wrap both ways, `set`.
- [ ] `DialInput`: remove the OFF stage (`Backlight` keeps `FULL`/`DIM` only; delete `OFF_AFTER_MS`; DIM→OFF transition gone). Update/replace the affected tests (dim at 120 s still; no off at 600 s).
- [ ] `DialUi`: `CardRing m_cards` initialised to `CLOCK`. Screen selection: belt screens (CONNECTING/STARTING/RUNNING) first as today; else if selector open → SELECTOR; else the current card: `TREADMILL` → existing Disconnected screen; `CLOCK` → clock card (Task 3 draws it; this task can draw a placeholder `CLOCK` `Font4`).
- [ ] Knob routing when not running/paused and selector closed: `detents > 0` → `next()` per detent, `< 0` → `prev()`; when the selector is open detents step it (unchanged); running → nudge (unchanged). Tap/hold on the Treadmill card unchanged; on the Clock card tap/hold do nothing (accept beep off). Side button: if belt running/paused → `stop()` as now; else `m_cards.set(TREADMILL)` (and close the selector).
- [ ] Frame key includes `CardId`. Dots shown on all card screens.
- [ ] Commit: `Card ring with knob navigation; side button homes to Treadmill; backlight dims but never turns off`.

## Task 3: Analogue clock card

**Files:** new `src/ClockFace.h/.cpp` (pure), new `test/test_clock_face/test_main.cpp`, `src/DialUi.h/.cpp`, `platformio.ini` native filter.

- [ ] `ClockFace` (pure): `struct Hand { int x0, y0, x1, y1; }`; `Hand hourHand(int h, int m, int cx, int cy, int len)`, `minuteHand(int m, int s, ...)`, `secondHand(int s, ...)`, `void tickMark(int i, int cx, int cy, int r0, int r1, int& x0, int& y0, int& x1, int& y1)` using `sinf/cosf`, 12 o'clock = up. Tests: 12:00:00 hands point straight up (x1 == cx, y1 == cy − len within 1 px); 3:00 hour hand points right; 6:30 hour hand is between 6 and 7 (angle 195°); second hand at 15 s points right.
- [ ] Render in `DialUi::drawClock(gfx)`: 12 ticks (`r0=104..r1=112`, longer at 12/3/6/9), hour hand length 56 width 5, minute 82 width 3, second 96 width 1 (red), centre dot r=4. Use `drawWideLine` if available in M5GFX (`LGFXBase::drawWideLine`), else `drawLine` ×3 offset. Date `Mon 3 Sep` `Font2` at (120, 168). When `!timeService.valid()`: face without hands and `--:--` `Font4` at the centre. `DialUi` gets a `const TimeService&` (constructor injection alongside the controller). Redraw when the second changes (frame key includes `hh*3600+mm*60+ss` and `valid`).
- [ ] Commit: `Analogue clock card (NTP/RTC time, 1 Hz redraw)`.

## Task 4: Docs and Phase D checklist

**Files:** `README.md`, `doc/AUDIT_2026-09-03.md`.

- [ ] README: "Desk mode" subsection: cards, knob to scroll, side button home, boot into clock, dim not off, `TIMEZONE_TZ` in `config.h`, RTC behaviour. Phase D checklist: boots into clock with correct local time after WiFi; time survives a reboot without WiFi (RTC); knob scrolls Treadmill ↔ Clock both directions; side button returns to Treadmill; belt screens override and return to the last card; dims at 2 min and never turns off; treadmill flows (selector, start, pause, stop) unchanged.
- [ ] Commit: `Docs: desk mode cards and clock`.
