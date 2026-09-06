#include "NetTask.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <ctype.h>
#include <string.h>
#include <strings.h> // strcasecmp

#include "NetManager.h"
#include "RemoteLog.h"
#include "TreadmillHandler.h"
#include "mqttview.h"
#include "platform.h"

static const char *TAG = "NetTask";

namespace
{
// PubSubClient's callback is a plain function pointer on ESP32, so the one live
// NetTask has to be reachable from a file-scope trampoline.
NetTask *s_self = nullptr;

constexpr UBaseType_t kQueueDepth = 8;

// Diagnostics lines published per net-task loop (spec 4.14). Four every 10 ms
// drains a burst fast enough to be useful while leaving NetManager::tick() —
// and with it the MQTT keepalive — the rest of the loop.
constexpr int kDiagPerLoop = 4;

// 12 KB (bumped from 8 KB for FlightsService, spec 4.9): the deepest paths
// were the discovery resync (a 255-byte topic buffer plus String/JSON
// scratch, roughly 3 KB with log formatting) and ArduinoOTA's 1460-byte
// read buffer during an update, which left ~4.5 KB headroom on 8 KB. On the
// Dial, FlightsService also runs here: its logo GET is a plain WiFiClient
// with a heap-allocated body buffer (shallower than the WiFiClientSecure/
// HTTPClient path it replaced in Plan 7), and its onStateMessage() JSON
// parse rides on the MQTT receive callback below. Neither overlaps the
// other (net task is single-threaded). Left at 12 KB rather than trimmed
// with the TLS stack: the resync/OTA paths are unchanged and the headroom
// is cheap. The one-shot high-water-mark log after the first resync
// confirms actual headroom on hardware.
constexpr uint32_t kStackBytes = 12288;

constexpr UBaseType_t kPriority = 1;

// Both boards (ESP32 DevKit, ESP32-S3 Dial) are dual-core and Arduino's loop()
// owns core 1, so networking gets core 0.
constexpr BaseType_t kCore = (portNUM_PROCESSORS > 1) ? 0 : tskNO_AFFINITY;

// Length-aware copy: MQTT payloads are not NUL-terminated.
bool payloadToBuf(const uint8_t *payload, unsigned int len, char *buf, size_t bufSize)
{
    if (len >= bufSize)
    {
        return false;
    }
    memcpy(buf, payload, len);
    buf[len] = '\0';
    return true;
}

// Replaces the String::trim() the old handlers relied on.
void trimInPlace(char *s)
{
    char *p = s;
    while (*p != '\0' && isspace((unsigned char)*p))
    {
        p++;
    }
    if (p != s)
    {
        memmove(s, p, strlen(p) + 1);
    }
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1]))
    {
        s[--n] = '\0';
    }
}

void mqttTrampoline(char *topic, uint8_t *payload, unsigned int length)
{
    if (s_self != nullptr)
    {
        s_self->onMqttMessage(topic, payload, length);
    }
}

void taskTrampoline(void *arg)
{
    static_cast<NetTask *>(arg)->run();
}
} // namespace

NetTask::NetTask(NetManager &net, MqttView &view, TreadmillHandler &treadmill)
    : m_net(net), m_view(view), m_treadmill(treadmill)
#if HAS_DIAL_UI
    , m_flights(net)
#endif
{
}

void NetTask::begin(const char *clientId)
{
    if (clientId != nullptr)
    {
        strncpy(m_clientId, clientId, sizeof(m_clientId) - 1);
    }

    // Recovery topics — allow restoring NVS totals / calibration after an
    // accidental "Erase Flash". Built once; logged at INFO on every MQTT connect
    // so they can be found in the serial monitor.
    snprintf(m_restoreTotalsTopic, sizeof(m_restoreTotalsTopic), "%s/restore-totals/set", m_clientId);
    snprintf(m_restoreCalibTopic, sizeof(m_restoreCalibTopic), "%s/restore-calibration/set", m_clientId);

    m_cmdQ = xQueueCreate(kQueueDepth, sizeof(Command));
    m_pubQ = xQueueCreate(kQueueDepth, sizeof(PublishItem));
    if (m_cmdQ == nullptr || m_pubQ == nullptr)
    {
        log_e("NetTask: queue allocation failed — networking disabled");
        return;
    }

    s_self = this;
    m_net.setMqttCallback(mqttTrampoline);
    m_net.onMqttConnected([this]() { this->onMqttConnected(); });

#if HAS_DIAL_UI
    // Runs on the caller's task (setup()), same as NetManager::begin() being
    // deferred into run() below rather than called here — a one-time boot
    // mount, not a per-loop cost, so it doesn't need the net task.
    m_flights.begin();
#endif

    if (xTaskCreatePinnedToCore(taskTrampoline, "net", kStackBytes, this, kPriority, &m_task, kCore) != pdPASS)
    {
        log_e("NetTask: task creation failed — networking disabled");
    }
}

