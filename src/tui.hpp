// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config.hpp"

namespace jotdo {

// The reminder browser. Returns the exit status.
int run_tui(const Account& account, const Config& config);

}  // namespace jotdo
