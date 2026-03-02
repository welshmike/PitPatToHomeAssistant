#pragma once
#include <Arduino.h>

#define SYSTEM_NAME "PaceKeeper"
#define VERSION "2026.1.1"

// SERVICE: 00001800-0000-1000-8000-00805f9b34fb (Generic Access Profile)
//   CHARACTERISTIC: 00002a01-0000-1000-8000-00805f9b34fb [read]
//   CHARACTERISTIC: 00002a00-0000-1000-8000-00805f9b34fb [read]
// SERVICE: 00001801-0000-1000-8000-00805f9b34fb (Generic Attribute Profile)
//   CHARACTERISTIC: 00002a05-0000-1000-8000-00805f9b34fb [indicate]
// SERVICE: 00001910-0000-1000-8000-00805f9b34fb (Vendor specific)
//   CHARACTERISTIC: 00002b10-0000-1000-8000-00805f9b34fb [read,notify]
//   CHARACTERISTIC: 00002b11-0000-1000-8000-00805f9b34fb [read,write-without-response,write]
// SERVICE: 0000fba0-0000-1000-8000-00805f9b34fb (Vendor specific)
#define SERVICE_PAD_UUID "0000ffff-0000-1000-8000-00805f9b34fb"
//  CHARACTERISTIC: 0000ff01-0000-1000-8000-00805f9b34fb [read,write]
#define CHARACTERISTIC_WRITE_UUID "0000ff01-0000-1000-8000-00805f9b34fb"
//  CHARACTERISTIC: 0000ff02-0000-1000-8000-00805f9b34fb [read,notify]
#define CHARACTERISTIC_NOTIFY_STATE_UUID "0000ff02-0000-1000-8000-00805f9b34fb"

// Stride length used to estimate step count from distance (no step sensor in PitPat protocol).
// Adjust in config.h if desired: #define STRIDE_LENGTH_M 0.70f (shorter) or 0.80f (taller)
#ifndef STRIDE_LENGTH_M
#define STRIDE_LENGTH_M 0.60f   // ~24 inches — calibrated for slow walking pad speeds (1–4 mph)
                                // Standard adult stride is ~0.762m but pads at desk speed
                                // produce shorter, shuffling steps. Override in config.h.
#endif

// Secondary service used for unlock handshake (best-effort, optional)
// The Q1 Classic Pro exposes this service; QZ writes the PitPat unlock bytes here
// to complete the connection handshake before data flows reliably.
#define SERVICE_UNLOCK_UUID "00001910-0000-1000-8000-00805f9b34fb"
#define CHARACTERISTIC_UNLOCK_UUID "00002b11-0000-1000-8000-00805f9b34fb"

// Declare strings as extern to avoid multiple-definition linker errors
extern const char* HOMEASSISTANT_STATUS_TOPIC;
extern const char* HOMEASSISTANT_STATUS_TOPIC_ALT;

class TreadMillData
{
public:
    enum Status
    {
        COUNTDOWN = 0,
        RUNNING = 1,
        PAUSED = 2,
        STOPPED = 3,
        DISCONNECTED = 100, // Internal state, don't use for treadmill communication
    };

    float speedCmd = 0.0;
    float speedFeedback = 0.0;
    float speedMax = 0.0;
    float distanceKm = 0.0;
    uint16_t calories = 0;
    uint32_t steps = 0;
    uint32_t durationSec = 0;
    uint8_t fwVersion = 0;
    Status status = DISCONNECTED; // default to DISCONNECTED when we start up
};
