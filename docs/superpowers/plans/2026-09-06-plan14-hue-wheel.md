# Plan 14: Lamp Colour page hue wheel — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the eight preset swatches on the Lamp card's Colour page with one continuous hue ring: tap the ring to jump to a hue, turn the knob for 5° steps.

**Architecture:** Geometry stays header-only in `src/LightLayout.h` (ring radii, hit annulus, `hueAt`, marker centre); the pure state machine `LightCardState` gains `selectHue()`/`editHue()` in place of `selectPreset()`/`preset()` and keeps the same settle/confirm flow; `DialLightsView` reserves the ring, marker and centre disc as `TRANSPARENT` on the 16-colour canvas and `paintLightTrueColour()` paints them in RGB565 directly on the display after the push (the existing trick, extended from eight discs to 72 arc segments). `DialUi` swaps the swatch hit-test for the annulus test and re-keys frames on the rounded hue. The MQTT payload and the HA automation are unchanged.

**Tech Stack:** PlatformIO (`pio` at `~/Library/Python/3.9/bin/pio`), envs `native` (Unity tests) and `dial-ota` (ESP32-S3, LovyanGFX via M5Dial). Spec: `docs/superpowers/specs/2026-09-03-m5dial-migration-design.md` §4.18 (and §4.12 for the page it lives in).

## Global Constraints

- Ring: outer radius 112, inner 86, hue 0 at 12 o'clock increasing clockwise; 72 segments of 5°; marker disc radius 11 on radius 99 with a 3 px outline (white, amber = `Col::PENDING` while settling/holding); centre disc radius 30 unchanged.
- Touch annulus radius 70–120. Knob `HUE_STEP = 5`°, wrapping at 360. Settle 300 ms, `CONFIRM_HUE_TOL` 2°, `PAGE_IDLE_MS` 10 s — all unchanged.
- Command payload unchanged: `{"state":"ON","hs_color":[round(hue),100]}`.
- The Colour page stays bare (no name, page dots or power glyph). Office card (no colour) unaffected.
- No new heap; true colour is only painted direct-to-display after the canvas push, never into a sprite.
- Never stage `src/config.h`. Commit after every task with `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>` as the last line of the message.
- Run tests with `~/Library/Python/3.9/bin/pio test -e native` (expect 303 → the new total, all passing) and build with `~/Library/Python/3.9/bin/pio run -e dial-ota`. Do not flash: the orchestrator flashes over OTA.

---

### Task 1: Geometry and state — `LightLayout` hue ring, `LightCardState::selectHue()/editHue()`, native tests

**Files:**
- Modify: `src/LightLayout.h` (the "Colour page" block at ~line 48–70 and the helpers `swatchCentre`, `hitSwatch`, `nearestPreset`)
- Modify: `src/LightCardState.h` (constants ~line 45–50, API ~line 111–152, member `m_preset` ~line 187)
- Modify: `src/LightCardState.cpp` (`detents()` COLOUR case ~line 259–276, `selectPreset()` ~line 284–300, `buildSettleCommand()` ~line 318–324, `preset()` ~line 358–367)
- Test: `test/test_light_card_state/test_main.cpp`

**Interfaces:**
- Consumes: nothing new.
- Produces (used by Task 2):
  - `LightLayout::kHueRingOuterR = 112`, `kHueRingInnerR = 86`, `kHueMarkerRingR = 99`, `kHueMarkerR = 11`, `kHueMarkerOutline = 3`, `kHueHitInnerR = 70`, `kHueHitOuterR = 120`, `kHueSegments = 72`, `kHueSegmentDeg = 5.0f`, `kCentreDiscR = 30` (kept).
  - `inline float LightLayout::wrapHue(float h)` → [0, 360).
  - `inline float LightLayout::hueAt(int x, int y)` → hue in [0, 360) of the screen point, 0 at 12 o'clock, clockwise.
  - `inline bool LightLayout::hitHueRing(int x, int y)`.
  - `inline void LightLayout::hueMarkerCentre(float hue, int& x, int& y)`.
  - `void LightCardState::selectHue(float hue, uint32_t nowMs)`, `float LightCardState::editHue() const`, `static constexpr int LightCardState::HUE_STEP = 5`.
  - Removed: `kPresetHues`, `kPresetCount`, `kSwatchRingR`, `kSwatchR`, `kSwatchRSelected`, `kSwatchHitR`, `kSwatchStartDeg`, `swatchCentre`, `hitSwatch`, `nearestPreset`, `LightCardState::selectPreset`, `LightCardState::preset`.

