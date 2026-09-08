# Homebrain PaceKeeper for WalkingPads in Home Assistant

PaceKeeper is an ESP32 bridge that connects via Bluetooth to your WalkingPad and exposes the device via MQTT to Home Assistant.

![Home Assistant PaceKeeper Device](doc/ha_device.png)

See the video on Youtube:

[![Usage video](https://img.youtube.com/vi/Pwt5jl2jNe4/0.jpg)](https://www.youtube.com/watch?v=Pwt5jl2jNe4)

## Supported Hardware

* PitPat-T01 Treadmill – Superun BA06-B1 [[AliExpress](https://s.click.aliexpress.com/e/_c3V1ssrv)]
* DeerRun Q1 Classic Pro (BA05/PitPat protocol variant)

## Required Tools

* ESP32 – I'm using a Wemos S3 Mini, but any ESP32 with Bluetooth should do [[AliExpress](https://de.aliexpress.com/item/1005006646247867.html)] [[Amazon](https://amzn.to/44VolhQ)]
* VS Code with PlatformIO

## Setup

### Find the Bluetooth Address of the Device

Get an app like **nRF Connect** – this app allows you to view Bluetooth connections on your phone.

* Turn the device on with the power switch
* Either use the app to initialize the device or follow the steps in the section **"Cloud Free Usage"**
* Open nRF Connect on your phone
* The device should show up as `PitPat-T01`
* Write down the Bluetooth address (it should look like `AA:BB:CC:11:22`)

### Preparation of Home Assistant for MQTT

* Add the MQTT integration and follow the setup steps:  
  <https://www.home-assistant.io/integrations/mqtt>

### Project Compilation

* Set up VS Code with PlatformIO  
  (<https://docs.platformio.org/en/latest/integration/ide/vscode.html#installation>)
* Clone this repo and open it in VS Code
* Copy `src/config.h.example` to `src/config.h`:
  ```bash
  cp src/config.h.example src/config.h
  ```
* Open `src/config.h` and fill in your values:
  ```cpp
  #define DEFAULT_STA_WIFI_SSID "your-wifi-name"
  #define DEFAULT_STA_WIFI_PASS "your-wifi-password"
  #define MQTT_SERVER           "192.168.x.x"       // your HA IP
  #define MQTT_USER             "your-mqtt-user"
  #define MQTT_PASS             "your-mqtt-password"
  #define TARGET_ADDRESS        "AA:BB:CC:11:22:33"  // from nRF Connect
  ```
* On the Dial, `config.h` also has `TIMEZONE_TZ` for the Clock card — a POSIX TZ string, defaulting
  to `Europe/London` (`GMT0BST,M3.5.0/1,M10.5.0`). Set it to your own timezone's POSIX TZ string if
  you're not in the UK; it's not needed on the DevKit build.
* `config.h` also has `FLIGHTS_LOGO_BASE_URL` for the Flights card's airline logos — see
  [Desk mode](#desk-mode) below for what it does and its default value; not needed on the
  DevKit build.
> **Note:** `src/config.h` is listed in `.gitignore` and will never be committed. Never commit your real credentials.
* Connect the ESP32 with a USB cable (you might have to hold **RST** and **BOOT** while plugging it in)
* Compile and flash the project via **PlatformIO → Upload and Monitor**, or from the CLI:
  ```bash
  pio run -e devkit-usb -t upload      # DevKit board, USB
  pio run -e devkit-ota -t upload      # DevKit board, OTA (after first USB flash)
  pio run -e dial-usb -t upload        # M5Stack Dial, USB — work in progress, see below
  pio run -e dial-ota -t upload        # M5Stack Dial, OTA
  pio test -e native                   # host-side unit tests, no hardware needed
  ```
* If everything goes well, you should see a bunch of log messages, and a new device called `PaceKeeper` should show up in your Home Assistant

## M5Stack Dial

The M5Stack Dial v1.1 is the current target for local, HA-free control: tap to pick a start speed
and go, tap/hold to pause/resume/stop, turn the dial to change speed, and read live stats on the
round display. The ESP32 DevKit build described above still works and is now the legacy target —
kept building so it stays available as a fallback.

### Hardware needed

* M5Stack Dial v1.1
* Power via USB-C (5 V) or the rear screw terminal (6–36 V) — no wiring required, everything
  (display, touch, encoder, buzzer) is built into the unit
* The same DeerRun Q1 / PitPat-T01 treadmill as above

### Start speed setting

Every "start" path — the Dial (tap-selector or hold), and the Home Assistant Start button — sends
the same configured start speed rather than a fixed value. It's the HA number entity
**Start Speed** (`start-speed`, 0.6–3.8 mph, step 0.1, default **1.0 mph**), settable from Home
Assistant like any other config entity; there is no on-Dial way to change the default yet — a Dial
configuration screen for this and similar settings is planned.

### Controls

* **Tap** the screen:
  * Disconnected: opens the **Start Speed selector** — the candidate speed starts at the
    configured default and shows large on the speed ring
  * Selector: confirms the candidate and starts the belt at that speed
  * Running: pause
  * Paused: resume at the previous speed
  * Connecting: cancel the connect
* **Hold** the screen for about 1 second:
  * Disconnected: starts the belt immediately at the configured default speed, skipping the
    selector
  * Selector: starts the belt at the default speed, skipping the candidate
  * Running or Paused: stop
  * Connecting: cancels the connect
* **Side WAKE button**: on Running/Paused the raw press is an emergency stop that always fires
  instantly, regardless of what's showing. On Connecting it's a harmless no-op refusal since
  there's no belt link yet, and it does not cancel an in-progress connect; only tap/hold on the
  Connecting screen do that. Otherwise, while the belt is idle, the button works in two stages: a
  decided single click (after M5Unified's 500 ms multi-click window) opens the **card menu**, or
  picks the highlighted card if the menu is already open; a **hold** (500 ms) always jumps
  straight to the Treadmill card from any screen, closing any open menu or selector on the
  way — see Desk mode below. On the Selector screen a click has no effect; a hold closes it
  (cancelling the candidate speed, no belt command sent) on its way home.
* **Rotate** the dial (one detent = one physical "click" of the encoder):
  * Selector: ±0.2 mph per detent, clamped to 0.6–3.8 mph
  * Running: ±0.1 mph per detent, queued and sent as a single speed command about 400 ms after
    the last click, so spinning several clicks quickly only produces one BLE write
  * Paused, Connecting: ignored
  * Treadmill/Disconnected, Clock, and any other idle card (Selector not open): no effect — the
    knob never scrolls the card ring; it always adjusts whatever value the card showing owns —
    see Desk mode below
* **Horizontal swipe** on the Selector screen: an alternate way to step the candidate, same
  0.2 mph per swipe as a detent — swiping right (clockwise) makes it faster, left makes it slower
* The Selector closes itself back to Disconnected after 20 seconds with no input
* If the screen is dimmed, the first tap, hold, turn, or swipe only wakes it — the input itself is
  discarded, so a brush of the dial in the dark can't start the belt

### What the screen shows

* **Disconnected**: last session's summary (time, distance, steps) with the hint
  "tap: speed   hold: start" — tap opens the Start Speed selector, hold starts right away at
  the configured default speed
* **Selector** ("START SPEED"): the candidate speed shown large, with an `mph` caption and an
  amber speed ring at that value, and hints "tap to start" / "hold: default". Opens (from
  Disconnected) at the configured default speed; closes back to Disconnected on tap (starts the
  belt), hold (starts at default), a hold of the side button (cancels, no belt command, and jumps
  home), or 20 seconds of inactivity
* **Connecting**: shown whenever a connect is in progress — including right after tapping start
  while the belt is unreachable, before the belt itself is counting down. "Connecting… attempt
  N" (attempt count only shown once there's been one) with a note that belt beeps are normal,
  and a hint that tap or hold cancels
* **Starting** (belt COUNTDOWN): reachable only once actually connected and the belt itself
  reports COUNTDOWN — a pulsing "STARTING" with a hint to tap to cancel
* **Running**: elapsed time in the centre, a speed ring around the edge (0–3.8 mph) with a
  numeric speed readout, distance and step count below
* **Paused**: the same layout as Running, dimmed, with a pulsing "PAUSED" label and a hint to tap
  to resume or hold to stop
* **Speed overlay**: after a rotate while running, the target speed and an amber target ring
  replace the centre content for 1.5 seconds, then revert automatically
* **Status dots** (BLE blue, WiFi green, MQTT green) show on every screen except Running, so the
  walking screen stays clean
* **Hold-to-stop/cancel progress**: while holding, a red arc outside the speed ring fills in on
  whichever screen is showing, not just Running/Paused
* Every screen has three small status dots along the top edge: BLE, WiFi, MQTT — lit when each is
  up

### Dimming

After 2 minutes idle on any card, the backlight drops to 20% brightness and stays there — it
never turns off. Any input restores full brightness (see the wake-only rule above). On the same
tick, a desk card other than Clock (Treadmill, Flights, Office, Lamp, Calendar) returns to the
Clock card, so a dimmed Dial always shows the clock and the next meeting. The return never
happens while the belt is connecting, counting down, running or paused, over an open menu or
speed selector, or while a Flights auto-show or Calendar nudge is holding its card up.

### Buzzer

A compile-time `DIAL_SOUND` flag (on by default for both `dial-usb` and `dial-ota`) enables short
confirmation tones: one short click on an accepted tap, two short beeps on stop, and a low buzz
when a command is refused (e.g. a command sent during the post-connect cooldown, or a disconnect
blocked while the belt is still active).

### Flashing

* First flash over USB-C:
  ```bash
  pio run -e dial-usb -t upload
  ```
* Subsequent flashes over the air, once the Dial is on WiFi:
  ```bash
  pio run -e dial-ota -t upload    # targets pacekeeper-dial.local
  ```
* Serial log output over USB-C:
  ```bash
  pio device monitor -e dial-usb
  ```
* With nothing plugged in, the same log lines (warnings, errors and the BLE connect/kick story)
  come out over MQTT on `pacekeeper-dial/diag`, plus a retained boot record on
  `pacekeeper-dial/diag/boot` — see [`doc/DIAGNOSTICS.md`](doc/DIAGNOSTICS.md).

### Desk mode

Away from the belt, the Dial is a small desk gadget: cards sit in a ring — Treadmill, Clock,
Flights, Office (light), Lamp (light), Calendar, with more (Music) coming in later sub-projects.
While the belt is idle, a **single click of the side button opens a card menu**: the six cards as
glyphs on a ring, the current one highlighted in amber with its name in the middle. Turn the knob
to move the highlight (it wraps at the ends); tap a glyph to go straight there, or tap anywhere
else on the face (or click the side button again) to take the highlighted one — the menu is never
a dead end. Eight seconds without input closes it unchanged. **Holding** the side button jumps straight back to the
Treadmill card from anywhere, closing the menu or the speed picker on the way; a **double click**
does the same but lands on the Clock card. While the belt is
actually running the side button is still the emergency stop, on the instant press rather than
after the click window. The knob never scrolls cards: it always adjusts whatever the card showing
owns (see each card below). Connecting/Starting/Running/Paused screens override whichever card is
showing, exactly as today, and the ring picks back up on the last card once the belt returns to
idle.

**The Dial boots into the Clock card**, not Disconnected — there is no idle return to a "home"
card, so the Treadmill card is reached explicitly via the menu or by holding the side button. The Clock is an analogue
face on the round display: 12 tick marks, hour/minute/second hands, and a small date (`Mon 3 Sep`)
above the centre, redrawn once a second. Time comes from NTP over WiFi, using the POSIX TZ
string `TIMEZONE_TZ` from `config.h` (default `Europe/London`), and is also kept in the Dial's
onboard BM8563 RTC — synced from NTP once it succeeds, and read back at boot — so the clock reads
correctly immediately after a power cycle even with no WiFi available. Before either source has a
time (a fresh device, no WiFi yet, an unset RTC), the face shows tick marks with no hands and
`--:--` instead of a time, with a "waiting for time" hint at the date's position. When the Calendar
feed is configured and fresh, today's next timed meeting appears below the centre on two lines —
the title, then its time `HH:MM` beneath it (Font2, dim), the time turning amber once the meeting
is under way; both lines disappear once today's last meeting has ended.

**Flights** shows the aircraft overhead, nearest first, up to 6 at a time. Each one gets its
airline logo (or the operator name as text when there's no logo) at the top, `callsign - type`
below it (e.g. `BAW117 - A320`), the route large in the middle as IATA airport codes
(`LHR -> JFK`, or `route unknown` before it's been enriched), a small line of the origin/destination
city names underneath when Home Assistant knows them (`London -> New York`), then altitude and
ground speed (`12,000 ft - 450 kt`) and distance and compass bearing (`3.1 mi NE`). Separators render as a
plain `-`/`->` rather than real dashes/arrows, since the Dial's built-in bitmap fonts are
ASCII-only. Below that, a row of small page dots — one per aircraft, filled for the one currently
shown — shows position in the list instead of a text index. Turning the knob or tapping the screen
cycles to the next aircraft. When the ring is idle on the Clock card and an aircraft comes into
range, the Flights card **auto-shows** itself over the Clock, and returns to the Clock on its own
once the last aircraft leaves range — unless you've cycled aircraft or navigated yourself in the
meantime, which cancels the automatic return. This is the `Flights Auto-show` switch in Home
Assistant (default on); turn it off there to reach Flights only from the card menu, like any
other card.

A second Home Assistant toggle, `PaceKeeper Dial airline flights only` (default on), hides
aircraft with no airline code — private planes, flying schools, most helicopters — so the card
only shows scheduled flights. Turn it off in HA to see everything within the radius.

All of the actual flight-tracking work — proximity, distance/bearing, route and operator lookup,
airline logos — now happens in **Home Assistant**, not on the Dial. The Dial only subscribes to
one retained MQTT topic and renders whatever Home Assistant last published; the only network call
it still makes itself is a plain-HTTP `GET` of a cached logo PNG from Home Assistant's own web
server. This replaced an earlier design (see `docs/superpowers/specs/2026-09-03-m5dial-migration-design.md`
§4.9, kept there for history) where the Dial queried a handful of public flight-data APIs directly
over HTTPS with TLS certificate checking disabled — each TLS connection on the ESP32-S3 cost around
45 KB of heap, WiFi and BLE share one antenna so the resulting request bursts could kick the
treadmill's Bluetooth link, and the request-pacing needed to avoid that made a newly-seen aircraft
take 8–10 seconds to fully enrich. Moving the work to Home Assistant (which already has the
[Flightradar24](https://github.com/AlexandrErohin/home-assistant-flightradar24) HACS integration
doing the ADS-B polling for its own dashboard) removes all of that from the firmware: no TLS, no
per-aircraft HTTP bursts, and a single small retained JSON message instead.

Setup and the full MQTT/YAML details live in [`doc/HA_FLIGHTS.md`](doc/HA_FLIGHTS.md). In short:
an HA automation publishes retained JSON on `pacekeeper-dial/flights/state` (nearest first, up to
6 aircraft) whenever the Flightradar24 sensor changes, at HA start, or when the Dial asks on
`pacekeeper-dial/flights/refresh` after each MQTT (re)connect; a second automation caches each new
airline's logo PNG into Home Assistant's `www/logos/` folder, which the Dial fetches over plain
HTTP as `http://<HA>:8123/local/logos/{IATA}.png` and caches itself under `/logos` in its own
onboard flash (LittleFS). `config.h`'s `FLIGHTS_LOGO_BASE_URL` points at that HA web server (see
[Project Compilation](#project-compilation) above); there is nothing left to configure for
location or radius on the Dial — both now live in the Flightradar24 integration's own options in
Home Assistant.

If Home Assistant goes quiet for two minutes with the card already showing a list, a small grey
dot appears near the top while that list keeps showing; if MQTT itself is down the card shows
"waiting for HA" instead; and when nothing is in range it draws a radar sweep (rotating one
revolution every 3 s) with "Searching" underneath, instead of a static empty message.

**Lights** are two cards, **Office** and **Lamp**, each controlling one Home Assistant light over
MQTT — nothing goes through HA's REST/WebSocket API, only plain topics (see below).

When the light is **off** the whole card is one switch-on target: a large power circle with
`tap to switch on` underneath. Tap anywhere and the light comes back on at whatever brightness it
was last at (the Dial sends `{"state":"ON"}` and lets the light restore itself); the knob, a swipe,
and a touch-hold all do nothing there — there is only the one face.

When it is **on** the card is a small stack of pages, shown by the dots near the top — Brightness,
then, on the Lamp, Colour, then Kelvin (Office has no Colour page, so it's just Brightness then
Kelvin). Swipe left or right to change page (the ends don't wrap), and **the knob always adjusts
the page you're looking at**:

* **Brightness**: the value ring around the edge plus the percentage in the middle; ±5 % per
  detent, clamped 1–100 %. The caption underneath shows the light's live colour state (`2700K` or
  `hue 240`).
* **Colour** (Lamp only): a continuous hue ring around the edge of the face with a marker at the
  selected hue and the colour itself in the centre disc. Tap anywhere on the ring to jump to that
  hue; each knob detent moves 15° (24 detents per revolution, wrapping), or put a finger on the
  ring and drag it round: the marker follows live and the lamp gets one command 300 ms after the
  finger pauses or lifts. The marker outline turns amber while the command settles and until HA
  echoes it. This page draws nothing but the ring, marker and disc: tap the centre disc to go back
  to Brightness (a touch starting on the ring scrubs instead), or swipe, or wait for the 10 s page
  timeout.
* **Kelvin**: the value large in the middle over a warm→cool bar with a marker at where this light
  sits inside its own min/max range; ±100 K per detent, clamped to that range.

The Lamp is in either temperature or hue mode at a time, so whichever of those two pages isn't
live draws dim with `not active - turn to use`; the first detent on it sends that mode's command
and makes it live. On the Brightness and Kelvin pages a small power glyph at the bottom switches
the light off (as does holding
anywhere on the card for a second, with the same red progress arc the belt's hold-to-stop uses).

The Kelvin and Colour pages are transient: ten seconds without a swipe, a detent or a ring tap
drops the card back to Brightness, so a card you glanced at earlier reads the same as one you have
just arrived on.

A command is sent once, 300 ms after the last detent, the same debounce the treadmill's own speed
control uses — spinning several clicks quickly still only produces one MQTT publish, and an edit
made just before you leave the card still goes out. After a command goes out the card keeps
showing the value it just sent for up to 1.5 seconds, so the reading doesn't flick back to HA's
old state while the echo is still in flight. Every arrival on a light card, and every switch-on,
starts on the Brightness page.

The card shows `waiting for HA` (dimmed) while MQTT itself is down, or `no data` (dimmed) once
MQTT is up but no retained state has arrived yet for that light (or HA reports it `unavailable`).
Neither face is interactive — there is nowhere for a command to go, or nothing to send it about.
Topics: commands go out on `pacekeeper-dial/light/{office|lamp}/set`, retained state
comes back on `pacekeeper-dial/light/{office|lamp}/state`, and the Dial asks for a fresh snapshot
on `pacekeeper-dial/light/refresh` once after each MQTT (re)connect. The two Home Assistant
automations that apply commands and mirror state are documented, with their full YAML, in
[`doc/HA_LIGHTS_AUTOMATIONS.md`](doc/HA_LIGHTS_AUTOMATIONS.md).

**Calendar** shows the next meeting from your Google Workspace calendar. The card displays: the meeting start time (Font4), title (wrapped to two lines in Font4), countdown timer (amber while in progress), and up to three following events (Font2). Today's all-day events collapse to one line at the top. Only *today's* meetings are shown: once the last one has ended the face reads `nothing more today`, with tomorrow's first meeting underneath as `Tomorrow 09:00 Title`. When the next meeting is due to start, the Dial **nudges** the Clock card — showing the Calendar card 5 minutes before (by default, configurable 1–30 min), and returning to Clock 1 minute after the meeting starts (configurable 0–10 min). Tapping the card while a nudge is active dismisses that specific meeting and returns to Clock immediately; belt running suppresses any nudge. The feed comes from a small Python relay on an always-on Mac mini, which turns the Google Calendar into the compact JSON the card expects (Home Assistant is not involved); see [`doc/CALENDAR_FEED.md`](doc/CALENDAR_FEED.md) for how it works and how to install it. The card is configured in `src/config.h` with `CALENDAR_URL` — the relay's `http://` address — and an optional `CALENDAR_TOKEN`, only needed if the relay's own token check is enabled. Without `CALENDAR_URL` the fetch service and the Calendar face are compiled out; the menu entry remains and shows the Clock until it is set. Setup and the full implementation details live in that doc.

Home Assistant settings for the Calendar card (the nudge switch is on by default; the two numbers default to 5 min and 1 min):

- **Calendar nudge** (switch, `cal_nudge`): enable or disable auto-show on the Clock card
- **Calendar lead** (number, `cal_lead`, 1–30 min, default **5**): how many minutes before the meeting start time the nudge appears
- **Calendar stay** (number, `cal_stay`, 0–10 min, default **1**): how many minutes after the meeting starts the Calendar card remains visible before returning to Clock

WiFi and BLE share one antenna, so background treadmill connects are held off while WiFi/MQTT come
up (first 30 s after boot) and for 6 s after each MQTT connect; a connect you ask for is never held.

The connect itself is staged and non-blocking: the Dial asks NimBLE for the link, carries on running
the UI, and only runs GATT setup once the belt has actually linked — a link that never comes up
inside 6 s is cancelled rather than disconnected, which is what used to send the belt into its
kicking phase.

### One device at a time

Only one BLE central can hold the Q1's connection, so run either the DevKit or the Dial — never
both at once. The Home Assistant device identity (`pacekeeper-bridge`) is independent of the
BLE/MAC address, so whichever board is running takes over the same HA device and entities; there
is nothing to reconfigure in Home Assistant when switching boards.

## Architecture

The firmware is split by concern: `TreadmillHandler` owns the BLE link to the belt (connect/
reconnect, keepalives, the BA05 protocol) and delegates session accounting to `SessionTracker`
and command intent to `TreadmillController`. MQTT messages are parsed on the net task into
`Command`s; the loop task's `drainCommands()` routes belt intents (start/pause/stop/speed/connect)
through `TreadmillController` and settings/calibration/restore straight to `TreadmillHandler`.
`SnapshotStore` holds the current `TreadMillData` behind a mutex so the BLE task and the main
loop can both read/write it safely. Two FreeRTOS tasks do the work: the main loop task drives
`TreadmillHandler::handle()` and the controller, while a dedicated `NetTask` owns WiFi/MQTT
(`NetManager`) and drains command/publish queues (`Commands.h`) so nothing but that task ever
touches `PubSubClient`. `board.h` selects per-target pins/partitions; the ESP32 DevKit is the
verified target today, and the M5Stack Dial (`dial-usb`/`dial-ota`) is a work-in-progress port —
its on-device UI is not yet documented here.

## Cloud Free Usage – Start Without WiFi, App, and Cloud Account

You’ll get a remote with it; it has **+**, **−**, and **play/pause** buttons. However, when you turn it on, it initially reacts with a long, annoying sound to any button press. When you turn it on with the power button, it will also take a while before showing display information, first lighting up all display segments.

That’s where you strike.

Turn it on and quickly press **(+)**; you will be greeted with a short sound. Then press **−, −, −, +, +**, wait **20 seconds**, turn it off and on again. It should now display something else, and you can start using it.

### Sequence

* Turn on using the `power` switch on the device
* Press `-` on the remote **3×**
* Press `+` on the remote **1×**
* Press `+` on the remote for **3 seconds**

Each correct input will be confirmed by a short, happy sound. Each incorrect input will be confirmed by a long, annoying sound.

Source:  
<https://www.reddit.com/r/treadmills/comments/1jtuwix/heres_how_you_unlock_superun_treadmills_without/>

## Fork

This project is a fork of [peteh/pacekeeper](https://github.com/peteh/pacekeeper), extended with support for the DeerRun Q1 Classic Pro, session delta tracking, mph units, step estimation, and Strava integration.

## Acknowledgements

I built this with the help of many other people who put effort into reverse-engineering the Bluetooth protocol.

### Web Bluetooth App (Python)

Python web interface to control the treadmill via Bluetooth but for another model.

GitHub project:  
<https://github.com/azmke/pitpat-treadmill-control>

### Web Bluetooth App (JavaScript)

A Web Bluetooth app written in JavaScript. Fully supports the B1 as well.

GitHub project:  
<https://github.com/KeiranY/PitPat-WebBT/>

### Zwift Integration by qdomyos

There is some work in a B1 sub-branch.

Source file:  
<https://github.com/cagnulein/qdomyos-zwift/blob/master/src/devices/deeruntreadmill/deerruntreadmill.cpp>

## Further Notes

Deerrun and Superun seem to use the same OEM hardware, so it's likely that those devices might work as well.
