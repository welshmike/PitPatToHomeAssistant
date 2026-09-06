#pragma once
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <math.h>

#include "platform.h"
#include "TreadmillState.h"
#include "SessionTracker.h"
#include "SnapshotStore.h"
#include "ITreadmillLink.h"

class TreadmillHandler : public NimBLEClientCallbacks, public ISessionEvents, public ITreadmillLink
{
public:
    TreadmillHandler();
    ~TreadmillHandler();
    void begin(NimBLEAddress address);
    // Return true when the command reached the belt (or was queued because the
    // link is down), false when sendCommand() refused or failed: a GATT write
    // failure flags DISCONNECTED in the snapshot; a post-connect cooldown block
    // or a missing write characteristic leaves the snapshot unchanged.
    bool setSpeed(uint16_t speed);
    bool startAtRaw(uint16_t raw) override;
    // Convenience wrapper — starts at the configured start speed. Kept for
    // main.cpp/tests; the controller calls startAtRaw(startSpeedRaw()) directly.
    bool start() { return startAtRaw(startSpeedRaw()); }
    bool pause() override;
    bool stop() override;

    // Returns true when observers should be pushed a fresh snapshot: a new BLE
    // packet arrived, a pause-timeout summary was committed, or the connection
    // timed out. main.cpp turns that into controller.publish().
    bool handle();

    void setAutoReconnect(const bool enable)
    {
        m_autoReconnect = enable;
        saveSettings();
    }

    bool getAutoReconnect() const
    {
        return m_autoReconnect;
    }

    // While held, handle() starts no background connect attempts — WiFi/MQTT
    // bring-up and the discovery burst share the antenna with BLE, and a
    // connect that collides with them loses service discovery and ends in a
    // disconnect() that sends the Q1 into its kicking phase (spec 4.15).
    // A user-requested connect always goes through. main.cpp owns the policy
    // (which net states hold, and for how long); this only stores the flag and
    // logs the transitions.
    void setConnectHold(bool hold);

    // If the timer is currently armed, rearm it with the new duration.
    void setIdleDisconnectMins(uint16_t mins);

    uint16_t getIdleDisconnectMins() const
    {
        return m_idleDisconnectMins;
    }

    // If the timer is currently armed, rearm it with the new duration.
    void setPauseTimeoutMins(uint16_t mins);

    uint16_t getPauseTimeoutMins() const
    {
        return m_pauseTimeoutMins;
    }

    // Configurable start speed — clamps to [SPEED_MIN_MPH, SPEED_MAX_MPH], rounds
    // to the nearest tenth (NVS storage unit), and persists.
    void setStartSpeedMph(float mph);

    float getStartSpeedMph() const
    {
        return (float)m_startSpeedTenths / 10.0f;
    }

    // ITreadmillLink: configured start speed converted to raw belt units.
    uint16_t startSpeedRaw() const override
    {
        return (uint16_t)lroundf(getStartSpeedMph() * SPEED_RAW_PER_MPH);
    }

    // Flights auto-show (spec 4.11): whether the Dial may raise the Flights
    // card over the Clock while aircraft are nearby. Belt-unrelated, but it
    // lives here because this is where every persisted user setting lives —
    // one NVS namespace, one saveSettings().
    void setFlightsAutoShow(bool on);

    bool getFlightsAutoShow() const
    {
        return m_flightsAutoShow;
    }

    // Dynamic Step Calibration
    void toggleCalibration() { m_state.toggleCalibration(m_snapshot.read().speedFeedback); }
    uint8_t getCalibrationPointCount() const { return m_state.getCalibrationPointCount(); }
    const CalibrationPoint* getCalibrationPoints() const { return m_state.getCalibrationPoints(); }
    void restoreCalibrationPoints(const CalibrationPoint* pts, uint8_t count) { m_state.restoreCalibrationPoints(pts, count); }

    // Getters for current NVS totals — used by the MQTT restore handler to supply
    // default values for any JSON fields that are absent from the restore payload.
    float    getTotalDistanceKm()  const { return m_state.getTotalDistanceKm(); }
    uint32_t getTotalSteps()       const { return m_state.getTotalSteps(); }
    uint32_t getTotalCalories()    const { return m_state.getTotalCalories(); }
    uint32_t getTotalDurationSec() const { return m_state.getTotalDurationSec(); }

