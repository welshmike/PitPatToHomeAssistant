#pragma once
#include "board.h"
#if HAS_DIAL_UI

#include <M5Dial.h>
#include <stdint.h>

#include "TreadmillController.h"
#include "NetStatus.h"

// Loop-task ISnapshotObserver that renders treadmill/net state to the Dial's
// round 240x240 display. Phase B smoke content only: three status dots, the
// status name, and speed/distance. No inputs yet — encoder/touch handling
// lands in a later task, but tick() still pumps M5Dial.update() every call so
// those state machines (debouncing, click detection) stay healthy meanwhile.
class DialUi : public ISnapshotObserver
{
public:
    // Must run FIRST in setup() on the Dial — M5Unified owns display/I2C/
    // Serial init via M5Dial.begin(). Creates the render canvas (16bpp,
    // 240x240); falls back to drawing straight onto M5Dial.Display if the
    // sprite allocation fails (logs ESP.getFreeHeap() before/after either way).
    void begin();

    // Call once per loop() iteration, after controller.tick(). Renders at
    // most once every kRenderIntervalMs.
    void tick(uint32_t nowMs);

    void onSnapshot(const TreadMillData& d) override;
    void onTargetSpeed(float mph, bool pending) override;
    void onNetStatus(NetStatus s) override;

private:
    static constexpr uint32_t kRenderIntervalMs = 50;

    void render();
    void draw(LovyanGFX& gfx);
    void drawStatusDots(LovyanGFX& gfx);
    static const char* statusName(TreadMillData::Status s);

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
