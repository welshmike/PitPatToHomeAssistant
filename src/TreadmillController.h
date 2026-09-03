#pragma once
#include <stdint.h>

#include "TreadmillData.h"
#include "NetStatus.h"
#include "ITreadmillLink.h"

// Intent layer between command sources (MQTT/HA today, the M5Dial touch+encoder
// next) and the BLE link. Command sources call this; nothing calls the link
// directly. Deliberately Arduino-free so it builds and is tested on the host —
// all timing arrives as `nowMs` from the caller.

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
    // Start at an explicit speed (mph), clamped to [SPEED_MIN_MPH, SPEED_MAX_MPH]
    // — used by the HA start-speed number and the Dial's start selector.
    void startAt(float mph);
    void pause();
    void resume();
    // Returns the link's own answer: true when the link accepted the stop (or
    // queued it), false when it refused/failed. Callers that give feedback for
    // a stop gesture (DialUi) use this instead of guessing from the snapshot.
    bool stop();

    // link paused -> resume; otherwise disconnected/stopped -> start,
    // running -> pause, paused -> resume, countdown -> stop.
    void toggleStartPause();

    // Clamped to [SPEED_MIN_MPH, SPEED_MAX_MPH]; below 0.55 mph this stops the
    // belt, so an HA slider dragged to 0 still means "stop".
    void setSpeedMph(float mph);

    // Encoder detents. Accumulates into a target and defers the actual command
    // by SPEED_SETTLE_MS so a fast spin sends one packet, not one per click.
    void nudgeSpeed(int clicks, uint32_t nowMs);
    void tick(uint32_t nowMs);

    // Both return the link's own answer: false means the link refused (already
    // in that state, or a disconnect blocked because the belt is active). On a
    // refusal the observers are re-notified with the current snapshot so a view
    // that showed the request optimistically — HA's Connect switch — snaps back.
    bool requestConnect();
    bool requestDisconnect();

    // Push the link's current snapshot to observers — called by the main loop
    // when the link flags new BLE data.
    void publish();

    // Fan the network state out to the views. The loop task calls this when
    // NetTask::status() changes.
    void publishNetStatus(NetStatus s);

    float targetSpeedMph() const { return m_targetMph; }

    // Pass-through: the configured start speed (HA number / Dial selector default).
    float startSpeedMph() const { return m_link.startSpeedRaw() / (float)SPEED_RAW_PER_MPH; }

    // The belt reports STOPPED while paused, so this is the only way to tell
    // "paused" apart from "stopped" — delegates to the link's own pause flag.
    bool isPaused() const { return m_link.isPaused(); }

    // Pass-throughs for the Dial UI's Connecting screen.
    bool isConnecting() const { return m_link.isConnecting(); }
    uint16_t connectAttempts() const { return m_link.connectAttempts(); }

private:
    void notifySnapshot(const TreadMillData& d);
    void notifyTarget(float mph, bool pending);
    // Applies the optimistic snapshot every command shares: mutate a copy of the
    // link's snapshot, hand it back to the link, then tell observers once.
    void publishOptimistic(const TreadMillData& d);
    // A command whose write failed: don't touch the snapshot, just re-notify with
    // whatever the link now holds (the handler wrote DISCONNECTED into it).
    void notifyLinkSnapshot();
    static float clampMph(float mph);
    static float roundToStep(float mph);

    ITreadmillLink&    m_link;
    ISnapshotObserver* m_observers[MAX_OBSERVERS] = {nullptr, nullptr, nullptr, nullptr};
    uint8_t            m_observerCount = 0;

    float    m_targetMph      = 0.0f;
    bool     m_nudgePending   = false;
    uint32_t m_settleDeadline = 0;
};
