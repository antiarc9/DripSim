// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 AntiArc. This program is free software: see the LICENSE file (GNU GPL version 3 or later). It comes with NO WARRANTY.
#include "SweatMode.h"

#include <atomic>

// Sweat on skin. Rain falls from the sky onto the skin that faces it; sweat comes out of the skin itself. The model:
//  - "heat" (0..1) is a slow sum of the player's effort (IWorld::Exertion: sprint, fight, low stamina). It rises in seconds, falls over minutes.
//  - while it is warm, tiny beads appear at pore-like spots, more on the chest, the back, the neck and the forehead than on the legs. They start invisible and GROW,
//    so nothing pops in; beads that touch merge; a bead that gets heavy for its slope runs down the live gravity direction, absorbing beads on its way.
//  - a thin even sheen (the wet film) lies over the skin while it is warm and dries away afterwards.
// It uses the same film layer, gravity field and drawing calls as the rain mode, and can run at the same time (each mode has its own layer).
//
// Forced mode (variant 1, the toggle key): holds a fixed heat wherever the player is, to test the look. Variant 2: started by a mod event.
namespace
{
    constexpr float kStep = 0.04f;  // runner time step (s)

    struct Settings
    {
        bool autoStart = true;       // start by itself when the player gets hot
        float startHeat = 0.25f;     // ... above this heat
        float riseTime = 14.0f;      // seconds of full effort to get to 63 % heat
        float coolTime = 120.0f;     // seconds a full heat takes to fall to 0 at rest
        float rate = 80.0f;          // new beads per second over the whole body at full heat
        float size = 1.0f;           // multiplies bead sizes
        float grow = 1.2f;           // multiplies how fast the beads grow
        float maxBead = 2.5f;       // mm: the largest real radius of a bead. A bead at this size stops growing, and beads do not merge past it
        float runSize = 1.3f;     // multiplies the bead size at which a bead starts to run (bigger = it holds longer)
        float runSpeed = 0.8f;       // multiplies the speed of the runs
        float wet = 1.0f;            // strength of the wet-darkened halo
        float gloss = 1.6f;          // glints on the beads
        float opacity = 0.6f;        // multiplies the body opacity (lower = clearer)
        float normals = 1.5f;        // the film's normal map strength (0 = none; needs NormalMaps=1 in DripSim.ini)
        float sheen = 2.5f;          // the skin's specular strength under the sweat (the engine's lights make the highlights); 0 = off
        float shininess = 90.0f;     // gloss exponent there (lower = broader, oilier highlights)
        float film = 0.6f;           // strength of the thin even sheen over the skin (0 = only beads)
        float beadScale = 1.6f;      // multiplies the drawn size of a bead only (not when it runs): far from the camera a real 0.5 mm bead is less than 1 screen pixel
        float dryTime = 70.0f;       // seconds the beads take to dry once the body has cooled
        float forced = 0.9f;         // heat of the forced (toggle key) mode
        int maxBeads = 1600;
        int maxRunners = 40;         // running drops at once
        bool dripBody = true;        // sweat runs down this surface (false = the beads just stay and merge)
        bool dripFace = false;       // off by default, as with the rain (see docs/mode-rain.md)
        bool face = true;            // sweat on the face (the forehead): asks the framework for the face surface
        float waterDepth = 25.0f;    // game units of water above the feet that cool the body at once (and stop new sweat)
    };
    Settings g_cfg;
    const DripSim::IWorld* g_world = nullptr;

