// Must precede "DialUi.h" (which pulls in M5Dial.h -> M5Unified.h -> M5GFX.h):
#include <math.h>
// M5GFX's platforms/esp32/common.hpp only compiles in the
// DataWrapperT<fs::LittleFSFS> specialization that drawPngFile(LittleFS, ...)
// needs (M1) when _LITTLEFS_H_ is already defined at the point it's
// processed — LittleFS.h has to be included before that chain, not after it.
#include <LittleFS.h>
#include "DialUi.h"
#if HAS_DIAL_UI

#include <Arduino.h>
#include <esp_log.h>
#include <string.h>
#include <time.h>

#include "DialFormat.h"
#include "TreadmillData.h"
#include "ClockFace.h"
#include "Geo.h"

namespace {
// Thick line without alpha blending: LovyanGFX's anti-aliased wide-line
// routine reads pixels back through the palette, which faults on a 4-bit
// palette sprite (LoadProhibited in copy_palette_affine, 2026-09-04). Draws
// `widthPx` parallel 1-px lines offset along the perpendicular instead.
void thickLine(LovyanGFX& gfx, int x0, int y0, int x1, int y1, int widthPx, uint32_t color)
{
    if (widthPx <= 1) { gfx.drawLine(x0, y0, x1, y1, color); return; }
    const float dx = (float)(x1 - x0), dy = (float)(y1 - y0);
    const float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.5f) { gfx.fillCircle(x0, y0, widthPx / 2, color); return; }
    const float px = -dy / len, py = dx / len; // unit perpendicular
    const float half = (widthPx - 1) / 2.0f;
    for (int i = 0; i < widthPx; ++i)
    {
        const float o = -half + i;
        gfx.drawLine((int)lroundf(x0 + px * o), (int)lroundf(y0 + py * o),
                     (int)lroundf(x1 + px * o), (int)lroundf(y1 + py * o), color);
    }
}

// Three 8px status dots along the top: BLE, WiFi, MQTT, left to right.
constexpr int32_t kDotY      = 14;
constexpr int32_t kDotRadius = 4; // 8px diameter
constexpr int32_t kBleDotX   = 96;
constexpr int32_t kWifiDotX  = 120;
constexpr int32_t kMqttDotX  = 144;

// Palette source values (2026-09-04): the 16 RGB888 colours behind DialUi::Col
// (declared in DialUi.h), in Col enum order — index i is the palette entry
// for Col value i. Each was the original RGB565 constant used before the
// canvas became a 4bpp palette sprite, expanded 565->888 the same way the
// display itself would (top bits replicated into the low bits), so the
// on-screen colour is unchanged:
//   BG          TFT_BLACK    0x0000 -> (  0,   0,   0)
//   TEXT        TFT_WHITE    0xFFFF -> (255, 255, 255)
//   DIM         TFT_DARKGREY 0x7BEF -> (123, 125, 123) -- also TFT_darkgrey's
//               365 "TICK" use, and dimColor565(TFT_WHITE) == TFT_DARKGREY
//               exactly (both decode to r=15,g=31,b=15 in 565 components),
//               so this same entry doubles as TEXT's paused-dim shade.
//   BLE_ON      TFT_BLUE     0x001F -> (  0,   0, 255)
//   NET_ON      TFT_GREEN    0x07E0 -> (  0, 255,   0)
//   SPEED       TFT_CYAN     0x07FF -> (  0, 255, 255)
//   PENDING     TFT_ORANGE   0xFDA0 -> (255, 182,   0)
//   RED         TFT_RED      0xF800 -> (255,   0,   0)
//   SECOND      color565(220,40,40) quantised to 565 then back -> (222,40,41)
//   DIM_DIM     dimColor565(TFT_DARKGREY)                      -> ( 57,  60,  57)
//   SPEED_DIM   dimColor565(TFT_CYAN)                          -> (  0, 125, 123)
//   PENDING_DIM dimColor565(TFT_ORANGE)                        -> (123,  89,   0)
// dimColor565() (the halving of each 565 channel used for the paused-state
// dim, formerly a runtime helper) is folded into these constants instead —
// there is nothing left to dim at draw time once the palette holds the
// dimmed shades directly.
static const uint32_t kPaletteRgb888[12] = {
    0x000000, // BG
    0xFFFFFF, // TEXT
    0x7B7D7B, // DIM
    0x0000FF, // BLE_ON
    0x00FF00, // NET_ON
    0x00FFFF, // SPEED
    0xFFB600, // PENDING
    0xFF0000, // RED
    0xDE2829, // SECOND
    0x393C39, // DIM_DIM
    0x007D7B, // SPEED_DIM
    0x7B5900, // PENDING_DIM
};

// Clock card layout (spec 4.8), centred on the same 240x240 canvas as the
// speed ring.
constexpr int32_t kClockR0        = 104; // tick inner radius
constexpr int32_t kClockR1        = 112; // tick outer radius (r0=100 at 12/3/6/9, longer ticks)
constexpr int32_t kClockR0Long    = 100;
constexpr int32_t kHourHandLen    = 56;
constexpr int32_t kMinuteHandLen  = 82;
constexpr int32_t kSecondHandLen  = 96;
constexpr float    kHourHandW      = 5.0f;
constexpr float    kMinuteHandW    = 3.0f;
constexpr float    kSecondHandW    = 1.0f;
constexpr int32_t kClockCentreDotR = 4;
constexpr int32_t kClockSecondDotR = 2;
constexpr int32_t kClockDateY      = 168; // date when valid, "waiting for time" hint when not

// Flights card layout (spec 4.9), centred on the same 240x240 canvas
// (centre x=120, reusing kRingCx/kRingCy where a row sits on that centre).
constexpr int32_t kFlightsLogoX         = 60;  // (240 - 120-wide sprite) / 2
constexpr int32_t kFlightsLogoY         = 16;  // sprite is 48 tall -> bottom at 64
constexpr int32_t kFlightsFallbackY     = 40;  // operatorName/callsign when there's no logo
constexpr int32_t kFlightsCallsignY     = 76;  // "callsign - type"
constexpr int32_t kFlightsRouteY        = 112; // "LHR -> JFK" / "route unknown"
constexpr int32_t kFlightsCityY         = 134; // "London -> New York" (fc/tc), when known
constexpr int32_t kFlightsAltY          = 152; // "12,000 ft - 450 kt"
constexpr int32_t kFlightsDistY         = 172; // "3.1 mi NE"
constexpr int32_t kFlightsHintY         = 214; // page dots row (one per aircraft, up to 6)
constexpr int32_t kFlightsEmptyCaptionY = 150; // "within N mi" under "no aircraft nearby"
constexpr int32_t kFlightsStaleDotX     = 120;
constexpr int32_t kFlightsStaleDotY     = 14;
constexpr int32_t kFlightsStaleDotR     = 3;
constexpr int32_t kFlightsDotR          = 3;  // page dot radius
constexpr int32_t kFlightsDotSpacing    = 12; // centre-to-centre spacing between page dots

// Lights card layout (Plan 6, spec 4.10) on the same 240x240 canvas: the
// title sits above the centred value/state, the caption just under it, and
// the on-screen buttons come from LightButtons::geom() (which is sized to
// clear the value ring's 108-118 track). The big value reuses the ring
// centre rather than kCentreY so it sits inside the ring, not above it.
constexpr int32_t kLightTitleY   = 40;
constexpr int32_t kLightCaptionY = 160;
// Gap between the Font7 brightness digits and the Font4 "%" drawn beside
// them: Font7 is a 7-segment font whose only glyphs are space, 0-9, ':',
// '.' and '-' (see Font7srle.h), so a '%' inside a Font7 string would print
// as a blank 12px cell. The digits and the percent sign are therefore drawn
// as two strings in two fonts, together centred on kRingCx.
constexpr int32_t kLightPctGap   = 4;

// Speed ring geometry, centred on the 240x240 canvas.
constexpr int32_t kRingCx    = 120;
constexpr int32_t kRingCy    = 120;
constexpr int32_t kRingOuter = 118;
constexpr int32_t kRingInner = 108;
constexpr float    kRingStartDeg = 120.0f; // 3 o'clock = 0 deg, clockwise; gap sits at the bottom (6 o'clock)
constexpr float    kRingSweepDeg = 300.0f;

// Long-press-to-stop progress: a thin arc just outside the speed ring.
constexpr int32_t kHoldOuter = 120;
constexpr int32_t kHoldInner = 116;

// Running/paused screen layout (all text uses middle_center datum).
constexpr int32_t kCentreX         = 120;
constexpr int32_t kCentreY         = 100; // big Font7: elapsed time, or the overlaid target speed
constexpr int32_t kOverlayCaptionY = 136; // "mph" caption under the overlaid speed
constexpr int32_t kPausedY         = 72;  // pulsing "PAUSED" label
constexpr int32_t kRowY            = 158; // distance (left) / steps (right)
constexpr int32_t kRowCaptionY     = 174;
constexpr int32_t kRowLeftX        = 72;
constexpr int32_t kRowRightX       = 168;
constexpr int32_t kSpeedReadoutY   = 196;
constexpr int32_t kHintY           = 222; // "tap resume - hold stop"

// Disconnected screen ("LAST SESSION" summary) — row reuses kRowLeftX/kRowRightX.
constexpr int32_t kDiscTitleY   = 60;
constexpr int32_t kDiscTimeY    = 96;
constexpr int32_t kDiscRowY     = 140;
constexpr int32_t kDiscCaptionY = 156;
constexpr int32_t kDiscHintY    = 210;

// Connecting screen.
constexpr int32_t kConnLabelY   = 96;
constexpr int32_t kConnAttemptY = 128;
constexpr int32_t kConnBeepY    = 160;
constexpr int32_t kConnHintY    = 210;

// Starting (COUNTDOWN) screen.
constexpr int32_t kStartLabelY = 96;
constexpr int32_t kStartHintY  = 210;

// Selector (start-speed picker) screen. Value uses kCentreY/kCentreX (same
// centre spot the running/overlay screens draw their big Font7 value at).
constexpr int32_t kSelectorTitleY = 60;
constexpr int32_t kSelectorMphY   = 140;
constexpr int32_t kSelectorHint1Y = 196;
constexpr int32_t kSelectorHint2Y = 214;

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

DialUi::DialUi(TreadmillController& controller, const TimeService& timeService,
               NetTask& net)
    : m_controller(controller), m_time(timeService), m_flights(net.flights()),
      m_net(net), m_lights(net.lights()),
      // Office has no colour control; the Lamp does (Plan 6). Indexed by
      // LightsModel::LightKey, so the order here is OFFICE then LAMP.
      m_lightCards{LightCardState(false), LightCardState(true)}
{
}

