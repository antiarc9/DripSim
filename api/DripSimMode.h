// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 AntiArc. This program is free software: see the LICENSE file (GNU GPL version 3 or later). It comes with NO WARRANTY.
#pragma once

// Helpers for a mode plugin: get the framework API and read a flat INI. Include after your precompiled header (needs <Windows.h>).
#include "DripSimAPI.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

namespace DripSim
{
    // The framework's IDripSim (nullptr if DripSim.dll is not loaded or speaks another version). Call at kPostLoad or later.
    inline IDripSim* RequestAPI()
    {
        const HMODULE module = ::GetModuleHandleA(kFrameworkModule);
        if (!module) return nullptr;
        using Request = void* (*)(std::uint32_t);
        const auto request = reinterpret_cast<Request>(::GetProcAddress(module, "DripSimRequestAPI"));
        return request ? static_cast<IDripSim*>(request(kApiVersion)) : nullptr;
    }

    // A flat Key=Value file (';' and '#' start a comment; [sections] are ignored). Data\SKSE\Plugins\<name>.ini
    class Ini
    {
    public:
        explicit Ini(const std::string& name)
        {
            std::ifstream file(std::filesystem::path("Data\\SKSE\\Plugins") / (name + ".ini"));
            std::string line;
            while (std::getline(file, line)) {
                const auto comment = line.find_first_of(";#");
                if (comment != std::string::npos) line.erase(comment);
                const auto eq = line.find('=');
                if (eq == std::string::npos) continue;
                m_values[Trim(line.substr(0, eq))] = Trim(line.substr(eq + 1));
            }
            m_found = file.eof() || !m_values.empty();
        }

        bool Found() const { return m_found; }
        float Float(const std::string& key, float fallback) const
        {
            const auto it = m_values.find(key);
            if (it == m_values.end()) return fallback;
            try {
                return std::stof(it->second);
            } catch (const std::exception&) {
                return fallback;
            }
        }
        int Int(const std::string& key, int fallback) const { return static_cast<int>(Float(key, static_cast<float>(fallback))); }
        bool Bool(const std::string& key, bool fallback) const
        {
            const auto it = m_values.find(key);
            if (it == m_values.end()) return fallback;
            return it->second != "0" && it->second != "false" && it->second != "False";
        }
        std::string Text(const std::string& key, const std::string& fallback) const
        {
            const auto it = m_values.find(key);
            return it == m_values.end() ? fallback : it->second;
        }

    private:
        static std::string Trim(const std::string& text)
        {
            std::size_t a = 0;
            std::size_t b = text.size();
            while (a < b && std::isspace(static_cast<unsigned char>(text[a]))) ++a;
            while (b > a && std::isspace(static_cast<unsigned char>(text[b - 1]))) --b;
            return text.substr(a, b - a);
        }

        std::map<std::string, std::string> m_values;
        bool m_found = false;
    };
}