    // Restore cumulative totals — call when NVS was wiped (accidental "Erase Flash").
    // Values are written immediately to NVS so they survive the next reboot.
    // Payload source: read the last good values from HA's sensor history for
    //   total_distance_km, total_steps, total_calories, total_duration_sec.
    void restoreTotals(float distKm, uint32_t steps, uint32_t calories, uint32_t durationSec);

    bool isConnected() const override
    {
        return m_pClient && m_pClient->isConnected();
    }

    // --- ITreadmillLink -----------------------------------------------------
    // Thin adapters over the existing API so TreadmillController can drive the
    // link without knowing anything about NimBLE.

    bool setSpeedRaw(uint16_t raw) override { return setSpeed(raw); }

    // The Q1 reports STOPPED while paused; the tracker's flag is the truth.
    bool isPaused() const override { return m_tracker.isPaused(); }
    uint16_t lastCommandedSpeedRaw() const override { return m_lastSpeed; }

    // True while handle() is working on a connect: a link/setup stage is in
    // flight (spec 4.17), or a connect is queued, auto-reconnect is on and
    // we're not up yet. The stage test matters because the link is already up
    // during SETUP, so the isConnected() test alone would report "connected"
    // before the write/notify characteristics exist.
    bool isConnecting() const override
    {
        return m_linkStage != LinkStage::IDLE ||
               (!isConnected() && m_doConnect && m_autoReconnect);
    }
    // Number of link attempts since the last requestConnect() call or
    // successful connection.
    uint16_t connectAttempts() const override { return m_connectAttempts; }

    TreadMillData snapshot() const override { return m_snapshot.read(); }

    // Writes the optimistic state into the shared snapshot. Deliberately does NOT
    // raise m_newDataAvailable: the controller already notifies its observers once
    // for the command that produced `d`, and flagging here would publish it twice.
    // Note: this is a read-modify-write from the loop task that can interleave
    // with the BLE task's own RMW in notifyCallback; the window is microseconds
    // and the next packet re-derives from the belt, so the effect is bounded.
    void publishOptimistic(const TreadMillData& d) override
    {
        m_snapshot.write(d);
    }

    // Returns true if the disconnect was initiated, false if blocked by the
    // safety check (belt still active).
    bool requestDisconnect() override;

    // Returns true if a connect was initiated, false if already connected.
    bool requestConnect() override;

    TreadMillData getLastData() const
    {
        return m_snapshot.read();
    }

private:
    bool sendCommand(const uint8_t *data, size_t length);
    void sendKeepalive();
    // Sent immediately after notification subscription to prevent reason=531 disconnect.
    // The Q1 expects a response within ~300ms of connect or it terminates the session.
    void sendInitSequence();

    // Staged connect (spec 4.17), both called from handle() on the loop task.
    // beginLink() issues the asynchronous connect and returns immediately;
    // completeSetup() runs the GATT setup once onConnect() has raised m_linkUp.
    bool beginLink();
    bool completeSetup();

    // BA05 protocol state
    // Heartbeat policy (changed 2026-09-03 from a 200 ms timer after an HCI snoop of the
    // PitPat app): the belt notifies ~1 Hz and the app replies with exactly one heartbeat
    // per notification. Unsolicited 200 ms heartbeats got us kicked (HCI 0x13) after ~11 s
    // on every connection. KEEPALIVE_FALLBACK_MS only fires if notifications stop.
    static constexpr unsigned long KEEPALIVE_FALLBACK_MS = 3000;
    static constexpr unsigned long POST_CONNECT_COOLDOWN = 3000; // ms — block commands after
                                                                  // reconnect until device settles
    uint8_t m_seqCounter = 0;
    unsigned long m_lastKeepalive = 0;
    // Set by notifyCallback (NimBLE task) when a valid packet arrives; handle() (loop task)
    // answers it with one heartbeat. Mirrors the PitPat app's reply-per-notification pattern.
    volatile bool m_keepaliveReplyPending = false;
    unsigned long m_lastConnectTime = 0;  // set on successful connect, gates command sending
    uint16_t m_lastSpeed = 0;  // last commanded speed, preserved for pause/resume
    void notifyCallback(
        NimBLERemoteCharacteristic *pBLERemoteCharacteristic,
        uint8_t *pData,
        size_t length,
        bool isNotify);

