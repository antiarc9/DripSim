// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 AntiArc. This program is free software: see the LICENSE file (GNU GPL version 3 or later). It comes with NO WARRANTY.
#pragma once

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"
#include "RE/S/SendHUDMessage.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
#include <vector>
#include <format>
#include <optional>
#include <random>
#include <memory>
#include <filesystem>
#include <string>
#include <string_view>

using namespace std::literals;
namespace logger = SKSE::log;
