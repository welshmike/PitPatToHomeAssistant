# Plan 17: Calendar card — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A sixth desk card on the Dial showing the next work-calendar meeting, fed by a Google Apps Script in Mike's work account over one small HTTPS request every five minutes, with a "meeting about to start" nudge that interrupts the Clock.

**Architecture:** Pure, natively tested pieces first — `CalendarModel` (JSON → snapshot, next-event and countdown helpers), `CalendarAutoShow` (the nudge state machine, same shape as `FlightsAutoShow`) and the `CardMenu` growth to six cards. Then the device pieces — `CalendarService` on the net task (TLS GET with the GTS root CAs, heap guard, back-off, `Guarded<Snapshot>`), the three HA settings through the existing NVS/MQTT/Command plumbing, and `DialUi` + `DialCalendarView` for the face and the nudge. Everything Calendar-specific compiles only when `CALENDAR_URL` is defined in the gitignored `src/config.h`.

**Tech Stack:** PlatformIO (`pio` at `~/Library/Python/3.9/bin/pio`), envs `native` (Unity, ArduinoJson available natively) and `dial-ota` (ESP32-S3, Arduino-ESP32 2.0.17, M5Dial/LovyanGFX, ArduinoJson 7, ArduinoHA-style MqttView). Spec: `docs/superpowers/specs/2026-09-03-m5dial-migration-design.md` §4.19.

## Global Constraints

- Payload contract (Apps Script → Dial): `{"t":<unix now>,"ev":[{"s":<start unix>,"e":<end unix>,"n":"<title ≤ 40 chars>","a":0|1,"l":"<location ≤ 24 chars>"}]}`, at most 5 events, response ≤ 2 KB.
- Config keys in `src/config.h` (gitignored, never staged): `CALENDAR_URL` (the Apps Script `/exec` URL) and `CALENDAR_TOKEN`. All Calendar code is inside `#ifdef CALENDAR_URL` (`#if HAS_CALENDAR` where `HAS_CALENDAR` is defined 1 in `board.h` when `CALENDAR_URL` is defined, else 0).
- Fetch: every 5 min (`kPollMs = 300000`) while WiFi is up; early refresh when a nudge is due within 60 s and the snapshot is older than 60 s; failure back-off 30 s doubling to a 30 min cap; heap guard free < 60 KB or largest block < 20 KB → skip; connect timeout 5 s, read 8 s; TLS via `setCACert()` with GTS Root R1 + GTS Root R4 PEMs concatenated (`CALENDAR_TLS_INSECURE` → `setInsecure()`); `setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS)`; static 2 KB buffer; net task only.
- Model: `Snapshot { bool valid; uint32_t fetchedAtEpoch; uint8_t count; Event ev[5]; }`, `Event { uint32_t start, end; bool allDay; char title[41]; char where[25]; }`; stale = `!valid` or `fetchedAtEpoch` older than 30 min (`kStaleSec = 1800`).
- Card order: Treadmill, Clock, Flights, Office, Lamp, Calendar. `CardMenu` ring of 6 at radius 88, hit radius 26 unchanged.
- Face geometry (240×240, centre 120,120): name at the existing `kNameY`; all-day line y 40 (Font2, DIM); start time y 62 (Font4); title lines centred on y 108 (Font4, ≤ 2 lines of ≤ 16 chars, ellipsis); countdown y 150 (Font2, PENDING while in progress, TEXT otherwise); following events y 176/192/208 (Font2, DIM) as `HH:MM Title`; empty `nothing more today` (Font4, DIM) at the centre with `Tomorrow HH:MM Title` (Font2, DIM) at y 150; offline `no calendar` (Font4, DIM) with `last update N min ago` (Font2, DIM) at y 150; `waiting for calendar` before the first fetch.
- Nudge: `SHOW_CALENDAR` when enabled, belt idle, current card CLOCK, data valid and not stale, next timed event not dismissed and `nextStart − nowEpoch ≤ leadSec`; `RETURN_TO_CLOCK` when this machine showed it, `nowEpoch ≥ nextStart + staySec`, current card still CALENDAR. `dismiss()` returns at once and blocks that start epoch. Manual navigation forgets the episode.
- Settings: switch **Calendar nudge** (NVS `cal_nudge`, default true), number **Calendar lead** (`cal_lead`, 1–30 min, default 5), number **Calendar stay** (`cal_stay`, 0–10 min, default 1). `CmdType::SET_CALENDAR_NUDGE / SET_CALENDAR_LEAD / SET_CALENDAR_STAY`, `PubType::CALENDAR_NUDGE / CALENDAR_LEAD / CALENDAR_STAY`.
- Times from Google are UTC epochs; display via `TimeService::localTime()` (Europe/London). Countdown forms: `in 25 min`, `in 2 h 05`, `now, 12 min left`, `starts now`.
- Loop task never touches sockets. No per-frame heap. Commit messages end with `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`. Never stage `src/config.h`. Native tests: `~/Library/Python/3.9/bin/pio test -e native` (324 now). Device build: `~/Library/Python/3.9/bin/pio run -e dial-ota` — with AND without `CALENDAR_URL` defined must build (the implementer of device tasks temporarily comments the define out of their local `src/config.h` for the second build, then restores it; never commit it). Do not upload.

---

### Task 1: Apps Script and deployment doc

**Files:**
- Create: `doc/CALENDAR_APPS_SCRIPT.md`
- Create: `doc/calendar_apps_script.gs`
- Modify: `src/config.h.example` (append the two keys)

**Interfaces:**
- Produces: the payload contract above; `CALENDAR_URL`, `CALENDAR_TOKEN` config keys.

- [ ] **Step 1: Write `doc/calendar_apps_script.gs`**

```javascript
// PaceKeeper Dial — calendar feed (spec 4.19).
// Deploy as a Web App: Execute as "Me", Who has access "Anyone".
// The Dial calls  <exec URL>?k=<TOKEN>  every 5 minutes.
const TOKEN = 'CHANGE-ME-to-a-long-random-string';
const MAX_EVENTS = 5;
const TITLE_MAX = 40;
const WHERE_MAX = 24;

function doGet(e) {
  const k = (e && e.parameter && e.parameter.k) || '';
  if (k !== TOKEN) {
    return ContentService.createTextOutput('{"error":"forbidden"}')
      .setMimeType(ContentService.MimeType.JSON);
  }
  const now = new Date();
  const endOfTomorrow = new Date(now);
  endOfTomorrow.setDate(endOfTomorrow.getDate() + 1);
  endOfTomorrow.setHours(23, 59, 59, 999);

  const cal = CalendarApp.getDefaultCalendar();
  const events = cal.getEvents(now, endOfTomorrow)
    .filter(ev => ev.getMyStatus() !== CalendarApp.GuestStatus.NO)
    .sort((a, b) => a.getStartTime() - b.getStartTime())
    .slice(0, MAX_EVENTS)
    .map(ev => ({
      s: Math.floor(ev.getStartTime().getTime() / 1000),
      e: Math.floor(ev.getEndTime().getTime() / 1000),
      n: clip(ev.getTitle() || '(no title)', TITLE_MAX),
      a: ev.isAllDayEvent() ? 1 : 0,
      l: clip(whereOf(ev), WHERE_MAX)
    }));

  const body = JSON.stringify({ t: Math.floor(now.getTime() / 1000), ev: events });
  return ContentService.createTextOutput(body).setMimeType(ContentService.MimeType.JSON);
}

function whereOf(ev) {
  const loc = (ev.getLocation() || '').trim();
  if (loc && !/^https?:\/\//i.test(loc)) return loc;
  const desc = (ev.getDescription() || '') + ' ' + loc;
  if (/meet\.google\.com/i.test(desc)) return 'Google Meet';
  if (/zoom\.us/i.test(desc)) return 'Zoom';
  if (/teams\.microsoft\.com/i.test(desc)) return 'Teams';
  return '';
}

function clip(s, n) {
  s = String(s).replace(/[\r\n]+/g, ' ').trim();
  return s.length > n ? s.slice(0, n - 1) + '…' : s;
}
```

