// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 AntiArc. This program is free software: see the LICENSE file (GNU GPL version 3 or later). It comes with NO WARRANTY.
#pragma once

// The skin surfaces the framework can paint (see Body.h). No engine types here, so the film and the modes can be built without the game.
namespace DS::Surface
{
    enum Id : int
    {
        Body = 0,
        Hands = 1,
        Feet = 2,
        Face = 3,
        Count = 4,
    };

    inline const char* Name(int id)
    {
        constexpr const char* names[Count] = { "body", "hands", "feet", "face" };
        return id >= 0 && id < Count ? names[id] : "?";
    }

    inline const char* Prefix(int id)  // the overlay shapes' name prefix
    {
        constexpr const char* prefixes[Count] = { "Body", "Hands", "Feet", "Face" };
        return id >= 0 && id < Count ? prefixes[id] : "";
    }

    inline std::int32_t Mask(int id)  // DripSim::kSurface*
    {
        return 1 << id;
    }
}

// Sizes of the UV atlas the modes and the field work in, and of the film that is drawn at twice that resolution (the modes never see the difference).
namespace DS::Atlas
{
    constexpr int kSim = 1024;   // the field's raster and the coordinates modes use (pixels)
    constexpr int kFilm = 2048;  // the film's thickness map, the final image and the owned texture
    constexpr int kTile = 32;   // film tile size (kFilm / kTile tiles a side)
}
