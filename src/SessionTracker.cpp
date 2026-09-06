#include "SessionTracker.h"

#ifndef NATIVE_TEST
// -DUSE_ESP_IDF_LOG (spec 4.14) makes TRACKER_LOG_I/W expand, via log_x(), to
// ESP_LOG_LEVEL_LOCAL(..., TAG, ...); esp32-hal-log.h has no default TAG.
static const char *TAG = "Session";
#endif

SessionTracker::SessionTracker(ITotalsStore& totals, ISessionEvents& events)
    : m_totals(totals), m_events(events)
{
}

void SessionTracker::onConnected(uint32_t nowMs)
{
    // Reset ALL per-connection state. Called from connectToDevice() before
    // subscribing, so any notification arriving during the rest of the connect
    // sequence sees a clean slate.
    //
    // Deliberately NOT reset here (matching the original code): m_lastStatus,
    // m_pausedDistKm/Steps/Calories/DurationSec and m_resumeDistanceM. The paused
    // snapshot must survive a mid-pause reconnect so it can still be committed.
    m_connectedAtMs           = nowMs;
    m_firstPacketAfterConnect = true;
    m_lastPacketType          = 0;
    m_connectionBaseDistKm    = 0.0f;
    m_connectionBaseCal       = 0;
    m_connectionBaseDurSec    = 0;
    m_sessionActive           = false;
    m_isPaused                = false;
    m_justResumedFromPause    = false;
}

SessionDelta SessionTracker::computeDelta(float distKm, uint32_t cal, uint32_t dur,
                                          float speedMph) const
{
    SessionDelta d;
    d.distKm = distKm - m_connectionBaseDistKm;
    if (d.distKm < 0.0f) d.distKm = distKm; // belt reset its odometer — never go negative
    d.steps       = (uint32_t)(d.distKm * 1000.0f / m_totals.stepLengthM(speedMph));
    d.calories    = (cal >= m_connectionBaseCal)    ? cal - m_connectionBaseCal    : cal;
    d.durationSec = (dur >= m_connectionBaseDurSec) ? dur - m_connectionBaseDurSec : dur;
    return d;
}

void SessionTracker::commitPausedSnapshot()
{
    SessionDelta d;
    d.distKm      = m_pausedDistKm;
    d.steps       = m_pausedSteps;
    d.calories    = m_pausedCalories;
    d.durationSec = m_pausedDurationSec;
    m_totals.addSession(d);
}

