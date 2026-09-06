#include "TreadmillHandler.h"
#include "ba05protocol.h"

// -DUSE_ESP_IDF_LOG (spec 4.14) makes log_x() expand to
// ESP_LOG_LEVEL_LOCAL(..., TAG, ...); esp32-hal-log.h has no default TAG.
static const char *TAG = "Treadmill";

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
bool TreadmillHandler::setSpeed(uint16_t speed)
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
        return true; // queued, not failed
    }
    uint8_t packet[27];
    // CMD1=0x01 for running, MODE=0x0C for running
    BA05Protocol::makePacket(speed, 0x01, 0x0C, m_seqCounter++, packet);
    printCommandPacket("setSpeed", packet, sizeof(packet));
    return this->sendCommand(packet, sizeof(packet));
}

// BA05 Protocol: Start at the given raw speed (configured start speed by default).
bool TreadmillHandler::startAtRaw(uint16_t raw)
{
    if (!isConnected())
    {
        log_i("startAtRaw() while disconnected — queuing command and auto-connecting");
        m_pendingCmd   = PendingCmd::START;
        m_pendingSpeed = raw;
        m_autoReconnect        = true;
        m_userRequestedConnect = false;
        m_reconnectNotBefore   = 0;
        m_doConnect            = true;
        return true; // queued, not failed
    }
    uint8_t packet[27];
    m_lastSpeed = raw;
    // CMD1=0x01 for running, MODE=0x0C for running
    BA05Protocol::makePacket(raw, 0x01, 0x0C, m_seqCounter++, packet);
    printCommandPacket("start", packet, sizeof(packet));
    return this->sendCommand(packet, sizeof(packet));
}

// BA05 Protocol: Stop
bool TreadmillHandler::stop()
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
    // CMD1=0x05 for stop, MODE=0x08 for stop, speed=0
    BA05Protocol::makePacket(0, 0x05, 0x08, m_seqCounter++, packet);
    printCommandPacket("stop", packet, sizeof(packet));
    const bool ok = this->sendCommand(packet, sizeof(packet));
    if (ok)
    {
        // Only clear the resume seed once the stop actually reached the belt
        // (or was queued) — if it failed while disconnected, resume() should
        // still restart at the speed the belt was last actually running.
        m_lastSpeed = 0;
    }
    return ok;
}

// BA05 Protocol: Pause (keep current speed in packet)
bool TreadmillHandler::pause()
{
    m_tracker.onPauseCommand();
    uint8_t packet[27];
    // CMD1=0x05 for pause, MODE=0x0A for pause, keep last speed
    BA05Protocol::makePacket(m_lastSpeed, 0x05, 0x0A, m_seqCounter++, packet);
    printCommandPacket("pause", packet, sizeof(packet));
    return this->sendCommand(packet, sizeof(packet));
}

void TreadmillHandler::restoreTotals(float distKm, uint32_t steps, uint32_t calories, uint32_t durationSec)
{
    m_state.restoreTotals(distKm, steps, calories, durationSec);
    m_snapshot.modify([&](TreadMillData& d) {
        d.totalDistanceKm  = m_state.getTotalDistanceKm();
        d.totalSteps       = m_state.getTotalSteps();
        d.totalCalories    = m_state.getTotalCalories();
        d.totalDurationSec = m_state.getTotalDurationSec();
    });
}

bool TreadmillHandler::requestDisconnect()
{
    // Cancel an in-flight connect attempt (kick phase / retry loop).
    // m_autoReconnect = false suppresses the next scheduled attempt in
    // handle(). Since spec 4.17 the connect is asynchronous, so a link
    // request may genuinely be pending right now: cancel it with
    // cancelConnect() — never disconnect(), which on a link that never came
    // up is the HCI 0x16 that starts the Q1's kicking phase.
    // The stage is tested BEFORE isConnected(): NimBLE flips isConnected() to
    // true at the GAP connect event, ~100 ms before onConnect() raises
    // m_linkUp, so a link that is still coming up would otherwise fall
    // straight through to the disconnect() path below.
    // (SETUP runs to completion inside handle() on this same task, so the
    // stage can only be IDLE or LINKING here.)
    if (isConnecting())
    {
        log_i("Connect cancelled by user while connecting");
        if (m_linkStage == LinkStage::LINKING && m_pClient)
        {
            // disconnect() only once onConnect() has confirmed the link is
            // genuinely up; on a still-pending link it is the HCI 0x16 above.
            if (m_linkUp)
            {
                m_pClient->disconnect();
            }
            else
            {
                m_pClient->cancelConnect();
            }
            m_linkStage = LinkStage::IDLE;
            m_linkUp    = false;
        }
        m_autoReconnect        = false;
        m_userRequestedConnect = false;
        m_doConnect            = false;
        m_reconnectNotBefore   = 0;
        m_pendingCmd           = PendingCmd::NONE;
        m_connectAttempts      = 0;
        return true;
    }
    if (!isConnected())
    {
        return false;
    }
    // Safety check — don't disconnect while belt is active
    TreadMillData snap = m_snapshot.read();
    if (snap.status == TreadMillData::RUNNING ||
        snap.status == TreadMillData::PAUSED ||
        snap.status == TreadMillData::COUNTDOWN)
    {
        log_w("Disconnect blocked — belt is active (status=%d)", (int)snap.status);
        return false;
    }
    log_i("Manual disconnect requested");
    m_autoReconnect = false;        // prevent onDisconnect() from immediately reconnecting
    m_userRequestedConnect = false; // user explicitly turned off — don't auto-reconnect
    m_reconnectNotBefore = 0;
    m_pClient->disconnect();
    // Don't deleteClient here — onDisconnect() fires shortly after and handles
    // cleanup. Deleting here and again in onDisconnect() is a double-free.
    return true;
}

