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

// An on colour card sitting on its Colour page (Lamp order: Brightness ->
// Colour -> Kelvin -- spec 4.18 amendment).
static LightCardState onColourPageCard()
{
    LightCardState card = onCard();
    card.swipe(1, 1000);
    return card;
}

// An on colour card sitting on its Kelvin page (two swipes now that Colour
// sits between Brightness and Kelvin on the Lamp).
static LightCardState onKelvinPageCard()
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
    TEST_ASSERT_EQUAL_INT(112, LightLayout::kHueRingOuterR);
    TEST_ASSERT_EQUAL_INT(86, LightLayout::kHueRingInnerR);
    TEST_ASSERT_EQUAL_INT(99, LightLayout::kHueMarkerRingR);
    TEST_ASSERT_EQUAL_INT(11, LightLayout::kHueMarkerR);
    TEST_ASSERT_EQUAL_INT(3, LightLayout::kHueMarkerOutline);
    TEST_ASSERT_EQUAL_INT(70, LightLayout::kHueHitInnerR);
    TEST_ASSERT_EQUAL_INT(120, LightLayout::kHueHitOuterR);
    TEST_ASSERT_EQUAL_INT(72, LightLayout::kHueSegments);
    TEST_ASSERT_EQUAL_FLOAT(5.0f, LightLayout::kHueSegmentDeg);
    TEST_ASSERT_EQUAL_INT(30, LightLayout::kCentreDiscR);
}

// ---------------------------------------------------------------------------
// LightLayout: hue ring geometry (spec 4.18)
// ---------------------------------------------------------------------------

static void test_wrapHue_normalisesIntoZeroTo360(void)
{
    TEST_ASSERT_EQUAL_FLOAT(0.0f, LightLayout::wrapHue(360.0f));
    TEST_ASSERT_EQUAL_FLOAT(355.0f, LightLayout::wrapHue(-5.0f));
    TEST_ASSERT_EQUAL_FLOAT(10.0f, LightLayout::wrapHue(730.0f));
    TEST_ASSERT_EQUAL_FLOAT(275.0f, LightLayout::wrapHue(275.0f));
}

// Hue 0 is 12 o'clock and increases clockwise, so the four cardinal points
// of the face read 0 / 90 / 180 / 270 and the top-right diagonal reads 45.
static void test_hueAt_cardinalPointsAndDiagonal(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, LightLayout::hueAt(120, 0));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 90.0f, LightLayout::hueAt(240, 120));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 180.0f, LightLayout::hueAt(120, 240));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 270.0f, LightLayout::hueAt(0, 120));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 45.0f, LightLayout::hueAt(170, 70));
}

static void test_hueAt_isAlwaysInZeroTo360(void)
{
    for (int x = 0; x < 240; x += 8)
    {
        for (int y = 0; y < 240; y += 8)
        {
            const float h = LightLayout::hueAt(x, y);
            TEST_ASSERT_TRUE(h >= 0.0f && h < 360.0f);
        }
    }
}

// The touch annulus is wider than the painted band on both sides.
static void test_hitHueRing_annulusBounds(void)
{
    TEST_ASSERT_TRUE(LightLayout::hitHueRing(120, 120 - LightLayout::kHueHitInnerR));
    TEST_ASSERT_FALSE(LightLayout::hitHueRing(120, 120 - LightLayout::kHueHitInnerR + 1));
    TEST_ASSERT_TRUE(LightLayout::hitHueRing(120, 120 - LightLayout::kHueHitOuterR));
    TEST_ASSERT_FALSE(LightLayout::hitHueRing(120, 120 - LightLayout::kHueHitOuterR - 1));
    TEST_ASSERT_TRUE(LightLayout::hitHueRing(120 + LightLayout::kHueMarkerRingR, 120));
    TEST_ASSERT_FALSE(LightLayout::hitHueRing(120, 120));
}

