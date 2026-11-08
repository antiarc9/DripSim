// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 AntiArc. This program is free software: see the LICENSE file (GNU GPL version 3 or later). It comes with NO WARRANTY.
#pragma once

#include "Surfaces.h"
#include "DripSimAPI.h"

// The film: per mode a layer = a thickness map H (1024 x 1024 floats) and its render into RGBA pixels (translucent fluid with bright rims, glints
// and a wet halo; the colours come from the layer's Look). A compositor stacks the layers into the one image shown on the body.
//
// H has two copies: `permanent` (landed deposits, run tracks, settled pools: stamps are max-blended and stay) and `view` (permanent plus the
// things that move this frame: landing deposits, runner heads, hanging beads). Everything is tiled (32 px tiles of the 2048 film): only tiles that changed are
// re-rendered and re-composited, so an update costs in proportion to what moved.
namespace DS::Film
{
    constexpr int kSimSize = DS::Atlas::kSim;  // what modes see (ILayer::Size)
    constexpr int kSize = DS::Atlas::kFilm;    // the actual film: 2x finer, coordinates are scaled on the way in
    constexpr int kScale = kSize / kSimSize;
    constexpr int kTile = DS::Atlas::kTile;
    constexpr int kTiles = kSize / kTile;

    class Layer final : public DripSim::ILayer
    {
    public:
        // ---- DripSim::ILayer (what a mode sees)
        void Clear() override;
        void Stamp(float x, float y, float rx, float ry, float angle, float height, float power, bool permanent) override;
        void Erase(float x, float y, float rx, float ry, float angle, float strength) override;
        void SetLook(const DripSim::Look* look) override;
        void SetAlpha(float alpha) override;
        void SetWetFilm(float belowZ, float amount) override;
        void SetSurface(int surface) { m_surface = surface; }
        std::int32_t Size() const override { return kSimSize; }  // modes draw in 1024 atlas pixels; the film scales them to its own resolution

        // ---- framework side
        void Reserve();  // allocates the maps (about 48 MB): done ahead of time by Prewarm so that the first start does not hitch
        void Allocate(const std::uint8_t* keepMask);  // keepMask: the surface's draw mask (Field::KeepMask), outlives the layer
        void BeginFrame();  // tiles that held moving things last frame are reset to the permanent film
        void EndFrame();
        bool Render();      // re-renders the dirty tiles into this layer's pixels; true if any
        bool TileChanged(int tile) const { return m_changed[static_cast<std::size_t>(tile)] != 0; }
        void ClearChanged() { m_changed.fill(0); }
        const std::uint32_t* Pixels() const { return m_pixels.data(); }
        bool WantsNormals() const;  // the look asks for a normal map and the framework's NormalMaps switch is on
        const std::int16_t* Slopes() const { return m_slope.empty() ? nullptr : m_slope.data(); }  // 2 per pixel: thickness slope in x, y per atlas pixel, times 8192
        float Alpha() const { return m_alpha; }
        const DripSim::Look& GetLook() const { return m_look; }
        bool AlphaChanged() const { return m_alphaChanged; }
        void ClearAlphaChanged() { m_alphaChanged = false; }

    private:
        void MarkRect(int x0, int y0, int x1, int y1, int margin);
        void RenderTile(int tx, int ty);

        const std::uint8_t* m_keep = nullptr;
        std::vector<float> m_perm;
        std::vector<float> m_view;
        std::vector<std::uint32_t> m_pixels;
        std::vector<std::int16_t> m_slope;  // only for layers whose look asks for a normal map
        std::array<std::uint8_t, kTiles * kTiles> m_dirty{};
        std::array<std::uint8_t, kTiles * kTiles> m_changed{};  // re-rendered since the compositor last looked
        std::array<std::uint8_t, kTiles * kTiles> m_dynLast{};
        std::array<std::uint8_t, kTiles * kTiles> m_dynNow{};
        DripSim::Look m_look = DripSim::DefaultLook();
        float m_alpha = 1.0f;
        int m_surface = 0;
        bool m_pristine = false;  // nothing was drawn since the maps were allocated or cleared
        float m_wetZ = 0.0f;       // the wet film: skin below this height (skin space) ...
        float m_wetAmount = 0.0f;  // ... gets a film of this strength (0 = none)
        bool m_alphaChanged = false;
    };

    // Each surface (body, hands, feet) has its own compositor: up to kMaxLayers layers, bottom to top in the order they were acquired, stacked
    // into that surface's final image.
    constexpr int kMaxLayers = 4;
    void SetNormalMapsEnabled(bool on);  // the framework's NormalMaps setting: with it off no layer computes slopes or normals
    Layer* Acquire(int surface, const std::uint8_t* keepMask);  // nullptr when all are taken; the layer comes back cleared with the default look
    void Release(int surface, Layer* layer);
    void Prewarm();  // allocates every layer / canvas buffer and the shared noise; call once from a background thread (Acquire waits for it)
    int ActiveLayers(int surface);
    std::uint64_t TilesRendered();  // total tiles rendered so far (a statistic, for the log and the preview)
    // Renders every layer's dirty tiles and recomposites what changed. True if the surface's final image changed.
    bool Render(int surface);
    void BeginFrame(int surface);
    void EndFrame(int surface);
    const std::uint32_t* Pixels(int surface);  // the final image, ABGR (R in the low byte), 2048 x 2048
    // The model-space normal map (RGBA8, the game's encoding: R = x, G = z, B = y of the skin-space normal) that goes with Pixels, or nullptr when no layer asks for one.
    // Changed tiles are the same as ChangedTiles.
    const std::uint32_t* NormalPixels(int surface);
    // 1 for every 64 px tile (row-major, kTiles x kTiles) the last Render(surface) changed in the final image: only those need uploading.
    const std::uint8_t* ChangedTiles(int surface);
    // The specular settings the surface's overlay material should have: those of the top layer that asks for any (false = leave the material alone).
    bool MaterialSpec(int surface, float* power, float* scale);
    void Reset(int surface);                   // drops all layers (used when the surface is lost)
}