void NetTask::run()
{
    // Non-blocking: WiFi/MQTT come up in the background, with backoff, forever.
    // Deliberately on this task so setup() never waits on an association.
    m_net.begin(m_clientId);

    for (;;)
    {
        m_net.tick(millis());
        m_status.store(m_net.status(), std::memory_order_relaxed);
        drainPublishQueue();
        drainDiagQueue();
#if HAS_DIAL_UI
        m_flights.tick(millis());
#endif
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

bool NetTask::enqueuePublish(const PublishItem &item)
{
    if (m_pubQ == nullptr)
    {
        return false;
    }
    // A dropped snapshot is superseded by the next one, and a dropped light
    // command is retryable by hand (tap or turn again) — neither is worth
    // blocking the caller's task for. A dropped setting echo would leave HA
    // showing a stale value, so those get a short wait.
    const TickType_t wait =
        (item.type == PubType::SNAPSHOT || item.type == PubType::LIGHT_CMD) ? 0 : pdMS_TO_TICKS(50);
    if (xQueueSend(m_pubQ, &item, wait) != pdTRUE)
    {
        log_w("Publish queue full — dropped item type %u", (unsigned)item.type);
        return false;
    }
    return true;
}

bool NetTask::receiveCommand(Command &out)
{
    return m_cmdQ != nullptr && xQueueReceive(m_cmdQ, &out, 0) == pdTRUE;
}

bool NetTask::enqueueCommand(const Command &cmd)
{
    if (m_cmdQ == nullptr)
    {
        return false;
    }
    // Zero wait: this runs inside PubSubClient::loop(), which must not stall.
    if (xQueueSend(m_cmdQ, &cmd, 0) != pdTRUE)
    {
        log_w("Command queue full — dropped command type %u", (unsigned)cmd.type);
        return false;
    }
    return true;
}

void NetTask::drainPublishQueue()
{
    if (m_pubQ == nullptr)
    {
        return;
    }
    PublishItem item;
    while (xQueueReceive(m_pubQ, &item, 0) == pdTRUE)
    {
        // Silently drop while MQTT is down — publishing would fail (and log an
        // error) for every queued item during an outage. The reconnect handler
        // runs a full resync, so nothing stays stale.
        if (!m_net.mqttUp())
        {
            continue;
        }

        switch (item.type)
        {
        case PubType::SNAPSHOT:
            m_view.publishState(item.snap);
            break;
        case PubType::AUTO_RECONNECT:
            m_view.publishAutoReconnectSetting(item.b);
            break;
        case PubType::IDLE_MINS:
            m_view.publishIdleDisconnectSetting(item.u16);
            break;
        case PubType::PAUSE_MINS:
            m_view.publishPauseTimeoutSetting(item.u16);
            break;
        case PubType::START_SPEED:
            m_view.publishStartSpeedSetting(item.f);
            break;
        case PubType::FLIGHTS_AUTO_SHOW:
            m_view.publishFlightsAutoShowSetting(item.b);
            break;
        case PubType::CALIB_COUNT:
            m_view.publishCalibrationPoints(item.u8);
            break;
        case PubType::FULL_RESYNC:
            fullResync();
            break;
        case PubType::LIGHT_CMD:
#if HAS_DIAL_UI
        {
            char topic[48];
            LightsService::setTopic(static_cast<LightsModel::LightKey>(item.lightKey), topic, sizeof(topic));
            m_net.mqtt().publish(topic, item.lightJson); // not retained — this is a command, not state
            log_i("NetTask: LIGHT_CMD publish [%s] %s", topic, item.lightJson);
        }
#endif
            break;
        }
    }
}

void NetTask::drainDiagQueue()
{
    // Lines queued while MQTT was down stay queued (RemoteLog's queue is the
    // only buffer), so the boot-time BLE story survives until the link is up.
    char line[RemoteLog::kLineLen];
    for (int i = 0; i < kDiagPerLoop; i++)
    {
        if (!m_net.mqttUp() || !RemoteLog::pop(line, sizeof(line)))
        {
            return;
        }
        // QoS 0, not retained: a diagnostics stream, not state. Deliberately
        // not logged on failure — a log line here would feed itself back into
        // the queue it just failed to drain.
        m_net.mqtt().publish(RemoteLog::kTopic, line);
    }
}

void NetTask::publishBootRecord()
{
    // The pre-reset log tail is a one-shot: fetch it before the boot record so
    // the record can carry the count, and only on the first MQTT connect of
    // this boot — later reconnects would otherwise replay a crash that is by
    // then minutes old.
    // static, not a local: 960 B is a tenth of the net task's stack and this
    // function runs during the MQTT connect, next to TLS's own appetite. Net
    // task only (onMqttConnected), never reentered, so no guard is needed.
    static char last[RemoteLog::kLastLines][RemoteLog::kLineLen];
    uint8_t lastCount = 0;
    const bool first  = !m_bootPublished;
    if (first && RemoteLog::resetWasCrash())
    {
        lastCount = RemoteLog::lastLines(last, RemoteLog::kLastLines);
    }

    // Retained (spec 4.14): whoever subscribes later still learns whether the
    // last restart was an OTA flash (SW) or a crash (PANIC/TASK_WDT/BROWNOUT),
    // and which build is running. "last_lines" says how many diag/last lines
    // accompanied this record — 0 on every reconnect.
    char json[192];
    snprintf(json, sizeof(json),
             "{\"reset\":\"%s\",\"build\":\"%s\",\"uptime_s\":%lu,\"heap\":%u,\"last_lines\":%u}",
             RemoteLog::resetReasonName(), RemoteLog::buildStamp(),
             (unsigned long)(millis() / 1000), (unsigned)ESP.getFreeHeap(), (unsigned)lastCount);
    m_net.mqtt().publish(RemoteLog::kBootTopic, json, true);

    // Oldest first, numbered back from the reset: [last-8] … [last-1], so the
    // last line the firmware managed to log is the one labelled [last-1].
    // QoS 0 and not retained — this is forensics, not state.
    for (uint8_t i = 0; i < lastCount; i++)
    {
        char payload[RemoteLog::kLineLen + 16];
        snprintf(payload, sizeof(payload), "[last-%u] %s", (unsigned)(lastCount - i), last[i]);
        m_net.mqtt().publish(RemoteLog::kLastTopic, payload);
    }

    if (first)
    {
        m_bootPublished = true;
        // Every case, POWERON and SW included: an OTA reboot is not a crash,
        // and the next one should report its own tail, not this boot's.
        RemoteLog::clearLastLines();
    }
}

void NetTask::onMqttConnected()
{
    PubSubClient &client = m_net.mqtt();

    // First, before the subscribe/resync burst: the cheapest message we have
    // and the one that says whether this connect follows a crash.
    publishBootRecord();

    client.subscribe(m_view.getSpeed().getCommandTopic(), 1);
    client.subscribe(m_view.getStartButton().getCommandTopic(), 1);
    client.subscribe(m_view.getPauseButton().getCommandTopic(), 1);
    client.subscribe(m_view.getStopButton().getCommandTopic(), 1);
    client.subscribe(m_view.getAutoReconnectSwitch().getCommandTopic(), 1);
    client.subscribe(m_view.getConnectSwitch().getCommandTopic(), 1);
    client.subscribe(m_view.getCalibrate20StepsButton().getCommandTopic(), 1);
    client.subscribe(m_view.getIdleDisconnectNumber().getCommandTopic(), 1);
    client.subscribe(m_view.getPauseTimeoutNumber().getCommandTopic(), 1);
    client.subscribe(m_view.getStartSpeedNumber().getCommandTopic(), 1);
    client.subscribe(m_view.getFlightsAutoShowSwitch().getCommandTopic(), 1);

    client.subscribe(m_restoreTotalsTopic);
    log_i("Restore-totals topic: %s", m_restoreTotalsTopic);
    client.subscribe(m_restoreCalibTopic);
    log_i("Restore-calibration topic: %s", m_restoreCalibTopic);

    client.subscribe(HOMEASSISTANT_STATUS_TOPIC);
    client.subscribe(HOMEASSISTANT_STATUS_TOPIC_ALT);

#if HAS_DIAL_UI
    // Lights card (Plan 6): subscribe both lights' retained state topics,
    // then ask HA to re-publish them (they're retained, so a fresh
    // subscribe alone would normally be enough — the explicit refresh
    // covers a broker that doesn't have a retained message yet, e.g. right
    // after the HA automation itself restarts). Grouped here with the other
    // subscribes, ahead of fullResync()'s treadmill-config publish/delay,
    // so the refresh trigger goes out as early as possible on reconnect.
    {
        char topic[48];
        LightsService::stateTopic(LightsModel::LightKey::OFFICE, topic, sizeof(topic));
        client.subscribe(topic, 0);
        LightsService::stateTopic(LightsModel::LightKey::LAMP, topic, sizeof(topic));
        client.subscribe(topic, 0);
        client.publish(LightsService::kRefreshTopic, "1");
    }

    // Flights card (spec 4.11): same shape as the lights above — subscribe
    // HA's retained aircraft topic, then ask the HA-side automation to
    // re-publish it so the card fills in immediately rather than waiting
    // for HA's next scheduled update.
    client.subscribe(FlightsService::kStateTopic, 0);
    client.publish(FlightsService::kRefreshTopic, "1");
#endif

    fullResync();
}

void NetTask::fullResync()
{
    char configUrl[256];
    snprintf(configUrl, sizeof(configUrl), "http://%s/", WiFi.localIP().toString().c_str());
    m_view.getDevice().setConfigurationUrl(configUrl);

    m_view.publishAllConfigs();
    delay(200); // give the broker time to process all config messages
    m_view.publishState(m_treadmill.getLastData());
    m_view.publishAutoReconnectSetting(m_treadmill.getAutoReconnect());
    m_view.publishIdleDisconnectSetting(m_treadmill.getIdleDisconnectMins());
    m_view.publishPauseTimeoutSetting(m_treadmill.getPauseTimeoutMins());
    m_view.publishStartSpeedSetting(m_treadmill.getStartSpeedMph());
    m_view.publishFlightsAutoShowSetting(m_treadmill.getFlightsAutoShow());
    m_view.publishCalibrationPoints(m_treadmill.getCalibrationPointCount());

    if (!m_stackLogged)
    {
        m_stackLogged = true;
        // Deepest publish path we have; whatever is left here is the real headroom.
        log_i("Net task stack high-water mark: %u bytes free",
              (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    }
}

void NetTask::onMqttMessage(char *topic, uint8_t *payload, unsigned int length)
{
    log_i("Message arrived [%s]", topic);
    for (unsigned int i = 0; i < length; i++)
    {
        Serial.print((char)payload[i]);
    }
    Serial.println();

#if HAS_DIAL_UI
    // Only the yes/no question here — onStateMessage() derives the key it
    // needs itself, so there's no second copy of it to keep in step.
    if (LightsService::isStateTopic(topic))
    {
        m_lights.onStateMessage(topic, payload, length);
        return;
    }

    // Flights card (spec 4.11): HA's retained aircraft list. Parsed inline
    // on this task — FlightsService::onStateMessage() is a bounded JSON
    // parse into a Guarded snapshot, no network and no queue hop needed.
    if (strcmp(topic, FlightsService::kStateTopic) == 0)
    {
        m_flights.onStateMessage(payload, length, millis());
        return;
    }
#endif

    Command cmd;
    char buf[32];

    if (strcmp(topic, m_view.getSpeed().getCommandTopic()) == 0)
    {
        if (!payloadToBuf(payload, length, buf, sizeof(buf)))
        {
            log_w("speed: payload too long (%u bytes)", length);
            return;
        }
        trimInPlace(buf);
        cmd.type = CmdType::SET_SPEED_MPH;
        cmd.f    = strtof(buf, nullptr);
        log_i("Setting speed to %.2f mph", cmd.f);
        enqueueCommand(cmd);
        return;
    }

    if (strcmp(topic, m_view.getStartButton().getCommandTopic()) == 0 ||
        strcmp(topic, m_view.getPauseButton().getCommandTopic()) == 0 ||
        strcmp(topic, m_view.getStopButton().getCommandTopic()) == 0 ||
        strcmp(topic, m_view.getCalibrate20StepsButton().getCommandTopic()) == 0)
    {
        if (!payloadToBuf(payload, length, buf, sizeof(buf)))
        {
            return;
        }
        trimInPlace(buf);
        if (strcasecmp(buf, "press") != 0)
        {
            return;
        }
        if (strcmp(topic, m_view.getStartButton().getCommandTopic()) == 0)
        {
            log_i("Start command received");
            cmd.type = CmdType::START;
        }
        else if (strcmp(topic, m_view.getPauseButton().getCommandTopic()) == 0)
        {
            log_i("Pause command received");
            cmd.type = CmdType::PAUSE;
        }
        else if (strcmp(topic, m_view.getStopButton().getCommandTopic()) == 0)
        {
            log_i("Stop command received");
            cmd.type = CmdType::STOP;
        }
        else
        {
            log_i("Calibration 20 Steps button pressed via MQTT");
            cmd.type = CmdType::TOGGLE_CALIBRATION;
        }
        enqueueCommand(cmd);
        return;
    }

    if (strcmp(topic, m_view.getAutoReconnectSwitch().getCommandTopic()) == 0)
    {
        if (!payloadToBuf(payload, length, buf, sizeof(buf)))
        {
            return;
        }
        trimInPlace(buf);
        log_i("Auto Reconnect command received: %s", buf);
        const MqttSwitch &sw = m_view.getAutoReconnectSwitch();
        if (strcasecmp(buf, sw.getOnState()) == 0)
        {
            cmd.b = true;
        }
        else if (strcasecmp(buf, sw.getOffState()) == 0)
        {
            cmd.b = false;
        }
        else
        {
            return;
        }
        cmd.type = CmdType::SET_AUTO_RECONNECT;
        enqueueCommand(cmd);
        return;
    }

    if (strcmp(topic, m_view.getConnectSwitch().getCommandTopic()) == 0)
    {
        if (!payloadToBuf(payload, length, buf, sizeof(buf)))
        {
            return;
        }
        trimInPlace(buf);
        const MqttSwitch &sw = m_view.getConnectSwitch();
        if (strcasecmp(buf, sw.getOnState()) == 0)
        {
            cmd.type = CmdType::CONNECT;
        }
        else if (strcasecmp(buf, sw.getOffState()) == 0)
        {
            cmd.type = CmdType::DISCONNECT;
        }
        else
        {
            return;
        }
        enqueueCommand(cmd);
        return;
    }

    if (strcmp(topic, m_view.getIdleDisconnectNumber().getCommandTopic()) == 0 ||
        strcmp(topic, m_view.getPauseTimeoutNumber().getCommandTopic()) == 0)
    {
        if (!payloadToBuf(payload, length, buf, sizeof(buf)))
        {
            return;
        }
        trimInPlace(buf);
        cmd.u16 = (uint16_t)strtoul(buf, nullptr, 10);
        if (strcmp(topic, m_view.getIdleDisconnectNumber().getCommandTopic()) == 0)
        {
            log_i("Idle disconnect setting received: %u min", cmd.u16);
            cmd.type = CmdType::SET_IDLE_MINS;
        }
        else
        {
            log_i("Pause timeout setting received: %u min", cmd.u16);
            cmd.type = CmdType::SET_PAUSE_MINS;
        }
        enqueueCommand(cmd);
        return;
    }

    if (strcmp(topic, m_view.getStartSpeedNumber().getCommandTopic()) == 0)
    {
        if (!payloadToBuf(payload, length, buf, sizeof(buf)))
        {
            return;
        }
        trimInPlace(buf);
        cmd.type = CmdType::SET_START_SPEED;
        cmd.f    = strtof(buf, nullptr);
        log_i("Start speed setting received: %.1f mph", cmd.f);
        enqueueCommand(cmd);
        return;
    }

    if (strcmp(topic, m_view.getFlightsAutoShowSwitch().getCommandTopic()) == 0)
    {
        if (!payloadToBuf(payload, length, buf, sizeof(buf)))
        {
            return;
        }
        trimInPlace(buf);
        log_i("Flights auto-show command received: %s", buf);
        const MqttSwitch &sw = m_view.getFlightsAutoShowSwitch();
        if (strcasecmp(buf, sw.getOnState()) == 0)
        {
            cmd.b = true;
        }
        else if (strcasecmp(buf, sw.getOffState()) == 0)
        {
            cmd.b = false;
        }
        else
        {
            return;
        }
        cmd.type = CmdType::SET_FLIGHTS_AUTO_SHOW;
        enqueueCommand(cmd);
        return;
    }

    if (strcmp(topic, m_restoreTotalsTopic) == 0)
    {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, payload, length);
        if (err)
        {
            log_e("restore-totals: JSON parse failed (%s) — payload: %.*s", err.c_str(),
                  (int)length, (const char*)payload);
            return;
        }
        cmd.type = CmdType::RESTORE_TOTALS;
        // Absent or unconvertible fields keep their current NVS value — the loop
        // task fills those in. A present-but-wrong-typed field (e.g. "steps":{})
        // must not be treated as "has" with a value of 0.
        cmd.totals.has[0] = doc["dist_km"].is<float>();
        cmd.totals.has[1] = doc["steps"].is<uint32_t>();
        cmd.totals.has[2] = doc["calories"].is<uint32_t>();
        cmd.totals.has[3] = doc["duration_sec"].is<uint32_t>();
        cmd.totals.distKm      = cmd.totals.has[0] ? doc["dist_km"].as<float>()      : 0.0f;
        cmd.totals.steps       = cmd.totals.has[1] ? doc["steps"].as<uint32_t>()     : 0;
        cmd.totals.calories    = cmd.totals.has[2] ? doc["calories"].as<uint32_t>()  : 0;
        cmd.totals.durationSec = cmd.totals.has[3] ? doc["duration_sec"].as<uint32_t>() : 0;
        log_i("Restore-totals payload accepted");
        enqueueCommand(cmd);
        return;
    }

    if (strcmp(topic, m_restoreCalibTopic) == 0)
    {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, payload, length);
        if (err)
        {
            log_e("restore-calibration: JSON parse failed (%s) — payload: %.*s", err.c_str(),
                  (int)length, (const char*)payload);
            return;
        }
        JsonArray arr = doc.as<JsonArray>();
        if (arr.isNull() || arr.size() == 0 || arr.size() > 10)
        {
            log_e("restore-calibration: expected JSON array of 1-10 points");
            return;
        }
        cmd.type = CmdType::RESTORE_CALIBRATION;
        for (JsonObject obj : arr)
        {
            cmd.calib.pts[cmd.calib.n].speedMph = obj["mph"];
            cmd.calib.pts[cmd.calib.n].spm      = obj["spm"];
            cmd.calib.n++;
        }
        log_i("Restore-calibration payload accepted: %u points", cmd.calib.n);
        enqueueCommand(cmd);
        return;
    }

    if (strcmp(topic, HOMEASSISTANT_STATUS_TOPIC) == 0 ||
        strcmp(topic, HOMEASSISTANT_STATUS_TOPIC_ALT) == 0)
    {
        if (!payloadToBuf(payload, length, buf, sizeof(buf)))
        {
            return;
        }
        trimInPlace(buf);
        if (strcmp(buf, "online") != 0)
        {
            return;
        }
        cmd.type = CmdType::HA_ONLINE;
        enqueueCommand(cmd);
    }
}
