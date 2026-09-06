#pragma once

#include <stdint.h>

#include "LightsModel.h"

// Pure, Arduino-free interaction state machine for one Lights card (Office
// or Lamp). No dependency on Arduino/M5 headers so this builds and is
// tested on the host (native env).
//
// Model (spec §4.12 "Lights v2", page order per the §4.18 amendment): the
// card is a small stack of pages — Brightness, Kelvin and, on the Lamp,
// Colour — and the knob always adjusts the page that is showing. A
// horizontal swipe changes page (no wrap); every arrival on the card and
// every switch-on starts on Brightness. Lamp runs Brightness -> Colour ->
// Kelvin; Office (no Colour page) runs Brightness -> Kelvin. When the light
// is off the whole face is a switch-on target (tapOn()); when it is on, the
// small power glyph and a touch-hold switch it off (powerOff()).
//
// DialUi owns one LightCardState per card, feeds it HA state via sync(),
// drives it from swipe()/tapOn()/powerOff()/selectHue()/detents(), polls
// tick() every frame, and draws from view()/page()/editHue()/ringFraction().
//
// Detents adjust a local optimistic copy of the page's field (view()) and arm
// a 300 ms settle deadline; tick() fires exactly one command 300 ms after the
// *last* detent. HA state (sync()) is adopted in full except for the field
// being edited while a settle is pending, so the user's in-flight edit isn't
// clobbered by a stale HA echo. Once a command has actually gone out, the
// fields it carried are held against sync() for up to CONFIRM_HOLD_MS, so the
// optimistic value doesn't snap back to HA's pre-command state in the gap
// before HA echoes.
class LightCardState
{
public:
    // Enum values are stable (FrameKey::lightPage stores card.page() as this
    // value) but no longer double as the swipe/page-dot order -- see
    // pageAt().
    enum class Page : uint8_t { BRIGHT, KELVIN, COLOUR };

    static constexpr uint32_t SETTLE_MS = 300;
    // Pages other than Brightness fall back to Brightness after this long
    // without a swipe, detent or ring tap (Mike, 2026-09-06: every menu
    // times out back to the card's default).
    static constexpr uint32_t PAGE_IDLE_MS = 10000;
    // How long an emitted command's fields survive a contradicting sync()
    // while waiting for HA to echo the change back.
    static constexpr uint32_t CONFIRM_HOLD_MS = 1500;
    // HA quantises colour temperature onto its own mired grid and rounds hue,
    // so an echo only has to land near the value we sent to count as
    // confirmation.
    static constexpr int CONFIRM_KELVIN_TOL = 50;
    static constexpr float CONFIRM_HUE_TOL = 2.0f;
    static constexpr int BRIGHT_STEP = 5;
    static constexpr int KELVIN_STEP = 100;
    static constexpr int HUE_STEP = 15; // degrees per detent on the Colour page (24 per revolution)

    // hasColour: true for the Lamp card (which has a Colour page), false for
    // Office (Brightness and Kelvin only).
    explicit LightCardState(bool hasColour);

    // 3 for the Lamp, 2 for Office.
    uint8_t pageCount() const { return m_hasColour ? 3 : 2; }

    // The card's page sequence (spec 4.18 amendment): Lamp is Brightness ->
    // Colour -> Kelvin, Office (no colour) is Brightness -> Kelvin. Indices
    // past the end clamp to the last page.
    Page pageAt(uint8_t i) const;
    uint8_t pageIndex() const { return m_pageIdx; }
    Page page() const { return pageAt(m_pageIdx); }

    // Back to the Brightness page. Deliberately does not touch a pending
    // settle or an in-flight command: DialUi calls this on arrival at the
    // card, and an edit made a moment ago still deserves to be sent.
    void resetPage();

