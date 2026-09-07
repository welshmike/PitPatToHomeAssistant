# Calendar feed for the Dial (Mac mini relay)

The Dial's Calendar card (spec §4.19) shows the next few meetings from the work Google
Workspace calendar. Nothing goes through Home Assistant.

## Why a relay

The original design was a Google Apps Script web app in the work account. That is dead:
the Workspace policy only allows Apps Script web apps to be shared with "anyone in the
company", and the Dial is not a company account.

The calendar's **secret iCal address** is available, but it is the *whole* calendar —
about 2 MB of `VEVENT`s and `RRULE`s. Streaming and expanding that on an ESP32-S3 with a
2 KB response buffer is not on.

So a small Python process on the always-on **Mac mini** does the work: it fetches the
`.ics` every 2 minutes, expands recurrences for *now → end of tomorrow* in Europe/London,
drops meetings you have declined, and serves the same compact JSON the Dial always
expected, over plain HTTP on the LAN.

```
Google Calendar  --https/.ics-->  Mac mini relay  --http/LAN-->  Dial
   (~2 MB)                        (2 min refresh)                (~300 B)
```

## What runs where

| Piece | Where | Notes |
| --- | --- | --- |
| `tools/calendar_feed/calendar_feed.py` | Mac mini | one process: refresh thread + `http.server` |
| `~/.config/pacekeeper/calendar.env` | Mac mini | `ICS_URL`, `MY_EMAIL`, optional `PORT`/`TOKEN`/`TZ`/`BIND`; **never in the repo** |
| `~/Library/Application Support/pacekeeper-calendar/venv` | Mac mini | `icalendar` + `recurring-ical-events` |
| `~/Library/LaunchAgents/com.pacekeeper.calendar-feed.plist` | Mac mini | `RunAtLoad` + `KeepAlive` |
| `~/Library/Logs/pacekeeper-calendar.log` | Mac mini | stdout and stderr |
| `CALENDAR_URL` / `CALENDAR_TOKEN` | `src/config.h` | gitignored |

## Payload

`GET /calendar.json` returns, with `Cache-Control: no-store`:

```json
{"t":1757260000,"ev":[{"s":1757261400,"e":1757263200,"n":"Standup","a":0,"l":"Google Meet"}]}
```

`t` is the relay's Unix time when the payload was built (the Dial's staleness clock);
each event has `s`/`e` start and end (Unix, UTC), `n` title (≤ 40 printable-ASCII chars,
overflow marked `...`), `a` = 1 for all-day, `l` location or meeting host (≤ 24 chars).
At most 5 events, sorted by start, now → end of tomorrow, declined events omitted,
recurring events expanded with `RECURRENCE-ID` overrides honoured. `STATUS:CANCELLED`
events are dropped too — a cancelled one-off never appears, and a single cancelled
instance of a recurring series (a `RECURRENCE-ID` override carrying `STATUS:CANCELLED`)
drops only that instance, not the series. An all-day event's `s`/`e` are local midnight
and the next local midnight; a single malformed `VEVENT` (bad or missing `DTSTART`, say)
is skipped and logged rather than failing the whole refresh.

`l` follows the old Apps Script rule: `LOCATION` unless it is a bare URL, else
`Google Meet` / `Zoom` / `Teams` if one of those appears in the description or location,
else empty. Non-ASCII is stripped rather than truncated mid-sequence — the Dial's
Font2/Font4 cannot draw those glyphs and its `char[41]`/`char[25]` fields are byte-sized.

`GET /health` returns `{"ok":…,"stale":…,"age_s":…,"last_error":"…"}` and never needs a
token — which is why its body is kept deliberately thin. `age_s` is seconds since the
last *successful* refresh (`-1` if there has never been one); `stale` is true once that
age passes two refresh cycles (10 minutes at the default `refresh_s`). `last_error` is
the failing exception's **class name only** (e.g. `"ValueError"`), never its message: a
few of icalendar's own `ValueError`s quote the offending content line verbatim, which can
be a `SUMMARY:` — a meeting title — so the full (secret-redacted) message goes to the log
only, never to an unauthenticated HTTP response.

## Install on the Mac mini

1. Get the code onto the mini — clone the repo, or copy the `tools/calendar_feed`
   directory anywhere stable (the launch agent points at wherever you run `install.sh`
   from, so do not move it afterwards without re-running).