- [ ] **Step 1: Replace the swatch tests with hue-ring tests (they will not compile yet)**

In `test/test_light_card_state/test_main.cpp`:

(a) In `test_layout_pageConstants`, replace the five swatch asserts (`kSwatchRingR`, `kSwatchStartDeg`, `kSwatchR`, `kSwatchRSelected`, `kSwatchHitR`) with:

```cpp
    TEST_ASSERT_EQUAL_INT(112, LightLayout::kHueRingOuterR);
    TEST_ASSERT_EQUAL_INT(86, LightLayout::kHueRingInnerR);
    TEST_ASSERT_EQUAL_INT(99, LightLayout::kHueMarkerRingR);
    TEST_ASSERT_EQUAL_INT(11, LightLayout::kHueMarkerR);
    TEST_ASSERT_EQUAL_INT(3, LightLayout::kHueMarkerOutline);
    TEST_ASSERT_EQUAL_INT(70, LightLayout::kHueHitInnerR);
    TEST_ASSERT_EQUAL_INT(120, LightLayout::kHueHitOuterR);
    TEST_ASSERT_EQUAL_INT(72, LightLayout::kHueSegments);
    TEST_ASSERT_EQUAL_FLOAT(5.0f, LightLayout::kHueSegmentDeg);
```
(keep the `kCentreDiscR == 30` assert.)

(b) Delete these test functions and their `RUN_TEST` lines: `test_layout_presetHues`, `test_swatchCentre_ringStartsHalfAStepPastTwelve`, `test_swatchCentre_goesClockwise`, `test_swatchCentre_everySwatchClearsThePowerGlyph`, `test_swatchCentre_topPairStraddlesTwelveOClock`, `test_hitSwatch_centresHitTheirOwnIndex`, `test_hitSwatch_edgeOfHitRadiusHitsAndJustPastMisses`, `test_hitSwatch_centreOfFaceMisses`, `test_hitSwatch_andPowerGlyph_neverClaimTheSamePoint`, `test_nearestPreset_exactHuesMapToThemselves`, `test_nearestPreset_roundsToNearest`, `test_nearestPreset_wrapsRoundTheCircle`. Replace the "LightLayout: swatch geometry + hit tests" section with:

```cpp
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
```

(c) Replace the three Colour-page detent tests with:

```cpp
static void test_detents_colourPage_stepsFiveDegreesPerDetent(void)
{
    LightCardState card = onCard(); // HA hue 0
    card.swipe(1, 1000);
    card.swipe(1, 1000);
    card.detents(1, 1000);
    TEST_ASSERT_EQUAL_FLOAT(5.0f, card.editHue());
    TEST_ASSERT_EQUAL_FLOAT(5.0f, card.view().hue);
    TEST_ASSERT_EQUAL_FLOAT(100.0f, card.view().sat);
    card.detents(3, 1050);
    TEST_ASSERT_EQUAL_FLOAT(20.0f, card.editHue());
}

static void test_detents_colourPage_wrapsBothWays(void)
{
    LightCardState card = onCard();
    card.swipe(1, 1000);
    card.swipe(1, 1000);
    card.detents(-1, 1000); // 0 -> 355
    TEST_ASSERT_EQUAL_FLOAT(355.0f, card.editHue());
    card.detents(1, 1100); // 355 -> 0
    TEST_ASSERT_EQUAL_FLOAT(0.0f, card.editHue());
}

static void test_detents_colourPage_fromTempMode_startsAtHaHue(void)
{
    LightCardState card(true);
    // TEMP mode, but HA still reports the last hue: 250.
    card.sync(makeState(true, 50, LightsModel::ColorMode::TEMP, 4000, 2000, 6500, 250, 100, true),
              1000);
    card.swipe(1, 1000);
    card.swipe(1, 1000);
    card.detents(1, 1000);
    TEST_ASSERT_EQUAL_FLOAT(255.0f, card.editHue());
    TEST_ASSERT_EQUAL_FLOAT(255.0f, card.view().hue);
    TEST_ASSERT_TRUE(card.colourLive());
}
```

(d) Replace `test_settle_colourPageEmitsHueCommandWithPresetHueAndFullSaturation` with:

