#pragma once

#include <stdint.h>
#include <stddef.h>
#include <atomic>

#include "board.h"
#include "config.h"
#include "FlightsModel.h"
#include "Guarded.h"

// HOME_LAT/HOME_LON/FLIGHTS_RADIUS_MI (spec 4.9) come from config.h. These
// fallbacks keep the build working (and testable) without a config.h edit —
// central London, 3 mi radius — but flag it on the Dial build so the
// fallback doesn't go unnoticed on real hardware.
#ifndef HOME_LAT
#define HOME_LAT 51.5074
#if HAS_DIAL_UI
#warning "HOME_LAT not set in config.h; flights card uses central London"
#endif
#endif

#ifndef HOME_LON
#define HOME_LON -0.1278
#endif

#ifndef FLIGHTS_RADIUS_MI
#define FLIGHTS_RADIUS_MI 3
#endif

class NetManager;

// Fetches aircraft near HOME_LAT/HOME_LON from adsb.fi, enriches them with
// route/operator detail from hexdb.io, and caches airline logos in LittleFS
// — all for the Dial's Flights card (spec 4.9). Every network call happens
// on the network task (see NetTask.h/.cpp): begin() and tick() must only
// ever be called from there. The loop task (DialUi) talks to this class
// only through setVisible(), setWantedLogo(), snapshot() and logoReady(),
// all of which are safe to call from any task. The logo PNG itself is never
// resident in this class — it lives only in /logos/{IATA}.png on LittleFS;
// DialUi decodes it straight from there once logoReady() says the file is
// present and valid (M1, spec review 2026-09-04).
//
// Device-only: this header is portable (no Arduino/ESP32 types), but the
// whole .cpp is compiled out (empty translation unit) unless HAS_DIAL_UI,
// so the class only actually does anything on the Dial build. NetTask.h
// only declares/uses the m_flights member under the same guard, so the
// DevKit build never instantiates or calls it.
class FlightsService
{
public:
    explicit FlightsService(NetManager &net) : m_net(net) {}

    // Mounts LittleFS on the `spiffs` partition (format-on-fail) and
    // creates /logos if missing. Logs the result and free space. Called
    // once from NetTask::begin(), which itself runs on the caller's task
    // (setup()), not the spawned net task — same as NetManager::begin()
    // being deferred, this is a one-time boot-time call, never repeated.
    void begin();

    // loop task -> net task, both benign single-word/small-buffer writes
    // guarded so a torn read on the net task is never observed.
    void setVisible(bool visible);
    void setWantedLogo(const char *iata); // up to 2 chars + NUL; longer is truncated

    // Net task only: drives the 20 s aircraft refresh, hexdb enrichment
    // (budgeted per call) and the logo download/cache for the wanted IATA.
    void tick(uint32_t nowMs);

    // loop task: safe from any task. Returns a copy.
    FlightsModel::FlightsSnapshot snapshot() const { return m_snapshot.read(); }

    // loop task: true once `iata`'s cached logo file (/logos/{iata}.png on
    // LittleFS) is confirmed present and valid — i.e. DialUi can go straight
    // to drawPngFile() on it. Never triggers network work itself — the
    // fetch/validate happens in tick() for whatever IATA setWantedLogo()
    // last set (M1, spec review 2026-09-04: no logo bytes are ever resident
    // in this class any more, only this tiny ready flag).
    bool logoReady(const char *iata) const;
    // Loop-task safe: the UI failed to decode this cached logo. Clears the
    // ready flag and deletes the file so tick() re-downloads it once.
    void invalidateLogo(const char *iata);

private:
    // Not part of FlightsModel: it's local buffering/caching state, not a
    // wire format, so it stays private to this class. Deliberately tiny —
    // no PNG bytes here (M1): the file on LittleFS is the only copy, and
    // DialUi decodes it directly from there.
    struct LogoStatus
    {
        char iata[3] = {0};
        bool ready   = false;
    };
    struct WantedIata
    {
        char v[3] = {0};
    };
    struct RouteEntry
    {
        char callsign[9] = {0};
        char from[5]     = {0};
        char to[5]       = {0};
    };
    struct AirportEntry
    {
        char icao[5] = {0};
        char iata[5] = {0};
    };
    struct OperatorEntry
    {
        char hex[7]          = {0};
        char operatorIcao[4] = {0};
        char operatorName[32] = {0};
    };
    struct MissingLogo
    {
        char iata[3] = {0};
    };
    // I1: per-ICAO failure counter for the airport-IATA lookup used inside
    // enrichRoute() (hexdb.io /api/v1/airport/icao/{ICAO}). Unlike the
    // route/operator lookups (which give up and negative-cache after their
    // one attempt for the tick), a from/to airport can legitimately take a
    // couple of tries to resolve, so this only gives up — caching an empty
    // IATA in m_airportCache via airportCachePut(icao, "") — after 2
    // failures, so a persistently-unresolvable ICAO stops consuming the
    // enrichment budget every tick.
    struct AirportFailure
    {
        char    icao[5] = {0};
        uint8_t count    = 0;
    };

