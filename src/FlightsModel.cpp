#include "FlightsModel.h"

#include <ArduinoJson.h>
#include <math.h>
#include <string.h>

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
        safeCopy(obj["fc"] | "", a.fromCity, sizeof(a.fromCity));
        safeCopy(obj["tc"] | "", a.toCity, sizeof(a.toCity));

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

} // namespace FlightsModel
