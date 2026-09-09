// Must precede "DialFlightsView.h" (which pulls in M5Dial.h -> M5Unified.h ->
// M5GFX.h): M5GFX's platforms/esp32/common.hpp only compiles the
// DataWrapperT<fs::LittleFSFS> specialization the LittleFS PNG path needs when
// _LITTLEFS_H_ is already defined at the point it is processed.
#include <LittleFS.h>
#include "DialFlightsView.h"
#if HAS_DIAL_UI

#include <Arduino.h>
#include <esp_log.h>
#include <stdio.h>
#include <string.h>

#include "ClockFace.h"
#include "DialGlyphs.h"
#include "Geo.h"

// -DUSE_ESP_IDF_LOG (spec 4.14) makes log_x() expand to
// ESP_LOG_LEVEL_LOCAL(..., TAG, ...); esp32-hal-log.h has no default TAG.
static const char *TAG = "DialFlights";

namespace
{

constexpr int32_t kCentreX = 120;
constexpr int32_t kRingCx  = 120;
constexpr int32_t kRingCy  = 120;

// Flights card layout (spec 4.9), centred on the same 240x240 canvas
// (centre x=120, reusing kRingCx/kRingCy where a row sits on that centre).
constexpr int32_t kFlightsLogoX         = 60;  // (240 - 120-wide logo) / 2
constexpr int32_t kFlightsLogoW         = 120;
constexpr int32_t kFlightsLogoH         = 48;
// The top of the card is a white band (0..kFlightsBandH) so airline logos —
// transparent PNGs drawn for light backgrounds — sit on white edge to edge
// (Mike, 2026-09-06). The logo sprite is white-backed and lands inside it.
constexpr int32_t kFlightsBandH         = 58;
constexpr int32_t kFlightsLogoY         = 8;   // sprite is 48 tall -> bottom at 56, inside the band
constexpr int32_t kFlightsFallbackY     = 30;  // operatorName/callsign (dark on the white band) when there's no logo
constexpr int32_t kFlightsCallsignY     = 76;  // "callsign - type"
constexpr int32_t kFlightsRouteY        = 112; // "LHR -> JFK" / "route unknown"
constexpr int32_t kFlightsCityY         = 134; // "London -> New York" (fc/tc), when known
constexpr int32_t kFlightsAltY          = 152; // "12,000 ft - 450 kt"
constexpr int32_t kFlightsDistY         = 172; // "3.1 mi NE"
constexpr int32_t kFlightsHintY         = 214; // page dots row (one per aircraft, up to 6)
constexpr int32_t kFlightsStaleDotX     = 120;
constexpr int32_t kFlightsStaleDotY     = 63;  // just under the white band, above the callsign row
constexpr int32_t kFlightsStaleDotR     = 3;
constexpr int32_t kFlightsDotR          = 3;  // page dot radius
constexpr int32_t kFlightsDotSpacing    = 12; // centre-to-centre spacing between page dots

// Radar empty state (spec 4.13), centred on kRingCx/kRingCy.
constexpr int32_t kRadarR0        = 36;
constexpr int32_t kRadarR1        = 72;
constexpr int32_t kRadarR2        = 108; // also the sweep/wedge outer radius
constexpr int32_t kRadarDotR      = 4;   // home dot
constexpr int32_t kRadarSweepW    = 2;   // sweep line stroke width
constexpr float    kRadarDegPerStep = 12.0f; // 360/30 steps, one revolution per 3 s at 10 Hz
constexpr int32_t kRadarTextY     = 196; // "Searching", below the outer ring's bottom chord

// Formats a non-negative integer with comma thousands separators by hand
// (spec 4.9: "12,000 ft", not "12000 ft") — no locale, no String, no heap.
// Truncates safely if `out` is too small; always NUL-terminates within n.
void formatThousands(int value, char* out, size_t n)
{
    if (value < 0)
    {
        value = 0;
    }
    char digits[12];
    const int len = snprintf(digits, sizeof(digits), "%d", value);
    int outPos = 0;
    for (int i = 0; i < len && outPos < static_cast<int>(n) - 1; ++i)
    {
        if (i > 0 && (len - i) % 3 == 0)
        {
            out[outPos++] = ',';
            if (outPos >= static_cast<int>(n) - 1)
            {
                break;
            }
        }
        out[outPos++] = digits[i];
    }
    out[outPos] = '\0';
}

} // namespace

