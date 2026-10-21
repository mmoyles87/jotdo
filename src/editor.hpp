// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace jotdo {

// Opens `text` in $VISUAL, $EDITOR or vi (run through /bin/sh, so "code -w" works) and
// returns the edited text, or nothing if it ends up the same as `original` (default: `text`).
std::optional<std::string> edit_text(const std::string& text, const std::optional<std::string>& original = std::nullopt);

// Keeps text that could not be saved in $XDG_STATE_HOME/jotdo/ and returns the file's path.
std::filesystem::path keep_unsaved(const std::string& text);

}  // namespace jotdo
