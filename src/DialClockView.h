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
// `title`/`when` are an optional pre-built title and "HH:MM" time for
// today's next meeting (spec 4.19 "clock layout" amendment,
// CalendarModel::clockLine()), drawn below the centre when non-null and
// non-empty: title on its own line, the time beneath it. `live` selects
// amber (PENDING) for the time once that meeting is under way, dim (DIM)
// otherwise; the title is always DIM. A build without HAS_CALENDAR (or a
// Clock draw with nothing to show) simply omits both arguments.
void drawClockCard(LovyanGFX& gfx, const DialTheme& theme, const TimeService& time,
                   const char* title = nullptr, const char* when = nullptr, bool live = false);

#endif // HAS_DIAL_UI
