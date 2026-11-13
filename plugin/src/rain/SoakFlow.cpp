// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 AntiArc. This program is free software: see the LICENSE file (GNU GPL version 3 or later). It comes with NO WARRANTY.
#include "SoakFlow.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <string>

namespace
{
    constexpr float kDt = 0.05f;         // simulation step (s): 20 steps a second
    constexpr float kSpeed = 640.0f;     // mm/s for a sheet of thickness 1 on a vertical skin (a film flows with h^2): 0.25 -> 40 mm/s
    constexpr float kMaxMm = 160.0f;     // fastest flow (mm/s)
    constexpr float kMeander = 0.5f;     // radians: the most a stream turns away from straight downhill
    constexpr float kLevel = 0.06f;      // share of the difference to a thinner neighbour that levels out each step
    constexpr float kChangeAbs = 0.4f;   // ... or by more than this x 0.1 x drawn in thickness
    constexpr float kChange = 0.05f;    // a tile is sent to the film again when a cell changed by more than this (in h / (h + 0.1))
    constexpr int kSend = 8;             // cells a side of a block handed to the film (8 cells = 32 film pixels = one film tile)
    constexpr int kSends = SoakFlow::kN / kSend;
    constexpr float kPushEvery = 0.25f;   // seconds between two hand-overs to the film (each one re-renders the changed tiles)

    // smooth noise 0..1 (value noise on a coarse grid, bilinear): patches a few centimetres across, not pixel noise
    std::vector<float> Blobs(int n, int grid, std::mt19937& rng)
    {
        std::uniform_real_distribution<float> u(0.0f, 1.0f);
        std::vector<float> g(static_cast<std::size_t>((grid + 1) * (grid + 1)));
        for (auto& v : g) v = u(rng);
        std::vector<float> out(static_cast<std::size_t>(n) * n);
        const float scale = static_cast<float>(grid) / static_cast<float>(n);
        for (int y = 0; y < n; ++y) {
            const float fy = static_cast<float>(y) * scale;
            const int iy = std::min(static_cast<int>(fy), grid - 1);
            const float ty = fy - static_cast<float>(iy);
            for (int x = 0; x < n; ++x) {
                const float fx = static_cast<float>(x) * scale;
                const int ix = std::min(static_cast<int>(fx), grid - 1);
                const float tx = fx - static_cast<float>(ix);
                const auto at = [&](int a, int b) { return g[static_cast<std::size_t>(b * (grid + 1) + a)]; };
                const float top = at(ix, iy) + (at(ix + 1, iy) - at(ix, iy)) * tx;
                const float bottom = at(ix, iy + 1) + (at(ix + 1, iy + 1) - at(ix, iy + 1)) * tx;
                out[static_cast<std::size_t>(y) * n + x] = top + (bottom - top) * ty;
            }
        }
        return out;
    }
}

void SoakFlow::Reset()
{
    Wait();
    *this = SoakFlow{};
}

void SoakFlow::RefreshGravity(const DripSim::IField& field, bool all)
{
    for (int ty = 0; ty < kTiles; ++ty) {
        for (int tx = 0; tx < kTiles; ++tx) {
            if (!all && !m_moving[static_cast<std::size_t>(ty * kTiles + tx)]) continue;
            for (int y = ty * kTile; y < (ty + 1) * kTile; ++y) {
                for (int x = tx * kTile; x < (tx + 1) * kTile; ++x) {
                    const auto i = static_cast<std::size_t>(y) * kN + x;
                    if (!m_inner[i]) continue;
                    const int g = m_gsrc[i];  // near an island edge the field's smoothed direction is not reliable: the nearest cell away from the edge
                    DripSim::Sample s{};
                    field.At(static_cast<float>((g % kN) * m_cell) + 0.5f * static_cast<float>(m_cell), static_cast<float>((g / kN) * m_cell) + 0.5f * static_cast<float>(m_cell), &s);
                    m_gx[i] = s.dx;
                    m_gy[i] = s.dy;
                    m_steep[i] = s.steep;
                    m_mm[i] = std::max(s.mmpx * static_cast<float>(m_cell), 0.1f);
                }
            }
        }
    }
    m_gravAge = 0.0f;
}

