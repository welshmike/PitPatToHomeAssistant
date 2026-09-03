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
    m_link.start();
    TreadMillData d = m_link.snapshot();
    d.status = TreadMillData::COUNTDOWN;
    publishOptimistic(d);
}

void TreadmillController::pause()
{
    m_link.pause();
    TreadMillData d = m_link.snapshot();
    d.status = TreadMillData::PAUSED;
    publishOptimistic(d);
}

void TreadmillController::stop()
{
    m_link.stop();
    TreadMillData d = m_link.snapshot();
    d.status = TreadMillData::STOPPED;
    publishOptimistic(d);
}

void TreadmillController::resume()
{
    // The handler already remembers m_lastSpeed and would reuse it, but the
    // controller derives the resume speed from the snapshot so this rule is
    // testable on the host without a live BLE link.
    TreadMillData d = m_link.snapshot();
    uint16_t raw = (d.speedCmd >= SPEED_MIN_MPH)
                       ? rawFromMph(d.speedCmd)
                       : START_SPEED_RAW;
    m_link.setSpeedRaw(raw);

    // Deliberately no optimistic RUNNING here: the belt runs its own countdown
    // on resume, so let the next notification say what actually happened.
    d.speedCmd = roundToStep((float)raw / SPEED_RAW_PER_MPH);
    publishOptimistic(d);
}

void TreadmillController::toggleStartPause()
{
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

    m_link.setSpeedRaw(raw);

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
        const TreadMillData d = m_link.snapshot();
        const bool live = (d.status == TreadMillData::RUNNING ||
                           d.status == TreadMillData::PAUSED ||
                           d.status == TreadMillData::COUNTDOWN);
        m_targetMph = (live && d.speedCmd >= SPEED_MIN_MPH) ? d.speedCmd : SPEED_MIN_MPH;
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

void TreadmillController::requestConnect()
{
    m_link.requestConnect();
}

void TreadmillController::requestDisconnect()
{
    m_link.requestDisconnect();
}

void TreadmillController::publish()
{
    notifySnapshot(m_link.snapshot());
}
