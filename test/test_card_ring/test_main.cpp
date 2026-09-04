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
// next() / prev() with wraparound (ring order: TREADMILL -> CLOCK -> FLIGHTS)
// ---------------------------------------------------------------------------

static void test_next_advancesThroughAllThreeCards_thenWraps(void)
{
    CardRing ring; // starts at CLOCK
    ring.next();
    TEST_ASSERT_TRUE(CardId::FLIGHTS == ring.current());
    ring.next();
    TEST_ASSERT_TRUE(CardId::TREADMILL == ring.current());
    ring.next();
    TEST_ASSERT_TRUE(CardId::CLOCK == ring.current());
}

static void test_prev_wrapsBackwardThroughAllThreeCards(void)
{
    CardRing ring; // starts at CLOCK
    ring.prev();
    TEST_ASSERT_TRUE(CardId::TREADMILL == ring.current());
    ring.prev();
    TEST_ASSERT_TRUE(CardId::FLIGHTS == ring.current());
    ring.prev();
    TEST_ASSERT_TRUE(CardId::CLOCK == ring.current());
}

// ---------------------------------------------------------------------------
// set()
// ---------------------------------------------------------------------------

static void test_set_movesDirectlyToGivenCard(void)
{
    CardRing ring;
    ring.set(CardId::TREADMILL);
    TEST_ASSERT_TRUE(CardId::TREADMILL == ring.current());

    ring.set(CardId::FLIGHTS);
    TEST_ASSERT_TRUE(CardId::FLIGHTS == ring.current());

    ring.set(CardId::CLOCK);
    TEST_ASSERT_TRUE(CardId::CLOCK == ring.current());
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();

    RUN_TEST(test_startsAtClock);
    RUN_TEST(test_next_advancesThroughAllThreeCards_thenWraps);
    RUN_TEST(test_prev_wrapsBackwardThroughAllThreeCards);
    RUN_TEST(test_set_movesDirectlyToGivenCard);

    return UNITY_END();
}
