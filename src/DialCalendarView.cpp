#include "DialCalendarView.h"
#if HAS_DIAL_UI && HAS_CALENDAR

#include <stdio.h>
#include <string.h>

namespace
{

constexpr int32_t kCentreX = 120;
constexpr int32_t kCentreY = 120;

// Card name, top of the face. The Lights cards draw theirs at y 34, but this
// card also has the spec's all-day row at y 40: two Font2 lines (16 px tall,
// middle_center) 6 px apart would overlap. The spec pins the all-day row to a
// number and leaves the name at "the usual top position", so the name is what
// moves — up to y 20, still clear of the round bezel (the chord at y 20 is
// 132 px wide, and "Calendar" in Font2 is ~60 px).
constexpr int32_t kNameY   = 20;
// Today's all-day events, collapsed into one line (spec 4.19).
constexpr int32_t kAllDayY = 40;
// The next timed event: start time, then the wrapped title, then the countdown.
constexpr int32_t kStartY  = 62;
constexpr int32_t kTitleY  = 108; // one line
constexpr int32_t kTitleY1 = 96;  // two lines: first
constexpr int32_t kTitleY2 = 120; // two lines: second
constexpr int32_t kCountY  = 150;
// The three following events, one Font2 line each.
constexpr int32_t kFollowY[3] = {176, 192, 208};
constexpr uint8_t kFollowMax  = 3;

// Title wrap: at most two lines of at most this many characters.
constexpr size_t kWrapCols = 16;
// Following-event lines are clipped to this many characters of title.
constexpr size_t kFollowTitleMax = 20;

// The epoch -> local time converter for this draw call. drawCalendarCard()
// is handed one as an argument (DialUi owns the localtime_r wrapper), but
// CalendarModel::firstOnLaterDay() takes a plain function pointer that cannot
// carry it, so it is parked here for localDayOf() below to reach. Loop task
// only, one draw at a time — no re-entrancy to worry about.
bool (*s_localTime)(uint32_t, struct tm&) = nullptr;

// Day number for firstOnLaterDay(): monotonic across a year boundary
// (tm_year * 400 + tm_yday), which is all that helper compares.
uint32_t localDayOf(uint32_t epoch)
{
    struct tm t;
    if (s_localTime == nullptr || !s_localTime(epoch, t))
    {
        return 0;
    }
    return static_cast<uint32_t>(t.tm_year) * 400u + static_cast<uint32_t>(t.tm_yday);
}

void hhmm(uint32_t epoch, char* buf, size_t cap)
{
    struct tm t;
    if (s_localTime != nullptr && s_localTime(epoch, t))
    {
        snprintf(buf, cap, "%02d:%02d", t.tm_hour, t.tm_min);
    }
    else
    {
        snprintf(buf, cap, "--:--");
    }
}

struct TitleLines
{
    char l1[kWrapCols + 1] = {0};
    char l2[kWrapCols + 1] = {0};
    bool two = false;
};

void trimTrailingSpaces(char* s)
{
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == ' ')
    {
        s[--n] = '\0';
    }
}

// Wraps `title` into at most two lines of at most kWrapCols characters,
// breaking at the last space that fits and hard-breaking a single word longer
// than a line. Anything that still does not fit is dropped and the second line
// ends in ".." (Font2/Font4 here have no single-glyph ellipsis). Fixed
// buffers, no heap.
void wrapTitle(const char* title, TitleLines& out)
{
    if (title == nullptr)
    {
        return;
    }
    while (*title == ' ')
    {
        ++title;
    }
    const size_t len = strlen(title);
    if (len <= kWrapCols)
    {
        memcpy(out.l1, title, len);
        out.l1[len] = '\0';
        trimTrailingSpaces(out.l1);
        return;
    }

    // len > kWrapCols, so title[kWrapCols] is a real character: a space there
    // means the first kWrapCols characters are a whole word sequence.
    size_t brk = 0;
    for (size_t i = 1; i <= kWrapCols; ++i)
    {
        if (title[i] == ' ')
        {
            brk = i;
        }
    }
    const size_t take   = (brk > 0) ? brk : kWrapCols;
    size_t       restAt = (brk > 0) ? brk + 1 : kWrapCols;
    memcpy(out.l1, title, take);
    out.l1[take] = '\0';
    trimTrailingSpaces(out.l1);

    while (title[restAt] == ' ')
    {
        ++restAt;
    }
    const char*  rest = title + restAt;
    const size_t rlen = strlen(rest);
    out.two = (rlen > 0);
    if (!out.two)
    {
        return;
    }
    if (rlen <= kWrapCols)
    {
        memcpy(out.l2, rest, rlen);
        out.l2[rlen] = '\0';
        trimTrailingSpaces(out.l2);
        return;
    }
    // More than two lines' worth: keep room for the ".." that says so.
    memcpy(out.l2, rest, kWrapCols - 2);
    out.l2[kWrapCols - 2] = '\0';
    trimTrailingSpaces(out.l2);
    const size_t n = strlen(out.l2);
    out.l2[n]     = '.';
    out.l2[n + 1] = '.';
    out.l2[n + 2] = '\0';
}

