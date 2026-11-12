// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 AntiArc. This program is free software: see the LICENSE file (GNU GPL version 3 or later). It comes with NO WARRANTY.
#include "CreamMode.h"

#include <atomic>
#include <cstdio>

// Cream on skin, built on the rain mode's bead / runner model with a thick, viscous, opaque fluid instead of water:
//  - blobs are bigger, irregular (two lobes) and opaque white with a soft satin gloss, not clear beads with one sharp glint;
//  - a blob only starts to run when it is heavy for the slope, and then it creeps: slowly, slowing further as it sets, leaving a thick
//    permanent streak behind and ending in a hanging bulb (instead of a thin wet trail that dries);
//  - nothing evaporates: the cream stays for StayTime seconds, then fades out (or the clean key wipes it at once).
//
// Splash (variant 1, default): a few (3-5) big globs land close together on the face or the upper front of the torso. The impact splays each
// one out with irregular fingers: fast at first, slowing down, most of it within a third of a second. For a few seconds after that it only
// changes a little (it keeps widening slowly and sags a bit downhill), and only then, where the skin may drip, it starts to creep down as streaks.
// Pour (variant 2): a thick stream onto the highest skin that faces up (top of the head / forehead, shoulders, chest), which then runs down.
namespace
{
    constexpr float kStep = 0.04f;  // runner time step (s)

    struct Settings
    {
        int globsMin = 3;            // big globs per splash: a random number from GlobsMin to GlobsMax
        int globsMax = 5;
        float globSize = 7.0f;       // mm: radius of a glob as it lands (each one 75-125 % of this)
        float faceSize = 1.3f;       // globs on the face are this much bigger (the face is small in the atlas, they looked like specks)
        float splashSpread = 40.0f;  // mm: how far apart the globs of one splash land (around one aim point)
        float spreadTime = 0.3f;     // seconds the impact takes to splay a glob out with its fingers (fast at first, slowing down)
        float holdTime = 1.5f;       // seconds a splash then changes only a little before it starts to creep (each glob 75-125 % of this)
        float holdSpread = 0.15f;    // share of the spreading that still happens slowly during the hold (0 = it stops when the impact is over)
        float holdSag = 0.3f;        // how far a glob sags downhill during the hold, in its radii (only where the skin may drip)
        float spread = 1.6f;         // its radius when splayed out, relative to when it landed (it gets flatter as it widens)
        int fingersMin = 1;          // irregular fingers the impact pushes out of each glob
        int fingersMax = 3;
        float fingerLength = 0.9f;   // how far they reach, in glob radii (each 50-150 % of this)
        float fingerWidth = 0.55f;   // how thick they are, in glob radii (each 80-125 % of this); thin ones look like torn needles
        int spatters = 1;            // small droplets thrown off around each glob (0 .. this many)
        float pourRate = 38.0f;      // blobs per second while pouring (strength 1)
        float pourTime = 3.0f;       // seconds a pour lasts (strength 1)
        float pourSpread = 7.0f;     // mm: how wide the stream lands around the pour point
        float faceShare = 0.6f;      // chance that a splash lands on the face (when the face surface is there); otherwise the upper front of the torso
        float aimHeight = 0.75f;     // splash on the body: where it is aimed, 0 feet .. 1 top of the head (0.75 = the chest)
        float aimSpread = 0.1f;      // how far around that height the impact spots may be
        float size = 1.0f;           // multiplies blob sizes
        float viscosity = 1.0f;      // higher = holds on steeper skin, creeps slower and not as far
        float opacity = 1.0f;        // multiplies the body opacity of the thick cream
        float gloss = 1.0f;          // highlights on the blobs
        float normals = 1.5f;        // the film's normal map strength (0 = none; needs NormalMaps=1 in DripSim.ini): relief the game's lights see
        float sheen = 1.4f;          // the skin's specular strength under the cream; 0 = off
        float shininess = 65.0f;     // gloss exponent there: lower than water (130), a broader, softer satin highlight
        float shade = 0.2f;          // how much darker the side of a blob away from the light is (0 = flat white)
        float stayTime = 600.0f;     // seconds the cream stays after the last splash / pour (0 = until cleaned)
        float fadeTime = 20.0f;      // seconds it takes to fade out after that
        float color[3] = { 246.0f, 243.0f, 234.0f };      // the thick cream (a slightly warm white)
        float warmColor[3] = { 241.0f, 229.0f, 192.0f };  // the yellowish tint it can take on
        float warmth = 0.35f;        // up to this much of the tint, picked at random each time (some batches are whiter, some a little yellow)
        float clearing = 0.45f;      // as it sits on the skin it thins out: paler and more see-through, by up to this much ...
        float clearTime = 240.0f;    // ... fully reached after this many seconds
        int maxBlobs = 900;
        int maxRunners = 80;
        bool face = true;
        bool hands = false;
        bool feet = false;
        // may it run on this surface? (the face's gravity field used to be noise: positions and UVs of the head were paired wrongly in the capture)
        bool dripBody = true;
        bool dripHands = true;
        bool dripFeet = true;
        bool dripFace = true;
    };
    Settings g_cfg;

    void ReadColor(const DripSim::Ini& ini, const char* key, float out[3])
    {
        const auto text = ini.Text(key, "");
        float c[3];
        if (std::sscanf(text.c_str(), "%f , %f , %f", &c[0], &c[1], &c[2]) != 3) return;
        for (int i = 0; i < 3; ++i) out[i] = std::clamp(c[i], 0.0f, 255.0f);
    }

