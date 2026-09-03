#include "mqttview.h"
#include <ArduinoJson.h>

MqttView::MqttView(PubSubClient *client)
    : m_client(client),
      m_device("pacekeeper-bridge", "PaceKeeper", SYSTEM_NAME, "maker_pt"),
      m_speed(&m_device, "speed", "Speed"),
      m_speedFeedback(&m_device, "speed-feedback", "Speed Feedback"),
      m_state(&m_device, "state", "State"),
      m_distance(&m_device, "distance", "Distance"),
      m_duration(&m_device, "duration", "Duration"),
      m_calories(&m_device, "calories", "Calories"),
      m_steps(&m_device, "steps", "Steps"),
      m_startBtn(&m_device, "start", "Start"),
      m_pauseBtn(&m_device, "pause", "Pause"),
      m_stopBtn(&m_device, "stop", "Stop"),
      m_connectSwitch(&m_device, "connect", "Connect BLE"),
      m_calibrate20StepsBtn(&m_device, "calibrate-20-steps", "Calibrate 20 Steps"),

      // Configuration Settings
      m_autoreconnectSwitch(&m_device, "auto-reconnect", "Auto Reconnect"),
      m_idleDisconnectMins(&m_device, "idle-disconnect-mins", "Idle Disconnect Minutes"),
      m_pauseTimeoutMins(&m_device, "pause-timeout-mins", "Pause Timeout Minutes"),
      // Cumulative totals
      m_totalDistance(&m_device, "total-distance", "Total Distance"),
      m_totalSteps(&m_device, "total-steps", "Total Steps"),
      m_totalCalories(&m_device, "total-calories", "Total Calories"),
      m_totalDuration(&m_device, "total-duration", "Total Duration"),
      // Session summary (last completed session)
      m_sessionDistance(&m_device, "session-distance", "Session Distance"),
      m_sessionSteps(&m_device, "session-steps", "Session Steps"),
      m_sessionCalories(&m_device, "session-calories", "Session Calories"),
      m_sessionDuration(&m_device, "session-duration", "Session Duration"),
      // Diagnostics Elements
      m_maxSpeed(&m_device, "max-speed", "Max Speed"),
      m_firmware(&m_device, "firmware", "Firmware Version"),
      m_calibrationPoints(&m_device, "calibration-points", "Saved Calibration Points")

