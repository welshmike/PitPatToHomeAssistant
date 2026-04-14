#include "TreadmillState.h"
#include "esp_log.h"

void TreadmillState::loadFromNVS() {
  Preferences prefs;
  prefs.begin("pacekeeper", true); // true = read-only
  m_stepLength = prefs.getFloat("step", STEP_LENGTH_M);
  m_totalDistanceKm = prefs.getFloat("tot_dist", 0.0f);
  m_totalSteps = prefs.getUInt("tot_steps", 0);
  m_totalCalories = prefs.getUInt("tot_cals", 0);
  m_totalDurationSec = prefs.getUInt("tot_dur", 0);
  prefs.end();
  
  loadCalibrationFromNVS();

  log_i("Step length loaded from NVS: %.2f m", m_stepLength);
  log_i("Totals loaded from NVS: dist=%.2f km  steps=%u  cal=%u  dur=%u s",
        m_totalDistanceKm, m_totalSteps, m_totalCalories, m_totalDurationSec);
}

void TreadmillState::saveTotalsToNVS() {
  Preferences prefs;
  prefs.begin("pacekeeper", false); // false = read-write
  prefs.putFloat("tot_dist", m_totalDistanceKm);
  prefs.putUInt("tot_steps", m_totalSteps);
  prefs.putUInt("tot_cals", m_totalCalories);
  prefs.putUInt("tot_dur", m_totalDurationSec);
  prefs.end();

  log_i("Totals flushed to NVS: dist=%.2f km  steps=%u  cal=%u  dur=%u s",
        m_totalDistanceKm, m_totalSteps, m_totalCalories, m_totalDurationSec);
}

void TreadmillState::restoreTotals(float distKm, uint32_t steps,
                                   uint32_t calories, uint32_t durationSec) {
  log_w("RESTORE TOTALS: dist=%.2f km  steps=%u  cal=%u  dur=%u s", distKm,
        steps, calories, durationSec);
  log_w("  (was: dist=%.2f km  steps=%u  cal=%u  dur=%u s)", m_totalDistanceKm,
        m_totalSteps, m_totalCalories, m_totalDurationSec);

  m_totalDistanceKm = distKm;
  m_totalSteps = steps;
  m_totalCalories = calories;
  m_totalDurationSec = durationSec;

  saveTotalsToNVS();
  log_w("Totals restored and saved to NVS.");
}

void TreadmillState::addSession(float distKm, uint32_t steps, uint32_t calories,
                                uint32_t durationSec) {
  m_totalDistanceKm += distKm;
  m_totalSteps += steps;
  m_totalCalories += calories;
  m_totalDurationSec += durationSec;
  saveTotalsToNVS();
}

void TreadmillState::setStepLength(float stepM) {
  m_stepLength = stepM;
  Preferences prefs;
  prefs.begin("pacekeeper", false); // false = read-write
  prefs.putFloat("step", stepM);
  prefs.end();
  log_i("Step length saved to NVS: %.2f m", stepM);
}

void TreadmillState::updateSession(float parsedDistanceM, float speedMph, uint32_t durationSec, uint16_t calories, TreadMillData& data)
{
    float distDelta = parsedDistanceM - m_lastDistanceM;
    if (distDelta < 0) {
        // Treadmill distance wrapped or reset
        distDelta = parsedDistanceM;
    }
    m_lastDistanceM = parsedDistanceM;

    if (distDelta > 0.0f) {
        float stepLen = getDynamicStepLength(speedMph);
        m_sessionAccumulatedSteps += (distDelta / stepLen);
    }

    data.steps = (uint32_t)m_sessionAccumulatedSteps;
    data.distanceKm = parsedDistanceM / 1000.0f;
    data.calories = calories;
    data.durationSec = durationSec;
}

void TreadmillState::endSession(TreadMillData& data)
{
    data.sessionDistanceKm  = data.distanceKm;
    data.sessionSteps       = data.steps;
    data.sessionCalories    = (uint32_t)data.calories;
    data.sessionDurationSec = data.durationSec;
    data.sessionComplete    = true;

    addSession(data.distanceKm, data.steps, (uint32_t)data.calories, data.durationSec);
    
    log_i("Session ended — totals: dist=%.2f km  steps=%u  cal=%u  dur=%u s",
          m_totalDistanceKm, m_totalSteps, m_totalCalories, m_totalDurationSec);
    log_i("Session summary: dist=%.3f km  steps=%u  cal=%u  dur=%u s",
          data.sessionDistanceKm, data.sessionSteps,
          data.sessionCalories, data.sessionDurationSec);
}

void TreadmillState::resetSession()
{
    m_lastDistanceM = 0.0f;
    m_sessionAccumulatedSteps = 0.0f;
    m_isCalibrating = false;
}

