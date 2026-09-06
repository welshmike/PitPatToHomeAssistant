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
| `pacekeeper-dial/diag/boot` | **yes** | `{"reset":"SW","build":"Sep  6 2026 12:41:07","uptime_s":37,"heap":118432}` |

The boot record is republished on every MQTT (re)connect, so a subscriber that arrives late still
learns how the Dial last restarted:

* `reset` — `esp_reset_reason()`: `POWERON` (cold start), `SW` (a clean restart, which is what an
  OTA flash looks like), `PANIC`, `INT_WDT`, `TASK_WDT`, `WDT`, `DEEPSLEEP`, `BROWNOUT`, `SDIO`,
  `UNKNOWN`. Anything other than `POWERON`/`SW` means the firmware fell over.
* `build` — `__DATE__ " " __TIME__` of `src/RemoteLog.cpp`. It only moves when that translation
  unit is recompiled (a clean build, or a change to the build flags), so treat it as "which build
  family", not "which commit"; `uptime_s` is the reliable "did my flash land" signal.
* `uptime_s`, `heap` — seconds since boot and free heap at the moment of the MQTT connect.

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
The net task publishes at most 4 lines per 10 ms loop.

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
