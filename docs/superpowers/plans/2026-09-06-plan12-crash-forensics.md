# Plan 12: Crash forensics — last log lines across a reset

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Checkbox steps.

**Goal:** spec §4.16. Keep the last eight forwarded log lines in RTC memory, publish them after a non-OTA reset, bracket the blocking NimBLE calls with log lines, and log long `loop()` iterations.

**Verification:** `pio test -e native`, `pio run -e devkit-usb`, `pio run -e dial-usb`; no new `src/` warnings. `pio` at `~/Library/Python/3.9/bin/pio`. Never stage `src/config.h`.

**Constraints:** the RemoteLog hook still allocates nothing and never blocks (the RTC write is a `memcpy` under a short critical section or a spinlock — no mutex, no queue wait); `TreadmillHandler` gains only `log_i` lines (no behaviour change); publishing only on the net task; `RTC_NOINIT_ATTR` data validated by magic + checksum before use.

---

## Task 1: RTC ring, boot publish, brackets, stall detector

**Files:** `src/RemoteLog.h/.cpp`, `src/NetTask.cpp` (`publishBootRecord()`), `src/TreadmillHandler.cpp` (log lines only), `src/main.cpp` (stall detector), `doc/DIAGNOSTICS.md` (new section), `doc/AUDIT_2026-09-03.md` (Phase I item: force a reset — e.g. pull the belt's power mid-connect — and read `diag/last`).

- [ ] `RemoteLog.cpp`: `struct RtcRing { uint32_t magic; uint32_t next; uint32_t count; char lines[8][kLineLen]; uint32_t checksum; }` as `RTC_NOINIT_ATTR`; `magic = 0x50414345` ("PACE"); checksum = simple 32-bit sum over `lines`+`next`+`count`. In the hook, after the filter decides to keep a line (regardless of whether the queue accepted it), copy it into `lines[next]` under `portENTER_CRITICAL`/`portEXIT_CRITICAL` with a static `portMUX_TYPE`, advance `next`/`count`, recompute the checksum. New API: `uint8_t lastLines(char (*out)[kLineLen], uint8_t cap)` (copies oldest-first, validates magic/checksum, returns count) and `void clearLastLines()`; `bool resetWasCrash()` (reason not POWERON/SW).
- [ ] `RemoteLog::begin()`: if the ring's magic/checksum are invalid, initialise it (do NOT clear a valid ring — `NetTask` reads it first).
- [ ] `NetTask::publishBootRecord()`: on the FIRST call after boot only (a `bool m_bootPublished`): if `resetWasCrash()`, fetch the lines, publish each to `pacekeeper-dial/diag/last` as `[last-N] <line>` (N = count..1, oldest first → `[last-8]` … `[last-1]`), QoS 0 not retained; add `"last_lines":N` (0 when none) to the retained boot JSON; then `clearLastLines()` in every case (SW/POWERON included). Later reconnects publish the boot record as today with `"last_lines":0`.
- [ ] `TreadmillHandler::connectToDevice()`: `log_i("connect() begin")` immediately before `m_pClient->connect(...)`; after it returns `log_i("connect() end %s in %lu ms", ok ? "ok" : "fail", elapsed)`; `log_i("discover begin")` before the getService loop and `log_i("discover end")` after it (both outcomes). Nothing else changes.
- [ ] `main.cpp` `loop()`: measure the iteration (`millis()` before/after the body); if `> 2000`, `log_w("loop stall %lu ms", …)`.
- [ ] `doc/DIAGNOSTICS.md`: "After a crash" section: what `diag/last` contains, that it appears once per boot only when the reset was not POWERON/SW, and how to read it (`mosquitto_sub … -t 'pacekeeper-dial/diag/#'` while the Dial reconnects; the retained `boot` record's `reset`/`last_lines` fields).
- [ ] Commit: `RemoteLog: last-lines RTC ring published after a crash; connect()/discovery brackets; loop stall log`.