- [ ] **Step 2: Write `doc/CALENDAR_APPS_SCRIPT.md`**

```markdown
# Calendar feed for the Dial (Google Apps Script)

The Dial's Calendar card (spec 4.19) reads the next few work-calendar events from a tiny web
app running inside Mike's **work** Google account. Nothing goes through Home Assistant.

## Deploy (once, ~5 minutes)

1. Signed in to the work Google account, open https://script.google.com and create a new
   project named `PaceKeeper Dial calendar`.
2. Replace the editor contents with `doc/calendar_apps_script.gs`. Change `TOKEN` to a long
   random string (e.g. `openssl rand -hex 24`).
3. Run `doGet` once from the editor (Run ▸ doGet) and accept the Calendar permission prompt.
4. Deploy ▸ New deployment ▸ type **Web app**; Execute as **Me**; Who has access **Anyone**.
   Copy the **Web app URL** (ends in `/exec`).
5. Test in a browser: `<Web app URL>?k=<TOKEN>` returns JSON like
   `{"t":1757260000,"ev":[{"s":1757261400,"e":1757263200,"n":"Standup","a":0,"l":"Google Meet"}]}`.
   Without the right `k` it returns `{"error":"forbidden"}`.
6. In `src/config.h` add
   `#define CALENDAR_URL   "https://script.google.com/macros/s/…/exec"` and
   `#define CALENDAR_TOKEN "<TOKEN>"`, then rebuild and flash. Without `CALENDAR_URL` the card
   is not compiled in.

Redeploy (Deploy ▸ Manage deployments ▸ edit ▸ New version) whenever the script changes; the
URL stays the same.

## Payload

`t` is the script's Unix time; each event has `s`/`e` start and end (Unix, UTC), `n` title
(≤ 40 chars), `a` 1 for all-day, `l` location or meeting host (≤ 24 chars). At most 5 events,
now → end of tomorrow, declined events omitted, recurring events already expanded.

## Notes

- The `/exec` URL answers with a 302 to `script.googleusercontent.com`; the Dial follows it.
- Both hosts chain to Google Trust Services roots R1/R4, which the firmware pins
  (`src/CalendarCerts.h`). If Google re-roots, define `CALENDAR_TLS_INSECURE` as a stopgap.
- If the company Workspace blocks "Anyone" web apps, the deployment step fails with a policy
  message; there is no device-side workaround.
```

- [ ] **Step 3: Append to `src/config.h.example`**

```c
// Calendar card (spec 4.19). Leave both undefined to build without the card.
// #define CALENDAR_URL   "https://script.google.com/macros/s/XXXXXXXX/exec"
// #define CALENDAR_TOKEN "long-random-string"
```

- [ ] **Step 4: Commit**

```bash
git add doc/CALENDAR_APPS_SCRIPT.md doc/calendar_apps_script.gs src/config.h.example
git commit -m "Calendar: Apps Script feed and deploy doc (spec 4.19)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 2: `CalendarModel` (pure)

**Files:**
- Create: `src/CalendarModel.h`, `src/CalendarModel.cpp`
- Create: `test/test_calendar_model/test_main.cpp`
- Modify: `platformio.ini` (`[env:native] build_src_filter` — add `+<CalendarModel.cpp>`)

**Interfaces (Produces):**
```cpp
namespace CalendarModel {
constexpr uint8_t  kMaxEvents = 5;
constexpr uint32_t kStaleSec  = 1800;
struct Event { uint32_t start = 0; uint32_t end = 0; bool allDay = false; char title[41] = {0}; char where[25] = {0}; };
struct Snapshot { bool valid = false; uint32_t fetchedAtEpoch = 0; uint8_t count = 0; Event ev[kMaxEvents]; };
// Parses the Apps Script payload. On success fills `out` (valid=true, fetchedAtEpoch = "t") and
// returns true; on malformed JSON / missing "ev" returns false and leaves `out` untouched.
bool parse(const char* json, size_t len, Snapshot& out);
// Index of the first non-all-day event whose end > nowEpoch, or -1.
int8_t nextTimed(const Snapshot& s, uint32_t nowEpoch);
// Index of the first event on a later local day than nowEpoch (used for "Tomorrow …"), or -1.
// `localDayOf(epoch)` is the caller-supplied day number (e.g. days since epoch in local time).
int8_t firstOnLaterDay(const Snapshot& s, uint32_t nowEpoch, uint32_t (*localDayOf)(uint32_t epoch));
// Writes one of: "in N min" (N < 60), "in H h MM" (>= 60 min), "now, N min left", "starts now"
// (|start - now| < 30 s, not yet ended). Returns chars written (0 if event has ended).
size_t countdownText(const Event& e, uint32_t nowEpoch, char* buf, size_t cap);
// Count of all-day events in the snapshot.
uint8_t allDayCount(const Snapshot& s);
bool isStale(const Snapshot& s, uint32_t nowEpoch);
}
```

- [ ] **Step 1: Write the failing tests — `test/test_calendar_model/test_main.cpp`**

