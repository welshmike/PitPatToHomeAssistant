#include "DialUi.h"
#if HAS_DIAL_UI

#include <Arduino.h>
#include <esp_log.h>

#include "DialFormat.h"
#include "TreadmillData.h"

namespace {

// Three 8px status dots along the top: BLE, WiFi, MQTT, left to right.
constexpr int32_t kDotY      = 14;
constexpr int32_t kDotRadius = 4; // 8px diameter
constexpr int32_t kBleDotX   = 96;
constexpr int32_t kWifiDotX  = 120;
constexpr int32_t kMqttDotX  = 144;

constexpr uint16_t kColBg        = TFT_BLACK;
constexpr uint16_t kColText      = TFT_WHITE;
constexpr uint16_t kColDim       = TFT_DARKGREY;
constexpr uint16_t kColBleOn     = TFT_BLUE;
constexpr uint16_t kColNetOn     = TFT_GREEN;
constexpr uint16_t kColSpeedVal  = TFT_CYAN;  // ring/centre while not pending
constexpr uint16_t kColPending   = TFT_ORANGE; // ring/centre while a nudge is settling or overlaid
constexpr uint16_t kColHold      = TFT_RED;    // long-press-to-stop progress arc

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

// Halves each RGB565 channel — the ~50% dim used for the whole paused
// layout. Cheap bit-twiddling, no float, safe to call once per element per
// frame.
uint16_t dimColor565(uint16_t c)
{
    const uint16_t r = (c >> 11) & 0x1F;
    const uint16_t g = (c >> 5) & 0x3F;
    const uint16_t b = c & 0x1F;
    return ((r >> 1) << 11) | ((g >> 1) << 5) | (b >> 1);
}

} // namespace

DialUi::DialUi(TreadmillController& controller) : m_controller(controller)
{
}

void DialUi::begin()
{
    // M5Unified owns display/I2C/Serial init on the Dial — this must be the
    // first thing setup() does.
    auto cfg = M5.config();
    cfg.serial_baudrate = 115200;
    cfg.internal_imu = false;
    cfg.internal_rtc = false;
    M5Dial.begin(cfg, true, false);

    M5Dial.Display.setTextDatum(middle_center);
    M5Dial.Display.fillScreen(kColBg);
    M5Dial.Display.setBrightness(kBrightFull);

    log_i("DialUi: free heap before sprite = %u bytes", (unsigned)ESP.getFreeHeap());
    m_canvas.setColorDepth(16);
    m_useCanvas = m_canvas.createSprite(240, 240);
    log_i("DialUi: free heap after sprite = %u bytes", (unsigned)ESP.getFreeHeap());

    if (!m_useCanvas)
    {
        log_e("DialUi: canvas createSprite(240,240) failed, drawing directly to M5Dial.Display");
    }
    else
    {
        m_canvas.setTextDatum(middle_center);
    }
}

void DialUi::tick(uint32_t nowMs)
{
    // Pumps M5Dial's own touch/button/encoder debouncing, then feeds those
    // readings into DialInput and acts on whatever intents come back. Runs
    // every call, independent of the render throttle below.
    M5Dial.update();
    handleInput(nowMs);

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

    if (ev.tap)
    {
        // The Connecting screen has no running belt to toggle — tap cancels
        // the in-flight connect there, same gesture as hold (C1).
        if (currentScreen(isPausedState()) == Screen::CONNECTING)
        {
            const bool cancelled = m_controller.requestDisconnect();
            playStopBeep(nowMs, cancelled);
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
#if DIAL_SOUND
            if (refused)
            {
                M5Dial.Speaker.tone(400, 120);
            }
            else
            {
                M5Dial.Speaker.tone(2000, 40);
            }
#endif
        }
    }

    if (ev.longPress)
    {
        // The Connecting screen's hold gesture cancels the in-flight connect
        // rather than stopping a belt that isn't running yet.
        if (currentScreen(isPausedState()) == Screen::CONNECTING)
        {
            const bool cancelled = m_controller.requestDisconnect();
            playStopBeep(nowMs, cancelled);
        }
        else
        {
            const bool stopped = m_controller.stop();
            playStopBeep(nowMs, stopped);
        }
    }

    if (ev.btnStop)
    {
        // Emergency stop: the side button always sends stop(), on every
        // screen — including Connecting, where it does NOT cancel the
        // connect (only tap/hold do that there); the write is simply
        // refused because the link is down, and that plays the refused tone
        // via the same feedback path (I1).
        const bool stopped = m_controller.stop();
        playStopBeep(nowMs, stopped);
    }

    if (ev.detents != 0)
    {
        // nudgeSpeed() synchronously fires onTargetSpeed(mph, pending=true)
        // via the controller's observer callback, so m_targetPending/
        // m_targetSpeedMph are already current by the time this returns —
        // arm the overlay deadline from the nowMs this call actually has.
        m_controller.nudgeSpeed(ev.detents, nowMs);
        m_speedOverlayUntilMs = nowMs + DIAL_SPEED_OVERLAY_MS;
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
    case DialInput::Backlight::OFF:  level = kBrightOff;  break;
    }
    M5Dial.Display.setBrightness(level);
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
    if (m_snapshot.status == TreadMillData::RUNNING || paused)
    {
        return Screen::RUNNING;
    }
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
    return Screen::DISCONNECTED;
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

    if (m_useCanvas)
    {
        draw(m_canvas, nowMs);
        // Explicit destination: m_canvas has no parent bound at construction
        // (see the m_canvas declaration in DialUi.h for why), so pushSprite()
        // needs to be told where to push rather than relying on one.
        m_canvas.pushSprite(&M5Dial.Display, 0, 0);
    }
    else
    {
        draw(M5Dial.Display, nowMs);
    }
}

