// SPDX-License-Identifier: GPL-3.0-or-later
#include "tui.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/string.hpp>
#include <ftxui/screen/terminal.hpp>

#include "dav.hpp"
#include "editor.hpp"
#include "form.hpp"
#include "ical.hpp"
#include "reminders.hpp"
#include "theme.hpp"
#include "when.hpp"

namespace jotdo {

namespace {

using namespace ftxui;
using Key = ftxui::Event;  // a key press or mouse event; Event is a calendar event here

Theme T;  // set by run_tui

constexpr int SIDE_WIDTH = 22;
constexpr int DETAIL_WIDTH = 46;
constexpr auto AUTO_REFRESH = std::chrono::minutes(2);

std::string lower(std::string s) {
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void pop_utf8(std::string& s) {
    while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80)
        s.pop_back();
    if (!s.empty())
        s.pop_back();
}

// At most `cols` display columns of s, with an ellipsis if it was cut.
std::string fit(const std::string& s, int cols) {
    if (cols <= 0)
        return "";
    if (string_width(s) <= cols)
        return s;
    std::string out;
    for (auto& g : Utf8ToGlyphs(s)) {
        if (string_width(out + g) > cols - 1)
            break;
        out += g;
    }
    return out + "…";
}

// Word-wrapped lines of at most `cols` columns.
std::vector<std::string> wrap(const std::string& text, int cols) {
    std::vector<std::string> out;
    size_t a = 0;
    while (a <= text.size()) {
        size_t e = text.find('\n', a);
        std::string line = text.substr(a, e == std::string::npos ? std::string::npos : e - a);
        a = e == std::string::npos ? text.size() + 1 : e + 1;
        std::string cur;
        size_t p = 0;
        while (p < line.size()) {
            size_t sp = line.find(' ', p);
            std::string word = line.substr(p, sp == std::string::npos ? std::string::npos : sp - p);
            p = sp == std::string::npos ? line.size() : sp + 1;
            std::string trial = cur.empty() ? word : cur + " " + word;
            if (string_width(trial) <= cols) {
                cur = trial;
                continue;
            }
            if (!cur.empty())
                out.push_back(cur);
            while (string_width(word) > cols) {  // a word longer than the line
                std::string piece;
                for (auto& g : Utf8ToGlyphs(word)) {
                    if (string_width(piece + g) > cols)
                        break;
                    piece += g;
                }
                out.push_back(piece);
                word = word.substr(piece.size());
            }
            cur = word;
        }
        out.push_back(cur);
    }
    while (!out.empty() && out.back().empty())
        out.pop_back();
    return out;
}

std::string clock(std::time_t t) {
    return format_time(t);
}

std::string day_heading(std::time_t day, std::time_t now) {
    char buf[32];
    std::tm tm{};
    localtime_r(&day, &tm);
    std::strftime(buf, sizeof buf, "%A %b %d", &tm);
    std::string rel = format_due({day, false}, now);
    return rel == "Today" || rel == "Tomorrow" ? rel + " · " + buf : buf;
}

enum class View { Today, Scheduled, All, List, Agenda };

struct SideItem {
    std::string label;
    View view;
    std::string list_href;  // for View::List
};

struct Row {
    enum Kind { Heading, Task, Event } kind;
    size_t index = 0;   // into tasks_ or events_
    std::time_t day = 0;
    std::string text;   // a heading's text
};

class App {
public:
    App(const Account& account, const Config& config)
        : dav_(account), rem_(dav_), agenda_days_(config.agenda_days), screen_(ScreenInteractive::Fullscreen()) {
        screen_.TrackMouse(config.mouse);
    }

