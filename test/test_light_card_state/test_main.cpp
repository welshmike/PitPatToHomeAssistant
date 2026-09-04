#include <unity.h>

#include "LightButtons.h"
#include "LightCardState.h"
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

// A plain (Office) light with no colour support.
static LightsModel::LightState plainState()
{
    return makeState(false, 50, LightsModel::ColorMode::NONE, 0, 2000, 6500, 0, 100, false);
}

// ---------------------------------------------------------------------------
// LightButtons: geom() / hitTest() / label()
// ---------------------------------------------------------------------------

static void test_geom_lamp_returnsSpecCentres(void)
{
    Geom power = geom(Button::POWER, true);
    Geom bright = geom(Button::BRIGHT, true);
    Geom colour = geom(Button::COLOUR, true);
    TEST_ASSERT_EQUAL_INT(56, power.cx);
    TEST_ASSERT_EQUAL_INT(186, power.cy);
    TEST_ASSERT_EQUAL_INT(22, power.r);
    TEST_ASSERT_EQUAL_INT(120, bright.cx);
    TEST_ASSERT_EQUAL_INT(206, bright.cy);
    TEST_ASSERT_EQUAL_INT(184, colour.cx);
    TEST_ASSERT_EQUAL_INT(186, colour.cy);
}

static void test_geom_office_returnsSpecCentres(void)
{
    Geom power = geom(Button::POWER, false);
    Geom bright = geom(Button::BRIGHT, false);
    TEST_ASSERT_EQUAL_INT(84, power.cx);
    TEST_ASSERT_EQUAL_INT(200, power.cy);
    TEST_ASSERT_EQUAL_INT(156, bright.cx);
    TEST_ASSERT_EQUAL_INT(200, bright.cy);
}

static void test_geom_office_colourIsZero(void)
{
    Geom colour = geom(Button::COLOUR, false);
    TEST_ASSERT_EQUAL_INT(0, colour.cx);
    TEST_ASSERT_EQUAL_INT(0, colour.cy);
    TEST_ASSERT_EQUAL_INT(0, colour.r);
}

static void test_hitTest_lamp_centreHitsEachButton(void)
{
    TEST_ASSERT_TRUE(Button::POWER == hitTest(56, 186, true));
    TEST_ASSERT_TRUE(Button::BRIGHT == hitTest(120, 206, true));
    TEST_ASSERT_TRUE(Button::COLOUR == hitTest(184, 186, true));
}

static void test_hitTest_lamp_edgeOfMarginHits(void)
{
    // r=22, margin=6 -> limit 28. Exactly on the limit still hits.
    TEST_ASSERT_TRUE(Button::POWER == hitTest(56 + 28, 186, true));
}

static void test_hitTest_lamp_justPastMarginMisses(void)
{
    TEST_ASSERT_TRUE(Button::NONE == hitTest(56 + 29, 186, true));
}

static void test_hitTest_office_centresHit(void)
{
    TEST_ASSERT_TRUE(Button::POWER == hitTest(84, 200, false));
    TEST_ASSERT_TRUE(Button::BRIGHT == hitTest(156, 200, false));
}

static void test_hitTest_office_colourNeverReturned(void)
{
    // Where the Lamp's COLOUR button would be (184,186) is empty space on
    // Office; hitTest must not claim it as COLOUR (or anything else).
    TEST_ASSERT_TRUE(Button::NONE == hitTest(184, 186, false));
}

static void test_hitTest_farAway_returnsNone(void)
{
    TEST_ASSERT_TRUE(Button::NONE == hitTest(0, 0, true));
}

static void test_label_returnsExpectedStrings(void)
{
    TEST_ASSERT_EQUAL_STRING("Power", label(Button::POWER));
    TEST_ASSERT_EQUAL_STRING("Bright", label(Button::BRIGHT));
    TEST_ASSERT_EQUAL_STRING("Colour", label(Button::COLOUR));
}

// ---------------------------------------------------------------------------
// LightCardState: POWER
// ---------------------------------------------------------------------------

