#include <unity.h>
#include <stdint.h>
#include "DialInput.h"

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// Detents
// ---------------------------------------------------------------------------

static void test_detents_fourPulses_oneDetent(void)
{
    DialInput input;
    uint32_t t = 1000;

    input.tick(0, false, 0, 0, false, t); // baseline: establishes m_lastCount

    DialEvents e = input.tick(4, false, 0, 0, false, t + 10);
    TEST_ASSERT_EQUAL_INT(1, e.detents);
}

static void test_detents_sixPulses_thenTwoMore_secondDetent(void)
{
    DialInput input;
    uint32_t t = 1000;

    input.tick(0, false, 0, 0, false, t); // baseline

    DialEvents e1 = input.tick(6, false, 0, 0, false, t + 10);
    TEST_ASSERT_EQUAL_INT(1, e1.detents); // 6/4 = 1, remainder 2

    DialEvents e2 = input.tick(8, false, 0, 0, false, t + 20);
    TEST_ASSERT_EQUAL_INT(1, e2.detents); // remainder 2 + 2 = 4 -> 1 more detent
}

static void test_detents_negativeDirection(void)
{
    DialInput input;
    uint32_t t = 1000;

    input.tick(100, false, 0, 0, false, t); // baseline at 100

    DialEvents e1 = input.tick(98, false, 0, 0, false, t + 10);
    TEST_ASSERT_EQUAL_INT(0, e1.detents); // delta -2, remainder -2, no detent yet

    DialEvents e2 = input.tick(96, false, 0, 0, false, t + 20);
    TEST_ASSERT_EQUAL_INT(-1, e2.detents); // remainder -2 + -2 = -4 -> -1 detent
}

// ---------------------------------------------------------------------------
// Tap / long press / drag
// ---------------------------------------------------------------------------

static void test_tap_insideTapWindow(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, true, 50, 50, false, t0); // touch down

    DialEvents e = input.tick(0, false, 0, 0, false, t0 + 300); // release at 300ms
    TEST_ASSERT_TRUE(e.tap);
    TEST_ASSERT_FALSE(e.longPress);
    TEST_ASSERT_FALSE(e.wake);
}

static void test_tap_outsideTapWindow_notATap(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, true, 50, 50, false, t0); // touch down

    DialEvents e = input.tick(0, false, 0, 0, false, t0 + 700); // release at 700ms
    TEST_ASSERT_FALSE(e.tap);
    TEST_ASSERT_FALSE(e.longPress);
}

static void test_drag_exceedsMovement_notTapNotLongPress(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, true, 50, 50, false, t0); // touch down at (50,50)

    // Moves 30px in x, well past the 20px threshold -> drag.
    DialEvents eMid = input.tick(0, true, 80, 50, false, t0 + 100);
    TEST_ASSERT_FALSE(eMid.tap);
    TEST_ASSERT_FALSE(eMid.longPress);

    // Held well past the long-press threshold, but it's a drag so neither
    // long press nor tap should fire, even on release.
    DialEvents eHeld = input.tick(0, true, 80, 50, false, t0 + 1100);
    TEST_ASSERT_FALSE(eHeld.longPress);

    DialEvents eRelease = input.tick(0, false, 0, 0, false, t0 + 1200);
    TEST_ASSERT_FALSE(eRelease.tap);
    TEST_ASSERT_FALSE(eRelease.longPress);
}

static void test_longPress_firesOnceAt1000ms_progressHalfAt500ms(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, true, 50, 50, false, t0); // touch down

    DialEvents e500 = input.tick(0, true, 50, 50, false, t0 + 500);
    TEST_ASSERT_FALSE(e500.longPress);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.5f, e500.holdProgress);

    DialEvents e1000 = input.tick(0, true, 50, 50, false, t0 + 1000);
    TEST_ASSERT_TRUE(e1000.longPress);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.0f, e1000.holdProgress);

    // Still held after firing: must not fire again, progress reported as 0.
    DialEvents e1100 = input.tick(0, true, 50, 50, false, t0 + 1100);
    TEST_ASSERT_FALSE(e1100.longPress);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, e1100.holdProgress);

    // Release after a long press must not also report a tap.
    DialEvents eRelease = input.tick(0, false, 0, 0, false, t0 + 1500);
    TEST_ASSERT_FALSE(eRelease.tap);
}

// ---------------------------------------------------------------------------
// Wake gating
// ---------------------------------------------------------------------------