    int run() {
        status_ = "loading…";
        auto ui = CatchEvent(Renderer([this] { return render(); }), [this](Key e) { return on_event(e); });
        screen_.Post([this] { refresh(); });
        ticker_ = std::jthread([this](std::stop_token st) {
            std::mutex m;
            std::condition_variable_any cv;
            std::unique_lock lock(m);
            // wait_for returns the predicate's value: true once a stop is requested, which ends
            // the loop; a timeout gives false and posts the refresh.
            while (!cv.wait_for(lock, st, AUTO_REFRESH, [&st] { return st.stop_requested(); }))
                screen_.Post([this] {
                    if (!busy_)
                        refresh(current_uid(), true);
                });
        });
        screen_.Loop(ui);
        ticker_.request_stop();  // before the screen goes away: nothing may be posted to it after the loop
        ticker_.join();
        return 0;
    }

private:
    Dav dav_;
    Reminders rem_;
    int agenda_days_;
    ScreenInteractive screen_;

    std::vector<Task> tasks_;
    std::vector<Event> events_;
    std::time_t events_from_ = 0;
    std::vector<SideItem> side_;
    int side_sel_ = 0;
    std::vector<Row> rows_;
    int row_sel_ = 0;
    bool side_focus_ = false;
    bool show_done_ = false;
    bool busy_ = false;  // the editor is open

    std::string filter_;
    bool filtering_ = false;
    bool adding_ = false;
    std::string add_text_;
    std::string status_;
    std::string pending_delete_;  // uid

    // Where things were drawn in the last frame, for the mouse.
    std::deque<Box> side_boxes_, row_boxes_;
    Box side_box_, rows_box_, search_box_;
    std::deque<std::pair<Box, Key>> buttons_;
    int last_click_row_ = -1;
    std::chrono::steady_clock::time_point last_click_;

    std::jthread ticker_;  // last, so it stops before the rest goes away

    // --- state ----------------------------------------------------------------

    const SideItem& side() const {
        static const SideItem none{"", View::All, ""};  // before the first load succeeds
        return side_.empty() ? none : side_[std::clamp(side_sel_, 0, static_cast<int>(side_.size()) - 1)];
    }

    Task* current() {
        if (rows_.empty())
            return nullptr;
        const Row& r = rows_[std::clamp(row_sel_, 0, static_cast<int>(rows_.size()) - 1)];
        return r.kind == Row::Task ? &tasks_[r.index] : nullptr;
    }

    const Event* current_event() const {
        if (rows_.empty())
            return nullptr;
        const Row& r = rows_[std::clamp(row_sel_, 0, static_cast<int>(rows_.size()) - 1)];
        return r.kind == Row::Event ? &events_[r.index] : nullptr;
    }

    std::string current_uid() {
        Task* t = current();
        return t ? t->uid : "";
    }

    bool selectable(int i) const { return i >= 0 && i < static_cast<int>(rows_.size()) && rows_[i].kind != Row::Heading; }

    void select_row(int i, int dir = 1) {
        if (rows_.empty()) {
            row_sel_ = 0;
            return;
        }
        i = std::clamp(i, 0, static_cast<int>(rows_.size()) - 1);
        int start = i;
        while (!selectable(i)) {
            i += dir;
            if (i < 0 || i >= static_cast<int>(rows_.size())) {
                dir = -dir, i = start;
                while (!selectable(i) && i >= 0 && i < static_cast<int>(rows_.size()))
                    i += dir;
                break;
            }
        }
        if (selectable(i)) {
            if (i != row_sel_)
                pending_delete_.clear();
            row_sel_ = i;
        }
    }

    void build_side() {
        std::string keep = side_.empty() ? "" : side().label;
        side_.clear();
        side_.push_back({"Today", View::Today, ""});
        side_.push_back({"Scheduled", View::Scheduled, ""});
        side_.push_back({"All", View::All, ""});
        for (auto& l : rem_.lists())
            side_.push_back({l.name, View::List, l.href});
        side_.push_back({"Agenda", View::Agenda, ""});
        side_sel_ = 0;
        for (size_t i = 0; i < side_.size(); ++i)
            if (side_[i].label == keep)
                side_sel_ = static_cast<int>(i);
    }

    bool matches(const Task& t) const {
        if (filter_.empty())
            return true;
        std::string q = lower(filter_);
        return lower(t.title).find(q) != std::string::npos || lower(t.notes).find(q) != std::string::npos;
    }

