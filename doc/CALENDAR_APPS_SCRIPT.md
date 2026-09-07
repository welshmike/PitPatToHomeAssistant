# Calendar feed for the Dial (Google Apps Script)

The Dial's Calendar card (spec 4.19) reads the next few work-calendar events from a tiny web
app running inside Mike's **work** Google account. Nothing goes through Home Assistant.

## Deploy (once, ~5 minutes)

1. Signed in to the work Google account, open https://script.google.com and create a new
   project named `PaceKeeper Dial calendar`.
2. Replace the editor contents with `doc/calendar_apps_script.gs`. Change `TOKEN` to a long
   random string (e.g. `openssl rand -hex 24`).
3. Run `doGet` once from the editor (Run ▸ doGet) and accept the Calendar permission prompt.
4. Deploy ▸ New deployment ▸ type **Web app**; Execute as **Me**; Who has access **Anyone**.
   Copy the **Web app URL** (ends in `/exec`).
5. Test in a browser: `<Web app URL>?k=<TOKEN>` returns JSON like
   `{"t":1757260000,"ev":[{"s":1757261400,"e":1757263200,"n":"Standup","a":0,"l":"Google Meet"}]}`.
   Without the right `k` it returns `{"error":"forbidden"}`.
6. In `src/config.h` add
   `#define CALENDAR_URL   "https://script.google.com/macros/s/…/exec"` and
   `#define CALENDAR_TOKEN "<TOKEN>"`, then rebuild and flash. Without `CALENDAR_URL` the card
   is not compiled in.

Redeploy (Deploy ▸ Manage deployments ▸ edit ▸ New version) whenever the script changes; the
URL stays the same.

## Payload

`t` is the script's Unix time; each event has `s`/`e` start and end (Unix, UTC), `n` title
(≤ 40 chars), `a` 1 for all-day, `l` location or meeting host (≤ 24 chars). At most 5 events,
now → end of tomorrow, declined events omitted, recurring events already expanded.

## Notes

- The `/exec` URL answers with a 302 to `script.googleusercontent.com`; the Dial follows it.
- Both hosts chain to Google Trust Services roots R1/R4, which the firmware pins
  (`src/CalendarCerts.h`). If Google re-roots, define `CALENDAR_TLS_INSECURE` as a stopgap.
- If the company Workspace blocks "Anyone" web apps, the deployment step fails with a policy
  message; there is no device-side workaround.
