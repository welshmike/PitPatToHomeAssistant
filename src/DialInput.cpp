#include "DialInput.h"

// Pure input logic — no Arduino, no floating hardware access. See DialInput.h
// for the API contract this implements.

void DialInput::wake(uint32_t nowMs)
{
    m_backlight = Backlight::FULL;
    m_lastActivityMs = nowMs;
    m_haveActivity = true;
}

void DialInput::updateBacklight(uint32_t nowMs)
{
    if (!m_haveActivity) {
        return;
    }
    uint32_t elapsed = nowMs - m_lastActivityMs; // uint32_t subtraction wraps correctly
    if (elapsed >= DIM_AFTER_MS) {
        m_backlight = Backlight::DIM;
    }
    // else: leave as-is. A reset to FULL happens explicitly wherever activity
    // is noted (wake(), noteActivity(), or the hasInput branch below). There
    // is no further stage past DIM — the backlight never turns off (4.8).
}

void DialInput::noteActivity(uint32_t nowMs)
{
    wake(nowMs);
}

DialEvents DialInput::tick(long encoderCount, bool touchDown, int touchX, int touchY,
                            bool btnClicked, bool btnSingleClicked, bool btnHold, uint32_t nowMs)
{
    DialEvents ev;

    long rawDelta = m_haveLastCount ? (encoderCount - m_lastCount) : 0;
    bool encoderMoving = rawDelta != 0;
    bool touchBeginning = touchDown && !m_touchActive;
    bool inputBeginning = touchBeginning || encoderMoving || btnClicked || btnSingleClicked || btnHold;

    if (inputBeginning && m_backlight != Backlight::FULL) {
        // Wake gating: this tick's input is entirely swallowed. Only wake=true
        // is reported; the backlight returns to FULL.
        ev.wake = true;
        wake(nowMs);

        if (touchBeginning) {
            m_touchActive = true;
            m_touchSwallowed = true;
            m_touchStartMs = nowMs;
            m_touchStartX = touchX;
            m_touchStartY = touchY;
            m_touchIsDrag = false;
            m_longPressFired = false;
            m_swipeFired = false;
        }

        // Discard the encoder pulses that arrived in this tick.
        m_lastCount = encoderCount;
        m_haveLastCount = true;

        return ev;
    }

    if (!m_haveLastCount) {
        // First-ever tick: establish the encoder baseline without emitting a
        // detent for whatever count the caller happens to start at.
        m_lastCount = encoderCount;
        m_haveLastCount = true;
    }

    if (!m_haveActivity) {
        // First-ever tick also establishes the activity/backlight-timer
        // baseline, so the dimming clock starts from "now", not from 0.
        m_lastActivityMs = nowMs;
        m_haveActivity = true;
    }

    bool hasInput = touchDown || encoderMoving || btnClicked || btnSingleClicked || btnHold;
    if (hasInput) {
        m_backlight = Backlight::FULL;
        m_lastActivityMs = nowMs;
    }

    // Detents.
    if (encoderMoving) {
        m_remainder += (int)rawDelta;
        ev.detents = m_remainder / PULSES_PER_DETENT;
        m_remainder = m_remainder % PULSES_PER_DETENT;
        m_lastCount = encoderCount;
    }

    // Touch: down / continuing / release.
    if (touchDown) {
        if (!m_touchActive) {
            m_touchActive = true;
            m_touchSwallowed = false;
            m_touchStartMs = nowMs;
            m_touchStartX = touchX;
            m_touchStartY = touchY;
            m_touchIsDrag = false;
            m_longPressFired = false;
            m_swipeFired = false;
        } else if (!m_touchSwallowed) {
            int dx = touchX - m_touchStartX;
            int dy = touchY - m_touchStartY;
            if (dx * dx + dy * dy > TAP_MAX_MOVE_PX * TAP_MAX_MOVE_PX) {
                m_touchIsDrag = true;
            }

            if (!m_swipeFired) {
                int adx = dx < 0 ? -dx : dx;
                int ady = dy < 0 ? -dy : dy;
                if (adx >= SWIPE_MIN_PX && adx > ady) {
                    ev.swipe = (dx > 0) ? 1 : -1;
                    m_swipeFired = true;
                }
            }

            uint32_t heldMs = nowMs - m_touchStartMs;
            if (m_touchIsDrag) {
                ev.holdProgress = 0.0f;
            } else if (!m_longPressFired) {
                float progress = (float)heldMs / (float)HOLD_MS;
                if (progress > 1.0f) {
                    progress = 1.0f;
                }
                ev.holdProgress = progress;
                if (heldMs >= HOLD_MS) {
                    ev.longPress = true;
                    m_longPressFired = true;
                }
            } else {
                ev.holdProgress = 0.0f;
            }
        }
    } else if (m_touchActive) {
        if (!m_touchSwallowed) {
            uint32_t heldMs = nowMs - m_touchStartMs;
            if (!m_touchIsDrag && !m_longPressFired && heldMs < TAP_MAX_MS) {
                ev.tap = true;
                ev.tapX = m_touchStartX;
                ev.tapY = m_touchStartY;
            }
        }
        m_touchActive = false;
        m_touchSwallowed = false;
        m_touchIsDrag = false;
        m_longPressFired = false;
        m_swipeFired = false;
        ev.holdProgress = 0.0f;
    }

    // Button.
    if (btnClicked) {
        ev.btnStop = true;
    }
    if (btnSingleClicked) {
        ev.btnClick = true;
    }
    if (btnHold) {
        ev.btnHold = true;
    }

    updateBacklight(nowMs);

    return ev;
}