    void LoadSettings()
    {
        const DripSim::Ini ini("DripSimSweat");
        Settings v;
        v.autoStart = ini.Bool("Auto", true);
        v.startHeat = ini.Float("StartHeat", 0.25f);
        v.riseTime = ini.Float("RiseTime", 14.0f);
        v.coolTime = ini.Float("CoolTime", 120.0f);
        v.rate = ini.Float("Rate", 80.0f);
        v.size = ini.Float("Size", 1.0f);
        v.grow = ini.Float("Grow", 1.2f);
        v.maxBead = std::max(ini.Float("MaxBead", 2.5f), 0.2f);
        v.runSize = ini.Float("RunSize", 1.3f);
        v.runSpeed = ini.Float("RunSpeed", 0.8f);
        v.wet = ini.Float("Wet", 1.0f);
        v.gloss = ini.Float("Gloss", 1.6f);
        v.opacity = ini.Float("Opacity", 0.6f);
        v.normals = ini.Float("NormalStrength", 1.5f);
        v.sheen = ini.Float("Sheen", 2.5f);
        v.shininess = ini.Float("Shininess", 90.0f);
        v.film = ini.Float("Film", 0.6f);
        v.beadScale = std::max(ini.Float("BeadScale", 1.6f), 0.2f);
        v.dryTime = ini.Float("DryTime", 70.0f);
        v.forced = ini.Float("ForcedHeat", 0.9f);
        v.maxBeads = ini.Int("MaxBeads", 1600);
        v.maxRunners = ini.Int("MaxRunners", 40);
        v.dripBody = ini.Bool("DripBody", true);
        v.dripFace = ini.Bool("DripFace", false);
        v.face = ini.Bool("Face", true);
        v.waterDepth = ini.Float("CoolWaterDepth", 25.0f);
        g_cfg = v;
        logger::info("Sweat: settings: auto {} start {:.2f} rise {:.0f}s cool {:.0f}s rate {:.0f}/s size {:.2f} grow {:.2f} film {:.2f} dry {:.0f}s maxBeads {} face {}", v.autoStart, v.startHeat,
            v.riseTime, v.coolTime, v.rate, v.size, v.grow, v.film, v.dryTime, v.maxBeads, v.face);
    }

    struct Bead
    {
        float x, y;
        float vol;       // mm^2 (r = sqrt(vol)): "volume" is area-like, enough for merging
        float grow;      // mm^2 per second at full heat
        float dryAfter;  // seconds after the body cooled when it evaporates
        float drawn = 0; // the volume it was last drawn with (0 = not drawn yet)
    };

    struct Runner
    {
        float x, y;
        float vol;
        float vx = 0, vy = 1;
        float age = 0, acc = 0;
        float ph = 0;
    };

    struct Trail
    {
        float x, y, r;
        double born;
        int stage;
    };

    // One skin surface (the body, the face): its layer and field from the framework, its own beads, runners and trail, and its share of the sweat.
    struct Surf
    {
        std::string name;
        DripSim::ILayer* layer = nullptr;
        const DripSim::IField* field = nullptr;
        float weight = 1.0f;  // share of the new beads (skin area x how sweaty this surface is, over the total)
        bool drip = true;
        std::vector<Bead> beads;
        std::vector<Runner> runners;
        std::vector<Trail> trail;
    };
    std::vector<Surf> g_surfs;
    Surf* g_cur = nullptr;  // the surface the helpers below work on (all on the main thread)

    int BeadCap() { return std::max(40, static_cast<int>(static_cast<float>(g_cfg.maxBeads) * g_cur->weight)); }
    int RunnerCap() { return std::max(4, static_cast<int>(static_cast<float>(g_cfg.maxRunners) * g_cur->weight)); }

    // Sweat is clear like water, but the skin under it looks oily: broad highlights (low Shininess), a faint halo of wet skin around each bead.
    void ApplyLook()
    {
        DripSim::Look look{};
        const float body[3] = { 235, 205, 180 };  // warm: on skin the sweat shows the skin, brighter
        const float rim[3] = { 230, 212, 198 };
        const float halo[3] = { 30, 20, 14 };
        for (int i = 0; i < 3; ++i) {
            look.bodyColor[i] = body[i];
            look.rimColor[i] = rim[i];
            look.haloColor[i] = halo[i];
        }
        look.bodyAlpha = std::min(0.03f * g_cfg.opacity, 1.0f);
        look.rimAlpha = 0.06f;
        look.rimDirection = 0.9f;
        look.haloAlpha = std::min(0.30f * g_cfg.wet, 1.0f);
        look.hole = 0.35f;
        look.glint = g_cfg.gloss;
        look.filmAlpha = g_cfg.film > 0.0f ? 0.05f : 0.0f;  // the thin sheen carries the skin's gloss between the beads
        look.mottle = 0.0f;
        look.brightness = 1.0f;
        look.shade = 1.1f;
        look.creamStart = -1.0f;  // below -0.5: the renderer draws a clear drop, with no milky centre
        look.specPower = g_cfg.sheen > 0.0f ? g_cfg.shininess : 0.0f;
        look.specScale = 2.0f * g_cfg.sheen;
        look.normalStrength = g_cfg.normals;
        for (auto& surf : g_surfs) surf.layer->SetLook(&look);
    }

