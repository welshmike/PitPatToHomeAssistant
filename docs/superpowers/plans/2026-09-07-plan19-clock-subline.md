# Plan 19: Next meeting on the Clock card — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One small line under the Clock card's date showing today's next meeting (`14:30 Standup`), amber while in progress, absent when there is none today.

**Architecture:** `CalendarModel::clockLine()` (pure) builds the text from the snapshot using the caller's `localDayOf` and `hhmm` callbacks; `DialUi::drawClock()` builds it once per frame under `#if HAS_CALENDAR` and passes it to `drawClockCard(gfx, theme, time, subline, live)`; `DialClockView` draws it at y 186 with `fitToRow`-style bezel trimming (copy the helper from `DialCalendarView.cpp` into a small shared header `src/DialTextFit.h` so both views use one implementation). Spec: §4.19 amendment "clock subline".

**Tech Stack:** PlatformIO `native` (348 tests) and `dial-ota`; `pio` at `~/Library/Python/3.9/bin/pio`.

## Global Constraints
- Subline at `kClockSublineY = 186`, Font2, `Col::DIM`, `Col::PENDING` when `nowEpoch + 30 >= start` (same predicate as the card's countdown colour); `HH:MM Title`, title clipped so the whole line fits `rowWidth(186)` (chord ≈ 200 px minus margin); only today's next timed event (`CalendarModel::nextTimedToday`), only when `snapshot.valid && !isStale`, `nowEpoch != 0`, `fetchedOnce`; otherwise no line (the date stays where it is).
- Build without `CALENDAR_URL` unchanged (subline `nullptr`). Never stage `src/config.h`. Commit message ends with `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`. Do not upload.

---

### Task 1: `clockLine()`, shared text-fit helper, clock view and DialUi wiring, docs

**Files:**
- Modify: `src/CalendarModel.h/.cpp` — add
  ```cpp
  // Builds "HH:MM Title" for today's next timed event (nextTimedToday) into buf,
  // returns chars written (0 = nothing to show). `live` is set true when the event
  // is in progress or starts within 30 s. `hhmm` formats an epoch as local HH:MM.
  size_t clockLine(const Snapshot& s, uint32_t nowEpoch, uint32_t (*localDayOf)(uint32_t),
                   void (*hhmm)(uint32_t epoch, char* out, size_t cap), char* buf, size_t cap, bool& live);
  ```
- Create: `src/DialTextFit.h` (device-only, `#if HAS_DIAL_UI`): `inline int32_t rowWidth(int32_t y)` and `inline void fitToRow(LovyanGFX& gfx, char* s, int32_t y, const lgfx::IFont* font)` moved verbatim from `DialCalendarView.cpp`; `DialCalendarView.cpp` includes it and drops its copies.
- Modify: `src/DialClockView.h/.cpp` — `drawClockCard(LovyanGFX&, const DialTheme&, const TimeService&, const char* subline = nullptr, bool live = false)`; when `subline && *subline` draw it at `kClockSublineY = 186` in `PENDING`/`DIM` after `fitToRow(..., 186, &fonts::Font2)`, drawn right after the date (before nothing — the hands are drawn earlier/later exactly as the date is today; keep the same order relative to the date).
- Modify: `src/DialUi.cpp` `drawClock()` — under `#if HAS_CALENDAR`: compute `nowEpoch`, `dataValid` as in the nudge block, call `CalendarModel::clockLine(m_calSnap, nowEpoch, localDayOf, hhmm, buf, sizeof buf, live)` using the same `localtime_r`-based callbacks the Calendar view uses (expose them from `DialCalendarView.h` as `calendarLocalDayOf`/`calendarHhmm`, or move them into `DialTextFit.h`'s sibling — implementer's call, but one definition), and pass `buf`/`live`; otherwise call the two-arg form. FrameKey: the clock already redraws every second (`clockSec`), so no new key field.
- Test: `test/test_calendar_model/test_main.cpp` — using `fakeDay` and a `fakeHhmm` (writes `"HH:MM"` from `epoch % 86400`): today's next → `"09:30 Standup"`-style string with `live == false` 25 min before and `live == true` 10 s before and during; after today's last event → returns 0 even though tomorrow has an event; all-day only today → 0; invalid snapshot → 0; a 40-char title is passed through untrimmed (trimming is the view's job) but the buffer never overflows (`cap` 16 → truncated, NUL-terminated, returns what fits).
- Docs: README Clock card bullet gains the subline sentence; spec §4.19 As-built one line if anything deviates; AUDIT Phase L two items: "Clock shows `HH:MM Title` under the date for today's next meeting, amber once it starts; line disappears after the day's last meeting ends".

- [ ] Steps: tests → RED → implement → GREEN (`pio test -e native`, expect 348 + ~5) → `pio run -e dial-ota` with and without `CALENDAR_URL` (both SUCCESS; restore config.h) → docs → commit `Clock: today's next meeting under the date (spec 4.19 amendment)`.

---

### Task 2: Date above the centre; title and time on two lines below (spec amendment "clock layout")

**Files:** `src/CalendarModel.h/.cpp` (`clockLine` → `size_t clockLine(const Snapshot&, uint32_t nowEpoch, localDayOf, hhmm, char* titleBuf, size_t titleCap, char* timeBuf, size_t timeCap, bool& live)` returning the title length, 0 = nothing), `test/test_calendar_model/test_main.cpp` (update the six clockLine tests to the two-buffer form; title has no time prefix; time buffer is `"HH:MM"`), `src/DialClockView.h/.cpp` (`kClockDateY = 72`; `drawClockCard(gfx, theme, time, const char* title = nullptr, const char* when = nullptr, bool live = false)`; title at `kClockEventTitleY = 168` `DIM`, time at `kClockEventTimeY = 186` `DIM`/`PENDING`; both through `fitToRow`; "waiting for time" stays at `kClockDateY`), `src/DialUi.cpp` `drawClock()` (two buffers), README Clock bullet, AUDIT Phase L item wording, spec as-built if anything deviates.

- [ ] Tests first → RED → implement → GREEN (`pio test -e native`, count unchanged at 354 unless a test is added for the time buffer) → both `dial-ota` variants SUCCESS (restore `src/config.h`) → docs → commit `Clock: date above the centre, meeting title and time below (spec 4.19 amendment)`.
