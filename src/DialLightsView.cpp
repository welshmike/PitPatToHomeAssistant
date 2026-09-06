#include "DialLightsView.h"
#if HAS_DIAL_UI

#include <math.h>
#include <stdio.h>

#include "DialGlyphs.h"
#include "LightLayout.h"

namespace
{

using LightLayout::kCentreX;
using LightLayout::kCentreY;

// Card name, top of every face.
constexpr int kNameY = 34;
// Off face: caption under the big power circle.
constexpr int kOffCaptionY = 196;
// Brightness page: the same 300-degree ring the speed/selector screens use
// (spec 4.12: "300 deg ring (existing geometry)"), repeated here rather than
// shared with DialUi because the two are the same numbers by design, not by
// dependency.
constexpr int kRingOuter = 118;
constexpr int kRingInner = 108;
constexpr float kRingStartDeg = 120.0f;
constexpr float kRingSweepDeg = 300.0f;
// Gap between the Font7 percentage digits and the Font4 "%" beside them:
// Font7 is a 7-segment font whose only glyphs are space, 0-9, ':', '.' and
// '-' (Font7srle.h), so a '%' inside a Font7 string prints as a blank cell.
constexpr int kPctGap = 4;
// Kelvin page: unit under the value, then the bar's marker and the caption.
constexpr int kKelvinUnitY   = 148;
// 190, not lower: the round bezel eats the ends of a 24-character Font2 line
// ("not active - turn to use") much below this.
constexpr int kKelvinCaptionY = 190;
// Colour page: the "not active" glyph sits where the centre disc would be.
constexpr int kColourGlyphR = 26;

// HSV (h in degrees, s/v 0..100) to RGB565. Used for the ring segments,
// marker and centre disc. Passing the uint16_t result to a LovyanGFX draw
// call selects convert_rgb565() (colortype.hpp: TYPECHECK(uint16_t)), which
// is what the display wants.
uint16_t hsvToRgb565(float h, float s, float v)
{
    h = fmodf(h, 360.0f);
    if (h < 0.0f)
    {
        h += 360.0f;
    }
    const float sat = s / 100.0f;
    const float val = v / 100.0f;
    const float c = val * sat;
    const float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    const float m = val - c;
    float rf = 0.0f, gf = 0.0f, bf = 0.0f;
    if      (h <  60.0f) { rf = c; gf = x; }
    else if (h < 120.0f) { rf = x; gf = c; }
    else if (h < 180.0f) { gf = c; bf = x; }
    else if (h < 240.0f) { gf = x; bf = c; }
    else if (h < 300.0f) { rf = x; bf = c; }
    else                 { rf = c; bf = x; }
    const uint8_t r = static_cast<uint8_t>(lroundf((rf + m) * 255.0f));
    const uint8_t g = static_cast<uint8_t>(lroundf((gf + m) * 255.0f));
    const uint8_t b = static_cast<uint8_t>(lroundf((bf + m) * 255.0f));
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// RGB565 of a hue at full saturation and value: the ring segments, the
// marker fill and the centre disc all go through this one function so they
// cannot disagree about what "hue 240" looks like.
uint16_t hueRgb565(float hue)
{
    return hsvToRgb565(hue, 100.0f, 100.0f);
}

int clampInt(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// The live colour state, as the Brightness page's caption shows it: the
// kelvin value, or the hue, whichever mode HA has the light in.
void formatColourState(const LightsModel::LightState& v, char* buf, size_t cap)
{
    if (v.mode == LightsModel::ColorMode::HS)
    {
        snprintf(buf, cap, "hue %d", static_cast<int>(v.hue));
    }
    else if (v.kelvin == 0)
    {
        snprintf(buf, cap, "--K");
    }
    else
    {
        snprintf(buf, cap, "%uK", static_cast<unsigned>(v.kelvin));
    }
}

void drawPageDots(LovyanGFX& gfx, const DialTheme& theme, const LightCardState& card)
{
    const int count = static_cast<int>(card.pageCount());
    const int firstX = kCentreX - ((count - 1) * LightLayout::kPageDotSpacing) / 2;
    for (int i = 0; i < count; ++i)
    {
        const int x = firstX + i * LightLayout::kPageDotSpacing;
        if (i == static_cast<int>(card.page()))
        {
            gfx.fillCircle(x, LightLayout::kPageDotY, LightLayout::kPageDotR, theme.col(Col::TEXT));
        }
        else
        {
            gfx.drawCircle(x, LightLayout::kPageDotY, LightLayout::kPageDotR, theme.col(Col::DIM));
        }
    }
}

void drawOffFace(LovyanGFX& gfx, const DialTheme& theme)
{
    // The whole face is the switch-on target; the circle just says where to
    // aim. 2 px so it reads as a button rather than a hairline.
    gfx.drawCircle(LightLayout::kPowerCircleX, LightLayout::kPowerCircleY,
                   LightLayout::kPowerCircleR, theme.col(Col::DIM));
    gfx.drawCircle(LightLayout::kPowerCircleX, LightLayout::kPowerCircleY,
                   LightLayout::kPowerCircleR - 1, theme.col(Col::DIM));
    drawPowerGlyph(gfx, LightLayout::kPowerCircleX, LightLayout::kPowerCircleY,
                   LightLayout::kPowerCircleR / 2, theme.col(Col::TEXT));
    gfx.setTextColor(theme.col(Col::DIM), theme.col(Col::BG));
    gfx.drawString("tap to switch on", kCentreX, kOffCaptionY, &fonts::Font2);
}

void drawBrightPage(LovyanGFX& gfx, const DialTheme& theme, const LightCardState& card)
{
    const LightsModel::LightState& v = card.view();

    gfx.fillArc(kCentreX, kCentreY, kRingOuter, kRingInner, kRingStartDeg,
                kRingStartDeg + kRingSweepDeg, theme.col(Col::DIM));
    const float sweep = card.ringFraction() * kRingSweepDeg;
    if (sweep > 0.0f)
    {
        gfx.fillArc(kCentreX, kCentreY, kRingOuter, kRingInner, kRingStartDeg,
                    kRingStartDeg + sweep,
                    theme.col(card.settling() ? Col::PENDING : Col::SPEED));
    }

    // Digits and percent sign are two strings in two fonts (see kPctGap),
    // together centred on the face.
    char pctBuf[8];
    snprintf(pctBuf, sizeof(pctBuf), "%u", static_cast<unsigned>(v.brightnessPct));
    const int32_t digitsW = gfx.textWidth(pctBuf, &fonts::Font7);
    const int32_t signW   = gfx.textWidth("%", &fonts::Font4);
    const int32_t left    = kCentreX - (digitsW + kPctGap + signW) / 2;
    gfx.setTextColor(theme.col(card.settling() ? Col::PENDING : Col::TEXT), theme.col(Col::BG));
    gfx.setTextDatum(middle_left);
    gfx.drawString(pctBuf, left, kCentreY, &fonts::Font7);
    gfx.drawString("%", left + digitsW + kPctGap, kCentreY, &fonts::Font4);
    gfx.setTextDatum(middle_center);

    char caption[16];
    formatColourState(v, caption, sizeof(caption));
    gfx.setTextColor(theme.col(Col::DIM), theme.col(Col::BG));
    gfx.drawString(caption, kCentreX, LightLayout::kBrightCaptionY, &fonts::Font2);
}

void drawKelvinPage(LovyanGFX& gfx, const DialTheme& theme, const LightCardState& card)
{
    const LightsModel::LightState& v = card.view();
    // The light is in one colour mode at a time: while it is in hue mode this
    // page is not live and draws dim until a detent sends its command.
    const bool live = card.kelvinLive();
    const Col valueCol = !live ? Col::DIM : (card.settling() ? Col::PENDING : Col::TEXT);

    char valueBuf[8];
    if (v.kelvin == 0)
    {
        snprintf(valueBuf, sizeof(valueBuf), "----");
    }
    else
    {
        snprintf(valueBuf, sizeof(valueBuf), "%u", static_cast<unsigned>(v.kelvin));
    }
    gfx.setTextColor(theme.col(valueCol), theme.col(Col::BG));
    gfx.drawString(valueBuf, kCentreX, LightLayout::kKelvinValueY, &fonts::Font7);
    gfx.drawString("K", kCentreX, kKelvinUnitY, &fonts::Font4);

    // Warm -> cool bar in three palette steps, then a marker at where this
    // light's kelvin sits inside its own [minKelvin, maxKelvin].
    const int barW = LightLayout::kKelvinBarX1 - LightLayout::kKelvinBarX0;
    const int stepW = barW / 3;
    const Col steps[3] = {Col::WARM, Col::NEUTRAL, Col::COOL};
    for (int i = 0; i < 3; ++i)
    {
        const int x = LightLayout::kKelvinBarX0 + i * stepW;
        const int w = (i == 2) ? (LightLayout::kKelvinBarX1 - x) : stepW;
        if (live)
        {
            gfx.fillRect(x, LightLayout::kKelvinBarY, w, LightLayout::kKelvinBarH,
                         theme.col(steps[i]));
        }
        else
        {
            gfx.drawRect(x, LightLayout::kKelvinBarY, w, LightLayout::kKelvinBarH,
                         theme.col(Col::DIM_DIM));
        }
    }

    const int span = static_cast<int>(v.maxKelvin) - static_cast<int>(v.minKelvin);
    const int offset = clampInt(static_cast<int>(v.kelvin) - static_cast<int>(v.minKelvin),
                                0, span > 0 ? span : 0);
    const int markerX = (span > 0)
                            ? LightLayout::kKelvinBarX0 + (offset * barW) / span
                            : LightLayout::kKelvinBarX0 + barW / 2;
    gfx.fillTriangle(markerX - 6, LightLayout::kKelvinBarY - 10,
                     markerX + 6, LightLayout::kKelvinBarY - 10,
                     markerX, LightLayout::kKelvinBarY - 1,
                     theme.col(live ? Col::TEXT : Col::DIM));

    char caption[28];
    if (live)
    {
        snprintf(caption, sizeof(caption), "%u - %uK", static_cast<unsigned>(v.minKelvin),
                 static_cast<unsigned>(v.maxKelvin));
    }
    else
    {
        snprintf(caption, sizeof(caption), "not active - turn to use");
    }
    gfx.setTextColor(theme.col(Col::DIM), theme.col(Col::BG));
    gfx.drawString(caption, kCentreX, kKelvinCaptionY, &fonts::Font2);
}

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
        paintHueWheel(gfx, card, theme.col(card.hueEditInFlight() ? Col::PENDING : Col::TEXT));
    }
}

} // namespace

