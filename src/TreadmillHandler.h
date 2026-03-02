#pragma once
#include <Arduino.h>
#include <NimBLEDevice.h>

#include "platform.h"

class TreadmillHandler : public NimBLEClientCallbacks
{
public:
    TreadmillHandler();
    ~TreadmillHandler();
    void begin(NimBLEAddress address);
    void setSpeed(uint16_t speed);
    void start();
    void pause();
    void stop();

    void handle();

    void setAutoReconnect(const bool enable)
    {
        m_autoReconnect = enable;
    }

    bool getAutoReconnect() const
    {
        return m_autoReconnect;
    }

    bool isConnected() const
    {
        return m_pClient && m_pClient->isConnected();
    }

    TreadMillData getLastData() const
    {
        return m_lastData;
    }

    void setCallback(std::function<void(const TreadMillData&)> callback)
    {
        m_onDataUpdate = callback;
    }
    

private:
    bool sendCommand(const uint8_t *data, size_t length);
    void makePacket(uint16_t speed, uint8_t cmd1, uint8_t mode, uint8_t *outPacket);
    void makeKeepalive(uint8_t *outPacket);
    void sendKeepalive();
    // Sent immediately after notification subscription to prevent reason=531 disconnect.
    // The Q1 expects a response within ~300ms of connect or it terminates the session.
    void sendInitSequence();
    bool connectToDevice();

    TreadMillData::Status m_lastStatus = TreadMillData::DISCONNECTED;

    // BA05 protocol state
    static constexpr uint16_t START_SPEED = 994;             // ~1.0 km/h in mph×1600 encoding
    static constexpr unsigned long KEEPALIVE_INTERVAL  = 200;  // ms — matches QZ poll rate
    static constexpr unsigned long POST_CONNECT_COOLDOWN = 3000; // ms — block commands after
                                                                  // reconnect until device settles
    uint8_t m_seqCounter = 0;
    unsigned long m_lastKeepalive = 0;
    unsigned long m_lastConnectTime = 0;  // set on successful connect, gates command sending
    uint16_t m_lastSpeed = 0;  // last commanded speed, preserved for pause/resume
    void notifyCallback(
        NimBLERemoteCharacteristic *pBLERemoteCharacteristic,
        uint8_t *pData,
        size_t length,
        bool isNotify);

    NimBLEClient *m_pClient = nullptr;
    NimBLERemoteCharacteristic *m_pNotifyCharacteristic = nullptr;
    NimBLERemoteCharacteristic *m_pWriteCharacteristic = nullptr;
    NimBLERemoteCharacteristic *m_pUnlockCharacteristic = nullptr;  // secondary handshake char
    NimBLEAddress m_targetAddress;
    bool m_doConnect = false;
    bool m_autoReconnect = true;

    unsigned long m_lastConnectAttempt = 0;

    unsigned long m_lastDataTimestamp = 0;
    TreadMillData m_lastData;


    void onConnect(BLEClient *pClient) override
    {
        log_i("Connected to device!");
    }

    void onDisconnect(BLEClient *pClient, int reason) override
    {
        // NimBLE reason = 512 + HCI error code. Common codes:
        //   0x08 (520) = connection timeout       — radio loss / device out of range
        //   0x13 (531) = remote user terminated   — device actively closed the connection
        //   0x16 (534) = local host terminated    — we called disconnect()
        //   0x3E (574) = failed to establish      — connect() timed out
        const char* desc = "unknown";
        switch (reason - 512) {
            case 0x08: desc = "connection timeout";             break;
            case 0x13: desc = "remote user terminated";         break;
            case 0x16: desc = "local host terminated";          break;
            case 0x3E: desc = "failed to establish connection"; break;
        }
        log_w("Disconnected reason=%d (HCI 0x%02X: %s) - will reconnect...",
              reason, reason - 512, desc);
        m_doConnect = true; // Trigger reconnect in loop
    }

    std::function<void(const TreadMillData&)> m_onDataUpdate = nullptr;

    const uint8_t CONNECTION_TIMEOUT = 30;
};