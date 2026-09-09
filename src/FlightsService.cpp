#include "FlightsService.h"

#if HAS_DIAL_UI

#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <string.h>
#include <strings.h> // strncasecmp
#include <stdio.h>
#include <stdlib.h>

#include "NetManager.h"

// -DUSE_ESP_IDF_LOG (spec 4.14) makes log_x() expand to
// ESP_LOG_LEVEL_LOCAL(..., TAG, ...); esp32-hal-log.h has no default TAG.
static const char *TAG = "Flights";

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

// Connect deadline, and the deadline for the whole response (status line,
// headers and body). The relay is on the LAN and a logo is 11.5 KB, so 3 s is
// generous — the point is that the net task also has to keep servicing
// PubSubClient (MQTT keepalive is 15 s, and NetTask::run() only calls
// m_net.tick() between tick() calls, never during one), so a stalled logo
// request must never be able to hold tick() open.
constexpr uint32_t kLogoTimeoutMs = 3000;


// Splits FLIGHTS_LOGO_BASE_URL ("http://host[:port][/path]") into its host,
// port (default 80) and path prefix (empty when the URL has none). Returns
// false for anything that isn't a plain-http URL with a non-empty host and
// a non-zero port — a typo in config.h then shows up as one log_e rather
// than a bad socket connect every tick.
bool splitBaseUrl(const char *url, char *host, size_t hostCap, uint16_t &port, char *path, size_t pathCap)
{
    static const char kScheme[] = "http://";
    if (url == nullptr || strncmp(url, kScheme, sizeof(kScheme) - 1) != 0)
    {
        return false;
    }
    const char *authority = url + sizeof(kScheme) - 1;
    const char *slash     = strchr(authority, '/');
    const char *colon     = strchr(authority, ':');
    if (colon != nullptr && slash != nullptr && colon > slash)
    {
        colon = nullptr; // a ':' inside the path, not a port separator
    }

    const char  *hostEnd = (colon != nullptr) ? colon : (slash != nullptr ? slash : authority + strlen(authority));
    const size_t hostLen = static_cast<size_t>(hostEnd - authority);
    if (hostLen == 0 || hostLen >= hostCap)
    {
        return false;
    }
    memcpy(host, authority, hostLen);
    host[hostLen] = '\0';

    port = 80;
    if (colon != nullptr)
    {
        const unsigned long p = strtoul(colon + 1, nullptr, 10);
        if (p == 0 || p > 65535)
        {
            return false;
        }
        port = static_cast<uint16_t>(p);
    }

    safeCopy(slash != nullptr ? slash : "", path, pathCap);
    return true;
}

