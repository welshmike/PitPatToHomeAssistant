#include <unity.h>

#include "LightCardState.h"
#include "LightLayout.h"
#include "LightsModel.h"

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static LightsModel::LightState makeState(bool on, uint8_t pct, LightsModel::ColorMode mode,
                                          uint16_t kelvin, uint16_t minK, uint16_t maxK,
                                          float hue, float sat, bool supportsColor)
{
    LightsModel::LightState s;
    s.valid = true;
    s.available = true;
    s.on = on;
    s.brightnessPct = pct;
    s.mode = mode;
    s.kelvin = kelvin;
    s.minKelvin = minK;
    s.maxKelvin = maxK;
    s.hue = hue;
    s.sat = sat;
    s.supportsColor = supportsColor;
    return s;
}

// A colour-capable light, off, mid brightness, TEMP mode, kelvin range 2000-6500.
static LightsModel::LightState colourState()
{
    return makeState(false, 50, LightsModel::ColorMode::TEMP, 4000, 2000, 6500, 0, 100, true);
}

// The same light, switched on.
static LightsModel::LightState colourOn()
{
    return makeState(true, 50, LightsModel::ColorMode::TEMP, 4000, 2000, 6500, 0, 100, true);
}

// A card sitting on an on, available, colour-capable light.
static LightCardState onCard()
{
    LightCardState card(true);
    card.sync(colourOn(), 1000);
    return card;
}

// The Office card (no Colour page), on an on, available light.
static LightCardState onOfficeCard()
{
    LightCardState card(false);
    LightsModel::LightState s = colourOn();
    s.supportsColor = false;
    card.sync(s, 1000);
    return card;
}

// An on colour card sitting on its Colour page.
static LightCardState onColourPageCard()
{
    LightCardState card = onCard();
    card.swipe(1, 1000);
    card.swipe(1, 1000);
    return card;
}

// ---------------------------------------------------------------------------
// LightLayout: constants
// ---------------------------------------------------------------------------

static void test_layout_offFaceConstants(void)
{
    TEST_ASSERT_EQUAL_INT(120, LightLayout::kPowerCircleX);
    TEST_ASSERT_EQUAL_INT(124, LightLayout::kPowerCircleY);
    TEST_ASSERT_EQUAL_INT(44, LightLayout::kPowerCircleR);
}

static void test_layout_onFaceConstants(void)
{
    TEST_ASSERT_EQUAL_INT(120, LightLayout::kPowerGlyphX);
    TEST_ASSERT_EQUAL_INT(208, LightLayout::kPowerGlyphY);
    TEST_ASSERT_EQUAL_INT(9, LightLayout::kPowerGlyphR);
    TEST_ASSERT_EQUAL_INT(18, LightLayout::kPowerGlyphHitR);
    TEST_ASSERT_EQUAL_INT(54, LightLayout::kPageDotY);
    TEST_ASSERT_EQUAL_INT(12, LightLayout::kPageDotSpacing);
    TEST_ASSERT_EQUAL_INT(3, LightLayout::kPageDotR);
}

static void test_layout_pageConstants(void)
{
    TEST_ASSERT_EQUAL_INT(160, LightLayout::kBrightCaptionY);
    TEST_ASSERT_EQUAL_INT(112, LightLayout::kKelvinValueY);
    TEST_ASSERT_EQUAL_INT(52, LightLayout::kKelvinBarX0);
    TEST_ASSERT_EQUAL_INT(188, LightLayout::kKelvinBarX1);
    TEST_ASSERT_EQUAL_INT(170, LightLayout::kKelvinBarY);
    TEST_ASSERT_EQUAL_INT(8, LightLayout::kKelvinBarH);
    TEST_ASSERT_EQUAL_INT(74, LightLayout::kSwatchRingR);
    TEST_ASSERT_EQUAL_FLOAT(22.5f, LightLayout::kSwatchStartDeg);
    TEST_ASSERT_EQUAL_INT(12, LightLayout::kSwatchR);
    TEST_ASSERT_EQUAL_INT(16, LightLayout::kSwatchRSelected);
    TEST_ASSERT_EQUAL_INT(14, LightLayout::kSwatchHitR);
    TEST_ASSERT_EQUAL_INT(30, LightLayout::kCentreDiscR);
}

static void test_layout_presetHues(void)
{
    TEST_ASSERT_EQUAL_UINT8(8, LightLayout::kPresetCount);
    const float expected[8] = {0, 30, 60, 120, 180, 240, 275, 320};
    for (uint8_t i = 0; i < 8; ++i)
    {
        TEST_ASSERT_EQUAL_FLOAT(expected[i], LightLayout::kPresetHues[i]);
    }
}

// ---------------------------------------------------------------------------
// LightLayout: swatch geometry + hit tests
// ---------------------------------------------------------------------------

// The ring is rotated half a step (kSwatchStartDeg = 22.5) so that no swatch
// sits at 12 or 6 o'clock, where the page-dot row and the power glyph live.
static void test_swatchCentre_ringStartsHalfAStepPastTwelve(void)
{
    int x = 0, y = 0;
    LightLayout::swatchCentre(0, x, y);
    TEST_ASSERT_EQUAL_INT(148, x);
    TEST_ASSERT_EQUAL_INT(52, y);
}

