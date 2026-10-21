// SPDX-License-Identifier: GPL-3.0-or-later
#include "when.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <map>
#include <sstream>
#include <stdexcept>

namespace jotdo {

namespace {

const std::array<const char*, 7> DAY_CODES = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};
const std::array<const char*, 7> DAY_NAMES = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
const std::array<const char*, 12> MONTHS = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};

std::string lower(std::string s) {
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
        return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Words, with commas as separators too.
std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> out;
    std::string w;
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c)) || c == ',') {
            if (!w.empty())
                out.push_back(w), w.clear();
        } else {
            w += c;
        }
    }
    if (!w.empty())
        out.push_back(w);
    return out;
}

bool all_digits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
}

int weekday(const std::string& w) {  // 0 = Sunday, -1 if not a day name
    static const std::map<std::string, int> days = {
        {"sun", 0}, {"sunday", 0}, {"mon", 1}, {"monday", 1}, {"tue", 2}, {"tues", 2}, {"tuesday", 2},
        {"wed", 3}, {"weds", 3}, {"wednesday", 3}, {"thu", 4}, {"thur", 4}, {"thurs", 4}, {"thursday", 4},
        {"fri", 5}, {"friday", 5}, {"sat", 6}, {"saturday", 6}};
    auto it = days.find(w);
    return it == days.end() ? -1 : it->second;
}

int month(const std::string& w) {  // 0-11, -1 if not a month name
    if (w.size() < 3)
        return -1;
    for (size_t i = 0; i < MONTHS.size(); ++i)
        if (w.compare(0, 3, MONTHS[i]) == 0) {
            static const std::array<const char*, 12> full = {"january", "february", "march", "april", "may", "june",
                                                             "july", "august", "september", "october", "november", "december"};
            std::string f = full[i];
            if (w == f.substr(0, w.size()) || w == "sept")
                return static_cast<int>(i);
        }
    return -1;
}

std::tm local(std::time_t t) {
    std::tm tm{};
    localtime_r(&t, &tm);
    return tm;
}

std::time_t make(int year, int mon, int mday, int hour = 0, int min = 0) {
    std::tm tm{};
    tm.tm_year = year - 1900, tm.tm_mon = mon, tm.tm_mday = mday, tm.tm_hour = hour, tm.tm_min = min;
    tm.tm_isdst = -1;
    return std::mktime(&tm);
}

// The date, or throws if it does not exist (Feb 31 would otherwise roll into March).
std::time_t make_date(int year, int mon, int mday, const std::string& input) {
    std::time_t t = make(year, mon, mday);
    std::tm tm{};
    localtime_r(&t, &tm);
    if (tm.tm_mon != mon || tm.tm_mday != mday)
        throw std::invalid_argument("there is no such date in \"" + input + "\"");
    return t;
}

// Digits as a number no larger than `limit`.
int small_number(const std::string& digits, int limit, const std::string& input) {
    if (digits.empty() || digits.size() > 9 || std::stol(digits) > limit)
        throw std::invalid_argument("the number in \"" + input + "\" is too large");
    return static_cast<int>(std::stol(digits));
}

std::time_t day_after(std::time_t day, int n) {
    std::tm tm = local(day);
    return make(tm.tm_year + 1900, tm.tm_mon, tm.tm_mday + n);
}