// See the DialUi::Col declaration in DialUi.h and the kPaletteRgb888 comment
// above for what each entry means. How LovyanGFX interprets the value
// returned here, for a 4bpp palette destination (m_useCanvas true):
//  - fillCircle/fillArc/drawCircle/drawString/fillScreen/... all route their
//    colour argument through LGFXBase::setColor() -> _write_conv.convert(T)
//    (lgfx/v1/LGFXBase.hpp, the `setColor`/`LGFX_INLINE_T` draw-call
//    forwarders). For T=uint32_t, color_conv_t::convert() calls
//    convert_rgb888(), a function pointer selected by
//    get_fp_convert_src<rgb888_t>(dst_depth) (lgfx/v1/misc/colortype.hpp,
//    get_fp_convert_src): a 4-bit *palette* dst_depth matches none of that
//    function's named pixel formats, so it falls through to
//    `switch (dst_depth & bit_mask) { case 4: return
//    convert_uint32_to_palette4; ... }`, and
//    `convert_uint32_to_palette4(uint32_t c) { return (c & 0x0F) * 0x11; }`
//    (colortype.hpp, near the top) takes the low nibble of whatever integer
//    was passed as the raw index — not an RGB value.
//  - setTextColor() makes this explicit rather than routing through convert():
//    `_text_style.fore_rgb888 = _text_style.back_rgb888 = this->hasPalette()
//    ? color : convert_to_rgb888(color);` (LGFXBase.hpp, setTextColor).
//  - drawWideLine()/drawWedgeLine() (used for the clock hands/ticks) instead
//    call `convert_to_rgb888(color)` and hand that to draw_wedgeline(), which
//    builds a one-pixel rgb888_t gradient and pushes it through the same
//    paletted pixelcopy path (misc/pixelcopy.cpp, the `dst_palette_` branch
//    of pixelcopy_t's constructor uses `copy_bit_affine`, which reads the
//    *first* in-memory byte of that rgb888_t). rgb888_t's field order is
//    `{ uint8_t b; uint8_t g; uint8_t r; }` (colortype.hpp, struct rgb888_t)
//    — for a small integer built via `rgb888_t(uint32_t)`, that first byte
//    is the low byte of the value that was passed, i.e. the same raw index
//    survives this longer path too.
// createPalette()/setPaletteColor() (LGFX_Sprite.hpp), by contrast, take a
// real RGB888/RGB565 value and store it in the palette table — that's why
// begin() below passes kPaletteRgb888[i] there rather than the index.
// Net effect: every draw call in this file can be handed col(Col::X) as its
// colour argument, uniformly, on either destination.
uint32_t DialUi::col(Col c) const
{
    const uint8_t idx = static_cast<uint8_t>(c);
    return m_useCanvas ? idx : kPaletteRgb888[idx];
}

void DialUi::begin()
{
    // M5Unified owns display/I2C/Serial init on the Dial — this must be the
    // first thing setup() does.
    auto cfg = M5.config();
    cfg.serial_baudrate = 115200;
    cfg.internal_imu = false;
    cfg.internal_rtc = true; // BM8563 RTC — TimeService reads it at boot, writes it after NTP sync
    M5Dial.begin(cfg, true, false);

    M5Dial.Display.setTextDatum(middle_center);
    M5Dial.Display.fillScreen(col(Col::BG));
    M5Dial.Display.setBrightness(kBrightFull);

    log_i("DialUi: free heap before sprite = %u bytes", (unsigned)ESP.getFreeHeap());
    // 4-bit (16-colour) palette: an 8th of the 8bpp canvas this replaced
    // (~28.8 KB instead of ~57.6 KB for the 240x240 frame) so WiFi TX
    // buffers have more heap to work with (2026-09-04).
    m_canvas.setColorDepth(4);
    m_useCanvas = m_canvas.createSprite(240, 240);
    if (m_useCanvas)
    {
        m_canvas.createPalette();
        for (uint8_t i = 0; i < 12; ++i)
        {
            m_canvas.setPaletteColor(i, kPaletteRgb888[i]);
        }
        m_canvas.setPaletteColor(static_cast<uint8_t>(Col::TRANSPARENT), 0x000000u);
    }
    log_i("DialUi: free heap after sprite = %u bytes", (unsigned)ESP.getFreeHeap());

    if (!m_useCanvas)
    {
        log_e("DialUi: canvas createSprite(240,240) failed, drawing directly to M5Dial.Display");
    }
    else
    {
        m_canvas.setTextDatum(middle_center);
    }

    // Flights card logos are decoded straight from LittleFS onto the frame
    // being drawn (no resident logo sprite): on the S3 without PSRAM the
    // extra 11.5 KB of heap starved the WiFi driver's TX buffers (2026-09-04).
    // A palette canvas would also posterise a PNG's full-colour pixels down
    // to 16 shades, so logos are never decoded into m_canvas at all — see
    // drawFlights()/render() for the direct-to-M5Dial.Display draw instead.
}

void DialUi::tick(uint32_t nowMs)
{
    // Pumps M5Dial's own touch/button/encoder debouncing, then feeds those
    // readings into DialInput and acts on whatever intents come back. Runs
    // every call, independent of the render throttle below.
    M5Dial.update();
    handleInput(nowMs);

    // Flights snapshot (spec 4.11): pulled on every tick, whatever is on
    // screen — the auto-show state machine below needs the aircraft count
    // while the Clock card is showing, not only while the Flights card is.
    // Still rate-limited to kFlightsSnapIntervalMs inside pollFlights().
    pollFlights(nowMs);

    Screen screenNow = currentScreen(isPausedState());

    // Auto-show (spec 4.11): the Flights card interrupts the Clock card
    // while aircraft are nearby and hands control back when they are gone.
    // Only meaningful while the belt is idle — i.e. the resolved screen is
    // one of the desk cards rather than Connecting/Starting/Running/Selector
    // — which is exactly what FlightsAutoShow's beltIdle argument means.
    {
        const bool beltIdle =
            (screenNow == Screen::DISCONNECTED || screenNow == Screen::CLOCK ||
             screenNow == Screen::FLIGHTS || screenNow == Screen::LIGHT_OFFICE ||
             screenNow == Screen::LIGHT_LAMP);
        // The count is only evidence while the snapshot is actually live:
        // stale (HA quiet) or offline (MQTT down) data is passed as
        // dataValid=false, which the state machine treats as zero aircraft.
        const bool dataValid = !(m_flightsSnap.offline || m_flightsSnap.stale);
        const FlightsAutoShow::Action action =
            m_autoShow.update(m_flightsSnap.count, m_cards.current(), beltIdle, dataValid);
        if (action == FlightsAutoShow::Action::SHOW_FLIGHTS)
        {
            m_cards.set(CardId::FLIGHTS);
            m_flightIdx = 0;
            // Nobody touched the Dial — wake the backlight ourselves so the
            // card that just raised itself is actually readable. Brightness
            // follows on the same tick rather than waiting for the next
            // handleInput().
            m_input.noteActivity(nowMs);
            applyBrightness();
        }
        else if (action == FlightsAutoShow::Action::RETURN_TO_CLOCK)
        {
            m_cards.set(CardId::CLOCK);
        }
        if (action != FlightsAutoShow::Action::NONE)
        {
            // The ring moved: re-resolve so the visibility/housekeeping below
            // and render() further down all act on the new card this tick.
            screenNow = currentScreen(isPausedState());
        }
    }

    // Flights card (spec 4.9): tell FlightsService whether the card is on
    // screen every tick (cheap atomic write), then run the visible card's
    // own housekeeping (idx clamp, wanted logo) while it is.
    m_flights.setVisible(screenNow == Screen::FLIGHTS);
    if (screenNow == Screen::FLIGHTS)
    {
        tickFlights();
    }

    // Lights cards (Plan 6): same shape as the flights housekeeping above —
    // pull HA state at most every kLightsSnapIntervalMs and let the visible
    // card's settle/idle timers run.
    if (screenNow == Screen::LIGHT_OFFICE || screenNow == Screen::LIGHT_LAMP)
    {
        tickLights(nowMs);
    }
    else
    {
        // Some other screen (CONNECTING/STARTING/RUNNING/SELECTOR/another
        // card) has taken over: drop any engagement on both light cards so a
        // pending settle can't fire a stale command long after the card left
        // the screen. releaseLightCards() is cheap and a no-op when nothing
        // is engaged/settling, so paying for it on every non-light tick is
        // fine.
        releaseLightCards();
    }

    if (nowMs - m_lastRenderMs < kRenderIntervalMs)
    {
        return;
    }
    m_lastRenderMs = nowMs;
    render(nowMs);
}

