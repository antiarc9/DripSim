// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 AntiArc. This program is free software: see the LICENSE file (GNU GPL version 3 or later). It comes with NO WARRANTY.
#include "Core.h"
#include "Spike.h"

namespace DS::Spike
{
    // Main::Update hook: once per game frame on the main thread.
    void OnFrame()
    {
        TextureOnFrame();
        DS::Core::OnFrame();
        GravityOnFrame();
    }
}