```cpp
static void test_settle_colourPageEmitsHueCommandWithRoundedHueAndFullSaturation(void)
{
    LightCardState card = onCard();
    card.swipe(1, 1000);
    card.swipe(1, 1000);
    card.detents(5, 1000); // 0 -> 25
    LightsModel::Command cmd = card.tick(1300);
    TEST_ASSERT_TRUE(LightsModel::Command::Type::HUE == cmd.type);
    TEST_ASSERT_TRUE(cmd.on);
    TEST_ASSERT_EQUAL_FLOAT(25.0f, cmd.hue);
    TEST_ASSERT_EQUAL_FLOAT(100.0f, cmd.sat);
}
```

(e) Replace the five `test_selectPreset_*` tests with:

```cpp
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
    card.swipe(1, 1000); // KELVIN
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
```

(f) Replace `test_preset_followsHaHueWhenIdle` and `test_preset_isThePendingOneWhileSettling` with:

```cpp
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
    card.swipe(1, 1000);
    card.detents(2, 1000); // 0 -> 10
    // A stale HA echo (still hue 0) mustn't drag the marker back.
    card.sync(colourOn(), 1050);
    TEST_ASSERT_EQUAL_FLOAT(10.0f, card.editHue());
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
```

(g) Update `RUN_TEST` calls in `main()` to match: remove the deleted names, add `test_wrapHue_normalisesIntoZeroTo360`, `test_hueAt_cardinalPointsAndDiagonal`, `test_hueAt_isAlwaysInZeroTo360`, `test_hitHueRing_annulusBounds`, `test_hueMarkerCentre_hue0IsTopHue90IsRight`, `test_hueMarkerCentre_roundTripsThroughHueAt`, `test_detents_colourPage_stepsFiveDegreesPerDetent`, `test_detents_colourPage_wrapsBothWays`, `test_detents_colourPage_fromTempMode_startsAtHaHue`, `test_settle_colourPageEmitsHueCommandWithRoundedHueAndFullSaturation`, `test_selectHue_armsSettleAndSendsThatHue`, `test_selectHue_normalisesTheHue`, `test_selectHue_whileOff_isIgnored`, `test_selectHue_offTheColourPage_isIgnored`, `test_selectHue_onACardWithoutColour_isIgnored`, `test_editHue_followsHaHueWhenIdle`, `test_editHue_isThePendingOneWhileSettling`, `test_editHue_holdsTheSentHueUntilConfirmedThenFollowsHa`.

- [ ] **Step 2: Run the native tests to see the compile failure**

Run: `~/Library/Python/3.9/bin/pio test -e native -f test_light_card_state 2>&1 | tail -15`
Expected: compile errors naming `kHueRingOuterR`, `hueAt`, `selectHue`, `editHue` as undeclared.

- [ ] **Step 3: Rewrite the Colour-page geometry in `src/LightLayout.h`**

Replace the block from the comment `// Colour page: eight preset swatches on a ring, selected colour in the` down to and including `static constexpr float kPresetHues[8] = {...};` with:

```cpp
// Colour page (spec 4.18): one continuous hue ring, a marker on it at the
// selected hue, and the selected colour in the centre disc. Hue 0 sits at
// 12 o'clock and increases clockwise, so hue h is at screen angle h - 90.
constexpr int kHueRingOuterR  = 112;
constexpr int kHueRingInnerR  = 86;  // 26 px band
constexpr int kHueMarkerRingR = 99;  // marker centre: the band's mid-radius
constexpr int kHueMarkerR     = 11;
constexpr int kHueMarkerOutline = 3; // white, amber while settling/holding
// Touch annulus, wider than the band on both sides for finger slop.
constexpr int kHueHitInnerR = 70;
constexpr int kHueHitOuterR = 120;
// The ring is painted as this many equal arc segments, each in the RGB of
// its start hue at full saturation and value.
constexpr int   kHueSegments   = 72;
constexpr float kHueSegmentDeg = 360.0f / static_cast<float>(kHueSegments);
constexpr int kCentreDiscR = 30;

constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
constexpr float kRadToDeg = 180.0f / 3.14159265358979323846f;

// Normalises any hue into [0, 360).
inline float wrapHue(float h)
{
    h = fmodf(h, 360.0f);
    if (h < 0.0f)
    {
        h += 360.0f;
    }
    // fmodf can return -0.0f, and (-0.0f < 0.0f) is false; fold it to +0.
    return (h == 0.0f) ? 0.0f : h;
}

// Hue of the screen point (x, y): the angle of the point around the face
// centre, 0 at 12 o'clock, clockwise. atan2f(dx, -dy) is exactly that
// convention (up = 0, right = +90). The centre itself reads 0.
inline float hueAt(int x, int y)
{
    const float dx = static_cast<float>(x - kCentreX);
    const float dy = static_cast<float>(y - kCentreY);
    return wrapHue(atan2f(dx, -dy) * kRadToDeg);
}

// Whether (x, y) is inside the ring's touch annulus.
inline bool hitHueRing(int x, int y)
{
    const int dx = x - kCentreX;
    const int dy = y - kCentreY;
    const int d2 = dx * dx + dy * dy;
    return d2 >= kHueHitInnerR * kHueHitInnerR && d2 <= kHueHitOuterR * kHueHitOuterR;
}

// Centre of the marker disc for `hue`, on the band's mid-radius.
inline void hueMarkerCentre(float hue, int& x, int& y)
{
    const float rad = (wrapHue(hue) - 90.0f) * kDegToRad;
    x = static_cast<int>(lroundf(static_cast<float>(kCentreX) +
                                 static_cast<float>(kHueMarkerRingR) * cosf(rad)));
    y = static_cast<int>(lroundf(static_cast<float>(kCentreY) +
                                 static_cast<float>(kHueMarkerRingR) * sinf(rad)));
}
```

