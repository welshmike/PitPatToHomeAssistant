#include "TimeMath.h"

namespace TimeMath
{

int64_t utcToEpoch(int year, int month, int day, int hour, int min, int sec)
{
    int64_t y = year;
    const int m = month;
    const int d = day;
    y -= (m <= 2) ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const uint32_t yoe = static_cast<uint32_t>(y - era * 400);           // [0, 399]
    const uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; // [0, 365]
    const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;          // [0, 146096]
    const int64_t days = era * 146097 + static_cast<int64_t>(doe) - 719468; // since 1970-01-01

    return days * 86400 + hour * 3600 + min * 60 + sec;
}

} // namespace TimeMath