```cpp
#include <unity.h>
#include <string.h>
#include "CalendarModel.h"

void setUp(void) {}
void tearDown(void) {}

static const char* kPayload =
    "{\"t\":1757260000,\"ev\":["
    "{\"s\":1757259000,\"e\":1757262600,\"n\":\"Standup\",\"a\":0,\"l\":\"Google Meet\"},"
    "{\"s\":1757246400,\"e\":1757332800,\"n\":\"Offsite\",\"a\":1,\"l\":\"\"},"
    "{\"s\":1757264400,\"e\":1757268000,\"n\":\"1:1 with a very long title that keeps going\",\"a\":0,\"l\":\"Room 4.02 on the fourth floor\"},"
    "{\"s\":1757340000,\"e\":1757343600,\"n\":\"Tomorrow first\",\"a\":0,\"l\":\"Zoom\"}"
    "]}";

static CalendarModel::Snapshot parsed(void)
{
    CalendarModel::Snapshot s;
    TEST_ASSERT_TRUE(CalendarModel::parse(kPayload, strlen(kPayload), s));
    return s;
}

static void test_parse_fillsSnapshot(void)
{
    CalendarModel::Snapshot s = parsed();
    TEST_ASSERT_TRUE(s.valid);
    TEST_ASSERT_EQUAL_UINT32(1757260000u, s.fetchedAtEpoch);
    TEST_ASSERT_EQUAL_UINT8(4, s.count);
    TEST_ASSERT_EQUAL_UINT32(1757259000u, s.ev[0].start);
    TEST_ASSERT_EQUAL_UINT32(1757262600u, s.ev[0].end);
    TEST_ASSERT_EQUAL_STRING("Standup", s.ev[0].title);
    TEST_ASSERT_EQUAL_STRING("Google Meet", s.ev[0].where);
    TEST_ASSERT_FALSE(s.ev[0].allDay);
    TEST_ASSERT_TRUE(s.ev[1].allDay);
}

static void test_parse_clipsTitleAndWhere(void)
{
    CalendarModel::Snapshot s = parsed();
    TEST_ASSERT_EQUAL_INT(40, (int)strlen(s.ev[2].title));
    TEST_ASSERT_EQUAL_INT(24, (int)strlen(s.ev[2].where));
}

static void test_parse_emptyListIsValidWithZeroEvents(void)
{
    CalendarModel::Snapshot s;
    const char* j = "{\"t\":1757260000,\"ev\":[]}";
    TEST_ASSERT_TRUE(CalendarModel::parse(j, strlen(j), s));
    TEST_ASSERT_TRUE(s.valid);
    TEST_ASSERT_EQUAL_UINT8(0, s.count);
}

static void test_parse_capsAtFiveEvents(void)
{
    const char* j = "{\"t\":1,\"ev\":[{\"s\":1,\"e\":2,\"n\":\"a\",\"a\":0,\"l\":\"\"},{\"s\":1,\"e\":2,\"n\":\"b\",\"a\":0,\"l\":\"\"},"
                    "{\"s\":1,\"e\":2,\"n\":\"c\",\"a\":0,\"l\":\"\"},{\"s\":1,\"e\":2,\"n\":\"d\",\"a\":0,\"l\":\"\"},"
                    "{\"s\":1,\"e\":2,\"n\":\"e\",\"a\":0,\"l\":\"\"},{\"s\":1,\"e\":2,\"n\":\"f\",\"a\":0,\"l\":\"\"}]}";
    CalendarModel::Snapshot s;
    TEST_ASSERT_TRUE(CalendarModel::parse(j, strlen(j), s));
    TEST_ASSERT_EQUAL_UINT8(5, s.count);
}

static void test_parse_malformedOrMissingEvLeavesOutUntouched(void)
{
    CalendarModel::Snapshot s;
    s.valid = true; s.count = 2; s.fetchedAtEpoch = 42;
    const char* bad = "{\"t\":5,\"ev\":[";
    TEST_ASSERT_FALSE(CalendarModel::parse(bad, strlen(bad), s));
    const char* noEv = "{\"error\":\"forbidden\"}";
    TEST_ASSERT_FALSE(CalendarModel::parse(noEv, strlen(noEv), s));
    TEST_ASSERT_EQUAL_UINT8(2, s.count);
    TEST_ASSERT_EQUAL_UINT32(42u, s.fetchedAtEpoch);
}

static void test_nextTimed_skipsAllDayAndFinished_inProgressWins(void)
{
    CalendarModel::Snapshot s = parsed();
    // Standup runs 1757259000..1757262600; at 1757260000 it is in progress -> index 0.
    TEST_ASSERT_EQUAL_INT8(0, CalendarModel::nextTimed(s, 1757260000u));
    // After Standup ends, the all-day Offsite is skipped -> the 1:1 (index 2).
    TEST_ASSERT_EQUAL_INT8(2, CalendarModel::nextTimed(s, 1757262601u));
    // After everything today -> tomorrow's (index 3); after that -> -1.
    TEST_ASSERT_EQUAL_INT8(3, CalendarModel::nextTimed(s, 1757268001u));
    TEST_ASSERT_EQUAL_INT8(-1, CalendarModel::nextTimed(s, 1757343601u));
}

static uint32_t fakeDay(uint32_t epoch) { return epoch / 86400u; }

static void test_firstOnLaterDay_findsTomorrow(void)
{
    CalendarModel::Snapshot s = parsed();
    TEST_ASSERT_EQUAL_INT8(3, CalendarModel::firstOnLaterDay(s, 1757260000u, fakeDay));
    TEST_ASSERT_EQUAL_INT8(-1, CalendarModel::firstOnLaterDay(s, 1757340000u, fakeDay));
}

static void test_countdownText_forms(void)
{
    char buf[32];
    CalendarModel::Event e;
    e.start = 10000; e.end = 13600; // 60 min long
    CalendarModel::countdownText(e, 10000u - 25 * 60, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("in 25 min", buf);
    CalendarModel::countdownText(e, 10000u - (2 * 3600 + 5 * 60), buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("in 2 h 05", buf);
    CalendarModel::countdownText(e, 10000u - 60 * 60, buf, sizeof(buf)); // exactly 60 min
    TEST_ASSERT_EQUAL_STRING("in 1 h 00", buf);
    CalendarModel::countdownText(e, 10000u - 20, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("starts now", buf);
    CalendarModel::countdownText(e, 10000u + 48 * 60, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("now, 12 min left", buf);
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)CalendarModel::countdownText(e, 13601u, buf, sizeof(buf)));
}

static void test_allDayCount_and_isStale(void)
{
    CalendarModel::Snapshot s = parsed();
    TEST_ASSERT_EQUAL_UINT8(1, CalendarModel::allDayCount(s));
    TEST_ASSERT_FALSE(CalendarModel::isStale(s, 1757260000u + 1799));
    TEST_ASSERT_TRUE(CalendarModel::isStale(s, 1757260000u + 1800));
    CalendarModel::Snapshot none;
    TEST_ASSERT_TRUE(CalendarModel::isStale(none, 1757260000u));
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_parse_fillsSnapshot);
    RUN_TEST(test_parse_clipsTitleAndWhere);
    RUN_TEST(test_parse_emptyListIsValidWithZeroEvents);
    RUN_TEST(test_parse_capsAtFiveEvents);
    RUN_TEST(test_parse_malformedOrMissingEvLeavesOutUntouched);
    RUN_TEST(test_nextTimed_skipsAllDayAndFinished_inProgressWins);
    RUN_TEST(test_firstOnLaterDay_findsTomorrow);
    RUN_TEST(test_countdownText_forms);
    RUN_TEST(test_allDayCount_and_isStale);
    return UNITY_END();
}
```

- [ ] **Step 2: Add `+<CalendarModel.cpp>` to the native `build_src_filter` in `platformio.ini`, run `~/Library/Python/3.9/bin/pio test -e native -f test_calendar_model 2>&1 | tail -5` → compile errors (header missing).**

