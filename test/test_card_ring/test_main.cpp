#include <unity.h>
#include "CardRing.h"

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// Initial state
// ---------------------------------------------------------------------------

static void test_startsAtClock(void)
{
    CardRing ring;
    TEST_ASSERT_TRUE(CardId::CLOCK == ring.current());
}

// ---------------------------------------------------------------------------
// next() / prev() with wraparound
// ---------------------------------------------------------------------------

static void test_next_fromClock_wrapsToTreadmill(void)
{
    CardRing ring;
    ring.next();
    TEST_ASSERT_TRUE(CardId::TREADMILL == ring.current());
}

static void test_next_fromTreadmill_wrapsToClock(void)
{
    CardRing ring;
    ring.next(); // -> TREADMILL
    ring.next(); // -> wraps back to CLOCK
    TEST_ASSERT_TRUE(CardId::CLOCK == ring.current());
}

static void test_prev_fromTreadmill_wrapsToClock(void)
{
    CardRing ring;
    ring.set(CardId::TREADMILL);
    ring.prev();
    TEST_ASSERT_TRUE(CardId::CLOCK == ring.current());
}

static void test_prev_fromClock_wrapsToTreadmill(void)
{
    CardRing ring;
    ring.prev(); // starts at CLOCK, wraps backward to TREADMILL
    TEST_ASSERT_TRUE(CardId::TREADMILL == ring.current());
}

// ---------------------------------------------------------------------------
// set()
// ---------------------------------------------------------------------------

static void test_set_movesDirectlyToGivenCard(void)
{
    CardRing ring;
    ring.set(CardId::TREADMILL);
    TEST_ASSERT_TRUE(CardId::TREADMILL == ring.current());

    ring.set(CardId::CLOCK);
    TEST_ASSERT_TRUE(CardId::CLOCK == ring.current());
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();

    RUN_TEST(test_startsAtClock);
    RUN_TEST(test_next_fromClock_wrapsToTreadmill);
    RUN_TEST(test_next_fromTreadmill_wrapsToClock);
    RUN_TEST(test_prev_fromTreadmill_wrapsToClock);
    RUN_TEST(test_prev_fromClock_wrapsToTreadmill);
    RUN_TEST(test_set_movesDirectlyToGivenCard);

    return UNITY_END();
}