static void test_wake_inputWhileDim_yieldsWakeOnly(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, false, 0, 0, false, t0); // baseline, establishes activity

    // Advance to the dim threshold with no activity.
    input.tick(0, false, 0, 0, false, t0 + 120000);
    TEST_ASSERT_TRUE(DialInput::Backlight::DIM == input.backlight());

    // A button click arrives while dim: should wake only, and swallow the click.
    DialEvents e = input.tick(0, false, 0, 0, true, t0 + 120001);
    TEST_ASSERT_TRUE(e.wake);
    TEST_ASSERT_FALSE(e.btnStop);
    TEST_ASSERT_TRUE(DialInput::Backlight::FULL == input.backlight());
}

static void test_wake_touchWhileDim_swallowsGestureUntilRelease(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, false, 0, 0, false, t0); // baseline
    input.tick(0, false, 0, 0, false, t0 + 120000); // now DIM
    TEST_ASSERT_TRUE(DialInput::Backlight::DIM == input.backlight());

    // Touch-down arrives while dim: wakes, swallows the gesture.
    DialEvents eDown = input.tick(0, true, 50, 50, false, t0 + 120001);
    TEST_ASSERT_TRUE(eDown.wake);
    TEST_ASSERT_FALSE(eDown.tap);

    // Quick release of that same (swallowed) gesture must not produce a tap.
    DialEvents eRelease = input.tick(0, false, 0, 0, false, t0 + 120100);
    TEST_ASSERT_FALSE(eRelease.tap);
    TEST_ASSERT_FALSE(eRelease.wake);
}

// ---------------------------------------------------------------------------
// Backlight dimming
// ---------------------------------------------------------------------------

static void test_backlight_dimAt120s_staysDimAt600s_activityResets(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, false, 0, 0, false, t0); // baseline
    TEST_ASSERT_TRUE(DialInput::Backlight::FULL == input.backlight());

    input.tick(0, false, 0, 0, false, t0 + 119999);
    TEST_ASSERT_TRUE(DialInput::Backlight::FULL == input.backlight());

    input.tick(0, false, 0, 0, false, t0 + 120000);
    TEST_ASSERT_TRUE(DialInput::Backlight::DIM == input.backlight());

    input.tick(0, false, 0, 0, false, t0 + 599999);
    TEST_ASSERT_TRUE(DialInput::Backlight::DIM == input.backlight());

    // No OFF stage (spec 4.8): still DIM at 600s and beyond, with no further
    // activity — the backlight never turns off on its own.
    input.tick(0, false, 0, 0, false, t0 + 600000);
    TEST_ASSERT_TRUE(DialInput::Backlight::DIM == input.backlight());

    input.noteActivity(t0 + 600000);
    TEST_ASSERT_TRUE(DialInput::Backlight::FULL == input.backlight());

    input.tick(0, false, 0, 0, false, t0 + 600100);
    TEST_ASSERT_TRUE(DialInput::Backlight::FULL == input.backlight());
}

// A touch-and-hold that starts while dim stays fully swallowed: the wake-only
// tick reports wake and nothing else, and the long-press threshold passing
// mid-hold must not fire longPress — only a fresh gesture, started while
// FULL, can trigger a long press.
static void test_wake_touchWhileDim_swallowsLongPressUntilRelease(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, false, 0, 0, false, t0); // baseline
    input.tick(0, false, 0, 0, false, t0 + 120000); // now DIM
    TEST_ASSERT_TRUE(DialInput::Backlight::DIM == input.backlight());

    // Touch-down arrives while dim: wakes, swallows the gesture. Only this
    // first tick reports wake.
    DialEvents eDown = input.tick(0, true, 50, 50, false, t0 + 120001);
    TEST_ASSERT_TRUE(eDown.wake);
    TEST_ASSERT_FALSE(eDown.tap);
    TEST_ASSERT_FALSE(eDown.longPress);

    // Held past the long-press threshold (1000 ms): still swallowed, so no
    // longPress and no further wake.
    DialEvents eHeld = input.tick(0, true, 50, 50, false, t0 + 120001 + DialInput::HOLD_MS);
    TEST_ASSERT_FALSE(eHeld.wake);
    TEST_ASSERT_FALSE(eHeld.longPress);

    DialEvents eHeldPast = input.tick(0, true, 50, 50, false, t0 + 120001 + DialInput::HOLD_MS + 500);
    TEST_ASSERT_FALSE(eHeldPast.wake);
    TEST_ASSERT_FALSE(eHeldPast.longPress);

    // Release of the whole swallowed hold: no tap, no longPress, no wake.
    DialEvents eRelease = input.tick(0, false, 0, 0, false, t0 + 120001 + DialInput::HOLD_MS + 1000);
    TEST_ASSERT_FALSE(eRelease.tap);
    TEST_ASSERT_FALSE(eRelease.longPress);
    TEST_ASSERT_FALSE(eRelease.wake);
}

