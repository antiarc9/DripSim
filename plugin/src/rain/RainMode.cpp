// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 AntiArc. This program is free software: see the LICENSE file (GNU GPL version 3 or later). It comes with NO WARRANTY.
#include "RainMode.h"
#include "LogLimit.h"

#include <atomic>

// Rain water on skin. The model is the classic "drops on a window": rain lands as small beads on surfaces that face the sky; beads that touch
// merge; a bead that gets heavy for the slope slides down the live gravity direction, absorbing beads on its way and leaving a thin wet trail
// that fades; when the rain stops the beads dry out. It draws into a film layer with a clear, glinting water look.
// It wets every skin surface the framework offers (body, hands, feet), each with its own beads and runners; the rain is shared out by surface area.
//
// Weather mode (variant -1): follows the game's rain and only wets the player when the sky is open above their head.
// Forced mode (variant 1, the toggle key): rains at a fixed intensity wherever the player is, to test the effect without waiting for weather.
namespace
{
    constexpr float kPi = 3.14159265f;
    constexpr float kStep = 0.04f;  // runner time step (s)

    struct Settings
    {
        bool autoStart = true;       // start by itself when it rains and the player is under open sky
        float rate = 110.0f;         // new drops per second over the whole body at full rain
        float size = 1.0f;           // multiplies bead sizes
        float wet = 1.0f;            // strength of the wet-darkened halo
        float gloss = 1.5f;          // glints on the beads
        float opacity = 0.6f;        // multiplies the water's body opacity (lower = clearer)
        float normals = 1.5f;        // the film's normal map strength (0 = none; needs NormalMaps=1 in DripSim.ini)
        float sheen = 2.0f;          // the skin's specular strength under the water (the engine's lights make the highlights); 0 = off
        float shininess = 130.0f;    // gloss exponent there (higher = tighter highlights)
        float dryTime = 45.0f;       // seconds the beads take to dry once the rain stops
        float forced = 0.9f;         // rain intensity of the forced (toggle key) mode
        int maxBeads = 1200;
        int maxRunners = 60;         // sliding drops at once
        float startIntensity = 0.2f; // weather mode starts by itself above this rain intensity
        bool needSky = true;         // weather mode: no rain on the player under a roof or tree
        bool dripBody = true;        // water runs down this surface (false = the beads just stay and merge)
        bool dripHands = true;
        bool dripFeet = true;
        bool dripFace = false;       // off by default: see docs/mode-rain.md (drops were seen running up the face)
        // soak: water that comes off a swim, a bath or a dunk (also the mod event DripSim_Soak and the soak key)
        bool enableSoak = false;     // master switch of everything soak (off in version 1): the key, the mod event, leaving water
        bool autoSoak = true;        // leaving deep enough water starts it by itself (only with enableSoak)
        float soakMinDepth = 25.0f;  // game units of water above the feet that count as having been in water (wading to the shins)
        float soakMinTime = 1.0f;    // seconds in the water at least
        float soakTime = 30.0f;      // seconds the water keeps streaming off after a full dunk
        float soakRate = 140.0f;     // new drops per second over the wet skin right after leaving the water
        int soakBurst = 900;         // drops that are there at once the moment the water is left (scaled by how wet it was)
        float soakSize = 1.5f;       // bead size relative to rain
        bool soakFilm = true;        // a thin clear film of water over the soaked skin (the wet sheen); false = only drops
        float soakDry = 90.0f;       // seconds the beads take to dry (instead of DryTime) after a soak
        // the look of a soak is its own (Soak* keys): rain and soak do not share Wet / Gloss / Opacity / NormalStrength / Sheen / Shininess. A missing key falls back to the rain value.
        float soakWet = 1.0f;
        float soakGloss = 1.5f;
        float soakOpacity = 0.6f;
        float soakNormals = 1.5f;
        float soakSheen = 2.0f;
        float soakShininess = 130.0f;
        float sheenAlpha = 1.0f;  // the sheet as a wet sheen: most opacity, normal-map bend, colour (0 = the wet-skin halo colour .. 1 = the bright rim colour)
        float sheenBump = 2.0f;
        float sheenTint = 0.0f;
    };
    Settings g_cfg;
    DripSim::IHost* g_host = nullptr;
    const DripSim::IWorld* g_world = nullptr;

