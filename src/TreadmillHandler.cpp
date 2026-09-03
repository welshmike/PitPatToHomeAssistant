#include "TreadmillHandler.h"
#include "ba05protocol.h"

TreadmillHandler::TreadmillHandler()
{
    m_pClient = nullptr;
    m_pNotifyCharacteristic = nullptr;
    m_pWriteCharacteristic = nullptr;
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
    m_lastSpeed = speed;
    if (!isConnected())
    {
        log_i("setSpeed() while disconnected — queuing command and auto-connecting");
        m_pendingCmd   = PendingCmd::SET_SPEED;
        m_pendingSpeed = speed;
        m_autoReconnect        = true;
        m_userRequestedConnect = false;
        m_reconnectNotBefore   = 0;
        m_doConnect            = true;
        return;
    }
    uint8_t packet[27];
    // CMD1=0x01 for running, MODE=0x0C for running
    BA05Protocol::makePacket(speed, 0x01, 0x0C, m_seqCounter++, packet);
    printCommandPacket("setSpeed", packet, sizeof(packet));
    this->sendCommand(packet, sizeof(packet));
}

// BA05 Protocol: Start at 1.0 km/h
void TreadmillHandler::start()
{
    if (!isConnected())
    {
        log_i("start() while disconnected — queuing command and auto-connecting");
        m_pendingCmd   = PendingCmd::START;
        m_pendingSpeed = START_SPEED_RAW;
        m_autoReconnect        = true;
        m_userRequestedConnect = false;
        m_reconnectNotBefore   = 0;
        m_doConnect            = true;
        return;
    }
    uint8_t packet[27];
    m_lastSpeed = START_SPEED_RAW;
    // CMD1=0x01 for running, MODE=0x0C for running
    BA05Protocol::makePacket(START_SPEED_RAW, 0x01, 0x0C, m_seqCounter++, packet);
    printCommandPacket("start", packet, sizeof(packet));
    this->sendCommand(packet, sizeof(packet));
}

