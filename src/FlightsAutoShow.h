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

    // How long the (effective) aircraft count has to stay at 0 before an
    // auto-shown Flights card hands back to Clock. Aircraft drop in and out
    // of HA's list as they cross the edge of its search area, so without
    // this the card would bounce back to Clock on a single empty poll and
    // then raise itself again seconds later (final review 2026-09-05). Any
    // non-zero count inside the window restarts it.
    static constexpr uint32_t kReturnHoldMs = 15000;

    // Enables/disables auto-show (the HA "Flights Auto-show" switch).
    // Disabling immediately forgets any in-progress auto-show episode, so a
    // later re-enable needs a fresh 0->>0 aircraft-count edge before it will
    // show again — it does not retroactively fire SHOW_FLIGHTS/RETURN_TO_CLOCK.
    void setEnabled(bool enabled);

    // Call once per poll with the latest aircraft count, the card currently
    // on screen, whether the belt is idle (the screen resolves to a card,
    // not Connecting/Starting/Running/Selector) and whether that count can
    // be trusted. `dataValid` is false when the snapshot is stale or
    // offline (HA gone quiet, or MQTT down): the count on screen may be
    // minutes old, so it is treated as 0 for BOTH decisions below — no
    // SHOW_FLIGHTS fires on untrusted data, and an auto-shown card hands
    // itself back to Clock rather than sitting on aircraft that may have
    // long since flown past. Returns the action the caller should take, if
    // any:
    //  - SHOW_FLIGHTS: enabled, beltIdle, currentCard == CLOCK, and the
    //    (effective) aircraft count has just gone from 0 to >0.
    //  - RETURN_TO_CLOCK: this state machine auto-showed Flights earlier,
    //    the (effective) count has been 0 continuously for kReturnHoldMs,
    //    and currentCard is still FLIGHTS (i.e. the user hasn't navigated
    //    away themselves).
    // `nowMs` is millis() from the caller; comparisons are wrap-safe.
    // If currentCard is neither CLOCK nor FLIGHTS while an auto-show is in
    // progress, the episode is silently forgotten (the user left some other
    // way, e.g. the belt started) — no RETURN_TO_CLOCK follows.
    Action update(uint32_t nowMs, uint8_t aircraftCount, CardId currentCard, bool beltIdle, bool dataValid);

    // Call when the user manually navigates (a card-menu selection, the
    // side button's hold-home, or the knob/a tap on the Flights card itself)
    // while an auto-show is in progress: cancels the automatic return so a
    // later count drop to 0 doesn't fight the user's own navigation.
    void noteManualNavigation();

private:
    bool m_enabled = true;
    bool m_autoShown = false;
    uint8_t m_prevCount = 0;

    // Start of the current run of zero (effective) aircraft — the return
    // hold above. m_haveZeroSince is false whenever the last count seen was
    // non-zero, which is what restarts the window.
    bool     m_haveZeroSince = false;
    uint32_t m_zeroSinceMs   = 0;
};
