#pragma once
#include <stdint.h>

// Speed constants — pure numbers, safe for host builds (no Arduino.h).
static constexpr float    SPEED_MIN_MPH     = 0.6f;
static constexpr float    SPEED_MAX_MPH     = 3.8f;
static constexpr float    SPEED_STEP_MPH    = 0.1f;
static constexpr uint16_t SPEED_RAW_PER_MPH = 1600;
static constexpr uint16_t SPEED_RAW_MAX     = 6080;   // 3.8 mph
// HA slider below this stops the belt; below SPEED_MIN_MPH there is no valid
// belt speed, so anything under this threshold means "stop", not "creep".
static constexpr float    SPEED_STOP_BELOW_MPH = 0.55f;
static constexpr uint16_t START_SPEED_RAW   = 994;    // ~1.0 km/h
static constexpr uint32_t SPEED_SETTLE_MS   = 400;
// How long the Dial's speed-change overlay stays on screen after a nudge.
// Used by the render task added in a later task.
static constexpr uint32_t DIAL_SPEED_OVERLAY_MS = 1500;

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

// One walking session's contribution to the all-time totals. Always a delta
// relative to the belt odometer reading captured at BLE connect time — never a
// raw odometer value. See doc/Q1_BLE_NOTES.md "Session Delta Accounting".
struct SessionDelta
{
    float    distKm      = 0.0f;
    uint32_t steps       = 0;
    uint32_t calories    = 0;
    uint32_t durationSec = 0;
};

// Cumulative totals store, as seen by SessionTracker. Implemented by
// TreadmillState (NVS-backed) on device and by a fake in the host tests.
class ITotalsStore
{
public:
    virtual float    totalDistanceKm()  const = 0;
    virtual uint32_t totalSteps()       const = 0;
    virtual uint32_t totalCalories()    const = 0;
    virtual uint32_t totalDurationSec() const = 0;
    virtual float    stepLengthM(float speedMph) const = 0;
    // Updates the in-memory totals only. The implementation is expected to
    // defer the flash write so this stays safe to call from a BLE callback.
    virtual void     addSession(const SessionDelta&) = 0;
    virtual ~ITotalsStore() = default;
};

// Side effects the session accounting needs from its owner. TreadmillHandler
// implements these with the BLE timer/flag code that used to be inline in
// notifyCallback().
class ISessionEvents
{
public:
    virtual void onSessionStarted() = 0; // belt moving: cancel idle timer
    virtual void onSessionEnded()   = 0; // clean STOPPED: autoReconnect=false, arm idle timer
    virtual void onPaused()         = 0; // arm pause timeout
    virtual void onResumed()        = 0; // cancel pause timeout
    virtual void onSettledIdle()    = 0; // first 0x2F STOPPED with no session: arm idle timer
    virtual ~ISessionEvents() = default;
};