    void LoadSettings()
    {
        const DripSim::Ini ini("DripSimRain");
        Settings v;
        v.autoStart = ini.Bool("Auto", true);
        v.rate = ini.Float("Rate", 110.0f);
        v.size = ini.Float("Size", 1.0f);
        v.wet = ini.Float("Wet", 1.0f);
        v.gloss = ini.Float("Gloss", 1.5f);
        v.opacity = ini.Float("Opacity", 0.6f);
        v.sheen = ini.Float("Sheen", 2.0f);
        v.normals = ini.Float("NormalStrength", 1.5f);
        v.shininess = ini.Float("Shininess", 130.0f);
        v.dryTime = ini.Float("DryTime", 45.0f);
        v.forced = ini.Float("ForcedIntensity", 0.9f);
        v.maxBeads = ini.Int("MaxBeads", 1200);
        v.maxRunners = ini.Int("MaxRunners", 60);
        v.needSky = ini.Bool("NeedSky", true);
        v.startIntensity = ini.Float("StartIntensity", 0.2f);
        v.dripBody = ini.Bool("DripBody", true);
        v.dripHands = ini.Bool("DripHands", true);
        v.dripFeet = ini.Bool("DripFeet", true);
        v.dripFace = ini.Bool("DripFace", false);
        v.enableSoak = ini.Bool("EnableSoak", false);
        v.autoSoak = v.enableSoak && ini.Bool("AutoSoak", true);
        v.soakMinDepth = ini.Float("SoakMinDepth", 25.0f);
        v.soakMinTime = ini.Float("SoakMinTime", 1.0f);
        v.soakTime = ini.Float("SoakTime", 30.0f);
        v.soakRate = ini.Float("SoakRate", 140.0f);
        v.soakBurst = ini.Int("SoakBurst", 900);
        v.soakSize = ini.Float("SoakSize", 1.5f);
        v.soakDry = ini.Float("SoakDryTime", 90.0f);
        v.soakFilm = ini.Bool("SoakFilm", true);
        v.soakWet = ini.Float("SoakWet", v.wet);
        v.soakGloss = ini.Float("SoakGloss", v.gloss);
        v.soakOpacity = ini.Float("SoakOpacity", v.opacity);
        v.soakNormals = ini.Float("SoakNormalStrength", v.normals);
        v.soakSheen = ini.Float("SoakSheen", v.sheen);
        v.soakShininess = ini.Float("SoakShininess", v.shininess);
        v.sheenAlpha = ini.Float("SoakSheenAlpha", 1.0f);
        v.sheenBump = ini.Float("SoakSheenBump", 2.0f);
        v.sheenTint = ini.Float("SoakSheenTint", 0.0f);
        g_cfg = v;
        if (DS::LogAllow("rain-settings")) logger::info("Rain: settings: auto {} rate {:.0f}/s size {:.2f} wet {:.2f} gloss {:.2f} opacity {:.2f} dry {:.0f}s forced {:.2f} maxBeads {} needSky {}", v.autoStart, v.rate, v.size, v.wet,
            v.gloss, v.opacity, v.dryTime, v.forced, v.maxBeads, v.needSky);
    }

    struct Bead
    {
        float x, y;
        float vol;       // mm^2 (r = sqrt(vol)): "volume" is area-like, enough for merging
        float dryAfter;  // seconds after the rain stopped when it evaporates
    };

    struct Runner
    {
        float x, y;
        float vol;
        float vx = 0, vy = 1;
        float age = 0, acc = 0;
        float ph = 0;
        float run = 0;
    };

    struct Trail
    {
        float x, y, r;
        double born;
        int stage;
    };

    // One skin surface (body, hands, feet): its layer and field from the framework, its own beads, runners and trail, and its share of the rain.
    struct Surf
    {
        std::string name;
        DripSim::ILayer* layer = nullptr;
        const DripSim::IField* field = nullptr;
        float weight = 1.0f;  // share of the rain: its skin area over the total
        bool drip = true;     // beads on this surface may slide (INI DripBody / DripHands / DripFeet / DripFace)
        std::vector<Bead> beads;
        std::vector<Runner> runners;
        std::vector<Trail> trail;
    };
    std::vector<Surf> g_surfs;
    Surf* g_cur = nullptr;  // the surface the helpers below work on (all on the main thread)

