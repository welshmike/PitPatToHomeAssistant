#include <unity.h>

#include "SpeedSelector.h"
#include "TreadmillData.h"

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// open()
// ---------------------------------------------------------------------------

static void test_open_setsValueToDefault(void)
{
    SpeedSelector sel;
    sel.open(1.0f, 1000);
    TEST_ASSERT_TRUE(sel.isOpen());
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, sel.value());
}

static void test_open_clampsBelowMinToSpeedMin(void)
{
    SpeedSelector sel;
    sel.open(0.3f, 1000);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, SPEED_MIN_MPH, sel.value());
}

static void test_open_clampsAboveMaxToSpeedMax(void)
{
    SpeedSelector sel;
    sel.open(9.0f, 1000);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, SPEED_MAX_MPH, sel.value());
}

// ---------------------------------------------------------------------------
// step()
// ---------------------------------------------------------------------------

static void test_step_plusThree_addsPointSixMph(void)
{
    SpeedSelector sel;
    sel.open(1.0f, 1000);
    sel.step(3, 1000);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.6f, sel.value());
}

static void test_step_minusTen_clampsToSpeedMin(void)
{
    SpeedSelector sel;
    sel.open(1.0f, 1000);
    sel.step(-10, 1000);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, SPEED_MIN_MPH, sel.value());
}

static void test_step_plusTwenty_clampsToSpeedMax(void)
{
    SpeedSelector sel;
    sel.open(1.0f, 1000);
    sel.step(20, 1000);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, SPEED_MAX_MPH, sel.value());
}

static void test_step_whenClosed_doesNotChangeValueOrOpenState(void)
{
    SpeedSelector sel;
    sel.open(1.0f, 1000);
    sel.close();
    sel.step(3, 1000);
    TEST_ASSERT_FALSE(sel.isOpen());
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, sel.value());
}

// ---------------------------------------------------------------------------
// tick() / timeout
// ---------------------------------------------------------------------------

static void test_tick_justBeforeTimeout_returnsFalseAndStaysOpen(void)
{
    SpeedSelector sel;
    sel.open(1.0f, 1000);
    bool fired = sel.tick(1000 + 19999);
    TEST_ASSERT_FALSE(fired);
    TEST_ASSERT_TRUE(sel.isOpen());
}

static void test_tick_atExactTimeout_returnsTrueAndCloses(void)
{
    SpeedSelector sel;
    sel.open(1.0f, 1000);
    bool fired = sel.tick(1000 + 20000);
    TEST_ASSERT_TRUE(fired);
    TEST_ASSERT_FALSE(sel.isOpen());
}

static void test_step_refreshesActivity_pushesOutTimeout(void)
{
    SpeedSelector sel;
    sel.open(1.0f, 1000);
    sel.step(1, 15000);

    // 19999ms after the step's activity timestamp: still open.
    TEST_ASSERT_FALSE(sel.tick(15000 + 19999));
    TEST_ASSERT_TRUE(sel.isOpen());

    // 20000ms after the step's activity timestamp: times out.
    TEST_ASSERT_TRUE(sel.tick(15000 + 20000));
    TEST_ASSERT_FALSE(sel.isOpen());
}

static void test_tick_afterClose_returnsFalse(void)
{
    SpeedSelector sel;
    sel.open(1.0f, 1000);
    sel.close();
    TEST_ASSERT_FALSE(sel.tick(1000 + 20000));
}

static void test_tick_wraparound_stillFiresAtTimeout(void)
{
    SpeedSelector sel;
    uint32_t openAt = UINT32_MAX - 100;
    sel.open(1.0f, openAt);

    // 19900ms later wraps past UINT32_MAX back around to 19900 - 100 = 19800... but
    // the elapsed time from openAt to 19900 (post-wrap) is 100 + 19900 = 20000ms.
    bool fired = sel.tick(19900);
    TEST_ASSERT_TRUE(fired);
    TEST_ASSERT_FALSE(sel.isOpen());
}

// ---------------------------------------------------------------------------
// close()
// ---------------------------------------------------------------------------

static void test_close_isIdempotent(void)
{
    SpeedSelector sel;
    sel.open(1.0f, 1000);
    sel.close();
    sel.close();
    TEST_ASSERT_FALSE(sel.isOpen());
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();

    RUN_TEST(test_open_setsValueToDefault);
    RUN_TEST(test_open_clampsBelowMinToSpeedMin);
    RUN_TEST(test_open_clampsAboveMaxToSpeedMax);

    RUN_TEST(test_step_plusThree_addsPointSixMph);
    RUN_TEST(test_step_minusTen_clampsToSpeedMin);
    RUN_TEST(test_step_plusTwenty_clampsToSpeedMax);
    RUN_TEST(test_step_whenClosed_doesNotChangeValueOrOpenState);

    RUN_TEST(test_tick_justBeforeTimeout_returnsFalseAndStaysOpen);
    RUN_TEST(test_tick_atExactTimeout_returnsTrueAndCloses);
    RUN_TEST(test_step_refreshesActivity_pushesOutTimeout);
    RUN_TEST(test_tick_afterClose_returnsFalse);
    RUN_TEST(test_tick_wraparound_stillFiresAtTimeout);

    RUN_TEST(test_close_isIdempotent);

    return UNITY_END();
}
