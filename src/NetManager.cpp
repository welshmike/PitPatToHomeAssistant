#include "NetManager.h"

#include <ArduinoOTA.h>
#include <esp_task_wdt.h>

namespace
{
// Retry ladder, shared by WiFi and MQTT. Reset to the first step on success.
constexpr uint32_t kBackoff[] = {1000, 2000, 5000, 10000, 30000};
constexpr uint8_t kBackoffCount = sizeof(kBackoff) / sizeof(kBackoff[0]);

// How long to wait for an association before tearing it down and retrying.
constexpr uint32_t kWifiConnectTimeoutMs = 15000;

// Bounds how long PubSubClient::connect() can block waiting for CONNACK.
constexpr uint16_t kMqttSocketTimeoutS = 5;

constexpr uint16_t kMqttBufferSize = 1024;

// millis()-rollover-safe "deadline has passed".
inline bool due(uint32_t nowMs, uint32_t deadlineMs)
{
    return (int32_t)(nowMs - deadlineMs) >= 0;
}
} // namespace

NetManager::NetManager(const char *ssid, const char *pass, const char *mqttHost, uint16_t mqttPort,
                       const char *mqttUser, const char *mqttPass)
    : m_ssid(ssid), m_pass(pass), m_mqttHost(mqttHost), m_mqttUser(mqttUser), m_mqttPass(mqttPass),
      m_mqttPort(mqttPort), m_client(m_net)
{
}

const char *NetManager::statusName(NetStatus s)
{
    switch (s)
    {
    case NetStatus::WIFI_DOWN:       return "WIFI_DOWN";
    case NetStatus::WIFI_CONNECTING: return "WIFI_CONNECTING";
    case NetStatus::WIFI_UP:         return "WIFI_UP";
    case NetStatus::MQTT_CONNECTING: return "MQTT_CONNECTING";
    case NetStatus::MQTT_UP:         return "MQTT_UP";
    }
    return "?";
}

uint32_t NetManager::backoffMs(uint8_t idx)
{
    return kBackoff[idx < kBackoffCount ? idx : (kBackoffCount - 1)];
}

void NetManager::setStatus(NetStatus next)
{
    if (next == m_status)
    {
        return;
    }
    log_i("Net: %s -> %s", statusName(m_status), statusName(next));
    m_status = next;
}

bool NetManager::wifiUp() const
{
    return m_status == NetStatus::WIFI_UP || m_status == NetStatus::MQTT_CONNECTING ||
           m_status == NetStatus::MQTT_UP;
}

bool NetManager::mqttUp() const
{
    return m_status == NetStatus::MQTT_UP;
}

void NetManager::setMqttCallback(MQTT_CALLBACK_SIGNATURE)
{
    m_client.setCallback(callback);
}

void NetManager::begin(const char *hostname)
{
    if (hostname != nullptr)
    {
        strncpy(m_hostname, hostname, sizeof(m_hostname) - 1);
    }

    m_client.setBufferSize(kMqttBufferSize);
    m_client.setSocketTimeout(kMqttSocketTimeoutS);
    m_client.setServer(m_mqttHost, m_mqttPort);

    WiFi.setHostname(m_hostname);
    WiFi.mode(WIFI_STA);
    // select the AP with the strongest signal
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);

    startWifi(millis());
}

void NetManager::startWifi(uint32_t nowMs)
{
    log_i("WiFi: associating with SSID %s", m_ssid);
    WiFi.begin(m_ssid, m_pass);
    m_wifiAttemptMs = nowMs;
    setStatus(NetStatus::WIFI_CONNECTING);
}

void NetManager::onWifiUp(uint32_t nowMs)
{
    m_wifiBackoffIdx = 0;
    log_i("WiFi up: SSID %s, IP %s", m_ssid, WiFi.localIP().toString().c_str());
    setStatus(NetStatus::WIFI_UP);
    startOtaOnce();
    // Try MQTT straight away; the ladder only applies after a failure.
    m_mqttBackoffIdx = 0;
    m_nextAttemptMs = nowMs;
}

