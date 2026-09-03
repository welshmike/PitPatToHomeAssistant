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
    // Keeps touch/button/encoder state machines healthy even though nothing
    // reads their state yet.
    M5Dial.update();

    if (nowMs - m_lastRenderMs < kRenderIntervalMs)
    {
        return;
    }
    m_lastRenderMs = nowMs;
    render();
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
