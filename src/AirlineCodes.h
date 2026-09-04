#pragma once

// Arduino-free ICAO->IATA airline code lookup, buildable and testable on the
// host (native env). Used by the Flights card to turn an ADS-B "flight"
// callsign (e.g. "BAW117") into the IATA airline code needed for the
// pics.avs.io logo URL.
namespace AirlineCodes
{

// Looks up the IATA airline code for an ICAO airline designator. `icao` may
// be a bare 3-letter ICAO code or a full flight callsign (e.g. "BAW117") —
// only the first three letters are used. Matching is case-insensitive.
// Returns nullptr if `icao` is nullptr, shorter than 3 letters, or not found
// in the table. The returned pointer is to static storage and never needs
// freeing.
const char* iataFromIcao(const char* icao);

} // namespace AirlineCodes