{

    m_device.setSWVersion(VERSION);

    // further mqtt device config
    m_speed.setCustomStateTopic(m_state.getStateTopic());
    m_speed.setUnit("mph");
    m_speed.setDeviceClass("speed");
    m_speed.setMin(0.0f);  // Must be 0.0 — HA ignores state updates where value < min,
                           // so min=0.6 caused the slider to be permanently unavailable
                           // when the belt stopped (speed_cmd publishes as 0.0).
                           // The command handler already treats speed<=100 raw as stop.
    m_speed.setMax(3.8f);
    m_speed.setStep(0.1f);
    m_speed.setMode(NumberMode::BOX);
    m_speed.setValueTemplate("{{ value_json.speed_cmd }}");

    m_speedFeedback.setCustomStateTopic(m_state.getStateTopic());
    m_speedFeedback.setUnit("mph");
    m_speedFeedback.setDeviceClass("speed");
    m_speedFeedback.setStateClass(MqttSensor::StateClass::MEASUREMENT);
    m_speedFeedback.setValueTemplate("{{ value_json.speed_feedback }}");

    m_distance.setCustomStateTopic(m_state.getStateTopic());
    m_distance.setUnit("km");
    m_distance.setDeviceClass("distance");
    m_distance.setStateClass(MqttSensor::StateClass::MEASUREMENT);
    m_distance.setValueTemplate("{{ value_json.distance_km }}");

    m_duration.setCustomStateTopic(m_state.getStateTopic());
    m_duration.setUnit("s");
    m_duration.setDeviceClass("duration");
    m_duration.setStateClass(MqttSensor::StateClass::MEASUREMENT);
    m_duration.setValueTemplate("{{ value_json.duration_sec }}");

    m_calories.setCustomStateTopic(m_state.getStateTopic());
    m_calories.setUnit("cal");
    m_calories.setDeviceClass("energy");
    m_calories.setStateClass(MqttSensor::StateClass::MEASUREMENT);
    m_calories.setValueTemplate("{{ value_json.calories }}");

    m_steps.setCustomStateTopic(m_state.getStateTopic());
    m_steps.setUnit("steps");
    m_steps.setStateClass(MqttSensor::StateClass::MEASUREMENT);
    m_steps.setIcon("mdi:shoe-print");
    m_steps.setValueTemplate("{{ value_json.steps }}");

    m_state.setValueTemplate("{{ value_json.state }}");
    m_state.setIcon("mdi:state-machine");

    m_autoreconnectSwitch.setEntityType(EntityCategory::CONFIG);
    m_autoreconnectSwitch.setIcon("mdi:autorenew");

    m_idleDisconnectMins.setEntityType(EntityCategory::CONFIG);
    m_idleDisconnectMins.setMin(0.0f);
    m_idleDisconnectMins.setMax(300.0f);
    m_idleDisconnectMins.setStep(1.0f);
    m_idleDisconnectMins.setMode(NumberMode::BOX);
    m_idleDisconnectMins.setIcon("mdi:timer-stop-outline");
    m_idleDisconnectMins.setUnit("min");

    m_pauseTimeoutMins.setEntityType(EntityCategory::CONFIG);
    m_pauseTimeoutMins.setMin(0.0f);
    m_pauseTimeoutMins.setMax(60.0f);
    m_pauseTimeoutMins.setStep(1.0f);
    m_pauseTimeoutMins.setMode(NumberMode::BOX);
    m_pauseTimeoutMins.setIcon("mdi:timer-pause-outline");
    m_pauseTimeoutMins.setUnit("min");

    // Cumulative totals — each has its own state topic so they can use retain=true
    // and survive MQTT reconnects without waiting for the next live packet.
    // TOTAL_INCREASING prevents HA from resetting to 0 on ESP32 reboot.
    m_totalDistance.setStateClass(MqttSensor::StateClass::TOTAL_INCREASING);
    m_totalDistance.setUnit("km");
    m_totalDistance.setDeviceClass("distance");
    m_totalDistance.setIcon("mdi:map-marker-distance");

    m_totalSteps.setStateClass(MqttSensor::StateClass::TOTAL_INCREASING);
    m_totalSteps.setUnit("steps");
    m_totalSteps.setIcon("mdi:shoe-print");

    m_totalCalories.setStateClass(MqttSensor::StateClass::TOTAL_INCREASING);
    m_totalCalories.setUnit("cal");
    m_totalCalories.setDeviceClass("energy");
    m_totalCalories.setIcon("mdi:fire");

    m_totalDuration.setStateClass(MqttSensor::StateClass::TOTAL_INCREASING);
    m_totalDuration.setUnit("s");
    m_totalDuration.setDeviceClass("duration");
    m_totalDuration.setIcon("mdi:timer-outline");

    // Session summary sensors — retain last completed session values for Strava etc.
    m_sessionDistance.setStateClass(MqttSensor::StateClass::MEASUREMENT);
    m_sessionDistance.setUnit("km");
    m_sessionDistance.setDeviceClass("distance");
    m_sessionDistance.setIcon("mdi:map-marker-distance");

    m_sessionSteps.setStateClass(MqttSensor::StateClass::MEASUREMENT);
    m_sessionSteps.setUnit("steps");
    m_sessionSteps.setIcon("mdi:shoe-print");

    m_sessionCalories.setStateClass(MqttSensor::StateClass::MEASUREMENT);
    m_sessionCalories.setUnit("cal");
    m_sessionCalories.setDeviceClass("energy");
    m_sessionCalories.setIcon("mdi:fire");

    m_sessionDuration.setStateClass(MqttSensor::StateClass::MEASUREMENT);
    m_sessionDuration.setUnit("s");
    m_sessionDuration.setDeviceClass("duration");
    m_sessionDuration.setIcon("mdi:timer-outline");

    m_maxSpeed.setCustomStateTopic(m_state.getStateTopic());
    m_maxSpeed.setEntityType(EntityCategory::DIAGNOSTIC);
    m_maxSpeed.setUnit("mph");
    m_maxSpeed.setDeviceClass("speed");
    m_maxSpeed.setValueTemplate("{{ value_json.speed_max }}");

    m_firmware.setCustomStateTopic(m_state.getStateTopic());
    m_firmware.setEntityType(EntityCategory::DIAGNOSTIC);
    m_firmware.setValueTemplate("{{ value_json.fw }}");
    m_firmware.setIcon("mdi:chip");

    m_calibrationPoints.setEntityType(EntityCategory::DIAGNOSTIC);
    m_calibrationPoints.setIcon("mdi:chart-line");

    m_startBtn.setIcon("mdi:play");
    m_pauseBtn.setIcon("mdi:pause");
    m_stopBtn.setIcon("mdi:stop");
    m_connectSwitch.setIcon("mdi:bluetooth-connect");
    m_calibrate20StepsBtn.setIcon("mdi:shoe-print");
}