void DialUi::handleInput(uint32_t nowMs)
{
    const long encoderCount = M5Dial.Encoder.read();
    const auto touch = M5Dial.Touch.getDetail();
    // BtnA wraps the Dial's single side button; wasClicked() is a one-tick
    // edge (auto-clears), which is what DialInput's btnClicked parameter
    // expects.
    const bool btnClicked = M5Dial.BtnA.wasClicked();

    const DialEvents ev = m_input.tick(encoderCount, touch.isPressed(), touch.x, touch.y,
                                        btnClicked, nowMs);
    m_holdProgress = ev.holdProgress;

    // Keep the screen awake through an active walk even with no touch/encoder
    // input at all.
    if (m_snapshot.status == TreadMillData::RUNNING || m_snapshot.status == TreadMillData::COUNTDOWN)
    {
        m_input.noteActivity(nowMs);
    }

    applyBrightness();

    // Selector housekeeping (spec 4.7): Connecting/Starting/Running(paused)
    // always win over an open selector, so close it as soon as any of those
    // becomes true — silently, no beep — before resolving the screen below.
    // Otherwise a stale selector (opened before a start elsewhere, e.g. HA)
    // could resurface once the belt stops again.
    if (m_selector.isOpen() &&
        (m_snapshot.status == TreadMillData::RUNNING || isPausedState() ||
         m_snapshot.status == TreadMillData::COUNTDOWN || m_controller.isConnecting()))
    {
        m_selector.close();
    }
    // Inactivity timeout: returns true (and closes) exactly once on the tick
    // that crosses SELECTOR_TIMEOUT_MS. The close is silent (no beep), so
    // the return value isn't needed here.
    m_selector.tick(nowMs);

    const Screen screen = currentScreen(isPausedState());

    if (ev.tap)
    {
        // The Connecting screen has no running belt to toggle — tap cancels
        // the in-flight connect there, same gesture as hold (C1).
        if (screen == Screen::CONNECTING)
        {
            const bool cancelled = m_controller.requestDisconnect();
            playStopBeep(nowMs, cancelled);
        }
        else if (screen == Screen::SELECTOR)
        {
            // Confirm the candidate speed and start.
            m_controller.startAt(m_selector.value());
            m_selector.close();
            playAcceptBeep(true);
        }
        else if (screen == Screen::DISCONNECTED)
        {
            // Open the picker at the configured start speed rather than
            // starting immediately (4.7); this is a pure local UI action, so
            // it always succeeds.
            m_selector.open(m_controller.startSpeedMph(), nowMs);
            playAcceptBeep(true);
        }
        else if (screen == Screen::CLOCK)
        {
            // The Clock card owns no value and starts nothing — tap is a
            // no-op, no beep (spec 4.8).
        }
        else if (screen == Screen::FLIGHTS)
        {
            // Cycle to the next aircraft (spec 4.9); wraps at the current
            // count, or stays at 0 when the list is empty.
            const uint8_t count = m_flightsSnap.count;
            m_flightIdx = static_cast<uint8_t>((m_flightIdx + 1) % (count > 0 ? count : 1));
            playAcceptBeep(true);
        }
        else if (screen == Screen::LIGHT_OFFICE || screen == Screen::LIGHT_LAMP)
        {
            // Lights card (Plan 6): only the on-screen buttons are live —
            // a tap on bare card background is a no-op with no beep, the
            // same as the Clock card's tap.
            const LightsModel::LightKey lightKey = (screen == Screen::LIGHT_LAMP)
                                                       ? LightsModel::LightKey::LAMP
                                                       : LightsModel::LightKey::OFFICE;
            LightCardState& card = lightCardFor(screen);
            const LightButtons::Button b =
                LightButtons::hitTest(ev.tapX, ev.tapY, card.hasColour());
            // MQTT down ("waiting for HA") means every button is drawn inert
            // (drawLightButtons() gates all three on mqttUp), and there is no
            // path for a command to reach HA anyway — so a hit on one is a
            // no-op with no beep, exactly like a tap on bare background.
            if (b != LightButtons::Button::NONE && m_netStatus == NetStatus::MQTT_UP)
            {
                // LightCardState silently ignores a tap it can't act on
                // (no data yet, colour on a light that has none), and
                // returns a command only for POWER or a flushed settle —
                // so "did something" is an engagement change or a command.
                const LightCardState::Engaged engagedBefore = card.engaged();
                const LightsModel::Command cmd = card.tapButton(b, nowMs);
                const bool acted = (card.engaged() != engagedBefore) ||
                                   (cmd.type != LightsModel::Command::Type::NONE);
                playAcceptBeep(acted);
                if (cmd.type != LightsModel::Command::Type::NONE)
                {
                    publishLightCommand(lightKey, cmd);
                }
            }
        }
        else
        {
            // The controller notifies observers (including this one, via
            // onSnapshot/onTargetSpeed) synchronously, so m_snapshot already
            // reflects the outcome by the time toggleStartPause() returns.
            const TreadMillData::Status statusBefore = m_snapshot.status;
            const float speedBefore = m_snapshot.speedCmd;
            m_controller.toggleStartPause();
            const bool refused = (m_snapshot.status == statusBefore) && (m_snapshot.speedCmd == speedBefore);
            playAcceptBeep(!refused);
        }
    }

    if (ev.longPress)
    {
        // The Connecting screen's hold gesture cancels the in-flight connect
        // rather than stopping a belt that isn't running yet.
        if (screen == Screen::CONNECTING)
        {
            const bool cancelled = m_controller.requestDisconnect();
            playStopBeep(nowMs, cancelled);
        }
        else if (screen == Screen::SELECTOR)
        {
            // Skip the candidate and start at the configured default.
            m_controller.start();
            m_selector.close();
            playAcceptBeep(true);
        }
        else if (screen == Screen::DISCONNECTED)
        {
            // start() returns void — tell accepted from refused the same way
            // the tap path does above: compare the snapshot status start()
            // published synchronously (COUNTDOWN on success) against before.
            const TreadMillData::Status statusBefore = m_snapshot.status;
            m_controller.start();
            const bool accepted = (m_snapshot.status != statusBefore);
            playAcceptBeep(accepted);
        }
        else if (screen == Screen::CLOCK || screen == Screen::FLIGHTS ||
                 screen == Screen::LIGHT_OFFICE || screen == Screen::LIGHT_LAMP)
        {
            // None of the desk cards owns a value to skip/confirm — hold is a
            // no-op, no beep (spec 4.8, spec 4.9, Plan 6).
        }
        else
        {
            const bool stopped = m_controller.stop();
            playStopBeep(nowMs, stopped);
        }
    }

    if (ev.btnStop)
    {
        if (screen == Screen::RUNNING || screen == Screen::STARTING || screen == Screen::CONNECTING)
        {
            // Emergency stop: the side button sends stop() while the belt is
            // running/paused/counting down, and unchanged on Connecting —
            // it does NOT cancel the connect there (only tap/hold do); the
            // write is simply refused because the link is down, and that
            // plays the refused tone via the same feedback path (I1).
            const bool stopped = m_controller.stop();
            playStopBeep(nowMs, stopped);
        }
        else
        {
            // Home: side button always returns to the Treadmill card (spec
            // 4.8), closing the selector first if it was open. Nothing is
            // running yet on any of these screens (Disconnected/Clock/
            // Selector), so there's no belt command to send.
            if (m_selector.isOpen())
            {
                m_selector.close();
            }
            // Leaving whatever card was showing: a light card parked
            // mid-edit must not come back engaged (Plan 6).
            releaseLightCards();
            // The user is driving the ring themselves — an auto-show in
            // progress must not later yank the card back to Clock (4.11).
            m_autoShow.noteManualNavigation();
            m_cards.set(CardId::TREADMILL);
            playAcceptBeep(true);
        }
    }

    if (ev.detents != 0)
    {
        if (screen == Screen::SELECTOR)
        {
            // The selector consumes detents itself; they must not reach
            // nudgeSpeed while it's open (4.7).
            m_selector.step(ev.detents, nowMs);
        }
        else
        {
            // Rotation adjusts speed while the belt is actually running
            // (unchanged). Otherwise, while parked on a card screen with no
            // selector open, it spins the card ring — one detent, one card
            // (spec 4.8). Connecting/Starting get neither: the knob is
            // ignored there.
            const bool beltRunning =
                (m_snapshot.status == TreadMillData::RUNNING) && !isPausedState();
            if (beltRunning)
            {
                // nudgeSpeed() synchronously fires onTargetSpeed(mph, pending=true)
                // via the controller's observer callback, so m_targetPending/
                // m_targetSpeedMph are already current by the time this returns —
                // arm the overlay deadline from the nowMs this call actually has.
                m_controller.nudgeSpeed(ev.detents, nowMs);
                m_speedOverlayUntilMs = nowMs + DIAL_SPEED_OVERLAY_MS;
            }
            else if (screen == Screen::DISCONNECTED || screen == Screen::CLOCK ||
                     screen == Screen::FLIGHTS || screen == Screen::LIGHT_OFFICE ||
                     screen == Screen::LIGHT_LAMP)
            {
                // On a light card with a control engaged the knob adjusts
                // that control rather than scrolling the ring (Plan 6); the
                // command follows 300 ms after the last detent, from
                // tickLights().
                LightCardState* engagedLight = nullptr;
                if (screen == Screen::LIGHT_OFFICE || screen == Screen::LIGHT_LAMP)
                {
                    LightCardState& card = lightCardFor(screen);
                    if (card.engaged() != LightCardState::Engaged::NONE)
                    {
                        engagedLight = &card;
                    }
                }

                if (engagedLight != nullptr)
                {
                    engagedLight->detents(ev.detents, nowMs);
                }
                else
                {
                    // The ring is about to move: drop any engagement first so
                    // a card left mid-edit doesn't come back engaged.
                    releaseLightCards();
                    // Manual navigation in either direction cancels any
                    // auto-show episode, so a later drop to zero aircraft
                    // doesn't fight the user's own scrolling (spec 4.11).
                    m_autoShow.noteManualNavigation();
                    if (ev.detents > 0)
                    {
                        for (int i = 0; i < ev.detents; ++i)
                        {
                            m_cards.next();
                        }
                    }
                    else
                    {
                        for (int i = 0; i < -ev.detents; ++i)
                        {
                            m_cards.prev();
                        }
                    }
                }
            }
        }
    }

    if (ev.swipe != 0 && screen == Screen::SELECTOR)
    {
        // Horizontal swipe is an alternate way to step the candidate speed
        // (right = +1 = faster), same grid as a detent (4.7).
        m_selector.step(ev.swipe, nowMs);
    }

    // ev.wake needs no handling beyond the brightness change already applied
    // above.

#if DIAL_SOUND
    if (m_secondBeepPending && (int32_t)(nowMs - m_secondBeepDueMs) >= 0)
    {
        M5Dial.Speaker.tone(1500, 60);
        m_secondBeepPending = false;
    }
#endif
}

void DialUi::pollFlights(uint32_t nowMs)
{
    // Pull a fresh copy at most every kFlightsSnapIntervalMs (and always on
    // the very first call) — snapshot() is a Guarded copy, cheap but no
    // reason to pay for it every loop() iteration. Unsigned subtraction, so
    // this stays correct across a millis() wrap.
    if (!m_haveFlightsSnap || (nowMs - m_lastFlightsSnapMs) >= kFlightsSnapIntervalMs)
    {
        m_lastFlightsSnapMs = nowMs;
        m_flightsSnap = m_flights.snapshot();
        m_haveFlightsSnap = true;
    }
}

void DialUi::tickFlights()
{
    // Clamp to the (possibly just-changed) aircraft count — 0 when empty.
    if (m_flightsSnap.count == 0 || m_flightIdx >= m_flightsSnap.count)
    {
        m_flightIdx = 0;
    }

    // Tell the net task which airline logo to have ready for the currently-
    // selected aircraft; empty string when there is none (list empty, or the
    // aircraft's airline isn't known yet). Minor: only actually call
    // setWantedLogo() when that IATA has changed since the last call — it's
    // a Guarded write (cheap, but not free), and tickFlights() runs every
    // tick() while the Flights card is showing, so without this it fires
    // every ~loop-tick rather than only on an actual selection/enrichment
    // change.
    const char* iata = (m_flightsSnap.count > 0) ? m_flightsSnap.ac[m_flightIdx].airlineIata : "";
    if (strncmp(iata, m_lastWantedIata, 2) != 0)
    {
        m_flights.setWantedLogo(iata);
        strncpy(m_lastWantedIata, iata, 2);
        m_lastWantedIata[2] = '\0';
    }
}