// "9", "9am", "9:30", "9:30pm", "21:00", "0930"; minutes after midnight, or -1.
int parse_time(const std::string& w, const std::string& next, bool* used_next) {
    *used_next = false;
    static const std::map<std::string, int> named = {{"noon", 12 * 60}, {"midday", 12 * 60}, {"midnight", 0},
                                                     {"morning", 9 * 60}, {"afternoon", 13 * 60}, {"evening", 18 * 60},
                                                     {"night", 20 * 60}};
    if (auto it = named.find(w); it != named.end())
        return it->second;
    std::string s = w, suffix;
    if (s.ends_with("am") || s.ends_with("pm"))
        suffix = s.substr(s.size() - 2), s.resize(s.size() - 2);
    else if (s.ends_with("a") || s.ends_with("p"))
        suffix = s.substr(s.size() - 1) + "m", s.resize(s.size() - 1);
    else if (next == "am" || next == "pm")
        suffix = next, *used_next = true;
    int h = -1, m = 0;
    if (auto colon = s.find_first_of(":."); colon != std::string::npos) {
        std::string hs = s.substr(0, colon), ms = s.substr(colon + 1);
        if (!all_digits(hs) || !all_digits(ms) || ms.size() != 2)
            return -1;
        h = std::stoi(hs), m = std::stoi(ms);
    } else if (all_digits(s) && s.size() <= 2) {
        h = std::stoi(s);
    } else if (all_digits(s) && s.size() == 4 && suffix.empty()) {
        h = std::stoi(s.substr(0, 2)), m = std::stoi(s.substr(2));
    } else {
        return -1;
    }
    if (!suffix.empty()) {
        if (h < 1 || h > 12)
            return -1;
        h = h % 12 + (suffix == "pm" ? 12 : 0);
    }
    if (h > 23 || m > 59)
        return -1;
    return h * 60 + m;
}

[[noreturn]] void cannot(const std::string& text) {
    throw std::invalid_argument("could not read \"" + text + "\" as a date (try tomorrow 9am, fri, oct 24, 2026-10-24 18:00, in 2h)");
}

}  // namespace

std::time_t add_days(std::time_t t, int days) {
    std::tm tm = local(t);
    tm.tm_mday += days, tm.tm_isdst = -1;
    return std::mktime(&tm);
}

std::time_t local_midnight(std::time_t t) {
    std::tm tm = local(t);
    return make(tm.tm_year + 1900, tm.tm_mon, tm.tm_mday);
}

