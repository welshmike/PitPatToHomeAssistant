#pragma once
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>

#include "platform.h"
#include "TreadmillState.h"

class TreadmillHandler : public NimBLEClientCallbacks
{
public:
    TreadmillHandler();
    ~TreadmillHandler();
    void begin(NimBLEAddress address);
    void setSpeed(uint16_t speed);
    void start();
    void pause();
    void stop();

    void handle();

    void setAutoReconnect(const bool enable)
    {
        m_autoReconnect = enable;
    }

    bool getAutoReconnect() const
    {
        return m_autoReconnect;
    }

    // Dynamic Step Calibration
    void toggleCalibration() { m_state.toggleCalibration(m_lastData.speedFeedback); }
    uint8_t getCalibrationPointCount() const { return m_state.getCalibrationPointCount(); }
    const CalibrationPoint* getCalibrationPoints() const { return m_state.getCalibrationPoints(); }

    void setStepLength(float stepM) { m_state.setStepLength(stepM); }

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
    void restoreTotals(float distKm, uint32_t steps, uint32_t calories, uint32_t durationSec) {
        m_state.restoreTotals(distKm, steps, calories, durationSec);
    }

    float getStepLength() const
    {
        return m_state.getStepLength();
    }

    bool isConnected() const
    {
        return m_pClient && m_pClient->isConnected();
    }

    // Returns true if the action was taken, false if blocked by safety check.
    bool toggleConnection()
    {
        if (isConnected())
        {
            // Safety check — don't disconnect while belt is active
            if (m_lastData.status == TreadMillData::RUNNING ||
                m_lastData.status == TreadMillData::PAUSED ||
                m_lastData.status == TreadMillData::COUNTDOWN)
            {
                log_w("Disconnect blocked — belt is active (status=%d)", (int)m_lastData.status);
                return false;
            }
            log_i("Manual disconnect requested from HA");
            m_autoReconnect = false;       // prevent onDisconnect() from immediately reconnecting
            m_userRequestedConnect = false; // user explicitly turned off — don't auto-reconnect
            m_reconnectNotBefore = 0;
            m_pClient->disconnect();
            // Don't deleteClient here — onDisconnect() fires shortly after and handles
            // cleanup. Deleting here and again in onDisconnect() is a double-free.
            return true;
        }
        else
        {
            log_i("Manual connect requested from HA");
            m_autoReconnect = true;
            m_userRequestedConnect = true;  // user explicitly turned on — keep retrying after idle kicks
            m_reconnectNotBefore = 0;       // allow immediate first attempt
            m_doConnect = true;
            return true;
        }
    }

    TreadMillData getLastData() const
    {
        return m_lastData;
    }

    void setCallback(std::function<void(const TreadMillData&)> callback)
    {
        m_onDataUpdate = callback;
    }
    

private:
    bool sendCommand(const uint8_t *data, size_t length);
    void sendKeepalive();
    // Sent immediately after notification subscription to prevent reason=531 disconnect.
    // The Q1 expects a response within ~300ms of connect or it terminates the session.
    void sendInitSequence();
    bool connectToDevice();

    TreadMillData::Status m_lastStatus = TreadMillData::DISCONNECTED;

    // BA05 protocol state
    static constexpr uint16_t START_SPEED = 994;             // ~1.0 km/h in mph×1600 encoding
    static constexpr unsigned long KEEPALIVE_INTERVAL  = 200;  // ms — matches QZ poll rate
    static constexpr unsigned long POST_CONNECT_COOLDOWN = 3000; // ms — block commands after
                                                                  // reconnect until device settles
    uint8_t m_seqCounter = 0;
    unsigned long m_lastKeepalive = 0;
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
    NimBLERemoteCharacteristic *m_pUnlockCharacteristic = nullptr;  // secondary handshake char
    NimBLEAddress m_targetAddress;
    bool m_doConnect = false;
    bool m_autoReconnect = true;

    unsigned long m_lastConnectAttempt = 0;

    unsigned long m_lastDataTimestamp = 0;
    TreadMillData m_lastData;

    // Set true in connectToDevice() so notifyCallback() can log the first post-connect
    // packet in full — helps diagnose the phantom RUNNING state on connect.
    volatile bool m_firstPacketAfterConnect = false;