void DialUi::tickLights(uint32_t nowMs)
{
    // Same rhythm as tickFlights(): snapshot() is a Guarded copy — cheap,
    // but no reason to pay for it on every loop() iteration. Both cards are
    // sync()ed from each fresh snapshot (not just the visible one) so the
    // other card is already current the moment the ring reaches it.
    if (!m_haveLightsSnap || (nowMs - m_lastLightsSnapMs) >= kLightsSnapIntervalMs)
    {
        m_lastLightsSnapMs = nowMs;
        const LightsModel::LightsSnapshot snap = m_lights.snapshot();
        m_haveLightsSnap = true;
        m_lightCards[static_cast<uint8_t>(LightsModel::LightKey::OFFICE)].sync(snap.office, nowMs);
        m_lightCards[static_cast<uint8_t>(LightsModel::LightKey::LAMP)].sync(snap.lamp, nowMs);
    }

    // Only the visible card can be engaged (handleInput() releases a card as
    // the ring leaves it), so only the visible card's settle/idle timers need
    // pumping — and tick() is a no-op on a card that isn't engaged anyway.
    // tickLights() only runs while tick() has already established the screen
    // is a light card, so m_cards.current() and that screen agree.
    const Screen lightScreen = (m_cards.current() == CardId::LIGHT_LAMP)
                                   ? Screen::LIGHT_LAMP
                                   : Screen::LIGHT_OFFICE;
    const LightsModel::LightKey key = (lightScreen == Screen::LIGHT_LAMP)
                                          ? LightsModel::LightKey::LAMP
                                          : LightsModel::LightKey::OFFICE;
    const LightsModel::Command cmd = lightCardFor(lightScreen).tick(nowMs);
    if (cmd.type != LightsModel::Command::Type::NONE)
    {
        publishLightCommand(key, cmd);
    }
}

void DialUi::publishLightCommand(LightsModel::LightKey key, const LightsModel::Command& cmd)
{
    PublishItem item;
    item.type     = PubType::LIGHT_CMD;
    item.lightKey = static_cast<uint8_t>(key);
    const size_t n = LightsModel::formatCommand(cmd, item.lightJson, sizeof(item.lightJson));
    if (n == 0)
    {
        log_w("DialUi: light %s command type %d did not format, dropped",
              LightsModel::keyName(key), (int)cmd.type);
        return;
    }
    log_i("DialUi: light %s -> %s", LightsModel::keyName(key), item.lightJson);
    m_net.enqueuePublish(item);
}

void DialUi::releaseLightCards()
{
    for (uint8_t i = 0; i < static_cast<uint8_t>(LightsModel::LightKey::COUNT); ++i)
    {
        m_lightCards[i].release();
    }
}

LightCardState& DialUi::lightCardFor(Screen screen)
{
    const LightsModel::LightKey key = (screen == Screen::LIGHT_LAMP)
                                          ? LightsModel::LightKey::LAMP
                                          : LightsModel::LightKey::OFFICE;
    return m_lightCards[static_cast<uint8_t>(key)];
}

const LightCardState& DialUi::lightCardFor(Screen screen) const
{
    const LightsModel::LightKey key = (screen == Screen::LIGHT_LAMP)
                                          ? LightsModel::LightKey::LAMP
                                          : LightsModel::LightKey::OFFICE;
    return m_lightCards[static_cast<uint8_t>(key)];
}

void DialUi::playStopBeep(uint32_t nowMs, bool accepted)
{
#if DIAL_SOUND
    if (accepted)
    {
        // Two short beeps 80 ms apart, non-blocking: play the first now and
        // let the scheduler in handleInput() fire the second once its
        // deadline passes.
        M5Dial.Speaker.tone(1500, 60);
        m_secondBeepPending = true;
        m_secondBeepDueMs = nowMs + 80;
    }
    else
    {
        M5Dial.Speaker.tone(400, 120);
    }
#else
    (void)nowMs;
    (void)accepted;
#endif
}

void DialUi::playAcceptBeep(bool accepted)
{
#if DIAL_SOUND
    if (accepted)
    {
        M5Dial.Speaker.tone(2000, 40);
    }
    else
    {
        M5Dial.Speaker.tone(400, 120);
    }
#else
    (void)accepted;
#endif
}

void DialUi::applyBrightness()
{
    const DialInput::Backlight bl = m_input.backlight();
    if (bl == m_lastBacklight)
    {
        return;
    }
    m_lastBacklight = bl;

    uint8_t level = kBrightFull;
    switch (bl)
    {
    case DialInput::Backlight::FULL: level = kBrightFull; break;
    case DialInput::Backlight::DIM:  level = kBrightDim;  break;
    }
    M5Dial.Display.setBrightness(level);
}

void DialUi::setFlightsAutoShow(bool on)
{
    m_autoShow.setEnabled(on);
}

void DialUi::onSnapshot(const TreadMillData& d)
{
    m_snapshot = d;
}

void DialUi::onTargetSpeed(float mph, bool pending)
{
    m_targetSpeedMph = mph;
    m_targetPending = pending;
}

void DialUi::onNetStatus(NetStatus s)
{
    m_netStatus = s;
}

bool DialUi::isPausedState() const
{
    // The belt reports STOPPED while paused, so the link's own pause flag
    // (surfaced via the controller) is the only way to tell "paused" apart
    // from "stopped"; PAUSED itself is included for the optimistic snapshot
    // window right after a pause command, before the link flag catches up.
    return m_controller.isPaused() || m_snapshot.status == TreadMillData::PAUSED;
}

DialUi::Screen DialUi::currentScreen(bool paused) const
{
    // Checked before COUNTDOWN: TreadmillHandler::start() while disconnected
    // queues the command and the controller optimistically publishes
    // COUNTDOWN right away, but the belt hasn't actually started counting
    // down — it's still mid-connect (or mid-kick-phase-retry). Showing
    // Connecting here is what makes that state cancellable at all.
    if (m_controller.isConnecting())
    {
        return Screen::CONNECTING;
    }
    if (m_snapshot.status == TreadMillData::COUNTDOWN)
    {
        return Screen::STARTING;
    }
    if (m_snapshot.status == TreadMillData::RUNNING || paused)
    {
        return Screen::RUNNING;
    }
    // handleInput() closes the selector as soon as any of the screens above
    // becomes true, but currentScreen() is also called from draw()/
    // buildFrameKey() before that housekeeping runs on a given tick (e.g.
    // the very first render), so the ordering above is what actually makes
    // those screens win, not just the close call.
    if (m_selector.isOpen())
    {
        return Screen::SELECTOR;
    }
    // Belt idle and no selector open: show whichever card the ring is
    // parked on (spec 4.8). TREADMILL is the existing Disconnected screen;
    // CLOCK is the clock card; FLIGHTS is the flights card (spec 4.9);
    // LIGHT_OFFICE/LIGHT_LAMP are the two HA lights cards (Plan 6).
    switch (m_cards.current())
    {
    case CardId::TREADMILL:
        return Screen::DISCONNECTED;
    case CardId::FLIGHTS:
        return Screen::FLIGHTS;
    case CardId::LIGHT_OFFICE:
        return Screen::LIGHT_OFFICE;
    case CardId::LIGHT_LAMP:
        return Screen::LIGHT_LAMP;
    case CardId::CLOCK:
    default:
        return Screen::CLOCK;
    }
}

DialUi::FrameKey DialUi::buildFrameKey(uint32_t nowMs) const
{
    const bool paused = isPausedState();
    FrameKey key;
    key.status        = static_cast<uint8_t>(m_snapshot.status);
    key.screen         = static_cast<uint8_t>(currentScreen(paused));
    key.paused        = paused;
    key.durationSec   = m_snapshot.durationSec;
    key.speedTenths   = static_cast<int32_t>(m_snapshot.speedFeedback * 10.0f);
    key.distanceCenti = static_cast<int32_t>(m_snapshot.distanceKm * 100.0f);
    key.steps          = m_snapshot.steps;
    key.targetTenths   = static_cast<int32_t>(m_targetSpeedMph * 10.0f);
    key.pending        = m_targetPending;
    key.overlayActive  = (int32_t)(m_speedOverlayUntilMs - nowMs) > 0;
    key.netStatus       = static_cast<uint8_t>(m_netStatus);
    key.holdUnits       = static_cast<int32_t>(m_holdProgress * 20.0f);
    key.pulsePhase       = static_cast<uint8_t>((nowMs / 500) % 2);
    key.connectAttempts  = m_controller.connectAttempts();
    key.sessionDurationSec   = m_snapshot.sessionDurationSec;
    key.sessionDistanceCenti = static_cast<int32_t>(m_snapshot.sessionDistanceKm * 100.0f);
    key.sessionSteps         = m_snapshot.sessionSteps;
    key.selectorOpen   = m_selector.isOpen();
    key.selectorTenths = static_cast<int32_t>(m_selector.value() * 10.0f);
    key.cardId          = static_cast<uint8_t>(m_cards.current());

    // Clock card redraw-once-a-second (spec 4.8): -1 while TimeService isn't
    // valid yet so the --:-- face doesn't redraw every tick; a real
    // hh*3600+mm*60+ss once it is, so the second hand advances.
    struct tm t;
    if (m_time.localTime(t))
    {
        key.clockSec = t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
    }
    else
    {
        key.clockSec = -1;
    }

    // Flights card (spec 4.9): idx/count/stale/offline cover most redraw
    // triggers (cycling aircraft, a fetch landing, going offline); the hash
    // catches the remaining case where a fresh snapshot lands with the same
    // idx/count but different data underneath it (e.g. a moving aircraft's
    // altitude/speed changed, or the whole list got replaced 1-for-1).
    key.flightIdx     = m_flightIdx;
    key.flightCount    = m_flightsSnap.count;
    key.flightStale    = m_flightsSnap.stale;
    key.flightOffline  = m_flightsSnap.offline;
    if (m_flightsSnap.count > 0 && m_flightIdx < m_flightsSnap.count)
    {
        // FNV-1a over the callsign bytes, then folded in altFt/100 and
        // gsKt/10 (coarsened the same way FrameKey coarsens speed/distance
        // elsewhere) — cheap, deterministic, no heap.
        const FlightsModel::Aircraft& a = m_flightsSnap.ac[m_flightIdx];
        uint32_t h = 2166136261u;
        for (const char* p = a.callsign; *p != '\0'; ++p)
        {
            h ^= static_cast<uint8_t>(*p);
            h *= 16777619u;
        }
        h ^= static_cast<uint32_t>(a.altFt / 100);
        h *= 16777619u;
        h ^= static_cast<uint32_t>(a.gsKt / 10);
        h *= 16777619u;
        key.flightHash = static_cast<uint16_t>(h ^ (h >> 16));
    }
    else
    {
        key.flightHash = 0;
    }

    // Lights cards (Plan 6): one FNV-1a over everything drawLight() reads off
    // the card that's actually showing — the drawn value, the engaged/settling
    // highlight and the button enable states all live in there. 0 when no
    // light card is showing, so the other screens never redraw on light state.
    key.lightMqttUp = (m_netStatus == NetStatus::MQTT_UP);
    const Screen lightScreen = static_cast<Screen>(key.screen);
    if (lightScreen == Screen::LIGHT_OFFICE || lightScreen == Screen::LIGHT_LAMP)
    {
        const LightsModel::LightKey lk = (lightScreen == Screen::LIGHT_LAMP)
                                             ? LightsModel::LightKey::LAMP
                                             : LightsModel::LightKey::OFFICE;
        const LightCardState& card = lightCardFor(lightScreen);
        const LightsModel::LightState& v = card.view();
        uint32_t h = 2166136261u;
        const uint32_t fields[10] = {
            static_cast<uint32_t>(lk),
            static_cast<uint32_t>(v.valid),
            static_cast<uint32_t>(v.available),
            static_cast<uint32_t>(v.on),
            static_cast<uint32_t>(v.brightnessPct),
            static_cast<uint32_t>(v.kelvin),
            static_cast<uint32_t>(static_cast<int>(v.hue)),
            static_cast<uint32_t>(card.engaged()),
            static_cast<uint32_t>(card.settling()),
            static_cast<uint32_t>(v.supportsColor),
        };
        for (uint32_t f : fields)
        {
            h ^= f;
            h *= 16777619u;
        }
        // mode picks the caption (kelvin vs hue), so it belongs in the hash too.
        h ^= static_cast<uint32_t>(v.mode);
        h *= 16777619u;
        key.lightHash = static_cast<uint16_t>(h ^ (h >> 16));
    }
    else
    {
        key.lightHash = 0;
    }

    return key;
}

