#pragma once
#include "board.h"
#if HAS_DIAL_UI

#include <M5Dial.h>
#include <stdint.h>

#include "TreadmillController.h"
#include "NetStatus.h"
#include "DialInput.h"
#include "SpeedSelector.h"
#include "CardRing.h"

// Loop-task ISnapshotObserver that renders treadmill/net state to the Dial's
// round 240x240 display and drives the controller from the encoder, touch
// screen and side button. tick() pumps M5Dial.update() and DialInput::tick()
// every call (so debouncing/click/hold state machines stay healthy at full
// loop rate), independent of the render throttle below.
class DialUi : public ISnapshotObserver
{
public:
    explicit DialUi(TreadmillController& controller);

    // Must run FIRST in setup() on the Dial — M5Unified owns display/I2C/
    // Serial init via M5Dial.begin(). Creates the render canvas (16bpp,
    // 240x240); falls back to drawing straight onto M5Dial.Display if the
    // sprite allocation fails (logs ESP.getFreeHeap() before/after either way).
    void begin();

    // Call once per loop() iteration, after controller.tick(). Reads inputs
    // and drives the controller every call; renders at most once every
    // kRenderIntervalMs.
    void tick(uint32_t nowMs);

    void onSnapshot(const TreadMillData& d) override;
    void onTargetSpeed(float mph, bool pending) override;
    void onNetStatus(NetStatus s) override;

private:
    static constexpr uint32_t kRenderIntervalMs = 50;

    // Display brightness levels, applied via M5Dial.Display.setBrightness()
    // only when DialInput's backlight state changes. No OFF level — the
    // backlight dims but never turns off (spec 4.8).
    static constexpr uint8_t kBrightFull = 255;
    static constexpr uint8_t kBrightDim  = 50;

    void render(uint32_t nowMs);
    void draw(LovyanGFX& gfx, uint32_t nowMs);
    void drawStatusDots(LovyanGFX& gfx);
    void drawDisconnected(LovyanGFX& gfx);
    void drawClock(LovyanGFX& gfx);
    void drawConnecting(LovyanGFX& gfx);
    void drawStarting(LovyanGFX& gfx, uint32_t nowMs);
    void drawRunning(LovyanGFX& gfx, bool paused, uint32_t nowMs);
    // Start-speed picker (spec 4.7), opened by a tap on Disconnected.
    void drawSelector(LovyanGFX& gfx);
    // Centre speed overlay (target speed in Font7 amber + "mph" caption, and
    // the amber target ring) — called from draw() for whichever screen is
    // showing, on top of it, while the overlay window (m_speedOverlayUntilMs)
    // is open. Shared by every screen, not just Running (I3).
    void drawSpeedOverlay(LovyanGFX& gfx, bool paused);
    // Long-press-to-stop progress ring — called from draw() for whichever
    // screen is showing, on top of it, whenever a hold is in progress (I4).
    void drawHoldArc(LovyanGFX& gfx);

    // Which of the six top-level screens is showing right now. Selection
    // order: controller.isConnecting() -> Connecting (checked BEFORE the
    // COUNTDOWN test below — a start() queued while disconnected makes the
    // controller publish an optimistic COUNTDOWN even though nothing is
    // actually counting down yet, so Connecting must win that race or the
    // Starting screen shows with no way to cancel it); COUNTDOWN -> Starting;
    // (RUNNING or paused) -> Running/Paused; m_selector.isOpen() -> Selector;
    // else the current card (m_cards.current(), spec 4.8): TREADMILL ->
    // Disconnected (the Treadmill card), CLOCK -> Clock. Connecting/Starting/
    // Running all win over an open selector — handleInput() closes it as soon
    // as any of those becomes true, so a stale selector can't resurface once
    // they clear; they also win over the card ring, which simply resumes on
    // its last card once the belt screens clear. `paused` is passed in rather
    // than recomputed so callers that already have it (draw(), handleInput())
    // don't pay for isPausedState() twice in the same tick.
    enum class Screen : uint8_t { DISCONNECTED, CLOCK, CONNECTING, STARTING, RUNNING, SELECTOR };
    Screen currentScreen(bool paused) const;

    void handleInput(uint32_t nowMs);
    void applyBrightness();
    // Plays the accepted/refused tone pair for a stop-or-cancel gesture
    // (Connecting-screen cancel, hold-to-stop, side button). No-op when
    // DIAL_SOUND is off.
    void playStopBeep(uint32_t nowMs, bool accepted);
    // Plays the tap-style accepted/refused tone (a single short beep, either
    // pitch) used for Selector open/confirm/cancel and start/pause toggling —
    // distinct from playStopBeep()'s two-tone stop/cancel pattern. No-op when
    // DIAL_SOUND is off.
    void playAcceptBeep(bool accepted);

