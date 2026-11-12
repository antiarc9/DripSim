// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 AntiArc. This program is free software: see the LICENSE file (GNU GPL version 3 or later). It comes with NO WARRANTY.
#pragma once

#include <chrono>
#include <map>
#include <mutex>
#include <string>

// Keeps the log small. A line that is written at every start of an effect (capture, textures, mode started / stopped) is written the first few times,
// then at most once a minute, with a note of how many were left out. A user's log was 1.6 MB for 15 minutes because something made the effect restart
// every 1.5 s and every start wrote ~60 lines. Usage:  if (DS::LogAllow("capture")) logger::info(...);
namespace DS
{
    inline bool LogAllow(const std::string& key, int first = 3, int everySeconds = 60)
    {
        struct Entry
        {
            int count = 0;
            int suppressed = 0;
            std::chrono::steady_clock::time_point last{};
        };
        static std::mutex mutex;
        static std::map<std::string, Entry> entries;
        std::scoped_lock lock(mutex);
        auto& e = entries[key];
        const auto now = std::chrono::steady_clock::now();
        ++e.count;
        if (e.count <= first || now - e.last >= std::chrono::seconds(everySeconds)) {
            if (e.count > first) e.last = now;
            e.suppressed = 0;
            return true;
        }
        ++e.suppressed;
        return false;
    }
}