    struct State
    {
        double t = 0;
        double coolFor = 0;    // seconds the body has been cool (0 while it is warm)
        bool forced = false;
        float film = 0;        // strength of the sheen now (0..1)
        float peak = 0;        // highest heat of this run: the sheen only falls to 70 % of it while the body is warm
        float spawnAcc = 0;
        float checkAcc = 0;    // time since the beads were last grown, merged and re-checked for running
        std::uint64_t frames = 0;
    };
    State g_s;
    std::mt19937 g_rng{ 11u };

    // The heat of the body: shared by Idle (the mode is not running) and Update (it is), so it carries on across a start and a stop.
    float g_heat = 0.0f;
    bool g_noAuto = false;  // the player stopped the mode (toggle key) while the body was still warm: no automatic start until the heat has fallen below StartHeat
    std::atomic<float> g_request{ 0.0f };
    std::atomic<float> g_heatOut{ 0.0f };  // the heat the sweat shows now (SweatHeatNow)

    float U(float a, float b) { return std::uniform_real_distribution<float>(a, b)(g_rng); }
    float Chance() { return U(0.0f, 1.0f); }

    bool InWater(const DripSim::IWorld* world) { return world->IsSwimming() || world->WaterDepth() >= g_cfg.waterDepth; }

    // Effort warms the body at a speed set by RiseTime; at rest it cools slowly, and water cools it fast.
    void StepHeat(const DripSim::IWorld* world, float dt)
    {
        const float request = g_request.exchange(0.0f);
        if (request > 0.0f) g_heat = std::max(g_heat, std::min(request, 1.0f));
        const float effort = world->Exertion();
        if (InWater(world)) {
            g_heat = std::max(0.0f, g_heat - dt * 6.0f / std::max(g_cfg.coolTime, 1.0f));
        } else if (effort > g_heat) {
            g_heat = std::min(1.0f, g_heat + (effort - g_heat) * std::min(dt / std::max(g_cfg.riseTime, 0.5f), 1.0f));
        } else {
            g_heat = std::max(0.0f, g_heat - dt / std::max(g_cfg.coolTime, 1.0f));
        }
    }

    DripSim::Sample At(float x, float y)
    {
        DripSim::Sample out{};
        g_cur->field->At(x, y, &out);
        return out;
    }

    float Radius(float vol) { return std::sqrt(std::max(vol, 0.0001f)); }

