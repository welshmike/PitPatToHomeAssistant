# PaceKeeper — Day / Week / Month Summaries: How They Worked and How to Troubleshoot

## Overview

The daily, weekly and monthly summaries are not calculated by the ESP32. They are derived entirely by Home Assistant from four MQTT sensor entities that the ESP32 publishes with the `state_class: TOTAL_INCREASING` property. This class tells HA the sensor never resets — it only ever goes up — so HA can calculate period deltas automatically by subtracting the value at the start of the period from the value at the end.

---

## The Data Pipeline

```
Belt (BLE packets)
  │
  ▼
TreadmillHandler::notifyCallback()
  │  Parses 0x2F / 0x34 packets
  │  Accumulates session distance/steps/calories/duration
  │
  ├─ On each running packet:
  │    Adds session delta to m_state totals (in memory only)
  │
  └─ On STOPPED transition (session ends):
       m_state.addSession(dist, steps, cal, dur)  ← adds to in-memory totals AND saves to NVS
       Sets data.sessionComplete = true
         │
         ▼
MqttView::publishState(data)
  │
  ├─ Publishes shared state JSON (speed, distance, status, etc.) to state topic
  │
  └─ Publishes 4 cumulative total sensors individually (with retain=true):
       sensor.pacekeeper_total_distance_km   ← TOTAL_INCREASING, unit: km
       sensor.pacekeeper_total_steps         ← TOTAL_INCREASING, unit: steps
       sensor.pacekeeper_total_calories      ← TOTAL_INCREASING, unit: cal
       sensor.pacekeeper_total_duration      ← TOTAL_INCREASING, unit: s
```

---

## The Four Cumulative Sensors

These are the sensors that power the summaries. All four are published to their own individual MQTT topics with `retain=true`.

| HA Entity | MQTT Object ID | State Class | Unit |
|---|---|---|---|
| `sensor.pacekeeper_total_distance_km` | `total-distance` | `total_increasing` | km |
| `sensor.pacekeeper_total_steps` | `total-steps` | `total_increasing` | steps |
| `sensor.pacekeeper_total_calories` | `total-calories` | `total_increasing` | cal |
| `sensor.pacekeeper_total_duration` | `total-duration` | `total_increasing` | s |

These values are **all-time lifetime totals** stored in ESP32 NVS flash. They are loaded on boot and only ever increase. Each session adds its results to them when the belt stops.

---

## How HA Calculates Daily / Weekly / Monthly Summaries

Because the sensors have `state_class: total_increasing`, HA's statistics engine records a snapshot of each sensor's value at the start and end of every hour in the `statistics` database. This means:

- **Daily summary** = value at midnight tonight − value at midnight last night
- **Weekly summary** = value at start of week Sunday/Monday − value at previous week start
- **Monthly summary** = value at start of this month − value at start of last month

