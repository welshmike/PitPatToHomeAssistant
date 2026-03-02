#include "TreadmillHandler.h"

// --- Helper functions to read uint16 and uint32 from bytes ---
uint16_t readU16(uint8_t *data, int offset)
{
    // Explicit big-endian read — memcpy would give host byte order (little-endian on ESP32)
    return ((uint16_t)data[offset] << 8) | data[offset + 1];
}

uint32_t readU32(uint8_t *data, int offset)
{
    return ((uint32_t)data[offset] << 24) |
           ((uint32_t)data[offset + 1] << 16) |
           ((uint32_t)data[offset + 2] << 8) |
           ((uint32_t)data[offset + 3]);
}

TreadmillHandler::TreadmillHandler()
{
    m_pClient = nullptr;
    m_pNotifyCharacteristic = nullptr;
    m_pWriteCharacteristic = nullptr;
    m_pUnlockCharacteristic = nullptr;
    m_doConnect = false;
}

TreadmillHandler::~TreadmillHandler()
{
    if (m_pClient)
    {
        m_pClient->disconnect();
        m_pClient = nullptr;
    }
}

// Helper to print command packet as hex
void printCommandPacket(const char* cmdName, const uint8_t* packet, size_t length)
{
    char hex[length * 3 + 1];
    for (size_t i = 0; i < length; i++)
        sprintf(hex + i * 3, "%02X ", packet[i]);
    hex[length * 3] = '\0';
    log_i("CMD: %s (seq=%u) | %s", cmdName, packet[2], hex);
}

// BA05 Protocol: Set speed (speed in mph * 1000, e.g., 2500 = 2.5 mph)
void TreadmillHandler::setSpeed(uint16_t speed)
{
    uint8_t packet[27];
    m_lastSpeed = speed;
    // CMD1=0x01 for running, MODE=0x0C for running
    this->makePacket(speed, 0x01, 0x0C, packet);
    printCommandPacket("setSpeed", packet, sizeof(packet));
    this->sendCommand(packet, sizeof(packet));
}

// BA05 Protocol: Start at 1.0 km/h
void TreadmillHandler::start()
{
    uint8_t packet[27];
    m_lastSpeed = START_SPEED;
    // CMD1=0x01 for running, MODE=0x0C for running
    this->makePacket(START_SPEED, 0x01, 0x0C, packet);
    printCommandPacket("start", packet, sizeof(packet));
    this->sendCommand(packet, sizeof(packet));
}

// BA05 Protocol: Stop
void TreadmillHandler::stop()
{
    uint8_t packet[27];
    m_lastSpeed = 0;
    // CMD1=0x05 for stop, MODE=0x08 for stop, speed=0
    this->makePacket(0, 0x05, 0x08, packet);
    printCommandPacket("stop", packet, sizeof(packet));
    this->sendCommand(packet, sizeof(packet));
}

// BA05 Protocol: Pause (keep current speed in packet)
void TreadmillHandler::pause()
{
    uint8_t packet[27];
    // CMD1=0x05 for pause, MODE=0x0A for pause, keep last speed
    this->makePacket(m_lastSpeed, 0x05, 0x0A, packet);
    printCommandPacket("pause", packet, sizeof(packet));
    this->sendCommand(packet, sizeof(packet));
}

// BA05 Protocol: 9-byte keepalive packet
void TreadmillHandler::makeKeepalive(uint8_t *outPacket)
{
    // Format: 4D 00 [SEQ] 05 6A 05 FD F8 43
    outPacket[0] = 0x4D;  // Start byte
    outPacket[1] = 0x00;
    outPacket[2] = m_seqCounter++;  // Sequence counter (auto-increment)
    outPacket[3] = 0x05;
    outPacket[4] = 0x6A;
    outPacket[5] = 0x05;
    outPacket[6] = 0xFD;
    outPacket[7] = 0xF8;
    outPacket[8] = 0x43;  // End byte
}

