#pragma once
#include <stdint.h>

// Pure, Arduino-free state machine for the encoder-driven speed picker. No
// dependency on Arduino/M5 headers so this builds and is tested on the host.
//
// The selector opens with a default speed, accepts relative steps (each
// worth SELECTOR_STEP_MPH, rounded to the SPEED_STEP_MPH grid and clamped to
// [SPEED_MIN_MPH, SPEED_MAX_MPH]), and auto-closes after SELECTOR_TIMEOUT_MS
// of inactivity. Callers drive the timeout by polling tick() with the
// current millis(); tick() reports true exactly once, on the poll that
// crosses the timeout, and closes the selector at the same time.
class SpeedSelector
{
public:
    // Opens the selector at defaultMph (clamped into range and rounded to
    // the 0.1 grid) and records nowMs as the last-activity time.
    void open(float defaultMph, uint32_t nowMs);

    // True from open() until close() (explicit or via tick() timeout).
    bool isOpen() const;

    // Adds n * SELECTOR_STEP_MPH to the current value, clamps to
    // [SPEED_MIN_MPH, SPEED_MAX_MPH], rounds to the 0.1 grid, and refreshes
    // the activity timestamp used for the timeout. No-op when closed.
    void step(int n, uint32_t nowMs);

    // The current selected speed, in mph.
    float value() const;

    // Polls the inactivity timeout. Returns true exactly once, on the call
    // where (nowMs - lastActivity), interpreted as a wraparound-safe 32-bit
    // duration, first reaches SELECTOR_TIMEOUT_MS; that call also closes the
    // selector. No-op (returns false) when already closed.
    bool tick(uint32_t nowMs);

    // Closes the selector. Idempotent.
    void close();

private:
    bool m_open = false;
    float m_value = 0.0f;
    uint32_t m_lastActivity = 0;
};
