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
// Splash (variant 1, default): a short spray of blobs and spatters over the front of the body and the face.
// Pour (variant 2): a thick stream onto the highest skin that faces up (top of the head / forehead, shoulders, chest), which then runs down.
namespace
{
    constexpr float kStep = 0.04f;  // runner time step (s)

    struct Settings
    {
        float splashBlobs = 34.0f;   // blobs per splash at strength 1
        float splashTime = 0.5f;     // seconds a splash lasts
        float pourRate = 38.0f;      // blobs per second while pouring (strength 1)
        float pourTime = 3.0f;       // seconds a pour lasts (strength 1)
        float pourSpread = 7.0f;     // mm: how wide the stream lands around the pour point
        float faceShare = 0.35f;     // share of a splash that lands on the face (when the face surface is there)
        float aimHeight = 0.72f;     // splash on the body: where it is aimed, 0 feet .. 1 top of the head
        float aimSpread = 0.22f;     // how far around that height it spreads
        float size = 1.0f;           // multiplies blob sizes
        float viscosity = 1.0f;      // higher = holds on steeper skin, creeps slower and not as far
        float opacity = 1.0f;        // multiplies the body opacity of the thick cream
        float gloss = 1.0f;          // highlights on the blobs
        float normals = 2.2f;        // the film's normal map strength (0 = none; needs NormalMaps=1 in DripSim.ini): lumpy, glossy relief
        float sheen = 1.4f;          // the skin's specular strength under the cream; 0 = off
        float shininess = 65.0f;     // gloss exponent there: lower than water (130), a broader, softer satin highlight
        float stayTime = 600.0f;     // seconds the cream stays after the last splash / pour (0 = until cleaned)
        float fadeTime = 20.0f;      // seconds it takes to fade out after that
        float color[3] = { 246.0f, 243.0f, 234.0f };  // the thick cream (a slightly warm white)
        int maxBlobs = 900;
        int maxRunners = 50;
        bool face = true;
        bool hands = true;
        bool feet = false;
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
        v.splashBlobs = ini.Float("SplashBlobs", v.splashBlobs);
        v.splashTime = std::max(ini.Float("SplashTime", v.splashTime), 0.05f);
        v.pourRate = ini.Float("PourRate", v.pourRate);
        v.pourTime = ini.Float("PourTime", v.pourTime);
        v.pourSpread = ini.Float("PourSpread", v.pourSpread);
        v.faceShare = std::clamp(ini.Float("FaceShare", v.faceShare), 0.0f, 1.0f);
        v.aimHeight = ini.Float("AimHeight", v.aimHeight);
        v.aimSpread = std::max(ini.Float("AimSpread", v.aimSpread), 0.02f);
        v.size = ini.Float("Size", v.size);
        v.viscosity = std::max(ini.Float("Viscosity", v.viscosity), 0.1f);
        v.opacity = ini.Float("Opacity", v.opacity);
        v.gloss = ini.Float("Gloss", v.gloss);
        v.normals = ini.Float("NormalStrength", v.normals);
        v.sheen = ini.Float("Sheen", v.sheen);
        v.shininess = ini.Float("Shininess", v.shininess);
        v.stayTime = ini.Float("StayTime", v.stayTime);
        v.fadeTime = std::max(ini.Float("FadeTime", v.fadeTime), 0.5f);
        ReadColor(ini, "Color", v.color);
        v.maxBlobs = ini.Int("MaxBlobs", v.maxBlobs);
        v.maxRunners = ini.Int("MaxRunners", v.maxRunners);
        v.face = ini.Bool("Face", v.face);
        v.hands = ini.Bool("Hands", v.hands);
        v.feet = ini.Bool("Feet", v.feet);
        g_cfg = v;
        logger::info("Cream: settings: splash {:.0f} blobs pour {:.0f}/s for {:.1f}s size {:.2f} viscosity {:.2f} opacity {:.2f} gloss {:.2f} stay {:.0f}s", v.splashBlobs, v.pourRate, v.pourTime,
            v.size, v.viscosity, v.opacity, v.gloss, v.stayTime);
    }

