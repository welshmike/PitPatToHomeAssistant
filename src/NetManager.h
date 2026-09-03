#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <functional>

#include "NetStatus.h"

class NetManager
{
public:
    NetManager(const char *ssid, const char *pass, const char *mqttHost, uint16_t mqttPort,
               const char *mqttUser, const char *mqttPass);

    // Kicks off the first WiFi association. Returns immediately — never blocks
    // waiting for an IP. `hostname` is copied (callers pass a temporary String).
    void begin(const char *hostname);

    // Drives the state machine. Call every loop iteration with millis().
    // Worst case blocking is one MQTT connect attempt (5 s socket timeout),
    // and only once per backoff interval.
    void tick(uint32_t nowMs);

    NetStatus status() const { return m_status; }
    PubSubClient &mqtt() { return m_client; }

    // Invoked once each time the MQTT session is (re)established: subscribe and
    // publish discovery configs / retained states from here.
    void onMqttConnected(std::function<void()> cb) { m_onMqttConnected = std::move(cb); }
    void setMqttCallback(MQTT_CALLBACK_SIGNATURE);

    bool wifiUp() const;
    bool mqttUp() const;

private:
    void setStatus(NetStatus next);
    void startWifi(uint32_t nowMs);
    void onWifiUp(uint32_t nowMs);
    void attemptMqtt(uint32_t nowMs);
    void startOtaOnce();
    // Backoff step for `idx`, clamped to the last (30 s) entry.
    static uint32_t backoffMs(uint8_t idx);
    static const char *statusName(NetStatus s);

    const char *m_ssid;
    const char *m_pass;
    const char *m_mqttHost;
    const char *m_mqttUser;
    const char *m_mqttPass;
    uint16_t m_mqttPort;

    char m_hostname[48] = {0}; // also the MQTT client id

    WiFiClient m_net;
    PubSubClient m_client;

    NetStatus m_status = NetStatus::WIFI_DOWN;
    uint32_t m_nextAttemptMs = 0;   // earliest time the current state may retry
    uint32_t m_wifiAttemptMs = 0;   // when the in-flight WiFi association started
    uint8_t m_wifiBackoffIdx = 0;
    uint8_t m_mqttBackoffIdx = 0;
    bool m_otaStarted = false;

    std::function<void()> m_onMqttConnected;
};
