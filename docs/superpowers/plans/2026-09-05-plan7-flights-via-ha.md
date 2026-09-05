# Plan 7: Flights via Home Assistant

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Checkbox steps.

**Goal:** spec §4.11. Home Assistant (Flightradar24 HACS integration) owns all flight data and logo caching; the Dial subscribes to one retained MQTT JSON, fetches logos over plain HTTP from HA, and auto-shows the Flights card over the Clock while aircraft are nearby (configurable HA switch, default on). All TLS code leaves the firmware.

**Verification for every task:** `pio test -e native`, `pio run -e devkit-usb`, `pio run -e dial-usb`; no new `src/` warnings. Commit per task. `pio` is at `~/Library/Python/3.9/bin/pio`.

**Constraints:** `doc/Q1_BLE_NOTES.md` behaviour unchanged; `TreadmillHandler`'s BLE path untouched (it only gains one persisted setting, like Start Speed). Loop task never touches sockets; net task never touches the display. `DialUi`: no `delay`, no per-frame heap, wrap-safe time, colours via `col(Col)`. Pure logic Arduino-free with Unity tests, listed in `[env:native]` `build_src_filter`. Dial-only code behind `HAS_DIAL_UI`. MQTT receive callback parses + stores only. The card layout (logo 120×48 at (60,16), `callsign - type`, route, `alt ft - gs kt`, `x.x mi NE`, page dots, stale dot) is unchanged.

**MQTT contract (HA side is Task 4, done by the controller through the HA API):**
- HA → Dial, retained: `pacekeeper-dial/flights/state` = `{"ts":<epoch s>,"ac":[{"cs":"BAW123","fl":"BA123","ty":"A320","al":"BA","an":"British Airways","fr":"LHR","to":"JFK","alt":12000,"gs":450,"di":2.3,"br":135,"gnd":0}, ...]}` nearest first, ≤ 6 entries, `an` ≤ 24 chars, missing strings `""`, missing numbers `0`.
- Dial → HA: `pacekeeper-dial/flights/refresh` = `1` after each MQTT connect.
- Logos: `GET <FLIGHTS_LOGO_BASE_URL>/logos/<IATA>.png` (default base `http://<MQTT_SERVER>:8123/local`), plain HTTP, `image/png`, ≤ 8 KB; 404 = no logo.

---

## Task 1: Pure model — `parseDialFlights`, `FlightsAutoShow`, remove dead parsers (TDD)

**Files:** `src/FlightsModel.h/.cpp`, `test/test_flights_model/test_main.cpp`, `test/fixtures/` (+ `dial_flights.json` and regenerate `fixtures.h` with the existing `generate_fixtures_h.py`), new `src/FlightsAutoShow.h/.cpp`, new `test/test_flights_auto_show/test_main.cpp`, `src/Geo.h/.cpp` + `test/test_geo/`, delete `src/AirlineCodes.h/.cpp` + `test/test_airline_codes/`, `platformio.ini` native filter.

