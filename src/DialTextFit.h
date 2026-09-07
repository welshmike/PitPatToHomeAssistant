#pragma once
#include "board.h"
#if HAS_DIAL_UI

#include <M5Dial.h>
#include <math.h>
#include <string.h>

// Round-face text-fit helpers, shared by every card whose lines must stay
// inside the 240x240 circular display: the Calendar card (spec 4.19) and the
// Clock card's meeting title/time lines (spec 4.19 amendment, Plan 19). Moved
// verbatim out of DialCalendarView.cpp so there is one definition instead of two.

namespace
{

constexpr int32_t kDialFaceCy = 120;
constexpr int32_t kDialFaceR  = 120;

// Visible width of the round face at row y, less a 6 px bezel margin per
// side. The face is a 120 px-radius circle centred on (120, 120); at row y
// the chord half-width is sqrt(r^2 - dy^2).
inline int32_t rowWidth(int32_t y)
{
    const float dy = static_cast<float>(y - kDialFaceCy);
    return static_cast<int32_t>(2.0f * sqrtf(kDialFaceR * kDialFaceR - dy * dy)) - 12;
}

// Trims `s` in place, one character at a time off the end, until it fits
// rowWidth(y) in `font`. Appends nothing (the wrap/clip upstream already
// decided how much text to show; this is only the last-resort bezel guard).
inline void fitToRow(LovyanGFX& gfx, char* s, int32_t y, const lgfx::IFont* font)
{
    size_t n = strlen(s);
    while (n > 0 && gfx.textWidth(s, font) > rowWidth(y)) { s[--n] = '\0'; }
}

} // namespace

#endif // HAS_DIAL_UI