    void LoadSettings()
    {
        const DripSim::Ini ini("DripSimCream");
        Settings v;
        v.globsMin = std::clamp(ini.Int("GlobsMin", v.globsMin), 1, 12);
        v.globsMax = std::clamp(ini.Int("GlobsMax", v.globsMax), v.globsMin, 12);
        v.globSize = std::max(ini.Float("GlobSize", v.globSize), 0.5f);
        v.faceSize = std::max(ini.Float("FaceSize", v.faceSize), 0.1f);
        v.fingerWidth = std::clamp(ini.Float("FingerWidth", v.fingerWidth), 0.1f, 1.5f);
        v.spreadTime = std::max(ini.Float("SpreadTime", v.spreadTime), 0.02f);
        v.holdTime = std::max(ini.Float("HoldTime", v.holdTime), 0.0f);
        v.holdSpread = std::clamp(ini.Float("HoldSpread", v.holdSpread), 0.0f, 0.9f);
        v.holdSag = std::clamp(ini.Float("HoldSag", v.holdSag), 0.0f, 2.0f);
        v.spread = std::clamp(ini.Float("Spread", v.spread), 1.0f, 4.0f);
        v.fingersMin = std::clamp(ini.Int("FingersMin", v.fingersMin), 0, 8);
        v.fingersMax = std::clamp(ini.Int("FingersMax", v.fingersMax), v.fingersMin, 8);
        v.fingerLength = std::max(ini.Float("FingerLength", v.fingerLength), 0.0f);
        v.spatters = std::clamp(ini.Int("Spatters", v.spatters), 0, 8);
        v.pourRate = ini.Float("PourRate", v.pourRate);
        v.pourTime = ini.Float("PourTime", v.pourTime);
        v.pourSpread = ini.Float("PourSpread", v.pourSpread);
        v.faceShare = std::clamp(ini.Float("FaceShare", v.faceShare), 0.0f, 1.0f);
        v.aimHeight = ini.Float("AimHeight", v.aimHeight);
        v.aimSpread = std::max(ini.Float("AimSpread", v.aimSpread), 0.02f);
        v.splashSpread = std::max(ini.Float("SplashSpread", v.splashSpread), 0.0f);
        v.size = ini.Float("Size", v.size);
        v.viscosity = std::max(ini.Float("Viscosity", v.viscosity), 0.1f);
        v.opacity = ini.Float("Opacity", v.opacity);
        v.gloss = ini.Float("Gloss", v.gloss);
        v.normals = ini.Float("NormalStrength", v.normals);
        v.sheen = ini.Float("Sheen", v.sheen);
        v.shininess = ini.Float("Shininess", v.shininess);
        v.shade = std::clamp(ini.Float("Shade", v.shade), 0.0f, 1.0f);
        v.stayTime = ini.Float("StayTime", v.stayTime);
        v.fadeTime = std::max(ini.Float("FadeTime", v.fadeTime), 0.5f);
        ReadColor(ini, "Color", v.color);
        ReadColor(ini, "WarmColor", v.warmColor);
        v.warmth = std::clamp(ini.Float("Warmth", v.warmth), 0.0f, 1.0f);
        v.clearing = std::clamp(ini.Float("Clearing", v.clearing), 0.0f, 1.0f);
        v.clearTime = std::max(ini.Float("ClearTime", v.clearTime), 1.0f);
        v.maxBlobs = ini.Int("MaxBlobs", v.maxBlobs);
        v.maxRunners = ini.Int("MaxRunners", v.maxRunners);
        v.face = ini.Bool("Face", v.face);
        v.hands = ini.Bool("Hands", v.hands);
        v.feet = ini.Bool("Feet", v.feet);
        v.dripBody = ini.Bool("DripBody", v.dripBody);
        v.dripHands = ini.Bool("DripHands", v.dripHands);
        v.dripFeet = ini.Bool("DripFeet", v.dripFeet);
        v.dripFace = ini.Bool("DripFace", v.dripFace);
        g_cfg = v;
        logger::info("Cream: settings: splash {}-{} globs of {:.1f} mm, pour {:.0f}/s for {:.1f}s size {:.2f} viscosity {:.2f} opacity {:.2f} gloss {:.2f} stay {:.0f}s", v.globsMin, v.globsMax, v.globSize, v.pourRate, v.pourTime,
            v.size, v.viscosity, v.opacity, v.gloss, v.stayTime);
    }

    struct Blob
    {
        float x, y;
        float vol;      // mm^2 (r = sqrt(vol))
        float stretch;  // >= 1: drawn out along ang (a spatter that hit at a slant)
        float ang;
        float lobe;     // direction of the second, smaller lobe (irregular outline)
        double holdUntil = 0.0;  // it does not start to creep before this time (a fresh splash sits for a moment)
    };

    struct Runner
    {
        float x, y;
        float vol;
        float vx = 0, vy = 1;
        float age = 0, acc = 0;
        float ph = 0;
    };

    struct Surf
    {
        std::string name;
        DripSim::ILayer* layer = nullptr;
        const DripSim::IField* field = nullptr;
        float weight = 1.0f;     // share of a splash
        bool drip = true;        // blobs here may run (DripBody / DripHands / DripFeet / DripFace)
        float pourX = -1.0f;     // where the pour lands on this surface (-1 = it gets none)
        float pourY = -1.0f;
        std::vector<Blob> blobs;
        std::vector<Runner> runners;
    };
    std::vector<Surf> g_surfs;
    Surf* g_cur = nullptr;

    struct State
    {
        double t = 0;
        double since = 0;       // seconds since the last cream landed
        float splashStr = 1;
        float pourLeft = 0;     // seconds of pouring left
        float pourStr = 1;
        float pourAcc = 0;
        float checkAcc = 0;
        float alpha = 1;
        float warmth = 0;       // this run's tint (0 white .. 1 WarmColor)
        double firstAt = -1;    // when the first cream landed (the thinning out counts from here)
        float lookAcc = 0;
        std::uint64_t frames = 0;
    };

    // A glob of a splash while it spreads: drawn fresh every frame (it gets wider and flatter, its fingers grow), stamped for good when it is done.
    struct Finger
    {
        float ang;    // direction in the atlas
        float len;    // how far it reaches when done, in glob radii
        float width;  // in glob radii
        float bend;   // radians it curves by along its length
        bool down;    // the downhill finger (skin that may drip): its tip carries on as a streak
    };
    struct Glob
    {
        std::size_t surf;
        float x, y;
        float r0;       // px, as it landed
        float stretch;  // drawn out along ang (the way it was flying)
        float ang;
        float lobe;
        float age = 0;
        float dur = 1;      // the impact
        float hold = 0;     // then the hold
        float sagX = 0;     // downhill direction (atlas, unit), 0 where it may not drip
        float sagY = 0;
        std::vector<Finger> fingers;
    };
    std::vector<Glob> g_globs;
    State g_s;
    std::mt19937 g_rng{ 11u };

