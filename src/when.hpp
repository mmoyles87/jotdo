// SPDX-License-Identifier: GPL-3.0-or-later
// Reading and writing due dates, repeats, alerts and priorities as people type them.
#pragma once

#include <ctime>
#include <optional>
#include <string>
#include <vector>

namespace jotdo {

// When a reminder is due: a day, or a day and a time (local time).
struct Due {
    std::time_t t = 0;  // for a day, local midnight
    bool has_time = false;
    bool operator==(const Due&) const = default;
};

// "today", "tomorrow 9am", "mon 8:00", "fri", "next tue 14:30", "in 2h", "in 3 days", "oct 24",
// "2026-10-24 18:00", "10/24", "noon", "tonight", or a time alone (the next time it comes round).
// Empty, "none" or "-" mean no due date. Throws std::invalid_argument on anything else.
std::optional<Due> parse_due(const std::string& text, std::time_t now);

std::string format_due(const Due& d, std::time_t now);  // "Today 09:00", "Tomorrow", "Mon 08:00", "Oct 24"
std::string format_due_exact(const Due& d);             // "2026-10-24 09:00", "2026-10-24"
bool is_overdue(const Due& d, std::time_t now);
bool is_today(const Due& d, std::time_t now);

// "daily", "weekdays", "weekly", "every 2 weeks", "every mon, wed", "monthly", "yearly", or an
// RRULE ("FREQ=MONTHLY;BYDAY=1MO"). Empty or "none" mean no repeat. Returns RRULE text.
std::string parse_repeat(const std::string& text);
std::string describe_repeat(const std::string& rrule);  // "every Monday"; the RRULE itself if unusual

// "at due", "15m before", "1h before, 1d before". Minutes before the due time.
std::vector<int> parse_alerts(const std::string& text);
std::string describe_alerts(const std::vector<int>& minutes);

// A length: "1h", "90m", "1h30m", "2 hours", "1 day". Minutes; throws on anything else.
int parse_duration(const std::string& text);

// "high", "medium", "low", "none", or "!!!", "!!", "!". iCalendar's 1 (high), 5, 9 and 0 (none).
int parse_priority(const std::string& text);
std::string describe_priority(int priority);  // "high", "medium", "low", ""

std::time_t local_midnight(std::time_t t);

}  // namespace jotdo