- [ ] `bool FlightsModel::parseDialFlights(const char* json, size_t len, FlightsSnapshot& out)`: ArduinoJson (doc ≤ 2 KB); fills `count` (≤ 6), per aircraft `callsign←cs`, `type←ty`, `airlineIata←al`, `operatorName←an`, `fromIata←fr`, `toIata←to`, `altFt←alt`, `gsKt←gs`, `distMi←di`, `bearing←br` (normalised 0–359), `onGround←gnd` (new `bool onGround` field), `flightNumber←fl` (new `char flightNumber[9]`), `routeKnown = operatorKnown = true`; `hex` set to `""`; `lat/lon/track` 0. Returns false on malformed JSON or missing `ac`. `out.fetchedMs`, `stale`, `offline` left to the caller. Keep entries in payload order (HA sorts).
- [ ] Fixture `test/fixtures/dial_flights.json` = a hand-written payload matching the contract above with 3 aircraft (one with empty `fr`/`to`/`al`, one on ground) — replace with a recorded HA payload in Task 4 if it differs. Tests: 3 parsed, field-by-field for the first, empty strings preserved, `gnd` → `onGround`, 7-entry payload capped at 6, malformed → false, `{"ts":1,"ac":[]}` → count 0 and true.
- [ ] Remove `parseAdsbFi`, `parseHexdbRoute/Airport/Aircraft` and their tests and fixtures; remove `AirlineCodes` entirely; `Geo` keeps only `compass8` (drop `distanceMiles`/`bearingDeg` and their tests). Update `platformio.ini` `build_src_filter` (`-AirlineCodes.cpp`, `+FlightsAutoShow.cpp`).
- [ ] `FlightsAutoShow` (pure): `enum class Action { NONE, SHOW_FLIGHTS, RETURN_TO_CLOCK }`; `void setEnabled(bool)`; `Action update(uint8_t aircraftCount, CardId currentCard, bool beltIdle)`; `void noteManualNavigation()`. Rules: disabled → always NONE and clears any auto state. SHOW_FLIGHTS when enabled, `beltIdle`, `currentCard == CLOCK`, previous count == 0 and count > 0 → sets `m_autoShown = true`. RETURN_TO_CLOCK when `m_autoShown`, count == 0 and `currentCard == FLIGHTS` → clears `m_autoShown`. `noteManualNavigation()` clears `m_autoShown` (user took over; no return). If `currentCard` is anything but CLOCK/FLIGHTS while `m_autoShown`, clear it (user left by other means). Tests: show on 0→1 from CLOCK; no show from LIGHT_OFFICE/TREADMILL; no show while belt not idle; return on →0 only when auto-shown; manual nav cancels return; disable mid-show → NONE and no later return; re-enable requires a fresh 0→>0 edge; count 2→1 → NONE.
- [ ] Commit: `Flights: parseDialFlights and FlightsAutoShow (pure, tests); drop adsb.fi/hexdb parsers and AirlineCodes`.

## Task 2: `FlightsService` fed by MQTT, plain-HTTP logos, NetTask wiring

**Files:** `src/FlightsService.h/.cpp` (rewrite, expect ~300 lines instead of 940), `src/NetTask.h/.cpp`, `src/NetManager.cpp` (`kMqttBufferSize = 2048`), `src/config.h.example` (+`FLIGHTS_LOGO_BASE_URL`, remove `HOME_LAT/HOME_LON/FLIGHTS_RADIUS_MI`), `src/config.h` (local, gitignored: same edit).

- [ ] `FlightsService` keeps its public contract for `DialUi` (`setVisible`, `setWantedLogo`, `snapshot`, `logoReady`, `invalidateLogo`, `begin`) and gains net-task-only `void onStateMessage(const uint8_t* payload, size_t len, uint32_t nowMs)` (parse with `parseDialFlights` into a working copy, stamp `fetchedMs`, `stale=false`, write the `Guarded` snapshot; `log_i` count + heap; `log_w` on parse failure keeping the old list) and `static constexpr const char* kStateTopic/kRefreshTopic`. `tick(nowMs)`: `offline = !m_net.mqttUp()`; `stale = mqttUp && (nowMs - lastMessageMs) > 120000` (no fetching of aircraft any more); logo work only while visible: same LittleFS cache/validation/negative cache as today, but the download is a plain `WiFiClient` HTTP/1.0 GET to `FLIGHTS_LOGO_BASE_URL "/logos/XX.png"` with 3 s connect/read timeouts, `Content-Type` check, ≤ 8 KB into the existing lazily-allocated buffer (keep `manageHttpBuf`). Delete: adsb.fi fetch, hexdb enrichment and its caches, `WiFiClientSecure`, `httpsGet`, the 1.5 s spacing guard, heap guards, `tickBudgetExceeded`, `isDefinitiveMiss`, the `HOME_*` fallbacks/warnings. Keep the class comment accurate.
- [ ] `NetTask::onMqttConnected()`: subscribe `FlightsService::kStateTopic` (QoS 0) and publish `1` to `kRefreshTopic` (next to the lights refresh). `onMqttMessage()`: if `strcmp(topic, kStateTopic) == 0` → `m_flights.onStateMessage(payload, length, millis())`, return.
- [ ] `kMqttBufferSize = 2048` with a comment on why. `config.h.example`: `#define FLIGHTS_LOGO_BASE_URL "http://" MQTT_SERVER ":8123/local"` documented; remove the `HOME_*` block from both example and local `config.h`.
- [ ] Commit: `FlightsService: MQTT-fed snapshot from HA, plain-HTTP logo cache; drop TLS and enrichment`.