- [ ] **Step 3: Implement `src/CalendarModel.h`** (the Interfaces block above, with `#pragma once`, `#include <stdint.h>`, `#include <stddef.h>` and a header comment naming spec 4.19) **and `src/CalendarModel.cpp`:**

```cpp
#include "CalendarModel.h"
#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>

namespace CalendarModel
{
namespace
{
void copyClipped(char* dst, size_t cap, const char* src)
{
    if (src == nullptr) { dst[0] = '\0'; return; }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}
} // namespace

bool parse(const char* json, size_t len, Snapshot& out)
{
    JsonDocument doc;
    if (deserializeJson(doc, json, len) != DeserializationError::Ok) return false;
    JsonArrayConst ev = doc["ev"].as<JsonArrayConst>();
    if (ev.isNull()) return false;
    Snapshot s;
    s.valid = true;
    s.fetchedAtEpoch = doc["t"] | 0u;
    for (JsonObjectConst o : ev)
    {
        if (s.count >= kMaxEvents) break;
        Event& e = s.ev[s.count];
        e.start  = o["s"] | 0u;
        e.end    = o["e"] | 0u;
        e.allDay = (o["a"] | 0) != 0;
        copyClipped(e.title, sizeof(e.title), o["n"] | "");
        copyClipped(e.where, sizeof(e.where), o["l"] | "");
        ++s.count;
    }
    out = s;
    return true;
}

int8_t nextTimed(const Snapshot& s, uint32_t nowEpoch)
{
    for (uint8_t i = 0; i < s.count; ++i)
        if (!s.ev[i].allDay && s.ev[i].end > nowEpoch) return (int8_t)i;
    return -1;
}

int8_t firstOnLaterDay(const Snapshot& s, uint32_t nowEpoch, uint32_t (*localDayOf)(uint32_t))
{
    const uint32_t today = localDayOf(nowEpoch);
    for (uint8_t i = 0; i < s.count; ++i)
        if (localDayOf(s.ev[i].start) > today) return (int8_t)i;
    return -1;
}

size_t countdownText(const Event& e, uint32_t nowEpoch, char* buf, size_t cap)
{
    if (buf == nullptr || cap == 0) return 0;
    if (nowEpoch >= e.end) { buf[0] = '\0'; return 0; }
    int n = 0;
    if (nowEpoch + 30 < e.start)
    {
        const uint32_t mins = (e.start - nowEpoch + 30) / 60; // round to nearest minute
        if (mins < 60) n = snprintf(buf, cap, "in %u min", (unsigned)mins);
        else n = snprintf(buf, cap, "in %u h %02u", (unsigned)(mins / 60), (unsigned)(mins % 60));
    }
    else if (nowEpoch < e.start + 30)
    {
        n = snprintf(buf, cap, "starts now");
    }
    else
    {
        const uint32_t left = (e.end - nowEpoch + 30) / 60;
        n = snprintf(buf, cap, "now, %u min left", (unsigned)left);
    }
    return (n < 0 || (size_t)n >= cap) ? 0 : (size_t)n;
}

uint8_t allDayCount(const Snapshot& s)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < s.count; ++i) if (s.ev[i].allDay) ++n;
    return n;
}

bool isStale(const Snapshot& s, uint32_t nowEpoch)
{
    return !s.valid || nowEpoch - s.fetchedAtEpoch >= kStaleSec;
}
} // namespace CalendarModel
```
(`"in 2 h 05"` for 125 min; `"in 1 h 00"` for exactly 60 — the test pins both. Check the `+30` rounding against the test values: 25 min → `(1500+30)/60 = 25`; 125 min → 125; 60 min → 60; 48 min in with 60 min length → left `(720+30)/60 = 12`.)

- [ ] **Step 4: Run** `~/Library/Python/3.9/bin/pio test -e native -f test_calendar_model 2>&1 | tail -4` → 9 PASS. Then the full suite → 333.

- [ ] **Step 5: Commit** `git add src/CalendarModel.h src/CalendarModel.cpp test/test_calendar_model/test_main.cpp platformio.ini` — `"Calendar: CalendarModel parse/next/countdown (spec 4.19)"` + Co-Authored-By line.

---

### Task 3: `CalendarAutoShow` (pure)

**Files:**
- Create: `src/CalendarAutoShow.h`, `src/CalendarAutoShow.cpp`
- Create: `test/test_calendar_auto_show/test_main.cpp`
- Modify: `platformio.ini` native filter (`+<CalendarAutoShow.cpp>`)

**Interfaces (Produces):**
```cpp
#include "CardRing.h"   // CardId (CardId::CALENDAR is added in Task 4; until then the tests use the
                        // symbol only via this header — Task 4 must land before the native suite
                        // compiles this test. Order tasks 3 and 4 accordingly: implement Task 4 FIRST
                        // if working out of order.)
class CalendarAutoShow {
public:
    enum class Action { NONE, SHOW_CALENDAR, RETURN_TO_CLOCK };
    void setEnabled(bool on);            // disabling forgets an in-progress episode
    void setLeadSec(uint32_t s);         // default 300
    void setStaySec(uint32_t s);         // default 60
    bool enabled() const;
    uint32_t leadSec() const; uint32_t staySec() const;
    // hasNext/nextStart describe CalendarModel::nextTimed() for the current snapshot; dataValid is
    // "snapshot valid and not stale". nowEpoch is TimeService time (0 when the clock is not valid ->
    // treated as !dataValid).
    Action update(uint32_t nowEpoch, bool hasNext, uint32_t nextStart, CardId currentCard,
                  bool beltIdle, bool dataValid);
    // Tap on the card during a nudge: returns true if an episode was active (caller switches to
    // Clock); that event's start is remembered and never nudges again.
    bool dismiss();
    void noteManualNavigation();         // forgets the episode without blocking the event
    bool isShowing() const;
private:
    bool m_enabled = true; uint32_t m_leadSec = 300; uint32_t m_staySec = 60;
    bool m_showing = false; uint32_t m_shownStart = 0; uint32_t m_dismissedStart = 0; bool m_haveDismissed = false;
};
```

- [ ] **Step 1: Tests — `test/test_calendar_auto_show/test_main.cpp`**

