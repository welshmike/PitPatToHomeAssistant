#pragma once

#include <Arduino.h>
#include <PubSubClient.h>
#include <MqttDevice.h>
#include "platform.h"
#include "settings.h"
#include "utils.h"
#include <ArduinoJson.h>
#include "TreadmillState.h"

class MqttView
{
public:
    MqttView(PubSubClient *client);

    MqttDevice &getDevice()
    {
        return m_device;
    }

    const MqttNumber &getSpeed() const
    {
        return m_speed;
    }

    const MqttButton &getStartButton() const
    {
        return m_startBtn;
    }

    const MqttButton &getPauseButton() const
    {
        return m_pauseBtn;
    }

    const MqttButton &getStopButton() const
    {
        return m_stopBtn;
    }

    const MqttSwitch &getConnectSwitch() const
    {
        return m_connectSwitch;
    }

    const MqttSwitch &getAutoReconnectSwitch() const
    {
        return m_autoreconnectSwitch;
    }

    const MqttNumber &getStepLengthNumber() const
    {
        return m_stepLengthNum;
    }

    const MqttButton &getCalibrate20StepsButton() const
    {
        return m_calibrate20StepsBtn;
    }

    void publishStepLengthSetting(float stepM);
    void publishCalibrationPoints(const CalibrationPoint* points, uint8_t count);
    void publishAllConfigs();
    void publishAutoReconnectSetting(bool enabled);
    void publishState(TreadMillData data);

private:
    PubSubClient *m_client;
    volatile bool m_publishingConfigs = false; // guard against concurrent NimBLE task publishes

    MqttDevice m_device;

    // Controls
    MqttNumber m_speed;
    MqttButton m_startBtn;
    MqttButton m_pauseBtn;
    MqttButton m_stopBtn;
    MqttSwitch m_connectSwitch;
    MqttButton m_calibrate20StepsBtn;

    // Sensors
    MqttSensor m_calories;
    MqttSensor m_distance;
    MqttSensor m_duration;
    MqttSensor m_speedFeedback;
    MqttSensor m_state;

    MqttSensor m_steps;

    // Cumulative totals
    MqttSensor m_totalDistance;
    MqttSensor m_totalSteps;
    MqttSensor m_totalCalories;
    MqttSensor m_totalDuration;

    // Session summary
    MqttSensor m_sessionDistance;
    MqttSensor m_sessionSteps;
    MqttSensor m_sessionCalories;
    MqttSensor m_sessionDuration;

    // Configuration
    MqttSwitch m_autoreconnectSwitch;
    MqttNumber m_stepLengthNum;

    // Diagnostics
    MqttSensor m_maxSpeed;
    MqttSensor m_firmware;
    MqttSensor m_calibrationPoints;

    void publishConfig(MqttEntity &entity);
    void publishMqttState(const MqttEntity &entity, const char *state);
};