# Q1 Classic Pro BLE Notes

Accumulated knowledge about the Q1 Classic Pro (BA05 BLE protocol) and how pacekeeper handles it.
Updated as discoveries are made.

---

## BA05 Protocol Packet Types

The Q1 sends two distinct packet types over the notify characteristic:

| Type | Length | Meaning |
|------|--------|---------|
| `0x2F` | ~50 bytes | **Stable mode** — normal operation, accurate speed/distance/status |
| `0x34` | ~50 bytes | **Kicking phase** — device is in an unstable post-connect state |
| `0x??` | 20 bytes | Short status packet — only speed feedback is valid |

The packet type is `pData[0]` (first byte of the notification payload).

### 0x34 "Kicking Phase"

After a fresh BLE connection, the Q1 sends `0x34` packets for several cycles before settling into `0x2F`.
During this phase:
- Status byte may read `RUNNING` even when the belt is physically stopped ("phantom RUNNING").
- The device actively disconnects the BLE connection (HCI 0x13, reason=531) approximately every ~10 seconds.
- This repeats for **~8 cycles** before settling — total kicking phase duration ~80–120 seconds.
- This is **hardware behaviour**: it cannot be eliminated, only minimised.

### 0x2F Stable Mode

Once the device settles to `0x2F`, it no longer kicks the connection unprompted.
Arrival of the first `0x2F` packet signals the idle-disconnect timer should be armed (if configured).

---

## HCI Disconnect Reason Codes

NimBLE reports `reason = 512 + HCI_error_code`. The raw HCI codes relevant to Q1:

| NimBLE reason | HCI code | Meaning | Q1 context |
|---------------|----------|---------|------------|
| 520 | 0x08 | Supervision timeout | Radio loss, out of range, or **intentional drop** (keepalives stopped) |
| 531 | 0x13 | Remote user terminated | **Q1 kicked us** — normal in 0x34 kicking phase, also ~3.5h idle kick |
| 534 | 0x16 | Local host terminated | We called `disconnect()` — triggers Q1's deeper kicking phase |
| 574 | 0x3E | Failed to establish | `connect()` timed out — no BLE connection made at all |

### Key insight: 0x16 vs 0x08

Calling `disconnect()` (HCI 0x16) puts Q1 into a **deep kick phase** on the next connect (~8 cycles).
Letting supervision timeout fire (HCI 0x08) causes a **lighter kick phase** (~2–3 cycles).

This is why all intentional disconnects (idle timer, pause timeout) use the supervision timeout approach:
stop keepalives → Q1's supervision timer (6s) fires → reason=520 → lighter reconnect.

---

## Kick Phase Mechanics

```
Connect → 0x34 packets → kicked (reason=531, ~10s) → reconnect → 0x34 again → ...
                                                  (repeats ~8 times)
                                                               ↓
                                                          0x2F stable
```

Each cycle: ~10s of `0x34` packets + `onDisconnect()` + `connectToDevice()` = ~15–20s per cycle.
Total to settle: **~2 minutes** with 5s backoff, **~4 minutes** with 20s backoff.

---

## Backoff Strategy

The reconnect backoff in `onDisconnect()` controls how long we wait between each kick cycle.
Shorter backoff = more beeps but faster settle. Longer = fewer beeps but slower.

| Scenario | Backoff | Rationale |
|----------|---------|-----------|
| Idle kick, user pressed Connect | **5s** | User is waiting at the treadmill — settle in ~2 min |
| Idle kick, auto-reconnect only | **20s** | Background reconnect — minimise beeping, ~4 min settle |
| Mid-session kick (belt was active) | **10s** | Without backoff: instant reconnect → instant kick → infinite loop |

"Belt was active" (`m_sessionActive`) is the ground truth for "belt physically moved".
This flag is immune to phantom RUNNING from `0x34` packets and residual belt odometer from prior sessions.

---

## Intentional Drop via Supervision Timeout

When pacekeeper wants to disconnect cleanly (idle timer, pause timeout), it does **not** call
`m_pClient->disconnect()`. Instead:

```cpp
m_intentionalDrop = true;
m_stopKeepalives  = true;
```

`sendKeepalive()` has an early-return guard:
```cpp
if (m_stopKeepalives) return;
```

Without keepalives, the Q1's supervision timer fires (~6s after the last packet) → `onDisconnect()`
fires with reason=520 → `m_intentionalDrop` cleanup runs → no reconnect.

`onDisconnect()` handles any reason code when `m_intentionalDrop` is set (in case Q1 kicks us
first before the supervision timeout):
```cpp
if (m_intentionalDrop)
{
    if ((reason - 512) == 0x08)
        log_i("Intentional drop: supervision timeout fired as expected ...");
    else
        log_w("Intentional drop: expected 0x08 but got HCI 0x%02X — cleaning up anyway", reason - 512);
    m_intentionalDrop = false;
    m_stopKeepalives  = false;
}
```

