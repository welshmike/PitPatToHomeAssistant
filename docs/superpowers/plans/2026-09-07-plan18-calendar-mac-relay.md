# Plan 18: Calendar feed via the Mac mini — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the Apps Script feed with a Python relay on the Mac mini that turns the calendar's secret `.ics` into the Dial's compact JSON over plain LAN HTTP, and let the firmware fetch `http://` URLs.

**Architecture:** `tools/calendar_feed/calendar_feed.py` = fetch + expand + serve, one process, stdlib `http.server` in a thread, refresh loop every 5 min; pure function `build_payload(ics_bytes, now, my_email)` unit-tested with pytest on a synthetic ICS. Firmware: `CalendarService::fetchNow()` picks `WiFiClient` vs `WiFiClientSecure` from the URL scheme; `CALENDAR_TOKEN` optional. Docs swap Apps Script for the relay.

**Tech Stack:** Python 3.9+ (macOS system python3 or Homebrew), `icalendar`, `recurring-ical-events`, `pytest` (dev only); PlatformIO (`pio` at `~/Library/Python/3.9/bin/pio`), envs `native` (348 tests) and `dial-ota`. Spec: §4.19 + its "feed source" amendment in `docs/superpowers/specs/2026-09-03-m5dial-migration-design.md`.

## Global Constraints

- Payload unchanged: `{"t":<unix now>,"ev":[{"s","e","n"(≤40 ASCII),"a"0|1,"l"(≤24 ASCII)}]}`, ≤ 5 events, window now → end of tomorrow (Europe/London), declined events dropped, sorted by start, all-day events carry `a:1` with `s`/`e` = local midnight boundaries as UTC epochs.
- Relay config file `~/.config/pacekeeper/calendar.env` (KEY=VALUE lines): `ICS_URL` (required), `MY_EMAIL` (optional), `PORT` (default 8765), `TOKEN` (optional; when set, requests need `?k=<TOKEN>` else 403). The `.ics` URL is never logged.
- Relay serves `GET /calendar.json` (200, `application/json`, `Cache-Control: no-store`) and `GET /health` (200 `ok` + last fetch age); everything else 404. Fetch every 300 s; on fetch/parse failure keep serving the last good payload and log a warning; `t` is the time of the last *successful* build so the Dial's 30-min staleness still works.
- Firmware: `CALENDAR_URL` may be `http://host:port/path` or `https://…`; `CALENDAR_TOKEN` optional; behaviour otherwise unchanged (5 min poll, back-off, heap guard, 12 s budget, HTTP/1.0). Native tests stay 348; both `dial-ota` variants build. Never stage `src/config.h`. Commit messages end with `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`. Do not upload.

---

### Task 1: The relay — `tools/calendar_feed/`

**Files:**
- Create: `tools/calendar_feed/calendar_feed.py`, `tools/calendar_feed/requirements.txt`, `tools/calendar_feed/test_calendar_feed.py`, `tools/calendar_feed/com.pacekeeper.calendar-feed.plist.template`, `tools/calendar_feed/install.sh`, `tools/calendar_feed/calendar.env.example`
- Create: `doc/CALENDAR_FEED.md`
- Delete: `doc/calendar_apps_script.gs`, `doc/CALENDAR_APPS_SCRIPT.md`

**Interfaces (Produces):** `build_payload(ics_bytes: bytes, now: datetime, my_email: str | None, tz_name="Europe/London") -> dict` (the JSON object); `serve(config)`.

- [ ] **Step 1: `requirements.txt`** — `icalendar>=5.0` and `recurring-ical-events>=2.1` (pin the current majors; `pytest` goes in `requirements-dev.txt`).

