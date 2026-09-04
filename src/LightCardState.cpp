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

bool LightCardState::confirmedBy(const LightsModel::LightState& s) const
{
    switch (m_confirmHold.type)
    {
    case LightsModel::Command::Type::POWER:
        return s.on == m_confirmHold.on;
    case LightsModel::Command::Type::BRIGHT:
        return s.brightnessPct == m_confirmHold.pct;
    case LightsModel::Command::Type::TEMP:
    {
        const int d = static_cast<int>(s.kelvin) - static_cast<int>(m_confirmHold.kelvin);
        return (d < 0 ? -d : d) <= CONFIRM_KELVIN_TOL;
    }
    case LightsModel::Command::Type::HUE:
    {
        float d = fabsf(s.hue - m_confirmHold.hue);
        if (d > 180.0f)
        {
            d = 360.0f - d; // shortest way round the circle: 359 vs 1 is 2 degrees
        }
        return d <= CONFIRM_HUE_TOL;
    }
    case LightsModel::Command::Type::NONE:
    default:
        return false;
    }
}

void LightCardState::beginConfirmHold(const LightsModel::Command& cmd, uint32_t nowMs)
{
    if (cmd.type == LightsModel::Command::Type::NONE)
    {
        return;
    }
    // One hold at a time: a newer command supersedes whatever was in flight.
    m_confirmHold = cmd;
    m_confirmHoldStart = nowMs;
}

void LightCardState::sync(const LightsModel::LightState& s, uint32_t nowMs)
{
    // Drop the awaiting-confirmation hold as soon as HA echoes what was sent,
    // or once the deadline passes (HA never echoed -- command lost, or the
    // light refused it -- so stop lying about it).
    if (m_confirmHold.type != LightsModel::Command::Type::NONE &&
        (confirmedBy(s) || elapsedAtLeast(nowMs, m_confirmHoldStart, CONFIRM_HOLD_MS)))
    {
        m_confirmHold.type = LightsModel::Command::Type::NONE;
    }

    if (!m_settling && m_confirmHold.type == LightsModel::Command::Type::NONE)
    {
        m_view = s;
        return;
    }

    LightsModel::LightState merged = s;

    // Awaiting confirmation: keep the fields the in-flight command carried.
    // BRIGHT/TEMP/HUE commands all set state:ON, so `on` is one of them.
    switch (m_confirmHold.type)
    {
    case LightsModel::Command::Type::POWER:
        merged.on = m_confirmHold.on;
        break;
    case LightsModel::Command::Type::BRIGHT:
        merged.on = m_confirmHold.on;
        merged.brightnessPct = m_confirmHold.pct;
        break;
    case LightsModel::Command::Type::TEMP:
        merged.on = m_confirmHold.on;
        merged.kelvin = static_cast<uint16_t>(
            clampInt(m_confirmHold.kelvin, merged.minKelvin, merged.maxKelvin));
        break;
    case LightsModel::Command::Type::HUE:
        merged.on = m_confirmHold.on;
        merged.hue = m_confirmHold.hue;
        merged.sat = m_confirmHold.sat;
        break;
    case LightsModel::Command::Type::NONE:
        break;
    }

    // Settling: keep the field the user is editing right now, so the pending
    // edit survives a stale HA echo. This wins over the hold above -- the
    // newer local value is the one about to be sent.
    if (m_settling)
    {
        // The pending command carries "state":"ON", so keep the optimistic
        // on too -- otherwise a still-live POWER-off hold (Power then Bright
        // within 1.5 s) would paint OFF while the user is dialling.
        merged.on = m_view.on;
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
    }

    m_view = merged;
}

void LightCardState::release()
{
    m_engaged = Engaged::NONE;
    m_settling = false;
}

LightsModel::Command LightCardState::tapButton(LightButtons::Button b, uint32_t nowMs)
{
    LightsModel::Command cmd; // Type::NONE

    switch (b)
    {
    case LightButtons::Button::NONE:
        break;

    case LightButtons::Button::POWER:
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

    case LightButtons::Button::BRIGHT:
    {
        if (!m_view.available)
        {
            break;
        }
        m_lastInput = nowMs;
        // Leaving a control -- releasing BRIGHT, or taking engagement over
        // from TEMP/HUE -- flushes that control's pending settle rather than
        // dropping it. m_settling is only ever set while engaged, so this
        // always describes the *old* field.
        if (m_settling)
        {
            cmd = buildSettleCommand();
        }
        const bool wasBright = (m_engaged == Engaged::BRIGHT);
        release();
        if (!wasBright)
        {
            m_engaged = Engaged::BRIGHT;
        }
        break;
    }

    case LightButtons::Button::COLOUR:
    {
        bool colourSupported = m_hasColour && m_view.supportsColor;
        if (!colourSupported || !m_view.available)
        {
            break;
        }
        m_lastInput = nowMs;
        // TEMP -> HUE flushes TEMP's pending settle exactly as the third-tap
        // release flushes HUE's; only release() drops one silently.
        if (m_settling)
        {
            cmd = buildSettleCommand();
        }
        const Engaged before = m_engaged;
        release();
        if (before == Engaged::TEMP)
        {
            m_engaged = Engaged::HUE;
        }
        else if (before != Engaged::HUE) // HUE: third tap, stay released
        {
            m_engaged = (m_view.mode != LightsModel::ColorMode::HS) ? Engaged::TEMP : Engaged::HUE;
        }
        if (m_engaged == Engaged::HUE && m_view.sat <= 0.0f)
        {
            m_view.sat = 100.0f;
        }
        break;
    }
    }

    beginConfirmHold(cmd, nowMs);
    return cmd;
}

void LightCardState::detents(int n, uint32_t nowMs)
{
    // n == 0 is no movement at all: nothing to change, and nothing worth
    // arming a settle (or an idle refresh) for.
    if (m_engaged == Engaged::NONE || n == 0)
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
        beginConfirmHold(cmd, nowMs);
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