---

## Idle-Disconnect Timer

- Armed on **first `0x2F` packet** (device settled to stable idle) if no active session.
- Also armed on **session end** (STOPPED transition with `m_sessionActive`).
- Default: 30 minutes. Configurable via HA number entity (NVS key `"idle"`).
- Disarmed when belt goes RUNNING (session starts).
- On fire: sets `m_intentionalDrop=true`, `m_stopKeepalives=true`, `m_autoReconnect=false`.

Without this timer, the Q1's own ~3.5h idle kick fires while `m_autoReconnect=true`, causing
an infinite reconnect loop until the device is manually disconnected.

---

## Pause Timeout Timer

- Armed on STOPPED transition when `m_isPaused=true`.
- Default: 10 minutes. Configurable via HA (NVS key `"pause"`).
- On fire: commits the paused session, sends stop command, then intentional-drop disconnect.
- Prevents data loss if user walks away and never resumes or explicitly stops.

---

## Zombie Connection Detection

A "zombie" is when `m_pClient->isConnected()` returns `true` but `writeValue()` fails at the
GATT layer (rc=7, `BLE_HS_ENOTCONN`).

This happens when the BLE link layer is still alive (pad still sending LL-level packets, satisfying
supervision timer) but the GATT connection is broken. Without intervention, this can persist for
many minutes until supervision timeout finally fires — the belt stops abruptly and the user is
thrown off at speed.

Fix in `sendCommand()`:
```cpp
if (!m_pClient->isConnected())
{
    // Link layer gone — onDisconnect() will fire
    m_doConnect = true;
}
else
{
    // Zombie: GATT broken, link alive — force HCI disconnect immediately
    log_e("Write failed: GATT layer broken despite isConnected()=true (zombie) — forcing disconnect");
    m_pClient->disconnect();
}
```

`disconnect()` fires `onDisconnect()` (reason=534) within milliseconds, which sets `m_doConnect=true`
(if `m_autoReconnect`), unblocking the reconnect loop immediately.

---

## NimBLE Async Connect Behaviour

`connect(addr, deleteAttributes, asyncConnect)` is called with `asyncConnect=true`:

```cpp
m_pClient->connect(m_targetAddress, false, true)
```

- Returns `true` immediately (before `onConnect()` fires, 500–900ms later).
- `onConnect()` sets `m_sendInitNow = true` (a flag read by `handle()` on Core 1).
- `writeValue(true)` cannot be called from `onConnect()` — it blocks for an ATT response
  processed by the same NimBLE task, causing deadlock.
- `getConnInfo()` called immediately after `connect()` returns zeros — normal, GATT layer not
  ready yet. Not an error.

---

## `writeValue` rc:7 on Subscribe

Calling `m_pNotifyCharacteristic->subscribe()` in `connectToDevice()` may log:
```
writeValue failed, rc: 7
```
(rc=7 = `BLE_HS_ENOTCONN` at GATT level — ATT layer not fully ready)

This is **expected and harmless**. NimBLE retries the CCCD write internally, and the Q1's
cached CCCD setting keeps notifications flowing regardless. The first notification packet
arrives within a few hundred milliseconds of `connect()` returning.

We subscribe early (before `onConnect()` fires) because the Q1 disconnects (reason=531) if it
receives no BLE traffic within ~300ms of connection. The subscribe attempt provides that
early traffic.

---

## GATT Cache Reuse (`deleteAttributes=false`)

```cpp
m_pClient->connect(m_targetAddress, false, true)
//                                  ^ deleteAttributes=false
```

On first boot the GATT cache is empty → full service/characteristic discovery runs.
On all subsequent reconnects the cached handles are reused — no discovery round-trips needed.

**Why this matters on C3:** Full GATT discovery on the single-core ESP32-C3 (RISC-V, shares CPU
between BLE and app tasks) was slow enough that the Q1's ~300ms connection timeout fired before
discovery completed, causing reason=531 kicks before the first keepalive could be sent.
`deleteAttributes=false` eliminated this problem.

`deleteClient()` is **never called** — it corrupts the NimBLE heap when called from within a
callback (Core 0). The client is reused across all reconnects; `connect()` clears stale state
internally.

---

## Multi-Core Threading Notes

ESP32-C3 is a single-core RISC-V — but NimBLE still uses RTOS tasks which interleave.

