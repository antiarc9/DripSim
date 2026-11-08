// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 AntiArc. This program is free software: see the LICENSE file (GNU GPL version 3 or later). It comes with NO WARRANTY.
#include "Spike.h"

#include "Body.h"
#include "Field.h"
#include "Core.h"

// M1 debug view: the live gravity field (Field.h) drawn on the body as an animated texture. Hue = downhill direction in the UV image,
// brightness = steepness, bars move downhill, grey = flat (steepness < 0.22, runners would pool there).
namespace
{
    constexpr float kPeriod = 22.0f;  // pixels between bars (about 30 mm on the 1024 atlas)
    constexpr float kSpeed = 0.9f;    // bar periods per second

    // the gravity view's calls into the texture module, always as its owner
    int TexStart(int s) { return DS::Spike::TextureStart(s, DS::Spike::kOwnerGravity); }
    int TexPoll(int s) { return DS::Spike::TexturePoll(s, DS::Spike::kOwnerGravity); }
    void TexStop(int s) { DS::Spike::TextureStop(s, DS::Spike::kOwnerGravity); }
    bool TexRunning(int s) { return DS::Spike::TextureRunning(s, DS::Spike::kOwnerGravity); }
    bool TexBusy(int s) { return DS::Spike::TextureBusy(s, DS::Spike::kOwnerGravity); }

    struct State
    {
        int surface = 0;
        bool waiting = false;
        bool texturing = false;
        bool running = false;
        std::shared_ptr<const DS::Body::Static> before;  // the capture that existed when the view was requested
        std::vector<std::uint32_t> color;                // per triangle, 0x00BBGGRR (0 = not drawn)
        std::uint64_t frame = 0;
        double updateMs = 0;
        std::chrono::steady_clock::time_point begin;
    };
    State g_state;

    std::uint32_t Hsv(float h, float sat, float val)
    {
        const float hh = (h - std::floor(h)) * 6.0f;
        const int i = static_cast<int>(hh);
        const float f = hh - static_cast<float>(i);
        const float p = val * (1 - sat);
        const float q = val * (1 - sat * f);
        const float t = val * (1 - sat * (1 - f));
        float r = val;
        float g = t;
        float b = p;
        switch (i % 6) {
        case 1: r = q; g = val; b = p; break;
        case 2: r = p; g = val; b = t; break;
        case 3: r = p; g = q; b = val; break;
        case 4: r = t; g = p; b = val; break;
        case 5: r = val; g = p; b = q; break;
        default: break;
        }
        const auto byte = [](float x) { return static_cast<std::uint32_t>(std::clamp(x, 0.0f, 1.0f) * 255.0f); };
        return byte(r) | (byte(g) << 8) | (byte(b) << 16);
    }

    void Stop(const char* why)
    {
        if (!g_state.running && !g_state.waiting) return;
        if (TexBusy(g_state.surface)) TexStop(g_state.surface);
        logger::info("M1: gravity view stopped ({}) after {} updates", why, g_state.frame);
        g_state = State{};
    }