Then delete `swatchCentre()`, `hitSwatch()` and `nearestPreset()` (keep `hitPowerGlyph()`), and in the file's header comment change "the colour swatch ring" to "the colour hue ring".

- [ ] **Step 4: Rewrite the state API in `src/LightCardState.h`**

Add after `static constexpr int KELVIN_STEP = 100;`:

```cpp
    static constexpr int HUE_STEP = 5; // degrees per detent on the Colour page
```

Update the `detents()` doc comment: replace `COLOUR +-1 preset, wrapping` with `COLOUR +-HUE_STEP degrees, wrapping at 360`, and the paragraph starting `On COLOUR, the first detent moves off the preset nearest` with:

```cpp
    // On COLOUR, the first detent steps from editHue() -- the pending edit if
    // one is in flight, otherwise the hue HA reports -- so a light in
    // temperature mode starts from its last colour; the edit also makes hue
    // mode live locally, matching the command that is about to go out.
```

Replace the `selectPreset` declaration and its comment with:

```cpp
    // Tap on the hue ring at `hue` degrees (LightLayout::hueAt). Same gating
    // as detents(), plus: only the Colour page of a colour-capable card has
    // the ring at all, so a call while another page is showing (or on Office)
    // is ignored. The hue is normalised into [0, 360).
    void selectHue(float hue, uint32_t nowMs);
```

Replace the `preset()` declaration and its comment with:

```cpp
    // The hue the marker shows: the edit being sent while it is pending or
    // awaiting confirmation, otherwise HA's reported hue, normalised. No
    // snapping -- a colour set from HA or the Hue app is shown faithfully.
    float editHue() const;
```

Replace the member `uint8_t m_preset = 0;    // preset being edited (only meaningful for a HUE edit/hold)` with:

```cpp
    float m_editHue = 0.0f;  // hue being edited (only meaningful for a HUE edit/hold)
```

Also fix the two comments mentioning `preset tap` on `m_lastEdit`/`m_pageInputMs` to say `ring tap`, and the class comment on line ~19–20 (`selectPreset()`, `preset()`) to `selectHue()`, `editHue()`.

- [ ] **Step 5: Rewrite the Colour-page logic in `src/LightCardState.cpp`**

In `detents()`, replace the `case Page::COLOUR:` block with:

```cpp
    case Page::COLOUR:
    {
        // Step from the hue the marker shows (editHue(): the pending edit, or
        // HA's hue when nothing is pending), wrapping both ways.
        m_editHue  = LightLayout::wrapHue(editHue() + static_cast<float>(n * HUE_STEP));
        m_view.hue = m_editHue;
        m_view.sat = 100.0f;
        m_view.mode = LightsModel::ColorMode::HS;
        m_pending = LightsModel::Command::Type::HUE;
        break;
    }
```

Replace `selectPreset()` entirely with:

```cpp
void LightCardState::selectHue(float hue, uint32_t nowMs)
{
    // Only the Colour page draws the ring, and only a colour-capable card has
    // that page — a tap that lands on those coordinates on any other page is
    // a tap on bare background.
    if (!m_hasColour || m_page != Page::COLOUR || !editable())
    {
        return;
    }

    m_editHue  = LightLayout::wrapHue(hue);
    m_view.hue = m_editHue;
    m_view.sat = 100.0f;
    m_view.mode = LightsModel::ColorMode::HS;
    m_pending = LightsModel::Command::Type::HUE;
    m_lastEdit = nowMs;
    m_pageInputMs = nowMs;
    m_settling = true;
}
```

