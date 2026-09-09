#include <math.h>
#include "DialUi.h"
#if HAS_DIAL_UI

#include <Arduino.h>
#include <esp_log.h>
#include <string.h>
#include <time.h>

#include "DialFormat.h"
#include "TreadmillData.h"
#include "DialClockView.h"
#include "DialFlightsView.h"
#include "DialGlyphs.h"
#include "DialLightsView.h"
#include "DialMenuView.h"
#if HAS_CALENDAR
#include "DialCalendarView.h"
#endif
#include "LightLayout.h"

// -DUSE_ESP_IDF_LOG (spec 4.14) makes log_x() expand to
// ESP_LOG_LEVEL_LOCAL(..., TAG, ...); esp32-hal-log.h has no default TAG.
static const char *TAG = "DialUi";

namespace {

// Three 8px status dots along the top: BLE, WiFi, MQTT, left to right.
constexpr int32_t kDotY      = 14;
constexpr int32_t kDotRadius = 4; // 8px diameter
constexpr int32_t kBleDotX   = 96;
constexpr int32_t kWifiDotX  = 120;
constexpr int32_t kMqttDotX  = 144;

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

} // namespace

DialUi::DialUi(TreadmillController& controller, const TimeService& timeService,
               NetTask& net)
    : m_controller(controller), m_time(timeService), m_flights(net.flights()),
      m_net(net), m_lights(net.lights()),
      // Office has no colour control; the Lamp does (Plan 6). Indexed by
      // LightsModel::LightKey, so the order here is OFFICE then LAMP.
      m_lightCards{LightCardState(false), LightCardState(true)}
#if HAS_CALENDAR
      // Declared after the lights members, so it is initialised last here too
      // (NetTask::calendar() is safe before NetTask::begin(), like flights()).
      , m_calendar(net.calendar())
#endif
{
}