2. Run the installer:

   ```sh
   cd /path/to/pacekeeper/tools/calendar_feed
   ./install.sh
   ```

   It creates the venv, installs the two packages, copies `calendar.env.example` to
   `~/.config/pacekeeper/calendar.env` (mode 600) if it is missing, renders and loads the
   launch agent, then prints `/health`.

3. Get the secret iCal address: Google Calendar → **Settings** → pick the calendar in the
   left sidebar → **Integrate calendar** → **Secret address in iCal format**. Copy it.

   > Anyone holding this URL can read the entire calendar. Keep it in `calendar.env`
   > only — never in the repo, an issue, or a chat.

4. Edit `~/.config/pacekeeper/calendar.env` — `ICS_URL` ships **commented out**, so an
   unedited copy fails clearly (`ICS_URL is not set`) rather than trying `CHANGE-ME` as a
   real URL. Uncomment it and fill it in:

   ```sh
   ICS_URL=https://calendar.google.com/calendar/ical/…/basic.ics
   MY_EMAIL=you@work.example.com
   #TOKEN=          # optional, see Privacy below
   ```

5. Restart the agent so it picks the file up:

   ```sh
   launchctl kickstart -k gui/$(id -u)/com.pacekeeper.calendar-feed
   ```

6. Check it, from the mini or any machine on the LAN:

   ```sh
   curl -s http://<mac-mini-ip>:8765/health
   curl -s http://<mac-mini-ip>:8765/calendar.json
   ```

   Both open in a browser too. `/calendar.json` should show your next few meetings.

## Point the Dial at the mini by name, not by IP

