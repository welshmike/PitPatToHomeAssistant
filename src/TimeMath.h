#pragma once

#include <stdint.h>

// Arduino-free UTC calendar math, buildable and testable on the host (native
// env). No dependency on <time.h>'s timegm()/mktime() (Arduino-ESP32's
// newlib lacks the former, and the latter applies the process TZ) — see
// TimeService.h for why the RTC/epoch conversion needs to stay TZ-agnostic.
namespace TimeMath
{

// Converts a UTC calendar date/time to a UTC epoch (seconds since
// 1970-01-01T00:00:00Z), via Howard Hinnant's days_from_civil algorithm.
// month is 1-12. Exact for the full range int64_t/int can represent;
// no validation of out-of-range fields (e.g. day 31 in April) is performed.
int64_t utcToEpoch(int year, int month, int day, int hour, int min, int sec);

} // namespace TimeMath