static void test_swatchCentre_goesClockwise(void)
{
    int x = 0, y = 0;
    LightLayout::swatchCentre(2, x, y); // quarter turn clockwise from swatch 0
    TEST_ASSERT_EQUAL_INT(188, x);
    TEST_ASSERT_EQUAL_INT(148, y);
    LightLayout::swatchCentre(4, x, y); // half turn: bottom right of 6 o'clock
    TEST_ASSERT_EQUAL_INT(92, x);
    TEST_ASSERT_EQUAL_INT(188, y);
}

// The two bottom swatches are the closest to the small power glyph; the
// rotation is what buys them their clearance, so assert it directly. 32 px is
// kSwatchHitR + kPowerGlyphHitR: below that the two hit discs would overlap.
static void test_swatchCentre_everySwatchClearsThePowerGlyph(void)
{
    for (uint8_t i = 0; i < LightLayout::kPresetCount; ++i)
    {
        int x = 0, y = 0;
        LightLayout::swatchCentre(i, x, y);
        const int dx = x - LightLayout::kPowerGlyphX;
        const int dy = y - LightLayout::kPowerGlyphY;
        const int minSep = LightLayout::kSwatchHitR + LightLayout::kPowerGlyphHitR;
        TEST_ASSERT_TRUE(dx * dx + dy * dy > minSep * minSep);
    }
}

// The top pair straddles 12 o'clock rather than sitting on it: swatch 7 to
// the left, swatch 0 to the right, both level with the page-dot row's y but
// well outside the widest row the dots can occupy (three dots, so 120 +/- one
// kPageDotSpacing plus kPageDotR). The Colour page hides the dots anyway, but
// the ring geometry is shared with the pages that draw them.
static void test_swatchCentre_topPairStraddlesTwelveOClock(void)
{
    int x = 0, y = 0;
    LightLayout::swatchCentre(7, x, y);
    TEST_ASSERT_EQUAL_INT(92, x);
    TEST_ASSERT_EQUAL_INT(52, y);

    const int dotHalfWidth = 2 * LightLayout::kPageDotSpacing; // generous
    for (uint8_t i = 0; i < LightLayout::kPresetCount; ++i)
    {
        LightLayout::swatchCentre(i, x, y);
        const int dy = y - LightLayout::kPageDotY;
        const int adx = (x - 120) < 0 ? (120 - x) : (x - 120);
        const bool onTheDotRow = (dy > -12 && dy < 12) && adx <= dotHalfWidth;
        TEST_ASSERT_FALSE(onTheDotRow);
    }
}

static void test_hitSwatch_centresHitTheirOwnIndex(void)
{
    for (uint8_t i = 0; i < LightLayout::kPresetCount; ++i)
    {
        int x = 0, y = 0;
        LightLayout::swatchCentre(i, x, y);
        TEST_ASSERT_EQUAL_INT(i, LightLayout::hitSwatch(x, y));
    }
}

static void test_hitSwatch_edgeOfHitRadiusHitsAndJustPastMisses(void)
{
    int x = 0, y = 0;
    LightLayout::swatchCentre(0, x, y);
    TEST_ASSERT_EQUAL_INT(0, LightLayout::hitSwatch(x, y - LightLayout::kSwatchHitR));
    TEST_ASSERT_EQUAL_INT(-1, LightLayout::hitSwatch(x, y - LightLayout::kSwatchHitR - 1));
}

static void test_hitSwatch_centreOfFaceMisses(void)
{
    TEST_ASSERT_EQUAL_INT(-1, LightLayout::hitSwatch(120, 120));
}

// Tap priority only matters if the two targets can ever both claim a point;
// sweep the whole round face and assert they never do.
static void test_hitSwatch_andPowerGlyph_neverClaimTheSamePoint(void)
{
    for (int y = 0; y <= 240; y += 2)
    {
        for (int x = 0; x <= 240; x += 2)
        {
            const int dx = x - 120;
            const int dy = y - 120;
            if (dx * dx + dy * dy > 120 * 120)
            {
                continue; // outside the round display
            }
            const bool swatch = LightLayout::hitSwatch(x, y) >= 0;
            const bool power  = LightLayout::hitPowerGlyph(x, y);
            TEST_ASSERT_FALSE(swatch && power);
        }
    }
}

static void test_hitPowerGlyph_withinAndOutsideHitRadius(void)
{
    TEST_ASSERT_TRUE(LightLayout::hitPowerGlyph(120, 208));
    TEST_ASSERT_TRUE(LightLayout::hitPowerGlyph(120, 208 + LightLayout::kPowerGlyphHitR));
    TEST_ASSERT_FALSE(LightLayout::hitPowerGlyph(120, 208 + LightLayout::kPowerGlyphHitR + 1));
    TEST_ASSERT_FALSE(LightLayout::hitPowerGlyph(120, 120));
}

// ---------------------------------------------------------------------------
// LightLayout: nearestPreset
// ---------------------------------------------------------------------------

static void test_nearestPreset_exactHuesMapToThemselves(void)
{
    for (uint8_t i = 0; i < LightLayout::kPresetCount; ++i)
    {
        TEST_ASSERT_EQUAL_UINT8(i, LightLayout::nearestPreset(LightLayout::kPresetHues[i]));
    }
}

