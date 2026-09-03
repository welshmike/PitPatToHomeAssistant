#include <unity.h>
#include <string.h>
#include "DialFormat.h"
#include "TreadmillData.h"

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// formatDuration
// ---------------------------------------------------------------------------

static void test_formatDuration_zero(void)
{
    char out[16] = {0};
    DialFormat::formatDuration(0, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("00:00", out);
}

static void test_formatDuration_59sec(void)
{
    char out[16] = {0};
    DialFormat::formatDuration(59, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("00:59", out);
}

static void test_formatDuration_oneHourMinusOneSec(void)
{
    char out[16] = {0};
    DialFormat::formatDuration(3599, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("59:59", out);
}

static void test_formatDuration_exactlyOneHour(void)
{
    char out[16] = {0};
    DialFormat::formatDuration(3600, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("1:00:00", out);
}

static void test_formatDuration_tenHoursOneMinOneSec(void)
{
    char out[16] = {0};
    DialFormat::formatDuration(36000 + 61, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("10:01:01", out);
}

static void test_formatDuration_truncatesInTinyBuffer(void)
{
    char out[4] = {0};
    // 3600s -> "1:00:00", must truncate safely to 3 chars + NUL.
    DialFormat::formatDuration(3600, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("1:0", out);
    TEST_ASSERT_EQUAL(0, out[3]);
}

// ---------------------------------------------------------------------------
// formatDistanceKm
// ---------------------------------------------------------------------------

static void test_formatDistanceKm_roundsToTwoDecimals(void)
{
    char out[16] = {0};
    DialFormat::formatDistanceKm(1.234f, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("1.23", out);
}

static void test_formatDistanceKm_negativeClampsToZero(void)
{
    char out[16] = {0};
    DialFormat::formatDistanceKm(-1.0f, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("0.00", out);
}

// ---------------------------------------------------------------------------
// formatSteps
// ---------------------------------------------------------------------------

static void test_formatSteps_zero(void)
{
    char out[16] = {0};
    DialFormat::formatSteps(0, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("0", out);
}

static void test_formatSteps_plainInteger(void)
{
    char out[16] = {0};
    DialFormat::formatSteps(12345, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("12345", out);
}

// ---------------------------------------------------------------------------
// formatSpeedMph
// ---------------------------------------------------------------------------

static void test_formatSpeedMph_oneDecimal(void)
{
    char out[16] = {0};
    DialFormat::formatSpeedMph(2.34f, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("2.3", out);
}

// ---------------------------------------------------------------------------
// speedToAngle
// ---------------------------------------------------------------------------

static void test_speedToAngle_zeroSpeedIsZeroDegrees(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, DialFormat::speedToAngle(0.0f));
}

static void test_speedToAngle_maxSpeedIs300Degrees(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 300.0f, DialFormat::speedToAngle(SPEED_MAX_MPH));
}

static void test_speedToAngle_1_9mphIs150Degrees(void)
{
    // 1.9 mph is half of SPEED_MAX_MPH (3.8) -> half of 300 deg = 150 deg.
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 150.0f, DialFormat::speedToAngle(1.9f));
}

static void test_speedToAngle_negativeClampsToZero(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, DialFormat::speedToAngle(-1.0f));
}

static void test_speedToAngle_aboveMaxClampsTo300(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 300.0f, DialFormat::speedToAngle(10.0f));
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();

    RUN_TEST(test_formatDuration_zero);
    RUN_TEST(test_formatDuration_59sec);
    RUN_TEST(test_formatDuration_oneHourMinusOneSec);
    RUN_TEST(test_formatDuration_exactlyOneHour);
    RUN_TEST(test_formatDuration_tenHoursOneMinOneSec);
    RUN_TEST(test_formatDuration_truncatesInTinyBuffer);

    RUN_TEST(test_formatDistanceKm_roundsToTwoDecimals);
    RUN_TEST(test_formatDistanceKm_negativeClampsToZero);

    RUN_TEST(test_formatSteps_zero);
    RUN_TEST(test_formatSteps_plainInteger);

    RUN_TEST(test_formatSpeedMph_oneDecimal);

    RUN_TEST(test_speedToAngle_zeroSpeedIsZeroDegrees);
    RUN_TEST(test_speedToAngle_maxSpeedIs300Degrees);
    RUN_TEST(test_speedToAngle_1_9mphIs150Degrees);
    RUN_TEST(test_speedToAngle_negativeClampsToZero);
    RUN_TEST(test_speedToAngle_aboveMaxClampsTo300);

    return UNITY_END();
}
