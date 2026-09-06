#include "CardMenu.h"

#include <math.h>

// Pure state machine — no Arduino, no display access. See CardMenu.h for the
// API contract this implements.

namespace
{

constexpr int kCentreX = 120;
constexpr int kCentreY = 120;
constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;

} // namespace

void CardMenu::open(CardId current, uint32_t nowMs)
{
    m_open = true;
    m_highlight = current;
    m_lastActivity = nowMs;
}

void CardMenu::close()
{
    m_open = false;
}

void CardMenu::detents(int n, uint32_t nowMs)
{
    if (!m_open) {
        return;
    }

    const int count = static_cast<int>(CardId::COUNT);
    int idx = (static_cast<int>(m_highlight) + n) % count;
    if (idx < 0) {
        idx += count;
    }
    m_highlight = static_cast<CardId>(idx);
    m_lastActivity = nowMs;
}

CardId CardMenu::select()
{
    m_open = false;
    return m_highlight;
}

bool CardMenu::tick(uint32_t nowMs)
{
    if (!m_open) {
        return false;
    }

    if (static_cast<int32_t>(nowMs - m_lastActivity) >= static_cast<int32_t>(kIdleCloseMs)) {
        m_open = false;
        return true;
    }

    return false;
}

void CardMenu::itemCentre(uint8_t index, int& x, int& y)
{
    const float angleDeg = -90.0f + static_cast<float>(index) * 72.0f;
    const float angleRad = angleDeg * kDegToRad;
    x = static_cast<int>(lroundf(static_cast<float>(kCentreX) + static_cast<float>(kRingRadius) * cosf(angleRad)));
    y = static_cast<int>(lroundf(static_cast<float>(kCentreY) + static_cast<float>(kRingRadius) * sinf(angleRad)));
}

int8_t CardMenu::hitTest(int x, int y)
{
    const uint8_t count = static_cast<uint8_t>(CardId::COUNT);
    for (uint8_t i = 0; i < count; ++i) {
        int cx = 0;
        int cy = 0;
        itemCentre(i, cx, cy);
        const int dx = x - cx;
        const int dy = y - cy;
        if (dx * dx + dy * dy <= kHitRadius * kHitRadius) {
            return static_cast<int8_t>(i);
        }
    }
    return -1;
}