In `buildSettleCommand()`, replace `c.hue = LightLayout::kPresetHues[m_preset];` with `c.hue = m_editHue;`.

Replace `preset()` with:

```cpp
float LightCardState::editHue() const
{
    const bool pendingHue = m_settling && m_pending == LightsModel::Command::Type::HUE;
    const bool holdingHue = m_confirmHold.type == LightsModel::Command::Type::HUE;
    if (pendingHue || holdingHue)
    {
        return m_editHue;
    }
    return LightLayout::wrapHue(m_view.hue);
}
```

Grep the file for any remaining `preset`/`Preset`/`kPresetHues` and fix comments (there are references in the `detents()` doc block and near `m_lastEdit`).

- [ ] **Step 6: Run the light-card tests**

Run: `~/Library/Python/3.9/bin/pio test -e native -f test_light_card_state 2>&1 | tail -6`
Expected: all PASS (the test count in this file goes from 71 to 74).

Note the device sources (`DialUi.cpp`, `DialLightsView.cpp`) still reference the removed symbols; they are not compiled by `native` and are fixed in Task 2. Do NOT run `pio run -e dial-ota` yet.

- [ ] **Step 7: Run the whole native suite**

Run: `~/Library/Python/3.9/bin/pio test -e native 2>&1 | tail -3`
Expected: `306 test cases: 306 succeeded` (303 − 12 removed + 15 added; if your count differs by one or two because of how many tests you replaced, that is fine as long as every test passes and nothing else changed).

- [ ] **Step 8: Commit**

