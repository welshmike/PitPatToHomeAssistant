# PaceKeeper on M5Stack Dial v1.1 — design

Date: 2026-09-03. Status: approved for planning.
Companion audit: `doc/AUDIT_2026-09-03.md`. BLE behaviour reference: `doc/Q1_BLE_NOTES.md`.

## 1. Goals

- Replace the ESP32 DevKit with an M5Stack Dial v1.1 as the single BLE bridge to the DeerRun Q1 Classic Pro.
- Local control without Home Assistant: tap the screen to start/pause/stop, turn the dial to change speed, live stats on the round display.
- Standalone first: BLE and local controls work with no WiFi. WiFi, MQTT and OTA connect in the background and retry forever.
- Keep the Home Assistant MQTT integration and entity set unchanged. The HA device identity (`pacekeeper-bridge`) is MAC independent, so the Dial takes over the existing HA device.
- Preserve every Q1 BLE behaviour documented in `doc/Q1_BLE_NOTES.md` (kick-phase backoff, supervision-timeout drop, zombie detection, GATT cache reuse, 200 ms keepalive).
- Keep the DevKit build working throughout, so every refactor step is testable against known-good hardware before the port.

## 2. Non-goals

- RFID, RTC, battery operation, Strava changes, new HA entities.
- ESPHome. Its BLE client cannot do the keepalive and kick-phase handling the Q1 needs.
- Running the DevKit and Dial at the same time. Only one BLE central can hold the Q1.

## 3. Hardware facts used by this design

| Item | Value |
|---|---|
| MCU | StampS3A, ESP32-S3FN8, dual core 240 MHz, 8 MB flash, no PSRAM |
| Display | 1.28" round 240x240, GC9A01 SPI (RS G4, MOSI G5, SCK G6, CS G7, RST G8, BL G9) |
| Touch | FT3267, I2C SDA G11 / SCL G12 / INT G14 |
| Encoder | A G41, B G40. 16 detents and 64 pulses per revolution, so 4 pulses per detent. **Confirmed on hardware 2026-09-03: `Encoder.read()` changes by 4 per click, clockwise is positive.** No push switch |
| Button | WAKE = BtnA on GPIO42 (side of body). RST also on body |
| Buzzer | G3 (M5Unified `Speaker`) |
| Power | USB-C 5 V or rear screw terminal 6 to 36 V. HOLD pin GPIO46 |
| Library | `m5stack/M5Dial` 1.0.3 on `M5Unified` + `M5GFX`. `M5Dial.begin(cfg, enableEncoder=true, enableRFID=false)` |

## 4. Architecture

```
            +------------------+          +-------------------+
 BLE  <---> | TreadmillHandler | -------> |  SessionTracker   | ---> TreadMillData snapshot
            |  (link layer)    | packets  | (+TreadmillState) |        (mutex guarded)
            +------------------+          +-------------------+
                     ^                              |
                     | commands                     v  observers
            +------------------+          +--------+----------+
            | TreadmillController| <----- | MqttView | DialUi |
            |  (intent layer)  |  input   +--------+----------+
            +------------------+               ^         ^
                                               |         |
                                          NetManager   M5Dial (encoder, touch, BtnA, display)
```

### 4.1 Unchanged
- `ba05protocol`: packet build and parse.
- `TreadmillState`: NVS totals and calibration curve.

### 4.2 `TreadmillHandler` (BLE link layer only)
Keeps: client lifecycle, `connectToDevice()`, connection params, early keepalive, subscribe, `onConnect`/`onDisconnect` with all reason-code and backoff logic, keepalive scheduling, `sendCommand()` with cooldown and zombie detection, idle-disconnect and pause-timeout timers, intentional-drop flags, pending-command execution after cooldown.

Changes:
- Parsing result and connection events are handed to `SessionTracker` instead of being accounted inside the handler.
- All session-accounting members move out (`m_connectionBase*`, `m_paused*`, `m_justResumedFromPause`, `m_resumeDistanceM`, `m_sessionActive` becomes a tracker query).
- `m_lastData` is replaced by a `SnapshotStore`: a `TreadMillData` plus a FreeRTOS mutex with `write(const TreadMillData&)` and `read() -> TreadMillData`. NimBLE task writes, main loop reads. Lock hold time is a struct copy.
- No NVS writes from BLE callbacks. `onDisconnect` and `notifyCallback` record a `PendingCommit` (delta values) that `handle()` flushes on the main loop.
- Timers keep living here because they decide when to drop the link, but they are armed and disarmed by tracker events (`sessionStarted`, `sessionEnded`, `paused`, `settledIdle`).

### 4.3 `SessionTracker` (new)
Pure logic, no Arduino dependencies beyond `millis()` injected as a parameter, so it compiles under the `native` test env.

Responsibilities:
- Connection baselines (distance, calories, duration) captured on the first full packet per connection; reset to zero when the belt odometer is seen below the baseline.
- `m_sessionActive` ground truth (speed > 0.001 while RUNNING).
- Pause snapshot and deferred commit; resume detection and spurious post-resume STOPPED suppression.
- The single `computeDelta(current, base)` used by live totals, pause snapshot, session end and disconnect commit.
- Emits events to the handler: `sessionStarted`, `sessionEnded(summary)`, `paused(snapshot)`, `resumed`, `settledIdle` (first 0x2F STOPPED with no active session).
- Produces the outgoing `TreadMillData` (live fields, totals, session summary, `sessionComplete`).

### 4.4 `TreadmillController` (new, intent layer)
The only thing MQTT and the Dial call.

| Method | Behaviour |
|---|---|
| `start()` | If stopped: send start at `START_SPEED`. If disconnected: connect with pending START. Optimistic status COUNTDOWN. |
| `pause()` | If running: send pause. Optimistic PAUSED. |
| `resume()` | If paused: send speed = last commanded speed. |
| `stop()` | Always allowed. Optimistic STOPPED. |
| `toggleStartPause()` | stopped→start, running→pause, paused→resume, countdown→stop. Used by screen tap. |
| `setSpeedMph(f)` | Clamp to [0.6, 3.8]. If running or paused: send speed. If stopped or disconnected: start at that speed (pending SET_SPEED when disconnected). Optimistic `speedCmd`. |
| `nudgeSpeed(clicks)` | Adjusts a target by 0.1 mph per click, clamps, publishes a `TargetSpeedChanged(target, deadlineMs)` observer event, and after `SPEED_SETTLE_MS` (400 ms) with no further clicks calls `setSpeedMph(target)`. |
| `requestConnect()` / `requestDisconnect()` | Wraps `toggleConnection()` safety logic. |

Constants live in `platform.h`: `SPEED_MIN_MPH 0.6`, `SPEED_MAX_MPH 3.8`, `SPEED_STEP_MPH 0.1`, `SPEED_RAW_PER_MPH 1600`, `SPEED_SETTLE_MS 400`, `START_SPEED_RAW 994`.

Observers implement `onSnapshot(const TreadMillData&)`, `onTargetSpeed(float mph)`, `onNetStatus(NetStatus)`. `MqttView` and `DialUi` register at boot. The controller republishes the snapshot after every command so optimistic state reaches both views once.

Speed encoding stays mph x 1600 raw, cap 6080 raw (3.8 mph), stop below 100 raw, exactly as today.

### 4.5 `NetManager` (new) and the network task
**Amended 2026-09-03 after Task 7 review:** `NetManager`, `MqttView`, `PubSubClient` and OTA run on a dedicated FreeRTOS task (`NetTask`), not on the loop task. `PubSubClient::connect()` can block 3 s (TCP) + 5 s (CONNACK) and `WiFiClient::write()` up to 10 s once the send buffer saturates after AP loss, both longer than the 6 s BLE supervision timeout. The loop task talks to the net task through two FreeRTOS queues: commands in (MQTT → controller) and publish items out (snapshots and settings → MQTT). See Plan 1 Task 9 for the exact structs.

