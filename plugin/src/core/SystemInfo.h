// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 AntiArc. This program is free software: see the LICENSE file (GNU GPL version 3 or later). It comes with NO WARRANTY.
#pragma once

#include <string>

namespace DS
{
    // One line about the player's machine for the log and the bug report: CPU (name, cores), RAM, GPU (name, video memory), Windows version, desktop size, ENB / ReShade.
    // Nothing personal (no user name, no folder paths) and nothing leaves the PC: the player decides to share the log.
    std::string SystemSummary();
}