bool lightTrueColourVisible(const LightCardState& card, bool mqttUp)
{
    const LightsModel::LightState& v = card.view();
    return mqttUp && v.valid && v.available && v.on && card.hasColour() &&
           card.page() == LightCardState::Page::COLOUR && card.colourLive();
}

void drawLightCard(LovyanGFX& gfx, const DialTheme& theme, const LightCardState& card,
                   const char* name, bool mqttUp)
{
    const LightsModel::LightState& v = card.view();

    // The Colour page draws bare: no card name, no page dots. The hue ring
    // fills the outer 26 px of the face, so there is no room left for either
    // without crowding (Mike: "hide the background") — and no power glyph
    // either: Mike wants nothing under the picker. Swipe back, or the 10 s
    // page timeout, leaves it.
    const bool bareColourPage = mqttUp && v.valid && v.available && v.on &&
                                card.page() == LightCardState::Page::COLOUR;

    gfx.setTextDatum(middle_center);
    if (!bareColourPage)
    {
        gfx.setTextColor(theme.col(Col::TEXT), theme.col(Col::BG));
        gfx.drawString(name, kCentreX, kNameY, &fonts::Font2);
    }

    // MQTT is the only path HA light state arrives on, so without it there is
    // nothing to show and nothing a tap could do.
    if (!mqttUp)
    {
        gfx.setTextColor(theme.col(Col::DIM), theme.col(Col::BG));
        gfx.drawString("waiting for HA", kCentreX, kCentreY, &fonts::Font4);
        return;
    }
    if (!v.valid || !v.available)
    {
        gfx.setTextColor(theme.col(Col::DIM), theme.col(Col::BG));
        gfx.drawString("no data", kCentreX, kCentreY, &fonts::Font4);
        return;
    }

    if (!v.on)
    {
        drawOffFace(gfx, theme);
        return;
    }

    if (!bareColourPage)
    {
        drawPageDots(gfx, theme, card);
    }
    switch (card.page())
    {
    case LightCardState::Page::KELVIN:
        drawKelvinPage(gfx, theme, card);
        break;
    case LightCardState::Page::COLOUR:
        drawColourPage(gfx, theme, card, lightTrueColourVisible(card, mqttUp));
        break;
    case LightCardState::Page::BRIGHT:
    default:
        drawBrightPage(gfx, theme, card);
        break;
    }

    // Small power glyph at the bottom: tap it (or hold anywhere) to switch off.
    // Not on the bare Colour page (see above).
    if (!bareColourPage)
    {
        drawPowerGlyph(gfx, LightLayout::kPowerGlyphX, LightLayout::kPowerGlyphY,
                       LightLayout::kPowerGlyphR, theme.col(Col::DIM));
    }
}

void paintLightTrueColour(LGFX_Device& display, const LightCardState& card)
{
    // This path paints straight to the display outside the canvas, so
    // LovyanGFX wants the outline colour as RGB888 (uint32_t) -- unlike the
    // ring and marker disc, which paintHueWheel() draws RGB565 (uint16_t) via
    // hueRgb565(). dialPaletteRgb888() gives the RGB888 value for a Col.
    const uint32_t outline =
        dialPaletteRgb888(static_cast<uint8_t>(card.hueEditInFlight() ? Col::PENDING : Col::TEXT));
    paintHueWheel(display, card, outline);
}

#endif // HAS_DIAL_UI