    void fetchAircraft(uint32_t nowMs);
    void enrich();
    // Returns the remaining budget. `budget` is the count of hexdb requests
    // still allowed this tick() call.
    int  enrichRoute(FlightsModel::Aircraft &a, int budget);
    int  enrichOperator(FlightsModel::Aircraft &a, int budget);
    void tickLogo(uint32_t nowMs);

    // I1: true once more than kTickWallCapMs has elapsed since this tick()
    // call began (m_tickStartMs, set at the top of tick()). Checked before
    // every hexdb/logo request past the first so a slow or stalled request
    // can't push the whole net task loop iteration past MQTT's 15 s
    // keepalive — NetTask::run() only calls PubSubClient's loop()
    // (m_net.tick()) between tick() calls, never during one, so tick()
    // itself must stay well under that. Aircraft fetch is exempt (it's
    // gated by its own 20 s interval and already has the tightest deadline
    // of the four request kinds); this only bounds how many *additional*
    // enrichment/logo requests a single tick() can chain.
    bool tickBudgetExceeded() const;

    // M2: (de)allocates m_httpBuf. `wantActive` is `visible && wifiUp` as
    // observed by tick(); called unconditionally at the top of every tick()
    // so the 60 s free-delay countdown advances even while the card is
    // off-screen. malloc's on the first tick the card is wanted and frees
    // kHttpBufFreeDelayMs after it stops being wanted (never frees mid-call
    // — every httpsGet() runs to completion synchronously inside tick(),
    // so "wantActive" only ever goes false between requests, not during
    // one).
    void manageHttpBuf(uint32_t nowMs, bool wantActive);

    // C2: cache-hit validation for /logos/{iata}.png — true iff the file
    // opens, its size is > 8 bytes and its first 8 bytes are the PNG
    // signature. Does not touch m_httpBuf.
    bool validateLogoFile(const char *path) const;

    // Round-robin caches, resolved before spending a hexdb request.
    bool routeCacheGet(const char *callsign, char from[5], char to[5]) const;
    void routeCachePut(const char *callsign, const char *from, const char *to);
    bool airportCacheGet(const char *icao, char iata[5]) const;
    void airportCachePut(const char *icao, const char *iata);
    bool operatorCacheGet(const char *hex, char operatorIcao[4], char operatorName[32]) const;
    void operatorCachePut(const char *hex, const char *operatorIcao, const char *operatorName);
    bool isLogoMissing(const char *iata) const;
    void markLogoMissing(const char *iata);
    // I1: bumps the failure count for `icao`, creating the entry on first
    // failure. Returns true once the count reaches 2 (caller should then
    // negative-cache the ICAO via airportCachePut(icao, "")).
    bool airportFailureBump(const char *icao);