// The marker sits on the band's mid-radius at the hue's angle.
static void test_hueMarkerCentre_hue0IsTopHue90IsRight(void)
{
    int x = 0;
    int y = 0;
    LightLayout::hueMarkerCentre(0.0f, x, y);
    TEST_ASSERT_EQUAL_INT(120, x);
    TEST_ASSERT_EQUAL_INT(120 - LightLayout::kHueMarkerRingR, y);
    LightLayout::hueMarkerCentre(90.0f, x, y);
    TEST_ASSERT_EQUAL_INT(120 + LightLayout::kHueMarkerRingR, x);
    TEST_ASSERT_EQUAL_INT(120, y);
    LightLayout::hueMarkerCentre(180.0f, x, y);
    TEST_ASSERT_EQUAL_INT(120, x);
    TEST_ASSERT_EQUAL_INT(120 + LightLayout::kHueMarkerRingR, y);
}

// hueAt() and hueMarkerCentre() are inverses: the marker for hue h reads back
// as h, all the way round.
static void test_hueMarkerCentre_roundTripsThroughHueAt(void)
{
    for (int h = 0; h < 360; h += 15)
    {
        int x = 0;
        int y = 0;
        LightLayout::hueMarkerCentre(static_cast<float>(h), x, y);
        TEST_ASSERT_FLOAT_WITHIN(0.7f, static_cast<float>(h), LightLayout::hueAt(x, y));
        TEST_ASSERT_TRUE(LightLayout::hitHueRing(x, y));
    }
}

static void test_hitPowerGlyph_withinAndOutsideHitRadius(void)
{
    TEST_ASSERT_TRUE(LightLayout::hitPowerGlyph(120, 208));
    TEST_ASSERT_TRUE(LightLayout::hitPowerGlyph(120, 208 + LightLayout::kPowerGlyphHitR));
    TEST_ASSERT_FALSE(LightLayout::hitPowerGlyph(120, 208 + LightLayout::kPowerGlyphHitR + 1));
    TEST_ASSERT_FALSE(LightLayout::hitPowerGlyph(120, 120));
}

// Colour page: a tap on the centre disc leaves the page (spec 4.18
// amendment). The hit radius sits well inside the ring's touch annulus so
// the two hit tests can never both claim the same point.
static void test_hitCentreDisc_withinAndOutside(void)
{
    TEST_ASSERT_EQUAL_INT(34, LightLayout::kCentreDiscHitR);
    TEST_ASSERT_TRUE(LightLayout::hitCentreDisc(120, 120));
    TEST_ASSERT_TRUE(LightLayout::hitCentreDisc(120, 120 - 34));
    TEST_ASSERT_FALSE(LightLayout::hitCentreDisc(120, 120 - 35));
    TEST_ASSERT_FALSE(LightLayout::hitCentreDisc(120 + 99, 120));

    for (int x = 0; x < 240; x += 4)
    {
        for (int y = 0; y < 240; y += 4)
        {
            TEST_ASSERT_FALSE(LightLayout::hitCentreDisc(x, y) && LightLayout::hitHueRing(x, y));
        }
    }
}

// ---------------------------------------------------------------------------
// LightCardState: pages
// ---------------------------------------------------------------------------

static void test_hueStep_is15Degrees(void)
{
    TEST_ASSERT_EQUAL_INT(15, LightCardState::HUE_STEP);
}

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

// Lamp order (spec 4.18 amendment): Brightness -> Colour -> Kelvin, clamping
// at Kelvin.
static void test_swipe_forwardWalksBrightColourKelvinAndClampsAtKelvin(void)
{
    LightCardState card = onCard();
    TEST_ASSERT_EQUAL_UINT8(0, card.pageIndex());
    TEST_ASSERT_TRUE(LightCardState::Page::BRIGHT == card.page());
    card.swipe(1, 1000);
    TEST_ASSERT_EQUAL_UINT8(1, card.pageIndex());
    TEST_ASSERT_TRUE(LightCardState::Page::COLOUR == card.page());
    card.swipe(1, 1000);
    TEST_ASSERT_EQUAL_UINT8(2, card.pageIndex());
    TEST_ASSERT_TRUE(LightCardState::Page::KELVIN == card.page());
    card.swipe(1, 1000);
    TEST_ASSERT_EQUAL_UINT8(2, card.pageIndex());
    TEST_ASSERT_TRUE(LightCardState::Page::KELVIN == card.page());
}

