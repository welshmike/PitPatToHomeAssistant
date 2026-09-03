#pragma once
#include <Arduino.h>
#include "board.h"
#include "TreadmillData.h"

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

// Step length used to estimate step count from distance (no step sensor in PitPat protocol).
// Counts individual foot strikes (left=1, right=2, left=3...), not stride pairs.
// Typical adult step length at slow walking pad speeds: 0.25–0.40m.
// Override in config.h if desired: #define STEP_LENGTH_M 0.35f
#ifndef STEP_LENGTH_M
#define STEP_LENGTH_M 0.30f   // ~12 inches — calibrated for slow walking pad speeds (1–4 mph)
#endif

// Declare strings as extern to avoid multiple-definition linker errors
extern const char* HOMEASSISTANT_STATUS_TOPIC;
extern const char* HOMEASSISTANT_STATUS_TOPIC_ALT;