void SoakFlow::Flood(const DripSim::IField& field, float zLine, float sheet, int streams, std::mt19937& rng)
{
    const auto cells = static_cast<std::size_t>(kN) * kN;
    if (m_h.empty()) {
        m_h.assign(cells, 0.0f);
        m_d.assign(cells, 0.0f);
        m_sent.assign(cells, 0.0f);
        m_gx.assign(cells, 0.0f);
        m_gy.assign(cells, 0.0f);
        m_steep.assign(cells, 0.0f);
        m_mm.assign(cells, 1.0f);
        m_inner.assign(cells, 0);
        m_gsrc.assign(cells, 0);
        m_twin.assign(cells, -1);
        m_wake.assign(static_cast<std::size_t>(kTiles) * kTiles, 0);
        m_active.assign(static_cast<std::size_t>(kTiles) * kTiles, 0);
        m_moving.assign(m_active.size(), 0);
        m_cell = std::max(1, field.Size() / kN);
        const auto blobs = Blobs(kN, 10, rng);
        m_keep.resize(cells);
        for (std::size_t i = 0; i < cells; ++i) m_keep[i] = 0.8f + 0.4f * blobs[i];  // (0.6..1.4 made a patchy left-behind film: in game every patch edge drew as crust)
        const auto turn = Blobs(kN, 12, rng);
        m_tc.resize(cells);
        m_ts.resize(cells);
        for (std::size_t i = 0; i < cells; ++i) {
            const float a = (turn[i] - 0.5f) * 2.0f * kMeander;
            m_tc[i] = std::cos(a);
            m_ts[i] = std::sin(a);
        }
        for (int y = 0; y < kN; ++y) {
            for (int x = 0; x < kN; ++x) m_inner[static_cast<std::size_t>(y) * kN + x] = field.TriAt(x * m_cell + m_cell / 2, y * m_cell + m_cell / 2) >= 0 ? 1 : 0;  // on the skin, up to the island edge (a 3 px margin left a dry line along every UV seam)
        }
        // across the UV seams (the framework's seam map): water that runs over an island edge goes on on the other side instead of dripping off
        int twins = 0;
        for (int y = 0; y < kN; ++y) {
            for (int x = 0; x < kN; ++x) {
                const auto i = static_cast<std::size_t>(y) * kN + x;
                if (!m_inner[i]) continue;
                const auto t = field.SeamTwin(x * m_cell + m_cell / 2, y * m_cell + m_cell / 2);
                if (t < 0) continue;
                const int fx = (t % field.Size()) / m_cell;
                const int fy = (t / field.Size()) / m_cell;
                if (fx < 0 || fy < 0 || fx >= kN || fy >= kN) continue;
                const auto j = static_cast<std::size_t>(fy) * kN + fx;
                if (j == i || !m_inner[j]) continue;
                m_twin[i] = static_cast<int>(j);
                ++twins;
            }
        }
        m_twins = twins;
        std::vector<std::uint8_t> deep(cells, 0);
        for (int y = 0; y < kN; ++y) {
            for (int x = 0; x < kN; ++x) deep[static_cast<std::size_t>(y) * kN + x] = field.Inner(x * m_cell + m_cell / 2, y * m_cell + m_cell / 2) ? 1 : 0;
        }
        for (int y = 0; y < kN; ++y) {
            for (int x = 0; x < kN; ++x) {
                const auto i = static_cast<std::size_t>(y) * kN + x;
                m_gsrc[i] = static_cast<int>(i);
                if (!m_inner[i] || deep[i]) continue;
                int best = 1 << 30;
                for (int oy = -4; oy <= 4; ++oy) {
                    for (int ox = -4; ox <= 4; ++ox) {
                        const int sx = x + ox;
                        const int sy = y + oy;
                        if (sx < 0 || sy < 0 || sx >= kN || sy >= kN) continue;
                        const auto j = static_cast<std::size_t>(sy) * kN + sx;
                        if (!deep[j] || ox * ox + oy * oy >= best) continue;
                        best = ox * ox + oy * oy;
                        m_gsrc[i] = static_cast<int>(j);
                    }
                }
            }
        }
    }
    // the sheet: even, a little thicker and thinner in broad patches (so it does not drain as one block)
    const auto vary = Blobs(kN, 16, rng);
    float zTop = -1.0e9f;
    std::vector<int> wet;
    for (int y = 0; y < kN; ++y) {
        for (int x = 0; x < kN; ++x) {
            const auto i = static_cast<std::size_t>(y) * kN + x;
            if (!m_inner[i]) continue;
            const auto tri = field.TriAt(x * m_cell + m_cell / 2, y * m_cell + m_cell / 2);
            if (tri < 0) continue;
            const float z = field.CenterZ(tri);
            if (z >= zLine) continue;
            m_h[i] = std::max(m_h[i], sheet * (0.7f + 0.6f * vary[i]));
            wet.push_back(static_cast<int>(i));
            zTop = std::max(zTop, z);
            m_active[static_cast<std::size_t>((y / kTile) * kTiles + x / kTile)] = 1;
        }
    }
    // stream sources: random cells in the top part of the wet skin (the neck, the shoulders)
    m_sources.clear();
    if (!wet.empty() && streams > 0) {
        const float span = std::max(zTop - field.ZMin(), 1.0f);
        std::vector<int> top;
        for (const int i : wet) {
            const auto tri = field.TriAt((i % kN) * m_cell + m_cell / 2, (i / kN) * m_cell + m_cell / 2);
            if (tri >= 0 && field.CenterZ(tri) > zTop - 0.12f * span) top.push_back(i);
        }
        std::ranges::shuffle(top, rng);
        for (int k = 0; k < streams && k < static_cast<int>(top.size()); ++k) m_sources.push_back(top[static_cast<std::size_t>(k)]);
    }
    m_used = !wet.empty();
    RefreshGravity(field, true);
}

