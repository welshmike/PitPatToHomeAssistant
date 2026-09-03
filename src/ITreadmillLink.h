#pragma once
#include <stdint.h>

#include "TreadmillData.h"

// What the controller (and the Dial UI, read-only) needs from the BLE link.
// TreadmillHandler implements it. Deliberately Arduino-free so it builds and
// is tested on the host.

class ITreadmillLink
{
public:
    virtual bool isConnected() const = 0;
    // The four command calls return true when the command was written to the
    // belt, or queued because the link is disconnected; false when the write
    // itself failed (GATT write error, link down, or post-connect cooldown).
    // On false the caller must not write optimistic state; it should republish
    // whatever the link's snapshot holds (a failed GATT write has already set
    // DISCONNECTED there; a cooldown block leaves it unchanged).
    virtual bool startAtRaw(uint16_t raw) = 0;  // start at raw (queues if disconnected)
    virtual bool pause() = 0;
    virtual bool stop() = 0;
    virtual bool setSpeedRaw(uint16_t raw) = 0; // queues if disconnected
    // The configured start speed (NVS "start", default START_SPEED_DEFAULT_MPH),
    // converted to raw units. start() paths use this instead of START_SPEED_RAW.
    virtual uint16_t startSpeedRaw() const = 0;
    virtual bool requestConnect() = 0;
    virtual bool requestDisconnect() = 0;       // refuses while the belt is active
    virtual TreadMillData snapshot() const = 0;
    virtual void publishOptimistic(const TreadMillData&) = 0; // write the snapshot only
    // The belt reports STOPPED while paused, so the link's own pause flag is the
    // only truth about a pause; lastCommandedSpeedRaw() is what resume() restores.
    virtual bool isPaused() const = 0;
    virtual uint16_t lastCommandedSpeedRaw() const = 0;
    // True while a connect is queued and auto-reconnect is enabled but the
    // link isn't up yet — i.e. handle() is actively retrying connectToDevice().
    virtual bool isConnecting() const = 0;
    // Number of connectToDevice() attempts made since the last requestConnect()
    // call or successful connection, whichever was most recent.
    virtual uint16_t connectAttempts() const = 0;
    virtual ~ITreadmillLink() = default;
};