static void test_wake_encoderWhileDim_rebasesBaselineForNextDetent(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, false, 0, 0, false, t0); // baseline, establishes activity

    // Advance to the dim threshold with no activity.
    input.tick(0, false, 0, 0, false, t0 + 120000);
    TEST_ASSERT_TRUE(DialInput::Backlight::DIM == input.backlight());

    // Encoder moves while dim: wakes, swallows the pulses, and rebases the
    // baseline to the count seen on this tick (8), not the old one (0).
    DialEvents eWake = input.tick(8, false, 0, 0, false, t0 + 120001);
    TEST_ASSERT_TRUE(eWake.wake);
    TEST_ASSERT_EQUAL_INT(0, eWake.detents);
    TEST_ASSERT_TRUE(DialInput::Backlight::FULL == input.backlight());

    // Next tick: delta is from the rebased baseline of 8, i.e. 12-8=4 -> 1
    // detent. If the baseline had stayed at 0, this would wrongly read 3.
    DialEvents eNext = input.tick(12, false, 0, 0, false, t0 + 120010);
    TEST_ASSERT_EQUAL_INT(1, eNext.detents);
    TEST_ASSERT_FALSE(eNext.wake);
}

// ---------------------------------------------------------------------------
// Tap boundary
// ---------------------------------------------------------------------------

static void test_tap_releaseAtExactlyTapMaxMs_notATap(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, true, 50, 50, false, t0); // touch down

    DialEvents e = input.tick(0, false, 0, 0, false, t0 + DialInput::TAP_MAX_MS);
    TEST_ASSERT_FALSE(e.tap);
}

static void test_tap_releaseJustUnderTapMaxMs_isATap(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, true, 50, 50, false, t0); // touch down

    DialEvents e = input.tick(0, false, 0, 0, false, t0 + DialInput::TAP_MAX_MS - 1);
    TEST_ASSERT_TRUE(e.tap);
}

// ---------------------------------------------------------------------------
// Horizontal swipe
// ---------------------------------------------------------------------------

static void test_swipe_right_firesOnceThenZero(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, true, 50, 50, false, t0); // touch down

    DialEvents e1 = input.tick(0, true, 90, 50, false, t0 + 100); // +40px right
    TEST_ASSERT_EQUAL_INT(1, e1.swipe);

    DialEvents e2 = input.tick(0, true, 90, 50, false, t0 + 150); // still held, no further move
    TEST_ASSERT_EQUAL_INT(0, e2.swipe);

    DialEvents e3 = input.tick(0, false, 0, 0, false, t0 + 200); // release
    TEST_ASSERT_EQUAL_INT(0, e3.swipe);
}

static void test_swipe_left_firesOnce(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, true, 50, 50, false, t0); // touch down

    DialEvents e1 = input.tick(0, true, 10, 50, false, t0 + 100); // -40px left
    TEST_ASSERT_EQUAL_INT(-1, e1.swipe);
}

static void test_swipe_diagonal_dyExceedsDx_notASwipe_isADrag(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, true, 50, 50, false, t0); // touch down

    // dx=40, dy=45: |dx| >= 40 but not > |dy| -> no swipe. Still a drag by
    // distance, so release must not be a tap.
    DialEvents eMid = input.tick(0, true, 90, 95, false, t0 + 100);
    TEST_ASSERT_EQUAL_INT(0, eMid.swipe);

    DialEvents eRelease = input.tick(0, false, 0, 0, false, t0 + 150);
    TEST_ASSERT_FALSE(eRelease.tap);
}

static void test_swipe_thenRelease_notTapNotLongPress(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, true, 50, 50, false, t0); // touch down

    DialEvents eSwipe = input.tick(0, true, 90, 50, false, t0 + 100);
    TEST_ASSERT_EQUAL_INT(1, eSwipe.swipe);

    DialEvents eRelease = input.tick(0, false, 0, 0, false, t0 + 200);
    TEST_ASSERT_FALSE(eRelease.tap);
    TEST_ASSERT_FALSE(eRelease.longPress);
}

static void test_swipe_thenHoldPast1000ms_noLongPress(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, true, 50, 50, false, t0); // touch down
    input.tick(0, true, 90, 50, false, t0 + 100); // swipe fires

    DialEvents eHeld = input.tick(0, true, 90, 50, false, t0 + 1100); // past 1000ms hold
    TEST_ASSERT_FALSE(eHeld.longPress);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, eHeld.holdProgress);
}

