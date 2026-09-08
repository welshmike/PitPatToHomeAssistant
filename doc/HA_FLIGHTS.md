# Home Assistant setup for the Dial's Flights card

The Dial's Flights card (spec §4.11) does no flight tracking of its own any more. All of it —
proximity, distance/bearing, route and operator lookup, airline logos — happens in Home Assistant;
the Dial only subscribes to one retained MQTT topic and fetches a cached logo PNG over plain HTTP.
Everything below (the Flightradar24 integration, the `downloader` integration, one `input_text`
helper and two automations) was created directly through the HA API on 2026-09-05
(`automation.pacekeeper_dial_flights_publish` and `automation.pacekeeper_dial_airline_logos`); the
YAML further down is a readable copy of exactly what's live, with the `id` HA assigns on creation
dropped (re-importing this YAML gets a fresh one).

## Why this moved out of the firmware

The Dial used to query public flight-data APIs directly over HTTPS (see
`docs/superpowers/specs/2026-09-03-m5dial-migration-design.md` §4.9 for what that looked like — kept
there as history, not current behaviour). Three problems with that, found during hardware testing
on 2026-09-04:

* **Heap:** each TLS connection on the ESP32-S3 cost roughly 45 KB of free heap, on a device that
  also needs headroom for BLE and the display.
