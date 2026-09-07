#include "CalendarAutoShow.h"

void CalendarAutoShow::setEnabled(bool on)
{
    m_enabled = on;
    // Forget any in-progress episode; re-enabling later needs its own fresh
    // lead-window crossing (no dismissed-start is recorded here).
    m_showing = false;
}

void CalendarAutoShow::setLeadSec(uint32_t s) { m_leadSec = s; }
void CalendarAutoShow::setStaySec(uint32_t s) { m_staySec = s; }

bool CalendarAutoShow::enabled() const { return m_enabled; }
uint32_t CalendarAutoShow::leadSec() const { return m_leadSec; }
uint32_t CalendarAutoShow::staySec() const { return m_staySec; }
bool CalendarAutoShow::isShowing() const { return m_showing; }

CalendarAutoShow::Action CalendarAutoShow::update(uint32_t nowEpoch, bool hasNext, uint32_t nextStart,
                                                   CardId currentCard, bool beltIdle, bool dataValid)
{
    if (!m_enabled || nowEpoch == 0) return Action::NONE;

    if (m_showing && currentCard != CardId::CALENDAR)
    {
        // The user left some other way (belt started, menu, another card):
        // the episode is over, and the event it was for must not fire
        // again.
        m_showing = false;
        m_dismissedStart = m_shownStart;
        m_haveDismissed = true;
        return Action::NONE;
    }

    if (m_showing)
    {
        if (nowEpoch >= m_shownStart + m_staySec)
        {
            m_showing = false;
            m_dismissedStart = m_shownStart;
            m_haveDismissed = true;
            return Action::RETURN_TO_CLOCK;
        }
        return Action::NONE;
    }

    if (!beltIdle || !dataValid || !hasNext || currentCard != CardId::CLOCK) return Action::NONE;
    if (m_haveDismissed && nextStart == m_dismissedStart) return Action::NONE;
    if (nextStart > nowEpoch + m_leadSec) return Action::NONE;
    // Lower bound as well as upper: an event already under way is not
    // something to interrupt the Clock for. Without this, arriving at the
    // Clock mid-meeting (boot, or a belt session that ends during one) shows
    // the card and then immediately hands it back on the next tick — a 250 ms
    // flash plus a backlight wake, and with stay = 0 that is guaranteed. Both
    // halves are needed: `nextStart < nowEpoch` catches a meeting still inside
    // its stay window, and the deadline test catches the moment the window has
    // already closed (including stay = 0 exactly at the start).
    if (nextStart < nowEpoch) return Action::NONE;
    if (nextStart + m_staySec <= nowEpoch) return Action::NONE;

    m_showing = true;
    m_shownStart = nextStart;
    return Action::SHOW_CALENDAR;
}

bool CalendarAutoShow::dismiss()
{
    if (!m_showing) return false;
    m_showing = false;
    m_dismissedStart = m_shownStart;
    m_haveDismissed = true;
    return true;
}

void CalendarAutoShow::noteManualNavigation() { m_showing = false; }
