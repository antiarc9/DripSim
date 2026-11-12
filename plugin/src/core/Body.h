// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 AntiArc. This program is free software: see the LICENSE file (GNU GPL version 3 or later). It comes with NO WARRANTY.
#pragma once

#include "Surfaces.h"

// The skin surfaces the framework can paint: each is found from its skee overlay shapes ("Body [Ovl#]", "Hands [Ovl#]", "Feet [Ovl#]", "Face [Ovl#]"), which are
// clones of the base skin shape and share its mesh, UVs and skin. Their data is read once from the GPU buffers (render thread) and shared with the
// gravity field.
namespace DS::Body
{
    struct Static
    {
        int surface = 0;
#ifdef DRIPSIM_PREVIEW
        int shape = 0;  // preview build (no engine): nothing to hold
#else
        RE::NiPointer<RE::BSGeometry> shape;  // the overlay shape it was read from (its skin instance holds the live bones)
        // The bones of the skin instance, with a reference each. skinInstance->bones holds plain pointers: when the game rebuilds the player's 3D (clothes change,
        // a scene, a re-equip) it frees the old bones, and reading them crashed (crash log 2026-11-08). With our own reference a bone stays valid memory; its
        // transform is then just stale until Field::Alive() sees that the shape was replaced.
        std::vector<RE::NiPointer<RE::NiAVObject>> boneRefs;
#endif
        std::uint32_t vertexCount = 0;
        std::uint32_t triangleCount = 0;
        std::uint32_t boneCount = 0;
        std::vector<float> position;            // 3 per vertex, skin space
        std::vector<float> uv;                  // 2 per vertex, [0,1], v down like DDS rows
        std::vector<float> weight;              // 4 per vertex
        std::vector<std::uint8_t> boneIndex;    // 4 per vertex: index into skinInstance->bones (the partition palette is already applied)
        std::vector<std::uint16_t> triangle;    // 3 per triangle, global vertex ids
        std::vector<float> skinToBoneRotation;  // 9 per bone (NiMatrix3 entry[row][col] order)
        std::vector<float> skinToBoneTranslate; // 3 per bone
        std::vector<float> skinToBoneScale;     // 1 per bone
    };

    // The data captured by the last request for a surface, or null before the first one finished.
    std::shared_ptr<const Static> Get(int surface);
    // Asks the render thread to (re)capture a surface; `writeFile` also writes DripSim_body.bin (S4, body only).
    void Request(int surface, bool writeFile);
}