static void test_nearestPreset_roundsToNearest(void)
{
    TEST_ASSERT_EQUAL_UINT8(5, LightLayout::nearestPreset(250.0f));  // 240
    TEST_ASSERT_EQUAL_UINT8(4, LightLayout::nearestPreset(200.0f));  // 180
    TEST_ASSERT_EQUAL_UINT8(1, LightLayout::nearestPreset(25.0f));   // 30
}

static void test_nearestPreset_wrapsRoundTheCircle(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, LightLayout::nearestPreset(350.0f)); // nearer 0 than 320
    TEST_ASSERT_EQUAL_UINT8(7, LightLayout::nearestPreset(335.0f)); // nearer 320 than 0
    TEST_ASSERT_EQUAL_UINT8(0, LightLayout::nearestPreset(340.0f)); // exactly between: lowest index
    TEST_ASSERT_EQUAL_UINT8(0, LightLayout::nearestPreset(-10.0f)); // out of range, normalised
    TEST_ASSERT_EQUAL_UINT8(0, LightLayout::nearestPreset(720.0f));
}

// ---------------------------------------------------------------------------
// LightCardState: pages
// ---------------------------------------------------------------------------

static void test_pageCount_lampHasThreeOfficeHasTwo(void)
{
    LightCardState lamp(true);
    LightCardState office(false);
    TEST_ASSERT_EQUAL_UINT8(3, lamp.pageCount());
    TEST_ASSERT_EQUAL_UINT8(2, office.pageCount());
}

static void test_page_startsOnBrightness(void)
{
    LightCardState card(true);
    TEST_ASSERT_TRUE(LightCardState::Page::BRIGHT == card.page());
}

static void test_swipe_forwardWalksPagesAndClampsAtColour(void)
{
    LightCardState card = onCard();
    card.swipe(1, 1000);
    TEST_ASSERT_TRUE(LightCardState::Page::KELVIN == card.page());
    card.swipe(1, 1000);
    TEST_ASSERT_TRUE(LightCardState::Page::COLOUR == card.page());
    card.swipe(1, 1000);
    TEST_ASSERT_TRUE(LightCardState::Page::COLOUR == card.page());
}

static void test_swipe_backwardClampsAtBrightness(void)
{
    LightCardState card = onCard();
    card.swipe(1, 1000);
    card.swipe(-1, 1000);
    TEST_ASSERT_TRUE(LightCardState::Page::BRIGHT == card.page());
    card.swipe(-1, 1000);
    TEST_ASSERT_TRUE(LightCardState::Page::BRIGHT == card.page());
}

static void test_pageIdle_returnsToBrightnessAfterTenSeconds(void)
{
    LightCardState card = onCard();
    card.swipe(1, 1000); // -> KELVIN
    TEST_ASSERT_TRUE(LightCardState::Page::KELVIN == card.page());
    card.tick(10999);
    TEST_ASSERT_TRUE(LightCardState::Page::KELVIN == card.page());
    card.tick(11000);
    TEST_ASSERT_TRUE(LightCardState::Page::BRIGHT == card.page());
}

static void test_pageIdle_detentExtendsTheTimeout(void)
{
    LightCardState card = onCard();
    card.swipe(1, 1000);      // -> KELVIN at t=1000
    card.detents(1, 6000);    // activity at t=6000
    card.tick(11000);         // 10 s after the swipe, 5 s after the detent
    TEST_ASSERT_TRUE(LightCardState::Page::KELVIN == card.page());
    card.tick(16000);
    TEST_ASSERT_TRUE(LightCardState::Page::BRIGHT == card.page());
}

static void test_swipe_officeClampsAtKelvin(void)
{
    LightCardState card = onOfficeCard();
    card.swipe(1, 1000);
    TEST_ASSERT_TRUE(LightCardState::Page::KELVIN == card.page());
    card.swipe(1, 1000);
    TEST_ASSERT_TRUE(LightCardState::Page::KELVIN == card.page());
}

// The off face is one switch-on target with no pages behind it, so a swipe
// there must not leave the card parked on a page nobody can see.
static void test_swipe_whileOff_isIgnored(void)
{
    LightCardState card(true);
    card.sync(colourState(), 1000); // off
    card.swipe(1, 1000);
    TEST_ASSERT_TRUE(LightCardState::Page::BRIGHT == card.page());
}

static void test_resetPage_returnsToBrightness(void)
{
    LightCardState card = onCard();
    card.swipe(1, 1000);
    card.swipe(1, 1000);
    card.resetPage();
    TEST_ASSERT_TRUE(LightCardState::Page::BRIGHT == card.page());
}

static void test_resetPage_leavesPendingSettleAlone(void)
{
    LightCardState card = onCard();
    card.swipe(1, 1000); // KELVIN
    card.detents(1, 1000);
    TEST_ASSERT_TRUE(card.settling());
    card.resetPage();
    TEST_ASSERT_TRUE(LightCardState::Page::BRIGHT == card.page());
    TEST_ASSERT_TRUE(card.settling());
    // ...and the pending command is still the kelvin one the user dialled.
    LightsModel::Command cmd = card.tick(1300);
    TEST_ASSERT_TRUE(LightsModel::Command::Type::TEMP == cmd.type);
    TEST_ASSERT_EQUAL_UINT16(4100, cmd.kelvin);
}

