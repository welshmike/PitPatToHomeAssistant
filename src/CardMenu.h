#pragma once
#include <stdint.h>
#include "CardRing.h"

// Pure state machine + shared geometry for the side-button ring menu that
// picks one of the six cards. Ring order matches CardId's declaration order
// (TREADMILL, CLOCK, FLIGHTS, LIGHT_OFFICE, LIGHT_LAMP, CALENDAR), COUNT
// excluded — see the static_assert below. No Arduino/display dependency:
// DialUi drives the state machine and reuses itemCentre()/hitTest() so the
// view draws and hit-tests the exact same six spots.
class CardMenu
{
public:
    static constexpr uint32_t kIdleCloseMs = 8000;
    static constexpr int kRingRadius = 88;
    static constexpr int kItemRadius = 18;
    static constexpr int kItemRadiusHighlight = 22;
    static constexpr int kHitRadius = 26;

    // Opens the menu with `current` highlighted and records nowMs as the
    // last-activity time for the idle-close timeout.
    void open(CardId current, uint32_t nowMs);

    // Closes the menu. Idempotent.
    void close();

    bool isOpen() const { return m_open; }

    CardId highlight() const { return m_highlight; }

    // Moves the highlight by n ring positions (wraps both ways) and
    // refreshes the idle timer. No-op when closed.
    void detents(int n, uint32_t nowMs);

    // Returns the current highlight and closes the menu. Calling this while
    // already closed still returns the last highlight (from the most recent
    // open()/detents()) and leaves the menu closed — it does not reopen it
    // or otherwise change state.
    CardId select();

    // Polls the idle timeout. Returns true exactly once, on the call where
    // (nowMs - lastActivity), interpreted as a wraparound-safe 32-bit
    // duration, first reaches kIdleCloseMs; that call also closes the menu.
    // No-op (returns false) when already closed.
    bool tick(uint32_t nowMs);

    // Geometry shared with the view: index 0 is 12 o'clock, clockwise,
    // around centre (120,120), at kRingRadius.
    static void itemCentre(uint8_t index, int& x, int& y);

    // Index of the item whose centre is within kHitRadius of (x,y), or -1
    // if none. Ties resolve to the lowest index (ring order).
    static int8_t hitTest(int x, int y);

private:
    static_assert(static_cast<uint8_t>(CardId::COUNT) == 6, "CardMenu ring assumes 6 cards");

    bool m_open = false;
    CardId m_highlight = CardId::TREADMILL;
    uint32_t m_lastActivity = 0;
};
