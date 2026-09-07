#!/usr/bin/env python3
"""PaceKeeper Dial calendar relay (spec 4.19, feed-source amendment).

Fetches a Google Calendar secret iCal address every 2 minutes, expands
recurrences for now -> end of tomorrow in Europe/London, drops declined
events, and serves the Dial's compact JSON on the LAN:

    {"t": <unix now>, "ev": [{"s": <start>, "e": <end>, "n": "<title>",
                              "a": 0|1, "l": "<where>"}, ...]}   (<= 5 events)

Configuration lives in ~/.config/pacekeeper/calendar.env and never in the
repo. The .ics URL is a secret: it is never logged and never echoed.

Run `calendar_feed.py --once` to print one payload and exit (non-zero on a
fetch or parse failure).
"""

import argparse
import hmac
import json
import logging
import os
import re
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import date, datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Dict, List, Optional, Tuple
from zoneinfo import ZoneInfo

import icalendar
import recurring_ical_events

REFRESH_S = 120
RETRY_S = 60
MAX_EVENTS = 5
TITLE_MAX = 40
WHERE_MAX = 24
DEFAULT_PORT = 8765
DEFAULT_TZ = "Europe/London"
FETCH_TIMEOUT_S = 20
MAX_ICS_BYTES = 32 * 1024 * 1024
USER_AGENT = "PaceKeeper-Dial-relay"
DEFAULT_ENV = os.path.expanduser("~/.config/pacekeeper/calendar.env")

log = logging.getLogger("calendar_feed")

_WS = re.compile(r"\s+")
_URL = re.compile(r"^https?://", re.I)


# --- configuration --------------------------------------------------------


def load_env(path: str = DEFAULT_ENV) -> Dict[str, str]:
    """Reads a KEY=VALUE file. Blank lines, `#` comments and lines without an
    `=` are ignored; surrounding quotes are stripped. A missing file is {}."""
    out = {}  # type: Dict[str, str]
    try:
        with open(path, "r", encoding="utf-8") as fh:
            lines = fh.readlines()
    except FileNotFoundError:
        return out
    for line in lines:
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        key = key.strip()
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in ("'", '"'):
            value = value[1:-1]
        if key:
            out[key] = value
    return out


def load_config(env_path: str = DEFAULT_ENV) -> Dict[str, str]:
    """Env file first, real environment variables win (handy for testing)."""
    cfg = load_env(env_path)
    for key in ("ICS_URL", "MY_EMAIL", "PORT", "TOKEN", "TZ", "BIND"):
        if os.environ.get(key):
            cfg[key] = os.environ[key]
    return cfg


# --- payload --------------------------------------------------------------


def ascii_clip(s: Any, n: int) -> str:
    """Collapses whitespace, strips everything outside printable ASCII, and
    clips to `n` *bytes* — which is what the Dial's fixed char[41]/char[25]
    fields hold. Stripping non-ASCII first means one character is always one
    byte, so a slice can never cut a UTF-8 sequence in half; the overflow
    marker is three ASCII dots for the same reason (U+2026 is not ASCII)."""
    s = _WS.sub(" ", str(s))
    s = "".join(c for c in s if " " <= c <= "~")
    s = _WS.sub(" ", s).strip()
    if len(s) <= n:
        return s
    if n <= 3:
        return s[:n]
    return s[: n - 3].rstrip() + "..."


def where_of(ev: Any) -> str:
    """LOCATION unless it is bare URL; then the meeting host named in
    DESCRIPTION + LOCATION; else "". Mirrors the old Apps Script's whereOf()."""
    loc = str(ev.get("LOCATION", "") or "").strip()
    if loc and not _URL.match(loc):
        return loc
    haystack = str(ev.get("DESCRIPTION", "") or "") + " " + loc
    for pattern, name in (
        (r"meet\.google\.com", "Google Meet"),
        (r"zoom\.us", "Zoom"),
        (r"teams\.microsoft\.com", "Teams"),
    ):
        if re.search(pattern, haystack, re.I):
            return name
    return ""


