#pragma once
#include <stddef.h>
#include <stdint.h>

// Pure, Arduino-free display formatting helpers for the M5Dial UI. No
// dependency on Arduino/M5 headers so this builds and is tested on the host.

namespace DialFormat
{
// Formats elapsed seconds as "mm:ss" below one hour (minutes zero-padded to
// two digits), or "h:mm:ss" from 3600s onward (hours not zero-padded).
// Always NUL-terminates within the first n bytes of out; truncates safely
// via snprintf if the buffer is too small.
void formatDuration(uint32_t sec, char* out, size_t n);

// Formats a distance in kilometres to two decimal places, e.g. "1.23".
// Negative input clamps to "0.00".
void formatDistanceKm(float km, char* out, size_t n);

// Formats a step count as a plain integer with no thousands separators.
void formatSteps(uint32_t steps, char* out, size_t n);

// Formats a speed in mph to one decimal place, e.g. "2.3".
void formatSpeedMph(float mph, char* out, size_t n);

// Maps a speed in mph linearly onto a 0..300 degree arc over
// [0, SPEED_MAX_MPH] (see TreadmillData.h). Clamps below 0 to 0 and above
// SPEED_MAX_MPH to 300. The ring itself starts at 120 degrees with the gap
// at the bottom; that rotational offset is applied by the renderer, not
// here.
float speedToAngle(float mph);

} // namespace DialFormat
