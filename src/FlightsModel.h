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
    char fromCity[13], toCity[13]; // route endpoint city names, HA-truncated to 12 chars; empty until known
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
// "an":"British Airways","fr":"LHR","to":"JFK","fc":"London","tc":"New York",
// "alt":12000,"gs":450,"di":2.3,"br":135,"gnd":0}, ...]}`, nearest first,
// already computed by HA.
// Per aircraft: callsign<-cs, type<-ty, airlineIata<-al, operatorName<-an,
// fromIata<-fr, toIata<-to, fromCity<-fc, toCity<-tc, altFt<-alt, gsKt<-gs,
// distMi<-di, bearing<-br (normalised to [0,359]), onGround<-gnd,
// flightNumber<-fl. Missing string fields become "" (fc/tc included, for
// payloads from before the city keys existed), missing numeric fields
// become 0.
// routeKnown/operatorKnown are always set true (HA already knows both, or
// reports "" if it doesn't); hex is always "" and lat/lon/track are always
// 0 (the Dial no longer has raw ADS-B position data). Caps at 6 aircraft
// (extras beyond the 6th are ignored) and sets out.count; leaves
// fetchedMs/stale/offline untouched for the caller to stamp. Returns false
// if the JSON is malformed or `ac` is missing/not an array.
bool parseDialFlights(const char* json, size_t len, FlightsSnapshot& out);

} // namespace FlightsModel
