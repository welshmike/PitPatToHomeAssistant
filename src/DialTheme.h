#pragma once
#include <stdint.h>

// The Dial UI's colour vocabulary, split out of DialUi (Plan 8 Task 3) so the
// card-menu, glyph and lights views can draw in the same colours without
// depending on DialUi itself. Pure: no Arduino, no M5, no display.
//
// Every colour the UI draws is one of at most 16 palette entries — the render
// canvas is a 4-bit (16-colour) palette sprite (240x240, ~28.8 KB; 2026-09-04:
// down from an 8bpp/RGB332 canvas to save another ~28.8 KB of heap for WiFi).
enum class Col : uint8_t
{
    BG = 0,     // screen background
    TEXT,       // primary text/foreground (also clock hands, ring track lit state's text)
    DIM,        // secondary/caption text, clock ticks, unlit ring track
    BLE_ON,     // BLE status dot when connected
    NET_ON,     // WiFi/MQTT status dots when up
    SPEED,      // speed ring/centre value while not pending (cyan)
    PENDING,    // speed ring/centre value while a nudge is settling, the selector,
                // the menu highlight and a settling light edit (amber)
    RED,        // long-press-to-stop progress arc
    SECOND,     // clock second hand — a distinct red shade from RED
    DIM_DIM,    // paused-dim shade of DIM (also the paused-dim shade of
                // TEXT: dimColor565(TFT_WHITE) == TFT_DARKGREY exactly,
                // so TEXT's paused shade is DIM itself, not DIM_DIM)
    SPEED_DIM,  // paused-dim shade of SPEED
    PENDING_DIM,// paused-dim shade of PENDING
    WARM,       // Lights Kelvin bar, warm end (spec 4.12)
    NEUTRAL,    // Lights Kelvin bar, middle
    COOL,       // Lights Kelvin bar, cool end
    TRANSPARENT = 15, // canvas-only: skipped by pushSprite(transp) so the display keeps
                      // what is under it (the airline logo, the hue ring, marker and centre disc)
};

// Palette entries actually registered from the table below: 0..14. Index 15
// (TRANSPARENT) carries no colour of its own — begin() registers it as black
// and the push treats it as "leave the display alone".
constexpr uint8_t kDialPaletteSize = 15;

// Palette source values (2026-09-04): the RGB888 colours behind Col, in enum
// order — index i is the palette entry for Col value i. Entries 0..11 were the
// original RGB565 constants used before the canvas became a 4bpp palette
// sprite, expanded 565->888 the same way the display itself would (top bits
// replicated into the low bits), so the on-screen colour is unchanged:
//   BG          TFT_BLACK    0x0000 -> (  0,   0,   0)
//   TEXT        TFT_WHITE    0xFFFF -> (255, 255, 255)
//   DIM         TFT_DARKGREY 0x7BEF -> (123, 125, 123) -- also TFT_darkgrey's
//               365 "TICK" use, and dimColor565(TFT_WHITE) == TFT_DARKGREY
//               exactly (both decode to r=15,g=31,b=15 in 565 components),
//               so this same entry doubles as TEXT's paused-dim shade.
//   BLE_ON      TFT_BLUE     0x001F -> (  0,   0, 255)
//   NET_ON      TFT_GREEN    0x07E0 -> (  0, 255,   0)
//   SPEED       TFT_CYAN     0x07FF -> (  0, 255, 255)
//   PENDING     TFT_ORANGE   0xFDA0 -> (255, 182,   0)
//   RED         TFT_RED      0xF800 -> (255,   0,   0)
//   SECOND      color565(220,40,40) quantised to 565 then back -> (222,40,41)
//   DIM_DIM     dimColor565(TFT_DARKGREY)                      -> ( 57,  60,  57)
//   SPEED_DIM   dimColor565(TFT_CYAN)                          -> (  0, 125, 123)
//   PENDING_DIM dimColor565(TFT_ORANGE)                        -> (123,  89,   0)
// dimColor565() (the halving of each 565 channel used for the paused-state
// dim, formerly a runtime helper) is folded into these constants instead —
// there is nothing left to dim at draw time once the palette holds the
// dimmed shades directly. Entries 12..14 are the Kelvin bar's three steps
// (spec 4.12: warm #FFB46B, neutral #FFF1D6, cool #CFE3FF), added in the
// slots the 12-colour palette left free.
inline uint32_t dialPaletteRgb888(uint8_t idx)
{
    static const uint32_t kPalette[kDialPaletteSize] = {
        0x000000, // BG
        0xFFFFFF, // TEXT
        0x7B7D7B, // DIM
        0x0000FF, // BLE_ON
        0x00FF00, // NET_ON
        0x00FFFF, // SPEED
        0xFFB600, // PENDING
        0xFF0000, // RED
        0xDE2829, // SECOND
        0x393C39, // DIM_DIM
        0x007D7B, // SPEED_DIM
        0x7B5900, // PENDING_DIM
        0xFFB46B, // WARM
        0xFFF1D6, // NEUTRAL
        0xCFE3FF, // COOL
    };
    return (idx < kDialPaletteSize) ? kPalette[idx] : 0u;
}

