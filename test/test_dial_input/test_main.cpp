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

    input.tick(0, false, 0, 0, false, false, false, t); // baseline: establishes m_lastCount

    DialEvents e = input.tick(4, false, 0, 0, false, false, false, t + 10);
    TEST_ASSERT_EQUAL_INT(1, e.detents);
}

static void test_detents_sixPulses_thenTwoMore_secondDetent(void)
{
    DialInput input;
    uint32_t t = 1000;

    input.tick(0, false, 0, 0, false, false, false, t); // baseline

    DialEvents e1 = input.tick(6, false, 0, 0, false, false, false, t + 10);
    TEST_ASSERT_EQUAL_INT(1, e1.detents); // 6/4 = 1, remainder 2

    DialEvents e2 = input.tick(8, false, 0, 0, false, false, false, t + 20);
    TEST_ASSERT_EQUAL_INT(1, e2.detents); // remainder 2 + 2 = 4 -> 1 more detent
}

static void test_detents_negativeDirection(void)
{
    DialInput input;
    uint32_t t = 1000;

    input.tick(100, false, 0, 0, false, false, false, t); // baseline at 100

    DialEvents e1 = input.tick(98, false, 0, 0, false, false, false, t + 10);
    TEST_ASSERT_EQUAL_INT(0, e1.detents); // delta -2, remainder -2, no detent yet

    DialEvents e2 = input.tick(96, false, 0, 0, false, false, false, t + 20);
    TEST_ASSERT_EQUAL_INT(-1, e2.detents); // remainder -2 + -2 = -4 -> -1 detent
}

// ---------------------------------------------------------------------------
// Tap / long press / drag
// ---------------------------------------------------------------------------

static void test_tap_insideTapWindow(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, true, 50, 50, false, false, false, t0); // touch down

    DialEvents e = input.tick(0, false, 0, 0, false, false, false, t0 + 300); // release at 300ms
    TEST_ASSERT_TRUE(e.tap);
    TEST_ASSERT_FALSE(e.longPress);
    TEST_ASSERT_FALSE(e.wake);
}

static void test_tap_reportsTouchDownPosition(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, true, 56, 186, false, false, false, t0); // touch down at (56,186)

    DialEvents e = input.tick(0, false, 0, 0, false, false, false, t0 + 300); // release at 300ms
    TEST_ASSERT_TRUE(e.tap);
    TEST_ASSERT_EQUAL_INT(56, e.tapX);
    TEST_ASSERT_EQUAL_INT(186, e.tapY);
}

static void test_tap_outsideTapWindow_notATap(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, true, 50, 50, false, false, false, t0); // touch down

    DialEvents e = input.tick(0, false, 0, 0, false, false, false, t0 + 700); // release at 700ms
    TEST_ASSERT_FALSE(e.tap);
    TEST_ASSERT_FALSE(e.longPress);
}

static void test_drag_exceedsMovement_notTapNotLongPress(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, true, 50, 50, false, false, false, t0); // touch down at (50,50)

    // Moves 30px in x, well past the 20px threshold -> drag.
    DialEvents eMid = input.tick(0, true, 80, 50, false, false, false, t0 + 100);
    TEST_ASSERT_FALSE(eMid.tap);
    TEST_ASSERT_FALSE(eMid.longPress);

    // Held well past the long-press threshold, but it's a drag so neither
    // long press nor tap should fire, even on release.
    DialEvents eHeld = input.tick(0, true, 80, 50, false, false, false, t0 + 1100);
    TEST_ASSERT_FALSE(eHeld.longPress);

    DialEvents eRelease = input.tick(0, false, 0, 0, false, false, false, t0 + 1200);
    TEST_ASSERT_FALSE(eRelease.tap);
    TEST_ASSERT_FALSE(eRelease.longPress);
}

static void test_longPress_firesOnceAt1000ms_progressHalfAt500ms(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, true, 50, 50, false, false, false, t0); // touch down

    DialEvents e500 = input.tick(0, true, 50, 50, false, false, false, t0 + 500);
    TEST_ASSERT_FALSE(e500.longPress);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.5f, e500.holdProgress);

    DialEvents e1000 = input.tick(0, true, 50, 50, false, false, false, t0 + 1000);
    TEST_ASSERT_TRUE(e1000.longPress);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.0f, e1000.holdProgress);

    // Still held after firing: must not fire again, progress reported as 0.
    DialEvents e1100 = input.tick(0, true, 50, 50, false, false, false, t0 + 1100);
    TEST_ASSERT_FALSE(e1100.longPress);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, e1100.holdProgress);

    // Release after a long press must not also report a tap.
    DialEvents eRelease = input.tick(0, false, 0, 0, false, false, false, t0 + 1500);
    TEST_ASSERT_FALSE(eRelease.tap);
}

