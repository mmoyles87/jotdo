// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
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
  jotdo [options]                          browse and edit reminders
  jotdo [options] list [LIST]              open reminders, by due date
  jotdo [options] today                    reminders due today, and overdue ones
  jotdo [options] all [LIST]               open and finished reminders
  jotdo [options] show REMINDER            print a reminder
  jotdo [options] new [FIELDS] [TITLE]     add a reminder; without a title, in $EDITOR
  jotdo [options] set REMINDER FIELDS      change only the fields given
  jotdo [options] edit REMINDER [FILE]     edit in $EDITOR, or replace from FILE
  jotdo [options] done REMINDER...         tick off (a repeating one moves to its next date)
  jotdo [options] undone REMINDER          tick back on
  jotdo [options] rm REMINDER              delete
  jotdo [options] agenda [DAYS]            events and reminders for the coming days
  jotdo [options] lists                    the account's lists

REMINDER is a number from `list`, `today` or `all`, or the start of a reminder's UID.

fields, for new and set:
  -t, --title TEXT       (set only; new takes the title as its arguments)
  -l, --list NAME        which list (new: the account's default_list)
  -d, --due WHEN         tomorrow 9am, fri, oct 24, 2026-10-24 18:00, in 2h, ...; none to clear
  -r, --repeat RULE      daily, weekdays, weekly, every 2 weeks, every mon, monthly, ...; none
  -A, --alert WHEN       at due, 15m before, 1h before, ...; none
  -p, --priority LEVEL   high, medium, low, none
  -n, --notes TEXT       replaces the notes; "" clears them
  -u, --url URL

options:
  -a, --account NAME     an account from the config (default: the config's `default`)
  --json                 print JSON instead of text (every command but the browser)
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
size_t find(const std::vector<jotdo::Task>& tasks, const std::string& ref) {
    if (!ref.empty() && ref.size() <= 6 && ref.find_first_not_of("0123456789") == std::string::npos) {
        size_t i = std::stoul(ref);
        if (i >= 1 && i <= tasks.size())
            return i - 1;
    }
    std::optional<size_t> hit;
    for (size_t i = 0; i < tasks.size(); ++i) {
        const auto& t = tasks[i];
        if (t.uid.size() >= ref.size() && strncasecmp(t.uid.c_str(), ref.c_str(), ref.size()) == 0) {
            if (hit)
                throw std::runtime_error(ref + " matches more than one reminder");
            hit = i;
        }
    }
    if (!hit)
        throw std::runtime_error("no reminder matches " + ref);
    return *hit;
}

// --- JSON -----------------------------------------------------------------------

std::string quote(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out + "\"";
}

std::string iso(std::time_t t, bool with_time) {
    char buf[40];
    std::tm tm{};
    localtime_r(&t, &tm);
    if (!with_time) {
        std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
        return buf;
    }
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S%z", &tm);
    std::string s = buf;  // +hhmm → +hh:mm
    s.insert(s.size() - 2, ":");
    return s;
}

std::string json_task(const jotdo::Task& t, size_t n, std::time_t now) {
    std::string alerts = "[";
    for (size_t k = 0; k < t.alerts.size(); ++k)
        alerts += (k ? "," : "") + quote(jotdo::describe_alerts({t.alerts[k]}));
    alerts += "]";
    std::string o = "{";
    o += "\"n\":" + std::to_string(n);
    o += ",\"uid\":" + quote(t.uid);
    o += ",\"title\":" + quote(t.title);
    o += ",\"list\":" + quote(t.list);
    o += ",\"done\":" + std::string(t.done ? "true" : "false");
    o += ",\"due\":" + (t.due ? quote(iso(t.due->t, t.due->has_time)) : "null");
    o += ",\"due_text\":" + (t.due ? quote(jotdo::format_due(*t.due, now)) : "null");
    o += ",\"overdue\":" + std::string(t.due && !t.done && jotdo::is_overdue(*t.due, now) ? "true" : "false");
    o += ",\"repeat\":" + (t.repeat.empty() ? "null" : quote(jotdo::describe_repeat(t.repeat)));
    o += ",\"rrule\":" + (t.repeat.empty() ? "null" : quote(t.repeat));
    o += ",\"alerts\":" + alerts;
    o += ",\"priority\":" + (t.priority ? quote(jotdo::describe_priority(t.priority)) : "null");
    o += ",\"notes\":" + quote(t.notes);
    o += ",\"url\":" + (t.url.empty() ? "null" : quote(t.url));
    o += ",\"completed\":" + (t.done && t.completed ? quote(iso(t.completed, true)) : "null");
    return o + "}";
}

std::string json_event(const jotdo::Event& e) {
    std::string o = "{";
    o += "\"title\":" + quote(e.title);
    o += ",\"start\":" + quote(iso(e.start, !e.all_day));
    o += ",\"end\":" + quote(iso(e.end, !e.all_day));
    o += ",\"all_day\":" + std::string(e.all_day ? "true" : "false");
    o += ",\"location\":" + (e.location.empty() ? "null" : quote(e.location));
    o += ",\"calendar\":" + quote(e.calendar);
    return o + "}";
}

// --- text -----------------------------------------------------------------------

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
    std::string where = show_list ? "  · " + t.list : "";
    std::printf("%3zu  %s  %s  %s%s%s\n", n, t.done ? "✓" : "○", pad(due, 15).c_str(), t.title.c_str(), marks.c_str(),
                where.c_str());
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

int agenda(jotdo::Reminders& r, int days, std::time_t now, bool json) {
    std::time_t from = jotdo::local_midnight(now);
    std::tm tm{};
    localtime_r(&from, &tm);
    tm.tm_mday += days, tm.tm_isdst = -1;
    std::time_t to = std::mktime(&tm);
    auto events = r.agenda(from, to);
    auto tasks = r.load();

    if (json) {
        std::string out = "{\"from\":" + quote(iso(from, false)) + ",\"to\":" + quote(iso(to, false)) + ",\"events\":[";
        for (size_t k = 0; k < events.size(); ++k)
            out += (k ? "," : "") + json_event(events[k]);
        out += "],\"reminders\":[";
        bool first = true;
        for (size_t i = 0; i < tasks.size(); ++i) {
            auto& t = tasks[i];
            if (t.done || !t.due || t.due->t >= to)
                continue;
            out += (first ? "" : ",") + json_task(t, i + 1, now);
            first = false;
        }
        std::printf("%s]}\n", out.c_str());
        return 0;
    }

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

// --- fields ---------------------------------------------------------------------

// The field flags for new and set, applied to `t` and `list`. Returns the words that are not
// flags (new's title). `set` passes allow_title so -t is accepted.
std::vector<std::string> apply_fields(const std::vector<std::string>& args, size_t from, jotdo::Task& t, std::string& list,
                                      jotdo::Reminders& r, std::time_t now, bool allow_title) {
    std::vector<std::string> words;
    for (size_t i = from; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= args.size())
                throw std::invalid_argument(a + " needs a value");
            return args[++i];
        };
        if ((a == "-t" || a == "--title") && allow_title) {
            t.title = value();
            if (t.title.find_first_not_of(" \t") == std::string::npos)
                throw std::invalid_argument("the title cannot be empty");
        } else if (a == "-l" || a == "--list") {
            list = r.list_named(value()).name;
        } else if (a == "-d" || a == "--due") {
            t.due = jotdo::parse_due(value(), now);
            if (!t.due)
                t.repeat.clear();  // no date, nothing to repeat from
        } else if (a == "-r" || a == "--repeat") {
            t.repeat = jotdo::parse_repeat(value());
        } else if (a == "-A" || a == "--alert") {
            t.alerts = jotdo::parse_alerts(value());
        } else if (a == "-p" || a == "--priority") {
            t.priority = jotdo::parse_priority(value());
        } else if (a == "-n" || a == "--notes") {
            t.notes = value();
        } else if (a == "-u" || a == "--url") {
            t.url = value();
        } else if (a == "--") {
            for (++i; i < args.size(); ++i)
                words.push_back(args[i]);
        } else if (a.starts_with("-") && a.size() > 1) {
            throw std::invalid_argument("unknown option " + a);
        } else {
            words.push_back(a);
        }
    }
    if (!t.repeat.empty() && !t.due)
        throw std::invalid_argument("a repeating reminder needs a due date (-d)");
    return words;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    std::string account;
    bool json = false;
    if (auto it = std::find(args.begin(), args.end(), "--json"); it != args.end())
        json = true, args.erase(it);
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

    auto fail = [&](const std::string& message) {
        if (json)
            std::printf("{\"error\":%s}\n", quote(message).c_str());
        else
            std::fprintf(stderr, "jotdo: %s\n", message.c_str());
        return 1;
    };

    try {
        auto config = jotdo::Config::load(jotdo::Config::default_path());
        const jotdo::Account& acc = config.account(account);
        if (args.empty()) {
            if (json)
                return fail("--json needs a command");
            return jotdo::run_tui(acc, config);
        }

        jotdo::Dav dav(acc);
        jotdo::Reminders reminders(dav);
        std::time_t now = std::time(nullptr);
        const std::string cmd = args[0];
        bool many_lists = reminders.lists().size() > 1;

        // One reminder after a change: its line, or its JSON.
        auto report = [&](const char* verb, const jotdo::Task& t) {
            if (json) {
                auto fresh = reminders.load();
                size_t n = 0;
                for (size_t i = 0; i < fresh.size(); ++i)
                    if (fresh[i].uid == t.uid)
                        n = i + 1;
                std::printf("{\"result\":%s,\"reminder\":%s}\n", quote(verb).c_str(), json_task(t, n, now).c_str());
            } else {
                std::printf("%s: %s%s · %s\n", verb, t.title.c_str(), t.due ? ("  " + jotdo::format_due(*t.due, now)).c_str() : "",
                            t.list.c_str());
            }
        };

        if ((cmd == "list" || cmd == "all" || cmd == "today") && args.size() <= 2) {
            auto tasks = reminders.load();
            std::string only = args.size() == 2 ? reminders.list_named(args[1]).name : "";
            std::string out = "[";
            bool any = false;
            for (size_t i = 0; i < tasks.size(); ++i) {
                auto& t = tasks[i];
                if (!only.empty() && t.list != only)
                    continue;
                if (cmd != "all" && t.done)
                    continue;
                if (cmd == "today" && (!t.due || !(jotdo::is_today(*t.due, now) || jotdo::is_overdue(*t.due, now))))
                    continue;
                if (json)
                    out += (any ? "," : "") + json_task(t, i + 1, now);
                else
                    print_task(i + 1, t, now, many_lists && only.empty());
                any = true;
            }
            if (json)
                std::printf("%s]\n", out.c_str());
            else if (!any)
                std::printf(cmd == "today" ? "nothing due today\n" : "no reminders\n");
            return 0;
        }
        if (cmd == "show" && args.size() == 2) {
            auto tasks = reminders.load();
            size_t i = find(tasks, args[1]);
            if (json)
                std::printf("%s\n", json_task(tasks[i], i + 1, now).c_str());
            else
                print_full(tasks[i], now);
            return 0;
        }
        if (cmd == "lists" && args.size() == 1) {
            auto tasks = reminders.load();
            std::string out = "[";
            bool first = true;
            for (auto& l : reminders.lists()) {
                size_t open = 0;
                for (auto& t : tasks)
                    open += t.list_href == l.href && !t.done;
                if (json) {
                    out += std::string(first ? "" : ",") + "{\"name\":" + quote(l.name) + ",\"open\":" + std::to_string(open) +
                           ",\"writable\":" + (l.writable ? "true" : "false") +
                           ",\"default\":" + (l.href == reminders.default_list().href ? "true" : "false") + "}";
                    first = false;
                } else {
                    std::printf("%-20s %3zu open%s\n", l.name.c_str(), open, l.writable ? "" : "  (read-only)");
                }
            }
            if (json)
                std::printf("%s]\n", out.c_str());
            return 0;
        }
        if (cmd == "agenda" && args.size() <= 2) {
            int days = args.size() == 2 ? std::stoi(args[1]) : config.agenda_days;
            return agenda(reminders, std::clamp(days, 1, 366), now, json);
        }
        if (cmd == "new") {
            jotdo::Task t;
            std::string list = reminders.default_list().name;
            auto words = apply_fields(args, 1, t, list, reminders, now, false);
            std::string title;
            for (auto& w : words)
                title += (title.empty() ? "" : " ") + w;
            if (title.empty()) {
                if (json)
                    return fail("new needs a title");
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
            reminders.save(t, reminders.list_named(list), now);
            report("added", t);
            return 0;
        }
        if (cmd == "set" && args.size() >= 3) {
            auto tasks = reminders.load();
            jotdo::Task t = tasks[find(tasks, args[1])];
            std::string list = t.list;
            auto words = apply_fields(args, 2, t, list, reminders, now, true);
            if (!words.empty())
                throw std::invalid_argument("set takes fields, not words (did you mean -t \"" + words[0] + "\"?)");
            reminders.save(t, reminders.list_named(list), now);
            report("saved", t);
            return 0;
        }
        if (cmd == "edit" && (args.size() == 2 || args.size() == 3)) {
            auto tasks = reminders.load();
            jotdo::Task t = tasks[find(tasks, args[1])];
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
            if (json)
                report("saved", t);
            return 0;
        }
        if ((cmd == "done" && args.size() >= 2) || (cmd == "undone" && args.size() == 2)) {
            auto tasks = reminders.load();
            std::vector<size_t> picked;
            for (size_t i = 1; i < args.size(); ++i)
                picked.push_back(find(tasks, args[i]));  // all looked up before any changes the numbering
            std::string out = "[";
            for (size_t k = 0; k < picked.size(); ++k) {
                jotdo::Task& t = tasks[picked[k]];
                bool moved = reminders.set_done(t, cmd == "done", now);
                const char* verb = moved ? "moved to next date" : cmd == "done" ? "done" : "reopened";
                if (json)
                    out += (k ? "," : "") + std::string("{\"result\":") + quote(verb) + ",\"reminder\":" + json_task(t, 0, now) + "}";
                else if (moved)
                    std::printf("next: %s  %s\n", t.title.c_str(), jotdo::format_due(*t.due, now).c_str());
                else
                    std::printf("%s: %s\n", verb, t.title.c_str());
            }
            if (json)
                std::printf("%s]\n", out.c_str());
            return 0;
        }
        if (cmd == "rm" && args.size() == 2) {
            auto tasks = reminders.load();
            const jotdo::Task& t = tasks[find(tasks, args[1])];
            reminders.remove(t);
            if (json)
                std::printf("{\"result\":\"deleted\",\"reminder\":%s}\n", json_task(t, 0, now).c_str());
            else
                std::printf("deleted: %s\n", t.title.c_str());
            return 0;
        }
        if (json)
            return fail("unknown command or wrong arguments; see jotdo --help");
        std::fputs(USAGE, stderr);
        return 2;
    } catch (const jotdo::Conflict&) {
        return fail("the reminder changed on the server since it was loaded; nothing was saved");
    } catch (const std::exception& e) {
        return fail(e.what());
    }
}