void DialUi::render(uint32_t nowMs)
{
    // Redraw-skip: only actually paint+push when something visible changed,
    // or kFrameElapsedMs has passed (so the 1 Hz PAUSED pulse still
    // animates, and any float jitter smaller than the FrameKey's coarsening
    // eventually still gets picked up).
    const FrameKey key     = buildFrameKey(nowMs);
    const bool     changed = !m_haveLastFrame || !(key == m_lastFrame);
    const bool     elapsed = (nowMs - m_lastFrameDrawMs) >= kFrameElapsedMs;
    if (!changed && !elapsed)
    {
        return;
    }
    m_lastFrame      = key;
    m_haveLastFrame  = true;
    m_lastFrameDrawMs = nowMs;

    m_logoToDraw[0] = '\0';
    if (m_useCanvas)
    {
        draw(m_canvas, nowMs);
        // Explicit destination: m_canvas has no parent bound at construction
        // (see the m_canvas declaration in DialUi.h for why), so pushSprite()
        // needs to be told where to push rather than relying on one.
        if (m_logoToDraw[0] != '\0')
        {
            // drawFlights() filled the logo rectangle with the TRANSPARENT
            // index, so this push leaves whatever is on the display there —
            // the full-colour logo decoded earlier — untouched. No flicker.
            m_canvas.pushSprite(&M5Dial.Display, 0, 0, static_cast<uint32_t>(Col::TRANSPARENT));
        }
        else
        {
            m_canvas.pushSprite(&M5Dial.Display, 0, 0);
            m_logoOnScreen[0] = '\0'; // opaque push wiped any logo
        }
    }
    else
    {
        draw(M5Dial.Display, nowMs);
        m_logoOnScreen[0] = '\0'; // direct draw repaints everything each frame
    }
    // Airline logo in full colour, drawn straight onto the display (the palette
    // canvas would posterise it), and only when the airline changed.
    if (m_logoToDraw[0] != '\0' && strncmp(m_logoToDraw, m_logoOnScreen, 2) != 0)
    {
        // The PNG decoder needs a few tens of KB; while the network task has a
        // TLS session open there may not be enough. Defer the decode (the
        // 250 ms frame fallback retries) instead of failing, and only remember
        // a failure as permanent when heap was ample at the time.
        const uint32_t largest  = ESP.getMaxAllocHeap();
        const uint32_t freeHeap = ESP.getFreeHeap();
        // pngle needs ~45 KB of workspace plus line buffers: wait for a quiet
        // moment (no TLS session open on the net task) before decoding.
        if (largest >= 48 * 1024 && freeHeap >= 80 * 1024)
        {
            char path[24];
            snprintf(path, sizeof(path), "/logos/%s.png", m_logoToDraw);
            // Decode into a temporary 16-bit sprite (black background, so the
            // PNG's transparent pixels do not show whatever was under them),
            // push it, free it. Decoding straight onto the display was found to
            // hold ~43 KB of heap afterwards (2026-09-04).
            const uint32_t heapBefore = ESP.getFreeHeap();
            bool pngOk = false;
            size_t dbgFsz = 0, dbgGot = 0; int dbgSprite = -1;
            {
                // Read the PNG ourselves (<= 8 KB) and decode from memory: the
                // drawPngFile(fs, path) wrapper was found to hold ~43 KB after
                // a successful display decode (2026-09-04).
                File f = LittleFS.open(path, "r");
                const size_t fsz = f ? (size_t)f.size() : 0;
                uint8_t* png = (f && fsz > 8 && fsz <= 8192) ? (uint8_t*)malloc(fsz) : nullptr;
                size_t got = 0;
                if (png) { got = f.read(png, fsz); }
                if (f) f.close();
                dbgFsz = fsz; dbgGot = got;
                if (png && got == fsz)
                {
                    M5Canvas tmp;
                    tmp.setColorDepth(16);
                    dbgSprite = tmp.createSprite(120, 48) ? 1 : 0;
                    if (dbgSprite == 1)
                    {
                        tmp.fillSprite(TFT_BLACK);
                        pngOk = tmp.drawPng(png, fsz, 0, 0);
                        if (pngOk) tmp.pushSprite(&M5Dial.Display, kFlightsLogoX, kFlightsLogoY);
                        // LovyanGFX keeps its ~45 KB pngle workspace allocated
                        // for reuse; release it or every decode leaks it.
                        tmp.releasePngMemory();
                        tmp.deleteSprite();
                    }
                }
                if (png) free(png);
            }
            log_i("DialUi: logo %s decode heap %u -> %u ok=%d file=%u read=%u sprite=%d", m_logoToDraw, (unsigned)heapBefore, (unsigned)ESP.getFreeHeap(), (int)pngOk, (unsigned)dbgFsz, (unsigned)dbgGot, (int)dbgSprite);
            if (pngOk)
            {
                strncpy(m_logoOnScreen, m_logoToDraw, 2);
                m_logoOnScreen[2] = '\0';
            }
            else if (!isLogoRetried(m_logoToDraw))
            {
                // First failure for this airline: the cached file may be bad
                // (seen with files written by an earlier session). Drop it and
                // let FlightsService download a fresh copy once.
                log_w("DialUi: logo %s decode failed (%u free / %u largest), dropping cache and retrying once",
                      m_logoToDraw, (unsigned)freeHeap, (unsigned)largest);
                markLogoRetried(m_logoToDraw);
                m_flights.invalidateLogo(m_logoToDraw);
                m_logoOnScreen[0] = '\0';
                m_lastFrame.flightHash ^= 0x5A5A;
            }
            else
            {
                log_w("DialUi: logo %s decode failed again, using text fallback for this session", m_logoToDraw);
                markLogoDecodeFailed(m_logoToDraw);
                m_logoOnScreen[0] = '\0';
                m_lastFrame.flightHash ^= 0x5A5A; // force a redraw so the text fallback appears
            }
        }
        else
        {
            m_lastFrame.flightHash ^= 0x5A5A; // force another frame soon to retry
        }
    }
}

void DialUi::drawStatusDots(LovyanGFX& gfx)
{
    const bool bleUp  = m_snapshot.status != TreadMillData::DISCONNECTED;
    const bool wifiUp = m_netStatus >= NetStatus::WIFI_UP;
    const bool mqttUp = m_netStatus == NetStatus::MQTT_UP;

    gfx.fillCircle(kBleDotX,  kDotY, kDotRadius, bleUp  ? col(Col::BLE_ON) : col(Col::DIM));
    gfx.fillCircle(kWifiDotX, kDotY, kDotRadius, wifiUp ? col(Col::NET_ON) : col(Col::DIM));
    gfx.fillCircle(kMqttDotX, kDotY, kDotRadius, mqttUp ? col(Col::NET_ON) : col(Col::DIM));
}

void DialUi::draw(LovyanGFX& gfx, uint32_t nowMs)
{
    gfx.fillScreen(col(Col::BG));
    gfx.setTextDatum(middle_center);

    const bool paused = isPausedState();
    const Screen screen = currentScreen(paused);
    // Status dots are hidden while the belt is running (clean screen for
    // walking); they show on every other screen, including Paused.
    // Status dots belong to the treadmill screens only: hidden while the belt is
    // running (clean walking screen) and on desk cards such as the Clock and
    // Flights (spec 4.8, spec 4.9).
    const bool showDots = !(screen == Screen::RUNNING && !paused) &&
                          screen != Screen::CLOCK && screen != Screen::FLIGHTS &&
                          screen != Screen::LIGHT_OFFICE && screen != Screen::LIGHT_LAMP;
    if (showDots)
    {
        drawStatusDots(gfx);
    }
    switch (screen)
    {
    case Screen::RUNNING:
        drawRunning(gfx, paused, nowMs);
        break;
    case Screen::STARTING:
        drawStarting(gfx, nowMs);
        break;
    case Screen::CONNECTING:
        drawConnecting(gfx);
        break;
    case Screen::SELECTOR:
        drawSelector(gfx);
        break;
    case Screen::CLOCK:
        drawClock(gfx);
        break;
    case Screen::FLIGHTS:
        drawFlights(gfx);
        break;
    case Screen::LIGHT_OFFICE:
        drawLight(gfx, LightsModel::LightKey::OFFICE);
        break;
    case Screen::LIGHT_LAMP:
        drawLight(gfx, LightsModel::LightKey::LAMP);
        break;
    case Screen::DISCONNECTED:
    default:
        drawDisconnected(gfx);
        break;
    }

    // Speed overlay (I3): drawn on top of whichever screen just painted,
    // for as long as the overlay window from the last nudge is open — not
    // just while Running. See the m_speedOverlayUntilMs comment in DialUi.h.
    // Never on Selector: it already shows the candidate speed itself, and a
    // stale overlay window can't be armed there anyway (nudgeSpeed() only
    // runs while Running), but guard explicitly rather than relying on that.
    const bool overlayActive =
        screen != Screen::SELECTOR && (int32_t)(m_speedOverlayUntilMs - nowMs) > 0;
    if (overlayActive)
    {
        drawSpeedOverlay(gfx, paused);
    }

    // Long-press-to-stop progress (I4): drawn on top of whichever screen
    // just painted, for as long as a hold is in progress — not just while
    // Running/Paused (e.g. holding to cancel from Connecting).
    if (m_holdProgress > 0.0f)
    {
        drawHoldArc(gfx);
    }
}

