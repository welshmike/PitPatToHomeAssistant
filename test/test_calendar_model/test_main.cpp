#include <unity.h>
#include <stdio.h>
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

static const char* kTomorrowMixedPayload =
    "{\"t\":1757260000,\"ev\":["
    "{\"s\":1757340000,\"e\":1757343600,\"n\":\"Tomorrow AllDay\",\"a\":1,\"l\":\"\"},"
    "{\"s\":1757344000,\"e\":1757347600,\"n\":\"Tomorrow Timed\",\"a\":0,\"l\":\"\"}"
    "]}";

// Events 0-2 fall on fakeDay 20338, event 3 ("Tomorrow first") on 20339.
static void test_nextTimedToday_stopsAtTheDayBoundary(void)
{
    CalendarModel::Snapshot s = parsed();
    // Standup is in progress at 1757260000 -> index 0.
    TEST_ASSERT_EQUAL_INT8(0, CalendarModel::nextTimedToday(s, 1757260000u, fakeDay));
    // After Standup ends the all-day Offsite is skipped -> the 1:1 (index 2).
    TEST_ASSERT_EQUAL_INT8(2, CalendarModel::nextTimedToday(s, 1757262601u, fakeDay));
    // After the 1:1 ends there is nothing more *today*: tomorrow's event is
    // excluded, unlike nextTimed(), which reports it (index 3).
    TEST_ASSERT_EQUAL_INT8(-1, CalendarModel::nextTimedToday(s, 1757268001u, fakeDay));
    TEST_ASSERT_EQUAL_INT8(3, CalendarModel::nextTimed(s, 1757268001u));
}

static void test_allDayToday_countsOnlyTodaysAllDayEvents(void)
{
    CalendarModel::Snapshot s = parsed();
    // The all-day Offsite starts 1757246400, which is fakeDay 20338 = today.
    TEST_ASSERT_EQUAL_UINT8(1, CalendarModel::allDayCountToday(s, 1757260000u, fakeDay));
    TEST_ASSERT_EQUAL_INT8(1, CalendarModel::firstAllDayToday(s, 1757260000u, fakeDay));
    // Standing on day 20339 it is yesterday's, and there is no all-day event
    // of its own that day.
    TEST_ASSERT_EQUAL_UINT8(0, CalendarModel::allDayCountToday(s, 1757340000u, fakeDay));
    TEST_ASSERT_EQUAL_INT8(-1, CalendarModel::firstAllDayToday(s, 1757340000u, fakeDay));
}

static void test_allDayToday_findsTomorrowsAllDayOnItsOwnDay(void)
{
    CalendarModel::Snapshot s;
    TEST_ASSERT_TRUE(CalendarModel::parse(kTomorrowMixedPayload, strlen(kTomorrowMixedPayload), s));
    // Index 0 is an all-day event on day 20339: none today, one that day.
    TEST_ASSERT_EQUAL_UINT8(0, CalendarModel::allDayCountToday(s, 1757260000u, fakeDay));
    TEST_ASSERT_EQUAL_UINT8(1, CalendarModel::allDayCountToday(s, 1757340000u, fakeDay));
    TEST_ASSERT_EQUAL_INT8(0, CalendarModel::firstAllDayToday(s, 1757340000u, fakeDay));
}

static const char* kTomorrowAllDayOnlyPayload =
    "{\"t\":1757260000,\"ev\":["
    "{\"s\":1757340000,\"e\":1757343600,\"n\":\"Tomorrow AllDay\",\"a\":1,\"l\":\"\"}"
    "]}";

static void test_firstTimedOnLaterDay_skipsAllDay(void)
{
    CalendarModel::Snapshot s;
    TEST_ASSERT_TRUE(CalendarModel::parse(kTomorrowMixedPayload, strlen(kTomorrowMixedPayload), s));
    // Index 0 (tomorrow) is all-day and must be skipped; index 1 (also
    // tomorrow, timed) is the one that should be reported.
    TEST_ASSERT_EQUAL_INT8(1, CalendarModel::firstTimedOnLaterDay(s, 1757260000u, fakeDay));
}

