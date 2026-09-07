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
