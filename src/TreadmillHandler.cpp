#include "TreadmillHandler.h"
#include "ba05protocol.h"

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
    BA05Protocol::makePacket(speed, 0x01, 0x0C, m_seqCounter++, packet);
    printCommandPacket("setSpeed", packet, sizeof(packet));
    this->sendCommand(packet, sizeof(packet));
}

// BA05 Protocol: Start at 1.0 km/h
void TreadmillHandler::start()
{
    uint8_t packet[27];
    m_lastSpeed = START_SPEED;
    // CMD1=0x01 for running, MODE=0x0C for running
    BA05Protocol::makePacket(START_SPEED, 0x01, 0x0C, m_seqCounter++, packet);
    printCommandPacket("start", packet, sizeof(packet));
    this->sendCommand(packet, sizeof(packet));
}

// BA05 Protocol: Stop
void TreadmillHandler::stop()
{
    if (m_isPaused && m_sessionActive)
    {
        // Belt is paused (stuck in STOPPED) — commit the stored paused session now,
        // since no new STOPPED transition will fire from the belt.
        log_i("Stop after pause — committing paused session: dist=%.3f km steps=%u",
              m_pausedDistKm, m_pausedSteps);
        m_state.addSession(m_pausedDistKm, m_pausedSteps, m_pausedCalories, m_pausedDurationSec);
        m_sessionActive = false;
        m_isPaused      = false;
        m_autoReconnect = false;
        m_userRequestedConnect = false;
    }
    uint8_t packet[27];
    m_lastSpeed = 0;
    // CMD1=0x05 for stop, MODE=0x08 for stop, speed=0
    BA05Protocol::makePacket(0, 0x05, 0x08, m_seqCounter++, packet);
    printCommandPacket("stop", packet, sizeof(packet));
    this->sendCommand(packet, sizeof(packet));
}

// BA05 Protocol: Pause (keep current speed in packet)
void TreadmillHandler::pause()
{
    m_isPaused = true;
    uint8_t packet[27];
    // CMD1=0x05 for pause, MODE=0x0A for pause, keep last speed
    BA05Protocol::makePacket(m_lastSpeed, 0x05, 0x0A, m_seqCounter++, packet);
    printCommandPacket("pause", packet, sizeof(packet));
    this->sendCommand(packet, sizeof(packet));
}



void TreadmillHandler::sendKeepalive()
{
    if (!isConnected()) return;

    uint8_t packet[9];
    BA05Protocol::makeKeepalive(m_seqCounter++, packet);
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
    m_state.loadFromNVS();

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
        // 27-byte packets are user commands (start/stop/pause/setSpeed).
        // 9-byte packets are keepalives. We treat these differently on failure:
        // - User command failure → user clearly wants to use the belt, so force
        //   reconnect and re-enable autoReconnect even if a prior session ended.
        // - Keepalive failure → don't override autoReconnect state; if it's false
        //   (session ended cleanly) we don't want to reconnect just to idle.
        bool isUserCommand = (length == 27);

        if (!m_pClient->isConnected())
        {
            // Link layer also gone — onDisconnect() will fire, but set state now so
            // HA gets a DISCONNECTED update immediately rather than waiting for the callback.
            log_e("Write failed: link layer gone — triggering immediate reconnect");
            m_lastData.status = TreadMillData::DISCONNECTED;
            m_newDataAvailable = true;
            if (isUserCommand)
            {
                m_autoReconnect        = true;
                m_userRequestedConnect = true;
            }
            m_doConnect = true;
        }
        else
        {
            // GATT layer broken but link layer still alive (zombie connection).
            // This is the "throw-off" scenario: writeValue() returns failure (rc=7,
            // BLE_HS_ENOTCONN at GATT level) but isConnected() is true because the
            // BLE link-layer supervision timer is still being satisfied by LL-level
            // packets (e.g. the pad's outbound notifications). Without intervention,
            // this state can persist for many minutes until the supervision timeout
            // finally expires — then the belt stops abruptly (the pad kills the motor
            // when BLE drops) and the user gets thrown off.
            //
            // Fix: call disconnect() explicitly from Core 1. This sends an HCI
            // disconnect command immediately, fires onDisconnect() (reason=534,
            // local host terminated) within milliseconds, and unblocks the reconnect
            // loop — no waiting for supervision timeout.
            log_e("Write failed: GATT layer broken despite isConnected()=true (zombie) — forcing disconnect");
            m_lastData.status = TreadMillData::DISCONNECTED;
            m_newDataAvailable = true;
            if (isUserCommand)
            {
                // User sent a command — they want to walk. Re-arm auto-reconnect so
                // onDisconnect() → m_doConnect chain fires even if a prior session
                // had set m_autoReconnect=false.
                m_autoReconnect        = true;
                m_userRequestedConnect = true;
            }
            // disconnect() fires onDisconnect() asynchronously on Core 0.
            // onDisconnect() nulls the characteristic pointers and sets m_doConnect=true
            // (if m_autoReconnect). Safe to call from Core 1 / handle() context.
            m_pClient->disconnect();
        }
        return false;
    }

    return true;
}