    float U(float a, float b) { return std::uniform_real_distribution<float>(a, b)(g_rng); }
    float Chance() { return U(0.0f, 1.0f); }
    float Gauss() { return std::normal_distribution<float>(0.0f, 1.0f)(g_rng); }

    DripSim::Sample At(float x, float y)
    {
        DripSim::Sample out{};
        g_cur->field->At(x, y, &out);
        return out;
    }

    float Radius(float vol) { return std::sqrt(std::max(vol, 0.01f)); }

    int BlobCap() { return std::max(40, static_cast<int>(static_cast<float>(g_cfg.maxBlobs) * g_cur->weight)); }
    int RunnerCap() { return std::max(6, static_cast<int>(static_cast<float>(g_cfg.maxRunners) * g_cur->weight)); }

    // a thick fluid holds on far steeper skin than water does before it creeps (water: ~0.8 mm on vertical skin; cream ~1.7 mm)
    float CriticalRadiusMm(float steep) { return std::max(4.4f / (1.0f + 1.6f * std::max(steep, 0.05f)) * std::sqrt(g_cfg.viscosity), 1.2f); }

    // Opaque, slightly warm white: a milky translucent film where it is thin (smears, the edges of a streak), a solid body where it is thick,
    // soft satin highlights (a broad specular lobe, mild rim) and a lumpy normal map. No wet-darkened halo: cream does not soak the skin.
    void ApplyLook()
    {
        DripSim::Look look{};
        // the tint: white .. a light yellow, a random amount per run. The longer it sits, the more it thins out: the tint fades,
        // the opaque body gets more see-through and less of it counts as thick (the edges and thin parts clear first)
        const float aged = g_s.firstAt < 0.0 ? 0.0f : std::min(static_cast<float>(g_s.t - g_s.firstAt) / g_cfg.clearTime, 1.0f);
        const float clear = g_cfg.clearing * aged * (2.0f - aged);  // eases out: most of it in the first half
        const float w = std::clamp(g_s.warmth * (1.0f - 0.6f * clear), 0.0f, 1.0f);
        float core[3];
        float film[3];
        const float thin[3] = { 230.0f, 226.0f, 214.0f };  // thin cream looks a touch yellower and greyer than thick: the skin shows through it
        for (int i = 0; i < 3; ++i) {
            core[i] = g_cfg.color[i] + (g_cfg.warmColor[i] - g_cfg.color[i]) * w;
            film[i] = thin[i] + (g_cfg.warmColor[i] * 0.95f - thin[i]) * std::min(w * 1.3f, 1.0f);
        }
        const float rim[3] = { 255.0f, 255.0f, 252.0f };
        const float halo[3] = { 60.0f, 48.0f, 40.0f };
        for (int i = 0; i < 3; ++i) {
            look.bodyColor[i] = film[i];
            look.coreColor[i] = core[i];
            look.rimColor[i] = rim[i];
            look.haloColor[i] = halo[i];
        }
        look.bodyAlpha = std::clamp(0.94f * g_cfg.opacity * (1.0f - 0.5f * clear), 0.0f, 1.0f);
        look.filmAlpha = std::clamp(0.34f * g_cfg.opacity * (1.0f - 0.7f * clear), 0.0f, 1.0f);  // thin cream: milky, the skin shows through a little
        look.creamStart = 0.16f + 0.3f * clear;  // above this thickness the opaque body comes in (full ~0.5 above)
        look.rimAlpha = 0.26f;
        look.rimDirection = 0.45f;  // the lit side of a blob's edge shines more, but the rim goes most of the way round (a thick, rounded edge)
        look.haloAlpha = 0.03f;
        look.hole = 0.22f;
        look.glint = 1.3f * g_cfg.gloss;  // wet highlights on the bulges, so a thick blob does not read as flat paint
        look.mottle = 0.55f;     // a little unevenness in the white, so large areas do not look like paint
        look.brightness = 1.0f;
        look.shade = g_cfg.shade;  // a soft dome: the side away from the light only a touch greyer
        look.smooth = 0.75f;    // overlapping stamps flow together into one surface (less: a fringe of fragments and glints)
        look.specPower = g_cfg.sheen > 0.0f ? g_cfg.shininess : 0.0f;
        look.specScale = 1.6f * g_cfg.sheen;
        look.normalStrength = g_cfg.normals;
        for (auto& surf : g_surfs) surf.layer->SetLook(&look);
    }

    void DrawBlob(const Blob& b)
    {
        const auto s = At(b.x, b.y);
        const float r = Radius(b.vol) / s.mmpx;
        // a heavy blob on a slope sags into a teardrop
        const float sag = std::min(s.steep, 1.0f) * std::min(Radius(b.vol) / 3.0f, 1.0f) * 0.5f;
        const float down = std::atan2(s.dy, s.dx);
        const bool hasDown = s.dx != 0.0f || s.dy != 0.0f;
        const float k = std::sqrt(b.stretch);
        g_cur->layer->Stamp(b.x, b.y, r * k, r / k, b.ang, 1.3f, 0.5f, true);
        g_cur->layer->Stamp(b.x + std::cos(b.lobe) * r * 0.5f, b.y + std::sin(b.lobe) * r * 0.5f, r * 0.7f, r * 0.6f, b.lobe, 1.15f, 0.5f, true);
        if (sag > 0.05f && hasDown) {
            g_cur->layer->Stamp(b.x + s.dx * r * 0.6f * sag * 2.0f, b.y + s.dy * r * 0.6f * sag * 2.0f, r * (0.75f + sag), r * 0.75f, down, 1.25f, 0.5f, true);
        }
    }

    void EraseBlob(const Blob& b)
    {
        const auto s = At(b.x, b.y);
        const float r = Radius(b.vol) / s.mmpx * std::sqrt(b.stretch) * 1.7f;
        g_cur->layer->Erase(b.x, b.y, r, r, 0, 1.0f);
    }

    void StartRunner(std::size_t index)
    {
        const Blob b = g_cur->blobs[index];
        // the blob is not erased: cream leaves most of itself where it was; only the part that runs off goes
        g_cur->blobs.erase(g_cur->blobs.begin() + static_cast<std::ptrdiff_t>(index));
        const auto s = At(b.x, b.y);
        Runner r;
        r.x = b.x;
        r.y = b.y;
        r.vol = b.vol * 0.65f;
        r.ph = U(0.0f, 6.283f);
        r.vx = s.dx;
        r.vy = s.dy;
        g_cur->runners.push_back(r);
    }