// Flights card (spec 4.9): nearest aircraft, nearest-first, cycled by the
// knob or a tap. `snap` is DialUi's own copy (written only by pollFlights()
// on the loop task) and `haveLogo` is its decision about whether the airline
// logo will be pushed after the frame — this draws what that implies, and
// reserves the logo rectangle when it will be.
void drawFlightsCard(LovyanGFX& gfx, const DialTheme& theme,
                     const FlightsModel::FlightsSnapshot& snap, uint8_t flightIdx,
                     bool haveLogo, uint8_t radarPhase)
{
    if (snap.stale)
    {
        gfx.fillCircle(kFlightsStaleDotX, kFlightsStaleDotY, kFlightsStaleDotR, theme.col(Col::DIM));
    }

    if (snap.offline)
    {
        gfx.setTextColor(theme.col(Col::TEXT), theme.col(Col::BG));
        gfx.drawString("waiting for HA", kRingCx, kRingCy, &fonts::Font4);
        return;
    }

    if (snap.count == 0)
    {
        // Spec 4.13 (2026-09-06): a radar sweep "looking for aircraft"
        // instead of the old static "no aircraft nearby" caption. The stale
        // dot above is unchanged — a stale empty list still shows the radar.
        drawRadarSweep(gfx, theme, radarPhase);
        return;
    }

    const uint8_t idx = (flightIdx < snap.count) ? flightIdx : 0;
    const FlightsModel::Aircraft& ac = snap.ac[idx];

    // Logo, else operatorName, else callsign (spec 4.9). Only (re)decode the
    // sprite when the current aircraft's airline differs from what's
    // resident and FlightsService actually has that logo ready — decoding is
    // a real PNG parse, not something to redo every frame. M1 (spec review
    // 2026-09-04): the logo is decoded straight off LittleFS — no PNG byte
    // buffer lives in either DialUi or FlightsService any more. LittleFS
    // reads are safe from the loop task (LittleFS is internally locked).
    // Decode the PNG straight from LittleFS onto this frame (a ~6 KB PNG at
    // <= 4 Hz is cheap on the S3); remember decode failures per session so a
    // bad file is not retried every frame. LittleFS reads are safe from the
    // loop task (internally locked).
    // The canvas is a 16-colour palette, which would posterise the PNG, so
    // the logo is drawn straight onto M5Dial.Display in render() after the
    // frame is pushed. Here we only decide whether there is a logo to draw.
    // White band across the top of the card (the round bezel clips the
    // corners). With a logo, the logo rectangle inside it is reserved as
    // TRANSPARENT so the white-backed sprite pushed in render() shows through
    // seamlessly; without one, the operator name is drawn dark on the band.
    gfx.fillRect(0, 0, 240, kFlightsBandH, theme.col(Col::TEXT));
    if (haveLogo)
    {
        // Reserve the logo rectangle: on the canvas the TRANSPARENT index keeps
        // the display's logo pixels through the push (see DialUi::render()).
        gfx.fillRect(kFlightsLogoX, kFlightsLogoY, 120, 48,
                     theme.useCanvas ? static_cast<uint32_t>(Col::TRANSPARENT) : theme.col(Col::BG));
    }
    else
    {
        gfx.setTextColor(theme.col(Col::BG), theme.col(Col::TEXT));
        const char* fallback = (ac.operatorKnown && ac.operatorName[0] != '\0') ? ac.operatorName : ac.callsign;
        gfx.drawString(fallback, kCentreX, kFlightsFallbackY, &fonts::Font2);
    }

    // "BA123 - A320": the flight number is what a passenger (or a plane
    // spotter with a phone) would recognise, so it wins over the raw ATC
    // callsign whenever HA has resolved one; "BAW123 - A320" is the
    // fallback when it hasn't (spec 4.11, final review 2026-09-05).
    const char* headline = (ac.flightNumber[0] != '\0') ? ac.flightNumber : ac.callsign;
    char line1[32];
    if (ac.type[0] != '\0')
    {
        snprintf(line1, sizeof(line1), "%s - %s", headline, ac.type);
    }
    else
    {
        snprintf(line1, sizeof(line1), "%s", headline);
    }
    gfx.setTextColor(theme.col(Col::TEXT), theme.col(Col::BG));
    gfx.drawString(line1, kCentreX, kFlightsCallsignY, &fonts::Font2);

    // Route, large: "LHR -> JFK" (ASCII arrow — Font4 may lack the real one)
    // or "route unknown" when not yet enriched.
    if (ac.routeKnown && ac.fromIata[0] != '\0' && ac.toIata[0] != '\0')
    {
        char routeBuf[16];
        snprintf(routeBuf, sizeof(routeBuf), "%s -> %s", ac.fromIata, ac.toIata);
        gfx.setTextColor(theme.col(Col::TEXT), theme.col(Col::BG));
        gfx.drawString(routeBuf, kCentreX, kFlightsRouteY, &fonts::Font4);
    }
    else
    {
        gfx.setTextColor(theme.col(Col::DIM), theme.col(Col::BG));
        gfx.drawString("route unknown", kCentreX, kFlightsRouteY, &fonts::Font2);
    }

    // City names, small, under the route line: "London -> New York" when
    // both ends are known, just the known one when only one is, nothing
    // when HA hasn't got either yet (fc/tc, spec 4.11 amendment).
    if (ac.fromCity[0] != '\0' || ac.toCity[0] != '\0')
    {
        char cityBuf[12 + 4 + 12 + 1];
        if (ac.fromCity[0] != '\0' && ac.toCity[0] != '\0')
        {
            snprintf(cityBuf, sizeof(cityBuf), "%s -> %s", ac.fromCity, ac.toCity);
        }
        else if (ac.fromCity[0] != '\0')
        {
            snprintf(cityBuf, sizeof(cityBuf), "%s", ac.fromCity);
        }
        else
        {
            snprintf(cityBuf, sizeof(cityBuf), "%s", ac.toCity);
        }
        gfx.setTextColor(theme.col(Col::DIM), theme.col(Col::BG));
        gfx.drawString(cityBuf, kCentreX, kFlightsCityY, &fonts::Font2);
    }

    // Altitude/speed: "12,000 ft - 450 kt", or "on ground" for an aircraft
    // HA flagged with gnd — "0 ft - 0 kt" reads like missing data rather
    // than a taxiing aircraft (final review 2026-09-05).
    char altSpeedBuf[32];
    if (ac.onGround)
    {
        snprintf(altSpeedBuf, sizeof(altSpeedBuf), "on ground");
    }
    else
    {
        char altBuf[12];
        formatThousands(ac.altFt, altBuf, sizeof(altBuf));
        snprintf(altSpeedBuf, sizeof(altSpeedBuf), "%s ft - %d kt", altBuf, ac.gsKt);
    }
    gfx.setTextColor(theme.col(Col::TEXT), theme.col(Col::BG));
    gfx.drawString(altSpeedBuf, kCentreX, kFlightsAltY, &fonts::Font2);

    // Distance/compass: "3.1 mi NE" (no index suffix — the page dots below
    // show position in the list instead).
    char distBuf[32];
    snprintf(distBuf, sizeof(distBuf), "%.1f mi %s", static_cast<double>(ac.distMi),
             Geo::compass8(static_cast<float>(ac.bearing)));
    gfx.setTextColor(theme.col(Col::TEXT), theme.col(Col::BG));
    gfx.drawString(distBuf, kCentreX, kFlightsDistY, &fonts::Font2);

    // Page dots: one per aircraft (up to 6, the list is never larger),
    // centred on kFlightsHintY — filled for the one currently shown, hollow
    // outlines for the rest. Replaces the old "tap: next" hint text.
    const int32_t dotsW = static_cast<int32_t>(snap.count - 1) * kFlightsDotSpacing;
    const int32_t firstDotX = kCentreX - dotsW / 2;
    for (uint8_t i = 0; i < snap.count; ++i)
    {
        const int32_t dotX = firstDotX + static_cast<int32_t>(i) * kFlightsDotSpacing;
        if (i == idx)
        {
            gfx.fillCircle(dotX, kFlightsHintY, kFlightsDotR, theme.col(Col::TEXT));
        }
        else
        {
            gfx.drawCircle(dotX, kFlightsHintY, kFlightsDotR, theme.col(Col::DIM));
        }
    }
}