static void test_tapPower_fromOff_setsOnAndReturnsPowerOnCommand(void)
{
    LightCardState card(true);
    card.sync(colourState()); // on = false
    LightsModel::Command cmd = card.tapButton(Button::POWER, 1000);
    TEST_ASSERT_TRUE(LightsModel::Command::Type::POWER == cmd.type);
    TEST_ASSERT_TRUE(cmd.on);
    TEST_ASSERT_TRUE(card.view().on);
}

static void test_tapPower_whenInvalid_isIgnored(void)
{
    LightCardState card(true);
    LightsModel::LightState s; // valid = false (default)
    card.sync(s);
    LightsModel::Command cmd = card.tapButton(Button::POWER, 1000);
    TEST_ASSERT_TRUE(LightsModel::Command::Type::NONE == cmd.type);
}

static void test_tapPower_releasesEngagement(void)
{
    LightCardState card(true);
    card.sync(colourState());
    card.tapButton(Button::BRIGHT, 1000); // engage
    TEST_ASSERT_TRUE(LightCardState::Engaged::BRIGHT == card.engaged());
    card.tapButton(Button::POWER, 1000);
    TEST_ASSERT_TRUE(LightCardState::Engaged::NONE == card.engaged());
}

// ---------------------------------------------------------------------------
// LightCardState: BRIGHT engage/release + detents
// ---------------------------------------------------------------------------

static void test_tapBright_engagesThenReleases(void)
{
    LightCardState card(true);
    card.sync(colourState());
    card.tapButton(Button::BRIGHT, 1000);
    TEST_ASSERT_TRUE(LightCardState::Engaged::BRIGHT == card.engaged());
    LightsModel::Command cmd = card.tapButton(Button::BRIGHT, 1000);
    TEST_ASSERT_TRUE(LightCardState::Engaged::NONE == card.engaged());
    TEST_ASSERT_TRUE(LightsModel::Command::Type::NONE == cmd.type);
}

static void test_tapBright_whenUnavailable_isIgnored(void)
{
    LightCardState card(true);
    LightsModel::LightState s = colourState();
    s.available = false;
    card.sync(s);
    card.tapButton(Button::BRIGHT, 1000);
    TEST_ASSERT_TRUE(LightCardState::Engaged::NONE == card.engaged());
}

static void test_detents_bright_stepsBy5(void)
{
    LightCardState card(true);
    card.sync(colourState()); // pct = 50
    card.tapButton(Button::BRIGHT, 1000);
    card.detents(2, 1000); // +10
    TEST_ASSERT_EQUAL_UINT8(60, card.view().brightnessPct);
}

static void test_detents_bright_clampsAt100(void)
{
    LightCardState card(true);
    card.sync(colourState());
    card.tapButton(Button::BRIGHT, 1000);
    card.detents(100, 1000);
    TEST_ASSERT_EQUAL_UINT8(100, card.view().brightnessPct);
}

static void test_detents_bright_clampsAt1(void)
{
    LightCardState card(true);
    card.sync(colourState());
    card.tapButton(Button::BRIGHT, 1000);
    card.detents(-100, 1000);
    TEST_ASSERT_EQUAL_UINT8(1, card.view().brightnessPct);
}

static void test_detents_whenNotEngaged_isNoOp(void)
{
    LightCardState card(true);
    card.sync(colourState());
    card.detents(2, 1000);
    TEST_ASSERT_EQUAL_UINT8(50, card.view().brightnessPct);
    TEST_ASSERT_FALSE(card.settling());
}

static void test_detents_setsOnLocallyAndArmsSettling(void)
{
    LightCardState card(true);
    LightsModel::LightState s = colourState();
    s.on = false;
    card.sync(s);
    card.tapButton(Button::BRIGHT, 1000);
    card.detents(1, 1000);
    TEST_ASSERT_TRUE(card.view().on);
    TEST_ASSERT_TRUE(card.settling());
}

// ---------------------------------------------------------------------------
// LightCardState: TEMP detents + clamp to HA min/max
// ---------------------------------------------------------------------------

