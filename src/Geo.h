#pragma once

// Arduino-free great-circle geometry, buildable and testable on the host
// (native env). Used by the Flights card to turn ADS-B aircraft lat/lon into
// a distance and compass direction relative to HOME_LAT/HOME_LON.
namespace Geo
{

// Plan 7 Task 2 removes this (dead once FlightsService is MQTT-fed)
// Great-circle distance between two WGS-84 points, in statute miles
// (haversine formula, Earth radius 3958.8 mi). Returns 0 for identical
// points.
float distanceMiles(float lat1, float lon1, float lat2, float lon2);

// Plan 7 Task 2 removes this (dead once FlightsService is MQTT-fed)
// Initial bearing from point 1 to point 2, in degrees clockwise from true
// north, normalised to [0, 360).
float bearingDeg(float lat1, float lon1, float lat2, float lon2);

// Maps a bearing (any value, normalised mod 360) to one of the 8 compass
// points "N", "NE", "E", "SE", "S", "SW", "W", "NW". Each point covers a
// 45deg sector centred on it (e.g. N covers [337.5, 360) union [0, 22.5)),
// lower bound inclusive.
const char* compass8(float deg);

} // namespace Geo
