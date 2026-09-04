#include "LightCardState.h"

#include <math.h>

namespace
{

// Wrap-safe "has at least thresholdMs elapsed from since to now", matching
// the pattern used by SpeedSelector's inactivity timeout.
bool elapsedAtLeast(uint32_t now, uint32_t since, uint32_t thresholdMs)
{
    return static_cast<int32_t>(now - since) >= static_cast<int32_t>(thresholdMs);
}

float clamp01(float v)
{
    if (v < 0.0f)
    {
        return 0.0f;
    }
    if (v > 1.0f)
    {
        return 1.0f;
    }
    return v;
}

int clampInt(int v, int lo, int hi)
{
    if (v < lo)
    {
        return lo;
    }
    if (v > hi)
    {
        return hi;
    }
    return v;
}

} // namespace

LightCardState::LightCardState(bool hasColour) : m_hasColour(hasColour)
{
}

void LightCardState::sync(const LightsModel::LightState& s)
{
    if (!m_settling)
    {
        m_view = s;
        return;
    }

    // Settling: adopt everything from HA except the field the user is
    // currently editing, so the pending edit survives a stale HA echo.
    LightsModel::LightState merged = s;
    switch (m_engaged)
    {
    case Engaged::BRIGHT:
        merged.brightnessPct = m_view.brightnessPct;
        break;
    case Engaged::TEMP:
        // Re-clamp into the freshly-adopted [minKelvin, maxKelvin] in case
        // sync() narrowed the bulb's range out from under the pending edit.
        merged.kelvin = static_cast<uint16_t>(
            clampInt(m_view.kelvin, merged.minKelvin, merged.maxKelvin));
        break;
    case Engaged::HUE:
        merged.hue = m_view.hue;
        merged.sat = m_view.sat;
        break;
    case Engaged::NONE:
        break;
    }
    m_view = merged;
}

void LightCardState::release()
{
    m_engaged = Engaged::NONE;
    m_settling = false;
}

LightsModel::Command LightCardState::tapButton(Button b, uint32_t nowMs)
{
    LightsModel::Command cmd; // Type::NONE

    switch (b)
    {
    case Button::NONE:
        break;

    case Button::POWER:
        if (!m_view.valid)
        {
            break;
        }
        m_view.on = !m_view.on;
        release();
        m_lastInput = nowMs;
        cmd.type = LightsModel::Command::Type::POWER;
        cmd.on = m_view.on;
        break;

    case Button::BRIGHT:
        if (!m_view.available)
        {
            break;
        }
        m_lastInput = nowMs;
        if (m_engaged == Engaged::BRIGHT)
        {
            if (m_settling)
            {
                cmd = buildSettleCommand();
            }
            release();
        }
        else
        {
            release();
            m_engaged = Engaged::BRIGHT;
        }
        break;

    case Button::COLOUR:
    {
        bool colourSupported = m_hasColour && m_view.supportsColor;
        if (!colourSupported || !m_view.available)
        {
            break;
        }
        m_lastInput = nowMs;
        if (m_engaged == Engaged::TEMP)
        {
            release();
            m_engaged = Engaged::HUE;
        }
        else if (m_engaged == Engaged::HUE)
        {
            if (m_settling)
            {
                cmd = buildSettleCommand();
            }
            release();
        }
        else
        {
            release();
            m_engaged = (m_view.mode != LightsModel::ColorMode::HS) ? Engaged::TEMP : Engaged::HUE;
        }
        if (m_engaged == Engaged::HUE && m_view.sat <= 0.0f)
        {
            m_view.sat = 100.0f;
        }
        break;
    }
    }

    return cmd;
}

void LightCardState::detents(int n, uint32_t nowMs)
{
    if (m_engaged == Engaged::NONE)
    {
        return;
    }

    m_lastInput = nowMs;
    m_lastDetent = nowMs;
    m_settling = true;
    m_view.on = true;

    switch (m_engaged)
    {
    case Engaged::BRIGHT:
    {
        int v = static_cast<int>(m_view.brightnessPct) + n * BRIGHT_STEP;
        m_view.brightnessPct = static_cast<uint8_t>(clampInt(v, 1, 100));
        break;
    }
    case Engaged::TEMP:
    {
        int v = static_cast<int>(m_view.kelvin) + n * KELVIN_STEP;
        v = clampInt(v, static_cast<int>(m_view.minKelvin), static_cast<int>(m_view.maxKelvin));
        m_view.kelvin = static_cast<uint16_t>(v);
        break;
    }
    case Engaged::HUE:
    {
        float v = fmodf(m_view.hue + static_cast<float>(n) * HUE_STEP, 360.0f);
        if (v < 0.0f)
        {
            v += 360.0f;
        }
        m_view.hue = v;
        break;
    }
    case Engaged::NONE:
        break;
    }
}

LightsModel::Command LightCardState::buildSettleCommand() const
{
    LightsModel::Command c;
    switch (m_engaged)
    {
    case Engaged::BRIGHT:
        c.type = LightsModel::Command::Type::BRIGHT;
        c.on = true;
        c.pct = m_view.brightnessPct;
        break;
    case Engaged::TEMP:
        c.type = LightsModel::Command::Type::TEMP;
        c.on = true;
        c.kelvin = m_view.kelvin;
        break;
    case Engaged::HUE:
        c.type = LightsModel::Command::Type::HUE;
        c.on = true;
        c.hue = m_view.hue;
        c.sat = m_view.sat;
        break;
    case Engaged::NONE:
        break;
    }
    return c;
}

LightsModel::Command LightCardState::tick(uint32_t nowMs)
{
    LightsModel::Command cmd; // Type::NONE

    if (m_engaged == Engaged::NONE)
    {
        return cmd;
    }

    if (m_settling && elapsedAtLeast(nowMs, m_lastDetent, SETTLE_MS))
    {
        cmd = buildSettleCommand();
        m_settling = false;
        return cmd;
    }

    if (elapsedAtLeast(nowMs, m_lastInput, IDLE_RELEASE_MS))
    {
        release();
    }

    return cmd;
}

float LightCardState::ringFraction() const
{
    switch (m_engaged)
    {
    case Engaged::TEMP:
    {
        float lo = static_cast<float>(m_view.minKelvin);
        float hi = static_cast<float>(m_view.maxKelvin);
        if (hi <= lo)
        {
            return 0.0f;
        }
        return clamp01((static_cast<float>(m_view.kelvin) - lo) / (hi - lo));
    }
    case Engaged::HUE:
        return clamp01(m_view.hue / 360.0f);
    case Engaged::BRIGHT:
    case Engaged::NONE:
    default:
        return clamp01(static_cast<float>(m_view.brightnessPct) / 100.0f);
    }
}