## Task 3: Auto-show setting end to end

**Files:** `src/TreadmillHandler.h/.cpp` (persisted `flightsAutoShow` next to start speed — NVS key `fl_auto`, default true), `src/Commands.h` (`CmdType::SET_FLIGHTS_AUTO_SHOW`, `PubType::FLIGHTS_AUTO_SHOW`), `src/MqttView.h/.cpp` (`MqttSwitch m_flightsAutoShow("flights-auto-show", "Flights Auto-show")`, `EntityCategory::CONFIG`, icon `mdi:airplane-clock`, `publishFlightsAutoShowSetting(bool)`, discovery + full resync), `src/NetTask.cpp` (subscribe command topic, parse ON/OFF → command, publish echo), `src/main.cpp` (drain → `treadmill.setFlightsAutoShow(b)`, `dialUi.setFlightsAutoShow(b)`, echo; boot → `dialUi.setFlightsAutoShow(treadmill.getFlightsAutoShow())`), `src/DialUi.h/.cpp`.

- [ ] `DialUi`: member `FlightsAutoShow m_autoShow`; `void setFlightsAutoShow(bool)`. In `tick()` (every call, belt idle or not): read the flights snapshot count at the existing 250 ms cadence even when the Flights card is not showing (move the snapshot poll out of `tickFlights` into a small `pollFlights(nowMs)` that runs always; `tickFlights` keeps the idx clamp / wanted-logo work for the visible card), then `m_autoShow.update(count, m_cards.current(), beltIdle)` where `beltIdle` = the screen resolves to a card (not CONNECTING/STARTING/RUNNING/SELECTOR); SHOW_FLIGHTS → `m_cards.set(CardId::FLIGHTS)`, `m_flightIdx = 0`, `m_input.noteActivity(nowMs)` (wake the backlight so the card is visible); RETURN_TO_CLOCK → `m_cards.set(CardId::CLOCK)`. Knob scroll and side-button home call `m_autoShow.noteManualNavigation()`. `FlightsService::setVisible` stays driven by the resolved screen.
- [ ] Commit: `Flights auto-show: HA switch + NVS setting, DialUi switches Clock <-> Flights`.

## Task 4: Home Assistant side (controller, via the HA API — not a subagent)

- [ ] After Mike restarts HA: configure the Flightradar24 integration (lat 51.566645, lon 0.005586, radius 5 km); configure `downloader` (download_dir `www`).
- [ ] Trigger-based template sensor `sensor.pacekeeper_dial_logos_cached` (attribute `logos`: list) fed by the logos automation; automation `PaceKeeper Dial: airline logos` (event `flightradar24_entry`, condition IATA non-empty and not in list, action `downloader.download_file` url `https://pics.avs.io/120/48/{{ iata }}.png`, `subdir: logos`, `filename: {{ iata }}.png`, `overwrite: true`); automation `PaceKeeper Dial: flights publish` per spec §4.11 (distance in statute miles and bearing computed from lat/lon; ≤ 6; compact keys; retained).
- [ ] Verify with automation traces; record one real payload into `test/fixtures/dial_flights.json` if it differs from the hand-written one and re-run native tests. Flash the Dial, confirm `FlightsService` logs a count, logos load over HTTP, heap at rest and during a logo fetch.

## Task 5: Docs and Phase G checklist

**Files:** `README.md`, `doc/HA_FLIGHTS.md` (new), `doc/AUDIT_2026-09-03.md`, spec §4.11 "as built" amendment if anything deviated, delete stale `HOME_*` mentions everywhere (`grep -rn HOME_LAT`).

- [ ] README Flights section rewritten: data from HA, the two topics, logo path, auto-show switch, what moved out of the firmware and why (heap/TLS/coex). `doc/HA_FLIGHTS.md`: integration install/config, template sensor + both automations as YAML read back from HA, MQTT contract, how to change radius (in HA now). Phase G checklist: retained flights payload at boot; card matches HA sensor; logo appears for a major airline within one refresh; unknown airline → text; auto-show interrupts Clock and returns; switch off in HA stops it; heap ≥ 100 KB free at rest and no TLS-sized dips; belt unaffected.
- [ ] Commit: `Docs: flights via Home Assistant`.
