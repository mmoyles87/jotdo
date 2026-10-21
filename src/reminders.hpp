// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <ctime>
#include <string>
#include <vector>

#include "dav.hpp"
#include "ical.hpp"

namespace jotdo {

// The account's task lists and the reminders in them.
class Reminders {
public:
    explicit Reminders(Dav& dav);

    // The lists that hold tasks: the account's `lists` in that order, or every one the server has.
    const std::vector<Collection>& lists();
    const Collection& list_named(const std::string& name);  // throws if there is none
    const Collection& default_list();

    // Every reminder in every list: open ones by due date (undated last), then done ones,
    // most recently finished first.
    std::vector<Task> load();

    // Saves `t` into `list`: as a new reminder when it has no href, over the loaded version
    // otherwise (moving it when the list changed). Updates t's href, etag and raw.
    // Throws Conflict, saving nothing, if it changed on the server since it was loaded.
    void save(Task& t, const Collection& list, std::time_t now);

    // Ticks a reminder off or back on. A repeating one moves on to its next date instead of
    // being finished, as on a phone; returns true when that happened.
    bool set_done(Task& t, bool done, std::time_t now);

    // Deletes the reminder. Throws Conflict if it changed on the server since it was loaded.
    void remove(const Task& t);

    // The events between `from` and `to` in the account's calendars, by start time.
    std::vector<Event> agenda(std::time_t from, std::time_t to);

private:
    Dav& dav_;
    std::vector<Collection> all_;
    std::vector<Collection> lists_;
    bool loaded_ = false;
    void discover();
};

// Sorts reminders the way load() returns them.
void sort_tasks(std::vector<Task>& tasks);

}  // namespace jotdo