State machine ticked from the net task: `WIFI_DOWN → WIFI_CONNECTING → WIFI_UP → MQTT_CONNECTING → MQTT_UP`. Backoff 1 s, 2 s, 5 s, 10 s, capped at 30 s. No forced reboot on WiFi loss. `WiFi.begin()` is called once after BLE init and never blocks. OTA and mDNS start on `WIFI_UP`. MQTT subscriptions, discovery configs and retained states are (re)published on each `MQTT_UP` and on HA birth message, as today. Exposes `status()` for the display.

### 4.6 `DialUi` (new, Dial env only)
Input:
- Encoder: `readAndReset()` every loop, divided by 4 to get detents, sign = clockwise positive. Calls `controller.nudgeSpeed(detents)`.
- Touch: tap (press and release under 600 ms, movement under 20 px) → `toggleStartPause()`. Long press (held 1000 ms) → `stop()`, with a filling ring drawn during the hold so the user sees it coming.
- BtnA (side button): `stop()`. Backup and emergency stop.
- Any input while the screen is dimmed or off only wakes the screen; the input itself is discarded. Prevents a brush of the dial starting the belt in the dark.

Render, 20 Hz max, into a full-screen M5GFX sprite then pushed, to avoid flicker:

| Screen | Trigger | Content |
|---|---|---|
| Disconnected | status DISCONNECTED, no connect in progress | Last session summary (time, distance, steps). Hint "tap or turn to start". |
| Connecting | connect requested, kick phase | "Connecting… attempt N". Note "belt beeps are normal". Cancel on long press. |
| Countdown | status COUNTDOWN | The BA05 packet does not carry the countdown number, so show a pulsing "STARTING" with "tap to cancel". |
| Running | status RUNNING | Time big (mm:ss, h:mm:ss over an hour) in the centre. Speed ring 0 to 3.8 mph around the edge with small numeric speed at the ring end. Row under time: distance km and steps. |
| Paused | status PAUSED, or STOPPED while tracker says paused | Same as Running, dimmed, "PAUSED" pulsing, "tap to resume, hold to stop". |
| Speed overlay | after `onTargetSpeed` | Target speed replaces the time for 1500 ms after the last click, ring shows target. Reverts automatically. |

Every screen has three status dots on the top edge: BLE (on when connected), WiFi, MQTT.

**Amended 2026-09-03 after Task 8 doc pass:** as built, the Paused hint text reads
"tap resume - hold stop" (not "tap to resume, hold to stop"), and the three status dots are
drawn at full brightness even on the Paused screen, unlike the rest of that screen's elements,
which are all halved. See `src/DialUi.cpp` (`drawRunning()`, `drawStatusDots()`).

**Amended 2026-09-03 after final review:** `currentScreen()` now checks `controller.isConnecting()`
before the COUNTDOWN test, not after — `TreadmillHandler::start()` while disconnected queues the
command and the controller optimistically publishes COUNTDOWN right away, so without this reorder
the Dial showed "STARTING / tap to cancel" for a belt that wasn't actually counting down, and its
tap-to-cancel went through `stop()`, which the disconnected link refuses (nothing was cleared).
Connecting now wins that race, and its cancel gesture is tap **or** hold (not hold alone) —
`handleInput()` checks `currentScreen() == CONNECTING` for both `ev.tap` and `ev.longPress` and
routes either one to `requestDisconnect()` instead of `toggleStartPause()`/`stop()`. The Starting
screen is consequently reachable only once actually connected and the belt itself reports
COUNTDOWN. See `src/DialUi.cpp` (`currentScreen()`, `handleInput()`).

Idle power: after 2 min in Disconnected or Stopped, brightness drops to 20 %. After 10 min the backlight goes off. Any input wakes at full brightness.

Buzzer: one short click on an accepted tap, two on stop, a low buzz when a command is refused (post-connect cooldown, disconnect blocked while belt active). Compile-time `DIAL_SOUND` flag defaults on.

### 4.7 `board.h`
Selected by build flag `-DBOARD_DEVKIT` or `-DBOARD_M5DIAL`. Provides `HAS_STATUS_LED`, `LED_BLE_PIN`, `HAS_DIAL_UI`, and serial settings. `main.cpp` contains no pin numbers.

## 5. Threading and timing

- Loop task has no long `delay()`. Order per pass: watchdog reset, `handler.handle()`, drain command queue into `controller`, `controller.tick()` (speed settle timer), `dialUi.tick()` (Dial only), `delay(1)`.
- Net task: `net.tick()`, drain publish queue into `MqttView`, `vTaskDelay(10 ms)`. Only this task touches sockets.
- NimBLE task: `notifyCallback` parses, calls `tracker.onPacket()`, writes the snapshot under the mutex, sets `m_newDataAvailable`. No MQTT, no NVS, no display from this task.
- `onDisconnect` runs on the NimBLE task: records pending commit, sets flags. Same rules.
- `MqttView::m_publishingConfigs` guard is removed; all publishes are already on one task.
- Watchdog stays at 300 s on the main loop task.

## 6. Build and toolchain