TreadMillData SessionTracker::onPacket(const BA05Protocol::ParsedData& parsed,
                                       const TreadMillData& prev,
                                       uint32_t nowMs)
{
    TreadMillData data = prev;
    TreadMillData::Status previousStatus = m_lastStatus;
    uint8_t packetType = parsed.packetType;

    if (packetType != m_lastPacketType)
    {
        unsigned long msAfterConnect = (unsigned long)(nowMs - m_connectedAtMs);
        if (m_lastPacketType == 0)
        {
            TRACKER_LOG_I("First packet: type=0x%02X len=%d at t=+%lums after connect",
                          (unsigned)packetType, (int)parsed.length, msAfterConnect);
        }
        else
        {
            TRACKER_LOG_I("Packet type changed: 0x%02X → 0x%02X at t=+%lums after connect",
                          (unsigned)m_lastPacketType, (unsigned)packetType, msAfterConnect);
        }
        m_lastPacketType = packetType;

        // Fix 2: arm idle timer when the device settles to stable 0x2F mode with
        // no active session. Covers: manual reconnect to idle belt, post-pause-timeout
        // reconnect, and boot connect to an already-idle belt.
        // Without this, Q1's own ~3.5h idle kick fires while m_autoReconnect=true
        // (left true from the initial connect intent), triggering an infinite reconnect loop.
        if (packetType == 0x2F &&
            parsed.status == TreadMillData::STOPPED &&
            !m_sessionActive &&
            !m_isPaused)
        {
            m_events.onSettledIdle();
        }
    }

    if (parsed.length >= 50 && (packetType == 0x2F || packetType == 0x34)) {
        data.speedFeedback = parsed.speedFeedback;
        data.speedCmd      = parsed.speedCmd;
        data.speedMax      = parsed.speedMax;
        data.fwVersion     = 0;
        data.status        = parsed.status;

        if (m_firstPacketAfterConnect)
        {
            m_firstPacketAfterConnect = false;
            // Record belt odometer at connection start. Every addSession() call uses
            // (current - base) so that reconnects mid-session don't double-count.
            m_connectionBaseDistKm  = parsed.distanceM / 1000.0f;
            m_connectionBaseCal     = parsed.calories;
            m_connectionBaseDurSec  = parsed.durationSec;
            // statusName covers the four belt-reported values (0..3); DISCONNECTED
            // is 100, so it has to be mapped explicitly rather than indexed.
            const char* statusName[] = {"COUNTDOWN","RUNNING","PAUSED","STOPPED"};
            const char* sn = ((int)data.status < 4)         ? statusName[(int)data.status]
                             : (data.status == TreadMillData::DISCONNECTED) ? "DISCONNECTED"
                                                                           : "UNKNOWN";
            TRACKER_LOG_W("FIRST POST-CONNECT PACKET: type=0x%02X len=%d → status=%s "
                          "speed=%.3f dist=%dm dur=%u cal=%u (baseline captured)",
                          (unsigned)packetType, (int)parsed.length, sn,
                          parsed.speedFeedback, (int)parsed.distanceM,
                          parsed.durationSec, (unsigned)parsed.calories);
            // The raw hex dump that used to follow is emitted by TreadmillHandler,
            // which still has the packet bytes.
        }

        // STATUS TRANSITION LOGGING — temporary, for pause vs stop diagnosis
        if (data.status != previousStatus)
        {
            auto statusStr = [](TreadMillData::Status s) -> const char* {
                switch(s) {
                    case TreadMillData::COUNTDOWN:    return "COUNTDOWN";
                    case TreadMillData::RUNNING:      return "RUNNING";
                    case TreadMillData::PAUSED:       return "PAUSED";
                    case TreadMillData::STOPPED:      return "STOPPED";
                    case TreadMillData::DISCONNECTED: return "DISCONNECTED";
                    default:                          return "UNKNOWN";
                }
            };
            TRACKER_LOG_W("STATUS: %s → %s  (flags=0x%02X  speed=%.2f  dist=%um  active=%d)",
                          statusStr(previousStatus), statusStr(data.status),
                          (unsigned)parsed.statusFlags,
                          parsed.speedFeedback, (unsigned)parsed.distanceM,
                          (int)m_sessionActive);
        }

        m_lastStatus = data.status;

        if (data.status == TreadMillData::RUNNING && data.speedFeedback > 0.001f)
        {
            if (m_isPaused && previousStatus == TreadMillData::STOPPED)
            {
                if (parsed.distanceM > 0)
                {
                    // Belt resumed from where it left off — session continues, no commit.
                    // Mark that we just resumed so we can suppress the belt's startup
                    // RUNNING→STOPPED→RUNNING sequence that fires in the next ~500 ms.
                    TRACKER_LOG_I("Resumed from pause (dist=%um) — session continues",
                                  (unsigned)parsed.distanceM);
                    m_justResumedFromPause = true;
                    m_resumeDistanceM      = parsed.distanceM;
                }
                else
                {
                    // Belt reset its odometer — commit the stored paused session, then start fresh
                    TRACKER_LOG_I("Belt reset after pause — committing paused session: dist=%.3f km steps=%u",
                                  m_pausedDistKm, m_pausedSteps);
                    data.sessionDistanceKm  = m_pausedDistKm;
                    data.sessionSteps       = m_pausedSteps;
                    data.sessionCalories    = m_pausedCalories;
                    data.sessionDurationSec = m_pausedDurationSec;
                    data.sessionComplete    = true;
                    commitPausedSnapshot();
                }
                m_isPaused = false;
                // Cancel pause timeout — belt has resumed.
                m_events.onResumed();
            }
            m_sessionActive = true;

            // Cancel idle-disconnect timer — belt is active.
            m_events.onSessionStarted();

            // If the belt's odometer is below our connection baseline, the user
            // pressed START and the belt reset its session counter since we connected.
            // Reset the bases to 0 so all deltas are relative to this walk's start.
            // This prevents long-session undercount (residual odometer from last session
            // at connect time was being subtracted from the committed delta).
            // NOTE: use parsed.distanceM directly — data.distanceKm is not yet set at
            // this point in the function (it's populated a few lines below).
            if (parsed.distanceM / 1000.0f < m_connectionBaseDistKm) {
                m_connectionBaseDistKm  = 0.0f;
                m_connectionBaseCal     = 0;
                m_connectionBaseDurSec  = 0;
                TRACKER_LOG_I("Belt odometer reset detected — connection bases updated to 0");
            }
        }

        if (data.status != TreadMillData::STOPPED)
        {
            data.distanceKm  = parsed.distanceM / 1000.0f;
            data.calories    = parsed.calories;
            data.steps       = (uint32_t)(parsed.distanceM / m_totals.stepLengthM(data.speedFeedback));
            data.durationSec = parsed.durationSec;
        }

        if (data.status == TreadMillData::STOPPED &&
            previousStatus != TreadMillData::STOPPED &&
            previousStatus != TreadMillData::DISCONNECTED &&
            data.distanceKm > 0.0f &&
            m_sessionActive)
        {
            if (m_justResumedFromPause && !m_isPaused && parsed.distanceM <= m_resumeDistanceM)
            {
                // Belt's startup RUNNING→STOPPED after a resume — distance hasn't advanced,
                // so this is not a real stop. Suppress the commit and wait for real RUNNING.
                TRACKER_LOG_I("Suppressing spurious post-resume STOPPED (dist=%um unchanged)",
                              (unsigned)parsed.distanceM);
                m_justResumedFromPause = false;
            }
            else if (m_isPaused)
            {
                // Belt reports STOPPED because we sent a pause command — defer the commit.
                // We'll commit when the belt resumes (RUNNING with dist=0 = reset) or
                // when stop()/onDisconnect() is called explicitly.
                SessionDelta p = computeDelta(data.distanceKm, data.calories,
                                              data.durationSec, data.speedFeedback);
                m_pausedDistKm      = p.distKm;
                m_pausedSteps       = p.steps;
                m_pausedCalories    = p.calories;
                m_pausedDurationSec = p.durationSec;
                m_justResumedFromPause = false; // clear in case user re-paused right after resume
                TRACKER_LOG_I("Pause-stop: deferring commit (dist=%.3f km steps=%u) — waiting for resume or stop",
                              m_pausedDistKm, m_pausedSteps);

                // Arm pause timeout — if the user never resumes, commit and disconnect.
                m_events.onPaused();
            }
            else
            {
                // Session summary: show the full belt distance for display in HA.
                data.sessionDistanceKm  = data.distanceKm;
                data.sessionSteps       = data.steps;
                data.sessionCalories    = (uint32_t)data.calories;
                data.sessionDurationSec = data.durationSec;
                data.sessionComplete    = true;

                // Totals: commit only the delta since this BLE connection started,
                // so reconnects mid-session don't double-count earlier distance.
                SessionDelta s = computeDelta(data.distanceKm, data.calories,
                                              data.durationSec, data.speedFeedback);
                m_totals.addSession(s);

                TRACKER_LOG_I("Session ended — totals: dist=%.2f km  steps=%u  cal=%u  dur=%u s",
                              m_totals.totalDistanceKm(), m_totals.totalSteps(),
                              m_totals.totalCalories(), m_totals.totalDurationSec());
                TRACKER_LOG_I("Session summary: dist=%.3f km  steps=%u  cal=%u  dur=%u s",
                              data.sessionDistanceKm, data.sessionSteps,
                              data.sessionCalories, data.sessionDurationSec);

                m_sessionActive        = false;
                m_justResumedFromPause = false;

                // Clear auto-reconnect flags and arm the idle-disconnect timer so BLE
                // disconnects automatically after the configured idle period. Prevents
                // the Q1 kicking loop that occurs when we stay connected with no activity.
                m_events.onSessionEnded();

                data.distanceKm  = 0.0f;
                data.calories    = 0;
                data.steps       = 0;
                data.durationSec = 0;
            }
        }

        if (data.status != TreadMillData::STOPPED && m_sessionActive)
        {
            // Use connection-relative delta, not the raw belt odometer.
            // Raw odometer causes a spike on mid-session reconnect: onDisconnect()
            // already committed the pre-reconnect delta to NVS, so adding the full
            // belt odometer again would double-count it in the HA utility_meter.
            SessionDelta l = computeDelta(data.distanceKm, data.calories,
                                          data.durationSec, data.speedFeedback);
            data.totalDistanceKm  = m_totals.totalDistanceKm()  + l.distKm;
            data.totalSteps       = m_totals.totalSteps()       + l.steps;
            data.totalCalories    = m_totals.totalCalories()    + l.calories;
            data.totalDurationSec = m_totals.totalDurationSec() + l.durationSec;
        }
        else if (m_isPaused && m_sessionActive)
        {
            // Belt is paused (STOPPED state) — hold totals at the paused snapshot.
            // Without this, total sensors drop back to bare NVS values during the pause
            // window. When the session resumes and totals rise again, utility_meter in HA
            // would count the pre-pause distance a second time (double-counting bug).
            data.totalDistanceKm  = m_totals.totalDistanceKm()  + m_pausedDistKm;
            data.totalSteps       = m_totals.totalSteps()       + m_pausedSteps;
            data.totalCalories    = m_totals.totalCalories()    + m_pausedCalories;
            data.totalDurationSec = m_totals.totalDurationSec() + m_pausedDurationSec;
        }
        else
        {
            data.totalDistanceKm  = m_totals.totalDistanceKm();
            data.totalSteps       = m_totals.totalSteps();
            data.totalCalories    = m_totals.totalCalories();
            data.totalDurationSec = m_totals.totalDurationSec();
        }

        TRACKER_LOG_D("speed=%.2f mph target=%.2f dist=%.3f km cal=%u dur=%u:%02u status=%d  "
                      "tot_dist=%.2f km tot_steps=%u",
                      data.speedFeedback, data.speedCmd, data.distanceKm,
                      data.calories, data.durationSec / 60, data.durationSec % 60, (int)data.status,
                      data.totalDistanceKm, data.totalSteps);
    }
    else if (parsed.length == 20) {
        data.speedFeedback = parsed.speedFeedback;
    }

    return data;
}

