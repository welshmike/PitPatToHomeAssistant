# Plan 6: Lights cards (Office + Lamp over HA/MQTT)

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Checkbox steps.

**Goal:** spec §4.10. Two desk cards (Office, Lamp) with on-screen `Power` / `Bright` / `Colour` buttons; knob adjusts the engaged value with a 300 ms settle; state mirrored from Home Assistant over retained MQTT; commands published to HA. The two HA automations already exist (created 2026-09-04 via the HA API): `automation.pacekeeper_dial_light_commands` and `automation.pacekeeper_dial_light_state_publish`.

**Verification for every task:** `pio test -e native`, `pio run -e devkit-usb`, `pio run -e dial-usb`; no new `src/` warnings. Commit per task.

**Constraints:** `doc/Q1_BLE_NOTES.md` behaviour unchanged; `TreadmillHandler` untouched. Loop task never touches sockets; net task never touches the display. `DialUi`: no `delay`, no per-frame heap, wrap-safe time, palette colours only via `col(Col)` (see `DialUi.h`; `thickLine()` not `drawWideLine`). Pure logic is Arduino-free with Unity tests under `test/test_*/` and listed in the `[env:native]` `build_src_filter`. Everything Dial-only sits behind `HAS_DIAL_UI` so `devkit-usb` still builds. Note `pio` is at `~/Library/Python/3.9/bin/pio`.

**MQTT contract (fixed — HA side is already live):** see spec §4.10. Set topic `pacekeeper-dial/light/{office|lamp}/set`; retained state `pacekeeper-dial/light/{office|lamp}/state`; refresh `pacekeeper-dial/light/refresh`.

---

## Task 1: `DialInput` tap position + `LightsModel` (pure, TDD)

**Files:** `src/DialInput.h/.cpp`, `test/test_dial_input/test_main.cpp`; new `src/LightsModel.h/.cpp`, new `test/test_lights_model/test_main.cpp`; `platformio.ini` native `build_src_filter` (+`LightsModel.cpp`).

- [ ] `DialEvents` gains `int tapX = 0, tapY = 0;` — the touch-**down** position of the gesture that produced `tap = true` (already tracked as `m_touchStartX/Y`). Test: press at (56,186), release within 600 ms and 20 px → `tap` with `tapX == 56, tapY == 186`; a drag that becomes a swipe reports no tap.
- [ ] `LightsModel.h` (POD, portable):
  ```cpp
  namespace LightsModel {
  enum class ColorMode : uint8_t { NONE, TEMP, HS };
  struct LightState { bool valid=false; bool available=false; bool on=false; uint8_t brightnessPct=0;
                      ColorMode mode=ColorMode::NONE; uint16_t kelvin=0; uint16_t minKelvin=2000, maxKelvin=6500;
                      float hue=0, sat=100; bool supportsColor=false; };
  struct LightsSnapshot { LightState office, lamp; };
  enum class LightKey : uint8_t { OFFICE, LAMP, COUNT };
  const char* keyName(LightKey k);            // "office" / "lamp"
  bool keyFromTopic(const char* topic, LightKey& out); // matches pacekeeper-dial/light/{key}/state
  bool parseLightState(const char* json, size_t len, LightState& out); // ArduinoJson, doc <= 512 B
  struct Command { enum class Type : uint8_t { NONE, POWER, BRIGHT, TEMP, HUE } type=Type::NONE;
                   bool on=false; uint8_t pct=0; uint16_t kelvin=0; float hue=0, sat=100; };
  size_t formatCommand(const Command& c, char* buf, size_t cap); // snprintf JSON per spec; 0 on NONE/overflow
  }
  ```
  `parseLightState`: `state` `"on"`/`"off"` → `available=true`; `"unavailable"`/`"unknown"`/missing → `available=false`, `valid=true`. `color_mode` `"color_temp"` → TEMP; `"xy"`/`"hs"`/`"rgb*"` → HS; null → NONE. `hs_color` `[h,s]` → hue/sat when present. Clamp pct 0–100. Missing min/max keep defaults. Malformed JSON → false.
  `formatCommand`: POWER → `{"state":"ON"}` / `{"state":"OFF"}`; BRIGHT → `{"state":"ON","brightness_pct":N}`; TEMP → `{"state":"ON","color_temp_kelvin":K}`; HUE → `{"state":"ON","hs_color":[H,S]}` with integers (hue rounded, sat rounded).
- [ ] Tests: the two real HA payloads recorded 2026-09-04 (add to `test/fixtures/fixtures.h` as raw strings): office off → `available && !on && brightnessPct==0 && minKelvin==2202 && maxKelvin==4000 && !supportsColor`; lamp → `supportsColor && minKelvin==2000 && maxKelvin==6535`; an on/colour_temp payload (`{"state":"on","brightness_pct":65,"color_mode":"color_temp","color_temp_kelvin":2700,...}`) → mode TEMP, kelvin 2700; an hs payload → mode HS, hue/sat; `"unavailable"` → `!available`; each `formatCommand` variant byte-exact; `keyFromTopic` for both topics and a non-matching topic.
- [ ] Commit: `Add DialInput tap position and LightsModel (HA light state parse, command format) with tests`.

