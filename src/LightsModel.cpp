#include "LightsModel.h"

#include <ArduinoJson.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

namespace LightsModel
{

namespace
{

const char kTopicPrefix[] = "pacekeeper-dial/light/";
const char kTopicSuffix[] = "/state";

// Maps HA's color_mode string onto our reduced enum: "color_temp" -> TEMP;
// any of the color-space modes ("xy", "hs", "rgb", "rgbw", "rgbww") -> HS;
// null/missing/anything else -> NONE.
ColorMode readColorMode(JsonVariant v)
{
    if (!v.is<const char*>())
    {
        return ColorMode::NONE;
    }
    const char* s = v.as<const char*>();
    if (strcmp(s, "color_temp") == 0)
    {
        return ColorMode::TEMP;
    }
    if (strcmp(s, "xy") == 0 || strncmp(s, "rgb", 3) == 0 || strcmp(s, "hs") == 0)
    {
        return ColorMode::HS;
    }
    return ColorMode::NONE;
}

} // namespace

const char* keyName(LightKey k)
{
    switch (k)
    {
    case LightKey::OFFICE:
        return "office";
    case LightKey::LAMP:
        return "lamp";
    default:
        return "";
    }
}

bool keyFromTopic(const char* topic, LightKey& out)
{
    if (topic == nullptr)
    {
        return false;
    }

    size_t prefixLen = strlen(kTopicPrefix);
    size_t suffixLen = strlen(kTopicSuffix);
    size_t topicLen = strlen(topic);
    if (topicLen <= prefixLen + suffixLen)
    {
        return false;
    }
    if (strncmp(topic, kTopicPrefix, prefixLen) != 0)
    {
        return false;
    }
    if (strcmp(topic + topicLen - suffixLen, kTopicSuffix) != 0)
    {
        return false;
    }

    size_t keyLen = topicLen - prefixLen - suffixLen;
    const char* keyStart = topic + prefixLen;

    for (uint8_t i = 0; i < static_cast<uint8_t>(LightKey::COUNT); i++)
    {
        LightKey k = static_cast<LightKey>(i);
        const char* name = keyName(k);
        if (strlen(name) == keyLen && strncmp(keyStart, name, keyLen) == 0)
        {
            out = k;
            return true;
        }
    }
    return false;
}

bool parseLightState(const char* json, size_t len, LightState& out)
{
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json, len);
    if (err)
    {
        return false;
    }

    LightState s;
    s.valid = true;

    JsonVariant stateVar = doc["state"];
    if (stateVar.is<const char*>())
    {
        const char* stateStr = stateVar.as<const char*>();
        if (strcmp(stateStr, "on") == 0)
        {
            s.available = true;
            s.on = true;
        }
        else if (strcmp(stateStr, "off") == 0)
        {
            s.available = true;
            s.on = false;
        }
        // "unavailable"/"unknown"/anything else -> available stays false.
    }
    // Missing "state" -> available stays false.

    JsonVariant brightnessVar = doc["brightness_pct"];
    if (brightnessVar.is<double>())
    {
        double pct = brightnessVar.as<double>();
        if (pct < 0)
        {
            pct = 0;
        }
        if (pct > 100)
        {
            pct = 100;
        }
        s.brightnessPct = static_cast<uint8_t>(pct + 0.5);
    }

    s.mode = readColorMode(doc["color_mode"]);

    JsonVariant kelvinVar = doc["color_temp_kelvin"];
    if (kelvinVar.is<double>())
    {
        s.kelvin = static_cast<uint16_t>(kelvinVar.as<double>() + 0.5);
    }

    JsonVariant hsVar = doc["hs_color"];
    if (hsVar.is<JsonArray>())
    {
        JsonArray hs = hsVar.as<JsonArray>();
        if (hs.size() >= 2)
        {
            s.hue = hs[0].as<float>();
            s.sat = hs[1].as<float>();
        }
    }

    JsonVariant minVar = doc["min_kelvin"];
    if (minVar.is<double>())
    {
        s.minKelvin = static_cast<uint16_t>(minVar.as<double>() + 0.5);
    }
    JsonVariant maxVar = doc["max_kelvin"];
    if (maxVar.is<double>())
    {
        s.maxKelvin = static_cast<uint16_t>(maxVar.as<double>() + 0.5);
    }

    JsonVariant supportsColorVar = doc["supports_color"];
    if (supportsColorVar.is<bool>())
    {
        s.supportsColor = supportsColorVar.as<bool>();
    }

    out = s;
    return true;
}

size_t formatCommand(const Command& c, char* buf, size_t cap)
{
    if (buf == nullptr || cap == 0)
    {
        return 0;
    }

    int written = -1;
    switch (c.type)
    {
    case Command::Type::NONE:
        return 0;
    case Command::Type::POWER:
        written = snprintf(buf, cap, "{\"state\":\"%s\"}", c.on ? "ON" : "OFF");
        break;
    case Command::Type::BRIGHT:
        written = snprintf(buf, cap, "{\"state\":\"ON\",\"brightness_pct\":%u}", static_cast<unsigned>(c.pct));
        break;
    case Command::Type::TEMP:
        written = snprintf(buf, cap, "{\"state\":\"ON\",\"color_temp_kelvin\":%u}", static_cast<unsigned>(c.kelvin));
        break;
    case Command::Type::HUE:
    {
        long hue = lroundf(c.hue);
        long sat = lroundf(c.sat);
        written = snprintf(buf, cap, "{\"state\":\"ON\",\"hs_color\":[%ld,%ld]}", hue, sat);
        break;
    }
    }

    if (written < 0 || static_cast<size_t>(written) >= cap)
    {
        return 0;
    }
    return static_cast<size_t>(written);
}

} // namespace LightsModel
