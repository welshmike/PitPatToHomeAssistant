#include "DialGlyphs.h"
#if HAS_DIAL_UI

#include <math.h>

namespace
{

constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;

int scaled(int r, float f)
{
    const int v = static_cast<int>(lroundf(static_cast<float>(r) * f));
    return v;
}

// Every stroke is at least 2 px: a 1-px line disappears against the black
// background on the Dial's round LCD at arm's length.
int strokeWidth(int r, float f)
{
    const int w = scaled(r, f);
    return w < 2 ? 2 : w;
}

void ringOutline(LovyanGFX& gfx, int cx, int cy, int r, int widthPx, uint32_t colour)
{
    for (int i = 0; i < widthPx; ++i)
    {
        gfx.drawCircle(cx, cy, r - i, colour);
    }
}

} // namespace

void dialThickLine(LovyanGFX& gfx, int x0, int y0, int x1, int y1, int widthPx, uint32_t colour)
{
    if (widthPx <= 1) { gfx.drawLine(x0, y0, x1, y1, colour); return; }
    const float dx = (float)(x1 - x0), dy = (float)(y1 - y0);
    const float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.5f) { gfx.fillCircle(x0, y0, widthPx / 2, colour); return; }
    const float px = -dy / len, py = dx / len; // unit perpendicular
    const float half = (widthPx - 1) / 2.0f;
    for (int i = 0; i < widthPx; ++i)
    {
        const float o = -half + i;
        gfx.drawLine((int)lroundf(x0 + px * o), (int)lroundf(y0 + py * o),
                     (int)lroundf(x1 + px * o), (int)lroundf(y1 + py * o), colour);
    }
}

void drawPowerGlyph(LovyanGFX& gfx, int cx, int cy, int r, uint32_t colour)
{
    const int w = strokeWidth(r, 0.22f);
    // fillArc's angles are degrees clockwise from 3 o'clock, so 270 is the top:
    // 315 -> 585 (== 225 the long way round) leaves a 90-degree gap centred
    // there for the vertical stroke to sit in.
    gfx.fillArc(cx, cy, r, r - w, 315.0f, 585.0f, colour);
    dialThickLine(gfx, cx, cy - r - w / 2, cx, cy - scaled(r, 0.05f), w, colour);
}

void drawSunGlyph(LovyanGFX& gfx, int cx, int cy, int r, uint32_t colour)
{
    gfx.fillCircle(cx, cy, scaled(r, 0.42f), colour);
    const float inner = static_cast<float>(r) * 0.66f;
    const float outer = static_cast<float>(r);
    for (int i = 0; i < 8; ++i)
    {
        const float a = static_cast<float>(i) * 45.0f * kDegToRad;
        const float ca = cosf(a), sa = sinf(a);
        dialThickLine(gfx, cx + (int)lroundf(inner * ca), cy + (int)lroundf(inner * sa),
                      cx + (int)lroundf(outer * ca), cy + (int)lroundf(outer * sa), 2, colour);
    }
}

void drawColourDotsGlyph(LovyanGFX& gfx, int cx, int cy, int r, uint32_t colour)
{
    // Three equal circles on a small ring, so they overlap in the middle.
    const int dotR = scaled(r, 0.52f);
    const float ring = static_cast<float>(r) * 0.44f;
    for (int i = 0; i < 3; ++i)
    {
        const float a = (-90.0f + static_cast<float>(i) * 120.0f) * kDegToRad;
        ringOutline(gfx, cx + (int)lroundf(ring * cosf(a)), cy + (int)lroundf(ring * sinf(a)),
                    dotR, 2, colour);
    }
}

void drawBeltGlyph(LovyanGFX& gfx, int cx, int cy, int r, uint32_t colour)
{
    const int halfW = scaled(r, 0.62f);
    const int halfH = static_cast<int>(r);
    const int radius = scaled(r, 0.32f);
    // Deck outline (2 px), portrait — the belt seen from above, as walked on.
    gfx.drawRoundRect(cx - halfW, cy - halfH, halfW * 2, halfH * 2, radius, colour);
    gfx.drawRoundRect(cx - halfW + 1, cy - halfH + 1, halfW * 2 - 2, halfH * 2 - 2,
                      radius - 1, colour);
    // Dashed centre line down the deck.
    const int dash = scaled(r, 0.22f) < 2 ? 2 : scaled(r, 0.22f);
    for (int y = cy - halfH + dash; y + dash <= cy + halfH - dash; y += dash * 2)
    {
        gfx.fillRect(cx - 1, y, 2, dash, colour);
    }
}

void drawClockGlyph(LovyanGFX& gfx, int cx, int cy, int r, uint32_t colour)
{
    ringOutline(gfx, cx, cy, r, 2, colour);
    // Ten past ten, the way a clock is drawn in every catalogue.
    dialThickLine(gfx, cx, cy, cx + scaled(r, 0.34f), cy - scaled(r, 0.34f), 2, colour);
    dialThickLine(gfx, cx, cy, cx - scaled(r, 0.30f), cy - scaled(r, 0.55f), 2, colour);
}

void drawPlaneGlyph(LovyanGFX& gfx, int cx, int cy, int r, uint32_t colour)
{
    // Nose up: fuselage, then the wings and tailplane across it.
    gfx.fillTriangle(cx, cy - r,
                     cx - scaled(r, 0.20f), cy + scaled(r, 0.75f),
                     cx + scaled(r, 0.20f), cy + scaled(r, 0.75f), colour);
    gfx.fillTriangle(cx - r, cy + scaled(r, 0.35f),
                     cx + r, cy + scaled(r, 0.35f),
                     cx, cy - scaled(r, 0.20f), colour);
    gfx.fillTriangle(cx - scaled(r, 0.42f), cy + r,
                     cx + scaled(r, 0.42f), cy + r,
                     cx, cy + scaled(r, 0.45f), colour);
}

void drawBulbGlyph(LovyanGFX& gfx, int cx, int cy, int r, uint32_t colour)
{
    const int bulbR = scaled(r, 0.60f);
    const int bulbY = cy - scaled(r, 0.25f);
    ringOutline(gfx, cx, bulbY, bulbR, 2, colour);
    // Screw base under it, two bars so it reads as a fitting rather than a box.
    const int baseW = scaled(r, 0.56f);
    const int baseY = bulbY + bulbR + 1;
    gfx.fillRect(cx - baseW / 2, baseY, baseW, 2, colour);
    gfx.fillRect(cx - baseW / 2, baseY + 3, baseW, 2, colour);
}

#endif // HAS_DIAL_UI
