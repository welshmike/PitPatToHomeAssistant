"""Tests for the PaceKeeper Dial calendar relay (spec 4.19, feed-source amendment).

Run inside the venv:  python -m pytest tools/calendar_feed -q

`now` is fixed at Tuesday 2026-09-08 08:00 Europe/London (BST, UTC+1), so the
feed window is  now -> Wednesday 2026-09-09 23:59:59 local.
"""

from datetime import datetime, timedelta
from zoneinfo import ZoneInfo

import pytest
import time as time_module

import calendar_feed


LONDON = ZoneInfo("Europe/London")
NOW = datetime(2026, 9, 8, 8, 0, 0, tzinfo=LONDON)  # a Tuesday
ME = "me@example.com"

# Exactly 60 characters, so it must be clipped to 40 with a trailing "...".
LONG_TITLE = "Quarterly business review with the extended leadership teams"
# Exactly 40 characters, so it must be clipped to 24.
LONG_LOCATION = "Building 3 Level 2 Meeting Room Ada Love"

# A weekly weekday standup that started months ago (2026-06-01 is a Monday);
# today's instance is moved to 10:00 by a RECURRENCE-ID override.
ICS = """BEGIN:VCALENDAR
VERSION:2.0
PRODID:-//PaceKeeper//test//EN
CALSCALE:GREGORIAN
BEGIN:VEVENT
UID:standup@example.com
DTSTAMP:20260901T080000Z
DTSTART;TZID=Europe/London:20260601T093000
DTEND;TZID=Europe/London:20260601T100000
RRULE:FREQ=WEEKLY;BYDAY=MO,TU,WE,TH,FR
SUMMARY:Standup
DESCRIPTION:Join at https://meet.google.com/abc-defg-hij
END:VEVENT
BEGIN:VEVENT
UID:standup@example.com
RECURRENCE-ID;TZID=Europe/London:20260908T093000
DTSTAMP:20260901T080000Z
DTSTART;TZID=Europe/London:20260908T100000
DTEND;TZID=Europe/London:20260908T103000
SUMMARY:Standup
DESCRIPTION:Join at https://meet.google.com/abc-defg-hij
END:VEVENT
BEGIN:VEVENT
UID:one-to-one@example.com
DTSTAMP:20260901T080000Z
DTSTART;TZID=Europe/London:20260908T140000
DTEND;TZID=Europe/London:20260908T150000
SUMMARY:1:1
ATTENDEE;CN=Someone;PARTSTAT=ACCEPTED:mailto:boss@example.com
ATTENDEE;CN=Me;PARTSTAT=DECLINED:mailto:ME@Example.com
END:VEVENT
BEGIN:VEVENT
UID:long@example.com
DTSTAMP:20260901T080000Z
DTSTART;TZID=Europe/London:20260908T113000
DTEND;TZID=Europe/London:20260908T120000
SUMMARY:{long_title}
LOCATION:{long_location}
END:VEVENT
BEGIN:VEVENT
UID:accents@example.com
DTSTAMP:20260901T080000Z
DTSTART;TZID=Europe/London:20260908T160000
DTEND;TZID=Europe/London:20260908T163000
SUMMARY:Café — naïve résumé ☕
END:VEVENT
BEGIN:VEVENT
UID:offsite@example.com
DTSTAMP:20260901T080000Z
DTSTART;VALUE=DATE:20260909
DTEND;VALUE=DATE:20260910
SUMMARY:Offsite
END:VEVENT
BEGIN:VEVENT
UID:past@example.com
DTSTAMP:20260901T080000Z
DTSTART;TZID=Europe/London:20260907T140000
DTEND;TZID=Europe/London:20260907T150000
SUMMARY:Yesterday retro
END:VEVENT
END:VCALENDAR
""".format(long_title=LONG_TITLE, long_location=LONG_LOCATION).replace("\n", "\r\n").encode("utf-8")


def epoch(y, mo, d, h=0, mi=0):
    return int(datetime(y, mo, d, h, mi, tzinfo=LONDON).timestamp())


def payload(my_email=ME):
    return calendar_feed.build_payload(ICS, NOW, my_email)


def titles(p):
    return [e["n"] for e in p["ev"]]


def by_title(p, name):
    for e in p["ev"]:
        if e["n"] == name:
            return e
    raise AssertionError("no event titled %r in %r" % (name, titles(p)))


