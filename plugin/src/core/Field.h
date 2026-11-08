// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 AntiArc. This program is free software: see the LICENSE file (GNU GPL version 3 or later). It comes with NO WARRANTY.
#pragma once

#include "Body.h"

// The live gravity field of one skin surface on its UV atlas (1024 x 1024): which triangle covers each pixel (static), and per triangle the live
// downhill direction in the image and the steepness (from the skeleton, see Field.cpp for the maths). One instance per surface.
namespace DS::Field
{
    constexpr int kSize = 1024;

    struct Sample
    {
        float dx = 0.0f;  // unit downhill direction in the image (x right, y down); 0,0 where flat
        float dy = 0.0f;
        float steep = 0.0f;  // 0 flat .. 1 vertical
        float mmpx = 1.5f;   // millimetres per pixel here
        float up = 0.0f;     // how much the surface faces the sky now (0..1)
        bool inner = false;  // at least 3 px away from the edge of a UV island
    };

    struct TriLive
    {
        float dx = 0.0f;
        float dy = 0.0f;
        float steep = 0.0f;
        float up = 0.0f;  // faces the sky: -(g . n), 0..1
        std::uint8_t flat = 1;
    };

    struct TriStatic
    {
        float a[6]{};
        float n[3]{};
        float mmpx = 1.5f;
        float cz = 0.0f;
        float ny = 0.0f;
        float rest = 0.0f;
        bool valid = false;
    };

    struct RayHit
    {
        float x = 0, y = 0;    // atlas pixel
        float tx = 0, ty = 1;  // unit travel direction along the surface in the atlas
        float dist = 0;
        float slant = 0;
        float point[3]{};
        float normal[3]{};
    };

#ifndef DRIPSIM_PREVIEW
    // True while this geometry is still part of the player's current 3D (a clothes change or a 3D rebuild replaces the overlay shapes: the old ones are then orphans that we
    // would go on drawing into for nobody). Searches the scene graph by name, so the caller should not ask more than about twice a second.
    bool ShapeStillAttached(RE::BSGeometry* shape);
#endif

    class Instance
    {
    public:
        // Rasterises the surface and builds the per-triangle statics. Needs the captured data (Body::Get(surface)).
        bool Build(std::shared_ptr<const Body::Static> body);
        bool Ready() const { return m_ready; }
        // The overlay shape it was read from still hangs under the player.
        bool Alive() const;
        // Recomputes gravity per triangle from the live bones.
        void Update();

        // World-space queries on the live skinned surface (positions are recomputed once per Update).
        bool Raycast(const float from[3], const float dir[3], float maxDist, RayHit* out) const;
        bool Bounds(float lo[3], float hi[3]) const;

        Sample At(float x, float y) const;  // smoothed over a few pixels
        bool Inner(int x, int y) const;
        bool Keep(int x, int y) const;  // inner dilated by 2 px: where the film may be drawn
        std::int32_t TriAt(int x, int y) const;
        const std::vector<std::uint8_t>& KeepMask() const { return m_keep; }

        // Rest-pose (anatomical) quantities of a triangle: height, front/back facing and how vertical it is when standing.
        float CenterZ(std::int32_t tri) const;
        float NormalY(std::int32_t tri) const;
        float RestSteep(std::int32_t tri) const;
        float ZMin() const { return m_zMin; }
        float ZMax() const { return m_zMax; }

        const std::vector<TriLive>& Live() const { return m_live; }
        const std::vector<TriStatic>& Tris() const { return m_tri; }
        const std::vector<std::int32_t>& TriRaster() const { return m_triId; }
        // The triangle ids dilated by a few pixels past the island edges (padding, so that bilinear sampling at a seam never meets nothing), and the smooth skin normal per pixel
        // (welded vertex normals interpolated over the triangle, x 16384, 3 per pixel, also in the padding; 0,0,0 = none).
        const std::vector<std::int32_t>& PaddedTriRaster() const { return m_padTri; }
        const std::vector<std::int16_t>& SmoothNormals() const { return m_smooth; }
        std::shared_ptr<const Body::Static> Data() const { return m_body; }

    private:
        std::shared_ptr<const Body::Static> m_body;
        std::vector<std::int32_t> m_triId;
        std::vector<std::int32_t> m_padTri;
        std::vector<std::int16_t> m_smooth;
        std::vector<std::uint8_t> m_inner;
        std::vector<std::uint8_t> m_keep;
        std::vector<TriStatic> m_tri;
        std::vector<TriLive> m_live;
        std::vector<float> m_boneG;
        float m_zMin = 0.0f;
        float m_zMax = 1.0f;
        bool m_ready = false;
        bool m_loggedBones = false;
        bool EnsureWorld() const;
        mutable std::vector<float> m_world;  // 3 per vertex, world space
        mutable bool m_worldValid = false;
        mutable std::chrono::steady_clock::time_point m_attachChecked{};
        mutable bool m_attached = true;
        mutable float m_lo[3]{};
        mutable float m_hi[3]{};
        mutable bool m_loggedWorld = false;
    };

    Instance& Get(int surface);
}
