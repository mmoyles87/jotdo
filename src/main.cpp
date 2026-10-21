// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <strings.h>
#include <unistd.h>

#include <ftxui/screen/string.hpp>

#include "config.hpp"
#include "dav.hpp"
#include "editor.hpp"
#include "form.hpp"
#include "ical.hpp"
#include "reminders.hpp"
#include "tui.hpp"
#include "when.hpp"

namespace {

const char* USAGE = R"(jotdo: reminders from your CalDAV server, in a terminal

usage:
  jotdo [options]                       browse and edit reminders
  jotdo [options] list [LIST]           open reminders, by due date
  jotdo [options] today                 reminders due today, and overdue ones
  jotdo [options] all [LIST]            open and finished reminders
  jotdo [options] show REMINDER         print a reminder
  jotdo [options] new [FIELDS] [TITLE]  add a reminder; without a title, in $EDITOR
  jotdo [options] edit REMINDER [FILE]  edit in $EDITOR, or replace from FILE
  jotdo [options] done REMINDER...      tick off (a repeating one moves to its next date)
  jotdo [options] undone REMINDER       tick back on
  jotdo [options] rm REMINDER           delete
  jotdo [options] agenda [DAYS]         events and reminders for the coming days
  jotdo [options] lists                 the account's lists

REMINDER is a number from `list`, `today` or `all`, or the start of a reminder's UID.

fields for new:
  -l, --list NAME        which list (default: the account's default_list)
  -d, --due WHEN         tomorrow 9am, fri, oct 24, 2026-10-24 18:00, in 2h, ...
  -r, --repeat RULE      daily, weekdays, weekly, every 2 weeks, every mon, monthly, ...
  -A, --alert WHEN       at due, 15m before, 1h before, ...
  -p, --priority LEVEL   high, medium, low
  -n, --notes TEXT

options:
  -a, --account NAME     an account from the config (default: the config's `default`)
  -h, --help

Accounts are read from ~/.config/jotdo/config.toml.
)";

std::string read_input(const std::string& file) {
    std::ostringstream s;
    if (file.empty() || file == "-") {
        s << std::cin.rdbuf();
    } else {
        std::ifstream f(file, std::ios::binary);
        if (!f)
            throw std::runtime_error("cannot read " + file);
        s << f.rdbuf();
    }
    return s.str();
}

// A number in range is a position in the full listing; anything else, digits included, a UID prefix.
jotdo::Task& find(std::vector<jotdo::Task>& tasks, const std::string& ref) {
    if (!ref.empty() && ref.size() <= 6 && ref.find_first_not_of("0123456789") == std::string::npos) {
        size_t i = std::stoul(ref);
        if (i >= 1 && i <= tasks.size())
            return tasks[i - 1];
    }
    jotdo::Task* hit = nullptr;
    for (auto& t : tasks)
        if (t.uid.size() >= ref.size() && strncasecmp(t.uid.c_str(), ref.c_str(), ref.size()) == 0) {
            if (hit)
                throw std::runtime_error(ref + " matches more than one reminder");
            hit = &t;
        }
    if (!hit)
        throw std::runtime_error("no reminder matches " + ref);
    return *hit;
}

std::string pad(const std::string& s, int cols) {
    int w = ftxui::string_width(s);
    return w >= cols ? s : s + std::string(cols - w, ' ');
}

void print_task(size_t n, const jotdo::Task& t, std::time_t now, bool show_list) {
    std::string due = t.due ? jotdo::format_due(*t.due, now) : "";
    if (t.due && !t.done && jotdo::is_overdue(*t.due, now))
        due = "! " + due;
    std::string marks;
    if (!t.repeat.empty())
        marks += " ↻";
    if (t.priority >= 1 && t.priority <= 4)
        marks += " !!!";
    else if (t.priority == 5)
        marks += " !!";
    if (!t.notes.empty())
        marks += " …";
    std::printf("%3zu  %s  %s  %s%s%s\n", n, t.done ? "✓" : "○", pad(due, 15).c_str(), t.title.c_str(), marks.c_str(),
                show_list ? ("  · " + t.list).c_str() : "");
}

void print_full(const jotdo::Task& t, std::time_t now) {
    std::printf("%s%s\n", t.done ? "✓ " : "", t.title.c_str());
    if (t.due)
        std::printf("due: %s (%s)\n", jotdo::format_due_exact(*t.due).c_str(), jotdo::format_due(*t.due, now).c_str());
    if (!t.repeat.empty())
        std::printf("repeat: %s\n", jotdo::describe_repeat(t.repeat).c_str());
    if (!t.alerts.empty())
        std::printf("alert: %s\n", jotdo::describe_alerts(t.alerts).c_str());
    if (t.priority)
        std::printf("priority: %s\n", jotdo::describe_priority(t.priority).c_str());
    std::printf("list: %s\n", t.list.c_str());
    if (!t.url.empty())
        std::printf("url: %s\n", t.url.c_str());
    if (!t.notes.empty())
        std::printf("\n%s\n", t.notes.c_str());
}

std::string clock(std::time_t t) {
    char buf[16];
    std::tm tm{};
    localtime_r(&t, &tm);
    std::strftime(buf, sizeof buf, "%H:%M", &tm);
    return buf;
}

std::string day_heading(std::time_t day, std::time_t now) {
    char buf[32];
    std::tm tm{};
    localtime_r(&day, &tm);
    std::strftime(buf, sizeof buf, "%a %b %d", &tm);
    std::string rel = jotdo::format_due({day, false}, now);
    return (rel == "Today" || rel == "Tomorrow") ? rel + " · " + buf : buf;
}

int agenda(jotdo::Reminders& r, int days, std::time_t now) {
    std::time_t from = jotdo::local_midnight(now);
    std::tm tm{};
    localtime_r(&from, &tm);
    tm.tm_mday += days, tm.tm_isdst = -1;
    std::time_t to = std::mktime(&tm);
    auto events = r.agenda(from, to);
    auto tasks = r.load();
    std::map<std::time_t, std::vector<std::string>> by_day;
    for (auto& e : events) {
        std::time_t day = jotdo::local_midnight(std::max(e.start, from));
        std::string when = e.all_day ? pad("all day", 13) : pad(clock(e.start) + "–" + clock(e.end), 13);
        by_day[day].push_back("  " + when + "  " + e.title + (e.calendar.empty() ? "" : "  · " + e.calendar));
    }
    for (size_t i = 0; i < tasks.size(); ++i) {
        auto& t = tasks[i];
        if (t.done || !t.due || t.due->t >= to)
            continue;
        std::time_t day = jotdo::local_midnight(t.due->t);
        if (day < from)
            day = from;  // overdue: shown today
        std::string when = t.due->has_time && t.due->t >= from ? clock(t.due->t) : (t.due->t < from ? "overdue" : "");
        by_day[day].push_back("  ○ " + pad(when, 11) + "  " + t.title + "  · " + t.list + "  [" + std::to_string(i + 1) + "]");
    }
    if (by_day.empty())
        std::printf("nothing in the next %d days\n", days);
    bool first = true;
    for (auto& [day, lines] : by_day) {
        std::printf("%s%s\n", first ? "" : "\n", day_heading(day, now).c_str());
        first = false;
        for (auto& l : lines)
            std::printf("%s\n", l.c_str());
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    std::string account;
    while (!args.empty() && (args[0] == "-a" || args[0] == "--account" || args[0] == "-h" || args[0] == "--help")) {
        if (args[0] == "-h" || args[0] == "--help") {
            std::fputs(USAGE, stdout);
            return 0;
        }
        if (args.size() < 2) {
            std::fputs(USAGE, stderr);
            return 2;
        }
        account = args[1];
        args.erase(args.begin(), args.begin() + 2);
    }

    try {
        auto config = jotdo::Config::load(jotdo::Config::default_path());
        const jotdo::Account& acc = config.account(account);
        if (args.empty())
            return jotdo::run_tui(acc, config);

        jotdo::Dav dav(acc);
        jotdo::Reminders reminders(dav);
        std::time_t now = std::time(nullptr);
        const std::string cmd = args[0];
        bool many_lists = reminders.lists().size() > 1;

        if ((cmd == "list" || cmd == "all" || cmd == "today") && args.size() <= 2) {
            auto tasks = reminders.load();
            std::string only = args.size() == 2 ? reminders.list_named(args[1]).name : "";
            bool any = false;
            for (size_t i = 0; i < tasks.size(); ++i) {
                auto& t = tasks[i];
                if (!only.empty() && t.list != only)
                    continue;
                if (cmd != "all" && t.done)
                    continue;
                if (cmd == "today" && (!t.due || !(jotdo::is_today(*t.due, now) || jotdo::is_overdue(*t.due, now))))
                    continue;
                print_task(i + 1, t, now, many_lists && only.empty());
                any = true;
            }
            if (!any)
                std::printf(cmd == "today" ? "nothing due today\n" : "no reminders\n");
            return 0;
        }
        if (cmd == "show" && args.size() == 2) {
            auto tasks = reminders.load();
            print_full(find(tasks, args[1]), now);
            return 0;
        }
        if (cmd == "lists" && args.size() == 1) {
            auto tasks = reminders.load();
            for (auto& l : reminders.lists()) {
                size_t open = 0;
                for (auto& t : tasks)
                    open += t.list_href == l.href && !t.done;
                std::printf("%-20s %3zu open%s\n", l.name.c_str(), open, l.writable ? "" : "  (read-only)");
            }
            return 0;
        }
        if (cmd == "agenda" && args.size() <= 2) {
            int days = args.size() == 2 ? std::stoi(args[1]) : config.agenda_days;
            return agenda(reminders, std::max(1, days), now);
        }
        if (cmd == "new") {
            jotdo::Task t;
            std::string list = reminders.default_list().name;
            std::string title;
            for (size_t i = 1; i < args.size(); ++i) {
                const std::string& a = args[i];
                auto value = [&]() -> std::string {
                    if (i + 1 >= args.size())
                        throw std::invalid_argument(a + " needs a value");
                    return args[++i];
                };
                if (a == "-l" || a == "--list")
                    list = reminders.list_named(value()).name;
                else if (a == "-d" || a == "--due")
                    t.due = jotdo::parse_due(value(), now);
                else if (a == "-r" || a == "--repeat")
                    t.repeat = jotdo::parse_repeat(value());
                else if (a == "-A" || a == "--alert")
                    t.alerts = jotdo::parse_alerts(value());
                else if (a == "-p" || a == "--priority")
                    t.priority = jotdo::parse_priority(value());
                else if (a == "-n" || a == "--notes")
                    t.notes = value();
                else if (a == "--")
                    for (++i; i < args.size(); ++i)
                        title += (title.empty() ? "" : " ") + args[i];
                else if (a.starts_with("-") && a.size() > 1)
                    throw std::invalid_argument("unknown option " + a);
                else
                    title += (title.empty() ? "" : " ") + a;
            }
            if (title.empty()) {
                std::string text;
                if (!isatty(STDIN_FILENO)) {
                    text = read_input("");
                } else {
                    auto edited = jotdo::edit_text(jotdo::form_text(t, list), jotdo::form_text(t, list));
                    if (!edited)
                        return 0;
                    text = *edited;
                }
                try {
                    jotdo::apply_form(text, t, list, now);
                } catch (const std::exception& e) {
                    auto kept = jotdo::keep_unsaved(text);
                    throw std::runtime_error(std::string(e.what()) + "; not saved, your text is in " + kept.string());
                }
            } else {
                t.title = title;
            }
            if (!t.repeat.empty() && !t.due)
                throw std::invalid_argument("a repeating reminder needs a due date (-d)");
            reminders.save(t, reminders.list_named(list), now);
            std::printf("added: %s%s · %s\n", t.title.c_str(), t.due ? ("  " + jotdo::format_due(*t.due, now)).c_str() : "",
                        t.list.c_str());
            return 0;
        }
        if (cmd == "edit" && (args.size() == 2 || args.size() == 3)) {
            auto tasks = reminders.load();
            jotdo::Task t = find(tasks, args[1]);
            std::string list = t.list;
            std::string text;
            if (args.size() == 3 || !isatty(STDIN_FILENO)) {
                text = read_input(args.size() == 3 ? args[2] : "");
            } else {
                auto edited = jotdo::edit_text(jotdo::form_text(t, list));
                if (!edited)
                    return 0;
                text = *edited;
            }
            try {
                jotdo::apply_form(text, t, list, now);
                reminders.save(t, reminders.list_named(list), now);
            } catch (const jotdo::Conflict&) {
                auto kept = jotdo::keep_unsaved(text);
                throw std::runtime_error("the reminder changed on the server while you edited; not saved, your text is in " +
                                         kept.string());
            } catch (const std::exception& e) {
                auto kept = jotdo::keep_unsaved(text);
                throw std::runtime_error(std::string(e.what()) + "; not saved, your text is in " + kept.string());
            }
            return 0;
        }
        if ((cmd == "done" && args.size() >= 2) || (cmd == "undone" && args.size() == 2)) {
            auto tasks = reminders.load();
            std::vector<jotdo::Task*> picked;
            for (size_t i = 1; i < args.size(); ++i)
                picked.push_back(&find(tasks, args[i]));  // all looked up before any changes the numbering
            for (auto* t : picked) {
                bool moved = reminders.set_done(*t, cmd == "done", now);
                if (moved)
                    std::printf("next: %s  %s\n", t->title.c_str(), jotdo::format_due(*t->due, now).c_str());
                else
                    std::printf("%s %s\n", cmd == "done" ? "done:" : "reopened:", t->title.c_str());
            }
            return 0;
        }
        if (cmd == "rm" && args.size() == 2) {
            auto tasks = reminders.load();
            const jotdo::Task& t = find(tasks, args[1]);
            reminders.remove(t);
            std::printf("deleted: %s\n", t.title.c_str());
            return 0;
        }
        std::fputs(USAGE, stderr);
        return 2;
    } catch (const jotdo::Conflict&) {
        std::fprintf(stderr, "jotdo: the reminder changed on the server since it was loaded; nothing was saved\n");
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "jotdo: %s\n", e.what());
        return 1;
    }
}