// Default screen: not connecting, not running/paused, not counting down
// (covers DISCONNECTED and a settled STOPPED with no belt activity) — shows
// the previous session's summary and a hint to start a new one.
void DialUi::drawDisconnected(LovyanGFX& gfx)
{
    gfx.setTextColor(col(Col::TEXT), col(Col::BG));
    gfx.drawString("LAST SESSION", kCentreX, kDiscTitleY, &fonts::Font2);

    // formatDuration(0) reads "00:00", which looks like a real (very short)
    // session rather than "no session yet" — show "--:--" instead.
    char timeBuf[16];
    if (m_snapshot.sessionDurationSec == 0)
    {
        snprintf(timeBuf, sizeof(timeBuf), "--:--");
    }
    else
    {
        DialFormat::formatDuration(m_snapshot.sessionDurationSec, timeBuf, sizeof(timeBuf));
    }
    gfx.drawString(timeBuf, kCentreX, kDiscTimeY, &fonts::Font4);

    // Distance/steps formatters already render zero sensibly ("0.00" / "0"),
    // so no zero-session branch is needed for these two.
    char distBuf[8];
    DialFormat::formatDistanceKm(m_snapshot.sessionDistanceKm, distBuf, sizeof(distBuf));
    gfx.setTextColor(col(Col::TEXT), col(Col::BG));
    gfx.drawString(distBuf, kRowLeftX, kDiscRowY, &fonts::Font4);
    gfx.setTextColor(col(Col::DIM), col(Col::BG));
    gfx.drawString("km", kRowLeftX, kDiscCaptionY, &fonts::Font2);

    char stepsBuf[12];
    DialFormat::formatSteps(m_snapshot.sessionSteps, stepsBuf, sizeof(stepsBuf));
    gfx.setTextColor(col(Col::TEXT), col(Col::BG));
    gfx.drawString(stepsBuf, kRowRightX, kDiscRowY, &fonts::Font4);
    gfx.setTextColor(col(Col::DIM), col(Col::BG));
    gfx.drawString("steps", kRowRightX, kDiscCaptionY, &fonts::Font2);

    gfx.setTextColor(col(Col::DIM), col(Col::BG));
    gfx.drawString("tap: speed   hold: start", kCentreX, kDiscHintY, &fonts::Font2);
}

// Clock card (spec 4.8): analogue face — 12 tick marks, hour/minute/second
// hands from TimeService's wall clock (NTP over WiFi, backed by the Dial's
// RTC so it reads correctly before WiFi comes up), small date at the 6
// o'clock position. Redrawn once a second via the clockSec field in
// FrameKey. When TimeService isn't valid yet (fresh device, no WiFi, empty
// RTC) draws the ticks with no hands and "--:--" instead of a time.
void DialUi::drawClock(LovyanGFX& gfx)
{
    // 12 tick marks; the four cardinal ones (12/3/6/9) run from a slightly
    // larger radius so they read as longer/bolder without a second draw call.
    for (int i = 0; i < 12; ++i)
    {
        const bool cardinal = (i % 3) == 0;
        const HandLine t = ClockFace::tick(i, kRingCx, kRingCy,
                                            cardinal ? kClockR0Long : kClockR0,
                                            kClockR1);
        thickLine(gfx, t.x0, t.y0, t.x1, t.y1, cardinal ? 3 : 2, col(Col::DIM));
    }

    struct tm t;
    if (!m_time.localTime(t))
    {
        gfx.setTextColor(col(Col::TEXT), col(Col::BG));
        gfx.drawString("--:--", kRingCx, kRingCy, &fonts::Font4);
        gfx.setTextColor(col(Col::DIM), col(Col::BG));
        gfx.drawString("waiting for time", kRingCx, kClockDateY, &fonts::Font2);
        return;
    }

    const HandLine hh = ClockFace::hand(ClockFace::hourAngle(t.tm_hour, t.tm_min),
                                         kRingCx, kRingCy, kHourHandLen);
    const HandLine mm = ClockFace::hand(ClockFace::minuteAngle(t.tm_min, t.tm_sec),
                                         kRingCx, kRingCy, kMinuteHandLen);
    const HandLine ss = ClockFace::hand(ClockFace::secondAngle(t.tm_sec),
                                         kRingCx, kRingCy, kSecondHandLen);

    thickLine(gfx, hh.x0, hh.y0, hh.x1, hh.y1, kHourHandW, col(Col::TEXT));
    thickLine(gfx, mm.x0, mm.y0, mm.x1, mm.y1, kMinuteHandW, col(Col::TEXT));
    thickLine(gfx, ss.x0, ss.y0, ss.x1, ss.y1, kSecondHandW, col(Col::SECOND));

    gfx.fillCircle(kRingCx, kRingCy, kClockCentreDotR, col(Col::TEXT));
    gfx.fillCircle(kRingCx, kRingCy, kClockSecondDotR, col(Col::SECOND));

    // "Mon 3 Sep" — %-e (no leading zero/space) isn't universally supported
    // by newlib's strftime, so use %e (space-padded to width 2) instead. On
    // a single-digit day that leaves "Mon  3 Sep" — the literal space in the
    // format plus %e's own pad space — so collapse that one double space.
    char dateBuf[16];
    strftime(dateBuf, sizeof(dateBuf), "%a %e %b", &t);
    for (char* p = dateBuf; *p != '\0'; ++p)
    {
        if (p[0] == ' ' && p[1] == ' ')
        {
            memmove(p, p + 1, strlen(p + 1) + 1);
            break;
        }
    }
    gfx.setTextColor(col(Col::DIM), col(Col::BG));
    gfx.drawString(dateBuf, kCentreX, kClockDateY, &fonts::Font2);
}

// Flights card (spec 4.9): nearest aircraft, nearest-first, cycled by tap
// (see handleInput()/tickFlights()). m_flightsSnap is written only by
// pollFlights() and m_flightIdx only by handleInput()/tickFlights()/tick()
// (all on the loop task), so this is a plain read — no locking needed on
// top of Guarded's own.
void DialUi::drawFlights(LovyanGFX& gfx)
{
    if (m_flightsSnap.stale)
    {
        gfx.fillCircle(kFlightsStaleDotX, kFlightsStaleDotY, kFlightsStaleDotR, col(Col::DIM));
    }

    if (m_flightsSnap.offline)
    {
        gfx.setTextColor(col(Col::TEXT), col(Col::BG));
        gfx.drawString("waiting for HA", kRingCx, kRingCy, &fonts::Font4);
        return;
    }

    if (m_flightsSnap.count == 0)
    {
        gfx.setTextColor(col(Col::TEXT), col(Col::BG));
        gfx.drawString("no aircraft nearby", kRingCx, kRingCy, &fonts::Font4);
        // The search radius is Home Assistant's business now (spec 4.11) —
        // the Dial no longer has FLIGHTS_RADIUS_MI to name here.
        gfx.setTextColor(col(Col::DIM), col(Col::BG));
        gfx.drawString("none in range", kCentreX, kFlightsEmptyCaptionY, &fonts::Font2);
        return;
    }

    const uint8_t idx = (m_flightIdx < m_flightsSnap.count) ? m_flightIdx : 0;
    const FlightsModel::Aircraft& ac = m_flightsSnap.ac[idx];

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
    const bool wantLogo = ac.airlineIata[0] != '\0' && !isLogoDecodeFailed(ac.airlineIata);
    const bool haveLogo = wantLogo && m_flights.logoReady(ac.airlineIata);
    if (haveLogo)
    {
        strncpy(m_logoToDraw, ac.airlineIata, 2);
        m_logoToDraw[2] = '\0';
        // Reserve the logo rectangle: on the canvas the TRANSPARENT index keeps
        // the display's logo pixels through the push (see render()).
        gfx.fillRect(kFlightsLogoX, kFlightsLogoY, 120, 48,
                     m_useCanvas ? static_cast<uint32_t>(Col::TRANSPARENT) : col(Col::BG));
    }
    else
    {
        gfx.setTextColor(col(Col::TEXT), col(Col::BG));
        const char* fallback = (ac.operatorKnown && ac.operatorName[0] != '\0') ? ac.operatorName : ac.callsign;
        gfx.drawString(fallback, kCentreX, kFlightsFallbackY, &fonts::Font2);
    }

    // "callsign - type"
    char line1[32];
    if (ac.type[0] != '\0')
    {
        snprintf(line1, sizeof(line1), "%s - %s", ac.callsign, ac.type);
    }
    else
    {
        snprintf(line1, sizeof(line1), "%s", ac.callsign);
    }
    gfx.setTextColor(col(Col::TEXT), col(Col::BG));
    gfx.drawString(line1, kCentreX, kFlightsCallsignY, &fonts::Font2);

    // Route, large: "LHR -> JFK" (ASCII arrow — Font4 may lack the real one)
    // or "route unknown" when not yet enriched.
    if (ac.routeKnown && ac.fromIata[0] != '\0' && ac.toIata[0] != '\0')
    {
        char routeBuf[16];
        snprintf(routeBuf, sizeof(routeBuf), "%s -> %s", ac.fromIata, ac.toIata);
        gfx.setTextColor(col(Col::TEXT), col(Col::BG));
        gfx.drawString(routeBuf, kCentreX, kFlightsRouteY, &fonts::Font4);
    }
    else
    {
        gfx.setTextColor(col(Col::DIM), col(Col::BG));
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
        gfx.setTextColor(col(Col::DIM), col(Col::BG));
        gfx.drawString(cityBuf, kCentreX, kFlightsCityY, &fonts::Font2);
    }

    // Altitude/speed: "12,000 ft - 450 kt".
    char altBuf[12];
    formatThousands(ac.altFt, altBuf, sizeof(altBuf));
    char altSpeedBuf[32];
    snprintf(altSpeedBuf, sizeof(altSpeedBuf), "%s ft - %d kt", altBuf, ac.gsKt);
    gfx.setTextColor(col(Col::TEXT), col(Col::BG));
    gfx.drawString(altSpeedBuf, kCentreX, kFlightsAltY, &fonts::Font2);

    // Distance/compass: "3.1 mi NE" (no index suffix — the page dots below
    // show position in the list instead).
    char distBuf[32];
    snprintf(distBuf, sizeof(distBuf), "%.1f mi %s", static_cast<double>(ac.distMi),
             Geo::compass8(static_cast<float>(ac.bearing)));
    gfx.setTextColor(col(Col::TEXT), col(Col::BG));
    gfx.drawString(distBuf, kCentreX, kFlightsDistY, &fonts::Font2);

    // Page dots: one per aircraft (up to 6, the list is never larger),
    // centred on kFlightsHintY — filled for the one currently shown, hollow
    // outlines for the rest. Replaces the old "tap: next" hint text.
    const int32_t dotsW = static_cast<int32_t>(m_flightsSnap.count - 1) * kFlightsDotSpacing;
    const int32_t firstDotX = kCentreX - dotsW / 2;
    for (uint8_t i = 0; i < m_flightsSnap.count; ++i)
    {
        const int32_t dotX = firstDotX + static_cast<int32_t>(i) * kFlightsDotSpacing;
        if (i == idx)
        {
            gfx.fillCircle(dotX, kFlightsHintY, kFlightsDotR, col(Col::TEXT));
        }
        else
        {
            gfx.drawCircle(dotX, kFlightsHintY, kFlightsDotR, col(Col::DIM));
        }
    }
}

