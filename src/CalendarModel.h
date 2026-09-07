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

// Index of the first non-all-day event whose end > nowEpoch, or -1.
int8_t nextTimed(const Snapshot& s, uint32_t nowEpoch);

// Index of the first event on a later local day than nowEpoch (used for "Tomorrow …"), or -1.
// `localDayOf(epoch)` is the caller-supplied day number (e.g. days since epoch in local time).
int8_t firstOnLaterDay(const Snapshot& s, uint32_t nowEpoch, uint32_t (*localDayOf)(uint32_t epoch));

// Same as firstOnLaterDay(), but skips all-day events: an all-day event has no
// meaningful start time to show next to "Tomorrow", so it would otherwise
// misreport e.g. a bank holiday as "Tomorrow 01:00 Bank Holiday". Returns the
// first *timed* event on a later local day than nowEpoch, or -1.
int8_t firstTimedOnLaterDay(const Snapshot& s, uint32_t nowEpoch,
                            uint32_t (*localDayOf)(uint32_t epoch));

// Writes one of: "in N min" (N < 60), "in H h MM" (>= 60 min), "now, N min left", "starts now"
// (|start - now| < 30 s, not yet ended). Returns chars written (0 if event has ended).
size_t countdownText(const Event& e, uint32_t nowEpoch, char* buf, size_t cap);

// Count of all-day events in the snapshot.
uint8_t allDayCount(const Snapshot& s);

bool isStale(const Snapshot& s, uint32_t nowEpoch);

} // namespace CalendarModel
