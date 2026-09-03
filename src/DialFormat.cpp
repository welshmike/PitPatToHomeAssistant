#include "DialFormat.h"

#include <stdio.h>

#include "TreadmillData.h"

namespace DialFormat
{

void formatDuration(uint32_t sec, char* out, size_t n)
{
    if (out == nullptr || n == 0) {
        return;
    }

    uint32_t hours = sec / 3600;
    uint32_t minutes = (sec % 3600) / 60;
    uint32_t seconds = sec % 60;

    if (hours == 0) {
        snprintf(out, n, "%02u:%02u", static_cast<unsigned>(minutes), static_cast<unsigned>(seconds));
    } else {
        snprintf(out, n, "%u:%02u:%02u", static_cast<unsigned>(hours), static_cast<unsigned>(minutes),
                  static_cast<unsigned>(seconds));
    }
}

void formatDistanceKm(float km, char* out, size_t n)
{
    if (out == nullptr || n == 0) {
        return;
    }

    if (km < 0.0f) {
        km = 0.0f;
    }

    snprintf(out, n, "%.2f", static_cast<double>(km));
}

void formatSteps(uint32_t steps, char* out, size_t n)
{
    if (out == nullptr || n == 0) {
        return;
    }

    snprintf(out, n, "%u", static_cast<unsigned>(steps));
}

void formatSpeedMph(float mph, char* out, size_t n)
{
    if (out == nullptr || n == 0) {
        return;
    }

    snprintf(out, n, "%.1f", static_cast<double>(mph));
}

float speedToAngle(float mph)
{
    if (mph <= 0.0f) {
        return 0.0f;
    }
    if (mph >= SPEED_MAX_MPH) {
        return 300.0f;
    }

    return (mph / SPEED_MAX_MPH) * 300.0f;
}

} // namespace DialFormat