void SoakFlow::Tick(float dt, const FlowParams& p, float source)
{
    const float dry = p.film / std::max(p.dryTime, 1.0f) * dt;  // a resting film is gone after dryTime
    const float pin = p.pin;
    // the stream sources: a little water each step, more at the start of the soak
    for (const int i : m_sources) m_d[static_cast<std::size_t>(i)] += 0.9f * p.streamRate * source * dt;
    std::ranges::fill(m_moving, std::uint8_t{ 0 });

    // surface tension levels moving water: one exchange for each pair of neighbours (right, down), symmetric, so it neither makes nor loses water
    for (int ty = 0; ty < kTiles; ++ty) {
        for (int tx = 0; tx < kTiles; ++tx) {
            if (!m_active[static_cast<std::size_t>(ty * kTiles + tx)]) continue;
            for (int y = ty * kTile; y < (ty + 1) * kTile; ++y) {
                for (int x = tx * kTile; x < (tx + 1) * kTile; ++x) {
                    const auto i = static_cast<std::size_t>(y) * kN + x;
                    if (!m_inner[i]) continue;
                    const float h = m_h[i];
                    const int ox[2] = { x + 1, x };
                    const int oy[2] = { y, y + 1 };
                    for (int k = 0; k < 2; ++k) {
                        if (ox[k] >= kN || oy[k] >= kN) continue;
                        const auto j = static_cast<std::size_t>(oy[k]) * kN + ox[k];
                        if (!m_inner[j]) continue;
                        const float hj = m_h[j];
                        if (std::max(h, hj) < pin * 0.8f) continue;  // resting film: it stays where it is
                        const float level = kLevel * (h - hj);
                        m_d[i] -= level;
                        m_d[j] += level;
                    }
                }
            }
        }
    }

    for (int ty = 0; ty < kTiles; ++ty) {
        for (int tx = 0; tx < kTiles; ++tx) {
            if (!m_active[static_cast<std::size_t>(ty * kTiles + tx)]) continue;
            for (int y = ty * kTile; y < (ty + 1) * kTile; ++y) {
                for (int x = tx * kTile; x < (tx + 1) * kTile; ++x) {
                    const auto i = static_cast<std::size_t>(y) * kN + x;
                    const float h = m_h[i];
                    if (h <= 0.0f) continue;
                    if (!m_inner[i]) {  // past an island edge: it runs off
                        m_d[i] -= h;
                        continue;
                    }
                    m_d[i] -= std::min(dry, h);
                    const float steep = m_steep[i];
                    if (h < pin * m_keep[i] || steep < 0.04f) continue;  // too thin to move, or flat skin: it rests and dries
                    m_moving[static_cast<std::size_t>(ty * kTiles + tx)] = 1;
                    const float keep = std::min(h, p.film);  // an even film behind the water
                    const float moving = (h - keep) * (1.0f - 4.0f * kLevel);  // (room for the levelling below: a cell never gives more than it has)
                    // sideways: towards the thicker neighbour across the slope (water gathers into streams)
                    // the downhill direction, turned a little by a fixed noise (streams wander; on a groove they would run as one ruler-straight line)
                    const float gx = m_gx[i] * m_tc[i] - m_gy[i] * m_ts[i];
                    const float gy = m_gx[i] * m_ts[i] + m_gy[i] * m_tc[i];
                    const float px = -gy;
                    const float py = gx;
                    const auto hAt = [&](float ox, float oy) {
                        const int sx = std::clamp(x + static_cast<int>(std::lround(ox)), 0, kN - 1);
                        const int sy = std::clamp(y + static_cast<int>(std::lround(oy)), 0, kN - 1);
                        return m_h[static_cast<std::size_t>(sy) * kN + sx];
                    };
                    const float side = (hAt(px * 1.5f, py * 1.5f) - hAt(-px * 1.5f, -py * 1.5f)) / (h + 0.05f);
                    const float pull = std::clamp(0.6f * p.gather * side, -0.8f, 0.8f);
                    float dx = gx + px * pull;
                    float dy = gy + py * pull;
                    const float len = std::max(std::hypot(dx, dy), 1e-4f);
                    dx /= len;
                    dy /= len;
                    // the speed of the levelled thickness (neighbours move together: the front does not break into dashes)
                    float hs = 0.5f * h;
                    const int nx[4] = { x - 1, x + 1, x, x };
                    const int ny[4] = { y, y, y - 1, y + 1 };
                    for (int k = 0; k < 4; ++k) {
                        const int sx = std::clamp(nx[k], 0, kN - 1);
                        const int sy = std::clamp(ny[k], 0, kN - 1);
                        const auto j = static_cast<std::size_t>(sy) * kN + sx;
                        hs += 0.125f * (m_inner[j] ? m_h[j] : h);
                    }
                    const float mm = std::min(kSpeed * p.speed * steep * hs * hs, kMaxMm * p.speed);  // (the speed of the levelled thickness: neighbours move together)
                    const float cells = std::min(mm * dt / m_mm[i], 0.95f);
                    // the moving water lands at the displaced position, shared over the 4 cells around it (bilinear)
                    const float fx = static_cast<float>(x) + dx * cells;
                    const float fy = static_cast<float>(y) + dy * cells;
                    const int x0 = static_cast<int>(std::floor(fx));
                    const int y0 = static_cast<int>(std::floor(fy));
                    const float ax = fx - static_cast<float>(x0);
                    const float ay = fy - static_cast<float>(y0);
                    m_d[i] -= moving;
                    const float w[4] = { (1 - ax) * (1 - ay), ax * (1 - ay), (1 - ax) * ay, ax * ay };
                    const int cx[4] = { x0, x0 + 1, x0, x0 + 1 };
                    const int cy[4] = { y0, y0, y0 + 1, y0 + 1 };
                    for (int k = 0; k < 4; ++k) {
                        if (cx[k] < 0 || cy[k] < 0 || cx[k] >= kN || cy[k] >= kN) continue;  // off the atlas: gone
                        const auto j = static_cast<std::size_t>(cy[k]) * kN + cx[k];
                        if (!m_inner[j]) {  // over an island edge: on across the seam, or (an open edge) it drips off
                            const int tw = m_twin[i];
                            if (tw >= 0) {
                                m_d[static_cast<std::size_t>(tw)] += moving * w[k];
                                m_wake[static_cast<std::size_t>(((tw / kN) / kTile) * kTiles + (tw % kN) / kTile)] = 1;
                            }
                            continue;
                        }
                        m_d[j] += moving * w[k];
                    }
                }
            }
        }
    }
    // apply: the active tiles and their neighbours (water can move one cell into the next tile)
    std::vector<std::uint8_t> touch(m_active.size(), 0);
    for (int ty = 0; ty < kTiles; ++ty) {
        for (int tx = 0; tx < kTiles; ++tx) {
            if (!m_active[static_cast<std::size_t>(ty * kTiles + tx)]) continue;
            for (int oy = -1; oy <= 1; ++oy) {
                for (int ox = -1; ox <= 1; ++ox) {
                    const int nx = tx + ox;
                    const int ny = ty + oy;
                    if (nx >= 0 && ny >= 0 && nx < kTiles && ny < kTiles) touch[static_cast<std::size_t>(ny * kTiles + nx)] = 1;
                }
            }
        }
    }
    for (const int i : m_sources) touch[static_cast<std::size_t>(((i / kN) / kTile) * kTiles + (i % kN) / kTile)] = 1;
    for (std::size_t t = 0; t < m_wake.size(); ++t) {  // tiles that got water across a seam
        if (m_wake[t]) touch[t] = 1;
        m_wake[t] = 0;
    }
    m_total = 0.0f;
    for (int ty = 0; ty < kTiles; ++ty) {
        for (int tx = 0; tx < kTiles; ++tx) {
            const auto t = static_cast<std::size_t>(ty * kTiles + tx);
            if (!touch[t]) continue;
            bool any = false;
            for (int y = ty * kTile; y < (ty + 1) * kTile; ++y) {
                for (int x = tx * kTile; x < (tx + 1) * kTile; ++x) {
                    const auto i = static_cast<std::size_t>(y) * kN + x;
                    float h = m_h[i] + m_d[i];
                    m_d[i] = 0.0f;
                    if (h < 1e-4f) h = 0.0f;
                    m_h[i] = h;
                    m_total += h;
                    any = any || h > 0.0f;
                }
            }
            m_active[t] = any ? 1 : 0;
        }
    }
}