The Dial's `CALENDAR_URL` is baked in at build time, but it does not need to be an IP
address. The Dial's `CALENDAR_URL` fetch uses a plain `WiFiClient`, and a plain
`WiFiClient` resolves hostnames through **unicast DNS** — whatever server the router hands
out over DHCP, normally the router itself. That is different from **mDNS**
(`http://mac-mini.local:8765/…`), which only Bonjour-aware stacks (like a Mac's) speak;
the Dial's HTTP client does not, and never will — `.local` names still do not work here.

This home router happens to also answer unicast DNS queries for its DHCP clients by
hostname, under the `.lan` suffix — so `macmini.lan` resolves for the Dial exactly the way
it resolves for any other unicast-DNS client. Verified from another machine on the same
network:

```
$ nslookup macmini.lan 192.168.1.1
Server:		192.168.1.1
Address:	192.168.1.1#53

Name:	macmini.lan
Address: 192.168.1.142
```

So `src/config.h` should point at the name, not an IP that can (and, on 2026-09-07, did —
three times) drift out from under it:

```c
#define CALENDAR_URL   "http://macmini.lan:8765/calendar.json"
// #define CALENDAR_TOKEN "…"   // optional; only if TOKEN is set in calendar.env
```

If this router is ever swapped for one that does not do DHCP-hostname DNS, or the mini is
moved to a network that does not, fall back to a fixed IP: on the router, find the mini's
Wi-Fi/Ethernet MAC and add a **DHCP reservation** for it (often "Static DHCP", "Address
reservation" or "DHCP fixed IP"). A reservation is better than a manually configured
static IP: the mini keeps using DHCP, so DNS and gateway stay correct, and the router will
not hand the address to anything else. Then use that IP in `CALENDAR_URL` instead of the
hostname.

> **A DHCP reservation will not stick if macOS is rotating the mini's MAC address.**
> "Private Wi-Fi Address" (System Settings → Wi-Fi → *(this network)* → Details) makes
> macOS present a different MAC on each join, which defeats a MAC-based reservation —
> this is what actually caused the mini's IP to change three times on 2026-09-07. Turn it
> **off** for this network before relying on a reservation (or on `macmini.lan` resolving
> to a stable address at all): Wi-Fi → Details → **Private Wi-Fi Address: Off**.

Then rebuild and flash. Without `CALENDAR_URL` the fetch service and the Calendar face
are compiled out; the menu entry stays and lands on the Clock face.

An `http://` URL uses a plain `WiFiClient`; `https://` still uses the pinned Google Trust
Services roots in `src/CalendarCerts.h`. When `CALENDAR_TOKEN` is defined the Dial appends
`?k=<token>`, and the relay checks it whenever `TOKEN` is set in `calendar.env`.

## Privacy

Without `TOKEN`, anyone on the LAN who finds port 8765 can read your next five meeting
titles. That may be fine on a home network. If it is not, generate a token
(`openssl rand -hex 24`), put it in `calendar.env` as `TOKEN=`, restart the agent, and set
the same string as `CALENDAR_TOKEN` in `src/config.h`. `/calendar.json` then answers
`{"error":"forbidden"}` (403) without the right `?k=`.

The traffic is plain HTTP on your own LAN, so the token is not secret against anyone
sniffing that LAN; it is a lock on the door, not a tunnel.

## Troubleshooting

```sh
tail -f ~/Library/Logs/pacekeeper-calendar.log
```

Each refresh logs `calendar refreshed: N event(s), B bytes` or
`calendar refresh failed (…)`. **The `.ics` URL is never logged**, and neither is the
`?k=` query string of an incoming request.

One-shot, outside launchd — fetches once, prints the JSON, exits non-zero on failure:

```sh
~/Library/Application\ Support/pacekeeper-calendar/venv/bin/python \
    /path/to/tools/calendar_feed/calendar_feed.py --once
```

| Symptom | Likely cause |
| --- | --- |
| `ICS_URL is not set` | `calendar.env`'s `ICS_URL` line is still commented out (the shipped default) or still has the placeholder, or the agent started before you edited it — `launchctl kickstart -k gui/$(id -u)/com.pacekeeper.calendar-feed` |
| `{"error":"no data"}` (503) | no fetch has succeeded yet; check the log |
| `HTTPError: HTTP Error 404` | the secret address was regenerated in Google Calendar; copy the new one |
| `port N already in use` in the log, relay not running | another process already has the port; the relay logs this and exits (`launchctl` will keep restarting and re-failing it) — free the port or change `PORT` |
| Dial shows `no calendar` | the mini is asleep or the agent is not running — `curl` `/health` from another machine; if `macmini.lan` itself does not resolve, check "Private Wi-Fi Address" (see above) and `nslookup macmini.lan <router-ip>` |
| Nothing on the LAN, works on the mini | `BIND` is set to `127.0.0.1`, or macOS's firewall is blocking the Python binary |
| Meetings an hour out | wrong `TZ` in `calendar.env` (default `Europe/London`) |

The relay keeps serving the **last good payload** when a fetch fails, with its original
`t`, so the Dial's `isStale()` reports `last update N min ago` rather than the card going
blank on one flaky fetch. A failed refresh retries in 60 s instead of the usual 120 s.

To stop or remove it:

```sh
launchctl bootout gui/$(id -u) ~/Library/LaunchAgents/com.pacekeeper.calendar-feed.plist
rm ~/Library/LaunchAgents/com.pacekeeper.calendar-feed.plist
```

### Log rotation

The plist logs stdout/stderr straight to `~/Library/Logs/pacekeeper-calendar.log` with no
rotation of its own — there is no `logrotate` on macOS, and this relay does not need
anything as heavyweight as a full `newsyslog.d` entry. At one line per 2-minute refresh
it grows slowly, but for an always-on process it is still worth bounding. Either:

- truncate it by hand occasionally (`launchctl kickstart` reopens the file, so this is
  safe with the agent running):

  ```sh
  : > ~/Library/Logs/pacekeeper-calendar.log
  ```

  (`truncate -s 0 ~/Library/Logs/pacekeeper-calendar.log` works the same way; either can
  be dropped in a monthly cron/launchd job), or

- let macOS's own log rotation daemon handle it, with a `newsyslog.d` drop-in:

  ```
  # /etc/newsyslog.d/pacekeeper-calendar.conf
  # logfilename                                mode count size(KB) when
  /Users/<you>/Library/Logs/pacekeeper-calendar.log  644  4     1024      *     J
  ```

  (keeps 4 rotated, gzip-compressed (`J`) copies, each capped at 1 MB — `man newsyslog.conf`
  for the field meanings). Requires `sudo` to install, since `/etc/newsyslog.d` is root-owned.

## Developing

```sh
python3 -m venv /tmp/cf
/tmp/cf/bin/pip install -r tools/calendar_feed/requirements-dev.txt
/tmp/cf/bin/python -m pytest tools/calendar_feed -q
```

The tests run `build_payload()` against a synthetic ICS at a fixed `now`
(Tuesday 2026-09-08 08:00 Europe/London) and drive the HTTP handler on an ephemeral port;
no network, no real calendar.
