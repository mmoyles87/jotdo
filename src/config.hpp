// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace jotdo {

struct Account {
    std::string name;
    std::string url;               // the CalDAV server, e.g. https://cloud.example.com or its DAV root
    std::string user;
    std::string password_command;  // run with /bin/sh; the first line of its output is the password
    std::string ca_file;           // extra trusted CA certificates (PEM), for a server with a private CA
    std::vector<std::string> lists;      // task lists to show, by name; empty: every list that holds tasks
    std::vector<std::string> calendars;  // calendars for the agenda, by name; empty: every one that holds events
    std::string default_list;            // where new reminders go; empty: the first list
    std::string default_calendar;        // where new events go; empty: Personal, or the first writable one
};

struct Config {
    std::string default_account;
    bool mouse = true;     // clicks and the wheel; false keeps the terminal's own selection
    int agenda_days = 7;   // how far ahead the agenda looks
    bool twelve_hour = true;  // clock = "12h" (9:00 AM) or "24h" (09:00)
    std::string theme_preset = "forest";
    std::map<std::string, std::string> theme_colors;  // [theme] keys other than preset
    std::map<std::string, Account> accounts;

    // $XDG_CONFIG_HOME/jotdo/config.toml, or ~/.config/jotdo/config.toml
    static std::filesystem::path default_path();
    static Config load(const std::filesystem::path& path);

    // The named account, or the default one when name is empty.
    const Account& account(const std::string& name) const;
};

std::string run_password_command(const std::string& command);

}  // namespace jotdo
