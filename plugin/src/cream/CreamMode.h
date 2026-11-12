// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 AntiArc. This program is free software: see the LICENSE file (GNU GPL version 3 or later). It comes with NO WARRANTY.
#pragma once

#include "DripSimMode.h"

// The cream mode's instance (registered with the framework by CreamMain.cpp) and its settings loader.
DripSim::IMode* CreamModeInstance();
void CreamLoadSettings();

// What a request does: a splash (a short spray of blobs and spatters over the front) or a pour (a thick stream from above that runs down).
enum class CreamKind : int
{
    Splash = 1,
    Pour = 2,
};

// A splash or pour asked for from outside (keys, the DripSim_Cream mod event): strength 0..1. A running cream mode takes it on its next Update,
// otherwise the next Start("cream", kind) does. Safe from any thread.
void CreamRequest(CreamKind kind, float strength);
