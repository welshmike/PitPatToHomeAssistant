#include "TimeService.h"

#include <Arduino.h>
#include <esp_log.h>
#include <sys/time.h>

#if HAS_DIAL_UI
#include <M5Unified.h>
#endif

namespace {

#if HAS_DIAL_UI
// Converts a UTC broken-down time to a UTC epoch (seconds since 1970-01-01).
// Arduino-ESP32's newlib doesn't provide timegm(), and mktime() applies the
// process TZ (wrong here — the RTC is stored in UTC, see TimeService.h), so
// this is Howard Hinnant's days_from_civil algorithm instead: no library
// dependency, no TZ state to save/restore, exact for the whole range the RTC
// can hold.
time_t utcTmToEpoch(const struct tm &utc)
{
    int64_t y = utc.tm_year + 1900;
    const int m = utc.tm_mon + 1;
    const int d = utc.tm_mday;
    y -= (m <= 2) ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const uint32_t yoe = static_cast<uint32_t>(y - era * 400);           // [0, 399]
    const uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; // [0, 365]
    const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;          // [0, 146096]
    const int64_t days = era * 146097 + static_cast<int64_t>(doe) - 719468; // since 1970-01-01

    return static_cast<time_t>(days * 86400 + utc.tm_hour * 3600 + utc.tm_min * 60 + utc.tm_sec);
}
#endif // HAS_DIAL_UI

} // namespace

void TimeService::begin()
{
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
    const time_t    epoch = utcTmToEpoch(utcTm);
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

    struct tm tmNow;
    if (!getLocalTime(&tmNow, 0))
    {
        return; // SNTP hasn't completed yet
    }
    if (tmNow.tm_year + 1900 < 2024)
    {
        return; // system clock not plausible yet
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
    return valid();
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