```cpp
#include <unity.h>
#include "CalendarAutoShow.h"
void setUp(void) {} void tearDown(void) {}
using A = CalendarAutoShow::Action;
static const uint32_t T0 = 1757260000u; // "now"
static const uint32_t START = T0 + 600;  // meeting in 10 min

static void test_noShow_beforeLead_thenShowAtLead(void)
{
    CalendarAutoShow n;
    TEST_ASSERT_TRUE(A::NONE == n.update(T0, true, START, CardId::CLOCK, true, true));           // 10 min out
    TEST_ASSERT_TRUE(A::NONE == n.update(START - 301, true, START, CardId::CLOCK, true, true));   // 5:01 out
    TEST_ASSERT_TRUE(A::SHOW_CALENDAR == n.update(START - 300, true, START, CardId::CLOCK, true, true));
    TEST_ASSERT_TRUE(n.isShowing());
    TEST_ASSERT_TRUE(A::NONE == n.update(START - 299, true, START, CardId::CALENDAR, true, true)); // no repeat
}

static void test_returnAfterStay(void)
{
    CalendarAutoShow n;
    n.update(START - 300, true, START, CardId::CLOCK, true, true);
    TEST_ASSERT_TRUE(A::NONE == n.update(START + 59, true, START, CardId::CALENDAR, true, true));
    TEST_ASSERT_TRUE(A::RETURN_TO_CLOCK == n.update(START + 60, true, START, CardId::CALENDAR, true, true));
    TEST_ASSERT_FALSE(n.isShowing());
    // The same event does not re-nudge after the return.
    TEST_ASSERT_TRUE(A::NONE == n.update(START + 61, true, START, CardId::CLOCK, true, true));
}

static void test_noShow_whenDisabled_beltBusy_notOnClock_orStale(void)
{
    CalendarAutoShow n;
    n.setEnabled(false);
    TEST_ASSERT_TRUE(A::NONE == n.update(START - 100, true, START, CardId::CLOCK, true, true));
    n.setEnabled(true);
    TEST_ASSERT_TRUE(A::NONE == n.update(START - 100, true, START, CardId::CLOCK, false, true));       // belt busy
    TEST_ASSERT_TRUE(A::NONE == n.update(START - 100, true, START, CardId::FLIGHTS, true, true));      // not on Clock
    TEST_ASSERT_TRUE(A::NONE == n.update(START - 100, true, START, CardId::CLOCK, true, false));       // stale
    TEST_ASSERT_TRUE(A::NONE == n.update(START - 100, false, 0, CardId::CLOCK, true, true));           // nothing next
    TEST_ASSERT_TRUE(A::SHOW_CALENDAR == n.update(START - 100, true, START, CardId::CLOCK, true, true));
}

static void test_dismiss_returnsNowAndBlocksOnlyThatEvent(void)
{
    CalendarAutoShow n;
    n.update(START - 300, true, START, CardId::CLOCK, true, true);
    TEST_ASSERT_TRUE(n.dismiss());
    TEST_ASSERT_FALSE(n.isShowing());
    TEST_ASSERT_TRUE(A::NONE == n.update(START - 200, true, START, CardId::CLOCK, true, true));
    const uint32_t NEXT = START + 3600;
    TEST_ASSERT_TRUE(A::SHOW_CALENDAR == n.update(NEXT - 300, true, NEXT, CardId::CLOCK, true, true));
    // dismiss() with nothing active returns false:
    CalendarAutoShow idle;
    TEST_ASSERT_FALSE(idle.dismiss());
}

static void test_manualNavigation_forgetsEpisode_noReturn(void)
{
    CalendarAutoShow n;
    n.update(START - 300, true, START, CardId::CLOCK, true, true);
    n.noteManualNavigation();
    TEST_ASSERT_FALSE(n.isShowing());
    TEST_ASSERT_TRUE(A::NONE == n.update(START + 120, true, START, CardId::LIGHT_LAMP, true, true));
}

static void test_leavingViaOtherCard_forgetsEpisode(void)
{
    CalendarAutoShow n;
    n.update(START - 300, true, START, CardId::CLOCK, true, true);
    TEST_ASSERT_TRUE(A::NONE == n.update(START - 100, true, START, CardId::FLIGHTS, true, true));
    TEST_ASSERT_FALSE(n.isShowing());
}

static void test_leadAndStaySetters(void)
{
    CalendarAutoShow n;
    n.setLeadSec(120); n.setStaySec(0);
    TEST_ASSERT_EQUAL_UINT32(120u, n.leadSec());
    TEST_ASSERT_TRUE(A::NONE == n.update(START - 121, true, START, CardId::CLOCK, true, true));
    TEST_ASSERT_TRUE(A::SHOW_CALENDAR == n.update(START - 120, true, START, CardId::CLOCK, true, true));
    TEST_ASSERT_TRUE(A::RETURN_TO_CLOCK == n.update(START, true, START, CardId::CALENDAR, true, true));
}

static void test_disableMidEpisode_forgets(void)
{
    CalendarAutoShow n;
    n.update(START - 300, true, START, CardId::CLOCK, true, true);
    n.setEnabled(false);
    TEST_ASSERT_FALSE(n.isShowing());
    TEST_ASSERT_TRUE(A::NONE == n.update(START + 120, true, START, CardId::CALENDAR, true, true));
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_noShow_beforeLead_thenShowAtLead);
    RUN_TEST(test_returnAfterStay);
    RUN_TEST(test_noShow_whenDisabled_beltBusy_notOnClock_orStale);
    RUN_TEST(test_dismiss_returnsNowAndBlocksOnlyThatEvent);
    RUN_TEST(test_manualNavigation_forgetsEpisode_noReturn);
    RUN_TEST(test_leavingViaOtherCard_forgetsEpisode);
    RUN_TEST(test_leadAndStaySetters);
    RUN_TEST(test_disableMidEpisode_forgets);
    return UNITY_END();
}
```
- [ ] **Step 2: Add `+<CalendarAutoShow.cpp>` to the native filter; run the focused test → compile failure.**

- [ ] **Step 3: Implement**

```cpp
// CalendarAutoShow.cpp
#include "CalendarAutoShow.h"
void CalendarAutoShow::setEnabled(bool on) { m_enabled = on; m_showing = false; }
void CalendarAutoShow::setLeadSec(uint32_t s) { m_leadSec = s; }
void CalendarAutoShow::setStaySec(uint32_t s) { m_staySec = s; }
bool CalendarAutoShow::enabled() const { return m_enabled; }
uint32_t CalendarAutoShow::leadSec() const { return m_leadSec; }
uint32_t CalendarAutoShow::staySec() const { return m_staySec; }
bool CalendarAutoShow::isShowing() const { return m_showing; }

CalendarAutoShow::Action CalendarAutoShow::update(uint32_t nowEpoch, bool hasNext, uint32_t nextStart,
                                                  CardId currentCard, bool beltIdle, bool dataValid)
{
    if (!m_enabled || nowEpoch == 0) return Action::NONE;
    if (m_showing && currentCard != CardId::CALENDAR)
    {
        // The user left some other way (belt started, menu, another card): the episode is over,
        // and the event it was for must not fire again.
        m_showing = false;
        m_dismissedStart = m_shownStart; m_haveDismissed = true;
        return Action::NONE;
    }
    if (m_showing)
    {
        if (nowEpoch >= m_shownStart + m_staySec)
        {
            m_showing = false;
            m_dismissedStart = m_shownStart; m_haveDismissed = true;
            return Action::RETURN_TO_CLOCK;
        }
        return Action::NONE;
    }
    if (!beltIdle || !dataValid || !hasNext || currentCard != CardId::CLOCK) return Action::NONE;
    if (m_haveDismissed && nextStart == m_dismissedStart) return Action::NONE;
    if (nextStart > nowEpoch + m_leadSec) return Action::NONE;
    m_showing = true;
    m_shownStart = nextStart;
    return Action::SHOW_CALENDAR;
}

bool CalendarAutoShow::dismiss()
{
    if (!m_showing) return false;
    m_showing = false;
    m_dismissedStart = m_shownStart; m_haveDismissed = true;
    return true;
}

void CalendarAutoShow::noteManualNavigation() { m_showing = false; }
```
Note `test_leavingViaOtherCard_forgetsEpisode` and `test_manualNavigation_forgetsEpisode_noReturn` both pass with this; `test_returnAfterStay`'s "no re-nudge after the return" passes because the return records the dismissal. This needs `CardId::CALENDAR` — do Task 4 first if it has not landed.

