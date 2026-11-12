// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 AntiArc. This program is free software: see the LICENSE file (GNU GPL version 3 or later). It comes with NO WARRANTY.
#include "Film.h"

#include "Field.h"

#include <execution>
#include <mutex>
#include <thread>


namespace
{
    constexpr int kN = DS::Film::kSize;
    constexpr int kTile = DS::Film::kTile;
    constexpr int kTiles = DS::Film::kTiles;
    constexpr int kSim = DS::Film::kSimSize;
    constexpr int kScale = DS::Film::kScale;
    constexpr float kScaleF = static_cast<float>(kScale);
    constexpr int kPad = 18;  // render region around a tile: 1 (blur) + 1 (sobel) + 12 (halo) + slack

    // shared noise maps: colour mottling, grain, and the variation of the rim along an edge
    std::vector<float> g_mott;
    std::vector<float> g_grain;
    std::vector<float> g_rimvar;
    // Runs fn(item) for every item on a few threads of below-normal priority (not the system pool with every core: the game's own threads come first). The calling
    // thread works too. The tile jobs are independent.
    template <class Fn>
    void ParallelTiles(const std::vector<int>& items, Fn&& fn)
    {
        const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
        const unsigned threads = std::clamp(hw / 3u, 2u, 6u);
        if (threads <= 1 || items.size() < 8) {
            for (const int t : items) fn(t);
            return;
        }
        std::atomic<std::size_t> next{ 0 };
        const auto work = [&] {
            for (std::size_t i = next.fetch_add(1); i < items.size(); i = next.fetch_add(1)) fn(items[i]);
        };
        std::vector<std::thread> helpers;
        helpers.reserve(threads - 1);
        for (unsigned k = 1; k < threads; ++k) {
            helpers.emplace_back([&] {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                work();
            });
        }
        work();
        for (auto& h : helpers) h.join();
    }

    bool g_normalsOn = false;
    std::atomic<std::uint64_t> g_tilesRendered{ 0 };

    struct Canvas
    {
        std::array<DS::Film::Layer, DS::Film::kMaxLayers> layers;
        std::array<bool, DS::Film::kMaxLayers> used{};
        std::vector<DS::Film::Layer*> order;  // acquired layers, bottom to top
        std::vector<std::uint32_t> final;
        std::vector<std::uint32_t> normal;  // the model-space normal map, when some layer wants one
        std::array<std::uint8_t, DS::Film::kTiles * DS::Film::kTiles> changed{};  // tiles of `final` changed by the last Render
        bool recomposite = true;
    };
    std::array<Canvas, DS::Surface::Count> g_canvas;

    Canvas& CanvasOf(int surface)
    {
        return g_canvas[static_cast<std::size_t>(std::clamp(surface, 0, static_cast<int>(DS::Surface::Count) - 1))];
    }

    std::size_t Index(int x, int y)
    {
        return static_cast<std::size_t>(y) * kN + static_cast<std::size_t>(x);
    }

    std::size_t NoiseIndex(int x, int y)
    {
        return static_cast<std::size_t>(y) * kSim + static_cast<std::size_t>(x);
    }

    // the surface's draw mask lives at the sim resolution
    std::size_t KeepIndex(int x, int y)
    {
        return static_cast<std::size_t>(y / kScale) * kSim + static_cast<std::size_t>(x / kScale);
    }

    // Separable box blur (3 passes ~ gaussian) of a white-noise image, normalised to unit standard deviation.
    std::vector<float> SmoothNoise(std::mt19937& rng, float sigma)
    {
        std::normal_distribution<float> normal(0.0f, 1.0f);
        std::vector<float> image(static_cast<std::size_t>(kSim) * kSim);
        for (auto& v : image) v = normal(rng);
        const int width = std::max(1, static_cast<int>(std::lround(std::sqrt(4.0f * sigma * sigma + 1.0f))));
        const int radius = width / 2;
        std::vector<float> tmp(image.size());
        for (int pass = 0; pass < 3; ++pass) {
            for (int y = 0; y < kSim; ++y) {
                float sum = 0;
                for (int x = -radius; x <= radius; ++x) sum += image[NoiseIndex(std::clamp(x, 0, kSim - 1), y)];
                for (int x = 0; x < kSim; ++x) {
                    tmp[NoiseIndex(x, y)] = sum / static_cast<float>(2 * radius + 1);
                    sum += image[NoiseIndex(std::min(x + radius + 1, kSim - 1), y)] - image[NoiseIndex(std::max(x - radius, 0), y)];
                }
            }
            for (int x = 0; x < kSim; ++x) {
                float sum = 0;
                for (int y = -radius; y <= radius; ++y) sum += tmp[NoiseIndex(x, std::clamp(y, 0, kSim - 1))];
                for (int y = 0; y < kSim; ++y) {
                    image[NoiseIndex(x, y)] = sum / static_cast<float>(2 * radius + 1);
                    sum += tmp[NoiseIndex(x, std::min(y + radius + 1, kSim - 1))] - tmp[NoiseIndex(x, std::max(y - radius, 0))];
                }
            }
        }
        double mean = 0;
        for (const float v : image) mean += v;
        mean /= static_cast<double>(image.size());
        double var = 0;
        for (const float v : image) var += (v - mean) * (v - mean);
        const float sd = static_cast<float>(std::sqrt(var / static_cast<double>(image.size()))) + 1e-6f;
        for (auto& v : image) v = static_cast<float>((v - mean) / sd);
        return image;
    }

    void EnsureShared()
    {
        if (!g_mott.empty()) return;
        std::mt19937 rng(12345u);
        g_mott = SmoothNoise(rng, 7.0f);
        g_grain = SmoothNoise(rng, 1.3f);
        g_rimvar = SmoothNoise(rng, 5.0f);
    }
}