```bash
git add src/LightLayout.h src/LightCardState.h src/LightCardState.cpp test/test_light_card_state/test_main.cpp
git commit -m "Lights: hue-ring geometry and selectHue()/editHue() replace the preset swatches (spec 4.18)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 2: Drawing and input — `DialLightsView` hue ring, `DialUi` ring tap and frame key

**Files:**
- Modify: `src/DialLightsView.h` (comments on `lightTrueColourVisible`/`paintLightTrueColour`)
- Modify: `src/DialLightsView.cpp` (`presetRgb565()` ~line 71–74, `drawColourPage()` ~line 230–291, `paintLightTrueColour()` ~line 370–382, and the `drawLightCard()` comment ~line 307–312)
- Modify: `src/DialUi.h` (`FrameKey::lightPreset` ~line 265 and its `operator==` ~line 288)
- Modify: `src/DialUi.cpp` (`handleLightTap()` swatch block ~line 593–608; frame-key assignment ~line 1025–1067; comments ~line 558, 1093, 1389)

**Interfaces:**
- Consumes (Task 1): `LightLayout::kHueRingOuterR/kHueRingInnerR/kHueMarkerRingR/kHueMarkerR/kHueMarkerOutline/kHueSegments/kHueSegmentDeg/kCentreDiscR`, `LightLayout::hueAt(int,int)`, `LightLayout::hitHueRing(int,int)`, `LightLayout::hueMarkerCentre(float,int&,int&)`, `LightCardState::selectHue(float,uint32_t)`, `LightCardState::editHue()`.
- Produces: nothing new for later tasks. `FrameKey::lightHue` (uint16_t, rounded degrees) replaces `lightPreset`.

- [ ] **Step 1: Update `src/DialLightsView.cpp`**

(a) Replace `presetRgb565()` with:

```cpp
// RGB565 of a hue at full saturation and value: the ring segments, the
// marker fill and the centre disc all go through this one function so they
// cannot disagree about what "hue 240" looks like.
uint16_t hueRgb565(float hue)
{
    return hsvToRgb565(hue, 100.0f, 100.0f);
}
```

and fix the `hsvToRgb565` comment above it: "Only used for the eight preset hues" → "Used for the ring segments, marker and centre disc".

(b) Replace `drawColourPage()` with:

```cpp
// The Colour page (spec 4.18): a continuous hue ring, a marker on it at the
// selected hue, and the selected colour in the centre disc. All three are true
// colour, which the 16-colour canvas cannot hold: with `trueColour` the canvas
// reserves them as TRANSPARENT and paintLightTrueColour() paints them on the
// display after the push. Without it (hue mode not live) the page draws dim:
// the band's two boundary circles, an outline where the marker sits, the
// colour-dots glyph and the "not active" caption.
void drawColourPage(LovyanGFX& gfx, const DialTheme& theme, const LightCardState& card,
                    bool trueColour)
{
    int mx = 0;
    int my = 0;
    LightLayout::hueMarkerCentre(card.editHue(), mx, my);

    if (!trueColour)
    {
        gfx.drawCircle(kCentreX, kCentreY, LightLayout::kHueRingOuterR, theme.col(Col::DIM_DIM));
        gfx.drawCircle(kCentreX, kCentreY, LightLayout::kHueRingInnerR, theme.col(Col::DIM_DIM));
        gfx.drawCircle(mx, my, LightLayout::kHueMarkerR, theme.col(Col::DIM));
        drawColourDotsGlyph(gfx, kCentreX, kCentreY, kColourGlyphR, theme.col(Col::DIM));
        gfx.setTextColor(theme.col(Col::DIM), theme.col(Col::BG));
        gfx.drawString("not active - turn to use", kCentreX, LightLayout::kBrightCaptionY,
                       &fonts::Font2);
        return;
    }

    if (theme.useCanvas)
    {
        // Reserved for paintLightTrueColour(): the push skips this index and
        // leaves the display's own pixels alone. The marker's reservation
        // includes its outline, which is painted there too (a canvas outline
        // would be covered by the ring segments painted after the push).
        const uint32_t t = static_cast<uint32_t>(Col::TRANSPARENT);
        gfx.fillArc(kCentreX, kCentreY, LightLayout::kHueRingOuterR, LightLayout::kHueRingInnerR,
                    0.0f, 360.0f, t);
        gfx.fillCircle(mx, my, LightLayout::kHueMarkerR + LightLayout::kHueMarkerOutline, t);
        gfx.fillCircle(kCentreX, kCentreY, LightLayout::kCentreDiscR, t);
    }
    else
    {
        // Direct-to-display fallback: no push, so paint the real colours here.
        paintHueWheel(gfx, card, theme.col(card.settling() ? Col::PENDING : Col::TEXT));
    }
}
```

(c) Add, in the same anonymous namespace ABOVE `drawColourPage()` (it is used by both it and `paintLightTrueColour()`):

```cpp
// Paints the true-colour parts of the Colour page onto `gfx`: the 72 ring
// segments, the centre disc in the selected hue, then the marker fill and its
// outline in `outlineColour`. Order matters: the marker overlaps the ring.
// fillArc's angles are degrees clockwise from 3 o'clock, so hue h (0 at 12
// o'clock) is at fillArc angle h - 90; adding 360 keeps every angle positive.
void paintHueWheel(LovyanGFX& gfx, const LightCardState& card, uint32_t outlineColour)
{
    for (int i = 0; i < LightLayout::kHueSegments; ++i)
    {
        const float hue = static_cast<float>(i) * LightLayout::kHueSegmentDeg;
        const float a0  = hue - 90.0f + 360.0f;
        // A hair of overlap so the segment boundaries never leave a gap.
        gfx.fillArc(kCentreX, kCentreY, LightLayout::kHueRingOuterR, LightLayout::kHueRingInnerR,
                    a0, a0 + LightLayout::kHueSegmentDeg + 0.5f, hueRgb565(hue));
    }

    const float sel = card.editHue();
    gfx.fillCircle(kCentreX, kCentreY, LightLayout::kCentreDiscR, hueRgb565(sel));

    int mx = 0;
    int my = 0;
    LightLayout::hueMarkerCentre(sel, mx, my);
    gfx.fillCircle(mx, my, LightLayout::kHueMarkerR + LightLayout::kHueMarkerOutline, outlineColour);
    gfx.fillCircle(mx, my, LightLayout::kHueMarkerR, hueRgb565(sel));
}
```

(d) Replace `paintLightTrueColour()` with:

```cpp
void paintLightTrueColour(LGFX_Device& display, const LightCardState& card)
{
    // The display is 16-bit, so the outline colours are RGB565 literals: white,
    // and TFT_ORANGE 0xFDA0 which is what Col::PENDING maps to (DialTheme.h).
    const uint32_t outline = card.settling() ? 0xFDA0u : 0xFFFFu;
    paintHueWheel(display, card, outline);
}
```

(e) In `drawLightCard()`, update the bare-page comment: "The swatch ring is unmistakable on its own, and with the ring rotated onto the top and bottom of the face there is no room left" → "The hue ring fills the outer 26 px of the face, so there is no room left".

(f) Grep `src/DialLightsView.cpp` for `preset`, `swatch`, `kPreset` and fix every remaining reference (the `drawBrightPage`/caption code is unaffected; only comments should remain).

- [ ] **Step 2: Update `src/DialLightsView.h` comments**

`lightTrueColourVisible`: "the eight swatches and the centre disc" → "the hue ring, its marker and the centre disc". `paintLightTrueColour`: replace its comment with:

```cpp
// Paints the hue ring (72 five-degree segments between LightLayout's inner and
// outer radii), the centre disc (kCentreDiscR) and the marker (kHueMarkerR
// plus its outline — white, amber while settling) in true colour straight
// onto the display. Only call it when lightTrueColourVisible() agrees, and
// only after the frame has been pushed.
```

- [ ] **Step 3: Update `src/DialUi.h` frame key**

Replace `uint8_t  lightPreset    = 0;` with:

```cpp
        uint16_t lightHue       = 0; // editHue() rounded to whole degrees