# --- self-check on the fixture -------------------------------------------------


def test_fixture_string_lengths():
    assert len(LONG_TITLE) == 60
    assert len(LONG_LOCATION) == 40
    assert NOW.strftime("%A") == "Tuesday"


# --- window ---------------------------------------------------------------------


def test_past_event_is_absent():
    assert "Yesterday retro" not in titles(payload())


def test_window_stops_at_end_of_tomorrow():
    # Wednesday's standup (tomorrow) is in; Thursday's is not.
    p = payload()
    starts = [e["s"] for e in p["ev"]]
    assert epoch(2026, 9, 9, 9, 30) in starts
    assert epoch(2026, 9, 10, 9, 30) not in starts


# --- declined ------------------------------------------------------------------


def test_declined_event_dropped_when_my_email_given():
    assert "1:1" not in titles(payload(ME))


def test_declined_event_kept_when_my_email_is_none():
    assert "1:1" in titles(payload(None))


def test_declined_match_is_case_insensitive():
    # The ICS spells the address "mailto:ME@Example.com".
    assert "1:1" not in titles(payload("ME@EXAMPLE.COM"))


# --- recurrence overrides -------------------------------------------------------


def test_today_standup_uses_the_recurrence_id_override():
    p = payload()
    starts = [e["s"] for e in p["ev"]]
    assert epoch(2026, 9, 8, 10, 0) in starts
    assert epoch(2026, 9, 8, 9, 30) not in starts


# --- cancelled ------------------------------------------------------------------


def test_cancelled_one_off_event_is_absent():
    ics = _mini_ics(
        "BEGIN:VEVENT\r\nUID:cancelled@example.com\r\nDTSTAMP:20260901T080000Z\r\n"
        "DTSTART;TZID=Europe/London:20260908T113000\r\n"
        "DTEND;TZID=Europe/London:20260908T120000\r\n"
        "SUMMARY:Cancelled meeting\r\nSTATUS:CANCELLED\r\nEND:VEVENT\r\n"
    )
    assert calendar_feed.build_payload(ics, NOW, ME)["ev"] == []


def test_cancelled_recurrence_instance_drops_only_that_instance():
    # A weekday series; only today's instance is cancelled, via a
    # RECURRENCE-ID override carrying STATUS:CANCELLED. Tomorrow's instance
    # (no override) is untouched.
    ics = _mini_ics(
        "BEGIN:VEVENT\r\nUID:cseries@example.com\r\nDTSTAMP:20260901T080000Z\r\n"
        "DTSTART;TZID=Europe/London:20260601T093000\r\n"
        "DTEND;TZID=Europe/London:20260601T100000\r\n"
        "RRULE:FREQ=WEEKLY;BYDAY=MO,TU,WE,TH,FR\r\n"
        "SUMMARY:Weekday sync\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nUID:cseries@example.com\r\n"
        "RECURRENCE-ID;TZID=Europe/London:20260908T093000\r\n"
        "DTSTAMP:20260901T080000Z\r\n"
        "DTSTART;TZID=Europe/London:20260908T093000\r\n"
        "DTEND;TZID=Europe/London:20260908T100000\r\n"
        "SUMMARY:Weekday sync\r\nSTATUS:CANCELLED\r\nEND:VEVENT\r\n"
    )
    p = calendar_feed.build_payload(ics, NOW, ME)
    starts = [e["s"] for e in p["ev"]]
    assert epoch(2026, 9, 8, 9, 30) not in starts  # today's instance, cancelled
    assert epoch(2026, 9, 9, 9, 30) in starts  # tomorrow's instance, untouched


# --- malformed input --------------------------------------------------------------


def test_one_malformed_vevent_does_not_abort_the_whole_refresh():
    # DTSTART:NOTADATE fails to parse; icalendar drops the property rather
    # than raising, leaving a VEVENT with no DTSTART at all, which used to
    # make recurring_ical_events raise for the *whole* calendar.
    ics = _mini_ics(
        "BEGIN:VEVENT\r\nUID:broken@example.com\r\nDTSTAMP:20260901T080000Z\r\n"
        "DTSTART:NOTADATE\r\nSUMMARY:Broken\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nUID:fine@example.com\r\nDTSTAMP:20260901T080000Z\r\n"
        "DTSTART;TZID=Europe/London:20260908T113000\r\n"
        "DTEND;TZID=Europe/London:20260908T120000\r\n"
        "SUMMARY:Fine\r\nEND:VEVENT\r\n"
    )
    assert titles(calendar_feed.build_payload(ics, NOW, ME)) == ["Fine"]


