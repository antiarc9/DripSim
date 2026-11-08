// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 AntiArc. This program is free software: see the LICENSE file (GNU GPL version 3 or later). It comes with NO WARRANTY.
#pragma once

#include "DripSimMode.h"

// The rain mode's instance (registered with the framework by RainMain.cpp) and its settings loader.
DripSim::IMode* RainModeInstance();
void RainLoadSettings();
// A soak from outside (the DripSim_Soak mod event, the soak key): strength 0..1 (how drenched), depth = game units of water the skin was in (1000 = all of it).
// The next Start("rain", 2) or the next Update of a running rain mode picks it up. Safe from any thread.
void RainRequestSoak(float strength, float depth);
// The soak is off unless DripSimRain.ini has EnableSoak=1 (version 1 ships without it): RainRequestSoak then does nothing.
bool RainSoakEnabled();