// Main thread. The steps run on a worker thread: while one runs, the main thread does not touch the maps; between two runs it hands the result to the film,
// refreshes the downhill direction from the field (the field is the framework's, read on the main thread only) and starts the next run.
void SoakFlow::Step(const DripSim::IField& field, DripSim::ILayer& layer, float dt, const FlowParams& p, float source, float drawn)
{
    if (!m_used) return;
    const auto t0 = std::chrono::steady_clock::now();
    m_acc = std::min(m_acc + dt, 4.0f * kDt);
    m_pushAcc += dt;
    m_gravAge += dt;
    if (m_job.valid()) {
        if (m_job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            m_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            return;  // still working: next frame
        }
        m_job.get();
    }
    m_used = std::ranges::any_of(m_active, [](std::uint8_t a) { return a != 0; }) || (!m_sources.empty() && source > 0.0f);
    Push(layer, drawn);
    if (!m_used) return;
    if (m_gravAge >= 0.4f) RefreshGravity(field, false);  // the pose changes: the downhill direction follows it 2.5 times a second (where water moves)
    int ticks = 0;
    while (m_acc >= kDt && ticks < 3) {
        m_acc -= kDt;
        ++ticks;
    }
    if (ticks > 0) {
        const float src = std::clamp(source, 0.0f, 1.0f);
        const float seconds = static_cast<float>(ticks) * kDt;
        const bool build = m_pushAcc + seconds >= kPushEvery && !m_outReady;
        m_job = std::async(std::launch::async, [this, p, src, ticks, seconds, build, drawn] {
            const auto w0 = std::chrono::steady_clock::now();
            for (int k = 0; k < ticks; ++k) Tick(kDt, p, src);
            if (build) BuildOut(drawn);
            Dump(seconds);
            m_jobMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w0).count();
        });
    }
    m_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// (both sides have no running step: see operator=)