void TreadmillHandler::handle()
{
    // handles reconnection
    // m_reconnectNotBefore is set after an idle kick (user-requested connect) to add
    // a 60s backoff between reconnect attempts — avoids rapid beeping while idle.
    if (m_doConnect && (millis() - m_lastConnectAttempt > 5000) && m_autoReconnect &&
        (m_reconnectNotBefore == 0 || millis() >= m_reconnectNotBefore))
    {
        m_lastConnectAttempt = millis();
        if (this->connectToDevice())
        {
            log_i("Connection successful.");
            m_doConnect = false;
            m_lastKeepalive = millis(); // Reset keepalive timer on connect
            // m_lastConnectTime is now set inside connectToDevice() before subscription
            // so that notification timing is accurate during the setup window.
        }
        else
        {
            log_e("Failed to connect - Retrying in 5 seconds...");
        }
    }

    // Re-send PitPat unlock bytes to 0x2b11 in response to first 0x34 packet this
    // connection. The Q1 pad may require the unlock handshake to be sent AFTER it
    // issues the 0x34 challenge (especially during active-walk reconnects) rather than
    // accepting the upfront unlock sent in sendInitSequence() before any notification.
    // This fires once per connection (m_sendUnlockNow set on type change TO 0x34).
    if (m_sendUnlockNow && isConnected())
    {
        m_sendUnlockNow = false;
        if (m_pUnlockCharacteristic)
        {
            uint8_t unlockData[] = {0x6B, 0x05, 0x9D, 0x98, 0x43};
            bool ok = m_pUnlockCharacteristic->writeValue(unlockData, sizeof(unlockData), false);
            log_i("0x34 challenge → unlock re-sent to 0x2b11: %s", ok ? "OK" : "FAILED");
        }
        else
        {
            log_w("0x34 challenge → unlock re-send skipped (0x2b11 not available this connection)");
        }
    }

    // Immediate keepalive requested by notifyCallback() (Core 0) in response to a 0x34
    // packet. Sent here on Core 1 to avoid calling writeValue() inside a NimBLE callback,
    // which deadlocks (writeValue blocks waiting for an ATT response processed by the same
    // NimBLE task that's currently blocked in the callback).
    if (m_sendKeepaliveNow && isConnected())
    {
        m_sendKeepaliveNow = false;
        sendKeepalive();
        m_lastKeepalive = millis(); // reset timer so we don't double-send immediately after
        log_i("Immediate keepalive sent (0x34 response, Core 1)");
    }

    // Send keepalive every KEEPALIVE_INTERVAL ms (200ms) to maintain connection
    if (isConnected() && (millis() - m_lastKeepalive >= KEEPALIVE_INTERVAL))
    {
        sendKeepalive();
        m_lastKeepalive = millis();
    }

    // Publish new BLE packet data from Core 1 (main loop).
    // notifyCallback() (Core 0) sets this flag when it has written fresh data to m_lastData.
    // We clear it before calling m_onDataUpdate so that another notifyCallback packet
    // arriving during the publish will re-set the flag and get published next cycle.
    if (m_newDataAvailable && m_onDataUpdate)
    {
        m_newDataAvailable = false;
        m_onDataUpdate(m_lastData);
    }

    // Check for connection timeout — catches radio loss / silent disconnects where
    // onDisconnect() never fired. Skip if already DISCONNECTED to avoid repeated
    // MQTT publishes every 30s when the idle kick has cleanly stopped auto-reconnect.
    if (m_lastData.status != TreadMillData::DISCONNECTED &&
        millis() - m_lastDataTimestamp > CONNECTION_TIMEOUT * 1000)
    {
        unsigned long elapsedSec = (millis() - m_lastDataTimestamp) / 1000;
        log_w("No data received for %lus (threshold %ds) - marking disconnected.",
              elapsedSec, CONNECTION_TIMEOUT);
        m_lastData.status = TreadMillData::DISCONNECTED;
        m_lastDataTimestamp = millis(); // reset so the check doesn't re-fire immediately
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
    }

    // Always request our preferred connection parameters — not just on first client
    // creation. NimBLE negotiates these with the peripheral at the start of each new
    // physical connection; calling this only once (inside the nullptr guard) means every
    // reconnect after boot uses whatever parameters were cached from the previous session,
    // which may differ from the values we want. Re-applying before connect() ensures
    // the peripheral sees a consistent parameter request every time.
    //   minInterval=12 (~15ms), maxInterval=24 (~30ms), latency=0, timeout=600 (6s)
    m_pClient->setConnectionParams(12, 24, 0, 600);

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
        // Reset ALL per-connection state BEFORE subscribing so that any notification
        // that arrives during the remainder of connectToDevice() (unlock discovery,
        // BLE param logging, sendInitSequence) sees a clean slate.
        //
        // Previously these resets lived at the END of connectToDevice(), which caused
        // a race: the first 0x34 notification arrived during setup and set flags
        // (m_lastPacketType=0x34, m_sendUnlockNow=true), then the end-of-function
        // resets wiped them — so the 0x2F transition was logged as "First packet"
        // instead of "Packet type changed", and m_sendUnlockNow was silently cleared.
        //
        // Setting m_lastConnectTime here also gives accurate t=+Xms timing for all
        // notifications that arrive during the connection setup window.
        m_lastConnectTime         = millis();
        m_firstPacketAfterConnect = true;
        m_lastPacketType          = 0;
        m_sendKeepaliveNow        = false;
        m_sendUnlockNow           = false;
        m_sessionActive           = false;
        m_isPaused                = false;
        m_justResumedFromPause    = false;
        m_reconnectNotBefore      = 0;

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

    // Log the actual negotiated connection parameters so we can verify the supervision
    // timeout the pad accepted. Our requested values: interval=12-24 (15-30ms),
    // latency=0, timeout=600 (6s). If the pad negotiated a much longer timeout, that
    // explains why onDisconnect() can take hundreds of seconds after the GATT layer breaks.
    NimBLEConnInfo connInfo = m_pClient->getConnInfo();
    log_i("Negotiated BLE params: interval=%u (%.1fms) latency=%u timeout=%u (%ums)",
          connInfo.getConnInterval(), connInfo.getConnInterval() * 1.25f,
          connInfo.getConnLatency(),
          connInfo.getConnTimeout(),
          connInfo.getConnTimeout() * 10);

    // CRITICAL: send the init/handshake immediately - don't wait for the keepalive timer.
    // The Q1 disconnects (reason=531) if it sees no write within ~300ms of connection.
    sendInitSequence();
    m_lastKeepalive = millis(); // prevent double-send when handle() fires next

    // Per-connection state was already reset above (before subscribe) to avoid
    // the race where notifications arrive during setup and then get wiped by resets
    // that execute after them. Nothing to do here.

    return true;
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
        return;
    }

    BA05Protocol::ParsedData parsed = BA05Protocol::parsePacket(pData, length);
    if (!parsed.valid)
    {
        log_w("Unexpected or invalid packet length: %d - ignoring", length);
        return;
    }

    TreadMillData data = m_lastData;
    TreadMillData::Status previousStatus = m_lastStatus;
    uint8_t packetType = parsed.packetType;

    if (packetType != m_lastPacketType)
    {
        unsigned long msAfterConnect = millis() - m_lastConnectTime;
        if (m_lastPacketType == 0)
        {
            log_i("First packet: type=0x%02X len=%d at t=+%lums after connect",
                  packetType, length, msAfterConnect);
        }
        else
        {
            log_i("Packet type changed: 0x%02X → 0x%02X at t=+%lums after connect",
                  m_lastPacketType, packetType, msAfterConnect);
        }
        m_lastPacketType = packetType;

        if (packetType == 0x34)
        {
            m_sendUnlockNow = true;
            log_i("0x34 onset → queued unlock re-send to 0x2b11 (Core 1)");
        }
    }

    if (length >= 50 && (packetType == 0x2F || packetType == 0x34)) {
        data.speedFeedback = parsed.speedFeedback;
        data.speedCmd      = parsed.speedCmd;
        data.speedMax      = parsed.speedMax;
        data.fwVersion     = 0;
        data.status        = parsed.status;

        if (m_firstPacketAfterConnect)
        {
            m_firstPacketAfterConnect = false;
            const char* statusName[] = {"COUNTDOWN","RUNNING","PAUSED","STOPPED","DISCONNECTED"};
            const char* sn = (data.status <= TreadMillData::DISCONNECTED)
                             ? statusName[(int)data.status] : "UNKNOWN";
            log_w("FIRST POST-CONNECT PACKET: type=0x%02X len=%d → status=%s "
                  "speed=%.3f dist=%dm dur=%u cal=%u",
                  packetType, length, sn,
                  parsed.speedFeedback, parsed.distanceM, parsed.durationSec, parsed.calories);
            char hex[length * 3 + 1];
            for (size_t i = 0; i < length; i++) sprintf(hex + i * 3, "%02X ", pData[i]);
            hex[length * 3 - 1] = '\0';
            log_w("FIRST POST-CONNECT RAW: %s", hex);
        }

        if (packetType == 0x34)
        {
            m_sendKeepaliveNow = true;
            log_i("0x34 → queued immediate keepalive (Core 1)");
        }

        // STATUS TRANSITION LOGGING — temporary, for pause vs stop diagnosis
        if (data.status != previousStatus)
        {
            auto statusStr = [](TreadMillData::Status s) -> const char* {
                switch(s) {
                    case TreadMillData::COUNTDOWN:    return "COUNTDOWN";
                    case TreadMillData::RUNNING:      return "RUNNING";
                    case TreadMillData::PAUSED:       return "PAUSED";
                    case TreadMillData::STOPPED:      return "STOPPED";
                    case TreadMillData::DISCONNECTED: return "DISCONNECTED";
                    default:                          return "UNKNOWN";
                }
            };
            log_w("STATUS: %s → %s  (flags=0x%02X  speed=%.2f  dist=%um  active=%d)",
                  statusStr(previousStatus), statusStr(data.status),
                  (packetType == 0x2F) ? pData[45] : pData[28],
                  parsed.speedFeedback, parsed.distanceM, m_sessionActive);
        }

        m_lastStatus = data.status;

        if (data.status == TreadMillData::RUNNING && data.speedFeedback > 0.001f)
        {
            if (m_isPaused && previousStatus == TreadMillData::STOPPED)
            {
                if (parsed.distanceM > 0)
                {
                    // Belt resumed from where it left off — session continues, no commit.
                    // Mark that we just resumed so we can suppress the belt's startup
                    // RUNNING→STOPPED→RUNNING sequence that fires in the next ~500 ms.
                    log_i("Resumed from pause (dist=%um) — session continues", parsed.distanceM);
                    m_justResumedFromPause = true;
                    m_resumeDistanceM      = parsed.distanceM;
                }
                else
                {
                    // Belt reset its odometer — commit the stored paused session, then start fresh
                    log_i("Belt reset after pause — committing paused session: dist=%.3f km steps=%u",
                          m_pausedDistKm, m_pausedSteps);
                    data.sessionDistanceKm  = m_pausedDistKm;
                    data.sessionSteps       = m_pausedSteps;
                    data.sessionCalories    = m_pausedCalories;
                    data.sessionDurationSec = m_pausedDurationSec;
                    data.sessionComplete    = true;
                    m_state.addSession(m_pausedDistKm, m_pausedSteps, m_pausedCalories, m_pausedDurationSec);
                }
                m_isPaused = false;
            }
            m_sessionActive = true;
        }

        if (data.status != TreadMillData::STOPPED)
        {
            data.distanceKm  = parsed.distanceM / 1000.0f;
            data.calories    = parsed.calories;
            data.steps       = (uint32_t)(parsed.distanceM / m_state.getStepLength());
            data.durationSec = parsed.durationSec;
        }

        if (data.status == TreadMillData::STOPPED &&
            previousStatus != TreadMillData::STOPPED &&
            previousStatus != TreadMillData::DISCONNECTED &&
            data.distanceKm > 0.0f &&
            m_sessionActive)
        {
            if (m_justResumedFromPause && !m_isPaused && parsed.distanceM <= m_resumeDistanceM)
            {
                // Belt's startup RUNNING→STOPPED after a resume — distance hasn't advanced,
                // so this is not a real stop. Suppress the commit and wait for real RUNNING.
                log_i("Suppressing spurious post-resume STOPPED (dist=%um unchanged)", parsed.distanceM);
                m_justResumedFromPause = false;
            }
            else if (m_isPaused)
            {
                // Belt reports STOPPED because we sent a pause command — defer the commit.
                // We'll commit when the belt resumes (RUNNING with dist=0 = reset) or
                // when stop()/onDisconnect() is called explicitly.
                m_pausedDistKm         = data.distanceKm;
                m_pausedSteps          = data.steps;
                m_pausedCalories       = (uint32_t)data.calories;
                m_pausedDurationSec    = data.durationSec;
                m_justResumedFromPause = false; // clear in case user re-paused right after resume
                log_i("Pause-stop: deferring commit (dist=%.3f km steps=%u) — waiting for resume or stop",
                      m_pausedDistKm, m_pausedSteps);
            }
            else
            {
                data.sessionDistanceKm  = data.distanceKm;
                data.sessionSteps       = data.steps;
                data.sessionCalories    = (uint32_t)data.calories;
                data.sessionDurationSec = data.durationSec;
                data.sessionComplete    = true;

                m_state.addSession(data.distanceKm, data.steps, (uint32_t)data.calories, data.durationSec);

                log_i("Session ended — totals: dist=%.2f km  steps=%u  cal=%u  dur=%u s",
                      m_state.getTotalDistanceKm(), m_state.getTotalSteps(),
                      m_state.getTotalCalories(), m_state.getTotalDurationSec());
                log_i("Session summary: dist=%.3f km  steps=%u  cal=%u  dur=%u s",
                      data.sessionDistanceKm, data.sessionSteps,
                      data.sessionCalories, data.sessionDurationSec);

                m_sessionActive        = false;
                m_autoReconnect        = false;
                m_userRequestedConnect = false;
                m_justResumedFromPause = false;

                data.distanceKm  = 0.0f;
                data.calories    = 0;
                data.steps       = 0;
                data.durationSec = 0;
            }
        }

        if (data.status != TreadMillData::STOPPED && m_sessionActive)
        {
            data.totalDistanceKm  = m_state.getTotalDistanceKm() + data.distanceKm;
            data.totalSteps       = m_state.getTotalSteps() + data.steps;
            data.totalCalories    = m_state.getTotalCalories() + (uint32_t)data.calories;
            data.totalDurationSec = m_state.getTotalDurationSec() + data.durationSec;
        }
        else
        {
            data.totalDistanceKm  = m_state.getTotalDistanceKm();
            data.totalSteps       = m_state.getTotalSteps();
            data.totalCalories    = m_state.getTotalCalories();
            data.totalDurationSec = m_state.getTotalDurationSec();
        }

        log_d("speed=%.2f mph target=%.2f dist=%.3f km cal=%u dur=%u:%02u status=%d  "
              "tot_dist=%.2f km tot_steps=%u",
              data.speedFeedback, data.speedCmd, data.distanceKm,
              data.calories, data.durationSec / 60, data.durationSec % 60, (int)data.status,
              data.totalDistanceKm, data.totalSteps);
    }
    else if (length == 20) {
        data.speedFeedback = parsed.speedFeedback;
    }

    m_lastData = data;
    m_lastDataTimestamp = millis();
    // Signal handle() (Core 1) that new data is ready to publish.
    // Do NOT call m_onDataUpdate() here — notifyCallback runs on Core 0 (NimBLE task)
    // and PubSubClient::publish() is not thread-safe. Calling it concurrently from
    // Core 0 and Core 1 (dead reckoning) caused ECONNRESET / double-publish bugs.
    m_newDataAvailable = true;
}
