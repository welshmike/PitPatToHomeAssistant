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
* `config.h` also has `HOME_LAT`, `HOME_LON` and `FLIGHTS_RADIUS_MI` for the Flights card — see
  [Desk mode](#desk-mode) below for what they do and their default values; not needed on the
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
* **Side WAKE button**: on Running/Paused this is an emergency stop that always fires regardless
  of what's showing. On the Selector screen it cancels the picker instead, without sending any
  belt command. On Connecting it's a harmless no-op refusal since there's no belt link yet, and it
  does not cancel an in-progress connect; only tap/hold on the Connecting screen do that.
  Otherwise (Disconnected, Clock, or any other idle card) it homes: closes the selector if one was
  open and returns to the Treadmill card — see Desk mode below
* **Rotate** the dial (one detent = one physical "click" of the encoder):
  * Selector: ±0.2 mph per detent, clamped to 0.6–3.8 mph
  * Running: ±0.1 mph per detent, queued and sent as a single speed command about 400 ms after
    the last click, so spinning several clicks quickly only produces one BLE write
  * Paused, Connecting: ignored
  * Treadmill/Disconnected, Clock, and any other idle card (Selector not open): scrolls the card
    ring one card per detent — see Desk mode below
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
  belt), hold (starts at default), the side button (cancels, no belt command), or 20 seconds of
  inactivity
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

After 2 minutes idle on any card (Treadmill/Disconnected, Clock, or a future card), the backlight
drops to 20% brightness and stays there — it never turns off. Any input restores full brightness
(see the wake-only rule above).

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

### Desk mode

Away from the belt, the Dial is a small desk gadget: cards sit in a ring — Treadmill, Clock,
Flights, Office (light), Lamp (light), with more (Calendar, Music) coming in later sub-projects.
While the belt is idle and no selector is open, the knob scrolls the ring one card per detent, in
either direction, and the side button always jumps back to the Treadmill card (still an emergency
stop when the belt is actually running). Connecting/Starting/Running/Paused screens override
whichever card is showing, exactly as today, and the ring picks back up on the last card once the
belt returns to idle.

**The Dial boots into the Clock card**, not Disconnected — there is no idle return to a "home"
card, so the Treadmill card is reached explicitly via the side button. The Clock is an analogue
face on the round display: 12 tick marks, hour/minute/second hands, and a small date (`Mon 3 Sep`)
at the 6 o'clock position, redrawn once a second. Time comes from NTP over WiFi, using the POSIX TZ
string `TIMEZONE_TZ` from `config.h` (default `Europe/London`), and is also kept in the Dial's
onboard BM8563 RTC — synced from NTP once it succeeds, and read back at boot — so the clock reads
correctly immediately after a power cycle even with no WiFi available. Before either source has a
time (a fresh device, no WiFi yet, an unset RTC), the face shows tick marks with no hands and
`--:--` instead of a time.