- [ ] **Step 2: Failing tests — `test_calendar_feed.py`** (run with `python3 -m pytest tools/calendar_feed -q` inside the venv). Build a synthetic ICS string with: a weekly recurring 30-min "Standup" at 09:30 Europe/London with `RRULE:FREQ=WEEKLY;BYDAY=MO,TU,WE,TH,FR` starting months ago; one instance moved via `RECURRENCE-ID` to 10:00 today; a one-off "1:1" today at 14:00–15:00 with `ATTENDEE;PARTSTAT=DECLINED:mailto:me@example.com`; an all-day "Offsite" tomorrow (`DTSTART;VALUE=DATE`); a one-off with a 60-char title and a `LOCATION` of 40 chars; a past event yesterday; a `meet.google.com` link in DESCRIPTION for the Standup. Fix `now` = a Tuesday 08:00 Europe/London. Assert: the past event is absent; the declined 1:1 is absent when `my_email="me@example.com"` and present when `my_email=None`; today's Standup instance is at 10:00 (the override), not 09:30; tomorrow's Standup 09:30 is present and Wednesday's… only through end of tomorrow; the all-day Offsite has `a==1`, `s` = tomorrow 00:00 London as UTC epoch, `e` = the day after 00:00; titles clipped to 40 ASCII chars ending `...`, `l` to 24; `l == "Google Meet"` for the Standup; at most 5 events, sorted by `s`; `t == int(now.timestamp())`; non-ASCII stripped.

- [ ] **Step 3: `calendar_feed.py`** — structure:
```python
#!/usr/bin/env python3
"""PaceKeeper Dial calendar relay (spec 4.19, feed-source amendment)."""
import json, os, sys, time, threading, logging, urllib.request
from datetime import datetime, timedelta, date, timezone
from zoneinfo import ZoneInfo
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import icalendar, recurring_ical_events

REFRESH_S = 300; MAX_EVENTS = 5; TITLE_MAX = 40; WHERE_MAX = 24

def load_env(path=os.path.expanduser("~/.config/pacekeeper/calendar.env")) -> dict: ...  # KEY=VALUE, ignore blanks/#
def ascii_clip(s: str, n: int) -> str: ...  # strip non-printable/non-ASCII, collapse whitespace, clip to n with "..."
def where_of(ev) -> str: ...               # LOCATION unless it is a URL; else Google Meet / Zoom / Teams from DESCRIPTION+LOCATION; else ""
def declined_by(ev, my_email) -> bool: ... # any ATTENDEE whose value endswith my_email (case-insensitive) with PARTSTAT=DECLINED
def build_payload(ics_bytes, now, my_email, tz_name="Europe/London") -> dict:
    tz = ZoneInfo(tz_name); cal = icalendar.Calendar.from_ical(ics_bytes)
    local_now = now.astimezone(tz); end = (local_now + timedelta(days=1)).replace(hour=23, minute=59, second=59, microsecond=0)
    out = []
    for ev in recurring_ical_events.of(cal).between(local_now, end):
        if my_email and declined_by(ev, my_email): continue
        ds, de = ev["DTSTART"].dt, ev.get("DTEND", ev["DTSTART"]).dt
        all_day = isinstance(ds, date) and not isinstance(ds, datetime)
        if all_day:
            s = datetime.combine(ds, datetime.min.time(), tz); e = datetime.combine(de, datetime.min.time(), tz)
        else:
            s = ds if ds.tzinfo else ds.replace(tzinfo=tz); e = de if de.tzinfo else de.replace(tzinfo=tz)
        if e <= now: continue   # (between() already excludes these; belt and braces)
        out.append({"s": int(s.timestamp()), "e": int(e.timestamp()), "n": ascii_clip(str(ev.get("SUMMARY", "(no title)")), TITLE_MAX),
                    "a": 1 if all_day else 0, "l": ascii_clip(where_of(ev), WHERE_MAX)})
    out.sort(key=lambda x: x["s"])
    return {"t": int(now.timestamp()), "ev": out[:MAX_EVENTS]}
```
plus `fetch_ics(url, timeout=20) -> bytes` (urllib, `User-Agent: PaceKeeper-Dial-relay`), a `Feed` class holding `payload_bytes`, `last_ok`, `last_error` under a lock with `refresh()` and a daemon loop, a `Handler(BaseHTTPRequestHandler)` for `/calendar.json` (token check when `TOKEN` set) and `/health`, logging via `logging` to stderr (launchd captures to the log path), `main()` with `--once` (print JSON and exit, for testing) and `--port`. Never print `ICS_URL`.