def test_zero_length_event_is_skipped():
    # No DTEND and no DURATION: recurring_ical_events fills in DTEND ==
    # DTSTART, which is not a meeting the Dial has anything to show.
    ics = _mini_ics(
        "BEGIN:VEVENT\r\nUID:instant@example.com\r\nDTSTAMP:20260901T080000Z\r\n"
        "DTSTART;TZID=Europe/London:20260908T113000\r\n"
        "SUMMARY:No duration\r\nEND:VEVENT\r\n"
    )
    assert calendar_feed.build_payload(ics, NOW, ME)["ev"] == []


# --- recurrence edge cases --------------------------------------------------------


def test_exdate_excludes_just_that_instance():
    ics = _mini_ics(
        "BEGIN:VEVENT\r\nUID:exdate@example.com\r\nDTSTAMP:20260901T080000Z\r\n"
        "DTSTART;TZID=Europe/London:20260601T093000\r\n"
        "DTEND;TZID=Europe/London:20260601T100000\r\n"
        "RRULE:FREQ=WEEKLY;BYDAY=MO,TU,WE,TH,FR\r\n"
        "EXDATE;TZID=Europe/London:20260908T093000\r\n"
        "SUMMARY:Exdate series\r\nEND:VEVENT\r\n"
    )
    p = calendar_feed.build_payload(ics, NOW, ME)
    starts = [e["s"] for e in p["ev"]]
    assert epoch(2026, 9, 8, 9, 30) not in starts  # excluded by EXDATE
    assert epoch(2026, 9, 9, 9, 30) in starts  # untouched


def test_duration_without_dtend_is_expanded():
    ics = _mini_ics(
        "BEGIN:VEVENT\r\nUID:duration@example.com\r\nDTSTAMP:20260901T080000Z\r\n"
        "DTSTART;TZID=Europe/London:20260908T113000\r\n"
        "DURATION:PT1H30M\r\nSUMMARY:Duration event\r\nEND:VEVENT\r\n"
    )
    e = by_title(calendar_feed.build_payload(ics, NOW, ME), "Duration event")
    assert e["s"] == epoch(2026, 9, 8, 11, 30)
    assert e["e"] == epoch(2026, 9, 8, 13, 0)  # DTSTART + PT1H30M


def test_floating_time_is_read_in_the_configured_timezone():
    # No TZID and no trailing Z: a "floating" local time, read in whatever
    # timezone build_payload is told (default Europe/London).
    ics = _mini_ics(
        "BEGIN:VEVENT\r\nUID:floating@example.com\r\nDTSTAMP:20260901T080000Z\r\n"
        "DTSTART:20260908T113000\r\nDTEND:20260908T120000\r\n"
        "SUMMARY:Floating event\r\nEND:VEVENT\r\n"
    )
    e = by_title(calendar_feed.build_payload(ics, NOW, ME), "Floating event")
    assert e["s"] == epoch(2026, 9, 8, 11, 30)
    assert e["e"] == epoch(2026, 9, 8, 12, 0)


def test_all_day_event_spans_the_dst_transition():
    # UK clocks go back on Sunday 25 Oct 2026 (BST -> GMT at 02:00 local),
    # so an all-day event's local-midnight-to-local-midnight span that day
    # is 25 hours of Unix time, not 24.
    ics = _mini_ics(
        "BEGIN:VEVENT\r\nUID:dst@example.com\r\nDTSTAMP:20260901T080000Z\r\n"
        "DTSTART;VALUE=DATE:20261025\r\nDTEND;VALUE=DATE:20261026\r\n"
        "SUMMARY:Clocks change\r\nEND:VEVENT\r\n"
    )
    now = datetime(2026, 10, 24, 8, 0, 0, tzinfo=LONDON)
    e = by_title(calendar_feed.build_payload(ics, now, ME), "Clocks change")
    assert e["a"] == 1
    assert e["s"] == epoch(2026, 10, 25)
    assert e["e"] == epoch(2026, 10, 26)
    assert e["e"] - e["s"] == 25 * 3600


