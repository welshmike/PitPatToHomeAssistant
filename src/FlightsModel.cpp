#include "FlightsModel.h"

#include <ArduinoJson.h>
#include <math.h>
#include <string.h>

#include "Geo.h"

namespace FlightsModel
{

namespace
{

// Copies `src` into `dst` (capacity dstSize), trimming leading/trailing
// ASCII spaces and truncating safely if needed. Always null-terminates.
// `src` may be nullptr, in which case dst becomes an empty string.
void trimCopy(const char* src, char* dst, size_t dstSize)
{
    dst[0] = '\0';
    if (!src || dstSize == 0)
    {
        return;
    }

    while (*src == ' ')
    {
        src++;
    }
    size_t len = strlen(src);
    while (len > 0 && src[len - 1] == ' ')
    {
        len--;
    }
    if (len > dstSize - 1)
    {
        len = dstSize - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

// Copies `src` into `dst` (capacity dstSize) verbatim, truncating safely if
// `src` is longer than dstSize - 1. Always null-terminates.
void safeCopy(const char* src, char* dst, size_t dstSize)
{
    dst[0] = '\0';
    if (!src || dstSize == 0)
    {
        return;
    }
    size_t len = strlen(src);
    if (len > dstSize - 1)
    {
        len = dstSize - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

// `alt_baro` is either a JSON number (feet) or the string "ground". Missing
// entirely also maps to 0.
int readAltFt(JsonVariant v)
{
    if (v.isNull() || v.is<const char*>())
    {
        return 0;
    }
    return static_cast<int>(v.as<double>());
}

// `gs`/`track` are JSON numbers when present; 0 when missing.
int readIntRounded(JsonVariant v)
{
    if (v.isNull() || v.is<const char*>())
    {
        return 0;
    }
    return static_cast<int>(lround(v.as<double>()));
}

} // namespace

bool parseDialFlights(const char* json, size_t len, FlightsSnapshot& out)
{
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json, len);
    if (err)
    {
        return false;
    }

    JsonVariant acVar = doc["ac"];
    if (!acVar.is<JsonArray>())
    {
        return false;
    }
    JsonArray arr = acVar.as<JsonArray>();

    size_t i = 0;
    for (JsonObject obj : arr)
    {
        if (i >= 6)
        {
            break;
        }

        Aircraft& a = out.ac[i];
        memset(&a, 0, sizeof(a));

        trimCopy(obj["cs"] | "", a.callsign, sizeof(a.callsign));
        trimCopy(obj["fl"] | "", a.flightNumber, sizeof(a.flightNumber));
        safeCopy(obj["ty"] | "", a.type, sizeof(a.type));
        safeCopy(obj["al"] | "", a.airlineIata, sizeof(a.airlineIata));
        safeCopy(obj["an"] | "", a.operatorName, sizeof(a.operatorName));
        safeCopy(obj["fr"] | "", a.fromIata, sizeof(a.fromIata));
        safeCopy(obj["to"] | "", a.toIata, sizeof(a.toIata));

        a.altFt = readIntRounded(obj["alt"]);
        a.gsKt = readIntRounded(obj["gs"]);
        a.distMi = obj["di"] | 0.0f;

        int bearing = static_cast<int>(lround(obj["br"] | 0.0)) % 360;
        if (bearing < 0)
        {
            bearing += 360;
        }
        a.bearing = bearing;

        a.onGround = obj["gnd"].as<bool>();

        // hex/lat/lon/track: no ADS-B position data reaches the Dial any
        // more, HA does that math. Left zeroed by the memset above.
        a.routeKnown = true;
        a.operatorKnown = true;

        i++;
    }

    out.count = static_cast<uint8_t>(i);
    return true;
}

// Plan 7 Task 2 removes this (dead once FlightsService is MQTT-fed)
bool parseAdsbFi(const char* json, size_t len, float homeLat, float homeLon, FlightsSnapshot& out)
{
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json, len);
    if (err)
    {
        return false;
    }

    JsonVariant aircraftVar = doc["aircraft"];
    if (!aircraftVar.is<JsonArray>())
    {
        return false;
    }
    JsonArray arr = aircraftVar.as<JsonArray>();

    // I2: streaming top-6 insertion, no intermediate candidates buffer. The
    // old version collected every parseable aircraft into a
    // candidates[64] array before sorting and capping at 6 — up to 64 *
    // sizeof(Aircraft) of net-task stack for a card that only ever shows 6.
    // Since FlightsSnapshot only ever holds 6, each parsed aircraft is
    // instead inserted directly into `kept` (ascending by distMi) as it's
    // parsed, evicting the current farthest kept entry once `kept` is full
    // and a nearer one turns up. `kept`/`keptCount` end up holding exactly
    // what the old candidates[]+sort+cap-at-6 pipeline produced — the 6
    // nearest, nearest first — using only one Aircraft-sized scratch (`a`)
    // plus the 6-entry `kept` array, regardless of how many aircraft are in
    // the response (so the old kMaxCandidates=64 cap on the result set is
    // gone too — every aircraft in the response is now considered).
    Aircraft kept[6];
    size_t keptCount = 0;

    for (JsonObject obj : arr)
    {
        JsonVariant latV = obj["lat"];
        JsonVariant lonV = obj["lon"];
        if (!latV.is<double>() || !lonV.is<double>())
        {
            continue;
        }

        char callsign[9];
        trimCopy(obj["flight"] | "", callsign, sizeof(callsign));
        if (callsign[0] == '\0')
        {
            continue;
        }

        Aircraft a;
        memset(&a, 0, sizeof(a));

        trimCopy(obj["hex"] | "", a.hex, sizeof(a.hex));
        memcpy(a.callsign, callsign, sizeof(callsign));
        safeCopy(obj["t"] | "", a.type, sizeof(a.type));

        a.altFt = readAltFt(obj["alt_baro"]);
        a.gsKt = readIntRounded(obj["gs"]);
        a.track = readIntRounded(obj["track"]);
        a.lat = latV.as<float>();
        a.lon = lonV.as<float>();
        a.distMi = Geo::distanceMiles(homeLat, homeLon, a.lat, a.lon);
        int bearing = static_cast<int>(lroundf(Geo::bearingDeg(homeLat, homeLon, a.lat, a.lon))) % 360;
        if (bearing < 0)
        {
            bearing += 360;
        }
        a.bearing = bearing;

        // Enrichment (route/operator) is fetched separately and not known yet.
        a.fromIata[0] = '\0';
        a.toIata[0] = '\0';
        a.airlineIata[0] = '\0';
        a.operatorName[0] = '\0';
        a.routeKnown = false;
        a.operatorKnown = false;

        if (keptCount < 6)
        {
            // Still filling up: insertion-sort `a` into its place among
            // what's kept so far (stable — equal distances keep arrival
            // order, matching the old full-sort's stability).
            size_t j = keptCount;
            while (j > 0 && kept[j - 1].distMi > a.distMi)
            {
                kept[j] = kept[j - 1];
                j--;
            }
            kept[j] = a;
            keptCount++;
        }
        else if (a.distMi < kept[keptCount - 1].distMi)
        {
            // Full: only displace the current farthest kept entry if `a`
            // is strictly nearer (ties keep whichever arrived first, same
            // as the old stable full-sort would after capping at 6).
            size_t j = keptCount - 1;
            while (j > 0 && kept[j - 1].distMi > a.distMi)
            {
                kept[j] = kept[j - 1];
                j--;
            }
            kept[j] = a;
        }
    }

    for (size_t i = 0; i < keptCount; i++)
    {
        out.ac[i] = kept[i];
    }
    out.count = static_cast<uint8_t>(keptCount);
    return true;
}

// Plan 7 Task 2 removes this (dead once FlightsService is MQTT-fed)
bool parseHexdbRoute(const char* json, size_t len, char from[5], char to[5])
{
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json, len);
    if (err)
    {
        return false;
    }

    const char* route = doc["route"] | "";
    if (route[0] == '\0' || strcmp(route, "unknown") == 0)
    {
        return false;
    }

    const char* dash = strchr(route, '-');
    if (!dash)
    {
        return false;
    }

    const size_t fromLen = static_cast<size_t>(dash - route);
    const size_t toLen = strlen(dash + 1);
    if (fromLen < 3 || fromLen > 4 || toLen < 3 || toLen > 4)
    {
        return false;
    }

    memcpy(from, route, fromLen);
    from[fromLen] = '\0';
    memcpy(to, dash + 1, toLen);
    to[toLen] = '\0';
    return true;
}

// Plan 7 Task 2 removes this (dead once FlightsService is MQTT-fed)
bool parseHexdbAirport(const char* json, size_t len, char iata[5])
{
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json, len);
    if (err)
    {
        return false;
    }

    const char* iataStr = doc["iata"] | "";
    if (iataStr[0] == '\0')
    {
        return false;
    }

    safeCopy(iataStr, iata, 5);
    return true;
}

// Plan 7 Task 2 removes this (dead once FlightsService is MQTT-fed)
bool parseHexdbAircraft(const char* json, size_t len, char operatorIcao[4], char operatorName[32])
{
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json, len);
    if (err)
    {
        return false;
    }

    const char* name = doc["RegisteredOwners"] | "";
    if (name[0] == '\0')
    {
        return false;
    }

    safeCopy(doc["OperatorFlagCode"] | "", operatorIcao, 4);
    safeCopy(name, operatorName, 32);
    return true;
}

} // namespace FlightsModel
