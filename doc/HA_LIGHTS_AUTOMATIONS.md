# Home Assistant automations for the Dial's Lights cards

The Dial's two Lights cards (Office, Lamp — spec §4.10, rebuilt as swipeable pages in §4.12) never
talk to Home Assistant's REST/WebSocket API. They only publish and subscribe to plain MQTT topics;
two HA automations do the actual `light.turn_on`/`light.turn_off` calls and mirror state back. Plan
8 changed the Dial's on-screen UI (pages and knob-as-value instead of on-screen buttons) but not
this MQTT contract or either automation below — nothing here needs to change for it. Both were created
directly through the HA API on 2026-09-04 (`automation.pacekeeper_dial_light_commands` and
`automation.pacekeeper_dial_light_state_publish`); the YAML below is a readable copy of exactly
what's live, with the `id` HA assigns on creation dropped (re-importing this YAML gets a fresh
one).

## MQTT contract

| Direction | Topic | Payload | Notes |
| --- | --- | --- | --- |
| Dial → HA (command) | `pacekeeper-dial/light/{office\|lamp}/set` | `{"state":"ON"\|"OFF", "brightness_pct":0-100, "color_temp_kelvin":K, "hs_color":[h,s]}` | `state` required, every other key optional. Any non-`OFF` command turns the light on. Not retained. |
| HA → Dial (state) | `pacekeeper-dial/light/{office\|lamp}/state` | `{"state":"on"\|"off"\|"unavailable", "brightness_pct":0-100, "color_mode":"color_temp"\|"xy"\|"hs"\|null, "color_temp_kelvin":K\|null, "hs_color":[h,s]\|null, "min_kelvin":K, "max_kelvin":K, "supports_color":bool}` | Retained. Published on every state/attribute change, at HA start, and in reply to `refresh`. |
| Dial → HA (refresh) | `pacekeeper-dial/light/refresh` | `1` | Published once after each MQTT (re)connect, after subscribing to both state topics — asks HA to re-publish both retained state topics so the Dial doesn't have to wait for the next real change. |

`office` is `light.mikes_office_2` (colour-temperature only, 2202–4000 K); `lamp` is `light.lamp`
(colour temperature 2000–6535 K, plus hue/saturation colour).

### Why `brightness_pct` / `color_temp_kelvin`, not HA's native units

Home Assistant's own light attributes are `brightness` (0–255) and `color_temp_kelvin` is actually
fine, but HA's *older* `color_temp` attribute is in mireds, not kelvin, and plenty of automations/
dashboards still surface that one. The Dial's `LightsModel::parseLightState()`
(`src/LightsModel.cpp`) and the card's rendering (`DialUi::drawLight()`) work entirely in
percent brightness and kelvin — there's no reason for embedded, allocation-free JSON parsing code
to also carry a 0–255↔percent or mired↔kelvin conversion, so the state-publish automation below
does that conversion once, in Jinja, on the HA side. The Dial never sees a raw 0–255 brightness
byte or a mired value.

## Automation 1: `pacekeeper_dial_light_commands`

Listens on both `.../set` topics and applies the Dial's command to the matching light.

```yaml
alias: "PaceKeeper Dial: light commands"
description: >-
  Applies light commands published by the PaceKeeper Dial on
  pacekeeper-dial/light/{office|lamp}/set. Payload JSON: {"state":"ON"|"OFF",
  "brightness_pct":0-100, "color_temp_kelvin":K, "hs_color":[h,s]} - every key optional
  except state.
triggers:
  - trigger: mqtt
    topic: pacekeeper-dial/light/office/set
    id: office
  - trigger: mqtt
    topic: pacekeeper-dial/light/lamp/set
    id: lamp
actions:
  - choose:
      - conditions:
          - condition: trigger
            id: office
          - "{{ is_off }}"
        sequence:
          - action: light.turn_off
            target:
              entity_id: light.mikes_office_2
      - conditions:
          - condition: trigger
            id: office
        sequence:
          - action: light.turn_on
            target:
              entity_id: light.mikes_office_2
            data: "{{ turn_on_data }}"
      - conditions:
          - condition: trigger
            id: lamp
          - "{{ is_off }}"
        sequence:
          - action: light.turn_off
            target:
              entity_id: light.lamp
      - conditions:
          - condition: trigger
            id: lamp
        sequence:
          - action: light.turn_on
            target:
              entity_id: light.lamp
            data: "{{ turn_on_data }}"
            note: >-
              Any non-OFF command turns the light on and applies whatever optional fields were sent.
mode: queued
max: 10
variables:
  p: "{{ trigger.payload_json }}"
  is_off: "{{ p.state is defined and (p.state | upper) == 'OFF' }}"
  turn_on_data: >-
    {% set ns = namespace(d={}) %}{% if p.brightness_pct is defined %}{% set ns.d =
    dict(ns.d, brightness_pct=[[p.brightness_pct | int(0), 1] | max, 100] | min) %}{% endif
    %}{% if p.color_temp_kelvin is defined %}{% set ns.d = dict(ns.d,
    color_temp_kelvin=p.color_temp_kelvin | int) %}{% endif %}{% if p.hs_color is defined
    %}{% set ns.d = dict(ns.d, hs_color=p.hs_color) %}{% endif %}{{ ns.d }}
```