    // a blob lands: it joins a blob it touches (the two flow into one), otherwise it is a new blob
    void Land(float x, float y, float rMm, float stretch, float ang)
    {
        const float vol = rMm * rMm;
        const auto s = At(x, y);
        for (std::size_t i = 0; i < g_cur->blobs.size(); ++i) {
            Blob& b = g_cur->blobs[i];
            if (std::hypot(b.x - x, b.y - y) * s.mmpx > (Radius(b.vol) + rMm) * 0.8f) continue;
            if (Radius(b.vol + vol) > (g_cur->drip ? 7.0f : 4.5f)) continue;  // a puddle that big on flat skin: the new cream lies beside it instead
            const float total = b.vol + vol;
            b.x = (b.x * b.vol + x * vol) / total;
            b.y = (b.y * b.vol + y * vol) / total;
            b.vol = total;
            b.stretch = 1.0f + (b.stretch - 1.0f) * 0.5f;
            DrawBlob(b);  // stamps are max-blended: the old shape stays under the grown one
            const auto here = At(b.x, b.y);
            if (g_cur->drip && g_s.t >= b.holdUntil && here.steep > 0.04f && here.inner && Radius(b.vol) >= CriticalRadiusMm(here.steep) && static_cast<int>(g_cur->runners.size()) < RunnerCap()) StartRunner(i);
            return;
        }
        if (static_cast<int>(g_cur->blobs.size()) >= BlobCap()) return;
        Blob b{ x, y, vol, stretch, ang, U(0.0f, 6.283f) };
        g_cur->blobs.push_back(b);
        DrawBlob(b);
    }

    void ReleaseHeavyBlobs()
    {
        if (!g_cur->drip) return;
        for (std::size_t i = g_cur->blobs.size(); i-- > 0;) {
            if (static_cast<int>(g_cur->runners.size()) >= RunnerCap()) return;
            const Blob& b = g_cur->blobs[i];
            if (g_s.t < b.holdUntil) continue;
            const auto s = At(b.x, b.y);
            if (s.steep <= 0.04f || !s.inner) continue;
            const float ratio = Radius(b.vol) / CriticalRadiusMm(s.steep);
            if (ratio < 0.9f) continue;
            if (Chance() < std::clamp((ratio - 0.9f) * 2.0f, 0.0f, 1.0f) * 0.35f) StartRunner(i);
        }
    }

    Surf* PickSurface()
    {
        float pick = Chance();
        for (auto& surf : g_surfs) {
            if (pick < surf.weight) return &surf;
            pick -= surf.weight;
        }
        return &g_surfs.back();
    }

    DripSim::IHost* g_host = nullptr;

    // The upper front of the torso, not the arms: the body's atlas cannot tell them apart, so this shoots horizontal rays at the live, posed
    // body around AimHeight from all sides. The hits on skin that faces forward (standing pose) show which way the body faces now; a hit counts
    // when it came from within ~50 degrees of that front, which leaves out the arms at the sides.
    bool AimTorso(const Surf& body, float& ox, float& oy)
    {
        float lo[3];
        float hi[3];
        if (!g_host || !g_host->BodyBounds(lo, hi)) return false;
        const float cx = (lo[0] + hi[0]) * 0.5f;
        const float cy = (lo[1] + hi[1]) * 0.5f;
        const float zSpan = hi[2] - lo[2];
        const float reach = std::max(hi[0] - lo[0], hi[1] - lo[1]) + 50.0f;
        struct Cand
        {
            float x, y, azimuth;
            bool front;
        };
        std::vector<Cand> cands;
        float fx = 0.0f;
        float fy = 0.0f;
        for (int i = 0; i < 64; ++i) {
            const float th = 6.283f * (static_cast<float>(i) + U(0.0f, 1.0f)) / 64.0f;
            const float z = lo[2] + zSpan * (g_cfg.aimHeight + U(-1.0f, 1.0f) * g_cfg.aimSpread);
            const float from[3] = { cx + std::cos(th) * reach, cy + std::sin(th) * reach, z };
            const float dir[3] = { -std::cos(th), -std::sin(th), 0.0f };
            DripSim::Hit hit{};
            if (!g_host->Raycast(from, dir, reach * 2.0f, &hit)) continue;
            const int px = static_cast<int>(hit.x);
            const int py = static_cast<int>(hit.y);
            if (!body.field->Inner(px, py)) continue;
            const auto tri = body.field->TriAt(px, py);
            if (tri < 0) continue;
            const float ny = body.field->NormalY(tri);
            if (ny > 0.5f) {  // clearly the front: its world normal points where the body faces
                fx += hit.normal[0];
                fy += hit.normal[1];
            }
            cands.push_back({ hit.x, hit.y, th, ny > 0.2f });
        }
        const float fl = std::hypot(fx, fy);
        if (fl < 1e-4f) return false;
        const float facing = std::atan2(fy, fx);
        std::vector<const Cand*> good;
        for (const auto& c : cands) {
            if (c.front && std::abs(std::remainder(c.azimuth - facing, 6.283f)) < 0.87f) good.push_back(&c);
        }
        if (good.empty()) return false;
        const Cand* c = good[static_cast<std::size_t>(U(0.0f, static_cast<float>(good.size()) - 0.001f))];
        ox = c->x;
        oy = c->y;
        return true;
    }

