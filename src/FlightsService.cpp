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
constexpr int       kEnrichBudget     = 3;
constexpr uint32_t kOverallDeadlineMs = 6000;

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

void FlightsService::tick(uint32_t nowMs)
{
    const bool wifiUp  = m_net.wifiUp();
    const bool visible = m_visible.load(std::memory_order_relaxed);

    if (!visible || !wifiUp)
    {
        // Leave the aircraft list alone (spec 4.9's "waiting for WiFi" state
        // still shows the card, and a card that's merely off-screen keeps
        // its last snapshot ready for when it's shown again) — just flag
        // offline so the UI knows not to trust it as live.
        m_snapshot.modify([wifiUp](FlightsModel::FlightsSnapshot &s) { s.offline = !wifiUp; });
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

void FlightsService::fetchAircraft(uint32_t nowMs)
{
    const double nm = (double)FLIGHTS_RADIUS_MI * 0.868976;
    char         url[128];
    snprintf(url, sizeof(url), "https://opendata.adsb.fi/api/v2/lat/%.6f/lon/%.6f/dist/%.1f",
             (double)HOME_LAT, (double)HOME_LON, nm);

    size_t len    = 0;
    int    status = httpsGet(url, m_httpBuf, sizeof(m_httpBuf), len, "application/json");

    FlightsModel::FlightsSnapshot working;
    memset(&working, 0, sizeof(working));

    const bool ok = (status == 200) &&
                    FlightsModel::parseAdsbFi(reinterpret_cast<const char *>(m_httpBuf), len,
                                               (float)HOME_LAT, (float)HOME_LON, working);

    if (!ok)
    {
        log_w("FlightsService: adsb.fi fetch failed (status=%d) — keeping previous list", status);
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

    log_i("FlightsService: adsb.fi fetch ok, %u aircraft, heap=%u",
          (unsigned)working.count, (unsigned)ESP.getFreeHeap());
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
        if (budget <= 0)
        {
            return budget;
        }
        char url[64];
        snprintf(url, sizeof(url), "https://hexdb.io/api/v1/route/icao/%s", a.callsign);
        size_t len    = 0;
        int    status = httpsGet(url, m_httpBuf, sizeof(m_httpBuf), len, "application/json");
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

    if (!haveFrom && budget > 0)
    {
        char url[64];
        snprintf(url, sizeof(url), "https://hexdb.io/api/v1/airport/icao/%s", fromIcao);
        size_t len    = 0;
        int    status = httpsGet(url, m_httpBuf, sizeof(m_httpBuf), len, "application/json");
        budget--;
        if (status == 200 && FlightsModel::parseHexdbAirport(reinterpret_cast<const char *>(m_httpBuf), len, fromIata))
        {
            airportCachePut(fromIcao, fromIata);
            haveFrom = true;
        }
    }

    if (!haveTo && budget > 0)
    {
        char url[64];
        snprintf(url, sizeof(url), "https://hexdb.io/api/v1/airport/icao/%s", toIcao);
        size_t len    = 0;
        int    status = httpsGet(url, m_httpBuf, sizeof(m_httpBuf), len, "application/json");
        budget--;
        if (status == 200 && FlightsModel::parseHexdbAirport(reinterpret_cast<const char *>(m_httpBuf), len, toIata))
        {
            airportCachePut(toIcao, toIata);
            haveTo = true;
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
        if (budget <= 0)
        {
            return budget;
        }
        char url[48];
        snprintf(url, sizeof(url), "https://hexdb.io/api/v1/aircraft/%s", a.hex);
        size_t len    = 0;
        int    status = httpsGet(url, m_httpBuf, sizeof(m_httpBuf), len, "application/json");
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

void FlightsService::tickLogo(uint32_t nowMs)
{
    (void)nowMs;

    const WantedIata wanted = m_wanted.read();
    if (wanted.v[0] == '\0')
    {
        return;
    }

    bool alreadyResident = false;
    m_logo.modify([&](LogoBuf &lb) {
        alreadyResident = lb.ready && strncmp(lb.iata, wanted.v, sizeof(lb.iata)) == 0;
    });
    if (alreadyResident)
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
        File f = LittleFS.open(path, "r");
        if (f)
        {
            const size_t fileLen = f.size();
            if (fileLen > 0 && fileLen <= sizeof(m_httpBuf))
            {
                const size_t got = f.read(m_httpBuf, fileLen);
                f.close();
                if (got == fileLen)
                {
                    m_logo.modify([&](LogoBuf &lb) {
                        memcpy(lb.data, m_httpBuf, fileLen);
                        lb.len = fileLen;
                        safeCopy(wanted.v, lb.iata, sizeof(lb.iata));
                        lb.ready = true;
                    });
                    log_i("FlightsService: logo %s loaded from cache (%u bytes)", wanted.v, (unsigned)fileLen);
                    return;
                }
            }
            else
            {
                f.close();
            }
        }
        log_w("FlightsService: logo %s cache file unreadable, refetching", wanted.v);
    }

    char url[48];
    snprintf(url, sizeof(url), "https://pics.avs.io/120/48/%s.png", wanted.v);
    size_t len    = 0;
    int    status = httpsGet(url, m_httpBuf, sizeof(m_httpBuf), len, "image/png");

    if (status == 404)
    {
        markLogoMissing(wanted.v);
        log_i("FlightsService: logo %s not found (404), remembered for this session", wanted.v);
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
    if (len == 0 || len > sizeof(m_httpBuf))
    {
        log_w("FlightsService: logo %s bad size (%u bytes), discarding", wanted.v, (unsigned)len);
        return;
    }

    File out = LittleFS.open(path, "w");
    if (out)
    {
        out.write(m_httpBuf, len);
        out.close();
    }
    else
    {
        log_w("FlightsService: failed to open %s for write", path);
    }

    m_logo.modify([&](LogoBuf &lb) {
        memcpy(lb.data, m_httpBuf, len);
        lb.len = len;
        safeCopy(wanted.v, lb.iata, sizeof(lb.iata));
        lb.ready = true;
    });
    log_i("FlightsService: logo %s downloaded and cached (%u bytes), heap=%u",
          wanted.v, (unsigned)len, (unsigned)ESP.getFreeHeap());
}

bool FlightsService::logoReady(const char *iata) const
{
    bool ready = false;
    m_logo.modify([&](LogoBuf &lb) {
        ready = lb.ready && strncmp(lb.iata, iata, sizeof(lb.iata)) == 0;
    });
    return ready;
}

bool FlightsService::copyLogo(const char *iata, uint8_t *dst, size_t dstCap, size_t &len) const
{
    bool ok = false;
    m_logo.modify([&](LogoBuf &lb) {
        if (lb.ready && strncmp(lb.iata, iata, sizeof(lb.iata)) == 0 && lb.len <= dstCap)
        {
            memcpy(dst, lb.data, lb.len);
            len = lb.len;
            ok  = true;
        }
    });
    return ok;
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

// --- HTTPS helper -----------------------------------------------------------

int FlightsService::httpsGet(const char *url, uint8_t *buf, size_t cap, size_t &len, const char *accept)
{
    len                    = 0;
    m_lastContentType[0]   = '\0';

    WiFiClientSecure client;
    client.setInsecure(); // public read-only data — no cert to pin (documented trade-off)

    HTTPClient http;
    http.setConnectTimeout(5000);
    http.setTimeout(5000);
    http.useHTTP10(true); // so getSize() reports Content-Length up front

    if (!http.begin(client, url))
    {
        log_w("FlightsService: httpsGet begin() failed for %s", url);
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
    const uint32_t deadline = millis() + kOverallDeadlineMs;
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