# --- all-day --------------------------------------------------------------------


def test_all_day_offsite_spans_local_midnights():
    e = by_title(payload(), "Offsite")
    assert e["a"] == 1
    assert e["s"] == epoch(2026, 9, 9)
    assert e["e"] == epoch(2026, 9, 10)


def test_timed_events_are_not_flagged_all_day():
    assert by_title(payload(), "Standup")["a"] == 0


# --- clipping -------------------------------------------------------------------


def test_title_clipped_to_40_ascii_chars():
    n = [t for t in titles(payload()) if t.startswith("Quarterly")][0]
    assert n == "Quarterly business review with the ex..."
    assert len(n) == 40


def test_location_clipped_to_24_chars():
    e = [x for x in payload()["ev"] if x["n"].startswith("Quarterly")][0]
    assert e["l"] == "Building 3 Level 2 Me..."
    assert len(e["l"]) == 24


def test_non_ascii_is_stripped():
    ns = titles(payload())
    assert "Caf nave rsum" in ns
    assert all(all(ord(c) < 128 for c in n) for n in ns)


# --- where_of -------------------------------------------------------------------


def test_standup_where_is_google_meet():
    assert by_title(payload(), "Standup")["l"] == "Google Meet"


def test_where_of_prefers_plain_location():
    assert calendar_feed.where_of({"LOCATION": "Room 4"}) == "Room 4"


def test_where_of_ignores_url_location_and_reads_description():
    assert calendar_feed.where_of({"LOCATION": "https://zoom.us/j/1"}) == "Zoom"
    assert calendar_feed.where_of(
        {"DESCRIPTION": "https://teams.microsoft.com/l/x"}
    ) == "Teams"
    assert calendar_feed.where_of({"LOCATION": "https://example.com/x"}) == ""
    assert calendar_feed.where_of({}) == ""


# --- shape ----------------------------------------------------------------------


def test_events_sorted_by_start():
    starts = [e["s"] for e in payload()["ev"]]
    assert starts == sorted(starts)


def test_at_most_five_events():
    p = payload(None)  # the declined 1:1 comes back too, so six qualify
    assert len(p["ev"]) == 5
    # The five earliest survive; Wednesday's standup is the one dropped.
    assert epoch(2026, 9, 9, 9, 30) not in [e["s"] for e in p["ev"]]


def test_t_is_now():
    assert payload()["t"] == int(NOW.timestamp())


def test_every_event_has_all_five_keys():
    for e in payload()["ev"]:
        assert set(e) == {"s", "e", "n", "a", "l"}
        assert isinstance(e["s"], int) and isinstance(e["e"], int)
        assert isinstance(e["n"], str) and isinstance(e["l"], str)
        assert e["a"] in (0, 1)
        assert len(e["n"]) <= 40 and len(e["l"]) <= 24
        assert e["e"] > e["s"]


def test_in_progress_event_is_kept():
    ics = _mini_ics(
        "BEGIN:VEVENT\r\nUID:now@example.com\r\nDTSTAMP:20260901T080000Z\r\n"
        "DTSTART;TZID=Europe/London:20260908T073000\r\n"
        "DTEND;TZID=Europe/London:20260908T090000\r\n"
        "SUMMARY:Already running\r\nEND:VEVENT\r\n"
    )
    p = calendar_feed.build_payload(ics, NOW, ME)
    assert titles(p) == ["Already running"]


def test_event_that_just_ended_is_dropped():
    ics = _mini_ics(
        "BEGIN:VEVENT\r\nUID:done@example.com\r\nDTSTAMP:20260901T080000Z\r\n"
        "DTSTART;TZID=Europe/London:20260908T070000\r\n"
        "DTEND;TZID=Europe/London:20260908T075900\r\n"
        "SUMMARY:Just finished\r\nEND:VEVENT\r\n"
    )
    assert calendar_feed.build_payload(ics, NOW, ME)["ev"] == []


def test_untitled_event_gets_a_placeholder():
    ics = _mini_ics(
        "BEGIN:VEVENT\r\nUID:blank@example.com\r\nDTSTAMP:20260901T080000Z\r\n"
        "DTSTART;TZID=Europe/London:20260908T113000\r\n"
        "DTEND;TZID=Europe/London:20260908T120000\r\nEND:VEVENT\r\n"
    )
    assert titles(calendar_feed.build_payload(ics, NOW, ME)) == ["(no title)"]