void SessionTracker::onDisconnected(const TreadMillData& last)
{
    if (m_sessionActive)
    {
        if (m_isPaused)
        {
            TRACKER_LOG_W("Disconnect while paused — committing paused session: dist=%.3f km steps=%u",
                          m_pausedDistKm, m_pausedSteps);
            commitPausedSnapshot();
            m_isPaused = false;
        }
        else if (last.distanceKm > 0.0f)
        {
            SessionDelta d = computeDelta(last.distanceKm, last.calories,
                                          last.durationSec, last.speedFeedback);
            TRACKER_LOG_W("Disconnect mid-session — committing delta: dist=%.3f km steps=%u cal=%u dur=%u s",
                          d.distKm, d.steps, d.calories, d.durationSec);
            m_totals.addSession(d);
        }
    }
}

void SessionTracker::onPauseCommand()
{
    m_isPaused = true;
}

bool SessionTracker::onStopCommand()
{
    if (m_isPaused && m_sessionActive)
    {
        // Belt is paused (stuck in STOPPED) — commit the stored paused session now,
        // since no new STOPPED transition will fire from the belt.
        TRACKER_LOG_I("Stop after pause — committing paused session: dist=%.3f km steps=%u",
                      m_pausedDistKm, m_pausedSteps);
        commitPausedSnapshot();
        m_sessionActive = false;
        m_isPaused      = false;
        return true;
    }
    return false;
}

