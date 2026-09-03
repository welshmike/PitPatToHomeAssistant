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

The M5Stack Dial v1.1 is the current target for local, HA-free control: tap to start/pause/
resume, turn the dial to change speed, and read live stats on the round display. The ESP32
DevKit build described above still works and is now the legacy target — kept building so it
stays available as a fallback.

### Hardware needed

* M5Stack Dial v1.1
* Power via USB-C (5 V) or the rear screw terminal (6–36 V) — no wiring required, everything
  (display, touch, encoder, buzzer) is built into the unit
* The same DeerRun Q1 / PitPat-T01 treadmill as above

### Controls

* **Tap** the screen: start (when disconnected or stopped), pause (when running), resume at the
  previous speed (when paused), or — while a connect is in progress (the Connecting screen) —
  cancel the connect
* **Hold** the screen for about 1 second: stop. While a connect is in progress, the same hold
  cancels the connect instead
* **Side WAKE button**: always stops, on every screen — an emergency stop that does not depend
  on what's showing. It does not cancel an in-progress connect (only tap/hold on the Connecting
  screen do that); pressing it there is a harmless no-op refusal since there's no belt link yet
* **Rotate** the dial: ±0.1 mph per detent (click). The change is queued and sent as a single
  speed command about 400 ms after the last click, so spinning several clicks quickly only
  produces one BLE write. The knob only changes speed while the belt is running; while stopped,
  paused, connecting or disconnected it is ignored (reserved for future screen navigation)
* If the screen is dimmed or off, the first tap, hold, or turn only wakes it — the input itself
  is discarded, so a brush of the dial in the dark can't start the belt

### What the screen shows

* **Disconnected**: last session's summary (time, distance, steps) with a hint to tap to start
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

After 2 minutes idle in Disconnected or Stopped, the backlight drops to 20% brightness. After
10 minutes it turns off. Any input restores full brightness (see the wake-only rule above).

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
