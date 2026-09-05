#pragma once

#include <stdint.h>

#include "CardRing.h"

// Pure state machine deciding when the Flights card should interrupt the
// Clock card, and when control should be handed back (spec 4.11 auto-show).
// Arduino-free; DialUi polls it (from the aircraft count in the latest
// FlightsSnapshot) every ~250 ms whether or not the Flights card is on
// screen, and applies whatever Action it returns.
class FlightsAutoShow
{
public:
    enum class Action
    {
        NONE,           // no change
        SHOW_FLIGHTS,   // switch the card ring to FLIGHTS
        RETURN_TO_CLOCK // switch the card ring back to CLOCK
    };

    // Enables/disables auto-show (the HA "Flights Auto-show" switch).
    // Disabling immediately forgets any in-progress auto-show episode, so a
    // later re-enable needs a fresh 0->>0 aircraft-count edge before it will
    // show again — it does not retroactively fire SHOW_FLIGHTS/RETURN_TO_CLOCK.
    void setEnabled(bool enabled);

    // Call once per poll with the latest aircraft count, the card currently
    // on screen, and whether the belt is idle (the screen resolves to a
    // card, not Connecting/Starting/Running/Selector). Returns the action
    // the caller should take, if any:
    //  - SHOW_FLIGHTS: enabled, beltIdle, currentCard == CLOCK, and the
    //    aircraft count has just gone from 0 to >0.
    //  - RETURN_TO_CLOCK: this state machine auto-showed Flights earlier,
    //    the count has dropped back to 0, and currentCard is still FLIGHTS
    //    (i.e. the user hasn't navigated away from it themselves).
    // If currentCard is neither CLOCK nor FLIGHTS while an auto-show is in
    // progress, the episode is silently forgotten (the user left some other
    // way, e.g. the belt started) — no RETURN_TO_CLOCK follows.
    Action update(uint8_t aircraftCount, CardId currentCard, bool beltIdle);

    // Call when the user manually navigates (knob scroll, side-button home)
    // while an auto-show is in progress: cancels the automatic return so a
    // later count drop to 0 doesn't fight the user's own navigation.
    void noteManualNavigation();

private:
    bool m_enabled = true;
    bool m_autoShown = false;
    uint8_t m_prevCount = 0;
};