static void test_swipe_whileDim_wakeOnly_noSwipe(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, false, 0, 0, false, t0); // baseline
    input.tick(0, false, 0, 0, false, t0 + 120000); // now DIM
    TEST_ASSERT_TRUE(DialInput::Backlight::DIM == input.backlight());

    // Touch-down arrives while dim: wakes, swallows the gesture.
    DialEvents eDown = input.tick(0, true, 50, 50, false, t0 + 120001);
    TEST_ASSERT_TRUE(eDown.wake);
    TEST_ASSERT_EQUAL_INT(0, eDown.swipe);
    TEST_ASSERT_TRUE(DialInput::Backlight::FULL == input.backlight());

    // A 40px move within the swallowed gesture must not emit a swipe.
    DialEvents eMove = input.tick(0, true, 90, 50, false, t0 + 120050);
    TEST_ASSERT_EQUAL_INT(0, eMove.swipe);
    TEST_ASSERT_TRUE(DialInput::Backlight::FULL == input.backlight());
}

static void test_swipe_39px_notASwipe_isADrag(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, true, 50, 50, false, t0); // touch down

    // 39px < SWIPE_MIN_PX (40): no swipe, but still exceeds TAP_MAX_MOVE_PX
    // (20) so it's a drag.
    DialEvents eMid = input.tick(0, true, 89, 50, false, t0 + 100);
    TEST_ASSERT_EQUAL_INT(0, eMid.swipe);

    DialEvents eRelease = input.tick(0, false, 0, 0, false, t0 + 200);
    TEST_ASSERT_FALSE(eRelease.tap);
}

// ---------------------------------------------------------------------------
// Wraparound-safe time maths
// ---------------------------------------------------------------------------

static void test_time_wraparoundSafe(void)
{
    DialInput input;
    uint32_t t0 = UINT32_MAX - 1000;

    input.tick(0, false, 0, 0, false, t0); // baseline near wraparound
    TEST_ASSERT_TRUE(DialInput::Backlight::FULL == input.backlight());

    uint32_t t1 = t0 + (uint32_t)120000; // wraps past UINT32_MAX
    input.tick(0, false, 0, 0, false, t1);
    TEST_ASSERT_TRUE(DialInput::Backlight::DIM == input.backlight());

    // No OFF stage (spec 4.8): still DIM well past the old OFF threshold,
    // even across a further millis() wrap.
    uint32_t t2 = t0 + (uint32_t)600000; // also wraps
    input.tick(0, false, 0, 0, false, t2);
    TEST_ASSERT_TRUE(DialInput::Backlight::DIM == input.backlight());
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();

    RUN_TEST(test_detents_fourPulses_oneDetent);
    RUN_TEST(test_detents_sixPulses_thenTwoMore_secondDetent);
    RUN_TEST(test_detents_negativeDirection);
    RUN_TEST(test_tap_insideTapWindow);
    RUN_TEST(test_tap_outsideTapWindow_notATap);
    RUN_TEST(test_drag_exceedsMovement_notTapNotLongPress);
    RUN_TEST(test_longPress_firesOnceAt1000ms_progressHalfAt500ms);
    RUN_TEST(test_wake_inputWhileDim_yieldsWakeOnly);
    RUN_TEST(test_wake_touchWhileDim_swallowsGestureUntilRelease);
    RUN_TEST(test_wake_touchWhileDim_swallowsLongPressUntilRelease);
    RUN_TEST(test_wake_encoderWhileDim_rebasesBaselineForNextDetent);
    RUN_TEST(test_tap_releaseAtExactlyTapMaxMs_notATap);
    RUN_TEST(test_tap_releaseJustUnderTapMaxMs_isATap);
    RUN_TEST(test_swipe_right_firesOnceThenZero);
    RUN_TEST(test_swipe_left_firesOnce);
    RUN_TEST(test_swipe_diagonal_dyExceedsDx_notASwipe_isADrag);
    RUN_TEST(test_swipe_thenRelease_notTapNotLongPress);
    RUN_TEST(test_swipe_thenHoldPast1000ms_noLongPress);
    RUN_TEST(test_swipe_whileDim_wakeOnly_noSwipe);
    RUN_TEST(test_swipe_39px_notASwipe_isADrag);
    RUN_TEST(test_backlight_dimAt120s_staysDimAt600s_activityResets);
    RUN_TEST(test_time_wraparoundSafe);

    return UNITY_END();
}
