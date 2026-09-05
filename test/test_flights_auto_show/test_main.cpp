#include <unity.h>

#include "CardRing.h"
#include "FlightsAutoShow.h"

void setUp(void) {}
void tearDown(void) {}

using Action = FlightsAutoShow::Action;

// DialUi polls the state machine roughly every 250 ms; the tests advance
// their own clock in the same steps (and in whole seconds where the
// return-hold window is what's under test).
static constexpr uint32_t kPoll = 250;
static constexpr uint32_t kHold = FlightsAutoShow::kReturnHoldMs;

static void test_showOnZeroToOneFromClock(void)
{
    FlightsAutoShow show;
    TEST_ASSERT_EQUAL(Action::NONE, show.update(0, 0, CardId::CLOCK, true, true));
    TEST_ASSERT_EQUAL(Action::SHOW_FLIGHTS, show.update(kPoll, 1, CardId::CLOCK, true, true));
}

static void test_noShowFromLightOffice(void)
{
    FlightsAutoShow show;
    show.update(0, 0, CardId::LIGHT_OFFICE, true, true);
    TEST_ASSERT_EQUAL(Action::NONE, show.update(kPoll, 1, CardId::LIGHT_OFFICE, true, true));
}

static void test_noShowFromTreadmill(void)
{
    FlightsAutoShow show;
    show.update(0, 0, CardId::TREADMILL, true, true);
    TEST_ASSERT_EQUAL(Action::NONE, show.update(kPoll, 1, CardId::TREADMILL, true, true));
}

static void test_noShowWhileBeltNotIdle(void)
{
    FlightsAutoShow show;
    show.update(0, 0, CardId::CLOCK, true, true);
    TEST_ASSERT_EQUAL(Action::NONE, show.update(kPoll, 1, CardId::CLOCK, false, true));
}

static void test_returnOnZero_onlyWhenAutoShown(void)
{
    // Never auto-shown (arrived at Flights without a Clock 0->>0 edge, e.g.
    // manual knob navigation): dropping to 0 must not force a return, however
    // long it stays there.
    FlightsAutoShow notAutoShown;
    notAutoShown.update(0, 5, CardId::FLIGHTS, true, true);
    notAutoShown.update(kPoll, 0, CardId::FLIGHTS, true, true);
    TEST_ASSERT_EQUAL(Action::NONE, notAutoShown.update(kPoll + kHold, 0, CardId::FLIGHTS, true, true));

    // Auto-shown: dropping to 0 while still on Flights returns to Clock once
    // the count has held at 0 for the full hold window.
    FlightsAutoShow autoShown;
    autoShown.update(0, 0, CardId::CLOCK, true, true);
    TEST_ASSERT_EQUAL(Action::SHOW_FLIGHTS, autoShown.update(kPoll, 1, CardId::CLOCK, true, true));
    TEST_ASSERT_EQUAL(Action::NONE, autoShown.update(2 * kPoll, 0, CardId::FLIGHTS, true, true));
    TEST_ASSERT_EQUAL(Action::RETURN_TO_CLOCK,
                      autoShown.update(2 * kPoll + kHold, 0, CardId::FLIGHTS, true, true));
}

static void test_manualNavigationCancelsReturn(void)
{
    FlightsAutoShow show;
    show.update(0, 0, CardId::CLOCK, true, true);
    TEST_ASSERT_EQUAL(Action::SHOW_FLIGHTS, show.update(kPoll, 1, CardId::CLOCK, true, true));

    show.noteManualNavigation();

    show.update(2 * kPoll, 0, CardId::FLIGHTS, true, true);
    TEST_ASSERT_EQUAL(Action::NONE, show.update(2 * kPoll + kHold, 0, CardId::FLIGHTS, true, true));
}

static void test_disableMidShow_returnsNoneAndNoLaterReturn(void)
{
    FlightsAutoShow show;
    show.update(0, 0, CardId::CLOCK, true, true);
    TEST_ASSERT_EQUAL(Action::SHOW_FLIGHTS, show.update(kPoll, 1, CardId::CLOCK, true, true));

    show.setEnabled(false);

    // Disabled: always NONE, even as the count drops back to 0 on Flights.
    show.update(2 * kPoll, 0, CardId::FLIGHTS, true, true);
    TEST_ASSERT_EQUAL(Action::NONE, show.update(2 * kPoll + kHold, 0, CardId::FLIGHTS, true, true));

    // Re-enabling doesn't retroactively fire the return either.
    show.setEnabled(true);
    TEST_ASSERT_EQUAL(Action::NONE, show.update(3 * kPoll + kHold, 0, CardId::FLIGHTS, true, true));
}

static void test_reEnableRequiresFreshEdge(void)
{
    FlightsAutoShow show;
    show.update(0, 0, CardId::CLOCK, true, true);
    TEST_ASSERT_EQUAL(Action::SHOW_FLIGHTS, show.update(kPoll, 1, CardId::CLOCK, true, true));

    // Disable while aircraft are still present (count never drops to 0).
    show.setEnabled(false);
    show.update(2 * kPoll, 1, CardId::CLOCK, true, true);
    show.setEnabled(true);

    // Count is still 1 (no 0->>0 edge since re-enabling): no show.
    TEST_ASSERT_EQUAL(Action::NONE, show.update(3 * kPoll, 1, CardId::CLOCK, true, true));

    // Now a genuine 0->>0 edge fires it.
    show.update(4 * kPoll, 0, CardId::CLOCK, true, true);
    TEST_ASSERT_EQUAL(Action::SHOW_FLIGHTS, show.update(5 * kPoll, 1, CardId::CLOCK, true, true));
}

