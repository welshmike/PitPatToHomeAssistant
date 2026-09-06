#pragma once
#include <stdint.h>

// Pure input logic for the M5Dial encoder/touch/button UI: turns raw per-tick
// readings into intents (detents, tap, long press with hold progress, wake,
// button stop) and owns the backlight FULL/DIM state machine. The backlight
// dims after DIM_AFTER_MS of inactivity but never turns off (spec 4.8) —
// there is no OFF stage. Knows nothing about Arduino, M5Unified or the
// display — a later task feeds it from M5Dial and maps its events onto the
// treadmill controller.
struct DialEvents
{
    int detents = 0;
    bool tap = false;
    int tapX = 0; // touch-down position of the gesture that produced tap=true
    int tapY = 0;
    bool longPress = false;
    float holdProgress = 0;
    bool wake = false;
    bool btnStop = false;
    int swipe = 0; // -1 left, +1 right, 0 none
    bool btnClick = false; // decided single click (from btnSingleClicked)
    bool btnHold = false;  // hold threshold reached (from btnHold)
};

class DialInput
{
public:
    static constexpr int PULSES_PER_DETENT = 4;
    static constexpr uint32_t TAP_MAX_MS = 600;
    static constexpr int TAP_MAX_MOVE_PX = 20;
    static constexpr uint32_t HOLD_MS = 1000;
    static constexpr int SWIPE_MIN_PX = 40;
    static constexpr uint32_t DIM_AFTER_MS = 120000;
    // How long after an acted-on stop the follow-up single click is dropped.
    // M5Unified decides wasSingleClicked() one _msecHold (500 ms) after the
    // release that already produced wasClicked(); 700 ms covers that plus the
    // tick the UI happens to see it on, and is far short of a deliberate
    // second press.
    static constexpr uint32_t SWALLOW_CLICK_MS = 700;

    enum class Backlight { FULL, DIM };

    // Feed one tick's raw readings; returns the intents derived from them.
    // btnClicked is the raw wasClicked() edge (drives btnStop, unchanged).
    // btnSingleClicked/btnHold are M5Dial's debounced wasSingleClicked()/
    // wasHold() edges, fed straight through to btnClick/btnHold; like
    // btnClicked, either counts as activity for the backlight and is
    // swallowed (wake-only) on the tick that wakes a dimmed backlight.
    DialEvents tick(long encoderCount, bool touchDown, int touchX, int touchY,
                     bool btnClicked, bool btnSingleClicked, bool btnHold, uint32_t nowMs);

    Backlight backlight() const { return m_backlight; }

    // Called by the UI on the tick it *acts* on btnStop (the belt's emergency
    // stop, accepted or refused). One physical press is reported twice by
    // M5Unified — wasClicked() at release, then wasSingleClicked() once the
    // multi-click window closes — and without this the second half would open
    // the card menu behind the stop. Arms a one-shot swallow of the next
    // btnSingleClicked within SWALLOW_CLICK_MS; btnHold is untouched, since a
    // hold never co-fires with a click. The UI does NOT call this when it
    // ignores btnStop (belt idle), so the click stays the menu gesture there.
    void consumeClick(uint32_t nowMs);

    // Called by the UI on belt activity (e.g. a running walk) so the
    // backlight stays FULL without any encoder/touch/button input.
    void noteActivity(uint32_t nowMs);

private:
    void wake(uint32_t nowMs);
    void updateBacklight(uint32_t nowMs);

    long m_lastCount = 0;
    bool m_haveLastCount = false;
    int m_remainder = 0;

    bool m_touchActive = false;   // a touch gesture is in progress
    bool m_touchSwallowed = false; // gesture started while waking; ignore until release
    uint32_t m_touchStartMs = 0;
    int m_touchStartX = 0;
    int m_touchStartY = 0;
    bool m_touchIsDrag = false;
    bool m_longPressFired = false;
    bool m_swipeFired = false;

    bool m_swallowClick = false;       // one-shot: drop the next btnSingleClicked
    uint32_t m_swallowClickUntilMs = 0; // ...but only until this instant

    Backlight m_backlight = Backlight::FULL;
    uint32_t m_lastActivityMs = 0;
    bool m_haveActivity = false;
};