- `platformio.ini` envs: `devkit-usb`, `devkit-ota` (today's `usb`/`ota`, renamed), `dial-usb`, `dial-ota`, `native` (unit tests).
- Platform: pin `espressif32` to a version with `m5stack-stamps3`. First try `6.13.0` (Arduino-ESP32 2.0.17, known good with NimBLE-Arduino 2.5.1 and M5Unified). If the Dial libraries require Arduino 3.x, move to `7.1.0`. The spike task decides and records the outcome in the plan.
- Dial `lib_deps` add `m5stack/M5Dial@^1.0.3`, `m5stack/M5Unified`, `m5stack/M5GFX`. Board `m5stack-stamps3`. USB CDC on boot for serial logs.
- Dial partition table `partitions_8mb.csv`: nvs 0x5000, otadata 0x2000, app0 3 MB, app1 3 MB, spiffs remainder, coredump 64 KB. DevKit keeps `partitions.csv`.
- NVS namespaces and keys unchanged. A fresh Dial has empty totals; restore them with the existing `restore-totals` and `restore-calibration` MQTT topics from HA sensor history.
- `config.h` gains nothing new. `TARGET_ADDRESS`, WiFi and MQTT settings are the same.

## 7. Testing

Automated, run by subagents on every task:
- `native` env with Unity: `SessionTracker` (baselines, odometer reset, pause/resume, spurious STOPPED suppression, mid-session disconnect commit, delta never negative), `TreadmillController` (clamps, toggle state table, nudge settle timer, pending command when disconnected), `BA05Protocol` (packet build checksum, parse of recorded 0x2F/0x34/20-byte samples from `treadmill_log.txt`).
- `pio run -e devkit-usb` and `pio run -e dial-usb` must both succeed after every task. Flash usage reported.

Hardware, by Mike, at the end of each phase (checklist in the plan):
- Phase A (DevKit refactor): unplug WiFi router for 30 s mid-walk, belt must keep running. Start/pause/stop/speed from HA unchanged. Session totals match a manual calculation.
- Phase B (Dial hello): display, encoder detents, touch tap/long press, BtnA, buzzer, serial over USB-C.
- Phase C (Dial full): all Dial screens; tap/turn/hold behaviours; kick phase completes and connects; HA still sees the same device and entities; OTA works.

## 8. Migration phases

- **Phase 0, spike (parallel with A):** toolchain bump, `dial-usb` env, `M5Dial` hello world showing encoder count and touch coordinates. Decides the platform version.
- **Phase A, refactor on DevKit:** `board.h`; `NetManager` and non-blocking `loop()`; `SnapshotStore` mutex and pending commits; `SessionTracker` extraction with unit tests; `TreadmillController` with unit tests; `MqttView` as observer; `main.cpp` reduced to wiring. Commit uncommitted baseline first.
- **Phase B, port:** build the refactored firmware for `dial-usb` with `DialUi` stubbed to status dots only. Verify BLE against the Q1 on the S3 radio.
- **Phase C, Dial UI:** screens, inputs, overlay, dimming, buzzer. Then `dial-ota` and README update.

## 9. Risks

- **S3 radio timing vs the Q1's ~300 ms kick window.** The DevKit needed the early keepalive and GATT cache reuse to survive it; the S3 should be at least as fast, but this is only provable on hardware in Phase B.
- **Shared internal I2C.** Touch, RTC and RFID share G11/G12. Touch polling from the main loop is fine; nothing else touches that bus in this design.
- **Toolchain jump.** Arduino-ESP32 2.0.17 → possibly 3.x changes the watchdog API (already handled behind `ESP_ARDUINO_VERSION_MAJOR`), `mdns.h`, and ArduinoOTA behaviour. The spike surfaces these before the app code depends on them.
- **Accidental start.** Mitigated by: wake-only input when dimmed, the belt's own countdown shown on screen, tap cancels during countdown, and minimum speed 0.6 mph.

**Amended 2026-09-03 after first hardware session:** rotate-to-start removed — the encoder only adjusts speed while the belt is RUNNING (not paused); on all other screens rotation is ignored and reserved for future screen navigation. Status dots are hidden on the Running screen. The speed overlay blanks the ring interior before drawing.

## 4.7 Start speed and start selector (added 2026-09-03, approved)

- Setting `startSpeedMph`: default 1.0, range [SPEED_MIN_MPH, SPEED_MAX_MPH], step 0.1. NVS namespace `pk_cfg`, key `start` (stored as tenths, uint16). HA number entity `start-speed` "Start Speed" (config category, min 0.6, max 3.8, step 0.1, unit mph). Every "start" path (HA Start button, Dial long press, queued start while disconnected) sends the configured speed instead of the fixed 994 raw.
- Dial: tap on Disconnected opens the **Selector** screen showing the candidate speed large (`Font7`) with `mph` caption, the speed ring at that value (amber), hints `tap to start` / `hold: default`. Knob click or horizontal swipe (right = faster) changes the candidate by 0.2 mph, clamped. Opens at the default. Second tap starts at the candidate; long press starts at the default; side button cancels; 20 s without input cancels. Long press on Disconnected starts at the default without the selector. Paused/Running/Connecting behaviour unchanged.
- Swipe: horizontal displacement ≥ 40 px within one touch with |dx| > |dy|; a swipe suppresses tap and long press for that touch.

## 4.8 Desk mode: cards, knob navigation, clock (added 2026-09-03, approved)

Decomposition agreed with Mike: (1) card framework + knob navigation + analogue clock; (2) HA control cards (lights, music) over MQTT; (3) flights overhead (direct HTTPS to an ADS-B feed); (4) Google Calendar (device-code OAuth, direct); Claude usage parked.

- **Cards** form a ring: Treadmill → Clock → (Calendar → Flights → Lights → Music, added by later sub-projects) → Treadmill. The Treadmill card is the existing Disconnected screen (tap = start-speed selector, hold = start). While the belt is not running and no selector is open, the knob rotates the ring (one detent = one card). Side button returns to the Treadmill card (and still stops the belt when running). Belt CONNECTING / STARTING / RUNNING / PAUSED screens override the card as today, and the ring resumes on the last card afterwards.
- A card may be **engaged** by tap where it owns a value (brightness, volume): while engaged the knob adjusts the value; hold or a 10 s timeout releases it. Cards without a value never engage.
- **Boot card is Clock.** No idle return. **Backlight dims after 2 min and never turns off** (the OFF stage is removed); the first input on a dimmed screen only wakes it.
- **Clock card**: analogue face on the round display — 12 tick marks, hour/minute/second hands, small date (`Mon 3 Sep`) at the 6 o'clock position, redrawn once a second. Time from NTP over WiFi with the POSIX TZ string `TIMEZONE_TZ` in `config.h` (default Europe/London), and the Dial's BM8563 RTC is synced from NTP and read at boot so the clock is right before WiFi is up. When time is unknown (fresh device, no WiFi, empty RTC) the face shows `--:--`.
- Data cards (later) refresh on their own interval and show a small stale marker when the last fetch failed.

## 4.9 Flights card (added 2026-09-04, approved)

Shows the aircraft overhead, nearest first, with airline logo, route, altitude and ground speed. Data is fetched directly by the Dial over HTTPS from the network task; nothing goes through Home Assistant.

- **Sources (all keyless, verified 2026-09-04):**
  - Aircraft: `https://opendata.adsb.fi/api/v2/lat/{lat}/lon/{lon}/dist/{nm}` → `aircraft[]` with `hex, flight, t, desc, alt_baro, gs, track, lat, lon`. (airplanes.live requires emailing for access; not used.)
  - Route: `https://hexdb.io/api/v1/route/icao/{callsign}` → `{"route":"VABB-EGLL"}` (ICAO airport codes).
  - Airport: `https://hexdb.io/api/v1/airport/icao/{ICAO}` → `{"iata":"LHR","airport":"Heathrow Airport","country_code":"GB"}`.
  - Operator: `https://hexdb.io/api/v1/aircraft/{hex}` → `{"RegisteredOwners":"Virgin Atlantic Airways","OperatorFlagCode":"VIR"}`.
  - Logo: `https://pics.avs.io/120/48/{IATA}.png` (needs the IATA airline code; firmware table maps ICAO→IATA for common airlines; text fallback otherwise). Logos cached in the Dial's LittleFS partition, downloaded once per airline.
- **Config** (`config.h`): `HOME_LAT`, `HOME_LON`, `FLIGHTS_RADIUS_MI` (default 3; converted to nautical miles for the query).
- **Refresh:** every 20 s while the Flights card is showing; no fetches otherwise. Enrichment (route/airports/operator/logo) is fetched once per callsign/code and cached in RAM for the session; at most one HTTPS request in flight at a time; 5 s timeouts; bodies capped at 16 KB. TLS uses `setInsecure()` (public read-only data; accepted trade-off, documented).
- **Card:** logo (120x48) at the top, else operator name in `Font2`; `callsign · type` under it; route large in the middle as `LHR → JFK` (IATA; `????` when unknown); rows `12,000 ft · 450 kt` and `3.1 mi NE · 2/5`. Tap cycles to the next aircraft (nearest first, up to 6). Stale marker (small grey dot at the top) when the last fetch failed; `no aircraft nearby` when the list is empty; `waiting for WiFi` when offline.
- **Threading:** `FlightsService` runs on the network task and publishes a `FlightsSnapshot` through a mutex-guarded store; `DialUi` (loop task) only reads snapshots and decodes the current logo PNG once per airline into a small sprite.

**Amended 2026-09-04 (Flights card as built):** the built-in M5GFX bitmap fonts are ASCII-only, so separators render as `-` (e.g. `BAW117 - A320`, `12,000 ft - 450 kt`) and the route arrow as `->`, consistent with the Paused hint. Stale marker is a 6 px grey dot at (120, 14).

**Amended 2026-09-04 (page dots):** the `tap: next` hint and the `2/5` index suffix were replaced by a row of page dots on the bottom row — one per aircraft (up to 6), filled for the one currently shown and hollow outlines for the rest — so position in the list is shown once, not repeated as text on every line.

**Superseded 2026-09-05:** this section's data path (the sources list, `HOME_LAT`/`HOME_LON`/`FLIGHTS_RADIUS_MI`, the refresh/enrichment/threading bullets) is replaced by §4.11 — all of it now runs in Home Assistant. The card layout, tap-to-cycle and page-dot bullets above still apply unchanged.

## 4.10 Lights cards (added 2026-09-04, approved)

Two desk cards control Home Assistant lights over MQTT: **Office** (`light.mikes_office_2`, a colour-temperature group, 2202–4000 K) and **Lamp** (`light.lamp`, colour temperature 2000–6535 K plus hue/saturation colour). Home Assistant mirrors light state to the Dial and applies the Dial's commands through two automations (created 2026-09-04, YAML kept in `doc/HA_LIGHTS_AUTOMATIONS.md`); nothing else on the Dial changes.

- **Ring order:** TREADMILL → CLOCK → FLIGHTS → LIGHT_OFFICE → LIGHT_LAMP → wrap. Knob scrolls cards as on the other desk cards **unless a value is engaged** (below). Side button homes to Treadmill (existing rule). Long press does nothing on these cards. No status dots (desk-card rule).
- **MQTT contract** (Dial is the MQTT client; HA automations are the counterpart):
  - Dial → HA command: `pacekeeper-dial/light/{office|lamp}/set`, JSON, `state` required, other keys optional: `{"state":"ON","brightness_pct":65}`, `{"state":"ON","color_temp_kelvin":3000}`, `{"state":"ON","hs_color":[210,100]}`, `{"state":"OFF"}`. Any non-OFF command turns the light on.
  - HA → Dial state (retained): `pacekeeper-dial/light/{office|lamp}/state` = `{"state":"on"|"off"|"unavailable", "brightness_pct":0-100, "color_mode":"color_temp"|"xy"|"hs"|null, "color_temp_kelvin":K|null, "hs_color":[h,s]|null, "min_kelvin":K, "max_kelvin":K, "supports_color":bool}`. Published on every state/attribute change, at HA start, and in reply to the Dial publishing anything on `pacekeeper-dial/light/refresh` (the Dial does so once after each MQTT (re)connect, after subscribing).
- **Card layout** (240×240 round): title `Font4` at y≈40 (`Office` / `Lamp`). Centre: when on, brightness `Font7` (`65%`); when off, `OFF` in `Font4` DIM; when no retained state yet or `unavailable`, `no data` DIM; when MQTT is down, `waiting for HA` DIM. Caption `Font2` under the value: `2700K` (temperature mode) or `hue 210` (colour mode). A 270° value ring (gap at the bottom, like the speed ring) shows the **engaged** value's position in its range — brightness 1–100 %, kelvin min–max, hue 0–360 — in PENDING amber while engaged and settling, SPEED cyan otherwise; when nothing is engaged the ring shows brightness.
- **Buttons** (on-screen, replacing long-press gestures): circles r = 22 px along the bottom arc, labels `Font2` beneath the centre. Lamp: `Power` (56,186), `Bright` (120,206), `Colour` (184,186). Office: `Power` (84,200), `Bright` (156,200). Hit-test is the circle plus a 6 px margin. The engaged button is drawn filled (PENDING) with BG-coloured label; others outlined DIM with TEXT label; all buttons drawn DIM_DIM and inert while the card shows `no data`/`waiting for HA` (Power stays active on `no data` so a light can still be switched on blind).
  - `Power`: publishes `{"state":"OFF"}` if the light is on, else `{"state":"ON"}`; optimistic local update; releases any engaged value.
  - `Bright`: engages brightness; tap again releases. Knob ±5 % per detent, clamped 1–100.
  - `Colour` (Lamp only): first tap engages the light's current colour mode (temperature if `color_mode == color_temp` or colour unsupported, else hue); tap again switches temperature ↔ hue; a third tap releases. Knob: ±100 K per detent clamped to min/max kelvin; hue ±10° per detent wrapping 0–360, saturation kept from HA (100 if none).
  - Engaged state auto-releases after 10 s without a detent or button tap.
- **Settle:** while engaged, detents change the local value immediately (ring and value redraw) and one command is published 300 ms after the last detent, like the treadmill speed settle. Incoming HA state is adopted immediately except for the engaged field while a settle is pending (so HA's echo can't fight the knob).
- **Threading:** MQTT receive (net task) parses the state JSON in `LightsModel::parseLightState` and writes a `Guarded<LightsSnapshot>` owned by `LightsService`; `DialUi` (loop task) reads it at most every 250 ms. Commands go loop → net through the existing publish queue as `PubType::LIGHT_CMD` carrying the light key and the pre-formatted JSON (≤ 96 bytes); the net task publishes to the set topic (QoS 0, not retained). Sizes: snapshot ~48 bytes; PublishItem grows ~100 bytes (×8 queue depth) — negligible against the Plan 5 heap budget.
- **Pure/testable pieces:** `LightsModel` (parse state JSON, format command JSON), `LightCardState` (engage/release/timeouts/detent maths/settle → command), `LightButtons` (geometry + hit-test), `DialInput` gains `tapX/tapY` (touch-down position) on `DialEvents`.

**Amended 2026-09-04 (Lights cards as built):** the spec's button positions collided with the value ring (radius 108–118 around (120, 120)), so the buttons shrank to r = 21 px with their labels drawn *inside* the circles rather than beneath them (a label below the lowest button falls off the round display: cy + r + 12 = 229 > 226). Final centres: Lamp `Power` (60,178) / `Bright` (120,196) / `Colour` (180,178); Office `Power` (84,190) / `Bright` (156,190). The value ring reuses the same 300° geometry as the speed ring (gap at the bottom), not the 270° the spec called for. Font7 has no `%` glyph, so the brightness value is drawn as Font7 digits with a separate Font4 `%` beside them, centred together. `Power` is drawn inert only until a first state message has parsed (`view().valid`) — it stays active through `unavailable`/`no data` so a light can still be switched on blind, per the spec's own note. A release *tap* (a second tap on `Bright`, or the third tap on `Colour`) flushes any pending knob change as one command immediately rather than dropping it; only leaving the card entirely — knob-scrolling away, the side button, or a belt/connection screen taking over — releases silently with the pending edit discarded.

## 4.11 Flights via Home Assistant (added 2026-09-05, approved; supersedes §4.9's data path)

All flight logic moves to Home Assistant; the Dial only displays. Motivation (2026-09-04 findings): each TLS connection on the S3 costs ~45 KB heap, WiFi/BLE share the antenna so request bursts kicked the belt, and the one-request-per-1.5 s pacing that fixed that made enrichment take 8–10 s per aircraft.

- **Source:** the Flightradar24 HACS integration (`AlexandrErohin/home-assistant-flightradar24`, no subscription, installed 2026-09-05) configured via the HA API with the home position (51.566645, 0.005586), radius 5 km (≈ the previous 3 mi), default scan interval and altitude filters. Its `sensor.flightradar24_current_in_area` carries a `flights` attribute with callsign, flight number, airline (name/IATA/ICAO), aircraft code, origin/destination IATA and city, altitude (ft), ground speed (kt), heading, lat/lon and distance (km). The same sensor and the integration's map card serve any HA dashboard Mike builds (no dashboard is created by this project).
- **HA → Dial state:** automation `PaceKeeper Dial: flights publish` — triggers: state of `sensor.flightradar24_current_in_area` (any change incl. attributes), HA start, MQTT `pacekeeper-dial/flights/refresh` (the Dial publishes `1` there after each MQTT connect). Publishes retained JSON to `pacekeeper-dial/flights/state`, nearest first, at most 6 aircraft, compact keys so six aircraft fit in ~800 B:
  `{"ts":<epoch s>,"ac":[{"cs":"BAW123","fl":"BA123","ty":"A320","al":"BA","an":"British Airways","fr":"LHR","to":"JFK","alt":12000,"gs":450,"di":2.3,"br":135,"gnd":0}]}`
  `di` is statute miles and `br` the bearing from home in degrees, both computed in the template from the flight's lat/lon (HA's Jinja has `sin/cos/atan2/radians`); `an` truncated to 24 chars; null/missing fields emitted as `""`/`0`. The Dial's MQTT buffer grows from 1024 to 2048 B.