## Task 2: `LightCardState` + `LightButtons` (pure, TDD)

**Files:** new `src/LightButtons.h` (header-only), new `src/LightCardState.h/.cpp`, new `test/test_light_card_state/test_main.cpp`, `platformio.ini` native filter.

- [ ] `LightButtons.h`: `enum class Button : uint8_t { NONE, POWER, BRIGHT, COLOUR };` `struct Geom { int cx, cy, r; };` `Geom geom(Button b, bool hasColour)` — Lamp (hasColour): POWER (56,186) BRIGHT (120,206) COLOUR (184,186); Office: POWER (84,200) BRIGHT (156,200); r = 22. `Button hitTest(int x, int y, bool hasColour)` — inside `r + 6`; COLOUR never returned when `!hasColour`. `const char* label(Button)` → `Power`/`Bright`/`Colour`.
- [ ] `LightCardState` (one per card, constructed with `bool hasColour`):
  ```cpp
  enum class Engaged : uint8_t { NONE, BRIGHT, TEMP, HUE };
  static constexpr uint32_t SETTLE_MS = 300, IDLE_RELEASE_MS = 10000;
  static constexpr int BRIGHT_STEP = 5, KELVIN_STEP = 100, HUE_STEP = 10;
  void sync(const LightsModel::LightState& s);            // adopt HA state; skip the engaged field while settling
  LightsModel::Command tapButton(Button b, uint32_t nowMs); // may return a POWER command immediately
  void detents(int n, uint32_t nowMs);                      // only meaningful while engaged; arms the settle
  LightsModel::Command tick(uint32_t nowMs);                // settle elapsed -> BRIGHT/TEMP/HUE command; idle -> release
  Engaged engaged() const; bool settling() const;
  const LightsModel::LightState& view() const;              // local (optimistic) state for drawing
  float ringFraction() const;                               // 0..1 of the engaged value in its range (brightness when NONE)
  ```
  Rules (spec §4.10): POWER toggles `on` optimistically, releases engagement, returns the command. BRIGHT: engage ↔ release. COLOUR (only when hasColour and colour supported): NONE → TEMP if `mode != HS` else HUE; TEMP → HUE; HUE → NONE. Any tap/detent refreshes the idle timer. Detents: BRIGHT ±5 clamp 1–100; TEMP ±100 clamp [minKelvin, maxKelvin]; HUE ±10 wrap 0–360; each detent sets `on = true` locally and (re)arms the settle deadline. `tick`: when settling and `now - lastDetent >= 300` → emit one command for the engaged field and clear settling; when engaged and `now - lastInput >= 10000` → release (no command). `sync` while settling keeps the local engaged field, adopts everything else. Buttons BRIGHT/COLOUR are ignored when `!view().available`; POWER is allowed when `valid` (blind switch-on), ignored when `!valid`.
- [ ] Tests: hit-test centres/edges/margin for both layouts; COLOUR ignored on Office; brightness detents clamp; kelvin clamp to the light's min/max after `sync`; hue wrap 350+20 → 10; settle emits exactly one command 300 ms after the *last* detent and none before; idle release at 10 s; POWER from off returns `{"state":"ON"}`-equivalent command and sets `on`; COLOUR cycle TEMP→HUE→NONE; `sync` during settle doesn't clobber the engaged value but does clobber it once settled; ringFraction for brightness 50 → 0.5, kelvin mid-range → 0.5.
- [ ] Commit: `Add LightCardState and LightButtons (pure card interaction, hit-testing) with tests`.

## Task 3: `LightsService` on the net task + publish path

**Files:** new `src/LightsService.h/.cpp` (Dial-only, `HAS_DIAL_UI`), `src/Commands.h`, `src/NetTask.h/.cpp`, `src/main.cpp` (nothing yet beyond compiling).