void DialUi::drawStatusDots(LovyanGFX& gfx)
{
    const bool bleUp  = m_snapshot.status != TreadMillData::DISCONNECTED;
    const bool wifiUp = m_netStatus >= NetStatus::WIFI_UP;
    const bool mqttUp = m_netStatus == NetStatus::MQTT_UP;

    gfx.fillCircle(kBleDotX,  kDotY, kDotRadius, bleUp  ? kColBleOn : kColDim);
    gfx.fillCircle(kWifiDotX, kDotY, kDotRadius, wifiUp ? kColNetOn : kColDim);
    gfx.fillCircle(kMqttDotX, kDotY, kDotRadius, mqttUp ? kColNetOn : kColDim);
}

void DialUi::draw(LovyanGFX& gfx, uint32_t nowMs)
{
    gfx.fillScreen(kColBg);
    gfx.setTextDatum(middle_center);

    drawStatusDots(gfx);

    const bool paused = isPausedState();
    switch (currentScreen(paused))
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
    case Screen::DISCONNECTED:
    default:
        drawDisconnected(gfx);
        break;
    }

    // Speed overlay (I3): drawn on top of whichever screen just painted,
    // for as long as the overlay window from the last nudge is open — not
    // just while Running. See the m_speedOverlayUntilMs comment in DialUi.h.
    const bool overlayActive = (int32_t)(m_speedOverlayUntilMs - nowMs) > 0;
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
    gfx.setTextColor(kColText, kColBg);
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
    gfx.setTextColor(kColText, kColBg);
    gfx.drawString(distBuf, kRowLeftX, kDiscRowY, &fonts::Font4);
    gfx.setTextColor(kColDim, kColBg);
    gfx.drawString("km", kRowLeftX, kDiscCaptionY, &fonts::Font2);

    char stepsBuf[12];
    DialFormat::formatSteps(m_snapshot.sessionSteps, stepsBuf, sizeof(stepsBuf));
    gfx.setTextColor(kColText, kColBg);
    gfx.drawString(stepsBuf, kRowRightX, kDiscRowY, &fonts::Font4);
    gfx.setTextColor(kColDim, kColBg);
    gfx.drawString("steps", kRowRightX, kDiscCaptionY, &fonts::Font2);

    gfx.setTextColor(kColDim, kColBg);
    gfx.drawString("tap or turn to start", kCentreX, kDiscHintY, &fonts::Font2);
}

void DialUi::drawConnecting(LovyanGFX& gfx)
{
    gfx.setTextColor(kColText, kColBg);
    gfx.drawString("Connecting...", kCentreX, kConnLabelY, &fonts::Font4);

    // Only show "attempt N" once there has actually been one — attempt 0
    // (the brief window before the first connectToDevice() call) just reads
    // "Connecting..." with no attempt line (I2).
    const uint16_t attempts = m_controller.connectAttempts();
    if (attempts > 0)
    {
        char attemptBuf[24];
        snprintf(attemptBuf, sizeof(attemptBuf), "attempt %u", (unsigned)attempts);
        gfx.setTextColor(kColDim, kColBg);
        gfx.drawString(attemptBuf, kCentreX, kConnAttemptY, &fonts::Font2);
    }

    gfx.setTextColor(kColDim, kColBg);
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
        gfx.setTextColor(kColText, kColBg);
        gfx.drawString("STARTING", kCentreX, kStartLabelY, &fonts::Font4);
    }
    gfx.setTextColor(kColDim, kColBg);
    gfx.drawString("tap to cancel", kCentreX, kStartHintY, &fonts::Font2);
}

