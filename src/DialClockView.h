#pragma once
#include "board.h"
#if HAS_DIAL_UI

#include <M5Dial.h>

#include "DialTheme.h"
#include "TimeService.h"

// The Clock card's analogue face (spec 4.8), split out of DialUi (Plan 8
// Task 3). Free function over the destination, the theme and the live
// TimeService — DialUi holds the reference, this only reads it.
void drawClockCard(LovyanGFX& gfx, const DialTheme& theme, const TimeService& time);

#endif // HAS_DIAL_UI
