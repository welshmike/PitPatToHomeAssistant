#include "FlightsService.h"

#if HAS_DIAL_UI

#include <Arduino.h>
#include <LittleFS.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <string.h>
#include <stdio.h>

#include "AirlineCodes.h"
#include "NetManager.h"

namespace
{

// Copies `src` into `dst` (capacity dstSize) verbatim, truncating safely.
// Always null-terminates. `src` may be nullptr, in which case dst becomes
// an empty string. Mirrors FlightsModel.cpp's private helper of the same
// name — kept local here rather than shared, since both are small and
// FlightsModel is deliberately Arduino-free.
void safeCopy(const char *src, char *dst, size_t dstSize)
{
    dst[0] = '\0';
    if (!src || dstSize == 0)
    {
        return;
    }
    size_t len = strlen(src);
    if (len > dstSize - 1)
    {
        len = dstSize - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

constexpr uint32_t kFetchIntervalMs   = 20000;
constexpr int       kEnrichBudget     = 1; // one background request per tick: keeps WiFi duty cycle low so BLE keeps its radio time (2026-09-04)

// I1: enrichment (hexdb) and logo (pics.avs.io) requests are background
// work riding along on the net task, which also has to keep servicing
// PubSubClient (MQTT keepalive is 15 s, and NetTask::run() only calls
// m_net.tick() — which drives PubSubClient::loop() — between tick() calls,
// never during one). They get short timeouts so one slow/stalled request
// can't eat the whole budget. The adsb.fi aircraft fetch is the one
// request tick() always wants to complete (it's already rate-limited to
// once per kFetchIntervalMs), so it keeps the older, more generous values.
constexpr uint32_t kAircraftConnectMs  = 5000;
constexpr uint32_t kAircraftReadMs     = 5000;
constexpr uint32_t kAircraftDeadlineMs = 6000;
constexpr uint32_t kEnrichConnectMs    = 2000;
constexpr uint32_t kEnrichReadMs       = 2000;
constexpr uint32_t kEnrichDeadlineMs   = 3000;

// I1: once a tick() call has been running longer than this, no further
// hexdb/logo requests are issued this tick — whatever enrichment/logo work
// didn't get done carries over to the next tick (nothing here is lost,
// just deferred). See FlightsService::tickBudgetExceeded().
constexpr uint32_t kTickWallCapMs = 4000;

// C2: PNG signature, checked before a downloaded logo is written to
// LittleFS and again on the cache-hit path (a file that fails this check —
// truncated, corrupted, or not a PNG at all — is removed and refetched).
constexpr uint8_t kPngSignature[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

bool isPngSignature(const uint8_t *data, size_t len)
{
    return len >= sizeof(kPngSignature) && memcmp(data, kPngSignature, sizeof(kPngSignature)) == 0;
}

} // namespace

void FlightsService::begin()
{
    if (!LittleFS.begin(true /*formatOnFail*/, "/lfs", 10, "spiffs"))
    {
        log_e("FlightsService: LittleFS mount failed (even after format) — logo cache disabled");
        return;
    }
    log_i("FlightsService: LittleFS mounted, %u/%u bytes used",
          (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());

    if (!LittleFS.exists("/logos") && !LittleFS.mkdir("/logos"))
    {
        log_w("FlightsService: failed to create /logos directory");
    }
}

void FlightsService::setVisible(bool visible)
{
    m_visible.store(visible, std::memory_order_relaxed);
}

void FlightsService::setWantedLogo(const char *iata)
{
    WantedIata w;
    safeCopy(iata, w.v, sizeof(w.v));
    m_wanted.write(w);
}

void FlightsService::invalidateLogo(const char *iata)
{
    m_logoStatus.modify([&](LogoStatus &ls) {
        if (strncmp(ls.iata, iata, sizeof(ls.iata)) == 0) { ls.ready = false; }
    });
    char path[24];
    snprintf(path, sizeof(path), "/logos/%.2s.png", iata);
    if (LittleFS.exists(path))
    {
        LittleFS.remove(path); // LittleFS is internally locked; safe from the loop task
        log_w("FlightsService: logo %.2s removed after a decode failure, will re-download once", iata);
    }
}

void FlightsService::tick(uint32_t nowMs)
{
    // I1: start of this tick() call's wall-clock budget — see
    // tickBudgetExceeded() and kTickWallCapMs.
    m_tickStartMs = nowMs;

    const bool wifiUp  = m_net.wifiUp();
    const bool visible = m_visible.load(std::memory_order_relaxed);

    // M2: advance the HTTP buffer's alloc/free lifecycle on every tick (even
    // while not visible) so the free-delay countdown actually elapses.
    manageHttpBuf(nowMs, visible && wifiUp);

    if (!visible || !wifiUp)
    {
        // Leave the aircraft list alone (spec 4.9's "waiting for WiFi" state
        // still shows the card, and a card that's merely off-screen keeps
        // its last snapshot ready for when it's shown again) — just flag
        // offline so the UI knows not to trust it as live.
        m_snapshot.modify([wifiUp](FlightsModel::FlightsSnapshot &s) { s.offline = !wifiUp; });
        return;
    }

    if (m_httpBuf == nullptr)
    {
        // malloc() failed this tick under heap pressure — try again next
        // tick rather than dereferencing a null buffer.
        log_w("FlightsService: HTTP buffer unavailable this tick, heap=%u", (unsigned)ESP.getFreeHeap());
        return;
    }

    m_snapshot.modify([](FlightsModel::FlightsSnapshot &s) { s.offline = false; });

    if (m_lastFetchMs == 0 || (nowMs - m_lastFetchMs) >= kFetchIntervalMs)
    {
        fetchAircraft(nowMs);
        m_lastFetchMs = nowMs;
    }

    enrich();
    tickLogo(nowMs);
}

void FlightsService::manageHttpBuf(uint32_t nowMs, bool wantActive)
{
    if (wantActive)
    {
        m_httpBufIdleSinceMs = 0;
        if (m_httpBuf == nullptr)
        {
            m_httpBuf = static_cast<uint8_t *>(malloc(kHttpBufCap));
            if (m_httpBuf == nullptr)
            {
                log_e("FlightsService: failed to allocate %u-byte HTTP buffer, heap=%u",
                      (unsigned)kHttpBufCap, (unsigned)ESP.getFreeHeap());
            }
            else
            {
                log_i("FlightsService: HTTP buffer allocated (%u bytes), heap=%u",
                      (unsigned)kHttpBufCap, (unsigned)ESP.getFreeHeap());
            }
        }
        return;
    }

    if (m_httpBuf == nullptr)
    {
        return;
    }
    if (m_httpBufIdleSinceMs == 0)
    {
        // Just stopped being wanted (setVisible(false) observed, or WiFi
        // dropped) — every httpsGet() call runs to completion synchronously
        // inside tick(), so there is never a request in progress here; start
        // the free-delay countdown.
        m_httpBufIdleSinceMs = nowMs;
        return;
    }
    if ((nowMs - m_httpBufIdleSinceMs) >= kHttpBufFreeDelayMs)
    {
        free(m_httpBuf);
        m_httpBuf = nullptr;
        m_httpBufIdleSinceMs = 0;
        log_i("FlightsService: HTTP buffer freed after %u ms idle, heap=%u",
              (unsigned)kHttpBufFreeDelayMs, (unsigned)ESP.getFreeHeap());
    }
}

void FlightsService::fetchAircraft(uint32_t nowMs)
{
    const double nm = (double)FLIGHTS_RADIUS_MI * 0.868976;
    char         url[128];
    snprintf(url, sizeof(url), "https://opendata.adsb.fi/api/v2/lat/%.6f/lon/%.6f/dist/%.1f",
             (double)HOME_LAT, (double)HOME_LON, nm);

    size_t len    = 0;
    int    status = httpsGet(url, m_httpBuf, kHttpBufCap, len, "application/json",
                              kAircraftConnectMs, kAircraftReadMs, kAircraftDeadlineMs);

    FlightsModel::FlightsSnapshot working;
    memset(&working, 0, sizeof(working));

    const bool ok = (status == 200) &&
                    FlightsModel::parseAdsbFi(reinterpret_cast<const char *>(m_httpBuf), len,
                                               (float)HOME_LAT, (float)HOME_LON, working);

    if (!ok)
    {
        log_w("FlightsService: adsb.fi fetch failed (status=%d) — keeping previous list, heap=%u minFree=%u",
              status, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
        m_snapshot.modify([](FlightsModel::FlightsSnapshot &prev) {
            prev.stale   = true;
            prev.offline = false;
        });
        return;
    }

    m_snapshot.modify([&](FlightsModel::FlightsSnapshot &prev) {
        // Carry over enrichment already known for aircraft still present
        // (matched by hex — callsign/position can be re-sorted, hex is the
        // stable ICAO 24-bit address), so a 20 s refetch doesn't discard
        // route/operator/logo work already paid for.
        for (uint8_t i = 0; i < working.count; i++)
        {
            FlightsModel::Aircraft &next = working.ac[i];
            for (uint8_t j = 0; j < prev.count; j++)
            {
                if (strcmp(next.hex, prev.ac[j].hex) == 0)
                {
                    const FlightsModel::Aircraft &old = prev.ac[j];
                    memcpy(next.fromIata, old.fromIata, sizeof(next.fromIata));
                    memcpy(next.toIata, old.toIata, sizeof(next.toIata));
                    memcpy(next.airlineIata, old.airlineIata, sizeof(next.airlineIata));
                    memcpy(next.operatorName, old.operatorName, sizeof(next.operatorName));
                    next.routeKnown    = old.routeKnown;
                    next.operatorKnown = old.operatorKnown;
                    break;
                }
            }

            // Free (no network call): resolve the airline IATA from the
            // callsign prefix directly when we don't already have one.
            if (next.airlineIata[0] == '\0')
            {
                const char *iata = AirlineCodes::iataFromIcao(next.callsign);
                if (iata != nullptr)
                {
                    safeCopy(iata, next.airlineIata, sizeof(next.airlineIata));
                }
            }
        }

        working.fetchedMs = nowMs;
        working.stale      = false;
        working.offline     = false;
        prev = working;
    });

    log_i("FlightsService: adsb.fi fetch ok, %u aircraft, heap=%u minFree=%u",
          (unsigned)working.count, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());

    // I2: one-shot, first successful fetch only — confirms actual net task
    // stack headroom on hardware.
    if (!m_aircraftStackLogged)
    {
        m_aircraftStackLogged = true;
        log_i("FlightsService: net task stack high-water mark %u bytes free",
              (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    }
}

void FlightsService::enrich()
{
    FlightsModel::FlightsSnapshot working = m_snapshot.read();
    if (working.count == 0)
    {
        return;
    }

    int budget = kEnrichBudget;
    for (uint8_t i = 0; i < working.count && budget > 0; i++)
    {
        FlightsModel::Aircraft &a = working.ac[i];
        if (!a.routeKnown)
        {
            budget = enrichRoute(a, budget);
        }
        if (a.routeKnown && !a.operatorKnown && budget > 0)
        {
            budget = enrichOperator(a, budget);
        }
    }

    // Merge back by hex rather than overwrite outright: fetchAircraft() runs
    // strictly before enrich() within the same tick() call (single-threaded
    // net task), so `working` and the live snapshot describe the same
    // aircraft list here — a plain write() is safe and cheaper than a
    // merge, but writing it explicitly this way keeps the intent clear and
    // costs nothing extra at this size (<=6 aircraft).
    m_snapshot.write(working);
}

int FlightsService::enrichRoute(FlightsModel::Aircraft &a, int budget)
{
    char fromIcao[5] = {0};
    char toIcao[5]   = {0};
    bool haveRoute   = routeCacheGet(a.callsign, fromIcao, toIcao);

    if (!haveRoute)
    {
        // I1: budget<=0 is the per-tick request-count cap (kEnrichBudget);
        // tickBudgetExceeded() is the wall-clock cap — either stops further
        // requests, deferring the rest to the next tick().
        if (budget <= 0 || tickBudgetExceeded())
        {
            return budget;
        }
        char url[64];
        snprintf(url, sizeof(url), "https://hexdb.io/api/v1/route/icao/%s", a.callsign);
        size_t len    = 0;
        int    status = httpsGet(url, m_httpBuf, kHttpBufCap, len, "application/json",
                                  kEnrichConnectMs, kEnrichReadMs, kEnrichDeadlineMs);
        budget--;

        if (status == 200 &&
            FlightsModel::parseHexdbRoute(reinterpret_cast<const char *>(m_httpBuf), len, fromIcao, toIcao))
        {
            routeCachePut(a.callsign, fromIcao, toIcao);
            haveRoute = true;
        }
        else
        {
            // No route on record (404/"unknown"/malformed): remember that
            // as "known empty" so this callsign isn't retried every tick.
            a.routeKnown   = true;
            a.fromIata[0]  = '\0';
            a.toIata[0]    = '\0';
            return budget;
        }
    }

    char fromIata[5] = {0};
    char toIata[5]   = {0};
    bool haveFrom    = airportCacheGet(fromIcao, fromIata);
    bool haveTo      = airportCacheGet(toIcao, toIata);

    if (!haveFrom && budget > 0 && !tickBudgetExceeded())
    {
        char url[64];
        snprintf(url, sizeof(url), "https://hexdb.io/api/v1/airport/icao/%s", fromIcao);
        size_t len    = 0;
        int    status = httpsGet(url, m_httpBuf, kHttpBufCap, len, "application/json",
                                  kEnrichConnectMs, kEnrichReadMs, kEnrichDeadlineMs);
        budget--;
        if (status == 200 && FlightsModel::parseHexdbAirport(reinterpret_cast<const char *>(m_httpBuf), len, fromIata))
        {
            airportCachePut(fromIcao, fromIata);
            haveFrom = true;
        }
        else if (airportFailureBump(fromIcao))
        {
            // I1: 2 failed lookups for this ICAO — negative-cache an empty
            // IATA so it stops consuming the enrichment budget every tick.
            airportCachePut(fromIcao, "");
            haveFrom = true;
            log_i("FlightsService: airport %s IATA unresolved after 2 tries, negative-cached", fromIcao);
        }
    }

    if (!haveTo && budget > 0 && !tickBudgetExceeded())
    {
        char url[64];
        snprintf(url, sizeof(url), "https://hexdb.io/api/v1/airport/icao/%s", toIcao);
        size_t len    = 0;
        int    status = httpsGet(url, m_httpBuf, kHttpBufCap, len, "application/json",
                                  kEnrichConnectMs, kEnrichReadMs, kEnrichDeadlineMs);
        budget--;
        if (status == 200 && FlightsModel::parseHexdbAirport(reinterpret_cast<const char *>(m_httpBuf), len, toIata))
        {
            airportCachePut(toIcao, toIata);
            haveTo = true;
        }
        else if (airportFailureBump(toIcao))
        {
            airportCachePut(toIcao, "");
            haveTo = true;
            log_i("FlightsService: airport %s IATA unresolved after 2 tries, negative-cached", toIcao);
        }
    }

    if (haveFrom && haveTo)
    {
        safeCopy(fromIata, a.fromIata, sizeof(a.fromIata));
        safeCopy(toIata, a.toIata, sizeof(a.toIata));
        a.routeKnown = true;
    }
    // else: route known, one or both IATA lookups still pending — retried
    // next tick (not marked routeKnown yet), bounded by the same budget.

    return budget;
}

int FlightsService::enrichOperator(FlightsModel::Aircraft &a, int budget)
{
    char operatorIcao[4] = {0};
    char operatorName[32] = {0};
    bool have = operatorCacheGet(a.hex, operatorIcao, operatorName);

    if (!have)
    {
        if (budget <= 0 || tickBudgetExceeded())
        {
            return budget;
        }
        char url[48];
        snprintf(url, sizeof(url), "https://hexdb.io/api/v1/aircraft/%s", a.hex);
        size_t len    = 0;
        int    status = httpsGet(url, m_httpBuf, kHttpBufCap, len, "application/json",
                                  kEnrichConnectMs, kEnrichReadMs, kEnrichDeadlineMs);
        budget--;

        if (status == 200 && FlightsModel::parseHexdbAircraft(reinterpret_cast<const char *>(m_httpBuf), len,
                                                                operatorIcao, operatorName))
        {
            operatorCachePut(a.hex, operatorIcao, operatorName);
            have = true;
        }
        else
        {
            // No operator on record: mark known so this hex isn't retried
            // every tick.
            a.operatorKnown = true;
            return budget;
        }
    }

    safeCopy(operatorName, a.operatorName, sizeof(a.operatorName));
    if (a.airlineIata[0] == '\0' && operatorIcao[0] != '\0')
    {
        const char *iata = AirlineCodes::iataFromIcao(operatorIcao);
        if (iata != nullptr)
        {
            safeCopy(iata, a.airlineIata, sizeof(a.airlineIata));
        }
    }
    a.operatorKnown = true;
    return budget;
}

// M1: the logo itself never comes back to this class any more — it's
// downloaded (or already cached) straight to /logos/{iata}.png on LittleFS,
// and DialUi decodes it from there once logoReady() says it's good. This
// function's only job is to make sure that file exists and is valid, and to
// record that in m_logoStatus (the small Guarded flag logoReady() reads).
void FlightsService::tickLogo(uint32_t nowMs)
{
    (void)nowMs;

    const WantedIata wanted = m_wanted.read();
    if (wanted.v[0] == '\0')
    {
        return;
    }

    bool alreadyReady = false;
    m_logoStatus.modify([&](LogoStatus &ls) {
        alreadyReady = ls.ready && strncmp(ls.iata, wanted.v, sizeof(ls.iata)) == 0;
    });
    if (alreadyReady)
    {
        return;
    }

    if (isLogoMissing(wanted.v))
    {
        return;
    }

    char path[24];
    snprintf(path, sizeof(path), "/logos/%s.png", wanted.v);

    if (LittleFS.exists(path))
    {
        if (validateLogoFile(path))
        {
            m_logoStatus.modify([&](LogoStatus &ls) {
                safeCopy(wanted.v, ls.iata, sizeof(ls.iata));
                ls.ready = true;
            });
            log_i("FlightsService: logo %s cache valid", wanted.v);
            return;
        }
        // C2: cache-hit file failed validation (too small / bad signature)
        // — remove it and fall through to a fresh download.
        log_w("FlightsService: logo %s cache file invalid, removing and refetching", wanted.v);
        LittleFS.remove(path);
    }

    // I1: the wall-clock cap applies here too — a logo download queued
    // behind a full budget of hexdb requests this tick() just defers to the
    // next one, same as any other request past the cap.
    if (tickBudgetExceeded())
    {
        return;
    }

    char url[48];
    snprintf(url, sizeof(url), "https://pics.avs.io/120/48/%s.png", wanted.v);
    size_t len    = 0;
    int    status = httpsGet(url, m_httpBuf, kHttpBufCap, len, "image/png",
                              kEnrichConnectMs, kEnrichReadMs, kEnrichDeadlineMs);

    if (status == 404)
    {
        markLogoMissing(wanted.v);
        log_i("FlightsService: logo %s not found (404), remembered for this session", wanted.v);
        return;
    }
    if (status == -3)
    {
        return; // heap guard skipped this request — transient, retry next tick
    }
    if (status == -2)
    {
        // M2: body exceeded kHttpBufCap. pics.avs.io 120x48 PNGs normally
        // run ~5-10 KB; an outlier this large is treated as missing for the
        // rest of this session rather than retried every tick (httpsGet()
        // already log_w'd the actual length).
        markLogoMissing(wanted.v);
        log_w("FlightsService: logo %s exceeds %u-byte cap, treating as missing for this session",
              wanted.v, (unsigned)kHttpBufCap);
        return;
    }
    if (status != 200)
    {
        log_w("FlightsService: logo %s download failed (status=%d)", wanted.v, status);
        return; // transient — retried next tick, not marked missing
    }
    if (strncmp(m_lastContentType, "image/png", 9) != 0)
    {
        log_w("FlightsService: logo %s unexpected content-type '%s', discarding", wanted.v, m_lastContentType);
        return;
    }
    // C2: verify the PNG signature before it ever touches LittleFS.
    if (len <= sizeof(kPngSignature) || !isPngSignature(m_httpBuf, len))
    {
        log_w("FlightsService: logo %s failed PNG signature check (%u bytes), discarding", wanted.v, (unsigned)len);
        return;
    }

    File out = LittleFS.open(path, "w");
    if (!out)
    {
        log_w("FlightsService: failed to open %s for write", path);
        return;
    }
    const size_t written = out.write(m_httpBuf, len);
    out.close();
    if (written != len)
    {
        // C2: partial/failed write — remove it rather than leave a
        // truncated file behind; retried next visible session.
        log_w("FlightsService: logo %s write incomplete (%u/%u bytes), removing",
              wanted.v, (unsigned)written, (unsigned)len);
        LittleFS.remove(path);
        return;
    }

    m_logoStatus.modify([&](LogoStatus &ls) {
        safeCopy(wanted.v, ls.iata, sizeof(ls.iata));
        ls.ready = true;
    });
    log_i("FlightsService: logo %s downloaded and cached (%u bytes), heap=%u",
          wanted.v, (unsigned)len, (unsigned)ESP.getFreeHeap());

    // I2: one-shot, first successful logo download only.
    if (!m_logoStackLogged)
    {
        m_logoStackLogged = true;
        log_i("FlightsService: net task stack high-water mark %u bytes free",
              (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    }
}

bool FlightsService::tickBudgetExceeded() const
{
    return (millis() - m_tickStartMs) >= kTickWallCapMs;
}

bool FlightsService::logoReady(const char *iata) const
{
    bool ready = false;
    m_logoStatus.modify([&](LogoStatus &ls) {
        ready = ls.ready && strncmp(ls.iata, iata, sizeof(ls.iata)) == 0;
    });
    return ready;
}

bool FlightsService::validateLogoFile(const char *path) const
{
    File f = LittleFS.open(path, "r");
    if (!f)
    {
        return false;
    }
    const size_t fileLen = f.size();
    if (fileLen <= sizeof(kPngSignature)) // C2: "size > 8"
    {
        f.close();
        return false;
    }
    uint8_t sig[sizeof(kPngSignature)];
    const size_t got = f.read(sig, sizeof(sig));
    f.close();
    return got == sizeof(sig) && isPngSignature(sig, got);
}

// --- caches: simple linear scan + round-robin replacement -----------------

bool FlightsService::routeCacheGet(const char *callsign, char from[5], char to[5]) const
{
    for (uint8_t i = 0; i < m_routeCount; i++)
    {
        if (strcmp(m_routeCache[i].callsign, callsign) == 0)
        {
            safeCopy(m_routeCache[i].from, from, 5);
            safeCopy(m_routeCache[i].to, to, 5);
            return true;
        }
    }
    return false;
}

void FlightsService::routeCachePut(const char *callsign, const char *from, const char *to)
{
    RouteEntry &e = m_routeCache[m_routeNext];
    safeCopy(callsign, e.callsign, sizeof(e.callsign));
    safeCopy(from, e.from, sizeof(e.from));
    safeCopy(to, e.to, sizeof(e.to));
    m_routeNext = (uint8_t)((m_routeNext + 1) % kRouteCacheSize);
    if (m_routeCount < kRouteCacheSize)
    {
        m_routeCount++;
    }
}

bool FlightsService::airportCacheGet(const char *icao, char iata[5]) const
{
    if (icao[0] == '\0')
    {
        return false;
    }
    for (uint8_t i = 0; i < m_airportCount; i++)
    {
        if (strcmp(m_airportCache[i].icao, icao) == 0)
        {
            safeCopy(m_airportCache[i].iata, iata, 5);
            return true;
        }
    }
    return false;
}

void FlightsService::airportCachePut(const char *icao, const char *iata)
{
    AirportEntry &e = m_airportCache[m_airportNext];
    safeCopy(icao, e.icao, sizeof(e.icao));
    safeCopy(iata, e.iata, sizeof(e.iata));
    m_airportNext = (uint8_t)((m_airportNext + 1) % kAirportCacheSize);
    if (m_airportCount < kAirportCacheSize)
    {
        m_airportCount++;
    }
}

bool FlightsService::operatorCacheGet(const char *hex, char operatorIcao[4], char operatorName[32]) const
{
    for (uint8_t i = 0; i < m_operatorCount; i++)
    {
        if (strcmp(m_operatorCache[i].hex, hex) == 0)
        {
            safeCopy(m_operatorCache[i].operatorIcao, operatorIcao, 4);
            safeCopy(m_operatorCache[i].operatorName, operatorName, 32);
            return true;
        }
    }
    return false;
}

void FlightsService::operatorCachePut(const char *hex, const char *operatorIcao, const char *operatorName)
{
    OperatorEntry &e = m_operatorCache[m_operatorNext];
    safeCopy(hex, e.hex, sizeof(e.hex));
    safeCopy(operatorIcao, e.operatorIcao, sizeof(e.operatorIcao));
    safeCopy(operatorName, e.operatorName, sizeof(e.operatorName));
    m_operatorNext = (uint8_t)((m_operatorNext + 1) % kOperatorCacheSize);
    if (m_operatorCount < kOperatorCacheSize)
    {
        m_operatorCount++;
    }
}

bool FlightsService::isLogoMissing(const char *iata) const
{
    for (uint8_t i = 0; i < m_missingCount; i++)
    {
        if (strncmp(m_missingLogos[i].iata, iata, sizeof(m_missingLogos[i].iata)) == 0)
        {
            return true;
        }
    }
    return false;
}

void FlightsService::markLogoMissing(const char *iata)
{
    safeCopy(iata, m_missingLogos[m_missingNext].iata, sizeof(m_missingLogos[m_missingNext].iata));
    m_missingNext = (uint8_t)((m_missingNext + 1) % kMissingLogoSize);
    if (m_missingCount < kMissingLogoSize)
    {
        m_missingCount++;
    }
}

// I1: negative-cache the from/to airport IATA lookup inside enrichRoute()
// after 2 failures, the same way a missing route/operator is negative-
// cached after its one attempt — otherwise a persistently-unresolvable
// ICAO (no IATA on record at hexdb.io) would spend enrichment budget on
// every single tick forever.
bool FlightsService::airportFailureBump(const char *icao)
{
    for (uint8_t i = 0; i < m_airportFailureCount; i++)
    {
        if (strcmp(m_airportFailures[i].icao, icao) == 0)
        {
            m_airportFailures[i].count++;
            return m_airportFailures[i].count >= 2;
        }
    }
    AirportFailure &e = m_airportFailures[m_airportFailureNext];
    safeCopy(icao, e.icao, sizeof(e.icao));
    e.count = 1;
    m_airportFailureNext = (uint8_t)((m_airportFailureNext + 1) % kAirportFailureSize);
    if (m_airportFailureCount < kAirportFailureSize)
    {
        m_airportFailureCount++;
    }
    return false;
}

// --- HTTPS helper -----------------------------------------------------------

int FlightsService::httpsGet(const char *url, uint8_t *buf, size_t cap, size_t &len, const char *accept,
                              uint32_t connectTimeoutMs, uint32_t readTimeoutMs, uint32_t overallDeadlineMs)
{
    // Radio sharing: WiFi and BLE share the S3's antenna. Back-to-back TLS
    // requests starved the belt heartbeat (kicks every ~11 s), so enforce a
    // minimum gap between any two HTTPS requests.
    {
        static uint32_t s_lastRequestMs = 0;
        const uint32_t now = millis();
        if (s_lastRequestMs != 0 && (uint32_t)(now - s_lastRequestMs) < 1500)
        {
            return -3; // transient: try again next tick
        }
        s_lastRequestMs = now;
    }
    // Heap guard: a TLS session needs ~45-55 KB transient, mostly as blocks of
    // 16 KB or less. Skip (rather than starve the WiFi driver's TX buffers,
    // which stalls every socket write; seen 2026-09-04) when total free heap
    // is under 60 KB or the largest free block is under 20 KB.
    {
        const size_t freeHeap = ESP.getFreeHeap();
        const size_t largest  = ESP.getMaxAllocHeap();
        if (freeHeap < 60 * 1024 || largest < 20 * 1024)
        {
            log_w("FlightsService: skipping fetch, heap free=%u largest=%u", (unsigned)freeHeap, (unsigned)largest);
            return -3;
        }
    }
    len                    = 0;
    m_lastContentType[0]   = '\0';

    WiFiClientSecure client;
    // I3 (attempted, not possible on this toolchain): the plan was
    // client.setBufferSizes(4096, 1024) before setInsecure() — responses
    // here are capped at kHttpBufCap (8 KB) and requests are tiny (a GET
    // line plus a couple of headers), so mbedTLS's default 16 KB TX/RX
    // buffers are more than this class ever needs. Checked directly against
    // the framework-arduinoespressif32 headers this project actually builds
    // against (package version 3.20017.241212, i.e. arduino-esp32 2.0.17,
    // pinned via espressif32@6.13.0 in platformio.ini): WiFiClientSecure has
    // no setBufferSizes() method in this release (see
    // libraries/WiFiClientSecure/src/WiFiClientSecure.h) — that API was
    // added in a later arduino-esp32 release. mbedTLS's SSL in/out content
    // length is fixed at build time in this SDK's prebuilt libmbedtls.a,
    // with no per-connection Arduino-level override available, so this
    // item is not achievable without bumping the pinned framework version
    // (out of scope here — see the final fix report).
    client.setInsecure(); // public read-only data — no cert to pin (documented trade-off)

    HTTPClient http;
    http.setConnectTimeout(connectTimeoutMs);
    http.setTimeout(readTimeoutMs);
    // I3: forces HTTP/1.0 semantics specifically so the server can't reply
    // with a chunked Transfer-Encoding — a chunked body carries no
    // Content-Length, which would defeat the pre-flight size-cap check
    // below. All three services this class talks to (adsb.fi, hexdb.io,
    // pics.avs.io) have been checked directly and none returns chunked
    // responses on the endpoints used here (verified 2026-09-04). If that
    // ever stopped being true, getSize() below returns -1, the size-cap
    // check is skipped (nothing to compare against), and the read loop
    // just stops at `cap` bytes — silently truncating rather than
    // rejecting — or when the connection closes, whichever comes first.
    http.useHTTP10(true);

    if (!http.begin(client, url))
    {
        log_w("FlightsService: httpsGet begin() failed for %s", url);
        // I2: begin() failure still leaves resources to release, same as
        // every other return path below.
        http.end();
        client.stop();
        return -1;
    }

    const char *headerKeys[] = {"Content-Type"};
    http.collectHeaders(headerKeys, 1);
    if (accept != nullptr)
    {
        http.addHeader("Accept", accept);
    }

    const int status = http.GET();
    if (status <= 0)
    {
        log_i("FlightsService: GET %s -> error %d, heap=%u", url, status, (unsigned)ESP.getFreeHeap());
        http.end();
        return status;
    }

    safeCopy(http.header("Content-Type").c_str(), m_lastContentType, sizeof(m_lastContentType));

    if (status != HTTP_CODE_OK)
    {
        log_i("FlightsService: GET %s -> status %d, heap=%u", url, status, (unsigned)ESP.getFreeHeap());
        http.end();
        return status;
    }

    const int contentLength = http.getSize();
    if (contentLength > 0 && (size_t)contentLength > cap)
    {
        log_w("FlightsService: GET %s -> body too large (%d > %u)", url, contentLength, (unsigned)cap);
        http.end();
        return -2;
    }

    WiFiClient    *stream   = http.getStreamPtr();
    const uint32_t deadline = millis() + overallDeadlineMs;
    size_t         total    = 0;
    while (http.connected() && total < cap)
    {
        if ((int32_t)(millis() - deadline) >= 0)
        {
            log_w("FlightsService: GET %s -> read deadline exceeded at %u bytes", url, (unsigned)total);
            break;
        }
        const size_t avail = stream->available();
        if (avail == 0)
        {
            if (contentLength > 0 && total >= (size_t)contentLength)
            {
                break;
            }
            delay(1);
            continue;
        }
        size_t toRead = avail;
        if (toRead > cap - total)
        {
            toRead = cap - total;
        }
        const int got = stream->readBytes(buf + total, toRead);
        if (got <= 0)
        {
            break;
        }
        total += (size_t)got;
        if (contentLength > 0 && total >= (size_t)contentLength)
        {
            break;
        }
    }
    http.end();

    len = total;
    log_i("FlightsService: GET %s -> status %d, %u bytes, heap=%u", url, status, (unsigned)len,
          (unsigned)ESP.getFreeHeap());
    return status;
}

#endif // HAS_DIAL_UI
