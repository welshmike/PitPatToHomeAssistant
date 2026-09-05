#pragma once

#include <stdint.h>
#include <stddef.h>
#include <atomic>

#include "board.h"
#include "config.h"
#include "FlightsModel.h"
#include "Guarded.h"

// Where the airline logos live (spec 4.11). Home Assistant serves its
// `config/www/` directory at `/local/`, so `<HA>:8123/local/logos/{IATA}.png`
// is a plain-HTTP file on the LAN — no TLS, no third-party API. Set
// FLIGHTS_LOGO_BASE_URL in config.h to override; the default assumes HA runs
// on the same host as the MQTT broker with the default 8123 port.
#ifndef FLIGHTS_LOGO_BASE_URL
#define FLIGHTS_LOGO_BASE_URL "http://" MQTT_SERVER ":8123/local"
#endif

class NetManager;

// Holds the Dial's view of the aircraft overhead, published by Home
// Assistant as a retained JSON message on kStateTopic (spec 4.11), and
// caches the matching airline logos in LittleFS for the Flights card. HA
// does all the ADS-B work — proximity, distance/bearing, route and operator
// lookup — so this class never talks to a flight API itself; the only
// network call left here is a plain-HTTP GET of a logo PNG from HA's own
// `/local/` directory.
//
// Threading, mirroring LightsService: onStateMessage() runs on the network
// task inside the MQTT receive callback, and begin()/tick() run on the
// network task too (see NetTask.h/.cpp) — none of the three may be called
// from the loop task. The loop task (DialUi) talks to this class only
// through setVisible(), setWantedLogo(), snapshot(), logoReady() and
// invalidateLogo(), all of which are safe from any task. The logo PNG
// itself is never resident in this class — it lives only in
// /logos/{IATA}.png on LittleFS; DialUi decodes it straight from there once
// logoReady() says the file is present and valid (M1, spec review
// 2026-09-04).
//
// Device-only: this header is portable (no Arduino/ESP32 types), but the
// whole .cpp is compiled out (empty translation unit) unless HAS_DIAL_UI,
// so the class only actually does anything on the Dial build. NetTask.h
// only declares/uses the m_flights member under the same guard, so the
// DevKit build never instantiates or calls it.
class FlightsService
{
public:
    // HA's retained aircraft payload, parsed by
    // FlightsModel::parseDialFlights(). Subscribed at QoS 0 on every MQTT
    // connect (see NetTask::onMqttConnected()).
    static constexpr const char *kStateTopic = "pacekeeper-dial/flights/state";
    // Published (a trivial "1" payload) after every MQTT connect so the
    // HA-side automation re-publishes kStateTopic — same pattern as
    // LightsService::kRefreshTopic, and for the same reason: the retained
    // message may not exist yet right after the automation itself restarts.
    static constexpr const char *kRefreshTopic = "pacekeeper-dial/flights/refresh";

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

    // Net task only: called from NetTask::onMqttMessage() for kStateTopic.
    // Parses `payload` into a working FlightsSnapshot with
    // FlightsModel::parseDialFlights(), stamps fetchedMs = nowMs and clears
    // stale/offline, then publishes it. A payload that fails to parse is
    // logged at log_w and dropped — the previous list stays on screen
    // rather than the card blanking on one bad message.
    void onStateMessage(const uint8_t *payload, size_t len, uint32_t nowMs);

    // Net task only: maintains the freshness flags (offline while MQTT is
    // down, stale once HA has gone quiet for kStaleAfterMs) and, while the
    // card is visible, downloads/validates the cached logo for the IATA
    // setWantedLogo() last named. No aircraft data is fetched here any
    // more — that all arrives via onStateMessage().
    void tick(uint32_t nowMs);

    // loop task: safe from any task. Returns a copy.
    FlightsModel::FlightsSnapshot snapshot() const { return m_snapshot.read(); }

    // loop task: true once `iata`'s cached logo file (/logos/{iata}.png on
    // LittleFS) is confirmed present and valid — i.e. DialUi can go straight
    // to drawPngFile() on it. Never triggers network work itself — the
    // fetch/validate happens in tick() for whatever IATA setWantedLogo()
    // last set (M1, spec review 2026-09-04: no logo bytes are ever resident
    // in this class any more, only this tiny ready flag).
    bool logoReady(const char *iata) const;
    // Loop-task safe: the UI failed to decode this cached logo. Clears the
    // ready flag and deletes the file so tick() re-downloads it once.
    void invalidateLogo(const char *iata);

private:
    // Not part of FlightsModel: it's local buffering/caching state, not a
    // wire format, so it stays private to this class. Deliberately tiny —
    // no PNG bytes here (M1): the file on LittleFS is the only copy, and
    // DialUi decodes it directly from there.
    struct LogoStatus
    {
        char iata[3] = {0};
        bool ready   = false;
    };
    struct WantedIata
    {
        char v[3] = {0};
    };
    struct MissingLogo
    {
        char iata[3] = {0};
    };

    void tickLogo(uint32_t nowMs);