    NimBLEClient *m_pClient = nullptr;
    NimBLERemoteCharacteristic *m_pNotifyCharacteristic = nullptr;
    NimBLERemoteCharacteristic *m_pWriteCharacteristic = nullptr;
    NimBLEAddress m_targetAddress;
    bool m_doConnect = false;
    bool m_autoReconnect = true;

    unsigned long m_lastConnectAttempt = 0;
    // Count of beginLink() attempts since requestConnect() or the last
    // successful connection — drives the Dial's "attempt N" Connecting screen.
    uint16_t m_connectAttempts = 0;

    // --- Staged connect (spec 4.17) --------------------------------------
    // NimBLE's connect(addr, deleteAttributes, asyncConnect=true) returns as
    // soon as the request is queued, so GATT setup has to wait for the link to
    // actually come up. handle() drives both stages on the loop task; the
    // NimBLE callbacks only set flags.
    //   IDLE    — nothing in flight
    //   LINKING — connect() issued, waiting for onConnect() or the timeout
    //   SETUP   — link is up, completeSetup() is running on the loop task
    enum class LinkStage : uint8_t { IDLE, LINKING, SETUP };
    // volatile: onDisconnect() clears the stage from the NimBLE task while
    // handle() reads and writes it on the loop task.
    volatile LinkStage m_linkStage   = LinkStage::IDLE;
    uint32_t           m_linkStartMs = 0;

    // Raised by onConnect() (NimBLE task); cleared when a link begins and in
    // onDisconnect(). The only cross-task signal the LINKING stage waits on.
    volatile bool m_linkUp = false;

    // Matches the 6 s supervision timeout we request in setConnectionParams().
    // On expiry the LINKING stage calls cancelConnect() — never disconnect(),
    // which on a link that never came up is the HCI 0x16 that starts the Q1's
    // kicking phase.
    static constexpr uint32_t kLinkTimeoutMs = 6000;

    unsigned long m_lastPacketMs = 0;
    SnapshotStore m_snapshot;

    // Set true in completeSetup() so notifyCallback() can dump the raw bytes of
    // the first post-connect packet — helps diagnose the phantom RUNNING state on
    // connect. SessionTracker keeps its own first-packet flag for the baseline
    // capture; this one exists only because the tracker never sees raw bytes.
    volatile bool m_logRawFirstPacket = false;

    // Set by onConnect() (Core 0) when GATT is ready. handle() (Core 1) sends the
    // initial keepalive — writeValue(true) cannot be called from a NimBLE callback
    // because it blocks waiting for an ATT response processed by the same task.
    volatile bool m_sendInitNow = false;

    // Set when the user explicitly requests a connect from HA (not auto-connect at boot).
    // Determines idle-kick behaviour: if the user asked to connect, keep retrying with a
    // 60-second backoff instead of stopping entirely. Cleared when user disconnects,
    // or after a real walking session ends (belt STOPPED after actual activity).
    bool m_userRequestedConnect = false;

    // Set by main.cpp from the net status (spec 4.15). Suppresses background
    // reconnect attempts only; m_userRequestedConnect overrides it.
    bool m_connectHold = false;

    // millis() timestamp — don't attempt reconnect before this time.
    // Set in onDisconnect() after an idle kick: 5s for user-requested connect,
    // 20s for background auto-reconnect. Controls beep rate during kick phase.
    unsigned long m_reconnectNotBefore = 0;

    // Configurable timers — persisted to NVS so settings survive reboots.
    // NVS namespace "pk_cfg"; keys "ar", "idle", "pause".
    // Only saved by the public setters (setAutoReconnect / setIdleDisconnectMins /
    // setPauseTimeoutMins). Internal runtime changes to m_autoReconnect (session end,
    // kicks, etc.) bypass the setter so they do NOT overwrite the user's preference.
    void saveSettings();

