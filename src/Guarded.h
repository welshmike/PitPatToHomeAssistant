#pragma once

// Generic mutex-guarded value: the same write/read/modify shape as
// SnapshotStore (see SnapshotStore.h — left untouched on purpose, it
// predates this and TreadmillController depends on its exact type), but
// templated so FlightsService can reuse it for FlightsSnapshot, the
// resident logo buffer and the wanted-logo flag instead of hand-rolling a
// mutex per field.
//
// Every accessor is const. The guard's whole job is safe concurrent access
// regardless of the constness of whatever owns it (the same trick
// SnapshotStore uses for its mutex), so a class can expose const getters
// (e.g. FlightsService::logoReady()) without needing a `mutable Guarded<T>`
// member at each call site — only the data and the mutex are mutable here.
#ifdef NATIVE_TEST
template <typename T>
class Guarded
{
public:
    void write(const T &d) const { m_data = d; }
    T    read() const { return m_data; }
    template <typename F> void modify(F f) const { f(m_data); }

private:
    mutable T m_data{};
};
#else
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

template <typename T>
class Guarded
{
public:
    Guarded() { m_mtx = xSemaphoreCreateMutex(); }

    void write(const T &d) const { lock(); m_data = d; unlock(); }
    T    read() const { lock(); T c = m_data; unlock(); return c; }
    template <typename F> void modify(F f) const { lock(); f(m_data); unlock(); }

private:
    void lock() const { xSemaphoreTake(m_mtx, portMAX_DELAY); }
    void unlock() const { xSemaphoreGive(m_mtx); }

    mutable SemaphoreHandle_t m_mtx;
    mutable T                 m_data{};
};
#endif