// ---------------------------------------------------------------------------
// LightCardState: detents per page
// ---------------------------------------------------------------------------

static void test_detents_brightPage_stepsBy5AndArmsSettle(void)
{
    LightCardState card = onCard();
    card.detents(2, 1000);
    TEST_ASSERT_EQUAL_UINT8(60, card.view().brightnessPct);
    TEST_ASSERT_TRUE(card.settling());
}

static void test_detents_brightPage_clampsTo1And100(void)
{
    LightCardState card = onCard();
    card.detents(20, 1000);
    TEST_ASSERT_EQUAL_UINT8(100, card.view().brightnessPct);
    card.detents(-40, 1100);
    TEST_ASSERT_EQUAL_UINT8(1, card.view().brightnessPct);
}

static void test_detents_kelvinPage_stepsBy100AndClampsToBounds(void)
{
    LightCardState card = onCard();
    card.swipe(1, 1000);
    card.detents(3, 1000);
    TEST_ASSERT_EQUAL_UINT16(4300, card.view().kelvin);
    card.detents(50, 1100);
    TEST_ASSERT_EQUAL_UINT16(6500, card.view().kelvin);
    card.detents(-100, 1200);
    TEST_ASSERT_EQUAL_UINT16(2000, card.view().kelvin);
}

static void test_detents_colourPage_movesOnePresetPerDetent(void)
{
    LightCardState card = onCard(); // hue 0 -> preset 0
    card.swipe(1, 1000);
    card.swipe(1, 1000);
    card.detents(1, 1000);
    TEST_ASSERT_EQUAL_UINT8(1, card.preset());
    TEST_ASSERT_EQUAL_FLOAT(30.0f, card.view().hue);
    TEST_ASSERT_EQUAL_FLOAT(100.0f, card.view().sat);
}

static void test_detents_colourPage_wrapsBothWays(void)
{
    LightCardState card = onCard();
    card.swipe(1, 1000);
    card.swipe(1, 1000);
    card.detents(-1, 1000); // 0 -> 7
    TEST_ASSERT_EQUAL_UINT8(7, card.preset());
    card.detents(1, 1100); // 7 -> 0
    TEST_ASSERT_EQUAL_UINT8(0, card.preset());
}

static void test_detents_colourPage_fromTempMode_startsAtNearestPresetToHue(void)
{
    LightCardState card(true);
    // TEMP mode, but HA still reports the last hue: 250 -> preset 5 (240).
    card.sync(makeState(true, 50, LightsModel::ColorMode::TEMP, 4000, 2000, 6500, 250, 100, true),
              1000);
    card.swipe(1, 1000);
    card.swipe(1, 1000);
    card.detents(1, 1000);
    TEST_ASSERT_EQUAL_UINT8(6, card.preset()); // 240 -> next preset, 275
    TEST_ASSERT_EQUAL_FLOAT(275.0f, card.view().hue);
}

static void test_detents_whileOff_isIgnored(void)
{
    LightCardState card(true);
    card.sync(colourState(), 1000); // off
    card.detents(2, 1000);
    TEST_ASSERT_EQUAL_UINT8(50, card.view().brightnessPct);
    TEST_ASSERT_FALSE(card.settling());
}

static void test_detents_whileUnavailable_isIgnored(void)
{
    LightCardState card(true);
    LightsModel::LightState s = colourOn();
    s.available = false;
    card.sync(s, 1000);
    card.detents(2, 1000);
    TEST_ASSERT_EQUAL_UINT8(50, card.view().brightnessPct);
    TEST_ASSERT_FALSE(card.settling());
}

static void test_detents_zero_isNoOp(void)
{
    LightCardState card = onCard();
    card.detents(0, 1000);
    TEST_ASSERT_EQUAL_UINT8(50, card.view().brightnessPct);
    TEST_ASSERT_FALSE(card.settling());
}

// ---------------------------------------------------------------------------
// LightCardState: power
// ---------------------------------------------------------------------------

static void test_tapOn_fromOff_switchesOnAndLandsOnBrightness(void)
{
    LightCardState card(true);
    card.sync(colourState(), 1000); // off
    card.swipe(1, 1000);                  // sitting on KELVIN when it went off
    LightsModel::Command cmd = card.tapOn(1000);
    TEST_ASSERT_TRUE(LightsModel::Command::Type::POWER == cmd.type);
    TEST_ASSERT_TRUE(cmd.on);
    TEST_ASSERT_TRUE(card.view().on);
    TEST_ASSERT_TRUE(LightCardState::Page::BRIGHT == card.page());
}

static void test_tapOn_whenAlreadyOn_isIgnored(void)
{
    LightCardState card = onCard();
    card.swipe(1, 1000);
    LightsModel::Command cmd = card.tapOn(1000);
    TEST_ASSERT_TRUE(LightsModel::Command::Type::NONE == cmd.type);
    TEST_ASSERT_TRUE(LightCardState::Page::KELVIN == card.page());
}

static void test_tapOn_whenNoStateYet_isIgnored(void)
{
    LightCardState card(true); // never synced: !valid
    LightsModel::Command cmd = card.tapOn(1000);
    TEST_ASSERT_TRUE(LightsModel::Command::Type::NONE == cmd.type);
    TEST_ASSERT_FALSE(card.view().on);
}