void MqttView::publishCalibrationPoints(const CalibrationPoint* points, uint8_t count)
{
    // HA state values are limited to 255 characters — the full JSON array for
    // 10 points exceeds this. Publish just the count as the state value instead.
    char buf[4];
    snprintf(buf, sizeof(buf), "%u", count);
    publishMqttState(m_calibrationPoints, buf);
}

void MqttView::publishAllConfigs()
{
    // Block publishState() for the duration of this call.
    // NimBLE callbacks run in a separate RTOS task and call publishState()
    // concurrently. PubSubClient is not thread-safe — interleaving publish()
    // calls from two tasks corrupts its internal state and causes ECONNRESET.
    m_publishingConfigs = true;

    // Controls
    publishConfig(m_startBtn);
    publishConfig(m_pauseBtn);
    publishConfig(m_stopBtn);
    publishConfig(m_connectSwitch);
    publishConfig(m_calibrate20StepsBtn);
    publishConfig(m_speed);

    // Sensors
    publishConfig(m_speedFeedback);
    publishConfig(m_state);
    publishConfig(m_distance);
    publishConfig(m_duration);
    publishConfig(m_calories);

    publishConfig(m_steps);  // estimated from distance / STEP_LENGTH_M

    // Cumulative totals
    publishConfig(m_totalDistance);
    publishConfig(m_totalSteps);
    publishConfig(m_totalCalories);
    publishConfig(m_totalDuration);

    // Session summary
    publishConfig(m_sessionDistance);
    publishConfig(m_sessionSteps);
    publishConfig(m_sessionCalories);
    publishConfig(m_sessionDuration);

    // Configuration
    publishConfig(m_autoreconnectSwitch);
    publishConfig(m_idleDisconnectMins);
    publishConfig(m_pauseTimeoutMins);

    // Diagnostics
    publishConfig(m_maxSpeed);
    publishConfig(m_firmware);
    publishConfig(m_calibrationPoints);

    m_publishingConfigs = false;
}

void MqttView::publishAutoReconnectSetting(bool enabled)
{
    if (enabled)
    {
        publishMqttState(m_autoreconnectSwitch, m_autoreconnectSwitch.getOnState());
    }
    else
    {
        publishMqttState(m_autoreconnectSwitch, m_autoreconnectSwitch.getOffState());
    }
}

void MqttView::publishIdleDisconnectSetting(uint16_t mins)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%u", mins);
    publishMqttState(m_idleDisconnectMins, buf);
}

void MqttView::publishPauseTimeoutSetting(uint16_t mins)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%u", mins);
    publishMqttState(m_pauseTimeoutMins, buf);
}