bool TreadmillHandler::requestConnect()
{
    if (isConnected())
    {
        // A link that is up but muted — keepalives stopped for an intentional
        // drop, or the keepalive-zombie debounce — is not a connection the user
        // can use, and left alone it only ends when the supervision timeout
        // fires. The user pressing Connect means they want to walk: force the
        // drop now so onDisconnect() -> m_doConnect brings up a fresh link.
        if (!m_stopKeepalives)
        {
            return false;
        }
        log_w("Manual connect on a muted link — forcing disconnect to re-link");
        m_autoReconnect        = true;
        m_userRequestedConnect = true;
        m_reconnectNotBefore   = 0;
        m_connectAttempts      = 0;
        // m_dropDeadlineMs is left as is: onDisconnect() clears it, and if the
        // terminate below is refused the deadline (when armed — only the
        // debounce arms it) is the remaining safety net.
        if (!m_pClient->disconnect())
        {
            log_e("disconnect() refused on the muted link — escalation deadline still armed");
        }
        return true;
    }
    log_i("Manual connect requested");
    m_autoReconnect = true;
    m_userRequestedConnect = true; // user explicitly turned on — keep retrying after idle kicks
    m_reconnectNotBefore = 0;      // allow immediate first attempt
    m_doConnect = true;
    m_connectAttempts = 0;
    return true;
}



void TreadmillHandler::sendKeepalive()
{
    if (!isConnected()) return;
    // The link comes up ~100 ms before onConnect() fires and completeSetup()
    // fetches the characteristic (spec 4.17), so isConnected() alone is not
    // enough — without this the seq counter would advance on a write that
    // sendCommand() refuses outright.
    if (m_pWriteCharacteristic == nullptr) return;
    if (m_stopKeepalives) return; // intentional drop in progress — let supervision timeout fire

    uint8_t packet[9];
    BA05Protocol::makeKeepalive(m_seqCounter, packet);
    // Burn the sequence number only on a write the belt actually saw: the
    // on-air numbering for successful sends is unchanged, but a refused or
    // failed write no longer leaves a gap the belt has to reconcile.
    if (this->sendCommand(packet, sizeof(packet)))
    {
        ++m_seqCounter;
    }
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
        m_startSpeedTenths   = prefs.getUShort("start", 10);
        m_flightsAutoShow    = prefs.getBool("fl_auto", true);
        prefs.end();
        // Clamp on load too — guards against a stale/out-of-range value written
        // by an older firmware or a manual NVS edit.
        {
            const uint16_t minTenths = (uint16_t)lroundf(SPEED_MIN_MPH * 10.0f);
            const uint16_t maxTenths = (uint16_t)lroundf(SPEED_MAX_MPH * 10.0f);
            if (m_startSpeedTenths < minTenths) m_startSpeedTenths = minTenths;
            if (m_startSpeedTenths > maxTenths) m_startSpeedTenths = maxTenths;
        }
        log_i("Settings loaded: autoReconnect=%d idleDisconnect=%u min pauseTimeout=%u min startSpeed=%u tenths flightsAutoShow=%d",
              m_autoReconnect, m_idleDisconnectMins, m_pauseTimeoutMins, m_startSpeedTenths,
              m_flightsAutoShow);
    }

    // Pre-populate m_snapshot totals from NVS so that the first publishState()
    // call (before any BLE connection) sends the correct cumulative values.
    // Without this, totals start at 0, the retained MQTT topic gets overwritten
    // with "0", and the HA utility_meter counts the full NVS total as a fresh
    // daily increase on every reboot — causing a spike equal to the entire NVS total.
    m_snapshot.modify([&](TreadMillData& d) {
        d.totalDistanceKm  = m_state.getTotalDistanceKm();
        d.totalSteps       = m_state.getTotalSteps();
        d.totalCalories    = m_state.getTotalCalories();
        d.totalDurationSec = m_state.getTotalDurationSec();
    });

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
    prefs.putUShort("start", m_startSpeedTenths);
    prefs.putBool("fl_auto", m_flightsAutoShow);
    prefs.end();
    log_i("Settings saved: autoReconnect=%d idleDisconnect=%u min pauseTimeout=%u min startSpeed=%u tenths flightsAutoShow=%d",
          m_autoReconnect, m_idleDisconnectMins, m_pauseTimeoutMins, m_startSpeedTenths,
          m_flightsAutoShow);
}