static void test_powerOff_switchesOffAndReturnsPowerOffCommand(void)
{
    LightCardState card = onCard();
    LightsModel::Command cmd = card.powerOff(1000);
    TEST_ASSERT_TRUE(LightsModel::Command::Type::POWER == cmd.type);
    TEST_ASSERT_FALSE(cmd.on);
    TEST_ASSERT_FALSE(card.view().on);
}

static void test_powerOff_dropsAPendingSettle(void)
{
    LightCardState card = onCard();
    card.detents(2, 1000);
    TEST_ASSERT_TRUE(card.settling());
    card.powerOff(1100);
    TEST_ASSERT_FALSE(card.settling());
    LightsModel::Command cmd = card.tick(1500);
    TEST_ASSERT_TRUE(LightsModel::Command::Type::NONE == cmd.type);
}

// ---------------------------------------------------------------------------
// LightCardState: settle timing and command shapes
// ---------------------------------------------------------------------------

static void test_settle_noCommandBeforeDeadline(void)
{
    LightCardState card = onCard();
    card.detents(1, 1000);
    TEST_ASSERT_TRUE(LightsModel::Command::Type::NONE == card.tick(1299).type);
    TEST_ASSERT_TRUE(card.settling());
}

static void test_settle_emitsExactlyOneBrightCommandAtDeadline(void)
{
    LightCardState card = onCard();
    card.detents(1, 1000);
    LightsModel::Command cmd = card.tick(1300);
    TEST_ASSERT_TRUE(LightsModel::Command::Type::BRIGHT == cmd.type);
    TEST_ASSERT_TRUE(cmd.on);
    TEST_ASSERT_EQUAL_UINT8(55, cmd.pct);
    TEST_ASSERT_FALSE(card.settling());
    TEST_ASSERT_TRUE(LightsModel::Command::Type::NONE == card.tick(1400).type);
}

static void test_settle_rearmsFromTheLastDetent(void)
{
    LightCardState card = onCard();
    card.detents(1, 1000);
    card.detents(1, 1200);
    TEST_ASSERT_TRUE(LightsModel::Command::Type::NONE == card.tick(1300).type);
    LightsModel::Command cmd = card.tick(1500);
    TEST_ASSERT_TRUE(LightsModel::Command::Type::BRIGHT == cmd.type);
    TEST_ASSERT_EQUAL_UINT8(60, cmd.pct);
}

static void test_settle_kelvinPageEmitsTempCommand(void)
{
    LightCardState card = onCard();
    card.swipe(1, 1000);
    card.detents(-2, 1000);
    LightsModel::Command cmd = card.tick(1300);
    TEST_ASSERT_TRUE(LightsModel::Command::Type::TEMP == cmd.type);
    TEST_ASSERT_TRUE(cmd.on);
    TEST_ASSERT_EQUAL_UINT16(3800, cmd.kelvin);
}

static void test_settle_colourPageEmitsHueCommandWithPresetHueAndFullSaturation(void)
{
    LightCardState card = onCard();
    card.swipe(1, 1000);
    card.swipe(1, 1000);
    card.detents(5, 1000); // preset 0 -> 5 (240)
    LightsModel::Command cmd = card.tick(1300);
    TEST_ASSERT_TRUE(LightsModel::Command::Type::HUE == cmd.type);
    TEST_ASSERT_TRUE(cmd.on);
    TEST_ASSERT_EQUAL_FLOAT(240.0f, cmd.hue);
    TEST_ASSERT_EQUAL_FLOAT(100.0f, cmd.sat);
}

static void test_settle_survivesAPageSwipeAndStillSendsTheEditedField(void)
{
    LightCardState card = onCard();
    card.swipe(1, 1000); // KELVIN
    card.detents(1, 1000);
    card.swipe(-1, 1000); // user flicks back to brightness before it settles
    LightsModel::Command cmd = card.tick(1300);
    TEST_ASSERT_TRUE(LightsModel::Command::Type::TEMP == cmd.type);
    TEST_ASSERT_EQUAL_UINT16(4100, cmd.kelvin);
}

// ---------------------------------------------------------------------------
// LightCardState: selectPreset
// ---------------------------------------------------------------------------

static void test_selectPreset_armsSettleAndSendsThatPresetsHue(void)
{
    LightCardState card = onColourPageCard();
    card.selectPreset(3, 1000);
    TEST_ASSERT_EQUAL_UINT8(3, card.preset());
    TEST_ASSERT_TRUE(card.settling());
    LightsModel::Command cmd = card.tick(1300);
    TEST_ASSERT_TRUE(LightsModel::Command::Type::HUE == cmd.type);
    TEST_ASSERT_EQUAL_FLOAT(120.0f, cmd.hue);
    TEST_ASSERT_EQUAL_FLOAT(100.0f, cmd.sat);
}

static void test_selectPreset_whileOff_isIgnored(void)
{
    LightCardState card(true);
    card.sync(colourState(), 1000); // off
    card.selectPreset(3, 1000); // page is BRIGHT and the light is off: doubly ignored
    TEST_ASSERT_FALSE(card.settling());
}

// Only the Colour page draws swatches: the same coordinates on the
// Brightness or Kelvin page are bare background.
static void test_selectPreset_offTheColourPage_isIgnored(void)
{
    LightCardState card = onCard(); // BRIGHT
    card.selectPreset(3, 1000);
    TEST_ASSERT_FALSE(card.settling());
    card.swipe(1, 1000); // KELVIN
    card.selectPreset(3, 1000);
    TEST_ASSERT_FALSE(card.settling());
}