// pageAt() sequences: Lamp is Brightness -> Colour -> Kelvin, Office (no
// colour) is Brightness -> Kelvin; an out-of-range index clamps to the last
// page.
static void test_pageAt_lampAndOfficeSequences(void)
{
    LightCardState lamp(true);
    TEST_ASSERT_TRUE(LightCardState::Page::BRIGHT == lamp.pageAt(0));
    TEST_ASSERT_TRUE(LightCardState::Page::COLOUR == lamp.pageAt(1));
    TEST_ASSERT_TRUE(LightCardState::Page::KELVIN == lamp.pageAt(2));
    TEST_ASSERT_TRUE(LightCardState::Page::KELVIN == lamp.pageAt(99));

    LightCardState office(false);
    TEST_ASSERT_TRUE(LightCardState::Page::BRIGHT == office.pageAt(0));
    TEST_ASSERT_TRUE(LightCardState::Page::KELVIN == office.pageAt(1));
    TEST_ASSERT_TRUE(LightCardState::Page::KELVIN == office.pageAt(99));
}

// Office has no Colour page: one swipe reaches Kelvin directly.
static void test_swipe_officeSkipsColour(void)
{
    LightCardState card = onOfficeCard();
    card.swipe(1, 1000);
    TEST_ASSERT_EQUAL_UINT8(1, card.pageIndex());
    TEST_ASSERT_TRUE(LightCardState::Page::KELVIN == card.page());
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
    card.swipe(1, 1000); // -> COLOUR
    TEST_ASSERT_TRUE(LightCardState::Page::COLOUR == card.page());
    card.tick(10999);
    TEST_ASSERT_TRUE(LightCardState::Page::COLOUR == card.page());
    card.tick(11000);
    TEST_ASSERT_TRUE(LightCardState::Page::BRIGHT == card.page());
}

