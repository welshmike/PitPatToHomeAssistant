#include "FlightsAutoShow.h"

void FlightsAutoShow::setEnabled(bool enabled)
{
    m_enabled = enabled;
    // Forget any in-progress episode; re-enabling later needs its own fresh
    // 0->>0 edge (tracked below via m_prevCount, which keeps updating even
    // while disabled).
    m_autoShown = false;
}

FlightsAutoShow::Action FlightsAutoShow::update(uint8_t aircraftCount, CardId currentCard, bool beltIdle,
                                               bool dataValid)
{
    // Stale or offline data is not evidence of anything: treat it as "no
    // aircraft" everywhere below, so an auto-shown card returns to Clock
    // when HA goes quiet and no fresh SHOW edge can be manufactured out of
    // a list nobody is maintaining any more.
    const uint8_t effCount = dataValid ? aircraftCount : 0;

    if (!m_enabled)
    {
        m_prevCount = effCount;
        return Action::NONE;
    }

    if (m_autoShown && currentCard != CardId::CLOCK && currentCard != CardId::FLIGHTS)
    {
        // The user left by some other means (e.g. the belt started and the
        // screen jumped to Treadmill): the episode is over, silently.
        m_autoShown = false;
    }

    Action action = Action::NONE;

    if (!m_autoShown && beltIdle && currentCard == CardId::CLOCK && m_prevCount == 0 && effCount > 0)
    {
        m_autoShown = true;
        action = Action::SHOW_FLIGHTS;
    }
    else if (m_autoShown && effCount == 0 && currentCard == CardId::FLIGHTS)
    {
        m_autoShown = false;
        action = Action::RETURN_TO_CLOCK;
    }

    m_prevCount = effCount;
    return action;
}

void FlightsAutoShow::noteManualNavigation()
{
    // User took over; don't auto-return later.
    m_autoShown = false;
}
