#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>
#include <Preferences.h>
#include "platform.h" // For STEP_LENGTH_M
#include "TreadmillData.h" // For ITotalsStore, SessionDelta

struct CalibrationPoint {
    float speedMph;
    float spm;
};

class TreadmillState : public ITotalsStore
{
public:
    TreadmillState() = default;
    
    void loadFromNVS();
    void restoreTotals(float distKm, uint32_t steps, uint32_t calories, uint32_t durationSec);

    // ITotalsStore. addSession() only touches memory and raises m_dirty, so it is
    // safe to call from the NimBLE callback; flush() does the flash write and is
    // called from TreadmillHandler::handle() on the main loop.
    void addSession(const SessionDelta& d) override;
    void flush();

    float getDynamicStepLength(float speedMph) const;
    float stepLengthM(float speedMph) const override { return getDynamicStepLength(speedMph); }
    void toggleCalibration(float currentSpeedMph);
    uint8_t getCalibrationPointCount() const { return m_numCalibrationPoints; }
    const CalibrationPoint* getCalibrationPoints() const { return m_calibrationPoints; }
    void restoreCalibrationPoints(const CalibrationPoint* points, uint8_t count);

    float    getTotalDistanceKm()  const { return m_totalDistanceKm; }
    uint32_t getTotalSteps()       const { return m_totalSteps; }
    uint32_t getTotalCalories()    const { return m_totalCalories; }
    uint32_t getTotalDurationSec() const { return m_totalDurationSec; }

    float    totalDistanceKm()  const override { return m_totalDistanceKm; }
    uint32_t totalSteps()       const override { return m_totalSteps; }
    uint32_t totalCalories()    const override { return m_totalCalories; }
    uint32_t totalDurationSec() const override { return m_totalDurationSec; }

private:
    void saveTotalsToNVS();

    // Raised by addSession() (NimBLE task); cleared by flush() (main loop) once the
    // totals reach NVS.
    volatile bool m_dirty = false;

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
