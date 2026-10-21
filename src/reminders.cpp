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
        dav_.remove(t.href, t.etag);
    if (etag.empty()) {  // the server changed what it stored, or did not say: read it back
        Resource r = dav_.get(href);
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

std::vector<Event> Reminders::agenda(std::time_t from, std::time_t to) {
    discover();
    std::vector<Event> out;
    const auto& wanted = dav_.account().calendars;
    for (auto& c : all_) {
        if (!c.events)
            continue;
        if (!wanted.empty() &&
            std::none_of(wanted.begin(), wanted.end(), [&](const std::string& n) { return lower(n) == lower(c.name); }))
            continue;
        for (auto& r : dav_.query(c, "VEVENT", from, to))
            for (auto& e : parse_events(r.data)) {
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
