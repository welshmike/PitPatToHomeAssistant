#pragma once

#include <Arduino.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "Commands.h"
#include "NetStatus.h"
#include "TreadmillController.h" // ISnapshotObserver, ITreadmillLink
#include "board.h"

#if HAS_DIAL_UI
#include "FlightsService.h"
#include "LightsService.h"
#endif

class NetManager;
class MqttView;
class TreadmillHandler;

// Everything that can block on a socket lives here.
//
// PubSubClient::connect() spins for up to 3 s on the TCP handshake plus 5 s on
// CONNACK, and WiFiClient::write() retries select() 10 x 1 s once the send buffer
// saturates after AP loss — both far longer than the belt's 6 s BLE supervision
// timeout. So WiFi, MQTT and OTA all run on this task and the loop task talks to
// them only through two queues.
//
// Runs: NetManager::begin/tick (WiFi + MQTT state machine + ArduinoOTA), the MQTT
// receive callback (parse + enqueue only), and every MqttView publish.
class NetTask
{
public:
    NetTask(NetManager &net, MqttView &view, TreadmillHandler &treadmill);

    // Creates both queues and starts the task. The task itself calls
    // NetManager::begin(), so no WiFi call ever happens on the loop task.
    // `clientId` is copied.
    void begin(const char *clientId);

    // Last state the net task saw. Single-byte load, safe from the loop task.
    NetStatus status() const { return m_status.load(std::memory_order_relaxed); }

    // loop task -> net task. SNAPSHOT items never wait (the next snapshot
    // supersedes a dropped one); settings items wait up to 50 ms so a one-shot
    // setting echo is not lost. Returns false if the item was dropped.
    bool enqueuePublish(const PublishItem &item);

    // loop task: pull one queued command, false when the queue is empty.
    bool receiveCommand(Command &out);

    // Net-task entry points. Public only so the C-style trampolines can reach
    // them; never call these from the loop task.
    void run();
    void onMqttConnected();
    void onMqttMessage(char *topic, uint8_t *payload, unsigned int length);

#if HAS_DIAL_UI
    // Dial only (spec 4.9/4.11). FlightsService::begin()/tick()/
    // onStateMessage() only ever run here (see NetTask::begin()/run()/
    // onMqttMessage()); the loop task (DialUi) talks to it only through the
    // loop-task-safe methods documented on the class itself.
    FlightsService &flights() { return m_flights; }

    // Dial only (Plan 6). LightsService::onStateMessage() only ever runs
    // here (see NetTask::onMqttMessage()); the loop task talks to it only
    // through the loop-task-safe snapshot().
    LightsService &lights() { return m_lights; }
#endif

private:
    void drainPublishQueue();
    void fullResync();
    bool enqueueCommand(const Command &cmd);

    NetManager       &m_net;
    MqttView         &m_view;
    TreadmillHandler &m_treadmill;

    QueueHandle_t m_cmdQ  = nullptr; // net -> loop, depth 8
    QueueHandle_t m_pubQ  = nullptr; // loop -> net, depth 8
    TaskHandle_t  m_task  = nullptr;

    std::atomic<NetStatus> m_status{NetStatus::WIFI_DOWN};

    char m_clientId[48]          = {0};
    char m_restoreTotalsTopic[64] = {0};
    char m_restoreCalibTopic[64]  = {0};

    bool m_stackLogged = false;

#if HAS_DIAL_UI
    FlightsService m_flights;
    LightsService  m_lights;
#endif
};

// The controller's bridge to the net task: turns observer notifications into
// publish-queue items so MqttView is only ever touched by the net task.
class PublishQueueObserver : public ISnapshotObserver
{
public:
    PublishQueueObserver(NetTask &net, ITreadmillLink &link) : m_net(net), m_link(link) {}

    void onSnapshot(const TreadMillData &d) override
    {
        PublishItem item;
        item.type = PubType::SNAPSHOT;
        item.snap = d;
        m_net.enqueuePublish(item);
    }

    // Nothing to do here at all: setSpeedMph() inside the controller's settle
    // already calls onSnapshot() synchronously once the target lands, which
    // enqueues the SNAPSHOT publish above. Publishing again here on
    // pending==false was a duplicate.
    void onTargetSpeed(float mph, bool pending) override
    {
        (void)mph;
        (void)pending;
    }

private:
    NetTask        &m_net;
    ITreadmillLink &m_link;
};
