#pragma once
#include <stdint.h>

// Pure input logic for the M5Dial encoder/touch/button UI: turns raw per-tick
// readings into intents (detents, tap, long press with hold progress, wake,
// button stop) and owns the backlight FULL/DIM/OFF state machine. Knows
// nothing about Arduino, M5Unified or the display — a later task feeds it
// from M5Dial and maps its events onto the treadmill controller.
struct DialEvents
{
    int detents = 0;
    bool tap = false;
    bool longPress = false;
    float holdProgress = 0;
    bool wake = false;
    bool btnStop = false;
    int swipe = 0; // -1 left, +1 right, 0 none
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
    static constexpr uint32_t OFF_AFTER_MS = 600000;

    enum class Backlight { FULL, DIM, OFF };

    // Feed one tick's raw readings; returns the intents derived from them.
    DialEvents tick(long encoderCount, bool touchDown, int touchX, int touchY,
                     bool btnClicked, uint32_t nowMs);

    Backlight backlight() const { return m_backlight; }

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

    Backlight m_backlight = Backlight::FULL;
    uint32_t m_lastActivityMs = 0;
    bool m_haveActivity = false;
};