// ---------------------------------------------------------------------------
// Wake gating
// ---------------------------------------------------------------------------

static void test_wake_inputWhileDim_yieldsWakeOnly(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, false, 0, 0, false, false, false, t0); // baseline, establishes activity

    // Advance to the dim threshold with no activity.
    input.tick(0, false, 0, 0, false, false, false, t0 + 120000);
    TEST_ASSERT_TRUE(DialInput::Backlight::DIM == input.backlight());

    // A button click arrives while dim: should wake only, and swallow the click.
    DialEvents e = input.tick(0, false, 0, 0, true, false, false, t0 + 120001);
    TEST_ASSERT_TRUE(e.wake);
    TEST_ASSERT_FALSE(e.btnStop);
    TEST_ASSERT_TRUE(DialInput::Backlight::FULL == input.backlight());
}

static void test_wake_touchWhileDim_swallowsGestureUntilRelease(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, false, 0, 0, false, false, false, t0); // baseline
    input.tick(0, false, 0, 0, false, false, false, t0 + 120000); // now DIM
    TEST_ASSERT_TRUE(DialInput::Backlight::DIM == input.backlight());

    // Touch-down arrives while dim: wakes, swallows the gesture.
    DialEvents eDown = input.tick(0, true, 50, 50, false, false, false, t0 + 120001);
    TEST_ASSERT_TRUE(eDown.wake);
    TEST_ASSERT_FALSE(eDown.tap);

    // Quick release of that same (swallowed) gesture must not produce a tap.
    DialEvents eRelease = input.tick(0, false, 0, 0, false, false, false, t0 + 120100);
    TEST_ASSERT_FALSE(eRelease.tap);
    TEST_ASSERT_FALSE(eRelease.wake);
}

// ---------------------------------------------------------------------------
// Side-button click / hold events
// ---------------------------------------------------------------------------

static void test_btnClick_singleClick_setsBtnClickOnlyOnThatTick(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, false, 0, 0, false, false, false, t0); // baseline

    DialEvents e = input.tick(0, false, 0, 0, false, true, false, t0 + 10);
    TEST_ASSERT_TRUE(e.btnClick);
    TEST_ASSERT_FALSE(e.btnHold);
    TEST_ASSERT_FALSE(e.btnStop);

    DialEvents eNext = input.tick(0, false, 0, 0, false, false, false, t0 + 20);
    TEST_ASSERT_FALSE(eNext.btnClick);
}

static void test_btnDoubleClick_setsBtnDoubleClickOnlyOnThatTick(void)
{
    DialInput input;
    uint32_t t0 = 1000;
    input.tick(0, false, 0, 0, false, false, false, t0); // baseline

    DialEvents e = input.tick(0, false, 0, 0, false, false, false, t0 + 10, true);
    TEST_ASSERT_TRUE(e.btnDoubleClick);
    TEST_ASSERT_FALSE(e.btnClick);
    TEST_ASSERT_FALSE(e.btnHold);
    TEST_ASSERT_FALSE(e.btnStop);

    DialEvents eNext = input.tick(0, false, 0, 0, false, false, false, t0 + 20);
    TEST_ASSERT_FALSE(eNext.btnDoubleClick);
}

static void test_btnDoubleClick_whileDim_wakesOnly(void)
{
    DialInput input;
    uint32_t t0 = 1000;
    input.tick(0, false, 0, 0, false, false, false, t0);
    input.tick(0, false, 0, 0, false, false, false, t0 + DialInput::DIM_AFTER_MS + 1);
    TEST_ASSERT_TRUE(DialInput::Backlight::DIM == input.backlight());

    DialEvents e = input.tick(0, false, 0, 0, false, false, false, t0 + DialInput::DIM_AFTER_MS + 2, true);
    TEST_ASSERT_TRUE(e.wake);
    TEST_ASSERT_FALSE(e.btnDoubleClick);
    TEST_ASSERT_TRUE(DialInput::Backlight::FULL == input.backlight());
}

static void test_btnHold_setsBtnHoldOnThatTick(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, false, 0, 0, false, false, false, t0); // baseline

    DialEvents e = input.tick(0, false, 0, 0, false, false, true, t0 + 10);
    TEST_ASSERT_TRUE(e.btnHold);
    TEST_ASSERT_FALSE(e.btnClick);
    TEST_ASSERT_FALSE(e.btnStop);
}