def test_empty_calendar_yields_empty_list():
    p = calendar_feed.build_payload(_mini_ics(""), NOW, ME)
    assert p == {"t": int(NOW.timestamp()), "ev": []}


def test_naive_now_is_read_as_london():
    naive = NOW.replace(tzinfo=None)
    assert calendar_feed.build_payload(ICS, naive, ME) == payload()


def _mini_ics(body):
    return (
        "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//PaceKeeper//test//EN\r\n"
        + body
        + "END:VCALENDAR\r\n"
    ).encode("utf-8")


# --- ascii_clip -----------------------------------------------------------------


@pytest.mark.parametrize(
    "raw,n,want",
    [
        ("Standup", 40, "Standup"),
        ("  padded  ", 40, "padded"),
        ("two\nlines", 40, "two lines"),
        ("tabs\tand   spaces", 40, "tabs and spaces"),
        ("abcdefghij", 8, "abcde..."),
        ("abcde     fghij", 9, "abcde..."),
        ("éèê", 40, ""),
        ("", 40, ""),
    ],
)
def test_ascii_clip(raw, n, want):
    got = calendar_feed.ascii_clip(raw, n)
    assert got == want
    assert len(got) <= n


# --- JSON is what CalendarModel::parse expects ----------------------------------


def test_json_round_trips_compactly():
    import json

    body = calendar_feed.payload_json(payload())
    assert body.startswith(b'{"t":')
    assert b", " not in body  # compact separators keep the 2 KB device cap
    assert json.loads(body.decode("ascii"))["ev"][0]["n"] == "Standup"


# --- serving --------------------------------------------------------------------


def test_feed_keeps_last_good_payload_when_a_fetch_fails():
    calls = []

    def flaky(url, timeout=20):
        calls.append(url)
        if len(calls) == 1:
            return ICS
        raise OSError("network down")

    feed = calendar_feed.Feed(
        {"ICS_URL": "https://example.invalid/basic.ics", "MY_EMAIL": ME},
        fetcher=flaky,
        clock=lambda: NOW,
    )
    assert feed.refresh() is True
    good = feed.snapshot()[0]
    assert feed.refresh() is False
    assert feed.snapshot()[0] == good  # still the last good body
    assert "network down" in feed.last_error


SECRET_ICS = "https://calendar.google.com/calendar/ical/SECRET-TOKEN/basic.ics"


@pytest.mark.parametrize(
    "message",
    [
        "HTTP Error 500 for " + SECRET_ICS,  # the whole URL
        "not found: /calendar/ical/SECRET-TOKEN/basic.ics",  # only the path
        "bad host calendar.google.com/calendar/ical/SECRET-TOKEN/basic.ics",
    ],
)
def test_secret_url_never_appears_in_the_error_text(message):
    def boom(url, timeout=20):
        raise OSError(message)

    feed = calendar_feed.Feed({"ICS_URL": SECRET_ICS}, fetcher=boom, clock=lambda: NOW)
    assert feed.refresh() is False
    assert "SECRET-TOKEN" not in feed.last_error
    assert "SECRET-TOKEN" not in calendar_feed.payload_json(feed.health()).decode()


def test_health_reports_state(monkeypatch):
    # health()'s age_s is measured against the real wall clock (so it stays
    # meaningful even if a test, or a future caller, fakes Feed's own
    # `clock`), so pin `time.time()` too for a deterministic age_s.
    monkeypatch.setattr(calendar_feed.time, "time", lambda: NOW.timestamp())
    feed = calendar_feed.Feed(
        {"ICS_URL": "https://example.invalid/basic.ics"},
        fetcher=lambda url, timeout=20: ICS,
        clock=lambda: NOW,
    )
    h0 = feed.health()
    assert h0["ok"] is False
    assert h0["stale"] is True  # never fetched
    assert h0["age_s"] == -1
    assert h0["last_error"] == ""
    feed.refresh()
    h = feed.health()
    assert h["ok"] is True
    assert h["stale"] is False
    assert h["age_s"] == 0
    assert h["last_error"] == ""
    assert set(h) == {"ok", "stale", "age_s", "last_error"}


