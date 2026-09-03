#pragma once

#include <stdint.h>
#include <time.h>

#include "config.h"
#include "board.h"

// POSIX TZ string (see `man tzset` / the tz database's `zone.tab` POSIX-TZ
// notes) used to convert NTP's UTC time into local time. Default is
// Europe/London ("GMT0BST,M3.5.0/1,M10.5.0" — GMT in winter, BST from the
// last Sunday in March to the last Sunday in October). Define TIMEZONE_TZ in
// config.h to override; this fallback keeps existing config.h files (which
// predate this macro) building without an edit.
#ifndef TIMEZONE_TZ
#define TIMEZONE_TZ "GMT0BST,M3.5.0/1,M10.5.0"
#endif

// Wall-clock time for the Dial's clock card: NTP over WiFi with the POSIX TZ
// above, backed by the Dial's BM8563 RTC so the clock reads correctly before
// WiFi comes up (and survives a WiFi outage). Device-only (Arduino) — not
// part of the native/host test build.
//
// The RTC is always read and written in UTC, never local time. Local time
// shifts twice a year (BST/GMT) but the RTC has no concept of timezones, so
// treating its stored value as local would make the clock jump by an hour at
// every DST boundary if the firmware didn't happen to rewrite it that day.
// Storing UTC means the RTC only ever needs the TZ rule applied at read time,
// same as the system clock.
//
// begin() reads the RTC once at boot (Dial only) so the clock card has a
// plausible time immediately, before WiFi/NTP. onWifiUp() arms NTP once WiFi
// is up. tick() polls for the first successful NTP sync, then writes the RTC
// once (UTC) and stops polling — NTP requeries are handled by the SNTP
// background task Arduino-ESP32 starts internally, not by this class.
class TimeService
{
public:
    enum class Source
    {
        NONE, // no RTC, no NTP yet — system clock is not to be trusted
        RTC,  // seeded from the Dial's BM8563 at boot
        NTP,  // confirmed via NTP; RTC has been rewritten to match
    };

    // Dial only: if the BM8563 RTC is enabled and holds a plausible date
    // (year >= 2024), seeds the system clock from it (UTC) and sets
    // source() to RTC. No-op on boards without a Dial RTC (DevKit) or when
    // the RTC is unset/uninitialised.
    void begin();

    // Call once, when NetStatus first reaches WIFI_UP. Starts Arduino-ESP32's
    // SNTP client against TIMEZONE_TZ; idempotent, so it is safe to call
    // again on a later WiFi reconnect.
    void onWifiUp();

    // Call every loop() iteration. Non-blocking. While source() != NTP and
    // WiFi has come up at least once, polls for a completed NTP sync at most
    // once per second; on the first success, writes the RTC (Dial only, UTC)
    // and sets source() to NTP.
    void tick(uint32_t nowMs);

    // True once the system clock holds a plausible date (year >= 2024),
    // regardless of source.
    bool valid() const;

    // Fills `out` via localtime_r(time(nullptr), ...) and returns valid().
    // `out` is filled either way, but its contents are meaningless (typically
    // 1970) when this returns false.
    bool localTime(struct tm &out) const;

    Source source() const { return m_source; }

private:
    void logSourceChange() const;

    static constexpr uint32_t kPollIntervalMs = 1000;

    Source   m_source      = Source::NONE;
    bool     m_wifiUp      = false;
    bool     m_tzConfigured = false;
    uint32_t m_lastPollMs  = 0;
};
