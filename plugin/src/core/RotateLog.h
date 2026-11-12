// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 AntiArc. This program is free software: see the LICENSE file (GNU GPL version 3 or later). It comes with NO WARRANTY.
#pragma once

// Include after the precompiled header (needs SKSE).
namespace DS
{
    // Keeps the log of the last game session: SKSE starts every plugin's log empty, so after a crash the next start would have wiped the log of the crashed session.
    // Call it first in SKSEPluginLoad, before SKSE::Init. <name>.log is copied to <name>.prev.log.
    inline void RotateLog(const char* name)
    {
        const auto directory = SKSE::log::log_directory();
        if (!directory) return;
        std::error_code ec;
        const auto current = *directory / (std::string(name) + ".log");
        if (std::filesystem::exists(current, ec)) std::filesystem::copy_file(current, *directory / (std::string(name) + ".prev.log"), std::filesystem::copy_options::overwrite_existing, ec);
    }
}