// Office has no Colour page at all, so no swatch tap can ever land.
static void test_selectPreset_onACardWithoutColour_isIgnored(void)
{
    LightCardState card = onOfficeCard();
    card.swipe(1, 1000);
    card.swipe(1, 1000); // clamps at KELVIN
    card.selectPreset(3, 1000);
    TEST_ASSERT_FALSE(card.settling());
}

static void test_selectPreset_outOfRangeIndex_isIgnored(void)
{
    LightCardState card = onColourPageCard();
    card.selectPreset(8, 1000);
    TEST_ASSERT_FALSE(card.settling());
}

// ---------------------------------------------------------------------------
// LightCardState: preset(), kelvinLive(), colourLive(), ringFraction()
// ---------------------------------------------------------------------------

static void test_preset_followsHaHueWhenIdle(void)
{
    LightCardState card(true);
    card.sync(makeState(true, 50, LightsModel::ColorMode::HS, 0, 2000, 6500, 350, 100, true), 1000);
    TEST_ASSERT_EQUAL_UINT8(0, card.preset()); // 350 wraps to preset 0
}

static void test_preset_isThePendingOneWhileSettling(void)
{
    LightCardState card = onCard();
    card.swipe(1, 1000);
    card.swipe(1, 1000);
    card.detents(2, 1000);
    // A stale HA echo (still hue 0) mustn't drag the highlight back.
    card.sync(colourOn(), 1050);
    TEST_ASSERT_EQUAL_UINT8(2, card.preset());
}

static void test_kelvinLiveAndColourLive_followTheReportedMode(void)
{
    LightCardState card(true);
    card.sync(colourOn(), 1000); // TEMP
    TEST_ASSERT_TRUE(card.kelvinLive());
    TEST_ASSERT_FALSE(card.colourLive());
    card.sync(makeState(true, 50, LightsModel::ColorMode::HS, 0, 2000, 6500, 240, 100, true), 1100);
    TEST_ASSERT_FALSE(card.kelvinLive());
    TEST_ASSERT_TRUE(card.colourLive());
}

static void test_colourEdit_makesColourLiveImmediately(void)
{
    LightCardState card = onCard(); // TEMP mode
    card.swipe(1, 1000);
    card.swipe(1, 1000);
    card.detents(1, 1000);
    TEST_ASSERT_TRUE(card.colourLive());
    TEST_ASSERT_FALSE(card.kelvinLive());
}

static void test_kelvinEdit_makesKelvinLiveImmediately(void)
{
    LightCardState card(true);
    card.sync(makeState(true, 50, LightsModel::ColorMode::HS, 3000, 2000, 6500, 240, 100, true),
              1000);
    card.swipe(1, 1000);
    card.detents(1, 1000);
    TEST_ASSERT_TRUE(card.kelvinLive());
    TEST_ASSERT_FALSE(card.colourLive());
}

static void test_ringFraction_isBrightnessOver100(void)
{
    LightCardState card = onCard();
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f, card.ringFraction());
    card.detents(4, 1000);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.7f, card.ringFraction());
}

// ---------------------------------------------------------------------------
// LightCardState: sync() during / after a settle
// ---------------------------------------------------------------------------

static void test_sync_duringSettle_keepsTheLocallyEditedField(void)
{
    LightCardState card = onCard();
    card.detents(2, 1000); // 60 locally
    card.sync(colourOn(), 1050); // HA still says 50
    TEST_ASSERT_EQUAL_UINT8(60, card.view().brightnessPct);
}

static void test_sync_duringSettle_adoptsEverythingElse(void)
{
    LightCardState card = onCard();
    card.detents(2, 1000);
    LightsModel::LightState s = colourOn();
    s.kelvin = 5000;
    card.sync(s, 1050);
    TEST_ASSERT_EQUAL_UINT8(60, card.view().brightnessPct);
    TEST_ASSERT_EQUAL_UINT16(5000, card.view().kelvin);
}

static void test_sync_afterSettleConfirmed_adoptsHaAgain(void)
{
    LightCardState card = onCard();
    card.detents(2, 1000);
    card.tick(1300);             // BRIGHT 60 goes out
    LightsModel::LightState s = colourOn();
    s.brightnessPct = 60;
    card.sync(s, 1350);          // echo confirms, hold ends
    s.brightnessPct = 33;        // someone else changed it
    card.sync(s, 1400);
    TEST_ASSERT_EQUAL_UINT8(33, card.view().brightnessPct);
}

static void test_sync_duringSettle_reclampsKelvinToNewBounds(void)
{
    LightCardState card = onCard();
    card.swipe(1, 1000);
    card.detents(30, 1000); // 7000 -> clamped to 6500 with the old bounds
    TEST_ASSERT_EQUAL_UINT16(6500, card.view().kelvin);
    LightsModel::LightState s = colourOn();
    s.maxKelvin = 4500;
    card.sync(s, 1050);
    TEST_ASSERT_EQUAL_UINT16(4500, card.view().kelvin);
}

// ---------------------------------------------------------------------------
// LightCardState: awaiting-confirmation hold (Plan 6 behaviour, kept)
// ---------------------------------------------------------------------------

