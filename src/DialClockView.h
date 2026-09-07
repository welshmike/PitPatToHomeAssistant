#pragma once
#include "board.h"
#if HAS_DIAL_UI

#include <M5Dial.h>

#include "DialTheme.h"
#include "TimeService.h"

// The Clock card's analogue face (spec 4.8), split out of DialUi (Plan 8
// Task 3). Free function over the destination, the theme and the live
// TimeService — DialUi holds the reference, this only reads it.
//
// `subline` is an optional pre-built "HH:MM Title" line for today's next
// meeting (spec 4.19 amendment, CalendarModel::clockLine()), drawn under the
// date when non-null and non-empty; `live` selects amber (PENDING) once that
// meeting is under way, dim (DIM) otherwise. A build without HAS_CALENDAR (or
// a Clock draw with nothing to show) simply omits both arguments.
void drawClockCard(LovyanGFX& gfx, const DialTheme& theme, const TimeService& time,
                   const char* subline = nullptr, bool live = false);

#endif // HAS_DIAL_UI
