#pragma once

#include <stdint.h>
#include <stddef.h>

// Arduino-free JSON parsing for the Flights card, buildable and testable on
// the host (native env). Turns adsb.fi / hexdb.io API responses into POD
// snapshots the UI can render directly; no dynamic allocation or Arduino
// dependency so the parsing logic can be unit tested off-device.
namespace FlightsModel
{

// One aircraft, nearest-first-sorted, ready to render.
struct Aircraft
{
    char hex[7];          // ICAO 24-bit address, e.g. "407131"
    char callsign[9];      // trimmed flight/callsign, e.g. "VIR359"
    char type[5];          // ICAO aircraft type, e.g. "B789"
    int altFt;              // barometric altitude, feet (0 if "ground")
    int gsKt;                // ground speed, knots (0 if missing)
    int track;               // true track, degrees [0,360) (0 if missing)
    float lat, lon;
    float distMi;             // great-circle distance from home, statute miles
    int bearing;               // bearing from home to aircraft, degrees [0,360)
    char fromIata[5], toIata[5]; // route endpoints (IATA once enriched); empty until known
    char airlineIata[3];          // 2-letter IATA airline code for the logo; empty until known
    char operatorName[32];         // operator/airline display name; empty until known
    bool routeKnown, operatorKnown; // whether enrichment has filled in the above
};

// A batch of aircraft near home, plus fetch bookkeeping for the UI.
struct FlightsSnapshot
{
    Aircraft ac[6];
    uint8_t count;
    uint32_t fetchedMs; // left for the caller to stamp; not set by parseAdsbFi
    bool stale;
    bool offline;
};

// Parses an adsb.fi `/v2/lat/.../lon/.../dist/...` response body. Keeps only
// aircraft with numeric lat/lon and a non-empty trimmed `flight`, computes
// distMi/bearing from (homeLat, homeLon) via Geo, sorts ascending by
// distance, and caps the result at 6 entries. `alt_baro` may be the JSON
// string "ground" (-> altFt 0); `gs`/`track` may be absent (-> 0). Sets
// out.count and fills out.ac; leaves fetchedMs/stale/offline untouched.
// Returns false on malformed/unparseable JSON.
bool parseAdsbFi(const char* json, size_t len, float homeLat, float homeLon, FlightsSnapshot& out);

// Parses a hexdb.io `/api/v1/route/icao/{callsign}` response body, e.g.
// {"route":"VABB-EGLL"}. Splits `route` on '-' into ICAO airport codes.
// Returns false if `route` is missing, the literal "unknown", or either side
// is not 3-4 characters long; `from`/`to` are left untouched in that case.
bool parseHexdbRoute(const char* json, size_t len, char from[5], char to[5]);

// Parses a hexdb.io `/api/v1/airport/icao/{ICAO}` response body and copies
// its `iata` field into `iata[5]`. Returns false if `iata` is missing/empty.
bool parseHexdbAirport(const char* json, size_t len, char iata[5]);

// Parses a hexdb.io `/api/v1/aircraft/{hex}` response body and copies
// `OperatorFlagCode` into `operatorIcao[4]` and `RegisteredOwners` into
// `operatorName[32]` (truncated safely if longer). Returns false if the
// JSON is malformed or `RegisteredOwners` is missing/empty.
bool parseHexdbAircraft(const char* json, size_t len, char operatorIcao[4], char operatorName[32]);

} // namespace FlightsModel
