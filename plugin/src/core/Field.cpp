// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 AntiArc. This program is free software: see the LICENSE file (GNU GPL version 3 or later). It comes with NO WARRANTY.
#include "Field.h"

#include <algorithm>
#include <unordered_map>

// Gravity per triangle from the live skeleton:
//   per bone i: g_i = S_i^T W_i^T g_world   (S = skin-to-bone rotation, W = bone world rotation, NiMatrix3 entry[row][col], g_world = (0,0,-1))
//   per vertex: weight blend of its four bones; per triangle: normalised mean of its vertices.
// Per triangle (static): J = d position / d pixel from its 3D edges and image edges, A = (J^T J)^-1 J^T, normal n.
// Live: gt = g - (g.n) n is the downhill direction on the surface, d = A gt its direction in the image, |gt| the steepness.
namespace
{
    constexpr float kUnitMm = 13.0f;  // one Skyrim unit is about 1.3 cm
    constexpr float kFlat = 0.22f;

    using DS::Field::TriStatic;

    // what Build assembles before it is moved into the instance
    struct State
    {
        std::shared_ptr<const DS::Body::Static> body;
        std::vector<std::int32_t> triId;
        std::vector<std::int32_t> padTri;
        std::vector<std::int16_t> smooth;
        std::vector<std::int32_t> twin;
        std::vector<std::uint8_t> inner;
        std::vector<std::uint8_t> keep;
        std::vector<TriStatic> tri;
        std::vector<DS::Field::TriLive> live;
        std::vector<float> boneG;
        float zMin = 0.0f;
        float zMax = 1.0f;
        bool ready = false;
    };

    std::size_t Index(int x, int y)
    {
        return static_cast<std::size_t>(y) * DS::Field::kSize + static_cast<std::size_t>(x);
    }

    bool InRaster(int x, int y)
    {
        return x >= 0 && y >= 0 && x < DS::Field::kSize && y < DS::Field::kSize;
    }

    // One 4-neighbour erosion (shrink = true) or dilation of `mask` limited to `limit` (when given).
    void Morph(std::vector<std::uint8_t>& mask, bool shrink, const std::vector<std::uint8_t>* limit)
    {
        constexpr int n = DS::Field::kSize;
        std::vector<std::uint8_t> out(mask.size(), 0);
        for (int y = 0; y < n; ++y) {
            for (int x = 0; x < n; ++x) {
                const auto i = Index(x, y);
                const auto at = [&](int xx, int yy) { return InRaster(xx, yy) ? mask[Index(xx, yy)] : std::uint8_t{ 0 }; };
                if (shrink) {
                    out[i] = mask[i] && at(x - 1, y) && at(x + 1, y) && at(x, y - 1) && at(x, y + 1);
                } else {
                    out[i] = mask[i] || at(x - 1, y) || at(x + 1, y) || at(x, y - 1) || at(x, y + 1);
                    if (limit && !(*limit)[i]) out[i] = 0;
                }
            }
        }
        mask.swap(out);
    }
}

