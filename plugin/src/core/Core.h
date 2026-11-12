// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 AntiArc. This program is free software: see the LICENSE file (GNU GPL version 3 or later). It comes with NO WARRANTY.
#pragma once

#include "DripSimAPI.h"

#include <string>

// The framework core: the mode registry and orchestrator. A mode asks to start (hotkey, API, auto); the core makes sure the body is captured,
// the gravity field is built and the owned overlay texture is on a free slot, gives the mode a film layer, calls it every frame, composites
// all layers and uploads the result. Everything here runs on the game's main thread unless noted.
namespace DS::Core
{
    DripSim::IDripSim* Api();

    void OnFrame();                                            // Main::Update hook
    bool WantsKey(std::uint32_t key);                          // any thread: is this key a built-in or registered hotkey?
    void OnKey(std::uint32_t key, bool shift, bool ctrl);      // game thread, after the key sink queued it
    void StopAll(DripSim::StopReason reason);
    void WriteReport(bool notify);                              // DripSim_report.txt in the SKSE log folder (notify: a line in the console)
    void SetSystemInfo(const std::string& text);               // the 'System: ...' line, for the report
    void SetGameInfo(const std::string& text);                 // the 'Game: ...' line, for the report
    void RegisterReportKey();                                  // KeyReport in DripSim.ini (default Shift+F6): writes DripSim_report.txt
    void ReloadAll(bool notify);                               // game thread: read every INI again (F10, the tuning panel)
    bool AnyActive();                                          // a mode is running or waiting to start
}

namespace DS::World
{
    DripSim::IWorld* Get();
    void Tick(float dt);  // once per frame: refreshes the sky-exposure sample
}