std::optional<Due> parse_due(const std::string& input, std::time_t now) {
    std::string text = lower(trim(input));
    if (text.empty() || text == "none" || text == "-" || text == "no")
        return std::nullopt;
    auto w = words(text);
    if (w.empty())
        cannot(input);
    std::tm today = local(now);
    std::time_t midnight = local_midnight(now);

    // in N units
    if (w[0] == "in" && w.size() >= 2) {
        std::string num = w[1], unit = w.size() >= 3 ? w[2] : "";
        size_t d = 0;
        while (d < num.size() && std::isdigit(static_cast<unsigned char>(num[d])))
            ++d;
        if (d == 0)
            cannot(input);
        if (d < num.size())
            unit = num.substr(d), num.resize(d);
        else if (w.size() != 3)
            cannot(input);
        int n = small_number(num, 100000, input);
        if (unit == "m" || unit == "min" || unit == "mins" || unit == "minute" || unit == "minutes")
            return Due{now + n * 60, true};
        if (unit == "h" || unit == "hr" || unit == "hrs" || unit == "hour" || unit == "hours")
            return Due{now + n * 3600, true};
        if (unit == "d" || unit == "day" || unit == "days")
            return Due{day_after(midnight, n), false};
        if (unit == "w" || unit == "wk" || unit == "week" || unit == "weeks")
            return Due{day_after(midnight, 7 * n), false};
        cannot(input);
    }

    std::optional<std::time_t> day;
    int minutes = -1;
    size_t i = 0;
    auto take_day = [&](std::time_t d) {
        if (day)
            cannot(input);
        day = d;
    };
    while (i < w.size()) {
        const std::string& t = w[i];
        std::string next = i + 1 < w.size() ? w[i + 1] : "";
        bool used_next = false;
        if (t == "at" || t == "on" || t == "by") {
            ++i;
        } else if (t == "today" || t == "tod") {
            take_day(midnight), ++i;
        } else if (t == "tonight") {
            take_day(midnight), ++i;
            if (minutes < 0)
                minutes = 20 * 60;
        } else if (t == "tomorrow" || t == "tmr" || t == "tmrw" || t == "tom") {
            take_day(day_after(midnight, 1)), ++i;
        } else if (t == "next" && weekday(next) >= 0) {
            int wd = weekday(next);
            int ahead = (wd - today.tm_wday + 7) % 7;
            take_day(day_after(midnight, ahead == 0 ? 7 : ahead)), i += 2;
        } else if (weekday(t) >= 0) {
            int ahead = (weekday(t) - today.tm_wday + 7) % 7;
            take_day(day_after(midnight, ahead == 0 ? 7 : ahead)), ++i;
        } else if (t.size() == 10 && t[4] == '-' && t[7] == '-' && all_digits(t.substr(0, 4)) && all_digits(t.substr(5, 2)) &&
                   all_digits(t.substr(8, 2))) {
            int y = std::stoi(t.substr(0, 4)), mo = std::stoi(t.substr(5, 2)), d = std::stoi(t.substr(8, 2));
            if (mo < 1 || mo > 12 || d < 1 || d > 31)
                cannot(input);
            take_day(make_date(y, mo - 1, d, input)), ++i;
        } else if (auto slash = t.find('/'); slash != std::string::npos && t.find(':') == std::string::npos) {
            std::string a = t.substr(0, slash), rest = t.substr(slash + 1), b = rest, c;
            if (auto s2 = rest.find('/'); s2 != std::string::npos)
                b = rest.substr(0, s2), c = rest.substr(s2 + 1);
            if (!all_digits(a) || !all_digits(b) || (!c.empty() && !all_digits(c)))
                cannot(input);
            int mo = std::stoi(a), d = std::stoi(b);
            if (mo < 1 || mo > 12 || d < 1 || d > 31)
                cannot(input);
            int y = c.empty() ? today.tm_year + 1900 : std::stoi(c) + (c.size() == 2 ? 2000 : 0);
            std::time_t when = make_date(y, mo - 1, d, input);
            if (c.empty() && when < midnight)
                when = make_date(y + 1, mo - 1, d, input);
            take_day(when), ++i;
        } else if (month(t) >= 0 && all_digits(next)) {  // oct 24 [2027]
            int mo = month(t), d = std::stoi(next);
            i += 2;
            int y = today.tm_year + 1900;
            bool year_given = i < w.size() && all_digits(w[i]) && w[i].size() == 4;
            if (year_given)
                y = std::stoi(w[i]), ++i;
            std::time_t when = make_date(y, mo, d, input);
            if (!year_given && when < midnight)
                when = make_date(y + 1, mo, d, input);
            take_day(when);
        } else if (all_digits(t) && month(next) >= 0 && t.size() <= 2) {  // 24 oct
            int d = std::stoi(t), mo = month(next);
            std::time_t when = make_date(today.tm_year + 1900, mo, d, input);
            if (when < midnight)
                when = make_date(today.tm_year + 1901, mo, d, input);
            take_day(when), i += 2;
        } else if (int m = parse_time(t, next, &used_next); m >= 0 && minutes < 0) {
            minutes = m;
            i += used_next ? 2 : 1;
        } else {
            cannot(input);
        }
    }
    if (!day && minutes < 0)
        cannot(input);
    if (!day) {  // a time alone: the next time it comes round
        std::time_t at = midnight + 0;
        std::tm tm = local(midnight);
        at = make(tm.tm_year + 1900, tm.tm_mon, tm.tm_mday, minutes / 60, minutes % 60);
        if (at <= now)
            at = make(tm.tm_year + 1900, tm.tm_mon, tm.tm_mday + 1, minutes / 60, minutes % 60);
        return Due{at, true};
    }
    if (minutes < 0)
        return Due{*day, false};
    std::tm tm = local(*day);
    return Due{make(tm.tm_year + 1900, tm.tm_mon, tm.tm_mday, minutes / 60, minutes % 60), true};
}

namespace {
bool twelve_hour_clock = true;
}

void set_clock(bool twelve_hour) {
    twelve_hour_clock = twelve_hour;
}