namespace DS::Field
{
    // The smooth skin normal per pixel: area-weighted face normals welded by position (the vertices on both sides of a UV seam share one normal, so the lighting is continuous across
    // it), interpolated over each triangle, then dilated 4 pixels past the island edges. Also the triangle ids dilated the same way.
    void BuildSmooth(const DS::Body::Static& b, const std::vector<std::int32_t>& triId, std::vector<std::int32_t>& padTri, std::vector<std::int16_t>& smooth)
    {
        const int n = DS::Field::kSize;
        const auto size = static_cast<float>(n);
        std::unordered_map<std::uint64_t, std::array<float, 3>> welded;
        const auto key = [](const float* p) {
            const auto q = [](float v) { return static_cast<std::uint64_t>(static_cast<std::int64_t>(std::lround(v * 20.0f)) & 0x1FFFFF); };
            return q(p[0]) | (q(p[1]) << 21) | (q(p[2]) << 42);
        };
        for (std::uint32_t t = 0; t < b.triangleCount; ++t) {
            const float* p[3];
            for (int k = 0; k < 3; ++k) p[k] = &b.position[static_cast<std::size_t>(b.triangle[t * 3 + static_cast<std::uint32_t>(k)]) * 3];
            const float e1[3] = { p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2] };
            const float e2[3] = { p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2] };
            const float fn[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };  // area weighted
            for (int k = 0; k < 3; ++k) {
                auto& acc = welded[key(p[k])];
                for (int c = 0; c < 3; ++c) acc[static_cast<std::size_t>(c)] += fn[c];
            }
        }
        const auto vertexNormal = [&](std::uint32_t vertex, float out[3]) {
            const auto& acc = welded[key(&b.position[static_cast<std::size_t>(vertex) * 3])];
            const float l = std::sqrt(acc[0] * acc[0] + acc[1] * acc[1] + acc[2] * acc[2]);
            for (int c = 0; c < 3; ++c) out[c] = l > 1e-9f ? acc[static_cast<std::size_t>(c)] / l : 0.0f;
        };
        padTri = triId;
        smooth.assign(triId.size() * 3, 0);
        std::vector<std::uint8_t> have(triId.size(), 0);
        for (int y = 0; y < n; ++y) {
            for (int x = 0; x < n; ++x) {
                const auto i = Index(x, y);
                const auto t = triId[i];
                if (t < 0) continue;
                const std::uint32_t v[3] = { b.triangle[static_cast<std::uint32_t>(t) * 3], b.triangle[static_cast<std::uint32_t>(t) * 3 + 1], b.triangle[static_cast<std::uint32_t>(t) * 3 + 2] };
                float px[3];
                float py[3];
                for (int k = 0; k < 3; ++k) {
                    px[k] = b.uv[static_cast<std::size_t>(v[k]) * 2] * size;
                    py[k] = b.uv[static_cast<std::size_t>(v[k]) * 2 + 1] * size;
                }
                const float denom = (py[1] - py[2]) * (px[0] - px[2]) + (px[2] - px[1]) * (py[0] - py[2]);
                if (std::fabs(denom) < 1e-9f) continue;
                const float fx = static_cast<float>(x) + 0.5f;
                const float fy = static_cast<float>(y) + 0.5f;
                float w[3];
                w[0] = ((py[1] - py[2]) * (fx - px[2]) + (px[2] - px[1]) * (fy - py[2])) / denom;
                w[1] = ((py[2] - py[0]) * (fx - px[2]) + (px[0] - px[2]) * (fy - py[2])) / denom;
                w[2] = 1.0f - w[0] - w[1];
                float nn[3] = { 0, 0, 0 };
                for (int k = 0; k < 3; ++k) {
                    float vn[3];
                    vertexNormal(v[k], vn);
                    for (int c = 0; c < 3; ++c) nn[c] += std::clamp(w[k], 0.0f, 1.0f) * vn[c];
                }
                const float l = std::sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
                if (l < 1e-6f) continue;
                for (int c = 0; c < 3; ++c) smooth[i * 3 + static_cast<std::size_t>(c)] = static_cast<std::int16_t>(std::clamp(nn[c] / l * 16384.0f, -16384.0f, 16384.0f));
                have[i] = 1;
            }
        }
        // padding: four rounds of copying from a neighbour that has a value
        for (int round = 0; round < 4; ++round) {
            auto next = have;
            auto nextTri = padTri;
            auto nextSmooth = smooth;
            for (int y = 0; y < n; ++y) {
                for (int x = 0; x < n; ++x) {
                    const auto i = Index(x, y);
                    if (have[i]) continue;
                    for (const auto [dx, dy] : { std::pair{ 1, 0 }, std::pair{ -1, 0 }, std::pair{ 0, 1 }, std::pair{ 0, -1 } }) {
                        const int nx = x + dx;
                        const int ny = y + dy;
                        if (!InRaster(nx, ny) || !have[Index(nx, ny)]) continue;
                        const auto j = Index(nx, ny);
                        next[i] = 1;
                        nextTri[i] = padTri[j];
                        for (int c = 0; c < 3; ++c) nextSmooth[i * 3 + static_cast<std::size_t>(c)] = smooth[j * 3 + static_cast<std::size_t>(c)];
                        break;
                    }
                }
            }
            have = std::move(next);
            padTri = std::move(nextTri);
            smooth = std::move(nextSmooth);
        }
    }

    // Across the UV seams: every skin pixel within a few pixels of an island edge gets its rest-pose position (barycentric over its triangle); a pixel whose nearest
    // edge pixel in 3D lies far away in the atlas (another island, or the far side of a cut in the same one) is its twin. Open edges (the neck hole) find nothing close.
    void BuildTwins(const DS::Body::Static& b, const std::vector<std::int32_t>& triId, const std::vector<std::uint8_t>& inner, std::vector<std::int32_t>& twin)
    {
        const int n = DS::Field::kSize;
        const auto size = static_cast<float>(n);
        twin.assign(triId.size(), -1);
        struct Edge
        {
            float p[3];
            std::int32_t pixel;
        };
        std::vector<Edge> edge;
        for (int y = 0; y < n; ++y) {
            for (int x = 0; x < n; ++x) {
                const auto i = Index(x, y);
                const auto t = triId[i];
                if (t < 0 || inner[i]) continue;
                const std::uint32_t v[3] = { b.triangle[static_cast<std::uint32_t>(t) * 3], b.triangle[static_cast<std::uint32_t>(t) * 3 + 1], b.triangle[static_cast<std::uint32_t>(t) * 3 + 2] };
                float px[3];
                float py[3];
                for (int k = 0; k < 3; ++k) {
                    px[k] = b.uv[static_cast<std::size_t>(v[k]) * 2] * size;
                    py[k] = b.uv[static_cast<std::size_t>(v[k]) * 2 + 1] * size;
                }
                const float denom = (py[1] - py[2]) * (px[0] - px[2]) + (px[2] - px[1]) * (py[0] - py[2]);
                if (std::fabs(denom) < 1e-9f) continue;
                const float fx = static_cast<float>(x) + 0.5f;
                const float fy = static_cast<float>(y) + 0.5f;
                float w[3];
                w[0] = std::clamp(((py[1] - py[2]) * (fx - px[2]) + (px[2] - px[1]) * (fy - py[2])) / denom, 0.0f, 1.0f);
                w[1] = std::clamp(((py[2] - py[0]) * (fx - px[2]) + (px[0] - px[2]) * (fy - py[2])) / denom, 0.0f, 1.0f);
                w[2] = std::max(1.0f - w[0] - w[1], 0.0f);
                Edge e{};
                for (int k = 0; k < 3; ++k) {
                    for (int c = 0; c < 3; ++c) e.p[c] += w[k] * b.position[static_cast<std::size_t>(v[k]) * 3 + static_cast<std::size_t>(c)];
                }
                e.pixel = static_cast<std::int32_t>(i);
                edge.push_back(e);
            }
        }
        constexpr float kCell = 0.25f;   // game units (~3 mm)
        constexpr float kMatch = 0.45f;  // the most a twin may be away in 3D (both sides of a seam stop a pixel or two short of it)
        constexpr int kFar = 8;          // pixels: closer than this in the atlas is the same side
        const auto cellOf = [](float v) { return static_cast<std::int64_t>(std::floor(v / kCell)); };
        const auto key = [](std::int64_t a, std::int64_t c, std::int64_t d) { return static_cast<std::uint64_t>((a & 0x1FFFFF) | ((c & 0x1FFFFF) << 21) | ((d & 0x1FFFFF) << 42)); };
        std::unordered_map<std::uint64_t, std::vector<std::int32_t>> grid;
        grid.reserve(edge.size());
        for (std::size_t k = 0; k < edge.size(); ++k) grid[key(cellOf(edge[k].p[0]), cellOf(edge[k].p[1]), cellOf(edge[k].p[2]))].push_back(static_cast<std::int32_t>(k));
        for (const auto& e : edge) {
            const int ex = e.pixel % n;
            const int ey = e.pixel / n;
            float best = kMatch * kMatch;
            std::int32_t found = -1;
            const auto cx = cellOf(e.p[0]);
            const auto cy = cellOf(e.p[1]);
            const auto cz = cellOf(e.p[2]);
            for (std::int64_t a = cx - 2; a <= cx + 2; ++a) {
                for (std::int64_t c = cy - 2; c <= cy + 2; ++c) {
                    for (std::int64_t d = cz - 2; d <= cz + 2; ++d) {
                        const auto it = grid.find(key(a, c, d));
                        if (it == grid.end()) continue;
                        for (const auto k : it->second) {
                            const auto& o = edge[static_cast<std::size_t>(k)];
                            const int ox = o.pixel % n;
                            const int oy = o.pixel / n;
                            if (std::abs(ox - ex) < kFar && std::abs(oy - ey) < kFar) continue;
                            const float dx = o.p[0] - e.p[0];
                            const float dy = o.p[1] - e.p[1];
                            const float dz = o.p[2] - e.p[2];
                            const float d2 = dx * dx + dy * dy + dz * dz;
                            if (d2 < best) {
                                best = d2;
                                found = o.pixel;
                            }
                        }
                    }
                }
            }
            twin[static_cast<std::size_t>(e.pixel)] = found;
        }
    }

    bool Instance::Build(std::shared_ptr<const Body::Static> bodyPtr)
    {
        if (!bodyPtr || bodyPtr->triangleCount == 0) return false;
        State s;
        s.body = std::move(bodyPtr);
        const auto& b = *s.body;
        const auto size = static_cast<float>(kSize);
        s.triId.assign(static_cast<std::size_t>(kSize) * kSize, -1);
        s.tri.assign(b.triangleCount, TriStatic{});
        s.live.assign(b.triangleCount, TriLive{});
        float zMin = 1e9f;
        float zMax = -1e9f;
        for (std::uint32_t t = 0; t < b.triangleCount; ++t) {
            const std::uint16_t v[3] = { b.triangle[t * 3], b.triangle[t * 3 + 1], b.triangle[t * 3 + 2] };
            float px[3];
            float py[3];
            const float* p[3];
            for (int k = 0; k < 3; ++k) {
                px[k] = b.uv[static_cast<std::size_t>(v[k]) * 2] * size;
                py[k] = b.uv[static_cast<std::size_t>(v[k]) * 2 + 1] * size;
                p[k] = &b.position[static_cast<std::size_t>(v[k]) * 3];
            }
            const float denom = (py[1] - py[2]) * (px[0] - px[2]) + (px[2] - px[1]) * (py[0] - py[2]);
            if (std::fabs(denom) > 1e-9f) {
                const int x0 = std::max(0, static_cast<int>(std::floor(std::min({ px[0], px[1], px[2] }))));
                const int x1 = std::min(kSize - 1, static_cast<int>(std::ceil(std::max({ px[0], px[1], px[2] }))));
                const int y0 = std::max(0, static_cast<int>(std::floor(std::min({ py[0], py[1], py[2] }))));
                const int y1 = std::min(kSize - 1, static_cast<int>(std::ceil(std::max({ py[0], py[1], py[2] }))));
                for (int y = y0; y <= y1; ++y) {
                    for (int x = x0; x <= x1; ++x) {
                        const float fx = static_cast<float>(x) + 0.5f;
                        const float fy = static_cast<float>(y) + 0.5f;
                        const float l0 = ((py[1] - py[2]) * (fx - px[2]) + (px[2] - px[1]) * (fy - py[2])) / denom;
                        const float l1 = ((py[2] - py[0]) * (fx - px[2]) + (px[0] - px[2]) * (fy - py[2])) / denom;
                        const float l2 = 1.0f - l0 - l1;
                        constexpr float eps = -1e-4f;
                        if (l0 >= eps && l1 >= eps && l2 >= eps) s.triId[Index(x, y)] = static_cast<std::int32_t>(t);
                    }
                }
            }
            auto& td = s.tri[t];
            td.cz = (p[0][2] + p[1][2] + p[2][2]) / 3.0f;
            zMin = std::min(zMin, td.cz);
            zMax = std::max(zMax, td.cz);
            const float d1x = px[1] - px[0];
            const float d1y = py[1] - py[0];
            const float d2x = px[2] - px[0];
            const float d2y = py[2] - py[0];
            const float det = d1x * d2y - d1y * d2x;
            float e1[3];
            float e2[3];
            for (int k = 0; k < 3; ++k) {
                e1[k] = p[1][k] - p[0][k];
                e2[k] = p[2][k] - p[0][k];
            }
            float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
            const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (nl < 1e-9f || std::fabs(det) < 1e-6f) continue;
            for (int k = 0; k < 3; ++k) td.n[k] = n[k] / nl;
            td.ny = td.n[1];
            td.rest = std::sqrt(std::clamp(1.0f - td.n[2] * td.n[2], 0.0f, 1.0f));
            const float area3 = 0.5f * nl;
            const float areaPx = 0.5f * std::fabs(det);
            td.mmpx = std::clamp(kUnitMm * std::sqrt(area3 / areaPx), 0.8f, 4.0f);
            float j0[3];
            float j1[3];
            for (int k = 0; k < 3; ++k) {
                j0[k] = (e1[k] * d2y - e2[k] * d1y) / det;
                j1[k] = (-e1[k] * d2x + e2[k] * d1x) / det;
            }
            const float a = j0[0] * j0[0] + j0[1] * j0[1] + j0[2] * j0[2];
            const float bb = j0[0] * j1[0] + j0[1] * j1[1] + j0[2] * j1[2];
            const float c = j1[0] * j1[0] + j1[1] * j1[1] + j1[2] * j1[2];
            const float det2 = a * c - bb * bb;
            if (det2 < 1e-12f) continue;
            const float i00 = c / det2;
            const float i01 = -bb / det2;
            const float i11 = a / det2;
            for (int k = 0; k < 3; ++k) {
                td.a[k] = i00 * j0[k] + i01 * j1[k];
                td.a[3 + k] = i01 * j0[k] + i11 * j1[k];
            }
            td.valid = true;
        }
        s.zMin = zMin;
        s.zMax = zMax;
        BuildSmooth(b, s.triId, s.padTri, s.smooth);
        std::vector<std::uint8_t> covered(s.triId.size(), 0);
        for (std::size_t i = 0; i < covered.size(); ++i) covered[i] = s.triId[i] >= 0 ? 1 : 0;
        s.inner = covered;
        for (int i = 0; i < 3; ++i) Morph(s.inner, true, nullptr);
        s.keep = s.inner;
        BuildTwins(b, s.triId, s.inner, s.twin);
        for (int i = 0; i < 2; ++i) Morph(s.keep, false, &covered);
        s.boneG.assign(static_cast<std::size_t>(b.boneCount) * 3, 0.0f);
        s.ready = true;
        m_body = std::move(s.body);
        m_triId = std::move(s.triId);
        m_padTri = std::move(s.padTri);
        m_smooth = std::move(s.smooth);
        m_twin = std::move(s.twin);
        m_inner = std::move(s.inner);
        m_keep = std::move(s.keep);
        m_tri = std::move(s.tri);
        m_live = std::move(s.live);
        m_boneG = std::move(s.boneG);
        m_zMin = s.zMin;
        m_zMax = s.zMax;
        m_ready = true;
        m_attached = true;
        m_attachChecked = std::chrono::steady_clock::now();
        m_loggedBones = false;
        const auto twins = std::ranges::count_if(m_twin, [](std::int32_t t) { return t >= 0; });
        logger::info("Field {}: built {} triangles, z {:.1f}..{:.1f}, {} edge pixels with a twin across a seam", DS::Surface::Name(m_body->surface), m_tri.size(), m_zMin, m_zMax, twins);
        return true;
    }

