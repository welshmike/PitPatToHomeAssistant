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

    void render();
    void draw(LovyanGFX& gfx);
    void drawStatusDots(LovyanGFX& gfx);
    static const char* statusName(TreadMillData::Status s);

    void handleInput(uint32_t nowMs);
    void applyBrightness();

    TreadmillController& m_controller;
    DialInput m_input;
    DialInput::Backlight m_lastBacklight = DialInput::Backlight::FULL;
    float m_holdProgress = 0.0f; // last tick's hold-in-progress fraction [0,1]; used by a later task's ring animation

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
};

#endif // HAS_DIAL_UI
