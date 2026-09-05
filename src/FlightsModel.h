#pragma once

#include <stdint.h>
#include <stddef.h>

// Arduino-free JSON parsing for the Flights card, buildable and testable on
// the host (native env). Turns HA's retained MQTT flights payload (spec
// 4.11) into POD snapshots the UI can render directly; no dynamic
// allocation (beyond ArduinoJson's own document) or Arduino dependency so
// the parsing logic can be unit tested off-device.
namespace FlightsModel
{

// One aircraft, nearest-first-sorted (HA sorts; this module preserves
// payload order), ready to render.
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
    int bearing;               // bearing from home to aircraft, degrees [0,359]
    char fromIata[5], toIata[5]; // route endpoints (IATA once enriched); empty until known
    char airlineIata[3];          // 2-letter IATA airline code for the logo; empty until known
    char operatorName[32];         // operator/airline display name; empty until known
    bool routeKnown, operatorKnown; // whether enrichment has filled in the above
    bool onGround;                   // HA's "gnd" flag
    char flightNumber[9];             // trimmed flight number, e.g. "BA123"
};

// A batch of aircraft near home, plus fetch bookkeeping for the UI.
struct FlightsSnapshot
{
    Aircraft ac[6];
    uint8_t count;
    uint32_t fetchedMs; // left for the caller to stamp; not set by parseDialFlights
    bool stale;
    bool offline;
};

// Parses HA's retained `pacekeeper-dial/flights/state` payload (spec 4.11):
// `{"ts":<epoch s>,"ac":[{"cs":"BAW123","fl":"BA123","ty":"A320","al":"BA",
// "an":"British Airways","fr":"LHR","to":"JFK","alt":12000,"gs":450,
// "di":2.3,"br":135,"gnd":0}, ...]}`, nearest first, already computed by HA.
// Per aircraft: callsign<-cs, type<-ty, airlineIata<-al, operatorName<-an,
// fromIata<-fr, toIata<-to, altFt<-alt, gsKt<-gs, distMi<-di, bearing<-br
// (normalised to [0,359]), onGround<-gnd, flightNumber<-fl. Missing string
// fields become "", missing numeric fields become 0.
// routeKnown/operatorKnown are always set true (HA already knows both, or
// reports "" if it doesn't); hex is always "" and lat/lon/track are always
// 0 (the Dial no longer has raw ADS-B position data). Caps at 6 aircraft
// (extras beyond the 6th are ignored) and sets out.count; leaves
// fetchedMs/stale/offline untouched for the caller to stamp. Returns false
// if the JSON is malformed or `ac` is missing/not an array.
bool parseDialFlights(const char* json, size_t len, FlightsSnapshot& out);

// Plan 7 Task 2 removes this (dead once FlightsService is MQTT-fed)
// Parses an adsb.fi `/v2/lat/.../lon/.../dist/...` response body. Keeps only
// aircraft with numeric lat/lon and a non-empty trimmed `flight`, computes
// distMi/bearing from (homeLat, homeLon) via Geo, sorts ascending by
// distance, and caps the result at 6 entries. `alt_baro` may be the JSON
// string "ground" (-> altFt 0); `gs`/`track` may be absent (-> 0). Sets
// out.count and fills out.ac; leaves fetchedMs/stale/offline untouched.
// Returns false on malformed/unparseable JSON.
bool parseAdsbFi(const char* json, size_t len, float homeLat, float homeLon, FlightsSnapshot& out);

// Plan 7 Task 2 removes this (dead once FlightsService is MQTT-fed)
// Parses a hexdb.io `/api/v1/route/icao/{callsign}` response body, e.g.
// {"route":"VABB-EGLL"}. Splits `route` on '-' into ICAO airport codes.
// Returns false if `route` is missing, the literal "unknown", or either side
// is not 3-4 characters long; `from`/`to` are left untouched in that case.
bool parseHexdbRoute(const char* json, size_t len, char from[5], char to[5]);

// Plan 7 Task 2 removes this (dead once FlightsService is MQTT-fed)
// Parses a hexdb.io `/api/v1/airport/icao/{ICAO}` response body and copies
// its `iata` field into `iata[5]`. Returns false if `iata` is missing/empty.
bool parseHexdbAirport(const char* json, size_t len, char iata[5]);

// Plan 7 Task 2 removes this (dead once FlightsService is MQTT-fed)
// Parses a hexdb.io `/api/v1/aircraft/{hex}` response body and copies
// `OperatorFlagCode` into `operatorIcao[4]` and `RegisteredOwners` into
// `operatorName[32]` (truncated safely if longer). Returns false if the
// JSON is malformed or `RegisteredOwners` is missing/empty.
bool parseHexdbAircraft(const char* json, size_t len, char operatorIcao[4], char operatorName[32]);

} // namespace FlightsModel
