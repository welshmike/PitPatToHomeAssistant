#include "DialClockView.h"
#if HAS_DIAL_UI

#include <string.h>
#include <time.h>

#include "ClockFace.h"
#include "DialGlyphs.h"
#include "DialTextFit.h"

namespace
{

constexpr int32_t kCentreX = 120;
constexpr int32_t kRingCx  = 120;
constexpr int32_t kRingCy  = 120;

// Clock card layout (spec 4.8), centred on the same 240x240 canvas as the
// speed ring.
constexpr int32_t kClockR0        = 104; // tick inner radius
constexpr int32_t kClockR1        = 112; // tick outer radius (r0=100 at 12/3/6/9, longer ticks)
constexpr int32_t kClockR0Long    = 100;
constexpr int32_t kHourHandLen    = 56;
constexpr int32_t kMinuteHandLen  = 82;
constexpr int32_t kSecondHandLen  = 96;
constexpr float    kHourHandW      = 5.0f;
constexpr float    kMinuteHandW    = 3.0f;
constexpr float    kSecondHandW    = 1.0f;
constexpr int32_t kClockCentreDotR = 4;
constexpr int32_t kClockSecondDotR = 2;
constexpr int32_t kClockDateY       = 72;  // date when valid, "waiting for time" hint when not
                                            // (spec 4.19 "clock layout" amendment: moved above
                                            // the centre to make room for the meeting lines below)
constexpr int32_t kClockEventTitleY = 168; // today's next meeting: title
constexpr int32_t kClockEventTimeY  = 186; // today's next meeting: time "HH:MM"

} // namespace

// Clock card (spec 4.8): analogue face — 12 tick marks, hour/minute/second
// hands from TimeService's wall clock (NTP over WiFi, backed by the Dial's
// RTC so it reads correctly before WiFi comes up), small date above the
// centre (spec 4.19 "clock layout" amendment). DialUi redraws it once a
// second via the clockSec field in its FrameKey. When TimeService isn't
// valid yet (fresh device, no WiFi, empty RTC) draws the ticks with no hands
// and "--:--" instead of a time.
void drawClockCard(LovyanGFX& gfx, const DialTheme& theme, const TimeService& time,
                   const char* title, const char* when, bool live)
{
    // 12 tick marks; the four cardinal ones (12/3/6/9) run from a slightly
    // larger radius so they read as longer/bolder without a second draw call.
    for (int i = 0; i < 12; ++i)
    {
        const bool cardinal = (i % 3) == 0;
        const HandLine t = ClockFace::tick(i, kRingCx, kRingCy,
                                            cardinal ? kClockR0Long : kClockR0,
                                            kClockR1);
        dialThickLine(gfx, t.x0, t.y0, t.x1, t.y1, cardinal ? 3 : 2, theme.col(Col::DIM));
    }

    struct tm t;
    if (!time.localTime(t))
    {
        gfx.setTextColor(theme.col(Col::TEXT), theme.col(Col::BG));
        gfx.drawString("--:--", kRingCx, kRingCy, &fonts::Font4);
        gfx.setTextColor(theme.col(Col::DIM), theme.col(Col::BG));
        gfx.drawString("waiting for time", kRingCx, kClockDateY, &fonts::Font2);
        return;
    }

    const HandLine hh = ClockFace::hand(ClockFace::hourAngle(t.tm_hour, t.tm_min),
                                         kRingCx, kRingCy, kHourHandLen);
    const HandLine mm = ClockFace::hand(ClockFace::minuteAngle(t.tm_min, t.tm_sec),
                                         kRingCx, kRingCy, kMinuteHandLen);
    const HandLine ss = ClockFace::hand(ClockFace::secondAngle(t.tm_sec),
                                         kRingCx, kRingCy, kSecondHandLen);

    dialThickLine(gfx, hh.x0, hh.y0, hh.x1, hh.y1, kHourHandW, theme.col(Col::TEXT));
    dialThickLine(gfx, mm.x0, mm.y0, mm.x1, mm.y1, kMinuteHandW, theme.col(Col::TEXT));
    dialThickLine(gfx, ss.x0, ss.y0, ss.x1, ss.y1, kSecondHandW, theme.col(Col::SECOND));

    gfx.fillCircle(kRingCx, kRingCy, kClockCentreDotR, theme.col(Col::TEXT));
    gfx.fillCircle(kRingCx, kRingCy, kClockSecondDotR, theme.col(Col::SECOND));

    // "Mon 3 Sep" — %-e (no leading zero/space) isn't universally supported
    // by newlib's strftime, so use %e (space-padded to width 2) instead. On
    // a single-digit day that leaves "Mon  3 Sep" — the literal space in the
    // format plus %e's own pad space — so collapse that one double space.
    char dateBuf[16];
    strftime(dateBuf, sizeof(dateBuf), "%a %e %b", &t);
    for (char* p = dateBuf; *p != '\0'; ++p)
    {
        if (p[0] == ' ' && p[1] == ' ')
        {
            memmove(p, p + 1, strlen(p + 1) + 1);
            break;
        }
    }
    gfx.setTextColor(theme.col(Col::DIM), theme.col(Col::BG));
    gfx.drawString(dateBuf, kCentreX, kClockDateY, &fonts::Font2);

    // Today's next meeting (spec 4.19 "clock layout" amendment): two Font2
    // lines below the centre — the title, then its time. The title is always
    // DIM; the time is amber (PENDING) once the meeting is under way, dim
    // (DIM) otherwise. Copied into fixed stack buffers (no heap) so
    // fitToRow() can trim each in place without mutating the caller's
    // string.
    if (title != nullptr && title[0] != '\0')
    {
        char titleBuf[48];
        strncpy(titleBuf, title, sizeof(titleBuf) - 1);
        titleBuf[sizeof(titleBuf) - 1] = '\0';
        fitToRow(gfx, titleBuf, kClockEventTitleY, &fonts::Font2);
        gfx.setTextColor(theme.col(Col::DIM), theme.col(Col::BG));
        gfx.drawString(titleBuf, kCentreX, kClockEventTitleY, &fonts::Font2);

        if (when != nullptr && when[0] != '\0')
        {
            char whenBuf[16];
            strncpy(whenBuf, when, sizeof(whenBuf) - 1);
            whenBuf[sizeof(whenBuf) - 1] = '\0';
            fitToRow(gfx, whenBuf, kClockEventTimeY, &fonts::Font2);
            gfx.setTextColor(theme.col(live ? Col::PENDING : Col::DIM), theme.col(Col::BG));
            gfx.drawString(whenBuf, kCentreX, kClockEventTimeY, &fonts::Font2);
        }
    }
}

#endif // HAS_DIAL_UI
