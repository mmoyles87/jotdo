// SPDX-License-Identifier: GPL-3.0-or-later
#include "reminders.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace jotdo {

namespace {

std::string lower(std::string s) {
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string href_for(const Collection& list, const std::string& uid) {
    std::string safe;
    for (char c : uid)
        safe += std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' ? c : '_';
    return list.href + safe + ".ics";
}

}  // namespace

void sort_tasks(std::vector<Task>& tasks) {
    std::stable_sort(tasks.begin(), tasks.end(), [](const Task& a, const Task& b) {
        if (a.done != b.done)
            return !a.done;
        if (a.done)
            return a.completed > b.completed;
        if (a.due.has_value() != b.due.has_value())
            return a.due.has_value();
        if (a.due && a.due->t != b.due->t)
            return a.due->t < b.due->t;
        if (a.due && a.due->has_time != b.due->has_time)
            return !a.due->has_time;  // a day's undated-time reminders before its timed ones
        return a.created < b.created;
    });
}

Reminders::Reminders(Dav& dav) : dav_(dav) {}

// A move is a copy to the new place, then a delete of the old one. If the old one changed in the
// meantime, the copy is taken back out, so the conflict leaves nothing behind.
void Reminders::finish_move(const std::string& new_href, const std::string& new_etag, const std::string& old_href,
                            const std::string& old_etag) {
    try {
        dav_.remove(old_href, old_etag);
    } catch (...) {
        try {
            std::string etag = new_etag.empty() ? dav_.get(new_href).etag : new_etag;
            dav_.remove(new_href, etag);
        } catch (...) {
            throw std::runtime_error("moving failed half way: a copy is left at " + new_href);
        }
        throw;
    }
}

void Reminders::discover() {
    if (loaded_)
        return;
    all_ = dav_.collections();
    const auto& wanted = dav_.account().lists;
    if (wanted.empty()) {
        for (auto& c : all_)
            if (c.tasks)
                lists_.push_back(c);
    } else {
        for (auto& name : wanted) {
            auto it = std::find_if(all_.begin(), all_.end(), [&](const Collection& c) { return lower(c.name) == lower(name); });
            if (it == all_.end())
                throw std::runtime_error("the server has no list named \"" + name + "\"");
            lists_.push_back(*it);
        }
    }
    if (lists_.empty())
        throw std::runtime_error("the account has no task lists");
    loaded_ = true;
}

const std::vector<Collection>& Reminders::lists() {
    discover();
    return lists_;
}

const Collection& Reminders::list_named(const std::string& name) {
    discover();
    for (auto& c : lists_)
        if (lower(c.name) == lower(name))
            return c;
    std::string names;
    for (auto& c : lists_)
        names += (names.empty() ? "" : ", ") + c.name;
    throw std::runtime_error("no list named \"" + name + "\" (there are " + names + ")");
}

const Collection& Reminders::default_list() {
    discover();
    const std::string& name = dav_.account().default_list;
    if (!name.empty())
        return list_named(name);
    for (auto& c : lists_)
        if (c.writable)
            return c;
    return lists_.front();
}

std::vector<Task> Reminders::load() {
    discover();
    std::vector<Task> out;
    for (auto& list : lists_)
        for (auto& r : dav_.query(list, "VTODO")) {
            auto t = parse_task(r.data);
            if (!t)
                continue;
            t->list = list.name;
            t->list_href = list.href;
            t->href = r.href;
            t->etag = r.etag;
            out.push_back(std::move(*t));
        }
    sort_tasks(out);
    return out;
}

void Reminders::save(Task& t, const Collection& list, std::time_t now) {
    if (!list.writable)
        throw std::runtime_error("the list \"" + list.name + "\" is read-only");
    if (t.title.find_first_not_of(" \t\r\n") == std::string::npos)
        throw std::invalid_argument("a reminder needs a title");
    if (t.uid.empty())
        t.uid = new_uid();
    std::string ics = write_task(t, now);
    bool moving = !t.href.empty() && t.list_href != list.href;
    std::string href = t.href.empty() || moving ? href_for(list, t.uid) : t.href;
    std::string etag = dav_.put(href, ics, t.href.empty() || moving ? "" : t.etag);
    if (moving)
        finish_move(href, etag, t.href, t.etag);
    if (etag.empty()) {  // the server did not say: read back what it stored, every field of it
        Resource r = dav_.get(href);
        auto fresh = parse_task(r.data);
        if (!fresh)
            throw std::runtime_error("the server stored something that is not a reminder at " + href);
        t = *fresh;
        etag = r.etag, ics = r.data;
    }
    t.href = href;
    t.etag = etag;
    t.raw = ics;
    t.list = list.name;
    t.list_href = list.href;
}

bool Reminders::set_done(Task& t, bool done, std::time_t now) {
    discover();
    auto it = std::find_if(lists_.begin(), lists_.end(), [&](const Collection& c) { return c.href == t.list_href; });
    if (it == lists_.end())
        throw std::runtime_error("the reminder's list is not one of the account's");
    Task next = t;
    bool advanced = false;
    if (done && !t.repeat.empty() && advance(next)) {
        advanced = true;
    } else {
        next.done = done;
        next.completed = done ? now : 0;
    }
    save(next, *it, now);
    t = next;
    return advanced;
}

void Reminders::remove(const Task& t) {
    if (t.href.empty())
        return;
    dav_.remove(t.href, t.etag);
}

std::vector<Collection> Reminders::calendars() {
    discover();
    std::vector<Collection> out;
    const auto& wanted = dav_.account().calendars;
    for (auto& c : all_)
        if (c.events && (wanted.empty() || std::any_of(wanted.begin(), wanted.end(),
                                                         [&](const std::string& n) { return lower(n) == lower(c.name); })))
            out.push_back(c);
    return out;
}

const Collection& Reminders::calendar_named(const std::string& name) {
    discover();
    for (auto& c : all_)
        if (c.events && lower(c.name) == lower(name))
            return c;
    std::string names;
    for (auto& c : calendars())
        names += (names.empty() ? "" : ", ") + c.name;
    throw std::runtime_error("no calendar named \"" + name + "\" (there are " + names + ")");
}

const Collection& Reminders::default_calendar() {
    discover();
    const std::string& name = dav_.account().default_calendar;
    if (!name.empty())
        return calendar_named(name);
    for (auto& c : all_)
        if (c.events && c.writable && lower(c.name) == "personal")
            return c;
    for (auto& c : all_)
        if (c.events && c.writable)
            return c;
    throw std::runtime_error("the account has no calendar it can write to");
}

std::vector<Event> Reminders::find_events(const std::string& uid) {
    std::vector<Event> out;
    for (auto& c : calendars())
        for (auto& r : dav_.find_uid(c, "VEVENT", uid)) {
            auto events = parse_events(r.data);
            if (events.empty())
                continue;
            Event e = events.front();
            for (auto& x : events)  // the series' main event, not an exception
                if (x.repeat.size() || !x.recurring) {
                    e = x;
                    break;
                }
            if (!e.uid.starts_with(uid) && e.uid != uid)
                continue;  // text-match finds the UID anywhere in it; only prefixes count
            e.calendar = c.name, e.calendar_href = c.href, e.color = c.color;
            e.href = r.href, e.etag = r.etag, e.raw = r.data;
            out.push_back(std::move(e));
        }
    return out;
}

void Reminders::save_event(Event& e, const Collection& calendar, std::time_t now) {
    if (!calendar.writable)
        throw std::runtime_error("the calendar \"" + calendar.name + "\" is read-only");
    if (e.uid.empty())
        e.uid = new_uid();
    std::string ics = write_event(e, now);
    bool moving = !e.href.empty() && e.calendar_href != calendar.href;
    std::string href = e.href.empty() || moving ? href_for(calendar, e.uid) : e.href;
    std::string etag = dav_.put(href, ics, e.href.empty() || moving ? "" : e.etag);
    if (moving)
        finish_move(href, etag, e.href, e.etag);
    if (etag.empty()) {
        Resource r = dav_.get(href);
        auto events = parse_events(r.data);
        if (events.empty())
            throw std::runtime_error("the server stored something that is not an event at " + href);
        std::string keep_uid = e.uid;
        e = events.front();
        e.uid = keep_uid;
        etag = r.etag, ics = r.data;
    }
    e.href = href, e.etag = etag, e.raw = ics;
    e.calendar = calendar.name, e.calendar_href = calendar.href, e.color = calendar.color;
}

void Reminders::remove_event(const Event& e) {
    if (!e.href.empty())
        dav_.remove(e.href, e.etag);
}

std::vector<Event> Reminders::agenda(std::time_t from, std::time_t to) {
    discover();
    std::vector<Event> out;
    for (auto& c : calendars()) {
        for (auto& r : dav_.query(c, "VEVENT", from, to))
            for (auto& e : parse_events(r.data)) {
                e.href = r.href;
                e.etag = r.etag;
                e.calendar_href = c.href;
                if (e.end <= from && !(e.end == e.start && e.start >= from))
                    continue;
                if (e.start >= to)
                    continue;
                e.calendar = c.name;
                e.color = c.color;
                out.push_back(std::move(e));
            }
    }
    std::stable_sort(out.begin(), out.end(), [](const Event& a, const Event& b) {
        if (a.start != b.start)
            return a.start < b.start;
        return a.all_day && !b.all_day;
    });
    return out;
}

}  // namespace jotdo
