#pragma once
#include <stdint.h>

// Speed constants — pure numbers, safe for host builds (no Arduino.h).
static constexpr float    SPEED_MIN_MPH     = 0.6f;
static constexpr float    SPEED_MAX_MPH     = 3.8f;
static constexpr float    SPEED_STEP_MPH    = 0.1f;
static constexpr uint16_t SPEED_RAW_PER_MPH = 1600;
static constexpr uint16_t SPEED_RAW_MAX     = 6080;   // 3.8 mph
static constexpr uint16_t SPEED_RAW_STOP_BELOW = 100; // < ~0.1 km/h => stop
static constexpr uint16_t START_SPEED_RAW   = 994;    // ~1.0 km/h
static constexpr uint32_t SPEED_SETTLE_MS   = 400;

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

    // All-time cumulative totals (managed by TreadmillHandler, persisted in NVS)
    float totalDistanceKm = 0.0;
    uint32_t totalSteps = 0;
    uint32_t totalCalories = 0;
    uint32_t totalDurationSec = 0;

    // Session summary — populated on STOPPED transition so Strava and other
    // automations can read the final session values after live fields clear to 0.
    float    sessionDistanceKm  = 0.0;
    uint32_t sessionSteps       = 0;
    uint32_t sessionCalories    = 0;
    uint32_t sessionDurationSec = 0;
    bool     sessionComplete    = false;
};
