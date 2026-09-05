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
// headers and body). HA is on the LAN and a logo is a few KB, so 3 s is
// generous — the point is that the net task also has to keep servicing
// PubSubClient (MQTT keepalive is 15 s, and NetTask::run() only calls
// m_net.tick() between tick() calls, never during one), so a stalled logo
// request must never be able to hold tick() open.
constexpr uint32_t kLogoTimeoutMs = 3000;

// C2: PNG signature, checked before a downloaded logo is written to
// LittleFS and again on the cache-hit path (a file that fails this check —
// truncated, corrupted, or not a PNG at all — is removed and refetched).
constexpr uint8_t kPngSignature[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

bool isPngSignature(const uint8_t *data, size_t len)
{
    return len >= sizeof(kPngSignature) && memcmp(data, kPngSignature, sizeof(kPngSignature)) == 0;
}

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
// downloaded (or already cached) straight to /logos/{iata}.png on LittleFS,
// and DialUi decodes it from there once logoReady() says it's good. This
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
    const int status = fetchLogo(wanted.v, len);

    if (status == 404)
    {
        // HA has no www/logos/{IATA}.png for this airline — remembered so
        // the card doesn't ask again every tick this session.
        markLogoMissing(wanted.v, nowMs);
        log_i("FlightsService: logo %s not found (404), remembered for %u ms", wanted.v,
              (unsigned)kLogoMissingTtlMs);
        return;
    }
    if (status == -2)
    {
        // M2: body exceeded kHttpBufCap. HA's logo PNGs run 3-5 KB; an
        // outlier this large is treated as missing for the rest of this
        // session rather than retried every tick (fetchLogo() already
        // log_w'd the actual length).
        markLogoMissing(wanted.v, nowMs);
        log_w("FlightsService: logo %s exceeds %u-byte cap, treating as missing for %u ms", wanted.v,
              (unsigned)kHttpBufCap, (unsigned)kLogoMissingTtlMs);
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
    if (strncasecmp(m_lastContentType, "image/png", 9) != 0)
    {
        backOffLogoRetry();
        log_w("FlightsService: logo %s unexpected content-type '%s', discarding", wanted.v, m_lastContentType);
        return;
    }
    // C2: verify the PNG signature before it ever touches LittleFS.
    if (len <= sizeof(kPngSignature) || !isPngSignature(m_httpBuf, len))
    {
        backOffLogoRetry();
        log_w("FlightsService: logo %s failed PNG signature check (%u bytes), discarding", wanted.v, (unsigned)len);
        return;
    }

    File out = LittleFS.open(path, "w");
    if (!out)
    {
        backOffLogoRetry();
        log_w("FlightsService: failed to open %s for write", path);
        return;
    }
    const size_t written = out.write(m_httpBuf, len);
    out.close();
    if (written != len)
    {
        // C2: partial/failed write — remove it rather than leave a
        // truncated file behind; retried next visible session.
        backOffLogoRetry();
        log_w("FlightsService: logo %s write incomplete (%u/%u bytes), removing",
              wanted.v, (unsigned)written, (unsigned)len);
        LittleFS.remove(path);
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

int FlightsService::fetchLogo(const char *iata, size_t &len)
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

    WiFiClient client;
    if (client.connect(host, port, (int32_t)kLogoTimeoutMs) != 1)
    {
        log_i("FlightsService: logo connect to %s:%u failed", host, (unsigned)port);
        client.stop();
        return -1;
    }

    // HTTP/1.0 with an explicit "Connection: close": no chunked encoding to
    // deal with, and the server closes the socket at the end of the body.
    char request[256];
    const int reqLen = snprintf(request, sizeof(request),
                                "GET %s/logos/%s.png HTTP/1.0\r\n"
                                "Host: %s\r\n"
                                "Accept: image/png\r\n"
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

    const uint32_t deadline = millis() + kLogoTimeoutMs;

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
        log_i("FlightsService: GET %s:%u%s/logos/%s.png -> status %d", host, (unsigned)port, basePath, iata, status);
        return status;
    }
    if (contentLength > (long)kHttpBufCap)
    {
        client.stop();
        log_w("FlightsService: logo %s body too large (%ld > %u)", iata, contentLength, (unsigned)kHttpBufCap);
        return -2;
    }

    size_t total = 0;
    while (total < kHttpBufCap)
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
        if (toRead > kHttpBufCap - total)
        {
            toRead = kHttpBufCap - total;
        }
        const int got = client.read(m_httpBuf + total, toRead);
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
    client.stop();

    if (contentLength > 0 && total < (size_t)contentLength)
    {
        // Connection closed early, or the 3 s deadline hit, before the
        // full advertised body arrived: treat as transient (retried by
        // tickLogo()) rather than caching a partial PNG.
        log_w("FlightsService: logo %s body truncated (%u/%ld bytes)", iata, (unsigned)total, contentLength);
        return -1;
    }

    if (contentLength < 0 && total >= kHttpBufCap)
    {
        // No Content-Length and we filled the buffer: the body is at least
        // as big as the cap, so it may well be truncated — treat it as
        // oversized rather than caching a partial PNG.
        log_w("FlightsService: logo %s body filled the %u-byte cap with no Content-Length",
              iata, (unsigned)kHttpBufCap);
        return -2;
    }

    len = total;
    log_i("FlightsService: GET %s:%u%s/logos/%s.png -> status %d, %u bytes, heap=%u", host, (unsigned)port,
          basePath, iata, status, (unsigned)len, (unsigned)ESP.getFreeHeap());
    return status;
}

#endif // HAS_DIAL_UI