static void test_detents_temp_clampsToLightsMinMax(void)
{
    LightCardState card(true);
    LightsModel::LightState s = colourState();
    s.kelvin = 6450;
    s.minKelvin = 2200;
    s.maxKelvin = 6500;
    card.sync(s);
    card.tapButton(Button::COLOUR, 1000); // mode TEMP already -> engages TEMP
    TEST_ASSERT_TRUE(LightCardState::Engaged::TEMP == card.engaged());
    card.detents(1, 1000); // +100 -> 6550, clamp to 6500
    TEST_ASSERT_EQUAL_UINT16(6500, card.view().kelvin);

    card.detents(-100, 2000); // way below min, clamp to 2200
    TEST_ASSERT_EQUAL_UINT16(2200, card.view().kelvin);
}

// ---------------------------------------------------------------------------
// LightCardState: HUE detents + wrap
// ---------------------------------------------------------------------------

static void test_detents_hue_wrapsAt360(void)
{
    LightCardState card(true);
    LightsModel::LightState s = colourState();
    s.mode = LightsModel::ColorMode::HS; // so COLOUR engages HUE directly
    s.hue = 350;
    card.sync(s);
    card.tapButton(Button::COLOUR, 1000);
    TEST_ASSERT_TRUE(LightCardState::Engaged::HUE == card.engaged());
    card.detents(2, 1000); // +20 -> 370 -> wraps to 10
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.0f, card.view().hue);
}

// ---------------------------------------------------------------------------
// LightCardState: COLOUR cycle + colour-support gating
// ---------------------------------------------------------------------------

static void test_colour_ignoredOnOfficeLayout(void)
{
    LightCardState card(false); // Office: hasColour = false
    card.sync(colourState());   // even if the light itself supports colour
    card.tapButton(Button::COLOUR, 1000);
    TEST_ASSERT_TRUE(LightCardState::Engaged::NONE == card.engaged());
}

static void test_colour_ignoredWhenLightDoesNotSupportIt(void)
{
    LightCardState card(true); // Lamp
    LightsModel::LightState s = colourState();
    s.supportsColor = false;
    card.sync(s);
    card.tapButton(Button::COLOUR, 1000);
    TEST_ASSERT_TRUE(LightCardState::Engaged::NONE == card.engaged());
}

static void test_colour_cycle_tempMode_goesTempThenHueThenNone(void)
{
    LightCardState card(true);
    card.sync(colourState()); // mode = TEMP

    card.tapButton(Button::COLOUR, 1000);
    TEST_ASSERT_TRUE(LightCardState::Engaged::TEMP == card.engaged());

    card.tapButton(Button::COLOUR, 1000);
    TEST_ASSERT_TRUE(LightCardState::Engaged::HUE == card.engaged());

    LightsModel::Command cmd = card.tapButton(Button::COLOUR, 1000);
    TEST_ASSERT_TRUE(LightCardState::Engaged::NONE == card.engaged());
    TEST_ASSERT_TRUE(LightsModel::Command::Type::NONE == cmd.type);
}

static void test_colour_cycle_hsMode_engagesHueDirectlyFromNone(void)
{
    LightCardState card(true);
    LightsModel::LightState s = colourState();
    s.mode = LightsModel::ColorMode::HS;
    card.sync(s);

    card.tapButton(Button::COLOUR, 1000);
    TEST_ASSERT_TRUE(LightCardState::Engaged::HUE == card.engaged());
}

static void test_colour_engageHue_zeroSaturation_seedsTo100(void)
{
    LightCardState card(true);
    LightsModel::LightState s = colourState();
    s.mode = LightsModel::ColorMode::HS;
    s.sat = 0;
    card.sync(s);

    card.tapButton(Button::COLOUR, 1000);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.0f, card.view().sat);
}

// ---------------------------------------------------------------------------
// LightCardState: release tap flushes a pending settle
// ---------------------------------------------------------------------------

