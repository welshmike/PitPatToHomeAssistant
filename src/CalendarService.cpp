#include "CalendarService.h"

#if HAS_CALENDAR

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <string.h>

#include "CalendarCerts.h"
#include "NetManager.h"

// -DUSE_ESP_IDF_LOG (spec 4.14) makes log_x() expand to
// ESP_LOG_LEVEL_LOCAL(..., TAG, ...); esp32-hal-log.h has no default TAG.
static const char *TAG = "Calendar";

uint8_t CalendarService::s_buf[CalendarService::kBufCap];

CalendarService::CalendarService(NetManager &net) : m_net(net) {}

void CalendarService::begin()
{
    // Nothing to mount — no LittleFS use here, unlike FlightsService's logo
    // cache. Present for symmetry with the other net-task services.
}

void CalendarService::requestRefresh()
{
    m_refreshWanted = true;
}

CalendarModel::Snapshot CalendarService::snapshot() const
{
    return m_snapshot.read();
}

bool CalendarService::fetchedOnce() const
{
    return m_fetchedOnce;
}

void CalendarService::tick(uint32_t nowMs)
{
    // Same gate FlightsService uses: no point opening a TLS connection while
    // the link itself is down, and wifiUp() covers MQTT_CONNECTING/MQTT_UP
    // too (this fetch needs only WiFi, not the broker).
    if (!m_net.wifiUp())
    {
        return;
    }

    bool due = (int32_t)(nowMs - m_nextFetchMs) >= 0;
    if (!due && m_refreshWanted)
    {
        // "Last fetch" isn't tracked directly — derive it from the
        // schedule: m_nextFetchMs was set to (last fetch time + delta),
        // where delta is the back-off window on a failure or kPollMs on a
        // success. Reversing that gives the last-fetch timestamp without
        // needing an extra field.
        const uint32_t delta       = (m_backoffMs != 0) ? m_backoffMs : kPollMs;
        const uint32_t lastFetchMs = m_nextFetchMs - delta;
        const uint32_t ageMs       = nowMs - lastFetchMs;
        if ((int32_t)(ageMs - 60000) >= 0)
        {
            due = true;
        }
    }

    if (!due)
    {
        return;
    }

    // Consumed either way: honoured just now, or too soon since the last
    // fetch — either way the loop task will ask again on its next poll if
    // it still wants one (DialUi calls requestRefresh() roughly once a
    // second while an event is imminent, see task 7).
    m_refreshWanted = false;
    fetchNow(nowMs);
}

bool CalendarService::fetchNow(uint32_t nowMs)
{
    // Heap guard (mirrors FlightsService's httpsGet): a TLS handshake's
    // mbedTLS buffers are the single biggest transient heap consumer in
    // this firmware, so refuse to start one while headroom is thin rather
    // than risk an allocation failure mid-handshake. Retried in a flat
    // 60 s regardless of the current back-off state.
    if (ESP.getFreeHeap() < 60 * 1024 || ESP.getMaxAllocHeap() < 20 * 1024)
    {
        log_w("Calendar: skipping fetch, heap free=%u largest=%u", (unsigned)ESP.getFreeHeap(),
              (unsigned)ESP.getMaxAllocHeap());
        m_nextFetchMs = nowMs + 60000;
        return false;
    }

    static const char kUrl[] = CALENDAR_URL "?k=" CALENDAR_TOKEN;

    WiFiClientSecure client;
#ifdef CALENDAR_TLS_INSECURE
    client.setInsecure();
#else
    client.setCACert(kCalendarRootCAs);
#endif

    HTTPClient http;
    http.setConnectTimeout(5000);
    http.setTimeout(8000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setReuse(false);

    if (!http.begin(client, kUrl))
    {
        log_w("Calendar: begin() failed for %s", kUrl);
        http.end();
        m_backoffMs   = m_backoffMs ? min(m_backoffMs * 2, kBackoffMaxMs) : kBackoffMinMs;
        m_nextFetchMs = nowMs + m_backoffMs;
        log_w("Calendar: fetch failed code=%d (begin failed), retry in %u s", -1, (unsigned)(m_backoffMs / 1000));
        return false;
    }

    const int code = http.GET();
    if (code != HTTP_CODE_OK)
    {
        http.end();
        m_backoffMs   = m_backoffMs ? min(m_backoffMs * 2, kBackoffMaxMs) : kBackoffMinMs;
        m_nextFetchMs = nowMs + m_backoffMs;
        log_w("Calendar: fetch failed code=%d (%s), retry in %u s", code, http.errorToString(code).c_str(),
              (unsigned)(m_backoffMs / 1000));
        return false;
    }

    // Read the body into the static buffer, same shape as FlightsService's
    // httpsGet: stop at kBufCap-1 (room for the NUL), honour Content-Length
    // when the server sends one so a keep-alive connection with no more
    // data doesn't stall the read loop until the deadline, and otherwise
    // bound the whole read by an 8 s deadline.
    const int      contentLength = http.getSize();
    WiFiClient    *stream        = http.getStreamPtr();
    const uint32_t deadline      = millis() + 8000;
    size_t         total         = 0;
    while (http.connected() && total < kBufCap - 1)
    {
        if ((int32_t)(millis() - deadline) >= 0)
        {
            log_w("Calendar: read deadline exceeded at %u bytes", (unsigned)total);
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
        if (toRead > kBufCap - 1 - total)
        {
            toRead = kBufCap - 1 - total;
        }
        const int got = stream->readBytes(reinterpret_cast<char *>(s_buf) + total, toRead);
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
    s_buf[total] = '\0';

    CalendarModel::Snapshot parsed;
    if (!CalendarModel::parse(reinterpret_cast<const char *>(s_buf), total, parsed))
    {
        m_backoffMs   = m_backoffMs ? min(m_backoffMs * 2, kBackoffMaxMs) : kBackoffMinMs;
        m_nextFetchMs = nowMs + m_backoffMs;
        log_w("Calendar: fetch failed code=%d (parse failed, %u bytes), retry in %u s", code, (unsigned)total,
              (unsigned)(m_backoffMs / 1000));
        return false;
    }

    m_snapshot.write(parsed);
    m_backoffMs   = 0;
    m_nextFetchMs = nowMs + kPollMs;
    log_i("Calendar: %u events, %u bytes", (unsigned)parsed.count, (unsigned)total);

    // One-shot, first successful fetch only — confirms actual net task
    // stack headroom on hardware, same as FlightsService's logo download.
    if (!m_fetchedOnce)
    {
        log_i("Calendar: net task stack high-water mark %u bytes free", (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    }
    m_fetchedOnce = true;

    return true;
}

#endif // HAS_CALENDAR