    // Idle-disconnect timer: fires after m_idleDisconnectMins of idle (post-session STOPPED).
    // 0 = disabled.
    uint16_t      m_idleDisconnectMins     = 30;
    unsigned long m_idleDisconnectDeadline = 0;
    unsigned long m_idleDisconnectArmedAt  = 0;

    // Pause timeout: fires if the belt is paused for longer than m_pauseTimeoutMins.
    // Commits the paused session and disconnects immediately.
    // 0 = disabled.
    uint16_t      m_pauseTimeoutMins     = 10;
    unsigned long m_pauseTimeoutDeadline = 0;
    unsigned long m_pauseTimeoutArmedAt  = 0;

    // Configured start speed, in tenths of mph (NVS key "start"). Default 10 =
    // START_SPEED_DEFAULT_MPH. Every "start" path uses this instead of the fixed
    // START_SPEED_RAW.
    uint16_t m_startSpeedTenths = 10;

    // Flights auto-show setting (NVS key "fl_auto"), default on.
    bool m_flightsAutoShow = true;

    // Pending command — queued when start()/setSpeed() is called while disconnected.
    // Executed in handle() once reconnected and POST_CONNECT_COOLDOWN has elapsed.
    enum class PendingCmd : uint8_t { NONE, START, SET_SPEED };
    PendingCmd m_pendingCmd   = PendingCmd::NONE;
    uint16_t   m_pendingSpeed = 0;

    // Intentional drop via supervision timeout — set before we stop keepalives so
    // that onDisconnect() knows the disconnect was deliberate, not radio loss.
    // In practice Q1 beats the 6s supervision timer with its own HCI 0x13 kick,
    // so onDisconnect() uses the m_intentionalDrop flag regardless of reason code.
    // volatile: set on the loop task, cleared by onDisconnect() on the NimBLE
    // task, and read by sendKeepalive()/handle() in between.
    volatile bool m_intentionalDrop = false;
    volatile bool m_stopKeepalives  = false;

    // Bounded escalation for the keepalive-zombie path (re-review of spec
    // 4.17): when a heartbeat write fails but the link layer is still up we
    // stop keepalives and let the supervision timeout drop the link (the
    // lighter kick path). If the pad negotiated a long supervision timeout that
    // wait is open-ended while the belt may be running and we are mute, so
    // handle() forces disconnect() once this deadline passes with the link still
    // up. 0 = no escalation pending. Deadline = 2x the negotiated supervision
    // timeout, clamped to [kDropEscalateMinMs, kDropEscalateMaxMs].
    uint32_t m_dropDeadlineMs = 0;
    // Negotiated supervision timeout in ms, captured in completeSetup(); falls
    // back to the 6 s we request if the stack reports 0.
    uint32_t m_connTimeoutMs = 6000;
    static constexpr uint32_t kDropEscalateMinMs = 12000;
    static constexpr uint32_t kDropEscalateMaxMs = 30000;

    // Set by notifyCallback() (Core 0) when a fresh BLE packet is ready.
    // Cleared by handle() (Core 1), which reports it to the caller. This keeps ALL
    // observer notifications on the loop task — MQTT publishing happens on the net
    // task via a queue, and nothing else may touch PubSubClient.
    volatile bool m_newDataAvailable = false;

    TreadmillState m_state;

    // All session accounting (baselines, deltas, pause/resume, live totals) lives
    // here. Declared after m_state so the reference is bound to a live object.
    SessionTracker m_tracker{m_state, *this};

    // --- ISessionEvents ---------------------------------------------------
    // These carry exactly the timer/flag code that used to sit inline in
    // notifyCallback() at each of these points.

    void onSessionStarted() override;
    void onSessionEnded() override;
    void onPaused() override;

    void onResumed() override
    {
        m_pauseTimeoutDeadline = 0;
    }

    void onSettledIdle() override;

    void onConnect(BLEClient *pClient) override;
    void onDisconnect(BLEClient *pClient, int reason) override;

    const uint8_t CONNECTION_TIMEOUT = 30;
};