    // Set true by notifyCallback() (Core 0) when a 0x34 packet is received, to request
    // an immediate keepalive response. NEVER call sendKeepalive()/writeValue() directly
    // from notifyCallback() — writeValue(true) blocks waiting for an ATT acknowledgement
    // that is processed by the same NimBLE task, causing a self-deadlock and watchdog reset.
    // handle() (Core 1) reads this flag, clears it, and sends the keepalive safely.
    volatile bool m_sendKeepaliveNow = false;

    // Set true by notifyCallback() (Core 0) when the packet type FIRST changes TO 0x34
    // (i.e. once per connection, on 0x34 onset). handle() (Core 1) re-sends the PitPat
    // unlock bytes to 0x2b11 as an explicit challenge response.
    //
    // During active-walk reconnects the pad sends 0x34 before accepting a stable 0x2F
    // session. Our init sequence sends unlock BEFORE the first notification arrives, but
    // the pad may only honour the unlock if it is sent AFTER the 0x34 challenge is issued.
    // Re-sending unlock in response to 0x34 onset tests this hypothesis.
    volatile bool m_sendUnlockNow = false;

    // Tracks the packet type byte (0x2F or 0x34) seen most recently.
    // Reset to 0 on each new connection. Used to log the moment the device
    // transitions from the 0x34 "kicking phase" to stable 0x2F mode.
    uint8_t m_lastPacketType = 0;

    // True once the belt has physically moved (speed > 0) in the current BLE session.
    // Reset on each new connection. Used by onDisconnect() as the ground-truth indicator
    // of "belt was genuinely active" — immune to phantom RUNNING status bytes (0x34 packets)
    // and residual distance from previous sessions.
    volatile bool m_sessionActive = false;

    // Set when we send a pause command. Suppresses the STOPPED commit that the belt
    // fires immediately after pause. Cleared when belt resumes (RUNNING) or on stop/disconnect.
    // On resume: if belt dist > 0 → session continues; if dist = 0 → belt reset, commit stored snapshot.
    volatile bool m_isPaused = false;
    float    m_pausedDistKm      = 0.0f;
    uint32_t m_pausedSteps       = 0;
    uint32_t m_pausedCalories    = 0;
    uint32_t m_pausedDurationSec = 0;

    // Set when a pause-resume is detected. The belt fires a brief RUNNING→STOPPED startup
    // sequence before settling into real RUNNING, so we suppress that spurious STOPPED commit.
    // Written by notifyCallback (Core 0), read by notifyCallback (Core 0) only — no volatile needed.
    bool     m_justResumedFromPause = false;
    uint32_t m_resumeDistanceM      = 0;

    // Set when the user explicitly requests a connect from HA (not auto-connect at boot).
    // Determines idle-kick behaviour: if the user asked to connect, keep retrying with a
    // 60-second backoff instead of stopping entirely. Cleared when user disconnects,
    // or after a real walking session ends (belt STOPPED after actual activity).
    bool m_userRequestedConnect = false;

    // millis() timestamp — don't attempt reconnect before this time.
    // Set after an idle kick when m_userRequestedConnect is true to back off 80s,
    // giving a ~90s cycle (80s wait + ~10s idle before next kick) = ~2 beeps/90s.
    unsigned long m_reconnectNotBefore = 0;

    // Set by notifyCallback() (Core 0) when a fresh BLE packet is ready.
    // Cleared by handle() (Core 1) after publishing. This keeps ALL m_onDataUpdate
    // calls on Core 1 — PubSubClient is not thread-safe, so calling publish() from
    // both cores concurrently corrupts its state and causes ECONNRESET from the broker.
    volatile bool m_newDataAvailable = false;

    TreadmillState m_state;


    void onConnect(BLEClient *pClient) override
    {
        log_i("Connected to device!");
    }

