// SPDX-License-Identifier: GPL-3.0-or-later
// A reminder as text in the editor:
//
//   Call the dentist
//   due: 2026-10-22 09:00
//   repeat: every Monday
//   alert: 1h before
//   priority: high
//   list: Personal
//
//   Notes, after the first blank line.
#pragma once

#include <ctime>
#include <string>

#include "ical.hpp"

namespace jotdo {

std::string form_text(const Task& t, const std::string& list);

// Reads the form back into `t` and `list`. Field values are read like the command line's
// (`due: tomorrow 9am` works). Throws std::invalid_argument, naming the line, on a bad value.
void apply_form(const std::string& text, Task& t, std::string& list, std::time_t now);

}  // namespace jotdo