- [ ] `Commands.h`: `PubType::LIGHT_CMD`; `PublishItem` gains `uint8_t lightKey = 0; char lightJson[96] = {0};` (stays trivially copyable).
- [ ] `LightsService` (mirrors `FlightsService`'s threading contract): `Guarded<LightsModel::LightsSnapshot> m_snapshot`; net-task-only `void onStateMessage(const char* topic, const uint8_t* payload, size_t len)` (uses `keyFromTopic` + `parseLightState`, logs at `log_i` on each state, `log_w` on parse failure); any-task `LightsSnapshot snapshot() const`. Topic strings as constants: `kSetTopicFmt "pacekeeper-dial/light/%s/set"`, `kStateTopicFmt "pacekeeper-dial/light/%s/state"`, `kRefreshTopic "pacekeeper-dial/light/refresh"`.
- [ ] `NetTask`: member `LightsService m_lights` + `LightsService& lights()` under `HAS_DIAL_UI`. `onMqttConnected()`: subscribe both state topics (QoS 0), then publish `1` to the refresh topic (after the existing subscriptions). `onMqttMessage()`: if the topic matches a light state topic → `m_lights.onStateMessage(...)` and return (before the existing chain). `drainPublishQueue()`: `LIGHT_CMD` → `client.publish(setTopic(lightKey), lightJson)` (not retained); log at `log_i`.
- [ ] Commit: `Add LightsService: HA light state over MQTT on the net task, LIGHT_CMD publish path`.

## Task 4: Lights cards in `DialUi`

**Files:** `src/CardRing.h` (+ `LIGHT_OFFICE`, `LIGHT_LAMP` after FLIGHTS), `test/test_card_ring/test_main.cpp` (ring order), `src/DialUi.h/.cpp`, `src/main.cpp` (pass `netTask.lights()` to `DialUi`).

- [ ] `DialUi` gets `LightsService&`; members `LightCardState m_lightCards[2]` (office `hasColour=false`, lamp `true`), `LightsModel::LightsSnapshot m_lightsSnap`, `m_lastLightsSnapMs` (poll ≤ every 250 ms while a light screen shows; `sync()` both cards from it). Screens `LIGHT_OFFICE`, `LIGHT_LAMP` in `currentScreen()` from the card ring; `tickLights(nowMs)` runs `tick()` on the visible card and enqueues any command via a new `void publishLightCommand(LightKey, const Command&)` (formats with `formatCommand`, fills `PublishItem`, calls `NetTask::enqueuePublish` — pass `NetTask&` or a small callback; **`DialUi` must not include PubSubClient**).
- [ ] Input on a light screen: `ev.tap` → `LightButtons::hitTest(ev.tapX, ev.tapY, hasColour)`; `Button::NONE` → no-op, no beep; else `tapButton()`; `playAcceptBeep(true)` when the tap did something, `(false)` when refused (unavailable). Detents: if `engaged() != NONE` → `detents(n)`, else scroll the card ring as on the other desk cards. Long press: no-op. Side button: existing home behaviour. Any `POWER` command returned by `tapButton()` is published immediately.
- [ ] `drawLight(gfx, LightKey)` per spec §4.10 layout: title `Font4` y 40; value `Font7` (`65%`) or `OFF`/`no data`/`waiting for HA` (`waiting for HA` when `m_netStatus != MQTT_UP`-equivalent — check `NetStatus` values); caption `Font2` (`2700K` / `hue 210`); 270° ring (reuse the speed-ring arc helper; gap at the bottom) with `ringFraction()`, PENDING while `engaged()!=NONE`, SPEED otherwise, DIM track; three/two buttons via `LightButtons::geom()` — engaged filled PENDING + BG label, others `drawCircle` DIM + TEXT label, inert ones DIM_DIM. Labels in `Font2` centred at `cy + r + 12` **but clipped to the round display** — if `cy + r + 12 > 226`, draw the label inside the circle instead (check both layouts on hardware).
- [ ] `FrameKey`: add `uint16_t lightHash` (FNV-1a of key, valid, available, on, pct, kelvin, hue rounded, engaged, settling) and `bool lightMqttUp`.
- [ ] Commit: `Lights cards: Office and Lamp with on-screen Power/Bright/Colour buttons and knob adjust`.

## Task 5: Docs and Phase F checklist

**Files:** `README.md`, `doc/AUDIT_2026-09-03.md`, new `doc/HA_LIGHTS_AUTOMATIONS.md`, spec §4.10 "as built" amendment if anything deviated.

- [ ] `doc/HA_LIGHTS_AUTOMATIONS.md`: the two automations' YAML as created 2026-09-04 (read back with the HA API or from the UI), the MQTT contract, and how to add a third light (new key → automation + `LightKey`).
- [ ] README Desk mode: Lights cards, buttons, knob behaviour, the MQTT topics, HA automation pointer. Phase F checklist: retained state arrives at boot (card shows real state without touching anything); `Power` toggles each light within ~1 s and the card follows HA; `Bright` + knob changes brightness with a single command per settle (watch HA traces: one run per knob burst); Lamp `Colour` cycles temp → hue → release, kelvin clamps at 2000/6535; 10 s auto-release; knob scrolls cards when nothing is engaged; unplug WiFi → `waiting for HA`; heap stays ≥ 90 KB free at rest (`Heap:` log); belt unaffected.
- [ ] Commit: `Docs: lights cards and HA automations`.
