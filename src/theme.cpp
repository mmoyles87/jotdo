// SPDX-License-Identifier: GPL-3.0-or-later
#include "theme.hpp"

#include <array>
#include <cctype>
#include <stdexcept>
#include <utility>

namespace jotdo {

namespace {

using C = ftxui::Color;

const std::array<const char*, 10> KEYS = {"background", "panel", "text", "muted", "status",
                                          "accent", "link", "highlight", "search", "selected-text"};

// In KEYS order.
const std::map<std::string, std::array<const char*, 10>> PRESETS = {
    {"forest", {"#232e1b", "#1c2415", "#ebdbb2", "#7c6f64", "#a89984", "#b8bb26", "#8ec07c", "#425433", "#2d3a22", "#1c2415"}},
    {"gruvbox", {"#282828", "#1d2021", "#ebdbb2", "#928374", "#a89984", "#fabd2f", "#83a598", "#504945", "#3c3836", "#1d2021"}},
    {"nord", {"#2e3440", "#272c36", "#d8dee9", "#616e88", "#81a1c1", "#88c0d0", "#a3be8c", "#434c5e", "#3b4252", "#2e3440"}},
    {"terminal", {"default", "default", "default", "bright-black", "bright-black", "green", "cyan", "bright-black", "default", "black"}},
};

const std::map<std::string, C::Palette16> ANSI = {
    {"black", C::Black},          {"red", C::Red},
    {"green", C::Green},          {"yellow", C::Yellow},
    {"blue", C::Blue},            {"magenta", C::Magenta},
    {"cyan", C::Cyan},            {"white", C::GrayLight},
    {"bright-black", C::GrayDark}, {"bright-red", C::RedLight},
    {"bright-green", C::GreenLight}, {"bright-yellow", C::YellowLight},
    {"bright-blue", C::BlueLight}, {"bright-magenta", C::MagentaLight},
    {"bright-cyan", C::CyanLight}, {"bright-white", C::White},
};

int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

}  // namespace

std::vector<std::string> theme_presets() {
    std::vector<std::string> out;
    for (auto& [name, _] : PRESETS)
        out.push_back(name);
    return out;
}

std::vector<std::string> theme_keys() {
    return {KEYS.begin(), KEYS.end()};
}

C parse_color(const std::string& v) {
    if (v == "default")
        return C::Default;
    if (v.size() == 7 && v[0] == '#') {
        int d[6];
        for (int i = 0; i < 6; ++i)
            if ((d[i] = hex(v[i + 1])) < 0)
                throw std::runtime_error("bad colour " + v);
        return C::RGB(static_cast<uint8_t>(d[0] * 16 + d[1]), static_cast<uint8_t>(d[2] * 16 + d[3]),
                      static_cast<uint8_t>(d[4] * 16 + d[5]));
    }
    if (auto it = ANSI.find(v); it != ANSI.end())
        return it->second;
    if (!v.empty() && v.size() <= 3 && v.find_first_not_of("0123456789") == std::string::npos && std::stoi(v) < 256)
        return C(static_cast<C::Palette256>(std::stoi(v)));
    throw std::runtime_error("bad colour \"" + v + "\": use #rrggbb, default, an ANSI name such as bright-black, or 0-255");
}

Theme make_theme(const std::string& preset, const std::map<std::string, std::string>& colors) {
    auto p = PRESETS.find(preset);
    if (p == PRESETS.end()) {
        std::string names;
        for (auto& n : theme_presets())
            names += (names.empty() ? "" : ", ") + n;
        throw std::runtime_error("no theme preset \"" + preset + "\" (there are " + names + ")");
    }
    std::array<C, 10> c;
    for (size_t i = 0; i < KEYS.size(); ++i)
        c[i] = parse_color(p->second[i]);
    for (auto& [key, value] : colors) {
        size_t i = 0;
        while (i < KEYS.size() && key != KEYS[i])
            ++i;
        if (i == KEYS.size())
            throw std::runtime_error("no theme colour \"" + key + "\"");
        try {
            c[i] = parse_color(value);
        } catch (const std::exception& e) {
            throw std::runtime_error("theme." + key + ": " + e.what());
        }
    }
    return {c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9]};
}

}  // namespace jotdo
