#include <unity.h>
#include "ClockFace.h"

void setUp(void) {}
void tearDown(void) {}

static const int kCx = 120;
static const int kCy = 120;

// ---------------------------------------------------------------------------
// Angle helpers
// ---------------------------------------------------------------------------

static void test_hourAngle_twelveOClock(void)
{
    TEST_ASSERT_EQUAL_FLOAT(0.0f, ClockFace::hourAngle(12, 0));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, ClockFace::hourAngle(0, 0));
}

static void test_hourAngle_threeOClock(void)
{
    TEST_ASSERT_EQUAL_FLOAT(90.0f, ClockFace::hourAngle(3, 0));
}

static void test_hourAngle_sixThirty(void)
{
    TEST_ASSERT_EQUAL_FLOAT(195.0f, ClockFace::hourAngle(6, 30));
}

static void test_minuteAngle_thirtyThirty(void)
{
    TEST_ASSERT_EQUAL_FLOAT(183.0f, ClockFace::minuteAngle(30, 30));
}

static void test_secondAngle_fifteen(void)
{
    TEST_ASSERT_EQUAL_FLOAT(90.0f, ClockFace::secondAngle(15));
}

static void test_angles_at_elevenFiftyNineFiftyNine(void)
{
    // 11:59:59 — hands are just about to wrap to 12:00:00/0/0. hourAngle
    // only takes (h, m), so at m=59 it's 330 + 29.5 = 359.5deg: short of a
    // full hour tick but still visibly "almost at 12".
    const float hourDeg = ClockFace::hourAngle(11, 59);
    TEST_ASSERT_TRUE(hourDeg < 360.0f);
    TEST_ASSERT_TRUE(hourDeg > 359.0f);

    TEST_ASSERT_FLOAT_WITHIN(0.01f, 359.9f, ClockFace::minuteAngle(59, 59));
    TEST_ASSERT_EQUAL_FLOAT(354.0f, ClockFace::secondAngle(59));
}

// ---------------------------------------------------------------------------
// hand() endpoints
// ---------------------------------------------------------------------------

static void test_hands_at_midnight_point_straight_up(void)
{
    const int len = 80;
    const HandLine hh = ClockFace::hand(ClockFace::hourAngle(12, 0), kCx, kCy, len);
    const HandLine mm = ClockFace::hand(ClockFace::minuteAngle(0, 0), kCx, kCy, len);
    const HandLine ss = ClockFace::hand(ClockFace::secondAngle(0), kCx, kCy, len);

    TEST_ASSERT_INT_WITHIN(1, kCx, hh.x1);
    TEST_ASSERT_INT_WITHIN(1, kCy - len, hh.y1);
    TEST_ASSERT_INT_WITHIN(1, kCx, mm.x1);
    TEST_ASSERT_INT_WITHIN(1, kCy - len, mm.y1);
    TEST_ASSERT_INT_WITHIN(1, kCx, ss.x1);
    TEST_ASSERT_INT_WITHIN(1, kCy - len, ss.y1);

    TEST_ASSERT_EQUAL_INT(kCx, hh.x0);
    TEST_ASSERT_EQUAL_INT(kCy, hh.y0);
}

static void test_hourHand_at_three_points_right(void)
{
    const int len = 56;
    const HandLine hh = ClockFace::hand(ClockFace::hourAngle(3, 0), kCx, kCy, len);
    TEST_ASSERT_INT_WITHIN(1, kCx + len, hh.x1);
    TEST_ASSERT_INT_WITHIN(1, kCy, hh.y1);
}

static void test_secondHand_at_fifteen_points_right(void)
{
    const int len = 96;
    const HandLine ss = ClockFace::hand(ClockFace::secondAngle(15), kCx, kCy, len);
    TEST_ASSERT_INT_WITHIN(1, kCx + len, ss.x1);
    TEST_ASSERT_INT_WITHIN(1, kCy, ss.y1);
}

// ---------------------------------------------------------------------------
// tick()
// ---------------------------------------------------------------------------

static void test_tick0_is_vertical_above_centre(void)
{
    const HandLine t = ClockFace::tick(0, kCx, kCy, 104, 112);
    TEST_ASSERT_INT_WITHIN(1, kCx, t.x0);
    TEST_ASSERT_INT_WITHIN(1, kCx, t.x1);
    TEST_ASSERT_TRUE(t.y0 < kCy);
    TEST_ASSERT_TRUE(t.y1 < kCy);
    TEST_ASSERT_TRUE(t.y1 < t.y0); // r1 (112) further from centre than r0 (104)
}

static void test_tick3_is_horizontal_right(void)
{
    const HandLine t = ClockFace::tick(3, kCx, kCy, 104, 112);
    TEST_ASSERT_INT_WITHIN(1, kCy, t.y0);
    TEST_ASSERT_INT_WITHIN(1, kCy, t.y1);
    TEST_ASSERT_TRUE(t.x0 > kCx);
    TEST_ASSERT_TRUE(t.x1 > kCx);
    TEST_ASSERT_TRUE(t.x1 > t.x0);
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_hourAngle_twelveOClock);
    RUN_TEST(test_hourAngle_threeOClock);
    RUN_TEST(test_hourAngle_sixThirty);
    RUN_TEST(test_minuteAngle_thirtyThirty);
    RUN_TEST(test_secondAngle_fifteen);
    RUN_TEST(test_angles_at_elevenFiftyNineFiftyNine);
    RUN_TEST(test_hands_at_midnight_point_straight_up);
    RUN_TEST(test_hourHand_at_three_points_right);
    RUN_TEST(test_secondHand_at_fifteen_points_right);
    RUN_TEST(test_tick0_is_vertical_above_centre);
    RUN_TEST(test_tick3_is_horizontal_right);
    return UNITY_END();
}
