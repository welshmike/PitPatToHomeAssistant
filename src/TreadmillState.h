#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>
#include <Preferences.h>
#include "platform.h" // For STEP_LENGTH_M

class TreadMillData;

struct CalibrationPoint {
    float speedMph;
    float spm;
};

class TreadmillState
{
public:
    TreadmillState() = default;
    
    void loadFromNVS();
    void saveTotalsToNVS();
    void restoreTotals(float distKm, uint32_t steps, uint32_t calories, uint32_t durationSec);

    void addSession(float distKm, uint32_t steps, uint32_t calories, uint32_t durationSec);
    
    float getDynamicStepLength(float speedMph) const;
    void toggleCalibration(float currentSpeedMph);
    uint8_t getCalibrationPointCount() const { return m_numCalibrationPoints; }
    const CalibrationPoint* getCalibrationPoints() const { return m_calibrationPoints; }
    void restoreCalibrationPoints(const CalibrationPoint* points, uint8_t count);

    float    getTotalDistanceKm()  const { return m_totalDistanceKm; }
    uint32_t getTotalSteps()       const { return m_totalSteps; }
    uint32_t getTotalCalories()    const { return m_totalCalories; }
    uint32_t getTotalDurationSec() const { return m_totalDurationSec; }

private:
    float m_totalDistanceKm = 0.0f;
    uint32_t m_totalSteps = 0;
    uint32_t m_totalCalories = 0;
    uint32_t m_totalDurationSec = 0;

    // Calibration profile
    CalibrationPoint m_calibrationPoints[10];
    uint8_t m_numCalibrationPoints = 0;
    
    bool m_isCalibrating = false;
    uint32_t m_calibrationStartTimeMs = 0;
    float m_calibrationSpeedMph = 0.0f;
    
    void addCalibrationPoint(float speedMph, float spm);
    void saveCalibrationToNVS();
    void loadCalibrationFromNVS();
};