// Radar "Searching" empty state (spec 4.13). `radarPhase` is 0..29 (12 deg
// per step, one revolution every 3 s at the 10 Hz this is redrawn while
// visible — see FrameKey::radarPhase in DialUi.cpp). Draw order matters:
// wedges first so the rings/crosshair lines stay visible on top of them,
// then the sweep line, then the centre dot, then the caption.
void drawRadarSweep(LovyanGFX& gfx, const DialTheme& theme, uint8_t radarPhase)
{
    // ClockFace::pointAt's angle convention (0 deg = 12 o'clock, clockwise)
    // is what the spec's "phase * 12 deg clockwise from 12 o'clock" means;
    // fillArc wants LovyanGFX's own convention (0 deg = 3 o'clock, clockwise),
    // so the wedges use `a` shifted by -90 instead.
    const float sweepDeg = static_cast<float>(radarPhase) * kRadarDegPerStep;
    const float a        = sweepDeg - 90.0f;

    // Trailing wedges behind the sweep line, faking phosphor persistence:
    // the 12 deg right behind the line brighter (SPEED_DIM), the next 12 deg
    // dimmer (DIM_DIM).
    gfx.fillArc(kRingCx, kRingCy, kRadarR2, 0, a - kRadarDegPerStep, a, theme.col(Col::SPEED_DIM));
    gfx.fillArc(kRingCx, kRingCy, kRadarR2, 0, a - 2 * kRadarDegPerStep, a - kRadarDegPerStep,
                theme.col(Col::DIM_DIM));

    // Range rings and crosshair, drawn over the wedges so they stay visible.
    gfx.drawCircle(kRingCx, kRingCy, kRadarR0, theme.col(Col::DIM_DIM));
    gfx.drawCircle(kRingCx, kRingCy, kRadarR1, theme.col(Col::DIM_DIM));
    gfx.drawCircle(kRingCx, kRingCy, kRadarR2, theme.col(Col::DIM_DIM));
    gfx.drawLine(kRingCx - kRadarR2, kRingCy, kRingCx + kRadarR2, kRingCy, theme.col(Col::DIM_DIM));
    gfx.drawLine(kRingCx, kRingCy - kRadarR2, kRingCx, kRingCy + kRadarR2, theme.col(Col::DIM_DIM));

    // Sweep line, centre to the outer ring, on top of the face.
    int sx, sy;
    ClockFace::pointAt(sweepDeg, kRingCx, kRingCy, kRadarR2, sx, sy);
    dialThickLine(gfx, kRingCx, kRingCy, sx, sy, kRadarSweepW, theme.col(Col::NET_ON));

    // Home dot, on top of the sweep line where it crosses the centre.
    gfx.fillCircle(kRingCx, kRingCy, kRadarDotR, theme.col(Col::NET_ON));

    gfx.setTextColor(theme.col(Col::DIM), theme.col(Col::BG));
    gfx.drawString("Searching", kCentreX, kRadarTextY, &fonts::Font2);
}