namespace DS::Film
{
    // ------------------------------------------------------------------ Layer
    void Layer::Reserve()
    {
        EnsureShared();
        if (m_perm.empty()) {
            m_perm.assign(static_cast<std::size_t>(kN) * kN, 0.0f);
            m_view.assign(m_perm.size(), 0.0f);
            m_pixels.assign(m_perm.size(), 0u);
            m_pristine = true;
        }
    }

    void Layer::Allocate(const std::uint8_t* keepMask)
    {
        Reserve();
        m_keep = keepMask;
        Clear();
        m_look = DripSim::DefaultLook();
        m_slope.clear();
        m_alpha = 1.0f;
        m_alphaChanged = true;
    }

    void Layer::Clear()
    {
        if (m_perm.empty()) return;
        if (!m_pristine) {  // (a layer that nothing was drawn into since it was allocated is already clear: 48 MB less to write)
            std::ranges::fill(m_perm, 0.0f);
            std::ranges::fill(m_view, 0.0f);
            std::ranges::fill(m_pixels, 0u);
            std::ranges::fill(m_slope, std::int16_t{ 0 });
            m_pristine = true;
        }
        m_dirty.fill(1);
        m_dynLast.fill(0);
        m_dynNow.fill(0);
    }

    void Layer::SetLook(const DripSim::Look* look)
    {
        if (!look || std::memcmp(&m_look, look, sizeof(DripSim::Look)) == 0) return;
        m_look = *look;
        m_dirty.fill(1);
    }

    void Layer::SetWetFilm(float belowZ, float amount)
    {
        amount = std::clamp(amount, 0.0f, 1.0f);
        // re-rendering the whole film is the expensive part: only on a real change (steps of 0.08 in strength, 2 units of water line), always at the ends
        const bool ends = (amount == 0.0f) != (m_wetAmount == 0.0f);
        if (!ends && std::fabs(amount - m_wetAmount) < 0.08f && std::fabs(belowZ - m_wetZ) < 2.0f) return;
        if (amount == 0.0f && m_wetAmount == 0.0f) return;
        m_wetAmount = amount;
        m_wetZ = belowZ;
        m_pristine = false;
        m_dirty.fill(1);
    }

    void Layer::SetAlpha(float alpha)
    {
        alpha = std::clamp(alpha, 0.0f, 1.0f);
        if (alpha == m_alpha) return;
        m_alpha = alpha;
        m_alphaChanged = true;
    }

    void Layer::MarkRect(int x0, int y0, int x1, int y1, int margin)
    {
        const int tx0 = std::max(0, (x0 - margin) / kTile);
        const int ty0 = std::max(0, (y0 - margin) / kTile);
        const int tx1 = std::min(kTiles - 1, (x1 + margin) / kTile);
        const int ty1 = std::min(kTiles - 1, (y1 + margin) / kTile);
        for (int ty = ty0; ty <= ty1; ++ty) {
            for (int tx = tx0; tx <= tx1; ++tx) m_dirty[static_cast<std::size_t>(ty * kTiles + tx)] = 1;
        }
    }

    bool Layer::WantsNormals() const
    {
        return g_normalsOn && m_look.normalStrength > 0.0f;
    }

    void SetNormalMapsEnabled(bool on)
    {
        g_normalsOn = on;
    }

    void Layer::BeginFrame()
    {
        if (WantsNormals() && m_slope.empty() && !m_perm.empty()) {  // (here, on the main thread: the tiles are rendered in parallel later)
            m_slope.assign(m_perm.size() * 2, 0);
            m_dirty.fill(1);
        }
        for (int ty = 0; ty < kTiles; ++ty) {
            for (int tx = 0; tx < kTiles; ++tx) {
                if (!m_dynLast[static_cast<std::size_t>(ty * kTiles + tx)]) continue;
                for (int y = ty * kTile; y < (ty + 1) * kTile; ++y) {
                    std::copy_n(m_perm.begin() + static_cast<std::ptrdiff_t>(Index(tx * kTile, y)), kTile, m_view.begin() + static_cast<std::ptrdiff_t>(Index(tx * kTile, y)));
                }
                MarkRect(tx * kTile, ty * kTile, (tx + 1) * kTile - 1, (ty + 1) * kTile - 1, 8);
            }
        }
        m_dynNow.fill(0);
    }

    void Layer::EndFrame()
    {
        m_dynLast = m_dynNow;
    }

    void Layer::Stamp(float x, float y, float rx, float ry, float angle, float height, float power, bool permanent)
    {
        if (m_perm.empty()) return;
        m_pristine = false;
        rx = std::max(rx, 0.5f) * kScaleF;
        ry = std::max(ry, 0.5f) * kScaleF;
        x *= kScaleF;
        y *= kScaleF;
        const float reach = std::max(rx, ry) + 1.5f;
        const int x0 = static_cast<int>(std::max(x - reach, 0.0f));
        const int x1 = static_cast<int>(std::min(x + reach + 1.0f, static_cast<float>(kN - 1)));
        const int y0 = static_cast<int>(std::max(y - reach, 0.0f));
        const int y1 = static_cast<int>(std::min(y + reach + 1.0f, static_cast<float>(kN - 1)));
        if (x1 < x0 || y1 < y0) return;
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        for (int py = y0; py <= y1; ++py) {
            for (int px = x0; px <= x1; ++px) {
                const float gx = static_cast<float>(px) + 0.5f - x;
                const float gy = static_cast<float>(py) + 0.5f - y;
                const float u = (gx * c + gy * s) / rx;
                const float v = (-gx * s + gy * c) / ry;
                const float q = 1.0f - (u * u + v * v);
                if (q <= 0.0f) continue;
                const float value = height * std::pow(q, power);
                const auto i = Index(px, py);
                if (permanent) m_perm[i] = std::max(m_perm[i], value);
                m_view[i] = std::max(m_view[i], value);
            }
        }
        MarkRect(x0, y0, x1, y1, 8);
        if (!permanent) {
            for (int ty = y0 / kTile; ty <= y1 / kTile; ++ty) {
                for (int tx = x0 / kTile; tx <= x1 / kTile; ++tx) m_dynNow[static_cast<std::size_t>(ty * kTiles + tx)] = 1;
            }
        }
    }