    int BeadCap() { return std::max(40, static_cast<int>(static_cast<float>(g_cfg.maxBeads) * g_cur->weight)); }
    int RunnerCap() { return std::max(6, static_cast<int>(static_cast<float>(g_cfg.maxRunners) * g_cur->weight)); }

    // Clear water: almost no body of its own (you see the skin through it), a thin bright rim, small glints, a dark side (shade) and the skin
    // darkened around it by wetness. Opaque white would read as milk.
    void ApplyLook(bool soak)
    {
        DripSim::Look look{};
        const float body[3] = { 235, 205, 180 };  // warm: on skin the water shows the skin, brighter
        const float rim[3] = { 230, 212, 198 };
        const float halo[3] = { 22, 16, 12 };
        for (int i = 0; i < 3; ++i) {
            look.bodyColor[i] = body[i];
            look.rimColor[i] = rim[i];
            look.haloColor[i] = halo[i];
        }
        // soak: the soak has its own look values (Soak* keys)
        const float wet = soak ? g_cfg.soakWet : g_cfg.wet;
        const float gloss = soak ? g_cfg.soakGloss : g_cfg.gloss;
        const float opacity = soak ? g_cfg.soakOpacity : g_cfg.opacity;
        const float normals = soak ? g_cfg.soakNormals : g_cfg.normals;
        const float sheen = soak ? g_cfg.soakSheen : g_cfg.sheen;
        const float shininess = soak ? g_cfg.soakShininess : g_cfg.shininess;
        look.bodyAlpha = std::min(0.03f * opacity, 1.0f);
        look.rimAlpha = 0.06f;  // only a faint edge: the look of a drop is its shadow side and one small highlight (Film.cpp), not a white ring
        look.rimDirection = 0.9f;
        look.haloAlpha = std::min(0.38f * wet, 1.0f);
        look.hole = 0.35f;
        look.glint = gloss;
        look.filmAlpha = g_cfg.soakFilm ? 0.07f : 0.0f;  // the thin water film (soak): a faint opacity in the wet-skin colour; it carries the skin's gloss between the drops
        look.mottle = 0.0f;
        look.brightness = 1.0f;
        look.shade = 1.1f;  // a dark far side, but see-through
        look.specPower = sheen > 0.0f ? shininess : 0.0f;
        look.specScale = 2.0f * sheen;
        look.normalStrength = normals;
        for (auto& surf : g_surfs) {
            surf.layer->SetLook(&look);
            surf.layer->SetFlowLook(g_cfg.sheenAlpha, g_cfg.sheenBump, g_cfg.sheenTint);  // the soak's draining sheet: a smooth wet sheen (Film.cpp), not drops
        }
    }

    struct State
    {
        double t = 0;
        double dryFor = 0;     // seconds since the rain stopped (0 while it rains)
        float rain = 0;        // current intensity 0..1 (the stronger of the weather and the soak)
        float wetness = 0;     // 0..1: builds while it rains (~25 s at full rain), fades when it stops; wet skin sheds water sooner
        bool forced = false;
        float soak = 0;        // 0..1: water coming off a dunk, runs down to 0 over soakTime
        float soakStart = 0;   // its strength when it began (sets how long it lasts)
        float film = 0;        // strength of the wet film (0..1) the last soak left: it holds while the soak runs, then dries away over soakDry
        float soakDepth = 0;   // game units of water the skin was in: only skin below this height gets wet
        bool soakLook = false; // the layer shows the soak look (set when a soak begins, cleared when its water has dried), not the rain look
        float spawnAcc = 0;
        float soakAcc = 0;
        float checkAcc = 0;    // time since the beads were last re-checked for sliding
        std::uint64_t frames = 0;
        double sawRain = 0;
    };
    State g_s;
    std::mt19937 g_rng{ 7u };

    float U(float a, float b) { return std::uniform_real_distribution<float>(a, b)(g_rng); }
    float Chance() { return U(0.0f, 1.0f); }

    DripSim::Sample At(float x, float y)
    {
        DripSim::Sample out{};
        g_cur->field->At(x, y, &out);
        return out;
    }

    float Radius(float vol) { return std::sqrt(std::max(vol, 0.01f)); }

    // when a new bead dries out (seconds after the wetting stopped): after a dunk the skin stays wet longer than after a shower of rain
    float DryAfter() { return U(0.0f, g_s.soak > 0.02f ? std::max(g_cfg.dryTime, g_cfg.soakDry) : g_cfg.dryTime); }