void SoakFlow::Assign(SoakFlow&& o)
{
    m_job = std::move(o.m_job);
    m_jobMs = std::move(o.m_jobMs);
    m_h = std::move(o.m_h);
    m_d = std::move(o.m_d);
    m_sent = std::move(o.m_sent);
    m_out = std::move(o.m_out);
    m_tmp = std::move(o.m_tmp);
    m_outReady = o.m_outReady;
    m_gx = std::move(o.m_gx);
    m_gy = std::move(o.m_gy);
    m_steep = std::move(o.m_steep);
    m_mm = std::move(o.m_mm);
    m_inner = std::move(o.m_inner);
    m_gsrc = std::move(o.m_gsrc);
    m_twin = std::move(o.m_twin);
    m_wake = std::move(o.m_wake);
    m_twins = o.m_twins;
    m_tc = std::move(o.m_tc);
    m_ts = std::move(o.m_ts);
    m_keep = std::move(o.m_keep);
    m_sources = std::move(o.m_sources);
    m_active = std::move(o.m_active);
    m_moving = std::move(o.m_moving);
    m_lastDt = std::move(o.m_lastDt);
    m_pushAcc = std::move(o.m_pushAcc);
    m_acc = std::move(o.m_acc);
    m_gravAge = std::move(o.m_gravAge);
    m_total = std::move(o.m_total);
    m_ms = std::move(o.m_ms);
    m_used = std::move(o.m_used);
    m_layerHasFlow = std::move(o.m_layerHasFlow);
    m_cell = std::move(o.m_cell);
    m_dumpAcc = std::move(o.m_dumpAcc);
    m_dumpIndex = std::move(o.m_dumpIndex);
}

