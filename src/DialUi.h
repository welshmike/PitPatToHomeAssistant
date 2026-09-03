#pragma once
#include "board.h"
#if HAS_DIAL_UI

#include <M5Dial.h>
#include <stdint.h>

#include "TreadmillController.h"
#include "NetStatus.h"
#include "DialInput.h"

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
    // only when DialInput's backlight state changes.
    static constexpr uint8_t kBrightFull = 255;
    static constexpr uint8_t kBrightDim  = 50;
    static constexpr uint8_t kBrightOff  = 0;

    void render(uint32_t nowMs);
    void draw(LovyanGFX& gfx, uint32_t nowMs);
    void drawStatusDots(LovyanGFX& gfx);
    void drawIdle(LovyanGFX& gfx);
    void drawRunning(LovyanGFX& gfx, bool paused, uint32_t nowMs);
    static const char* statusName(TreadMillData::Status s);

    void handleInput(uint32_t nowMs);
    void applyBrightness();

    // Redraw-skip key for the render() throttle below: every field that can
    // change what drawRunning()/drawIdle() puts on screen, coarsened to the
    // granularity that's actually visible (tenths of mph, hundredths of a
    // km, 1/20ths of hold progress) so float jitter below that doesn't
    // trigger a redraw. POD, compared field-by-field — no heap, no float
    // comparison.
    struct FrameKey
    {
        uint8_t  status        = 0;
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

        bool operator==(const FrameKey& o) const
        {
            return status == o.status && paused == o.paused && durationSec == o.durationSec &&
                   speedTenths == o.speedTenths && distanceCenti == o.distanceCenti &&
                   steps == o.steps && targetTenths == o.targetTenths && pending == o.pending &&
                   overlayActive == o.overlayActive && netStatus == o.netStatus &&
                   holdUnits == o.holdUnits && pulsePhase == o.pulsePhase;
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
    DialInput::Backlight m_lastBacklight = DialInput::Backlight::FULL;
    float m_holdProgress = 0.0f; // last tick's hold-in-progress fraction [0,1]; drives the long-press ring

#if DIAL_SOUND
    bool m_secondBeepPending = false;
    uint32_t m_secondBeepDueMs = 0;
#endif

    M5Canvas m_canvas{&M5Dial.Display};
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