void TreadmillHandler::setStartSpeedMph(float mph)
{
    if (mph < SPEED_MIN_MPH) mph = SPEED_MIN_MPH;
    if (mph > SPEED_MAX_MPH) mph = SPEED_MAX_MPH;
    m_startSpeedTenths = (uint16_t)lroundf(mph * 10.0f);
    saveSettings();
    log_i("Start speed set: %.1f mph (%u tenths)", getStartSpeedMph(), m_startSpeedTenths);
}

void TreadmillHandler::setFlightsAutoShow(bool on)
{
    m_flightsAutoShow = on;
    saveSettings();
    log_i("Flights auto-show set: %d", m_flightsAutoShow);
}

void TreadmillHandler::setConnectHold(bool hold)
{
    if (hold == m_connectHold)
    {
        return; // called every loop; only the transition is worth a line
    }
    m_connectHold = hold;
    log_i("BLE connect hold %s", m_connectHold ? "on" : "off");
}

void TreadmillHandler::setIdleDisconnectMins(uint16_t mins)
{
    m_idleDisconnectMins = mins;
    saveSettings();
    // If the timer is currently armed, rearm it with the new duration.
    if (m_idleDisconnectDeadline != 0)
    {
        if (mins == 0)
        {
            m_idleDisconnectDeadline = 0;
            log_i("Idle disconnect timer disarmed (mins=0)");
        }
        else
        {
            m_idleDisconnectDeadline = m_idleDisconnectArmedAt + (uint32_t)mins * 60000UL;
            unsigned long elapsed = millis() - m_idleDisconnectArmedAt;
            log_i("Idle disconnect timer rearmed: %u min (%lu s elapsed)",
                  mins, elapsed / 1000);
        }
    }
}

void TreadmillHandler::setPauseTimeoutMins(uint16_t mins)
{
    m_pauseTimeoutMins = mins;
    saveSettings();
    // If the timer is currently armed, rearm it with the new duration.
    if (m_pauseTimeoutDeadline != 0)
    {
        if (mins == 0)
        {
            m_pauseTimeoutDeadline = 0;
            log_i("Pause timeout timer disarmed (mins=0)");
        }
        else
        {
            m_pauseTimeoutDeadline = m_pauseTimeoutArmedAt + (uint32_t)mins * 60000UL;
            unsigned long elapsed = millis() - m_pauseTimeoutArmedAt;
            log_i("Pause timeout timer rearmed: %u min (%lu s elapsed)",
                  mins, elapsed / 1000);
        }
    }
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
            m_snapshot.modify([&](TreadMillData& d) { d.status = TreadMillData::DISCONNECTED; });
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
            log_e("Write failed: GATT layer broken despite isConnected()=true (zombie)");
            m_snapshot.modify([&](TreadMillData& d) { d.status = TreadMillData::DISCONNECTED; });
            m_newDataAvailable = true;

            // Debounce (2026-09-06): a single failed heartbeat is not worth an
            // HCI 0x16. The belt reads 0x16 as a local host termination and
            // answers it with a multi-cycle kick phase; letting the 6 s
            // supervision timeout fire instead gives it 0x08 and the lighter
            // path (doc/Q1_BLE_NOTES.md). Same for any write issued while a
            // link stage is still in flight — that link may be seconds old and
            // the write may simply have raced the GATT setup.
            if (!isUserCommand || m_linkStage != LinkStage::IDLE)
            {
                uint32_t escalateMs = m_connTimeoutMs * 2;
                if (escalateMs < kDropEscalateMinMs) escalateMs = kDropEscalateMinMs;
                if (escalateMs > kDropEscalateMaxMs) escalateMs = kDropEscalateMaxMs;
                if (isUserCommand)
                    log_w("command write failed during link setup — letting supervision timeout drop the link (forced after %u ms)",
                          escalateMs);
                else
                    log_w("keepalive write failed — letting supervision timeout drop the link (forced after %u ms)",
                          escalateMs);
                m_stopKeepalives  = true;
                m_intentionalDrop = true;
                // Bounded: if the link is still up after this, handle() forces
                // disconnect() — see m_dropDeadlineMs in the header.
                if (m_dropDeadlineMs == 0)
                {
                    // | 1: 0 is the "not armed" sentinel.
                    m_dropDeadlineMs = (millis() + escalateMs) | 1u;
                }
            }
            else
            {
                // User sent a command — they want to walk. Re-arm auto-reconnect so
                // onDisconnect() → m_doConnect chain fires even if a prior session
                // had set m_autoReconnect=false.
                m_autoReconnect        = true;
                m_userRequestedConnect = true;
                // disconnect() fires onDisconnect() asynchronously on Core 0.
                // onDisconnect() nulls the characteristic pointers and sets m_doConnect=true
                // (if m_autoReconnect). Safe to call from Core 1 / handle() context.
                // Re-check the link: it can have dropped between the failed write
                // and here, in which case onDisconnect() is already on its way.
                if (m_pClient->isConnected())
                {
                    log_e("Zombie on a user command — forcing disconnect");
                    m_pClient->disconnect();
                }
            }
        }
        return false;
    }

    // A user command that the belt accepted while an intentional drop was
    // pending (pause timeout / idle disconnect window) means the belt may now be
    // running on our say-so: keep the link alive rather than letting the
    // deliberate supervision-timeout drop fire under a moving belt.
    // Only start/speed commands (cmd1 = 0x01): stop/pause leave no moving belt,
    // so an idle/pause drop can stand. Auto-reconnect is re-armed too — the
    // idle/pause paths turned it off before muting, and a belt we just started
    // must survive a mid-session kick.
    if (isSpeedSet && m_stopKeepalives)
    {
        log_w("Start/speed command accepted on a muted link — cancelling the intentional drop");
        m_stopKeepalives       = false;
        m_intentionalDrop      = false;
        m_dropDeadlineMs       = 0;
        m_autoReconnect        = true;
        m_userRequestedConnect = true;
    }
    return true;
}

