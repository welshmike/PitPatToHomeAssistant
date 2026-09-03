#pragma once
#include <stdint.h>

#include "TreadmillData.h"
#include "NetStatus.h"

// Intent layer between command sources (MQTT/HA today, the M5Dial touch+encoder
// next) and the BLE link. Command sources call this; nothing calls the link
// directly. Deliberately Arduino-free so it builds and is tested on the host —
// all timing arrives as `nowMs` from the caller.

// What the controller needs from the BLE link. TreadmillHandler implements it.
class ITreadmillLink
{
public:
    virtual bool isConnected() const = 0;
    virtual void start() = 0;                   // start at START_SPEED_RAW (queues if disconnected)
    virtual void pause() = 0;
    virtual void stop() = 0;
    virtual void setSpeedRaw(uint16_t raw) = 0; // queues if disconnected
    virtual bool requestConnect() = 0;
    virtual bool requestDisconnect() = 0;       // refuses while the belt is active
    virtual TreadMillData snapshot() const = 0;
    virtual void publishOptimistic(const TreadMillData&) = 0; // write snapshot + flag new data
    virtual ~ITreadmillLink() = default;
};

// What views (MQTT publisher, Dial UI) implement to be told about state changes.
class ISnapshotObserver
{
public:
    virtual void onSnapshot(const TreadMillData&) = 0;
    // pending=true while the nudge settle timer is still running, false once the
    // target has actually been sent to the belt.
    virtual void onTargetSpeed(float mph, bool pending) = 0;
    virtual void onNetStatus(NetStatus) {}
    virtual ~ISnapshotObserver() = default;
};

class TreadmillController
{
public:
    explicit TreadmillController(ITreadmillLink& link);

    // Fixed capacity, no heap — one MQTT view plus one Dial view today.
    static constexpr uint8_t MAX_OBSERVERS = 4;
    void addObserver(ISnapshotObserver& obs);

    void start();
    void pause();
    void resume();
    void stop();

    // disconnected/stopped -> start, running -> pause, paused -> resume,
    // countdown -> stop.
    void toggleStartPause();

    // Clamped to [SPEED_MIN_MPH, SPEED_MAX_MPH]; below 0.55 mph this stops the
    // belt, so an HA slider dragged to 0 still means "stop".
    void setSpeedMph(float mph);

    // Encoder detents. Accumulates into a target and defers the actual command
    // by SPEED_SETTLE_MS so a fast spin sends one packet, not one per click.
    void nudgeSpeed(int clicks, uint32_t nowMs);
    void tick(uint32_t nowMs);

    void requestConnect();
    void requestDisconnect();

    // Push the link's current snapshot to observers — called by the main loop
    // when the link flags new BLE data.
    void publish();

    float targetSpeedMph() const { return m_targetMph; }

private:
    void notifySnapshot(const TreadMillData& d);
    void notifyTarget(float mph, bool pending);
    // Applies the optimistic snapshot every command shares: mutate a copy of the
    // link's snapshot, hand it back to the link, then tell observers once.
    void publishOptimistic(const TreadMillData& d);
    static float clampMph(float mph);
    static float roundToStep(float mph);

    ITreadmillLink&    m_link;
    ISnapshotObserver* m_observers[MAX_OBSERVERS] = {nullptr, nullptr, nullptr, nullptr};
    uint8_t            m_observerCount = 0;

    float    m_targetMph      = 0.0f;
    bool     m_nudgePending   = false;
    uint32_t m_settleDeadline = 0;
};