static void test_btnStop_stillFiresOnRawClick_independentOfSingleClickHold(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, false, 0, 0, false, false, false, t0); // baseline

    // The instant wasClicked edge (btnClicked) still fires btnStop, even
    // when neither the debounced single-click nor hold has resolved yet.
    DialEvents e = input.tick(0, false, 0, 0, true, false, false, t0 + 10);
    TEST_ASSERT_TRUE(e.btnStop);
    TEST_ASSERT_FALSE(e.btnClick);
    TEST_ASSERT_FALSE(e.btnHold);
}

// ---------------------------------------------------------------------------
// consumeClick(): swallowing the single click that follows an acted-on stop
// ---------------------------------------------------------------------------
//
// M5Unified reports one physical press twice: wasClicked() at release, then
// wasSingleClicked() about 500 ms later once the multi-click window closes.
// The belt's emergency stop acts on the first; the second must not then open
// the card menu behind it. The UI says so by calling consumeClick() on the
// tick it acts on btnStop — the swallow is never armed on cards where the
// click is the legitimate menu gesture.

static void test_consumeClick_swallowsTheFollowingSingleClick(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, false, 0, 0, false, false, false, t0); // baseline

    DialEvents stop = input.tick(0, false, 0, 0, true, false, false, t0 + 10);
    TEST_ASSERT_TRUE(stop.btnStop);

    input.consumeClick(t0 + 10); // the UI acted on the stop

    DialEvents e = input.tick(0, false, 0, 0, false, true, false, t0 + 510);
    TEST_ASSERT_FALSE(e.btnClick);
}

static void test_singleClick_afterAnUnconsumedStop_stillFires(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, false, 0, 0, false, false, false, t0); // baseline

    DialEvents stop = input.tick(0, false, 0, 0, true, false, false, t0 + 10);
    TEST_ASSERT_TRUE(stop.btnStop);
    // No consumeClick(): the UI ignored btnStop (belt idle), so the decided
    // click is the card-menu gesture and must survive.

    DialEvents e = input.tick(0, false, 0, 0, false, true, false, t0 + 510);
    TEST_ASSERT_TRUE(e.btnClick);
}

static void test_consumeClick_windowExpires(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, false, 0, 0, false, false, false, t0); // baseline
    input.tick(0, false, 0, 0, true, false, false, t0 + 10);
    input.consumeClick(t0 + 10);

    // 800 ms later is past SWALLOW_CLICK_MS: a genuinely new press gets
    // through rather than being eaten by a stale latch.
    DialEvents e = input.tick(0, false, 0, 0, false, true, false, t0 + 810);
    TEST_ASSERT_TRUE(e.btnClick);
}

static void test_consumeClick_swallowsOnlyOneClick(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, false, 0, 0, false, false, false, t0); // baseline
    input.tick(0, false, 0, 0, true, false, false, t0 + 10);
    input.consumeClick(t0 + 10);

    DialEvents swallowed = input.tick(0, false, 0, 0, false, true, false, t0 + 510);
    TEST_ASSERT_FALSE(swallowed.btnClick);

    // A second press inside the same window is a real one — the latch is
    // spent, not a 700 ms blackout.
    DialEvents e = input.tick(0, false, 0, 0, false, true, false, t0 + 600);
    TEST_ASSERT_TRUE(e.btnClick);
}

static void test_consumeClick_doesNotSwallowHold(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, false, 0, 0, false, false, false, t0); // baseline
    input.tick(0, false, 0, 0, true, false, false, t0 + 10);
    input.consumeClick(t0 + 10);

    DialEvents e = input.tick(0, false, 0, 0, false, false, true, t0 + 510);
    TEST_ASSERT_TRUE(e.btnHold);
}

static void test_wake_btnSingleClickedWhileDim_wakesOnly(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, false, 0, 0, false, false, false, t0); // baseline
    input.tick(0, false, 0, 0, false, false, false, t0 + 120000); // now DIM
    TEST_ASSERT_TRUE(DialInput::Backlight::DIM == input.backlight());

    DialEvents e = input.tick(0, false, 0, 0, false, true, false, t0 + 120001);
    TEST_ASSERT_TRUE(e.wake);
    TEST_ASSERT_FALSE(e.btnClick);
    TEST_ASSERT_TRUE(DialInput::Backlight::FULL == input.backlight());
}