// Airline logos from pics.avs.io are transparent PNGs drawn for light
// backgrounds, so the sprite is white-backed — the same white as the card's
// top band that drawFlightsCard() lays down around it. Decoding straight onto
// the display was found to hold ~43 KB of heap afterwards, and so was the
// drawPngFile(fs, path) wrapper, so the file (<= 8 KB) is read here and
// decoded from memory into a sprite that is freed immediately (2026-09-04).
bool dialDrawAirlineLogo(const char* iata)
{
    // The relay serves each logo as raw big-endian RGB565, 120 x 48, already
    // composited on white (spec 4.11 amendment 2026-09-09), so this is a
    // straight copy from LittleFS to the panel one row at a time from a
    // 240-byte stack buffer: no PNG decoder, no sprite, no heap at all.
    // The panel's native byte order is big-endian 565 too, so the pixels go
    // over SPI untouched.
    char path[24];
    snprintf(path, sizeof(path), "/logos/%s.565", iata);

    File f = LittleFS.open(path, "r");
    const size_t fsz = f ? (size_t)f.size() : 0;
    constexpr size_t kRowBytes = kFlightsLogoW * 2;
    constexpr size_t kLogoBytes = kRowBytes * kFlightsLogoH;
    bool ok = (fsz == kLogoBytes);
    if (ok)
    {
        lgfx::swap565_t row[kFlightsLogoW];
        M5Dial.Display.startWrite();
        for (int32_t y = 0; y < kFlightsLogoH; y++)
        {
            if (f.read(reinterpret_cast<uint8_t*>(row), kRowBytes) != kRowBytes)
            {
                ok = false;
                break;
            }
            M5Dial.Display.pushImage(kFlightsLogoX, kFlightsLogoY + y, kFlightsLogoW, 1, row);
        }
        M5Dial.Display.endWrite();
    }
    if (f) f.close();
    log_i("DialFlightsView: logo %s ok=%d file=%u", iata, (int)ok, (unsigned)fsz);
    return ok;
}

#endif // HAS_DIAL_UI