def declined_by(ev: Any, my_email: Optional[str]) -> bool:
    """True when an ATTENDEE address ending in `my_email` has PARTSTAT=DECLINED."""
    if not my_email:
        return False
    att = ev.get("ATTENDEE")
    if att is None:
        return False
    if not isinstance(att, list):
        att = [att]
    want = my_email.strip().lower()
    for a in att:
        if not str(a).strip().lower().endswith(want):
            continue
        params = getattr(a, "params", None) or {}
        if str(params.get("PARTSTAT", "")).upper() == "DECLINED":
            return True
    return False


def window_end(local_now: datetime, tz) -> datetime:
    """End of *tomorrow* in `tz` — the far edge of the Dial's feed window."""
    return (local_now.astimezone(tz) + timedelta(days=1)).replace(
        hour=23, minute=59, second=59, microsecond=0
    )


def _as_aware(value, tz) -> datetime:
    """iCalendar values arrive as `date` (all-day), aware `datetime` (TZID or
    UTC) or naive `datetime` (floating). Normalise to an aware datetime."""
    if isinstance(value, datetime):
        return value if value.tzinfo is not None else value.replace(tzinfo=tz)
    return datetime.combine(value, datetime.min.time(), tz)


def _drop_unparseable_vevents(cal: "icalendar.Calendar") -> int:
    """icalendar silently drops a VEVENT property it cannot parse (e.g. a
    malformed `DTSTART:NOTADATE`), leaving the component with no DTSTART key
    at all. recurring_ical_events then raises a bare KeyError for the whole
    calendar rather than just that one event. Strip such VEVENTs before
    expansion so one broken meeting cannot take the rest of the feed down
    with it. Returns the number of components dropped (for a log line, never
    the event content itself)."""
    kept = []
    dropped = 0
    for comp in cal.subcomponents:
        if comp.name == "VEVENT" and "DTSTART" not in comp:
            dropped += 1
            continue
        kept.append(comp)
    cal.subcomponents = kept
    return dropped


def build_payload(
    ics_bytes: bytes,
    now: datetime,
    my_email: Optional[str],
    tz_name: str = DEFAULT_TZ,
) -> Dict[str, Any]:
    """The whole feed, from raw .ics to the object the Dial parses."""
    tz = ZoneInfo(tz_name)
    if now.tzinfo is None:
        now = now.replace(tzinfo=tz)
    local_now = now.astimezone(tz)
    end = window_end(local_now, tz)

    cal = icalendar.Calendar.from_ical(ics_bytes)
    dropped = _drop_unparseable_vevents(cal)
    if dropped:
        log.warning("skipping %d unparseable VEVENT(s)", dropped)

    try:
        # between() expands RRULEs, honours RECURRENCE-ID overrides and
        # EXDATEs, and already keeps events that are under way but not those
        # that ended. skip_bad_series drops individual bad RRULE/series
        # errors instead of aborting the whole expansion.
        occurrences = recurring_ical_events.of(cal, skip_bad_series=True).between(
            local_now, end
        )
    except Exception as exc:  # noqa: BLE001 - one bad calendar must not 500
        log.warning("recurrence expansion failed: %s", type(exc).__name__)
        occurrences = []

    out = []  # type: List[Dict[str, Any]]
    for ev in occurrences:
        try:
            if str(ev.get("STATUS", "") or "").strip().upper() == "CANCELLED":
                continue
            if declined_by(ev, my_email):
                continue
            ds = ev["DTSTART"].dt
            de = ev.get("DTEND", ev["DTSTART"]).dt
            all_day = isinstance(ds, date) and not isinstance(ds, datetime)
            start = _as_aware(ds, tz)
            finish = _as_aware(de, tz)
            if finish <= start:  # zero (or negative) length - nothing to show
                continue
            if finish <= now:  # belt and braces; between() excludes these already
                continue
            entry = {
                "s": int(start.timestamp()),
                "e": int(finish.timestamp()),
                "n": ascii_clip(ev.get("SUMMARY", "(no title)"), TITLE_MAX)
                or "(no title)",
                "a": 1 if all_day else 0,
                "l": ascii_clip(where_of(ev), WHERE_MAX),
            }
        except Exception as exc:  # noqa: BLE001 - one bad event must not drop the rest
            log.warning("skipping malformed event: %s", type(exc).__name__)
            continue
        out.append(entry)
    out.sort(key=lambda x: (x["s"], x["e"], x["n"]))
    return {"t": int(now.timestamp()), "ev": out[:MAX_EVENTS]}