static void test_wake_btnHoldWhileDim_wakesOnly(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, false, 0, 0, false, false, false, t0); // baseline
    input.tick(0, false, 0, 0, false, false, false, t0 + 120000); // now DIM
    TEST_ASSERT_TRUE(DialInput::Backlight::DIM == input.backlight());

    DialEvents e = input.tick(0, false, 0, 0, false, false, true, t0 + 120001);
    TEST_ASSERT_TRUE(e.wake);
    TEST_ASSERT_FALSE(e.btnHold);
    TEST_ASSERT_TRUE(DialInput::Backlight::FULL == input.backlight());
}

// ---------------------------------------------------------------------------
// Backlight dimming
// ---------------------------------------------------------------------------

static void test_backlight_dimAt120s_staysDimAt600s_activityResets(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, false, 0, 0, false, false, false, t0); // baseline
    TEST_ASSERT_TRUE(DialInput::Backlight::FULL == input.backlight());

    input.tick(0, false, 0, 0, false, false, false, t0 + 119999);
    TEST_ASSERT_TRUE(DialInput::Backlight::FULL == input.backlight());

    input.tick(0, false, 0, 0, false, false, false, t0 + 120000);
    TEST_ASSERT_TRUE(DialInput::Backlight::DIM == input.backlight());

    input.tick(0, false, 0, 0, false, false, false, t0 + 599999);
    TEST_ASSERT_TRUE(DialInput::Backlight::DIM == input.backlight());

    // No OFF stage (spec 4.8): still DIM at 600s and beyond, with no further
    // activity — the backlight never turns off on its own.
    input.tick(0, false, 0, 0, false, false, false, t0 + 600000);
    TEST_ASSERT_TRUE(DialInput::Backlight::DIM == input.backlight());

    input.noteActivity(t0 + 600000);
    TEST_ASSERT_TRUE(DialInput::Backlight::FULL == input.backlight());

    input.tick(0, false, 0, 0, false, false, false, t0 + 600100);
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

    input.tick(0, false, 0, 0, false, false, false, t0); // baseline
    input.tick(0, false, 0, 0, false, false, false, t0 + 120000); // now DIM
    TEST_ASSERT_TRUE(DialInput::Backlight::DIM == input.backlight());

    // Touch-down arrives while dim: wakes, swallows the gesture. Only this
    // first tick reports wake.
    DialEvents eDown = input.tick(0, true, 50, 50, false, false, false, t0 + 120001);
    TEST_ASSERT_TRUE(eDown.wake);
    TEST_ASSERT_FALSE(eDown.tap);
    TEST_ASSERT_FALSE(eDown.longPress);

    // Held past the long-press threshold (1000 ms): still swallowed, so no
    // longPress and no further wake.
    DialEvents eHeld = input.tick(0, true, 50, 50, false, false, false, t0 + 120001 + DialInput::HOLD_MS);
    TEST_ASSERT_FALSE(eHeld.wake);
    TEST_ASSERT_FALSE(eHeld.longPress);

    DialEvents eHeldPast = input.tick(0, true, 50, 50, false, false, false, t0 + 120001 + DialInput::HOLD_MS + 500);
    TEST_ASSERT_FALSE(eHeldPast.wake);
    TEST_ASSERT_FALSE(eHeldPast.longPress);

    // Release of the whole swallowed hold: no tap, no longPress, no wake.
    DialEvents eRelease = input.tick(0, false, 0, 0, false, false, false, t0 + 120001 + DialInput::HOLD_MS + 1000);
    TEST_ASSERT_FALSE(eRelease.tap);
    TEST_ASSERT_FALSE(eRelease.longPress);
    TEST_ASSERT_FALSE(eRelease.wake);
}

static void test_wake_encoderWhileDim_rebasesBaselineForNextDetent(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, false, 0, 0, false, false, false, t0); // baseline, establishes activity

    // Advance to the dim threshold with no activity.
    input.tick(0, false, 0, 0, false, false, false, t0 + 120000);
    TEST_ASSERT_TRUE(DialInput::Backlight::DIM == input.backlight());

    // Encoder moves while dim: wakes, swallows the pulses, and rebases the
    // baseline to the count seen on this tick (8), not the old one (0).
    DialEvents eWake = input.tick(8, false, 0, 0, false, false, false, t0 + 120001);
    TEST_ASSERT_TRUE(eWake.wake);
    TEST_ASSERT_EQUAL_INT(0, eWake.detents);
    TEST_ASSERT_TRUE(DialInput::Backlight::FULL == input.backlight());

    // Next tick: delta is from the rebased baseline of 8, i.e. 12-8=4 -> 1
    // detent. If the baseline had stayed at 0, this would wrongly read 3.
    DialEvents eNext = input.tick(12, false, 0, 0, false, false, false, t0 + 120010);
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

    input.tick(0, true, 50, 50, false, false, false, t0); // touch down

    DialEvents e = input.tick(0, false, 0, 0, false, false, false, t0 + DialInput::TAP_MAX_MS);
    TEST_ASSERT_FALSE(e.tap);
}