static void test_countDropTwoToOne_isNone(void)
{
    FlightsAutoShow show;
    show.update(0, 0, CardId::CLOCK, true, true);
    TEST_ASSERT_EQUAL(Action::SHOW_FLIGHTS, show.update(kPoll, 2, CardId::CLOCK, true, true));
    TEST_ASSERT_EQUAL(Action::NONE, show.update(2 * kPoll, 1, CardId::FLIGHTS, true, true));
}

static void test_leavingByOtherMeans_clearsAutoShownWithoutReturn(void)
{
    FlightsAutoShow show;
    show.update(0, 0, CardId::CLOCK, true, true);
    TEST_ASSERT_EQUAL(Action::SHOW_FLIGHTS, show.update(kPoll, 1, CardId::CLOCK, true, true));

    // Belt starts running: the screen jumps to Treadmill without going
    // through noteManualNavigation(). The auto-show episode still ends.
    TEST_ASSERT_EQUAL(Action::NONE, show.update(2 * kPoll, 1, CardId::TREADMILL, true, true));

    // Back to Clock later with count at 0: no phantom return fires, because
    // there was no fresh 0->>0 edge under Clock.
    TEST_ASSERT_EQUAL(Action::NONE, show.update(3 * kPoll, 0, CardId::CLOCK, true, true));
}

// Stale/offline data must not be trusted: the aircraft count on screen may
// be minutes old, so `dataValid` false is treated as a count of 0 for both
// decisions below (final review 2026-09-05).

static void test_dataInvalidWhileAutoShown_returnsToClock(void)
{
    FlightsAutoShow show;
    show.update(0, 0, CardId::CLOCK, true, true);
    TEST_ASSERT_EQUAL(Action::SHOW_FLIGHTS, show.update(kPoll, 2, CardId::CLOCK, true, true));

    // HA goes quiet: the last list still says 2 aircraft, but it can no
    // longer be believed, so the auto-shown card hands back to Clock once
    // the hold window has passed with the data still untrusted.
    show.update(2 * kPoll, 2, CardId::FLIGHTS, true, false);
    TEST_ASSERT_EQUAL(Action::RETURN_TO_CLOCK,
                      show.update(2 * kPoll + kHold, 2, CardId::FLIGHTS, true, false));
}

static void test_dataInvalid_noShowOnZeroToTwoEdge(void)
{
    FlightsAutoShow show;
    show.update(0, 0, CardId::CLOCK, true, false);

    // A 0->>0 edge on untrusted data is not an edge at all: both samples
    // count as 0, so nothing is raised over the Clock card.
    TEST_ASSERT_EQUAL(Action::NONE, show.update(kPoll, 2, CardId::CLOCK, true, false));
}

// Return hysteresis: aircraft drop in and out of HA's list at the edge of
// range, so the card must not bounce back to Clock the instant the count
// touches 0 (final review 2026-09-05).

static void test_returnHold_firesAtWindow_notJustBefore(void)
{
    FlightsAutoShow show;
    show.update(0, 0, CardId::CLOCK, true, true);
    TEST_ASSERT_EQUAL(Action::SHOW_FLIGHTS, show.update(1000, 1, CardId::CLOCK, true, true));

    // Count drops to 0 at t = 2000; the hold runs from there.
    TEST_ASSERT_EQUAL(Action::NONE, show.update(2000, 0, CardId::FLIGHTS, true, true));

    // 14 s of zero: still showing Flights.
    TEST_ASSERT_EQUAL(Action::NONE, show.update(2000 + kHold - 1000, 0, CardId::FLIGHTS, true, true));

    // 15 s: hands back to Clock.
    TEST_ASSERT_EQUAL(Action::RETURN_TO_CLOCK, show.update(2000 + kHold, 0, CardId::FLIGHTS, true, true));
}

static void test_returnHold_resetByNonZeroCountInBetween(void)
{
    FlightsAutoShow show;
    show.update(0, 0, CardId::CLOCK, true, true);
    TEST_ASSERT_EQUAL(Action::SHOW_FLIGHTS, show.update(1000, 1, CardId::CLOCK, true, true));

    // Zero from t = 2000, but one aircraft reappears at t = 12000: the hold
    // restarts from there, so the original 15 s deadline passes with nothing.
    TEST_ASSERT_EQUAL(Action::NONE, show.update(2000, 0, CardId::FLIGHTS, true, true));
    TEST_ASSERT_EQUAL(Action::NONE, show.update(12000, 1, CardId::FLIGHTS, true, true));
    TEST_ASSERT_EQUAL(Action::NONE, show.update(13000, 0, CardId::FLIGHTS, true, true));
    TEST_ASSERT_EQUAL(Action::NONE, show.update(2000 + kHold, 0, CardId::FLIGHTS, true, true));

    // The restarted hold expires 15 s after the count went back to 0.
    TEST_ASSERT_EQUAL(Action::RETURN_TO_CLOCK, show.update(13000 + kHold, 0, CardId::FLIGHTS, true, true));
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
    RUN_TEST(test_returnHold_firesAtWindow_notJustBefore);
    RUN_TEST(test_returnHold_resetByNonZeroCountInBetween);
    return UNITY_END();
}
