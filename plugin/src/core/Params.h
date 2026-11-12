// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 AntiArc. This program is free software: see the LICENSE file (GNU GPL version 3 or later). It comes with NO WARRANTY.
#pragma once

// The framework's own settings, read from Data\SKSE\Plugins\DripSim.ini (re-read on F10). Each mode has its own INI.
namespace DS::Params
{
    struct Values
    {
        bool enabled = true;  // false = modes cannot start
        float refraction = 0.0f;  // spike: screen-space refraction of the fluid (0 = off, try 0.05..0.3); needs NormalMaps
        bool perfLog = false;  // PERF lines in DripSim.log every ~5 s (game frame time, our cost); also a baseline line while no effect runs
        bool donor = false;  // draw into another overlay's texture object (old method: that overlay's own slot then shows our film too); off = our own placeholder texture file
        bool overlayDecal = false;  // spike S7: draw the overlay shapes as decals (depth bias) against z-fighting patches
        bool premultiplied = false;  // spike: the overlay blends premultiplied (made the body black in shadow in game: off)
        bool autoReport = true;  // write DripSim_report.txt by itself when an effect ends (at most every 5 minutes): players need no key for a bug report
        bool hotkeys = false;  // with false DripSim reacts to no key but F10 (no report key, no effect keys, no debug keys); the effects still start by themselves
        bool debugKeys = false;  // Ctrl+H shape dump, Ctrl+Shift+H body dump, Ctrl+Shift+G gravity view (the author's tools)
        bool specMaps = true;  // with NormalMaps: a gloss map in the overlay's specular slot (wet skin shines, dry does not)
        bool normalMaps = false;  // give the fluid a normal map (needs a model-space overlay); off until it is verified in game
        float rainDensityRef = 1500.0f;  // precipitation particle density that counts as the heaviest rain (calibrate with the log line Weather: ...)
    };

    const Values& Get();
    // Re-reads the INI. Returns a one-line summary for the console.
    std::string Load();
}
