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
// only through setVisible(), setWantedLogo(), snapshot(), logoReady() and
// copyLogo(), all of which are safe to call from any task.
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

    // loop task: true once `iata`'s logo is the one currently resident in
    // the service's logo buffer and ready to copy out. Never triggers
    // network work itself — the fetch happens in tick() for whatever IATA
    // setWantedLogo() last set.
    bool logoReady(const char *iata) const;

    // loop task: copies the resident logo for `iata` into `dst` (capacity
    // `dstCap`) if it is the one currently loaded and it fits. Returns
    // false (dst/len untouched) if not ready, iata doesn't match, or it
    // doesn't fit.
    bool copyLogo(const char *iata, uint8_t *dst, size_t dstCap, size_t &len) const;

private:
    // Not part of FlightsModel: it's local buffering/caching state, not a
    // wire format, so it stays private to this class.
    struct LogoBuf
    {
        char    iata[3] = {0};
        uint8_t data[16384];
        size_t  len   = 0;
        bool    ready = false;
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

    void fetchAircraft(uint32_t nowMs);
    void enrich();
    // Returns the remaining budget. `budget` is the count of hexdb requests
    // still allowed this tick() call.
    int  enrichRoute(FlightsModel::Aircraft &a, int budget);
    int  enrichOperator(FlightsModel::Aircraft &a, int budget);
    void tickLogo(uint32_t nowMs);

    // Round-robin caches, resolved before spending a hexdb request.
    bool routeCacheGet(const char *callsign, char from[5], char to[5]) const;
    void routeCachePut(const char *callsign, const char *from, const char *to);
    bool airportCacheGet(const char *icao, char iata[5]) const;
    void airportCachePut(const char *icao, const char *iata);
    bool operatorCacheGet(const char *hex, char operatorIcao[4], char operatorName[32]) const;
    void operatorCachePut(const char *hex, const char *operatorIcao, const char *operatorName);
    bool isLogoMissing(const char *iata) const;
    void markLogoMissing(const char *iata);

    // Private HTTPS GET helper: WiFiClientSecure::setInsecure() (public,
    // read-only data — no certificate to pin), 5 s connect/read timeouts,
    // useHTTP10(true) so Content-Length is known up front, body read via
    // getStreamPtr() in chunks capped at `cap` bytes with an overall 6 s
    // deadline. `accept` sets the Accept request header. Returns the HTTP
    // status code, or a negative value on a transport/library error or an
    // oversized body. One request at a time; net task only. Every call
    // logs status, bytes and ESP.getFreeHeap() at log_i. The response's
    // Content-Type (if any) is left in m_lastContentType for callers that
    // need to gate on it (the logo download does; JSON callers don't).
    int httpsGet(const char *url, uint8_t *buf, size_t cap, size_t &len, const char *accept);

    NetManager &m_net;

    // loop task writes, net task reads (setVisible()/tick()) — a plain
    // atomic flag, same idiom as NetTask's own m_status.
    std::atomic<bool> m_visible{false};

    Guarded<FlightsModel::FlightsSnapshot> m_snapshot;
    Guarded<LogoBuf>                       m_logo;
    Guarded<WantedIata>                    m_wanted;

    uint32_t m_lastFetchMs = 0; // 0 = never fetched, forces an immediate fetch

    // Shared HTTP scratch buffer: one request in flight at a time, reused
    // for adsb.fi JSON, hexdb JSON and logo PNG bodies alike. This is the
    // largest static buffer FlightsService owns, tied with LogoBuf::data.
    uint8_t m_httpBuf[16384];
    char    m_lastContentType[40] = {0};

    static constexpr uint8_t kRouteCacheSize    = 24;
    static constexpr uint8_t kAirportCacheSize  = 32;
    static constexpr uint8_t kOperatorCacheSize = 24;
    static constexpr uint8_t kMissingLogoSize   = 16;

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
};