bool TreadmillHandler::handle()
{
    // Raised by any path that used to invoke the data-update callback. The caller
    // turns it into a single controller.publish() once per cycle.
    bool newData = false;

    // handles reconnection
    // m_reconnectNotBefore is set after an idle kick (user-requested connect) to add
    // a 60s backoff between reconnect attempts — avoids rapid beeping while idle.
    // m_connectHold (spec 4.15): main.cpp holds background connects off the air
    // while WiFi/MQTT are bringing up on the shared antenna. A connect the user
    // asked for is never held.
    // m_linkStage == IDLE: never start a second link while one is in flight —
    // the 6 s link timeout outlives the 5 s attempt spacing (spec 4.17).
    if (m_doConnect && m_linkStage == LinkStage::IDLE &&
        (millis() - m_lastConnectAttempt > 5000) && m_autoReconnect &&
        (m_reconnectNotBefore == 0 || millis() >= m_reconnectNotBefore) &&
        (!m_connectHold || m_userRequestedConnect))
    {
        m_lastConnectAttempt = millis();
        ++m_connectAttempts;
        if (!this->beginLink())
        {
            // connect() refused the request outright (no link was ever queued).
            log_e("Failed to connect - Retrying in 5 seconds...");
        }
    }

    // Staged connect, stage 2 (spec 4.17). beginLink() only queues the connect;
    // onConnect() raises m_linkUp from the NimBLE task when the link is really
    // up, and only then may we run GATT discovery. Doing that work against a
    // pending link is what used to fail service discovery and end in the
    // disconnect() that starts the Q1's kicking phase.
    if (m_linkStage == LinkStage::LINKING)
    {
        if (!m_linkUp && (millis() - m_linkStartMs >= kLinkTimeoutMs))
        {
            // Re-test m_linkUp immediately before cancelling: onConnect() runs
            // on the NimBLE task and can raise it in the window between the
            // deadline test above and the cancelConnect() below. Cancelling a
            // link that has just come up would be an HCI 0x16 on a live
            // connection — exactly the thing that starts the Q1's kicking
            // phase. If it came up, do nothing here and let the SETUP branch
            // below run on this same tick.
            if (!m_linkUp)
            {
                // cancelConnect(), never disconnect(): the link never came up, and
                // an HCI 0x16 on a pending connection is what sends the Q1 into its
                // kicking phase. m_doConnect stays true — the 5 s m_lastConnectAttempt
                // spacing schedules the next try.
                if (m_pClient)
                {
                    m_pClient->cancelConnect();
                }
                log_w("link timeout after %lu ms, cancelled",
                      (unsigned long)(millis() - m_linkStartMs));
                m_linkStage = LinkStage::IDLE;
            }
        }

        if (m_linkStage == LinkStage::LINKING && m_linkUp)
        {
            log_i("link up after %lu ms", (unsigned long)(millis() - m_linkStartMs));
            m_linkStage = LinkStage::SETUP;
            if (this->completeSetup())
            {
                log_i("Connection successful.");
                m_doConnect = false;
                // Deliberately NOT resetting m_connectAttempts here: a successful
                // setup only means the kick phase's GATT handshake
                // went through, not that the link is stable — the Q1 can still
                // bounce us straight back into another attempt. The Dial's
                // "attempt N" should keep counting through that. See the
                // POST_CONNECT_COOLDOWN check below for the actual reset point.
                m_lastKeepalive = millis(); // Reset keepalive timer on connect
                // m_lastConnectTime is now set inside completeSetup() before subscription
                // so that notification timing is accurate during the setup window.
            }
            else
            {
                log_e("Failed to connect - Retrying in 5 seconds...");
            }
            m_linkStage = LinkStage::IDLE;
        }
    }

    // Attempt counter: only reset once the connection has been stable for
    // POST_CONNECT_COOLDOWN — see the comment above. Otherwise it's reset
    // only in requestConnect() (a fresh user-initiated connect). Cheap to
    // re-assign 0 every tick once stable; no separate "already reset" flag
    // needed.
    if (isConnected() && (millis() - m_lastConnectTime >= POST_CONNECT_COOLDOWN))
    {
        m_connectAttempts = 0;
    }

    // Send the initial keepalive once GATT is ready (onConnect set this flag).
    // Cannot write from onConnect() directly — writeValue(true) blocks for an ATT
    // response that would be processed by the same NimBLE task, causing deadlock.
    // m_pWriteCharacteristic gate: NimBLE flips isConnected() to true at the GAP
    // connect event, ~100 ms before onConnect() fires and completeSetup() fetches
    // the characteristic (spec 4.17). handle() runs inside that window, so every
    // write path is gated on the characteristic, not just on the link.
    if (m_sendInitNow && isConnected() && m_pWriteCharacteristic != nullptr)
    {
        m_sendInitNow = false;
        sendInitSequence();
        m_lastKeepalive = millis();
    }

    // Bounded escalation for the keepalive-zombie path: the debounce above
    // (sendCommand) stopped keepalives and armed this deadline. If the link
    // layer is still up when it passes, the supervision timeout is not coming
    // soon enough — force the drop so the reconnect chain runs.
    // Gated on stage IDLE and the muted flag: the debounce can re-arm the
    // deadline after onDisconnect() has already cleared it (the write failed
    // on a link that dropped in the same instant), and a stale deadline must
    // never disconnect() the NEXT link while it is still LINKING — that is the
    // HCI 0x16 on a coming-up link that spec 4.17 exists to avoid.
    if (m_dropDeadlineMs != 0 && (int32_t)(millis() - m_dropDeadlineMs) >= 0)
    {
        m_dropDeadlineMs = 0;
        if (m_linkStage == LinkStage::IDLE && m_stopKeepalives &&
            m_pClient && m_pClient->isConnected())
        {
            log_e("Zombie link still up after the escalation deadline — forcing disconnect");
            if (!m_pClient->disconnect())
            {
                log_e("disconnect() refused on the zombie link");
            }
        }
    }

    // Heartbeat: reply once to each belt notification (the PitPat app's pattern, ~1 Hz),
    // with a fallback only if the belt goes quiet. See KEEPALIVE_FALLBACK_MS in the header.
    // Gated on the write characteristic as well as the link — see the note above.
    if (isConnected() && m_pWriteCharacteristic != nullptr)
    {
        bool reply = m_keepaliveReplyPending;
        if (reply || (millis() - m_lastKeepalive >= KEEPALIVE_FALLBACK_MS))
        {
            m_keepaliveReplyPending = false;
            sendKeepalive();
            m_lastKeepalive = millis();
        }
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
        TreadMillData committed = m_tracker.onPauseTimeout(m_snapshot.read());
        m_snapshot.write(committed);
        newData = true;

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
        m_pWriteCharacteristic != nullptr &&
        !m_sendInitNow &&
        millis() - m_lastConnectTime >= POST_CONNECT_COOLDOWN)
    {
        log_i("Executing pending command after reconnect");
        PendingCmd cmd = m_pendingCmd;
        m_pendingCmd = PendingCmd::NONE;
        if (cmd == PendingCmd::START)
            startAtRaw(m_pendingSpeed);
        else if (cmd == PendingCmd::SET_SPEED)
            setSpeed(m_pendingSpeed);
    }

    // Publish new BLE packet data from Core 1 (main loop).
    // notifyCallback() (Core 0) sets this flag when it has written fresh data to m_snapshot.
    // We clear it here so that another notifyCallback packet arriving during the
    // caller's publish re-sets the flag and gets published next cycle.
    if (m_newDataAvailable)
    {
        m_newDataAvailable = false;
        newData = true;
    }

    // Check for connection timeout — catches radio loss / silent disconnects where
    // onDisconnect() never fired. Skip if already DISCONNECTED to avoid repeated
    // MQTT publishes every 30s when the idle kick has cleanly stopped auto-reconnect.
    TreadMillData connCheck = m_snapshot.read();
    if (connCheck.status != TreadMillData::DISCONNECTED &&
        millis() - m_lastPacketMs > CONNECTION_TIMEOUT * 1000)
    {
        unsigned long elapsedSec = (millis() - m_lastPacketMs) / 1000;
        log_w("No data received for %lus (threshold %ds) - marking disconnected.",
              elapsedSec, CONNECTION_TIMEOUT);
        m_snapshot.modify([&](TreadMillData& d) { d.status = TreadMillData::DISCONNECTED; });
        m_lastPacketMs = millis(); // reset so the check doesn't re-fire immediately
        newData = true;
    }

    // Deferred NVS write. addSession() only touches memory (it runs on the NimBLE
    // task), so the flash write happens here on the main loop instead.
    m_state.flush();

    return newData;
}

