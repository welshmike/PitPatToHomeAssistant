#pragma once
#include <stdint.h>

#include "TreadmillData.h"
#include "ba05protocol.h"

// Logging. The messages here are the owner's primary diagnostic against the belt,
// so they are kept verbatim from the TreadmillHandler code this class replaces.
#ifdef NATIVE_TEST
  #include <cstdio>
  #define TRACKER_LOG_I(...) do { std::printf(__VA_ARGS__); std::printf("\n"); } while (0)
  #define TRACKER_LOG_W(...) TRACKER_LOG_I(__VA_ARGS__)
  // log_d is compiled out at CORE_DEBUG_LEVEL=3 on device; keep it silent here too.
  #define TRACKER_LOG_D(...) do { } while (0)
#else
  #include <esp32-hal-log.h>
  #define TRACKER_LOG_I(...) log_i(__VA_ARGS__)
  #define TRACKER_LOG_W(...) log_w(__VA_ARGS__)
  #define TRACKER_LOG_D(...) log_d(__VA_ARGS__)
#endif

// Pure session accounting for the BA05 belt: baselines, deltas, pause/resume
// bookkeeping and the live totals shown in Home Assistant. Knows nothing about
// BLE, NVS or Arduino — all wall-clock time arrives as nowMs parameters and all
// side effects go through ISessionEvents / ITotalsStore.
//
// Threading (unchanged from the code this replaces): onPacket() and
// onDisconnected() run on the NimBLE task; onConnected(), onPauseCommand(),
// onStopCommand() and onPauseTimeout() run on the main loop. m_sessionActive and
// m_isPaused are volatile because they are read across that boundary.
class SessionTracker
{
public:
    SessionTracker(ITotalsStore& totals, ISessionEvents& events);

    // Reset per-connection state. Call from completeSetup() before subscribing,
    // with the same millis() value the handler records as its connect time.
    void onConnected(uint32_t nowMs);

    // Fold one parsed notification into the session. Returns the new snapshot;
    // the caller stores it and publishes it from the main loop.
    TreadMillData onPacket(const BA05Protocol::ParsedData& p,
                           const TreadMillData& prev,
                           uint32_t nowMs);

    // Mid-session or paused disconnect: commit whatever is owed.
    void onDisconnected(const TreadMillData& last);

    void onPauseCommand();

    // Commits a paused session if one is pending. Returns true if it committed,
    // in which case the caller also clears its auto-reconnect flags.
    bool onStopCommand();

    // Pause timeout fired: commit the paused snapshot and return the session
    // summary snapshot to publish before disconnecting.
    TreadMillData onPauseTimeout(const TreadMillData& last);

    bool sessionActive() const { return m_sessionActive; }
    bool isPaused()      const { return m_isPaused; }

private:
    // (current - connection base), floored at the raw value if the belt reset
    // its odometer beneath the base. Never returns a negative distance.
    SessionDelta computeDelta(float distKm, uint32_t cal, uint32_t dur, float speedMph) const;

    // Commit the stored paused snapshot to totals.
    void commitPausedSnapshot();

    ITotalsStore&   m_totals;
    ISessionEvents& m_events;

    // Belt odometer values recorded at the START of the current BLE connection.
    // Every addSession() commits (current - base) so that reconnects mid-session
    // don't count the same distance twice.
    float    m_connectionBaseDistKm = 0.0f;
    uint16_t m_connectionBaseCal    = 0;
    uint32_t m_connectionBaseDurSec = 0;

    // True once the belt has physically moved (speed > 0) in this BLE session.
    // Ground truth for "belt was genuinely active" — immune to the phantom
    // RUNNING status of 0x34 packets and to residual odometer from prior walks.
    volatile bool m_sessionActive = false;

    // Set when a pause command is sent. Suppresses the STOPPED commit the belt
    // fires straight afterwards; the session is committed on resume-with-reset,
    // stop, pause timeout or disconnect instead.
    volatile bool m_isPaused = false;
    float    m_pausedDistKm      = 0.0f;
    uint32_t m_pausedSteps       = 0;
    uint32_t m_pausedCalories    = 0;
    uint32_t m_pausedDurationSec = 0;

    // The belt fires a brief RUNNING->STOPPED startup sequence after a resume;
    // this suppresses that spurious STOPPED commit. NimBLE task only.
    bool     m_justResumedFromPause = false;
    uint32_t m_resumeDistanceM      = 0;

    // Most recent packet type byte (0x2F or 0x34); 0 until the first packet of a
    // connection. Used to detect the transition out of the 0x34 kicking phase.
    uint8_t m_lastPacketType = 0;

    // Baseline capture happens on the first full packet of each connection.
    volatile bool m_firstPacketAfterConnect = false;

    // Previous belt status, for transition detection. Deliberately NOT reset by
    // onConnected() — it survives across reconnects exactly as it did when it
    // lived in TreadmillHandler.
    TreadMillData::Status m_lastStatus = TreadMillData::DISCONNECTED;

    // millis() at connect, for the "t=+Nms after connect" packet-type logging.
    uint32_t m_connectedAtMs = 0;
};