void TreadmillHandler::sendKeepalive()
{
    if (!isConnected()) return;

    uint8_t packet[9];
    makeKeepalive(packet);
    this->sendCommand(packet, sizeof(packet));
}

// Sent immediately after subscribing to notifications.
// The Q1 Classic Pro silently disconnects (BLE reason=531, remote user terminated)
// if nothing is written to it within ~300ms of connection. Two-step approach:
//   1. If the secondary unlock characteristic (0x2b11) is present, write the
//      PitPat unlock bytes to it (raw, no BA05 envelope) - same as QZ does.
//   2. Send an immediate keepalive on the primary write characteristic so the
//      device knows we're a valid client.
void TreadmillHandler::sendInitSequence()
{
    if (!isConnected()) return;

    // Step 1: write unlock bytes to secondary characteristic (best-effort)
    if (m_pUnlockCharacteristic)
    {
        // Raw PitPat unlock command - no BA05 envelope needed for this characteristic
        uint8_t unlockData[] = {0x6B, 0x05, 0x9D, 0x98, 0x43};
        bool ok = m_pUnlockCharacteristic->writeValue(unlockData, sizeof(unlockData), false);
        log_i("Unlock write %s", ok ? "succeeded" : "failed (non-fatal)");
    }
    else
    {
        log_w("No unlock characteristic - skipping unlock step");
    }

    // Step 2: immediate keepalive on the main write characteristic
    sendKeepalive();
    log_i("Init sequence complete - keepalive sent");
}

void TreadmillHandler::begin(NimBLEAddress address)
{
    m_targetAddress = address;
    m_doConnect = true;
}

// Send data to the write characteristic
bool TreadmillHandler::sendCommand(const uint8_t *data, size_t length)
{
    if (!m_pWriteCharacteristic || !m_pClient->isConnected())
    {
        log_e("Cannot write, characteristic not ready or client disconnected");
        return false;
    }

    // Block speed-set commands (27-byte packets) during post-connect cooldown.
    // Sending a queued speed command immediately after reconnect causes reason=531:
    // the device needs time to settle after the init sequence. Start/stop/pause
    // (also 27-byte but with cmd1=0x05) and keepalives (9-byte) are always allowed.
    bool isSpeedSet = (length == 27 && data[12] == 0x01); // cmd1=0x01 = running/speed
    if (isSpeedSet && (millis() - m_lastConnectTime < POST_CONNECT_COOLDOWN))
    {
        log_w("Speed command blocked - post-connect cooldown (%lums remaining)",
              POST_CONNECT_COOLDOWN - (millis() - m_lastConnectTime));
        return false;
    }

    bool success = m_pWriteCharacteristic->writeValue(data, length, true);
    if (!success)
    {
        log_e("Failed to write to treadmill");
        return false;
    }

    return true;
}

void TreadmillHandler::handle()
{
    // handles reconnection
    if (m_doConnect && (millis() - m_lastConnectAttempt > 5000) && m_autoReconnect)
    {
        m_lastConnectAttempt = millis();
        if (this->connectToDevice())
        {
            log_i("Connection successful.");
            m_doConnect = false;
            m_lastKeepalive = millis();   // Reset keepalive timer on connect
            m_lastConnectTime = millis(); // Start post-connect cooldown
        }
        else
        {
            log_e("Failed to connect - Retrying in 5 seconds...");
        }
    }

    // Send keepalive every KEEPALIVE_INTERVAL ms (200ms) to maintain connection
    if (isConnected() && (millis() - m_lastKeepalive >= KEEPALIVE_INTERVAL))
    {
        sendKeepalive();
        m_lastKeepalive = millis();
    }

    // Check for connection timeout and send state updates if needed
    if (millis() - m_lastDataTimestamp > CONNECTION_TIMEOUT * 1000)
    {
        unsigned long elapsedSec = (millis() - m_lastDataTimestamp) / 1000;
        log_w("No data received for %lus (threshold %ds) - marking disconnected.",
              elapsedSec, CONNECTION_TIMEOUT);
        m_lastData.status = TreadMillData::DISCONNECTED;
        m_lastDataTimestamp = millis(); // prevent repeated updates
        if (m_onDataUpdate)
        {
            m_onDataUpdate(m_lastData);
        }
    }
}