static void test_tapBright_reTapWithPendingSettle_flushesSettleCommand(void)
{
    LightCardState card(true);
    card.sync(colourState()); // pct = 50
    card.tapButton(Button::BRIGHT, 1000);
    card.detents(1, 1000); // pct -> 55, settling

    LightsModel::Command cmd = card.tapButton(Button::BRIGHT, 1100);
    TEST_ASSERT_TRUE(LightsModel::Command::Type::BRIGHT == cmd.type);
    TEST_ASSERT_EQUAL_UINT8(55, cmd.pct);
    TEST_ASSERT_TRUE(LightCardState::Engaged::NONE == card.engaged());
    TEST_ASSERT_FALSE(card.settling());
}

static void test_tapColour_thirdTapWithPendingSettle_flushesSettleCommand(void)
{
    LightCardState card(true);
    LightsModel::LightState s = colourState();
    s.mode = LightsModel::ColorMode::HS; // COLOUR engages HUE directly
    card.sync(s);
    card.tapButton(Button::COLOUR, 1000); // engage HUE
    card.detents(1, 1000);                // hue changes, settling

    LightsModel::Command cmd = card.tapButton(Button::COLOUR, 1100); // third tap: release
    TEST_ASSERT_TRUE(LightsModel::Command::Type::HUE == cmd.type);
    TEST_ASSERT_TRUE(LightCardState::Engaged::NONE == card.engaged());
    TEST_ASSERT_FALSE(card.settling());
}

static void test_tapBright_reTapWithNoPendingSettle_returnsNone(void)
{
    LightCardState card(true);
    card.sync(colourState());
    card.tapButton(Button::BRIGHT, 1000); // engage, no detent

    LightsModel::Command cmd = card.tapButton(Button::BRIGHT, 1100); // release
    TEST_ASSERT_TRUE(LightsModel::Command::Type::NONE == cmd.type);
    TEST_ASSERT_TRUE(LightCardState::Engaged::NONE == card.engaged());
}

// ---------------------------------------------------------------------------
// LightCardState: settle timing
// ---------------------------------------------------------------------------

static void test_settle_noCommandBeforeDeadline(void)
{
    LightCardState card(true);
    card.sync(colourState());
    card.tapButton(Button::BRIGHT, 1000);
    card.detents(1, 1000);
    LightsModel::Command cmd = card.tick(1000 + 299);
    TEST_ASSERT_TRUE(LightsModel::Command::Type::NONE == cmd.type);
    TEST_ASSERT_TRUE(card.settling());
}

static void test_settle_emitsExactlyOneCommandAtDeadline(void)
{
    LightCardState card(true);
    card.sync(colourState());
    card.tapButton(Button::BRIGHT, 1000);
    card.detents(1, 1000); // -> 55

    LightsModel::Command cmd = card.tick(1000 + 300);
    TEST_ASSERT_TRUE(LightsModel::Command::Type::BRIGHT == cmd.type);
    TEST_ASSERT_EQUAL_UINT8(55, cmd.pct);
    TEST_ASSERT_FALSE(card.settling());

    // Engagement itself is not released by the settle.
    TEST_ASSERT_TRUE(LightCardState::Engaged::BRIGHT == card.engaged());

    // A further tick with no new detent doesn't re-emit.
    LightsModel::Command cmd2 = card.tick(1000 + 301);
    TEST_ASSERT_TRUE(LightsModel::Command::Type::NONE == cmd2.type);
}

static void test_settle_rearmsFromLastDetent(void)
{
    LightCardState card(true);
    card.sync(colourState());
    card.tapButton(Button::BRIGHT, 1000);
    card.detents(1, 1000);   // last detent at 1000
    card.detents(1, 1200);   // last detent moves to 1200

    // 300ms after the *first* detent (1300) should NOT have fired yet.
    LightsModel::Command early = card.tick(1300);
    TEST_ASSERT_TRUE(LightsModel::Command::Type::NONE == early.type);

    // 300ms after the *last* detent (1500) fires.
    LightsModel::Command late = card.tick(1500);
    TEST_ASSERT_TRUE(LightsModel::Command::Type::BRIGHT == late.type);
}

// ---------------------------------------------------------------------------
// LightCardState: idle release
// ---------------------------------------------------------------------------