    // Moves `dir` pages (a left swipe is +1), clamped to [BRIGHT,
    // pageCount()-1] — the ends do not wrap. Ignored while the light is off:
    // the off face is a single switch-on target with no pages behind it, so a
    // swipe there must not leave the card on a page nobody can see.
    void swipe(int dir, uint32_t nowMs);

    // Adopts HA's light state into view(), subject to two overlapping
    // protections:
    //
    // 1. While settling() (a detent-driven command is pending), keeps the
    //    locally-edited field (brightnessPct on BRIGHT, kelvin on KELVIN,
    //    hue+sat on COLOUR) and adopts everything else. When kelvin is the
    //    field being kept, the retained value is re-clamped into the
    //    freshly-adopted [minKelvin, maxKelvin] in case sync() narrowed the
    //    bulb's range.
    //
    // 2. While awaiting confirmation of a command that has already gone out
    //    (see CONFIRM_HOLD_MS), keeps that command's own fields: `on` for
    //    POWER; brightnessPct (and `on`, which BRIGHT/TEMP/HUE commands all
    //    set) for BRIGHT; kelvin for TEMP; hue+sat for HUE. TEMP and HUE also
    //    keep the colour mode they put the light into, so the page the user
    //    just made live doesn't flicker back to "not active". Without this
    //    the next sync() -- DialUi re-syncs every 250 ms -- would snap the
    //    view back to HA's pre-command state until HA's echo arrives. The
    //    hold ends early the moment `s` carries the value that was sent
    //    (exactly for on/pct, within CONFIRM_KELVIN_TOL / CONFIRM_HUE_TOL for
    //    kelvin/hue), and otherwise at the CONFIRM_HOLD_MS deadline, after
    //    which HA wins again -- so a change made elsewhere (wall switch, HA
    //    UI) still lands, just a beat later.
    //
    // Everything not protected by either is adopted as-is.
    void sync(const LightsModel::LightState& s, uint32_t nowMs);

    // Tap on the off face. Ignored unless view().valid and the light is off;
    // this class itself permits a switch-on once any state has been parsed,
    // even if the light is currently unavailable -- DialUi is what gates
    // taps on view().available before ever calling this. Sets view().on
    // optimistically, returns to the Brightness page and returns the POWER
    // on command.
    LightsModel::Command tapOn(uint32_t nowMs);

    // Tap on the small power glyph, or a touch-hold. Ignored unless
    // view().valid. Clears view().on optimistically, drops any pending settle
    // (the POWER command wins) and returns the POWER off command.
    LightsModel::Command powerOff(uint32_t nowMs);

    // Adjusts the showing page's value by n detents -- BRIGHT +-5 clamped
    // 1-100; KELVIN +-100 clamped to [minKelvin, maxKelvin]; COLOUR
    // +-HUE_STEP degrees, wrapping at 360 -- and (re)arms the 300 ms settle
    // deadline from nowMs.
    // Ignored when n == 0, or while the light is off or unavailable (the off
    // face has no value to turn, and there is nothing to send to a light that
    // isn't there).
    //
    // On COLOUR, the first detent steps from editHue() -- the pending edit if
    // one is in flight, otherwise the hue HA reports -- so a light in
    // temperature mode starts from its last colour; the edit also makes hue
    // mode live locally, matching the command that is about to go out.
    void detents(int n, uint32_t nowMs);

    // Tap on the hue ring at `hue` degrees (LightLayout::hueAt). Same gating
    // as detents(), plus: only the Colour page of a colour-capable card has
    // the ring at all, so a call while another page is showing (or on Office)
    // is ignored. The hue is normalised into [0, 360).
    void selectHue(float hue, uint32_t nowMs);