    // sliding starts when a bead is heavy for the slope: steep surfaces release small drops, flat ones hold big beads
    float CriticalRadiusMm(float steep) { return std::max(2.6f / (1.0f + 2.2f * std::max(steep, 0.12f)) * (1.25f - 0.45f * g_s.rain - 0.25f * g_s.wetness), 0.5f); }

    void DrawBead(const Bead& b, bool permanent)
    {
        const auto s = At(b.x, b.y);
        const float r = Radius(b.vol) / s.mmpx;  // px
        // on a slope a bead sags: a little longer along the way down, narrower across
        const float sag = std::min(s.steep, 1.0f) * std::min(r / 2.0f, 1.0f) * 0.4f;
        const float ang = std::atan2(s.dy, s.dx);
        if (sag > 0.03f && (s.dx != 0.0f || s.dy != 0.0f)) g_cur->layer->Stamp(b.x, b.y, r * (1.0f + sag), r * (1.0f - 0.4f * sag), ang, 1.1f, 0.55f, permanent);
        else g_cur->layer->Stamp(b.x, b.y, r, r, 0, 1.1f, 0.55f, permanent);
    }

    void EraseBead(const Bead& b)
    {
        const auto s = At(b.x, b.y);
        const float r = Radius(b.vol) / s.mmpx * 1.5f;  // covers a sagging bead too
        g_cur->layer->Erase(b.x, b.y, r, r, 0, 1.0f);
    }

    // a runner reaching the end of its life (no volume left, flat ground, island edge) just dries; one that stops on a slope settles as a bead
    void Settle(const Runner& r, bool asBead)
    {
        if (!asBead || r.vol < 0.5f || static_cast<int>(g_cur->beads.size()) >= BeadCap()) return;
        Bead b{ r.x, r.y, r.vol, DryAfter() };
        g_cur->beads.push_back(b);
        DrawBead(b, true);
    }

