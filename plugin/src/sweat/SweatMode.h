// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 AntiArc. This program is free software: see the LICENSE file (GNU GPL version 3 or later). It comes with NO WARRANTY.
#pragma once

#include "DripSimMode.h"

// The sweat mode's instance (registered with the framework by SweatMain.cpp) and its settings loader.
DripSim::IMode* SweatModeInstance();
void SweatLoadSettings();
// Heat from outside (the DripSim_Sweat mod event, the toggle key): 0..1, how hot the body gets at once. The next Update of a running sweat mode picks it up. Safe from any thread.
void SweatRequestHeat(float heat);
// The heat the sweat shows now, 0..1 (a running forced mode counts its forced heat). Safe from any thread. Other plugins read it through the export DripSimSweatHeat.
float SweatHeatNow();
