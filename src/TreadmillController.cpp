#include "TreadmillController.h"

#include <math.h>

TreadmillController::TreadmillController(ITreadmillLink& link)
    : m_link(link)
{
}

void TreadmillController::addObserver(ISnapshotObserver& obs)
{
    if (m_observerCount < MAX_OBSERVERS)
    {
        m_observers[m_observerCount++] = &obs;
    }
}

void TreadmillController::notifySnapshot(const TreadMillData& d)
{
    for (uint8_t i = 0; i < m_observerCount; ++i)
    {
        m_observers[i]->onSnapshot(d);
    }
}

void TreadmillController::notifyTarget(float mph, bool pending)
{
    for (uint8_t i = 0; i < m_observerCount; ++i)
    {
        m_observers[i]->onTargetSpeed(mph, pending);
    }
}

void TreadmillController::publishOptimistic(const TreadMillData& d)
{
    m_link.publishOptimistic(d);
    notifySnapshot(d);
}

void TreadmillController::notifyLinkSnapshot()
{
    notifySnapshot(m_link.snapshot());
}

float TreadmillController::clampMph(float mph)
{
    if (mph < SPEED_MIN_MPH) return SPEED_MIN_MPH;
    if (mph > SPEED_MAX_MPH) return SPEED_MAX_MPH;
    return mph;
}

float TreadmillController::roundToStep(float mph)
{
    return roundf(mph / SPEED_STEP_MPH) * SPEED_STEP_MPH;
}

// Shared mph -> raw conversion used by resume() and setSpeedMph(): scale to
// the belt's raw units and cap at SPEED_RAW_MAX.
static uint16_t rawFromMph(float mph)
{
    uint16_t raw = (uint16_t)lroundf(mph * SPEED_RAW_PER_MPH);
    if (raw > SPEED_RAW_MAX) raw = SPEED_RAW_MAX;
    return raw;
}

void TreadmillController::start()
{
    if (!m_link.start())
    {
        // The write failed and the link has already put DISCONNECTED in its
        // snapshot — publishing COUNTDOWN over it would hide the failure.
        notifyLinkSnapshot();
        return;
    }
    TreadMillData d = m_link.snapshot();
    d.status = TreadMillData::COUNTDOWN;
    publishOptimistic(d);
}

void TreadmillController::pause()
{
    if (!m_link.pause())
    {
        notifyLinkSnapshot();
        return;
    }
    TreadMillData d = m_link.snapshot();
    d.status = TreadMillData::PAUSED;
    publishOptimistic(d);
}

void TreadmillController::stop()
{
    if (!m_link.stop())
    {
        notifyLinkSnapshot();
        return;
    }
    TreadMillData d = m_link.snapshot();
    d.status = TreadMillData::STOPPED;
    publishOptimistic(d);
}

void TreadmillController::resume()
{
    // The paused snapshot reports STOPPED with speedCmd 0 (the Q1's own view of
    // a pause), so the speed to resume at can only come from the link's last
    // commanded value.
    uint16_t raw = m_link.lastCommandedSpeedRaw();
    if (raw < START_SPEED_RAW) raw = START_SPEED_RAW;

    if (!m_link.setSpeedRaw(raw))
    {
        notifyLinkSnapshot();
        return;
    }

    // Deliberately no optimistic RUNNING here: the belt runs its own countdown
    // on resume, so let the next notification say what actually happened.
    TreadMillData d = m_link.snapshot();
    d.speedCmd = roundToStep((float)raw / SPEED_RAW_PER_MPH);
    publishOptimistic(d);
}

void TreadmillController::toggleStartPause()
{
    // The link's pause flag outranks the belt status: the Q1 reports STOPPED
    // while paused, so the status alone would send a start instead of a resume.
    if (m_link.isPaused())
    {
        resume();
        return;
    }

    switch (m_link.snapshot().status)
    {
    case TreadMillData::DISCONNECTED:
    case TreadMillData::STOPPED:
        start();
        break;
    case TreadMillData::RUNNING:
        pause();
        break;
    case TreadMillData::PAUSED:
        resume();
        break;
    case TreadMillData::COUNTDOWN:
        stop();
        break;
    }
}

void TreadmillController::setSpeedMph(float mph)
{
    // An HA slider dragged to 0 has to mean "stop", not "creep at the minimum".
    if (mph < SPEED_STOP_BELOW_MPH)
    {
        stop();
        return;
    }

    const float clamped = clampMph(mph);
    uint16_t raw = rawFromMph(clamped);

    TreadMillData d = m_link.snapshot();
    const bool wasIdle = (d.status == TreadMillData::STOPPED ||
                          d.status == TreadMillData::DISCONNECTED);

    if (!m_link.setSpeedRaw(raw))
    {
        notifyLinkSnapshot();
        return;
    }

    d.speedCmd = roundToStep(clamped);
    if (wasIdle)
    {
        // setSpeedRaw() starts the belt (queueing if disconnected), so the belt
        // is about to run its countdown.
        d.status = TreadMillData::COUNTDOWN;
    }
    publishOptimistic(d);
}

void TreadmillController::nudgeSpeed(int clicks, uint32_t nowMs)
{
    if (!m_nudgePending)
    {
        if (m_link.isPaused())
        {
            // Paused: the snapshot's speedCmd is 0, so seed from the speed the
            // belt was last told to run at.
            m_targetMph = clampMph((float)m_link.lastCommandedSpeedRaw() / SPEED_RAW_PER_MPH);
        }
        else
        {
            const TreadMillData d = m_link.snapshot();
            const bool live = (d.status == TreadMillData::RUNNING ||
                               d.status == TreadMillData::PAUSED ||
                               d.status == TreadMillData::COUNTDOWN);
            m_targetMph = (live && d.speedCmd >= SPEED_MIN_MPH) ? d.speedCmd : SPEED_MIN_MPH;
        }
    }

    m_targetMph = roundToStep(clampMph(m_targetMph + clicks * SPEED_STEP_MPH));
    m_nudgePending   = true;
    m_settleDeadline = nowMs + SPEED_SETTLE_MS;
    notifyTarget(m_targetMph, true);
}

void TreadmillController::tick(uint32_t nowMs)
{
    // Signed difference so a millis() wrap doesn't strand a pending nudge.
    if (m_nudgePending && (int32_t)(nowMs - m_settleDeadline) >= 0)
    {
        m_nudgePending = false;
        setSpeedMph(m_targetMph);
        notifyTarget(m_targetMph, false);
    }
}

bool TreadmillController::requestConnect()
{
    const bool started = m_link.requestConnect();
    if (!started)
    {
        // Already connected — re-notify so a view that flipped its switch
        // optimistically is corrected.
        notifySnapshot(m_link.snapshot());
    }
    return started;
}

bool TreadmillController::requestDisconnect()
{
    const bool wasConnected = m_link.isConnected();
    const bool started      = m_link.requestDisconnect();
    if (!started)
    {
        // Not connected, or the link refused because the belt is still active.
        // Either way the view's optimistic OFF is wrong — push the truth back.
        notifySnapshot(m_link.snapshot());
        return false;
    }
    if (wasConnected)
    {
        // Disconnect is in flight; don't make views wait for the BLE callback.
        TreadMillData d = m_link.snapshot();
        d.status = TreadMillData::DISCONNECTED;
        publishOptimistic(d);
    }
    return true;
}

void TreadmillController::publish()
{
    notifySnapshot(m_link.snapshot());
}

void TreadmillController::publishNetStatus(NetStatus s)
{
    for (uint8_t i = 0; i < m_observerCount; ++i)
    {
        m_observers[i]->onNetStatus(s);
    }
}
