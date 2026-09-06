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

// HSV (h in degrees, s/v 0..100) to RGB565. Only used for the eight preset
// hues at full saturation and value, but written generally so the swatch and
// the disc cannot disagree about what "hue 240" looks like. Passing the
// uint16_t result to a LovyanGFX draw call selects convert_rgb565()
// (colortype.hpp: TYPECHECK(uint16_t)), which is what the display wants.
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

uint16_t presetRgb565(uint8_t i)
{
    return hsvToRgb565(LightLayout::kPresetHues[i % LightLayout::kPresetCount], 100.0f, 100.0f);
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

void drawColourPage(LovyanGFX& gfx, const DialTheme& theme, const LightCardState& card,
                    bool trueColour)
{
    const uint8_t sel = card.preset();

    for (uint8_t i = 0; i < LightLayout::kPresetCount; ++i)
    {
        int x = 0;
        int y = 0;
        LightLayout::swatchCentre(i, x, y);
        const int r = (i == sel) ? LightLayout::kSwatchRSelected : LightLayout::kSwatchR;
        if (!trueColour)
        {
            // Hue mode is not live: the whole page draws dim, so the swatches
            // are outlines rather than colour (and nothing is reserved for
            // paintLightTrueColour() to fill).
            gfx.drawCircle(x, y, r, theme.col(Col::DIM_DIM));
            continue;
        }
        if (theme.useCanvas)
        {
            // Reserved for paintLightTrueColour(): the push skips this index
            // and leaves the display's own pixels alone.
            gfx.fillCircle(x, y, r, static_cast<uint32_t>(Col::TRANSPARENT));
        }
        else
        {
            // Direct-to-display fallback: no push, so paint the real colour here.
            gfx.fillCircle(x, y, r, presetRgb565(i));
        }
    }

    if (!trueColour)
    {
        drawColourDotsGlyph(gfx, kCentreX, kCentreY, kColourGlyphR, theme.col(Col::DIM));
        gfx.setTextColor(theme.col(Col::DIM), theme.col(Col::BG));
        gfx.drawString("not active - turn to use", kCentreX, LightLayout::kBrightCaptionY,
                       &fonts::Font2);
        return;
    }

    // Selected swatch's 3 px outline, just outside the fill: white, amber
    // while the edit is still settling.
    int sx = 0;
    int sy = 0;
    LightLayout::swatchCentre(sel, sx, sy);
    const uint32_t outline = theme.col(card.settling() ? Col::PENDING : Col::TEXT);
    for (int i = 1; i <= 3; ++i)
    {
        gfx.drawCircle(sx, sy, LightLayout::kSwatchRSelected + i, outline);
    }

    if (theme.useCanvas)
    {
        gfx.fillCircle(kCentreX, kCentreY, LightLayout::kCentreDiscR,
                       static_cast<uint32_t>(Col::TRANSPARENT));
    }
    else
    {
        gfx.fillCircle(kCentreX, kCentreY, LightLayout::kCentreDiscR, presetRgb565(sel));
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

    gfx.setTextDatum(middle_center);
    gfx.setTextColor(theme.col(Col::TEXT), theme.col(Col::BG));
    gfx.drawString(name, kCentreX, kNameY, &fonts::Font2);

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

    drawPageDots(gfx, theme, card);
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
    drawPowerGlyph(gfx, LightLayout::kPowerGlyphX, LightLayout::kPowerGlyphY,
                   LightLayout::kPowerGlyphR, theme.col(Col::DIM));
}

void paintLightTrueColour(LGFX_Device& display, const LightCardState& card)
{
    const uint8_t sel = card.preset();
    for (uint8_t i = 0; i < LightLayout::kPresetCount; ++i)
    {
        int x = 0;
        int y = 0;
        LightLayout::swatchCentre(i, x, y);
        display.fillCircle(x, y, (i == sel) ? LightLayout::kSwatchRSelected : LightLayout::kSwatchR,
                           presetRgb565(i));
    }
    display.fillCircle(kCentreX, kCentreY, LightLayout::kCentreDiscR, presetRgb565(sel));
}

#endif // HAS_DIAL_UI
