// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 AntiArc. This program is free software: see the LICENSE file (GNU GPL version 3 or later). It comes with NO WARRANTY.
#include "Params.h"

#include "DripSimMode.h"

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
        values.donor = ini.Bool("DonorTextures", false);
        values.perfLog = ini.Bool("PerfLog", false);
        values.refraction = ini.Float("Refraction", 0.0f);
        values.rainDensityRef = std::max(ini.Float("RainDensityRef", 1500.0f), 1.0f);
        g_values = values;
        const auto summary = std::format("framework {}, rain density ref {:.0f}, normal maps {}, refraction {:.2f}", values.enabled ? "enabled" : "DISABLED", values.rainDensityRef, values.normalMaps ? "on" : "off", values.refraction);
        logger::info("Params: {}", summary);
        return summary;
    }
}
