// SPDX-License-Identifier: GPL-3.0-or-later
#include "config.hpp"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>

#include <sys/wait.h>

#include <toml++/toml.hpp>

namespace jotdo {

std::filesystem::path Config::default_path() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg) / "jotdo/config.toml";
    const char* home = std::getenv("HOME");
    if (!home)
        throw std::runtime_error("HOME is not set");
    return std::filesystem::path(home) / ".config/jotdo/config.toml";
}

namespace {

std::vector<std::string> strings(const toml::table& a, const char* field, const std::string& where) {
    std::vector<std::string> out;
    auto* arr = a[field].as_array();
    if (!arr)
        return out;
    for (auto& v : *arr) {
        auto s = v.value<std::string>();
        if (!s)
            throw std::runtime_error(where + "." + field + " must be a list of strings");
        out.push_back(*s);
    }
    return out;
}

}  // namespace

Config Config::load(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path))
        throw std::runtime_error("no config at " + path.string() + " (see README for the format)");

    toml::table t;
    try {
        t = toml::parse_file(path.string());
    } catch (const toml::parse_error& e) {
        throw std::runtime_error(path.string() + ": " + std::string(e.description()));
    }

    Config c;
    c.default_account = t["default"].value_or(std::string{});
    c.mouse = t["mouse"].value_or(true);
    c.agenda_days = t["agenda_days"].value_or(7);
    if (c.agenda_days < 1 || c.agenda_days > 366)
        throw std::runtime_error(path.string() + ": agenda_days must be between 1 and 366");
    if (auto* theme = t["theme"].as_table()) {
        for (auto& [key, node] : *theme) {
            auto v = node.value<std::string>();
            if (!v)
                throw std::runtime_error(path.string() + ": theme." + std::string(key.str()) + " must be a string");
            if (key.str() == "preset")
                c.theme_preset = *v;
            else
                c.theme_colors[std::string(key.str())] = *v;
        }
    }
    auto* accounts = t["accounts"].as_table();
    if (!accounts || accounts->empty())
        throw std::runtime_error(path.string() + ": no [accounts.<name>] sections");

    for (auto& [key, node] : *accounts) {
        auto* a = node.as_table();
        if (!a)
            continue;
        Account acc;
        acc.name = std::string(key.str());
        std::string where = path.string() + ": accounts." + acc.name;
        auto need = [&](const char* field) {
            auto v = (*a)[field].value<std::string>();
            if (!v)
                throw std::runtime_error(where + " is missing " + field);
            return *v;
        };
        acc.url = need("url");
        while (acc.url.ends_with("/"))
            acc.url.pop_back();
        if (!acc.url.starts_with("https://") && !acc.url.starts_with("http://"))
            acc.url = "https://" + acc.url;
        acc.user = need("user");
        acc.password_command = need("password_command");
        acc.ca_file = (*a)["ca_file"].value_or(std::string{});
        if (acc.ca_file.starts_with("~/"))
            if (const char* home = std::getenv("HOME"))
                acc.ca_file = home + acc.ca_file.substr(1);
        acc.lists = strings(*a, "lists", where);
        acc.calendars = strings(*a, "calendars", where);
        acc.default_list = (*a)["default_list"].value_or(std::string{});
        acc.default_calendar = (*a)["default_calendar"].value_or(std::string{});
        c.accounts.emplace(acc.name, std::move(acc));
    }
    if (c.default_account.empty())
        c.default_account = c.accounts.begin()->first;
    return c;
}

const Account& Config::account(const std::string& name) const {
    const std::string& want = name.empty() ? default_account : name;
    auto it = accounts.find(want);
    if (it == accounts.end())
        throw std::runtime_error("no account named \"" + want + "\" in the config");
    return it->second;
}

std::string run_password_command(const std::string& command) {
    FILE* p = popen(command.c_str(), "r");
    if (!p)
        throw std::runtime_error("could not run password_command");
    std::string out;
    char buf[256];
    while (size_t n = fread(buf, 1, sizeof buf, p))
        out.append(buf, n);
    int status = pclose(p);
    if (status != 0)
        throw std::runtime_error("password_command failed (exit status " + std::to_string(WEXITSTATUS(status)) + ")");
    if (auto nl = out.find('\n'); nl != std::string::npos)
        out.resize(nl);
    if (out.empty())
        throw std::runtime_error("password_command printed nothing");
    return out;
}

}  // namespace jotdo