float TreadmillState::getDynamicStepLength(float speedMph) const
{
    if (m_numCalibrationPoints == 0) {
        return m_stepLength; // fallback to static
    }
    
    // If only 1 point, use it as a flat baseline
    if (m_numCalibrationPoints == 1) {
        float spm = m_calibrationPoints[0].spm;
        float speedMperMin = speedMph * 26.8224f; // 1 mph = 26.8224 m/min
        return speedMperMin / spm;
    }

    // Find the two points bounding the current speed
    int idxA = 0;
    int idxB = 1;
    for (int i = 0; i < m_numCalibrationPoints - 1; i++) {
        if (speedMph >= m_calibrationPoints[i].speedMph && 
            speedMph <= m_calibrationPoints[i+1].speedMph) {
            idxA = i;
            idxB = i + 1;
            break;
        }
        // If speed is higher than all points, we'll end up extrapolating from the last two
        idxA = i;
        idxB = i + 1;
    }

    float pA_speed = m_calibrationPoints[idxA].speedMph;
    float pA_spm   = m_calibrationPoints[idxA].spm;
    float pB_speed = m_calibrationPoints[idxB].speedMph;
    float pB_spm   = m_calibrationPoints[idxB].spm;

    // Linear interpolation of SPM
    float expectedSpm = pA_spm;
    if (pB_speed != pA_speed) { // avoid div by zero
        float slope = (pB_spm - pA_spm) / (pB_speed - pA_speed);
        expectedSpm = pA_spm + slope * (speedMph - pA_speed);
    }
    
    // Safety bounds (e.g., minimum 40 SPM, maximum 250 SPM)
    if (expectedSpm < 40.0f) expectedSpm = 40.0f;
    if (expectedSpm > 250.0f) expectedSpm = 250.0f;

    // Convert SPM back to step length at this speed
    float speedMperMin = speedMph * 26.8224f; // 1 mph = 26.8224 m/min
    float stepLen = speedMperMin / expectedSpm;
    
    // Safety bounds on step length
    if (stepLen < 0.1f) return 0.1f;
    if (stepLen > 1.5f) return 1.5f;
    
    return stepLen;
}

void TreadmillState::toggleCalibration(float currentSpeedMph)
{
    if (!m_isCalibrating) {
        // Start calibration
        m_isCalibrating = true;
        m_calibrationStartTimeMs = millis();
        m_calibrationSpeedMph = currentSpeedMph;
        log_i("Calibration Started at %.1f mph", currentSpeedMph);
    } else {
        // Complete calibration
        m_isCalibrating = false;
        uint32_t deltaMs = millis() - m_calibrationStartTimeMs;
        if (deltaMs < 5000) {
            log_w("Calibration aborted: Time too short (%u ms). Must be at least 20 steps.", deltaMs);
            return;
        }
        
        // 20 total steps in deltaMs
        float mins = (float)deltaMs / 60000.0f;
        float actualSpm = 20.0f / mins;
        
        log_i("Calibration Completed: 20 steps over %.1f sec = %.1f SPM (Speed: %.1f mph)", 
              deltaMs / 1000.0f, actualSpm, m_calibrationSpeedMph);
              
        addCalibrationPoint(m_calibrationSpeedMph, actualSpm);
    }
}

void TreadmillState::addCalibrationPoint(float speedMph, float spm)
{
    // See if we have an exact match for this speed and replace it
    for (int i = 0; i < m_numCalibrationPoints; i++) {
        // Float comparison with 0.1 delta
        if (abs(m_calibrationPoints[i].speedMph - speedMph) < 0.1f) {
            m_calibrationPoints[i].spm = spm;
            saveCalibrationToNVS();
            return;
        }
    }

    if (m_numCalibrationPoints >= 10) {
        log_w("Calibration curve full! Replacing the oldest/closest point logic not implemented. Skipping save.");
        return;
    }

    // Add and sort the array by speed
    m_calibrationPoints[m_numCalibrationPoints].speedMph = speedMph;
    m_calibrationPoints[m_numCalibrationPoints].spm = spm;
    m_numCalibrationPoints++;

    // Bubble sort is fine for n<=10
    for (int i = 0; i < m_numCalibrationPoints - 1; i++) {
        for (int j = 0; j < m_numCalibrationPoints - i - 1; j++) {
            if (m_calibrationPoints[j].speedMph > m_calibrationPoints[j+1].speedMph) {
                CalibrationPoint temp = m_calibrationPoints[j];
                m_calibrationPoints[j] = m_calibrationPoints[j+1];
                m_calibrationPoints[j+1] = temp;
            }
        }
    }
    saveCalibrationToNVS();
}

void TreadmillState::saveCalibrationToNVS()
{
    Preferences prefs;
    prefs.begin("pacekeeper", false);
    prefs.putBytes("calib_pts", m_calibrationPoints, sizeof(CalibrationPoint) * m_numCalibrationPoints);
    prefs.putUInt("calib_cnt", m_numCalibrationPoints);
    prefs.end();
    log_i("Saved %u calibration points to NVS.", m_numCalibrationPoints);
}

void TreadmillState::restoreCalibrationPoints(const CalibrationPoint* points, uint8_t count)
{
    uint8_t n = count > 10 ? 10 : count;
    m_numCalibrationPoints = n;
    for (uint8_t i = 0; i < n; i++)
        m_calibrationPoints[i] = points[i];
    saveCalibrationToNVS();
    log_i("Restored %u calibration points from MQTT.", n);
}

void TreadmillState::loadCalibrationFromNVS()
{
    Preferences prefs;
    prefs.begin("pacekeeper", true);
    m_numCalibrationPoints = prefs.getUInt("calib_cnt", 0);
    if (m_numCalibrationPoints > 10) m_numCalibrationPoints = 10; // safety
    
    if (m_numCalibrationPoints > 0) {
        prefs.getBytes("calib_pts", m_calibrationPoints, sizeof(CalibrationPoint) * m_numCalibrationPoints);
        log_i("Loaded %u calibration points from NVS.", m_numCalibrationPoints);
        for(int i=0; i<m_numCalibrationPoints; i++) {
            log_i("  Point %d: %.1f mph -> %.1f SPM", i, m_calibrationPoints[i].speedMph, m_calibrationPoints[i].spm);
        }
    } else {
        log_i("No dynamic calibration points found in NVS; using static step length.");
    }
    prefs.end();
}