```

and in `operator==` replace `lightPreset == o.lightPreset` with `lightHue == o.lightHue`.

- [ ] **Step 4: Update `src/DialUi.cpp`**

(a) In `handleLightTap()`, replace the swatch block:

```cpp
    if (card.page() == LightCardState::Page::COLOUR && card.hasColour())
    {
        const int8_t swatch = LightLayout::hitSwatch(x, y);
        if (swatch >= 0)
        {
            // Same settle path as a detent: the command goes out 300 ms later
            // from tickLights(), so a quick second choice replaces the first.
            card.selectPreset(static_cast<uint8_t>(swatch), nowMs);
            playAcceptBeep(true);
            return;
        }
    }
```

with:

```cpp
    // The ring is the Colour page's whole point (spec 4.18): a tap inside its
    // touch annulus selects the hue at that angle. The power glyph is not
    // drawn on this page, so there is nothing else to hit.
    if (card.page() == LightCardState::Page::COLOUR && card.hasColour())
    {
        if (LightLayout::hitHueRing(x, y))
        {
            // Same settle path as a detent: the command goes out 300 ms later
            // from tickLights(), so a quick second choice replaces the first.
            const float hue = LightLayout::hueAt(x, y);
            log_i("Light tap: hue ring at (%d,%d) -> %.0f", x, y, static_cast<double>(hue));
            card.selectHue(hue, nowMs);
            playAcceptBeep(true);
            return;
        }
    }
```

and rewrite the comment above it ("Swatches first: ... the priority is free.") to a single line: `// Colour page: only the hue ring is a target (the glyph is not drawn there).`

(b) In the frame-key code replace `key.lightPreset   = card.preset();` with:

```cpp
        key.lightHue      = static_cast<uint16_t>(lroundf(card.editHue())) % 360u;
```

and the zeroing `key.lightPreset   = 0;` with `key.lightHue      = 0;`. Update the comment above ("preset/settling drive the Colour page's selected swatch") to "editHue/settling drive the Colour page's marker".

(c) Grep `src/DialUi.cpp` for `swatch`, `preset`, `Preset` and fix the remaining comments (around lines 558, 1093, 1133, 1389): they describe "swatches and centre disc"; say "hue ring, marker and centre disc".

- [ ] **Step 5: Build the device firmware**

Run: `~/Library/Python/3.9/bin/pio run -e dial-ota 2>&1 | grep -E "error|warning: unused|SUCCESS|FAILED" | tail -8`
Expected: `[SUCCESS]`, no errors. If `fillArc` complains about argument types, cast the angles to `float` explicitly (LovyanGFX: `fillArc(x, y, r0, r1, angle0, angle1, color)`).

- [ ] **Step 6: Run the native suite once more (nothing should change)**

Run: `~/Library/Python/3.9/bin/pio test -e native 2>&1 | tail -2`
Expected: all pass, same count as Task 1 Step 7.

- [ ] **Step 7: Commit**

```bash
git add src/DialLightsView.h src/DialLightsView.cpp src/DialUi.h src/DialUi.cpp
git commit -m "Lights: paint the hue ring, marker and centre disc in true colour; ring tap selects the hue (spec 4.18)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 3: Docs — README, audit Phase K checklist, spec as-built note

**Files:**
- Modify: `README.md` (the Colour page paragraph ~line 286–301)
- Modify: `doc/AUDIT_2026-09-03.md` (append `## Phase K hardware checklist (hue wheel)` after Phase J)
- Modify: `docs/superpowers/specs/2026-09-03-m5dial-migration-design.md` (§4.12 Colour-page bullet gets a superseded marker; §4.18 gets an as-built line only if the implementation deviated)

