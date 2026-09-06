#include "LightCardState.h"

#include <math.h>

#include "LightLayout.h"

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

void LightCardState::resetPage()
{
    m_page = Page::BRIGHT;
}

void LightCardState::swipe(int dir)
{
    const int last = static_cast<int>(pageCount()) - 1;
    m_page = static_cast<Page>(clampInt(static_cast<int>(m_page) + dir, 0, last));
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
    // BRIGHT/TEMP/HUE commands all set state:ON, so `on` is one of them, and
    // TEMP/HUE also decide the light's colour mode.
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
        merged.mode = LightsModel::ColorMode::TEMP;
        merged.kelvin = static_cast<uint16_t>(
            clampInt(m_confirmHold.kelvin, merged.minKelvin, merged.maxKelvin));
        break;
    case LightsModel::Command::Type::HUE:
        merged.on = m_confirmHold.on;
        merged.mode = LightsModel::ColorMode::HS;
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
        // on too -- otherwise a still-live POWER-off hold (Power then a
        // brightness edit within 1.5 s) would paint OFF while the user is
        // dialling.
        merged.on = m_view.on;
        switch (m_pending)
        {
        case LightsModel::Command::Type::BRIGHT:
            merged.brightnessPct = m_view.brightnessPct;
            break;
        case LightsModel::Command::Type::TEMP:
            merged.mode = m_view.mode;
            // Re-clamp into the freshly-adopted [minKelvin, maxKelvin] in case
            // sync() narrowed the bulb's range out from under the pending edit.
            merged.kelvin = static_cast<uint16_t>(
                clampInt(m_view.kelvin, merged.minKelvin, merged.maxKelvin));
            break;
        case LightsModel::Command::Type::HUE:
            merged.mode = m_view.mode;
            merged.hue = m_view.hue;
            merged.sat = m_view.sat;
            break;
        default:
            break;
        }
    }

    m_view = merged;
}

LightsModel::Command LightCardState::tapOn(uint32_t nowMs)
{
    LightsModel::Command cmd; // Type::NONE
    // valid (not available) is the gate: LightsModel::parseLightState() never
    // sets available without valid, and a light HA currently calls
    // unavailable can still be worth a blind switch-on.
    if (!m_view.valid || m_view.on)
    {
        return cmd;
    }

    m_view.on = true;
    resetPage();
    cmd.type = LightsModel::Command::Type::POWER;
    cmd.on = true;
    beginConfirmHold(cmd, nowMs);
    return cmd;
}

LightsModel::Command LightCardState::powerOff(uint32_t nowMs)
{
    LightsModel::Command cmd; // Type::NONE
    if (!m_view.valid)
    {
        return cmd;
    }

    m_view.on = false;
    // Switching off wins over whatever was being dialled: drop the settle
    // rather than sending a command that would turn the light back on.
    m_settling = false;
    m_pending = LightsModel::Command::Type::NONE;
    cmd.type = LightsModel::Command::Type::POWER;
    cmd.on = false;
    beginConfirmHold(cmd, nowMs);
    return cmd;
}

void LightCardState::detents(int n, uint32_t nowMs)
{
    // n == 0 is no movement at all: nothing to change, and nothing worth
    // arming a settle for.
    if (n == 0 || !editable())
    {
        return;
    }

    switch (m_page)
    {
    case Page::BRIGHT:
    {
        const int v = static_cast<int>(m_view.brightnessPct) + n * BRIGHT_STEP;
        m_view.brightnessPct = static_cast<uint8_t>(clampInt(v, 1, 100));
        m_pending = LightsModel::Command::Type::BRIGHT;
        break;
    }
    case Page::KELVIN:
    {
        int v = static_cast<int>(m_view.kelvin) + n * KELVIN_STEP;
        v = clampInt(v, static_cast<int>(m_view.minKelvin), static_cast<int>(m_view.maxKelvin));
        m_view.kelvin = static_cast<uint16_t>(v);
        // The command about to go out puts the light in temperature mode, so
        // the page stops drawing "not active" straight away.
        m_view.mode = LightsModel::ColorMode::TEMP;
        m_pending = LightsModel::Command::Type::TEMP;
        break;
    }
    case Page::COLOUR:
    {
        // Start from the preset the light is already nearest (preset() falls
        // back to nearestPreset(view().hue) when nothing is pending), then
        // step, wrapping both ways.
        const int count = static_cast<int>(LightLayout::kPresetCount);
        int idx = (static_cast<int>(preset()) + n) % count;
        if (idx < 0)
        {
            idx += count;
        }
        m_preset = static_cast<uint8_t>(idx);
        m_view.hue = LightLayout::kPresetHues[m_preset];
        m_view.sat = 100.0f;
        m_view.mode = LightsModel::ColorMode::HS;
        m_pending = LightsModel::Command::Type::HUE;
        break;
    }
    }

    m_lastEdit = nowMs;
    m_settling = true;
}

void LightCardState::selectPreset(uint8_t i, uint32_t nowMs)
{
    if (i >= LightLayout::kPresetCount || !editable())
    {
        return;
    }

    m_preset = i;
    m_view.hue = LightLayout::kPresetHues[i];
    m_view.sat = 100.0f;
    m_view.mode = LightsModel::ColorMode::HS;
    m_pending = LightsModel::Command::Type::HUE;
    m_lastEdit = nowMs;
    m_settling = true;
}

LightsModel::Command LightCardState::buildSettleCommand() const
{
    LightsModel::Command c;
    switch (m_pending)
    {
    case LightsModel::Command::Type::BRIGHT:
        c.type = LightsModel::Command::Type::BRIGHT;
        c.on = true;
        c.pct = m_view.brightnessPct;
        break;
    case LightsModel::Command::Type::TEMP:
        c.type = LightsModel::Command::Type::TEMP;
        c.on = true;
        c.kelvin = m_view.kelvin;
        break;
    case LightsModel::Command::Type::HUE:
        c.type = LightsModel::Command::Type::HUE;
        c.on = true;
        c.hue = LightLayout::kPresetHues[m_preset];
        c.sat = 100.0f;
        break;
    default:
        break;
    }
    return c;
}

LightsModel::Command LightCardState::tick(uint32_t nowMs)
{
    LightsModel::Command cmd; // Type::NONE

    if (!m_settling || !elapsedAtLeast(nowMs, m_lastEdit, SETTLE_MS))
    {
        return cmd;
    }

    cmd = buildSettleCommand();
    m_settling = false;
    beginConfirmHold(cmd, nowMs);
    return cmd;
}

uint8_t LightCardState::preset() const
{
    const bool pendingHue = m_settling && m_pending == LightsModel::Command::Type::HUE;
    const bool holdingHue = m_confirmHold.type == LightsModel::Command::Type::HUE;
    if (pendingHue || holdingHue)
    {
        return m_preset;
    }
    return LightLayout::nearestPreset(m_view.hue);
}

float LightCardState::ringFraction() const
{
    return clamp01(static_cast<float>(m_view.brightnessPct) / 100.0f);
}

// --- Plan 8 Task 3 removes this ------------------------------------------
LightsModel::Command LightCardState::tapButton(LightButtons::Button b, uint32_t nowMs)
{
    // The old three-button face is gone; only power still has a v2 meaning,
    // so the pre-rewrite DialUi keeps a working power button and nothing
    // else until Task 3 replaces that view.
    if (b != LightButtons::Button::POWER)
    {
        return LightsModel::Command();
    }
    return m_view.on ? powerOff(nowMs) : tapOn(nowMs);
}
// -------------------------------------------------------------------------