// Reads one CRLF- (or LF-) terminated line into `buf`, dropping the line
// terminator. Returns false if the deadline passes or the peer closes
// before a full line arrives. Lines longer than the buffer are truncated,
// with the rest of the line still consumed, so an unexpectedly long header
// can't desynchronise the parse.
bool readLine(WiFiClient &client, char *buf, size_t cap, uint32_t deadlineMs)
{
    size_t n = 0;
    for (;;)
    {
        if ((int32_t)(millis() - deadlineMs) >= 0)
        {
            return false;
        }
        if (client.available() == 0)
        {
            if (!client.connected())
            {
                return false;
            }
            delay(1);
            continue;
        }
        const int c = client.read();
        if (c < 0)
        {
            continue;
        }
        if (c == '\n')
        {
            if (n > 0 && buf[n - 1] == '\r')
            {
                n--;
            }
            buf[n] = '\0';
            return true;
        }
        if (n + 1 < cap)
        {
            buf[n++] = static_cast<char>(c);
        }
    }
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
    // Sweep leftovers: PNGs cached by builds before the raw-565 format
    // (2026-09-09) and any .tmp a reset interrupted mid-download. Collected
    // 16 names at a time (removing while iterating a LittleFS directory is
    // not safe), rescanning until a pass finds nothing — the first boot on
    // this format found ~40 PNGs; a bounded pass count keeps a pathological
    // directory from stalling boot.
    for (uint8_t pass = 0; pass < 8; pass++)
    {
        File dir = LittleFS.open("/logos");
        if (!dir || !dir.isDirectory())
        {
            break;
        }
        char    stale[16][32];
        uint8_t n = 0;
        for (;;)
        {
            File f = dir.openNextFile();
            if (!f)
            {
                break;
            }
            // name() is the bare file name on this core's LittleFS wrapper;
            // strip any directory part defensively so the concatenation below
            // is right either way.
            const char *name  = f.name();
            const char *slash = strrchr(name, '/');
            if (slash != nullptr)
            {
                name = slash + 1;
            }
            const size_t nl = strlen(name);
            if (nl >= 4 && (strcmp(name + nl - 4, ".png") == 0 || strcmp(name + nl - 4, ".tmp") == 0))
            {
                snprintf(stale[n++], sizeof(stale[0]), "/logos/%s", name);
            }
            f.close();
            if (n >= 16)
            {
                break;
            }
        }
        dir.close();
        for (uint8_t i = 0; i < n; i++)
        {
            LittleFS.remove(stale[i]);
            log_i("FlightsService: removed stale logo cache file %s", stale[i]);
        }
        if (n < 16)
        {
            break;
        }
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
    snprintf(path, sizeof(path), "/logos/%.2s.565", iata);
    if (LittleFS.exists(path))
    {
        LittleFS.remove(path); // LittleFS is internally locked; safe from the loop task
        log_w("FlightsService: logo %.2s removed after a decode failure, will re-download once", iata);
    }
    // The file is gone, so the next tickLogo() will want to download it —
    // ask it to skip the retry window and any back-off accumulated for this
    // IATA, so the re-download happens on the very next net-task pass
    // rather than up to a minute later. The net task owns the gate itself;
    // this is a one-word atomic hand-off (loop task -> net task).
    m_logoRetryReset.store(true, std::memory_order_relaxed);
}

void FlightsService::onStateMessage(const uint8_t *payload, size_t len, uint32_t nowMs)
{
    FlightsModel::FlightsSnapshot working;
    memset(&working, 0, sizeof(working));

    if (!FlightsModel::parseDialFlights(reinterpret_cast<const char *>(payload), len, working))
    {
        // Keep whatever is already on screen: one malformed message
        // shouldn't blank the card. Note this is not the path an oversized
        // HA payload takes — PubSubClient drops a packet larger than its
        // receive buffer whole, so nothing reaches the parser at all and
        // the symptom is the list going `stale` after kStaleAfterMs.
        // tick() ages the old list out that way either way.
        log_w("FlightsService: flights payload failed to parse (%u bytes) — keeping previous list", (unsigned)len);
        return;
    }

    working.fetchedMs = nowMs;
    working.stale     = false;
    working.offline   = false;
    m_snapshot.write(working);

    m_lastMessageMs = nowMs;
    m_haveMessage   = true;

    log_i("FlightsService: flights state accepted, %u aircraft, heap=%u",
          (unsigned)working.count, (unsigned)ESP.getFreeHeap());
}

void FlightsService::tick(uint32_t nowMs)
{
    const bool mqttUp  = m_net.mqttUp();
    const bool wifiUp  = m_net.wifiUp();
    const bool visible = m_visible.load(std::memory_order_relaxed);

    // M2: advance the HTTP buffer's alloc/free lifecycle on every tick (even
    // while not visible) so the free-delay countdown actually elapses.
    manageHttpBuf(nowMs, visible && wifiUp);

    // Freshness, every tick regardless of visibility: the card is fed
    // asynchronously now, so "offline" is simply MQTT being down, and
    // "stale" is HA having gone quiet for kStaleAfterMs with a list still
    // on screen. Neither flag ever discards aircraft — the last list stays
    // put, flagged, until HA sends a new one.
    const bool stale = mqttUp && m_haveMessage && (nowMs - m_lastMessageMs) > kStaleAfterMs;
    m_snapshot.modify([mqttUp, stale](FlightsModel::FlightsSnapshot &s) {
        s.offline = !mqttUp;
        s.stale   = stale;
    });

    if (!visible || !wifiUp || !mqttUp || m_httpBuf == nullptr)
    {
        // Not on screen, no WiFi, no MQTT, or malloc() failed this tick
        // under heap pressure — nothing to do but try again next tick.
        // MQTT is part of the gate because the logo host is Home Assistant
        // itself (FLIGHTS_LOGO_BASE_URL defaults to MQTT_SERVER:8123): if
        // the broker is unreachable, HA's web server almost certainly is
        // too, so a GET would just burn a connect timeout every window.
        return;
    }

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
        // dropped) — every fetchLogo() call runs to completion synchronously
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

// M1: the logo itself never comes back to this class any more — it's
// downloaded (or already cached) straight to /logos/{iata}.565 on LittleFS,
// and DialFlightsView copies it to the panel from there once logoReady()
// says it's good. This
// function's only job is to make sure that file exists and is valid, and to
// record that in m_logoStatus (the small Guarded flag logoReady() reads).
void FlightsService::tickLogo(uint32_t nowMs)
{
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

    // invalidateLogo() asks for the next attempt to be immediate (the
    // cached file has just been deleted after a decode failure).
    if (m_logoRetryReset.exchange(false, std::memory_order_relaxed))
    {
        m_logoAttempted = false;
        m_logoRetryMs   = kLogoRetryMs;
    }

    if (isLogoMissing(wanted.v, nowMs))
    {
        return;
    }

    char path[24];
    snprintf(path, sizeof(path), "/logos/%s.565", wanted.v);
    char tmpPath[24];
    snprintf(tmpPath, sizeof(tmpPath), "/logos/%s.tmp", wanted.v);

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
        // Cache-hit file failed validation (wrong size)
        // — remove it and fall through to a fresh download.
        log_w("FlightsService: logo %s cache file invalid, removing and refetching", wanted.v);
        LittleFS.remove(path);
    }

    // Bounded retry: see kLogoRetryMs/m_logoRetryMs. The cache-hit path
    // above is deliberately outside this gate — it costs one LittleFS stat
    // and is how a freshly-wanted logo goes ready with no network at all.
    const bool sameAsLastAttempt =
        strncmp(m_lastLogoAttemptIata, wanted.v, sizeof(m_lastLogoAttemptIata)) == 0;
    if (m_logoAttempted && sameAsLastAttempt && (nowMs - m_lastLogoAttemptMs) < m_logoRetryMs)
    {
        return;
    }
    if (!sameAsLastAttempt)
    {
        // A different airline: this one's history is its own, so start it
        // at the base window rather than inheriting the previous IATA's
        // back-off (cycling through aircraft still fetches promptly).
        m_logoRetryMs = kLogoRetryMs;
    }
    m_logoAttempted     = true;
    m_lastLogoAttemptMs = nowMs;
    safeCopy(wanted.v, m_lastLogoAttemptIata, sizeof(m_lastLogoAttemptIata));

    size_t    len    = 0;
    const int status = fetchLogo(wanted.v, tmpPath, len);

    if (status == 404)
    {
        // The relay has no logo for this airline (pics.avs.io 404) —
        // remembered for kLogoMissingTtlMs so the card doesn't ask again
        // every tick.
        markLogoMissing(wanted.v, nowMs);
        log_i("FlightsService: logo %s not found (404), remembered for %u ms", wanted.v,
              (unsigned)kLogoMissingTtlMs);
        return;
    }
    if (status == -2)
    {
        // Body longer than a raw 120x48 image: not something this build can
        // show, so treated as missing for kLogoMissingTtlMs rather than
        // retried every tick (fetchLogo() already log_w'd the length).
        markLogoMissing(wanted.v, nowMs);
        log_w("FlightsService: logo %s oversized, treating as missing for %u ms", wanted.v,
              (unsigned)kLogoMissingTtlMs);
        return;
    }
    if (status != 200)
    {
        // Transient (a connect/transport error is status < 0, a 5xx is the
        // number itself) — retried after the back-off window, not marked
        // missing.
        backOffLogoRetry();
        log_w("FlightsService: logo %s download failed (status=%d), next attempt in %u ms", wanted.v, status,
              (unsigned)m_logoRetryMs);
        return;
    }
    if (strncasecmp(m_lastContentType, "application/octet-stream", 24) != 0)
    {
        backOffLogoRetry();
        LittleFS.remove(tmpPath);
        log_w("FlightsService: logo %s unexpected content-type '%s', discarding", wanted.v, m_lastContentType);
        return;
    }
    // The only validation a raw image needs: exactly 120 x 48 x 2 bytes.
    if (len != kLogoBytes)
    {
        backOffLogoRetry();
        LittleFS.remove(tmpPath);
        log_w("FlightsService: logo %s wrong size (%u bytes, want %u), discarding", wanted.v, (unsigned)len,
              (unsigned)kLogoBytes);
        return;
    }
    // Complete and the right size: promote the temp file. littlefs's rename
    // replaces an existing destination atomically, so a stale copy is never
    // removed first — a reset between the two steps would otherwise leave
    // the airline logo-less until the next download.
    if (!LittleFS.rename(tmpPath, path))
    {
        backOffLogoRetry();
        LittleFS.remove(tmpPath);
        log_w("FlightsService: logo %s rename to %s failed", wanted.v, path);
        return;
    }

    m_logoRetryMs = kLogoRetryMs; // success: back to the base window
    m_logoStatus.modify([&](LogoStatus &ls) {
        safeCopy(wanted.v, ls.iata, sizeof(ls.iata));
        ls.ready = true;
    });
    log_i("FlightsService: logo %s downloaded and cached (%u bytes), heap=%u",
          wanted.v, (unsigned)len, (unsigned)ESP.getFreeHeap());

    // I2: one-shot, first successful logo download only — confirms actual
    // net task stack headroom on hardware.
    if (!m_logoStackLogged)
    {
        m_logoStackLogged = true;
        log_i("FlightsService: net task stack high-water mark %u bytes free",
              (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    }
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
    f.close();
    return fileLen == kLogoBytes;
}

// --- retry back-off --------------------------------------------------------

// One consecutive failure for the current IATA: double the retry window, up
// to kLogoRetryMaxMs. Reset to kLogoRetryMs by a success, a change of wanted
// airline (tickLogo()) or invalidateLogo().
void FlightsService::backOffLogoRetry()
{
    if (m_logoRetryMs < kLogoRetryMaxMs)
    {
        const uint32_t doubled = m_logoRetryMs * 2;
        m_logoRetryMs = (doubled > kLogoRetryMaxMs) ? kLogoRetryMaxMs : doubled;
    }
}

// --- negative cache: simple linear scan + round-robin replacement ----------

// A hit that has aged past kLogoMissingTtlMs is dropped and reported as "not
// missing", so the logo is fetched again: HA's automation downloads a new
// airline's PNG a few seconds after publishing the aircraft, so an early 404
// means "not yet", not "never", and must not latch for the whole session.
bool FlightsService::isLogoMissing(const char *iata, uint32_t nowMs)
{
    for (uint8_t i = 0; i < m_missingCount; i++)
    {
        if (m_missingLogos[i].iata[0] == '\0')
        {
            continue; // slot freed by an earlier expiry
        }
        if (strncmp(m_missingLogos[i].iata, iata, sizeof(m_missingLogos[i].iata)) != 0)
        {
            continue;
        }
        // Wrap-safe age compare (millis() rolls over every ~49 days).
        if ((int32_t)(nowMs - m_missingLogos[i].whenMs) >= (int32_t)kLogoMissingTtlMs)
        {
            m_missingLogos[i].iata[0] = '\0';
            log_i("FlightsService: logo %.2s negative-cache entry expired, will retry", iata);
            return false;
        }
        return true;
    }
    return false;
}

void FlightsService::markLogoMissing(const char *iata, uint32_t nowMs)
{
    // Reuse an expired/free slot before overwriting a live one; only fall
    // back to round-robin replacement when the table is genuinely full.
    for (uint8_t i = 0; i < m_missingCount; i++)
    {
        if (m_missingLogos[i].iata[0] == '\0')
        {
            safeCopy(iata, m_missingLogos[i].iata, sizeof(m_missingLogos[i].iata));
            m_missingLogos[i].whenMs = nowMs;
            return;
        }
    }

    safeCopy(iata, m_missingLogos[m_missingNext].iata, sizeof(m_missingLogos[m_missingNext].iata));
    m_missingLogos[m_missingNext].whenMs = nowMs;
    m_missingNext = (uint8_t)((m_missingNext + 1) % kMissingLogoSize);
    if (m_missingCount < kMissingLogoSize)
    {
        m_missingCount++;
    }
}

// --- plain-HTTP logo GET ---------------------------------------------------

int FlightsService::fetchLogo(const char *iata, const char *path, size_t &len)
{
    len                  = 0;
    m_lastContentType[0] = '\0';

    char     host[64];
    char     basePath[64];
    uint16_t port = 80;
    if (!splitBaseUrl(FLIGHTS_LOGO_BASE_URL, host, sizeof(host), port, basePath, sizeof(basePath)))
    {
        // Malformed macro is a boot-time config mistake, not a transient
        // condition — log it once rather than on every tickLogo() retry.
        if (!m_baseUrlLogged)
        {
            m_baseUrlLogged = true;
            log_e("FlightsService: FLIGHTS_LOGO_BASE_URL '%s' is not a plain http:// URL", FLIGHTS_LOGO_BASE_URL);
        }
        return -1;
    }

    // One deadline for the whole GET — connect, headers and body — so the
    // net task is never held for more than kLogoTimeoutMs per attempt.
    const uint32_t deadline = millis() + kLogoTimeoutMs;

    WiFiClient client;
    if (client.connect(host, port, (int32_t)kLogoTimeoutMs) != 1 || (int32_t)(millis() - deadline) >= 0)
    {
        log_i("FlightsService: logo connect to %s:%u failed or timed out", host, (unsigned)port);
        client.stop();
        return -1;
    }

    // HTTP/1.0 with an explicit "Connection: close": no chunked encoding to
    // deal with, and the server closes the socket at the end of the body.
    char request[256];
    const int reqLen = snprintf(request, sizeof(request),
                                "GET %s/logo/%s.565 HTTP/1.0\r\n"
                                "Host: %s\r\n"
                                "Accept: application/octet-stream\r\n"
                                "Connection: close\r\n"
                                "\r\n",
                                basePath, iata, host);
    if (reqLen <= 0 || (size_t)reqLen >= sizeof(request))
    {
        client.stop();
        log_w("FlightsService: logo request line too long, skipping");
        return -1;
    }
    client.write(reinterpret_cast<const uint8_t *>(request), (size_t)reqLen);

    char line[128];
    if (!readLine(client, line, sizeof(line), deadline))
    {
        client.stop();
        log_w("FlightsService: logo %s — no status line before deadline", iata);
        return -1;
    }
    // "HTTP/1.x <code> <reason>"
    const char *sp = strchr(line, ' ');
    if (sp == nullptr || strncmp(line, "HTTP/1.", 7) != 0)
    {
        client.stop();
        log_w("FlightsService: logo %s — bad status line '%s'", iata, line);
        return -1;
    }
    const int status = (int)strtol(sp + 1, nullptr, 10);

    long contentLength = -1;
    for (;;)
    {
        if (!readLine(client, line, sizeof(line), deadline))
        {
            client.stop();
            log_w("FlightsService: logo %s — headers truncated (status=%d)", iata, status);
            return -1;
        }
        if (line[0] == '\0')
        {
            break; // end of headers
        }
        if (strncasecmp(line, "Content-Type:", 13) == 0)
        {
            const char *v = line + 13;
            while (*v == ' ') { v++; }
            safeCopy(v, m_lastContentType, sizeof(m_lastContentType));
        }
        else if (strncasecmp(line, "Content-Length:", 15) == 0)
        {
            contentLength = strtol(line + 15, nullptr, 10);
        }
    }

    if (status != 200)
    {
        client.stop();
        log_i("FlightsService: GET %s:%u%s/logo/%s.565 -> status %d", host, (unsigned)port, basePath, iata, status);
        return status;
    }
    if (contentLength > (long)kLogoBytes)
    {
        client.stop();
        log_w("FlightsService: logo %s body too large (%ld > %u)", iata, contentLength, (unsigned)kLogoBytes);
        return -2;
    }

    // Stream the body straight into the temp file, kHttpBufCap bytes at a
    // time: the raw image is 11.5 KB, which is more heap than the net task
    // should hold while the card is up, and LittleFS writes are cheap.
    File out = LittleFS.open(path, "w");
    if (!out)
    {
        client.stop();
        log_w("FlightsService: failed to open %s for write", path);
        return -1;
    }

    size_t total   = 0;
    int    result  = status;
    while (true)
    {
        if ((int32_t)(millis() - deadline) >= 0)
        {
            log_w("FlightsService: logo %s read deadline exceeded at %u bytes", iata, (unsigned)total);
            break;
        }
        const int avail = client.available();
        if (avail <= 0)
        {
            if (!client.connected())
            {
                break; // server closed: body complete (HTTP/1.0, Connection: close)
            }
            if (contentLength > 0 && total >= (size_t)contentLength)
            {
                break;
            }
            delay(1);
            continue;
        }
        size_t toRead = (size_t)avail;
        if (toRead > kHttpBufCap)
        {
            toRead = kHttpBufCap;
        }
        if (total + toRead > kLogoBytes)
        {
            // Longer than a raw 120x48 image can be: not ours to cache.
            log_w("FlightsService: logo %s body exceeds %u bytes", iata, (unsigned)kLogoBytes);
            result = -2;
            break;
        }
        const int got = client.read(m_httpBuf, toRead);
        if (got <= 0)
        {
            break;
        }
        if (out.write(m_httpBuf, (size_t)got) != (size_t)got)
        {
            log_w("FlightsService: logo %s write to %s failed at %u bytes", iata, path, (unsigned)total);
            result = -1;
            break;
        }
        total += (size_t)got;
        if (contentLength > 0 && total >= (size_t)contentLength)
        {
            break;
        }
    }
    client.stop();
    out.close();

    if (result == status && contentLength > 0 && total < (size_t)contentLength)
    {
        // Connection closed early, or the 3 s deadline hit, before the
        // full advertised body arrived: treat as transient (retried by
        // tickLogo()) rather than caching a partial image.
        log_w("FlightsService: logo %s body truncated (%u/%ld bytes)", iata, (unsigned)total, contentLength);
        result = -1;
    }
    if (result != status)
    {
        LittleFS.remove(path);
        return result;
    }

    len = total;
    log_i("FlightsService: GET %s:%u%s/logo/%s.565 -> status %d, %u bytes, heap=%u", host, (unsigned)port,
          basePath, iata, status, (unsigned)len, (unsigned)ESP.getFreeHeap());
    return status;
}

#endif // HAS_DIAL_UI
