#pragma once
#include "board.h"
#if HAS_DIAL_UI

#include <M5Dial.h>

#include "CardMenu.h"
#include "DialTheme.h"

// The side button's card menu (spec 4.12), drawn from CardMenu's own geometry
// so what is painted and what CardMenu::hitTest() accepts cannot drift apart.
// Free function over the destination and the theme: no DialUi dependency.
//
// Five glyphs on a ring of radius 88 (Treadmill at 12 o'clock, then clockwise),
// the highlighted one filled amber at radius 22 with its glyph knocked out in
// the background colour, the rest DIM outlines at radius 18. The highlighted
// card's name sits in the middle in Font4, with the `turn - tap` hint beneath.
void drawCardMenu(LovyanGFX& gfx, const DialTheme& theme, const CardMenu& menu);

#endif // HAS_DIAL_UI
