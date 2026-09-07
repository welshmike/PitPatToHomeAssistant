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

namespace
{
// Doubling back-off, capped. Written as an explicit ternary rather than
// min(m_backoffMs * 2, kBackoffMaxMs): min() takes its arguments by
// reference, which odr-uses the static constexpr members. The device build
// is gnu++11 (arduino-esp32 2.0.17's default), so they are not implicitly
// inline and have no out-of-class definition — that only links by luck of
// inlining. Passing/returning by value here never takes their address.
uint32_t nextBackoffMs(uint32_t currentMs)
{
    if (currentMs == 0)
    {
        return CalendarService::kBackoffMinMs;
    }
    const uint32_t doubled = currentMs * 2;
    return (doubled > CalendarService::kBackoffMaxMs) ? CalendarService::kBackoffMaxMs : doubled;
}

// Logs the net task's stack high-water mark once, after the first fetch
// *attempt* — success or failure. Scope-exit rather than a line at the end
// of fetchNow() because the failure paths return early, and a failing fetch
// (TLS handshake, redirect, error body) is exactly the deep path worth
// measuring. Runs after the WiFiClientSecure/HTTPClient locals are gone,
// which is fine: the FreeRTOS watermark is the minimum ever recorded for
// the task, not the depth at the moment it is read.
struct StackLogOnce
{
    bool &logged;
    ~StackLogOnce()
    {
        if (!logged)
        {
            logged = true;
            log_i("Calendar: net task stack high-water mark %u bytes free",
                  (unsigned)uxTaskGetStackHighWaterMark(nullptr));
        }
    }
};
} // namespace

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
        // Honour the early refresh only if the last *attempt* is more than
        // 60 s old — m_lastFetchMs, not something derived from
        // m_nextFetchMs, because the heap guard reschedules without
        // touching m_backoffMs and any derivation would then read a stale
        // interval. Never having attempted one at all makes the request
        // due immediately.
        const uint32_t ageMs = nowMs - m_lastFetchMs;
        if (!m_haveLastFetch || (int32_t)(ageMs - 60000) >= 0)
        {
            due = true;
        }
        else
        {
            // Too soon: drop the request rather than latching it. Leaving
            // it set would re-enter this branch on every tick, and a low
            // heap (which reschedules in 60 s and logs a warning each time)
            // would turn that into a log storm on MQTT. The loop task asks
            // again on its next poll if it still wants one — DialUi calls
            // requestRefresh() roughly once a second while an event is
            // imminent (task 7).
            m_refreshWanted = false;
        }
    }

    if (!due)
    {
        return;
    }

    // Consumed: this fetch serves whatever the loop task was asking for.
    m_refreshWanted = false;
    fetchNow(nowMs);
}

