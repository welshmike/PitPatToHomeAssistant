#pragma once

#include <stdint.h>
#include <stddef.h>

#include "board.h"
// board.h already pulls config.h in (guarded the same way) to decide
// HAS_CALENDAR, but the CALENDAR_TOKEN check below needs it directly too;
// same guard as board.h so a native build that somehow reached this header
// (it doesn't today — see the HAS_CALENDAR comment below) can't be made to
// require the gitignored config.h just by being compiled with NATIVE_TEST.
#if !defined(NATIVE_TEST)
#include "config.h"
#endif
#include "CalendarModel.h"
#include "Guarded.h"

class NetManager;

// Calendar feed (spec 4.19, amended 2026-09-07): the Calendar card's only
// network call. CALENDAR_URL is normally the Mac-mini relay's plain-HTTP
// address on the LAN (doc/CALENDAR_FEED.md) — the same shape as
// FlightsService's logo GETs — but an https:// URL (e.g. talking to Google
// directly) is still supported and is this project's only TLS fetch,
// verified against Google's own root CAs (CalendarCerts.h). CalendarService
// picks the client from the URL scheme at compile time; see kIsHttps in
// CalendarService.cpp. Polls on an interval with exponential back-off on
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

// CALENDAR_TOKEN is optional (doc/CALENDAR_FEED.md): the Mac-mini relay only
// checks `?k=` when its own TOKEN is configured. When CALENDAR_TOKEN is
// defined it is appended as "?k=" CALENDAR_TOKEN; otherwise the fetch uses
// CALENDAR_URL as-is. See kUrl in CalendarService.cpp.

class CalendarService
{
public:
    static constexpr uint32_t kPollMs = 60000, kBackoffMinMs = 30000, kBackoffMaxMs = 1800000;
    static constexpr size_t   kBufCap = 2048;

    explicit CalendarService(NetManager &net);

    // Nothing to mount (no LittleFS use, unlike FlightsService) — present
    // for symmetry with the other net-task services and in case a future
    // step needs one-time setup here. Called from setup() on the caller's
    // task, before the net task exists.
    void begin();

    // Net task only: fetches on the poll interval / back-off schedule
    // (fetchNow()), and immediately if requestRefresh() was called and the
    // last fetch is more than 60 s old.
    //
    // Blocks the net task for the duration of a fetch. kFetchBudgetMs
    // (12 s) bounds only the body read; HTTPClient::GET()'s header phase
    // (connect + response headers) runs before that budget is even set and
    // is bounded per hop by the connect/read timeouts (4 s + 6 s), not
    // interruptibly. The Mac-mini relay answers directly (one hop), but an
    // https:// CALENDAR_URL pointed at Google redirects to a second host, so
    // a server that stalls before headers on both hops can hold the net task
    // for ~2 * (4 + 6) = 20 s before the read window even starts — worst
    // case ≈ 20-22 s on a pathological server. NetManager sets a 60 s MQTT
    // keepalive (rather than PubSubClient's 15 s default) to give the
    // broker enough tolerance for that. Every other tick returns in µs.
    void tick(uint32_t nowMs);

    // Loop task asks for an early refresh (nudge due within 60 s); honoured
    // if the last fetch is > 60 s old. Loop-task safe: a single volatile
    // bool write/read, same shape as FlightsService's setVisible().
    void requestRefresh();

    // Loop task: safe from any task. Returns a copy.
    CalendarModel::Snapshot snapshot() const;

    // Loop task: true once a fetch has fully succeeded — the GET returned
    // 200 and the body parsed — so snapshot() holds real data. A failed
    // attempt leaves it false however many times it is retried.
    bool fetchedOnce() const;

private:
    // Wall-clock budget for the body read, once headers are in — it does
    // not bound HTTPClient::GET()'s header phase (see tick()'s comment for
    // the worst case there). The net task also drives NetManager::tick(),
    // which is why NetManager gives PubSubClient a 60 s keepalive rather
    // than its 15 s default: a fetch that is going badly past this budget
    // is abandoned (and backed off) inside it, but the header phase alone
    // can already run past the old 15 s default.
    static constexpr uint32_t kFetchBudgetMs = 12000;

    // Net task only. Heap-guards, then performs the HTTPS GET + JSON parse;
    // returns true on a successfully parsed snapshot, false otherwise
    // (caller applies back-off).
    bool fetchNow(uint32_t nowMs);

    NetManager &m_net;
    Guarded<CalendarModel::Snapshot> m_snapshot;

    uint32_t m_nextFetchMs = 0;
    uint32_t m_backoffMs   = 0;

    // When the last fetch was *attempted* — recorded even for the attempts
    // that the heap guard turns away, because the 60 s rule in tick() is
    // about how recently we last went near the network, not about success.
    // Tracked directly rather than derived from m_nextFetchMs: the heap
    // guard reschedules without touching m_backoffMs, which would make any
    // such derivation read a stale interval and let a latched
    // m_refreshWanted re-enter fetchNow() on every tick.
    uint32_t m_lastFetchMs   = 0;
    bool     m_haveLastFetch = false;

    // volatile like m_refreshWanted: written on the net task, read on the
    // loop task (fetchedOnce()). A plain bool would let the compiler hoist
    // the loop task's read out of its polling loop.
    volatile bool m_fetchedOnce   = false;
    volatile bool m_refreshWanted = false;

    // One-shot latch for the net-task stack high-water-mark log, which
    // fires after the first fetch *attempt* — a failing fetch goes just as
    // deep as a succeeding one, and is the more likely first result on a
    // misconfigured feed. Net task only.
    bool m_stackLogged = false;

    static uint8_t s_buf[kBufCap];
};
#endif // HAS_CALENDAR