- **Logos:** the native `downloader` integration (download dir `www`) plus automation `PaceKeeper Dial: airline logos` — trigger `flightradar24_entry` event with a non-empty `airline_iata`; condition: IATA not already in the `logos` attribute of a trigger-based template sensor `sensor.pacekeeper_dial_logos_cached` (persisted list); action: `downloader.download_file` of `https://pics.avs.io/120/48/{IATA}.png` to `logos/{IATA}.png` with `overwrite: true`, then the sensor appends the IATA. HA serves the file at `http://<HA>:8123/local/logos/{IATA}.png`. A 404 (unknown airline at pics.avs.io) leaves no file; the Dial then gets 404 from HA and uses the text fallback as before.
- **Dial:** `NetTask` subscribes to `pacekeeper-dial/flights/state` (QoS 0) and publishes the refresh; `FlightsService::onStateMessage()` (net task) parses via the new pure `FlightsModel::parseDialFlights(json, len, FlightsSnapshot&)` into the existing `FlightsSnapshot`/`Aircraft` (fields `hex` and `type` keep their meaning: `hex` unused → callsign-keyed; `operatorName` ← `an`; `airlineIata` ← `al`; `fromIata`/`toIata` ← `fr`/`to`; `routeKnown`/`operatorKnown` always true). `stale` = no message for 120 s while MQTT is up; `offline` = MQTT down. Logo download: plain HTTP `WiFiClient` GET of `FLIGHTS_LOGO_BASE_URL "/logos/XX.png"` (default `"http://" MQTT_SERVER ":8123/local"`, overridable in `config.h`), same LittleFS cache, PNG signature check, ≤ 8 KB body, same `DialUi` decode path; 404 remembered per session. Removed: adsb.fi/hexdb parsing and caches, `AirlineCodes`, `WiFiClientSecure`/TLS, request spacing and heap guards, `HOME_LAT`/`HOME_LON`/`FLIGHTS_RADIUS_MI`; `Geo` keeps only `compass8`. Card layout, tap-to-cycle and page dots unchanged.
- **Auto-show (new, configurable):** when the belt is idle and the Clock card is showing, a change of aircraft count from 0 to > 0 switches the card ring to Flights; when the count returns to 0 (and the switch was automatic) the ring returns to Clock. A manual knob scroll or side button while auto-shown cancels the automatic return. Only the Clock card is interrupted — Treadmill, Lights and any belt/selector screen are never preempted. Setting `Flights Auto-show` (HA switch via MQTT discovery, `EntityCategory::CONFIG`, plus NVS, default on) follows the existing settings pattern (`CmdType::SET_FLIGHTS_AUTO_SHOW`, `PubType::FLIGHTS_AUTO_SHOW`, stored with the other settings in `TreadmillState`, echoed on change and full resync). Pure `FlightsAutoShow` state machine with Unity tests.
- **Docs:** README (Flights section rewritten), `doc/HA_FLIGHTS.md` (integration setup, both automations' YAML, the template sensor, MQTT contract), Phase G hardware checklist.

**Amended 2026-09-05 (as built):** the cached-logo list is an `input_text` helper
(`input_text.pacekeeper_dial_logos_cached`, comma-separated IATA codes, capped at ~80) rather than
the trigger-based template sensor sketched above, because trigger-based template sensors cannot be
created through the HA API; the logos automation triggers on the sensor's state change (and HA
start), not on the `flightradar24_entry` event, so airlines already overhead at startup are fetched
too, not only newly-arriving ones. `FlightsService` gained a 5 s retry gate on logo network fetches
(`kLogoRetryMs`), keyed on the last attempted IATA, so a stalled or failing logo fetch isn't retried
every tick. The Flights card's offline caption reads `waiting for HA` (offline now means MQTT down,
not WiFi down); the empty-state second line reads `none in range` (the search radius is HA's
business now, not a Dial-side config value to name). Verified live 2026-09-05: payload
`{"ts":1788596952,"ac":[{"cs":"BAW84NT","fl":"BA847","ty":"A320","al":"BA","an":"British Airways","fr":"WAW","to":"LHR","alt":5850,"gs":269,"di":0.8,"br":246,"gnd":0}]}`
(167 B for one aircraft); logos served at `http://<HA>:8123/local/logos/VS.png` (3501 B) and
`.../2L.png` (4475 B).