def payload_json(payload: Dict[str, Any]) -> bytes:
    """Compact ASCII JSON — the Dial reads at most 2 KB into a static buffer."""
    return json.dumps(payload, separators=(",", ":"), ensure_ascii=True).encode("ascii")


# --- fetching -------------------------------------------------------------


def fetch_ics(url: str, timeout: int = FETCH_TIMEOUT_S) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        body = resp.read(MAX_ICS_BYTES + 1)
    if len(body) > MAX_ICS_BYTES:
        raise ValueError("calendar is larger than %d bytes" % MAX_ICS_BYTES)
    return body


def _redact(text: str, secret: Optional[str]) -> str:
    """urllib puts the URL it failed on into its message, sometimes whole and
    sometimes only the path. The .ics address is the only secret here, so
    every form of it is scrubbed before it reaches a log line or /health."""
    text = str(text)
    if not secret:
        return text
    parts = urllib.parse.urlsplit(secret)
    for candidate in (
        secret,
        urllib.parse.quote(secret, safe=""),
        secret.split("://", 1)[-1],
        parts.path + ("?" + parts.query if parts.query else ""),
        parts.path,
        parts.query,
    ):
        if candidate and len(candidate) > 4:
            text = text.replace(candidate, "<ics-url>")
    return text


class Feed(object):
    """Holds the last good payload. A failed refresh keeps serving it (with
    its original `t`, so the Dial's isStale() can notice), and records why."""

    def __init__(self, config, fetcher=fetch_ics, clock=None):
        self._config = dict(config)
        self._fetch = fetcher
        self._clock = clock or (lambda: datetime.now(timezone.utc))
        self._lock = threading.Lock()
        self._body = None  # type: Optional[bytes]
        self._count = 0
        self.last_ok = 0
        self.last_error = ""
        self.last_error_type = ""
        self._stop = threading.Event()

    @property
    def url(self) -> str:
        return self._config.get("ICS_URL", "")

    def snapshot(self) -> Tuple[Optional[bytes], int]:
        with self._lock:
            return self._body, self.last_ok

    def refresh(self) -> bool:
        url = self.url
        try:
            if not url:
                raise ValueError("ICS_URL is not set")
            ics = self._fetch(url, timeout=FETCH_TIMEOUT_S)
            payload = build_payload(
                ics,
                self._clock(),
                self._config.get("MY_EMAIL") or None,
                self._config.get("TZ") or DEFAULT_TZ,
            )
            body = payload_json(payload)
        except Exception as exc:  # noqa: BLE001 - any failure keeps the old body
            # The full, redacted message is only ever logged. /health only
            # ever gets the exception's class name (see health() below) -
            # the message can quote raw calendar content (icalendar puts the
            # offending content line, e.g. a SUMMARY, straight into some of
            # its ValueErrors), and /health needs no token to read.
            message = "%s: %s" % (type(exc).__name__, _redact(exc, url))
            with self._lock:
                self.last_error = message
                self.last_error_type = type(exc).__name__
            log.warning("calendar refresh failed (%s)", message)
            return False
        with self._lock:
            self._body = body
            self._count = len(payload["ev"])
            self.last_ok = payload["t"]
            self.last_error = ""
            self.last_error_type = ""
        log.info("calendar refreshed: %d event(s), %d bytes", self._count, len(body))
        return True

    def health(self) -> Dict[str, Any]:
        """Deliberately thin: no token guards /health, so nothing here may
        reveal calendar content. `last_error` is the failing exception's
        class name only (e.g. "ValueError"), never its message."""
        with self._lock:
            ok = self._body is not None
            age = int(time.time()) - self.last_ok if self.last_ok else -1
            # A body old enough to have missed at least one refresh cycle -
            # or one that was never fetched at all (age < 0) - counts stale.
            stale = age < 0 or age > REFRESH_S * 2
            return {
                "ok": ok,
                "stale": stale,
                "age_s": age,
                "last_error": self.last_error_type,
            }

    def run_forever(self) -> None:
        while not self._stop.is_set():
            ok = self.refresh()
            self._stop.wait(REFRESH_S if ok else RETRY_S)

    def start(self) -> threading.Thread:
        thread = threading.Thread(target=self.run_forever, name="feed", daemon=True)
        thread.start()
        return thread

    def stop(self) -> None:
        self._stop.set()