**Interfaces:** none.

- [ ] **Step 1: README**

Replace the Colour page description (the sentences mentioning "One preset per detent (wrapping), or tap a swatch directly" and "The swatches are rotated half a step so none sits at 12 or 6 o'clock") with:

```markdown
- **Colour** (Lamp only): a continuous hue ring around the edge of the face with a marker at the
  selected hue and the colour itself in the centre disc. Tap anywhere on the ring to jump to that
  hue; each knob detent moves 5° (72 detents per revolution, wrapping). The marker outline turns
  amber while the command settles and until HA echoes it. This page draws nothing but the ring,
  marker and disc: leave it by swiping back or waiting for the 10 s page timeout.
```

Also change "a detent or a swatch tap" (~line 300) to "a detent or a ring tap".

- [ ] **Step 2: Audit Phase K**

Append to `doc/AUDIT_2026-09-03.md`:

```markdown
## Phase K hardware checklist (hue wheel)

Real-hardware verification for the Colour page hue ring (spec 4.18), to be run by Mike once Plan 14
is flashed. Watch `pacekeeper-dial/light/lamp/set` (e.g. MQTT Explorer) alongside.

- [ ] Lamp on, swipe to the Colour page: a full-spectrum ring fills the outer band, red at 12 o'clock,
      green at about 4 o'clock, blue at about 8 o'clock; marker sits on the lamp's current hue; centre
      disc shows that colour. No card name, dots or power glyph.
- [ ] Tap the ring at 3 o'clock: marker jumps there with an amber outline, `hs_color [90,100]` is
      published within ~0.3 s, the lamp goes green, the outline turns white when HA echoes.
- [ ] Three clockwise detents: one command `hs_color [105,100]` after the last detent; marker moves
      15° clockwise.
- [ ] Anticlockwise past 12 o'clock wraps (e.g. 5 → 0 → 355).
- [ ] Tap just inside the ring (about 75 px from centre) and just outside (about 118 px): both count.
      Tap the centre disc: nothing happens.
- [ ] Change the lamp's colour from HA: the marker follows to the new hue within a second.
- [ ] Set the lamp to a colour temperature from the Kelvin page, swipe to Colour: the ring is two dim
      circles with the "not active - turn to use" caption; one detent brings the colour ring back.
- [ ] Office card: Brightness and Kelvin pages only, nothing changed.
- [ ] 10 s without input on the Colour page returns to Brightness.
- [ ] Heap line in the log (`Heap:` every 15 s) is unchanged before and after the Colour page.
```

- [ ] **Step 3: Spec §4.12 superseded marker**

In `docs/superpowers/specs/2026-09-03-m5dial-migration-design.md`, prefix the §4.12 bullet beginning `- **Colour page:** eight preset hues` with `*(superseded 2026-09-06 by §4.18 — continuous hue ring)*` right after `**Colour page:**`. Do the same for §4.12 amendment 1 (`**The swatch ring is rotated half a step.**`): prefix `*(superseded by §4.18)*`. If anything in the implementation differs from §4.18 (radii, segment count, marker size), add a one-line `**As built:**` bullet at the end of §4.18 saying what and why; otherwise leave §4.18 alone.

- [ ] **Step 4: Commit**

```bash
git add README.md doc/AUDIT_2026-09-03.md docs/superpowers/specs/2026-09-03-m5dial-migration-design.md
git commit -m "Docs: hue wheel in README, Phase K checklist, spec 4.12 superseded markers

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

## Self-review notes

- Spec coverage: geometry/touch/knob/state/command → Task 1; drawing/frame key/cost → Task 2; success checklist and docs → Task 3. `CONFIRM_HUE_TOL` and the Hue-bulb 258-vs-275 note need no code change (covered by `test_editHue_holdsTheSentHueUntilConfirmedThenFollowsHa`).
- Names used across tasks: `hueAt`, `hitHueRing`, `hueMarkerCentre`, `wrapHue`, `selectHue`, `editHue`, `HUE_STEP`, `kHue*`, `lightHue`, `hueRgb565`, `paintHueWheel` — consistent.
- The `-0.0f` fold in `wrapHue` exists because `fmodf(-360.0f, 360.0f)` is `-0.0f`, which `TEST_ASSERT_EQUAL_FLOAT(0.0f, ...)` accepts but which would print as `-0` in the frame key; harmless, kept for tidiness.