static void test_tap_releaseJustUnderTapMaxMs_isATap(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, true, 50, 50, false, false, false, t0); // touch down

    DialEvents e = input.tick(0, false, 0, 0, false, false, false, t0 + DialInput::TAP_MAX_MS - 1);
    TEST_ASSERT_TRUE(e.tap);
}

// ---------------------------------------------------------------------------
// Horizontal swipe
// ---------------------------------------------------------------------------

static void test_swipe_right_firesOnceThenZero(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, true, 50, 50, false, false, false, t0); // touch down

    DialEvents e1 = input.tick(0, true, 90, 50, false, false, false, t0 + 100); // +40px right
    TEST_ASSERT_EQUAL_INT(1, e1.swipe);

    DialEvents e2 = input.tick(0, true, 90, 50, false, false, false, t0 + 150); // still held, no further move
    TEST_ASSERT_EQUAL_INT(0, e2.swipe);

    DialEvents e3 = input.tick(0, false, 0, 0, false, false, false, t0 + 200); // release
    TEST_ASSERT_EQUAL_INT(0, e3.swipe);
}

static void test_swipe_left_firesOnce(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, true, 50, 50, false, false, false, t0); // touch down

    DialEvents e1 = input.tick(0, true, 10, 50, false, false, false, t0 + 100); // -40px left
    TEST_ASSERT_EQUAL_INT(-1, e1.swipe);
}

static void test_swipe_diagonal_dyExceedsDx_notASwipe_isADrag(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, true, 50, 50, false, false, false, t0); // touch down

    // dx=40, dy=45: |dx| >= 40 but not > |dy| -> no swipe. Still a drag by
    // distance, so release must not be a tap.
    DialEvents eMid = input.tick(0, true, 90, 95, false, false, false, t0 + 100);
    TEST_ASSERT_EQUAL_INT(0, eMid.swipe);

    DialEvents eRelease = input.tick(0, false, 0, 0, false, false, false, t0 + 150);
    TEST_ASSERT_FALSE(eRelease.tap);
}

static void test_swipe_thenRelease_notTapNotLongPress(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, true, 50, 50, false, false, false, t0); // touch down

    DialEvents eSwipe = input.tick(0, true, 90, 50, false, false, false, t0 + 100);
    TEST_ASSERT_EQUAL_INT(1, eSwipe.swipe);

    DialEvents eRelease = input.tick(0, false, 0, 0, false, false, false, t0 + 200);
    TEST_ASSERT_FALSE(eRelease.tap);
    TEST_ASSERT_FALSE(eRelease.longPress);
}

static void test_swipe_thenHoldPast1000ms_noLongPress(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, true, 50, 50, false, false, false, t0); // touch down
    input.tick(0, true, 90, 50, false, false, false, t0 + 100); // swipe fires

    DialEvents eHeld = input.tick(0, true, 90, 50, false, false, false, t0 + 1100); // past 1000ms hold
    TEST_ASSERT_FALSE(eHeld.longPress);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, eHeld.holdProgress);
}

static void test_swipe_whileDim_wakeOnly_noSwipe(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, false, 0, 0, false, false, false, t0); // baseline
    input.tick(0, false, 0, 0, false, false, false, t0 + 120000); // now DIM
    TEST_ASSERT_TRUE(DialInput::Backlight::DIM == input.backlight());

    // Touch-down arrives while dim: wakes, swallows the gesture.
    DialEvents eDown = input.tick(0, true, 50, 50, false, false, false, t0 + 120001);
    TEST_ASSERT_TRUE(eDown.wake);
    TEST_ASSERT_EQUAL_INT(0, eDown.swipe);
    TEST_ASSERT_TRUE(DialInput::Backlight::FULL == input.backlight());

    // A 40px move within the swallowed gesture must not emit a swipe.
    DialEvents eMove = input.tick(0, true, 90, 50, false, false, false, t0 + 120050);
    TEST_ASSERT_EQUAL_INT(0, eMove.swipe);
    TEST_ASSERT_TRUE(DialInput::Backlight::FULL == input.backlight());
}

