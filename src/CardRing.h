#pragma once
#include <stdint.h>

// Which desk-mode card is on screen while the belt is idle. Pure state
// machine — no Arduino, no display — driven by knob detents in DialUi. New
// cards (Calendar, Flights, Lights, Music — spec 4.8) are added to this ring
// by later sub-projects; keep COUNT last.
enum class CardId : uint8_t
{
    TREADMILL,
    CLOCK,
    COUNT
};

// Wraps a single "current card" cursor around the CardId ring. next()/prev()
// wrap at both ends so the knob can be spun freely in either direction.
class CardRing
{
public:
    CardId current() const { return m_current; }

    void next()
    {
        uint8_t i = static_cast<uint8_t>(m_current) + 1;
        if (i >= static_cast<uint8_t>(CardId::COUNT)) {
            i = 0;
        }
        m_current = static_cast<CardId>(i);
    }

    void prev()
    {
        uint8_t i = static_cast<uint8_t>(m_current);
        if (i == 0) {
            i = static_cast<uint8_t>(CardId::COUNT);
        }
        m_current = static_cast<CardId>(i - 1);
    }

    void set(CardId id) { m_current = id; }

private:
    // Boot card is Clock (spec 4.8): no idle return, the ring starts here.
    CardId m_current = CardId::CLOCK;
};