void NetManager::attemptMqtt(uint32_t nowMs)
{
    (void)nowMs;
    setStatus(NetStatus::MQTT_CONNECTING);

    const bool ok = (m_mqttUser == nullptr || strlen(m_mqttUser) == 0)
                        ? m_client.connect(m_hostname)
                        : m_client.connect(m_hostname, m_mqttUser, m_mqttPass);

    if (ok)
    {
        m_mqttBackoffIdx = 0;
        setStatus(NetStatus::MQTT_UP);
        if (m_onMqttConnected)
        {
            m_onMqttConnected();
        }
        return;
    }

    const uint32_t delayMs = backoffMs(m_mqttBackoffIdx);
    if (m_mqttBackoffIdx + 1 < kBackoffCount)
    {
        m_mqttBackoffIdx++;
    }
    log_w("MQTT connect failed (state %d), retrying in %u ms", m_client.state(), delayMs);
    // Measure the backoff from now: connect() may have blocked for seconds.
    m_nextAttemptMs = millis() + delayMs;
    setStatus(NetStatus::WIFI_UP);
}

void NetManager::startOtaOnce()
{
    if (m_otaStarted)
    {
        return;
    }
    m_otaStarted = true;

    ArduinoOTA.onStart([]()
                       {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else { // U_FS
      type = "filesystem";
    }

    // NOTE: if updating FS this would be the place to unmount FS using FS.end()
    log_i("Start updating %s", type.c_str()); });
    ArduinoOTA.onEnd([]()
                     { log_i("End"); });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
                          {
// reset watchdog during update
#ifdef ESP32
            esp_task_wdt_reset();
#endif
        log_i("Progress: %u%%\r", (progress / (total / 100))); });
    ArduinoOTA.onError([](ota_error_t error)
                       {
    log_e("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      log_e("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      log_e("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      log_e("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      log_e("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      log_e("End Failed");
    } });
    // Also starts mDNS.
    ArduinoOTA.begin();
    log_i("OTA ready");
}

void NetManager::tick(uint32_t nowMs)
{
    const bool associated = (WiFi.status() == WL_CONNECTED);

    // Losing WiFi drops us out of any higher state, whatever we were doing.
    if (!associated && m_status != NetStatus::WIFI_DOWN && m_status != NetStatus::WIFI_CONNECTING)
    {
        log_w("WiFi lost");
        // Close the socket directly rather than PubSubClient::disconnect(): that
        // writes a DISCONNECT packet, and a write on a socket whose link has just
        // gone away blocks for up to 10 s (WiFiClient retries select() 10x1s) —
        // long enough to lose the belt (6 s BLE supervision timeout). PubSubClient
        // notices the closed socket on its next connect()/connected() call.
        m_net.stop();
        m_wifiBackoffIdx = 0;
        m_nextAttemptMs = nowMs; // re-associate immediately
        setStatus(NetStatus::WIFI_DOWN);
    }

    switch (m_status)
    {
    case NetStatus::WIFI_DOWN:
        if (due(nowMs, m_nextAttemptMs))
        {
            startWifi(nowMs);
        }
        break;

    case NetStatus::WIFI_CONNECTING:
        if (associated)
        {
            onWifiUp(nowMs);
        }
        else if (nowMs - m_wifiAttemptMs >= kWifiConnectTimeoutMs)
        {
            const uint32_t delayMs = backoffMs(m_wifiBackoffIdx);
            if (m_wifiBackoffIdx + 1 < kBackoffCount)
            {
                m_wifiBackoffIdx++;
            }
            log_w("WiFi association timed out, retrying in %u ms", delayMs);
            WiFi.disconnect();
            m_nextAttemptMs = nowMs + delayMs;
            setStatus(NetStatus::WIFI_DOWN);
        }
        break;

    case NetStatus::WIFI_UP:
        ArduinoOTA.handle();
        if (due(nowMs, m_nextAttemptMs))
        {
            attemptMqtt(nowMs);
        }
        break;

    case NetStatus::MQTT_CONNECTING:
        // Transient: attemptMqtt() always leaves this state before returning.
        break;

    case NetStatus::MQTT_UP:
        ArduinoOTA.handle();
        if (!m_client.connected())
        {
            // Backoff was reset on connect, so the first retry is one step in.
            m_nextAttemptMs = nowMs + backoffMs(m_mqttBackoffIdx);
            if (m_mqttBackoffIdx + 1 < kBackoffCount)
            {
                m_mqttBackoffIdx++;
            }
            log_w("MQTT disconnected (state %d)", m_client.state());
            setStatus(NetStatus::WIFI_UP);
        }
        else
        {
            m_client.loop();
        }
        break;
    }
}
