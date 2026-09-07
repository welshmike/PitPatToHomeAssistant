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
