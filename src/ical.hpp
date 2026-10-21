// SPDX-License-Identifier: GPL-3.0-or-later
// Reminders (VTODO) and events (VEVENT) in iCalendar text.
#pragma once

#include <ctime>
#include <optional>
#include <string>
#include <vector>

#include "when.hpp"

namespace jotdo {

struct Task {
    std::string uid;
    std::string title;
    std::string notes;
    std::optional<Due> due;
    std::string repeat;       // RRULE text; empty when it does not repeat
    std::vector<int> alerts;  // minutes before the due time, ascending
    int priority = 0;         // iCalendar: 1 high, 5 medium, 9 low, 0 none
    bool done = false;
    std::time_t completed = 0;
    std::time_t created = 0;
    std::time_t modified = 0;
    std::string url;

    // Where it lives on the server.
    std::string list;       // the list's name
    std::string list_href;  // the list's URL
    std::string href;       // the object's URL
    std::string etag;
    std::string raw;        // the whole object as loaded; empty for a new reminder
};

// The first VTODO in `ics`, or nothing if there is none.
std::optional<Task> parse_task(const std::string& ics);

// `t` as an iCalendar object: `t.raw` with the fields above replaced and everything else (other
// properties, alarms that did not change, X- extensions) kept, or a new object when raw is empty.
std::string write_task(const Task& t, std::time_t now);

// Moves a repeating reminder on to its next occurrence after its current due date. Returns
// false, changing nothing, when the repeat has no occurrence left.
bool advance(Task& t);

struct Event {
    std::string uid;
    std::string title;
    std::string location;
    std::string notes;
    std::time_t start = 0;
    std::time_t end = 0;      // exclusive; for an all-day event, midnight after its last day
    bool all_day = false;
    std::string repeat;       // the series' RRULE; empty when it does not repeat
    bool recurring = false;   // one occurrence of a repeating event (or the series itself)
    std::vector<int> alerts;  // minutes before the start

    // Where it lives on the server.
    std::string calendar;       // the calendar's name
    std::string calendar_href;  // the calendar's URL
    std::string color;
    std::string href;  // the object's URL
    std::string etag;
    std::string raw;   // the whole object as loaded (the series, not one occurrence); empty when new
};

// Every VEVENT in `ics` (for an expanded query, each occurrence).
std::vector<Event> parse_events(const std::string& ics);

// `e` as an iCalendar object: `e.raw` with the fields above replaced in its main event (the one
// without a RECURRENCE-ID) and everything else kept, or a new object when raw is empty.
std::string write_event(const Event& e, std::time_t now);

std::string new_uid();
std::string local_tzid();  // the system's zone, "America/New_York"; "UTC" if unknown

}  // namespace jotdo
