#include <unity.h>
#include <stdint.h>
#include <math.h>
#include "CardMenu.h"
#include "CardRing.h"

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// open() / isOpen() / highlight()
// ---------------------------------------------------------------------------

static void test_open_setsOpenAndHighlightsCurrent(void)
{
    CardMenu menu;
    menu.open(CardId::FLIGHTS, 1000);
    TEST_ASSERT_TRUE(menu.isOpen());
    TEST_ASSERT_TRUE(CardId::FLIGHTS == menu.highlight());
}

static void test_close_isIdempotent(void)
{
    CardMenu menu;
    menu.open(CardId::CLOCK, 1000);
    menu.close();
    menu.close();
    TEST_ASSERT_FALSE(menu.isOpen());
}

// ---------------------------------------------------------------------------
// detents() — wraps both ways, no-op when closed
// ---------------------------------------------------------------------------

static void test_detents_advancesThroughRing_wrapsForward(void)
{
    CardMenu menu;
    menu.open(CardId::TREADMILL, 1000);

    menu.detents(1, 1010);
    TEST_ASSERT_TRUE(CardId::CLOCK == menu.highlight());
    menu.detents(1, 1020);
    TEST_ASSERT_TRUE(CardId::FLIGHTS == menu.highlight());
    menu.detents(1, 1030);
    TEST_ASSERT_TRUE(CardId::LIGHT_OFFICE == menu.highlight());
    menu.detents(1, 1040);
    TEST_ASSERT_TRUE(CardId::LIGHT_LAMP == menu.highlight());
    menu.detents(1, 1050); // wraps past the end back to TREADMILL
    TEST_ASSERT_TRUE(CardId::TREADMILL == menu.highlight());
}

static void test_detents_negative_wrapsBackward(void)
{
    CardMenu menu;
    menu.open(CardId::TREADMILL, 1000);

    menu.detents(-1, 1010); // wraps past the start to LIGHT_LAMP
    TEST_ASSERT_TRUE(CardId::LIGHT_LAMP == menu.highlight());
    menu.detents(-1, 1020);
    TEST_ASSERT_TRUE(CardId::LIGHT_OFFICE == menu.highlight());
}

static void test_detents_multiStep_wrapsCorrectly(void)
{
    CardMenu menu;
    menu.open(CardId::CLOCK, 1000); // index 1

    menu.detents(7, 1010); // 1 + 7 = 8, mod 5 = 3 -> LIGHT_OFFICE
    TEST_ASSERT_TRUE(CardId::LIGHT_OFFICE == menu.highlight());
}

static void test_detents_whenClosed_isNoOp(void)
{
    CardMenu menu;
    menu.open(CardId::CLOCK, 1000);
    menu.close();
    menu.detents(1, 1010);
    TEST_ASSERT_TRUE(CardId::CLOCK == menu.highlight());
    TEST_ASSERT_FALSE(menu.isOpen());
}

// ---------------------------------------------------------------------------
// select()
// ---------------------------------------------------------------------------

static void test_select_returnsHighlightAndCloses(void)
{
    CardMenu menu;
    menu.open(CardId::TREADMILL, 1000);
    menu.detents(2, 1010); // -> FLIGHTS

    CardId picked = menu.select();
    TEST_ASSERT_TRUE(CardId::FLIGHTS == picked);
    TEST_ASSERT_FALSE(menu.isOpen());
}

// Documented behaviour: calling select() on an already-closed menu returns
// the last highlight (from the most recent open/detents) and leaves the
// menu closed -- it does not reopen it or change the highlight.
static void test_select_whenAlreadyClosed_returnsLastHighlightAndStaysClosed(void)
{
    CardMenu menu;
    menu.open(CardId::LIGHT_OFFICE, 1000);
    menu.close();

    CardId picked = menu.select();
    TEST_ASSERT_TRUE(CardId::LIGHT_OFFICE == picked);
    TEST_ASSERT_FALSE(menu.isOpen());
}

// ---------------------------------------------------------------------------
// tick() / idle close
// ---------------------------------------------------------------------------

static void test_tick_justBeforeIdleClose_returnsFalseAndStaysOpen(void)
{
    CardMenu menu;
    menu.open(CardId::CLOCK, 1000);

    bool closed = menu.tick(1000 + CardMenu::kIdleCloseMs - 1);
    TEST_ASSERT_FALSE(closed);
    TEST_ASSERT_TRUE(menu.isOpen());
}

static void test_tick_atExactlyIdleClose_returnsTrueAndCloses(void)
{
    CardMenu menu;
    menu.open(CardId::CLOCK, 1000);

    bool closed = menu.tick(1000 + CardMenu::kIdleCloseMs);
    TEST_ASSERT_TRUE(closed);
    TEST_ASSERT_FALSE(menu.isOpen());
}

static void test_tick_onlyReportsCloseOnTheClosingTick(void)
{
    CardMenu menu;
    menu.open(CardId::CLOCK, 1000);

    TEST_ASSERT_TRUE(menu.tick(1000 + CardMenu::kIdleCloseMs));
    // Already closed: further ticks report false, not another close.
    TEST_ASSERT_FALSE(menu.tick(1000 + CardMenu::kIdleCloseMs + 1000));
}