#ifndef DRIPSIM_PREVIEW
    bool ShapeStillAttached(RE::BSGeometry* shape)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* root = player && player->Is3DLoaded() ? player->Get3D(false) : nullptr;
        if (!shape || !root) return false;
        const RE::BSFixedString name = shape->name;
        return root->GetObjectByName(name) == static_cast<RE::NiAVObject*>(shape);
    }
#endif

    bool Instance::Alive() const
    {
#ifdef DRIPSIM_PREVIEW
        return m_ready;
#else
        if (!m_ready || !m_body->shape || !m_body->shape->parent) return false;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->Is3DLoaded()) return false;
        if (m_body->shape->GetGeometryRuntimeData().skinInstance == nullptr) return false;
        const auto now = std::chrono::steady_clock::now();
        if (now - m_attachChecked > std::chrono::milliseconds(400)) {  // is it still in the player's 3D, or did a clothes change replace it?
            m_attachChecked = now;
            m_attached = ShapeStillAttached(m_body->shape.get());
            if (!m_attached) logger::info("Field {}: the overlay shape is no longer part of the player's 3D (clothes change or rebuild)", DS::Surface::Name(m_body->surface));
        }
        return m_attached;
#endif
    }

    bool Instance::EnsureWorld() const
    {
        if (m_worldValid) return true;
        if (!m_ready) return false;
        const auto& b = *m_body;
        m_world.assign(static_cast<std::size_t>(b.vertexCount) * 3, 0.0f);
#ifdef DRIPSIM_PREVIEW
        std::copy(b.position.begin(), b.position.begin() + static_cast<std::ptrdiff_t>(m_world.size()), m_world.begin());
#else
        auto* skin = b.shape ? b.shape->GetGeometryRuntimeData().skinInstance.get() : nullptr;
        if (!skin || b.boneRefs.size() < b.boneCount || b.skinToBoneTranslate.size() < static_cast<std::size_t>(b.boneCount) * 3) return false;
        // per bone: world position = R * (s * p) + T with R = boneWorld.R * skinToBone.R, s = both scales, T = boneWorld.R * (boneWorld.s * skinToBone.T) + boneWorld.T
        std::vector<float> palette(static_cast<std::size_t>(b.boneCount) * 13, 0.0f);
        for (std::uint32_t i = 0; i < b.boneCount; ++i) {
            auto* bone = b.boneRefs[i].get();  // our own reference: never a freed bone
            if (!bone) continue;
            const auto& w = bone->world;
            const float* sr = &b.skinToBoneRotation[static_cast<std::size_t>(i) * 9];
            const float* st = &b.skinToBoneTranslate[static_cast<std::size_t>(i) * 3];
            float* m = &palette[static_cast<std::size_t>(i) * 13];
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) m[r * 3 + c] = w.rotate.entry[r][0] * sr[0 * 3 + c] + w.rotate.entry[r][1] * sr[1 * 3 + c] + w.rotate.entry[r][2] * sr[2 * 3 + c];
            }
            const float wt[3] = { w.translate.x, w.translate.y, w.translate.z };
            for (int r = 0; r < 3; ++r) m[9 + r] = w.rotate.entry[r][0] * w.scale * st[0] + w.rotate.entry[r][1] * w.scale * st[1] + w.rotate.entry[r][2] * w.scale * st[2] + wt[r];
            m[12] = w.scale * b.skinToBoneScale[i];
        }
        for (std::uint32_t v = 0; v < b.vertexCount; ++v) {
            const float* p = &b.position[static_cast<std::size_t>(v) * 3];
            float acc[3] = { 0, 0, 0 };
            float wsum = 0;
            for (int k = 0; k < 4; ++k) {
                const float w = b.weight[static_cast<std::size_t>(v) * 4 + static_cast<std::size_t>(k)];
                const std::size_t bi = b.boneIndex[static_cast<std::size_t>(v) * 4 + static_cast<std::size_t>(k)];
                if (w <= 0.0f || bi >= b.boneCount || palette[bi * 13 + 12] == 0.0f) continue;
                const float* m = &palette[bi * 13];
                const float s = m[12];
                for (int r = 0; r < 3; ++r) acc[r] += w * (m[r * 3] * s * p[0] + m[r * 3 + 1] * s * p[1] + m[r * 3 + 2] * s * p[2] + m[9 + r]);
                wsum += w;
            }
            for (int r = 0; r < 3; ++r) m_world[static_cast<std::size_t>(v) * 3 + static_cast<std::size_t>(r)] = wsum > 0.0f ? acc[r] / wsum : 0.0f;
        }