- [ ] **Step 4: Run focused (8 PASS) and full suite. Commit** `git add src/CalendarAutoShow.* test/test_calendar_auto_show/test_main.cpp platformio.ini` — `"Calendar: CalendarAutoShow nudge state machine (spec 4.19)"`.

---

### Task 4: `CardId::CALENDAR`, six-card menu, glyph and name

**Files:**
- Modify: `src/CardRing.h` (enum + ring comment), `src/CardMenu.h:56` (static_assert → 6), `src/CardMenu.cpp` (nothing else should need to change: `itemCentre`/`detents`/`hitTest` derive from `CardId::COUNT` — verify), `src/DialMenuView.cpp` (`cardName` → `"Calendar"`, glyph case), `src/DialGlyphs.h/.cpp` (`drawCalendarGlyph`), `src/DialUi.cpp` `cardToScreen`-style switch (~line 941: add `case CardId::CALENDAR: return Screen::CALENDAR;` — Screen::CALENDAR is added in Task 7; for THIS task map it to `Screen::CLOCK` with a `// Task 7 replaces` comment so the device build stays green)
- Test: `test/test_card_menu/test_main.cpp`, `test/test_card_ring/test_main.cpp`

**Interfaces (Produces):** `CardId::CALENDAR` (value 5, before `COUNT`), `void drawCalendarGlyph(LovyanGFX& gfx, int cx, int cy, int r, uint32_t colour)`, `cardName(CardId::CALENDAR) == "Calendar"`.

- [ ] **Step 1: Tests.** In `test/test_card_menu/test_main.cpp` update the wrap test (`1 + 7 = 8, mod 6 = 2 -> FLIGHTS`) and add:
```cpp
static void test_ring_hasSixItems_sixtyDegreesApart(void)
{
    TEST_ASSERT_EQUAL_UINT8(6, static_cast<uint8_t>(CardId::COUNT));
    int x0, y0, x1, y1;
    CardMenu::itemCentre(0, x0, y0); // Treadmill at 12 o'clock
    TEST_ASSERT_EQUAL_INT(120, x0);
    TEST_ASSERT_EQUAL_INT(120 - CardMenu::kRingRadius, y0);
    CardMenu::itemCentre(5, x1, y1); // Calendar at 10 o'clock: (-sin60, -cos60) * 88
    TEST_ASSERT_INT_WITHIN(1, 120 - 76, x1);
    TEST_ASSERT_INT_WITHIN(1, 120 - 44, y1);
    TEST_ASSERT_EQUAL_INT(5, CardMenu::hitTest(x1, y1));
}
```
In `test/test_card_ring/test_main.cpp` update any test that walks the ring end to end so the sequence is TREADMILL → CLOCK → FLIGHTS → LIGHT_OFFICE → LIGHT_LAMP → CALENDAR → TREADMILL (read the file; adjust the wrap expectations). Run both focused suites → failures.

- [ ] **Step 2: Implement.** `CardRing.h`: add `CALENDAR,` before `COUNT` and extend the ring comment. `CardMenu.h:56`: `static_assert(... == 6, "CardMenu ring assumes 6 cards")`. `DialGlyphs.h/.cpp`:
```cpp
// Calendar: a page with a thicker date bar across the top and two binder rings.
void drawCalendarGlyph(LovyanGFX& gfx, int cx, int cy, int r, uint32_t colour)
{
    const int w = r * 2 - 4, h = r * 2 - 6;
    const int x0 = cx - w / 2, y0 = cy - h / 2 + 2;
    gfx.drawRoundRect(x0, y0, w, h, 3, colour);
    gfx.fillRect(x0, y0, w, 5, colour);                    // date bar
    gfx.fillRect(x0 + w / 4 - 1, y0 - 3, 2, 6, colour);    // ring 1
    gfx.fillRect(x0 + (3 * w) / 4 - 1, y0 - 3, 2, 6, colour); // ring 2
    // two rows of "days"
    for (int i = 0; i < 3; ++i) gfx.fillRect(x0 + 3 + i * (w / 3), y0 + 9, 3, 3, colour);
    for (int i = 0; i < 3; ++i) gfx.fillRect(x0 + 3 + i * (w / 3), y0 + 15, 3, 3, colour);
}
```
`DialMenuView.cpp`: `case CardId::CALENDAR: return "Calendar";` and `case CardId::CALENDAR: drawCalendarGlyph(gfx, x, y, r, colour); break;`. `DialUi.cpp` card→screen switch: `case CardId::CALENDAR: return Screen::CLOCK; // Task 7 gives Calendar its own Screen`. Check `CardMenu.cpp` for any literal 5/72° and replace with `CardId::COUNT`-derived maths.

- [ ] **Step 3: Native suite passes; `pio run -e dial-ota` passes. Commit** — `"Cards: CardId::CALENDAR, six-item menu ring, calendar glyph (spec 4.19)"`.

---

### Task 5: Settings — NVS, Commands, HA entities

**Files:**
- Modify: `src/TreadmillHandler.h/.cpp` (settings block ~line 262–320: `m_calendarNudge`, `m_calendarLeadMin`, `m_calendarStayMin`, getters/setters with `saveSettings()`, NVS keys `cal_nudge`/`cal_lead`/`cal_stay`), `src/Commands.h` (`SET_CALENDAR_NUDGE`, `SET_CALENDAR_LEAD`, `SET_CALENDAR_STAY`; `CALENDAR_NUDGE`, `CALENDAR_LEAD`, `CALENDAR_STAY`), `src/mqttview.h/.cpp` (`MqttSwitch m_calendarNudge("calendar-nudge","Calendar nudge", icon mdi:calendar-clock)`, `MqttNumber m_calendarLead("calendar-lead","Calendar lead", min 1 max 30 step 1 unit "min", mdi:timer-sand)`, `MqttNumber m_calendarStay("calendar-stay","Calendar stay", min 0 max 10 step 1 unit "min")`, all `EntityCategory::CONFIG`, registered in `publishConfig` and state-publish paths like `m_flightsAutoShow` / `m_idleDisconnectMins`), `src/NetTask.cpp` (command parsing next to `SET_FLIGHTS_AUTO_SHOW` ~line 610 and the number parsing ~line 568; publish cases next to `PubType::FLIGHTS_AUTO_SHOW` ~line 234), `src/main.cpp` `drainCommands()` (three cases mirroring `SET_FLIGHTS_AUTO_SHOW`, calling `treadmill.setCalendarNudge/LeadMin/StayMin`, then `enqueue(PubType::CALENDAR_*)`; the `dialUi.setCalendar*` calls are added in Task 7 — leave a `// Task 7: dialUi.setCalendarNudge(cmd.b)` comment), boot publish of the three settings wherever `FLIGHTS_AUTO_SHOW` is first published after MQTT connect.