* **Radio coexistence:** WiFi and BLE share one antenna, so a burst of HTTPS requests (fetching an
  aircraft's route, airport names and operator in quick succession) could starve the BLE link and
  kick the treadmill's Bluetooth connection.
* **Latency:** the request-pacing added to fix the coexistence problem (one request at a time,
  spaced out) meant a newly-seen aircraft took 8–10 seconds to fully enrich with route/operator/logo.

Home Assistant already runs the [Flightradar24 HACS
integration](https://github.com/AlexandrErohin/home-assistant-flightradar24) for its own dashboards,
which does all of that ADS-B polling and enrichment anyway. Two HA automations turn its sensor into
one small retained MQTT message and a folder of cached logo PNGs; the Dial does no TLS, no
per-aircraft HTTP bursts, and no pacing logic at all any more.

## Install and configure

### Flightradar24 integration

Installed via HACS: `AlexandrErohin/home-assistant-flightradar24`, v2.1.2, no subscription/API key
needed.

**Via the UI:** HACS → search "Flightradar24" → Download, restart HA, then Settings → Devices &
Services → Add Integration → "Flightradar24" and fill in the config flow.

**As configured (2026-09-05):**

| Option | Value |
| --- | --- |
| Latitude | `51.566645` |
| Longitude | `0.005586` |
| Radius | `5000` m (≈ 3.1 mi — matching the Dial's previous default search radius) |
| Everything else | integration defaults (default scan interval, no altitude filter) |

This creates `sensor.flightradar24_current_in_area`, whose `flights` attribute is a list with one
entry per aircraft in range — callsign, flight number, airline (name/IATA/ICAO), aircraft type
code, origin/destination IATA, altitude (ft), ground speed (kt), heading, lat/lon and distance
(km). Both automations below read this one sensor; nothing else in HA needs to poll ADS-B data
separately.

### `downloader` integration

Used by the logos automation to save each airline's PNG into HA's own web-served folder.

**Via the UI:** Settings → Devices & Services → Add Integration → "Downloader", set **Download
Directory** to `www` (HA serves `config/www/` at `/local/`, so a file saved to `www/logos/BA.png`
is served at `http://<HA>:8123/local/logos/BA.png`).

**As configured:** `download_dir: www`.

### `input_text.pacekeeper_dial_logos_cached` helper

Records which airline IATA codes already have a cached logo, so the logos automation doesn't
re-download one every time the sensor updates.

**Via the UI:** Settings → Devices & Services → Helpers → Add Helper → Text, name it (entity ID
`input_text.pacekeeper_dial_logos_cached`), **Maximum length** `255`.

**As configured:** a comma-separated list of 2-letter IATA codes, capped at the newest ~80 (well
inside the 255-character maximum) by the logos automation itself each time it appends one.

This is an `input_text`, not a template sensor, because HA's API cannot create a **trigger-based**
template sensor (the kind needed to persist a list across restarts and append to it from an
automation) — only the config-flow `template` helper, which doesn't support that pattern. A plain
`input_text` gives the same "remember what's cached" behaviour and is fully API-creatable. One
consequence: the logos automation triggers on the sensor's own state (and HA start), not on the
Flightradar24 integration's `flightradar24_entry` event as the original spec draft assumed — this
also means airlines already overhead at HA startup get their logos fetched immediately, not only
newly-arriving ones.

### `input_boolean.pacekeeper_dial_airline_flights_only` helper (added 2026-09-05)

A toggle for the publish automation: when **on** (the default as configured), aircraft with no
airline IATA code — private, general-aviation and most helicopters — are left out of the payload,
so the Dial only shows airline flights. Turn it **off** to see everything the integration reports.
Toggling it republishes immediately (it is a trigger of the publish automation), so the Dial
follows within a second.

**Via the UI:** Settings → Devices & Services → Helpers → Add Helper → Toggle, name it
`PaceKeeper Dial airline flights only` (entity ID `input_boolean.pacekeeper_dial_airline_flights_only`),
icon `mdi:airplane-check`. Add it to any dashboard as a normal toggle.

## MQTT contract

| Direction | Topic | Payload | Notes |
| --- | --- | --- | --- |
| HA → Dial (state) | `pacekeeper-dial/flights/state` | JSON, see below | Retained. Published on any change to `sensor.flightradar24_current_in_area` (incl. attributes), at HA start, and in reply to `refresh`. |
| Dial → HA (refresh) | `pacekeeper-dial/flights/refresh` | `1` | Published once after each MQTT (re)connect — asks HA to re-publish the retained state so the Dial doesn't wait for the next real change. |
| Dial ← HA (logo) | `http://<HA>:8123/local/logos/{IATA}.png` | PNG, plain HTTP | Not MQTT — a `WiFiClient` GET from `FlightsService`. 404 means no cached logo for that airline (yet — HA may still be downloading it); the Dial retries after 60 s and falls back to text. |

State payload — nearest first, at most 6 aircraft, compact keys so six aircraft fit in roughly
800 bytes (`FlightsService`'s MQTT buffer is 2048 bytes):

```json
{"ts":1788596952,"ac":[{"cs":"BAW84NT","fl":"BA847","ty":"A320","al":"BA","an":"British Airways","fr":"WAW","to":"LHR","fc":"Warsaw","tc":"London","alt":5850,"gs":269,"di":0.8,"br":246,"gnd":0}]}
```

| Key | Meaning | Unit |
| --- | --- | --- |
| `ts` | Message timestamp | epoch seconds |
| `ac` | Array of aircraft, nearest first, max 6 | — |
| `cs` | Callsign | string, ≤ 8 chars |
| `fl` | Flight number | string, ≤ 8 chars |
| `ty` | Aircraft type code | ICAO string, ≤ 4 chars |
| `al` | Airline IATA code | string, ≤ 2 chars, `""` if unknown |
| `an` | Operator/airline display name | string, ≤ 24 chars, `""` if unknown |
| `fr` | Origin airport | IATA code, ≤ 4 chars, `""` if unknown |
| `to` | Destination airport | IATA code, ≤ 4 chars, `""` if unknown |
| `fc` | Origin city name | string, ≤ 12 chars, `""` if unknown |
| `tc` | Destination city name | string, ≤ 12 chars, `""` if unknown |
| `alt` | Altitude | feet, `0` if on the ground/unknown |
| `gs` | Ground speed | knots, `0` if unknown |
| `di` | Distance from home | statute miles, one decimal place |
| `br` | Bearing from home to the aircraft | degrees, `0`–`359` |
| `gnd` | On-ground flag | `0` or `1` |

Verified live on 2026-09-05 with one aircraft overhead: the payload above (167 bytes), and logos
served at `http://<HA>:8123/local/logos/VS.png` (3501 bytes) and `.../2L.png` (4475 bytes).

## Automation 1: `pacekeeper_dial_flights_publish`

Reads `sensor.flightradar24_current_in_area`, computes distance (statute miles) and bearing from
each aircraft's lat/lon using HA's own Jinja trig functions (`sin`/`cos`/`atan2`/`radians`) so the
Dial does no maths at all, and publishes the retained state JSON.

```yaml
alias: 'PaceKeeper Dial: flights publish'
description: Publishes the aircraft currently over the house (from the Flightradar24 integration)
  as one retained compact JSON on pacekeeper-dial/flights/state for the PaceKeeper Dial's
  Flights card. Nearest first, max 6. Distance (statute miles) and bearing are computed here
  so the Dial does no maths. When input_boolean.pacekeeper_dial_airline_flights_only is on,
  aircraft without an airline IATA code (private/GA) are left out.
triggers:
  - trigger: state
    entity_id: sensor.flightradar24_current_in_area
    id: changed
    note: 'No to/from: attribute-only updates (positions) fire too.'
  - trigger: state
    entity_id: input_boolean.pacekeeper_dial_airline_flights_only
    id: filter
    note: Republish immediately when the filter is toggled.
  - trigger: homeassistant
    event: start
    id: start
  - trigger: mqtt
    topic: pacekeeper-dial/flights/refresh
    id: refresh
    note: The Dial publishes here after each MQTT connect.
actions:
  - action: mqtt.publish
    data:
      topic: pacekeeper-dial/flights/state
      retain: true
      payload: '{{ payload }}'
mode: queued
max: 3
variables:
  lat0: 51.566645
  lon0: 0.005586
  airline_only: >-
    {{ is_state('input_boolean.pacekeeper_dial_airline_flights_only', 'on') }}
  flights: >-
    {{ state_attr('sensor.flightradar24_current_in_area', 'flights') | default([], true) }}
  payload: >-
    {% set ns = namespace(items=[]) %}{% for f in flights if f.latitude is number and f.longitude
    is number and (not airline_only or (f.airline_iata or '') | length == 2) %}{% set la0 = lat0 * pi / 180 %}{% set la1 = f.latitude * pi / 180 %}{% set
    dlat = la1 - la0 %}{% set dlon = (f.longitude - lon0) * pi / 180 %}{% set a = sin(dlat
    / 2) ** 2 + cos(la0) * cos(la1) * sin(dlon / 2) ** 2 %}{% set dist_mi = 3958.8 * 2 * atan2(sqrt(a),
    sqrt(1 - a)) %}{% set y = sin(dlon) * cos(la1) %}{% set x = cos(la0) * sin(la1) - sin(la0)
    * cos(la1) * cos(dlon) %}{% set br = ((atan2(y, x) * 180 / pi) + 360) % 360 %}{% set ns.items
    = ns.items + [{'cs': (f.callsign or '')[:8], 'fl': (f.flight_number or '')[:8], 'ty':
    (f.aircraft_code or '')[:4], 'al': (f.airline_iata or '')[:2], 'an': (f.airline_short
    or f.airline or '')[:24], 'fr': (f.airport_origin_code_iata or '')[:4], 'to': (f.airport_destination_code_iata
    or '')[:4], 'fc': (f.airport_origin_city or '')[:12], 'tc': (f.airport_destination_city
    or '')[:12], 'alt': (f.altitude or 0) | int, 'gs': (f.ground_speed or 0) | int, 'di': dist_mi
    | round(1), 'br': br | round(0) | int, 'gnd': (f.on_ground or 0) | int}] %}{% endfor %}{{
    {'ts': now().timestamp() | int, 'ac': (ns.items | sort(attribute='di'))[:6]} | to_json
    }}
```

`lat0`/`lon0` are the home position — the same values passed to the Flightradar24 integration's own
config, kept here too since the integration doesn't expose them back to a template. `3958.8` is the
Earth's mean radius in statute miles (the haversine formula's usual constant, converted from
kilometres). Aircraft missing `latitude`/`longitude` are skipped entirely (no distance/bearing can
be computed); every other field falls back to `""`/`0` rather than omitting the key, matching what
`FlightsModel::parseDialFlights` expects on the Dial side.

## Automation 2: `pacekeeper_dial_airline_logos`

For every airline IATA code currently overhead that isn't already recorded in
`input_text.pacekeeper_dial_logos_cached`, downloads its logo and appends the code.

```yaml
alias: 'PaceKeeper Dial: airline logos'
description: 'Caches airline logos for the PaceKeeper Dial: for every airline IATA code currently
  over the house that is not yet in input_text.pacekeeper_dial_logos_cached, downloads https://pics.avs.io/120/48/{IATA}.png
  to www/logos/{IATA}.png (served at /local/logos/{IATA}.png) and records the code. Unknown
  airlines (404) leave no file; the Dial falls back to text.'
triggers:
  - trigger: state
    entity_id: sensor.flightradar24_current_in_area
    id: changed
  - trigger: homeassistant
    event: start
    id: start
conditions:
  - condition: template
    value_template: '{{ wanted | count > 0 }}'
    note: Only run when there is at least one new airline to fetch.
actions:
  - repeat:
      for_each: '{{ wanted }}'
      sequence:
        - action: downloader.download_file
          data:
            url: https://pics.avs.io/120/48/{{ repeat.item }}.png
            subdir: logos
            filename: '{{ repeat.item }}.png'
            overwrite: true
          continue_on_error: true
          note: 404 for an unknown airline must not stop the other downloads; the code is still
            recorded so it is not retried every scan.
        - action: input_text.set_value
          target:
            entity_id: input_text.pacekeeper_dial_logos_cached
          data:
            value: >-
              {{ ((states('input_text.pacekeeper_dial_logos_cached').split(',') | select('match',
              '^[A-Z0-9]{2}$') | list)[-80:] + [repeat.item]) | join(',') }}
mode: single
max_exceeded: silent
variables:
  cached: >-
    {{ states('input_text.pacekeeper_dial_logos_cached').split(',') | select('match', '^[A-Z0-9]{2}$')
    | list }}
  wanted: >-
    {{ (state_attr('sensor.flightradar24_current_in_area', 'flights') | default([], true))
    | map(attribute='airline_iata') | select('string') | select('match', '^[A-Z0-9]{2}$')
    | unique | reject('in', cached) | list }}
```

Triggering on the sensor's own state (rather than the integration's `flightradar24_entry` event, as
originally sketched) means an airline already overhead when HA starts gets its logo fetched on the
`homeassistant: start` trigger, not only on its next arrival. `[-80:]` keeps the recorded list to
the newest ~80 codes, comfortably inside the helper's 255-character maximum, without ever needing
to prune by hand.

## Changing radius, position, scan interval or the airline-only filter

Radius, position and scan interval live in the Flightradar24 integration's own options, not in the
Dial's `config.h`: Settings → Devices & Services → Flightradar24 → **Configure** (radius set to
4000 m on 2026-09-05, down from 5000 m, to cut the number of light aircraft). The airline-only
filter is the `input_boolean.pacekeeper_dial_airline_flights_only` toggle described above. Nothing
on the Dial or in either automation needs to change — both read whatever HA currently reports.

## Adding a dashboard card

This project deliberately creates no dashboard for flights. The Flightradar24 integration ships its
own `custom:flightradar24-card` (installed automatically with the integration via HACS) that reads
`sensor.flightradar24_current_in_area` directly and draws a live map — add that card to any HA
dashboard rather than building a bespoke one here.

## Troubleshooting

* **Log lines like `Flightradar24: rate limited, backing off` (or similar) in HA's log.** Expected
  — see the [integration's own README](https://github.com/AlexandrErohin/home-assistant-flightradar24)
  for its rate-limiting behaviour; it self-recovers and does not need anything changed on the
  PaceKeeper side.
* **Small grey dot near the top of the Flights card.** HA hasn't published a fresh
  `pacekeeper-dial/flights/state` for 120 seconds while MQTT is otherwise up — the previous
  aircraft list keeps showing until a new message arrives. Check the two automations' traces
  (Settings → Automations → *PaceKeeper Dial: flights publish* → Traces) for whether
  `sensor.flightradar24_current_in_area` is still updating.
* **Card shows `waiting for HA`.** MQTT itself is down from the Dial's point of view, not a flights
  problem specifically — check the Dial's other MQTT-fed cards (Lights) for the same symptom, and
  the broker/network between the Dial and HA.
* **A known major airline still shows as text, not its logo.** Check
  `input_text.pacekeeper_dial_logos_cached` for that IATA code; if it's missing, check the
  *PaceKeeper Dial: airline logos* automation's trace and the `downloader` integration's log for a
  failed download (a non-2-letter or lowercase `airline_iata` from the integration would fail the
  `^[A-Z0-9]{2}$` filter and never be requested).
