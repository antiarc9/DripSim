// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 AntiArc. This program is free software: see the LICENSE file (GNU GPL version 3 or later). It comes with NO WARRANTY.
#pragma once

// Force-included into every file of the preview program (it replaces the plugins' precompiled header): the standard headers they rely on and a
// stand-in for the game's logger. The film, the field and both modes compile unchanged against this, with no game and no CommonLib.
#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <execution>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

using namespace std::literals;

namespace logger
{
    inline bool verbose = false;

    template <class... Args>
    void info(std::format_string<Args...> text, Args&&... args)
    {
        if (verbose) std::puts(std::format(text, std::forward<Args>(args)...).c_str());
    }

    template <class... Args>
    void warn(std::format_string<Args...> text, Args&&... args)
    {
        if (verbose) std::puts(("warning: " + std::format(text, std::forward<Args>(args)...)).c_str());
    }

    template <class... Args>
    void error(std::format_string<Args...> text, Args&&... args)
    {
        std::puts(("error: " + std::format(text, std::forward<Args>(args)...)).c_str());
    }
}