    void Layer::Erase(float x, float y, float rx, float ry, float angle, float strength)
    {
        if (m_perm.empty()) return;
        rx = std::max(rx, 0.5f) * kScaleF;
        ry = std::max(ry, 0.5f) * kScaleF;
        x *= kScaleF;
        y *= kScaleF;
        const float reach = std::max(rx, ry) + 1.5f;
        const int x0 = static_cast<int>(std::max(x - reach, 0.0f));
        const int x1 = static_cast<int>(std::min(x + reach + 1.0f, static_cast<float>(kN - 1)));
        const int y0 = static_cast<int>(std::max(y - reach, 0.0f));
        const int y1 = static_cast<int>(std::min(y + reach + 1.0f, static_cast<float>(kN - 1)));
        if (x1 < x0 || y1 < y0) return;
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        for (int py = y0; py <= y1; ++py) {
            for (int px = x0; px <= x1; ++px) {
                const float gx = static_cast<float>(px) + 0.5f - x;
                const float gy = static_cast<float>(py) + 0.5f - y;
                const float u = (gx * c + gy * s) / rx;
                const float v = (-gx * s + gy * c) / ry;
                const float q = 1.0f - (u * u + v * v);
                if (q <= 0.0f) continue;
                const float keep = 1.0f - std::clamp(std::sqrt(q) * 1.6f * strength, 0.0f, 1.0f);
                const auto i = Index(px, py);
                m_perm[i] *= keep;
                m_view[i] *= keep;
            }
        }
        MarkRect(x0, y0, x1, y1, 8);
    }

    // the fake light the film is shaded with: from the upper left of the atlas (x right, y down) and above the skin; h is its half vector with a viewer above the skin
    constexpr float kLightX = -0.55f * 0.62f;
    constexpr float kLightY = -0.83f * 0.62f;
    constexpr float kLightZ = 0.7867f;
    constexpr float kHalfLen = 0.529f;  // 1 / |light + (0,0,1)|
    constexpr float kHalfX = kLightX * kHalfLen;
    constexpr float kHalfY = kLightY * kHalfLen;
    constexpr float kHalfZ = (kLightZ + 1.0f) * kHalfLen;

    void Layer::RenderTile(int tx, int ty)
    {
        const auto& look = m_look;
        const int x0 = tx * kTile;
        const int y0 = ty * kTile;
        const int rx0 = std::max(0, x0 - kPad);
        const int ry0 = std::max(0, y0 - kPad);
        const int rx1 = std::min(kN, x0 + kTile + kPad);
        const int ry1 = std::min(kN, y0 + kTile + kPad);
        const int w = rx1 - rx0;
        const int h = ry1 - ry0;
        const auto at = [w](int x, int y) { return static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x); };
        thread_local std::vector<float> hw;    // blurred, masked thickness
        thread_local std::vector<float> tmp;
        thread_local std::vector<float> af;    // film alpha
        thread_local std::vector<float> rgbF;  // film colour (straight alpha)
        thread_local std::vector<float> blur;  // blurred film alpha, for the halo
        thread_local std::vector<float> slx;   // thickness slopes (for the normal map)
        thread_local std::vector<float> sly;
        const auto size = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
        for (auto* v : { &hw, &tmp, &af, &blur }) v->assign(size, 0.0f);
        rgbF.assign(size * 3, 0.0f);
        const bool wantNormals = !m_slope.empty() && WantsNormals();
        if (wantNormals) {
            slx.assign(size, 0.0f);
            sly.assign(size, 0.0f);
        }