// Staged connect, stage 1 (spec 4.17): create/configure the client and issue the
// asynchronous connect. Nothing here blocks — NimBLE's connect() with
// asyncConnect=true returns as soon as the request is queued, so the loop task
// stays responsive while the link comes up. handle()'s LINKING block waits for
// onConnect() to raise m_linkUp (or for kLinkTimeoutMs) and then runs
// completeSetup().
bool TreadmillHandler::beginLink()
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

    m_linkUp      = false;
    m_linkStartMs = millis();
    // A fresh link measures the fallback heartbeat from now. Without this the
    // stale m_lastKeepalive from the previous connection is already older than
    // KEEPALIVE_FALLBACK_MS, and handle() fires an unsolicited fallback write
    // in the first seconds of the new connection — exactly the unsolicited
    // traffic the belt kicks us for (doc/Q1_BLE_NOTES.md, revision 2026-09-03).
    m_lastKeepalive = millis();

    // Already linked — same short-circuit the old code had (isConnected() skipped
    // the connect call). Mark the link up so handle()'s LINKING block falls
    // straight through to SETUP on this same tick.
    if (m_pClient->isConnected())
    {
        m_linkUp    = true;
        m_linkStage = LinkStage::LINKING;
        return true;
    }

    // deleteAttributes=false: reuse cached GATT service/characteristic handles from the
    // previous connection. Full discovery was causing the belt to kick the connection
    // (reason=531) before service enumeration completed — especially on single-core C3
    // where BLE tasks share CPU with the main loop. On first boot the cache is empty
    // so discovery runs normally; subsequent reconnects skip it entirely.
    // asyncConnect=true (the third argument): connect() only queues the request and
    // returns — a true return means "accepted", not "linked". The link is up when
    // onConnect() fires. Bracketed for the RTC log ring (spec 4.16): a "link begin"
    // with no "link up"/"link timeout" after it names the stage that stalled.
    log_i("link begin");
    if (!m_pClient->connect(m_targetAddress, false, true))
    {
        log_e("Failed to connect to treadmill at %s", m_targetAddress.toString().c_str());
        return false;
    }
    m_linkStage = LinkStage::LINKING;
    return true;
}