**Amended 2026-09-05 (city names):** the publish automation now also sends `fc`/`tc` (origin/destination
city name, HA-truncated to 12 chars, `""` if unknown), and the Flights card draws them as a small
`Font2` line (`"London -> New York"`, or just the one known city, or nothing) beneath the IATA route
line, parsed by the same `FlightsModel::parseDialFlights` (fields `fromCity`/`toCity`; absent keys —
older payloads — parse as empty, same as any other missing string field).


**Amended 2026-09-05 (airline-only filter):** `input_boolean.pacekeeper_dial_airline_flights_only` (default on) is read by the publish automation; when on, aircraft with no airline IATA code are omitted from the payload, and toggling it republishes at once. Radius reduced to 4000 m the same day. Purely HA-side; no firmware change.

**Amended 2026-09-06 (logo band):** the top 58 px of the Flights card are white; the airline logo (white-backed sprite, 90 % scale) sits inside at y = 8, the text fallback is drawn dark on the band, and the stale dot moves to (120, 63). Logos from pics.avs.io are designed for light backgrounds.
## 4.12 Card menu, side-button semantics and Lights v2 (added 2026-09-06, approved; supersedes §4.10's controls)

Design session 2026-09-06 (mockups: Artifact "Dial Lights Redesign", option A2). Motivation: the Lights cards' text-label buttons were ugly and the engage/release model was clumsy; the knob doubled as card scroller, which stopped it meaning "the value on this page".