    size_t count(View v, const std::string& href) const {
        std::time_t now = std::time(nullptr);
        size_t n = 0;
        for (auto& t : tasks_) {
            if (t.done)
                continue;
            if (v == View::Today && !(t.due && (is_today(*t.due, now) || is_overdue(*t.due, now))))
                continue;
            if (v == View::Scheduled && !t.due)
                continue;
            if (v == View::List && t.list_href != href)
                continue;
            ++n;
        }
        return n;
    }

    void build_rows(const std::string& keep_uid = "") {
        std::time_t now = std::time(nullptr);
        rows_.clear();
        const SideItem& s = side();
        std::time_t today = local_midnight(now);

        if (s.view == View::Agenda) {
            std::tm tm{};
            localtime_r(&today, &tm);
            tm.tm_mday += agenda_days_, tm.tm_isdst = -1;
            std::time_t end = std::mktime(&tm);
            std::vector<std::tuple<std::time_t, int, Row>> items;  // day, order, row
            for (size_t i = 0; i < events_.size(); ++i) {
                std::time_t day = local_midnight(std::max(events_[i].start, today));
                items.push_back({day, events_[i].all_day ? 0 : 2, Row{Row::Event, i, day, ""}});
            }
            for (size_t i = 0; i < tasks_.size(); ++i) {
                auto& t = tasks_[i];
                if (t.done || !t.due || t.due->t >= end || !matches(t))
                    continue;
                std::time_t day = std::max(local_midnight(t.due->t), today);
                items.push_back({day, 1, Row{Row::Task, i, day, ""}});
            }
            std::stable_sort(items.begin(), items.end(), [&](auto& a, auto& b) {
                if (std::get<0>(a) != std::get<0>(b))
                    return std::get<0>(a) < std::get<0>(b);
                auto when = [&](const Row& r) {
                    return r.kind == Row::Event ? (events_[r.index].all_day ? 0 : events_[r.index].start)
                                                : (tasks_[r.index].due->has_time ? tasks_[r.index].due->t : 0);
                };
                return when(std::get<2>(a)) < when(std::get<2>(b));
            });
            std::time_t last = 0;
            for (auto& [day, _, row] : items) {
                if (day != last)
                    rows_.push_back({Row::Heading, 0, day, day_heading(day, now)}), last = day;
                rows_.push_back(row);
            }
        } else {
            std::time_t last = -1;
            for (size_t i = 0; i < tasks_.size(); ++i) {
                auto& t = tasks_[i];
                if (!matches(t))
                    continue;
                if (t.done && !(show_done_ && s.view == View::List))
                    continue;
                if (s.view == View::List && t.list_href != s.list_href)
                    continue;
                if (s.view == View::Today && !(t.due && (is_today(*t.due, now) || is_overdue(*t.due, now))))
                    continue;
                if (s.view == View::Scheduled) {
                    if (!t.due)
                        continue;
                    bool late = is_overdue(*t.due, now);
                    std::time_t day = late ? today - 1 : std::max(local_midnight(t.due->t), today);  // one Overdue heading
                    if (day != last)
                        rows_.push_back({Row::Heading, 0, day, late ? "Overdue" : day_heading(day, now)});
                    last = day;
                }
                if (t.done && (rows_.empty() || rows_.back().text != "Done") && s.view == View::List)
                    rows_.push_back({Row::Heading, 0, 0, "Done"});
                rows_.push_back({Row::Task, i, 0, ""});
            }
        }
        row_sel_ = 0;
        for (size_t k = 0; k < rows_.size(); ++k)
            if (rows_[k].kind == Row::Task && !keep_uid.empty() && tasks_[rows_[k].index].uid == keep_uid)
                row_sel_ = static_cast<int>(k);
        select_row(row_sel_);
    }

