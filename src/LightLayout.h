#pragma once

#include <math.h>
#include <stdint.h>

// Pure geometry for the Lights card faces (spec §4.12 "Lights v2").
// Header-only and Arduino-free: DialUi draws from these constants and
// hit-tests taps with the same helpers, so what is painted and what is
// touchable can never drift apart. Coordinates are card-local pixels on the
// Dial's 240x240 round display; the face centre is (120, 120).
//
// Replaces LightButtons.h — there are no on-screen text buttons any more,
// just the off-face power circle, the small power glyph, the page dots and
// the colour swatch ring.
namespace LightLayout
{

constexpr int kCentreX = 120;
constexpr int kCentreY = 120;

// Off face: one big circle carrying the power glyph, tappable anywhere on
// the face (DialUi treats the whole card as the target), drawn here.
constexpr int kPowerCircleX = 120;
constexpr int kPowerCircleY = 124;
constexpr int kPowerCircleR = 44;

// On face: the small power glyph that switches the light off.
constexpr int kPowerGlyphX = 120;
constexpr int kPowerGlyphY = 208;
constexpr int kPowerGlyphR = 9;
constexpr int kPowerGlyphHitR = 18; // generous: a tap on the glyph did not register on hardware at r 14 (2026-09-06)

// On face: the page dots (one per LightCardState::Page), centred on kCentreX.
constexpr int kPageDotY = 54;
constexpr int kPageDotSpacing = 12;
constexpr int kPageDotR = 3;

// Brightness page: caption showing the live colour state under the value.
constexpr int kBrightCaptionY = 160;

// Kelvin page: big value, then the warm->cool bar with its marker.
constexpr int kKelvinValueY = 112;
constexpr int kKelvinBarX0 = 52;
constexpr int kKelvinBarX1 = 188;
constexpr int kKelvinBarY = 170;
constexpr int kKelvinBarH = 8;

// Colour page: eight preset swatches on a ring, selected colour in the
// centre disc.
constexpr int kSwatchRingR = 74;
constexpr int kSwatchR = 12;
constexpr int kSwatchRSelected = 16;
constexpr int kSwatchHitR = 14; // swatch r 12 fully covered; kept small so the bottom pair stays clear of the power glyph
constexpr int kCentreDiscR = 30;

constexpr uint8_t kPresetCount = 8;

// Angle of swatch 0, measured clockwise from 12 o'clock. Half a step (45/2),
// so the ring is rotated to leave both 12 and 6 o'clock empty: the page-dot
// row lives at the top and the small power glyph at the bottom, and a swatch
// on either would sit under (or next to) them. With this rotation the closest
// swatch centres — 3 and 4, at 157.5 and 202.5 degrees, ~(148,188) and
// ~(92,188) — are ~34.5 px from the power glyph centre (120,208), comfortably
// past the kSwatchHitR + kPowerGlyphHitR = 32 px at which the two hit discs
// would start to overlap (see the layout tests).
constexpr float kSwatchStartDeg = 22.5f;

// The eight preset hues, all at saturation 100. Index order is ring order:
// index 0 sits at kSwatchStartDeg and the rest run clockwise.
static constexpr float kPresetHues[8] = {0, 30, 60, 120, 180, 240, 275, 320};

// Centre of swatch `i` (0..kPresetCount-1); out-of-range indices wrap.
inline void swatchCentre(uint8_t i, int& x, int& y)
{
    constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
    const float angleDeg = -90.0f + kSwatchStartDeg +
                           static_cast<float>(i % kPresetCount) *
                               (360.0f / static_cast<float>(kPresetCount));
    const float angleRad = angleDeg * kDegToRad;
    x = static_cast<int>(lroundf(static_cast<float>(kCentreX) +
                                 static_cast<float>(kSwatchRingR) * cosf(angleRad)));
    y = static_cast<int>(lroundf(static_cast<float>(kCentreY) +
                                 static_cast<float>(kSwatchRingR) * sinf(angleRad)));
}

// Index of the swatch whose centre is within kSwatchHitR of (x, y), or -1
// if none. Ties resolve to the lowest index.
inline int8_t hitSwatch(int x, int y)
{
    for (uint8_t i = 0; i < kPresetCount; ++i)
    {
        int cx = 0;
        int cy = 0;
        swatchCentre(i, cx, cy);
        const int dx = x - cx;
        const int dy = y - cy;
        if (dx * dx + dy * dy <= kSwatchHitR * kSwatchHitR)
        {
            return static_cast<int8_t>(i);
        }
    }
    return -1;
}

// Whether (x, y) hits the small power glyph on the on face.
inline bool hitPowerGlyph(int x, int y)
{
    const int dx = x - kPowerGlyphX;
    const int dy = y - kPowerGlyphY;
    return dx * dx + dy * dy <= kPowerGlyphHitR * kPowerGlyphHitR;
}

// The preset whose hue is closest to `hue` going the short way round the
// circle (so 350 is preset 0, not preset 7). `hue` need not be normalised.
inline uint8_t nearestPreset(float hue)
{
    float h = fmodf(hue, 360.0f);
    if (h < 0.0f)
    {
        h += 360.0f;
    }
    uint8_t best = 0;
    float bestDist = 360.0f;
    for (uint8_t i = 0; i < kPresetCount; ++i)
    {
        float d = fabsf(h - kPresetHues[i]);
        if (d > 180.0f)
        {
            d = 360.0f - d;
        }
        if (d < bestDist)
        {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

} // namespace LightLayout