// How the UI turns a Col into the value a LovyanGFX draw call wants. The one
// piece of state is which destination is being drawn to.
//
// For a 4bpp palette destination (useCanvas true) every draw call wants the
// raw palette index, not an RGB value:
//  - fillCircle/fillArc/drawCircle/drawString/fillScreen/... all route their
//    colour argument through LGFXBase::setColor() -> _write_conv.convert(T)
//    (lgfx/v1/LGFXBase.hpp, the `setColor`/`LGFX_INLINE_T` draw-call
//    forwarders). For T=uint32_t, color_conv_t::convert() calls
//    convert_rgb888(), a function pointer selected by
//    get_fp_convert_src<rgb888_t>(dst_depth) (lgfx/v1/misc/colortype.hpp,
//    get_fp_convert_src): a 4-bit *palette* dst_depth matches none of that
//    function's named pixel formats, so it falls through to
//    `switch (dst_depth & bit_mask) { case 4: return
//    convert_uint32_to_palette4; ... }`, and
//    `convert_uint32_to_palette4(uint32_t c) { return (c & 0x0F) * 0x11; }`
//    (colortype.hpp, near the top) takes the low nibble of whatever integer
//    was passed as the raw index — not an RGB value.
//  - setTextColor() makes this explicit rather than routing through convert():
//    `_text_style.fore_rgb888 = _text_style.back_rgb888 = this->hasPalette()
//    ? color : convert_to_rgb888(color);` (LGFXBase.hpp, setTextColor).
//  - drawWideLine()/drawWedgeLine() (used for the clock hands/ticks) instead
//    call `convert_to_rgb888(color)` and hand that to draw_wedgeline(), which
//    builds a one-pixel rgb888_t gradient and pushes it through the same
//    paletted pixelcopy path (misc/pixelcopy.cpp, the `dst_palette_` branch
//    of pixelcopy_t's constructor uses `copy_bit_affine`, which reads the
//    *first* in-memory byte of that rgb888_t). rgb888_t's field order is
//    `{ uint8_t b; uint8_t g; uint8_t r; }` (colortype.hpp, struct rgb888_t)
//    — for a small integer built via `rgb888_t(uint32_t)`, that first byte
//    is the low byte of the value that was passed, i.e. the same raw index
//    survives this longer path too.
// createPalette()/setPaletteColor() (LGFX_Sprite.hpp), by contrast, take a
// real RGB888/RGB565 value and store it in the palette table — that's why
// DialUi::begin() passes dialPaletteRgb888(i) there rather than the index.
// Net effect: every draw call can be handed col(Col::X) as its colour
// argument, uniformly, on either destination.
struct DialTheme
{
    // Set once by DialUi::begin(): true while drawing into the 4bpp canvas,
    // false for the direct-to-M5Dial.Display fallback used when the sprite
    // allocation fails.
    bool useCanvas = false;

    uint32_t col(Col c) const
    {
        const uint8_t idx = static_cast<uint8_t>(c);
        return useCanvas ? idx : dialPaletteRgb888(idx);
    }
};
