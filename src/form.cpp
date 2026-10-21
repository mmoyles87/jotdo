// SPDX-License-Identifier: GPL-3.0-or-later
#include "form.hpp"

#include <cctype>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "when.hpp"

namespace jotdo {

namespace {

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos)
        return "";
    size_t b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

const std::vector<std::string> FIELDS = {"due", "repeat", "alert", "alerts", "priority", "list", "url"};

// "due: x" → {"due", "x"}; anything else → {"", ""}.
std::pair<std::string, std::string> field(const std::string& line) {
    auto colon = line.find(':');
    if (colon == std::string::npos)
        return {};
    std::string key = trim(line.substr(0, colon));
    for (auto& c : key)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (auto& f : FIELDS)
        if (key == f)
            return {key, trim(line.substr(colon + 1))};
    return {};
}

}  // namespace

std::string form_text(const Task& t, const std::string& list) {
    std::string out = t.title + "\n";
    out += "due: " + (t.due ? format_due_exact(*t.due) : "") + "\n";
    out += "repeat: " + describe_repeat(t.repeat) + "\n";
    out += "alert: " + describe_alerts(t.alerts) + "\n";
    out += "priority: " + describe_priority(t.priority) + "\n";
    out += "list: " + list + "\n";
    if (!t.url.empty())
        out += "url: " + t.url + "\n";
    out += "\n" + t.notes;
    if (!t.notes.empty() && !t.notes.ends_with("\n"))
        out += "\n";
    return out;
}

void apply_form(const std::string& text, Task& t, std::string& list, std::time_t now) {
    std::vector<std::string> lines;
    std::stringstream ss(text);
    std::string line;
    while (std::getline(ss, line))
        lines.push_back(line);
    size_t i = 0;
    while (i < lines.size() && trim(lines[i]).empty())
        ++i;
    if (i == lines.size())
        throw std::invalid_argument("the reminder is empty");
    std::string title = trim(lines[i++]);
    if (field(title).first.size())
        throw std::invalid_argument("the first line is the title");

    Task out = t;
    out.title = title;
    std::string new_list = list;
    for (; i < lines.size(); ++i) {
        auto [key, value] = field(lines[i]);
        if (key.empty())
            break;
        try {
            if (key == "due")
                out.due = parse_due(value, now);
            else if (key == "repeat")
                out.repeat = parse_repeat(value);
            else if (key == "alert" || key == "alerts")
                out.alerts = parse_alerts(value);
            else if (key == "priority")
                out.priority = parse_priority(value);
            else if (key == "list")
                new_list = value.empty() ? list : value;
            else if (key == "url")
                out.url = value;
        } catch (const std::invalid_argument& e) {
            throw std::invalid_argument(key + ": " + e.what());
        }
    }
    if (i < lines.size() && trim(lines[i]).empty())
        ++i;  // the blank line before the notes
    std::string notes;
    for (; i < lines.size(); ++i)
        notes += lines[i] + "\n";
    while (!notes.empty() && (notes.back() == '\n' || notes.back() == ' '))
        notes.pop_back();
    out.notes = notes;
    if (!out.repeat.empty() && !out.due)
        throw std::invalid_argument("repeat: a repeating reminder needs a due date");
    t = out;
    list = new_list;
}

}  // namespace jotdo