    // Finger scrub on the hue ring (spec 4.18 amendment). scrub() is called on
    // every tick a claimed gesture is held, with the hue under the finger.
    // The first call of a gesture selects that hue (same as a tap); later
    // calls select again only when the finger has moved >= SCRUB_MIN_DEG from
    // the hue this gesture last selected -- never from editHue(), which HA's
    // echo can move -- so a resting finger yields exactly one command 300 ms
    // after it stops. Every call refreshes the page-idle timer so the Colour
    // page cannot time out under a held finger. Gated like selectHue(): only
    // the Colour page of an editable, colour-capable card. endScrub() closes
    // the gesture; the next scrub() starts a new one.
    void scrub(float hue, uint32_t nowMs);
    void endScrub();
    bool scrubbing() const { return m_scrubActive; }
    static constexpr float SCRUB_MIN_DEG = 1.0f;

    // Polls the settle timer. If a settle is due (>=300 ms since the last
    // detent or ring tap), emits exactly one command for the edited field
    // and clears settling() (and the pending edit with it). Otherwise returns
    // a NONE command. There is no idle release: the pages are always live.
    // DialUi polls this on BOTH cards every frame, whatever is on screen, so
    // an edit made a moment before the card was left still goes out.
    LightsModel::Command tick(uint32_t nowMs);

    bool settling() const { return m_settling; }

    // Local (optimistic) light state to draw from.
    const LightsModel::LightState& view() const { return m_view; }

    // Whether this card offers the Colour page (Lamp) or not (Office) — the
    // same flag passed to the constructor, exposed so callers don't have to
    // re-derive it from the card's LightKey.
    bool hasColour() const { return m_hasColour; }

    // The hue the marker shows: the edit being sent while it is pending or
    // awaiting confirmation, otherwise HA's reported hue, normalised. No
    // snapping -- a colour set from HA or the Hue app is shown faithfully.
    float editHue() const;

    // True while a HUE edit is in flight: settling (command not yet sent) or
    // awaiting HA's confirmation of the sent command. Drives the marker's
    // amber outline on the Colour page.
    bool hueEditInFlight() const;

    // Which of the two colour pages the light is actually in: HA reports one
    // mode at a time, and the other page draws dim ("not active - turn to
    // use") until a detent on it sends that mode's command.
    bool kelvinLive() const { return m_view.mode != LightsModel::ColorMode::HS; }
    bool colourLive() const { return m_view.mode == LightsModel::ColorMode::HS; }

    // 0..1 brightness, for the value ring on the Brightness page.
    float ringFraction() const;

private:
    LightsModel::Command buildSettleCommand() const;

    // Starts (or replaces) the awaiting-confirmation hold for a command that
    // has just been handed to the caller. No-op for a NONE command.
    void beginConfirmHold(const LightsModel::Command& cmd, uint32_t nowMs);

    // True if `s` carries the values m_confirmHold sent, within the per-field
    // tolerance. False when no hold is active.
    bool confirmedBy(const LightsModel::LightState& s) const;

    // Whether the light can be edited at all right now.
    bool editable() const { return m_view.on && m_view.available; }

    bool m_hasColour;
    LightsModel::LightState m_view;
    uint8_t m_pageIdx = 0; // index into the card's page sequence (pageAt)
    // What the pending settle will send (NONE when nothing is pending). This
    // is the page the edit was made on, not the page showing now: a swipe
    // mid-settle must not change what gets sent.
    LightsModel::Command::Type m_pending = LightsModel::Command::Type::NONE;
    bool m_settling = false;
    uint32_t m_lastEdit = 0; // last detent/ring tap; drives the settle deadline
    uint32_t m_pageInputMs = 0; // last swipe/detent/ring tap; drives PAGE_IDLE_MS
    float m_editHue = 0.0f;  // hue being edited (only meaningful for a HUE edit/hold)
    bool m_scrubActive = false;   // a finger-scrub gesture is in progress
    float m_scrubLastHue = 0.0f;  // hue this gesture last selected
    // The command whose echo we're waiting for (type NONE == no hold), and
    // when it went out.
    LightsModel::Command m_confirmHold;
    uint32_t m_confirmHoldStart = 0;
};
