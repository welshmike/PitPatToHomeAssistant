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
        saveSettings();
    }

    bool getAutoReconnect() const
    {
        return m_autoReconnect;
    }

    void setIdleDisconnectMins(uint16_t mins)
    {
        m_idleDisconnectMins = mins;
        saveSettings();
        // If the timer is currently armed, rearm it with the new duration.
        if (m_idleDisconnectDeadline != 0)
        {
            if (mins == 0)
            {
                m_idleDisconnectDeadline = 0;
                log_i("Idle disconnect timer disarmed (mins=0)");
            }
            else
            {
                m_idleDisconnectDeadline = m_idleDisconnectArmedAt + (uint32_t)mins * 60000UL;
                unsigned long elapsed = millis() - m_idleDisconnectArmedAt;
                log_i("Idle disconnect timer rearmed: %u min (%lu s elapsed)",
                      mins, elapsed / 1000);
            }
        }
    }

    uint16_t getIdleDisconnectMins() const
    {
        return m_idleDisconnectMins;
    }

    void setPauseTimeoutMins(uint16_t mins)
    {
        m_pauseTimeoutMins = mins;
        saveSettings();
        // If the timer is currently armed, rearm it with the new duration.
        if (m_pauseTimeoutDeadline != 0)
        {
            if (mins == 0)
            {
                m_pauseTimeoutDeadline = 0;
                log_i("Pause timeout timer disarmed (mins=0)");
            }
            else
            {
                m_pauseTimeoutDeadline = m_pauseTimeoutArmedAt + (uint32_t)mins * 60000UL;
                unsigned long elapsed = millis() - m_pauseTimeoutArmedAt;
                log_i("Pause timeout timer rearmed: %u min (%lu s elapsed)",
                      mins, elapsed / 1000);
            }
        }
    }

    uint16_t getPauseTimeoutMins() const
    {
        return m_pauseTimeoutMins;
    }

    // Dynamic Step Calibration
    void toggleCalibration() { m_state.toggleCalibration(m_lastData.speedFeedback); }
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
    void restoreTotals(float distKm, uint32_t steps, uint32_t calories, uint32_t durationSec) {
        m_state.restoreTotals(distKm, steps, calories, durationSec);
        m_lastData.totalDistanceKm  = m_state.getTotalDistanceKm();
        m_lastData.totalSteps       = m_state.getTotalSteps();
        m_lastData.totalCalories    = m_state.getTotalCalories();
        m_lastData.totalDurationSec = m_state.getTotalDurationSec();
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
    NimBLEAddress m_targetAddress;
    bool m_doConnect = false;
    bool m_autoReconnect = true;

    unsigned long m_lastConnectAttempt = 0;

    unsigned long m_lastDataTimestamp = 0;
    TreadMillData m_lastData;

    // Set true in connectToDevice() so notifyCallback() can log the first post-connect
    // packet in full — helps diagnose the phantom RUNNING state on connect.
    volatile bool m_firstPacketAfterConnect = false;

    // Set by onConnect() (Core 0) when GATT is ready. handle() (Core 1) sends the
    // initial keepalive — writeValue(true) cannot be called from a NimBLE callback
    // because it blocks waiting for an ATT response processed by the same task.
    volatile bool m_sendInitNow = false;

    // Belt odometer values recorded at the START of the current BLE connection.
    // Used to compute per-connection deltas so that reconnects mid-session don't
    // cause the same distance/calories/duration to be committed multiple times.
    // Set on the first full packet after connect (notifyCallback); reset per-connection.
    float    m_connectionBaseDistKm  = 0.0f;
    uint16_t m_connectionBaseCal     = 0;
    uint32_t m_connectionBaseDurSec  = 0;

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

    // Pending command — queued when start()/setSpeed() is called while disconnected.
    // Executed in handle() once reconnected and POST_CONNECT_COOLDOWN has elapsed.
    enum class PendingCmd : uint8_t { NONE, START, SET_SPEED };
    PendingCmd m_pendingCmd   = PendingCmd::NONE;
    uint16_t   m_pendingSpeed = 0;

    // Intentional drop via supervision timeout — set before we stop keepalives so
    // that onDisconnect() knows the disconnect was deliberate, not radio loss.
    // In practice Q1 beats the 6s supervision timer with its own HCI 0x13 kick,
    // so onDisconnect() uses the m_intentionalDrop flag regardless of reason code.
    bool m_intentionalDrop = false;
    bool m_stopKeepalives  = false;

    // Set by notifyCallback() (Core 0) when a fresh BLE packet is ready.
    // Cleared by handle() (Core 1) after publishing. This keeps ALL m_onDataUpdate
    // calls on Core 1 — PubSubClient is not thread-safe, so calling publish() from
    // both cores concurrently corrupts its state and causes ECONNRESET from the broker.
    volatile bool m_newDataAvailable = false;

    TreadmillState m_state;


    void onConnect(BLEClient *pClient) override
    {
        log_i("Connected to device!");
        m_sendInitNow = true;
#if HAS_STATUS_LED
        digitalWrite(LED_BLE_PIN, HIGH); // active-high: on
#endif
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
        log_w("Disconnected reason=%d (HCI 0x%02X: %s)",
              reason, reason - 512, desc);

        // Intentional drop via supervision timeout — we stopped keepalives deliberately
        // so Q1 sees a supervision timeout (0x08) rather than HCI 0x16 (local host terminated).
        // Clear both flags regardless of the actual reason code; if something else fired
        // first (e.g., Q1 kicked us), we still want to clean up.
        if (m_intentionalDrop)
        {
            if ((reason - 512) == 0x08)
                log_i("Intentional drop: supervision timeout fired as expected — Q1 should see lighter kick phase");
            else
                log_w("Intentional drop: expected 0x08 but got HCI 0x%02X — cleaning up anyway", reason - 512);
            m_intentionalDrop = false;
            m_stopKeepalives  = false;
        }

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
                float dDist = m_lastData.distanceKm - m_connectionBaseDistKm;
                if (dDist < 0.0f) dDist = m_lastData.distanceKm;
                uint32_t dSteps = (uint32_t)(dDist * 1000.0f / m_state.getDynamicStepLength(m_lastData.speedFeedback));
                uint32_t dCal = (m_lastData.calories >= m_connectionBaseCal)
                                ? m_lastData.calories - m_connectionBaseCal : m_lastData.calories;
                uint32_t dDur = (m_lastData.durationSec >= m_connectionBaseDurSec)
                                ? m_lastData.durationSec - m_connectionBaseDurSec : m_lastData.durationSec;
                log_w("Disconnect mid-session — committing delta: dist=%.3f km steps=%u cal=%u dur=%u s",
                      dDist, dSteps, dCal, dDur);
                m_state.addSession(dDist, dSteps, dCal, dDur);
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
                // User explicitly pressed Connect — use a short 5s backoff so the Q1's
                // kick phase (~5-8 cycles) settles in ~2 min. The longer 20s backoff
                // made the kick phase take 4+ min (30s/cycle × 8 kicks), which felt
                // like the device was broken.
                m_reconnectNotBefore = millis() + 5000UL;
                log_w("Idle kick (user-requested) — backing off 5s before reconnect");
            }
            else if (m_autoReconnect)
            {
                // Auto-reconnect (background, no user session): back off 20s to keep
                // beeping infrequent while working through the kick phase.
                m_reconnectNotBefore = millis() + 20000UL;
                log_w("Idle kick (auto-reconnect) — backing off 20s before reconnect");
            }
            // else: m_autoReconnect is already false (session ended) — don't reconnect.
        }
        else if (isIdleKick && beltWasActive)
        {
            // Mid-session kick: Q1 kicked us (reason=531) while the belt was active.
            // Without backoff we reconnect immediately, Q1 kicks again, and this loops
            // for minutes. Add a 10s pause to let Q1 cycle out of its kicking phase
            // before the next connect attempt (~10s connect + 10s wait = ~20s/cycle).
            m_reconnectNotBefore = millis() + 10000UL;
            log_w("Mid-session kick — backing off 10s before reconnect to work through kicking phase");
        }

        // Disarm both timers — either they fired and triggered this disconnect, or we
        // were kicked/timed-out externally. Either way, nothing left to fire.
        m_idleDisconnectDeadline = 0;
        m_pauseTimeoutDeadline   = 0;

        // Clear pending command only when we're not going to reconnect.
        // If m_autoReconnect is still true (kicked while connecting to execute a
        // command), preserve the command so it fires once the belt settles.
        // If we're disconnecting cleanly (pause timeout, user disconnect), the
        // command is stale and should be dropped.
        if (!m_autoReconnect)
        {
            m_pendingCmd = PendingCmd::NONE;
        }

#if HAS_STATUS_LED
        digitalWrite(LED_BLE_PIN, LOW); // active-high: off
#endif

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

        if (m_autoReconnect)
        {
            m_doConnect = true; // Trigger reconnect in handle() loop
        }
    }

    std::function<void(const TreadMillData&)> m_onDataUpdate = nullptr;

    const uint8_t CONNECTION_TIMEOUT = 30;
};