**Interfaces (Produces):** `bool TreadmillHandler::getCalendarNudge() const; uint16_t getCalendarLeadMin() const; uint16_t getCalendarStayMin() const; void setCalendarNudge(bool); void setCalendarLeadMin(uint16_t); void setCalendarStayMin(uint16_t);` (setters clamp 1–30 / 0–10, persist, `log_i`). `Command.b` carries the switch, `Command.u16` the minutes.

- [ ] **Step 1: Implement** by copying the `flightsAutoShow` (switch) and `idleDisconnectMins` (number) patterns end to end: NVS load with defaults `true / 5 / 1`, `saveSettings()` puts, MqttView entity construction + config + state publish, NetTask topic → Command, main.cpp dispatch + publish. These settings are NOT behind `HAS_CALENDAR` (HA sees them even on a build without the card; harmless and keeps discovery stable).

- [ ] **Step 2: Build** `pio run -e dial-ota` → SUCCESS; native suite unchanged. Verify with `grep -n "cal_nudge\|cal_lead\|cal_stay" src/TreadmillHandler.cpp` (6 hits: 3 loads, 3 saves).

- [ ] **Step 3: Commit** — `"Calendar: nudge/lead/stay settings in NVS and HA (spec 4.19)"`.

---

### Task 6: `CalendarService` on the net task

**Files:**
- Create: `src/CalendarService.h`, `src/CalendarService.cpp`, `src/CalendarCerts.h`
- Modify: `src/board.h` (`#if defined(CALENDAR_URL)` → `#define HAS_CALENDAR 1` else `0`; `board.h` must include `config.h` first — check how `FLIGHTS_LOGO_BASE_URL` reaches code and follow it), `src/NetTask.h/.cpp` (member `CalendarService m_calendar` under `#if HAS_CALENDAR`, `begin()`, `tick(millis())` next to `m_flights.tick`, accessor `CalendarService& calendar()`)

**Interfaces (Produces):**
```cpp
#if HAS_CALENDAR
class CalendarService {
public:
    static constexpr uint32_t kPollMs = 300000, kBackoffMinMs = 30000, kBackoffMaxMs = 1800000;
    static constexpr size_t   kBufCap = 2048;
    explicit CalendarService(NetManager& net);
    void begin();
    void tick(uint32_t nowMs);                      // net task only
    // Loop task asks for an early refresh (nudge due within 60 s); honoured if the last fetch is > 60 s old.
    void requestRefresh();
    CalendarModel::Snapshot snapshot() const;       // Guarded copy
    bool fetchedOnce() const;
private:
    bool fetchNow(uint32_t nowMs);
    NetManager& m_net; Guarded<CalendarModel::Snapshot> m_snapshot;
    uint32_t m_nextFetchMs = 0, m_backoffMs = 0; bool m_fetchedOnce = false; volatile bool m_refreshWanted = false;
    static uint8_t s_buf[kBufCap];
};
#endif
```

- [ ] **Step 1: `src/CalendarCerts.h`** — `static const char kCalendarRootCAs[] PROGMEM = R"(-----BEGIN CERTIFICATE----- ... GTS Root R1 ... -----END CERTIFICATE-----\n-----BEGIN CERTIFICATE----- ... GTS Root R4 ... -----END CERTIFICATE-----\n)";` Obtain the PEMs from https://pki.goog/repository/ (GTS Root R1: serial 02 03 e5 93 6f 31 b0 13 49 88 6b a2 17, valid to 2036-06-22; GTS Root R4 likewise). Verify each with `openssl x509 -noout -subject -enddate` before pasting; the implementer downloads `https://pki.goog/repo/certs/gtsr1.pem` and `gtsr4.pem` with curl.

- [ ] **Step 2: `CalendarService.cpp`** — `tick()`: return unless `m_net.status() >= NetStatus::WIFI_UP` (use whatever NetManager exposes; FlightsService used the same gate); if `m_refreshWanted` and last fetch older than 60 s, or `nowMs >= m_nextFetchMs`, call `fetchNow`. `fetchNow`: heap guard (`ESP.getFreeHeap() < 60*1024 || ESP.getMaxAllocHeap() < 20*1024` → `log_w("Calendar: skipping fetch, heap free=%u largest=%u")`, retry in 60 s, return false); build URL `CALENDAR_URL "?k=" CALENDAR_TOKEN`; `WiFiClientSecure client; #ifdef CALENDAR_TLS_INSECURE client.setInsecure(); #else client.setCACert(kCalendarRootCAs); #endif`; `HTTPClient http; http.setConnectTimeout(5000); http.setTimeout(8000); http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); http.setReuse(false); http.begin(client, url); int code = http.GET();` — on 200 read up to `kBufCap-1` bytes from `http.getStream()` with a deadline, NUL-terminate, `CalendarModel::parse`; on success `m_snapshot.write(s)`, `m_fetchedOnce = true`, `m_backoffMs = 0`, `m_nextFetchMs = nowMs + kPollMs`, `log_i("Calendar: %u events, %d bytes", ...)`; on any failure `m_backoffMs = m_backoffMs ? min(m_backoffMs*2, kBackoffMaxMs) : kBackoffMinMs; m_nextFetchMs = nowMs + m_backoffMs; log_w("Calendar: fetch failed code=%d (%s), retry in %u s", ...)`. Always `http.end()`. Log the stack high-water mark once after the first fetch as FlightsService did.

- [ ] **Step 3: NetTask wiring** under `#if HAS_CALENDAR`; `board.h` define. Build twice: with `CALENDAR_URL` defined in `src/config.h` (add the real values Mike will supply, or placeholders `"https://script.google.com/macros/s/x/exec"` / `"x"` locally — never commit config.h) and with it commented out. Both `SUCCESS`. Confirm `Heap:` cost is acceptable by reasoning: 2 KB static buffer + TLS session only during fetch.

- [ ] **Step 4: Commit** — `"Calendar: CalendarService TLS fetch on the net task, GTS roots, back-off (spec 4.19)"`.

---

### Task 7: `DialUi` — Calendar screen, view, nudge, settings hooks