static void test_tick_refreshedByDetent_pushesOutIdleClose(void)
{
    CardMenu menu;
    menu.open(CardId::CLOCK, 1000);

    // A detent shortly before the timeout refreshes the idle clock.
    menu.detents(1, 1000 + CardMenu::kIdleCloseMs - 1);
    TEST_ASSERT_FALSE(menu.tick(1000 + 2 * CardMenu::kIdleCloseMs - 2));
    TEST_ASSERT_TRUE(menu.isOpen());

    TEST_ASSERT_TRUE(menu.tick(1000 + 2 * CardMenu::kIdleCloseMs - 1));
    TEST_ASSERT_FALSE(menu.isOpen());
}

static void test_tick_whenClosed_returnsFalse(void)
{
    CardMenu menu;
    menu.open(CardId::CLOCK, 1000);
    menu.close();
    TEST_ASSERT_FALSE(menu.tick(1000 + CardMenu::kIdleCloseMs));
}

// ---------------------------------------------------------------------------
// itemCentre() — index 0 at 12 o'clock, clockwise, centre (120,120)
// ---------------------------------------------------------------------------

static void test_itemCentre_index0_isTopCentre(void)
{
    int x = 0, y = 0;
    CardMenu::itemCentre(0, x, y);
    TEST_ASSERT_EQUAL_INT(120, x);
    TEST_ASSERT_EQUAL_INT(32, y);
}

static void test_itemCentre_index1_isClockwiseFromTop(void)
{
    int x = 0, y = 0;
    CardMenu::itemCentre(1, x, y);
    TEST_ASSERT_EQUAL_INT(204, x);
    TEST_ASSERT_EQUAL_INT(93, y);
}

static void test_itemCentre_allFiveFitWithinRing(void)
{
    for (uint8_t i = 0; i < static_cast<uint8_t>(CardId::COUNT); ++i) {
        int x = 0, y = 0;
        CardMenu::itemCentre(i, x, y);
        int dx = x - 120;
        int dy = y - 120;
        float dist = sqrtf(static_cast<float>(dx * dx + dy * dy));
        // Within a whole pixel of kRingRadius (88); the rest is int rounding.
        TEST_ASSERT_FLOAT_WITHIN(1.0f, static_cast<float>(CardMenu::kRingRadius), dist);
    }
}

// ---------------------------------------------------------------------------
// hitTest() — centre / margin / miss
// ---------------------------------------------------------------------------

static void test_hitTest_exactCentre_hitsThatItem(void)
{
    TEST_ASSERT_EQUAL_INT8(0, CardMenu::hitTest(120, 32));
    TEST_ASSERT_EQUAL_INT8(1, CardMenu::hitTest(204, 93));
}

static void test_hitTest_withinHitRadius_stillHits(void)
{
    // Index 0 centre is (120,32); a point kHitRadius-1 px away still hits.
    TEST_ASSERT_EQUAL_INT8(0, CardMenu::hitTest(120, 32 + CardMenu::kHitRadius - 1));
}

static void test_hitTest_justOutsideHitRadius_misses(void)
{
    // Straight down from index 0's centre, kHitRadius+5 px away: no item.
    TEST_ASSERT_EQUAL_INT8(-1, CardMenu::hitTest(120, 32 + CardMenu::kHitRadius + 5));
}

static void test_hitTest_farFromAnyItem_misses(void)
{
    TEST_ASSERT_EQUAL_INT8(-1, CardMenu::hitTest(0, 0));
    TEST_ASSERT_EQUAL_INT8(-1, CardMenu::hitTest(120, 120)); // dead centre, no item there
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();

    RUN_TEST(test_open_setsOpenAndHighlightsCurrent);
    RUN_TEST(test_close_isIdempotent);

    RUN_TEST(test_detents_advancesThroughRing_wrapsForward);
    RUN_TEST(test_detents_negative_wrapsBackward);
    RUN_TEST(test_detents_multiStep_wrapsCorrectly);
    RUN_TEST(test_detents_whenClosed_isNoOp);

    RUN_TEST(test_select_returnsHighlightAndCloses);
    RUN_TEST(test_select_whenAlreadyClosed_returnsLastHighlightAndStaysClosed);

    RUN_TEST(test_tick_justBeforeIdleClose_returnsFalseAndStaysOpen);
    RUN_TEST(test_tick_atExactlyIdleClose_returnsTrueAndCloses);
    RUN_TEST(test_tick_onlyReportsCloseOnTheClosingTick);
    RUN_TEST(test_tick_refreshedByDetent_pushesOutIdleClose);
    RUN_TEST(test_tick_whenClosed_returnsFalse);

    RUN_TEST(test_itemCentre_index0_isTopCentre);
    RUN_TEST(test_itemCentre_index1_isClockwiseFromTop);
    RUN_TEST(test_itemCentre_allFiveFitWithinRing);

    RUN_TEST(test_hitTest_exactCentre_hitsThatItem);
    RUN_TEST(test_hitTest_withinHitRadius_stillHits);
    RUN_TEST(test_hitTest_justOutsideHitRadius_misses);
    RUN_TEST(test_hitTest_farFromAnyItem_misses);

    return UNITY_END();
}