static void test_pageIdle_detentExtendsTheTimeout(void)
{
    LightCardState card = onCard();
    card.swipe(1, 1000);      // -> COLOUR at t=1000
    card.detents(1, 6000);    // activity at t=6000
    card.tick(11000);         // 10 s after the swipe, 5 s after the detent
    TEST_ASSERT_TRUE(LightCardState::Page::COLOUR == card.page());
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
    LightCardState card = onKelvinPageCard();
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

// resetPage() from the Colour page (the centre-disc tap) also ends any
// scrub gesture in progress, but never touches a pending settle -- an edit
// made a moment ago still deserves to be sent.
static void test_resetPage_fromColourReturnsToBrightnessAndEndsScrub(void)
{
    LightCardState card = onColourPageCard();
    card.scrub(90.0f, 1000);
    TEST_ASSERT_TRUE(card.scrubbing());
    card.resetPage();
    TEST_ASSERT_TRUE(LightCardState::Page::BRIGHT == card.page());
    TEST_ASSERT_FALSE(card.scrubbing());
    TEST_ASSERT_TRUE(card.settling()); // resetPage() never drops an edit
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
    LightCardState card = onKelvinPageCard();
    card.detents(3, 1000);
    TEST_ASSERT_EQUAL_UINT16(4300, card.view().kelvin);
    card.detents(50, 1100);
    TEST_ASSERT_EQUAL_UINT16(6500, card.view().kelvin);
    card.detents(-100, 1200);
    TEST_ASSERT_EQUAL_UINT16(2000, card.view().kelvin);
}

static void test_detents_colourPage_stepsFifteenDegreesPerDetent(void)
{
    LightCardState card = onCard(); // HA hue 0
    card.swipe(1, 1000);
    card.detents(1, 1000);
    TEST_ASSERT_EQUAL_FLOAT(15.0f, card.editHue());
    TEST_ASSERT_EQUAL_FLOAT(15.0f, card.view().hue);
    TEST_ASSERT_EQUAL_FLOAT(100.0f, card.view().sat);
    card.detents(3, 1050);
    TEST_ASSERT_EQUAL_FLOAT(60.0f, card.editHue());
}

static void test_detents_colourPage_wrapsBothWays(void)
{
    LightCardState card = onCard();
    card.swipe(1, 1000);
    card.detents(-1, 1000); // 0 -> 345
    TEST_ASSERT_EQUAL_FLOAT(345.0f, card.editHue());
    card.detents(1, 1100); // 345 -> 0
    TEST_ASSERT_EQUAL_FLOAT(0.0f, card.editHue());
}

static void test_detents_colourPage_fromTempMode_startsAtHaHue(void)
{
    LightCardState card(true);
    // TEMP mode, but HA still reports the last hue: 250.
    card.sync(makeState(true, 50, LightsModel::ColorMode::TEMP, 4000, 2000, 6500, 250, 100, true),
              1000);
    card.swipe(1, 1000);
    card.detents(1, 1000);
    TEST_ASSERT_EQUAL_FLOAT(265.0f, card.editHue());
    TEST_ASSERT_EQUAL_FLOAT(265.0f, card.view().hue);
    TEST_ASSERT_TRUE(card.colourLive());
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
    TEST_ASSERT_TRUE(LightCardState::Page::COLOUR == card.page());
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
    LightCardState card = onKelvinPageCard();
    card.detents(-2, 1000);
    LightsModel::Command cmd = card.tick(1300);
    TEST_ASSERT_TRUE(LightsModel::Command::Type::TEMP == cmd.type);
    TEST_ASSERT_TRUE(cmd.on);
    TEST_ASSERT_EQUAL_UINT16(3800, cmd.kelvin);
}

static void test_settle_colourPageEmitsHueCommandWithRoundedHueAndFullSaturation(void)
{
    LightCardState card = onCard();
    card.swipe(1, 1000);
    card.detents(5, 1000); // 0 -> 75
    LightsModel::Command cmd = card.tick(1300);
    TEST_ASSERT_TRUE(LightsModel::Command::Type::HUE == cmd.type);
    TEST_ASSERT_TRUE(cmd.on);
    TEST_ASSERT_EQUAL_FLOAT(75.0f, cmd.hue);
    TEST_ASSERT_EQUAL_FLOAT(100.0f, cmd.sat);
}

static void test_settle_survivesAPageSwipeAndStillSendsTheEditedField(void)
{
    LightCardState card = onKelvinPageCard();
    card.detents(1, 1000);
    card.swipe(-1, 1000); // user flicks back to Colour before it settles
    TEST_ASSERT_TRUE(LightCardState::Page::COLOUR == card.page());
    LightsModel::Command cmd = card.tick(1300);
    TEST_ASSERT_TRUE(LightsModel::Command::Type::TEMP == cmd.type);
    TEST_ASSERT_EQUAL_UINT16(4100, cmd.kelvin);
    // The Colour page itself was never edited, so it must not read live.
    TEST_ASSERT_FALSE(card.colourLive());
}

// ---------------------------------------------------------------------------
// LightCardState: selectHue
// ---------------------------------------------------------------------------

static void test_selectHue_armsSettleAndSendsThatHue(void)
{
    LightCardState card = onColourPageCard();
    card.selectHue(137.4f, 1000);
    TEST_ASSERT_EQUAL_FLOAT(137.4f, card.editHue());
    TEST_ASSERT_TRUE(card.settling());
    TEST_ASSERT_TRUE(card.colourLive());
    LightsModel::Command cmd = card.tick(1300);
    TEST_ASSERT_TRUE(LightsModel::Command::Type::HUE == cmd.type);
    TEST_ASSERT_EQUAL_FLOAT(137.4f, cmd.hue);
    TEST_ASSERT_EQUAL_FLOAT(100.0f, cmd.sat);
    // formatCommand() rounds on the wire: 137.4 -> 137.
    char buf[64];
    TEST_ASSERT_TRUE(LightsModel::formatCommand(cmd, buf, sizeof(buf)) > 0);
    TEST_ASSERT_EQUAL_STRING("{\"state\":\"ON\",\"hs_color\":[137,100]}", buf);
}

static void test_selectHue_normalisesTheHue(void)
{
    LightCardState card = onColourPageCard();
    card.selectHue(-90.0f, 1000);
    TEST_ASSERT_EQUAL_FLOAT(270.0f, card.editHue());
}

static void test_selectHue_whileOff_isIgnored(void)
{
    LightCardState card(true);
    card.sync(colourState(), 1000); // off
    card.selectHue(90.0f, 1000); // page is BRIGHT and the light is off: doubly ignored
    TEST_ASSERT_FALSE(card.settling());
}

// Only the Colour page draws the ring: the same coordinates on the
// Brightness or Kelvin page are bare background.
static void test_selectHue_offTheColourPage_isIgnored(void)
{
    LightCardState card = onCard(); // BRIGHT
    card.selectHue(90.0f, 1000);
    TEST_ASSERT_FALSE(card.settling());
    card.swipe(1, 1000); // -> COLOUR
    card.swipe(1, 1000); // -> KELVIN
    card.selectHue(90.0f, 1000);
    TEST_ASSERT_FALSE(card.settling());
}

// Office has no Colour page at all, so no ring tap can ever land.
static void test_selectHue_onACardWithoutColour_isIgnored(void)
{
    LightCardState card = onOfficeCard();
    card.swipe(1, 1000);
    card.swipe(1, 1000); // clamps at KELVIN
    card.selectHue(90.0f, 1000);
    TEST_ASSERT_FALSE(card.settling());
}

// A tap on the dim ring (lamp in TEMP mode) still selects that hue and makes
// colour live -- same as a detent, so the two boundary circles are a real target.
static void test_selectHue_onDimColourPage_makesColourLive(void)
{
    LightCardState card = onColourPageCard(); // colourOn() is TEMP mode -> colour not live
    TEST_ASSERT_FALSE(card.colourLive());
    card.selectHue(90.0f, 1000);
    TEST_ASSERT_TRUE(card.colourLive());
    TEST_ASSERT_EQUAL_FLOAT(90.0f, card.editHue());
    TEST_ASSERT_TRUE(card.settling());
}

// ---------------------------------------------------------------------------
// LightCardState: scrub (finger on the hue ring)
// ---------------------------------------------------------------------------

static void test_scrub_firstCallSelectsTheHue(void)
{
    LightCardState card = onColourPageCard();
    card.scrub(90.0f, 1000);
    TEST_ASSERT_TRUE(card.scrubbing());
    TEST_ASSERT_TRUE(card.settling());
    TEST_ASSERT_EQUAL_FLOAT(90.0f, card.editHue());
    LightsModel::Command cmd = card.tick(1300);
    TEST_ASSERT_TRUE(LightsModel::Command::Type::HUE == cmd.type);
    TEST_ASSERT_EQUAL_FLOAT(90.0f, cmd.hue);
}

// A resting finger sends exactly one command: repeated scrub() calls at the
// same hue after the send must not re-arm the settle.
static void test_scrub_restingFingerSendsOnce(void)
{
    LightCardState card = onColourPageCard();
    card.scrub(90.0f, 1000);
    TEST_ASSERT_TRUE(LightsModel::Command::Type::HUE == card.tick(1300).type);
    int sent = 0;
    for (uint32_t t = 1310; t <= 4000; t += 30)
    {
        card.scrub(90.3f, t); // sub-degree jitter
        if (card.tick(t).type != LightsModel::Command::Type::NONE) ++sent;
    }
    TEST_ASSERT_EQUAL_INT(0, sent);
}

// HA's echo of the sent hue (1.5 deg off, inside CONFIRM_HUE_TOL) confirms the
// hold and must not make a still finger re-send.
static void test_scrub_haEchoDoesNotRetrigger(void)
{
    LightCardState card = onColourPageCard();
    card.scrub(275.0f, 1000);
    (void)card.tick(1300);
    card.sync(makeState(true, 50, LightsModel::ColorMode::HS, 0, 2000, 6500, 273.5f, 100, true), 1400);
    TEST_ASSERT_FALSE(card.hueEditInFlight());
    int sent = 0;
    for (uint32_t t = 1410; t <= 4000; t += 30)
    {
        card.scrub(275.0f, t);
        if (card.tick(t).type != LightsModel::Command::Type::NONE) ++sent;
    }
    TEST_ASSERT_EQUAL_INT(0, sent);
}

// The Hue bulb reports 258 for a commanded 275: after the hold expires the
// marker follows HA, but a still finger must still not re-send.
static void test_scrub_bulbReportingDifferentHueDoesNotRetrigger(void)
{
    LightCardState card = onColourPageCard();
    card.scrub(275.0f, 1000);
    (void)card.tick(1300);
    const LightsModel::LightState bulbState =
        makeState(true, 50, LightsModel::ColorMode::HS, 0, 2000, 6500, 258.0f, 100, true);
    card.sync(bulbState, 1400);
    int sent = 0;
    for (uint32_t t = 1410; t <= 6000; t += 30)
    {
        card.scrub(275.0f, t);
        if (card.tick(t).type != LightsModel::Command::Type::NONE) ++sent;
    }
    TEST_ASSERT_EQUAL_INT(0, sent);
    // DialUi re-syncs every 250 ms regardless of the scrub; simulate the next
    // one arriving once CONFIRM_HOLD_MS has passed. That is what actually
    // lets go of the sent value and lets the marker show the bulb's truth --
    // a still finger must not turn that into a resend either.
    card.sync(bulbState, 6000);
    TEST_ASSERT_EQUAL_FLOAT(258.0f, card.editHue());
}

static void test_scrub_movingFingerReselectsAndSendsOnceAfterItStops(void)
{
    LightCardState card = onColourPageCard();
    int sent = 0;
    uint32_t t = 1000;
    for (float h = 90.0f; h <= 180.0f; h += 3.0f, t += 50) // 1.5 s sweep
    {
        card.scrub(h, t);
        if (card.tick(t).type != LightsModel::Command::Type::NONE) ++sent;
    }
    TEST_ASSERT_EQUAL_INT(0, sent); // never still for 300 ms while moving
    for (uint32_t u = t; u <= t + 1000; u += 50) // finger stops at 180
    {
        card.scrub(180.0f, u);
        if (card.tick(u).type != LightsModel::Command::Type::NONE) ++sent;
    }
    TEST_ASSERT_EQUAL_INT(1, sent);
}

static void test_scrub_wrapAwareThreshold(void)
{
    LightCardState card = onColourPageCard();
    card.scrub(359.5f, 1000);
    (void)card.tick(1300);
    card.scrub(0.2f, 1400); // 0.7 deg away across the seam: below threshold
    TEST_ASSERT_FALSE(card.settling());
    card.scrub(1.0f, 1500); // 1.5 deg away: re-select
    TEST_ASSERT_TRUE(card.settling());
}

static void test_scrub_keepsThePageAliveUnderAHeldFinger(void)
{
    LightCardState card = onColourPageCard();
    card.scrub(90.0f, 1000);
    for (uint32_t t = 1000; t <= 1000 + LightCardState::PAGE_IDLE_MS + 5000; t += 500)
    {
        card.scrub(90.0f, t);
        (void)card.tick(t);
        TEST_ASSERT_TRUE(LightCardState::Page::COLOUR == card.page());
    }
}

static void test_endScrub_nextGestureSelectsAgainEvenAtTheSameHue(void)
{
    LightCardState card = onColourPageCard();
    card.scrub(90.0f, 1000);
    (void)card.tick(1300);
    card.endScrub();
    TEST_ASSERT_FALSE(card.scrubbing());
    card.scrub(90.0f, 5000); // a new tap on the same hue re-sends (harmless, expected)
    TEST_ASSERT_TRUE(card.settling());
}

static void test_scrub_ignoredWhenOffPageOrOff(void)
{
    LightCardState card = onCard(); // BRIGHT page
    card.scrub(90.0f, 1000);
    TEST_ASSERT_FALSE(card.settling());
    TEST_ASSERT_FALSE(card.scrubbing());
    LightCardState off(true);
    off.sync(colourState(), 1000);
    off.scrub(90.0f, 1000);
    TEST_ASSERT_FALSE(off.settling());
}

// ---------------------------------------------------------------------------
// LightCardState: editHue(), kelvinLive(), colourLive(), ringFraction()
// ---------------------------------------------------------------------------

// No snapping: the marker shows exactly what HA reports.
static void test_editHue_followsHaHueWhenIdle(void)
{
    LightCardState card(true);
    card.sync(makeState(true, 50, LightsModel::ColorMode::HS, 0, 2000, 6500, 258.3f, 97.0f, true),
              1000);
    TEST_ASSERT_EQUAL_FLOAT(258.3f, card.editHue());
}

static void test_editHue_isThePendingOneWhileSettling(void)
{
    LightCardState card = onCard();
    card.swipe(1, 1000);
    card.detents(2, 1000); // 0 -> 30
    // A stale HA echo (still hue 0) mustn't drag the marker back.
    card.sync(colourOn(), 1050);
    TEST_ASSERT_EQUAL_FLOAT(30.0f, card.editHue());
}

// After the command goes out the marker stays on the sent hue until HA
// confirms (or the 1.5 s hold expires), then follows HA.
static void test_editHue_holdsTheSentHueUntilConfirmedThenFollowsHa(void)
{
    LightCardState card = onColourPageCard();
    card.selectHue(275.0f, 1000);
    (void)card.tick(1300); // command out, confirm hold starts
    card.sync(colourOn(), 1400); // stale echo: hue 0, TEMP mode
    TEST_ASSERT_EQUAL_FLOAT(275.0f, card.editHue());
    // HA echoes the Hue bulb's version of 275: within tolerance -> confirmed.
    card.sync(makeState(true, 50, LightsModel::ColorMode::HS, 0, 2000, 6500, 273.3f, 92.0f, true),
              1500);
    TEST_ASSERT_EQUAL_FLOAT(273.3f, card.editHue());
}

static void test_hueEditInFlight_trueWhileSettlingAndUntilConfirmed(void)
{
    LightCardState card = onColourPageCard();
    TEST_ASSERT_FALSE(card.hueEditInFlight());
    card.selectHue(120.0f, 1000);
    TEST_ASSERT_TRUE(card.hueEditInFlight());   // settling
    (void)card.tick(1300);                       // command sent, hold begins
    TEST_ASSERT_TRUE(card.hueEditInFlight());   // awaiting confirmation
    card.sync(makeState(true, 50, LightsModel::ColorMode::HS, 0, 2000, 6500, 121.0f, 100, true), 1500);
    TEST_ASSERT_FALSE(card.hueEditInFlight());  // confirmed within tolerance
}

static void test_hueEditInFlight_falseForABrightnessEdit(void)
{
    LightCardState card = onCard(); // Brightness page
    card.detents(1, 1000);
    TEST_ASSERT_TRUE(card.settling());
    TEST_ASSERT_FALSE(card.hueEditInFlight());
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
    card.detents(1, 1000);
    TEST_ASSERT_TRUE(card.colourLive());
    TEST_ASSERT_FALSE(card.kelvinLive());
}

static void test_kelvinEdit_makesKelvinLiveImmediately(void)
{
    LightCardState card(true);
    card.sync(makeState(true, 50, LightsModel::ColorMode::HS, 3000, 2000, 6500, 240, 100, true),
              1000);
    card.swipe(1, 1000); // -> COLOUR
    card.swipe(1, 1000); // -> KELVIN
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
    LightCardState card = onKelvinPageCard();
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
    LightCardState card = onKelvinPageCard();
    card.detents(1, 1000); // 4100
    card.tick(1300);
    LightsModel::LightState s = colourOn();
    s.kelvin = 4060; // within +-50: counts as the echo
    card.sync(s, 1350);
    TEST_ASSERT_EQUAL_UINT16(4060, card.view().kelvin);
}

static void test_confirmHold_temp_kelvinOutsideToleranceKeepsLocal(void)
{
    LightCardState card = onKelvinPageCard();
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
    card.selectHue(240.0f, 1000);
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
    card.selectHue(240.0f, 1000);
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

    RUN_TEST(test_wrapHue_normalisesIntoZeroTo360);
    RUN_TEST(test_hueAt_cardinalPointsAndDiagonal);
    RUN_TEST(test_hueAt_isAlwaysInZeroTo360);
    RUN_TEST(test_hitHueRing_annulusBounds);
    RUN_TEST(test_hueMarkerCentre_hue0IsTopHue90IsRight);
    RUN_TEST(test_hueMarkerCentre_roundTripsThroughHueAt);
    RUN_TEST(test_hitPowerGlyph_withinAndOutsideHitRadius);
    RUN_TEST(test_hitCentreDisc_withinAndOutside);

    RUN_TEST(test_hueStep_is15Degrees);
    RUN_TEST(test_pageCount_lampHasThreeOfficeHasTwo);
    RUN_TEST(test_page_startsOnBrightness);
    RUN_TEST(test_pageAt_lampAndOfficeSequences);
    RUN_TEST(test_swipe_forwardWalksBrightColourKelvinAndClampsAtKelvin);
    RUN_TEST(test_swipe_backwardClampsAtBrightness);
    RUN_TEST(test_swipe_officeClampsAtKelvin);
    RUN_TEST(test_swipe_officeSkipsColour);
    RUN_TEST(test_pageIdle_returnsToBrightnessAfterTenSeconds);
    RUN_TEST(test_pageIdle_detentExtendsTheTimeout);
    RUN_TEST(test_swipe_whileOff_isIgnored);
    RUN_TEST(test_resetPage_returnsToBrightness);
    RUN_TEST(test_resetPage_leavesPendingSettleAlone);
    RUN_TEST(test_resetPage_fromColourReturnsToBrightnessAndEndsScrub);

    RUN_TEST(test_detents_brightPage_stepsBy5AndArmsSettle);
    RUN_TEST(test_detents_brightPage_clampsTo1And100);
    RUN_TEST(test_detents_kelvinPage_stepsBy100AndClampsToBounds);
    RUN_TEST(test_detents_colourPage_stepsFifteenDegreesPerDetent);
    RUN_TEST(test_detents_colourPage_wrapsBothWays);
    RUN_TEST(test_detents_colourPage_fromTempMode_startsAtHaHue);
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
    RUN_TEST(test_settle_colourPageEmitsHueCommandWithRoundedHueAndFullSaturation);
    RUN_TEST(test_settle_survivesAPageSwipeAndStillSendsTheEditedField);

    RUN_TEST(test_selectHue_armsSettleAndSendsThatHue);
    RUN_TEST(test_selectHue_normalisesTheHue);
    RUN_TEST(test_selectHue_whileOff_isIgnored);
    RUN_TEST(test_selectHue_offTheColourPage_isIgnored);
    RUN_TEST(test_selectHue_onACardWithoutColour_isIgnored);
    RUN_TEST(test_selectHue_onDimColourPage_makesColourLive);

    RUN_TEST(test_scrub_firstCallSelectsTheHue);
    RUN_TEST(test_scrub_restingFingerSendsOnce);
    RUN_TEST(test_scrub_haEchoDoesNotRetrigger);
    RUN_TEST(test_scrub_bulbReportingDifferentHueDoesNotRetrigger);
    RUN_TEST(test_scrub_movingFingerReselectsAndSendsOnceAfterItStops);
    RUN_TEST(test_scrub_wrapAwareThreshold);
    RUN_TEST(test_scrub_keepsThePageAliveUnderAHeldFinger);
    RUN_TEST(test_endScrub_nextGestureSelectsAgainEvenAtTheSameHue);
    RUN_TEST(test_scrub_ignoredWhenOffPageOrOff);

    RUN_TEST(test_editHue_followsHaHueWhenIdle);
    RUN_TEST(test_editHue_isThePendingOneWhileSettling);
    RUN_TEST(test_editHue_holdsTheSentHueUntilConfirmedThenFollowsHa);
    RUN_TEST(test_hueEditInFlight_trueWhileSettlingAndUntilConfirmed);
    RUN_TEST(test_hueEditInFlight_falseForABrightnessEdit);
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
