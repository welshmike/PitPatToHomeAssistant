#pragma once

#include <stdint.h>
#include <stddef.h>

// Arduino-free JSON parsing for the Calendar card (spec 4.19), buildable and
// testable on the host (native env). Turns the Apps Script payload
// {"t":<epoch>,"ev":[{"s":<start>,"e":<end>,"n":<title>,"a":<0|1 allDay>,
// "l":<where>}, ...]} into a POD snapshot the UI can render directly.
namespace CalendarModel
{

constexpr uint8_t  kMaxEvents = 5;
constexpr uint32_t kStaleSec  = 1800;

struct Event
{
    uint32_t start = 0;
    uint32_t end   = 0;
    bool     allDay = false;
    char     title[41] = {0};
    char     where[25] = {0};
};

struct Snapshot
{
    bool     valid = false;
    uint32_t fetchedAtEpoch = 0;
    uint8_t  count = 0;
    Event    ev[kMaxEvents];
};

// Parses the Apps Script payload. On success fills `out` (valid=true, fetchedAtEpoch = "t") and
// returns true; on malformed JSON / missing "ev" returns false and leaves `out` untouched.
bool parse(const char* json, size_t len, Snapshot& out);

// Index of the first non-all-day event whose end > nowEpoch, or -1. Day-blind:
// the feed runs to the end of *tomorrow*, so after today's last meeting this
// resolves to tomorrow's first one. That is what the nudge wants (a 09:00
// meeting should still nudge from the Clock at 08:55, even though 08:55 and
// 09:00 straddle no day boundary but a late-night session might), but it is
// not what the card face wants — see nextTimedToday() for that.
int8_t nextTimed(const Snapshot& s, uint32_t nowEpoch);

// Index of the first non-all-day event on the *same local day* as nowEpoch
// whose end > nowEpoch, or -1. This is the card face's "next meeting": once
// today's last one has ended it returns -1, which is what makes the
// "nothing more today" + "Tomorrow …" face reachable.
// `localDayOf(epoch)` is the caller-supplied day number (e.g. days since epoch
// in local time); a plain function pointer, so the caller parks whatever
// timezone context it needs somewhere the callback can reach.
int8_t nextTimedToday(const Snapshot& s, uint32_t nowEpoch, uint32_t (*localDayOf)(uint32_t epoch));

// Number of all-day events whose start falls on nowEpoch's local day. The feed
// carries tomorrow's all-day events too, and they must not be counted into
// today's "All day: …" line.
uint8_t allDayCountToday(const Snapshot& s, uint32_t nowEpoch,
                         uint32_t (*localDayOf)(uint32_t epoch));

// Index of the first all-day event on nowEpoch's local day, or -1. Used for the
// single-event form of that line ("All day: Offsite").
int8_t firstAllDayToday(const Snapshot& s, uint32_t nowEpoch,
                        uint32_t (*localDayOf)(uint32_t epoch));

// Index of the first *timed* event on a later local day than nowEpoch, or -1
// (used for "Tomorrow …"). All-day events are skipped: one has no meaningful
// start time to show next to "Tomorrow", so it would otherwise misreport e.g. a
// bank holiday as "Tomorrow 01:00 Bank Holiday".
int8_t firstTimedOnLaterDay(const Snapshot& s, uint32_t nowEpoch,
                            uint32_t (*localDayOf)(uint32_t epoch));

// Writes one of: "in N min" (N < 60), "in H h MM" (>= 60 min), "now, N min left", "starts now"
// (|start - now| < 30 s, not yet ended). Returns chars written (0 if event has ended).
size_t countdownText(const Event& e, uint32_t nowEpoch, char* buf, size_t cap);

// Count of all-day events in the whole snapshot, today's and tomorrow's alike.
// The card face wants allDayCountToday(); this is the day-blind total.
uint8_t allDayCount(const Snapshot& s);

bool isStale(const Snapshot& s, uint32_t nowEpoch);

// Fills `titleBuf` with the title and `timeBuf` with "HH:MM" for today's
// next timed event (nextTimedToday). Returns the title length (0 = nothing
// to show: no event left today, an all-day-only day, or an invalid/empty
// snapshot); both buffers are left empty and NUL-terminated in that case.
// `live` is set true when the event is in progress or starts within 30 s,
// false otherwise (including when nothing is written). `hhmm` formats an
// epoch as local "HH:MM" the same way the Calendar card does. Each buffer is
// truncated to fit its own `cap` (NUL-terminated, returns what fits) rather
// than overflowing; trimming to the round bezel is the view's job, not this
// one's.
size_t clockLine(const Snapshot& s, uint32_t nowEpoch, uint32_t (*localDayOf)(uint32_t),
                 void (*hhmm)(uint32_t epoch, char* out, size_t cap), char* titleBuf,
                 size_t titleCap, char* timeBuf, size_t timeCap, bool& live);

} // namespace CalendarModel