    // keep_uid by value: callers pass a uid that lives in tasks_, which this replaces.
    void refresh(std::string keep_uid = "", bool quiet = false) {
        std::vector<Task> tasks;
        std::vector<Event> events;
        try {
            tasks = rem_.load();
            if (side_.empty())
                build_side();
            std::time_t now = std::time(nullptr);
            std::time_t from = local_midnight(now);
            std::tm tm{};
            localtime_r(&from, &tm);
            tm.tm_mday += agenda_days_, tm.tm_isdst = -1;
            events = rem_.agenda(from, std::mktime(&tm));
            events_from_ = from;
        } catch (const std::exception& e) {
            status_ = std::string("load failed: ") + e.what();
            return;  // the rows still index the old lists, which stay as they were
        }
        tasks_ = std::move(tasks);  // replaced together, with the rows rebuilt right after
        events_ = std::move(events);
        build_rows(keep_uid);
        if (!quiet)
            count_status();
    }

    void count_status() {
        size_t open = 0;
        for (auto& t : tasks_)
            open += !t.done;
        status_ = std::to_string(open) + " open";
        if (!filter_.empty())
            status_ += " · searching “" + filter_ + "”";
    }

    // --- actions --------------------------------------------------------------

    const Collection& list_for_new() {
        const SideItem& s = side();
        if (s.view == View::List)
            for (auto& l : rem_.lists())
                if (l.href == s.list_href)
                    return l;
        return rem_.default_list();
    }

    std::optional<std::string> edit(const std::string& text, const std::optional<std::string>& original = std::nullopt) {
        std::optional<std::string> out;
        std::string error;
        busy_ = true;
        screen_.WithRestoredIO([&] {
            try {
                out = edit_text(text, original);
            } catch (const std::exception& e) {
                error = e.what();
            }
        })();
        busy_ = false;
        if (!error.empty())
            status_ = error;
        return out;
    }

    // Saves `t` (from the form `text`) into the list named `list`; keeps the text if that fails.
    bool save(Task& t, const std::string& list, const std::string& text) {
        try {
            rem_.save(t, rem_.list_named(list), std::time(nullptr));
        } catch (const Conflict&) {
            auto kept = keep_unsaved(text);
            refresh(t.uid);
            status_ = "changed on the server while you edited: not saved; your text is in " + kept.string();
            return false;
        } catch (const std::exception& e) {
            auto kept = keep_unsaved(text);
            status_ = std::string("not saved: ") + e.what() + "; your text is in " + kept.string();
            return false;
        }
        refresh(t.uid);
        return true;
    }

    void edit_current() {
        Task* cur = current();
        if (!cur)
            return;
        Task t = *cur;
        std::string list = t.list;
        auto text = edit(form_text(t, list));
        if (!text)
            return;
        try {
            apply_form(*text, t, list, std::time(nullptr));
        } catch (const std::exception& e) {
            auto kept = keep_unsaved(*text);
            status_ = std::string("not saved: ") + e.what() + "; your text is in " + kept.string();
            return;
        }
        if (save(t, list, *text))
            status_ = "saved “" + t.title + "”";
    }

    void new_task() {
        Task t;
        std::string list = list_for_new().name;
        if (side().view == View::Today)
            t.due = Due{local_midnight(std::time(nullptr)), false};
        std::string blank = form_text(t, list);
        auto text = edit(blank, blank);
        if (!text)
            return;
        try {
            apply_form(*text, t, list, std::time(nullptr));
        } catch (const std::exception& e) {
            auto kept = keep_unsaved(*text);
            status_ = std::string("not saved: ") + e.what() + "; your text is in " + kept.string();
            return;
        }
        if (save(t, list, *text))
            status_ = "added “" + t.title + "”";
    }

    // "Buy milk @ tomorrow 9am": a title, and optionally a due date after the last " @".
    void quick_add(const std::string& input) {
        std::string title = input, when;
        if (auto at = input.rfind(" @"); at != std::string::npos)
            title = input.substr(0, at), when = input.substr(at + 2);
        else if (input.starts_with("@"))
            title.clear();
        Task t;
        t.title = title;
        try {
            if (!when.empty())
                t.due = parse_due(when, std::time(nullptr));
            else if (side().view == View::Today)
                t.due = Due{local_midnight(std::time(nullptr)), false};
            rem_.save(t, list_for_new(), std::time(nullptr));
        } catch (const std::exception& e) {
            status_ = std::string("not added: ") + e.what();
            return;
        }
        refresh(t.uid);
        status_ = "added “" + t.title + "”" + (t.due ? " · " + format_due(*t.due, std::time(nullptr)) : "");
    }

