#include <unity.h>
#include <string.h>
#include "Geo.h"

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// compass8() sector boundaries: 45deg sectors centred on each compass point,
// lower bound inclusive (e.g. N covers [337.5, 360) U [0, 22.5)).
// ---------------------------------------------------------------------------

static void test_compass8_north(void)
{
    TEST_ASSERT_EQUAL_STRING("N", Geo::compass8(0.0f));
}

static void test_compass8_northBoundary_337_5(void)
{
    TEST_ASSERT_EQUAL_STRING("N", Geo::compass8(337.5f));
}

static void test_compass8_northNear360(void)
{
    TEST_ASSERT_EQUAL_STRING("N", Geo::compass8(359.0f));
}

static void test_compass8_northEastBoundary_22_5(void)
{
    TEST_ASSERT_EQUAL_STRING("NE", Geo::compass8(22.5f));
}

static void test_compass8_northEast_44(void)
{
    TEST_ASSERT_EQUAL_STRING("NE", Geo::compass8(44.0f));
}

static void test_compass8_northEast_45(void)
{
    TEST_ASSERT_EQUAL_STRING("NE", Geo::compass8(45.0f));
}

static void test_compass8_eastBoundary_67_5(void)
{
    TEST_ASSERT_EQUAL_STRING("E", Geo::compass8(67.5f));
}

static void test_compass8_east_90(void)
{
    TEST_ASSERT_EQUAL_STRING("E", Geo::compass8(90.0f));
}

static void test_compass8_southEast_135(void)
{
    TEST_ASSERT_EQUAL_STRING("SE", Geo::compass8(135.0f));
}

static void test_compass8_south_180(void)
{
    TEST_ASSERT_EQUAL_STRING("S", Geo::compass8(180.0f));
}

static void test_compass8_southWest_225(void)
{
    TEST_ASSERT_EQUAL_STRING("SW", Geo::compass8(225.0f));
}

static void test_compass8_west_270(void)
{
    TEST_ASSERT_EQUAL_STRING("W", Geo::compass8(270.0f));
}

static void test_compass8_northWest_315(void)
{
    TEST_ASSERT_EQUAL_STRING("NW", Geo::compass8(315.0f));
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_compass8_north);
    RUN_TEST(test_compass8_northBoundary_337_5);
    RUN_TEST(test_compass8_northNear360);
    RUN_TEST(test_compass8_northEastBoundary_22_5);
    RUN_TEST(test_compass8_northEast_44);
    RUN_TEST(test_compass8_northEast_45);
    RUN_TEST(test_compass8_eastBoundary_67_5);
    RUN_TEST(test_compass8_east_90);
    RUN_TEST(test_compass8_southEast_135);
    RUN_TEST(test_compass8_south_180);
    RUN_TEST(test_compass8_southWest_225);
    RUN_TEST(test_compass8_west_270);
    RUN_TEST(test_compass8_northWest_315);
    return UNITY_END();
}
