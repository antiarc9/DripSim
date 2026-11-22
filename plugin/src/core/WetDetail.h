// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 AntiArc. This program is free software: see the LICENSE file (GNU GPL version 3 or later). It comes with NO WARRANTY.
#pragma once

// The baked wet-skin detail of the body (scripts/bake_wet_detail.py, 2026-11-18): fine water specks, small beads and short teardrops in the
// CBBE 3BA body UV, at the film's resolution. Water films (not cream) on the body multiply their gloss map by it and add its height to their
// normal map where the skin is wet, so wet skin shows small drops instead of one even shine. No engine types here.
namespace DS::WetDetail
{
    // Reads Data\SKSE\Plugins\DripSim_WetDetail.bin the first time (later calls do nothing). Missing or wrong file: logged, no detail.
    void Load();
    bool Ready();
    // The gloss of plain wet skin is 1; a speck or a bead is up to about 1.4, the gaps a little below 1. x, y: film pixels.
    float Gloss(int x, int y);
    // The slope of the water's height, in mm per film pixel (central differences).
    void Slope(int x, int y, float& sx, float& sy);
}
