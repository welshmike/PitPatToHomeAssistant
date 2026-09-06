#pragma once

// Remote diagnostics log (spec 4.14).
//
// The Dial lives where USB is impractical — it is flashed over OTA and there is
// no serial monitor attached when the interesting things happen (the recurring
// BLE kick cycle). RemoteLog taps ESP-IDF's log output so those lines can be
// republished over MQTT by the net task.
//
// How the capture works: the build defines -DUSE_ESP_IDF_LOG, so Arduino's
// log_e/log_w/log_i expand to ESP_LOG_LEVEL_LOCAL(..., TAG, ...) and end up in
// esp_log_write() like every other ESP-IDF log line. begin() installs a
// vprintf hook there; the hook keeps calling the previous one first, so serial
// output is byte-for-byte what it was before.
//
// Device-only: the whole file is behind ARDUINO so [env:native] never sees
// FreeRTOS or esp_log (RemoteLog.cpp is deliberately not in that env's
// build_src_filter either).
#ifdef ARDUINO

#include <stddef.h>
#include <stdint.h>

namespace RemoteLog
{
// Longest line kept (including the NUL). Anything longer is truncated — ESP-IDF
// prefixes each line with "E (12345) Tag: ", so this leaves ~100 characters of
// message, enough for every log line we emit.
constexpr size_t kLineLen = 120;

// MQTT topics. Fixed strings rather than clientId-derived: they are read by a
// human with mosquitto_sub, and the DevKit and the Dial are never both live.
constexpr const char *kTopic     = "pacekeeper-dial/diag";
constexpr const char *kBootTopic = "pacekeeper-dial/diag/boot";
constexpr const char *kLastTopic = "pacekeeper-dial/diag/last";

// Crash forensics (spec 4.16): how many forwarded lines the RTC ring keeps.
// 8 x kLineLen = 960 B of RTC slow memory (8 KB on both boards), enough for the
// tail of a connect attempt without crowding out anything else that wants it.
constexpr size_t kLastLines = 8;

// Call as the very first statement of setup(). Creates the queue, raises the
// ESP-IDF runtime log level (Arduino's initArduino() pins every tag at the
// packaged sdkconfig's CONFIG_LOG_DEFAULT_LEVEL, which is ERROR — that would
// silence the W and I lines CORE_DEBUG_LEVEL=3 compiles in) and installs the
// vprintf hook. Safe to call twice; the second call does nothing.
void begin();

// Net task only: the forwarded lines from before the last reset, oldest first.
// Copies at most cap rows into out and returns how many were written (0 when
// the ring did not survive, or after clearLastLines()). The ring is validated
// (magic + checksum) once, in begin(); see the note there.
uint8_t lastLines(char (*out)[kLineLen], uint8_t cap);

// Net task only: drop the saved lines and re-arm the RTC ring for this boot.
// Called after the boot record is published, whether or not anything was
// published, so the next crash reports only its own tail.
void clearLastLines();

// True when esp_reset_reason() is anything other than POWERON or SW, i.e. the
// firmware fell over rather than being power-cycled or restarted (an OTA flash
// looks like SW).
bool resetWasCrash();

// Net task only: take the oldest queued line, false when there is none.
// Copies at most cap-1 characters plus a NUL.
bool pop(char *out, size_t cap);

// Short name for esp_reset_reason(): POWERON, SW, PANIC, INT_WDT, TASK_WDT,
// WDT, DEEPSLEEP, BROWNOUT, SDIO or UNKNOWN. Always a valid string.
const char *resetReasonName();

// __DATE__ " " __TIME__ of RemoteLog.cpp. Note this is that translation unit's
// compile time, so it only moves when RemoteLog.cpp itself is rebuilt (a clean
// build, or a change to build flags); see doc/DIAGNOSTICS.md.
const char *buildStamp();
} // namespace RemoteLog

#endif // ARDUINO
