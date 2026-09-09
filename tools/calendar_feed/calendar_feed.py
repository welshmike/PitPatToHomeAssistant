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
from PIL import Image

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

# Airline logos for the Flights card (spec 4.11, amended 2026-09-09). The Dial
# has no PSRAM and a PNG decode needs ~60 KB of heap it cannot spare, so the
# relay does the decoding: each logo is fetched once from pics.avs.io,
# composited on white at 90 % inside a 120x48 canvas (the same geometry the
# Dial used to render itself) and served as raw big-endian RGB565 — exactly
# LOGO_BYTES — which the Dial copies to its panel row by row.
LOGO_SOURCE = "https://pics.avs.io/120/48/{iata}.png"
LOGO_W, LOGO_H = 120, 48
LOGO_ART_W, LOGO_ART_H = 108, 43
LOGO_ART_X, LOGO_ART_Y = 6, 2
LOGO_BYTES = LOGO_W * LOGO_H * 2
LOGO_FETCH_TIMEOUT_S = 10
LOGO_MISS_TTL_S = 600
LOGO_ROUTE = re.compile(r"^/logo/([A-Z0-9]{2})\.565$")
DEFAULT_LOGO_DIR = os.path.expanduser(
    "~/Library/Application Support/pacekeeper-calendar/logos"
)
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


# --- airline logos ---------------------------------------------------------


def png_to_rgb565(png_bytes: bytes) -> bytes:
    """A 120x48 raw big-endian RGB565 image: the PNG (any size, alpha honoured)
    scaled to LOGO_ART_W x LOGO_ART_H and composited on white at
    (LOGO_ART_X, LOGO_ART_Y). Always exactly LOGO_BYTES long."""
    import io

    with Image.open(io.BytesIO(png_bytes)) as src:
        art = src.convert("RGBA").resize((LOGO_ART_W, LOGO_ART_H), Image.LANCZOS)
    canvas = Image.new("RGBA", (LOGO_W, LOGO_H), (255, 255, 255, 255))
    canvas.alpha_composite(art, (LOGO_ART_X, LOGO_ART_Y))
    out = bytearray(LOGO_BYTES)
    i = 0
    for r, g, b in canvas.convert("RGB").getdata():
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out[i] = v >> 8
        out[i + 1] = v & 0xFF
        i += 2
    return bytes(out)


def fetch_logo_png(iata: str, timeout: int = LOGO_FETCH_TIMEOUT_S) -> Optional[bytes]:
    """The airline's PNG from pics.avs.io, or None when it has none (404).
    Any other failure raises."""
    req = urllib.request.Request(
        LOGO_SOURCE.format(iata=iata), headers={"User-Agent": USER_AGENT}
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.read()
    except urllib.error.HTTPError as exc:
        if exc.code == 404:
            return None
        raise


class LogoStore(object):
    """Converted logos, cached on disk (forever — airline marks rarely change)
    and in memory; misses remembered for LOGO_MISS_TTL_S. Thread-safe:
    every request handler thread may call get()."""

    def __init__(self, cache_dir: str, fetcher=fetch_logo_png, clock=time.monotonic):
        self.cache_dir = cache_dir
        self.fetcher = fetcher
        self.clock = clock
        self._lock = threading.Lock()
        self._mem: Dict[str, bytes] = {}
        self._miss: Dict[str, float] = {}

    def _path(self, iata: str) -> str:
        return os.path.join(self.cache_dir, iata + ".565")

    def get(self, iata: str) -> Tuple[int, Optional[bytes]]:
        """(200, image) / (404, None) when the airline has no logo /
        (503, None) when it could not be fetched or converted right now."""
        with self._lock:
            data = self._mem.get(iata)
            if data is not None:
                return 200, data
            missed_at = self._miss.get(iata)
            if missed_at is not None and self.clock() - missed_at < LOGO_MISS_TTL_S:
                return 404, None
            path = self._path(iata)
            try:
                with open(path, "rb") as fh:
                    data = fh.read()
                if len(data) == LOGO_BYTES:
                    self._mem[iata] = data
                    return 200, data
                log.warning("logo %s: cached file is %d bytes, refetching", iata, len(data))
            except FileNotFoundError:
                pass
            except OSError as exc:
                log.warning("logo %s: cache read failed: %s", iata, type(exc).__name__)
            try:
                png = self.fetcher(iata)
            except Exception as exc:  # network, DNS, 5xx: transient
                log.warning("logo %s: fetch failed: %s", iata, type(exc).__name__)
                return 503, None
            if png is None:
                log.info("logo %s: no logo at source (404)", iata)
                self._miss[iata] = self.clock()
                return 404, None
            try:
                data = png_to_rgb565(png)
            except Exception as exc:
                log.warning("logo %s: conversion failed: %s", iata, type(exc).__name__)
                return 503, None
            self._mem[iata] = data
            try:
                os.makedirs(self.cache_dir, exist_ok=True)
                tmp = path + ".tmp"
                with open(tmp, "wb") as fh:
                    fh.write(data)
                os.replace(tmp, path)
            except OSError as exc:
                log.warning("logo %s: cache write failed: %s", iata, type(exc).__name__)
            log.info("logo %s: fetched and converted (%d bytes)", iata, len(data))
            return 200, data


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


def make_handler(feed: Feed, token: Optional[str], logos: Optional[LogoStore] = None):
    class Handler(BaseHTTPRequestHandler):
        server_version = "PaceKeeperCalendarRelay/1.1"
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
            elif logos is not None and LOGO_ROUTE.match(route):
                # Public airline marks: no token. The Dial caches the file
                # itself, so no-store here costs nothing.
                iata = LOGO_ROUTE.match(route).group(1)
                status, data = logos.get(iata)
                if status == 200:
                    self._send(200, data, "application/octet-stream")
                elif status == 404:
                    self._send(404, b'{"error":"no logo"}')
                else:
                    self._send(503, b'{"error":"logo unavailable"}')
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
    logos = LogoStore(config.get("LOGO_DIR") or DEFAULT_LOGO_DIR)
    try:
        httpd = ThreadingHTTPServer((bind, port), make_handler(feed, token, logos))
    except OSError:
        feed.stop()
        log.error("port %d already in use", port)
        sys.exit(1)
    httpd.daemon_threads = True
    log.info(
        "serving /calendar.json and /logo/XX.565 on %s:%d (token %s, refresh %ds, logos in %s)",
        bind,
        port,
        "required" if token else "off",
        REFRESH_S,
        logos.cache_dir,
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