- `notifyCallback()` runs on the **NimBLE task** (conceptually "Core 0").
- `handle()` and `m_onDataUpdate()` run on the **main loop** (conceptually "Core 1").
- `PubSubClient::publish()` is **not thread-safe** — calling it from both contexts concurrently
  corrupts internal state → `ECONNRESET` from the broker.

Fix: `notifyCallback()` sets `volatile bool m_newDataAvailable = true`. `handle()` reads and
clears the flag, then calls `m_onDataUpdate()`. All MQTT publishes happen on the main loop only.

Same issue exists in `MqttView::publishAllConfigs()` — guarded by `m_publishingConfigs` flag
to block concurrent `publishState()` calls during HA config broadcast.

---

## Connection Parameters

Requested in `connectToDevice()` before every `connect()` call:
```cpp
m_pClient->setConnectionParams(12, 24, 0, 600);
//  minInterval=12 (~15ms), maxInterval=24 (~30ms), latency=0, timeout=600 (6s)
```

The 6s supervision timeout means keepalives must arrive within 6 seconds.
`KEEPALIVE_INTERVAL = 200ms` provides a large margin.

`setDataLen(64)` was **removed** — caused `Set data length error: 514` on every connect because
64 bytes is below the minimum negotiable PDU size. MTU is auto-negotiated (255 bytes in practice).

---

## `m_sessionActive` — Ground Truth for Belt Activity

Set when `speedFeedback > 0.001` and status is RUNNING.
Reset on each new BLE connection (in `connectToDevice()` before subscribe).

Used in `onDisconnect()` to distinguish:
- **Idle kick** (`!beltWasActive`): Q1's routine timeout, no real session in progress.
- **Mid-session kick** (`beltWasActive`): Belt was actually moving — commit delta to NVS,
  reconnect with 10s backoff.

Immune to:
- Phantom RUNNING from `0x34` packets (flags=0xBC, speed=0).
- Residual belt `distanceKm` from a previous session (belt doesn't reset odometer between sessions).

---

## Session Delta Accounting

Belt odometer at connection start is captured in `m_connectionBaseDistKm` (and `BaseCal`, `BaseDurSec`).
All `addSession()` calls commit `(current - base)`, not the raw odometer.

This prevents double-counting when:
- A mid-session BLE reconnect occurs (prior delta already committed in `onDisconnect()`).
- The belt still shows a non-zero odometer from the previous session at connect time.

If `parsed.distanceM / 1000.0f < m_connectionBaseDistKm`, the user pressed START and the belt
reset its odometer — bases are zeroed to prevent negative delta.

---

## NVS Persistence

| Namespace | Key | Default | Description |
|-----------|-----|---------|-------------|
| `pk_cfg` | `ar` | `true` | Auto-reconnect enabled |
| `pk_cfg` | `idle` | `30` | Idle disconnect minutes (0 = disabled) |
| `pk_cfg` | `pause` | `10` | Pause timeout minutes (0 = disabled) |
| `pk_state` | various | — | Cumulative totals (distance, steps, calories, duration) |
| `pk_calib` | various | — | Calibration points for dynamic step length |

Settings are only written to NVS by the public setters (`setAutoReconnect`, `setIdleDisconnectMins`,
`setPauseTimeoutMins`). Internal runtime changes to `m_autoReconnect` (session end, kicks, etc.)
bypass the setter so the user's preference is preserved across reboots.

---

## Known Remaining Behaviours / Gotchas

- **~2 min settle after pressing Connect**: Q1 always needs ~8 kick cycles before stabilising.
  With 5s backoff this is ~2 min. There is no workaround — the belt's firmware controls this.

- **Beeping during kick phase**: Each BLE connection attempt causes a beep from the treadmill pad.
  ~8 beeps are expected when pressing Connect from cold. Not a bug.

- **Phantom RUNNING on connect**: First `0x34` packet after connect shows status=RUNNING with
  speed=0. `m_sessionActive` guards against treating this as a real session.

- **Q1 idle kick at ~3.5h**: If we stay connected idle that long, the Q1 kicks us (reason=531)
  exactly as it would during the kick phase. The idle-disconnect timer (default 30 min) prevents
  ever reaching this — but it's good to know it exists.

- **`getConnInfo()` zeros post-connect**: Normal; called before GATT layer is ready.
  The values fill in once the connection is fully established.

- **Calibration points HA entity**: HA state values are capped at 255 chars. Full JSON for 10
  calibration points exceeds this. The entity publishes count only; points are stored in NVS and
  accessible via a separate restore mechanism.

- **Restore after NVS wipe**: If "Erase Flash" accidentally clears NVS totals, use
  `restoreTotals(distKm, steps, calories, durationSec)` — reads last good values from HA sensor
  history, writes immediately to NVS.
