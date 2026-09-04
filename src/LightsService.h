#pragma once

#include <stdint.h>
#include <stddef.h>

#include "board.h"
#include "LightsModel.h"
#include "Guarded.h"

// Holds the Dial's view of the two HA lights (Office, Lamp) driven over MQTT
// (Plan 6). Mirrors FlightsService's threading contract: onStateMessage()
// must only ever be called from the net task (from NetTask::onMqttMessage(),
// see NetTask.h/.cpp), while snapshot() is safe from any task.
//
// Device-only: this header is portable (no Arduino/ESP32 types), but the
// whole .cpp is compiled out (empty translation unit) unless HAS_DIAL_UI, so
// the class only actually does anything on the Dial build. NetTask.h only
// declares/uses the m_lights member under the same guard, so the DevKit
// build never instantiates or calls it.
class LightsService
{
public:
    // Published (a trivial "1" payload) after every MQTT connect so the
    // Home-Assistant-side automation re-publishes both lights' retained
    // state topics.
    static constexpr const char *kRefreshTopic = "pacekeeper-dial/light/refresh";

    // Builds "pacekeeper-dial/light/{key}/state" / ".../set" into buf
    // (capacity cap) via snprintf, where {key} is LightsModel::keyName(key).
    static void stateTopic(LightsModel::LightKey key, char *buf, size_t cap);
    static void setTopic(LightsModel::LightKey key, char *buf, size_t cap);

    // True if `topic` is either light's state topic. Just keyFromTopic()
    // with the matched key discarded, for callers that only need the yes/no
    // answer — NetTask::onMqttMessage() routing an inbound message here
    // rather than deriving (and throwing away) a key of its own.
    static bool isStateTopic(const char *topic);

    // Net-task only: called from NetTask::onMqttMessage() once `topic` is
    // known to be a light's state topic. Matches the key with
    // LightsModel::keyFromTopic(), parses `payload` with
    // LightsModel::parseLightState() and writes that light's slot in
    // m_snapshot. Logs one line at log_i per accepted state (key, on/off,
    // brightness); log_w on a topic that doesn't match either key or a
    // payload that fails to parse.
    void onStateMessage(const char *topic, const uint8_t *payload, size_t len);

    // Any task: returns a copy.
    LightsModel::LightsSnapshot snapshot() const { return m_snapshot.read(); }

private:
    Guarded<LightsModel::LightsSnapshot> m_snapshot;
};
