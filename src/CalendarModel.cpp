#include "CalendarModel.h"
#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>

namespace CalendarModel
{
namespace
{
void copyClipped(char* dst, size_t cap, const char* src)
{
    if (src == nullptr) { dst[0] = '\0'; return; }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}
} // namespace

bool parse(const char* json, size_t len, Snapshot& out)
{
    JsonDocument doc;
    if (deserializeJson(doc, json, len) != DeserializationError::Ok) return false;
    JsonArrayConst ev = doc["ev"].as<JsonArrayConst>();
    if (ev.isNull()) return false;
    Snapshot s;
    s.valid = true;
    s.fetchedAtEpoch = doc["t"] | 0u;
    for (JsonObjectConst o : ev)
    {
        if (s.count >= kMaxEvents) break;
        Event& e = s.ev[s.count];
        e.start  = o["s"] | 0u;
        e.end    = o["e"] | 0u;
        e.allDay = (o["a"] | 0) != 0;
        copyClipped(e.title, sizeof(e.title), o["n"] | "");
        copyClipped(e.where, sizeof(e.where), o["l"] | "");
        ++s.count;
    }
    out = s;
    return true;
}

int8_t nextTimed(const Snapshot& s, uint32_t nowEpoch)
{
    for (uint8_t i = 0; i < s.count; ++i)
        if (!s.ev[i].allDay && s.ev[i].end > nowEpoch) return (int8_t)i;
    return -1;
}

int8_t firstOnLaterDay(const Snapshot& s, uint32_t nowEpoch, uint32_t (*localDayOf)(uint32_t))
{
    const uint32_t today = localDayOf(nowEpoch);
    for (uint8_t i = 0; i < s.count; ++i)
        if (localDayOf(s.ev[i].start) > today) return (int8_t)i;
    return -1;
}

size_t countdownText(const Event& e, uint32_t nowEpoch, char* buf, size_t cap)
{
    if (buf == nullptr || cap == 0) return 0;
    if (nowEpoch >= e.end) { buf[0] = '\0'; return 0; }
    int n = 0;
    if (nowEpoch + 30 < e.start)
    {
        const uint32_t mins = (e.start - nowEpoch + 30) / 60; // round to nearest minute
        if (mins < 60) n = snprintf(buf, cap, "in %u min", (unsigned)mins);
        else n = snprintf(buf, cap, "in %u h %02u", (unsigned)(mins / 60), (unsigned)(mins % 60));
    }
    else if (nowEpoch < e.start + 30)
    {
        n = snprintf(buf, cap, "starts now");
    }
    else
    {
        const uint32_t left = (e.end - nowEpoch + 30) / 60;
        n = snprintf(buf, cap, "now, %u min left", (unsigned)left);
    }
    return (n < 0 || (size_t)n >= cap) ? 0 : (size_t)n;
}

uint8_t allDayCount(const Snapshot& s)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < s.count; ++i) if (s.ev[i].allDay) ++n;
    return n;
}

bool isStale(const Snapshot& s, uint32_t nowEpoch)
{
    return !s.valid || nowEpoch - s.fetchedAtEpoch >= kStaleSec;
}
} // namespace CalendarModel
