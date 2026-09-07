#pragma once

#include <stdint.h>

#include "CardRing.h"

// Pure state machine deciding when the Calendar card should interrupt the
// Clock card ahead of a meeting, and when control should be handed back
// (spec 4.19 nudge). Arduino-free; DialUi polls it every ~250 ms with the
// current CalendarModel::nextTimed() event for the latest snapshot, whether
// or not the Calendar card is on screen, and applies whatever Action it
// returns. Same shape as FlightsAutoShow.
//
// Dismissed-start rule: a tap on the card during a nudge (dismiss()), and
// the auto-return after the stay window, both remember the *start epoch*
// of the event that was showing (m_dismissedStart), not just "an episode
// happened". That single remembered start is compared against nextStart on
// every later update(): as long as CalendarModel::nextTimed() keeps
// resolving to the same event (same start), no new nudge fires for it,
// but the moment the snapshot advances to a different event (a different
// start epoch) the block no longer applies. This is what makes "dismiss
// blocks only that event" and "the same event does not re-nudge after the
// return" hold, while a later, different meeting still nudges normally.
// Manual navigation away (noteManualNavigation()) is deliberately weaker:
// it forgets the in-progress episode without recording a dismissed start,
// so it never blocks the event it interrupted.
class CalendarAutoShow
{
public:
    enum class Action
    {
        NONE,            // no change
        SHOW_CALENDAR,   // switch the card ring to CALENDAR
        RETURN_TO_CLOCK  // switch the card ring back to CLOCK
    };

    // Enables/disables the nudge (the HA "Calendar nudge" switch). Disabling
    // immediately forgets any in-progress episode (isShowing() becomes
    // false), so a later re-enable needs a fresh lead-window crossing
    // before it will show again — it does not retroactively fire
    // SHOW_CALENDAR/RETURN_TO_CLOCK, and it does not record a dismissed
    // start (the interrupted event can still nudge again once re-enabled).
    void setEnabled(bool on);

    // Seconds before an event's start at which the nudge fires. Default 300.
    void setLeadSec(uint32_t s);

    // Seconds after an event's start that an auto-shown card stays up
    // before handing back to Clock. Default 60.
    void setStaySec(uint32_t s);

    bool enabled() const;
    uint32_t leadSec() const;
    uint32_t staySec() const;

    // Call once per poll. hasNext/nextStart describe
    // CalendarModel::nextTimed() for the current snapshot; dataValid is
    // "snapshot valid and not stale". nowEpoch is TimeService time (0 when
    // the clock is not valid -> treated as !dataValid, i.e. no action).
    // Returns:
    //  - SHOW_CALENDAR: enabled, beltIdle, currentCard == CLOCK, data valid,
    //    hasNext, nextStart is within leadSec of nowEpoch, the event has not
    //    already started (and its return deadline, nextStart + staySec, is
    //    still in the future — otherwise the card would show and be handed
    //    straight back), and nextStart is not the dismissed-start (see above).
    //  - RETURN_TO_CLOCK: this machine auto-showed Calendar earlier, the
    //    stay window has elapsed, and currentCard is still CALENDAR (the
    //    user hasn't navigated away themselves).
    //  - NONE otherwise. If currentCard is anything other than CALENDAR
    //    while a nudge is showing, the episode is over, silently: the user
    //    left some other way (belt started, menu, another card), and that
    //    event's start is recorded so it does not nudge again.
    Action update(uint32_t nowEpoch, bool hasNext, uint32_t nextStart, CardId currentCard,
                  bool beltIdle, bool dataValid);

    // Call when the card itself is tapped during a nudge: ends the episode,
    // records that event's start so it never nudges again, and tells the
    // caller to switch to Clock. Returns false (no-op) if no episode was
    // showing.
    bool dismiss();

    // Call when the user manually navigates away (knob, card menu, side
    // button) while a nudge is showing: ends the episode without recording
    // a dismissed start, so the same event can still nudge again later
    // (e.g. if the snapshot re-resolves nextTimed() to it after some other
    // card took over in between).
    void noteManualNavigation();

    bool isShowing() const;

private:
    bool     m_enabled = true;
    uint32_t m_leadSec = 300;
    uint32_t m_staySec = 60;

    bool     m_showing = false;
    uint32_t m_shownStart = 0;

    // The start epoch of the last event whose nudge episode ended via
    // dismiss() or an auto-return (not via noteManualNavigation() or
    // setEnabled(false)) — see the dismissed-start rule above.
    uint32_t m_dismissedStart = 0;
    bool     m_haveDismissed = false;
};
