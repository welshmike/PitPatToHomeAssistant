# Plan 10: Remote diagnostics log over MQTT

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Checkbox steps.

**Goal:** spec §4.14. Forward the Dial's warning/error log lines (and BLE-related info lines) to `pacekeeper-dial/diag` over MQTT, plus a retained boot record, so BLE kick cycles can be diagnosed with no USB.

**Verification:** `pio test -e native`, `pio run -e devkit-usb`, `pio run -e dial-usb`; no new `src/` warnings. `pio` at `~/Library/Python/3.9/bin/pio`. Never stage `src/config.h`.

**Constraints:** `TreadmillHandler` untouched; the vprintf hook allocates nothing, blocks on nothing, and always chains to the previous vprintf; loop task never touches sockets; the DevKit build must still compile (RemoteLog is device-only but not Dial-only — both boards may use it; guard with `#ifdef ARDUINO`).

---

## Task 1: `RemoteLog` + NetTask publish + boot record

**Files:** new `src/RemoteLog.h/.cpp`, `src/NetTask.h/.cpp`, `src/main.cpp`, `platformio.ini` (`[common] build_flags` += `-DUSE_ESP_IDF_LOG`; confirm `CORE_DEBUG_LEVEL=3` stays), new `doc/DIAGNOSTICS.md`, README pointer.

- [ ] `RemoteLog` (static class or namespace): `begin()` installs the hook and remembers the previous vprintf; `bool pop(char* out, size_t cap)` for the net task; internal `QueueHandle_t` of `Line { char text[120]; }` depth 24; `uint16_t dropped` folded into the next line as a `[+N dropped]` suffix. Hook: `int hook(const char* fmt, va_list ap)` → `vsnprintf` into a stack buffer (240 B), forward to the previous vprintf FIRST, then decide via the filter (level = second char of the ESP-IDF line `E (`/`W (`/`I (`; keyword allowlist for I), trim trailing newline, copy into a `Line`, `xQueueSend(..., 0)`. Must be safe from any task/core; no ISR context is expected (esp_log is never called from ISRs here), but use `xQueueSendFromISR` if `xPortInIsrContext()`.
- [ ] `NetTask::run()`: after `drainPublishQueue()`, while `m_net.mqttUp()` and up to 4 times, `RemoteLog::pop()` → `client.publish("pacekeeper-dial/diag", line)`. `onMqttConnected()`: publish retained `pacekeeper-dial/diag/boot` JSON (`esp_reset_reason()` mapped to a short name, `__DATE__ " " __TIME__`, `millis()/1000`, `ESP.getFreeHeap()`), using a stack `char[160]` and `snprintf`.
- [ ] `main.cpp`: `RemoteLog::begin()` as the first statement of `setup()` (before `dialUi.begin()`), and one `log_w("boot: reset=%s", ...)` so the boot reason is also in the stream.
- [ ] Check `-DUSE_ESP_IDF_LOG` compiles everywhere: Arduino's macros then need a `TAG` in each TU (the header defaults it to `"ARDUINO"`); fix any TU that breaks. Confirm on the serial monitor format change (lines become `I (12345) ARDUINO: ...`) is acceptable — update `doc/AUDIT_2026-09-03.md` serial-capture note if it greps on the old format.
- [ ] `doc/DIAGNOSTICS.md`: what is forwarded, the two topics, `mosquitto_sub -h <host> -u … -P … -t 'pacekeeper-dial/diag/#' -v`, and a paho one-liner; note the filter keywords and how to widen them.
- [ ] Commit: `RemoteLog: forward warning/error and BLE log lines over MQTT; retained boot record`.