    // Redraw-skip key for the render() throttle below: every field that can
    // change what drawRunning()/drawIdle() puts on screen, coarsened to the
    // granularity that's actually visible (tenths of mph, hundredths of a
    // km, 1/20ths of hold progress) so float jitter below that doesn't
    // trigger a redraw. POD, compared field-by-field — no heap, no float
    // comparison.
    struct FrameKey
    {
        uint8_t  status        = 0;
        uint8_t  screen        = 0;
        bool     paused        = false;
        uint32_t durationSec   = 0;
        int32_t  speedTenths   = 0;
        int32_t  distanceCenti = 0;
        uint32_t steps         = 0;
        int32_t  targetTenths  = 0;
        bool     pending       = false;
        bool     overlayActive = false;
        uint8_t  netStatus     = 0;
        int32_t  holdUnits     = 0;
        uint8_t  pulsePhase    = 0;
        uint16_t connectAttempts   = 0;
        uint32_t sessionDurationSec = 0;
        int32_t  sessionDistanceCenti = 0;
        uint32_t sessionSteps        = 0;
        bool     selectorOpen   = false;
        int32_t  selectorTenths = 0;
        uint8_t  cardId         = 0; // CardId of m_cards.current() (spec 4.8)

        bool operator==(const FrameKey& o) const
        {
            return status == o.status && screen == o.screen && paused == o.paused &&
                   durationSec == o.durationSec && speedTenths == o.speedTenths &&
                   distanceCenti == o.distanceCenti && steps == o.steps &&
                   targetTenths == o.targetTenths && pending == o.pending &&
                   overlayActive == o.overlayActive && netStatus == o.netStatus &&
                   holdUnits == o.holdUnits && pulsePhase == o.pulsePhase &&
                   connectAttempts == o.connectAttempts &&
                   sessionDurationSec == o.sessionDurationSec &&
                   sessionDistanceCenti == o.sessionDistanceCenti &&
                   sessionSteps == o.sessionSteps &&
                   selectorOpen == o.selectorOpen && selectorTenths == o.selectorTenths &&
                   cardId == o.cardId;
        }
    };
    FrameKey buildFrameKey(uint32_t nowMs) const;
    bool isPausedState() const;

    // render() runs at most every kRenderIntervalMs (tick()'s own throttle,
    // <= 20 Hz) but only actually redraws+pushes when the FrameKey changed or
    // kFrameElapsedMs has passed since the last real draw (so the 1 Hz PAUSED
    // pulse still animates while nothing else is changing).
    static constexpr uint32_t kFrameElapsedMs = 250;
    FrameKey m_lastFrame;
    bool     m_haveLastFrame  = false;
    uint32_t m_lastFrameDrawMs = 0;

    TreadmillController& m_controller;
    DialInput m_input;
    SpeedSelector m_selector;
    // Which desk-mode card is showing while the belt is idle (spec 4.8);
    // boots on CLOCK and is driven by knob detents in handleInput().
    CardRing m_cards;
    DialInput::Backlight m_lastBacklight = DialInput::Backlight::FULL;
    float m_holdProgress = 0.0f; // last tick's hold-in-progress fraction [0,1]; drives the long-press ring

#if DIAL_SOUND
    bool m_secondBeepPending = false;
    uint32_t m_secondBeepDueMs = 0;
#endif

    // Default-constructed (no parent bound at construction time): `dialUi` is
    // a global in main.cpp, and M5Dial.Display is a reference bound inside a
    // library global's own constructor — global init order across
    // translation units is unspecified, so binding to it here would be a
    // static-init-order hazard. The sprite is pushed to an explicit
    // destination (&M5Dial.Display) in render() instead.
    M5Canvas m_canvas;
    bool m_useCanvas = false;
    uint32_t m_lastRenderMs = 0;

    // Last state pushed by the controller. Defaults mirror a fresh boot:
    // disconnected belt, no network yet — so the very first render (before
    // any observer callback fires) already reads correctly.
    TreadMillData m_snapshot;
    float m_targetSpeedMph = 0.0f;
    bool m_targetPending = false;
    NetStatus m_netStatus = NetStatus::WIFI_DOWN;

    // Deadline for the centre speed overlay: nowMs + DIAL_SPEED_OVERLAY_MS,
    // (re)armed each time a nudge lands a fresh onTargetSpeed(pending=true).
    // onTargetSpeed() itself isn't handed nowMs (the ISnapshotObserver
    // interface is shared with mqttview and the host tests), so the deadline
    // is captured at the one call site that can trigger pending=true —
    // handleInput()'s nudgeSpeed() call — right after the synchronous
    // notifyTarget() callback has run. 0 means "never armed"; compared via
    // signed subtraction so it stays correct across a millis() wrap.
    uint32_t m_speedOverlayUntilMs = 0;
};

#endif // HAS_DIAL_UI