`turn_on_data` builds the `light.turn_on` service-call data from whatever optional keys the Dial
actually sent — a `BRIGHT` command carries only `brightness_pct`, a `TEMP` command only
`color_temp_kelvin`, a `HUE` command only `hs_color`, so the light's other attributes (whichever
one the Dial isn't currently adjusting) are left exactly as HA already has them.

## Automation 2: `pacekeeper_dial_light_state_publish`

Mirrors both lights' state to their retained topics whenever either changes, when HA restarts, and
whenever the Dial asks via `refresh`.

```yaml
alias: "PaceKeeper Dial: light state publish"
description: >-
  Mirrors the state of the two Dial-controlled lights to retained MQTT topics
  pacekeeper-dial/light/{office|lamp}/state as JSON, on any change, at HA start, and when
  the Dial asks on pacekeeper-dial/light/refresh.
triggers:
  - trigger: state
    entity_id:
      - light.mikes_office_2
      - light.lamp
    id: changed
    note: "No to/from so attribute-only changes (brightness, colour) also fire."
  - trigger: homeassistant
    event: start
    id: start
  - trigger: mqtt
    topic: pacekeeper-dial/light/refresh
    id: refresh
    note: "Dial publishes here after (re)connecting to MQTT to get a fresh snapshot."
actions:
  - repeat:
      for_each:
        - key: office
          entity: light.mikes_office_2
        - key: lamp
          entity: light.lamp
      sequence:
        - action: mqtt.publish
          data:
            topic: "pacekeeper-dial/light/{{ repeat.item.key }}/state"
            retain: true
            payload: >-
              {{ {'state': states(repeat.item.entity), 'brightness_pct':
              (((state_attr(repeat.item.entity, 'brightness') | int(0)) * 100) / 255) | round(0) |
              int, 'color_mode': state_attr(repeat.item.entity, 'color_mode'), 'color_temp_kelvin':
              state_attr(repeat.item.entity, 'color_temp_kelvin'), 'hs_color':
              state_attr(repeat.item.entity, 'hs_color'), 'min_kelvin': state_attr(repeat.item.entity,
              'min_color_temp_kelvin'), 'max_kelvin': state_attr(repeat.item.entity,
              'max_color_temp_kelvin'), 'supports_color': ('xy' in (state_attr(repeat.item.entity,
              'supported_color_modes') or []) or 'hs' in (state_attr(repeat.item.entity,
              'supported_color_modes') or []))} | to_json }}
mode: queued
max: 10
```

The trigger has no `to`/`from`, so an attribute-only change (brightness or colour moved without
the on/off state changing) still fires it — that's the case that matters most when the Dial's own
knob-driven command lands and HA's echo needs to reach the card quickly.

## Adding a third light

1. **Firmware:** add the new key to `LightsModel::LightKey` in `src/LightsModel.h` (before
   `COUNT`) and its lowercase topic-segment name in `LightsModel::keyName()`
   (`src/LightsModel.cpp`). Add a matching `CardId`/`Screen` pair (`src/CardRing.h` and the
   `Screen` enum in `src/DialUi.h`) in ring order, and wire it into `DialUi::currentScreen()`,
   `tickLights()`/`lightCardFor()`'s screen↔key mapping, and the `case` in `render()` that calls
   `drawLight()`. `DialUi::m_lightCards` is sized `LightsModel::LightKey::COUNT` already, so it
   grows on its own — no fixed-size array to touch there. Decide `hasColour` for the new
   `LightCardState` constructor argument based on whether the light supports colour.
2. **HA automations:** in `pacekeeper_dial_light_commands`, add a third MQTT trigger
   (`pacekeeper-dial/light/{key}/set`, its own `id`) and a matching pair of `choose` branches (off
   / turn-on-with-data) targeting the new entity. In `pacekeeper_dial_light_state_publish`, add
   the new entity to the state trigger's `entity_id` list and a third `{key, entity}` pair to the
   `repeat.for_each` list — the `sequence` template already works for any entity via
   `repeat.item.entity`/`repeat.item.key`, so nothing else in that automation changes.
3. Re-run the two Phase F checks that matter most for a new light (retained state at boot, `Power`
   toggling within ~1 s) before considering it done.