static void test_idle_releasesSilentlyAt10s(void)
{
    LightCardState card(true);
    card.sync(colourState());
    card.tapButton(Button::BRIGHT, 1000);

    LightsModel::Command cmd = card.tick(1000 + 10000);
    TEST_ASSERT_TRUE(LightsModel::Command::Type::NONE == cmd.type);
    TEST_ASSERT_TRUE(LightCardState::Engaged::NONE == card.engaged());
}

static void test_idle_notYetReleasedJustBeforeDeadline(void)
{
    LightCardState card(true);
    card.sync(colourState());
    card.tapButton(Button::BRIGHT, 1000);

    card.tick(1000 + 9999);
    TEST_ASSERT_TRUE(LightCardState::Engaged::BRIGHT == card.engaged());
}

static void test_idle_detentRefreshesTimer(void)
{
    LightCardState card(true);
    card.sync(colourState());
    card.tapButton(Button::BRIGHT, 1000);
    card.detents(1, 5000); // refreshes idle to 5000; settle also due at 5300

    // Consume the settle first so it doesn't mask the idle assertion below.
    card.tick(5300);

    // 9999ms after the refreshed input time (5000) -> still engaged.
    card.tick(5000 + 9999);
    TEST_ASSERT_TRUE(LightCardState::Engaged::BRIGHT == card.engaged());
}

// ---------------------------------------------------------------------------
// LightCardState: sync() during / after settle
// ---------------------------------------------------------------------------

static void test_sync_duringSettle_keepsLocalEngagedValue(void)
{
    LightCardState card(true);
    card.sync(colourState()); // pct 50
    card.tapButton(Button::BRIGHT, 1000);
    card.detents(1, 1000); // local pct -> 55, settling

    LightsModel::LightState echo = colourState();
    echo.brightnessPct = 50; // stale HA echo of the pre-command value
    card.sync(echo);

    TEST_ASSERT_EQUAL_UINT8(55, card.view().brightnessPct);
}

static void test_sync_duringSettle_adoptsOtherFields(void)
{
    LightCardState card(true);
    card.sync(colourState());
    card.tapButton(Button::BRIGHT, 1000);
    card.detents(1, 1000);

    LightsModel::LightState echo = colourState();
    echo.brightnessPct = 50;
    echo.kelvin = 5000; // some unrelated field HA updated
    card.sync(echo);

    TEST_ASSERT_EQUAL_UINT16(5000, card.view().kelvin);
}

static void test_sync_afterSettled_adoptsEngagedFieldToo(void)
{
    LightCardState card(true);
    card.sync(colourState());
    card.tapButton(Button::BRIGHT, 1000);
    card.detents(1, 1000);
    card.tick(1000 + 300); // settle fires, settling() -> false

    LightsModel::LightState confirmed = colourState();
    confirmed.brightnessPct = 55; // HA confirms the new value
    card.sync(confirmed);

    TEST_ASSERT_EQUAL_UINT8(55, card.view().brightnessPct);
}

static void test_sync_duringSettle_reclampsKelvinToNewBounds(void)
{
    LightCardState card(true);
    LightsModel::LightState s = colourState();
    s.kelvin = 6500;
    s.minKelvin = 2000;
    s.maxKelvin = 6500;
    card.sync(s);
    card.tapButton(Button::COLOUR, 1000); // engages TEMP (mode == TEMP)
    card.detents(0, 1000);                // arm settling; kelvin stays 6500 pending

    LightsModel::LightState echo = colourState();
    echo.minKelvin = 2000;
    echo.maxKelvin = 6000; // HA narrows the bulb's max kelvin
    card.sync(echo);

    TEST_ASSERT_EQUAL_UINT16(6000, card.view().kelvin);

    LightsModel::Command cmd = card.tick(1000 + 300);
    TEST_ASSERT_TRUE(LightsModel::Command::Type::TEMP == cmd.type);
    TEST_ASSERT_EQUAL_UINT16(6000, cmd.kelvin);
}