// BA05 Protocol: Stop
void TreadmillHandler::stop()
{
    // Always disarm the pause timeout — either we're explicitly stopping, or the
    // pause timeout handler cleared the session flags and is about to disconnect.
    m_pauseTimeoutDeadline = 0;

    // Commits the stored paused session if one is pending — no new STOPPED
    // transition will arrive from the belt to do it for us.
    if (m_tracker.onStopCommand())
    {
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
    m_tracker.onPauseCommand();
    uint8_t packet[27];
    // CMD1=0x05 for pause, MODE=0x0A for pause, keep last speed
    BA05Protocol::makePacket(m_lastSpeed, 0x05, 0x0A, m_seqCounter++, packet);
    printCommandPacket("pause", packet, sizeof(packet));
    this->sendCommand(packet, sizeof(packet));
}



void TreadmillHandler::sendKeepalive()
{
    if (!isConnected()) return;
    if (m_stopKeepalives) return; // intentional drop in progress — let supervision timeout fire

    uint8_t packet[9];
    BA05Protocol::makeKeepalive(m_seqCounter++, packet);
    this->sendCommand(packet, sizeof(packet));
}

// Sent immediately after subscribing to notifications.
// The Q1 Classic Pro silently disconnects (BLE reason=531, remote user terminated)
// if nothing is written to it within ~300ms of connection.
void TreadmillHandler::sendInitSequence()
{
    if (!isConnected() || !m_pWriteCharacteristic) return;
    sendKeepalive();
    log_i("Init sequence complete - keepalive sent");
}

void TreadmillHandler::begin(NimBLEAddress address)
{
    m_state.loadFromNVS();

    // Load user-configurable settings from NVS. Defaults apply on first boot.
    {
        Preferences prefs;
        prefs.begin("pk_cfg", true); // read-only
        m_autoReconnect      = prefs.getBool("ar",    true);
        m_idleDisconnectMins = prefs.getUShort("idle", 30);
        m_pauseTimeoutMins   = prefs.getUShort("pause", 10);
        prefs.end();
        log_i("Settings loaded: autoReconnect=%d idleDisconnect=%u min pauseTimeout=%u min",
              m_autoReconnect, m_idleDisconnectMins, m_pauseTimeoutMins);
    }

    // Pre-populate m_lastData totals from NVS so that the first publishState()
    // call (before any BLE connection) sends the correct cumulative values.
    // Without this, m_lastData.total* = 0, the retained MQTT topic gets overwritten
    // with "0", and the HA utility_meter counts the full NVS total as a fresh
    // daily increase on every reboot — causing a spike equal to the entire NVS total.
    m_lastData.totalDistanceKm  = m_state.getTotalDistanceKm();
    m_lastData.totalSteps       = m_state.getTotalSteps();
    m_lastData.totalCalories    = m_state.getTotalCalories();
    m_lastData.totalDurationSec = m_state.getTotalDurationSec();

    m_targetAddress = address;
    m_doConnect = true;
}

void TreadmillHandler::saveSettings()
{
    Preferences prefs;
    prefs.begin("pk_cfg", false); // read-write
    prefs.putBool("ar",     m_autoReconnect);
    prefs.putUShort("idle",  m_idleDisconnectMins);
    prefs.putUShort("pause", m_pauseTimeoutMins);
    prefs.end();
    log_i("Settings saved: autoReconnect=%d idleDisconnect=%u min pauseTimeout=%u min",
          m_autoReconnect, m_idleDisconnectMins, m_pauseTimeoutMins);
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

    // Send the initial keepalive once GATT is ready (onConnect set this flag).
    // Cannot write from onConnect() directly — writeValue(true) blocks for an ATT
    // response that would be processed by the same NimBLE task, causing deadlock.
    if (m_sendInitNow && isConnected())
    {
        m_sendInitNow = false;
        sendInitSequence();
        m_lastKeepalive = millis();
    }

    // Send keepalive every KEEPALIVE_INTERVAL ms (200ms) to maintain connection
    if (isConnected() && (millis() - m_lastKeepalive >= KEEPALIVE_INTERVAL))
    {
        sendKeepalive();
        m_lastKeepalive = millis();
    }

    // Pause timeout: commit session and disconnect if belt has been paused too long.
    // Prevents data loss if the user walks away and never resumes or stops.
    if (m_pauseTimeoutDeadline != 0 && millis() >= m_pauseTimeoutDeadline &&
        m_tracker.isPaused() && isConnected())
    {
        log_w("Pause timeout (%u min) — committing paused session and disconnecting",
              m_pauseTimeoutMins);
        m_pauseTimeoutDeadline = 0;

        // Commits the paused snapshot, clears the tracker's session flags (so the
        // stop() below can't double-commit) and returns the summary to publish.
        m_lastData = m_tracker.onPauseTimeout(m_lastData);
        if (m_onDataUpdate) m_onDataUpdate(m_lastData);

        m_autoReconnect        = false;
        m_userRequestedConnect = false;

        stop(); // sends stop command to belt
        // Stop keepalives — let supervision timeout fire naturally (reason=520, HCI 0x08).
        // Calling disconnect() would send HCI 0x16, putting Q1 in a deep kicking phase
        // (8 cycles) on the next reconnect. Supervision timeout → lighter phase (2-3 cycles).
        m_intentionalDrop = true;
        m_stopKeepalives  = true;
        log_i("Pause timeout: stopping keepalives for intentional supervision timeout");
    }

    // Idle-disconnect timer: auto-disconnect after configured idle period.
    // Only fires when connected and not mid-session (timer is disarmed on RUNNING).
    if (m_idleDisconnectDeadline != 0 && millis() >= m_idleDisconnectDeadline && isConnected())
    {
        log_i("Idle disconnect timer fired (%u min) — stopping keepalives for intentional supervision timeout",
              m_idleDisconnectMins);
        m_idleDisconnectDeadline = 0; // onDisconnect() also clears this, but be explicit
        m_autoReconnect          = false;
        m_userRequestedConnect   = false;
        // Stop keepalives — let supervision timeout fire naturally (reason=520, HCI 0x08).
        // Calling disconnect() sends HCI 0x16, putting Q1 in a deep kicking phase (8 cycles)
        // on the next connect. Supervision timeout → lighter phase (2-3 cycles).
        m_intentionalDrop = true;
        m_stopKeepalives  = true;
    }

    // Execute pending command once reconnected and POST_CONNECT_COOLDOWN has elapsed.
    // Pending commands are queued by start()/setSpeed() when called while disconnected.
    if (m_pendingCmd != PendingCmd::NONE &&
        isConnected() &&
        !m_sendInitNow &&
        millis() - m_lastConnectTime >= POST_CONNECT_COOLDOWN)
    {
        log_i("Executing pending command after reconnect");
        PendingCmd cmd = m_pendingCmd;
        m_pendingCmd = PendingCmd::NONE;
        if (cmd == PendingCmd::START)
            start();
        else if (cmd == PendingCmd::SET_SPEED)
            setSpeed(m_pendingSpeed);
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

    // Deferred NVS write. addSession() only touches memory (it runs on the NimBLE
    // task), so the flash write happens here on the main loop instead.
    m_state.flush();
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

    // deleteAttributes=false: reuse cached GATT service/characteristic handles from the
    // previous connection. Full discovery was causing the belt to kick the connection
    // (reason=531) before service enumeration completed — especially on single-core C3
    // where BLE tasks share CPU with the main loop. On first boot the cache is empty
    // so discovery runs normally; subsequent reconnects skip it entirely.
    if (!m_pClient->isConnected() && !m_pClient->connect(m_targetAddress, false, true))
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

    // Send an immediate keepalive now that we have the write characteristic.
    // The Q1 kicks the connection (reason=531) if it receives no BLE write within
    // ~300ms of connecting. On ESP32 dual-core, characteristic discovery takes
    // 600-1100ms, and by the time we reach the subscribe below the belt has already
    // kicked. Sending here resets the belt's kick timer before we retrieve the
    // notify characteristic.
    sendKeepalive();
    log_i("Early keepalive sent to reset belt kick timer");

    m_pNotifyCharacteristic = pService->getCharacteristic(CHARACTERISTIC_NOTIFY_STATE_UUID);
    if (!m_pNotifyCharacteristic)
    {
        log_e("Notify characteristic not found!");
        m_pClient->disconnect();
        return false;
    }

    if (!m_pNotifyCharacteristic->canNotify())
    {
        log_e("Notify characteristic cannot notify!");
        m_pClient->disconnect();
        return false;
    }

    // Reset ALL per-connection state BEFORE subscribing so that any notification
    // that arrives during the remainder of connectToDevice() sees a clean slate.
    m_lastConnectTime         = millis();
    m_logRawFirstPacket       = true;
    m_sendInitNow             = false;
    m_reconnectNotBefore      = 0;
    m_intentionalDrop         = false;
    m_stopKeepalives          = false;
    // Baselines, packet-type tracking and session/pause flags.
    m_tracker.onConnected(m_lastConnectTime);

    // Subscribe early — the Q1 needs BLE activity within the first few hundred ms
    // of connect or it terminates the session. Calling subscribe() here (before
    // onConnect fires) provides that early traffic. The write may log rc:7 if the
    // ATT layer isn't fully ready yet, but NimBLE retries internally and the
    // Q1's cached CCCD keeps notifications flowing regardless.
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

    // All session accounting happens in SessionTracker (pure, host-tested).
    TreadMillData data = m_tracker.onPacket(parsed, m_lastData, millis());

    // Raw hex dump of the first full packet after connect. The tracker logs the
    // decoded form; only the handler still has the bytes to dump.
    if (m_logRawFirstPacket && length >= 50 &&
        (parsed.packetType == 0x2F || parsed.packetType == 0x34))
    {
        m_logRawFirstPacket = false;
        char hex[length * 3 + 1];
        for (size_t i = 0; i < length; i++) sprintf(hex + i * 3, "%02X ", pData[i]);
        hex[length * 3 - 1] = '\0';
        log_w("FIRST POST-CONNECT RAW: %s", hex);
    }

    m_lastData = data;
    m_lastDataTimestamp = millis();
    // Signal handle() (Core 1) that new data is ready to publish.
    // Do NOT call m_onDataUpdate() here — notifyCallback runs on Core 0 (NimBLE task)
    // and PubSubClient::publish() is not thread-safe. Calling it concurrently from
    // Core 0 and Core 1 (dead reckoning) caused ECONNRESET / double-publish bugs.
    m_newDataAvailable = true;
}