    void toggle_current() {
        Task* cur = current();
        if (!cur)
            return;
        Task t = *cur;
        bool done = !t.done;
        try {
            bool moved = rem_.set_done(t, done, std::time(nullptr));
            refresh(moved || !done ? t.uid : "");
            status_ = moved ? "next: “" + t.title + "” " + format_due(*t.due, std::time(nullptr))
                            : (done ? "done: “" : "reopened: “") + t.title + "”";
        } catch (const Conflict&) {
            refresh(t.uid);
            status_ = "it changed on the server: reloaded, try again";
        } catch (const std::exception& e) {
            status_ = std::string("failed: ") + e.what();
        }
    }

    void delete_current() {
        Task* cur = current();
        if (!cur)
            return;
        if (pending_delete_ != cur->uid) {
            pending_delete_ = cur->uid;
            status_ = "press d again to delete “" + cur->title + "”";
            return;
        }
        pending_delete_.clear();
        std::string title = cur->title;
        try {
            rem_.remove(*cur);
        } catch (const Conflict&) {
            refresh(cur->uid);
            status_ = "it changed on the server: reloaded, not deleted";
            return;
        } catch (const std::exception& e) {
            status_ = std::string("delete failed: ") + e.what();
            return;
        }
        int keep = row_sel_;
        refresh();
        select_row(keep);
        status_ = "deleted “" + title + "”";
    }

    void open_side(int i) {
        if (side_.empty())
            return;
        side_sel_ = std::clamp(i, 0, static_cast<int>(side_.size()) - 1);
        pending_delete_.clear();
        build_rows();
    }

    // --- input ----------------------------------------------------------------

    bool on_mouse(Key e) {
        const Mouse& m = e.mouse();
        if (m.button == Mouse::WheelUp || m.button == Mouse::WheelDown) {
            int dir = m.button == Mouse::WheelDown ? 1 : -1;
            if (side_box_.Contain(m.x, m.y))
                open_side(side_sel_ + dir);
            else
                select_row(row_sel_ + dir, dir);
            return true;
        }
        if (m.button != Mouse::Left || m.motion != Mouse::Pressed)
            return false;
        if (search_box_.Contain(m.x, m.y)) {
            filtering_ = true, adding_ = false;
            return true;
        }
        filtering_ = false;
        for (auto& [box, ev] : buttons_)
            if (box.Contain(m.x, m.y))
                return on_event(ev);
        for (size_t k = 0; k < side_boxes_.size(); ++k)
            if (side_boxes_[k].Contain(m.x, m.y)) {
                side_focus_ = false;
                open_side(static_cast<int>(k));
                return true;
            }
        for (size_t k = 0; k < row_boxes_.size(); ++k)
            if (row_boxes_[k].Contain(m.x, m.y) && selectable(static_cast<int>(k))) {
                side_focus_ = false;
                bool on_circle = m.x <= row_boxes_[k].x_min + 3;
                auto now = std::chrono::steady_clock::now();
                bool twice = static_cast<int>(k) == last_click_row_ && now - last_click_ < std::chrono::milliseconds(400);
                last_click_row_ = twice ? -1 : static_cast<int>(k), last_click_ = now;
                select_row(static_cast<int>(k));
                if (on_circle && current())
                    toggle_current();
                else if (twice && current())
                    edit_current();
                return true;
            }
        return true;
    }