    // M2: (de)allocates m_httpBuf. `wantActive` is `visible && wifiUp` as
    // observed by tick(); called unconditionally at the top of every tick()
    // so the 60 s free-delay countdown advances even while the card is
    // off-screen. malloc's on the first tick the card is wanted and frees
    // kHttpBufFreeDelayMs after it stops being wanted (never frees mid-call
    // — every logo GET runs to completion synchronously inside tick(), so
    // "wantActive" only ever goes false between requests, not during one).
    void manageHttpBuf(uint32_t nowMs, bool wantActive);

    // C2: cache-hit validation for /logos/{iata}.png — true iff the file
    // opens, its size is > 8 bytes and its first 8 bytes are the PNG
    // signature. Does not touch m_httpBuf.
    bool validateLogoFile(const char *path) const;

    bool isLogoMissing(const char *iata) const;
    void markLogoMissing(const char *iata);

    // Plain-HTTP GET of FLIGHTS_LOGO_BASE_URL "/logos/{iata}.png" into
    // m_httpBuf (HA serves these off its own `config/www/`, so there is no
    // TLS and no third party involved any more). Raw WiFiClient rather than
    // HTTPClient: an HTTP/1.0 request line, three headers and a
    // status-line/header parse is all this needs, and it keeps the net
    // task's stack and heap cost to the 8 KB body buffer. Connect and
    // response deadlines are both kLogoTimeoutMs. Returns the HTTP status
    // code, or a negative value on a transport error (-1) or a body over
    // kHttpBufCap (-2). Leaves the response's Content-Type in
    // m_lastContentType and the body length in `len`. Net task only.
    int fetchLogo(const char *iata, size_t &len);

    NetManager &m_net;

    // loop task writes, net task reads (setVisible()/tick()) — a plain
    // atomic flag, same idiom as NetTask's own m_status.
    std::atomic<bool> m_visible{false};

    Guarded<FlightsModel::FlightsSnapshot> m_snapshot;
    Guarded<LogoStatus>                    m_logoStatus;
    Guarded<WantedIata>                    m_wanted;

    // Net task only: when the last kStateTopic message was accepted, and
    // whether one ever has been. HA republishes on its own schedule, so
    // silence for longer than this means the data on screen is no longer
    // trustworthy — but only once there is something to call stale.
    static constexpr uint32_t kStaleAfterMs = 120000;
    uint32_t m_lastMessageMs = 0;
    bool     m_haveMessage   = false;

    // I2: one-shot stack high-water-mark log after the first successful
    // logo download, so hardware headroom on the net task's stack is
    // visible in the log without repeating every tick.
    bool m_logoStackLogged = false;

    // One-shot latch for the "FLIGHTS_LOGO_BASE_URL is not a plain http://
    // URL" log_e in fetchLogo(): a malformed macro is a boot-time config
    // mistake, not a transient condition, so it's logged once rather than
    // on every tickLogo() retry (every kLogoRetryMs, for as long as a logo
    // is wanted).
    bool m_baseUrlLogged = false;

    // Net task only: rate limit for the transient-failure logo retry path.
    // tickLogo() runs on every net-task pass (~10 ms), so without this a
    // logo HA can't serve for a non-404 reason — a 500, a wrong
    // content-type, a refused connection — would be re-requested a hundred
    // times a second. Success and a 404 both stop retrying by other means
    // (the ready flag and the negative cache), so this only bounds that one
    // path; a change of wanted airline resets it, so cycling through
    // aircraft still fetches immediately. Takes over from the old 1.5 s
    // inter-request spacing guard, which existed for TLS/radio sharing.
    // The gate is keyed on m_lastLogoAttemptIata alone — it only suppresses
    // repeat attempts for the *same* IATA within the window; it has no
    // memory of any other airline's attempt history.
    static constexpr uint32_t kLogoRetryMs = 5000;
    uint32_t m_lastLogoAttemptMs      = 0;
    bool     m_logoAttempted          = false;
    char     m_lastLogoAttemptIata[3] = {0};

    // M2: HTTP scratch buffer for the logo body. Lazily malloc'ed/freed by
    // manageHttpBuf() (see tick()) rather than a static member, so it only
    // actually costs heap while the Flights card is visible and WiFi is up
    // (plus a kHttpBufFreeDelayMs grace window after that stops being true,
    // so flipping cards doesn't churn malloc/free). 8 KB: HA's logo PNGs
    // measure 3-5 KB, and an outlier over the cap is treated as
    // missing-for-session rather than grown into (spec review 2026-09-04).
    static constexpr size_t   kHttpBufCap = 8192;
    static constexpr uint32_t kHttpBufFreeDelayMs = 60000;
    uint8_t  *m_httpBuf = nullptr;
    uint32_t  m_httpBufIdleSinceMs = 0; // 0 = not counting down (active, or already freed)
    char      m_lastContentType[40] = {0};

    static constexpr uint8_t kMissingLogoSize = 16;
    MissingLogo   m_missingLogos[kMissingLogoSize];
    uint8_t       m_missingCount = 0;
    uint8_t       m_missingNext  = 0;
};
