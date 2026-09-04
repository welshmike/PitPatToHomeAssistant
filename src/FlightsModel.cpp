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

    // adsb.fi's radius query keeps result sets small; this is a generous
    // upper bound so we can sort before capping at 6.
    static const size_t kMaxCandidates = 64;
    Aircraft candidates[kMaxCandidates];
    size_t n = 0;

    for (JsonObject obj : arr)
    {
        if (n >= kMaxCandidates)
        {
            break;
        }

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

        Aircraft& a = candidates[n];
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

        n++;
    }

    // Insertion sort ascending by distMi. n is small (a handful of aircraft
    // within the query radius), so O(n^2) is fine and avoids pulling in
    // <algorithm> for the device build.
    for (size_t i = 1; i < n; i++)
    {
        Aircraft key = candidates[i];
        size_t j = i;
        while (j > 0 && candidates[j - 1].distMi > key.distMi)
        {
            candidates[j] = candidates[j - 1];
            j--;
        }
        candidates[j] = key;
    }

    const size_t kept = n < 6 ? n : 6;
    for (size_t i = 0; i < kept; i++)
    {
        out.ac[i] = candidates[i];
    }
    out.count = static_cast<uint8_t>(kept);
    return true;
}

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