    void Update()
    {
        const auto started = std::chrono::steady_clock::now();
        auto& s = g_state;
        auto& field = DS::Field::Get(s.surface);
        field.Update();
        const auto& live = field.Live();
        const auto& raster = field.TriRaster();
        constexpr int n = DS::Atlas::kFilm;
        constexpr int scale = DS::Atlas::kFilm / DS::Atlas::kSim;
        for (std::size_t t = 0; t < live.size(); ++t) {
            s.color[t] = live[t].flat ? (live[t].steep > 0.0f ? 0x00A0A0A0u : 0u)
                                      : Hsv(std::atan2(live[t].dy, live[t].dx) / (2.0f * 3.14159265f) + 0.5f, 1.0f, 0.55f + 0.45f * live[t].steep);
        }
        const double time = std::chrono::duration<double>(std::chrono::steady_clock::now() - s.begin).count();
        std::vector<std::uint32_t> image(static_cast<std::size_t>(n) * n, 0);
        for (int y = 0; y < n; ++y) {
            for (int x = 0; x < n; ++x) {
                const auto id = raster[static_cast<std::size_t>(y / scale) * DS::Atlas::kSim + static_cast<std::size_t>(x / scale)];
                if (id < 0 || s.color[static_cast<std::size_t>(id)] == 0) continue;
                auto c = s.color[static_cast<std::size_t>(id)];
                const auto& l = live[static_cast<std::size_t>(id)];
                if (!l.flat) {
                    const double phase = (static_cast<double>(x / scale) * l.dx + static_cast<double>(y / scale) * l.dy) / kPeriod - time * kSpeed;
                    if (phase - std::floor(phase) >= 0.5) c = (c >> 1) & 0x007F7F7Fu;
                }
                image[static_cast<std::size_t>(y) * n + static_cast<std::size_t>(x)] = 0xE6000000u | c;
            }
        }
        DS::Spike::TextureSetFrame(s.surface, DS::Spike::kOwnerGravity, image.data());
        s.updateMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        if (++s.frame % 120 == 0) {
            logger::info("M1: update {:.2f} ms avg", s.updateMs / 120.0);
            s.updateMs = 0;
        }
    }
}

namespace DS::Spike
{
    void BeginView(int surface)
    {
        g_state = State{};
        g_state.surface = surface;
        g_state.before = DS::Body::Get(surface);
        g_state.waiting = true;
        DS::Body::Request(surface, false);
        logger::info("M1: gravity view of '{}' requested; waiting for the capture", DS::Surface::Name(surface));
    }

    // Ctrl+Shift+G: body, then each next press the next surface (hands, feet, face), then off.
    void ToggleGravityView()
    {
        if (g_state.running || g_state.waiting) {
            const int next = g_state.surface + 1;
            Stop("next");
            if (next < DS::Surface::Count) BeginView(next);
            return;
        }
        if (DS::Core::AnyActive()) {
            logger::info("M1: a mode is running; stop it first");
            return;
        }
        BeginView(DS::Surface::Body);
    }

    bool GravityViewActive()
    {
        return g_state.running || g_state.waiting;
    }

    void GravityOnFrame()
    {
        auto& s = g_state;
        if (s.waiting) {
            auto body = DS::Body::Get(s.surface);
            if (!body || body == s.before) return;
            s.waiting = false;
            if (DS::Core::AnyActive()) {
                logger::info("M1: a mode started meanwhile; the gravity view gives way");
                s = State{};
                return;
            }
            int state = 0;
            if (DS::Field::Get(s.surface).Build(body)) state = TexStart(s.surface);
            if (state == 2) {
                s.texturing = true;
            } else if (state != 1) {
                logger::warn("M1: could not start the '{}' view (needs a free '{} [Ovl#]' slot)", DS::Surface::Name(s.surface), DS::Surface::Prefix(s.surface));
                if (TexBusy(s.surface)) TexStop(s.surface);
                s = State{};
                return;
            }
        }
        if (s.texturing) {
            const int state = TexPoll(s.surface);
            if (state == 2) return;
            s.texturing = false;
            if (state != 1) {
                logger::warn("M1: the '{}' texture could not be set up", DS::Surface::Name(s.surface));
                s = State{};
                return;
            }
        }
        if (!s.running && !s.waiting && !s.texturing && TexRunning(s.surface) && s.color.empty()) {
            s.color.assign(DS::Field::Get(s.surface).Live().size(), 0);
            s.begin = std::chrono::steady_clock::now();
            s.running = true;
            logger::info("M1: gravity view of '{}' running ({} triangles)", DS::Surface::Name(s.surface), s.color.size());
            if (auto* console = RE::ConsoleLog::GetSingleton()) console->Print(std::format("DripSim M1: gravity view of the {} (bars move downhill, grey = flat); Ctrl+Shift+G = next surface / off", DS::Surface::Name(s.surface)).c_str());
            return;
        }
        if (!s.running) return;
        if (!TexRunning(s.surface) || !DS::Field::Get(s.surface).Alive()) return Stop("texture or surface lost");
        Update();
    }
}