    // Private HTTPS GET helper: WiFiClientSecure::setInsecure() (public,
    // read-only data — no certificate to pin), useHTTP10(true) so
    // Content-Length is known up front, body read via getStreamPtr() in
    // chunks capped at `cap` bytes. `accept` sets the Accept request
    // header. Returns the HTTP status code, or a negative value on a
    // transport/library error or an oversized body. One request at a time;
    // net task only. Every call logs status, bytes and ESP.getFreeHeap() at
    // log_i. The response's Content-Type (if any) is left in
    // m_lastContentType for callers that need to gate on it (the logo
    // download does; JSON callers don't).
    //
    // I1: `connectTimeoutMs`/`readTimeoutMs` bound HTTPClient's own
    // connect/read timeouts, and `overallDeadlineMs` bounds the body-read
    // loop below — all three are per-call now rather than fixed constants,
    // so every caller can choose how much of the net task's time budget a
    // given request is allowed to spend (see tick()'s wall-clock cap and
    // the per-call values fetchAircraft()/enrichRoute()/enrichOperator()/
    // tickLogo() pass).
    int httpsGet(const char *url, uint8_t *buf, size_t cap, size_t &len, const char *accept,
                 uint32_t connectTimeoutMs, uint32_t readTimeoutMs, uint32_t overallDeadlineMs);

    NetManager &m_net;

    // loop task writes, net task reads (setVisible()/tick()) — a plain
    // atomic flag, same idiom as NetTask's own m_status.
    std::atomic<bool> m_visible{false};

    Guarded<FlightsModel::FlightsSnapshot> m_snapshot;
    Guarded<LogoStatus>                    m_logoStatus;
    Guarded<WantedIata>                    m_wanted;

    uint32_t m_lastFetchMs = 0; // 0 = never fetched, forces an immediate fetch

    // I1: wall-clock start of the current tick() call, used by
    // tickBudgetExceeded() to bound how many requests a single tick() can
    // chain (see tick()/tickBudgetExceeded() comments).
    uint32_t m_tickStartMs = 0;

    // I2: one-shot stack high-water-mark logs — one after the first
    // successful aircraft fetch, one after the first successful logo
    // download — so hardware headroom on the net task's stack is visible
    // in the log without repeating every tick.
    bool m_aircraftStackLogged = false;
    bool m_logoStackLogged     = false;

    // M2: shared HTTP scratch buffer — one request in flight at a time,
    // reused for adsb.fi JSON, hexdb JSON and logo PNG bodies alike. Lazily
    // malloc'ed/freed by manageHttpBuf() (see tick()) rather than a static
    // member, so it only actually costs heap while the Flights card is
    // visible and WiFi is up (plus a kHttpBufFreeDelayMs grace window after
    // that stops being true, so flipping cards doesn't churn malloc/free).
    // 8 KB (down from the old static 16 KB): adsb.fi JSON for a 3 mi radius
    // and hexdb.io responses are well under that, and pics.avs.io 120x48
    // logo PNGs run ~5-10 KB — an outlier over the cap is treated as
    // missing-for-session rather than grown into (spec review 2026-09-04).
    static constexpr size_t   kHttpBufCap = 8192;
    static constexpr uint32_t kHttpBufFreeDelayMs = 60000;
    uint8_t  *m_httpBuf = nullptr;
    uint32_t  m_httpBufIdleSinceMs = 0; // 0 = not counting down (active, or already freed)
    char      m_lastContentType[40] = {0};

    static constexpr uint8_t kRouteCacheSize          = 24;
    static constexpr uint8_t kAirportCacheSize        = 32;
    static constexpr uint8_t kOperatorCacheSize       = 24;
    static constexpr uint8_t kMissingLogoSize         = 16;
    static constexpr uint8_t kAirportFailureSize      = 16;

    RouteEntry    m_routeCache[kRouteCacheSize];
    uint8_t       m_routeCount = 0;
    uint8_t       m_routeNext  = 0;

    AirportEntry  m_airportCache[kAirportCacheSize];
    uint8_t       m_airportCount = 0;
    uint8_t       m_airportNext  = 0;

    OperatorEntry m_operatorCache[kOperatorCacheSize];
    uint8_t       m_operatorCount = 0;
    uint8_t       m_operatorNext  = 0;

    MissingLogo   m_missingLogos[kMissingLogoSize];
    uint8_t       m_missingCount = 0;
    uint8_t       m_missingNext  = 0;

    AirportFailure m_airportFailures[kAirportFailureSize];
    uint8_t        m_airportFailureCount = 0;
    uint8_t        m_airportFailureNext  = 0;
};
