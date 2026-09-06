#include "TimeService.h"

#include <Arduino.h>
#include <esp_log.h>
#include <stdlib.h>
#include <sys/time.h>

#include "TimeMath.h"

#if HAS_DIAL_UI
#include <M5Unified.h>
#endif

// -DUSE_ESP_IDF_LOG (spec 4.14) makes log_x() expand to
// ESP_LOG_LEVEL_LOCAL(..., TAG, ...); esp32-hal-log.h has no default TAG.
static const char *TAG = "TimeService";

void TimeService::begin()
{
    // Apply the TZ rule unconditionally, before anything reads local time.
    // Without this, localtime_r() returns UTC (the C library's default)
    // until onWifiUp()'s configTzTime() runs — which can be minutes into
    // boot, or never, if WiFi never comes up. Safe to call on both boards:
    // it only affects how time_t is rendered, not the underlying clock.
    setenv("TZ", TIMEZONE_TZ, 1);
    tzset();

#if HAS_DIAL_UI
    if (!M5.Rtc.isEnabled())
    {
        log_i("TimeService: RTC not present/enabled, no boot time available");
        return;
    }

    const m5::rtc_datetime_t dt = M5.Rtc.getDateTime();
    if (dt.date.year < 2024)
    {
        log_i("TimeService: RTC date implausible (year=%d), skipping boot read", dt.date.year);
        return;
    }

    const struct tm utcTm = dt.get_tm(); // RTC is always stored/read as UTC
    const time_t    epoch = static_cast<time_t>(TimeMath::utcToEpoch(
        utcTm.tm_year + 1900, utcTm.tm_mon + 1, utcTm.tm_mday,
        utcTm.tm_hour, utcTm.tm_min, utcTm.tm_sec));
    struct timeval  tv    = {};
    tv.tv_sec  = epoch;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);

    m_source = Source::RTC;
    logSourceChange();
#endif // HAS_DIAL_UI
}

void TimeService::onWifiUp()
{
    if (m_tzConfigured)
    {
        return;
    }
    m_tzConfigured = true;
    m_wifiUp       = true;
    configTzTime(TIMEZONE_TZ, "pool.ntp.org", "time.google.com");
    log_i("TimeService: NTP configured (tz=%s)", TIMEZONE_TZ);
}

void TimeService::tick(uint32_t nowMs)
{
    if (m_source == Source::NTP || !m_wifiUp)
    {
        return;
    }

    // Poll at most once a second; never block (0 ms timeout).
    if (nowMs - m_lastPollMs < kPollIntervalMs)
    {
        return;
    }
    m_lastPollMs = nowMs;

    // Non-blocking: getLocalTime() delay(10)s internally on failure, which
    // would stall the loop() every poll until SNTP completes. time()/
    // localtime_r() never block, so read the system clock directly and use
    // the same plausibility check to detect "SNTP hasn't completed yet".
    const time_t nowSec = time(nullptr);
    struct tm    tmNow;
    localtime_r(&nowSec, &tmNow);
    if (tmNow.tm_year + 1900 < 2024)
    {
        return; // system clock not plausible yet (SNTP hasn't completed)
    }

    m_source = Source::NTP;

#if HAS_DIAL_UI
    if (M5.Rtc.isEnabled())
    {
        const time_t   now = time(nullptr); // epoch is UTC regardless of TZ
        struct tm      utcTm;
        gmtime_r(&now, &utcTm);
        const m5::rtc_datetime_t dt(utcTm);
        M5.Rtc.setDateTime(dt);
    }
#endif // HAS_DIAL_UI

    logSourceChange();
}

bool TimeService::valid() const
{
    const time_t now = time(nullptr);
    struct tm    t;
    localtime_r(&now, &t);
    return (t.tm_year + 1900) >= 2024;
}

bool TimeService::localTime(struct tm &out) const
{
    const time_t now = time(nullptr);
    localtime_r(&now, &out);
    return (out.tm_year + 1900) >= 2024;
}

void TimeService::logSourceChange() const
{
    const time_t now = time(nullptr);
    struct tm    t;
    localtime_r(&now, &t);
    char buf[20]; // "YYYY-MM-DD HH:MM:SS" + NUL
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);

    const char *srcName = (m_source == Source::RTC) ? "RTC" : (m_source == Source::NTP) ? "NTP" : "NONE";
    log_i("Time: source=%s local=%s", srcName, buf);
}