bool CalendarService::fetchNow(uint32_t nowMs)
{
    // Every attempt counts as "went near the network", including the ones
    // the heap guard turns away below: tick()'s 60 s early-refresh rule is
    // there to stop re-entry, so a skipped attempt has to move it too.
    m_lastFetchMs   = nowMs;
    m_haveLastFetch = true;

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

    // Deep-path measurement, first attempt only, success or failure.
    StackLogOnce stackLog{m_stackLogged};

    // Wall-clock budget for everything below (spec 4.19). The connect
    // timeouts are per hop, so the untrimmed worst case is 5 s to the Apps
    // Script host + 5 s to its redirect target + 8 s of reading ≈ 18 s,
    // which is longer than PubSubClient's 15 s keepalive — the net task
    // drives that too, so the broker would drop us on every bad fetch. The
    // budget bounds the part we can bound (the read window, below); the
    // individual connects stay per hop because HTTPClient offers no way to
    // shorten them mid-request.
    const uint32_t budgetEndMs = millis() + kFetchBudgetMs;

    // Not logged anywhere, ever: the token rides in the query string, and
    // RemoteLog forwards warnings to MQTT (spec 4.14) where anyone on the
    // broker would see it.
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
    // Forces HTTP/1.0 so the server cannot answer with a chunked
    // Transfer-Encoding. This is not cosmetic: the read loop below pulls
    // bytes straight off getStreamPtr(), which is the raw client — only
    // HTTPClient::writeToStream() de-chunks — so a chunked reply would land
    // in s_buf as "2b\r\n{...}\r\n0\r\n\r\n" and fail CalendarModel::parse
    // every single time. Apps Script's exec URL redirects to a
    // googleusercontent host that does reply chunked over HTTP/1.1, so this
    // is the normal case here, not an edge one. Same reason (and the same
    // call) as the pre-Plan-7 FlightsService::httpsGet helper. It also
    // means a Content-Length is present whenever the server sends one,
    // which the loop below uses to finish early.
    http.useHTTP10(true);

    if (!http.begin(client, kUrl))
    {
        // No URL in the message — see the kUrl comment above.
        http.end();
        m_backoffMs   = nextBackoffMs(m_backoffMs);
        m_nextFetchMs = nowMs + m_backoffMs;
        log_w("Calendar: fetch failed code=%d (begin failed), retry in %u s", -1, (unsigned)(m_backoffMs / 1000));
        return false;
    }

    const int code = http.GET();
    if (code != HTTP_CODE_OK)
    {
        http.end();
        m_backoffMs   = nextBackoffMs(m_backoffMs);
        m_nextFetchMs = nowMs + m_backoffMs;
        log_w("Calendar: fetch failed code=%d (%s), retry in %u s", code, http.errorToString(code).c_str(),
              (unsigned)(m_backoffMs / 1000));
        return false;
    }

    // Read the body into the static buffer, same shape as FlightsService's
    // httpsGet: stop at kBufCap-1 (room for the NUL), honour Content-Length
    // when the server sends one so a keep-alive connection with no more
    // data doesn't stall the read loop until the deadline, and otherwise
    // bound the whole read by a deadline.
    const int   contentLength = http.getSize();
    WiFiClient *stream        = http.getStreamPtr();
    if (stream == nullptr)
    {
        // Documented as possible when the connection is already gone.
        http.end();
        m_backoffMs   = nextBackoffMs(m_backoffMs);
        m_nextFetchMs = nowMs + m_backoffMs;
        log_w("Calendar: fetch failed code=%d (no stream), retry in %u s", code, (unsigned)(m_backoffMs / 1000));
        return false;
    }

    // Read window: the 8 s socket timeout, or whatever is left of the
    // overall fetch budget if the connects have already eaten into it. A
    // budget that is already spent means no window at all, so the loop
    // exits on its first deadline check and the attempt is backed off.
    const int32_t  budgetLeftMs = (int32_t)(budgetEndMs - millis());
    const uint32_t readWindowMs = (budgetLeftMs <= 0) ? 0 : (budgetLeftMs > 8000) ? 8000 : (uint32_t)budgetLeftMs;
    const uint32_t deadline     = millis() + readWindowMs;
    size_t         total        = 0;
    bool           timedOut     = false;
    while (http.connected() && total < kBufCap - 1)
    {
        if ((int32_t)(millis() - deadline) >= 0)
        {
            log_w("Calendar: read deadline exceeded at %u bytes", (unsigned)total);
            timedOut = true;
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

    // A truncated body would only fail to parse a moment later, but say so
    // explicitly: running out of budget is a fetch failure and takes the
    // back-off, it is not "the feed sent us bad JSON".
    if (timedOut)
    {
        m_backoffMs   = nextBackoffMs(m_backoffMs);
        m_nextFetchMs = nowMs + m_backoffMs;
        log_w("Calendar: fetch failed code=%d (out of time, %u bytes), retry in %u s", code, (unsigned)total,
              (unsigned)(m_backoffMs / 1000));
        return false;
    }

    CalendarModel::Snapshot parsed;
    if (!CalendarModel::parse(reinterpret_cast<const char *>(s_buf), total, parsed))
    {
        m_backoffMs   = nextBackoffMs(m_backoffMs);
        m_nextFetchMs = nowMs + m_backoffMs;
        log_w("Calendar: fetch failed code=%d (parse failed, %u bytes), retry in %u s", code, (unsigned)total,
              (unsigned)(m_backoffMs / 1000));
        return false;
    }

    m_snapshot.write(parsed);
    m_backoffMs   = 0;
    m_nextFetchMs = nowMs + kPollMs;
    log_i("Calendar: %u events, %u bytes", (unsigned)parsed.count, (unsigned)total);

    m_fetchedOnce = true;

    return true;
}

#endif // HAS_CALENDAR