    struct Blob
    {
        float x, y;
        float vol;      // mm^2 (r = sqrt(vol))
        float stretch;  // >= 1: drawn out along ang (a spatter that hit at a slant)
        float ang;
        float lobe;     // direction of the second, smaller lobe (irregular outline)
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
        float splashLeft = 0;   // blobs of the current splash still to come
        float splashRate = 0;   // ... per second
        float splashStr = 1;
        float pourLeft = 0;     // seconds of pouring left
        float pourStr = 1;
        float spawnAcc = 0;
        float pourAcc = 0;
        float checkAcc = 0;
        float alpha = 1;
        std::uint64_t frames = 0;
    };
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
        const float film[3] = { 228.0f, 226.0f, 220.0f };
        const float rim[3] = { 255.0f, 255.0f, 252.0f };
        const float halo[3] = { 60.0f, 48.0f, 40.0f };
        for (int i = 0; i < 3; ++i) {
            look.bodyColor[i] = film[i];
            look.coreColor[i] = g_cfg.color[i];
            look.rimColor[i] = rim[i];
            look.haloColor[i] = halo[i];
        }
        look.bodyAlpha = std::clamp(0.94f * g_cfg.opacity, 0.0f, 1.0f);
        look.filmAlpha = std::clamp(0.34f * g_cfg.opacity, 0.0f, 1.0f);  // thin cream: milky, the skin shows through a little
        look.creamStart = 0.16f;                                          // above this thickness the opaque body comes in (full ~0.5 above)
        look.rimAlpha = 0.32f;
        look.rimDirection = 0.45f;  // the lit side of a blob's edge shines more, but the rim goes most of the way round (a thick, rounded edge)
        look.haloAlpha = 0.03f;
        look.hole = 0.22f;
        look.glint = 0.9f * g_cfg.gloss;
        look.mottle = 0.4f;     // a little unevenness in the white, so large areas do not look like paint
        look.brightness = 1.0f;
        look.shade = 0.5f;      // a soft dome: the side away from the light a touch greyer
        look.smooth = 0.55f;    // overlapping blobs flow together into one surface
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
            if (Radius(b.vol + vol) > 7.0f) continue;  // a puddle that big on flat skin: the new cream lies beside it instead
            const float total = b.vol + vol;
            b.x = (b.x * b.vol + x * vol) / total;
            b.y = (b.y * b.vol + y * vol) / total;
            b.vol = total;
            b.stretch = 1.0f + (b.stretch - 1.0f) * 0.5f;
            DrawBlob(b);  // stamps are max-blended: the old shape stays under the grown one
            const auto here = At(b.x, b.y);
            if (here.steep > 0.04f && here.inner && Radius(b.vol) >= CriticalRadiusMm(here.steep) && static_cast<int>(g_cur->runners.size()) < RunnerCap()) StartRunner(i);
            return;
        }
        if (static_cast<int>(g_cur->blobs.size()) >= BlobCap()) return;
        Blob b{ x, y, vol, stretch, ang, U(0.0f, 6.283f) };
        g_cur->blobs.push_back(b);
        DrawBlob(b);
    }

    void ReleaseHeavyBlobs()
    {
        for (std::size_t i = g_cur->blobs.size(); i-- > 0;) {
            if (static_cast<int>(g_cur->runners.size()) >= RunnerCap()) return;
            const Blob& b = g_cur->blobs[i];
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

    // one hit of a splash: a blob drawn out along the way it was flying, and a few small spatters thrown on ahead of it
    void SplashHit()
    {
        g_cur = PickSurface();
        const auto* f = g_cur->field;
        const int size = f->Size();
        const bool body = g_cur->name == "body";
        const float zSpan = std::max(f->ZMax() - f->ZMin(), 1e-3f);
        for (int attempt = 0; attempt < 40; ++attempt) {
            const float x = U(0.0f, static_cast<float>(size - 1));
            const float y = U(0.0f, static_cast<float>(size - 1));
            if (!f->Inner(static_cast<int>(x), static_cast<int>(y))) continue;
            const auto tri = f->TriAt(static_cast<int>(x), static_cast<int>(y));
            if (tri < 0) continue;
            float accept = std::max(f->NormalY(tri), 0.0f) + 0.03f;  // the front faces the spray
            if (body) {
                const float zn = (f->CenterZ(tri) - f->ZMin()) / zSpan;
                const float d = (zn - g_cfg.aimHeight) / g_cfg.aimSpread;
                accept *= std::exp(-d * d);
            }
            if (Chance() > accept) continue;
            const auto s = At(x, y);
            const float str = 0.6f + 0.4f * g_s.splashStr;
            const float rMm = (Chance() < 0.25f ? U(2.4f, 4.0f) : U(0.9f, 2.4f)) * g_cfg.size * str;
            // the flight direction along the skin: mostly sideways / up on the front, pulled a little downhill
            const float ang = U(0.0f, 6.283f);
            float dx = std::cos(ang) + 0.4f * s.dx;
            float dy = std::sin(ang) + 0.4f * s.dy;
            const float n = std::max(std::hypot(dx, dy), 1e-4f);
            dx /= n;
            dy /= n;
            const float stretch = U(1.0f, 2.6f);
            Land(x, y, rMm, stretch, std::atan2(dy, dx));
            const int spatters = static_cast<int>(U(0.0f, 4.0f));
            float reach = rMm * std::sqrt(stretch);
            for (int i = 0; i < spatters; ++i) {
                reach += U(1.5f, 4.0f) * rMm * 0.6f;
                const float px = x + dx * reach / s.mmpx + Gauss() * 0.8f;
                const float py = y + dy * reach / s.mmpx + Gauss() * 0.8f;
                if (!f->Inner(static_cast<int>(px), static_cast<int>(py))) break;
                Land(px, py, rMm * U(0.18f, 0.4f), U(1.0f, 1.8f), std::atan2(dy, dx));
            }
            return;
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
        const float setting = 1.0f / (1.0f + r.age / (10.0f / g_cfg.viscosity));
        const float pulse = (std::sin(r.age * 2.1f + r.ph) + 1.0f) * 0.5f;  // a slow, uneven creep
        const float vmm = 6.0f * rMm * s.steep * setting * (0.45f + 0.55f * pulse) / g_cfg.viscosity;
        const bool stop = !s.inner || s.steep < 0.05f || r.vol < 0.8f || (r.age > 1.0f && vmm < 0.25f);
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
            g_s.splashLeft += g_cfg.splashBlobs * splash;
            g_s.splashRate = std::max(g_s.splashRate, g_cfg.splashBlobs * splash / g_cfg.splashTime);
            logger::info("Cream: splash {:.2f}", splash);
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
            g_rng.seed(std::random_device{}());
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

            if (g_s.splashLeft > 0.0f) {
                g_s.spawnAcc += std::min(g_s.splashRate * dt, g_s.splashLeft);
                const int n = static_cast<int>(g_s.spawnAcc);
                g_s.spawnAcc -= static_cast<float>(n);
                g_s.splashLeft -= static_cast<float>(n);
                for (int i = 0; i < n; ++i) SplashHit();
                g_s.since = 0;
                if (g_s.splashLeft < 1.0f) g_s.splashLeft = 0.0f, g_s.splashRate = 0.0f;
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
                    const float share = !hasFace ? 1.0f : surf.name == "face" ? 0.45f : 0.55f;
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
            g_cur = nullptr;
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
