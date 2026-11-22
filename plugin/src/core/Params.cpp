// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 AntiArc. This program is free software: see the LICENSE file (GNU GPL version 3 or later). It comes with NO WARRANTY.
#include "Params.h"

#include "DripSimMode.h"
#include "WetDetail.h"

namespace
{
    DS::Params::Values g_values;
}

namespace DS::Params
{
    const Values& Get()
    {
        return g_values;
    }

    std::string Load()
    {
        const DripSim::Ini ini("DripSim");
        Values values;
        values.enabled = ini.Bool("Enabled", true);
        values.normalMaps = ini.Bool("NormalMaps", false);
        values.specMaps = ini.Bool("SpecMaps", true);
        values.reloadKey = ini.Bool("ReloadKey", true);
        values.debugKeys = ini.Bool("DebugKeys", false);
        values.hotkeys = ini.Bool("Hotkeys", false);
        values.autoReport = ini.Bool("AutoReport", true);
        values.premultiplied = ini.Bool("PremultipliedAlpha", false);
        values.overlayDecal = ini.Bool("OverlayDecal", false);
        values.donor = ini.Bool("DonorTextures", false);
        values.perfLog = ini.Bool("PerfLog", false);
        values.refraction = ini.Float("Refraction", 0.0f);
        values.rainDensityRef = std::max(ini.Float("RainDensityRef", 1500.0f), 1.0f);
        values.wetDetail = std::clamp(ini.Float("WetDetail", 0.6f), 0.0f, 3.0f);
        values.wetDetailBump = std::clamp(ini.Float("WetDetailBump", 1.0f), 0.0f, 5.0f);
        g_values = values;
        if (values.wetDetail > 0.0f) DS::WetDetail::Load();  // (once; read on the main thread before any film renders with it)
        const auto summary = std::format("framework {}, rain density ref {:.0f}, normal maps {}, refraction {:.2f}, wet detail {:.2f} / bump {:.2f}{}", values.enabled ? "enabled" : "DISABLED", values.rainDensityRef, values.normalMaps ? "on" : "off", values.refraction,
            values.wetDetail, values.wetDetailBump, values.wetDetail > 0.0f && !DS::WetDetail::Ready() ? " (no map)" : "");
        logger::info("Params: {}", summary);
        return summary;
    }
}
