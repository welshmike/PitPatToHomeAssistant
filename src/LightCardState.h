#pragma once

#include <stdint.h>

#include "LightButtons.h"
#include "LightsModel.h"

// Pure, Arduino-free interaction state machine for one Lights card (Office
// or Lamp). No dependency on Arduino/M5 headers so this builds and is
// tested on the host (native env).
//
// DialUi owns one LightCardState per card, feeds it HA state via sync(),
// drives it from taps (tapButton()) and encoder detents (detents()), polls
// tick() every frame, and draws from view()/ringFraction()/engaged().
//
// Model: at most one control is "engaged" at a time (BRIGHT, TEMP or HUE).
// While engaged, encoder detents adjust a local optimistic copy of that
// field (view()) and arm a 300ms settle deadline; tick() fires exactly one
// command 300ms after the *last* detent. HA state (sync()) is adopted in
// full except for the engaged field while a settle is pending, so the
// user's in-flight edit isn't clobbered by a stale HA echo. A 10s idle
// timer (reset by any tap or detent) silently releases engagement if the
// user walks away mid-edit.
class LightCardState
{
public:
    enum class Engaged : uint8_t { NONE, BRIGHT, TEMP, HUE };

    static constexpr uint32_t SETTLE_MS = 300;
    static constexpr uint32_t IDLE_RELEASE_MS = 10000;
    static constexpr int BRIGHT_STEP = 5;
    static constexpr int KELVIN_STEP = 100;
    static constexpr int HUE_STEP = 10;

    // hasColour: true for the Lamp card (has a COLOUR button), false for
    // Office. Colour is only actually offered when the light itself also
    // reports supportsColor via sync().
    explicit LightCardState(bool hasColour);

    // Adopts HA's light state into view(). While settling() (a detent-driven
    // command is pending), keeps only the locally-edited engaged field
    // (brightnessPct for BRIGHT, kelvin for TEMP, hue+sat for HUE) and
    // adopts everything else, including `on`. When TEMP is the field being
    // kept, the retained kelvin is re-clamped into the freshly-adopted
    // [minKelvin, maxKelvin] in case sync() narrowed the bulb's range.
    // Otherwise adopts everything.
    void sync(const LightsModel::LightState& s);

    // Handles a tap on button b (from LightButtons::hitTest()).
    //
    // POWER: ignored unless view().valid; toggles view().on, releases any
    // engagement (dropping a pending settle without sending it -- the
    // POWER command wins), and returns a POWER command immediately.
    //
    // BRIGHT: ignored unless view().available; toggles engagement between
    // NONE and BRIGHT. Engaging never returns a command (detents + tick()
    // produce the BRIGHT command). Releasing (re-tap while engaged) flushes
    // a pending settle: if settling(), returns the same command tick()
    // would have emitted (via buildSettleCommand()) instead of dropping it;
    // otherwise returns NONE.
    //
    // COLOUR: ignored unless hasColour && view().supportsColor &&
    // view().available. Cycles NONE -> (TEMP if view().mode != HS, else
    // HUE) -> HUE -> NONE (third tap releases). Entering HUE with no usable
    // saturation from HA (sat <= 0) seeds it to 100. Engaging (first/second
    // tap) never returns a command. Releasing (third tap, from HUE) flushes
    // a pending settle the same way BRIGHT's release does.
    //
    // Any tap that isn't ignored refreshes the idle timer.
    LightsModel::Command tapButton(Button b, uint32_t nowMs);

    // Adjusts the engaged field by n detents (BRIGHT +-5 clamped 1-100;
    // TEMP +-100 clamped to [minKelvin, maxKelvin]; HUE +-10 wrapping
    // 0-360), sets view().on = true, and (re)arms the 300ms settle deadline
    // from nowMs. Refreshes the idle timer. No-op while !engaged().
    void detents(int n, uint32_t nowMs);

    // Polls the settle and idle timers. If a settle is due (>=300ms since
    // the last detent), emits exactly one command for the engaged field and
    // clears settling() (engagement itself is NOT released). Otherwise, if
    // idle for >=10s since the last tap/detent, silently releases
    // engagement (no command) -- except a still-pending settle is emitted
    // first (checked ahead of the idle test), matching the settle-then-idle
    // ordering when both are simultaneously due. No-op (returns a NONE
    // command) while !engaged().
    LightsModel::Command tick(uint32_t nowMs);

    Engaged engaged() const { return m_engaged; }
    bool settling() const { return m_settling; }

    // Local (optimistic) light state to draw from.
    const LightsModel::LightState& view() const { return m_view; }

    // 0..1 position of the engaged value within its range (brightness when
    // !engaged()): brightnessPct/100, (kelvin-min)/(max-min), or hue/360.
    float ringFraction() const;

    // Drops engagement immediately and silently: any pending settle is
    // discarded rather than emitted (unlike a release *tap*, which flushes
    // it). DialUi calls this when the card ring scrolls off this card so a
    // card left mid-edit doesn't come back engaged, and doesn't fire a
    // command minutes later for an edit the user walked away from.
    void release();

private:
    LightsModel::Command buildSettleCommand() const;

    bool m_hasColour;
    LightsModel::LightState m_view;
    Engaged m_engaged = Engaged::NONE;
    bool m_settling = false;
    uint32_t m_lastInput = 0;  // last tap/detent while engaged; drives the idle timer
    uint32_t m_lastDetent = 0; // last detent; drives the settle deadline
};