void MqttView::publishState(TreadMillData data)
{
    if (m_publishingConfigs) return; // avoid concurrent publish with publishAllConfigs()
    JsonDocument state;
    state["speed_cmd"]      = roundf(data.speedCmd      * 10) / 10.0f;
    state["speed_feedback"] = roundf(data.speedFeedback * 10) / 10.0f;
    state["speed_max"]      = roundf(data.speedMax      * 10) / 10.0f;
    state["distance_km"] = data.distanceKm;
    state["duration_sec"] = data.durationSec;
    state["calories"] = data.calories;
    state["steps"] = data.steps;
    state["fw"] = data.fwVersion;

    switch (data.status)
    {
    case TreadMillData::COUNTDOWN:
        state["state"] = "countdown";
        break;
    case TreadMillData::RUNNING:
        state["state"] = "running";
        break;
    case TreadMillData::PAUSED:
        state["state"] = "paused";
        break;
    case TreadMillData::STOPPED:
        state["state"] = "stopped";
        break;
    case TreadMillData::DISCONNECTED:
        state["state"] = "disconnected";
        break;
    default:
        state["state"] = "unknown";
        break;
    }
    String stateStr;
    serializeJson(state, stateStr);
    publishMqttState(m_state, stateStr.c_str());

    // BLE connect switch — ON when connected, OFF when disconnected.
    // Publishing on every state update keeps HA in sync automatically,
    // including after a blocked disconnect attempt.
    publishMqttState(m_connectSwitch,
        data.status == TreadMillData::DISCONNECTED
            ? m_connectSwitch.getOffState()
            : m_connectSwitch.getOnState());

    // Cumulative totals — published individually (not via the shared state JSON)
    // so they retain their last value on MQTT reconnect without a live packet.
    char buf[32];
    snprintf(buf, sizeof(buf), "%.3f", data.totalDistanceKm);
    publishMqttState(m_totalDistance, buf);

    snprintf(buf, sizeof(buf), "%u", data.totalSteps);
    publishMqttState(m_totalSteps, buf);

    snprintf(buf, sizeof(buf), "%u", data.totalCalories);
    publishMqttState(m_totalCalories, buf);

    snprintf(buf, sizeof(buf), "%u", data.totalDurationSec);
    publishMqttState(m_totalDuration, buf);

    // Session summary — only published once on STOPPED transition
    if (data.sessionComplete)
    {
        snprintf(buf, sizeof(buf), "%.3f", data.sessionDistanceKm);
        publishMqttState(m_sessionDistance, buf);

        snprintf(buf, sizeof(buf), "%u", data.sessionSteps);
        publishMqttState(m_sessionSteps, buf);

        snprintf(buf, sizeof(buf), "%u", data.sessionCalories);
        publishMqttState(m_sessionCalories, buf);

        snprintf(buf, sizeof(buf), "%u", data.sessionDurationSec);
        publishMqttState(m_sessionDuration, buf);
    }
}

void MqttView::publishConfig(MqttEntity &entity)
{
    String payload = entity.getHomeAssistantConfigPayload();
    char topic[255];
    // retain=true is required for HA MQTT discovery — without it, config messages
    // are lost on any MQTT reconnect and HA won't rediscover entities until the
    // next HA "homeassistant/status: online" birth message triggers a re-publish.
    entity.getHomeAssistantConfigTopic(topic, sizeof(topic));
    if (!m_client->publish(topic, payload.c_str(), true))
    {
        log_e("Failed to publish config (%d bytes) to %s", payload.length(), topic);
    }
    entity.getHomeAssistantConfigTopicAlt(topic, sizeof(topic));
    if (!m_client->publish(topic, payload.c_str(), true))
    {
        log_e("Failed to publish config (%d bytes) to %s", payload.length(), topic);
    }
}

void MqttView::publishMqttState(const MqttEntity &entity, const char *state)
{
    // retain=true: broker always holds the latest state so HA can initialise
    // entities immediately on reload/reconnect without waiting for the next
    // live publish. Without this, number entities start as "unknown" and
    // won't accept input until a live message happens to arrive.
    if (!m_client->publish(entity.getStateTopic(), state, true))
    {
        log_e("Failed to publish state to %s", entity.getStateTopic());
    }
}