void SoakFlow::Wait()
{
    if (m_job.valid()) m_job.wait();
}

void SoakFlow::Dump(float dt)
{
    // development only: DRIPSIM_FLOW_DUMP=<folder> writes the thickness map once a second as a PGM (0..0.5 -> black..white), for the preview
    static const std::string dumpDir = [] {
        char* value = nullptr;
        std::size_t length = 0;
        std::string out;
        if (_dupenv_s(&value, &length, "DRIPSIM_FLOW_DUMP") == 0 && value) out = value;
        std::free(value);
        return out;
    }();
    if (!dumpDir.empty() && m_cell > 0) {
        m_dumpAcc += dt;
        if (m_dumpAcc >= 1.0f) {
            m_dumpAcc -= 1.0f;
            ++m_dumpIndex;
            const std::string path = dumpDir + "/flow_" + std::to_string(m_dumpIndex) + ".pgm";
            FILE* f = nullptr;
            if (fopen_s(&f, path.c_str(), "wb") == 0 && f) {
                std::fprintf(f, "P5\n%d %d\n255\n", kN, kN);
                std::vector<unsigned char> row(kN);
                for (int y = 0; y < kN; ++y) {
                    for (int x = 0; x < kN; ++x) {
                        const auto i = static_cast<std::size_t>(y) * kN + x;
                        row[static_cast<std::size_t>(x)] = !m_inner[i] ? 0 : static_cast<unsigned char>(std::clamp(30.0f + m_h[i] * 450.0f, 0.0f, 255.0f));
                    }
                    std::fwrite(row.data(), 1, row.size(), f);
                }
                std::fclose(f);
            }
        }
    }
}

void SoakFlow::Push(DripSim::ILayer& layer, float drawn)
{
    if (m_h.empty()) return;
    if (!m_used) {
        if (m_layerHasFlow) {
            layer.SetFlow(nullptr, kN, nullptr, kSends);
            m_layerHasFlow = false;
            std::ranges::fill(m_sent, 0.0f);
        }
        return;
    }
    if (!m_outReady) return;  // (the worker builds the smoothed map, BuildOut)
    m_outReady = false;
    m_pushAcc = 0.0f;
    Send(layer, drawn);
}