### Navigation (all cards)
- **Knob never scrolls cards.** It always adjusts the value of the page showing: Lights — brightness / kelvin / colour preset; Flights — cycles aircraft (replaces tap-to-cycle, tap still cycles too); Clock and Treadmill (Disconnected) — no effect.
- **Side button while the belt is running, paused, counting down or connecting:** instant stop/refuse exactly as today (`wasClicked`).
- **Side button, belt idle:** *single click* opens the **card menu**; *hold* (M5Unified `wasHold`, 500 ms) jumps to the Treadmill card, closing any menu or selector. Single click is decided after M5Unified's multi-click window (~350 ms); that latency is accepted on desk cards only.
- **Card menu** (`CardMenu`, pure): the five cards drawn as glyphs on a ring (radius 88, Treadmill at 12 o'clock, then Clock, Flights, Office, Lamp clockwise; item radius 18, highlighted 22 filled amber with the glyph in BG colour, name of the highlighted item in Font4 at the centre, hint `turn - tap` in Font2 beneath). Opens with the current card highlighted. Knob detents move the highlight (wrapping); tap on a glyph (hit radius 26) selects that card, tap elsewhere or a second click selects the highlighted one; 8 s without input closes without change. Selecting or hold-home counts as manual navigation for `FlightsAutoShow`. Belt screens preempt and close the menu.

### Lights v2 (`LightCardState` rewritten)
- **Off:** name (Font2, y 34), a power circle r 44 at (120,124) with the power glyph, caption `tap to switch on`. Tap anywhere on the face → `{"state":"ON"}` (light restores its own last brightness); knob does nothing; touch-hold does nothing.
- **On:** page dots at y 54 (Lamp: Brightness → Kelvin → Colour; Office: Brightness → Kelvin). Horizontal swipe changes page (no wrap). Every arrival on the card, and every switch-on, starts on Brightness. Small power glyph at (120,212) r 8: tap (hit r 14) → off. Touch-hold anywhere 1 s → off, with the existing red hold arc.
- **Brightness page:** 300° ring (existing geometry) as track + level, Font7 percentage at centre, `%` Font4 beside it, caption of the live colour state (`2700K` / `hue 240`) at y 160. Knob ±5 % per detent, clamped 1–100.
- **Kelvin page:** value Font7-sized at y 112, `K` beneath, warm→cool bar (three palette steps, the 3 free slots: warm `#FFB46B`, neutral `#FFF1D6`, cool `#CFE3FF`) at y 170 with a triangle marker at the light's kelvin within `[minKelvin, maxKelvin]`; caption shows the range. Knob ±100 K, clamped.
- **Colour page:** *(superseded 2026-09-06 by §4.18 — continuous hue ring)* eight preset hues (0, 30, 60, 120, 180, 240, 275, 320 at saturation 100) on a ring of radius 74 starting at 12 o'clock, r 12 (selected r 16 with a 3 px outline: white, amber while settling); centre disc r 30 in the selected colour. Swatches and disc are true colour: the canvas reserves them as TRANSPARENT and `DialUi` fills them directly on the display after the frame push (no sprite). Knob ±1 preset per detent (wrapping); tap on a swatch (hit r 18) selects it. HA's reported hue snaps to the nearest preset for the highlight.
- **Live mode:** the Lamp is either in temperature or hue mode. The page for the inactive mode draws dim with caption `not active - turn to use`; the first detent on it sends that mode's command and makes it live.
- **Settle and confirmation:** unchanged from Plan 6 — one command 300 ms after the last detent (brightness_pct / color_temp_kelvin / hs_color [preset, 100]); optimistic values held until HA echoes or 1.5 s pass. No engage state, no 10 s release, no Power/Bright/Colour buttons (`LightButtons.h` removed; geometry moves to `LightLayout.h`).
- **HA side:** unchanged (same topics, same command shapes).

### Structure
- `DialUi.cpp` is ~2000 lines. The `Col` palette enum and `col()` move to `DialTheme.h` (`struct DialTheme { bool useCanvas; uint32_t col(Col) const; }`); Lights drawing moves to `DialLightsView.cpp`, menu drawing to `DialMenuView.cpp`, primitive glyphs (power, sun, colour dots, belt, clock, plane, bulb) to `DialGlyphs.cpp` — all device-only, free functions over `LovyanGFX&` and `const DialTheme&`.

**Amended 2026-09-06 (as built):** the view split went further than the spec's own list — Flights and Clock drawing also moved out of `DialUi.cpp`, into `DialFlightsView.cpp` and `DialClockView.cpp` respectively, on the same free-function-over-`LovyanGFX&`/`DialTheme&` pattern as Lights/Menu/Glyphs. The hold-progress arc is suppressed on two screens where a hold does nothing: the card menu (whose ring sits where the arc would draw) and a light card that is already off (nothing left to switch). The Kelvin page's "not active - turn to use" caption sits at y 190, not higher — the round bezel eats the ends of that 24-character Font2 line any further up. `DialUi::tickLights()` polls **both** light cards' settle timers every frame regardless of which is showing, so an edit made just before the ring moves off a light card still goes out 300 ms later; `LightCardState::tick()` is a no-op when nothing is pending. `LightCardState::selectPreset()` only acts on the Colour page of a colour-capable, editable card — a tap landing on those coordinates on any other page (or on Office, which has none) is treated as a tap on bare background. `LightCardState::swipe()` is a no-op while the light is off, since the off face has only the one power target and no pages to move between. Single-click latency is accepted only on the desk cards that use it for the card menu — the belt's own emergency stop still fires on the raw, undebounced press. That latency is **500 ms**, not the ~350 ms this section's Navigation bullet estimated: M5Unified decides `wasSingleClicked()` one `_msecHold` (500 ms) after the release, the same threshold `wasHold()` fires on while still held.

**Amended 2026-09-06 (Lights v2 review):** five further as-built deviations, all approved in review.

1. **The swatch ring is rotated half a step.** *(superseded by §4.18)* `LightLayout::kSwatchStartDeg = 22.5` is the angle of swatch 0 clockwise from 12 o'clock (radius still 74), so no swatch sits at 12 or 6 o'clock. Starting at 12 put swatch 0 under the page-dot row and swatch 4 on the small power glyph, whose hit discs overlapped: a tap at the bottom of the ring was ambiguous. Rotated, the nearest centres (swatches 3 and 4, at 157.5 and 202.5 degrees, ~(148,188) and ~(92,188)) are ~34.5 px from the glyph centre (120,208, hit r 18), past the `kSwatchHitR` (14) `+ kPowerGlyphHitR` (18) = 32 px at which the discs would touch; a layout test sweeps the round face and asserts the two hit tests never both claim a point.
2. **The Colour page draws bare:** no card name, no page dots and no power glyph — only the ring and the centre disc (Mike, 2026-09-06: nothing under the picker). Leave it by swiping back or via the 10 s page timeout; the glyph is not a tap target there.
3. **Tap priority on the Colour page is swatch before power glyph**, so a swatch wins outright regardless of what the geometry later becomes. **Power glyph enlarged (2026-09-06):** a tap on it did not register on hardware at r 8 / hit r 14, so it is now drawn at (120,208) r 9 with hit r 18, and `kSwatchHitR` drops to 14 (swatch r 12 still fully covered) to keep the bottom swatches ~35 px from the glyph, past the new 32 px touch distance. Light-card taps and page-idle resets are logged at `log_i` for the next serial session.
4. **A stop swallows the click that follows it.** One physical press produces both `wasClicked()` (at release, the emergency stop) and `wasSingleClicked()` (500 ms later, the card-menu gesture); without this the stop also opened the menu behind itself on whichever card the ring returned to. `DialUi` calls `DialInput::consumeClick()` on the tick it *acts* on `btnStop`, arming a one-shot 700 ms swallow of the next decided click. It is not armed where `btnStop` is ignored (belt idle), so an ordinary idle click still opens the menu. `wasHold()` is untouched — a hold never co-fires with a click.
5. **Pages fall back to Brightness after 10 s idle** (`LightCardState::PAGE_IDLE_MS`, implemented in cb1d34b): the Kelvin and Colour pages are transient, so a card left on one and glanced at later reads as brightness, the same state a fresh arrival gives. Any swipe, detent or swatch tap restarts the timer.

## 4.13 Flights radar empty state (added 2026-09-06, approved)

When the Flights card is showing and the aircraft list is empty (and neither `offline` nor `stale` applies), the card draws a radar sweep instead of `no aircraft nearby` (Mike, 2026-09-06: "fake a radar screen looking for aircraft, text Searching").

- **Face:** three concentric range rings (radii 36, 72, 108) in `DIM_DIM`, crosshair lines through the centre in `DIM_DIM`, a 4 px home dot at the centre in `NET_ON`.
- **Sweep:** a radius line from the centre to r 108 in `NET_ON`, rotating clockwise one revolution every 3 s, with two trailing wedges behind it (12° each, `fillArc` between r 0 and 108) in `SPEED_DIM` then `DIM_DIM` to fake persistence. Drawn on the palette canvas — no new palette entries.
- **Text:** `Searching` in `Font2`, `DIM`, centred at y 196 (below the outer ring's bottom chord); the page-dots row stays empty; the stale dot rule is unchanged (a stale list with count 0 still shows the radar plus the stale dot).
- **Timing:** the sweep phase is `(nowMs / 100) % 30` (12° per step, 10 Hz). `FrameKey` carries `radarPhase` only while this state is visible, so the card redraws at 10 Hz then and never otherwise. No heap, no `delay`; the phase derives from `nowMs`, so it is wrap-safe.
- **Untouched:** `offline` (`waiting for HA`) and populated states; auto-show; `FlightsService`.

## 4.14 Remote diagnostics log over MQTT (added 2026-09-06, approved)

The Dial lives where USB is impractical (Mike: "OTA only"), and the recurring BLE kick cycles (4, 5 and 6 Sep) cannot be diagnosed from Home Assistant, which only sees successful connects. The Dial therefore forwards its own log lines over MQTT.

- **Capture:** build with `-DUSE_ESP_IDF_LOG` so Arduino's `log_e/w/i` route through ESP-IDF's logger, and install `esp_log_set_vprintf()` (`RemoteLog::begin()`, called first in `setup()`). The hook still calls the previous vprintf (serial output unchanged), formats the line into a fixed 120-byte buffer and pushes it onto a FreeRTOS queue (depth 24, zero wait; a dropped-line counter is reported in the next forwarded line). It runs on whichever task logged (NimBLE host task included) and must not allocate or block.
- **Filter:** levels E and W are always forwarded; I lines only when the text contains one of `connect`, `Connect`, `kick`, `Kick`, `Subscribed`, `BLE`, `Net status`, `boot`. D/V never. This keeps the volume to a few lines per event rather than the 15 s heap heartbeat.
- **Publish:** `NetTask::run()` drains up to 4 lines per loop when MQTT is up and publishes each to `pacekeeper-dial/diag` (QoS 0, not retained). Lines queued while MQTT is down are kept until the queue fills, so the boot-time BLE story survives until the link comes up. On each MQTT connect the Dial also publishes `pacekeeper-dial/diag/boot` (retained) = `{"reset":"<esp_reset_reason name>","build":"<__DATE__ __TIME__>","uptime_s":N,"heap":N}`.
- **Reading:** `doc/DIAGNOSTICS.md` documents a one-line `mosquitto_sub` and a paho snippet; the retained boot record shows whether the Dial crashed (reset reason) versus rebooted for OTA.
- **Untouched:** BLE behaviour (`TreadmillHandler` only gains no code — its existing `log_*` calls are what get forwarded); Q1_BLE_NOTES.

## 4.15 BLE connect scheduling around WiFi bring-up (added 2026-09-06, approved)

Evidence from the remote log (§4.14), boot of 2026-09-06 11:27: MQTT came up at 4.4 s and immediately published its discovery/resync burst; the first BLE connect began at 5.6 s; `getService()` failed 5 times at 200 ms spacing and at 6.6 s the firmware called `disconnect()` (HCI 0x16), which the code already documents as sending the Q1 into a multi-cycle "kicking phase". The second attempt at 8.1 s, with WiFi quiet, succeeded first time. Every recorded kick cycle (4, 5 and 6 Sep) followed a reboot or a network event, i.e. a moment of heavy WiFi traffic on the shared antenna.

- **Connect hold:** `TreadmillHandler::setConnectHold(bool)`; while held, `handle()` makes no connect attempts unless the connect was user-requested (`m_userRequestedConnect`). `main.cpp` derives the hold from the net status it already tracks: held while status is `WIFI_CONNECTING` or `MQTT_CONNECTING` **during the first 30 s after boot**, and for `kPostMqttHoldMs = 6000` after every transition into `MQTT_UP` (the resync burst). Never held when WiFi is simply down (standalone use) or after the boot window, so a Dial without a network still connects within seconds.
- **Service discovery patience** *(superseded 2026-09-06 by §4.17 — discovery now runs only after the link is up, guarded by 3 × 100 ms)*: the getService retry loop becomes 8 × 250 ms (2 s) instead of 5 × 200 ms. Rationale: the belt did not kick us during the 1 s we spent retrying; our own disconnect is what hurt. Everything else in `connectToDevice()` and the Q1 protocol handling is unchanged.
- **Visibility:** the RemoteLog INFO filter also forwards every line whose tag is `Treadmill` (so the full connect sequence — service found, characteristic, keepalive, subscribed — and any future kick phase are visible over MQTT).
- **Success criteria (Phase I):** on an OTA reboot with the belt on, the log shows the connect starting ≥ 6 s after `Net status 4`, `Service found on attempt 1`, `Subscribed to notifications`, and no kick within the next 10 minutes; a Dial booted with WiFi off connects within 15 s.

## 4.16 Crash forensics: last log lines across a reset (added 2026-09-06, approved)

The first retained boot record (§4.14) showed the Dial had reset with `TASK_WDT` — the 300 s loop-task watchdog — so the main loop had frozen for five minutes; the freeze itself left no trace. That is also what every "kick cycle then silence" episode looked like from Home Assistant, which has no availability topic for the Dial and so cannot tell a hung Dial from a quiet one.

- **RTC ring:** `RemoteLog` keeps the last `kLastLines = 8` forwarded lines (E/W and the filtered I lines, same 120-byte cap) in an `RTC_NOINIT_ATTR` ring with a magic word and a simple checksum, written in the hook after the line passes the filter. RTC slow memory survives every reset except power-on, so the ring shows what the firmware was doing just before a watchdog or panic.
- **Boot publish:** on the first MQTT connect after boot, when `esp_reset_reason()` is anything other than `POWERON` or `SW`, `NetTask` publishes the ring oldest-first, one line each, to `pacekeeper-dial/diag/last` (QoS 0, **not** retained) prefixed `[last-N] ` where N counts back from the reset, then clears the ring. The retained `diag/boot` record gains `"last_lines": N`. On `POWERON`/`SW` the ring is cleared silently (an OTA reboot is not a crash).
- **Watchdog bracket:** the two blocking calls the loop task makes into NimBLE are bracketed by `log_i` lines so the ring's tail names the culprit: `connect() begin`/`end (ok|fail, N ms)` around `m_pClient->connect(...)` and `discover begin`/`end` around the getService loop. No behaviour change.
- **Loop stall detector:** `main.cpp` logs `log_w("loop stall %u ms")` whenever a single `loop()` iteration took longer than 2 s (measured with `millis()`), so a stall shorter than the 300 s watchdog still leaves evidence.

**Amended 2026-09-06 (as built):** `begin()` snapshots a valid RTC ring into RAM before the hook can overwrite it (the boot itself logs eight lines within seconds). `diag/last` is published as ONE retained JSON object `{"reset","build","lines":[...]}` (oldest first), only when the reset was a crash, so it can be read at any time; clean boots leave the previous record in place. The loop-stall threshold is 3 s (the 2 s GATT discovery loop is legitimate). The Plan 11 boot-hold window is latched rather than compared to `millis()`.

## 4.17 Staged BLE connect (added 2026-09-06, approved)

Diagnosis from the §4.16 brackets: `m_pClient->connect(m_targetAddress, false, true)` returns in 0 ms because NimBLE-Arduino 2.x's third parameter is `asyncConnect`. `connectToDevice()` then ran GATT discovery against a link that did not exist yet; `Connected to device!` arrived ~900 ms later, so discovery succeeded only when that landed inside the retry window (attempt 4–5), and otherwise the firmware called `disconnect()` on a pending connection — the HCI 0x16 that starts the Q1's kick phase. The boot-time failures, the kick cycles and the 1–2 s loop-task stalls all follow from this.

- **Stages** (all on the loop task, driven from `handle()`; the NimBLE callbacks only set flags):
  1. `LINKING`: issue `connect(addr, deleteAttributes=false, asyncConnect=true)`, record `m_linkStartMs`, return. Nothing blocks.
  2. `onConnect()` (NimBLE task) sets `m_linkUp = true` (it already sets `m_sendInitNow`).
  3. `handle()` sees `m_linkUp` → `SETUP`: the existing sequence — `getService` (now expected on attempt 1; keep at most 3 retries × 100 ms as a guard), write characteristic, early keepalive, notify subscribe, negotiated-params log, `m_lastConnectTime`, `Connection successful` — exactly the current code from that point on, including the existing failure paths (which may still `disconnect()`, because by then the link is real).
  4. If `LINKING` lasts longer than `kLinkTimeoutMs = 6000` (the supervision timeout we request): `cancelConnect()`, `log_w("link timeout, cancelled")`, and schedule the next attempt through the existing 5 s retry path. **Never** `disconnect()` a link that never came up.
- **Unchanged:** keepalive-per-notification, notification parsing, idle/kick backoffs, `POST_CONNECT_COOLDOWN`, the connect hold (§4.15), user-requested connect semantics, `doc/Q1_BLE_NOTES.md` protocol. `connectAttempts` counts LINKING starts as before.
- **Observability:** brackets become `link begin` / `link up after N ms` / `link timeout after N ms` / `setup begin` / `setup end`; the stall detector should now stay silent during connects.
- **Success (Phase J):** boot log shows `link up after <1500 ms` then `Service found on attempt 1`, `Connection successful`; no `loop stall`; belt power-cycled while connected → reconnect without a kick phase; belt switched off → `link timeout after 6000 ms` every ~11 s with the UI still responsive (knob/menu), no `disconnect()`.
- **As built (2026-09-06, post-review):** the link comes up ~100 ms before `onConnect()`, so every write path (init, heartbeat, pending command) is gated on `m_pWriteCharacteristic`, not just `isConnected()`. A heartbeat write that fails while the link layer is still up (a zombie) no longer calls `disconnect()`; it stops keepalives so the supervision timeout drops the link (the lighter kick path), **bounded** by an escalation deadline of 2× the negotiated supervision timeout, clamped to 12–30 s, after which `handle()` forces `disconnect()`. A user Connect on such a muted link forces the drop immediately and re-links. Failed writes do not advance the keepalive sequence number.

## 4.18 Lamp Colour page: hue wheel (added 2026-09-06, approved)

Replaces the eight preset swatches of §4.12 with one continuous hue ring (Mike, 2026-09-06: "could we do a wheel of colours rather than swatches"). Mockup: https://claude.ai/code/artifact/387401ff-ac83-43bd-a5cb-0868e59771be. Everything else on the Lamp card — pages, swipe, 10 s page idle, settle/confirm, bare Colour page, payload — is unchanged.

- **Ring geometry** (`LightLayout`): annulus centred on (120,120), outer radius 112, inner radius 86 (26 px band); hue 0 at 12 o'clock increasing clockwise, so hue *h* sits at angle *h* − 90° in screen terms. Drawn as 72 five-degree `fillArc` segments, each in the RGB565 of `hsv(h, 100, 100)` for the segment's start hue. The marker is a disc of radius 11 centred on the ring's mid-radius (99) at the selected hue, filled with that hue and outlined 3 px white, amber while the edit is settling or awaiting confirmation. The centre disc (radius 30) keeps showing the selected colour as today. The power glyph, card name and page dots stay off this page (§4.12 amendment 2).
- **Touch:** the hit region is the annulus from radius 70 to 120 (finger slop on both sides). A tap at (x,y) inside it selects `hueAt(x,y)` = `atan2` angle from 12 o'clock clockwise, in [0,360). Taps elsewhere on the page do nothing (tap-anywhere-on is only for the off face).
- **Knob:** ±`HUE_STEP` = 5° per detent, wrapping at 360 (72 detents per revolution). Same 300 ms settle as every page: a fast turn sends one command.
- **State** (`LightCardState`): `selectPreset(uint8_t)` and `preset()` are replaced by `selectHue(float hue, uint32_t nowMs)` and `editHue()`. `editHue()` is the hue the marker shows: the pending/held edit hue while a HUE edit is settling or awaiting confirmation, otherwise HA's reported hue (`view().hue`) — no snapping, so a colour set from HA or the Hue app is shown faithfully. The first detent on the Colour page steps from `editHue()`. `nearestPreset`, `kPresetHues`, `kPresetCount`, `swatchCentre`, `hitSwatch`, `kSwatch*` are removed.
- **Command:** unchanged shape, `{"state":"ON","hs_color":[round(hue),100]}` — HA's light-commands automation is untouched. `CONFIRM_HUE_TOL` stays 2°; the Hue bulb reports hue 258 for a commanded 275 in xy mode, so confirmation may time out at 1.5 s exactly as it does today and the marker then follows HA's value — accepted, the marker simply lands where the bulb says it is.
- **Drawing** (`DialLightsView`): the palette canvas reserves the ring annulus, the marker disc (radius 11 + 3 px outline) and the centre disc as `TRANSPARENT`; `paintLightTrueColour()` then paints, directly on the display after the push and in this order, the 72 segments, the centre disc, the marker fill and its outline (white `0xFFFF` or the amber RGB565 of `Col::PENDING`). The outline must be painted here, not on the canvas, because the segments painted afterwards would cover a canvas outline. When colour is not live (`!colourLive()`), the canvas draws the ring's two boundary circles in `DIM_DIM`, the marker as a `DIM` outline at `editHue()`, the colour-dots glyph and the existing "not active - turn to use" caption; nothing is reserved. The direct-to-display fallback (`useCanvas == false`) paints the true colours inline as today.
- **Frame key:** `lightPreset` becomes `lightHue` (rounded degrees, uint16_t) so a detent or tap redraws; the ring itself is static between frames.
- **Cost:** 72 small arcs plus three discs per Colour-page frame, only when the frame key changes, at most 10 Hz — well under the budget the swatches used; no new heap.
- **Tests (native):** `LightLayout` — `hueAt` at the four cardinal points (12 o'clock → 0, 3 → 90, 6 → 180, 9 → 270) and a diagonal, `hitHueRing` inside/outside both radii, marker centre for hue 0 and 90; `LightCardState` — `selectHue` arms a settle and emits `hs_color [h,100]`, detents wrap both ways (355 + 5 → 0, 0 − 5 → 355), `editHue()` shows the pending hue while settling and HA's hue after confirmation or hold expiry, a tap or detent on a non-colour card or another page is ignored as before. The swatch/glyph overlap sweep test is deleted with the swatches.
- **Success (Phase K, Mike on hardware):** tapping the ring at 3 o'clock sends hue 90 and the lamp goes green; three clockwise detents from there send 105; the marker sits on the lamp's live hue after an HA-side change; the ring dims with the caption when the lamp is on a colour temperature and a tap brings colour back; Office (no colour) is unaffected.
