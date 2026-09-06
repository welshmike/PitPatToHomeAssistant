# Plan 11: BLE connect scheduling around WiFi bring-up

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Checkbox steps.

**Goal:** spec §4.15. Stop the boot/reconnect-time BLE connect from colliding with the MQTT bring-up burst, be patient with GATT discovery instead of disconnecting (which triggers the Q1 kick phase), and make the whole connect sequence visible over the remote log.

**Verification:** `pio test -e native`, `pio run -e devkit-usb`, `pio run -e dial-usb`; no new `src/` warnings. `pio` at `~/Library/Python/3.9/bin/pio`. Never stage `src/config.h`.

**Constraints:** Q1 protocol handling (`doc/Q1_BLE_NOTES.md`), keepalive-per-notification and every write/notify path in `TreadmillHandler` unchanged — only the connect *scheduling* (a hold flag) and the discovery retry count/spacing change. Loop task never touches sockets. No new heap.

---

## Task 1: Connect hold, discovery patience, log filter

**Files:** `src/TreadmillHandler.h/.cpp`, `src/main.cpp`, `src/RemoteLog.cpp`, `test/test_controller/...` only if `ITreadmillLink` changes (it should not), `doc/AUDIT_2026-09-03.md` (Phase I checklist), spec §4.15 as-built note if anything deviates, README one line.

- [ ] `TreadmillHandler`: `void setConnectHold(bool hold)` + `bool m_connectHold = false;`. In `handle()`, the reconnect `if` gains `&& (!m_connectHold || m_userRequestedConnect)`. Log once per transition at `log_i` (`"BLE connect hold on/off"`) — the transition, not every tick.
- [ ] `main.cpp` (`loop()`, next to the existing `Net status` change detection): track `g_mqttUpSinceMs` (set on transition into `MQTT_UP`, 0 otherwise) and compute `hold = (millis() < 30000 && (status == WIFI_CONNECTING || status == MQTT_CONNECTING)) || (status == MQTT_UP && g_mqttUpSinceMs != 0 && millis() - g_mqttUpSinceMs < kPostMqttHoldMs)` with `kPostMqttHoldMs = 6000`; call `treadmill.setConnectHold(hold)` every loop (cheap; the handler logs only on change).
- [ ] `connectToDevice()`: retry loop `for (int retry = 0; retry < 8; retry++)` with `delay(250)`; log text `retry %d/8`. Nothing else in the function changes.
- [ ] `RemoteLog.cpp` filter: an INFO line is also forwarded when it contains `Treadmill:` (the tag). Keep the keyword list.
- [ ] `doc/AUDIT_2026-09-03.md`: "## Phase I hardware checklist (BLE connect hold)" with the spec's success criteria, read via `pacekeeper-dial/diag`.
- [ ] Commit: `BLE: hold connects during WiFi/MQTT bring-up, 2 s service-discovery patience, full Treadmill log over MQTT`.