    bool on_event(const Key& e) {
        if (e.is_mouse())
            return on_mouse(e);
        if (filtering_ || adding_) {
            std::string& buf = filtering_ ? filter_ : add_text_;
            if (e == Key::Return) {
                if (adding_) {
                    std::string text = add_text_;
                    adding_ = false, add_text_.clear();
                    if (text.find_first_not_of(' ') != std::string::npos)
                        quick_add(text);
                } else {
                    filtering_ = false;
                }
            } else if (e == Key::Escape) {
                if (filtering_) {
                    filtering_ = false, filter_.clear();
                    build_rows(current_uid());
                    count_status();
                } else {
                    adding_ = false, add_text_.clear();
                    status_ = "";
                }
            } else if (e == Key::Backspace) {
                pop_utf8(buf);
                if (filtering_)
                    build_rows(), count_status();
            } else if (e == Key::ArrowDown && filtering_) {
                filtering_ = false;
            } else if (e.is_character()) {
                buf += e.character();
                if (filtering_)
                    build_rows(), count_status();
            } else {
                return false;
            }
            return true;
        }

        if (e == Key::Character('q')) {
            screen_.Exit();
        } else if (e == Key::Tab || e == Key::Character('h') || e == Key::Character('l') || e == Key::ArrowLeft ||
                   e == Key::ArrowRight) {
            side_focus_ = e == Key::Tab ? !side_focus_ : (e == Key::Character('h') || e == Key::ArrowLeft);
        } else if (e == Key::Character('j') || e == Key::ArrowDown) {
            side_focus_ ? open_side(side_sel_ + 1) : select_row(row_sel_ + 1, 1);
        } else if (e == Key::Character('k') || e == Key::ArrowUp) {
            side_focus_ ? open_side(side_sel_ - 1) : select_row(row_sel_ - 1, -1);
        } else if (e == Key::Character('g') || e == Key::Home) {
            side_focus_ ? open_side(0) : select_row(0, 1);
        } else if (e == Key::Character('G') || e == Key::End) {
            side_focus_ ? open_side(static_cast<int>(side_.size()) - 1) : select_row(static_cast<int>(rows_.size()) - 1, -1);
        } else if (e.is_character() && e.character().size() == 1 && e.character()[0] >= '1' && e.character()[0] <= '9') {
            open_side(e.character()[0] - '1');
        } else if (e == Key::Return) {
            if (side_focus_)
                side_focus_ = false;
            else
                edit_current();
        } else if (e == Key::Character('e')) {
            edit_current();
        } else if (e == Key::Character(' ') || e == Key::Character('x')) {
            toggle_current();
        } else if (e == Key::Character('n')) {
            new_task();
        } else if (e == Key::Character('a')) {
            adding_ = true, add_text_.clear();
        } else if (e == Key::Character('d')) {
            delete_current();
        } else if (e == Key::Character('c')) {
            show_done_ = !show_done_;
            build_rows(current_uid());
            status_ = show_done_ ? "showing finished reminders in lists" : "hiding finished reminders";
        } else if (e == Key::Character('/')) {
            filtering_ = true;
        } else if (e == Key::Character('r')) {
            status_ = "loading…";
            refresh(current_uid());
        } else if (e == Key::Escape) {
            pending_delete_.clear();
            if (!filter_.empty()) {
                filter_.clear();
                build_rows(current_uid());
                count_status();
            }
        } else {
            return false;
        }
        return true;
    }

    // --- drawing --------------------------------------------------------------