static void test_swipe_39px_notASwipe_isADrag(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    input.tick(0, true, 50, 50, false, false, false, t0); // touch down

    // 39px < SWIPE_MIN_PX (40): no swipe, but still exceeds TAP_MAX_MOVE_PX
    // (20) so it's a drag.
    DialEvents eMid = input.tick(0, true, 89, 50, false, false, false, t0 + 100);
    TEST_ASSERT_EQUAL_INT(0, eMid.swipe);

    DialEvents eRelease = input.tick(0, false, 0, 0, false, false, false, t0 + 200);
    TEST_ASSERT_FALSE(eRelease.tap);
}

// ---------------------------------------------------------------------------
// Finger report + claimTouch (hue-ring scrub, spec 4.18 amendment)
// ---------------------------------------------------------------------------

static void test_touchHeld_reportsPositionEveryTickAndStartPoint(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    DialEvents eDown = input.tick(0, true, 120, 20, false, false, false, t0);
    TEST_ASSERT_TRUE(eDown.touchHeld);
    TEST_ASSERT_EQUAL_INT(120, eDown.touchX);
    TEST_ASSERT_EQUAL_INT(20, eDown.touchY);
    TEST_ASSERT_EQUAL_INT(120, eDown.touchStartX);
    TEST_ASSERT_EQUAL_INT(20, eDown.touchStartY);

    DialEvents eMove = input.tick(0, true, 200, 60, false, false, false, t0 + 100);
    TEST_ASSERT_TRUE(eMove.touchHeld);
    TEST_ASSERT_EQUAL_INT(200, eMove.touchX);
    TEST_ASSERT_EQUAL_INT(60, eMove.touchY);
    TEST_ASSERT_EQUAL_INT(120, eMove.touchStartX); // start point is sticky
    TEST_ASSERT_EQUAL_INT(20, eMove.touchStartY);

    DialEvents eUp = input.tick(0, false, 0, 0, false, false, false, t0 + 200);
    TEST_ASSERT_FALSE(eUp.touchHeld);
}

static void test_touchHeld_falseWithNoTouch(void)
{
    DialInput input;
    DialEvents e = input.tick(0, false, 0, 0, false, false, false, 1000);
    TEST_ASSERT_FALSE(e.touchHeld);
}

// A touch that wakes a dimmed backlight is swallowed: no finger report for it.
static void test_touchHeld_falseWhileWakeSwallowed(void)
{
    DialInput input;
    uint32_t t0 = 1000;
    input.tick(0, false, 0, 0, false, false, false, t0);
    DialEvents eDim = input.tick(0, false, 0, 0, false, false, false, t0 + DialInput::DIM_AFTER_MS + 1);
    TEST_ASSERT_TRUE(DialInput::Backlight::DIM == input.backlight());
    (void)eDim;
    DialEvents eDown = input.tick(0, true, 100, 100, false, false, false, t0 + DialInput::DIM_AFTER_MS + 10);
    TEST_ASSERT_TRUE(eDown.wake);
    TEST_ASSERT_FALSE(eDown.touchHeld);
    DialEvents eHold = input.tick(0, true, 100, 100, false, false, false, t0 + DialInput::DIM_AFTER_MS + 200);
    TEST_ASSERT_FALSE(eHold.touchHeld);
}

// claimTouch(): the gesture keeps reporting the finger but produces no swipe,
// no long press (holdProgress 0) and no tap on release.
static void test_claimTouch_mutesSwipeLongPressAndTap(void)
{
    DialInput input;
    uint32_t t0 = 1000;

    DialEvents eDown = input.tick(0, true, 50, 120, false, false, false, t0);
    TEST_ASSERT_TRUE(eDown.touchHeld);
    input.claimTouch();

    // 60 px to the right: would be a swipe if unclaimed.
    DialEvents eMove = input.tick(0, true, 110, 120, false, false, false, t0 + 100);
    TEST_ASSERT_TRUE(eMove.touchHeld);
    TEST_ASSERT_EQUAL_INT(110, eMove.touchX);
    TEST_ASSERT_EQUAL_INT(0, eMove.swipe);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, eMove.holdProgress);

    // Held well past HOLD_MS without moving: would be a long press if unclaimed.
    DialEvents eHeld = input.tick(0, true, 110, 120, false, false, false, t0 + DialInput::HOLD_MS + 200);
    TEST_ASSERT_FALSE(eHeld.longPress);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, eHeld.holdProgress);
    TEST_ASSERT_TRUE(eHeld.touchHeld);

    DialEvents eUp = input.tick(0, false, 0, 0, false, false, false, t0 + DialInput::HOLD_MS + 300);
    TEST_ASSERT_FALSE(eUp.tap);
    TEST_ASSERT_FALSE(eUp.touchHeld);
}