// I4: see m_logoFailed's comment in DialUi.h. `iata` is compared with
// strncmp(..., 2) throughout, same as m_logoIata/ac.airlineIata elsewhere in
// drawFlights() — a 2-letter IATA code, not necessarily NUL-padded past that.
bool DialUi::isLogoDecodeFailed(const char* iata) const
{
    for (uint8_t i = 0; i < m_logoFailedCount; i++)
    {
        if (strncmp(m_logoFailed[i], iata, 2) == 0)
        {
            return true;
        }
    }
    return false;
}

void DialUi::markLogoDecodeFailed(const char* iata)
{
    if (isLogoDecodeFailed(iata))
    {
        return;
    }
    strncpy(m_logoFailed[m_logoFailedNext], iata, 2);
    m_logoFailed[m_logoFailedNext][2] = '\0';
    m_logoFailedNext = (uint8_t)((m_logoFailedNext + 1) % kLogoFailedSize);
    if (m_logoFailedCount < kLogoFailedSize)
    {
        m_logoFailedCount++;
    }
}

// Lights card (Plan 6, spec 4.10). One HA light: title, the value ring
// reused from the speed/selector screens, a centred value or state string,
// a caption for the colour field, and the Power/Bright(/Colour) buttons.
void DialUi::drawLight(LovyanGFX& gfx, LightsModel::LightKey key)
{
    const LightCardState& card = m_lightCards[static_cast<uint8_t>(key)];
    const bool hasColour = card.hasColour();
    const LightsModel::LightState& v = card.view();
    const LightCardState::Engaged engaged = card.engaged();

    gfx.setTextColor(col(Col::TEXT), col(Col::BG));
    gfx.drawString(hasColour ? "Lamp" : "Office", kRingCx, kLightTitleY, &fonts::Font4);

    // MQTT is the only path HA light state arrives on, so without it there is
    // nothing to show and nothing worth ringing.
    const bool mqttUp   = (m_netStatus == NetStatus::MQTT_UP);
    const bool haveData = v.valid && v.available;

    if (mqttUp && haveData)
    {
        // Same two-arc idiom as drawSelector(): DIM track, then the value arc
        // from the same start angle. Amber while a control is engaged (the
        // knob is live and a command is coming), cyan otherwise.
        gfx.fillArc(kRingCx, kRingCy, kRingOuter, kRingInner, kRingStartDeg,
                    kRingStartDeg + kRingSweepDeg, col(Col::DIM));
        const float sweep = card.ringFraction() * kRingSweepDeg;
        if (sweep > 0.0f)
        {
            gfx.fillArc(kRingCx, kRingCy, kRingOuter, kRingInner, kRingStartDeg,
                        kRingStartDeg + sweep,
                        col(engaged != LightCardState::Engaged::NONE ? Col::PENDING : Col::SPEED));
        }
    }

    if (!mqttUp)
    {
        gfx.setTextColor(col(Col::DIM), col(Col::BG));
        gfx.drawString("waiting for HA", kRingCx, kRingCy, &fonts::Font4);
    }
    else if (!haveData)
    {
        // No retained state yet (or HA reports the light unavailable). Power
        // stays live: LightCardState allows a blind switch-on once the state
        // topic has at least parsed.
        gfx.setTextColor(col(Col::DIM), col(Col::BG));
        gfx.drawString("no data", kRingCx, kRingCy, &fonts::Font4);
    }
    else
    {
        if (v.on)
        {
            // Font7 has no '%' glyph (see kLightPctGap), so the digits and
            // the percent sign are two strings centred together.
            char pctBuf[8];
            snprintf(pctBuf, sizeof(pctBuf), "%u", (unsigned)v.brightnessPct);
            const int32_t digitsW = gfx.textWidth(pctBuf, &fonts::Font7);
            const int32_t signW   = gfx.textWidth("%", &fonts::Font4);
            const int32_t left    = kRingCx - (digitsW + kLightPctGap + signW) / 2;

            gfx.setTextColor(
                col(engaged == LightCardState::Engaged::BRIGHT ? Col::PENDING : Col::TEXT),
                col(Col::BG));
            gfx.setTextDatum(middle_left);
            gfx.drawString(pctBuf, left, kRingCy, &fonts::Font7);
            gfx.drawString("%", left + digitsW + kLightPctGap, kRingCy, &fonts::Font4);
            gfx.setTextDatum(middle_center);
        }
        else
        {
            gfx.setTextColor(col(Col::DIM), col(Col::BG));
            gfx.drawString("OFF", kRingCx, kRingCy, &fonts::Font4);
        }

        // Colour caption: kelvin unless HA is reporting the light in hue/sat
        // mode. Amber while that field is the engaged one.
        char caption[16];
        Col captionCol = Col::DIM;
        if (v.mode == LightsModel::ColorMode::HS)
        {
            snprintf(caption, sizeof(caption), "hue %d", static_cast<int>(v.hue));
            if (engaged == LightCardState::Engaged::HUE)
            {
                captionCol = Col::PENDING;
            }
        }
        else
        {
            if (v.kelvin == 0)
            {
                snprintf(caption, sizeof(caption), "--K");
            }
            else
            {
                snprintf(caption, sizeof(caption), "%uK", (unsigned)v.kelvin);
            }
            if (engaged == LightCardState::Engaged::TEMP)
            {
                captionCol = Col::PENDING;
            }
        }
        gfx.setTextColor(col(captionCol), col(Col::BG));
        gfx.drawString(caption, kRingCx, kLightCaptionY, &fonts::Font2);
    }

    drawLightButtons(gfx, card, hasColour, mqttUp);
}

void DialUi::drawLightButtons(LovyanGFX& gfx, const LightCardState& card, bool hasColour,
                              bool mqttUp)
{
    const LightsModel::LightState& v = card.view();
    const LightCardState::Engaged engaged = card.engaged();
    const bool haveData = v.valid && v.available;

    const LightButtons::Button buttons[3] = {LightButtons::Button::POWER,
                                            LightButtons::Button::BRIGHT,
                                            LightButtons::Button::COLOUR};
    for (LightButtons::Button b : buttons)
    {
        if (b == LightButtons::Button::COLOUR && !hasColour)
        {
            continue;
        }

        // Engaged wins over active: it's the one control the knob is driving.
        bool isEngaged = false;
        bool isActive  = false;
        switch (b)
        {
        case LightButtons::Button::POWER:
            // Power stays live through "no data" (available == false) — a
            // blind switch-on is allowed (LightCardState::tapButton()) — but
            // LightCardState::tapButton(POWER) still refuses when !v.valid
            // (no retained state has parsed at all yet), so the button must
            // not be drawn active before that. v.valid is strictly weaker
            // than the BRIGHT/COLOUR gate below (haveData == valid &&
            // available), so this can't light up more than they do.
            isActive = mqttUp && v.valid;
            break;
        case LightButtons::Button::BRIGHT:
            isEngaged = (engaged == LightCardState::Engaged::BRIGHT);
            isActive  = mqttUp && haveData;
            break;
        case LightButtons::Button::COLOUR:
        default:
            isEngaged = (engaged == LightCardState::Engaged::TEMP ||
                         engaged == LightCardState::Engaged::HUE);
            isActive  = mqttUp && haveData && v.supportsColor;
            break;
        }

        const LightButtons::Geom g = LightButtons::geom(b, hasColour);
        Col labelCol;
        Col labelBg = Col::BG;
        if (isEngaged)
        {
            gfx.fillCircle(g.cx, g.cy, g.r, col(Col::PENDING));
            // Text background must be the fill, not the screen background, or
            // each glyph's background rect punches a black box in the circle.
            labelCol = Col::BG;
            labelBg  = Col::PENDING;
        }
        else if (isActive)
        {
            gfx.drawCircle(g.cx, g.cy, g.r, col(Col::DIM));
            labelCol = Col::TEXT;
        }
        else
        {
            gfx.drawCircle(g.cx, g.cy, g.r, col(Col::DIM_DIM));
            labelCol = Col::DIM_DIM;
        }
        // Label inside the circle: below it would fall off the round display
        // for the lowest button (cy + r + 12 = 229 > 226).
        gfx.setTextColor(col(labelCol), col(labelBg));
        gfx.drawString(LightButtons::label(b), g.cx, g.cy, &fonts::Font2);
    }
}

