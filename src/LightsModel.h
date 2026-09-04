#pragma once

#include <stdint.h>
#include <stddef.h>

// Arduino-free parsing/formatting for the Lights card (Home Assistant lights
// over MQTT), buildable and testable on the host (native env). Turns the
// retained HA light-state JSON into a POD snapshot the UI can render, and
// formats the Dial's outgoing command JSON. No dynamic allocation or
// Arduino dependency so the logic can be unit tested off-device.
namespace LightsModel
{

enum class ColorMode : uint8_t { NONE, TEMP, HS };

// One light's current state, as parsed from its retained HA state topic.
struct LightState
{
    bool valid = false;       // parse succeeded
    bool available = false;   // state is "on"/"off" (not unavailable/unknown/missing)
    bool on = false;
    uint8_t brightnessPct = 0; // clamped 0-100
    ColorMode mode = ColorMode::NONE;
    uint16_t kelvin = 0;
    uint16_t minKelvin = 2000, maxKelvin = 6500;
    float hue = 0, sat = 100;
    bool supportsColor = false;
};

// The two lights the Dial's Lights card controls.
struct LightsSnapshot
{
    LightState office, lamp;
};

enum class LightKey : uint8_t { OFFICE, LAMP, COUNT };

// "office" / "lamp".
const char* keyName(LightKey k);

// Matches an MQTT topic of the form pacekeeper-dial/light/{key}/state and
// writes the matching key to `out`. Returns false (leaving `out` untouched)
// if the topic doesn't match either key.
bool keyFromTopic(const char* topic, LightKey& out);

// Parses a retained HA light-state JSON payload (doc <= 512 B). Returns
// false on malformed JSON; otherwise fills `out` (valid=true).
bool parseLightState(const char* json, size_t len, LightState& out);

// An outgoing command to publish to a light's command topic.
struct Command
{
    enum class Type : uint8_t { NONE, POWER, BRIGHT, TEMP, HUE } type = Type::NONE;
    bool on = false;
    uint8_t pct = 0;
    uint16_t kelvin = 0;
    float hue = 0, sat = 100;
};

// Formats `c` as JSON into buf (capacity cap) via snprintf. Returns the
// number of bytes written (excluding the NUL), or 0 if c.type is NONE or
// the formatted output would overflow `buf`.
size_t formatCommand(const Command& c, char* buf, size_t cap);

} // namespace LightsModel