// Worker: the map as the film should draw it (x drawn, smoothed, carried past the island edges) into m_out.
void SoakFlow::BuildOut(float drawn)
{
    if (m_out.size() != m_h.size()) m_out.assign(m_h.size(), 0.0f);
    if (m_tmp.size() != m_h.size()) m_tmp.assign(m_h.size(), 0.0f);
    for (std::size_t i = 0; i < m_h.size(); ++i) m_out[i] = m_inner[i] ? m_h[i] * drawn : 0.0f;
    // smooth (a 3 x 3 binomial over the skin only, twice): the film draws the map as a sheen and only upsamples it; a hard cell edge showed as crust
    for (int round = 0; round < 2; ++round) {
        for (int y = 0; y < kN; ++y) {
            for (int x = 0; x < kN; ++x) {
                const auto i = static_cast<std::size_t>(y) * kN + x;
                if (!m_inner[i]) {
                    m_tmp[i] = 0.0f;
                    continue;
                }
                float sum = 0.0f;
                float ws = 0.0f;
                for (int oy = -1; oy <= 1; ++oy) {
                    const int sy = y + oy;
                    if (sy < 0 || sy >= kN) continue;
                    for (int ox = -1; ox <= 1; ++ox) {
                        const int sx = x + ox;
                        if (sx < 0 || sx >= kN) continue;
                        const auto j = static_cast<std::size_t>(sy) * kN + sx;
                        if (!m_inner[j]) continue;
                        const float wgt = (ox == 0 ? 2.0f : 1.0f) * (oy == 0 ? 2.0f : 1.0f);
                        sum += wgt * m_out[j];
                        ws += wgt;
                    }
                }
                m_tmp[i] = sum / ws;
            }
        }
        m_out.swap(m_tmp);
    }
    // past the island edges: the thickness of the nearest skin, 3 cells out (the film blends cells bilinearly and pads past the edge: a drop to 0 there drew a line along every seam)
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < kN; ++y) {
            for (int x = 0; x < kN; ++x) {
                const auto i = static_cast<std::size_t>(y) * kN + x;
                if (m_inner[i] || m_out[i] > 0.0f) continue;
                float sum = 0.0f;
                int n = 0;
                const int nx[4] = { x - 1, x + 1, x, x };
                const int ny[4] = { y, y, y - 1, y + 1 };
                for (int k = 0; k < 4; ++k) {
                    if (nx[k] < 0 || ny[k] < 0 || nx[k] >= kN || ny[k] >= kN) continue;
                    const float v = m_out[static_cast<std::size_t>(ny[k]) * kN + nx[k]];
                    if (v > 0.0f) {
                        sum += v;
                        ++n;
                    }
                }
                if (n > 0) m_out[i] = -sum / static_cast<float>(n);  // (negative: filled in this pass, not a source for it)
            }
        }
        for (auto& v : m_out) v = std::fabs(v);
    }
    m_outReady = true;
}

// Main thread: the blocks of m_out that changed as the film sees them go to the layer.
void SoakFlow::Send(DripSim::ILayer& layer, float drawn)
{
    // only the blocks that changed as the film sees them (the comparison is on the final map). A block next to a changed one also goes when it changed by a third
    // of that: blocks sent at different times leave a step at their edge, and the normal map showed it as a square (game test 2026-11-12, NormalStrength 3.1)
    std::uint8_t changed[kSends * kSends] = {};  // (blocks of 8 cells = one film tile: a 32-cell block re-rendered 36 film tiles)
    float score[kSends * kSends] = {};
    const float unit = 0.1f * std::max(drawn, 0.01f);
    for (int ty = 0; ty < kSends; ++ty) {
        for (int tx = 0; tx < kSends; ++tx) {
            float worst = 0.0f;
            for (int y = ty * kSend; y < (ty + 1) * kSend; ++y) {
                for (int x = tx * kSend; x < (tx + 1) * kSend; ++x) {
                    const auto i = static_cast<std::size_t>(y) * kN + x;
                    // the sheen saturates at a thin film: a change in a thin film counts much more than the same change in a thick stream; and in absolute terms
                    const float d = std::fabs(m_out[i] - m_sent[i]) / (kChangeAbs * unit) * kChange;
                    const float c = std::fabs(m_out[i] / (m_out[i] + unit) - m_sent[i] / (m_sent[i] + unit));
                    worst = std::max(worst, std::max(d, c));
                }
            }
            score[ty * kSends + tx] = worst;
        }
    }
    bool any = false;
    for (int ty = 0; ty < kSends; ++ty) {
        for (int tx = 0; tx < kSends; ++tx) {
            const float sc = score[ty * kSends + tx];
            bool c = sc > kChange;
            for (int oy = -1; oy <= 1 && !c && sc > kChange / 3.0f; ++oy) {
                for (int ox = -1; ox <= 1; ++ox) {
                    const int nx = tx + ox;
                    const int ny = ty + oy;
                    if (nx >= 0 && ny >= 0 && nx < kSends && ny < kSends && score[ny * kSends + nx] > kChange) {
                        c = true;
                        break;
                    }
                }
            }
            if (!c) continue;
            for (int y = ty * kSend; y < (ty + 1) * kSend; ++y) {
                std::copy_n(m_out.begin() + static_cast<std::ptrdiff_t>(y * kN + tx * kSend), kSend, m_sent.begin() + static_cast<std::ptrdiff_t>(y * kN + tx * kSend));
            }
            changed[ty * kSends + tx] = 1;
            any = true;
        }
    }
    if (!any) return;
    layer.SetFlow(m_sent.data(), kN, changed, kSends);
    m_layerHasFlow = true;
}