    // where a splash is aimed: the face (FaceShare), otherwise the upper front of the torso; skin that faces the front
    bool FindAim(Surf*& surf, float& ox, float& oy)
    {
        for (int pickSurf = 0; pickSurf < 4; ++pickSurf) {
            surf = PickSurface();
            g_cur = surf;
            if (surf->name == "body" && AimTorso(*surf, ox, oy)) return true;
            const auto* f = surf->field;
            const int size = f->Size();
            const bool body = surf->name == "body";
            const float zSpan = std::max(f->ZMax() - f->ZMin(), 1e-3f);
            for (int attempt = 0; attempt < 600; ++attempt) {
                const float x = U(0.0f, static_cast<float>(size - 1));
                const float y = U(0.0f, static_cast<float>(size - 1));
                if (!f->Inner(static_cast<int>(x), static_cast<int>(y))) continue;
                const auto tri = f->TriAt(static_cast<int>(x), static_cast<int>(y));
                if (tri < 0) continue;
                float accept = std::max(f->NormalY(tri), 0.0f);  // the front faces the spray
                if (body) {
                    const float zn = (f->CenterZ(tri) - f->ZMin()) / zSpan;
                    const float d = (zn - g_cfg.aimHeight) / g_cfg.aimSpread;
                    accept *= std::exp(-d * d);
                }
                if (Chance() > accept) continue;
                ox = x;
                oy = y;
                return true;
            }
        }
        return false;
    }

    int RandInt(int lo, int hi) { return std::uniform_int_distribution<int>(lo, std::max(lo, hi))(g_rng); }

    // A splash lands: GlobsMin..GlobsMax big globs close together around one aim point, each with its own fingers to grow.
    void SpawnSplash(float strength)
    {
        Surf* surf = nullptr;
        float ax = 0.0f;
        float ay = 0.0f;
        if (!FindAim(surf, ax, ay)) return;
        g_cur = surf;
        const auto* f = surf->field;
        const auto centre = At(ax, ay);
        const int n = RandInt(g_cfg.globsMin, g_cfg.globsMax);
        const float str = 0.6f + 0.4f * std::min(strength, 1.5f);
        for (int i = 0; i < n; ++i) {
            float x = ax;
            float y = ay;
            for (int attempt = 0; attempt < 12; ++attempt) {  // around the aim point, the first one right on it
                const float distMm = i == 0 ? 0.0f : g_cfg.splashSpread * std::min(std::abs(Gauss()) * 0.5f, 1.0f);
                const float dir = U(0.0f, 6.283f);
                const float px = ax + std::cos(dir) * distMm / centre.mmpx;
                const float py = ay + std::sin(dir) * distMm / centre.mmpx;
                if (!f->Inner(static_cast<int>(px), static_cast<int>(py))) continue;
                x = px;
                y = py;
                break;
            }
            const auto s = At(x, y);
            const float rMm = g_cfg.globSize * U(0.75f, 1.25f) * g_cfg.size * str * (surf->name == "face" ? g_cfg.faceSize : 1.0f);
            Glob g;
            g.surf = static_cast<std::size_t>(surf - g_surfs.data());
            g.x = x;
            g.y = y;
            g.r0 = rMm / std::max(s.mmpx, 1e-3f);
            g.stretch = U(1.0f, 1.5f);
            g.ang = U(0.0f, 6.283f);
            g.lobe = U(0.0f, 6.283f);
            g.dur = g_cfg.spreadTime * U(0.8f, 1.2f);  // the impact: fast, whatever the viscosity
            g.hold = g_cfg.holdTime * U(0.75f, 1.25f);
            if (surf->drip && s.steep > 0.1f) {
                const float dl = std::hypot(s.dx, s.dy);
                if (dl > 1e-4f) {
                    const float k = std::min(s.steep * 1.3f, 1.0f) / dl;
                    g.sagX = s.dx * k;
                    g.sagY = s.dy * k;
                }
            }
            const int fingers = RandInt(g_cfg.fingersMin, g_cfg.fingersMax);
            const float base = U(0.0f, 6.283f);
            for (int k = 0; k < fingers; ++k) {  // spread round the glob, not bunched up
                const float ang = base + 6.283f * (static_cast<float>(k) + U(-0.3f, 0.3f)) / static_cast<float>(std::max(fingers, 1));
                g.fingers.push_back({ ang, g_cfg.fingerLength * U(0.5f, 1.5f), g_cfg.fingerWidth * U(0.8f, 1.25f), U(-0.4f, 0.4f), false });
            }
            // where the skin may drip, the finger nearest to downhill becomes the long one that turns into a streak
            if (surf->drip && s.steep > 0.1f && (s.dx != 0.0f || s.dy != 0.0f)) {
                const float down = std::atan2(s.dy, s.dx);
                Finger* best = nullptr;
                float bestDiff = 10.0f;
                for (auto& fg : g.fingers) {
                    const float diff = std::abs(std::remainder(fg.ang - down, 6.283f));
                    if (diff < bestDiff) bestDiff = diff, best = &fg;
                }
                if (!best || bestDiff > 0.8f) {
                    g.fingers.push_back({ down, 0.0f, 0.0f, 0.0f, true });
                    best = &g.fingers.back();
                }
                best->ang = down;
                best->bend = 0.0f;
                best->len = g_cfg.fingerLength * U(1.5f, 2.5f) * std::min(s.steep * 1.5f, 1.0f);
                best->width = g_cfg.fingerWidth * U(0.85f, 1.1f);
                best->down = true;
            }
            // a few small droplets thrown off as it hit
            const int spat = RandInt(0, g_cfg.spatters);
            for (int k = 0; k < spat; ++k) {
                const float dir = U(0.0f, 6.283f);
                const float dist = g.r0 * U(1.5f, 2.8f);
                const float px = x + std::cos(dir) * dist;
                const float py = y + std::sin(dir) * dist;
                if (f->Inner(static_cast<int>(px), static_cast<int>(py))) Land(px, py, rMm * U(0.12f, 0.25f), U(1.0f, 1.8f), dir);
            }
            g_globs.push_back(std::move(g));
        }
        logger::info("Cream: splash {:.2f} on {}: {} globs", strength, surf->name, n);
    }