bool TreadmillHandler::connectToDevice()
{
    if (m_pClient == nullptr)
    {
        m_pClient = BLEDevice::createClient();
        // NOTE: setDataLen(64) removed - caused "Set data length error: 514" on every
        // connect because 64 bytes is below the minimum negotiable PDU size.
        // MTU is negotiated automatically (255 bytes, confirmed in logs).
        m_pClient->setClientCallbacks(this, false);
        m_pClient->setConnectionParams(12, 24, 0, 600); // 15ms to 30ms interval, no latency, 6s timeout
    }

    log_i("Connecting to %s", m_targetAddress.toString().c_str());

    if (!m_pClient->isConnected() && !m_pClient->connect(m_targetAddress, true, true))
    {
        log_e("Failed to connect to treadmill at %s", m_targetAddress.toString().c_str());
        return false;
    }

    // NOTE: delay(500) removed. The Q1 Classic Pro disconnects (reason=531, remote user
    // terminated) if it receives no write within ~300ms of connecting. Every millisecond
    // spent here is time we're not spending on the handshake.

    // Retry service discovery up to 5 times with a short delay between attempts
    NimBLERemoteService *pService = nullptr;
    for (int retry = 0; retry < 5; retry++)
    {
        pService = m_pClient->getService(SERVICE_PAD_UUID);
        if (pService)
        {
            log_i("Service found on attempt %d", retry + 1);
            break;
        }
        log_w("Service not found, retry %d/5...", retry + 1);
        delay(200); // reduced from 500ms - device won't wait long
    }
    if (!pService)
    {
        log_e("Failed to find treadmill service UUID after 5 retries: %s", SERVICE_PAD_UUID);
        m_pClient->disconnect();
        return false;
    }

    m_pWriteCharacteristic = pService->getCharacteristic(CHARACTERISTIC_WRITE_UUID);
    if (!m_pWriteCharacteristic || !m_pWriteCharacteristic->canWrite())
    {
        log_e("Write characteristic not found or not writable!");
        m_pClient->disconnect();
        return false;
    }
    log_i("Write characteristic initialized");

    m_pNotifyCharacteristic = pService->getCharacteristic(CHARACTERISTIC_NOTIFY_STATE_UUID);
    if (!m_pNotifyCharacteristic)
    {
        log_e("Notify characteristic not found!");
        m_pClient->disconnect();
        return false;
    }

    if (m_pNotifyCharacteristic->canNotify())
    {
        m_pNotifyCharacteristic->subscribe(
            true,
            [this](NimBLERemoteCharacteristic *chr,
                   uint8_t *data,
                   size_t length,
                   bool isNotify)
            {
                this->notifyCallback(chr, data, length, isNotify);
            });
        log_i("Subscribed to notifications.");
    }
    else
    {
        log_e("Notify characteristic cannot notify!");
        m_pClient->disconnect();
        return false;
    }

    // Best-effort: discover the secondary unlock characteristic (service 0x1910, char 0x2b11).
    // QZ uses this for the PitPat unlock handshake. The Q1's variant may or may not need it,
    // but we try - failure here is non-fatal.
    m_pUnlockCharacteristic = nullptr;
    NimBLERemoteService *pUnlockService = m_pClient->getService(SERVICE_UNLOCK_UUID);
    if (pUnlockService)
    {
        m_pUnlockCharacteristic = pUnlockService->getCharacteristic(CHARACTERISTIC_UNLOCK_UUID);
        if (m_pUnlockCharacteristic)
            log_i("Unlock characteristic (0x2b11) found.");
        else
            log_w("Unlock characteristic not found - continuing without it.");
    }
    else
    {
        log_w("Unlock service (0x1910) not found - continuing without it.");
    }

    // CRITICAL: send the init/handshake immediately - don't wait for the keepalive timer.
    // The Q1 disconnects (reason=531) if it sees no write within ~300ms of connection.
    sendInitSequence();
    m_lastKeepalive = millis(); // prevent double-send when handle() fires next

    return true;
}