static void test_confirmHold_power_staleEchoBeforeDeadline_keepsLocalOn(void)
{
    LightCardState card(true);
    card.sync(colourState(), 1000); // off
    card.tapOn(1000);
    TEST_ASSERT_TRUE(card.view().on);
    card.sync(colourState(), 1500); // HA hasn't caught up: still off
    TEST_ASSERT_TRUE(card.view().on);
}

static void test_confirmHold_power_staleEchoAfterDeadline_adoptsHa(void)
{
    LightCardState card(true);
    card.sync(colourState(), 1000);
    card.tapOn(1000);
    card.sync(colourState(), 2500); // 1.5 s later, HA still says off: HA wins
    TEST_ASSERT_FALSE(card.view().on);
}

static void test_confirmHold_power_matchingEchoEndsTheHoldEarly(void)
{
    LightCardState card(true);
    card.sync(colourState(), 1000);
    card.tapOn(1000);
    card.sync(colourOn(), 1100);    // echo: on
    card.sync(colourState(), 1200); // then genuinely off again (wall switch)
    TEST_ASSERT_FALSE(card.view().on);
}

static void test_confirmHold_bright_keepsSentPctUntilEchoed(void)
{
    LightCardState card = onCard();
    card.detents(2, 1000);
    card.tick(1300); // BRIGHT 60 sent
    TEST_ASSERT_FALSE(card.settling());
    card.sync(colourOn(), 1350); // stale echo, still 50
    TEST_ASSERT_EQUAL_UINT8(60, card.view().brightnessPct);
    card.sync(colourOn(), 2900); // past the 1.5 s deadline
    TEST_ASSERT_EQUAL_UINT8(50, card.view().brightnessPct);
}

static void test_confirmHold_temp_kelvinWithinToleranceConfirms(void)
{
    LightCardState card = onCard();
    card.swipe(1, 1000);
    card.detents(1, 1000); // 4100
    card.tick(1300);
    LightsModel::LightState s = colourOn();
    s.kelvin = 4060; // within +-50: counts as the echo
    card.sync(s, 1350);
    TEST_ASSERT_EQUAL_UINT16(4060, card.view().kelvin);
}

static void test_confirmHold_temp_kelvinOutsideToleranceKeepsLocal(void)
{
    LightCardState card = onCard();
    card.swipe(1, 1000);
    card.detents(1, 1000); // 4100
    card.tick(1300);
    LightsModel::LightState s = colourOn();
    s.kelvin = 3000; // stale
    card.sync(s, 1350);
    TEST_ASSERT_EQUAL_UINT16(4100, card.view().kelvin);
}

static void test_confirmHold_hue_withinToleranceConfirms(void)
{
    LightCardState card = onCard();
    card.swipe(1, 1000);
    card.swipe(1, 1000);
    card.detents(5, 1000); // preset 5, hue 240
    card.tick(1300);
    LightsModel::LightState s = colourOn();
    s.mode = LightsModel::ColorMode::HS;
    s.hue = 241.0f;
    card.sync(s, 1350);
    TEST_ASSERT_EQUAL_FLOAT(241.0f, card.view().hue);
}

static void test_confirmHold_hue_outsideToleranceKeepsLocal(void)
{
    LightCardState card = onCard();
    card.swipe(1, 1000);
    card.swipe(1, 1000);
    card.detents(5, 1000);
    card.tick(1300);
    card.sync(colourOn(), 1350); // stale: hue 0
    TEST_ASSERT_EQUAL_FLOAT(240.0f, card.view().hue);
    TEST_ASSERT_EQUAL_FLOAT(100.0f, card.view().sat);
    TEST_ASSERT_TRUE(card.colourLive());
}

static void test_confirmHold_powerOffThenTapOn_keepsOptimisticOn(void)
{
    LightCardState card = onCard();
    card.powerOff(1000);
    TEST_ASSERT_FALSE(card.view().on);
    card.tapOn(1100);
    TEST_ASSERT_TRUE(card.view().on);
    card.sync(colourState(), 1150); // HA still reports off
    TEST_ASSERT_TRUE(card.view().on);
}

static void test_confirmHold_duringSettleAfterPowerOff_keepsOptimisticOn(void)
{
    LightCardState card = onCard();
    card.powerOff(1000);   // POWER off hold armed
    card.tapOn(1050);      // straight back on
    card.detents(2, 1100); // and dial the brightness while it settles
    card.sync(colourState(), 1150); // HA still says off
    TEST_ASSERT_TRUE(card.view().on);
    TEST_ASSERT_EQUAL_UINT8(60, card.view().brightnessPct);
}