    void onDisconnect(BLEClient *pClient, int reason) override
    {
        // NimBLE reason = 512 + HCI error code. Common codes:
        //   0x08 (520) = connection timeout       — radio loss / device out of range
        //   0x13 (531) = remote user terminated   — device actively closed the connection
        //   0x16 (534) = local host terminated    — we called disconnect()
        //   0x3E (574) = failed to establish      — connect() timed out
        const char* desc = "unknown";
        switch (reason - 512) {
            case 0x08: desc = "connection timeout";             break;
            case 0x13: desc = "remote user terminated";         break;
            case 0x16: desc = "local host terminated";          break;
            case 0x3E: desc = "failed to establish connection"; break;
        }
        log_w("Disconnected reason=%d (HCI 0x%02X: %s) – will reconnect...",
              reason, reason - 512, desc);

        if (m_sessionActive)
        {
            if (m_isPaused)
            {
                log_w("Disconnect while paused — committing paused session: dist=%.3f km steps=%u",
                      m_pausedDistKm, m_pausedSteps);
                m_state.addSession(m_pausedDistKm, m_pausedSteps, m_pausedCalories, m_pausedDurationSec);
                m_isPaused = false;
            }
            else if (m_lastData.distanceKm > 0.0f)
            {
                log_w("Disconnect mid-session — committing live session: dist=%.3f km steps=%u cal=%u dur=%u s",
                      m_lastData.distanceKm, m_lastData.steps,
                      (uint32_t)m_lastData.calories, m_lastData.durationSec);
                m_state.addSession(m_lastData.distanceKm, m_lastData.steps,
                                   (uint32_t)m_lastData.calories, m_lastData.durationSec);
            }
        }

        // Reason 531 (HCI 0x13: remote user terminated) = treadmill kicked us.
        // When the belt is idle (stopped/disconnected) this is just the treadmill's
        // ~10s idle BLE timeout. Reconnecting immediately causes the pad to beep on
        // every connect/disconnect cycle. Instead, treat it like a manual disconnect —
        // stop auto-reconnecting and let the user reconnect when they want to walk.
        // If the belt was RUNNING/PAUSED/COUNTDOWN when kicked, reconnect immediately
        // to preserve the active session.
        bool isIdleKick = ((reason - 512) == 0x13);
        // Use m_sessionActive as the ground truth for "belt was genuinely active".
        // This flag is only set when we observe speed > 0 (belt physically moving),
        // so it's immune to:
        //   - Phantom RUNNING status from 0x34 packets (flags=0xBC with speed=0)
        //   - Residual distanceKm from a previous session persisting on reconnect
        bool beltWasActive = m_sessionActive;

        if (isIdleKick && !beltWasActive)
        {
            if (m_userRequestedConnect)
            {
                // User explicitly asked to connect from HA — back off 80s between
                // reconnects (~90s cycle including the ~10s idle window before the kick).
                m_reconnectNotBefore = millis() + 80000UL;
                log_w("Idle kick — user-requested connect, backing off 80s before reconnect");
            }
            else if (m_autoReconnect)
            {
                // Auto-connect (boot or nightly reconnect). The Q1 goes through a
                // ~1-2 minute "kicking phase" after certain state changes where it
                // terminates every connection after ~10s, then accepts a stable one.
                // Keep reconnecting to work through this phase, but back off 30s to
                // reduce beeping (was every 10s, now every ~40s = ~3 beeps/min → stable).
                m_reconnectNotBefore = millis() + 30000UL;
                log_w("Idle kick (auto-connect) — backing off 30s before reconnect to work through kicking phase");
            }
            // else: m_autoReconnect is already false (session ended) — don't reconnect.
        }

        // Always publish DISCONNECTED so HA switch reflects the real state.
        m_lastData.status = TreadMillData::DISCONNECTED;
        m_newDataAvailable = true;

        // Do NOT deleteClient here — calling NimBLEDevice::deleteClient() from within
        // a NimBLE callback (Core 0) is unsafe and corrupts the NimBLE heap, causing
        // subsequent connections to be unstable. The original pacekeeper code never
        // deletes the client — it reuses it, letting connect(addr, true, true) clear
        // the GATT cache on the next connect attempt.
        // Null the characteristic pointers — they're stale after disconnect and
        // connectToDevice() re-fetches them on the next successful connection.
        m_pWriteCharacteristic  = nullptr;
        m_pNotifyCharacteristic = nullptr;
        m_pUnlockCharacteristic = nullptr;

        if (m_autoReconnect)
        {
            m_doConnect = true; // Trigger reconnect in handle() loop
        }
    }

    std::function<void(const TreadMillData&)> m_onDataUpdate = nullptr;

    const uint8_t CONNECTION_TIMEOUT = 30;
};