static void test_firstTimedOnLaterDay_allDayOnlyIsMinusOne(void)
{
    CalendarModel::Snapshot s;
    TEST_ASSERT_TRUE(
        CalendarModel::parse(kTomorrowAllDayOnlyPayload, strlen(kTomorrowAllDayOnlyPayload), s));
    TEST_ASSERT_EQUAL_INT8(-1, CalendarModel::firstTimedOnLaterDay(s, 1757260000u, fakeDay));
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

static uint32_t fakeDay2(uint32_t epoch) { return epoch / 86400u; }

// Writes "HH:MM" from epoch % 86400, same shape as the real hhmm() but
// timezone-free so the tests can use plain arithmetic epochs.
static void fakeHhmm(uint32_t epoch, char* out, size_t cap)
{
    const uint32_t sod = epoch % 86400u;
    snprintf(out, cap, "%02u:%02u", (unsigned)(sod / 3600u), (unsigned)((sod % 3600u) / 60u));
}

// Day 0 events: Standup 09:30-10:00 (34200-36000). Day 1: Later, timed
// (120600-122400) and an all-day event, so day-0-only and all-day-skip both
// get exercised.
static const char* kClockPayload =
    "{\"t\":1000000,\"ev\":["
    "{\"s\":34200,\"e\":36000,\"n\":\"Standup\",\"a\":0,\"l\":\"\"},"
    "{\"s\":120600,\"e\":122400,\"n\":\"Later\",\"a\":0,\"l\":\"\"}"
    "]}";

static const char* kClockAllDayOnlyPayload =
    "{\"t\":1000000,\"ev\":["
    "{\"s\":0,\"e\":86400,\"n\":\"Offsite\",\"a\":1,\"l\":\"\"}"
    "]}";

static void test_clockLine_todaysNextEvent_notYetLive25MinBefore(void)
{
    CalendarModel::Snapshot s;
    TEST_ASSERT_TRUE(CalendarModel::parse(kClockPayload, strlen(kClockPayload), s));
    char title[48];
    char when[8];
    bool live = true;
    const size_t n = CalendarModel::clockLine(s, 34200u - 25 * 60, fakeDay2, fakeHhmm, title,
                                               sizeof(title), when, sizeof(when), live);
    TEST_ASSERT_EQUAL_STRING("Standup", title);
    TEST_ASSERT_EQUAL_STRING("09:30", when);
    TEST_ASSERT_EQUAL_UINT(strlen(title), n);
    TEST_ASSERT_FALSE(live);
}

static void test_clockLine_liveWithin30sAndDuring(void)
{
    CalendarModel::Snapshot s;
    TEST_ASSERT_TRUE(CalendarModel::parse(kClockPayload, strlen(kClockPayload), s));
    char title[48];
    char when[8];
    bool live = false;

    // 10 s before the start.
    CalendarModel::clockLine(s, 34200u - 10, fakeDay2, fakeHhmm, title, sizeof(title), when,
                              sizeof(when), live);
    TEST_ASSERT_EQUAL_STRING("Standup", title);
    TEST_ASSERT_EQUAL_STRING("09:30", when);
    TEST_ASSERT_TRUE(live);

    // In progress.
    live = false;
    CalendarModel::clockLine(s, 35000u, fakeDay2, fakeHhmm, title, sizeof(title), when,
                              sizeof(when), live);
    TEST_ASSERT_EQUAL_STRING("Standup", title);
    TEST_ASSERT_EQUAL_STRING("09:30", when);
    TEST_ASSERT_TRUE(live);
}

static void test_clockLine_afterTodaysLastEvent_ignoresTomorrow(void)
{
    CalendarModel::Snapshot s;
    TEST_ASSERT_TRUE(CalendarModel::parse(kClockPayload, strlen(kClockPayload), s));
    char title[48] = "unset";
    char when[8] = "unset";
    bool live = true;
    const size_t n = CalendarModel::clockLine(s, 36001u, fakeDay2, fakeHhmm, title, sizeof(title),
                                               when, sizeof(when), live);
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)n);
    TEST_ASSERT_EQUAL_STRING("", title);
    TEST_ASSERT_EQUAL_STRING("", when);
    TEST_ASSERT_FALSE(live);
    // nextTimed() (day-blind) would still find tomorrow's event; clockLine's
    // nextTimedToday() must not.
    TEST_ASSERT_EQUAL_INT8(1, CalendarModel::nextTimed(s, 36001u));
}

static void test_clockLine_allDayOnlyToday_isZero(void)
{
    CalendarModel::Snapshot s;
    TEST_ASSERT_TRUE(
        CalendarModel::parse(kClockAllDayOnlyPayload, strlen(kClockAllDayOnlyPayload), s));
    char title[48];
    char when[8];
    bool live = true;
    const size_t n = CalendarModel::clockLine(s, 40000u, fakeDay2, fakeHhmm, title, sizeof(title),
                                               when, sizeof(when), live);
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)n);
    TEST_ASSERT_EQUAL_STRING("", title);
    TEST_ASSERT_EQUAL_STRING("", when);
    TEST_ASSERT_FALSE(live);
}

static void test_clockLine_invalidSnapshot_isZero(void)
{
    CalendarModel::Snapshot s; // default: valid=false, count=0
    char title[48];
    char when[8];
    bool live = true;
    const size_t n = CalendarModel::clockLine(s, 34200u, fakeDay2, fakeHhmm, title, sizeof(title),
                                               when, sizeof(when), live);
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)n);
    TEST_ASSERT_EQUAL_STRING("", title);
    TEST_ASSERT_EQUAL_STRING("", when);
    TEST_ASSERT_FALSE(live);
}