int main(int, char**)
{
    UNITY_BEGIN();

    RUN_TEST(test_layout_offFaceConstants);
    RUN_TEST(test_layout_onFaceConstants);
    RUN_TEST(test_layout_pageConstants);
    RUN_TEST(test_layout_presetHues);

    RUN_TEST(test_swatchCentre_ringStartsHalfAStepPastTwelve);
    RUN_TEST(test_swatchCentre_goesClockwise);
    RUN_TEST(test_swatchCentre_everySwatchClearsThePowerGlyph);
    RUN_TEST(test_swatchCentre_topPairStraddlesTwelveOClock);
    RUN_TEST(test_hitSwatch_andPowerGlyph_neverClaimTheSamePoint);
    RUN_TEST(test_hitSwatch_centresHitTheirOwnIndex);
    RUN_TEST(test_hitSwatch_edgeOfHitRadiusHitsAndJustPastMisses);
    RUN_TEST(test_hitSwatch_centreOfFaceMisses);
    RUN_TEST(test_hitPowerGlyph_withinAndOutsideHitRadius);

    RUN_TEST(test_nearestPreset_exactHuesMapToThemselves);
    RUN_TEST(test_nearestPreset_roundsToNearest);
    RUN_TEST(test_nearestPreset_wrapsRoundTheCircle);

    RUN_TEST(test_pageCount_lampHasThreeOfficeHasTwo);
    RUN_TEST(test_page_startsOnBrightness);
    RUN_TEST(test_swipe_forwardWalksPagesAndClampsAtColour);
    RUN_TEST(test_swipe_backwardClampsAtBrightness);
    RUN_TEST(test_swipe_officeClampsAtKelvin);
    RUN_TEST(test_pageIdle_returnsToBrightnessAfterTenSeconds);
    RUN_TEST(test_pageIdle_detentExtendsTheTimeout);
    RUN_TEST(test_swipe_whileOff_isIgnored);
    RUN_TEST(test_resetPage_returnsToBrightness);
    RUN_TEST(test_resetPage_leavesPendingSettleAlone);

    RUN_TEST(test_detents_brightPage_stepsBy5AndArmsSettle);
    RUN_TEST(test_detents_brightPage_clampsTo1And100);
    RUN_TEST(test_detents_kelvinPage_stepsBy100AndClampsToBounds);
    RUN_TEST(test_detents_colourPage_movesOnePresetPerDetent);
    RUN_TEST(test_detents_colourPage_wrapsBothWays);
    RUN_TEST(test_detents_colourPage_fromTempMode_startsAtNearestPresetToHue);
    RUN_TEST(test_detents_whileOff_isIgnored);
    RUN_TEST(test_detents_whileUnavailable_isIgnored);
    RUN_TEST(test_detents_zero_isNoOp);

    RUN_TEST(test_tapOn_fromOff_switchesOnAndLandsOnBrightness);
    RUN_TEST(test_tapOn_whenAlreadyOn_isIgnored);
    RUN_TEST(test_tapOn_whenNoStateYet_isIgnored);
    RUN_TEST(test_powerOff_switchesOffAndReturnsPowerOffCommand);
    RUN_TEST(test_powerOff_dropsAPendingSettle);

    RUN_TEST(test_settle_noCommandBeforeDeadline);
    RUN_TEST(test_settle_emitsExactlyOneBrightCommandAtDeadline);
    RUN_TEST(test_settle_rearmsFromTheLastDetent);
    RUN_TEST(test_settle_kelvinPageEmitsTempCommand);
    RUN_TEST(test_settle_colourPageEmitsHueCommandWithPresetHueAndFullSaturation);
    RUN_TEST(test_settle_survivesAPageSwipeAndStillSendsTheEditedField);

    RUN_TEST(test_selectPreset_armsSettleAndSendsThatPresetsHue);
    RUN_TEST(test_selectPreset_whileOff_isIgnored);
    RUN_TEST(test_selectPreset_offTheColourPage_isIgnored);
    RUN_TEST(test_selectPreset_onACardWithoutColour_isIgnored);
    RUN_TEST(test_selectPreset_outOfRangeIndex_isIgnored);

    RUN_TEST(test_preset_followsHaHueWhenIdle);
    RUN_TEST(test_preset_isThePendingOneWhileSettling);
    RUN_TEST(test_kelvinLiveAndColourLive_followTheReportedMode);
    RUN_TEST(test_colourEdit_makesColourLiveImmediately);
    RUN_TEST(test_kelvinEdit_makesKelvinLiveImmediately);
    RUN_TEST(test_ringFraction_isBrightnessOver100);

    RUN_TEST(test_sync_duringSettle_keepsTheLocallyEditedField);
    RUN_TEST(test_sync_duringSettle_adoptsEverythingElse);
    RUN_TEST(test_sync_afterSettleConfirmed_adoptsHaAgain);
    RUN_TEST(test_sync_duringSettle_reclampsKelvinToNewBounds);

    RUN_TEST(test_confirmHold_power_staleEchoBeforeDeadline_keepsLocalOn);
    RUN_TEST(test_confirmHold_power_staleEchoAfterDeadline_adoptsHa);
    RUN_TEST(test_confirmHold_power_matchingEchoEndsTheHoldEarly);
    RUN_TEST(test_confirmHold_bright_keepsSentPctUntilEchoed);
    RUN_TEST(test_confirmHold_temp_kelvinWithinToleranceConfirms);
    RUN_TEST(test_confirmHold_temp_kelvinOutsideToleranceKeepsLocal);
    RUN_TEST(test_confirmHold_hue_withinToleranceConfirms);
    RUN_TEST(test_confirmHold_hue_outsideToleranceKeepsLocal);
    RUN_TEST(test_confirmHold_powerOffThenTapOn_keepsOptimisticOn);
    RUN_TEST(test_confirmHold_duringSettleAfterPowerOff_keepsOptimisticOn);

    return UNITY_END();
}