std::string format_time(std::time_t t) {
    std::tm tm = local(t);
    char buf[16];
    if (!twelve_hour_clock) {
        std::snprintf(buf, sizeof buf, "%02d:%02d", tm.tm_hour, tm.tm_min);
        return buf;
    }
    int h = tm.tm_hour % 12 == 0 ? 12 : tm.tm_hour % 12;
    std::snprintf(buf, sizeof buf, "%d:%02d %s", h, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
    return buf;
}

std::string format_span(std::time_t start, std::time_t end) {
    std::string a = format_time(start), b = format_time(end);
    if (twelve_hour_clock && a.size() > 3 && b.size() > 3 && a.substr(a.size() - 2) == b.substr(b.size() - 2) &&
        local_midnight(start) == local_midnight(end))
        a.resize(a.size() - 3);  // 9:00–10:30 AM
    return a + "–" + b;
}

std::string format_due(const Due& d, std::time_t now) {
    std::tm tm = local(d.t), n = local(now);
    std::time_t day = local_midnight(d.t), today = local_midnight(now);
    long days = std::lround(std::difftime(day, today) / 86400.0);
    char buf[48];
    std::string date;
    if (days == 0)
        date = "Today";
    else if (days == 1)
        date = "Tomorrow";
    else if (days == -1)
        date = "Yesterday";
    else if (days > 1 && days < 7) {
        std::strftime(buf, sizeof buf, "%a", &tm);
        date = buf;
    } else {
        std::strftime(buf, sizeof buf, "%b", &tm);
        date = std::string(buf) + " " + std::to_string(tm.tm_mday);  // Nov 2, not Nov 02
        if (tm.tm_year != n.tm_year)
            date += " " + std::to_string(tm.tm_year + 1900);
    }
    if (!d.has_time)
        return date;
    return date + " " + format_time(d.t);
}

std::string format_due_exact(const Due& d) {
    std::tm tm = local(d.t);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
    return d.has_time ? std::string(buf) + " " + format_time(d.t) : std::string(buf);
}

bool is_overdue(const Due& d, std::time_t now) {
    return d.has_time ? d.t < now : d.t < local_midnight(now);
}

bool is_today(const Due& d, std::time_t now) {
    return local_midnight(d.t) == local_midnight(now);
}

std::string parse_repeat(const std::string& input) {
    std::string text = lower(trim(input));
    if (text.empty() || text == "none" || text == "never" || text == "-" || text == "no")
        return "";
    std::string raw = trim(input);
    if (lower(raw).starts_with("freq=")) {
        std::string up;
        for (char c : raw)
            up += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return up;
    }
    auto w = words(text);
    if (!w.empty() && w[0] == "repeat")
        w.erase(w.begin());
    std::vector<std::string> kept;
    for (auto& x : w)
        if (x != "and" && x != "on")
            kept.push_back(x);
    w = kept;
    if (w.empty())
        throw std::invalid_argument("could not read the repeat \"" + input + "\"");

    auto days_rule = [&](size_t from) {
        std::string by;
        for (size_t k = from; k < w.size(); ++k) {
            int d = weekday(w[k]);
            if (d < 0)
                throw std::invalid_argument("could not read the repeat \"" + input + "\" (" + w[k] + " is not a day)");
            by += (by.empty() ? "" : ",") + std::string(DAY_CODES[d]);
        }
        return "FREQ=WEEKLY;BYDAY=" + by;
    };

    const std::string& a = w[0];
    if (w.size() == 1) {
        if (a == "daily" || a == "everyday")
            return "FREQ=DAILY";
        if (a == "weekdays")
            return "FREQ=WEEKLY;BYDAY=MO,TU,WE,TH,FR";
        if (a == "weekends")
            return "FREQ=WEEKLY;BYDAY=SA,SU";
        if (a == "weekly")
            return "FREQ=WEEKLY";
        if (a == "biweekly" || a == "fortnightly")
            return "FREQ=WEEKLY;INTERVAL=2";
        if (a == "monthly")
            return "FREQ=MONTHLY";
        if (a == "yearly" || a == "annually")
            return "FREQ=YEARLY";
    }
    if (a == "weekly" && w.size() >= 2)
        return days_rule(1);
    if (a == "every" && w.size() >= 2) {
        static const std::map<std::string, std::string> unit = {
            {"day", "DAILY"}, {"days", "DAILY"}, {"week", "WEEKLY"}, {"weeks", "WEEKLY"},
            {"month", "MONTHLY"}, {"months", "MONTHLY"}, {"year", "YEARLY"}, {"years", "YEARLY"}};
        if (w.size() == 2 && unit.count(w[1]))
            return "FREQ=" + unit.at(w[1]);
        if (w.size() == 2 && w[1] == "weekday")
            return "FREQ=WEEKLY;BYDAY=MO,TU,WE,TH,FR";
        if (w.size() == 3 && all_digits(w[1]) && unit.count(w[2])) {
            int n = std::stoi(w[1]);
            if (n < 1)
                throw std::invalid_argument("a repeat needs an interval of at least 1");
            return "FREQ=" + unit.at(w[2]) + (n > 1 ? ";INTERVAL=" + std::to_string(n) : "");
        }
        if (weekday(w[1]) >= 0)
            return days_rule(1);
    }
    throw std::invalid_argument("could not read the repeat \"" + input +
                                "\" (try daily, weekdays, weekly, every 2 weeks, every mon, monthly, yearly)");
}

std::string describe_repeat(const std::string& rrule) {
    if (rrule.empty())
        return "";
    std::map<std::string, std::string> parts;
    std::stringstream ss(rrule);
    std::string part;
    while (std::getline(ss, part, ';'))
        if (auto eq = part.find('='); eq != std::string::npos)
            parts[part.substr(0, eq)] = part.substr(eq + 1);
    for (auto& [k, _] : parts)
        if (k != "FREQ" && k != "INTERVAL" && k != "BYDAY" && k != "WKST")
            return rrule;  // COUNT, UNTIL, BYMONTHDAY, BYSETPOS...: shown as written
    std::string freq = parts["FREQ"];
    int n = parts.count("INTERVAL") ? std::stoi(parts["INTERVAL"]) : 1;
    std::string byday = parts["BYDAY"];
    static const std::map<std::string, std::pair<std::string, std::string>> names = {
        {"DAILY", {"daily", "days"}}, {"WEEKLY", {"weekly", "weeks"}}, {"MONTHLY", {"monthly", "months"}}, {"YEARLY", {"yearly", "years"}}};
    auto it = names.find(freq);
    if (it == names.end())
        return rrule;
    if (byday.empty())
        return n == 1 ? it->second.first : "every " + std::to_string(n) + " " + it->second.second;
    if (freq != "WEEKLY" || n != 1)
        return rrule;
    if (byday == "MO,TU,WE,TH,FR")
        return "weekdays";
    if (byday == "SA,SU" || byday == "SU,SA")
        return "weekends";
    std::vector<std::string> days;
    std::stringstream ds(byday);
    while (std::getline(ds, part, ',')) {
        auto code = std::find_if(DAY_CODES.begin(), DAY_CODES.end(), [&](const char* c) { return part == c; });
        if (code == DAY_CODES.end())
            return rrule;  // 1MO and the like
        days.push_back(DAY_NAMES[code - DAY_CODES.begin()]);
    }
    if (days.size() == 1)
        return "every " + days[0];
    std::string out = "every ";
    for (size_t k = 0; k < days.size(); ++k)
        out += (k ? ", " : "") + days[k].substr(0, 3);
    return out;
}

std::vector<int> parse_alerts(const std::string& input) {
    std::string text = lower(trim(input));
    std::vector<int> out;
    if (text.empty() || text == "none" || text == "-" || text == "no")
        return out;
    std::string cleaned;
    for (size_t k = 0; k < text.size(); ++k)
        cleaned += text[k] == ',' ? ';' : text[k];
    std::stringstream ss(cleaned);
    std::string item;
    auto fail = [&] {
        throw std::invalid_argument("could not read the alert \"" + input + "\" (try at due, 15m before, 1h before, 1d before)");
    };
    while (std::getline(ss, item, ';')) {
        auto w = words(item);
        std::vector<std::string> kept;
        for (auto& x : w)
            if (x != "and")
                kept.push_back(x);
        w = kept;
        if (w.empty())
            continue;
        std::string joined;
        for (auto& x : w)
            joined += (joined.empty() ? "" : " ") + x;
        if (joined == "at due" || joined == "at time" || joined == "on time" || joined == "due" || joined == "0") {
            out.push_back(0);
            continue;
        }
        if (w.back() != "before")
            fail();
        w.pop_back();
        int total = 0;
        auto add = [&](int n, const std::string& unit) {
            if (unit == "m" || unit == "min" || unit == "mins" || unit == "minute" || unit == "minutes")
                total += n;
            else if (unit == "h" || unit == "hr" || unit == "hrs" || unit == "hour" || unit == "hours")
                total += n * 60;
            else if (unit == "d" || unit == "day" || unit == "days")
                total += n * 1440;
            else if (unit == "w" || unit == "week" || unit == "weeks")
                total += n * 10080;
            else
                fail();
        };
        // "1h30m", "1h 30m", "90 minutes", "1 day"
        for (size_t k = 0; k < w.size(); ++k) {
            const std::string& word = w[k];
            size_t p = 0;
            if (word.empty() || !std::isdigit(static_cast<unsigned char>(word[0])))
                fail();
            while (p < word.size()) {
                size_t d = p;
                while (d < word.size() && std::isdigit(static_cast<unsigned char>(word[d])))
                    ++d;
                if (d == p)
                    fail();
                int n = small_number(word.substr(p, d - p), 1000000, input);
                size_t u = d;
                while (u < word.size() && std::isalpha(static_cast<unsigned char>(word[u])))
                    ++u;
                std::string unit = word.substr(d, u - d);
                if (unit.empty()) {
                    if (u != word.size() || k + 1 >= w.size())
                        fail();
                    unit = w[++k];
                }
                add(n, unit);
                p = u;
            }
        }
        out.push_back(total);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::string describe_alerts(const std::vector<int>& minutes) {
    std::string out;
    for (int m : minutes) {
        if (m < 0)
            continue;  // an alarm after the due time: kept on the server, not shown
        std::string one;
        if (m == 0) {
            one = "at due";
        } else {
            int mm = m;
            if (mm >= 10080 && mm % 10080 == 0)
                one = std::to_string(mm / 10080) + "w";
            else {
                if (mm >= 1440)
                    one += std::to_string(mm / 1440) + "d", mm %= 1440;
                if (mm >= 60)
                    one += std::to_string(mm / 60) + "h", mm %= 60;
                if (mm > 0)
                    one += std::to_string(mm) + "m";
            }
            one += " before";
        }
        out += (out.empty() ? "" : ", ") + one;
    }
    return out;
}

int parse_duration(const std::string& input) {
    std::vector<int> m;
    try {
        m = parse_alerts(trim(input) + " before");
    } catch (const std::invalid_argument&) {
        m.clear();
    }
    if (m.size() != 1 || m[0] <= 0)
        throw std::invalid_argument("could not read the length \"" + input + "\" (try 1h, 90m, 1h30m, 2 hours, 1 day)");
    return m[0];
}

int parse_priority(const std::string& input) {
    std::string t = lower(trim(input));
    if (t.empty() || t == "none" || t == "-" || t == "0" || t == "no")
        return 0;
    if (t == "high" || t == "!!!" || t == "h")
        return 1;
    if (t == "medium" || t == "med" || t == "!!" || t == "m")
        return 5;
    if (t == "low" || t == "!" || t == "l")
        return 9;
    if (all_digits(t) && t.size() == 1)
        return std::stoi(t);
    throw std::invalid_argument("could not read the priority \"" + input + "\" (high, medium, low or none)");
}

std::string describe_priority(int p) {
    if (p >= 1 && p <= 4)
        return "high";
    if (p == 5)
        return "medium";
    if (p >= 6 && p <= 9)
        return "low";
    return "";
}

}  // namespace jotdo