// Every colour goes through the theme: see DialTheme.h for what each Col
// means and for how LovyanGFX interprets the returned value on a 4bpp palette
// canvas versus the direct-to-display fallback.
uint32_t DialUi::col(Col c) const
{
    return m_theme.col(c);
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
    m_theme.useCanvas = m_canvas.createSprite(240, 240);
    if (m_theme.useCanvas)
    {
        m_canvas.createPalette();
        for (uint8_t i = 0; i < kDialPaletteSize; ++i)
        {
            m_canvas.setPaletteColor(i, dialPaletteRgb888(i));
        }
        m_canvas.setPaletteColor(static_cast<uint8_t>(Col::TRANSPARENT), 0x000000u);
    }
    log_i("DialUi: free heap after sprite = %u bytes", (unsigned)ESP.getFreeHeap());

    if (!m_theme.useCanvas)
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
        const bool beltIdle = isDeskScreen(screenNow);
        // The count is only evidence while the snapshot is actually live:
        // stale (HA quiet) or offline (MQTT down) data is passed as
        // dataValid=false, which the state machine treats as zero aircraft.
        const bool dataValid = !(m_flightsSnap.offline || m_flightsSnap.stale);
        const FlightsAutoShow::Action action =
            m_autoShow.update(nowMs, m_flightsSnap.count, m_cards.current(), beltIdle, dataValid);
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

#if HAS_CALENDAR
    // Calendar snapshot and nudge (spec 4.19), same shape as the two blocks
    // above: the snapshot is pulled on every tick whatever is on screen,
    // because the nudge needs the next meeting while the Clock card is up,
    // and the state machine is then run with it every tick.
    pollCalendar(nowMs);
    {
        // Wall-clock epoch, or 0 when TimeService has no time yet — which
        // CalendarAutoShow treats as "no usable data", so nothing nudges
        // before the clock is up.
        const uint32_t nowEpoch = m_time.valid() ? static_cast<uint32_t>(time(nullptr)) : 0;
        const bool dataValid = (nowEpoch != 0) && m_calSnap.valid &&
                               !CalendarModel::isStale(m_calSnap, nowEpoch);
        const int8_t idx = CalendarModel::nextTimed(m_calSnap, nowEpoch);
        const uint32_t nextStart = (idx >= 0) ? m_calSnap.ev[idx].start : 0;
        // Early refresh (spec 4.19): as an event's start closes in on the
        // nudge window, ask for a fetch newer than the 5-minute poll would
        // give — a meeting cancelled or moved in the last few minutes should
        // not raise the card. CalendarService rate-limits this to one fetch a
        // minute, so asking on every tick inside the window is free.
        if (idx >= 0 && dataValid && nextStart > nowEpoch &&
            (nextStart - nowEpoch) <= m_calNudge.leadSec() + 60)
        {
            m_calendar.requestRefresh();
        }
        const CalendarAutoShow::Action action =
            m_calNudge.update(nowEpoch, idx >= 0, nextStart, m_cards.current(),
                              isDeskScreen(screenNow), dataValid);
        if (action == CalendarAutoShow::Action::SHOW_CALENDAR)
        {
            m_cards.set(CardId::CALENDAR);
            // Nobody touched the Dial — wake the backlight ourselves so the
            // card that just raised itself is readable (as the Flights
            // auto-show above does).
            m_input.noteActivity(nowMs);
            applyBrightness();
        }
        else if (action == CalendarAutoShow::Action::RETURN_TO_CLOCK)
        {
            m_cards.set(CardId::CLOCK);
        }
        if (action != CalendarAutoShow::Action::NONE)
        {
            screenNow = currentScreen(isPausedState());
        }
    }
#endif

    // Idle return (spec 4.8 amendment): parked on any desk card but Clock
    // with no input for kIdleReturnMs -> back to Clock. Only while the belt
    // is idle (the resolved screen is a desk card; Connecting/Starting/
    // Running/Paused never get here), never over an open menu or selector
    // (they have their own shorter timeouts), and never while an auto-show
    // owns the card — a nudge raised 5 min ahead must outlive 2 min of
    // nobody touching the Dial. The auto-shows note activity when they raise
    // a card, so a card the user scrolled to *during* an auto-show is timed
    // from that scroll. navigateToCard() also lands the light cards on
    // their first page, as the hold-home gesture does.
    if (isDeskScreen(screenNow) && screenNow != Screen::MENU &&
        m_cards.current() != CardId::CLOCK && !m_autoShow.isShowing() &&
#if HAS_CALENDAR
        !m_calNudge.isShowing() &&
#endif
        m_input.idleFor(nowMs, kIdleReturnMs))
    {
        navigateToCard(CardId::CLOCK);
        screenNow = currentScreen(isPausedState());
    }

    // Flights card (spec 4.9): tell FlightsService whether the card is on
    // screen every tick (cheap atomic write), then run the visible card's
    // own housekeeping (idx clamp, wanted logo) while it is.
    m_flights.setVisible(screenNow == Screen::FLIGHTS);
    if (screenNow == Screen::FLIGHTS)
    {
        tickFlights();
    }

    // Lights cards (Plan 6, spec 4.12): same shape as pollFlights() above —
    // runs on every tick, whatever is on screen, so an edit made a moment
    // before the ring left the card still goes out 300 ms later.
    tickLights(nowMs);

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
    // BtnA wraps the Dial's single side button. wasClicked() is the instant
    // press-and-release edge, which is what the belt's emergency stop wants
    // (btnStop). wasSingleClicked() is the same press decided only after
    // M5Unified's multi-click window — one _msecHold, i.e. 500 ms after the
    // release — and wasHold() fires at the same 500 ms while still held: the
    // pair that drives the card menu and the jump home (spec 4.12). All three
    // are one-tick edges that auto-clear. Because one press yields both
    // wasClicked() and wasSingleClicked(), the btnStop path below tells
    // DialInput to swallow the second half when it acts on the first.
    const bool btnClicked = M5Dial.BtnA.wasClicked();
    const bool btnSingleClicked = M5Dial.BtnA.wasSingleClicked();
    const bool btnHeld = M5Dial.BtnA.wasHold();
    const bool btnDoubleClicked = M5Dial.BtnA.wasDoubleClicked();

    const DialEvents ev = m_input.tick(encoderCount, touch.isPressed(), touch.x, touch.y,
                                        btnClicked, btnSingleClicked, btnHeld, nowMs,
                                        btnDoubleClicked);
    m_holdProgress = ev.holdProgress;

    // Keep the screen awake through an active walk even with no touch/encoder
    // input at all.
    if (m_snapshot.status == TreadMillData::RUNNING || m_snapshot.status == TreadMillData::COUNTDOWN)
    {
        m_input.noteActivity(nowMs);
    }

    applyBrightness();

    // Selector/menu housekeeping (spec 4.7, 4.12): Connecting/Starting/Running
    // (paused included) always win over both, so close them as soon as any of
    // those becomes true — silently, no beep — before resolving the screen
    // below. Otherwise a stale selector or menu (opened before a start
    // elsewhere, e.g. HA) could resurface once the belt stops again.
    const bool beltScreen =
        (m_snapshot.status == TreadMillData::RUNNING || isPausedState() ||
         m_snapshot.status == TreadMillData::COUNTDOWN || m_controller.isConnecting());
    if (beltScreen)
    {
        if (m_selector.isOpen())
        {
            m_selector.close();
        }
        m_menu.close();
    }
    // Inactivity timeouts: each returns true (and closes) exactly once on the
    // tick that crosses its threshold. Both closes are silent, so neither
    // return value is needed here.
    m_selector.tick(nowMs);
    m_menu.tick(nowMs);

    const Screen screen = currentScreen(isPausedState());

    // Hue-ring scrub (spec 4.18 amendment). The claim is decided ONCE, on the
    // gesture's first tick: a touch that begins inside the ring's annulus on
    // the lit Lamp Colour page belongs to the ring until the finger lifts.
    // Every held tick then feeds the hue under the finger to the card, which
    // owns the send policy (LightCardState::scrub: first call selects, later
    // calls only on >= 1 deg movement from the last selection, page-idle
    // refreshed). claimTouch() mutes the swipe / long-press / tap the same
    // motion would otherwise produce, so a swipe that merely ARRIVES on the
    // Colour page is never hijacked, and the off face's tap-to-switch-on is
    // untouched because the claim requires view().on.
    if (ev.touchBegan && screen == Screen::LIGHT_LAMP && lightCardLive(screen))
    {
        LightCardState& card = lightCardFor(screen);
        if (card.view().on && card.page() == LightCardState::Page::COLOUR &&
            LightLayout::hitHueRing(ev.touchStartX, ev.touchStartY))
        {
            m_scrubbing = true;
            m_input.claimTouch();
            playAcceptBeep(true); // one tone per gesture, as a tap used to give
        }
    }
    if (m_scrubbing)
    {
        if (!ev.touchHeld || screen != Screen::LIGHT_LAMP)
        {
            m_scrubbing = false;
            lightCardFor(Screen::LIGHT_LAMP).endScrub();
        }
        else
        {
            lightCardFor(screen).scrub(LightLayout::hueAt(ev.touchX, ev.touchY), nowMs);
        }
    }

    if (ev.tap)
    {
        // The Connecting screen has no running belt to toggle — tap cancels
        // the in-flight connect there, same gesture as hold (C1).
        if (screen == Screen::CONNECTING)
        {
            const bool cancelled = m_controller.requestDisconnect();
            playStopBeep(nowMs, cancelled);
        }
        else if (screen == Screen::MENU)
        {
            // A tap on a glyph picks that card; anywhere else takes the
            // highlighted one, so the menu is never a dead end (spec 4.12).
            const int8_t hit = CardMenu::hitTest(ev.tapX, ev.tapY);
            const CardId target =
                (hit >= 0) ? static_cast<CardId>(hit) : m_menu.highlight();
            m_menu.close();
            navigateToCard(target);
            playAcceptBeep(true);
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
            // The user is reading the card, not just watching it appear: an
            // auto-show episode must not yank it back to Clock underneath
            // them when the count later drops (spec 4.11).
            m_autoShow.noteManualNavigation();
#if HAS_CALENDAR
            m_calNudge.noteManualNavigation();
#endif
            playAcceptBeep(true);
        }
#if HAS_CALENDAR
        else if (screen == Screen::CALENDAR)
        {
            // The card owns no value and starts nothing: a tap only dismisses
            // a nudge that raised it, which returns to the Clock card at once
            // (spec 4.19). Silent either way, like the Clock card's tap — the
            // dismiss is not a command that can be refused.
            if (m_calNudge.dismiss())
            {
                m_cards.set(CardId::CLOCK);
            }
        }
#endif
        else if (screen == Screen::LIGHT_OFFICE || screen == Screen::LIGHT_LAMP)
        {
            handleLightTap(screen, ev.tapX, ev.tapY, nowMs);
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
        else if (screen == Screen::LIGHT_OFFICE || screen == Screen::LIGHT_LAMP)
        {
            // Hold anywhere on a lit light card switches it off (spec 4.12);
            // on the off face there is nothing to hold for, and the whole
            // face is inert while MQTT is down or the card has no live state
            // (lightCardLive()).
            LightCardState& card = lightCardFor(screen);
            if (lightCardLive(screen) && card.view().on)
            {
                const LightsModel::Command cmd = card.powerOff(nowMs);
                if (cmd.type != LightsModel::Command::Type::NONE)
                {
                    publishLightCommand(lightKeyFor(screen), cmd);
                    playAcceptBeep(true);
                }
            }
        }
        else if (screen == Screen::CLOCK || screen == Screen::FLIGHTS ||
                 screen == Screen::CALENDAR || screen == Screen::MENU)
        {
            // None of these cards owns a value to skip/confirm, and the menu
            // is driven by the knob and taps — hold is a no-op, no beep. The
            // Calendar card belongs here rather than in the fall-through
            // below, which would try to stop a belt that isn't running.
        }
        else
        {
            const bool stopped = m_controller.stop();
            playStopBeep(nowMs, stopped);
        }
    }

    if (ev.btnStop)
    {
        // Emergency stop: the instant click edge stops the belt while it is
        // running/paused/counting down, and on Connecting it is unchanged —
        // it does NOT cancel the connect there (only tap/hold do); the write
        // is simply refused because the link is down, and that plays the
        // refused tone via the same feedback path (I1). On every other screen
        // the click is left to btnClick below, which decides it only after
        // M5Unified's multi-click window (spec 4.12).
        if (screen == Screen::RUNNING || screen == Screen::STARTING || screen == Screen::CONNECTING)
        {
            const bool stopped = m_controller.stop();
            playStopBeep(nowMs, stopped);
            // M5Unified reports this one press twice: wasClicked() now, then
            // wasSingleClicked() ~500 ms later. Tell DialInput to drop that
            // second half so the stop doesn't also open the card menu behind
            // itself. Only on the acted-on path — where btnStop is ignored,
            // the decided click is the legitimate menu gesture.
            m_input.consumeClick(nowMs);
        }
    }

    if (ev.btnHold && !beltScreen)
    {
        // Home: holding the side button returns to the Treadmill card,
        // closing any menu or selector first (spec 4.12). Belt idle only: the
        // belt screens already win over whichever card the ring is parked on,
        // so there is nothing to navigate to while one is up — and without
        // the guard a hold during a run would beep and re-point the ring
        // underneath it, for no visible effect.
        m_menu.close();
        if (m_selector.isOpen())
        {
            m_selector.close();
        }
        navigateToCard(CardId::TREADMILL);
        playAcceptBeep(true);
    }

    if (ev.btnDoubleClick && !beltScreen)
    {
        // Double click: straight to the Clock card from any desk card or the
        // menu (spec 4.12 amendment 2026-09-08), the same shape as hold-home
        // above but landing on the Clock, which is where the Dial rests.
        m_menu.close();
        if (m_selector.isOpen())
        {
            m_selector.close();
        }
        navigateToCard(CardId::CLOCK);
        playAcceptBeep(true);
    }

    if (ev.btnClick)
    {
        // Belt idle: the decided single click opens the card menu, and a
        // second one takes the highlighted card (spec 4.12).
        if (screen == Screen::MENU)
        {
            navigateToCard(m_menu.select());
            playAcceptBeep(true);
        }
        else if (screen == Screen::DISCONNECTED || screen == Screen::CLOCK ||
                 screen == Screen::FLIGHTS || screen == Screen::LIGHT_OFFICE ||
                 screen == Screen::LIGHT_LAMP || screen == Screen::CALENDAR)
        {
            // The two are never open at once, and this is what makes that
            // true rather than an accident of the branch list above: the
            // menu's open always closes the selector first. Silent — the
            // accept beep below covers the gesture.
            if (m_selector.isOpen())
            {
                m_selector.close();
            }
            m_menu.open(m_cards.current(), nowMs);
            playAcceptBeep(true);
        }
    }

    if (ev.detents != 0)
    {
        if (screen == Screen::MENU)
        {
            // The knob moves the highlight round the ring (wrapping) and
            // refreshes the menu's own idle timer.
            m_menu.detents(ev.detents, nowMs);
        }
        else if (screen == Screen::SELECTOR)
        {
            // The selector consumes detents itself; they must not reach
            // nudgeSpeed while it's open (4.7).
            m_selector.step(ev.detents, nowMs);
        }
        else if ((m_snapshot.status == TreadMillData::RUNNING) && !isPausedState())
        {
            // nudgeSpeed() synchronously fires onTargetSpeed(mph, pending=true)
            // via the controller's observer callback, so m_targetPending/
            // m_targetSpeedMph are already current by the time this returns —
            // arm the overlay deadline from the nowMs this call actually has.
            m_controller.nudgeSpeed(ev.detents, nowMs);
            m_speedOverlayUntilMs = nowMs + DIAL_SPEED_OVERLAY_MS;
        }
        else if (screen == Screen::FLIGHTS)
        {
            // The knob never scrolls cards any more (spec 4.12): here it
            // cycles aircraft, wrapping both ways.
            const int count = (m_flightsSnap.count > 0) ? m_flightsSnap.count : 1;
            int idx = (static_cast<int>(m_flightIdx) + ev.detents) % count;
            if (idx < 0)
            {
                idx += count;
            }
            m_flightIdx = static_cast<uint8_t>(idx);
            // Reading the card counts as driving it: an auto-show episode
            // must not yank it back to Clock underneath the user (4.11).
            m_autoShow.noteManualNavigation();
#if HAS_CALENDAR
            m_calNudge.noteManualNavigation();
#endif
        }
        else if ((screen == Screen::LIGHT_OFFICE || screen == Screen::LIGHT_LAMP) &&
                 lightCardLive(screen))
        {
            // The knob adjusts the page that is showing — brightness, kelvin
            // or hue. LightCardState ignores it while the light is
            // off, and the command follows 300 ms after the last detent,
            // from tickLights(). The lightCardLive() guard keeps the whole
            // face inert while MQTT is down or the card has no live state.
            lightCardFor(screen).detents(ev.detents, nowMs);
        }
        // Clock, Disconnected, Connecting and Starting own no value the knob
        // could turn: nothing happens there.
    }

    if (ev.swipe != 0)
    {
        if (screen == Screen::SELECTOR)
        {
            // Horizontal swipe is an alternate way to step the candidate speed
            // (right = +1 = faster), same grid as a detent (4.7).
            m_selector.step(ev.swipe, nowMs);
        }
        else if ((screen == Screen::LIGHT_OFFICE || screen == Screen::LIGHT_LAMP) &&
                 lightCardLive(screen))
        {
            // Carousel sense: dragging the face to the left (ev.swipe == -1)
            // brings the next page in, which is LightCardState's +1. Ignored
            // while the light is off — the off face has no pages — and, via
            // lightCardLive(), while MQTT is down or the card has no live
            // state.
            lightCardFor(screen).swipe(-ev.swipe, nowMs);
        }
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

// Tap on a light card (spec 4.12): the whole off face switches the light on;
// on the lit face only the small power glyph is a tap target here. On the
// Colour page a touch that starts on the hue ring is claimed by the scrub
// path in handleInput() before a tap can ever reach this function (spec 4.18
// amendment) — everything else is bare background — no beep, exactly like
// the Clock card's tap.
void DialUi::handleLightTap(Screen screen, int x, int y, uint32_t nowMs)
{
    // Hardware diagnostics (2026-09-06): taps on the bottom power glyph were
    // not registering; log where taps actually land on the light cards.
    log_i("Light tap at %d,%d", x, y);
    LightCardState& card = lightCardFor(screen);
    const LightsModel::LightState& v = card.view();

    if (!lightCardLive(screen))
    {
        // MQTT down ("waiting for HA") or no live state yet ("no data"): the
        // face is inert, and it says so on screen — a refused tone on top of
        // that is just noise. Silent, like a tap on the Clock card (and as
        // the README describes both faces).
        return;
    }

    if (!v.on)
    {
        const LightsModel::Command cmd = card.tapOn(nowMs);
        if (cmd.type != LightsModel::Command::Type::NONE)
        {
            // The light comes back on where it left off, on the Brightness
            // page (LightCardState::tapOn() resets it), and so does the other
            // card if the ring visits it next.
            resetLightPages();
            publishLightCommand(lightKeyFor(screen), cmd);
            playAcceptBeep(true);
        }
        return;
    }

    // Colour page: the hue ring is claimed on the touch-down tick by the
    // scrub path in handleInput(), so a ring touch never reaches here as a
    // tap; the power glyph is not drawn on the Colour page, so it is not a
    // target there either. A tap on the centre disc leaves the page (spec
    // 4.18 amendment): the disc sits inside the ring's touch annulus, so a
    // tap there is never claimed and always arrives as ev.tap.
    if (card.page() == LightCardState::Page::COLOUR && LightLayout::hitCentreDisc(x, y))
    {
        card.resetPage();
        playAcceptBeep(true);
        return;
    }

    if (card.page() != LightCardState::Page::COLOUR && LightLayout::hitPowerGlyph(x, y))
    {
        const LightsModel::Command cmd = card.powerOff(nowMs);
        if (cmd.type != LightsModel::Command::Type::NONE)
        {
            publishLightCommand(lightKeyFor(screen), cmd);
            playAcceptBeep(true);
        }
    }
}

// Menu selection and the hold-home gesture (spec 4.12). Both are the user
// driving the ring themselves, so both cancel any auto-show episode in
// progress (4.11), and both land the light cards back on their first page.
void DialUi::navigateToCard(CardId id)
{
    m_autoShow.noteManualNavigation();
#if HAS_CALENDAR
    m_calNudge.noteManualNavigation();
#endif
    resetLightPages();
    m_cards.set(id);
}

void DialUi::resetLightPages()
{
    for (uint8_t i = 0; i < static_cast<uint8_t>(LightsModel::LightKey::COUNT); ++i)
    {
        m_lightCards[i].resetPage();
    }
}

LightsModel::LightKey DialUi::lightKeyFor(Screen screen)
{
    return (screen == Screen::LIGHT_LAMP) ? LightsModel::LightKey::LAMP
                                          : LightsModel::LightKey::OFFICE;
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

#if HAS_CALENDAR
void DialUi::pollCalendar(uint32_t nowMs)
{
    // Same rhythm as pollFlights(), an order of magnitude slower: snapshot()
    // is a Guarded copy, and the relay only re-reads Google every 2 minutes.
    // Unsigned subtraction, so this stays correct across a millis() wrap.
    if (!m_haveCalSnap || (nowMs - m_lastCalSnapMs) >= kCalendarSnapIntervalMs)
    {
        m_lastCalSnapMs = nowMs;
        m_calSnap = m_calendar.snapshot();
        m_haveCalSnap = true;
    }
}
#endif

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
    // Same rhythm as pollFlights(): snapshot() is a Guarded copy — cheap, but
    // no reason to pay for it on every loop() iteration. Both cards are
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

    // Both cards' settle timers are polled every tick, whatever is showing:
    // an edit is committed 300 ms after the last detent even if the user has
    // walked the ring on to another card in the meantime (spec 4.12 — there
    // is no engage/release state left to discard it). tick() is a no-op on a
    // card with nothing pending.
    for (uint8_t i = 0; i < static_cast<uint8_t>(LightsModel::LightKey::COUNT); ++i)
    {
        const LightCardState::Page pageBefore = m_lightCards[i].page();
        const LightsModel::Command cmd = m_lightCards[i].tick(nowMs);
        if (m_lightCards[i].page() != pageBefore)
        {
            // Only tick() itself changes the page here: the PAGE_IDLE_MS fallback.
            log_i("Light %u page idle timeout -> Brightness", (unsigned)i);
        }
        if (cmd.type != LightsModel::Command::Type::NONE)
        {
            publishLightCommand(static_cast<LightsModel::LightKey>(i), cmd);
        }
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

LightCardState& DialUi::lightCardFor(Screen screen)
{
    return m_lightCards[static_cast<uint8_t>(lightKeyFor(screen))];
}

// Whether a touch-hold on `screen` has anything to do — i.e. whether the red
// hold arc is worth drawing, and so whether its progress belongs in the frame
// key at all. Not the menu, whose own ring sits exactly where the arc would
// draw and which a hold does nothing on; not a light card that is already
// off, where there is nothing left to switch.
bool DialUi::holdDoesSomething(Screen screen) const
{
    if (screen == Screen::MENU)
    {
        return false;
    }
    if (screen == Screen::LIGHT_OFFICE || screen == Screen::LIGHT_LAMP)
    {
        return lightCardLive(screen) && lightCardFor(screen).view().on;
    }
    return true;
}

const LightCardState& DialUi::lightCardFor(Screen screen) const
{
    return m_lightCards[static_cast<uint8_t>(lightKeyFor(screen))];
}

// Whether a light card is a live input target (spec 4.12): MQTT is up and
// the card's own state is both valid (parsed at least once) and available
// (HA currently reports the light reachable). Mirrors the gate
// handleLightTap() applied inline before this existed; now every light-card
// input path -- knob detents, swipe, hold and tap -- and holdDoesSomething()
// share this one predicate, so the face is inert as a whole rather than only
// its tap handler while MQTT is down.
bool DialUi::lightCardLive(Screen s) const
{
    if (m_netStatus != NetStatus::MQTT_UP)
    {
        return false;
    }
    const LightsModel::LightState& v = lightCardFor(s).view();
    return v.valid && v.available;
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

// Calendar nudge settings (spec 4.19). HA owns all three; TreadmillHandler
// persists them and main.cpp pushes them here at boot and on every change.
// Lead and stay come in whole minutes (the HA numbers) — CalendarAutoShow
// works in seconds. No-ops on a build without CALENDAR_URL, where there is no
// calendar card, no service and no nudge to configure.
void DialUi::setCalendarNudge(bool on)
{
#if HAS_CALENDAR
    m_calNudge.setEnabled(on);
#else
    (void)on;
#endif
}

void DialUi::setCalendarLeadMin(uint16_t mins)
{
#if HAS_CALENDAR
    m_calNudge.setLeadSec(static_cast<uint32_t>(mins) * 60u);
#else
    (void)mins;
#endif
}

void DialUi::setCalendarStayMin(uint16_t mins)
{
#if HAS_CALENDAR
    m_calNudge.setStaySec(static_cast<uint32_t>(mins) * 60u);
#else
    (void)mins;
#endif
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

// One Screen per desk card, plus DISCONNECTED (the Treadmill card with no
// belt) and MENU (a card-level overlay, not a belt screen): adding a card to
// the ring means adding its screen here, or the belt-idle test silently stops
// covering it. Both auto-show state machines take this as their beltIdle
// argument, so this is the one list that decides "the belt is idle and the
// user is looking at a card".
bool DialUi::isDeskScreen(Screen s)
{
    static_assert(static_cast<int>(CardId::COUNT) == 6,
                  "update the desk-card screen list in DialUi::isDeskScreen");
    return s == Screen::DISCONNECTED || s == Screen::CLOCK || s == Screen::FLIGHTS ||
           s == Screen::LIGHT_OFFICE || s == Screen::LIGHT_LAMP || s == Screen::CALENDAR ||
           s == Screen::MENU;
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
    // handleInput() closes the selector and the menu as soon as any of the
    // screens above becomes true, but currentScreen() is also called from
    // draw()/buildFrameKey() before that housekeeping runs on a given tick
    // (e.g. the very first render), so the ordering above is what actually
    // makes those screens win, not just the close calls.
    //
    // The menu and the selector are never open at once: opening the menu
    // closes the selector (handleInput()'s btnClick path), and confirming or
    // cancelling the selector closes it.
    if (m_menu.isOpen())
    {
        return Screen::MENU;
    }
    if (m_selector.isOpen())
    {
        return Screen::SELECTOR;
    }
    // Belt idle and no selector open: show whichever card the ring is
    // parked on (spec 4.8). TREADMILL is the existing Disconnected screen;
    // CLOCK is the clock card; FLIGHTS is the flights card (spec 4.9);
    // LIGHT_OFFICE/LIGHT_LAMP are the two HA lights cards (Plan 6); CALENDAR
    // is the calendar card (spec 4.19), which falls back to the clock screen
    // on a build without CALENDAR_URL.
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
    case CardId::CALENDAR:
#if HAS_CALENDAR
        return Screen::CALENDAR;
#else
        // No CALENDAR_URL, so no calendar service and no face to draw: the
        // card is still in the ring and the menu (CardId is not conditional),
        // and it lands on the Clock rather than an empty screen.
        return Screen::CLOCK;
#endif
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
    // Same predicate the hold arc draws under: on a screen where a hold does
    // nothing there is no arc to redraw, so letting holdUnits climb would
    // churn the key (and a full repaint per tick) for an identical frame.
    key.holdUnits       = holdDoesSomething(currentScreen(paused))
                              ? static_cast<int32_t>(m_holdProgress * 20.0f)
                              : 0;
    key.pulsePhase       = static_cast<uint8_t>((nowMs / 500) % 2);
    key.connectAttempts  = m_controller.connectAttempts();
    key.sessionDurationSec   = m_snapshot.sessionDurationSec;
    key.sessionDistanceCenti = static_cast<int32_t>(m_snapshot.sessionDistanceKm * 100.0f);
    key.sessionSteps         = m_snapshot.sessionSteps;
    key.selectorOpen   = m_selector.isOpen();
    key.selectorTenths = static_cast<int32_t>(m_selector.value() * 10.0f);
    key.cardId          = static_cast<uint8_t>(m_cards.current());
    // Card menu (spec 4.12): open/closed and which item is highlighted are
    // everything drawCardMenu() reads.
    key.menuOpen        = m_menu.isOpen();
    key.menuHighlight   = static_cast<uint8_t>(m_menu.highlight());

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
        // FNV-1a over the callsign and flight-number bytes (both are drawn
        // now), then folded in altFt/100, gsKt/10 (coarsened the same way
        // FrameKey coarsens speed/distance elsewhere) and the on-ground
        // flag — cheap, deterministic, no heap.
        const FlightsModel::Aircraft& a = m_flightsSnap.ac[m_flightIdx];
        uint32_t h = 2166136261u;
        for (const char* p = a.callsign; *p != '\0'; ++p)
        {
            h ^= static_cast<uint8_t>(*p);
            h *= 16777619u;
        }
        for (const char* p = a.flightNumber; *p != '\0'; ++p)
        {
            h ^= static_cast<uint8_t>(*p);
            h *= 16777619u;
        }
        h ^= static_cast<uint32_t>(a.onGround ? 1u : 0u);
        h *= 16777619u;
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
    // Radar empty state (spec 4.13): the sweep phase only belongs in the key
    // while it's actually what's on screen — screen == FLIGHTS, the list is
    // empty and HA isn't offline (the offline "waiting for HA" text wins
    // over the radar, spec 4.13 "Untouched"). Zero otherwise so no other
    // screen, and not even a populated or offline Flights card, redraws at
    // 10 Hz because of this field.
    key.radarPhase = (static_cast<Screen>(key.screen) == Screen::FLIGHTS &&
                       m_flightsSnap.count == 0 && !m_flightsSnap.offline)
                          ? static_cast<uint8_t>((nowMs / 100) % 30)
                          : 0;

    // Lights cards (spec 4.12): the page decides which face is drawn, so it
    // gets its own field alongside a FNV-1a over the light state the face
    // reads (key, valid, available, brightnessPct, kelvin, its min/max bounds,
    // hue, supportsColor, mode). editHue/settling drive the Colour page's
    // marker and every "an edit is in flight" amber, and `on` is the
    // off-face/on-face split — all cheap to compare directly. Zeroed when no
    // light card is showing, so the other screens never redraw on light state.
    key.lightMqttUp = (m_netStatus == NetStatus::MQTT_UP);
    const Screen lightScreen = static_cast<Screen>(key.screen);
    if (lightScreen == Screen::LIGHT_OFFICE || lightScreen == Screen::LIGHT_LAMP)
    {
        const LightCardState& card = lightCardFor(lightScreen);
        const LightsModel::LightState& v = card.view();
        uint32_t h = 2166136261u;
        const uint32_t fields[10] = {
            static_cast<uint32_t>(lightKeyFor(lightScreen)),
            static_cast<uint32_t>(v.valid),
            static_cast<uint32_t>(v.available),
            static_cast<uint32_t>(v.brightnessPct),
            static_cast<uint32_t>(v.kelvin),
            // The Kelvin page's bar marker and range caption move with the
            // bulb's own bounds, so a narrowed range has to redraw too.
            static_cast<uint32_t>(v.minKelvin),
            static_cast<uint32_t>(v.maxKelvin),
            static_cast<uint32_t>(static_cast<int>(v.hue)),
            static_cast<uint32_t>(v.supportsColor),
            // mode picks the live page (and the "not active" faces), so it
            // belongs in the hash too.
            static_cast<uint32_t>(v.mode),
        };
        for (uint32_t f : fields)
        {
            h ^= f;
            h *= 16777619u;
        }
        key.lightHash     = static_cast<uint16_t>(h ^ (h >> 16));
        key.lightPage     = static_cast<uint8_t>(card.page());
        key.lightHue      = static_cast<uint16_t>(lroundf(card.editHue())) % 360u;
        key.lightSettling = card.settling() || card.hueEditInFlight();
        key.lightOn       = v.on;
    }
    else
    {
        key.lightHash     = 0;
        key.lightPage     = 0;
        key.lightHue      = 0;
        key.lightSettling = false;
        key.lightOn       = false;
    }

#if HAS_CALENDAR
    // Calendar card (spec 4.19): FNV-1a over the snapshot fields the face
    // reads, every event start (a replaced list of the same length has to
    // redraw), the wall clock coarsened to whole minutes (so the countdown
    // and the "last update N min ago" line tick over once a minute, not 20
    // times a second), whether the first fetch has landed, and whether a
    // nudge is showing (a dismiss returns to the Clock, but the flag also
    // ends the amber episode). Zero unless the Calendar screen is up.
    if (static_cast<Screen>(key.screen) == Screen::CALENDAR)
    {
        const uint32_t nowEpoch = m_time.valid() ? static_cast<uint32_t>(time(nullptr)) : 0;
        uint32_t h = 2166136261u;
        const uint32_t fields[5] = {
            static_cast<uint32_t>(m_calSnap.valid),
            m_calSnap.fetchedAtEpoch,
            static_cast<uint32_t>(m_calSnap.count),
            nowEpoch / 60u,
            static_cast<uint32_t>(m_calendar.fetchedOnce() ? 1u : 0u) |
                static_cast<uint32_t>(m_calNudge.isShowing() ? 2u : 0u),
        };
        for (uint32_t f : fields)
        {
            h ^= f;
            h *= 16777619u;
        }
        for (uint8_t i = 0; i < m_calSnap.count; ++i)
        {
            h ^= m_calSnap.ev[i].start;
            h *= 16777619u;
        }
        key.calHash = static_cast<uint16_t>(h ^ (h >> 16));
    }
    else
    {
        key.calHash = 0;
    }
#endif

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
    // The Colour page's hue ring, marker and centre disc are true colour,
    // which a 16-entry palette cannot hold: the canvas leaves them as
    // TRANSPARENT holes and they are painted straight onto the display below.
    const Screen screen = static_cast<Screen>(key.screen);
    const bool lightTrueColour =
        (screen == Screen::LIGHT_OFFICE || screen == Screen::LIGHT_LAMP) &&
        lightTrueColourVisible(lightCardFor(screen), m_netStatus == NetStatus::MQTT_UP);

    if (m_theme.useCanvas)
    {
        draw(m_canvas, nowMs);
        // Explicit destination: m_canvas has no parent bound at construction
        // (see the m_canvas declaration in DialUi.h for why), so pushSprite()
        // needs to be told where to push rather than relying on one.
        if (m_logoToDraw[0] != '\0' || lightTrueColour)
        {
            // The frame left the logo rectangle (drawFlights) or the hue ring,
            // marker and centre disc (drawLight) filled with the TRANSPARENT
            // index, so this push leaves whatever is on the display there
            // untouched — the full-colour logo decoded earlier, or last
            // frame's colours, which paintLightTrueColour() repaints below.
            // No flicker.
            m_canvas.pushSprite(&M5Dial.Display, 0, 0, static_cast<uint32_t>(Col::TRANSPARENT));
        }
        else
        {
            m_canvas.pushSprite(&M5Dial.Display, 0, 0);
        }
        if (m_logoToDraw[0] == '\0')
        {
            m_logoOnScreen[0] = '\0'; // nothing reserved the logo rectangle this frame
        }
    }
    else
    {
        draw(M5Dial.Display, nowMs);
        m_logoOnScreen[0] = '\0'; // direct draw repaints everything each frame
    }

    // The hue ring, its marker and the selected colour in the middle, in true
    // colour rather than the palette's 16 (spec 4.12). Only on the frames the
    // Colour page is actually up. The direct-to-display fallback painted them
    // itself (there is no push to work around).
    if (lightTrueColour && m_theme.useCanvas)
    {
        paintLightTrueColour(M5Dial.Display, lightCardFor(screen));
    }

    // Airline logo in full colour, drawn straight onto the display (the palette
    // canvas would posterise it), and only when the airline changed.
    if (m_logoToDraw[0] != '\0' && strncmp(m_logoToDraw, m_logoOnScreen, 2) != 0)
    {
        // A raw-565 copy from LittleFS to the panel: no decoder, no heap, so
        // the only way this fails is a bad or short cache file.
        const bool drawn = dialDrawAirlineLogo(m_logoToDraw);
        if (drawn)
        {
            strncpy(m_logoOnScreen, m_logoToDraw, 2);
            m_logoOnScreen[2] = '\0';
        }
        else if (!isLogoRetried(m_logoToDraw))
        {
            // First failure for this airline: the cached file may be bad.
            // Drop it and let FlightsService download a fresh copy once.
            log_w("DialUi: logo %s draw failed, dropping cache and retrying once", m_logoToDraw);
            markLogoRetried(m_logoToDraw);
            m_flights.invalidateLogo(m_logoToDraw);
            m_logoOnScreen[0] = '\0';
            m_lastFrame.flightHash ^= 0x5A5A;
        }
        else
        {
            log_w("DialUi: logo %s draw failed again, using text fallback for this session", m_logoToDraw);
            markLogoDecodeFailed(m_logoToDraw);
            m_logoOnScreen[0] = '\0';
            m_lastFrame.flightHash ^= 0x5A5A; // force a redraw so the text fallback appears
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
                          screen != Screen::LIGHT_OFFICE && screen != Screen::LIGHT_LAMP &&
                          screen != Screen::CALENDAR && screen != Screen::MENU;
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
        drawFlights(gfx, nowMs);
        break;
    case Screen::LIGHT_OFFICE:
        drawLight(gfx, LightsModel::LightKey::OFFICE);
        break;
    case Screen::LIGHT_LAMP:
        drawLight(gfx, LightsModel::LightKey::LAMP);
        break;
#if HAS_CALENDAR
    case Screen::CALENDAR:
        drawCalendar(gfx);
        break;
#endif
    case Screen::MENU:
        drawCardMenu(gfx, m_theme, m_menu);
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

    // Long-press progress (I4): drawn on top of whichever screen just
    // painted, for as long as a hold is in progress — not just while
    // Running/Paused (e.g. holding to cancel from Connecting, or holding a
    // lit light card off, spec 4.12). Not on the menu, whose ring sits where
    // the arc would draw and which a hold does nothing on; and not on a light
    // card that is already off, where the hold has nothing to switch.
    if (m_holdProgress > 0.0f && holdDoesSomething(screen))
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

#if HAS_CALENDAR
namespace
{
// Google hands out UTC epochs and the face shows London times, so every event
// time goes through localtime_r() — TimeService::localTime() only ever
// formats *now*, which is no use for a meeting that starts at 14:30. The TZ
// is set once by TimeService (Europe/London), so localtime_r() here agrees
// with the clock card. Shared by drawClock() (spec 4.19 amendment) and
// drawCalendar() below.
bool calendarLocalTime(uint32_t epoch, struct tm& out)
{
    const time_t t = static_cast<time_t>(epoch);
    return localtime_r(&t, &out) != nullptr;
}
} // namespace
#endif

// Clock card (spec 4.8): the analogue face is drawn by DialClockView from the
// live TimeService — see there for the layout and the invalid-time state.
// Spec 4.19 "clock layout" amendment: when a calendar snapshot is fresh, also
// builds today's next-meeting title and time and hands them to
// drawClockCard() to draw below the centre. Mirrors the dataValid computation
// in the nudge block above (pollCalendar()'s caller); `m_calendar.fetchedOnce()`
// guards against a default-constructed (never-fetched) snapshot happening to
// look "valid".
void DialUi::drawClock(LovyanGFX& gfx)
{
#if HAS_CALENDAR
    char title[48] = {0};
    char when[16]  = {0};
    bool live = false;
    const uint32_t nowEpoch = m_time.valid() ? static_cast<uint32_t>(time(nullptr)) : 0;
    const bool dataValid = (nowEpoch != 0) && m_calSnap.valid &&
                           !CalendarModel::isStale(m_calSnap, nowEpoch);
    if (m_calendar.fetchedOnce() && dataValid)
    {
        setCalendarLocalTime(&calendarLocalTime);
        CalendarModel::clockLine(m_calSnap, nowEpoch, &calendarLocalDayOf, &calendarHhmm,
                                  title, sizeof(title), when, sizeof(when), live);
    }
    drawClockCard(gfx, m_theme, m_time, title[0] ? title : nullptr, when[0] ? when : nullptr,
                  live);
#else
    drawClockCard(gfx, m_theme, m_time);
#endif
}

// Flights card (spec 4.9): decides whether the airline logo will be pushed
// onto the display after this frame (and remembers which one for render()),
// then hands the drawing to DialFlightsView. m_flightsSnap is written only by
// pollFlights() and m_flightIdx only by handleInput()/tickFlights()/tick()
// (all on the loop task), so this is a plain read — no locking needed on top
// of Guarded's own. The radar phase (spec 4.13) is derived from nowMs the
// same way buildFrameKey() derives FrameKey::radarPhase — see that field's
// comment for why the redraw-gating copy lives there instead of being passed
// through here.
void DialUi::drawFlights(LovyanGFX& gfx, uint32_t nowMs)
{
    bool haveLogo = false;
    if (!m_flightsSnap.offline && m_flightsSnap.count > 0)
    {
        const uint8_t idx = (m_flightIdx < m_flightsSnap.count) ? m_flightIdx : 0;
        const FlightsModel::Aircraft& ac = m_flightsSnap.ac[idx];
        // Only (re)decode a logo the net task actually has cached and that
        // hasn't already failed to decode this session (I4).
        const bool wantLogo = ac.airlineIata[0] != '\0' && !isLogoDecodeFailed(ac.airlineIata);
        // The raw-565 logo push needs no heap (spec 4.11 amendment
        // 2026-09-09), so a cached logo is always drawable this frame.
        haveLogo = wantLogo && m_flights.logoReady(ac.airlineIata);
        if (haveLogo)
        {
            strncpy(m_logoToDraw, ac.airlineIata, 2);
            m_logoToDraw[2] = '\0';
        }
    }
    const uint8_t radarPhase = (!m_flightsSnap.offline && m_flightsSnap.count == 0)
                                   ? static_cast<uint8_t>((nowMs / 100) % 30)
                                   : 0;
    drawFlightsCard(gfx, m_theme, m_flightsSnap, m_flightIdx, haveLogo, radarPhase);
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

// Lights card (spec 4.12). Everything on the face comes from the card's own
// state and LightLayout's geometry — see DialLightsView.cpp. The Colour page's
// hue ring, marker and centre disc are left as TRANSPARENT holes here and
// filled in true colour by render() after the push.
void DialUi::drawLight(LovyanGFX& gfx, LightsModel::LightKey key)
{
    const LightCardState& card = m_lightCards[static_cast<uint8_t>(key)];
    drawLightCard(gfx, m_theme, card,
                  (key == LightsModel::LightKey::LAMP) ? "Lamp" : "Office",
                  m_netStatus == NetStatus::MQTT_UP);
}

#if HAS_CALENDAR
void DialUi::drawCalendar(LovyanGFX& gfx)
{
    const uint32_t nowEpoch = m_time.valid() ? static_cast<uint32_t>(time(nullptr)) : 0;
    drawCalendarCard(gfx, m_theme, m_calSnap, nowEpoch, &calendarLocalTime,
                     m_calendar.fetchedOnce());
}
#endif

void DialUi::drawConnecting(LovyanGFX& gfx)
{
    gfx.setTextColor(col(Col::TEXT), col(Col::BG));
    gfx.drawString("Connecting...", kCentreX, kConnLabelY, &fonts::Font4);

    // Only show "attempt N" once there has actually been one — attempt 0
    // (the brief window before the first beginLink() call) just reads
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
    // Paused dims each colour to its Col::*_DIM palette entry — see the
    // palette comment in DialTheme.h for how those shades were derived
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