// Staged connect, stage 3 (spec 4.17): the GATT setup, run on the loop task once
// the link is genuinely up. Everything from here on is the pre-4.17 code —
// service discovery, write characteristic, early keepalive, notify subscribe,
// negotiated-params log — including its failure paths, which may still call
// disconnect() because by this point the link is real.
bool TreadmillHandler::completeSetup()
{
    log_i("setup begin");

    // NOTE: delay(500) removed. The Q1 Classic Pro disconnects (reason=531, remote user
    // terminated) if it receives no write within ~300ms of connecting. Every millisecond
    // spent here is time we're not spending on the handshake.

    // The link is up before we get here, so the service is expected on attempt 1;
    // 3 x 100ms is only a guard against NimBLE's discovery landing a tick late
    // (spec 4.17, replacing the 8 x 250ms patience of spec 4.15 — that existed
    // solely to cover the pending-link case this staging removes).
    NimBLERemoteService *pService = nullptr;
    for (int retry = 0; retry < 3; retry++)
    {
        pService = m_pClient->getService(SERVICE_PAD_UUID);
        if (pService)
        {
            log_i("Service found on attempt %d", retry + 1);
            break;
        }
        log_w("Service not found, retry %d/3...", retry + 1);
        delay(100);
    }
    if (!pService)
    {
        log_e("Failed to find treadmill service UUID after 3 retries: %s", SERVICE_PAD_UUID);
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

    // A fresh link is never a muted one: clear the intentional-drop flags before
    // the early keepalive below, or sendKeepalive() would refuse it if a
    // previous drop was still flagged.
    m_intentionalDrop = false;
    m_stopKeepalives  = false;
    m_dropDeadlineMs  = 0;

    // Send an immediate keepalive now that we have the write characteristic.
    // The Q1 kicks the connection (reason=531) if it receives no BLE write within
    // ~300ms of connecting. On ESP32 dual-core, characteristic discovery takes
    // 600-1100ms, and by the time we reach the subscribe below the belt has already
    // kicked. Sending here resets the belt's kick timer before we retrieve the
    // notify characteristic.
    sendKeepalive();
    log_i("Early keepalive sent");

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
    // that arrives during the remainder of completeSetup() sees a clean slate.
    m_lastConnectTime         = millis();
    m_logRawFirstPacket       = true;
    m_sendInitNow             = false;
    m_reconnectNotBefore      = 0;
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
    // Captured once per link; a later peripheral-initiated parameter update is
    // not tracked — acceptable because the escalation deadline is clamped to
    // 12-30 s anyway.
    m_connTimeoutMs = connInfo.getConnTimeout() ? connInfo.getConnTimeout() * 10u : 6000u;
    log_i("Negotiated BLE params: interval=%u (%.1fms) latency=%u timeout=%u (%ums)",
          connInfo.getConnInterval(), connInfo.getConnInterval() * 1.25f,
          connInfo.getConnLatency(),
          connInfo.getConnTimeout(),
          connInfo.getConnTimeout() * 10);

    log_i("setup end");
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
    m_keepaliveReplyPending = true; // answer every belt frame with one heartbeat (loop task sends it)
    if (!parsed.valid)
    {
        log_w("Unexpected or invalid packet length: %d - ignoring", length);
        return;
    }

    // All session accounting happens in SessionTracker (pure, host-tested).
    TreadMillData prev = m_snapshot.read();
    TreadMillData data = m_tracker.onPacket(parsed, prev, millis());

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

    m_snapshot.write(data);
    m_lastPacketMs = millis();
    // Signal handle() (Core 1) that new data is ready to publish.
    // Do NOT notify observers here — notifyCallback runs on Core 0 (NimBLE task)
    // and PubSubClient::publish() is not thread-safe. Publishing concurrently from
    // Core 0 and Core 1 (dead reckoning) caused ECONNRESET / double-publish bugs.
    m_newDataAvailable = true;
}

// --- ISessionEvents ---------------------------------------------------
// These carry exactly the timer/flag code that used to sit inline in
// notifyCallback() at each of these points.

void TreadmillHandler::onSessionStarted()
{
    // Cancel idle-disconnect timer — belt is active.
    if (m_idleDisconnectDeadline != 0)
    {
        log_i("Idle disconnect timer cancelled — session started");
        m_idleDisconnectDeadline = 0;
    }
}

void TreadmillHandler::onSessionEnded()
{
    m_autoReconnect        = false;
    m_userRequestedConnect = false;

    // Arm idle-disconnect timer so BLE disconnects automatically after
    // the configured idle period. Prevents the Q1 kicking loop that
    // occurs when we stay connected with no belt activity.
    if (m_idleDisconnectMins > 0)
    {
        m_idleDisconnectArmedAt  = millis();
        m_idleDisconnectDeadline = m_idleDisconnectArmedAt + (uint32_t)m_idleDisconnectMins * 60000UL;
        log_i("Idle disconnect timer armed: %u min", m_idleDisconnectMins);
    }
}

void TreadmillHandler::onPaused()
{
    // Arm pause timeout — if the user never resumes, commit and disconnect.
    if (m_pauseTimeoutMins > 0)
    {
        m_pauseTimeoutArmedAt  = millis();
        m_pauseTimeoutDeadline = m_pauseTimeoutArmedAt + (uint32_t)m_pauseTimeoutMins * 60000UL;
        log_i("Pause timeout armed: %u min", m_pauseTimeoutMins);
    }
}

void TreadmillHandler::onSettledIdle()
{
    if (m_idleDisconnectMins > 0 && m_idleDisconnectDeadline == 0)
    {
        m_idleDisconnectArmedAt  = millis();
        m_idleDisconnectDeadline = m_idleDisconnectArmedAt + (uint32_t)m_idleDisconnectMins * 60000UL;
        log_i("Idle disconnect timer armed on stable idle connection: %u min", m_idleDisconnectMins);
    }
}

void TreadmillHandler::onConnect(BLEClient *pClient)
{
    log_i("Connected to device!");
    // The LINKING stage waits on this; handle() runs the GATT setup from the loop
    // task. Nothing blocking may happen here — this is the NimBLE host task.
    m_linkUp      = true;
    m_sendInitNow = true;
#if HAS_STATUS_LED
    digitalWrite(LED_BLE_PIN, HIGH); // active-high: on
#endif
}

void TreadmillHandler::onDisconnect(BLEClient *pClient, int reason)
{
    // Clear the staged-connect state first (spec 4.17): a drop mid-LINKING (a
    // connect that failed to establish) or mid-SETUP must never leave the stage
    // stuck — handle() would then start no further attempts.
    m_linkUp         = false;
    m_linkStage      = LinkStage::IDLE;
    m_dropDeadlineMs = 0;

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
    log_w("Disconnected reason=%d (HCI 0x%02X: %s)",
          reason, reason - 512, desc);

    // Intentional drop via supervision timeout — we stopped keepalives deliberately
    // so Q1 sees a supervision timeout (0x08) rather than HCI 0x16 (local host terminated).
    // Clear both flags regardless of the actual reason code; if something else fired
    // first (e.g., Q1 kicked us), we still want to clean up.
    if (m_intentionalDrop)
    {
        if ((reason - 512) == 0x08)
            log_i("Intentional drop: supervision timeout fired as expected — Q1 should see lighter kick phase");
        else
            log_w("Intentional drop: expected 0x08 but got HCI 0x%02X — cleaning up anyway", reason - 512);
        m_intentionalDrop = false;
        m_stopKeepalives  = false;
    }

    // Use m_tracker.sessionActive() as the ground truth for "belt was genuinely
    // active". This flag is only set when we observe speed > 0 (belt physically
    // moving), so it's immune to:
    //   - Phantom RUNNING status from 0x34 packets (flags=0xBC with speed=0)
    //   - Residual distanceKm from a previous session persisting on reconnect
    // Read BEFORE onDisconnected() so the backoff decision below is unaffected
    // by any state the commit clears.
    bool beltWasActive = m_tracker.sessionActive();
    TreadMillData last = m_snapshot.read();
    m_tracker.onDisconnected(last);

    // Reason 531 (HCI 0x13: remote user terminated) = treadmill kicked us.
    // When the belt is idle (stopped/disconnected) this is just the treadmill's
    // ~10s idle BLE timeout. Reconnecting immediately causes the pad to beep on
    // every connect/disconnect cycle. Instead, treat it like a manual disconnect —
    // stop auto-reconnecting and let the user reconnect when they want to walk.
    // If the belt was RUNNING/PAUSED/COUNTDOWN when kicked, reconnect immediately
    // to preserve the active session.
    bool isIdleKick = ((reason - 512) == 0x13);

    if (isIdleKick && !beltWasActive)
    {
        if (m_userRequestedConnect)
        {
            // User explicitly pressed Connect — use a short 5s backoff so the Q1's
            // kick phase (~5-8 cycles) settles in ~2 min. The longer 20s backoff
            // made the kick phase take 4+ min (30s/cycle × 8 kicks), which felt
            // like the device was broken.
            m_reconnectNotBefore = millis() + 5000UL;
            log_w("Idle kick (user-requested) — backing off 5s before reconnect");
        }
        else if (m_autoReconnect)
        {
            // Auto-reconnect (background, no user session): back off 20s to keep
            // beeping infrequent while working through the kick phase.
            m_reconnectNotBefore = millis() + 20000UL;
            log_w("Idle kick (auto-reconnect) — backing off 20s before reconnect");
        }
        // else: m_autoReconnect is already false (session ended) — don't reconnect.
    }
    else if (isIdleKick && beltWasActive)
    {
        // Mid-session kick: Q1 kicked us (reason=531) while the belt was active.
        // Without backoff we reconnect immediately, Q1 kicks again, and this loops
        // for minutes. Add a 10s pause to let Q1 cycle out of its kicking phase
        // before the next connect attempt (~10s connect + 10s wait = ~20s/cycle).
        m_reconnectNotBefore = millis() + 10000UL;
        log_w("Mid-session kick — backing off 10s before reconnect to work through kicking phase");
    }

    // Disarm both timers — either they fired and triggered this disconnect, or we
    // were kicked/timed-out externally. Either way, nothing left to fire.
    m_idleDisconnectDeadline = 0;
    m_pauseTimeoutDeadline   = 0;

    // Clear pending command only when we're not going to reconnect.
    // If m_autoReconnect is still true (kicked while connecting to execute a
    // command), preserve the command so it fires once the belt settles.
    // If we're disconnecting cleanly (pause timeout, user disconnect), the
    // command is stale and should be dropped.
    if (!m_autoReconnect)
    {
        m_pendingCmd = PendingCmd::NONE;
    }

#if HAS_STATUS_LED
    digitalWrite(LED_BLE_PIN, LOW); // active-high: off
#endif

    // Always publish DISCONNECTED so HA switch reflects the real state.
    m_snapshot.modify([&](TreadMillData& d) { d.status = TreadMillData::DISCONNECTED; });
    m_newDataAvailable = true;

    // Do NOT deleteClient here — calling NimBLEDevice::deleteClient() from within
    // a NimBLE callback (Core 0) is unsafe and corrupts the NimBLE heap, causing
    // subsequent connections to be unstable. The original pacekeeper code never
    // deletes the client — it reuses it, letting connect(addr, true, true) clear
    // the GATT cache on the next connect attempt.
    // Null the characteristic pointers — they're stale after disconnect and
    // completeSetup() re-fetches them on the next successful connection.
    m_pWriteCharacteristic  = nullptr;
    m_pNotifyCharacteristic = nullptr;

    if (m_autoReconnect)
    {
        m_doConnect = true; // Trigger reconnect in handle() loop
    }
}
