#include <unity.h>

#include "CardRing.h"
#include "FlightsAutoShow.h"

void setUp(void) {}
void tearDown(void) {}

using Action = FlightsAutoShow::Action;

static void test_showOnZeroToOneFromClock(void)
{
    FlightsAutoShow show;
    TEST_ASSERT_EQUAL(Action::NONE, show.update(0, CardId::CLOCK, true, true));
    TEST_ASSERT_EQUAL(Action::SHOW_FLIGHTS, show.update(1, CardId::CLOCK, true, true));
}

static void test_noShowFromLightOffice(void)
{
    FlightsAutoShow show;
    show.update(0, CardId::LIGHT_OFFICE, true, true);
    TEST_ASSERT_EQUAL(Action::NONE, show.update(1, CardId::LIGHT_OFFICE, true, true));
}

static void test_noShowFromTreadmill(void)
{
    FlightsAutoShow show;
    show.update(0, CardId::TREADMILL, true, true);
    TEST_ASSERT_EQUAL(Action::NONE, show.update(1, CardId::TREADMILL, true, true));
}

static void test_noShowWhileBeltNotIdle(void)
{
    FlightsAutoShow show;
    show.update(0, CardId::CLOCK, true, true);
    TEST_ASSERT_EQUAL(Action::NONE, show.update(1, CardId::CLOCK, false, true));
}

static void test_returnOnZero_onlyWhenAutoShown(void)
{
    // Never auto-shown (arrived at Flights without a Clock 0->>0 edge, e.g.
    // manual knob navigation): dropping to 0 must not force a return.
    FlightsAutoShow notAutoShown;
    notAutoShown.update(5, CardId::FLIGHTS, true, true);
    TEST_ASSERT_EQUAL(Action::NONE, notAutoShown.update(0, CardId::FLIGHTS, true, true));

    // Auto-shown: dropping to 0 while still on Flights returns to Clock.
    FlightsAutoShow autoShown;
    autoShown.update(0, CardId::CLOCK, true, true);
    TEST_ASSERT_EQUAL(Action::SHOW_FLIGHTS, autoShown.update(1, CardId::CLOCK, true, true));
    TEST_ASSERT_EQUAL(Action::RETURN_TO_CLOCK, autoShown.update(0, CardId::FLIGHTS, true, true));
}

static void test_manualNavigationCancelsReturn(void)
{
    FlightsAutoShow show;
    show.update(0, CardId::CLOCK, true, true);
    TEST_ASSERT_EQUAL(Action::SHOW_FLIGHTS, show.update(1, CardId::CLOCK, true, true));

    show.noteManualNavigation();

    TEST_ASSERT_EQUAL(Action::NONE, show.update(0, CardId::FLIGHTS, true, true));
}

static void test_disableMidShow_returnsNoneAndNoLaterReturn(void)
{
    FlightsAutoShow show;
    show.update(0, CardId::CLOCK, true, true);
    TEST_ASSERT_EQUAL(Action::SHOW_FLIGHTS, show.update(1, CardId::CLOCK, true, true));

    show.setEnabled(false);

    // Disabled: always NONE, even as the count drops back to 0 on Flights.
    TEST_ASSERT_EQUAL(Action::NONE, show.update(0, CardId::FLIGHTS, true, true));

    // Re-enabling doesn't retroactively fire the return either.
    show.setEnabled(true);
    TEST_ASSERT_EQUAL(Action::NONE, show.update(0, CardId::FLIGHTS, true, true));
}

static void test_reEnableRequiresFreshEdge(void)
{
    FlightsAutoShow show;
    show.update(0, CardId::CLOCK, true, true);
    TEST_ASSERT_EQUAL(Action::SHOW_FLIGHTS, show.update(1, CardId::CLOCK, true, true));

    // Disable while aircraft are still present (count never drops to 0).
    show.setEnabled(false);
    show.update(1, CardId::CLOCK, true, true);
    show.setEnabled(true);

    // Count is still 1 (no 0->>0 edge since re-enabling): no show.
    TEST_ASSERT_EQUAL(Action::NONE, show.update(1, CardId::CLOCK, true, true));

    // Now a genuine 0->>0 edge fires it.
    show.update(0, CardId::CLOCK, true, true);
    TEST_ASSERT_EQUAL(Action::SHOW_FLIGHTS, show.update(1, CardId::CLOCK, true, true));
}

static void test_countDropTwoToOne_isNone(void)
{
    FlightsAutoShow show;
    show.update(0, CardId::CLOCK, true, true);
    TEST_ASSERT_EQUAL(Action::SHOW_FLIGHTS, show.update(2, CardId::CLOCK, true, true));
    TEST_ASSERT_EQUAL(Action::NONE, show.update(1, CardId::FLIGHTS, true, true));
}

static void test_leavingByOtherMeans_clearsAutoShownWithoutReturn(void)
{
    FlightsAutoShow show;
    show.update(0, CardId::CLOCK, true, true);
    TEST_ASSERT_EQUAL(Action::SHOW_FLIGHTS, show.update(1, CardId::CLOCK, true, true));

    // Belt starts running: the screen jumps to Treadmill without going
    // through noteManualNavigation(). The auto-show episode still ends.
    TEST_ASSERT_EQUAL(Action::NONE, show.update(1, CardId::TREADMILL, true, true));

    // Back to Clock later with count at 0: no phantom return fires, because
    // there was no fresh 0->>0 edge under Clock.
    TEST_ASSERT_EQUAL(Action::NONE, show.update(0, CardId::CLOCK, true, true));
}

// Stale/offline data must not be trusted: the aircraft count on screen may
// be minutes old, so `dataValid` false is treated as a count of 0 for both
// decisions below (final review 2026-09-05).

static void test_dataInvalidWhileAutoShown_returnsToClock(void)
{
    FlightsAutoShow show;
    show.update(0, CardId::CLOCK, true, true);
    TEST_ASSERT_EQUAL(Action::SHOW_FLIGHTS, show.update(2, CardId::CLOCK, true, true));

    // HA goes quiet: the last list still says 2 aircraft, but it can no
    // longer be believed, so the auto-shown card hands back to Clock.
    TEST_ASSERT_EQUAL(Action::RETURN_TO_CLOCK, show.update(2, CardId::FLIGHTS, true, false));
}

static void test_dataInvalid_noShowOnZeroToTwoEdge(void)
{
    FlightsAutoShow show;
    show.update(0, CardId::CLOCK, true, false);

    // A 0->>0 edge on untrusted data is not an edge at all: both samples
    // count as 0, so nothing is raised over the Clock card.
    TEST_ASSERT_EQUAL(Action::NONE, show.update(2, CardId::CLOCK, true, false));
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
    RUN_TEST(test_dataInvalidWhileAutoShown_returnsToClock);
    RUN_TEST(test_dataInvalid_noShowOnZeroToTwoEdge);
    return UNITY_END();
}
