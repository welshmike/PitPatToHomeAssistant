#include <unity.h>

#include "CardRing.h"
#include "FlightsAutoShow.h"

void setUp(void) {}
void tearDown(void) {}

using Action = FlightsAutoShow::Action;

static void test_showOnZeroToOneFromClock(void)
{
    FlightsAutoShow show;
    TEST_ASSERT_EQUAL(Action::NONE, show.update(0, CardId::CLOCK, true));
    TEST_ASSERT_EQUAL(Action::SHOW_FLIGHTS, show.update(1, CardId::CLOCK, true));
}

static void test_noShowFromLightOffice(void)
{
    FlightsAutoShow show;
    show.update(0, CardId::LIGHT_OFFICE, true);
    TEST_ASSERT_EQUAL(Action::NONE, show.update(1, CardId::LIGHT_OFFICE, true));
}

static void test_noShowFromTreadmill(void)
{
    FlightsAutoShow show;
    show.update(0, CardId::TREADMILL, true);
    TEST_ASSERT_EQUAL(Action::NONE, show.update(1, CardId::TREADMILL, true));
}

static void test_noShowWhileBeltNotIdle(void)
{
    FlightsAutoShow show;
    show.update(0, CardId::CLOCK, true);
    TEST_ASSERT_EQUAL(Action::NONE, show.update(1, CardId::CLOCK, false));
}

static void test_returnOnZero_onlyWhenAutoShown(void)
{
    // Never auto-shown (arrived at Flights without a Clock 0->>0 edge, e.g.
    // manual knob navigation): dropping to 0 must not force a return.
    FlightsAutoShow notAutoShown;
    notAutoShown.update(5, CardId::FLIGHTS, true);
    TEST_ASSERT_EQUAL(Action::NONE, notAutoShown.update(0, CardId::FLIGHTS, true));

    // Auto-shown: dropping to 0 while still on Flights returns to Clock.
    FlightsAutoShow autoShown;
    autoShown.update(0, CardId::CLOCK, true);
    TEST_ASSERT_EQUAL(Action::SHOW_FLIGHTS, autoShown.update(1, CardId::CLOCK, true));
    TEST_ASSERT_EQUAL(Action::RETURN_TO_CLOCK, autoShown.update(0, CardId::FLIGHTS, true));
}

static void test_manualNavigationCancelsReturn(void)
{
    FlightsAutoShow show;
    show.update(0, CardId::CLOCK, true);
    TEST_ASSERT_EQUAL(Action::SHOW_FLIGHTS, show.update(1, CardId::CLOCK, true));

    show.noteManualNavigation();

    TEST_ASSERT_EQUAL(Action::NONE, show.update(0, CardId::FLIGHTS, true));
}

static void test_disableMidShow_returnsNoneAndNoLaterReturn(void)
{
    FlightsAutoShow show;
    show.update(0, CardId::CLOCK, true);
    TEST_ASSERT_EQUAL(Action::SHOW_FLIGHTS, show.update(1, CardId::CLOCK, true));

    show.setEnabled(false);

    // Disabled: always NONE, even as the count drops back to 0 on Flights.
    TEST_ASSERT_EQUAL(Action::NONE, show.update(0, CardId::FLIGHTS, true));

    // Re-enabling doesn't retroactively fire the return either.
    show.setEnabled(true);
    TEST_ASSERT_EQUAL(Action::NONE, show.update(0, CardId::FLIGHTS, true));
}

static void test_reEnableRequiresFreshEdge(void)
{
    FlightsAutoShow show;
    show.update(0, CardId::CLOCK, true);
    TEST_ASSERT_EQUAL(Action::SHOW_FLIGHTS, show.update(1, CardId::CLOCK, true));

    // Disable while aircraft are still present (count never drops to 0).
    show.setEnabled(false);
    show.update(1, CardId::CLOCK, true);
    show.setEnabled(true);

    // Count is still 1 (no 0->>0 edge since re-enabling): no show.
    TEST_ASSERT_EQUAL(Action::NONE, show.update(1, CardId::CLOCK, true));

    // Now a genuine 0->>0 edge fires it.
    show.update(0, CardId::CLOCK, true);
    TEST_ASSERT_EQUAL(Action::SHOW_FLIGHTS, show.update(1, CardId::CLOCK, true));
}

static void test_countDropTwoToOne_isNone(void)
{
    FlightsAutoShow show;
    show.update(0, CardId::CLOCK, true);
    TEST_ASSERT_EQUAL(Action::SHOW_FLIGHTS, show.update(2, CardId::CLOCK, true));
    TEST_ASSERT_EQUAL(Action::NONE, show.update(1, CardId::FLIGHTS, true));
}

static void test_leavingByOtherMeans_clearsAutoShownWithoutReturn(void)
{
    FlightsAutoShow show;
    show.update(0, CardId::CLOCK, true);
    TEST_ASSERT_EQUAL(Action::SHOW_FLIGHTS, show.update(1, CardId::CLOCK, true));

    // Belt starts running: the screen jumps to Treadmill without going
    // through noteManualNavigation(). The auto-show episode still ends.
    TEST_ASSERT_EQUAL(Action::NONE, show.update(1, CardId::TREADMILL, true));

    // Back to Clock later with count at 0: no phantom return fires, because
    // there was no fresh 0->>0 edge under Clock.
    TEST_ASSERT_EQUAL(Action::NONE, show.update(0, CardId::CLOCK, true));
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_showOnZeroToOneFromClock);
    RUN_TEST(test_noShowFromLightOffice);
    RUN_TEST(test_noShowFromTreadmill);
    RUN_TEST(test_noShowWhileBeltNotIdle);
    RUN_TEST(test_returnOnZero_onlyWhenAutoShown);
    RUN_TEST(test_manualNavigationCancelsReturn);
    RUN_TEST(test_disableMidShow_returnsNoneAndNoLaterReturn);
    RUN_TEST(test_reEnableRequiresFreshEdge);
    RUN_TEST(test_countDropTwoToOne_isNone);
    RUN_TEST(test_leavingByOtherMeans_clearsAutoShownWithoutReturn);
    return UNITY_END();
}