// BA05 Protocol: Generates a 27-byte command packet
// Format: 4D 00 [SEQ] 17 6A 17 00 00 00 00 [SPEED_HI] [SPEED_LO] [CMD1] 00 50 00 [MODE] 00 00 00 00 00 00 00 00 [CHECKSUM] 43
//
// Speed encoding: mph * 1600  (same as status packets; e.g. 3.0 km/h = 1.864 mph = raw 2982)
// Checksum: XOR of bytes [5..24] — verified against QZ DeerRun source
void TreadmillHandler::makePacket(uint16_t speed, uint8_t cmd1, uint8_t mode, uint8_t *outPacket)
{
    // outPacket must be at least 27 bytes

    // Header
    outPacket[0] = 0x4D;  // Start byte (BA05)
    outPacket[1] = 0x00;
    outPacket[2] = m_seqCounter++;  // Sequence counter (auto-increment)
    outPacket[3] = 0x17;  // Payload length (23 bytes follow before end byte)
    outPacket[4] = 0x6A;
    outPacket[5] = 0x17;

    // Reserved bytes
    outPacket[6] = 0x00;
    outPacket[7] = 0x00;
    outPacket[8] = 0x00;
    outPacket[9] = 0x00;

    // Speed (uint16 big-endian, mph * 1600)
    outPacket[10] = (speed >> 8) & 0xFF;
    outPacket[11] = speed & 0xFF;

    // Command byte 1
    outPacket[12] = cmd1;  // 0x01 for running, 0x05 for pause/stop

    outPacket[13] = 0x00;
    outPacket[14] = 0x50;  // was 0x46 - corrected from QZ source
    outPacket[15] = 0x00;

    // Mode byte
    outPacket[16] = mode;  // 0x0C=running, 0x0A=pause, 0x08=stop

    // Reserved bytes
    outPacket[17] = 0x00;
    outPacket[18] = 0x00;
    outPacket[19] = 0x00;
    outPacket[20] = 0x00;
    outPacket[21] = 0x00;
    outPacket[22] = 0x00;
    outPacket[23] = 0x00;
    outPacket[24] = 0x00;

    // Checksum: XOR of bytes [5..24] — QZ uses this range (not [1..24])
    uint8_t checksum = 0;
    for (int i = 5; i <= 24; ++i)
    {
        checksum ^= outPacket[i];
    }
    outPacket[25] = checksum;

    // End byte
    outPacket[26] = 0x43;
}