# --- serving --------------------------------------------------------------


def token_ok(token: Optional[str], path: str) -> bool:
    """No TOKEN configured means no check; otherwise `?k=` must match.

    hmac.compare_digest() raises TypeError on a `str` argument that is not
    pure ASCII, so both sides are encoded to bytes first - a non-ASCII
    TOKEN (or query value) must fail the comparison, never crash it."""
    if not token:
        return True
    token_bytes = token.encode("utf-8", "surrogateescape")
    query = urllib.parse.urlparse(path).query
    for value in urllib.parse.parse_qs(query).get("k", []):
        if hmac.compare_digest(value.encode("utf-8", "surrogateescape"), token_bytes):
            return True
    return False


def make_handler(feed: Feed, token: Optional[str]):
    class Handler(BaseHTTPRequestHandler):
        server_version = "PaceKeeperCalendarRelay/1.0"
        protocol_version = "HTTP/1.1"

        def _send(self, status, body, content_type="application/json"):
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            # The Dial must never be handed a cached meeting list.
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(body)

        def do_GET(self):  # noqa: N802 - BaseHTTPRequestHandler's naming
            route = urllib.parse.urlparse(self.path).path
            if route == "/calendar.json":
                if not token_ok(token, self.path):
                    self._send(403, b'{"error":"forbidden"}')
                    return
                body, _ = feed.snapshot()
                if body is None:
                    self._send(503, b'{"error":"no data"}')
                    return
                self._send(200, body)
            elif route == "/health":
                self._send(200, payload_json(feed.health()))
            else:
                self._send(404, b'{"error":"not found"}')

        do_HEAD = do_GET

        def log_message(self, fmt, *args):
            # Deliberately drops the query string: it carries TOKEN.
            log.info(
                "%s %s", self.address_string(), urllib.parse.urlparse(self.path).path
            )

    return Handler


def serve(config: Dict[str, str]) -> int:
    port = int(config.get("PORT") or DEFAULT_PORT)
    bind = config.get("BIND") or "0.0.0.0"
    token = config.get("TOKEN") or None
    feed = Feed(config)
    feed.start()
    try:
        httpd = ThreadingHTTPServer((bind, port), make_handler(feed, token))
    except OSError:
        feed.stop()
        log.error("port %d already in use", port)
        sys.exit(1)
    httpd.daemon_threads = True
    log.info(
        "serving /calendar.json on %s:%d (token %s, refresh %ds)",
        bind,
        port,
        "required" if token else "off",
        REFRESH_S,
    )
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        log.info("stopping")
    finally:
        feed.stop()
        httpd.server_close()
    return 0


# --- entry point ----------------------------------------------------------


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="PaceKeeper Dial calendar relay")
    parser.add_argument("--env", default=DEFAULT_ENV, help="path to calendar.env")
    parser.add_argument("--port", type=int, default=None, help="override PORT")
    parser.add_argument(
        "--once",
        action="store_true",
        help="fetch once, print the JSON and exit (non-zero on failure)",
    )
    parser.add_argument("--verbose", action="store_true", help="debug logging")
    args = parser.parse_args(argv)

    logging.basicConfig(
        stream=sys.stderr,
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
    )

    config = load_config(args.env)
    if args.port is not None:
        config["PORT"] = str(args.port)
    if not config.get("ICS_URL"):
        log.error("ICS_URL is not set in %s", args.env)
        return 2

    if args.once:
        feed = Feed(config)
        if not feed.refresh():
            sys.stderr.write(feed.last_error + "\n")
            return 1
        body, _ = feed.snapshot()
        sys.stdout.write(body.decode("ascii") + "\n")
        return 0

    return serve(config)


if __name__ == "__main__":
    sys.exit(main())
