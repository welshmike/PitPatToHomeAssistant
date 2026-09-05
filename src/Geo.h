#pragma once

// Arduino-free compass helper, buildable and testable on the host (native
// env). Used by the Flights card to render the bearing Home Assistant sends
// with each aircraft as a compass point. The great-circle distance/bearing
// maths that used to live here went with Plan 7: HA does that now.
namespace Geo
{

// Maps a bearing (any value, normalised mod 360) to one of the 8 compass
// points "N", "NE", "E", "SE", "S", "SW", "W", "NW". Each point covers a
// 45deg sector centred on it (e.g. N covers [337.5, 360) union [0, 22.5)),
// lower bound inclusive.
const char* compass8(float deg);

} // namespace Geo
