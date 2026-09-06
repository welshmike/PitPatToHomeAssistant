# Plan 13: Staged BLE connect

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Checkbox steps.

**Goal:** spec §4.17. Split `connectToDevice()` into a non-blocking LINKING stage (async `connect()`, wait for `onConnect`) and a SETUP stage (the existing discovery/subscribe code), with `cancelConnect()` on a link timeout instead of `disconnect()`.

**Verification:** `pio test -e native`, `pio run -e devkit-usb`, `pio run -e dial-usb`; no new `src/` warnings. `pio` at `~/Library/Python/3.9/bin/pio`. Never stage `src/config.h`. The controller verifies on hardware via `pacekeeper-dial/diag`.

**Constraints:** every write/notify/keepalive/parse path and every existing backoff/kick rule in `TreadmillHandler` stays byte-for-byte; only the connect sequencing changes. NimBLE callbacks (`onConnect`, `onDisconnect`) must not call into NimBLE and must not block (they run on the host task). `ITreadmillLink`/`TreadmillController` untouched (native tests stay 303).

---

## Task 1: LINKING / SETUP stages

**Files:** `src/TreadmillHandler.h/.cpp`, `doc/AUDIT_2026-09-03.md` (Phase J), README (one line), spec as-built note if anything deviates.

- [ ] `TreadmillHandler.h`: `enum class LinkStage : uint8_t { IDLE, LINKING, SETUP }`, `LinkStage m_linkStage = IDLE; uint32_t m_linkStartMs = 0; volatile bool m_linkUp = false;` (set in `onConnect`, cleared when a stage starts and in `onDisconnect`). `static constexpr uint32_t kLinkTimeoutMs = 6000;`
- [ ] `connectToDevice()` splits into `bool beginLink()` (client creation/params as today, `log_i("link begin")`, `connect(addr, false, true)`; on `false` return log + false; else `m_linkStage = LINKING`, `m_linkStartMs = millis()`, return true) and `bool completeSetup()` (from the current getService loop to `Connection successful`, unchanged apart from the retry guard becoming 3 × 100 ms and the brackets `setup begin` / `setup end`).
- [ ] `handle()`: the existing reconnect `if` calls `beginLink()` instead of `connectToDevice()` (attempt counting/logging as today). New block right after it: `if (m_linkStage == LINKING) { if (m_linkUp) { log_i("link up after %lu ms"); m_linkStage = SETUP; if (completeSetup()) { …the current success handling: m_doConnect=false, m_lastKeepalive=millis()… } else { …the current failure handling… } m_linkStage = IDLE; } else if (millis() - m_linkStartMs >= kLinkTimeoutMs) { m_pClient->cancelConnect(); log_w("link timeout after %lu ms, cancelled"); m_linkStage = IDLE; /* m_doConnect stays true; the 5 s m_lastConnectAttempt spacing schedules the next try */ } }`.
- [ ] `onConnect()`: additionally `m_linkUp = true`. `onDisconnect()`: `m_linkUp = false; m_linkStage = IDLE;` at the top (a drop mid-setup must not leave the stage stuck).
- [ ] `requestDisconnect()`/cancel paths that currently call `disconnect()` while connecting: if `m_linkStage == LINKING` use `cancelConnect()` instead.
- [ ] Phase J checklist per spec; README one line under the BLE notes.
- [ ] Commit: `BLE: staged connect — async link, setup after onConnect, cancelConnect on link timeout`.