// --- Notification callback ---
void TreadmillHandler::notifyCallback(
    BLERemoteCharacteristic *pBLERemoteCharacteristic,
    uint8_t *pData,
    size_t length,
    bool isNotify)
{
    if (length <= 5)
    {
        // 5-byte packets are keepalive ACKs from the device - ignore silently
        return;
    }
    if (length < 20)
    {
        log_w("Unexpected packet length: %d - ignoring", length);
        return;
    }

    // Use last known data as starting point
    TreadMillData data = m_lastData;

    // Packet type detection based on byte 3
    uint8_t packetType = pData[3];

    if (length >= 50 && (packetType == 0x2F || packetType == 0x34)) {
        // BA05 packet parsing - field positions verified against live captures:
        //
        // 0x2F (51-byte) packets: normal running data - confirmed via hex dump 2025-02
        // 0x34 (56-byte) packets: extended data - same header, +5 device serial bytes
        //                         inserted at [36], shifting tail bytes
        //
        // Byte  Field              Notes
        // [7-8] current speed      raw / 1600 = mph; display shows mph e.g. 0.6
        // [9-10] target speed      same encoding
        // [11-12] max speed        same encoding (may be 0 in 0x2F packets)
        // [13-14] distance         uint16 BE, metres; was incorrectly uint8 before
        // [23]  calories           uint8
        // [25]  duration minutes   uint8
        // [26-27] duration ms      uint16 BE, milliseconds within current minute
        // [45]  status flags (0x2F packets)  0x08=running, 0x00=stopped
        // [28]  status flags (0x34 packets)  pending further capture to verify

        uint16_t current_speed = readU16(pData, 7);
        uint16_t target_speed  = readU16(pData, 9);
        uint16_t distance_m    = readU16(pData, 13);  // uint16 - was uint8, max was 255m
        uint8_t  calories      = pData[23];
        uint8_t  duration_min  = pData[25];
        uint16_t duration_ms   = readU16(pData, 26);

        // byte[25] is NOT a minutes counter — it counts uint16 overflows of the ms timer.
        // Each overflow = 65536ms (~65.5s). Treating it as 60s/min gave ~8% error.
        // Verified: 26 overflows × 65536 + 53064ms = 1757s ≈ 29:17 (belt showed 29:16 ✓)
        uint32_t duration_total_sec = ((uint32_t)duration_min * 65536 + duration_ms) / 1000;

        // Speed: device encodes as mph * 1600. Confirmed: 1000 raw = 0.625 mph
        // All speed fields stored and published in mph to match belt display.
        float speed_mph  = current_speed / 1600.0f;
        float target_mph = target_speed  / 1600.0f;
        float max_mph    = readU16(pData, 11) / 1600.0f;

        data.speedFeedback = speed_mph;
        data.speedCmd      = target_mph;
        data.speedMax      = max_mph;
        data.fwVersion     = 0;
        // distanceKm, calories, durationSec, steps set below as session deltas

        // Status byte differs by packet type.
        // 0x2F (51-byte): byte [45] - observed values: 0x08, 0x06 (running), 0x00 (stopped)
        // 0x34 (56-byte): byte [28] - original assumption, not yet re-verified
        //
        // We've seen both 0x06 and 0x08 while the belt is running, so don't rely on a
        // specific bitmask. Instead map known explicit values and treat anything else
        // non-zero as RUNNING (belt moving but in an unclassified active state).
        uint8_t flags = (packetType == 0x2F) ? pData[45] : pData[28];
        if      (flags == 0x00) data.status = TreadMillData::STOPPED;
        else if (flags == 0x18) data.status = TreadMillData::COUNTDOWN;
        else if (flags == 0x10) data.status = TreadMillData::PAUSED;
        else                    data.status = TreadMillData::RUNNING;  // 0x06, 0x08, etc.

        m_lastStatus = data.status;

        // Use raw values from the belt as source of truth — no session delta tracking.
        // The belt accumulates distance/calories/duration until powered off, matching
        // its own display exactly regardless of ESP32 reconnects or reflashes.
        //
        // When STOPPED the belt resets its counters to 0 in the stopped packet.
        // We preserve the last running values so HA automations (e.g. Strava) can
        // still read the final session totals after the belt stops.
        if (data.status != TreadMillData::STOPPED)
        {
            data.distanceKm  = distance_m / 1000.0f;
            data.calories    = calories;
            data.durationSec = duration_total_sec;
            data.steps       = (uint32_t)(distance_m / STRIDE_LENGTH_M);
        }
        // else: preserve m_lastData values (belt has reset counters to 0)

        log_d("speed=%.2f mph target=%.2f dist=%.3f km cal=%u dur=%u:%02u status=%d",
              data.speedFeedback, data.speedCmd, data.distanceKm,
              data.calories, data.durationSec / 60, data.durationSec % 60, (int)data.status);
    }
    else if (length == 20) {
        // Short status packet - speed only, offset unverified for Q1
        uint16_t current_speed = readU16(pData, 9);
        data.speedFeedback = current_speed / 1600.0f;  // mph, same encoding as main packets
    }

    m_lastData = data;
    m_lastDataTimestamp = millis();
    if (m_onDataUpdate)
    {
        m_onDataUpdate(data);
    }
}