// A quick claimed touch is not a tap either: the claimer already acted on it.
static void test_claimTouch_quickReleaseIsNotATap(void)
{
    DialInput input;
    uint32_t t0 = 1000;
    input.tick(0, true, 50, 120, false, false, false, t0);
    input.claimTouch();
    DialEvents eUp = input.tick(0, false, 0, 0, false, false, false, t0 + 100);
    TEST_ASSERT_FALSE(eUp.tap);
}

// The claim is per gesture: the next touch behaves normally again.
static void test_claimTouch_clearsOnRelease(void)
{
    DialInput input;
    uint32_t t0 = 1000;
    input.tick(0, true, 50, 120, false, false, false, t0);
    input.claimTouch();
    input.tick(0, false, 0, 0, false, false, false, t0 + 100);

    input.tick(0, true, 60, 60, false, false, false, t0 + 500);
    DialEvents eUp = input.tick(0, false, 0, 0, false, false, false, t0 + 600);
    TEST_ASSERT_TRUE(eUp.tap);
    TEST_ASSERT_EQUAL_INT(60, eUp.tapX);
}

// claimTouch() with no gesture in progress is a harmless no-op.
static void test_claimTouch_withoutTouch_isNoOp(void)
{
    DialInput input;
    input.claimTouch();
    input.tick(0, true, 60, 60, false, false, false, 1000);
    DialEvents eUp = input.tick(0, false, 0, 0, false, false, false, 1100);
    TEST_ASSERT_TRUE(eUp.tap);
}

// touchBegan: the gesture-start edge (spec 4.18 amendment, hue-ring scrub).
static void test_touchBegan_onlyOnTheDownTick(void)
{
    DialInput input;
    uint32_t t0 = 1000;
    DialEvents eDown = input.tick(0, true, 120, 20, false, false, false, t0);
    TEST_ASSERT_TRUE(eDown.touchBegan);
    TEST_ASSERT_TRUE(eDown.touchHeld);
    DialEvents eHold = input.tick(0, true, 121, 20, false, false, false, t0 + 50);
    TEST_ASSERT_FALSE(eHold.touchBegan);
    TEST_ASSERT_TRUE(eHold.touchHeld);
    DialEvents eUp = input.tick(0, false, 0, 0, false, false, false, t0 + 100);
    TEST_ASSERT_FALSE(eUp.touchBegan);
    // Next gesture begins again.
    DialEvents eDown2 = input.tick(0, true, 60, 60, false, false, false, t0 + 500);
    TEST_ASSERT_TRUE(eDown2.touchBegan);
}
static void test_touchBegan_falseWhenWakeSwallowed(void)
{
    DialInput input;
    uint32_t t0 = 1000;
    input.tick(0, false, 0, 0, false, false, false, t0);
    input.tick(0, false, 0, 0, false, false, false, t0 + DialInput::DIM_AFTER_MS + 1);
    DialEvents eDown = input.tick(0, true, 100, 100, false, false, false, t0 + DialInput::DIM_AFTER_MS + 10);
    TEST_ASSERT_TRUE(eDown.wake);
    TEST_ASSERT_FALSE(eDown.touchBegan);
}

// ---------------------------------------------------------------------------
// Wraparound-safe time maths
// ---------------------------------------------------------------------------

static void test_time_wraparoundSafe(void)
{
    DialInput input;
    uint32_t t0 = UINT32_MAX - 1000;

    input.tick(0, false, 0, 0, false, false, false, t0); // baseline near wraparound
    TEST_ASSERT_TRUE(DialInput::Backlight::FULL == input.backlight());

    uint32_t t1 = t0 + (uint32_t)120000; // wraps past UINT32_MAX
    input.tick(0, false, 0, 0, false, false, false, t1);
    TEST_ASSERT_TRUE(DialInput::Backlight::DIM == input.backlight());

    // No OFF stage (spec 4.8): still DIM well past the old OFF threshold,
    // even across a further millis() wrap.
    uint32_t t2 = t0 + (uint32_t)600000; // also wraps
    input.tick(0, false, 0, 0, false, false, false, t2);
    TEST_ASSERT_TRUE(DialInput::Backlight::DIM == input.backlight());
}

// ---------------------------------------------------------------------------
// idleFor() — the idle-return clock (spec 4.8 amendment 2026-09-08)
// ---------------------------------------------------------------------------