These are available via:
- **Statistics cards** in the HA dashboard (using `statistics-graph` or `apexcharts-card` with `statistics` type)
- **Template sensors** using `states()` or the `recorder/statistics_during_period` WebSocket API
- **Utility meters** (if you've configured them separately in `configuration.yaml`)

HA does **not** require the ESP32 to be on at midnight — it uses the last known value before the period boundary as the baseline.

---

## NVS Storage Keys

The totals are persisted in ESP32 NVS under the `pacekeeper` namespace:

| NVS Key | Type | Description |
|---|---|---|
| `tot_dist` | float | Total distance in km |
| `tot_steps` | uint32 | Total step count |
| `tot_cals` | uint32 | Total calories |
| `tot_dur` | uint32 | Total duration in seconds |
| `step` | float | Static step length in metres |
| `calib_cnt` | uint32 | Number of dynamic calibration points |
| `calib_pts` | bytes | Calibration point array |

`loadFromNVS()` is called once in `begin()` at boot. `saveTotalsToNVS()` is called inside `addSession()` each time a session ends.

---

## What the Refactor Changed

The major restructuring that could affect the summaries:

### 1. `TreadmillState` extracted as a separate class
Previously, `m_totalDistanceKm`, `m_totalSteps`, `m_totalCalories`, `m_totalDurationSec` were direct member variables on `TreadmillHandler`. They are now owned by `TreadmillState m_state`.

`loadFromNVS()` is now called as `m_state.loadFromNVS()` inside `TreadmillHandler::begin()` — previously it was called directly in `begin()` and loaded into handler member vars.

### 2. Packet parsing moved to `BA05Protocol`
`notifyCallback()` now calls `BA05Protocol::parsePacket()` to decode the BLE packet, then reads `parsed.distanceM`, `parsed.calories`, etc. instead of parsing inline. If `BA05Protocol::parsePacket()` returns incorrect values (e.g. wrong distance, or `valid=false` for packets it should accept), sessions will accumulate incorrectly or not at all.

### 3. `addSession()` path
Old flow in `notifyCallback()`:
```cpp
m_totalDistanceKm  += data.distanceKm;
m_totalSteps       += data.steps;
...
saveTotalsToNVS();
m_autoReconnect = false;
```

New flow:
```cpp
m_state.addSession(data.distanceKm, data.steps, (uint32_t)data.calories, data.durationSec);
// saveTotalsToNVS() is now called inside addSession()
m_autoReconnect = false;
```

The session-end condition (the `if` block guarding this) is unchanged: requires `STOPPED`, `m_sessionActive == true`, `distanceKm > 0`, and a real status transition.

### 4. `updateSession()` and `endSession()` exist but are NOT called
`TreadmillState` has `updateSession()` and `endSession()` methods that implement a dynamic delta-based step calculation. These are **not yet wired up** in `notifyCallback()`. The active path still uses the old inline logic in `notifyCallback()` calling `m_state.addSession()` directly. This is intentional for now but means the new dynamic step calculation is dead code.

---

## Most Likely Causes of Broken Summaries

### The ESP32 NVS totals were wiped during a `pio run --target erase`
The two erase+reflash cycles during this session **reset NVS to zero**. The cumulative totals published to MQTT after erase will be `0.0`, which HA sees as a valid decrease. `TOTAL_INCREASING` sensors tolerate a small decrease (treats it as meter rollover), but a reset to zero on a large value will be treated as a massive negative delta, corrupting the statistics history.

**How to check:** In HA Developer Tools → States, look at `sensor.pacekeeper_total_distance_km`. If it shows a much lower value than expected, the NVS was wiped and not restored.

**Fix:** Use the restore-totals MQTT topic. Publish to `pacekeeper-{mac}/restore-totals/set`:
```json
{"dist_km": 12.34, "steps": 15000, "calories": 800, "duration_sec": 5400}
```
Read the correct values from HA's statistics history before the zeroing event.

### Entity ID changed after refactor
If any of the MQTT discovery config payloads changed (object ID, device ID, or MQTT topic), HA creates a new entity with a new entity ID and a fresh empty statistics history. The old entity's history is orphaned.

**How to check:** In HA Settings → Devices → PaceKeeper, look for duplicate entities (e.g. two "Total Distance" sensors). If the new one has no history, this is the cause.

**Fix:** Delete the duplicate old entity, then rename or redirect the new one to match the historical entity ID if you need continuity.

### `BA05Protocol::parsePacket()` returns wrong `distanceM`
If the refactored parser reads the distance field from the wrong byte offset, or returns `valid=false` on valid packets, `data.distanceKm` will be zero or wrong. Sessions will either not be detected, or will log the wrong distance.

**How to check:** In the serial log, look at `FIRST POST-CONNECT PACKET` log lines. The `dist=XXm` value should match what the belt's own display shows.

### `m_sessionActive` not being set
The session-end condition requires `m_sessionActive == true`. This flag is only set when `speedFeedback > 0.001 mph`. If the refactored `BA05Protocol::parsePacket()` returns zero speed when the belt is actually running, `m_sessionActive` never gets set and no session is ever committed.

**How to check:** Serial log — look for `Session ended` log lines after a walk. If they're absent, the session commit is not firing.

### `retain=true` not surviving MQTT reconnect
The four total sensors are published with `retain=true` so the broker holds the last value. If the broker was restarted or the retain store was cleared, all four sensors will show `unknown` in HA after the next HA restart, until the next MQTT publish from the ESP32.

**How to check:** Disconnect the ESP32 (turn off BLE switch in HA), restart HA, wait 30 seconds, check if the four total sensors still show their last values. If they show `unknown`, the retain messages are not in the broker.

---

## Quick Diagnostic Checklist

1. **Check NVS values at boot** — serial log line: `Totals loaded from NVS: dist=X km  steps=X  cal=X  dur=X s`
2. **Check session commit fires** — serial log: `Session ended — totals:` and `Session summary:` after stopping the belt
3. **Check MQTT publish** — HA Developer Tools → MQTT → Subscribe to `homeassistant/#` and watch for the total sensor state topics updating after a session
4. **Check for duplicate entities** — HA Settings → Devices → PaceKeeper
5. **Check statistics continuity** — HA Developer Tools → Statistics → look for gaps or resets in the total distance/steps sensors around the date of the refactor/erase
6. **Check parsed distance** — `FIRST POST-CONNECT RAW:` log line, byte [13-14] (big-endian uint16) should equal belt display distance in metres