def test_health_last_error_is_exception_class_only_never_the_message():
    # icalendar puts the offending content line straight into some of its
    # ValueErrors, so a malformed body containing something like a meeting
    # SUMMARY must never reach /health, token or no token.
    def html_body(url, timeout=20):
        return b"<html>Sign in</html>"

    feed = calendar_feed.Feed(
        {"ICS_URL": "https://example.invalid/basic.ics"},
        fetcher=html_body,
        clock=lambda: NOW,
    )
    assert feed.refresh() is False
    assert feed.last_error.startswith("ValueError")  # full message, log-only
    body = calendar_feed.payload_json(feed.health())
    assert b'"last_error":"ValueError"' in body
    assert b"Sign in" not in body


def test_health_is_stale_after_two_missed_refresh_cycles(monkeypatch):
    fixed_wall_clock = NOW.timestamp()
    monkeypatch.setattr(calendar_feed.time, "time", lambda: fixed_wall_clock)
    feed = calendar_feed.Feed(
        {"ICS_URL": "https://example.invalid/basic.ics"},
        fetcher=lambda url, timeout=20: ICS,
        clock=lambda: NOW,
    )
    assert feed.refresh() is True
    assert feed.health()["stale"] is False
    monkeypatch.setattr(
        calendar_feed.time,
        "time",
        lambda: fixed_wall_clock + calendar_feed.REFRESH_S * 2 + 1,
    )
    assert feed.health()["stale"] is True


def test_token_check():
    assert calendar_feed.token_ok(None, "/calendar.json") is True
    assert calendar_feed.token_ok("s3cret", "/calendar.json?k=s3cret") is True
    assert calendar_feed.token_ok("s3cret", "/calendar.json?k=nope") is False
    assert calendar_feed.token_ok("s3cret", "/calendar.json") is False


def test_token_check_non_ascii_token_never_raises():
    # hmac.compare_digest() raises TypeError given a non-ASCII str, so both
    # sides must be encoded to bytes first: a non-ASCII TOKEN must fail the
    # check, never crash the request handler.
    assert calendar_feed.token_ok("café", "/calendar.json?k=nope") is False
    assert calendar_feed.token_ok("café", "/calendar.json?k=caf%C3%A9") is True


def test_serve_exits_cleanly_when_the_port_is_already_in_use(monkeypatch):
    import socket

    # No network in this test: the port clash is the only thing under test.
    monkeypatch.setattr(calendar_feed.Feed, "start", lambda self: None)
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.bind(("127.0.0.1", 0))
    sock.listen(1)
    port = sock.getsockname()[1]
    try:
        with pytest.raises(SystemExit) as exc_info:
            calendar_feed.serve(
                {
                    "ICS_URL": "https://example.invalid/basic.ics",
                    "PORT": str(port),
                    "BIND": "127.0.0.1",
                }
            )
        assert exc_info.value.code == 1
    finally:
        sock.close()


@pytest.fixture
def relay():
    """A live relay on an ephemeral port, TOKEN set, primed with the fixture."""
    import threading
    from http.server import ThreadingHTTPServer

    feed = calendar_feed.Feed(
        {"ICS_URL": "https://example.invalid/basic.ics", "MY_EMAIL": ME},
        fetcher=lambda url, timeout=20: ICS,
        clock=lambda: NOW,
    )
    httpd = ThreadingHTTPServer(
        ("127.0.0.1", 0), calendar_feed.make_handler(feed, "s3cret")
    )
    httpd.daemon_threads = True
    thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    thread.start()
    base = "http://127.0.0.1:%d" % httpd.server_address[1]
    try:
        yield base, feed
    finally:
        httpd.shutdown()
        httpd.server_close()
        thread.join(timeout=5)


def _get(url):
    import urllib.error
    import urllib.request

    try:
        with urllib.request.urlopen(url, timeout=5) as resp:
            return resp.status, dict(resp.headers), resp.read()
    except urllib.error.HTTPError as exc:
        return exc.code, dict(exc.headers), exc.read()


def test_calendar_json_is_served_uncacheable(relay):
    base, feed = relay
    feed.refresh()
    status, headers, body = _get(base + "/calendar.json?k=s3cret")
    assert status == 200
    assert headers["Cache-Control"] == "no-store"
    assert headers["Content-Type"] == "application/json"
    assert body == calendar_feed.payload_json(payload())


