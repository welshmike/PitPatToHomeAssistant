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
// the colour hue ring.
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
// convention (up = 0, right = +90). The centre itself reads 180 and is
// never hit-tested (hitHueRing excludes it).
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

// Whether (x, y) hits the small power glyph on the on face.
inline bool hitPowerGlyph(int x, int y)
{
    const int dx = x - kPowerGlyphX;
    const int dy = y - kPowerGlyphY;
    return dx * dx + dy * dy <= kPowerGlyphHitR * kPowerGlyphHitR;
}

// Colour page: the centre disc is a tap target that leaves the page. Hit
// radius a little larger than the disc (kCentreDiscR 30) and well inside the
// ring's touch annulus (kHueHitInnerR 70), so the two can never both claim
// a point.
constexpr int kCentreDiscHitR = 34;
inline bool hitCentreDisc(int x, int y)
{
    const int dx = x - kCentreX;
    const int dy = y - kCentreY;
    return dx * dx + dy * dy <= kCentreDiscHitR * kCentreDiscHitR;
}

} // namespace LightLayout