- [ ] **Step 4: `install.sh`** — bash, idempotent: `set -euo pipefail`; `DIR=$(cd "$(dirname "$0")" && pwd)`; creates `~/Library/Application Support/pacekeeper-calendar/venv` with `python3 -m venv`, `pip install -r requirements.txt`; creates `~/.config/pacekeeper/calendar.env` from `calendar.env.example` if missing and tells the user to edit it; renders the plist template (substituting `__PYTHON__` = venv python, `__SCRIPT__` = absolute path of `calendar_feed.py`, `__HOME__`) into `~/Library/LaunchAgents/com.pacekeeper.calendar-feed.plist`; `launchctl bootout gui/$(id -u) <plist>` (ignore failure) then `launchctl bootstrap gui/$(id -u) <plist>`; prints `curl -s http://localhost:8765/health` result. Plist: `RunAtLoad`, `KeepAlive`, `StandardOutPath`/`StandardErrorPath` under `~/Library/Logs/pacekeeper-calendar.log`, `EnvironmentVariables` `PYTHONUNBUFFERED=1`.

- [ ] **Step 5: `doc/CALENDAR_FEED.md`** — why (company policy, 2 MB ICS), what runs where, install steps on the Mac mini (clone or copy `tools/calendar_feed`, run `install.sh`, edit `calendar.env` with the secret iCal address from Google Calendar → Settings → Integrate calendar → Secret address, `launchctl kickstart -k gui/$(id -u)/com.pacekeeper.calendar-feed`, check `/health` and `/calendar.json` in a browser), the `config.h` lines (`CALENDAR_URL "http://<mac-mini-ip>:8765/calendar.json"`, optional `CALENDAR_TOKEN`), how to give the mini a fixed IP (DHCP reservation), troubleshooting (log path, `--once`), privacy note (titles are visible to anyone on the LAN unless `TOKEN` is set). Delete the two Apps Script files.

- [ ] **Step 6: Run** the pytest suite in a venv (`python3 -m venv /tmp/cf && /tmp/cf/bin/pip install -r tools/calendar_feed/requirements.txt pytest && /tmp/cf/bin/python -m pytest tools/calendar_feed -q`) → all pass; `python3 -m pyflakes` or `python3 -m py_compile` on the script. **Commit** (`tools/calendar_feed/*`, `doc/CALENDAR_FEED.md`, deletions) — `"Calendar: Mac-mini ICS relay replaces the Apps Script feed (spec 4.19 amendment)"`.

---

### Task 2: Firmware `http://` support, optional token, docs

**Files:**
- Modify: `src/CalendarService.h/.cpp` (scheme detection; `WiFiClient` path; token optional), `src/config.h.example` (new key comments), `README.md` (Calendar section: relay, `http://`, link to `doc/CALENDAR_FEED.md`; remove Apps Script mentions), `doc/AUDIT_2026-09-03.md` (Phase L: "pulling the Apps Script deployment" → "stopping the relay (`launchctl bootout …`)"), spec §4.19 As-built (one line: `http://` path added, token optional).

- [ ] **Step 1: `CalendarService`** — `static constexpr bool kIsHttps = (CALENDAR_URL[0]=='h' && CALENDAR_URL[4]=='s');` (constexpr string test on the literal; or `strncmp` at runtime once); in `fetchNow()`: `WiFiClient plain; WiFiClientSecure tls; Client* client = kIsHttps ? (setCACert…, &tls) : &plain;` with both objects local (so heap for TLS is only allocated on the https path — `WiFiClientSecure` allocates on connect, not construction; confirm and note); `http.begin(*client, kUrl)`. Token: `#ifdef CALENDAR_TOKEN` → `kUrl = CALENDAR_URL "?k=" CALENDAR_TOKEN` else `kUrl = CALENDAR_URL`; remove the `#error` that required `CALENDAR_TOKEN`. Comments updated (the redirect/chunked notes apply to the https path; the relay answers HTTP/1.0 with `Content-Length`). Keep `useHTTP10(true)`.
- [ ] **Step 2: Build both variants** (with the placeholder defines, and with them commented out) → SUCCESS; native 348. Set the local placeholder `CALENDAR_URL` to an `http://192.0.2.1:8765/calendar.json` form for the with-calendar build so the plain path compiles — then restore whatever was there, never stage `src/config.h`.
- [ ] **Step 3: Docs** as listed. **Commit** — `"Calendar: fetch over plain HTTP from the LAN relay; token optional; docs"`.
