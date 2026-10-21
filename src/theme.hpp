// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <map>
#include <string>
#include <vector>

#include <ftxui/screen/color.hpp>

namespace jotdo {

struct Theme {
    ftxui::Color background;     // the detail pane
    ftxui::Color panel;          // the list, status bar and footer
    ftxui::Color text;
    ftxui::Color muted;          // dates, hints, placeholders
    ftxui::Color status;         // the status bar's text
    ftxui::Color accent;         // the selected reminder, headings, circles
    ftxui::Color link;           // links, list names
    ftxui::Color highlight;      // the open search bar, the divider
    ftxui::Color search;         // the search bar
    ftxui::Color selected_text;  // text on the selected reminder
};

std::vector<std::string> theme_presets();  // forest, gruvbox, nord, terminal
std::vector<std::string> theme_keys();     // background, panel, ... selected-text

// A preset with some colours replaced. A colour is "#rrggbb", "default" (the terminal's own),
// an ANSI name ("red", "bright-black", ...) or a 256-colour index. Throws on an unknown
// preset, key or colour.
Theme make_theme(const std::string& preset, const std::map<std::string, std::string>& colors);

ftxui::Color parse_color(const std::string& value);

}  // namespace jotdo