void DialUi::drawConnecting(LovyanGFX& gfx)
{
    gfx.setTextColor(col(Col::TEXT), col(Col::BG));
    gfx.drawString("Connecting...", kCentreX, kConnLabelY, &fonts::Font4);

    // Only show "attempt N" once there has actually been one — attempt 0
    // (the brief window before the first connectToDevice() call) just reads
    // "Connecting..." with no attempt line (I2).
    const uint16_t attempts = m_controller.connectAttempts();
    if (attempts > 0)
    {
        char attemptBuf[24];
        snprintf(attemptBuf, sizeof(attemptBuf), "attempt %u", (unsigned)attempts);
        gfx.setTextColor(col(Col::DIM), col(Col::BG));
        gfx.drawString(attemptBuf, kCentreX, kConnAttemptY, &fonts::Font2);
    }

    gfx.setTextColor(col(Col::DIM), col(Col::BG));
    gfx.drawString("belt beeps are normal", kCentreX, kConnBeepY, &fonts::Font2);
    gfx.drawString("tap or hold to cancel", kCentreX, kConnHintY, &fonts::Font2);
}

// Reachable only when actually connected and the belt itself reports
// COUNTDOWN — currentScreen() checks isConnecting() first, so a start()
// queued while disconnected shows Connecting instead (C1).
void DialUi::drawStarting(LovyanGFX& gfx, uint32_t nowMs)
{
    // Blink 1 Hz: on for the first 500 ms of each 1000 ms window — same
    // pulse mechanism as the PAUSED label on the running screen.
    const uint8_t pulsePhase = static_cast<uint8_t>((nowMs / 500) % 2);
    if (pulsePhase == 0)
    {
        gfx.setTextColor(col(Col::TEXT), col(Col::BG));
        gfx.drawString("STARTING", kCentreX, kStartLabelY, &fonts::Font4);
    }
    gfx.setTextColor(col(Col::DIM), col(Col::BG));
    gfx.drawString("tap to cancel", kCentreX, kStartHintY, &fonts::Font2);
}

// Start-speed picker (spec 4.7): opened by a tap on Disconnected. Same ring
// geometry as the running screen but always amber (the "pending" colour),
// since nothing has actually started yet.
void DialUi::drawSelector(LovyanGFX& gfx)
{
    gfx.fillArc(kRingCx, kRingCy, kRingOuter, kRingInner, kRingStartDeg,
                kRingStartDeg + kRingSweepDeg, col(Col::DIM));

    const float value = m_selector.value();
    const float ringSweep = DialFormat::speedToAngle(value);
    if (ringSweep > 0.0f)
    {
        gfx.fillArc(kRingCx, kRingCy, kRingOuter, kRingInner, kRingStartDeg,
                    kRingStartDeg + ringSweep, col(Col::PENDING));
    }

    gfx.setTextColor(col(Col::TEXT), col(Col::BG));
    gfx.drawString("START SPEED", kCentreX, kSelectorTitleY, &fonts::Font2);

    char valueBuf[16];
    DialFormat::formatSpeedMph(value, valueBuf, sizeof(valueBuf));
    gfx.setTextColor(col(Col::PENDING), col(Col::BG));
    gfx.drawString(valueBuf, kCentreX, kCentreY, &fonts::Font7);
    gfx.drawString("mph", kCentreX, kSelectorMphY, &fonts::Font2);

    gfx.setTextColor(col(Col::DIM), col(Col::BG));
    gfx.drawString("tap to start", kCentreX, kSelectorHint1Y, &fonts::Font2);
    gfx.drawString("hold: default", kCentreX, kSelectorHint2Y, &fonts::Font2);
}

void DialUi::drawRunning(LovyanGFX& gfx, bool paused, uint32_t nowMs)
{
    // Paused dims each colour to its DialUi::Col::*_DIM palette entry — see
    // the kPaletteRgb888 comment above for how those shades were derived
    // (formerly a runtime dimColor565() halving, now baked into the
    // palette). TEXT's paused shade is DIM itself (dimColor565(TFT_WHITE)
    // == TFT_DARKGREY exactly), not a dedicated TEXT_DIM entry.
    const uint32_t colTrack = paused ? col(Col::DIM_DIM)   : col(Col::DIM);
    const uint32_t colText  = paused ? col(Col::DIM)       : col(Col::TEXT);
    const uint32_t colDim   = paused ? col(Col::DIM_DIM)   : col(Col::DIM);
    const uint32_t colCyan  = paused ? col(Col::SPEED_DIM) : col(Col::SPEED);

    // Speed ring: dark 300 deg track (120 deg gap centred at the bottom),
    // then the current-speed arc from the same start angle. fillArc's
    // fmodf() handles the >360 deg wrap on its own, so one call each is
    // enough. drawSpeedOverlay() (called from draw(), I3) repaints this in
    // amber with the target speed instead while a nudge's overlay window is
    // open, so this is only what shows the rest of the time.
    gfx.fillArc(kRingCx, kRingCy, kRingOuter, kRingInner, kRingStartDeg,
                kRingStartDeg + kRingSweepDeg, colTrack);

    const float ringSweep = DialFormat::speedToAngle(m_snapshot.speedFeedback);
    if (ringSweep > 0.0f)
    {
        gfx.fillArc(kRingCx, kRingCy, kRingOuter, kRingInner, kRingStartDeg,
                    kRingStartDeg + ringSweep, colCyan);
    }

    // Centre: elapsed time. drawSpeedOverlay() (draw(), I3) repaints this
    // with the target speed instead while a nudge's overlay window is open.
    char centreBuf[16];
    DialFormat::formatDuration(m_snapshot.durationSec, centreBuf, sizeof(centreBuf));
    gfx.setTextColor(colText, col(Col::BG));
    gfx.drawString(centreBuf, kCentreX, kCentreY, &fonts::Font7);

    // Row: distance (left) / steps (right), each with a caption beneath.
    char distBuf[8];
    DialFormat::formatDistanceKm(m_snapshot.distanceKm, distBuf, sizeof(distBuf));
    gfx.setTextColor(colText, col(Col::BG));
    gfx.drawString(distBuf, kRowLeftX, kRowY, &fonts::Font4);
    gfx.setTextColor(colDim, col(Col::BG));
    gfx.drawString("km", kRowLeftX, kRowCaptionY, &fonts::Font2);

    char stepsBuf[12];
    DialFormat::formatSteps(m_snapshot.steps, stepsBuf, sizeof(stepsBuf));
    gfx.setTextColor(colText, col(Col::BG));
    gfx.drawString(stepsBuf, kRowRightX, kRowY, &fonts::Font4);
    gfx.setTextColor(colDim, col(Col::BG));
    gfx.drawString("steps", kRowRightX, kRowCaptionY, &fonts::Font2);

    // Speed readout, fixed position (not on the ring) for stability.
    char speedBuf[8];
    DialFormat::formatSpeedMph(m_snapshot.speedFeedback, speedBuf, sizeof(speedBuf));
    char speedLine[16];
    snprintf(speedLine, sizeof(speedLine), "%s mph", speedBuf);
    gfx.setTextColor(colText, col(Col::BG));
    gfx.drawString(speedLine, kCentreX, kSpeedReadoutY, &fonts::Font4);

    if (paused)
    {
        // Blink 1 Hz: on for the first 500 ms of each 1000 ms window.
        const uint8_t pulsePhase = static_cast<uint8_t>((nowMs / 500) % 2);
        if (pulsePhase == 0)
        {
            gfx.setTextColor(colText, col(Col::BG));
            gfx.drawString("PAUSED", kCentreX, kPausedY, &fonts::Font4);
        }
        gfx.setTextColor(colDim, col(Col::BG));
        gfx.drawString("tap resume - hold stop", kCentreX, kHintY, &fonts::Font2);
    }
}

// Centre speed overlay (I3): target speed big in Font7 amber with an "mph"
// caption, and the amber target ring — shared by every screen, called from
// draw() on top of whatever the current screen just painted, for as long as
// the overlay window from the last nudge (m_speedOverlayUntilMs) is open.
void DialUi::drawSpeedOverlay(LovyanGFX& gfx, bool paused)
{
    const uint32_t colTrack = paused ? col(Col::DIM_DIM)     : col(Col::DIM);
    const uint32_t colAmber = paused ? col(Col::PENDING_DIM) : col(Col::PENDING);

    gfx.fillArc(kRingCx, kRingCy, kRingOuter, kRingInner, kRingStartDeg,
                kRingStartDeg + kRingSweepDeg, colTrack);

    const float ringSweep = DialFormat::speedToAngle(m_targetSpeedMph);
    if (ringSweep > 0.0f)
    {
        gfx.fillArc(kRingCx, kRingCy, kRingOuter, kRingInner, kRingStartDeg,
                    kRingStartDeg + ringSweep, colAmber);
    }

    // Blank everything inside the ring so the screen's own centre content
    // (time, row, PAUSED label, hints) does not show through the overlay,
    // then put the status dots back since they sit inside that circle.
    gfx.fillCircle(kRingCx, kRingCy, kRingInner - 2, col(Col::BG));
    if (!(m_snapshot.status == TreadMillData::RUNNING && !paused))
    {
        drawStatusDots(gfx);
    }

    char centreBuf[16];
    DialFormat::formatSpeedMph(m_targetSpeedMph, centreBuf, sizeof(centreBuf));
    gfx.setTextColor(colAmber, col(Col::BG));
    gfx.drawString(centreBuf, kCentreX, kCentreY, &fonts::Font7);
    gfx.drawString("mph", kCentreX, kOverlayCaptionY, &fonts::Font2);
}

// Long-press-to-stop/cancel progress (I4): thin red arc just outside the
// speed ring — shared by every screen, called from draw() on top of
// whatever the current screen just painted, for as long as a hold is in
// progress.
void DialUi::drawHoldArc(LovyanGFX& gfx)
{
    gfx.fillArc(kRingCx, kRingCy, kHoldOuter, kHoldInner, kRingStartDeg,
                kRingStartDeg + kRingSweepDeg * m_holdProgress, col(Col::RED));
}

#endif // HAS_DIAL_UI

#if HAS_DIAL_UI
bool DialUi::isLogoRetried(const char* iata) const
{
    for (uint8_t i = 0; i < m_logoRetriedCount; i++)
    {
        if (strncmp(m_logoRetried[i], iata, 2) == 0) return true;
    }
    return false;
}

void DialUi::markLogoRetried(const char* iata)
{
    strncpy(m_logoRetried[m_logoRetriedNext], iata, 2);
    m_logoRetried[m_logoRetriedNext][2] = '\0';
    m_logoRetriedNext = (uint8_t)((m_logoRetriedNext + 1) % kLogoFailedSize);
    if (m_logoRetriedCount < kLogoFailedSize) m_logoRetriedCount++;
}
#endif
