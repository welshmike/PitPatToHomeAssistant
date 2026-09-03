#include "SpeedSelector.h"

#include <math.h>

#include "TreadmillData.h"

namespace
{

float clampAndRound(float mph)
{
    if (mph < SPEED_MIN_MPH) {
        mph = SPEED_MIN_MPH;
    } else if (mph > SPEED_MAX_MPH) {
        mph = SPEED_MAX_MPH;
    }

    return roundf(mph / SPEED_STEP_MPH) * SPEED_STEP_MPH;
}

} // namespace

void SpeedSelector::open(float defaultMph, uint32_t nowMs)
{
    m_open = true;
    m_value = clampAndRound(defaultMph);
    m_lastActivity = nowMs;
}

bool SpeedSelector::isOpen() const
{
    return m_open;
}

void SpeedSelector::step(int n, uint32_t nowMs)
{
    if (!m_open) {
        return;
    }

    m_value = clampAndRound(m_value + static_cast<float>(n) * SELECTOR_STEP_MPH);
    m_lastActivity = nowMs;
}

float SpeedSelector::value() const
{
    return m_value;
}

bool SpeedSelector::tick(uint32_t nowMs)
{
    if (!m_open) {
        return false;
    }

    if (static_cast<int32_t>(nowMs - m_lastActivity) >= static_cast<int32_t>(SELECTOR_TIMEOUT_MS)) {
        m_open = false;
        return true;
    }

    return false;
}

void SpeedSelector::close()
{
    m_open = false;
}