def test_calendar_json_requires_the_token(relay):
    base, feed = relay
    feed.refresh()
    assert _get(base + "/calendar.json")[0] == 403
    assert _get(base + "/calendar.json?k=wrong")[0] == 403


def test_calendar_json_is_503_before_the_first_fetch(relay):
    base, _feed = relay
    status, _headers, body = _get(base + "/calendar.json?k=s3cret")
    assert status == 503
    assert b"no data" in body


def test_health_needs_no_token_and_is_json(relay):
    import json

    base, feed = relay
    feed.refresh()
    status, headers, body = _get(base + "/health")
    assert status == 200
    assert headers["Cache-Control"] == "no-store"
    assert json.loads(body.decode())["ok"] is True


def test_unknown_route_is_404(relay):
    base, _feed = relay
    assert _get(base + "/nope")[0] == 404


def test_load_env(tmp_path):
    p = tmp_path / "calendar.env"
    p.write_text(
        "# a comment\n"
        "\n"
        "ICS_URL=https://example.invalid/basic.ics\n"
        'MY_EMAIL="me@example.com"\n'
        "PORT = 9000 \n"
        "BROKEN\n"
    )
    cfg = calendar_feed.load_env(str(p))
    assert cfg["ICS_URL"] == "https://example.invalid/basic.ics"
    assert cfg["MY_EMAIL"] == "me@example.com"
    assert cfg["PORT"] == "9000"
    assert "BROKEN" not in cfg


def test_load_env_missing_file_is_empty(tmp_path):
    assert calendar_feed.load_env(str(tmp_path / "nope.env")) == {}


def test_window_end_is_end_of_tomorrow():
    assert calendar_feed.window_end(NOW, LONDON) == datetime(
        2026, 9, 9, 23, 59, 59, tzinfo=LONDON
    )
    assert calendar_feed.window_end(
        NOW + timedelta(days=1), LONDON
    ) == datetime(2026, 9, 10, 23, 59, 59, tzinfo=LONDON)


# --- airline logos (spec 4.11 amendment 2026-09-09) --------------------------


def _red_png(w=120, h=48):
    """An opaque red PNG with a transparent 10-px frame, as bytes."""
    import io

    from PIL import Image

    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    for x in range(10, w - 10):
        for y in range(10, h - 10):
            img.putpixel((x, y), (255, 0, 0, 255))
    buf = io.BytesIO()
    img.save(buf, format="PNG")
    return buf.getvalue()


def _px(data, x, y):
    i = (y * calendar_feed.LOGO_W + x) * 2
    return (data[i] << 8) | data[i + 1]


def test_png_to_rgb565_geometry_white_ground_and_byte_order():
    data = calendar_feed.png_to_rgb565(_red_png())
    assert len(data) == calendar_feed.LOGO_BYTES == 11520
    # The canvas margin and the PNG's transparent frame come out white.
    assert _px(data, 0, 0) == 0xFFFF
    assert _px(data, 119, 47) == 0xFFFF
    assert _px(data, 8, 24) == 0xFFFF
    # The opaque red block lands scaled and centred; big-endian 565 red.
    assert _px(data, 60, 24) == 0xF800
    assert data[(24 * 120 + 60) * 2] == 0xF8
    assert data[(24 * 120 + 60) * 2 + 1] == 0x00


def test_png_to_rgb565_accepts_other_sizes():
    data = calendar_feed.png_to_rgb565(_red_png(300, 90))
    assert len(data) == calendar_feed.LOGO_BYTES
    assert _px(data, 60, 24) == 0xF800