    Element render() {
        side_boxes_.clear(), row_boxes_.clear(), buttons_.clear();
        std::time_t now = std::time(nullptr);
        Element bar = text(" jotdo · " + dav_.account().user + " · " + status_) | color(T.status) | bgcolor(T.panel);

        Element prompt;
        if (adding_)
            prompt = hbox(text(" + " + add_text_) | color(T.text), text("▏") | color(T.accent),
                          text("   title @ when, e.g. Call Mum @ fri 6pm") | color(T.muted)) |
                     bgcolor(T.highlight);
        else if (filtering_)
            prompt = hbox(text(" /" + filter_) | color(T.text), text("▏") | color(T.accent)) | bgcolor(T.highlight);
        else if (!filter_.empty())
            prompt = text(" /" + filter_) | color(T.text) | bgcolor(T.search);
        else
            prompt = text(" / search · a add") | color(T.muted) | bgcolor(T.search);
        prompt = prompt | reflect(search_box_);

        auto dim = Terminal::Size();
        bool detail = dim.dimx >= SIDE_WIDTH + DETAIL_WIDTH + 30;
        int main_width = dim.dimx - SIDE_WIDTH - 1 - (detail ? DETAIL_WIDTH + 1 : 0);

        Elements body{sidebar(), separator() | color(T.highlight), main_pane(main_width, now) | flex | reflect(rows_box_)};
        if (detail) {
            body.push_back(separator() | color(T.highlight));
            body.push_back(detail_pane(now) | size(WIDTH, EQUAL, DETAIL_WIDTH));
        }

        Elements hints{text(" ")};
        for (auto [label, key] : {std::pair{"space done", ' '}, {"a add", 'a'}, {"n new", 'n'}, {"e edit", 'e'}, {"d delete", 'd'},
                                  {"c finished", 'c'}, {"/ search", '/'}, {"r refresh", 'r'}, {"q quit", 'q'}}) {
            buttons_.emplace_back(Box{}, Key::Character(key));
            hints.push_back(text(label) | reflect(buttons_.back().first));
            hints.push_back(text(" · "));
        }
        hints.push_back(text("tab lists"));
        Element footer = hbox(std::move(hints)) | color(T.muted) | bgcolor(T.panel);
        return vbox({bar, prompt, hbox(std::move(body)) | flex, footer}) | bgcolor(T.background);
    }

    Element sidebar() {
        Elements items;
        for (size_t k = 0; k < side_.size(); ++k) {
            const SideItem& s = side_[k];
            bool sel = static_cast<int>(k) == side_sel_;
            if (s.view == View::List && (k == 0 || side_[k - 1].view != View::List))
                items.push_back(text(" Lists") | color(T.muted));
            if (s.view == View::Agenda)
                items.push_back(text(""));
            std::string n = s.view == View::Agenda ? "" : std::to_string(count(s.view, s.list_href));
            int room = SIDE_WIDTH - 3 - static_cast<int>(n.size());
            Element row = hbox({text(" " + fit(s.label, room)), filler(), text(n + " ")});
            row = row | color(sel ? T.selected_text : (s.view == View::List ? T.link : T.text));
            if (sel)
                row = row | bold | bgcolor(side_focus_ ? T.accent : T.highlight);
            side_boxes_.emplace_back();
            items.push_back(row | reflect(side_boxes_.back()));
            if (s.view == View::All)
                items.push_back(text(""));
        }
        return vbox(std::move(items)) | size(WIDTH, EQUAL, SIDE_WIDTH) | bgcolor(T.panel) | reflect(side_box_);
    }

    Element main_pane(int width, std::time_t now) {
        Elements items;
        bool many = rem_.lists().size() > 1;
        for (size_t k = 0; k < rows_.size(); ++k) {
            const Row& r = rows_[k];
            bool sel = static_cast<int>(k) == row_sel_;
            Element row;
            if (r.kind == Row::Heading) {
                row = text(" " + r.text) | bold | color(r.text == "Overdue" ? Color(Color::RedLight) : T.accent);
                if (k > 0)
                    row = vbox({text(""), row});
            } else if (r.kind == Row::Event) {
                const Event& ev = events_[r.index];
                std::string when = ev.all_day ? "all day" : format_span(ev.start, ev.end);
                std::string right = ev.calendar;
                int room = width - 22 - string_width(right) - 2;
                row = hbox({text("   " + when) | color(T.muted) | size(WIDTH, EQUAL, 21), text(" "),
                            text(fit(ev.title, room)) | color(T.text), filler(), text(right + " ") | color(T.muted)});
            } else {
                const Task& t = tasks_[r.index];
                std::string due;
                if (t.due)
                    due = side().view == View::Agenda || side().view == View::Scheduled
                              ? (t.due->has_time ? clock(t.due->t) : "")
                              : format_due(*t.due, now);
                bool late = t.due && !t.done && is_overdue(*t.due, now);
                std::string marks;
                if (!t.repeat.empty())
                    marks += " ↻";
                if (t.priority >= 1 && t.priority <= 4)
                    marks += " !!!";
                else if (t.priority == 5)
                    marks += " !!";
                if (!t.notes.empty())
                    marks += " …";
                std::string where = many && side().view != View::List ? t.list : "";
                int right = string_width(due) + string_width(where) + 4;
                int room = width - 4 - string_width(marks) - right;
                Element circle = text(t.done ? " ● " : " ○ ") | color(t.done ? T.muted : T.accent);
                Element title = text(fit(t.title, room)) | color(t.done ? T.muted : T.text);
                if (t.done)
                    title = title | strikethrough;
                row = hbox({circle, title, text(marks) | color(T.accent), filler(),
                            text(due + " ") | color(late ? Color(Color::RedLight) : T.link),
                            text(where.empty() ? "" : where + " ") | color(T.muted)});
            }
            if (sel && r.kind != Row::Heading) {
                row = row | bgcolor(side_focus_ ? T.highlight : T.accent);
                if (!side_focus_)
                    row = row | color(T.selected_text);
                row = row | focus;
            }
            row_boxes_.emplace_back();
            items.push_back(row | reflect(row_boxes_.back()));
        }
        if (items.empty()) {
            std::string why = !filter_.empty() ? " Nothing matches the search."
                              : side().view == View::Agenda ? " Nothing in the next " + std::to_string(agenda_days_) + " days."
                                                            : " Nothing here. Press a to add a reminder.";
            items.push_back(text(why) | color(T.muted));
        }
        return vbox(std::move(items)) | yframe | vscroll_indicator;
    }