    float Smooth(float a, float b, float x)
    {
        const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    float Band(float a, float b, float f) { return Smooth(a - 0.04f, a + 0.04f, f) * (1.0f - Smooth(b - 0.04f, b + 0.04f, f)); }

    // How likely sweat comes out at a spot: f = the standing-pose height inside the surface (0 lowest .. 1 highest), ny = front (+1) or back (-1).
    // Chest, back, neck and forehead sweat most; the belly and thighs less; the shins little.
    float RegionWeight(const Surf& surf, float f, float ny)
    {
        if (surf.name == "face") return 0.3f + 0.7f * Smooth(0.45f, 0.8f, f);  // the forehead most
        if (surf.name != "body") return 0.4f;
        float w = 0.1f;
        w = std::max(w, 0.45f * Band(0.16f, 0.45f, f));  // thighs
        w = std::max(w, 0.6f * Band(0.50f, 0.66f, f));   // belly, small of the back
        w = std::max(w, 1.0f * Band(0.64f, 0.88f, f));   // chest, upper back, arms
        w = std::max(w, 0.8f * Smooth(0.86f, 0.92f, f)); // neck, shoulders
        return w * (0.55f + 0.45f * std::fabs(ny));
    }

    // Draws a bead. On a slope it sags: a little longer along the way down, narrower across.
    void DrawBead(Bead& b)
    {
        const auto s = At(b.x, b.y);
        const float r = Radius(b.vol) * g_cfg.beadScale / s.mmpx;  // px
        const float sag = std::min(s.steep, 1.0f) * std::min(r / 2.0f, 1.0f) * 0.4f;
        const float ang = std::atan2(s.dy, s.dx);
        if (sag > 0.03f && (s.dx != 0.0f || s.dy != 0.0f)) g_cur->layer->Stamp(b.x, b.y, r * (1.0f + sag), r * (1.0f - 0.4f * sag), ang, 1.1f, 0.55f, true);
        else g_cur->layer->Stamp(b.x, b.y, r, r, 0, 1.1f, 0.55f, true);
        b.drawn = b.vol;
    }

    void EraseBead(Bead& b)
    {
        if (b.drawn <= 0.0f) return;
        const auto s = At(b.x, b.y);
        const float r = std::max(Radius(b.drawn) * g_cfg.beadScale / s.mmpx * 1.4f, 1.0f);  // covers a sagging bead too
        g_cur->layer->Erase(b.x, b.y, r, r, 0, 1.0f);
        b.drawn = 0.0f;
    }

    // sliding starts when a bead is heavy for the slope: steep skin releases small drops, flat skin holds big beads. Sweat is thicker than rain: it holds longer.
    float CriticalRadiusMm(float steep, float heat) { return std::max(3.0f * g_cfg.runSize / (1.0f + 2.2f * std::max(steep, 0.12f)) * (1.15f - 0.35f * heat), 0.5f); }

    void StartRunner(std::size_t beadIndex)
    {
        Bead b = g_cur->beads[beadIndex];
        EraseBead(g_cur->beads[beadIndex]);
        g_cur->beads.erase(g_cur->beads.begin() + static_cast<std::ptrdiff_t>(beadIndex));
        Runner r;
        r.x = b.x;
        r.y = b.y;
        r.vol = b.vol;
        r.ph = U(0, 6.283f);
        const auto s = At(b.x, b.y);
        r.vx = s.dx;
        r.vy = s.dy;
        g_cur->runners.push_back(r);
    }

    // a runner reaching the end of its life just dries; one that stops on a slope settles as a bead
    void Settle(const Runner& r)
    {
        if (r.vol < 0.4f || static_cast<int>(g_cur->beads.size()) >= BeadCap()) return;
        Bead b{ r.x, r.y, r.vol, U(0.005f, 0.02f), U(0.0f, g_cfg.dryTime), 0.0f };
        g_cur->beads.push_back(b);
        DrawBead(g_cur->beads.back());
    }

    // a new bead appears: tiny, and it grows. It joins a bead it lies on.
    void Nucleate(float x, float y)
    {
        if (static_cast<int>(g_cur->beads.size()) >= BeadCap()) return;
        const auto s = At(x, y);
        for (const Bead& b : g_cur->beads) {
            if (std::hypot(b.x - x, b.y - y) * s.mmpx < Radius(b.vol) + 0.25f) return;  // it would only be inside a bead that is already there
        }
        Bead b{ x, y, U(0.03f, 0.12f) * g_cfg.size, U(0.012f, 0.05f) * g_cfg.grow, U(0.0f, g_cfg.dryTime), 0.0f };
        g_cur->beads.push_back(b);  // not drawn yet: the first growth step draws it
    }

    void Spawn(float heat, float dt)
    {
        g_s.spawnAcc += g_cfg.rate * std::pow(heat, 1.2f) * dt;
        const int n = static_cast<int>(g_s.spawnAcc);
        g_s.spawnAcc -= static_cast<float>(n);
        for (int i = 0; i < n; ++i) {
            float pick = Chance();  // which surface: by skin area and how sweaty it is
            std::size_t index = 0;
            for (; index + 1 < g_surfs.size(); ++index) {
                if (pick < g_surfs[index].weight) break;
                pick -= g_surfs[index].weight;
            }
            g_cur = &g_surfs[index];
            const int size = g_cur->field->Size();
            const float zMin = g_cur->field->ZMin();
            const float zSpan = std::max(g_cur->field->ZMax() - zMin, 1.0f);
            for (int attempt = 0; attempt < 40; ++attempt) {
                const float x = U(0.0f, static_cast<float>(size - 1));
                const float y = U(0.0f, static_cast<float>(size - 1));
                const auto ix = static_cast<std::int32_t>(x);
                const auto iy = static_cast<std::int32_t>(y);
                if (!g_cur->field->Inner(ix, iy)) continue;
                const auto tri = g_cur->field->TriAt(ix, iy);
                if (tri < 0) continue;
                const float f = (g_cur->field->CenterZ(tri) - zMin) / zSpan;
                if (Chance() > RegionWeight(*g_cur, f, g_cur->field->NormalY(tri))) continue;
                Nucleate(x, y);
                break;
            }
        }
    }

    // Every 0.25 s on every surface: the beads grow with the heat, the ones that touch merge, the heavy ones start to run, the grown ones are redrawn.
    // The volume of a bead of the largest allowed radius.
    float MaxBeadVolume()
    {
        float lo = 0.0f, hi = 40.0f;
        for (int i = 0; i < 24; ++i) {
            const float mid = 0.5f * (lo + hi);
            if (Radius(mid) < g_cfg.maxBead) lo = mid;
            else hi = mid;
        }
        return lo;
    }

    void GrowBeads(float heat, float dt)
    {
        const float cap = MaxBeadVolume();  // without this limit, a full bead list only makes the old beads bigger and bigger
        for (Bead& b : g_cur->beads) b.vol = std::min(b.vol + b.grow * heat * dt, cap);
    }

    // Beads that touch merge into one. The beads are sorted by x, so each one is only compared with the ones close to it on the atlas.
    void MergeBeads()
    {
        auto& beads = g_cur->beads;
        if (beads.size() < 2) return;
        std::sort(beads.begin(), beads.end(), [](const Bead& a, const Bead& b) { return a.x < b.x; });
        const float density = std::clamp(static_cast<float>(beads.size()) / static_cast<float>(std::max(BeadCap(), 1)), 0.0f, 1.0f);
        const float spread = 0.85f + 0.5f * density;  // a crowded surface coalesces into bigger beads
        std::vector<float> px(beads.size());  // radius of each bead in atlas pixels
        float widest = 0.0f;
        for (std::size_t i = 0; i < beads.size(); ++i) {
            px[i] = Radius(beads[i].vol) * g_cfg.beadScale / At(beads[i].x, beads[i].y).mmpx;
            widest = std::max(widest, px[i]);
        }
        const float window = 2.0f * widest * spread + 1.0f;
        for (std::size_t i = 0; i < beads.size(); ++i) {
            if (beads[i].vol <= 0.0f) continue;
            for (std::size_t j = i + 1; j < beads.size() && beads[j].x - beads[i].x <= window; ++j) {
                if (beads[j].vol <= 0.0f) continue;
                const float reach = (px[i] + px[j]) * spread;
                if (std::hypot(beads[i].x - beads[j].x, beads[i].y - beads[j].y) > reach) continue;
                const float total = beads[i].vol + beads[j].vol;
                if (Radius(total) > g_cfg.maxBead * 1.15f) continue;  // beads never merge past the size limit
                EraseBead(beads[i]);
                EraseBead(beads[j]);
                beads[i].x = (beads[i].x * beads[i].vol + beads[j].x * beads[j].vol) / total;
                beads[i].y = (beads[i].y * beads[i].vol + beads[j].y * beads[j].vol) / total;
                beads[i].grow = std::max(beads[i].grow, beads[j].grow);
                beads[i].vol = total;
                px[i] = Radius(total) * g_cfg.beadScale / At(beads[i].x, beads[i].y).mmpx;
                beads[j].vol = 0.0f;  // removed below
            }
        }
        std::erase_if(beads, [](const Bead& b) { return b.vol <= 0.0f; });
    }

    // between 85 % and 100 % of the run size a bead only has a chance to start, so they set off one after another
    void ReleaseHeavyBeads(float heat)
    {
        if (!g_cur->drip) return;
        for (std::size_t i = g_cur->beads.size(); i-- > 0;) {
            if (static_cast<int>(g_cur->runners.size()) >= RunnerCap()) return;
            const Bead& b = g_cur->beads[i];
            const auto s = At(b.x, b.y);
            if (s.steep <= 0.08f || !s.inner) continue;
            const float ratio = Radius(b.vol) / CriticalRadiusMm(s.steep, heat);
            if (ratio < 0.85f) continue;
            const float chance = std::clamp((ratio - 0.85f) * 2.0f, 0.0f, 1.0f) * 0.5f;
            if (Chance() < chance) StartRunner(i);
        }
    }

    void RedrawBeads()
    {
        for (Bead& b : g_cur->beads) {
            const float now = Radius(b.vol);
            if (b.drawn <= 0.0f) {
                const auto s = At(b.x, b.y);
                if (now * g_cfg.beadScale / s.mmpx >= 0.45f) DrawBead(b);  // a bead too small to see stays unseen until it has grown
            } else if (now > Radius(b.drawn) * 1.07f) {
                EraseBead(b);
                DrawBead(b);
            }
        }
    }

    void StepRunner(Runner& r, double now, float heat)
    {
        const auto s = At(r.x, r.y);
        if (!s.inner || s.steep < 0.06f || r.vol < 0.3f) {  // flat ground, island edge, or used up
            if (s.inner && r.vol >= 0.3f) Settle(r);
            r.vol = 0;
            return;
        }
        float gx = s.dx;
        float gy = s.dy;
        const float n = std::hypot(gx, gy);
        if (n < 1e-6f) {
            r.vol = 0;
            return;
        }
        gx /= n;
        gy /= n;
        r.vx = 0.8f * r.vx + 0.2f * gx;
        r.vy = 0.8f * r.vy + 0.2f * gy;
        const float nn = std::max(std::hypot(r.vx, r.vy), 1e-6f);
        r.vx /= nn;
        r.vy /= nn;
        const float rMm = Radius(r.vol);
        const float pulse = (std::sin(r.age * 5.3f + r.ph) + 1.0f) * 0.5f;  // stick-slip on the skin
        const float vmm = 28.0f * g_cfg.runSpeed * std::pow(rMm, 1.1f) * s.steep * (0.35f + 0.65f * pulse) * (0.75f + 0.4f * heat);
        const float stepMm = vmm * kStep;
        r.x += r.vx * stepMm / s.mmpx;
        r.y += r.vy * stepMm / s.mmpx;
        r.vol -= 0.016f * stepMm;  // a little stays behind on the skin
        // a thin wet trail: stamped along the whole step, faded out later (two steps)
        const float tr = std::max(0.5f / s.mmpx, 0.9f);
        const float stepPx = stepMm / s.mmpx;
        const int pieces = std::max(1, static_cast<int>(std::ceil(stepPx / (0.5f * tr))));
        for (int i = 1; i <= pieces; ++i) {
            const float f = static_cast<float>(i) / static_cast<float>(pieces);
            const float tx = r.x - r.vx * stepPx * (1.0f - f);
            const float ty = r.y - r.vy * stepPx * (1.0f - f);
            g_cur->layer->Stamp(tx, ty, tr, tr, 0, 0.4f, 0.5f, true);
            g_cur->trail.push_back({ tx, ty, tr, now, 0 });
        }
        // absorb the beads on the way
        for (std::size_t i = 0; i < g_cur->beads.size();) {
            Bead& b = g_cur->beads[i];
            if (std::hypot(b.x - r.x, b.y - r.y) * s.mmpx < (Radius(b.vol) + Radius(r.vol)) * 0.9f) {
                EraseBead(b);
                r.vol += b.vol;
                g_cur->beads.erase(g_cur->beads.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
    }

    void DrawRunner(const Runner& r)
    {
        const auto s = At(r.x, r.y);
        const float rh = Radius(r.vol) / s.mmpx;
        const float ang = std::atan2(r.vy, r.vx);
        g_cur->layer->Stamp(r.x, r.y, rh * 1.25f, rh * 0.95f, ang, 1.15f, 0.55f, false);
        g_cur->layer->Stamp(r.x - r.vx * rh * 1.4f, r.y - r.vy * rh * 1.4f, rh * 0.7f, rh * 0.55f, ang, 0.7f, 0.55f, false);
    }

    void FadeTrails(double now)
    {
        for (std::size_t i = 0; i < g_cur->trail.size();) {
            Trail& t = g_cur->trail[i];
            const double age = now - t.born;
            if (t.stage == 0 && age > 1.6) {
                g_cur->layer->Erase(t.x, t.y, t.r * 1.2f, t.r * 1.2f, 0, 0.5f);
                t.stage = 1;
            } else if (t.stage == 1 && age > 3.0) {
                g_cur->layer->Erase(t.x, t.y, t.r * 1.3f, t.r * 1.3f, 0, 1.0f);
                g_cur->trail.erase(g_cur->trail.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            ++i;
        }
    }

    class SweatMode final : public DripSim::IMode
    {
    public:
        const char* Name() const override { return "sweat"; }

        bool Start(DripSim::IHost* host, std::int32_t variant) override
        {
            g_world = host->World();
            if (!g_world) return false;
            LoadSettings();
            g_surfs.clear();
            float total = 0.0f;
            for (std::int32_t i = 0; i < host->SurfaceCount(); ++i) {
                Surf surf;
                surf.name = host->SurfaceName(i);
                surf.layer = host->SurfaceLayer(i);
                surf.field = host->SurfaceField(i);
                surf.drip = surf.name == "body" ? g_cfg.dripBody : surf.name == "face" ? g_cfg.dripFace : true;
                if (!surf.layer || !surf.field) continue;
                // skin area in mm^2, sampled every 4th pixel
                double area = 0.0;
                for (int y = 0; y < surf.field->Size(); y += 4) {
                    for (int x = 0; x < surf.field->Size(); x += 4) {
                        if (!surf.field->Inner(x, y)) continue;
                        DripSim::Sample sample{};
                        surf.field->At(static_cast<float>(x), static_cast<float>(y), &sample);
                        area += static_cast<double>(sample.mmpx) * sample.mmpx * 16.0;
                    }
                }
                const float bias = surf.name == "face" ? 0.3f : 1.0f;  // the face atlas holds more skin area than the visible face (mouth, eyes): 3.0 gave the face 67 % of the beads in game
                surf.weight = static_cast<float>(area) * bias;
                total += surf.weight;
                g_surfs.push_back(std::move(surf));
            }
            if (g_surfs.empty() || total <= 0.0f) return false;
            std::string names;
            for (auto& surf : g_surfs) {
                surf.weight /= total;
                names += std::format("{}{} {:.0f}%{}", names.empty() ? "" : ", ", surf.name, surf.weight * 100.0f, surf.drip ? "" : " (no running)");
            }
            g_s = State{};
            g_s.forced = variant == 1;
            g_rng.seed(std::random_device{}());
            ApplyLook();
            logger::info("Sweat: surfaces {}; heat {:.2f}{}", names, g_heat, g_s.forced ? " (forced mode holds its own heat)" : "");
            host->Notify(g_s.forced ? "DripSim sweat: forced sweat on; press the toggle key again to stop" : "DripSim sweat: you are sweating");
            return true;
        }

        void Update(DripSim::IHost* host, float dt) override
        {
            g_s.t += dt;
            ++g_s.frames;
            ApplyLook();
            StepHeat(g_world, dt);
            const float heat = g_s.forced ? std::max(g_heat, g_cfg.forced) : g_heat;
            g_heatOut.store(heat);  // (the forced heat is only for this run: the real heat must not stay high, or the mode would restart by itself after the toggle key stops it)
            const bool warm = heat >= 0.05f;
            if (warm) g_s.coolFor = 0;
            else g_s.coolFor += dt;
            if (warm && !InWater(g_world)) Spawn(heat, dt);

            // the thin even sheen: it builds in seconds while warm and dries away over DryTime
            g_s.peak = std::max(g_s.peak, heat);
            const float target = warm ? std::min(1.0f, std::max(heat, 0.7f * g_s.peak) * 1.2f) * g_cfg.film : 0.0f;
            if (target > g_s.film) g_s.film = std::min(target, g_s.film + dt / 8.0f);
            else g_s.film = std::max(target, g_s.film - dt / std::max(g_cfg.dryTime, 1.0f));

            g_s.checkAcc += dt;
            const bool check = g_s.checkAcc >= 0.25f;
            const float checkDt = g_s.checkAcc;
            if (check) g_s.checkAcc = 0.0f;
            std::size_t beads = 0;
            std::size_t runners = 0;
            std::size_t trails = 0;
            for (auto& surf : g_surfs) {
                g_cur = &surf;
                // beads dry out once the body has cooled, each at its own time
                if (g_s.coolFor > 0) {
                    for (std::size_t i = 0; i < g_cur->beads.size();) {
                        if (g_s.coolFor > g_cur->beads[i].dryAfter) {
                            EraseBead(g_cur->beads[i]);
                            g_cur->beads.erase(g_cur->beads.begin() + static_cast<std::ptrdiff_t>(i));
                        } else {
                            ++i;
                        }
                    }
                }
                g_cur->layer->SetWetFilm(1.0e6f, g_s.film);  // the whole surface (the water line is far above the skin)
                if (check) {
                    if (warm) GrowBeads(heat, checkDt);
                    MergeBeads();
                    ReleaseHeavyBeads(heat);
                    RedrawBeads();
                }
                for (auto& r : g_cur->runners) {  // runners (fixed steps), then this frame's heads
                    r.acc += dt;
                    while (r.vol > 0 && r.acc >= kStep) {
                        r.acc -= kStep;
                        r.age += kStep;
                        StepRunner(r, g_s.t, heat);
                    }
                }
                std::erase_if(g_cur->runners, [](const Runner& r) { return r.vol <= 0.0f; });
                for (const auto& r : g_cur->runners) DrawRunner(r);
                FadeTrails(g_s.t);
                beads += g_cur->beads.size();
                runners += g_cur->runners.size();
                trails += g_cur->trail.size();
            }

            if (!g_s.forced && g_s.coolFor > 1.0 && beads == 0 && runners == 0 && trails == 0 && g_s.film <= 0.0f) host->Finish();
            if (g_s.frames % 600 == 0) logger::info("Sweat: t={:.0f}s heat {:.2f} film {:.2f} beads {} runners {} trail {}", g_s.t, heat, g_s.film, beads, runners, trails);
        }

        void Stop(DripSim::IHost*, DripSim::StopReason reason) override
        {
            if (reason == DripSim::StopReason::Requested && g_heat > g_cfg.startHeat) g_noAuto = true;  // a stop by the player, not the mode finishing (that only happens when the body is cool)
            logger::info("Sweat: stopped after {:.0f} s{}", g_s.t, g_noAuto ? " (no automatic start until the body has cooled)" : "");
            g_s = State{};
            g_surfs.clear();
            g_cur = nullptr;
            g_world = nullptr;
        }

        // Automatic start: the body gets hot. (The heat is followed here while the mode is not running.)
        void Idle(DripSim::IDripSim* api, float dt) override
        {
            const auto* world = api->World();
            if (!world) return;
            StepHeat(world, dt);
            g_heatOut.store(g_heat);
            if (g_heat > 0.05f) {  // why it does or does not start: one line every ~5 s while the body is warm
                static float logAcc = 0.0f;
                logAcc += dt;
                if (logAcc >= 5.0f) {
                    logAcc = 0.0f;
                    logger::info("Sweat: idle, heat {:.2f} (starts above {:.2f}), exertion {:.2f}", g_heat, g_cfg.startHeat, world->Exertion());
                }
            }
            if (g_noAuto && g_heat < g_cfg.startHeat) g_noAuto = false;
            if (g_cfg.autoStart && !g_noAuto && g_heat > g_cfg.startHeat && !InWater(world)) api->Start("sweat", -1);
        }

        void Reload() override { LoadSettings(); }
        std::int32_t Surfaces() const override { return g_cfg.face ? DripSim::kSurfaceFace : 0; }  // the body is always given
    };

    SweatMode g_mode;
}

float SweatHeatNow()
{
    return g_heatOut.load();
}

DripSim::IMode* SweatModeInstance()
{
    return &g_mode;
}

void SweatLoadSettings()
{
    LoadSettings();
}

void SweatRequestHeat(float heat)
{
    g_request.store(std::clamp(heat, 0.05f, 1.0f));
}
