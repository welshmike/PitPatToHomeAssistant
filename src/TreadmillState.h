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
    
    // New dynamic step algorithm methods
    void updateSession(float parsedDistanceM, float speedMph, uint32_t durationSec, uint16_t calories, TreadMillData& data);
    void endSession(TreadMillData& data);
    void resetSession();
    
    float getDynamicStepLength(float speedMph) const;
    void toggleCalibration(float currentSpeedMph);
    uint8_t getCalibrationPointCount() const { return m_numCalibrationPoints; }
    const CalibrationPoint* getCalibrationPoints() const { return m_calibrationPoints; }
    void restoreCalibrationPoints(const CalibrationPoint* points, uint8_t count);

    // Legacy fallback (still useful if 0 cal points)
    void setStepLength(float stepM);
    float getStepLength() const { return m_stepLength; }
    
    float    getTotalDistanceKm()  const { return m_totalDistanceKm; }
    uint32_t getTotalSteps()       const { return m_totalSteps; }
    uint32_t getTotalCalories()    const { return m_totalCalories; }
    uint32_t getTotalDurationSec() const { return m_totalDurationSec; }

private:
    float m_stepLength = STEP_LENGTH_M;
    float m_totalDistanceKm = 0.0f;
    uint32_t m_totalSteps = 0;
    uint32_t m_totalCalories = 0;
    uint32_t m_totalDurationSec = 0;

    // Session iteration variables
    float m_lastDistanceM = 0.0f;
    float m_sessionAccumulatedSteps = 0.0f;
    
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
