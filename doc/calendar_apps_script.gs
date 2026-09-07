// PaceKeeper Dial — calendar feed (spec 4.19).
// Deploy as a Web App: Execute as "Me", Who has access "Anyone".
// The Dial calls  <exec URL>?k=<TOKEN>  every 5 minutes.
const TOKEN = 'CHANGE-ME-to-a-long-random-string';
const MAX_EVENTS = 5;
const TITLE_MAX = 40;
const WHERE_MAX = 24;

function doGet(e) {
  const k = (e && e.parameter && e.parameter.k) || '';
  if (k !== TOKEN) {
    return ContentService.createTextOutput('{"error":"forbidden"}')
      .setMimeType(ContentService.MimeType.JSON);
  }
  const now = new Date();
  const endOfTomorrow = new Date(now);
  endOfTomorrow.setDate(endOfTomorrow.getDate() + 1);
  endOfTomorrow.setHours(23, 59, 59, 999);

  const cal = CalendarApp.getDefaultCalendar();
  const events = cal.getEvents(now, endOfTomorrow)
    .filter(ev => ev.getMyStatus() !== CalendarApp.GuestStatus.NO)
    .sort((a, b) => a.getStartTime() - b.getStartTime())
    .slice(0, MAX_EVENTS)
    .map(ev => ({
      s: Math.floor(ev.getStartTime().getTime() / 1000),
      e: Math.floor(ev.getEndTime().getTime() / 1000),
      n: clip(ev.getTitle() || '(no title)', TITLE_MAX),
      a: ev.isAllDayEvent() ? 1 : 0,
      l: clip(whereOf(ev), WHERE_MAX)
    }));

  const body = JSON.stringify({ t: Math.floor(now.getTime() / 1000), ev: events });
  return ContentService.createTextOutput(body).setMimeType(ContentService.MimeType.JSON);
}

function whereOf(ev) {
  const loc = (ev.getLocation() || '').trim();
  if (loc && !/^https?:\/\//i.test(loc)) return loc;
  const desc = (ev.getDescription() || '') + ' ' + loc;
  if (/meet\.google\.com/i.test(desc)) return 'Google Meet';
  if (/zoom\.us/i.test(desc)) return 'Zoom';
  if (/teams\.microsoft\.com/i.test(desc)) return 'Teams';
  return '';
}

function clip(s, n) {
  s = String(s).replace(/[\r\n]+/g, ' ').trim();
  return s.length > n ? s.slice(0, n - 1) + '…' : s;
}