// "All day: Standup" for a single all-day event, "All day: 2 events" for more.
void drawAllDayLine(LovyanGFX& gfx, const DialTheme& theme, const CalendarModel::Snapshot& s,
                    uint8_t count)
{
    char buf[40];
    if (count == 1)
    {
        const char* title = "";
        for (uint8_t i = 0; i < s.count; ++i)
        {
            if (s.ev[i].allDay)
            {
                title = s.ev[i].title;
                break;
            }
        }
        snprintf(buf, sizeof(buf), "All day: %.20s", title);
    }
    else
    {
        snprintf(buf, sizeof(buf), "All day: %u events", (unsigned)count);
    }
    gfx.setTextColor(theme.col(Col::DIM), theme.col(Col::BG));
    gfx.drawString(buf, kCentreX, kAllDayY, &fonts::Font2);
}

} // namespace

void drawCalendarCard(LovyanGFX& gfx, const DialTheme& theme, const CalendarModel::Snapshot& s,
                      uint32_t nowEpoch, bool (*localTime)(uint32_t epoch, struct tm& out),
                      bool fetchedOnce)
{
    s_localTime = localTime;
    gfx.setTextDatum(middle_center);

    gfx.setTextColor(theme.col(Col::TEXT), theme.col(Col::BG));
    gfx.drawString("Calendar", kCentreX, kNameY, &fonts::Font2);

    // Before the first fetch has landed there is nothing to say about the
    // calendar itself — this is boot, not an outage (spec 4.19).
    if (!fetchedOnce)
    {
        gfx.setTextColor(theme.col(Col::DIM), theme.col(Col::BG));
        gfx.drawString("waiting for calendar", kCentreX, kCentreY, &fonts::Font4);
        return;
    }

    // No clock yet is as unusable as a stale snapshot: every line on the face
    // below is relative to now. isStale() itself would say so anyway (its
    // unsigned age underflows against nowEpoch 0), but say it deliberately.
    if (nowEpoch == 0 || CalendarModel::isStale(s, nowEpoch))
    {
        gfx.setTextColor(theme.col(Col::DIM), theme.col(Col::BG));
        gfx.drawString("no calendar", kCentreX, kCentreY, &fonts::Font4);
        if (s.valid && nowEpoch > s.fetchedAtEpoch)
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "last update %u min ago",
                     (unsigned)((nowEpoch - s.fetchedAtEpoch) / 60));
            gfx.drawString(buf, kCentreX, kCountY, &fonts::Font2);
        }
        return;
    }

    const uint8_t allDay = CalendarModel::allDayCount(s);
    if (allDay > 0)
    {
        drawAllDayLine(gfx, theme, s, allDay);
    }

    const int8_t idx = CalendarModel::nextTimed(s, nowEpoch);
    if (idx < 0)
    {
        // Nothing timed left today; tomorrow's first event, if the payload
        // reached that far, goes underneath (spec 4.19).
        gfx.setTextColor(theme.col(Col::DIM), theme.col(Col::BG));
        gfx.drawString("nothing more today", kCentreX, kCentreY, &fonts::Font4);
        const int8_t j = CalendarModel::firstOnLaterDay(s, nowEpoch, &localDayOf);
        if (j >= 0)
        {
            char when[8];
            hhmm(s.ev[j].start, when, sizeof(when));
            char buf[48];
            snprintf(buf, sizeof(buf), "Tomorrow %s %.*s", when, (int)kFollowTitleMax,
                     s.ev[j].title);
            gfx.drawString(buf, kCentreX, kCountY, &fonts::Font2);
        }
        return;
    }

    const CalendarModel::Event& next = s.ev[idx];

    char startBuf[8];
    hhmm(next.start, startBuf, sizeof(startBuf));
    gfx.setTextColor(theme.col(Col::TEXT), theme.col(Col::BG));
    gfx.drawString(startBuf, kCentreX, kStartY, &fonts::Font4);

    TitleLines lines;
    wrapTitle(next.title, lines);
    if (lines.two)
    {
        gfx.drawString(lines.l1, kCentreX, kTitleY1, &fonts::Font4);
        gfx.drawString(lines.l2, kCentreX, kTitleY2, &fonts::Font4);
    }
    else
    {
        gfx.drawString(lines.l1, kCentreX, kTitleY, &fonts::Font4);
    }

    // Amber while the meeting is under way, plain text while it is still
    // ahead (spec 4.19). countdownText() returns 0 only for an event that has
    // ended, which nextTimed() has already ruled out.
    char countBuf[24];
    if (CalendarModel::countdownText(next, nowEpoch, countBuf, sizeof(countBuf)) > 0)
    {
        gfx.setTextColor(theme.col(nowEpoch >= next.start ? Col::PENDING : Col::TEXT),
                         theme.col(Col::BG));
        gfx.drawString(countBuf, kCentreX, kCountY, &fonts::Font2);
    }

    // Up to three following timed events, "14:30 Title", dim. All-day events
    // are already summarised on their own line above.
    gfx.setTextColor(theme.col(Col::DIM), theme.col(Col::BG));
    uint8_t row = 0;
    for (uint8_t i = static_cast<uint8_t>(idx) + 1; i < s.count && row < kFollowMax; ++i)
    {
        if (s.ev[i].allDay)
        {
            continue;
        }
        char when[8];
        hhmm(s.ev[i].start, when, sizeof(when));
        char buf[40];
        snprintf(buf, sizeof(buf), "%s %.*s", when, (int)kFollowTitleMax, s.ev[i].title);
        gfx.drawString(buf, kCentreX, kFollowY[row], &fonts::Font2);
        ++row;
    }
}

#endif // HAS_DIAL_UI && HAS_CALENDAR