    // One glob at spread e (0 as it landed .. 1 done): a core that widens and flattens (same volume over a bigger area), a second lobe for an
    // uneven outline, and the fingers grown to e of their length, each ending in a little bulb where the cream gathers.
    void DrawGlob(const Glob& g, float e, float sag, bool permanent)
    {
        g_cur = &g_surfs[g.surf];
        const auto* f = g_cur->field;
        const float grow = 1.0f + (g_cfg.spread - 1.0f) * e;
        const float r = g.r0 * grow;
        const float h = std::max(1.35f / (1.0f + 0.5f * (grow * grow - 1.0f)), 0.75f);
        const float k = std::sqrt(g.stretch);
        g_cur->layer->Stamp(g.x, g.y, r * k, r / k, g.ang, h, 0.5f, permanent);
        if (sag > 0.0f && (g.sagX != 0.0f || g.sagY != 0.0f)) {
            // the heavy lower part slumps: a second, slightly smaller body further downhill, joined to the first
            const float off = sag * g.r0;
            const float sx = g.x + g.sagX * off;
            const float sy = g.y + g.sagY * off;
            if (f->Inner(static_cast<int>(sx), static_cast<int>(sy))) {
                const float da = std::atan2(g.sagY, g.sagX);
                g_cur->layer->Stamp((g.x + sx) * 0.5f, (g.y + sy) * 0.5f, r * 0.85f + off * 0.5f, r * 0.8f, da, h * 0.97f, 0.5f, permanent);
                g_cur->layer->Stamp(sx, sy, r * 0.8f, r * 0.75f, da, h, 0.5f, permanent);
            }
        }
        g_cur->layer->Stamp(g.x + std::cos(g.lobe) * r * 0.45f, g.y + std::sin(g.lobe) * r * 0.45f, r * 0.75f, r * 0.6f, g.lobe, h * 0.95f, 0.5f, permanent);
        for (const auto& fg : g.fingers) {
            const float len = fg.len * g.r0 * e;
            const float w0 = fg.width * g.r0;
            if (len < 0.5f || w0 < 0.5f) continue;
            const int pieces = std::max(2, static_cast<int>(std::ceil(len / (0.35f * w0))));
            float px = g.x;
            float py = g.y;
            for (int i = 1; i <= pieces; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(pieces);
                const float a = fg.ang + fg.bend * t;
                const float d = r * 0.6f + len * t;
                const float nx = g.x + std::cos(a) * d;
                const float ny = g.y + std::sin(a) * d;
                if (!f->Inner(static_cast<int>(nx), static_cast<int>(ny))) break;
                px = nx;
                py = ny;
                const float w = w0 * (1.0f - 0.35f * t) * grow;
                g_cur->layer->Stamp(px, py, w, w * 0.85f, a, h * (1.0f - 0.2f * t), 0.5f, permanent);
            }
            const float tip = w0 * 0.85f * grow;
            g_cur->layer->Stamp(px, py, tip, tip, 0.0f, h, 0.5f, permanent);
        }
    }

    // where a finger's tip is now (the end of its last stamp on the skin)
    void FingerTip(const Glob& g, const Finger& fg, float& tx, float& ty)
    {
        const float r = g.r0 * g_cfg.spread;
        const float a = fg.ang + fg.bend;
        tx = g.x + std::cos(a) * (r * 0.6f + fg.len * g.r0);
        ty = g.y + std::sin(a) * (r * 0.6f + fg.len * g.r0);
        if (!g_surfs[g.surf].field->Inner(static_cast<int>(tx), static_cast<int>(ty))) tx = g.x, ty = g.y;
    }