    void StartRunner(std::size_t beadIndex)
    {
        const Bead b = g_cur->beads[beadIndex];
        EraseBead(b);
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

    // a new drop lands: it merges with a bead it touches, otherwise it becomes a bead of its own
    void Land(float x, float y, float rMm)
    {
        const float vol = rMm * rMm;
        const auto s = At(x, y);
        for (std::size_t i = 0; i < g_cur->beads.size(); ++i) {
            Bead& b = g_cur->beads[i];
            const float density = std::clamp(static_cast<float>(g_cur->beads.size()) / static_cast<float>(std::max(BeadCap(), 1)), 0.0f, 1.0f);
            const float reach = (Radius(b.vol) + rMm) * (0.85f + 0.6f * density) / s.mmpx;  // a crowded surface coalesces into bigger beads
            if (std::hypot(b.x - x, b.y - y) > reach) continue;
            if (!g_cur->drip && Radius(b.vol + vol) > 2.4f) continue;  // no dripping here: beads stop growing instead of running
            EraseBead(b);
            const float total = b.vol + vol;
            b.x = (b.x * b.vol + x * vol) / total;
            b.y = (b.y * b.vol + y * vol) / total;
            b.vol = total;
            const auto here = At(b.x, b.y);
            if (g_cur->drip && here.steep > 0.08f && Radius(b.vol) >= CriticalRadiusMm(here.steep)) {
                StartRunner(i);  // erases the bead (at its new size) and lets it slide
            } else {
                DrawBead(b, true);
            }
            return;
        }
        if (static_cast<int>(g_cur->beads.size()) >= BeadCap()) return;
        Bead b{ x, y, vol, DryAfter() };
        g_cur->beads.push_back(b);
        DrawBead(b, true);
    }

    // Every bead is re-checked a few times a second, not only when something lands on it: a bead that is heavy for its slope starts to slide,
    // sooner the wetter the skin is. Between 85 % and 100 % of the slide size it only has a chance to, so they set off one after another.
    void ReleaseHeavyBeads()
    {
        if (!g_cur->drip) return;
        for (std::size_t i = g_cur->beads.size(); i-- > 0;) {
            if (static_cast<int>(g_cur->runners.size()) >= RunnerCap()) return;
            const Bead& b = g_cur->beads[i];
            const auto s = At(b.x, b.y);
            if (s.steep <= 0.08f || !s.inner) continue;
            const float ratio = Radius(b.vol) / CriticalRadiusMm(s.steep);
            if (ratio < 0.85f) continue;
            const float chance = std::clamp((ratio - 0.85f) * 2.0f, 0.0f, 1.0f) * 0.5f;
            if (Chance() < chance) StartRunner(i);
        }
    }

    void Spawn(float intensity, float dt)
    {
        g_s.spawnAcc += g_cfg.rate * std::pow(intensity, 1.1f) * dt;
        const int n = static_cast<int>(g_s.spawnAcc);
        g_s.spawnAcc -= static_cast<float>(n);
        for (int i = 0; i < n; ++i) {
            float pick = Chance();  // which surface this drop falls on: by skin area
            std::size_t index = 0;
            for (; index + 1 < g_surfs.size(); ++index) {
                if (pick < g_surfs[index].weight) break;
                pick -= g_surfs[index].weight;
            }
            g_cur = &g_surfs[index];
            const int size = g_cur->field->Size();
            for (int attempt = 0; attempt < 24; ++attempt) {  // a random spot of that surface that faces the sky
                const float x = U(0.0f, static_cast<float>(size - 1));
                const float y = U(0.0f, static_cast<float>(size - 1));
                if (!g_cur->field->Inner(static_cast<int>(x), static_cast<int>(y))) continue;
                const auto s = At(x, y);
                if (Chance() > std::pow(s.up, 1.4f) + 0.04f) continue;
                Land(x, y, U(0.5f, 1.3f) * g_cfg.size * (0.6f + 0.7f * intensity + 0.3f * g_s.wetness));
                break;
            }
        }
    }

    // ---------------------------------------------------------------- soak: water that comes off a dunk
    float g_bodyZMin = 0.0f;  // lowest point of the body in the field's skin space: the feet, the reference for the water line

    // Was this spot of the skin under the water line? Heights are the standing-pose (anatomical) ones of the triangle under the pixel.
    bool Submerged(float x, float y)
    {
        const auto tri = g_cur->field->TriAt(static_cast<std::int32_t>(x), static_cast<std::int32_t>(y));
        return tri >= 0 && g_cur->field->CenterZ(tri) - g_bodyZMin < g_s.soakDepth;
    }

    // n drops on the skin that was in the water: anywhere there (facing the sky a little more), sized by how soaked it is
    void SoakDrops(int n)
    {
        for (int i = 0; i < n; ++i) {
            float pick = Chance();
            std::size_t index = 0;
            for (; index + 1 < g_surfs.size(); ++index) {
                if (pick < g_surfs[index].weight) break;
                pick -= g_surfs[index].weight;
            }
            g_cur = &g_surfs[index];
            const int size = g_cur->field->Size();
            for (int attempt = 0; attempt < 24; ++attempt) {
                const float x = U(0.0f, static_cast<float>(size - 1));
                const float y = U(0.0f, static_cast<float>(size - 1));
                if (!g_cur->field->Inner(static_cast<int>(x), static_cast<int>(y)) || !Submerged(x, y)) continue;
                const auto s = At(x, y);
                if (Chance() > 0.55f + 0.45f * s.up) continue;
                // a few big drops among many tiny ones, as on real wet skin
                const float radius = Chance() < 0.7f ? U(0.35f, 0.9f) : U(1.1f, 2.6f);
                Land(x, y, radius * g_cfg.size * g_cfg.soakSize * (0.7f + 0.5f * g_s.soak));
                break;
            }
        }
    }

    void SpawnSoak(float dt)
    {
        g_s.soakAcc += g_cfg.soakRate * std::pow(g_s.soak, 1.2f) * dt;
        const int n = static_cast<int>(g_s.soakAcc);
        g_s.soakAcc -= static_cast<float>(n);
        SoakDrops(n);
    }

    // The water was just left (or a mod / key asked for a soak): the skin below the water line is wet at once, then the water keeps coming off.
    void BeginSoak(float strength, float depth)
    {
        strength = std::clamp(strength, 0.05f, 1.0f);
        const float before = g_s.soak;
        g_s.soak = std::max(g_s.soak, strength);
        g_s.soakStart = std::max(g_s.soakStart * (before > 0.02f ? 1.0f : 0.0f), g_s.soak);
        g_s.soakDepth = std::max(g_s.soakDepth * (before > 0.02f ? 1.0f : 0.0f), depth);
        g_s.wetness = std::max(g_s.wetness, g_s.soak);
        g_s.film = std::max(g_s.film, strength);
        g_s.dryFor = 0;
        g_s.soakLook = true;
        const int burst = static_cast<int>(static_cast<float>(g_cfg.soakBurst) * strength * (1.0f - 0.5f * before));
        SoakDrops(burst);
        logger::info("Rain: soak {:.2f} (water to {:.0f} units): {} drops at once", g_s.soak, depth, burst);
    }

    // What the framework world says about water, watched over time: leaving water one has been in for a moment starts a soak.
    struct Dunk
    {
        float inFor = 0;     // seconds in the water (deep enough)
        float outFor = 0;    // seconds since it was last deep enough
        float maxDepth = 0;  // the deepest it got
        bool in = false;
    };
    Dunk g_dunk;

    float WaterNow(const DripSim::IWorld* world)
    {
        float depth = world->WaterDepth();
        if (world->IsSwimming()) depth = std::max(depth, 400.0f);  // afloat: all of the body is in
        return depth;
    }

    // true once when the player has left the water: *depth is how deep it was
    bool WatchWater(const DripSim::IWorld* world, float dt, float* depth)
    {
        const float now = WaterNow(world);
        if (now >= g_cfg.soakMinDepth) {
            g_dunk.inFor += dt;
            g_dunk.outFor = 0;
            g_dunk.maxDepth = std::max(g_dunk.maxDepth, now);
            if (g_dunk.inFor >= g_cfg.soakMinTime) g_dunk.in = true;
            return false;
        }
        g_dunk.outFor += dt;
        if (g_dunk.outFor < 1.0f) return false;  // bobbing at the surface or stepping back into the shallows is not leaving
        const bool left = g_dunk.in;
        *depth = g_dunk.maxDepth;
        g_dunk = Dunk{};
        return left;
    }

    float SoakStrength(float depth, bool swam) { return swam ? 1.0f : std::clamp(0.4f + 0.6f * std::min(depth / 110.0f, 1.0f), 0.3f, 1.0f); }

    // a soak asked for from outside (mod event, key), taken over by the mode on its next Start / Update
    struct SoakRequest
    {
        std::atomic<float> strength{ 0.0f };
        std::atomic<float> depth{ 0.0f };
    };
    SoakRequest g_request;

    void TakeRequest()
    {
        const float strength = g_request.strength.exchange(0.0f);
        if (strength > 0.0f) BeginSoak(strength, g_request.depth.load());
    }

    void StepRunner(Runner& r, double now)
    {
        const auto s = At(r.x, r.y);
        if (!s.inner || s.steep < 0.06f || r.vol < 0.4f) {  // flat ground, island edge, or used up
            if (s.inner && r.vol >= 0.4f) Settle(r, true);
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
        const float vmm = 28.0f * std::pow(rMm, 1.1f) * s.steep * (0.35f + 0.65f * pulse) * (0.8f + 0.5f * g_s.rain);  // heavier rain, faster rivulets
        const float stepMm = vmm * kStep;
        r.x += r.vx * stepMm / s.mmpx;
        r.y += r.vy * stepMm / s.mmpx;
        r.run += stepMm;
        r.vol -= 0.014f * stepMm * (1.2f - 0.6f * g_s.rain);  // water stays behind on the skin (less in a downpour: the rivulets reach further)
        // a thin wet trail: stamped now, faded out later (two steps)
        const float tr = std::max(0.55f / s.mmpx, 0.9f);  // (a wider trail: a hairline looked like a string)
        // stamped along the whole step (a stamp every half radius) so the trail is one continuous line, not a row of dots
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
            const Bead& b = g_cur->beads[i];
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
        g_cur->layer->Stamp(r.x, r.y, rh * 1.25f, rh * 0.95f, ang, 1.15f, 0.55f, false);  // a drop drawn out along its motion
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

    class RainMode final : public DripSim::IMode
    {
    public:
        const char* Name() const override { return "rain"; }

        bool Start(DripSim::IHost* host, std::int32_t variant) override
        {
            g_host = host;
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
                surf.drip = surf.name == "body" ? g_cfg.dripBody : surf.name == "hands" ? g_cfg.dripHands : surf.name == "feet" ? g_cfg.dripFeet : g_cfg.dripFace;
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
                surf.weight = static_cast<float>(area);
                total += surf.weight;
                g_surfs.push_back(std::move(surf));
            }
            if (g_surfs.empty() || total <= 0.0f) return false;
            std::string names;
            for (auto& surf : g_surfs) {
                surf.weight /= total;
                names += std::format("{}{} {:.0f}%{}", names.empty() ? "" : ", ", surf.name, surf.weight * 100.0f, surf.drip ? "" : " (no dripping)");
            }
            g_s = State{};
            g_s.forced = variant == 1;
            g_rng.seed(std::random_device{}());
            g_bodyZMin = host->Field() ? host->Field()->ZMin() : 0.0f;
            ApplyLook(g_s.soakLook);
            if (variant == 2) TakeRequest();  // started by a soak: the water that was asked for lands at once
            if (DS::LogAllow("rain-surfaces")) logger::info("Rain: surfaces {}", names);
            if (DS::LogAllow("rain-weather")) logger::info("Rain: weather '{}' intensity {:.2f}{}", g_world->WeatherName(), g_world->RainIntensity(), g_s.forced ? " (forced mode ignores it)" : "");
            host->Notify(g_s.forced ? "DripSim rain: forced rain on; press the toggle key again to stop" : variant == 2 ? "DripSim water: it runs off your skin" : "DripSim rain: it is raining on you");
            return true;
        }

        void Update(DripSim::IHost* host, float dt) override
        {
            g_s.t += dt;
            ++g_s.frames;
            ApplyLook(g_s.soakLook);
            float rain = 0.0f;
            if (g_s.forced) {
                rain = g_cfg.forced;
            } else {
                const bool open = !g_cfg.needSky || (g_world->SkyExposed() && !g_world->IsInterior());
                rain = open ? g_world->RainIntensity() : 0.0f;
            }
            // a dunk: asked for from outside, or the player has just left the water
            TakeRequest();
            float depth = 0.0f;
            if (g_cfg.autoSoak && WatchWater(g_world, dt, &depth)) BeginSoak(SoakStrength(depth, depth >= 400.0f), depth);
            if (g_s.soakLook && g_s.soak <= 0.02f && g_s.dryFor > static_cast<double>(g_cfg.soakDry) + 1.0) g_s.soakLook = false;  // all the soak water has dried: the rain look again
            if (g_s.soak > 0.0f) g_s.soak = std::max(0.0f, g_s.soak - dt * g_s.soakStart / std::max(g_cfg.soakTime * (0.4f + 0.6f * g_s.soakStart), 1.0f));
            const float source = rain;
            rain = std::max(rain, g_s.soak * 0.9f);
            g_s.rain = rain;
            if (rain < 0.05f) {
                g_s.dryFor += dt;
                g_s.wetness = std::max(0.0f, g_s.wetness - dt / std::max(g_cfg.dryTime, 1.0f));
            } else {
                g_s.dryFor = 0;
                g_s.wetness = std::max(std::min(1.0f, g_s.wetness + dt * 0.045f * source), g_s.soak);
                if (source >= 0.05f) Spawn(source, dt);
                if (g_s.soak >= 0.02f) SpawnSoak(dt);
            }

            g_s.checkAcc += dt;
            const bool check = g_s.checkAcc >= 0.25f;
            if (check) g_s.checkAcc = 0.0f;
            std::size_t beads = 0;
            std::size_t runners = 0;
            std::size_t trails = 0;
            double flowMs = 0.0;
            double flowJobMs = 0.0;
            float flowWater = 0.0f;
            bool flowing = false;
            for (auto& surf : g_surfs) {
                g_cur = &surf;
                // beads dry out once the rain has stopped, each at its own time
                if (g_s.dryFor > 0) {
                    for (std::size_t i = 0; i < g_cur->beads.size();) {
                        if (g_s.dryFor > g_cur->beads[i].dryAfter) {
                            EraseBead(g_cur->beads[i]);
                            g_cur->beads.erase(g_cur->beads.begin() + static_cast<std::ptrdiff_t>(i));
                        } else {
                            ++i;
                        }
                    }
                }
                if (g_cfg.soakFilm) {  // the even wet sheen of the skin that was under water
                    const float held = g_s.soak > 0.02f ? 1.0f : std::max(0.0f, 1.0f - static_cast<float>(g_s.dryFor) / std::max(g_cfg.soakDry, 1.0f));
                    // (the body's flow sheet is the wet skin there: the full-skin film on top of it only made every tile of the body heavy to render)
                    const bool sheet = false;  // (the free soak has no sheet)
                    // the face: no wet tint. Its overlay still z-fights with the head in some triangles (OverlayDecal only made it smaller); with the correct
                    // (premultiplied) colours the tint darkens the skin, and the hidden triangles showed as patches (game test 2026-11-12). Its wet look is the shine.
                    const bool face = g_cur->name == "face";
                    g_cur->layer->SetWetFilm(g_bodyZMin + g_s.soakDepth, sheet || face ? 0.0f : g_s.film * held);
                    if (held <= 0.0f) g_s.film = 0.0f;
                }
                if (check) ReleaseHeavyBeads();

                // runners (fixed steps), then this frame's heads
                for (auto& r : g_cur->runners) {
                    r.acc += dt;
                    while (r.vol > 0 && r.acc >= kStep) {
                        r.acc -= kStep;
                        r.age += kStep;
                        StepRunner(r, g_s.t);
                    }
                }
                std::erase_if(g_cur->runners, [](const Runner& r) { return r.vol <= 0.0f; });
                for (const auto& r : g_cur->runners) DrawRunner(r);
                FadeTrails(g_s.t);
                beads += g_cur->beads.size();
                runners += g_cur->runners.size();
                trails += g_cur->trail.size();
            }

            if (!g_s.forced && g_s.dryFor > 1.0 && beads == 0 && runners == 0 && trails == 0 && g_s.film <= 0.0f && !flowing) host->Finish();
            if (g_s.frames % 600 == 0) {
                logger::info("Rain: t={:.0f}s rain {:.2f} wetness {:.2f} beads {} runners {} trail {} sheet water {:.0f} (main thread {:.2f} ms, worker {:.1f} ms a run)", g_s.t, rain, g_s.wetness, beads, runners, trails, flowWater, flowMs, flowJobMs);
            }
        }

        void Stop(DripSim::IHost*, DripSim::StopReason) override
        {
            if (DS::LogAllow("rain-stopped")) logger::info("Rain: stopped after {:.0f} s", g_s.t);
            g_s = State{};
            g_surfs.clear();
            g_cur = nullptr;
            g_world = nullptr;
            g_host = nullptr;
        }

        // Automatic start: rain under open sky.
        void Idle(DripSim::IDripSim* api, float dt) override
        {
            const auto* world = api->World();
            if (!world) return;
            float depth = 0.0f;
            if (g_cfg.autoSoak && WatchWater(world, dt, &depth)) {  // out of the water: it comes off the skin
                RainRequestSoak(SoakStrength(depth, depth >= 400.0f), depth);
                api->Start("rain", 2);
                return;
            }
            if (!g_cfg.autoStart) return;
            const float intensity = world->RainIntensity();
            const bool sheltered = world->IsInterior() || !world->SkyExposed();
            if (intensity > 0.02f) {  // why it does or does not start: one line every ~5 s while the weather is rainy
                static float logAcc = 0.0f;
                logAcc += dt;
                if (logAcc >= 5.0f) {
                    logAcc = 0.0f;
                    logger::info("Rain: idle, intensity {:.2f} (starts above {:.2f}), {}", intensity, g_cfg.startIntensity, sheltered ? "sheltered (interior or something above the head)" : "open sky");
                }
            }
            if (sheltered) return;
            if (intensity > g_cfg.startIntensity) api->Start("rain", -1);
        }

        void Reload() override { LoadSettings(); }
        std::int32_t Surfaces() const override { return DripSim::kSurfaceHands | DripSim::kSurfaceFeet | DripSim::kSurfaceFace; }  // the body is always given
    };

    RainMode g_mode;
}

DripSim::IMode* RainModeInstance()
{
    return &g_mode;
}

void RainLoadSettings()
{
    LoadSettings();
}

bool RainSoakEnabled()
{
    return g_cfg.enableSoak;
}

void RainRequestSoak(float strength, float depth)
{
    if (!g_cfg.enableSoak) return;
    g_request.depth.store(std::max(depth, 1.0f));
    g_request.strength.store(std::clamp(strength, 0.05f, 1.0f));
}
