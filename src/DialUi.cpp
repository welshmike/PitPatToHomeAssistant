#include "DialUi.h"
#if HAS_DIAL_UI

#include <Arduino.h>
#include <esp_log.h>

namespace {

// Three 8px status dots along the top: BLE, WiFi, MQTT, left to right.
constexpr int32_t kDotY      = 14;
constexpr int32_t kDotRadius = 4; // 8px diameter
constexpr int32_t kBleDotX   = 96;
constexpr int32_t kWifiDotX  = 120;
constexpr int32_t kMqttDotX  = 144;

constexpr uint16_t kColBg     = TFT_BLACK;
constexpr uint16_t kColText   = TFT_WHITE;
constexpr uint16_t kColDim    = TFT_DARKGREY;
constexpr uint16_t kColBleOn  = TFT_BLUE;
constexpr uint16_t kColNetOn  = TFT_GREEN;

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
    render();
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

    if (ev.longPress || ev.btnStop)
    {
        m_controller.stop();
#if DIAL_SOUND
        // Two short beeps 80 ms apart, non-blocking: play the first now and
        // let the scheduler below fire the second once its deadline passes.
        M5Dial.Speaker.tone(1500, 60);
        m_secondBeepPending = true;
        m_secondBeepDueMs = nowMs + 80;
#endif
    }

    if (ev.detents != 0)
    {
        m_controller.nudgeSpeed(ev.detents, nowMs);
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

const char* DialUi::statusName(TreadMillData::Status s)
{
    switch (s)
    {
    case TreadMillData::COUNTDOWN:    return "COUNTDOWN";
    case TreadMillData::RUNNING:      return "RUNNING";
    case TreadMillData::PAUSED:       return "PAUSED";
    case TreadMillData::STOPPED:      return "STOPPED";
    case TreadMillData::DISCONNECTED: return "DISCONNECTED";
    default:                          return "UNKNOWN";
    }
}

void DialUi::render()
{
    if (m_useCanvas)
    {
        draw(m_canvas);
        m_canvas.pushSprite(0, 0);
    }
    else
    {
        draw(M5Dial.Display);
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

void DialUi::draw(LovyanGFX& gfx)
{
    gfx.fillScreen(kColBg);
    gfx.setTextDatum(middle_center);

    drawStatusDots(gfx);

    gfx.setTextColor(kColText, kColBg);
    gfx.drawString(statusName(m_snapshot.status), 120, 60, &fonts::Font4);

    char line[32];
    snprintf(line, sizeof(line), "%.1f mph", m_snapshot.speedFeedback);
    gfx.drawString(line, 120, 130, &fonts::Font2);

    snprintf(line, sizeof(line), "%.2f km", m_snapshot.distanceKm);
    gfx.drawString(line, 120, 160, &fonts::Font2);
}

#endif // HAS_DIAL_UI
