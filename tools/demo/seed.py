#!/usr/bin/env python3
"""Fill the demo server with reminder lists, reminders and a few calendar events.

    seed.py URL USER

Standard library only. Dates are relative to today, so the demo always has something due.
"""
import base64
import datetime as dt
import sys
import urllib.parse
import urllib.request
import uuid

LISTS = [  # (name, colour, component)
    ("Personal", "#4A90D9", "VTODO"),
    ("Family", "#E5A50A", "VTODO"),
    ("Groceries", "#57A05A", "VTODO"),
    ("Home", "#C061CB", "VEVENT"),
]

# (list, title, due: (days from today, "HH:MM" or None) or None, extra properties)
REMINDERS = [
    ("Personal", "Return the library books", (-1, None), {}),
    ("Personal", "Renew passport", (0, "10:00"), {"PRIORITY": "1", "ALARM": "-PT1H"}),
    ("Personal", "Call the dentist", (0, "15:30"), {}),
    ("Personal", "Back up the laptop", (1, "09:00"), {"RRULE": "FREQ=WEEKLY"}),
    ("Personal", "Pay the water bill", (3, "18:00"), {"RRULE": "FREQ=MONTHLY", "ALARM": "-P1D"}),
    ("Personal", "Book flights to Lisbon", (4, None), {"PRIORITY": "5",
        "DESCRIPTION": "Window seats. Check whether the 7:40 still runs on Saturdays."}),
    ("Personal", "Read The Overstory", None, {}),
    ("Family", "Pick up the cake", (0, "17:00"), {"ALARM": "-PT30M",
        "DESCRIPTION": "Order number 2231, paid for. Ask them to write the name on it."}),
    ("Family", "Put the bins out", (2, "08:00"), {"RRULE": "FREQ=WEEKLY"}),
    ("Family", "Book the boiler service", (9, None), {}),
    ("Family", "Sign the permission slip", (-2, None), {"DONE": True}),
    ("Groceries", "Oat milk", None, {}),
    ("Groceries", "Coffee beans", None, {"PRIORITY": "9"}),
    ("Groceries", "Lemons", None, {}),
    ("Groceries", "Sourdough flour", None, {"DONE": True}),
]

# (title, days from today, "HH:MM", minutes, location)
EVENTS = [
    ("Dinner with Sam", 0, "19:30", 120, "Bar Douro"),
    ("Dentist", 1, "10:00", 45, ""),
    ("Farmers market", 5, "09:00", 90, "Riverside"),
]


def request(method, url, user, body=None, headers=None):
    req = urllib.request.Request(url, data=body.encode() if body else None, method=method)
    req.add_header("Authorization", "Basic " + base64.b64encode(f"{user}:demo".encode()).decode())
    for k, v in (headers or {}).items():
        req.add_header(k, v)
    with urllib.request.urlopen(req) as r:
        return r.status


def text(s):
    """An iCalendar TEXT value: backslashes, commas and semicolons escaped."""
    return s.replace("\\", "\\\\").replace(",", "\\,").replace(";", "\\;")


def stamp(t):
    return t.astimezone(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")


def when(days, hhmm):
    day = dt.date.today() + dt.timedelta(days=days)
    if hhmm is None:
        return None, day
    h, m = map(int, hhmm.split(":"))
    return dt.datetime.combine(day, dt.time(h, m)).astimezone(), day


def vtodo(title, due, extra):
    now = dt.datetime.now(dt.timezone.utc)
    lines = ["BEGIN:VCALENDAR", "VERSION:2.0", "PRODID:-//jotdo demo//EN", "BEGIN:VTODO",
             f"UID:{uuid.uuid4()}", f"DTSTAMP:{stamp(now)}", f"SUMMARY:{text(title)}"]
    if due:
        t, day = when(*due)
        lines.append(f"DUE:{stamp(t)}" if t else f"DUE;VALUE=DATE:{day:%Y%m%d}")
    for key in ("PRIORITY", "RRULE"):
        if key in extra:
            lines.append(f"{key}:{extra[key]}")
    if "DESCRIPTION" in extra:
        lines.append(f"DESCRIPTION:{text(extra['DESCRIPTION'])}")
    if extra.get("DONE"):
        lines += ["STATUS:COMPLETED", f"COMPLETED:{stamp(now)}"]
    else:
        lines.append("STATUS:NEEDS-ACTION")
    if "ALARM" in extra:
        lines += ["BEGIN:VALARM", "ACTION:DISPLAY", "DESCRIPTION:Reminder",
                  f"TRIGGER;RELATED=END:{extra['ALARM']}", "END:VALARM"]
    lines += ["END:VTODO", "END:VCALENDAR"]
    return "\r\n".join(lines) + "\r\n"


def vevent(title, days, hhmm, minutes, location):
    start, _ = when(days, hhmm)
    lines = ["BEGIN:VCALENDAR", "VERSION:2.0", "PRODID:-//jotdo demo//EN", "BEGIN:VEVENT",
             f"UID:{uuid.uuid4()}", f"DTSTAMP:{stamp(dt.datetime.now(dt.timezone.utc))}",
             f"SUMMARY:{text(title)}", f"DTSTART:{stamp(start)}",
             f"DTEND:{stamp(start + dt.timedelta(minutes=minutes))}"]
    if location:
        lines.append(f"LOCATION:{text(location)}")
    lines += ["END:VEVENT", "END:VCALENDAR"]
    return "\r\n".join(lines) + "\r\n"


def main():
    base, user = sys.argv[1].rstrip("/"), sys.argv[2]
    home = f"{base}/{user}/"
    request("PROPFIND", home, user, headers={"Depth": "0"})  # Radicale creates the home on first use
    hrefs = {}
    for name, colour, component in LISTS:
        hrefs[name] = href = home + urllib.parse.quote(name.lower()) + "/"
        request("MKCALENDAR", href, user, f"""<?xml version="1.0" encoding="utf-8"?>
<c:mkcalendar xmlns:d="DAV:" xmlns:c="urn:ietf:params:xml:ns:caldav" xmlns:a="http://apple.com/ns/ical/">
  <d:set><d:prop>
    <d:displayname>{name}</d:displayname>
    <a:calendar-color>{colour}</a:calendar-color>
    <c:supported-calendar-component-set><c:comp name="{component}"/></c:supported-calendar-component-set>
  </d:prop></d:set>
</c:mkcalendar>""", {"Content-Type": "application/xml"})
    ics = {"Content-Type": "text/calendar"}
    for name, title, due, extra in REMINDERS:
        request("PUT", hrefs[name] + f"{uuid.uuid4()}.ics", user, vtodo(title, due, extra), ics)
    for event in EVENTS:
        request("PUT", hrefs["Home"] + f"{uuid.uuid4()}.ics", user, vevent(*event), ics)
    print(f"seeded {len(REMINDERS)} reminders in {len(LISTS) - 1} lists and {len(EVENTS)} events")


if __name__ == "__main__":
    main()