#endif
        m_lo[0] = m_lo[1] = m_lo[2] = 1e30f;
        m_hi[0] = m_hi[1] = m_hi[2] = -1e30f;
        for (std::uint32_t v = 0; v < b.vertexCount; ++v) {
            const float* c = &m_world[static_cast<std::size_t>(v) * 3];
            if (c[0] == 0.0f && c[1] == 0.0f && c[2] == 0.0f) continue;  // an unweighted vertex
            for (int r = 0; r < 3; ++r) {
                m_lo[r] = std::min(m_lo[r], c[r]);
                m_hi[r] = std::max(m_hi[r], c[r]);
            }
        }
        m_worldValid = true;
        if (!m_loggedWorld) {
            m_loggedWorld = true;
#ifdef DRIPSIM_PREVIEW
            logger::info("Field {}: world bounds ({:.1f},{:.1f},{:.1f})..({:.1f},{:.1f},{:.1f})", DS::Surface::Name(b.surface), m_lo[0], m_lo[1], m_lo[2], m_hi[0], m_hi[1], m_hi[2]);
#else
            auto* player = RE::PlayerCharacter::GetSingleton();
            const auto pos = player ? player->GetPosition() : RE::NiPoint3{};
            logger::info("Field {}: world bounds ({:.1f},{:.1f},{:.1f})..({:.1f},{:.1f},{:.1f}), player at ({:.1f},{:.1f},{:.1f})", DS::Surface::Name(b.surface), m_lo[0], m_lo[1], m_lo[2],
                m_hi[0], m_hi[1], m_hi[2], pos.x, pos.y, pos.z);
#endif
        }
        return true;
    }

    bool Instance::Bounds(float lo[3], float hi[3]) const
    {
        if (!EnsureWorld()) return false;
        std::memcpy(lo, m_lo, sizeof m_lo);
        std::memcpy(hi, m_hi, sizeof m_hi);
        return m_hi[2] > m_lo[2];
    }

    // Moeller-Trumbore against every valid triangle, two-sided; the nearest hit inside maxDist wins.
    bool Instance::Raycast(const float from[3], const float dirIn[3], float maxDist, RayHit* out) const
    {
        if (!out || !EnsureWorld()) return false;
        const auto& b = *m_body;
        float d[3] = { dirIn[0], dirIn[1], dirIn[2] };
        const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (dl < 1e-9f) return false;
        for (float& c : d) c /= dl;
        float best = maxDist;
        std::int32_t bestTri = -1;
        float bu = 0;
        float bv = 0;
        for (std::uint32_t t = 0; t < b.triangleCount; ++t) {
            if (!m_tri[t].valid) continue;
            const float* p0 = &m_world[static_cast<std::size_t>(b.triangle[t * 3]) * 3];
            const float* p1 = &m_world[static_cast<std::size_t>(b.triangle[t * 3 + 1]) * 3];
            const float* p2 = &m_world[static_cast<std::size_t>(b.triangle[t * 3 + 2]) * 3];
            const float e1[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
            const float e2[3] = { p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2] };
            const float h[3] = { d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0] };
            const float det = e1[0] * h[0] + e1[1] * h[1] + e1[2] * h[2];
            if (std::fabs(det) < 1e-9f) continue;
            const float inv = 1.0f / det;
            const float sv[3] = { from[0] - p0[0], from[1] - p0[1], from[2] - p0[2] };
            const float u = inv * (sv[0] * h[0] + sv[1] * h[1] + sv[2] * h[2]);
            if (u < 0.0f || u > 1.0f) continue;
            const float q[3] = { sv[1] * e1[2] - sv[2] * e1[1], sv[2] * e1[0] - sv[0] * e1[2], sv[0] * e1[1] - sv[1] * e1[0] };
            const float v = inv * (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]);
            if (v < 0.0f || u + v > 1.0f) continue;
            const float dist = inv * (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]);
            if (dist < 0.0f || dist >= best) continue;
            best = dist;
            bestTri = static_cast<std::int32_t>(t);
            bu = u;
            bv = v;
        }
        if (bestTri < 0) return false;
        const auto t = static_cast<std::uint32_t>(bestTri);
        const std::uint32_t id[3] = { b.triangle[t * 3], b.triangle[t * 3 + 1], b.triangle[t * 3 + 2] };
        const float* p0 = &m_world[static_cast<std::size_t>(id[0]) * 3];
        const float* p1 = &m_world[static_cast<std::size_t>(id[1]) * 3];
        const float* p2 = &m_world[static_cast<std::size_t>(id[2]) * 3];
        const float e1[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
        const float e2[3] = { p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2] };
        float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
        const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (nl < 1e-12f) return false;
        for (float& c : n) c /= nl;
        if (n[0] * d[0] + n[1] * d[1] + n[2] * d[2] > 0.0f) {
            for (float& c : n) c = -c;  // face the ray
        }
        const float size = static_cast<float>(kSize);
        float uv[3][2];
        for (int k = 0; k < 3; ++k) {
            uv[k][0] = b.uv[static_cast<std::size_t>(id[k]) * 2] * size;
            uv[k][1] = b.uv[static_cast<std::size_t>(id[k]) * 2 + 1] * size;
        }
        out->x = uv[0][0] + bu * (uv[1][0] - uv[0][0]) + bv * (uv[2][0] - uv[0][0]);
        out->y = uv[0][1] + bu * (uv[1][1] - uv[0][1]) + bv * (uv[2][1] - uv[0][1]);
        out->dist = best;
        for (int k = 0; k < 3; ++k) {
            out->point[k] = from[k] + d[k] * best;
            out->normal[k] = n[k];
        }
        // the ray's tangential part, expressed in the atlas: tg = a e1 + c e2 (least squares in the triangle plane), then a (uv1 - uv0) + c (uv2 - uv0)
        const float dn = d[0] * n[0] + d[1] * n[1] + d[2] * n[2];
        const float tg[3] = { d[0] - dn * n[0], d[1] - dn * n[1], d[2] - dn * n[2] };
        out->slant = std::sqrt(tg[0] * tg[0] + tg[1] * tg[1] + tg[2] * tg[2]);
        const float g11 = e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2];
        const float g12 = e1[0] * e2[0] + e1[1] * e2[1] + e1[2] * e2[2];
        const float g22 = e2[0] * e2[0] + e2[1] * e2[1] + e2[2] * e2[2];
        const float r1 = tg[0] * e1[0] + tg[1] * e1[1] + tg[2] * e1[2];
        const float r2 = tg[0] * e2[0] + tg[1] * e2[1] + tg[2] * e2[2];
        const float gd = g11 * g22 - g12 * g12;
        out->tx = 0.0f;
        out->ty = 1.0f;
        if (std::fabs(gd) > 1e-12f) {
            const float a = (r1 * g22 - r2 * g12) / gd;
            const float c = (r2 * g11 - r1 * g12) / gd;
            const float ux = a * (uv[1][0] - uv[0][0]) + c * (uv[2][0] - uv[0][0]);
            const float uy = a * (uv[1][1] - uv[0][1]) + c * (uv[2][1] - uv[0][1]);
            const float ul = std::sqrt(ux * ux + uy * uy);
            if (ul > 1e-9f) {
                out->tx = ux / ul;
                out->ty = uy / ul;
            }
        }
        return true;
    }

    void Instance::Update()
    {
        if (!Alive()) return;
        m_worldValid = false;
        const auto& b = *m_body;
#ifdef DRIPSIM_PREVIEW
        for (std::uint32_t i = 0; i < b.boneCount; ++i) {  // standing: every bone sees the world down as (0,0,-1) in skin space
            m_boneG[static_cast<std::size_t>(i) * 3] = 0.0f;
            m_boneG[static_cast<std::size_t>(i) * 3 + 1] = 0.0f;
            m_boneG[static_cast<std::size_t>(i) * 3 + 2] = -1.0f;
        }
#else
        auto* skin = b.shape->GetGeometryRuntimeData().skinInstance.get();
        if (!skin || b.boneRefs.size() < b.boneCount) return;
        for (std::uint32_t i = 0; i < b.boneCount; ++i) {
            auto* bone = b.boneRefs[i].get();
            float g[3] = { 0, 0, -1 };
            if (bone) {
                const auto& w = bone->world.rotate.entry;
                const float x[3] = { -w[2][0], -w[2][1], -w[2][2] };  // W^T (0,0,-1)
                const float* r = &b.skinToBoneRotation[static_cast<std::size_t>(i) * 9];
                for (int m = 0; m < 3; ++m) g[m] = r[0 * 3 + m] * x[0] + r[1 * 3 + m] * x[1] + r[2 * 3 + m] * x[2];  // S^T x
            }
            std::memcpy(&m_boneG[static_cast<std::size_t>(i) * 3], g, sizeof g);
        }
        bool logBones = false;
        static bool s_loggedOnce[DS::Surface::Count] = {};
        if (!m_loggedBones) {  // each bone's world-down in skin space (an upright body part should be near (0,0,-1)): once per build, and only once per game session
            m_loggedBones = true;
            if (b.surface >= 0 && b.surface < DS::Surface::Count && !s_loggedOnce[b.surface]) {
                s_loggedOnce[b.surface] = true;
                logBones = true;
            }
        }
        if (logBones) {
            std::string text;
            for (std::uint32_t i = 0; i < b.boneCount && i < 24; ++i) {
                auto* bone = b.boneRefs[i].get();
                text += std::format("\n    [{}] {} ({:.2f}, {:.2f}, {:.2f})", i, bone && bone->name.c_str() ? bone->name.c_str() : "?", m_boneG[i * 3], m_boneG[i * 3 + 1], m_boneG[i * 3 + 2]);
            }
            logger::info("Field {}: bone gravity (world down in skin space):{}", DS::Surface::Name(b.surface), text);
        }
#endif
        for (std::uint32_t t = 0; t < b.triangleCount; ++t) {
            auto& live = m_live[t];
            const auto& td = m_tri[t];
            live.flat = 1;
            live.dx = live.dy = live.steep = live.up = 0.0f;
            if (!td.valid) continue;
            float g[3] = { 0, 0, 0 };
            for (int k = 0; k < 3; ++k) {
                const std::size_t v = b.triangle[t * 3 + static_cast<std::uint32_t>(k)];
                for (int m = 0; m < 4; ++m) {
                    const float w = b.weight[v * 4 + static_cast<std::size_t>(m)];
                    if (w <= 0.0f) continue;
                    const std::size_t bi = b.boneIndex[v * 4 + static_cast<std::size_t>(m)];
                    if (bi >= b.boneCount) continue;
                    for (int c = 0; c < 3; ++c) g[c] += w * m_boneG[bi * 3 + static_cast<std::size_t>(c)];
                }
            }
            const float gl = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
            if (gl < 1e-6f) continue;
            for (float& c : g) c /= gl;
            const float dot = g[0] * td.n[0] + g[1] * td.n[1] + g[2] * td.n[2];
            live.up = std::clamp(-dot, 0.0f, 1.0f);
            const float gt[3] = { g[0] - dot * td.n[0], g[1] - dot * td.n[1], g[2] - dot * td.n[2] };
            live.steep = std::sqrt(gt[0] * gt[0] + gt[1] * gt[1] + gt[2] * gt[2]);
            const float dx = td.a[0] * gt[0] + td.a[1] * gt[1] + td.a[2] * gt[2];
            const float dy = td.a[3] * gt[0] + td.a[4] * gt[1] + td.a[5] * gt[2];
            const float dl = std::sqrt(dx * dx + dy * dy);
            if (live.steep < kFlat || dl < 1e-12f) continue;
            live.flat = 0;
            live.dx = dx / dl;
            live.dy = dy / dl;
        }
    }

    std::int32_t Instance::TriAt(int x, int y) const
    {
        return m_ready && InRaster(x, y) ? m_triId[Index(x, y)] : -1;
    }

    std::int32_t Instance::SeamTwin(int x, int y) const
    {
        return m_ready && InRaster(x, y) && !m_twin.empty() ? m_twin[Index(x, y)] : -1;
    }

    bool Instance::Inner(int x, int y) const
    {
        return m_ready && InRaster(x, y) && m_inner[Index(x, y)];
    }

    bool Instance::Keep(int x, int y) const
    {
        return m_ready && InRaster(x, y) && m_keep[Index(x, y)];
    }

    Sample Instance::At(float x, float y) const
    {
        Sample out;
        if (!m_ready) return out;
        const int cx = static_cast<int>(std::lround(x));
        const int cy = static_cast<int>(std::lround(y));
        out.inner = Inner(cx, cy);
        static constexpr int offsets[5][2] = { { 0, 0 }, { 3, 0 }, { -3, 0 }, { 0, 3 }, { 0, -3 } };
        float sx = 0;
        float sy = 0;
        float steep = 0;
        float upSum = 0;
        int count = 0;
        for (const auto& o : offsets) {
            const auto id = TriAt(cx + o[0], cy + o[1]);
            if (id < 0) continue;
            const auto& live = m_live[static_cast<std::size_t>(id)];
            ++count;
            steep += live.steep;
            upSum += live.up;
            if (live.flat) continue;
            sx += live.dx * live.steep;
            sy += live.dy * live.steep;
            if (o[0] == 0 && o[1] == 0) out.mmpx = m_tri[static_cast<std::size_t>(id)].mmpx;
        }
        const auto centre = TriAt(cx, cy);
        if (centre >= 0) out.mmpx = m_tri[static_cast<std::size_t>(centre)].mmpx;
        if (count > 0) {
            out.steep = steep / static_cast<float>(count);
            out.up = upSum / static_cast<float>(count);
        }
        const float l = std::sqrt(sx * sx + sy * sy);
        if (l > 1e-9f) {
            out.dx = sx / l;
            out.dy = sy / l;
        }
        return out;
    }

    float Instance::CenterZ(std::int32_t tri) const
    {
        return tri >= 0 ? m_tri[static_cast<std::size_t>(tri)].cz : 0.0f;
    }

    float Instance::NormalY(std::int32_t tri) const
    {
        return tri >= 0 ? m_tri[static_cast<std::size_t>(tri)].ny : 0.0f;
    }

    float Instance::RestSteep(std::int32_t tri) const
    {
        return tri >= 0 ? m_tri[static_cast<std::size_t>(tri)].rest : 0.0f;
    }


    Instance& Get(int surface)
    {
        static std::array<Instance, DS::Surface::Count> instances;
        return instances[static_cast<std::size_t>(std::clamp(surface, 0, static_cast<int>(DS::Surface::Count) - 1))];
    }
}
