#pragma once
#include "TreadmillData.h"
#ifdef NATIVE_TEST
class SnapshotStore {
public:
    void write(const TreadMillData& d) { m_data = d; }
    TreadMillData read() const { return m_data; }
    template<typename F> void modify(F f) { f(m_data); }
private: TreadMillData m_data;
};
#else
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
class SnapshotStore {
public:
    SnapshotStore() { m_mtx = xSemaphoreCreateMutex(); }
    void write(const TreadMillData& d) { lock(); m_data = d; unlock(); }
    TreadMillData read() const { lock(); TreadMillData c = m_data; unlock(); return c; }
    template<typename F> void modify(F f) { lock(); f(m_data); unlock(); }
private:
    void lock() const { xSemaphoreTake(m_mtx, portMAX_DELAY); }
    void unlock() const { xSemaphoreGive(m_mtx); }
    mutable SemaphoreHandle_t m_mtx; TreadMillData m_data;
};
#endif