static void test_clockLine_longTitleTruncatesSafelyIntoSmallBuffers(void)
{
    const char* payload =
        "{\"t\":1000000,\"ev\":["
        "{\"s\":34200,\"e\":36000,"
        "\"n\":\"1234567890123456789012345678901234567890\",\"a\":0,\"l\":\"\"}"
        "]}";
    CalendarModel::Snapshot s;
    TEST_ASSERT_TRUE(CalendarModel::parse(payload, strlen(payload), s));
    TEST_ASSERT_EQUAL_INT(40, (int)strlen(s.ev[0].title));

    // Generous buffers: the 40-char title and "09:30" pass through untrimmed.
    char bigTitle[64];
    char bigWhen[8];
    bool live = false;
    size_t n = CalendarModel::clockLine(s, 34200u, fakeDay2, fakeHhmm, bigTitle, sizeof(bigTitle),
                                         bigWhen, sizeof(bigWhen), live);
    TEST_ASSERT_EQUAL_STRING("1234567890123456789012345678901234567890", bigTitle);
    TEST_ASSERT_EQUAL_STRING("09:30", bigWhen);
    TEST_ASSERT_EQUAL_UINT(strlen(bigTitle), n);

    // Tight title buffer (cap 16): truncated, NUL-terminated, returns what
    // fits, never overflows. Time buffer stays generous here.
    char smallTitle[16];
    n = CalendarModel::clockLine(s, 34200u, fakeDay2, fakeHhmm, smallTitle, sizeof(smallTitle),
                                  bigWhen, sizeof(bigWhen), live);
    TEST_ASSERT_EQUAL_UINT(15, n);
    TEST_ASSERT_EQUAL_UINT(15, strlen(smallTitle));
    TEST_ASSERT_EQUAL_UINT('\0', smallTitle[15]);
    TEST_ASSERT_EQUAL_STRING("09:30", bigWhen);

    // Tight time buffer (cap 4): truncated, NUL-terminated, never overflows.
    // Title buffer stays generous here.
    char smallWhen[4];
    n = CalendarModel::clockLine(s, 34200u, fakeDay2, fakeHhmm, bigTitle, sizeof(bigTitle),
                                  smallWhen, sizeof(smallWhen), live);
    TEST_ASSERT_EQUAL_STRING("1234567890123456789012345678901234567890", bigTitle);
    TEST_ASSERT_EQUAL_UINT(3, strlen(smallWhen));
    TEST_ASSERT_EQUAL_UINT('\0', smallWhen[3]);
}

static void test_allDayCount_and_isStale(void)
{
    CalendarModel::Snapshot s = parsed();
    TEST_ASSERT_EQUAL_UINT8(1, CalendarModel::allDayCount(s));
    TEST_ASSERT_FALSE(CalendarModel::isStale(s, 1757260000u + 1799));
    TEST_ASSERT_TRUE(CalendarModel::isStale(s, 1757260000u + 1800));
    CalendarModel::Snapshot none;
    TEST_ASSERT_TRUE(CalendarModel::isStale(none, 1757260000u));
    // A device clock a little behind the feed's "t" must not underflow into a
    // huge age and report a snapshot that has only just arrived as stale.
    TEST_ASSERT_FALSE(CalendarModel::isStale(s, 1757260000u - 5));
    TEST_ASSERT_FALSE(CalendarModel::isStale(s, 1757260000u));
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
    RUN_TEST(test_nextTimedToday_stopsAtTheDayBoundary);
    RUN_TEST(test_allDayToday_countsOnlyTodaysAllDayEvents);
    RUN_TEST(test_allDayToday_findsTomorrowsAllDayOnItsOwnDay);
    RUN_TEST(test_firstTimedOnLaterDay_skipsAllDay);
    RUN_TEST(test_firstTimedOnLaterDay_allDayOnlyIsMinusOne);
    RUN_TEST(test_countdownText_forms);
    RUN_TEST(test_clockLine_todaysNextEvent_notYetLive25MinBefore);
    RUN_TEST(test_clockLine_liveWithin30sAndDuring);
    RUN_TEST(test_clockLine_afterTodaysLastEvent_ignoresTomorrow);
    RUN_TEST(test_clockLine_allDayOnlyToday_isZero);
    RUN_TEST(test_clockLine_invalidSnapshot_isZero);
    RUN_TEST(test_clockLine_longTitleTruncatesSafelyIntoSmallBuffers);
    RUN_TEST(test_allDayCount_and_isStale);
    return UNITY_END();
}
