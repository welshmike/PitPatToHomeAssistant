#include <unity.h>
#include <string.h>
#include "Geo.h"

void setUp(void) {}
void tearDown(void) {}

// Central London -> Heathrow. Expected values computed with a standalone
// Python haversine/initial-bearing check (WGS-84 spherical, R = 3958.8 mi):
// distance ~= 14.499 mi, bearing ~= 261.94 deg (just south of due west,
// matching Heathrow being almost directly west of central London).
static const float kLondonLat = 51.5074f;
static const float kLondonLon = -0.1278f;
static const float kHeathrowLat = 51.4775f;
static const float kHeathrowLon = -0.4614f;

static void test_distanceMiles_londonToHeathrow(void)
{
    const float d = Geo::distanceMiles(kLondonLat, kLondonLon, kHeathrowLat, kHeathrowLon);
    TEST_ASSERT_FLOAT_WITHIN(0.4f, 14.5f, d);
}

static void test_distanceMiles_samePoint_isZero(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, Geo::distanceMiles(kLondonLat, kLondonLon, kLondonLat, kLondonLon));
}

static void test_bearingDeg_londonToHeathrow(void)
{
    const float b = Geo::bearingDeg(kLondonLat, kLondonLon, kHeathrowLat, kHeathrowLon);
    TEST_ASSERT_FLOAT_WITHIN(3.0f, 262.0f, b);
}

static void test_bearingDeg_dueNorth(void)
{
    // Same longitude, target north of origin -> bearing 0.
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 0.0f, Geo::bearingDeg(51.0f, 0.0f, 52.0f, 0.0f));
}

static void test_bearingDeg_dueEast(void)
{
    // Same latitude, target east of origin -> bearing 90.
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 90.0f, Geo::bearingDeg(0.0f, 0.0f, 0.0f, 1.0f));
}

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
    RUN_TEST(test_distanceMiles_londonToHeathrow);
    RUN_TEST(test_distanceMiles_samePoint_isZero);
    RUN_TEST(test_bearingDeg_londonToHeathrow);
    RUN_TEST(test_bearingDeg_dueNorth);
    RUN_TEST(test_bearingDeg_dueEast);
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
