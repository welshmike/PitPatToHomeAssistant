#pragma once
#include "board.h"
#if HAS_DIAL_UI && HAS_CALENDAR

#include <M5Dial.h>
#include <stdint.h>
#include <time.h>

#include "CalendarModel.h"
#include "DialTheme.h"

// The Calendar card's face (spec 4.19), a free function over the destination
// and the theme like the other card views (DialFlightsView, DialLightsView).
// Pure drawing: every decision it makes comes from its arguments, so DialUi
// keeps the polling, the nudge and the settings.
//
// `s` is DialUi's own copy of the CalendarService snapshot (pulled on the loop
// task by pollCalendar()); `nowEpoch` is TimeService's UTC epoch, or 0 when
// the clock is not valid yet. `localTime` converts an epoch to local (London)
// broken-down time — DialUi passes a localtime_r wrapper; TimeService's own
// localTime() only ever formats *now*, which is no use for an event's start.
// `fetchedOnce` is CalendarService::fetchedOnce(): false means no fetch has
// completed yet ("waiting for calendar") rather than a failed one.
void drawCalendarCard(LovyanGFX& gfx, const DialTheme& theme, const CalendarModel::Snapshot& s,
                      uint32_t nowEpoch, bool (*localTime)(uint32_t epoch, struct tm& out),
                      bool fetchedOnce);

#endif // HAS_DIAL_UI && HAS_CALENDAR