    Element field(const std::string& label, const std::string& value, Color c) {
        return hbox({text(" " + label) | color(T.muted) | size(WIDTH, EQUAL, 11), text(value) | color(c)});
    }

    Element detail_pane(std::time_t now) {
        int width = DETAIL_WIDTH - 3;
        Elements rows;
        if (const Event* ev = current_event()) {
            for (auto& l : wrap(ev->title, width))
                rows.push_back(text(" " + l) | bold | color(T.accent));
            rows.push_back(text(""));
            rows.push_back(field("When", ev->all_day ? format_due({ev->start, false}, now) + ", all day"
                                                  : format_due({ev->start, false}, now) + " " + format_span(ev->start, ev->end),
                                 T.text));
            if (!ev->location.empty())
                rows.push_back(field("Where", fit(ev->location, width - 10), T.text));
            rows.push_back(field("Calendar", ev->calendar, T.link));
        } else if (Task* t = current()) {
            for (auto& l : wrap(t->title, width))
                rows.push_back(text(" " + l) | bold | color(t->done ? T.muted : T.accent));
            rows.push_back(text(""));
            if (t->due) {
                bool late = !t->done && is_overdue(*t->due, now);
                rows.push_back(field("Due", format_due(*t->due, now) + (late ? " (overdue)" : ""),
                                     late ? Color(Color::RedLight) : T.text));
            }
            if (!t->repeat.empty())
                rows.push_back(field("Repeats", fit(describe_repeat(t->repeat), width - 10), T.text));
            if (!t->alerts.empty())
                rows.push_back(field("Alerts", fit(describe_alerts(t->alerts), width - 10), T.text));
            if (t->priority)
                rows.push_back(field("Priority", describe_priority(t->priority), T.text));
            rows.push_back(field("List", t->list, T.link));
            if (t->done && t->completed)
                rows.push_back(field("Finished", format_due({t->completed, true}, now), T.muted));
            if (!t->url.empty())
                rows.push_back(field("Link", fit(t->url, width - 10), T.link) | hyperlink(t->url));
            if (!t->notes.empty()) {
                rows.push_back(text(""));
                for (auto& l : wrap(t->notes, width))
                    rows.push_back(text(" " + l) | color(T.text));
            }
        } else {
            rows.push_back(text(" No reminder selected.") | color(T.muted));
        }
        return vbox(std::move(rows)) | yframe | bgcolor(T.background);
    }
};

}  // namespace

int run_tui(const Account& account, const Config& config) {
    T = make_theme(config.theme_preset, config.theme_colors);
    App app(account, config);
    return app.run();
}

}  // namespace jotdo