static void test_idleFor_falseBeforeFirstTick(void)
{
    DialInput input;
    TEST_ASSERT_FALSE(input.idleFor(5000000, 1));
}

static void test_idleFor_trueOnTheDimTick_resetByInput(void)
{
    DialInput input;
    uint32_t t = 1000;
    input.tick(0, false, 0, 0, false, false, false, t); // baseline = activity

    TEST_ASSERT_FALSE(input.idleFor(t + DialInput::DIM_AFTER_MS - 1, DialInput::DIM_AFTER_MS));
    TEST_ASSERT_TRUE(input.idleFor(t + DialInput::DIM_AFTER_MS, DialInput::DIM_AFTER_MS));

    // A detent at t+60 s restarts the clock.
    input.tick(4, false, 0, 0, false, false, false, t + 60000);
    TEST_ASSERT_FALSE(input.idleFor(t + DialInput::DIM_AFTER_MS, DialInput::DIM_AFTER_MS));
    TEST_ASSERT_TRUE(input.idleFor(t + 60000 + DialInput::DIM_AFTER_MS, DialInput::DIM_AFTER_MS));
}

static void test_idleFor_resetByNoteActivity_andWrapSafe(void)
{
    DialInput input;
    uint32_t t0 = UINT32_MAX - 1000;
    input.tick(0, false, 0, 0, false, false, false, t0);
    input.noteActivity(t0 + 500); // e.g. an auto-show raising a card
    TEST_ASSERT_FALSE(input.idleFor(t0 + 500 + 119999, 120000)); // wraps
    TEST_ASSERT_TRUE(input.idleFor(t0 + 500 + 120000, 120000));  // wraps
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();

    RUN_TEST(test_detents_fourPulses_oneDetent);
    RUN_TEST(test_detents_sixPulses_thenTwoMore_secondDetent);
    RUN_TEST(test_detents_negativeDirection);
    RUN_TEST(test_tap_insideTapWindow);
    RUN_TEST(test_tap_reportsTouchDownPosition);
    RUN_TEST(test_tap_outsideTapWindow_notATap);
    RUN_TEST(test_drag_exceedsMovement_notTapNotLongPress);
    RUN_TEST(test_longPress_firesOnceAt1000ms_progressHalfAt500ms);
    RUN_TEST(test_wake_inputWhileDim_yieldsWakeOnly);
    RUN_TEST(test_wake_touchWhileDim_swallowsGestureUntilRelease);
    RUN_TEST(test_wake_touchWhileDim_swallowsLongPressUntilRelease);
    RUN_TEST(test_btnClick_singleClick_setsBtnClickOnlyOnThatTick);
    RUN_TEST(test_btnDoubleClick_setsBtnDoubleClickOnlyOnThatTick);
    RUN_TEST(test_btnDoubleClick_whileDim_wakesOnly);
    RUN_TEST(test_btnHold_setsBtnHoldOnThatTick);
    RUN_TEST(test_btnStop_stillFiresOnRawClick_independentOfSingleClickHold);
    RUN_TEST(test_consumeClick_swallowsTheFollowingSingleClick);
    RUN_TEST(test_singleClick_afterAnUnconsumedStop_stillFires);
    RUN_TEST(test_consumeClick_windowExpires);
    RUN_TEST(test_consumeClick_swallowsOnlyOneClick);
    RUN_TEST(test_consumeClick_doesNotSwallowHold);
    RUN_TEST(test_wake_btnSingleClickedWhileDim_wakesOnly);
    RUN_TEST(test_wake_btnHoldWhileDim_wakesOnly);
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
    RUN_TEST(test_touchHeld_reportsPositionEveryTickAndStartPoint);
    RUN_TEST(test_touchHeld_falseWithNoTouch);
    RUN_TEST(test_touchHeld_falseWhileWakeSwallowed);
    RUN_TEST(test_claimTouch_mutesSwipeLongPressAndTap);
    RUN_TEST(test_claimTouch_quickReleaseIsNotATap);
    RUN_TEST(test_claimTouch_clearsOnRelease);
    RUN_TEST(test_claimTouch_withoutTouch_isNoOp);
    RUN_TEST(test_touchBegan_onlyOnTheDownTick);
    RUN_TEST(test_touchBegan_falseWhenWakeSwallowed);
    RUN_TEST(test_time_wraparoundSafe);
    RUN_TEST(test_idleFor_falseBeforeFirstTick);
    RUN_TEST(test_idleFor_trueOnTheDimTick_resetByInput);
    RUN_TEST(test_idleFor_resetByNoteActivity_andWrapSafe);

    return UNITY_END();
}