def test_logo_store_caches_hits_and_remembers_misses(tmp_path):
    calls = []

    def fetcher(iata, timeout=10):
        calls.append(iata)
        if iata == "ZZ":
            return None
        if iata == "ER":
            raise OSError("boom")
        return _red_png()

    now = [1000.0]
    store = calendar_feed.LogoStore(
        str(tmp_path / "logos"), fetcher=fetcher, clock=lambda: now[0], sync=True
    )

    status, data = store.get("BA")
    assert status == 200 and len(data) == calendar_feed.LOGO_BYTES
    assert (tmp_path / "logos" / "BA.565").read_bytes() == data
    assert store.get("BA") == (200, data)
    assert calls == ["BA"]  # memory hit, no refetch

    # A fresh store finds the disk cache without fetching.
    store2 = calendar_feed.LogoStore(
        str(tmp_path / "logos"), fetcher=fetcher, clock=lambda: now[0], sync=True
    )
    assert store2.get("BA") == (200, data)
    assert calls == ["BA"]

    # 404 at the source is remembered for LOGO_MISS_TTL_S, then retried.
    assert store.get("ZZ") == (404, None)
    assert store.get("ZZ") == (404, None)
    assert calls == ["BA", "ZZ"]
    now[0] += calendar_feed.LOGO_MISS_TTL_S + 1
    assert store.get("ZZ") == (404, None)
    assert calls == ["BA", "ZZ", "ZZ"]

    # Transient failure is 503 and not remembered.
    assert store.get("ER") == (503, None)
    assert store.get("ER") == (503, None)
    assert calls[-2:] == ["ER", "ER"]
    assert not (tmp_path / "logos" / "ER.565").exists()


def test_logo_store_refetches_a_short_cache_file(tmp_path):
    d = tmp_path / "logos"
    d.mkdir()
    (d / "BA.565").write_bytes(b"\x00" * 100)
    store = calendar_feed.LogoStore(str(d), fetcher=lambda iata, timeout=10: _red_png(), sync=True)
    status, data = store.get("BA")
    assert status == 200 and len(data) == calendar_feed.LOGO_BYTES
    assert (d / "BA.565").read_bytes() == data


@pytest.fixture
def relay_with_logos(tmp_path):
    import threading
    from http.server import ThreadingHTTPServer

    feed = calendar_feed.Feed(
        {"ICS_URL": "https://example.invalid/basic.ics", "MY_EMAIL": ME},
        fetcher=lambda url, timeout=20: ICS,
        clock=lambda: NOW,
    )

    def fetcher(iata, timeout=10):
        return None if iata == "ZZ" else _red_png()

    logos = calendar_feed.LogoStore(str(tmp_path / "logos"), fetcher=fetcher, sync=True)
    httpd = ThreadingHTTPServer(
        ("127.0.0.1", 0), calendar_feed.make_handler(feed, "s3cret", logos)
    )
    httpd.daemon_threads = True
    thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    thread.start()
    base = "http://127.0.0.1:%d" % httpd.server_address[1]
    try:
        yield base
    finally:
        httpd.shutdown()
        httpd.server_close()
        thread.join(timeout=5)


def test_logo_endpoint_serves_raw_565_without_a_token(relay_with_logos):
    base = relay_with_logos
    status, headers, body = _get(base + "/logo/BA.565")
    assert status == 200
    assert headers["Content-Type"] == "application/octet-stream"
    assert headers["Content-Length"] == str(calendar_feed.LOGO_BYTES)
    assert len(body) == calendar_feed.LOGO_BYTES
    assert _px(body, 60, 24) == 0xF800


def test_logo_endpoint_404s_for_unknown_airline_and_bad_paths(relay_with_logos):
    base = relay_with_logos
    assert _get(base + "/logo/ZZ.565")[0] == 404
    assert _get(base + "/logo/ba.565")[0] == 404
    assert _get(base + "/logo/BAX.565")[0] == 404
    assert _get(base + "/logo/BA.png")[0] == 404
    assert _get(base + "/logos/BA.565")[0] == 404


def test_relay_without_a_logo_store_404s_the_logo_route(relay):
    base, _feed = relay
    assert _get(base + "/logo/BA.565")[0] == 404


def test_logo_store_answers_503_while_fetching_in_the_background(tmp_path):
    import threading

    release = threading.Event()
    started = threading.Event()

    def slow_fetcher(iata, timeout=10):
        started.set()
        release.wait(5)
        return _red_png()

    store = calendar_feed.LogoStore(str(tmp_path / "logos"), fetcher=slow_fetcher)
    assert store.get("BA") == (503, None)  # kicked off, not waited for
    assert started.wait(2)
    assert store.get("BA") == (503, None)  # still pending: no second thread
    release.set()
    deadline = time_module.monotonic() + 5
    while time_module.monotonic() < deadline:
        status, data = store.get("BA")
        if status == 200:
            break
        time_module.sleep(0.02)
    assert status == 200 and len(data) == calendar_feed.LOGO_BYTES
    assert (tmp_path / "logos" / "BA.565").exists()
