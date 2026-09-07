#pragma once

#include <stdint.h>
#include <stddef.h>

#include "board.h"
#include "config.h"
#include "CalendarModel.h"
#include "Guarded.h"

class NetManager;

// Apps Script calendar feed (spec 4.19): the Calendar card's only network
// call, and the only TLS fetch in this project (everything else — MQTT,
// FlightsService's logo GETs — is plain-TCP on the LAN). Fetches
// CALENDAR_URL over HTTPS, verified against Google's own root CAs
// (CalendarCerts.h), on a poll interval with exponential back-off on
// failure.
//
// Threading, mirroring FlightsService: begin()/tick() run on the network
// task only (see NetTask.h/.cpp) and must never be called from the loop
// task. The loop task talks to this class only through requestRefresh(),
// snapshot() and fetchedOnce(), all of which are safe from any task.
//
// Device-only: this header is portable (no Arduino/ESP32 types leak into
// the class's public surface), but the whole .cpp is compiled out (empty
// translation unit) unless HAS_CALENDAR, so the class only actually does
// anything on a build where config.h defines CALENDAR_URL. NetTask.h only
// declares/uses the m_calendar member under the same guard, so a build
// without CALENDAR_URL never instantiates or calls it.
#if HAS_CALENDAR
class CalendarService
{
public:
    static constexpr uint32_t kPollMs = 300000, kBackoffMinMs = 30000, kBackoffMaxMs = 1800000;
    static constexpr size_t   kBufCap = 2048;

    explicit CalendarService(NetManager &net);

    // Nothing to mount (no LittleFS use, unlike FlightsService) — present
    // for symmetry with the other net-task services and in case a future
    // step needs one-time setup here. Net task only.
    void begin();

    // Net task only: fetches on the poll interval / back-off schedule
    // (fetchNow()), and immediately if requestRefresh() was called and the
    // last fetch is more than 60 s old.
    void tick(uint32_t nowMs);

    // Loop task asks for an early refresh (nudge due within 60 s); honoured
    // if the last fetch is > 60 s old. Loop-task safe: a single volatile
    // bool write/read, same shape as FlightsService's setVisible().
    void requestRefresh();

    // Loop task: safe from any task. Returns a copy.
    CalendarModel::Snapshot snapshot() const;

    // Loop task: true once the first fetch (success or failure that still
    // produced a parsed snapshot) has completed.
    bool fetchedOnce() const;

private:
    // Net task only. Heap-guards, then performs the HTTPS GET + JSON parse;
    // returns true on a successfully parsed snapshot, false otherwise
    // (caller applies back-off).
    bool fetchNow(uint32_t nowMs);

    NetManager &m_net;
    Guarded<CalendarModel::Snapshot> m_snapshot;

    uint32_t m_nextFetchMs = 0;
    uint32_t m_backoffMs   = 0;
    bool     m_fetchedOnce = false;
    volatile bool m_refreshWanted = false;

    static uint8_t s_buf[kBufCap];
};
#endif // HAS_CALENDAR
