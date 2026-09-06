# Remote diagnostics over MQTT

The Dial is flashed over the air and normally has nothing plugged into its USB-C port, so the
serial monitor is not available when the interesting things happen — in particular the recurring
BLE "kick cycle", where the belt drops the link a few seconds after each connect. Home Assistant
only ever sees the successful connects, so the firmware republishes its own log lines over MQTT
(spec 4.14, `src/RemoteLog.cpp`).

Both boards do this; the topics are fixed strings, and only one board is ever live at a time.

## Topics

| Topic | Retained | Payload |
| --- | --- | --- |
| `pacekeeper-dial/diag` | no (QoS 0) | one log line per message, e.g. `W (48213) Treadmill: Mid-session kick — backing off 10s before reconnect to work through kicking phase` |
| `pacekeeper-dial/diag/boot` | **yes** | `{"reset":"SW","build":"Sep  6 2026 12:41:07","uptime_s":37,"heap":118432,"last_lines":0}` |
| `pacekeeper-dial/diag/last` | no (QoS 0) | one pre-crash log line per message, e.g. `[last-1] I (5642) Treadmill: connect() begin` — see [After a crash](#after-a-crash) |

The boot record is republished on every MQTT (re)connect, so a subscriber that arrives late still
learns how the Dial last restarted:

* `reset` — `esp_reset_reason()`: `POWERON` (cold start), `SW` (a clean restart, which is what an
  OTA flash looks like), `PANIC`, `INT_WDT`, `TASK_WDT`, `WDT`, `DEEPSLEEP`, `BROWNOUT`, `SDIO`,
  `UNKNOWN`. Anything other than `POWERON`/`SW` means the firmware fell over.
* `build` — `__DATE__ " " __TIME__` of `src/RemoteLog.cpp`. It only moves when that translation
  unit is recompiled (a clean build, or a change to the build flags), so treat it as "which build
  family", not "which commit"; `uptime_s` is the reliable "did my flash land" signal.
* `uptime_s`, `heap` — seconds since boot and free heap at the moment of the MQTT connect.
* `last_lines` — how many `diag/last` messages accompanied this record. Non-zero only on the first
  MQTT connect after a crash; `0` on every reconnect and after a clean restart.

## Reading the stream

```bash
mosquitto_sub -h <broker-host> -u <user> -P <pass> -t 'pacekeeper-dial/diag/#' -v
```

`-v` prints the topic before each payload, so the boot record and the log lines are
distinguishable. Both topics are plain text; nothing is batched.

With paho-mqtt, if you want to timestamp or file the lines:

```python
import paho.mqtt.client as mqtt

def on_connect(c, _u, _f, _rc):
    c.subscribe("pacekeeper-dial/diag/#")

def on_message(_c, _u, msg):
    print(msg.topic, msg.payload.decode("utf-8", "replace"), flush=True)

c = mqtt.Client()
c.username_pw_set("<user>", "<pass>")
c.on_connect, c.on_message = on_connect, on_message
c.connect("<broker-host>", 1883, 60)
c.loop_forever()
```

## After a crash

A hung Dial publishes nothing, so the freeze that precedes a watchdog reset used to leave no trace
at all — the first retained boot record showed `TASK_WDT` (the 300 s loop-task watchdog) with
nothing to say what the loop had been doing. `RemoteLog` therefore keeps the last **8** forwarded
lines in a ring in RTC slow memory (`RTC_NOINIT_ATTR`, `src/RemoteLog.cpp`, spec 4.16). That memory
is not zeroed by the startup code and survives a panic, either watchdog and a brownout — everything
except a power cycle, where it holds whatever the SRAM powered up as, which is why the ring carries
a magic word and a checksum and is ignored unless both match.

What you get, **once per boot**, on the first MQTT connect:

* one message per saved line on `pacekeeper-dial/diag/last`, oldest first, prefixed `[last-N] `
  where N counts back from the reset — `[last-8]` … `[last-1]`, so `[last-1]` is the last thing the
  firmware managed to log before it stopped;
* `"last_lines":N` in the retained `diag/boot` record, so you know how many to expect.

It appears **only when `esp_reset_reason()` is something other than `POWERON` or `SW`** — a power
cycle has no ring to read and an OTA flash (`SW`) is not a crash. In every case the ring is cleared
after that first connect, so later reconnects publish `"last_lines":0` and nothing on `diag/last`,
and the next crash reports its own tail rather than replaying this one.

To read it, subscribe *before* the Dial reconnects — `diag/last` is not retained:

```bash
mosquitto_sub -h <broker-host> -u <user> -P <pass> -t 'pacekeeper-dial/diag/#' -v
```

then reset the Dial (or wait for it to fall over). The retained `diag/boot` record arrives first
with the `reset` reason and `last_lines`, then the `[last-N]` lines, then the normal live stream.
If you missed them, the retained boot record still tells you *that* it crashed and how many lines
were published; the lines themselves are gone.

Two things exist purely to make that tail readable:

* **Brackets around the blocking BLE calls** (`src/TreadmillHandler.cpp`): `connect() begin` /
  `connect() end ok|fail in N ms` around `NimBLEClient::connect()`, and `discover begin` /
  `discover end` around the service-discovery retry loop. A `begin` with no matching `end` at the
  tail of the ring names which call the loop task froze in.
* **The loop stall detector** (`src/main.cpp`): a `loop stall N ms` warning whenever one `loop()`
  iteration takes longer than `kLoopStallMs` (2 s). A normal pass is single-digit milliseconds, so
  this fires long before the 300 s watchdog does — and being a W line it is always forwarded. The
  one routine pass that trips it is a connect whose service discovery runs all 8 × 250 ms retries.

## What gets forwarded

Capture is a single `esp_log_set_vprintf()` hook installed by `RemoteLog::begin()`, the first
statement of `setup()`. The build defines `-DUSE_ESP_IDF_LOG` for `src/` only
(`build_src_flags` in `platformio.ini`), so the firmware's `log_e/log_w/log_i` go through
ESP-IDF's logger and past that hook. The hook calls the previous vprintf first, so serial output
is unchanged apart from the line format, which is now ESP-IDF's:

```
W (48213) Treadmill: Mid-session kick — backing off 10s before reconnect
```

(`<level> (<ms since boot>) <tag>: <message>`, where the tag is the per-file `TAG` — `Treadmill`,
`NetTask`, `NetManager`, `App`, `Session`, `Lights`, `Flights`, `DialUi`, …)

Filter, applied in the hook:

* **E** and **W** — always forwarded.
* **I** — only when the text contains one of `connect`, `Connect`, `kick`, `Kick`, `Subscribed`,
  `BLE`, `Net status`, `boot`. That keeps the connect/kick story and drops the 15 s heap
  heartbeat and the per-command chatter.
* **D** and **V** — never (they are not compiled in at `CORE_DEBUG_LEVEL=3` anyway).
* A line with no recognisable `E (` / `W (` / `I (` prefix is treated as **I**.

Lines are truncated to 119 characters and the trailing newline is stripped. The queue holds 24
lines; the hook never blocks, so a burst that overflows it is counted and reported as a
`[+N dropped] ` prefix on the next line that does get through. Lines queued while MQTT is down stay
queued, so the boot-time BLE story survives until the link comes up (about 5 s after power-on).
The net task publishes at most 4 lines per 10 ms loop. Every line that passes the filter also goes
into the RTC ring described under [After a crash](#after-a-crash), whether or not the queue had room
for it — a burst that overflows the queue is exactly the burst whose tail is worth keeping.

### Widening or narrowing the filter

* **More INFO lines:** add a keyword to `kInfoKeywords` in `src/RemoteLog.cpp`. Matching is a plain
  case-sensitive `strstr`, so add both cases (`connect`/`Connect`) when you mean either.
* **Everything at INFO:** delete the `if (level == 'I' && !infoWanted(buf))` early return in
  `hook()`. Expect a line every 15 s from the heap heartbeat alone.
* **Debug lines too:** they are compiled out — raise `CORE_DEBUG_LEVEL` in `[common]` first, then
  drop the `'D'`/`'V'` early return.
* **Quieter:** `RemoteLog::begin()` calls `esp_log_level_set("*", ESP_LOG_INFO)`. This is required —
  Arduino's `initArduino()` pins every tag at the packaged sdkconfig's `CONFIG_LOG_DEFAULT_LEVEL`,
  which is `ERROR`, and without raising it the firmware's own `log_w`/`log_i` would be dropped
  inside `esp_log_write()` (on serial as well as here). The side effect is that ESP-IDF's own
  components (`wifi`, `esp_netif`, …) now log at INFO too; to silence those while keeping ours, set
  `"*"` back to `ESP_LOG_ERROR` and call `esp_log_level_set()` once per firmware tag instead.

### Known gap: NimBLE's own lines

NimBLE-Arduino writes its `E NimBLEClient: ...` lines with `console_printf()` (a plain `printf`)
rather than `esp_log_write()` — see `USING_NIMBLE_ARDUINO_HEADERS` in its `nimconfig.h`. They still
reach the serial console but they do **not** pass the vprintf hook, so they are not forwarded.
What is forwarded is `TreadmillHandler`'s own account of the same events (`Disconnected reason=…
(HCI 0x…)`, `Idle kick`, `Mid-session kick`, `Connection successful`, `Subscribed to
notifications`), which is what the kick-cycle analysis in `doc/Q1_BLE_NOTES.md` is built on.