TreadMillData SessionTracker::onPauseTimeout(const TreadMillData& last)
{
    // Commit the stored paused session snapshot.
    commitPausedSnapshot();
    TRACKER_LOG_I("Pause timeout session committed: dist=%.3f km  steps=%u  cal=%u  dur=%u s",
                  m_pausedDistKm, m_pausedSteps, m_pausedCalories, m_pausedDurationSec);

    // Session summary for MQTT, published before disconnecting.
    TreadMillData data = last;
    data.sessionDistanceKm  = m_pausedDistKm;
    data.sessionSteps       = m_pausedSteps;
    data.sessionCalories    = m_pausedCalories;
    data.sessionDurationSec = m_pausedDurationSec;
    data.sessionComplete    = true;
    data.totalDistanceKm    = m_totals.totalDistanceKm();
    data.totalSteps         = m_totals.totalSteps();
    data.totalCalories      = m_totals.totalCalories();
    data.totalDurationSec   = m_totals.totalDurationSec();
    data.distanceKm  = 0.0f;
    data.calories    = 0;
    data.steps       = 0;
    data.durationSec = 0;
    data.status      = TreadMillData::STOPPED;

    // Clear session flags before the caller sends stop() so it doesn't double-commit.
    m_sessionActive        = false;
    m_isPaused             = false;
    m_justResumedFromPause = false;

    return data;
}