        // Hw = blur(view) * keep   (gaussian sigma 0.7 ~ [0.2 0.6 0.2], separable). The view is copied once with a 1 px border, edge-clamped.
        thread_local std::vector<float> src;
        const int sw = w + 2;
        const int sh = h + 2;
        src.assign(static_cast<std::size_t>(sw) * static_cast<std::size_t>(sh), 0.0f);
        float peak = 0.0f;
        for (int y = 0; y < sh; ++y) {
            const int gy = std::clamp(ry0 + y - 1, 0, kN - 1);
            for (int x = 0; x < sw; ++x) {
                const float v = m_view[Index(std::clamp(rx0 + x - 1, 0, kN - 1), gy)];
                src[static_cast<std::size_t>(y) * static_cast<std::size_t>(sw) + static_cast<std::size_t>(x)] = v;
                peak = std::max(peak, v);
            }
        }
        const std::uint8_t* keep = m_keep;
        if (!keep) return;
        // The draw mask over the render region. A UV seam is not an edge of the liquid: everything below that compares or blurs neighbouring pixels
        // (slopes, rim, glint, smoothing, halo) only looks at pixels of the same skin, otherwise every island border shows as a bright outline.
        thread_local std::vector<float> km;
        km.assign(size, 0.0f);
        bool allSkin = true;  // the whole region is skin (no seam near this tile): the blurs can skip the mask weights
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const bool in = keep[KeepIndex(rx0 + x, ry0 + y)] != 0;
                km[at(x, y)] = in ? 1.0f : 0.0f;
                allSkin = allSkin && in;
            }
        }
        const auto byte = [](float value) { return static_cast<std::uint32_t>(std::clamp(value, 0.0f, 255.0f) + 0.5f); };
        if (peak <= 0.012f && m_wetAmount <= 0.0f) {  // no film anywhere near this tile: it is just transparent
            for (int gy = y0; gy < y0 + kTile; ++gy) std::fill_n(m_pixels.begin() + static_cast<std::ptrdiff_t>(Index(x0, gy)), kTile, 0u);
            if (!m_slope.empty()) {
                for (int gy = y0; gy < y0 + kTile; ++gy) std::fill_n(m_slope.begin() + static_cast<std::ptrdiff_t>(Index(x0, gy) * 2), kTile * 2, std::int16_t{ 0 });
            }
            return;
        }
        // horizontal pass over all src rows
        thread_local std::vector<float> hsrc;
        hsrc.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(sh), 0.0f);
        for (int y = 0; y < sh; ++y) {
            const float* row = &src[static_cast<std::size_t>(y) * static_cast<std::size_t>(sw)];
            float* out = &hsrc[static_cast<std::size_t>(y) * static_cast<std::size_t>(w)];
            for (int x = 0; x < w; ++x) out[x] = 0.2f * row[x] + 0.6f * row[x + 1] + 0.2f * row[x + 2];
        }
        for (int y = 0; y < h; ++y) {
            const float* up = &hsrc[static_cast<std::size_t>(y) * static_cast<std::size_t>(w)];
            const float* mid = up + w;
            const float* down = mid + w;
            for (int x = 0; x < w; ++x) {
                hw[at(x, y)] = (0.2f * up[x] + 0.6f * mid[x] + 0.2f * down[x]) * km[at(x, y)];
            }
        }

        // the wet film: a thin even film (with a little variation) over the skin below the water line
        if (m_wetAmount > 0.0f) {
            const auto& field = DS::Field::Get(m_surface);
            const auto& raster = field.PaddedTriRaster();  // (up to the island edges and a little past them: a film that stops short of a seam shows as a line)
            if (!raster.empty()) {
                for (int y = 0; y < h; ++y) {
                    for (int x = 0; x < w; ++x) {
                        const int gx = rx0 + x;
                        const int gy = ry0 + y;
                        const auto tri = raster[static_cast<std::size_t>(gy / kScale) * kSim + static_cast<std::size_t>(gx / kScale)];
                        if (tri < 0) continue;
                        const float soft = std::clamp((m_wetZ - field.CenterZ(tri)) / 8.0f + 0.5f, 0.0f, 1.0f);  // a soft water line
                        if (soft <= 0.0f) continue;
                        const float vary = std::clamp(0.85f + 0.15f * g_mott[NoiseIndex(gx / kScale, gy / kScale)], 0.5f, 1.0f);
                        hw[at(x, y)] = std::max(hw[at(x, y)], 0.05f * m_wetAmount * soft * vary);
                    }
                }
            }
        }

        // Separable blur of `img` over the skin only (normalised by the mask, so a seam does not pull the edge down toward zero); 0 off the skin.
        thread_local std::vector<float> wtmp;
        const auto maskedBlur = [&](std::vector<float>& img, const float* k, int radius, std::vector<float>& out) {
            if (allSkin) {  // plain separable blur, edge-clamped
                float total = 0;
                for (int i = -radius; i <= radius; ++i) total += k[i + radius];
                for (int y = 0; y < h; ++y) {
                    for (int x = 0; x < w; ++x) {
                        float sum = 0;
                        for (int i = -radius; i <= radius; ++i) sum += k[i + radius] * img[at(std::clamp(x + i, 0, w - 1), y)];
                        tmp[at(x, y)] = sum;
                    }
                }
                for (int y = 0; y < h; ++y) {
                    for (int x = 0; x < w; ++x) {
                        float sum = 0;
                        for (int i = -radius; i <= radius; ++i) sum += k[i + radius] * tmp[at(x, std::clamp(y + i, 0, h - 1))];
                        out[at(x, y)] = sum / (total * total);
                    }
                }
                return;
            }
            wtmp.assign(size, 0.0f);
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    float sum = 0;
                    float ws = 0;
                    for (int i = -radius; i <= radius; ++i) {
                        const auto j = at(std::clamp(x + i, 0, w - 1), y);
                        sum += k[i + radius] * img[j] * km[j];
                        ws += k[i + radius] * km[j];
                    }
                    tmp[at(x, y)] = sum;
                    wtmp[at(x, y)] = ws;
                }
            }
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    float sum = 0;
                    float ws = 0;
                    for (int i = -radius; i <= radius; ++i) {
                        const auto j = at(x, std::clamp(y + i, 0, h - 1));
                        sum += k[i + radius] * tmp[j];
                        ws += k[i + radius] * wtmp[j];
                    }
                    out[at(x, y)] = km[at(x, y)] > 0.0f && ws > 1e-6f ? sum / ws : 0.0f;
                }
            }
        };

        // optional extra smoothing (a 13-tap binomial blur, separable) blended in by look.smooth: lobes and stamps run together into one liquid surface
        if (look.smooth > 0.0f) {
            const float k[13] = { 1, 12, 66, 220, 495, 792, 924, 792, 495, 220, 66, 12, 1 };
            thread_local std::vector<float> sm;
            sm.assign(size, 0.0f);
            maskedBlur(hw, k, 6, sm);
            for (std::size_t i = 0; i < size; ++i) hw[i] = hw[i] * (1.0f - look.smooth) + sm[i] * look.smooth;
        }

        // a neighbour's thickness for slopes and curvature: off the skin (across a seam) it counts as the same as here, so seams are flat
        const auto hn = [&](int x, int y, float here) { return km[at(x, y)] > 0.0f ? hw[at(x, y)] : here; };

        const bool hasCore = look.coreColor[0] + look.coreColor[1] + look.coreColor[2] > 0.0f;

        // the film itself: translucent body + bright rim along edges and ridges + wet glints
        for (int y = 1; y < h - 1; ++y) {
            for (int x = 1; x < w - 1; ++x) {
                const float v = hw[at(x, y)];
                if (v <= 0.012f) continue;
                const float gxv = (hn(x + 1, y - 1, v) + 2 * hn(x + 1, y, v) + hn(x + 1, y + 1, v) - hn(x - 1, y - 1, v) - 2 * hn(x - 1, y, v) - hn(x - 1, y + 1, v)) / 8.0f * kScaleF;
                const float gyv = (hn(x - 1, y + 1, v) + 2 * hn(x, y + 1, v) + hn(x + 1, y + 1, v) - hn(x - 1, y - 1, v) - 2 * hn(x, y - 1, v) - hn(x + 1, y - 1, v)) / 8.0f * kScaleF;
                const float gm = std::sqrt(gxv * gxv + gyv * gyv);
                if (wantNormals) {
                    slx[at(x, y)] = gxv;
                    sly[at(x, y)] = gyv;
                }
                if (look.creamStart <= 0.0f && look.filmAlpha > 0.0f && gm < 0.004f && v < 0.07f) {
                    // flat thin film (most of a soaked body): just the wet-skin tint, none of the drop shading
                    af[at(x, y)] = look.filmAlpha * std::clamp((v - 0.006f) / 0.04f, 0.0f, 1.0f);
                    for (int c = 0; c < 3; ++c) rgbF[at(x, y) * 3 + static_cast<std::size_t>(c)] = look.haloColor[c];
                    continue;
                }
                const float lit = std::clamp(-(gxv * -0.55f + gyv * -0.83f) * 4.0f, 0.0f, 1.0f);  // slopes facing the fake light shine most
                const auto gi = NoiseIndex((rx0 + x) / kScale, (ry0 + y) / kScale);
                const float rimvar = std::clamp(0.5f + 0.5f * std::tanh(1.3f * g_rimvar[gi]), 0.0f, 1.0f);
                const float away = std::clamp((gxv * -0.55f + gyv * -0.83f) * 4.0f, 0.0f, 1.0f);  // slopes facing away from the fake light
                float aBody;
                float creamMix = 0.0f;
                const float filmA = look.creamStart <= 0.0f ? look.filmAlpha * std::clamp((v - 0.006f) / 0.04f, 0.0f, 1.0f) : 0.0f;  // water: a thin film that darkens the skin a little, as wet skin is
                if (look.creamStart > 0.0f) {
                    // two fluids: a clear glossy film wherever there is any liquid, and the opaque cream only where it is thick
                    const float t = std::clamp((v - look.creamStart) / 0.5f, 0.0f, 1.0f);
                    const float cream = t * t * (3.0f - 2.0f * t);
                    creamMix = cream;
                    const float cover = std::clamp((v - 0.012f) / 0.05f, 0.0f, 1.0f);
                    aBody = std::max(std::min(look.bodyAlpha * (1.0f + 1.4f * look.shade * away), 1.0f) * cream, look.filmAlpha * cover);
                } else {
                    aBody = std::min(look.bodyAlpha * (1.0f + 1.4f * look.shade * away), 1.0f) * (1.0f - std::exp(-v / look.hole));
                }
                const float lap = (hn(x - 1, y, v) + hn(x + 1, y, v) + hn(x, y - 1, v) + hn(x, y + 1, v) - 4.0f * v) * kScaleF * kScaleF;  // < 0 on a bulge (per sim pixel squared)
                // cream: thin parts (an old streak, a neck of a strand) are a see-through film, so their edges and bulges must not light up white;
                // rim and glint come in with the thickness, fully where the opaque body is
                const float creamBody = look.creamStart > 0.0f ? std::clamp(v / (look.creamStart + 0.35f), 0.0f, 1.0f) : 1.0f;
                const float glint = std::clamp(-lap * 7.0f, 0.0f, 1.0f) * (0.4f + 0.6f * lit) * look.glint * (look.creamStart <= 0.0f ? 0.3f : creamBody);  // (water: the sharp highlight comes from the dome shading below)
                const float facing = (1.0f - look.rimDirection) * (0.45f + 0.55f * lit) + look.rimDirection * (0.05f + 0.95f * lit * lit);  // the crescent facing the light
                // (the bright edge line of a thin film is a water look: a clear film's meniscus; not for cream)
                const float thinEdge = look.filmAlpha > 0.0f && look.creamStart <= 0.0f ? 10.0f * std::exp(-4.0f * v) : 0.0f;
                const float rimBase = std::min(std::clamp(gm * (5.0f + thinEdge), 0.0f, 1.0f) * facing * look.rimAlpha, 1.0f) * (0.55f + 0.45f * rimvar) * creamBody;
                const float rim = 1.0f - (1.0f - rimBase) * (1.0f - std::min(glint * 0.85f, 0.95f));
                const float mod = 1.0f + look.mottle * (0.055f * g_mott[gi] + 0.02f * g_grain[gi]);
                float a = 1.0f - (1.0f - aBody) * (1.0f - rim);
                const float inv = 1.0f / std::max(a, 1e-4f);
                float rgb[3];
                for (int c = 0; c < 3; ++c) {
                    const float base = hasCore ? look.bodyColor[c] + (look.coreColor[c] - look.bodyColor[c]) * creamMix : look.bodyColor[c];  // the warm glow of the thick cream
                    rgb[c] = (base * mod * (1.0f - 0.6f * look.shade * away) * aBody * (1.0f - rim) + look.rimColor[c] * rim) * inv;
                }
                if (look.creamStart <= 0.0f && look.glint > 0.0f) {
                    // water: a dome whose surface normal follows the thickness slope. Clear in the middle, a soft shadow on the side facing away from the light,
                    // and one small sharp highlight where the dome mirrors the light (instead of a wide white rim: that read as glitter)
                    const float nx = -gxv * 1.6f;
                    const float ny = -gyv * 1.6f;
                    const float nl = 1.0f / std::sqrt(nx * nx + ny * ny + 1.0f);
                    const float tilt = std::sqrt(nx * nx + ny * ny) * nl;  // 0 flat .. 1 on edge
                    const float ndl = (nx * kLightX + ny * kLightY + kLightZ) * nl;
                    const float ndh = (nx * kHalfX + ny * kHalfY + kHalfZ) * nl;
                    const float shadow = std::clamp((0.62f - ndl) * 1.3f, 0.0f, 1.0f) * std::min(tilt * 1.6f, 1.0f) * std::min(look.shade * 0.28f, 0.6f);
                    const float spot = std::clamp(std::pow(std::max(ndh, 0.0f), 70.0f) * look.glint * 1.1f, 0.0f, 1.0f) * std::min(v * 6.0f, 1.0f);
                    const float bounce = std::clamp((0.5f - ndl) * 1.4f, 0.0f, 1.0f) * std::clamp(1.0f - tilt * 2.5f, 0.0f, 1.0f) * std::min(v * 2.0f, 1.0f) * 0.025f;  // light focused through the drop, far side
                    float acc[3] = { look.haloColor[0] * filmA, look.haloColor[1] * filmA, look.haloColor[2] * filmA };  // the film under everything else
                    float aa = filmA;
                    const auto over = [&](const float colour[3], float alpha) {
                        for (int c = 0; c < 3; ++c) acc[c] = acc[c] * (1.0f - alpha) + colour[c] * alpha;
                        aa = aa * (1.0f - alpha) + alpha;
                    };
                    const float dark[3] = { 6.0f, 8.0f, 12.0f };
                    const float lightC[3] = { look.rimColor[0] * 0.8f, look.rimColor[1] * 0.8f, look.rimColor[2] * 0.8f };
                    const float white[3] = { 255.0f, 255.0f, 255.0f };
                    // the drop as a lens: the skin seen through it is a little brighter and warmer in the middle
                    const float lens = std::clamp(1.0f - tilt * 2.0f, 0.0f, 1.0f) * std::min(v * 1.2f, 1.0f) * 0.07f;
                    const float warm[3] = { 255.0f, 236.0f, 222.0f };
                    over(rgb, a);  // the drop's own body and rim over the film
                    over(warm, lens);
                    over(dark, shadow);
                    over(lightC, bounce);
                    over(white, spot);
                    a = aa;
                    const float inv2 = 1.0f / std::max(a, 1e-4f);
                    for (int c = 0; c < 3; ++c) rgb[c] = acc[c] * inv2;
                }
                af[at(x, y)] = a;
                for (int c = 0; c < 3; ++c) rgbF[at(x, y) * 3 + static_cast<std::size_t>(c)] = rgb[c];
            }
        }

        // halo: wet-darkened skin around the film = blurred coverage (two rounds of a 13-wide box blur ~ sigma 2.2), as sliding sums: the cost does not depend on the width
        blur = af;
        float maxA = 0.0f;
        for (const float a : af) maxA = std::max(maxA, a);
        for (int round = 0; round < (look.haloAlpha > 0.0f && maxA > 1e-4f ? 2 : 0); ++round) {
            for (int y = 0; y < h; ++y) {
                const float* row = &blur[at(0, y)];
                float* out = &tmp[at(0, y)];
                float sum = 0;
                for (int k = -6; k <= 6; ++k) sum += row[std::clamp(k, 0, w - 1)];
                for (int x = 0; x < w; ++x) {
                    out[x] = sum * (1.0f / 13.0f);
                    sum += row[std::min(x + 7, w - 1)] - row[std::max(x - 6, 0)];
                }
            }
            for (int x = 0; x < w; ++x) {
                float sum = 0;
                for (int k = -6; k <= 6; ++k) sum += tmp[at(x, std::clamp(k, 0, h - 1))];
                for (int y = 0; y < h; ++y) {
                    blur[at(x, y)] = sum * (1.0f / 13.0f);
                    sum += tmp[at(x, std::min(y + 7, h - 1))] - tmp[at(x, std::max(y - 6, 0))];
                }
            }
        }

        const auto& padRaster = DS::Field::Get(m_surface).PaddedTriRaster();
        for (int gy = y0; gy < y0 + kTile; ++gy) {
            for (int gx = x0; gx < x0 + kTile; ++gx) {
                const int x = gx - rx0;
                const int y = gy - ry0;
                const float aF = af[at(x, y)];
                // the wet darkening runs up to the island edge and a little past it (the padded mask; the draw mask stops ~2 px short, which left a light line at every seam)
                const bool inPad = padRaster.empty() ? keep[KeepIndex(gx, gy)] != 0 : padRaster[static_cast<std::size_t>(gy / kScale) * kSim + static_cast<std::size_t>(gx / kScale)] >= 0;
                const float halo = inPad ? look.haloAlpha * std::clamp(blur[at(x, y)] * 1.8f, 0.0f, 1.0f) : 0.0f;
                const float aOut = aF + halo * (1.0f - aF);
                float out[3] = { 0, 0, 0 };
                if (aOut > 1e-4f) {
                    for (int c = 0; c < 3; ++c) out[c] = (rgbF[at(x, y) * 3 + static_cast<std::size_t>(c)] * aF + look.haloColor[c] * (halo * (1.0f - aF))) / aOut;
                }
                if (wantNormals) {
                    constexpr float kFix = 8192.0f;
                    m_slope[Index(gx, gy) * 2] = static_cast<std::int16_t>(std::clamp(slx[at(x, y)] * kFix, -32000.0f, 32000.0f));
                    m_slope[Index(gx, gy) * 2 + 1] = static_cast<std::int16_t>(std::clamp(sly[at(x, y)] * kFix, -32000.0f, 32000.0f));
                }
                m_pixels[Index(gx, gy)] = (byte(aOut * 255.0f) << 24) | (byte(out[2] * look.brightness) << 16) | (byte(out[1] * look.brightness) << 8) | byte(out[0] * look.brightness);
            }
        }
    }

    bool Layer::Render()
    {
        if (m_pixels.empty()) return false;
        std::vector<int> tiles;
        for (int t = 0; t < kTiles * kTiles; ++t) {
            if (!m_dirty[static_cast<std::size_t>(t)]) continue;
            m_dirty[static_cast<std::size_t>(t)] = 0;
            tiles.push_back(t);
        }
        if (tiles.empty()) return false;
        m_pristine = false;  // (pixels and slopes get written)
        // tiles are independent (each writes only its own pixels and its own flag): render them on all cores
        g_tilesRendered += tiles.size();
        ParallelTiles(tiles, [this](int t) {
            RenderTile(t % kTiles, t / kTiles);
            m_changed[static_cast<std::size_t>(t)] = 1;
        });
        return true;
    }

    // ------------------------------------------------------------------ the normal map of one tile of the final image
    constexpr float kHeightUnits = 0.15f;  // game units of fluid height per unit of thickness (a thick deposit, 1.4, stands about 3 mm proud)
    void NormalTile(Canvas& c, int surface, int tx, int ty)
    {
        const auto& field = DS::Field::Get(surface);
        const auto& tris = field.Tris();
        const auto& raster = field.PaddedTriRaster();
        const auto& smooth = field.SmoothNormals();  // continuous across the UV seams, also in the padding past the island edges
        const auto byte = [](float value) { return static_cast<std::uint32_t>(std::clamp(value, 0.0f, 255.0f) + 0.5f); };
        for (int y = ty * kTile; y < (ty + 1) * kTile; ++y) {
            for (int x = tx * kTile; x < (tx + 1) * kTile; ++x) {
                const auto i = Index(x, y);
                const Layer* top = nullptr;  // the topmost layer with film here
                for (auto it = c.order.rbegin(); it != c.order.rend(); ++it) {
                    if ((*it)->Pixels()[i] >> 24 && (*it)->Alpha() > 0.0f && (*it)->WantsNormals()) {
                        top = *it;
                        break;
                    }
                }
                float n[3] = { 0.0f, 1.0f, 0.0f };
                const auto tri = raster.empty() ? -1 : raster[static_cast<std::size_t>(y / kScale) * kSim + static_cast<std::size_t>(x / kScale)];
                if (tri >= 0 && tris[static_cast<std::size_t>(tri)].valid) {
                    const auto& td = tris[static_cast<std::size_t>(tri)];
                    std::memcpy(n, td.n, sizeof n);
                    if (!smooth.empty()) {  // the smooth skin normal instead of the triangle's
                        const auto si = (static_cast<std::size_t>(y / kScale) * kSim + static_cast<std::size_t>(x / kScale)) * 3;
                        const float sx = static_cast<float>(smooth[si]);
                        const float sy = static_cast<float>(smooth[si + 1]);
                        const float sz = static_cast<float>(smooth[si + 2]);
                        if (sx != 0.0f || sy != 0.0f || sz != 0.0f) {
                            n[0] = sx * (1.0f / 16384.0f);
                            n[1] = sy * (1.0f / 16384.0f);
                            n[2] = sz * (1.0f / 16384.0f);
                        }
                    }
                    if (top && top->Slopes()) {
                        // the surface of the fluid is the skin pushed out along its normal by the thickness: its normal leans away from the thickness gradient,
                        // grad H (3D) = H_x * (gradient of the atlas x coordinate) + H_y * (... of y), both known per triangle
                        const float hx = static_cast<float>(top->Slopes()[i * 2]) * (1.0f / 8192.0f);
                        const float hy = static_cast<float>(top->Slopes()[i * 2 + 1]) * (1.0f / 8192.0f);
                        const float k = top->GetLook().normalStrength * kHeightUnits;
                        for (int m = 0; m < 3; ++m) n[m] -= k * (hx * td.a[m] + hy * td.a[3 + m]);
                        const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                        if (l > 1e-6f) {
                            for (float& v : n) v /= l;
                        }
                    }
                }
                // the game's model-space encoding (measured on the body's own normal map): R = x, G = z, B = y
                c.normal[i] = (255u << 24) | (byte((n[1] * 0.5f + 0.5f) * 255.0f) << 16) | (byte((n[2] * 0.5f + 0.5f) * 255.0f) << 8) | byte((n[0] * 0.5f + 0.5f) * 255.0f);
            }
        }
    }

    // ------------------------------------------------------------------ compositor (one per surface)
    std::mutex g_prewarmMutex;  // held by Prewarm (and by Acquire): the buffers are allocated once, in the background

    void Prewarm()
    {
        std::scoped_lock lock(g_prewarmMutex);
        EnsureShared();
        for (int s = 0; s < DS::Surface::Count; ++s) {
            auto& c = CanvasOf(s);
            if (c.final.empty()) c.final.assign(static_cast<std::size_t>(kN) * kN, 0u);
            for (auto& layer : c.layers) layer.Reserve();
        }
    }

    Layer* Acquire(int surface, const std::uint8_t* keepMask)
    {
        std::scoped_lock lock(g_prewarmMutex);
        auto& c = CanvasOf(surface);
        if (c.final.empty()) c.final.assign(static_cast<std::size_t>(kN) * kN, 0u);
        for (std::size_t i = 0; i < c.layers.size(); ++i) {
            if (c.used[i]) continue;
            c.used[i] = true;
            c.layers[i].Allocate(keepMask);
            c.layers[i].SetSurface(surface);
            c.order.push_back(&c.layers[i]);
            c.recomposite = true;
            return &c.layers[i];
        }
        return nullptr;
    }

    void Release(int surface, Layer* layer)
    {
        auto& c = CanvasOf(surface);
        for (std::size_t i = 0; i < c.layers.size(); ++i) {
            if (&c.layers[i] != layer) continue;
            c.used[i] = false;
            std::erase(c.order, layer);
            c.recomposite = true;  // the picture changes where this layer had film
            return;
        }
    }

    std::uint64_t TilesRendered()
    {
        return g_tilesRendered.load();
    }

    int ActiveLayers(int surface)
    {
        return static_cast<int>(CanvasOf(surface).order.size());
    }

    void BeginFrame(int surface)
    {
        for (auto* layer : CanvasOf(surface).order) layer->BeginFrame();
    }

    void EndFrame(int surface)
    {
        for (auto* layer : CanvasOf(surface).order) layer->EndFrame();
    }

    void Reset(int surface)
    {
        auto& c = CanvasOf(surface);
        c.order.clear();
        c.used.fill(false);
        c.recomposite = true;
    }

    const std::uint32_t* Pixels(int surface)
    {
        return CanvasOf(surface).final.data();
    }

    const std::uint32_t* NormalPixels(int surface)
    {
        const auto& c = CanvasOf(surface);
        return c.normal.empty() ? nullptr : c.normal.data();
    }

    const std::uint8_t* ChangedTiles(int surface)
    {
        return CanvasOf(surface).changed.data();
    }

    bool MaterialSpec(int surface, float* power, float* scale)
    {
        auto& c = CanvasOf(surface);
        for (auto it = c.order.rbegin(); it != c.order.rend(); ++it) {
            const auto& look = (*it)->GetLook();
            if (look.specScale > 0.0f && look.specPower > 0.0f) {
                *power = look.specPower;
                *scale = look.specScale * (*it)->Alpha();
                return true;
            }
        }
        return false;
    }

    bool Render(int surface)
    {
        auto& c = CanvasOf(surface);
        if (c.final.empty()) return false;
        c.changed.fill(0);
        bool any = false;
        bool allTiles = c.recomposite;
        c.recomposite = false;
        bool wantNormals = false;
        for (auto* layer : c.order) wantNormals = wantNormals || layer->WantsNormals();
        if (wantNormals && c.normal.empty()) {
            c.normal.assign(c.final.size(), 0xFFFF8080u);
            allTiles = true;
        }
        if (!wantNormals && !c.normal.empty()) c.normal.clear();
        for (auto* layer : c.order) {
            if (layer->Render()) any = true;
            if (layer->AlphaChanged()) {
                allTiles = true;
                layer->ClearAlphaChanged();
            }
        }
        if (!any && !allTiles) return false;
        std::vector<int> tiles;
        for (int t = 0; t < kTiles * kTiles; ++t) {
            bool changed = allTiles;
            for (auto* layer : c.order) {
                if (layer->TileChanged(t)) changed = true;
            }
            if (changed) tiles.push_back(t);
        }
        for (auto* layer : c.order) layer->ClearChanged();
        const bool single = c.order.size() == 1 && c.order[0]->Alpha() >= 1.0f;
        const auto byte = [](float value) { return static_cast<std::uint32_t>(std::clamp(value, 0.0f, 255.0f) + 0.5f); };
        ParallelTiles(tiles, [&](int t) {
            const int tx = t % kTiles;
            const int ty = t / kTiles;
            c.changed[static_cast<std::size_t>(t)] = 1;
            if (single) {  // one opaque layer: the layer's pixels are the picture
                for (int y = ty * kTile; y < (ty + 1) * kTile; ++y) {
                    std::copy_n(c.order[0]->Pixels() + Index(tx * kTile, y), kTile, c.final.begin() + static_cast<std::ptrdiff_t>(Index(tx * kTile, y)));
                }
                return;
            }
            for (int y = ty * kTile; y < (ty + 1) * kTile; ++y) {
                for (int x = tx * kTile; x < (tx + 1) * kTile; ++x) {
                    const auto i = Index(x, y);
                    float r = 0;
                    float g = 0;
                    float b = 0;
                    float a = 0;  // premultiplied accumulation, bottom to top
                    for (auto* layer : c.order) {
                        const std::uint32_t p = layer->Pixels()[i];
                        const float la = static_cast<float>(p >> 24) * (1.0f / 255.0f) * layer->Alpha();
                        if (la <= 0.0f) continue;
                        r = r * (1.0f - la) + static_cast<float>(p & 0xFF) * la;
                        g = g * (1.0f - la) + static_cast<float>((p >> 8) & 0xFF) * la;
                        b = b * (1.0f - la) + static_cast<float>((p >> 16) & 0xFF) * la;
                        a = a + la * (1.0f - a);
                    }
                    if (a <= 1e-4f) {
                        c.final[i] = 0;
                        continue;
                    }
                    c.final[i] = (byte(a * 255.0f) << 24) | (byte(b / a) << 16) | (byte(g / a) << 8) | byte(r / a);
                }
            }
        });
        if (wantNormals) {
            ParallelTiles(tiles, [&](int t) { NormalTile(c, surface, t % kTiles, t / kTiles); });
        }
        return true;
    }
}