void DialUi::drawRunning(LovyanGFX& gfx, bool paused, uint32_t nowMs)
{
    const uint16_t colTrack = paused ? dimColor565(kColDim)      : kColDim;
    const uint16_t colText  = paused ? dimColor565(kColText)     : kColText;
    const uint16_t colDim   = paused ? dimColor565(kColDim)      : kColDim;
    const uint16_t colCyan  = paused ? dimColor565(kColSpeedVal) : kColSpeedVal;

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
    gfx.setTextColor(colText, kColBg);
    gfx.drawString(centreBuf, kCentreX, kCentreY, &fonts::Font7);

    // Row: distance (left) / steps (right), each with a caption beneath.
    char distBuf[8];
    DialFormat::formatDistanceKm(m_snapshot.distanceKm, distBuf, sizeof(distBuf));
    gfx.setTextColor(colText, kColBg);
    gfx.drawString(distBuf, kRowLeftX, kRowY, &fonts::Font4);
    gfx.setTextColor(colDim, kColBg);
    gfx.drawString("km", kRowLeftX, kRowCaptionY, &fonts::Font2);

    char stepsBuf[12];
    DialFormat::formatSteps(m_snapshot.steps, stepsBuf, sizeof(stepsBuf));
    gfx.setTextColor(colText, kColBg);
    gfx.drawString(stepsBuf, kRowRightX, kRowY, &fonts::Font4);
    gfx.setTextColor(colDim, kColBg);
    gfx.drawString("steps", kRowRightX, kRowCaptionY, &fonts::Font2);

    // Speed readout, fixed position (not on the ring) for stability.
    char speedBuf[8];
    DialFormat::formatSpeedMph(m_snapshot.speedFeedback, speedBuf, sizeof(speedBuf));
    char speedLine[16];
    snprintf(speedLine, sizeof(speedLine), "%s mph", speedBuf);
    gfx.setTextColor(colText, kColBg);
    gfx.drawString(speedLine, kCentreX, kSpeedReadoutY, &fonts::Font4);

    if (paused)
    {
        // Blink 1 Hz: on for the first 500 ms of each 1000 ms window.
        const uint8_t pulsePhase = static_cast<uint8_t>((nowMs / 500) % 2);
        if (pulsePhase == 0)
        {
            gfx.setTextColor(colText, kColBg);
            gfx.drawString("PAUSED", kCentreX, kPausedY, &fonts::Font4);
        }
        gfx.setTextColor(colDim, kColBg);
        gfx.drawString("tap resume - hold stop", kCentreX, kHintY, &fonts::Font2);
    }
}

// Centre speed overlay (I3): target speed big in Font7 amber with an "mph"
// caption, and the amber target ring — shared by every screen, called from
// draw() on top of whatever the current screen just painted, for as long as
// the overlay window from the last nudge (m_speedOverlayUntilMs) is open.
void DialUi::drawSpeedOverlay(LovyanGFX& gfx, bool paused)
{
    const uint16_t colTrack = paused ? dimColor565(kColDim)     : kColDim;
    const uint16_t colAmber = paused ? dimColor565(kColPending) : kColPending;

    gfx.fillArc(kRingCx, kRingCy, kRingOuter, kRingInner, kRingStartDeg,
                kRingStartDeg + kRingSweepDeg, colTrack);

    const float ringSweep = DialFormat::speedToAngle(m_targetSpeedMph);
    if (ringSweep > 0.0f)
    {
        gfx.fillArc(kRingCx, kRingCy, kRingOuter, kRingInner, kRingStartDeg,
                    kRingStartDeg + ringSweep, colAmber);
    }

    char centreBuf[16];
    DialFormat::formatSpeedMph(m_targetSpeedMph, centreBuf, sizeof(centreBuf));
    gfx.setTextColor(colAmber, kColBg);
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
                kRingStartDeg + kRingSweepDeg * m_holdProgress, kColHold);
}

#endif // HAS_DIAL_UI
