#pragma once
#include <stdint.h>

#include "TreadmillData.h"
#include "TreadmillState.h" // CalibrationPoint

// Messages exchanged between the two tasks.
//
//   net task  --Command-->  loop task    (MQTT receive callback parses, loop executes)
//   loop task --PublishItem--> net task  (loop task never touches PubSubClient)
//
// Both types are trivially copyable so the FreeRTOS queues can memcpy them.
// Device-only header: CalibrationPoint lives in TreadmillState.h, which pulls in
// Arduino/Preferences, so this must not reach the native (host) build.

enum class CmdType : uint8_t
{
    NONE,
    START,
    PAUSE,
    STOP,
    SET_SPEED_MPH,
    CONNECT,
    DISCONNECT,
    SET_AUTO_RECONNECT,
    SET_IDLE_MINS,
    SET_PAUSE_MINS,
    SET_START_SPEED,
    SET_FLIGHTS_AUTO_SHOW,
    SET_CALENDAR_NUDGE,
    SET_CALENDAR_LEAD,
    SET_CALENDAR_STAY,
    TOGGLE_CALIBRATION,
    RESTORE_TOTALS,
    RESTORE_CALIBRATION,
    HA_ONLINE,
};

struct Command
{
    CmdType  type = CmdType::NONE;
    float    f    = 0;     // SET_SPEED_MPH / SET_START_SPEED
    bool     b    = false; // SET_AUTO_RECONNECT / SET_FLIGHTS_AUTO_SHOW / SET_CALENDAR_NUDGE
    uint16_t u16  = 0;     // SET_IDLE_MINS / SET_PAUSE_MINS / SET_CALENDAR_LEAD / SET_CALENDAR_STAY

    // RESTORE_TOTALS. has[] marks which fields the JSON payload actually carried;
    // the loop task fills the rest from the current NVS values.
    struct
    {
        float    distKm      = 0;
        uint32_t steps       = 0;
        uint32_t calories    = 0;
        uint32_t durationSec = 0;
        bool     has[4]      = {false, false, false, false};
    } totals;

    // RESTORE_CALIBRATION. Same 10-point cap TreadmillState enforces.
    struct
    {
        CalibrationPoint pts[10] = {};
        uint8_t          n       = 0;
    } calib;
};

enum class PubType : uint8_t
{
    SNAPSHOT,
    AUTO_RECONNECT,
    IDLE_MINS,
    PAUSE_MINS,
    START_SPEED,
    FLIGHTS_AUTO_SHOW,
    CALENDAR_NUDGE,
    CALENDAR_LEAD,
    CALENDAR_STAY,
    CALIB_COUNT,
    FULL_RESYNC,
    LIGHT_CMD,
};

struct PublishItem
{
    PubType       type = PubType::SNAPSHOT;
    TreadMillData snap;         // SNAPSHOT
    bool          b    = false; // AUTO_RECONNECT / FLIGHTS_AUTO_SHOW / CALENDAR_NUDGE
    uint16_t      u16  = 0;     // IDLE_MINS / PAUSE_MINS / CALENDAR_LEAD / CALENDAR_STAY
    uint8_t       u8   = 0;     // CALIB_COUNT
    float         f    = 0;     // START_SPEED
    uint8_t       lightKey  = 0;     // LIGHT_CMD: LightsModel::LightKey
    char          lightJson[96] = {0}; // LIGHT_CMD: formatted command JSON
};