**Flights** shows the aircraft overhead, nearest first, up to 6 at a time. Each one gets its
airline logo (or the operator name as text when there's no logo) at the top, `callsign - type`
below it (e.g. `BAW117 - A320`), the route large in the middle as IATA airport codes
(`LHR -> JFK`, or `route unknown` before it's been enriched), then altitude and ground speed
(`12,000 ft - 450 kt`) and distance and compass bearing (`3.1 mi NE`). Separators render as a
plain `-`/`->` rather than real dashes/arrows, since the Dial's built-in bitmap fonts are
ASCII-only. Below that, a row of small page dots — one per aircraft, filled for the one currently
shown — shows position in the list instead of a text index. Tapping the screen cycles to the next
aircraft.

Three keyless public data services are queried directly from the Dial over HTTPS — nothing goes
through Home Assistant: [adsb.fi](https://adsb.fi) for nearby aircraft, [hexdb.io](https://hexdb.io)
for route/airport/operator lookups, and [pics.avs.io](https://pics.avs.io) for airline logos. The
Dial talks to all three with certificate checking disabled (`WiFiClientSecure::setInsecure()`) —
an accepted trade-off given the data is public and read-only. Logos are cached under `/logos` in
the Dial's onboard flash (LittleFS) after their first download, so repeat views of a known airline
don't re-fetch the image. The card only fetches while it's actually showing: aircraft data
refreshes every 20 seconds while visible, and nothing is fetched while another card or the
Treadmill screen is on top.

Add three lines to `config.h` to set your location — the example below is central London:
```cpp
#define HOME_LAT           51.5074
#define HOME_LON           -0.1278
#define FLIGHTS_RADIUS_MI  3
```
`HOME_LAT`/`HOME_LON` are decimal degrees (positive north/east, negative south/west) — a postcode
centroid is close enough. `FLIGHTS_RADIUS_MI` is the search radius in statute miles. If these are
omitted, `FlightsService.h` falls back to the same central-London default shown above and (on the
Dial build only) emits a compile-time warning so the fallback doesn't go unnoticed.

If the last fetch failed, a small grey dot appears near the top of the card while the previously
known aircraft (if any) keep showing; if WiFi itself is down the card shows "waiting for WiFi"
instead; and when the radius is genuinely quiet (typically overnight) it shows "no aircraft
nearby" with the configured radius underneath.

**Lights** are two cards, **Office** and **Lamp**, each controlling one Home Assistant light over
MQTT — nothing goes through HA's REST/WebSocket API, only plain topics (see below). Each card
shows the light's title, its brightness large in the middle when on (`65%`) or `OFF` dimmed when
off, a caption underneath for colour temperature (`2700K`) or hue (`hue 210`), and a value ring
around the edge in the same style as the speed ring. Three small on-screen buttons along the
bottom replace the belt cards' tap/hold gestures:

* **Power** toggles the light on/off immediately — it stays live even before any brightness/colour
  data has arrived (a "blind" switch-on), as long as MQTT is up and at least one state message has
  been seen since boot
* **Bright** engages brightness: once engaged (the button fills amber) the knob adjusts it ±5% per
  detent, clamped 1–100%; tapping **Bright** again releases it, sending immediately whatever change
  hadn't yet settled
* **Colour** (Lamp only — Office is colour-temperature-only): first tap engages colour temperature
  or hue (whichever HA currently reports), second tap switches to the other, third tap releases the
  same way **Bright**'s second tap does. Knob: ±100 K per detent clamped to the light's own
  min/max kelvin (2000–6535 K for the Lamp), or ±10° per detent wrapping 0–360° for hue

While a value is engaged, rotating the knob adjusts it instead of scrolling the card ring; a
command is sent once, 300 ms after the last detent, the same debounce the treadmill's own speed
control uses — spinning several clicks quickly still only produces one MQTT publish. Engagement
also releases itself automatically after 10 seconds with no tap or detent, and silently (dropping
any not-yet-sent change) the moment the card ring scrolls away, the side button homes, or a
belt/connection screen takes over. Switching straight from one control to another (`Bright` while
colour is engaged, or `Colour` while brightness is) sends the change you had made rather than
dropping it. After a command goes out the card keeps showing the value it just sent for up to 1.5
seconds, so the reading doesn't flick back to HA's old state while the echo is still in flight.
With nothing engaged, the knob scrolls the card ring exactly
like the other desk cards.

The card shows `waiting for HA` (dimmed) while MQTT itself is down, or `no data` (dimmed) once
MQTT is up but no retained state has arrived yet for that light (or HA reports it `unavailable`).
During `waiting for HA` all three buttons are inert — there is nowhere for a command to go. During
`no data`, `Power` stays tappable (once a first message has parsed) and `Bright`/`Colour` are
inert. Topics: commands go out on `pacekeeper-dial/light/{office|lamp}/set`, retained state
comes back on `pacekeeper-dial/light/{office|lamp}/state`, and the Dial asks for a fresh snapshot
on `pacekeeper-dial/light/refresh` once after each MQTT (re)connect. The two Home Assistant
automations that apply commands and mirror state are documented, with their full YAML, in
[`doc/HA_LIGHTS_AUTOMATIONS.md`](doc/HA_LIGHTS_AUTOMATIONS.md).

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
