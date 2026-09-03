#include <unity.h>
#include "TimeMath.h"

void setUp(void) {}
void tearDown(void) {}

static void test_epoch_zero(void)
{
    TEST_ASSERT_EQUAL_INT64(0, TimeMath::utcToEpoch(1970, 1, 1, 0, 0, 0));
}

static void test_epoch_y2k_plus_two_months(void)
{
    TEST_ASSERT_EQUAL_INT64(951868800, TimeMath::utcToEpoch(2000, 3, 1, 0, 0, 0));
}

static void test_epoch_leap_day(void)
{
    TEST_ASSERT_EQUAL_INT64(1709164800, TimeMath::utcToEpoch(2024, 2, 29, 0, 0, 0));
}

static void test_epoch_present_day(void)
{
    TEST_ASSERT_EQUAL_INT64(1788436800, TimeMath::utcToEpoch(2026, 9, 3, 12, 0, 0));
}

static void test_epoch_far_future(void)
{
    TEST_ASSERT_EQUAL_INT64(4102444800, TimeMath::utcToEpoch(2100, 1, 1, 0, 0, 0));
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_epoch_zero);
    RUN_TEST(test_epoch_y2k_plus_two_months);
    RUN_TEST(test_epoch_leap_day);
    RUN_TEST(test_epoch_present_day);
    RUN_TEST(test_epoch_far_future);
    return UNITY_END();
}
