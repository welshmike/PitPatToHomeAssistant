#pragma once
#include "board.h"
#if HAS_DIAL_UI

#include <M5Dial.h>
#include <stdint.h>

// The Dial's icon set (spec 4.12), drawn from LovyanGFX primitives only —
// circles, arcs, lines, rectangles and triangles. No fonts, no bitmaps, no
// anti-aliasing (the 4bpp palette canvas has no alpha to blend into), so every
// stroke is 2-4 px to stay legible at the sizes these are used: r 10 on the
// card menu's ring items, r 8 for the small power glyph on a lit light card,
// r 26 inside the off face's power circle.
//
// Each takes the destination, the glyph centre, a radius that bounds the whole
// glyph, and the already-resolved colour (DialTheme::col(...)), so the same
// call works on the palette canvas and on the direct-to-display fallback.

// Thick line without alpha blending: LovyanGFX's anti-aliased wide-line
// routine reads pixels back through the palette, which faults on a 4-bit
// palette sprite (LoadProhibited in copy_palette_affine, 2026-09-04). Draws
// `widthPx` parallel 1-px lines offset along the perpendicular instead.
// Shared with DialUi's clock hands/ticks.
void dialThickLine(LovyanGFX& gfx, int x0, int y0, int x1, int y1, int widthPx, uint32_t colour);

// IEC power symbol: a 270-degree arc open at the top with a vertical stroke
// through the gap. Lights cards (off face and the small off button).
void drawPowerGlyph(LovyanGFX& gfx, int cx, int cy, int r, uint32_t colour);

// Sun: a filled disc with eight short rays. Card menu, Office light.
void drawSunGlyph(LovyanGFX& gfx, int cx, int cy, int r, uint32_t colour);

// Three overlapping outlined circles — the usual "colour" mark. Drawn on the
// Lamp's Colour page while the light is in temperature mode (that page is not
// live, so nothing on it is painted in true colour).
void drawColourDotsGlyph(LovyanGFX& gfx, int cx, int cy, int r, uint32_t colour);

// Treadmill belt seen from above: a rounded rectangle with a dashed centre
// line. Card menu, Treadmill card.
void drawBeltGlyph(LovyanGFX& gfx, int cx, int cy, int r, uint32_t colour);

// Clock face: a circle with an hour and a minute hand. Card menu, Clock card.
void drawClockGlyph(LovyanGFX& gfx, int cx, int cy, int r, uint32_t colour);

// Aircraft from above: fuselage, wings, tailplane. Card menu, Flights card.
void drawPlaneGlyph(LovyanGFX& gfx, int cx, int cy, int r, uint32_t colour);

// Light bulb: a circle on a small rectangular base. Card menu, Lamp card.
void drawBulbGlyph(LovyanGFX& gfx, int cx, int cy, int r, uint32_t colour);

// Calendar: a page with a thicker date bar across the top and two binder
// rings. Card menu, Calendar card.
void drawCalendarGlyph(LovyanGFX& gfx, int cx, int cy, int r, uint32_t colour);

#endif // HAS_DIAL_UI