**Files:**
- Create: `src/DialCalendarView.h`, `src/DialCalendarView.cpp`
- Modify: `src/DialUi.h` (`Screen::CALENDAR`; members `CalendarService& m_calendar; CalendarModel::Snapshot m_calSnap; bool m_haveCalSnap; uint32_t m_lastCalSnapMs; CalendarAutoShow m_calNudge;` under `#if HAS_CALENDAR`; `FrameKey` gains `uint16_t calHash`; setters `setCalendarNudge(bool)`, `setCalendarLead(uint16_t min)`, `setCalendarStay(uint16_t min)`), `src/DialUi.cpp` (constructor binds `net.calendar()`; `pollCalendar(nowMs)` every 1000 ms like `pollFlights`; nudge block next to the Flights auto-show block; card→screen `CardId::CALENDAR → Screen::CALENDAR`; `handleInput`: tap on CALENDAR → `if (m_calNudge.dismiss()) { m_cards.set(CardId::CLOCK); }`; `navigateToCard()` and the menu/hold paths call `m_calNudge.noteManualNavigation()` alongside `m_autoShow.noteManualNavigation()`; draw dispatch `case Screen::CALENDAR: drawCalendarCard(gfx, m_theme, m_calSnap, nowEpoch, localTimeFn, m_calendar.fetchedOnce());`; the belt-idle/desk-card screen lists that mention `Screen::FLIGHTS` get `Screen::CALENDAR` too (grep `Screen::FLIGHTS ||`)), `src/main.cpp` (Task 5's three commented hooks → real `dialUi.setCalendar*` calls under `#if HAS_DIAL_UI && HAS_CALENDAR`; on boot after settings load, push the persisted values into DialUi the same way flights auto-show is pushed)
- Modify: `src/DialUi.h` comment for the desk cards list.

**Interfaces:**
- Consumes: Tasks 2, 3, 4, 5, 6.
- Produces: `void drawCalendarCard(LovyanGFX& gfx, const DialTheme& theme, const CalendarModel::Snapshot& s, uint32_t nowEpoch, bool (*localTime)(uint32_t epoch, struct tm& out), bool fetchedOnce);`

- [ ] **Step 1: `DialCalendarView.cpp`** (device-only, `#if HAS_DIAL_UI && HAS_CALENDAR`):
  - `hhmm(epoch, buf)` via the `localTime` callback → `"%02d:%02d"`.
  - `!fetchedOnce` → name + `waiting for calendar` (Font4, DIM) centred.
  - `CalendarModel::isStale(s, nowEpoch)` → name + `no calendar` (Font4, DIM) at centre + `last update N min ago` (Font2, DIM) at y 150 when `s.valid`.
  - all-day line at y 40 when `allDayCount > 0`: `All day: <title>` or `All day: N events` (Font2, DIM).
  - `idx = nextTimed(s, nowEpoch)`: if `< 0` → `nothing more today` (Font4, DIM) at centre; `j = firstOnLaterDay(...)` (localDayOf derived from `localTime`) → `Tomorrow HH:MM Title` (Font2, DIM) at y 150. Else: start `HH:MM` Font4 at y 62; title wrapped into ≤ 2 lines of ≤ 16 chars at word boundaries (break a single long word hard), Font4, centred on y 108 (one line → y 108; two → y 96 and y 120), ellipsis `…` replaced by `..` if the font lacks it; countdown at y 150 in `PENDING` when `nowEpoch >= start` else `TEXT`; then following timed events (skip all-day) at y 176/192/208 as `HH:MM ` + title clipped to 20 chars, Font2 DIM.
  - `gfx.setTextDatum(middle_center)` like the other views; name drawn at the same `kNameY` the Lights view uses.

- [ ] **Step 2: `DialUi` wiring** as listed. `pollCalendar`: copy `m_calendar.snapshot()` at most every 1000 ms; compute `nowEpoch = m_time.valid() ? (uint32_t)time(nullptr) : 0`; `dataValid = m_calSnap.valid && !isStale(m_calSnap, nowEpoch) && nowEpoch != 0`; `idx = nextTimed(...)`; `action = m_calNudge.update(nowEpoch, idx >= 0, idx >= 0 ? m_calSnap.ev[idx].start : 0, m_cards.current(), beltIdle, dataValid)`; `SHOW_CALENDAR → m_cards.set(CardId::CALENDAR)`; `RETURN_TO_CLOCK → m_cards.set(CardId::CLOCK)`. Early refresh: if `idx >= 0 && dataValid && m_calSnap.ev[idx].start - nowEpoch <= m_calNudge.leadSec() + 60` → `m_calendar.requestRefresh()` (the service rate-limits it). `FrameKey.calHash` = FNV-1a over `(valid, fetchedAtEpoch, count, nowEpoch/60, ev[i].start for i<count, fetchedOnce, nudge showing)` when the Calendar screen is showing, else 0 — so the countdown redraws once a minute. `setCalendarNudge/Lead/Stay` forward to `m_calNudge` (lead/stay in seconds = min × 60).

- [ ] **Step 3: Build both ways** (with/without `CALENDAR_URL`); native suite unchanged. **Commit** — `"Calendar card: face, nudge, settings hooks (spec 4.19)"`.

---

### Task 8: Docs — README, audit Phase L, spec as-built

**Files:** `README.md` (desk cards list: add Calendar; a short "Calendar" section pointing at `doc/CALENDAR_APPS_SCRIPT.md`; HA entities table: the three new settings), `doc/AUDIT_2026-09-03.md` (append `## Phase L hardware checklist (calendar)` — the Success bullet of §4.19 as checkboxes, plus `Heap:` stays above 80 KB across ten fetches and the belt link survives them), spec §4.19 (`**As built:**` bullet only if anything deviated).

- [ ] **Step 1: Edit the three docs. Step 2: Commit** — `"Docs: Calendar card, Phase L checklist"`.

---

## Self-review notes

- Coverage: script/deploy → T1; model → T2; nudge → T3; card/menu/glyph → T4; settings → T5; fetch/TLS/back-off/heap guard → T6; face/nudge integration/FrameKey/early refresh → T7; docs/Phase L → T8.
- Order: T4 before T3's test compiles (`CardId::CALENDAR`); execute 1, 2, 4, 3, 5, 6, 7, 8. The orchestrator dispatches in that order.
- Names used across tasks: `CalendarModel::{parse,nextTimed,firstOnLaterDay,countdownText,allDayCount,isStale,Snapshot,Event,kMaxEvents,kStaleSec}`, `CalendarAutoShow::{update,dismiss,noteManualNavigation,setEnabled,setLeadSec,setStaySec,isShowing,leadSec,staySec}`, `CalendarService::{begin,tick,requestRefresh,snapshot,fetchedOnce}`, `NetTask::calendar()`, `drawCalendarGlyph`, `drawCalendarCard`, `HAS_CALENDAR`, `CardId::CALENDAR`, `Screen::CALENDAR`, `TreadmillHandler::{get,set}Calendar{Nudge,LeadMin,StayMin}`, `CmdType::SET_CALENDAR_{NUDGE,LEAD,STAY}`, `PubType::CALENDAR_{NUDGE,LEAD,STAY}` — consistent.
