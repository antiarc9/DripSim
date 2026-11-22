// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 AntiArc. This program is free software: see the LICENSE file (GNU GPL version 3 or later). It comes with NO WARRANTY.
#include "WetDetail.h"

#include "Surfaces.h"

namespace
{
    // the file: "DSWD", u32 version (1), u32 size, f32 base (the gloss of plain wet skin, 0..1), f32 mm per height unit,
    // then size*size u8 gloss, then size*size u16 height (rows top-down, the same layout as the film)
    constexpr int kN = DS::Atlas::kFilm;
    std::vector<std::uint8_t> g_gloss;
    std::vector<std::uint16_t> g_height;
    float g_invBase = 1.0f;
    float g_mmPerUnit = 0.0f;
    std::atomic<bool> g_ready{ false };
    bool g_tried = false;

    std::size_t At(int x, int y)
    {
        return static_cast<std::size_t>(std::clamp(y, 0, kN - 1)) * kN + static_cast<std::size_t>(std::clamp(x, 0, kN - 1));
    }
}

namespace DS::WetDetail
{
    void Load()
    {
        if (g_tried) return;
        g_tried = true;
        const std::filesystem::path path = std::filesystem::path("Data") / "SKSE" / "Plugins" / "DripSim_WetDetail.bin";
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            logger::info("WetDetail: no {} (no wet-skin detail)", path.generic_string());
            return;
        }
        char magic[4] = {};
        std::uint32_t version = 0, size = 0;
        float base = 0.0f, mm = 0.0f;
        in.read(magic, 4);
        in.read(reinterpret_cast<char*>(&version), 4);
        in.read(reinterpret_cast<char*>(&size), 4);
        in.read(reinterpret_cast<char*>(&base), 4);
        in.read(reinterpret_cast<char*>(&mm), 4);
        if (!in || std::memcmp(magic, "DSWD", 4) != 0 || version != 1 || size != static_cast<std::uint32_t>(kN) || !(base > 0.05f && base <= 1.0f) || !(mm > 0.0f && mm < 1.0f)) {
            logger::warn("WetDetail: {} is not a version 1 map of {} px (version {}, size {}); no wet-skin detail", path.generic_string(), kN, version, size);
            return;
        }
        std::vector<std::uint8_t> gloss(static_cast<std::size_t>(kN) * kN);
        std::vector<std::uint16_t> height(static_cast<std::size_t>(kN) * kN);
        in.read(reinterpret_cast<char*>(gloss.data()), static_cast<std::streamsize>(gloss.size()));
        in.read(reinterpret_cast<char*>(height.data()), static_cast<std::streamsize>(height.size() * 2));
        if (!in) {
            logger::warn("WetDetail: {} is cut short; no wet-skin detail", path.generic_string());
            return;
        }
        g_gloss = std::move(gloss);
        g_height = std::move(height);
        g_invBase = 1.0f / (base * 255.0f);
        g_mmPerUnit = mm;
        g_ready = true;
        logger::info("WetDetail: loaded {} ({} px, plain wet skin {:.2f})", path.generic_string(), kN, base);
    }

    bool Ready()
    {
        return g_ready.load(std::memory_order_acquire);
    }

    float Gloss(int x, int y)
    {
        return static_cast<float>(g_gloss[At(x, y)]) * g_invBase;
    }

    void Slope(int x, int y, float& sx, float& sy)
    {
        sx = (static_cast<float>(g_height[At(x + 1, y)]) - static_cast<float>(g_height[At(x - 1, y)])) * 0.5f * g_mmPerUnit;
        sy = (static_cast<float>(g_height[At(x, y + 1)]) - static_cast<float>(g_height[At(x, y - 1)])) * 0.5f * g_mmPerUnit;
    }
}