// ---------------------------------------------------------------------------
// LightCardState: ringFraction()
// ---------------------------------------------------------------------------

static void test_ringFraction_brightness50_isHalf(void)
{
    LightCardState card(true);
    LightsModel::LightState s = colourState();
    s.brightnessPct = 50;
    card.sync(s);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f, card.ringFraction());
}

static void test_ringFraction_kelvinMidRange_isHalf(void)
{
    LightCardState card(true);
    LightsModel::LightState s = colourState();
    s.minKelvin = 2000;
    s.maxKelvin = 6000;
    s.kelvin = 4000; // midpoint
    card.sync(s);
    card.tapButton(Button::COLOUR, 1000); // engages TEMP (mode == TEMP)
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f, card.ringFraction());
}

int main(int argc, char** argv)
{
    UNITY_BEGIN();

    RUN_TEST(test_geom_lamp_returnsSpecCentres);
    RUN_TEST(test_geom_office_returnsSpecCentres);
    RUN_TEST(test_geom_office_colourIsZero);
    RUN_TEST(test_hitTest_lamp_centreHitsEachButton);
    RUN_TEST(test_hitTest_lamp_edgeOfMarginHits);
    RUN_TEST(test_hitTest_lamp_justPastMarginMisses);
    RUN_TEST(test_hitTest_office_centresHit);
    RUN_TEST(test_hitTest_office_colourNeverReturned);
    RUN_TEST(test_hitTest_farAway_returnsNone);
    RUN_TEST(test_label_returnsExpectedStrings);

    RUN_TEST(test_tapPower_fromOff_setsOnAndReturnsPowerOnCommand);
    RUN_TEST(test_tapPower_whenInvalid_isIgnored);
    RUN_TEST(test_tapPower_releasesEngagement);

    RUN_TEST(test_tapBright_engagesThenReleases);
    RUN_TEST(test_tapBright_whenUnavailable_isIgnored);
    RUN_TEST(test_detents_bright_stepsBy5);
    RUN_TEST(test_detents_bright_clampsAt100);
    RUN_TEST(test_detents_bright_clampsAt1);
    RUN_TEST(test_detents_whenNotEngaged_isNoOp);
    RUN_TEST(test_detents_setsOnLocallyAndArmsSettling);

    RUN_TEST(test_detents_temp_clampsToLightsMinMax);

    RUN_TEST(test_detents_hue_wrapsAt360);

    RUN_TEST(test_colour_ignoredOnOfficeLayout);
    RUN_TEST(test_colour_ignoredWhenLightDoesNotSupportIt);
    RUN_TEST(test_colour_cycle_tempMode_goesTempThenHueThenNone);
    RUN_TEST(test_colour_cycle_hsMode_engagesHueDirectlyFromNone);
    RUN_TEST(test_colour_engageHue_zeroSaturation_seedsTo100);

    RUN_TEST(test_tapBright_reTapWithPendingSettle_flushesSettleCommand);
    RUN_TEST(test_tapColour_thirdTapWithPendingSettle_flushesSettleCommand);
    RUN_TEST(test_tapBright_reTapWithNoPendingSettle_returnsNone);

    RUN_TEST(test_settle_noCommandBeforeDeadline);
    RUN_TEST(test_settle_emitsExactlyOneCommandAtDeadline);
    RUN_TEST(test_settle_rearmsFromLastDetent);

    RUN_TEST(test_idle_releasesSilentlyAt10s);
    RUN_TEST(test_idle_notYetReleasedJustBeforeDeadline);
    RUN_TEST(test_idle_detentRefreshesTimer);

    RUN_TEST(test_sync_duringSettle_keepsLocalEngagedValue);
    RUN_TEST(test_sync_duringSettle_adoptsOtherFields);
    RUN_TEST(test_sync_afterSettled_adoptsEngagedFieldToo);
    RUN_TEST(test_sync_duringSettle_reclampsKelvinToNewBounds);

    RUN_TEST(test_ringFraction_brightness50_isHalf);
    RUN_TEST(test_ringFraction_kelvinMidRange_isHalf);

    return UNITY_END();
}