    // The fresh globs: the impact splays them out (fast, then slowing), the hold lets them widen a little more and sag; then each is stamped
    // for good and its cream counts as blobs (its middle, its sagged lower part and the tip of its downhill finger), which, on skin that may
    // drip, creep on as streaks when they are heavy.
    void UpdateGlobs(float dt)
    {
        for (std::size_t i = 0; i < g_globs.size();) {
            Glob& g = g_globs[i];
            g.age += dt;
            const float impact = 1.0f - g_cfg.holdSpread;  // the share of the spreading the impact does
            float e;
            float sag = 0.0f;
            if (g.age < g.dur) {
                const float t = g.age / g.dur;
                e = impact * (1.0f - (1.0f - t) * (1.0f - t));  // fast at first, slowing down
            } else {
                const float u = std::min((g.age - g.dur) / std::max(g.hold, 1e-3f), 1.0f);
                const float slow = 1.0f - (1.0f - u) * (1.0f - u);
                e = impact + (1.0f - impact) * slow;  // carries on where the impact left off, ever slower
                sag = g_cfg.holdSag * u * u;          // the slump starts gently and grows
            }
            if (g.age < g.dur + g.hold) {
                DrawGlob(g, e, sag, false);
                ++i;
                continue;
            }
            sag = g_cfg.holdSag;
            DrawGlob(g, 1.0f, sag, true);
            g_cur = &g_surfs[g.surf];
            const auto s = At(g.x, g.y);
            const double hold = g_s.t;  // the hold is over: from now on it may creep
            const float rMid = g.r0 * g_cfg.spread * s.mmpx * 0.45f;
            const float mx = g.x + g.sagX * sag * g.r0;
            const float my = g.y + g.sagY * sag * g.r0;
            const bool sagged = (g.sagX != 0.0f || g.sagY != 0.0f) && sag > 0.0f && g_cur->field->Inner(static_cast<int>(mx), static_cast<int>(my));
            if (static_cast<int>(g_cur->blobs.size()) < BlobCap()) {
                g_cur->blobs.push_back({ sagged ? mx : g.x, sagged ? my : g.y, rMid * rMid, 1.0f, 0.0f, U(0.0f, 6.283f), hold });
            }
            for (const auto& fg : g.fingers) {
                if (!fg.down) continue;
                float bx = g.x;
                float by = g.y;
                FingerTip(g, fg, bx, by);
                const float rTip = fg.width * g.r0 * s.mmpx * 1.1f;
                if (static_cast<int>(g_cur->blobs.size()) < BlobCap()) g_cur->blobs.push_back({ bx, by, rTip * rTip, 1.0f, 0.0f, U(0.0f, 6.283f), hold + 0.5 });
            }
            g_globs.erase(g_globs.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }

    // where a pour lands on a surface: the highest skin there that faces up now (a little random, so two pours do not hit the same spot)
    void ChoosePourPoint(Surf& surf)
    {
        surf.pourX = surf.pourY = -1.0f;
        if (surf.name != "body" && surf.name != "face") return;
        g_cur = &surf;
        const auto* f = surf.field;
        const int size = f->Size();
        float best = -1e30f;
        for (int i = 0; i < 1500; ++i) {
            const float x = U(0.0f, static_cast<float>(size - 1));
            const float y = U(0.0f, static_cast<float>(size - 1));
            if (!f->Inner(static_cast<int>(x), static_cast<int>(y))) continue;
            const auto tri = f->TriAt(static_cast<int>(x), static_cast<int>(y));
            if (tri < 0) continue;
            const auto s = At(x, y);
            if (s.up < 0.3f) continue;
            const float score = f->CenterZ(tri) + (f->ZMax() - f->ZMin()) * (0.04f * U(-1.0f, 1.0f) + 0.03f * s.up + 0.03f * f->NormalY(tri));
            if (score > best) {
                best = score;
                surf.pourX = x;
                surf.pourY = y;
            }
        }
    }

    void PourOn(Surf& surf, int n)
    {
        if (surf.pourX < 0.0f) return;
        g_cur = &surf;
        for (int i = 0; i < n; ++i) {
            const auto s = At(surf.pourX, surf.pourY);
            const float spread = g_cfg.pourSpread / std::max(s.mmpx, 1e-3f);
            const float x = surf.pourX + Gauss() * spread;
            const float y = surf.pourY + Gauss() * spread;
            if (!surf.field->Inner(static_cast<int>(x), static_cast<int>(y))) continue;
            Land(x, y, U(1.6f, 3.2f) * g_cfg.size * (0.7f + 0.3f * g_s.pourStr), 1.0f, 0.0f);
        }
    }

    void StepRunner(Runner& r)
    {
        const auto s = At(r.x, r.y);
        const float rMm = Radius(r.vol);
        // it slows as it sets; when it hardly moves any more, or the skin flattens out or ends, the rest of it stays as a hanging bulb
        const float setting = 1.0f / (1.0f + r.age / (6.0f / g_cfg.viscosity));
        const float pulse = (std::sin(r.age * 2.1f + r.ph) + 1.0f) * 0.5f;  // a slow, uneven creep
        const float vmm = 1.6f * rMm * s.steep * setting * (0.45f + 0.55f * pulse) / g_cfg.viscosity;  // a few mm per second at most
        const bool stop = !s.inner || s.steep < 0.05f || r.vol < 0.8f || (r.age > 1.5f && vmm < 0.35f) || r.age > 25.0f;
        if (stop) {
            if (s.inner && r.vol >= 0.3f && static_cast<int>(g_cur->blobs.size()) < BlobCap()) {
                Blob b{ r.x, r.y, std::max(r.vol, 0.6f) * 1.2f, 1.2f, std::atan2(r.vy, r.vx), std::atan2(r.vy, r.vx) };
                g_cur->blobs.push_back(b);
                DrawBlob(b);
            }
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
        r.vx = 0.88f * r.vx + 0.12f * gx;  // heavy: turns slowly
        r.vy = 0.88f * r.vy + 0.12f * gy;
        const float nn = std::max(std::hypot(r.vx, r.vy), 1e-6f);
        r.vx /= nn;
        r.vy /= nn;
        const float stepMm = vmm * kStep;
        const float stepPx = stepMm / s.mmpx;
        r.x += r.vx * stepPx;
        r.y += r.vy * stepPx;
        r.vol -= 0.07f * rMm * stepMm;  // a thick streak stays behind on the skin
        // the streak: wide and nearly as thick as the head, permanent (it does not dry like water)
        const float tr = std::max(rMm * 0.62f / s.mmpx, 0.9f);
        const int pieces = std::max(1, static_cast<int>(std::ceil(stepPx / (0.4f * tr))));
        for (int i = 1; i <= pieces; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(pieces);
            const float wob = 1.0f + 0.12f * std::sin((r.age + t * kStep) * 9.0f + r.ph);
            g_cur->layer->Stamp(r.x - r.vx * stepPx * (1.0f - t), r.y - r.vy * stepPx * (1.0f - t), tr * wob, tr * wob, 0, 0.8f, 0.5f, true);
        }
        // blobs on the way flow into it
        for (std::size_t i = 0; i < g_cur->blobs.size();) {
            const Blob& b = g_cur->blobs[i];
            if (std::hypot(b.x - r.x, b.y - r.y) * s.mmpx < (Radius(b.vol) + rMm) * 0.75f) {
                r.vol += b.vol * 0.5f;  // half of it joins, the rest stays as a smear
                g_cur->blobs.erase(g_cur->blobs.begin() + static_cast<std::ptrdiff_t>(i));
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
        g_cur->layer->Stamp(r.x, r.y, rh * 1.2f, rh * 1.0f, ang, 1.35f, 0.5f, false);  // the round, heavy head
        g_cur->layer->Stamp(r.x - r.vx * rh * 1.1f, r.y - r.vy * rh * 1.1f, rh * 0.9f, rh * 0.75f, ang, 1.0f, 0.5f, false);
    }

    // a request from outside (key, mod event), taken over on the next Start / Update
    struct Request
    {
        std::atomic<float> splash{ 0.0f };
        std::atomic<float> pour{ 0.0f };
    };
    Request g_request;

    void TakeRequests()
    {
        const float splash = g_request.splash.exchange(0.0f);
        const float pour = g_request.pour.exchange(0.0f);
        if (splash > 0.0f) {
            g_s.splashStr = splash;
            SpawnSplash(splash);
        }
        if (pour > 0.0f) {
            g_s.pourStr = pour;
            if (g_s.pourLeft <= 0.0f) {
                for (auto& surf : g_surfs) ChoosePourPoint(surf);
            }
            g_s.pourLeft += g_cfg.pourTime * pour;
            logger::info("Cream: pour {:.2f} for {:.1f} s", pour, g_s.pourLeft);
        }
        if (splash > 0.0f || pour > 0.0f) {
            g_s.since = 0;
            if (g_s.firstAt < 0.0) g_s.firstAt = g_s.t;
            if (g_s.alpha < 1.0f) {
                g_s.alpha = 1.0f;
                for (auto& surf : g_surfs) surf.layer->SetAlpha(1.0f);
            }
        }
    }

    class CreamMode final : public DripSim::IMode
    {
    public:
        const char* Name() const override { return "cream"; }

        bool Start(DripSim::IHost* host, std::int32_t variant) override
        {
            g_host = host;
            LoadSettings();
            g_surfs.clear();
            float total = 0.0f;
            float face = 0.0f;
            for (std::int32_t i = 0; i < host->SurfaceCount(); ++i) {
                Surf surf;
                surf.name = host->SurfaceName(i);
                surf.layer = host->SurfaceLayer(i);
                surf.field = host->SurfaceField(i);
                if (!surf.layer || !surf.field) continue;
                surf.drip = surf.name == "body" ? g_cfg.dripBody : surf.name == "hands" ? g_cfg.dripHands : surf.name == "feet" ? g_cfg.dripFeet : g_cfg.dripFace;
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
                if (surf.name == "face") face = surf.weight;
                else total += surf.weight;
                g_surfs.push_back(std::move(surf));
            }
            if (g_surfs.empty() || total <= 0.0f) return false;
            // the splash share: the face gets FaceShare (it faces the spray and is small), the rest by skin area
            const float faceShare = face > 0.0f ? g_cfg.faceShare : 0.0f;
            std::string names;
            for (auto& surf : g_surfs) {
                surf.weight = surf.name == "face" ? faceShare : surf.weight / total * (1.0f - faceShare);
                names += std::format("{}{} {:.0f}%", names.empty() ? "" : ", ", surf.name, surf.weight * 100.0f);
            }
            g_s = State{};
            g_globs.clear();
            g_rng.seed(std::random_device{}());
            g_s.warmth = g_cfg.warmth * U(0.0f, 1.0f) * U(0.0f, 1.0f);  // mostly near white, now and then noticeably yellowish
            ApplyLook();
            // started by a key or event: its request is waiting; started some other way (console, preview): do what the variant says
            if (g_request.splash.load() <= 0.0f && g_request.pour.load() <= 0.0f) CreamRequest(variant == 2 ? CreamKind::Pour : CreamKind::Splash, 1.0f);
            TakeRequests();
            logger::info("Cream: surfaces {}", names);
            host->Notify(variant == 2 ? "DripSim cream: poured over you" : "DripSim cream: splashed");
            return true;
        }

        void Update(DripSim::IHost* host, float dt) override
        {
            g_s.t += dt;
            g_s.since += dt;
            ++g_s.frames;
            TakeRequests();
            g_s.lookAcc += dt;
            if (g_s.lookAcc >= 1.0f) {  // the slow thinning out
                g_s.lookAcc = 0.0f;
                ApplyLook();
            }

            if (!g_globs.empty()) {
                UpdateGlobs(dt);
                g_s.since = 0;
            }
            if (g_s.pourLeft > 0.0f) {
                g_s.pourLeft -= dt;
                g_s.pourAcc += g_cfg.pourRate * g_s.pourStr * dt;
                const int n = static_cast<int>(g_s.pourAcc);
                g_s.pourAcc -= static_cast<float>(n);
                // the stream falls on the face first (when it is there and gets some), the rest runs onto the body
                for (auto& surf : g_surfs) {
                    if (surf.pourX < 0.0f) continue;
                    const bool hasFace = std::ranges::any_of(g_surfs, [](const Surf& s) { return s.name == "face" && s.pourX >= 0.0f; });
                    const float share = !hasFace ? 1.0f : surf.name == "face" ? 0.3f : 0.7f;  // little on the face: it does not run there
                    PourOn(surf, static_cast<int>(std::round(static_cast<float>(n) * share + Chance() - 0.5f)));
                }
                g_s.since = 0;
            }

            g_s.checkAcc += dt;
            const bool check = g_s.checkAcc >= 0.25f;
            if (check) g_s.checkAcc = 0.0f;
            std::size_t blobs = 0;
            std::size_t runners = 0;
            for (auto& surf : g_surfs) {
                g_cur = &surf;
                if (check) ReleaseHeavyBlobs();
                for (auto& r : g_cur->runners) {
                    r.acc += dt;
                    while (r.vol > 0 && r.acc >= kStep) {
                        r.acc -= kStep;
                        r.age += kStep;
                        StepRunner(r);
                    }
                }
                std::erase_if(g_cur->runners, [](const Runner& r) { return r.vol <= 0.0f; });
                for (const auto& r : g_cur->runners) DrawRunner(r);
                blobs += g_cur->blobs.size();
                runners += g_cur->runners.size();
            }
            if (runners > 0) g_s.since = std::min(g_s.since, 1.0);

            // it stays, then fades out as a whole and the mode ends
            if (g_cfg.stayTime > 0.0f && g_s.since > g_cfg.stayTime) {
                g_s.alpha = std::max(0.0f, 1.0f - static_cast<float>(g_s.since - g_cfg.stayTime) / g_cfg.fadeTime);
                for (auto& surf : g_surfs) surf.layer->SetAlpha(g_s.alpha);
                if (g_s.alpha <= 0.0f) host->Finish();
            }
            if (g_s.frames % 600 == 0) logger::info("Cream: t={:.0f}s blobs {} runners {} since {:.0f}s", g_s.t, blobs, runners, g_s.since);
        }

        void Stop(DripSim::IHost*, DripSim::StopReason) override
        {
            logger::info("Cream: stopped after {:.0f} s", g_s.t);
            g_s = State{};
            g_surfs.clear();
            g_globs.clear();
            g_cur = nullptr;
            g_host = nullptr;
            g_request.splash.store(0.0f);
            g_request.pour.store(0.0f);
        }

        void Idle(DripSim::IDripSim*, float) override {}  // only ever started on request
        void Reload() override
        {
            LoadSettings();
            if (!g_surfs.empty()) ApplyLook();
        }
        std::int32_t Surfaces() const override
        {
            return (g_cfg.face ? DripSim::kSurfaceFace : 0) | (g_cfg.hands ? DripSim::kSurfaceHands : 0) | (g_cfg.feet ? DripSim::kSurfaceFeet : 0);
        }
    };

    CreamMode g_mode;
}

DripSim::IMode* CreamModeInstance()
{
    return &g_mode;
}

void CreamLoadSettings()
{
    LoadSettings();
}

void CreamRequest(CreamKind kind, float strength)
{
    strength = std::clamp(strength, 0.05f, 1.0f);
    auto& slot = kind == CreamKind::Pour ? g_request.pour : g_request.splash;
    float old = slot.load();
    while (!slot.compare_exchange_weak(old, std::min(old + strength, 2.0f))) {}
}
