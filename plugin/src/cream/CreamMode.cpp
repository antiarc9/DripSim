// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 AntiArc. This program is free software: see the LICENSE file (GNU GPL version 3 or later). It comes with NO WARRANTY.
#include "CreamMode.h"

#include <atomic>
#include <cstdio>

// Cream on skin, built on the rain mode's bead / runner model with a thick, viscous, opaque fluid instead of water.
//
// Splash (variant 1, default): a shot of a few strands from a source 50-75 cm in front of the target and below it (the face, or the upper front
// of the torso). The source stays put for the shot, the aim sweeps across the target, so the strands land along a narrow cone that comes from
// below. A strand in the air is not a drop: it has a length, a tilt against its flight, a slight bend, and its thickness varies along it.
// Every point of it is cast as a ray against the live, posed skin (face, body, hands), so it lands where it really meets the skin: a strand that
// flies end-on lands as a compact blob, a tilted one as an arc or a short thread across (over the tip of the nose, say), and whatever is behind
// a ridge is shadowed by it. Where a strand is thickest, its cream gathers into blobs. After 1-2 s they start to creep down (on skin that may
// drip): the heavier, the further. A blob leaves a streak as wide as itself that is thick and white right behind it and thins out with age, so
// it is thinnest and most see-through back where the blob started.
//
// Pour (variant 2): a thick stream onto the highest skin that faces up (top of the head / forehead, shoulders, chest), which then runs down.
// Nothing evaporates: the cream stays for StayTime seconds, then fades out (or the clean key wipes it at once).
namespace
{
    constexpr float kStep = 0.04f;    // runner time step (s)
    constexpr float kUnitMm = 13.0f;  // mm per game unit (the same scale as the field's mm per pixel)

    struct Settings
    {
        // the shot
        int strandsMin = 3;          // strands per shot: a random number from StrandsMin to StrandsMax
        int strandsMax = 5;
        float strandInterval = 0.15f;  // seconds between the strands of one shot
        float strandLength = 6.0f;   // cm: how long a strand is in the air (each 60-140 % of this)
        float strandWidth = 6.0f;    // mm: how thick a strand is on average (it varies along the strand)
        float unevenness = 0.7f;     // how much its thickness varies along it: 0 = even, 1 = thick lumps and thin necks
        float blobShare = 0.35f;     // share of strands that fly end-on and land as one compact blob; the rest land as arcs and threads
        float shotDistanceMin = 50.0f;  // cm: how far the source is from the target
        float shotDistanceMax = 75.0f;
        float shotElevationMin = 20.0f;  // degrees: how far below the target the source is (as an angle)
        float shotElevationMax = 50.0f;
        float shotSide = 35.0f;      // degrees: how far to either side of straight ahead the source may be
        float sweep = 0.6f;          // how far the aim sweeps across the target during a shot (0 = all on one spot, 1 = its whole height)
        float holdTime = 1.5f;       // seconds the landed cream sits before it starts to creep (each blob 70-130 % of this)
        float faceShare = 0.6f;      // chance that a shot goes at the face (when the face surface is there); otherwise the upper front of the torso
        float aimHeight = 0.75f;     // a shot at the torso: where it is aimed, 0 feet .. 1 top of the head (0.75 = the chest)
        // streaks
        float trailFade = 8.0f;      // seconds a streak takes to thin out behind its blob (most of it in the first third)
        float trailFloor = 0.06f;    // how thick it stays, relative to fresh (a milky, see-through film)
        // pour
        float pourRate = 38.0f;      // blobs per second while pouring (strength 1)
        float pourTime = 3.0f;       // seconds a pour lasts (strength 1)
        float pourSpread = 7.0f;     // mm: how wide the stream lands around the pour point
        // flow and look
        float size = 1.0f;           // multiplies strand and blob sizes
        float viscosity = 1.0f;      // how much it takes to set it moving: higher = only thicker blobs creep, and on steeper skin
        float flowSpeed = 0.4f;      // how fast it creeps (1 = the old, quicker cream; ~0.3-0.5 = egg white: slow but it keeps going)
        float setTime = 20.0f;       // seconds after which a creeping blob has slowed to half (higher = it keeps creeping longer)
        float flowTime = 60.0f;      // seconds a blob creeps at most
        float opacity = 1.0f;        // multiplies the body opacity of the thick cream
        float gloss = 1.0f;          // highlights on the blobs
        float normals = 1.5f;        // the film's normal map strength (0 = none; needs NormalMaps=1 in DripSim.ini): relief the game's lights see
        float sheen = 1.4f;          // the skin's specular strength under the cream; 0 = off
        float shininess = 65.0f;     // gloss exponent there: lower than water (130), a broader, softer satin highlight
        float shade = 0.2f;          // how strongly a thick blob is shaded as a dome (lit side, darker edges, a soft highlight; 0 = flat white)
        float contactShadow = 0.3f;  // the soft shadow thick cream casts on the skin around it (0 = none)
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
        bool dripBody = true;        // may it run on this surface?
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
        v.strandsMin = std::clamp(ini.Int("StrandsMin", v.strandsMin), 1, 12);
        v.strandsMax = std::clamp(ini.Int("StrandsMax", v.strandsMax), v.strandsMin, 12);
        v.strandInterval = std::clamp(ini.Float("StrandInterval", v.strandInterval), 0.0f, 2.0f);
        v.strandLength = std::max(ini.Float("StrandLength", v.strandLength), 0.5f);
        v.strandWidth = std::max(ini.Float("StrandWidth", v.strandWidth), 0.5f);
        v.unevenness = std::clamp(ini.Float("Unevenness", v.unevenness), 0.0f, 1.0f);
        v.blobShare = std::clamp(ini.Float("BlobShare", ini.Float("CompactShare", v.blobShare)), 0.0f, 1.0f);
        v.shotDistanceMin = std::max(ini.Float("ShotDistanceMin", v.shotDistanceMin), 5.0f);
        v.shotDistanceMax = std::max(ini.Float("ShotDistanceMax", v.shotDistanceMax), v.shotDistanceMin);
        v.shotElevationMin = std::clamp(ini.Float("ShotElevationMin", v.shotElevationMin), -60.0f, 80.0f);
        v.shotElevationMax = std::clamp(ini.Float("ShotElevationMax", v.shotElevationMax), v.shotElevationMin, 80.0f);
        v.shotSide = std::clamp(ini.Float("ShotSide", v.shotSide), 0.0f, 80.0f);
        v.sweep = std::clamp(ini.Float("Sweep", v.sweep), 0.0f, 1.0f);
        v.holdTime = std::max(ini.Float("HoldTime", v.holdTime), 0.0f);
        v.faceShare = std::clamp(ini.Float("FaceShare", v.faceShare), 0.0f, 1.0f);
        v.aimHeight = ini.Float("AimHeight", v.aimHeight);
        v.trailFade = std::max(ini.Float("TrailFade", v.trailFade), 0.5f);
        v.trailFloor = std::clamp(ini.Float("TrailFloor", v.trailFloor), 0.01f, 1.0f);
        v.pourRate = ini.Float("PourRate", v.pourRate);
        v.pourTime = ini.Float("PourTime", v.pourTime);
        v.pourSpread = ini.Float("PourSpread", v.pourSpread);
        v.size = ini.Float("Size", v.size);
        v.viscosity = std::max(ini.Float("Viscosity", v.viscosity), 0.1f);
        v.flowSpeed = std::max(ini.Float("FlowSpeed", v.flowSpeed), 0.01f);
        v.setTime = std::max(ini.Float("SetTime", v.setTime), 0.5f);
        v.flowTime = std::max(ini.Float("FlowTime", v.flowTime), 1.0f);
        v.opacity = ini.Float("Opacity", v.opacity);
        v.gloss = ini.Float("Gloss", v.gloss);
        v.normals = ini.Float("NormalStrength", v.normals);
        v.sheen = ini.Float("Sheen", v.sheen);
        v.shininess = ini.Float("Shininess", v.shininess);
        v.shade = std::clamp(ini.Float("Shade", v.shade), 0.0f, 1.0f);
        v.contactShadow = std::clamp(ini.Float("ContactShadow", v.contactShadow), 0.0f, 1.0f);
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
        logger::info("Cream: settings: shot {}-{} strands of {:.1f} cm x {:.1f} mm from {:.0f}-{:.0f} cm, hold {:.1f}s, pour {:.0f}/s for {:.1f}s, size {:.2f} viscosity {:.2f} flow {:.2f} set {:.0f}s stay {:.0f}s",
            v.strandsMin, v.strandsMax, v.strandLength, v.strandWidth, v.shotDistanceMin, v.shotDistanceMax, v.holdTime, v.pourRate, v.pourTime, v.size, v.viscosity, v.flowSpeed, v.setTime, v.stayTime);
    }

    struct Blob
    {
        float x, y;
        float vol;      // mm^2 (r = sqrt(vol))
        float stretch;  // >= 1: drawn out along ang
        float ang;
        float lobe;     // direction of the second, smaller lobe (irregular outline)
        double holdUntil = 0.0;  // it does not start to creep before this time (fresh cream sits for a moment)
    };

    struct Runner
    {
        float x, y;
        float vol;
        float vx = 0, vy = 1;
        float age = 0, acc = 0;
        float ph = 0;
        float lastX = -1e9f, lastY = -1e9f;  // where its last streak point was recorded
        // diagnostics: where it started, and its standing-pose height there (a streak that runs downhill loses height)
        float x0 = 0, y0 = 0, z0 = 0;
        float dx0 = 0, dy0 = 0, steep0 = 0;
    };

    // a point of a streak that is thinning out: erased a little at a time until it is down to TrailFloor
    struct TrailPt
    {
        float x, y, r;
        double born;
        int passes = 0;
    };

    struct Surf
    {
        std::string name;
        std::int32_t index = 0;  // the host's surface index (for SurfaceRaycast)
        DripSim::ILayer* layer = nullptr;
        const DripSim::IField* field = nullptr;
        float weight = 1.0f;
        bool drip = true;        // blobs here may run (DripBody / DripHands / DripFeet / DripFace)
        float pourX = -1.0f;     // where the pour lands on this surface (-1 = it gets none)
        float pourY = -1.0f;
        std::vector<Blob> blobs;
        std::vector<Runner> runners;
        std::vector<TrailPt> trail;
        float lo[3] = { 0, 0, 0 };  // world bounds, fetched for each shot (a ray that misses them cannot hit this surface)
        float hi[3] = { 0, 0, 0 };
        bool hasBounds = false;
    };
    std::vector<Surf> g_surfs;
    Surf* g_cur = nullptr;
    DripSim::IHost* g_host = nullptr;

    struct State
    {
        double t = 0;
        double since = 0;       // seconds since the last cream landed
        float pourLeft = 0;     // seconds of pouring left
        float pourStr = 1;
        float pourAcc = 0;
        float checkAcc = 0;
        float trailAcc = 0;
        float alpha = 1;
        float warmth = 0;       // this run's tint (0 white .. 1 WarmColor)
        double firstAt = -1;    // when the first cream landed (the thinning out counts from here)
        float lookAcc = 0;
        std::uint64_t frames = 0;
    };
    State g_s;
    std::mt19937 g_rng{ 11u };

    float U(float a, float b) { return std::uniform_real_distribution<float>(a, b)(g_rng); }
    float Chance() { return U(0.0f, 1.0f); }
    float Gauss() { return std::normal_distribution<float>(0.0f, 1.0f)(g_rng); }
    int RandInt(int lo, int hi) { return std::uniform_int_distribution<int>(lo, std::max(lo, hi))(g_rng); }

    // ---------------------------------------------------------------- small vector helpers (world space, game units, z up)
    struct V3
    {
        float x = 0, y = 0, z = 0;
    };
    V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
    V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
    V3 operator*(V3 a, float k) { return { a.x * k, a.y * k, a.z * k }; }
    float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    V3 Cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
    float Len(V3 a) { return std::sqrt(Dot(a, a)); }
    V3 Norm(V3 a)
    {
        const float l = Len(a);
        return l > 1e-6f ? a * (1.0f / l) : V3{ 0, 0, 1 };
    }

    DripSim::Sample At(float x, float y)
    {
        DripSim::Sample out{};
        g_cur->field->At(x, y, &out);
        return out;
    }

    float Radius(float vol) { return std::sqrt(std::max(vol, 0.01f)); }

    int BlobCap() { return std::max(60, g_cfg.maxBlobs / std::max(static_cast<int>(g_surfs.size()), 1)); }
    int RunnerCap() { return std::max(10, g_cfg.maxRunners / std::max(static_cast<int>(g_surfs.size()), 1)); }

    // a thick fluid holds on far steeper skin than water does before it creeps (water: ~0.8 mm on vertical skin; cream ~1.7 mm)
    float CriticalRadiusMm(float steep) { return std::max(4.4f / (1.0f + 1.6f * std::max(steep, 0.05f)) * std::sqrt(g_cfg.viscosity), 1.2f); }

    // Opaque, slightly warm white: a milky translucent film where it is thin (smears, an old streak), a solid body where it is thick,
    // soft satin highlights (a broad specular lobe, mild rim), dome shading and a lumpy normal map, and a soft contact shadow on the skin around it.
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
        const float halo[3] = { 38.0f, 28.0f, 24.0f };  // the contact shadow: the skin darkened next to thick cream
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
        look.haloAlpha = g_cfg.contactShadow;  // (for cream the framework draws the halo from the thick parts only, a little away from the light)
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
        g_cur->layer->Stamp(b.x + std::cos(b.lobe) * r * 0.4f, b.y + std::sin(b.lobe) * r * 0.4f, r * 0.75f, r * 0.65f, b.lobe, 1.2f, 0.5f, true);
        if (sag > 0.05f && hasDown) {
            g_cur->layer->Stamp(b.x + s.dx * r * 0.6f * sag * 2.0f, b.y + s.dy * r * 0.6f * sag * 2.0f, r * (0.75f + sag), r * 0.75f, down, 1.25f, 0.5f, true);
        }
    }

    void StartRunner(std::size_t index)
    {
        const Blob b = g_cur->blobs[index];
        g_cur->blobs.erase(g_cur->blobs.begin() + static_cast<std::ptrdiff_t>(index));
        const auto s = At(b.x, b.y);
        // the blob leaves: only a thin layer stays where it sat (the start of its streak, which thins out from there)
        const float rpx = Radius(b.vol) / s.mmpx * std::sqrt(b.stretch) * 1.25f;
        g_cur->layer->Erase(b.x, b.y, rpx, rpx, 0, 0.45f);
        Runner r;
        r.x = b.x;
        r.y = b.y;
        r.vol = b.vol * 0.85f;
        r.ph = U(0.0f, 6.283f);
        r.vx = s.dx;
        r.vy = s.dy;
        r.x0 = b.x;
        r.y0 = b.y;
        r.z0 = g_cur->field->CenterZ(g_cur->field->TriAt(static_cast<int>(b.x), static_cast<int>(b.y)));
        r.dx0 = s.dx;
        r.dy0 = s.dy;
        r.steep0 = s.steep;
        g_cur->runners.push_back(r);
    }

    // Diagnostics: a streak that ended higher (standing pose) than it started has run uphill (the gravity field is wrong there).
    void CheckClimb(const Runner& r)
    {
        const auto tri = g_cur->field->TriAt(static_cast<int>(r.x), static_cast<int>(r.y));
        if (tri < 0) return;
        const float z1 = g_cur->field->CenterZ(tri);
        if (z1 - r.z0 < 0.4f) return;  // 0.4 units ~ 5 mm
        const auto s = At(r.x, r.y);
        logger::warn("Cream: streak on {} ran UP {:.1f} units: atlas ({:.0f},{:.0f}) -> ({:.0f},{:.0f}) in {:.1f} s; field at start dir ({:.2f},{:.2f}) steep {:.2f}, at end dir ({:.2f},{:.2f}) steep {:.2f}",
            g_cur->name, z1 - r.z0, r.x0, r.y0, r.x, r.y, r.age, r.dx0, r.dy0, r.steep0, s.dx, s.dy, s.steep);
    }

    // a blob lands: it joins a blob it touches (the two flow into one), otherwise it is a new blob. hold: it may not creep before this time.
    void Land(float x, float y, float rMm, float stretch, float ang, double hold = 0.0)
    {
        const float vol = rMm * rMm;
        const auto s = At(x, y);
        for (std::size_t i = 0; i < g_cur->blobs.size(); ++i) {
            Blob& b = g_cur->blobs[i];
            if (std::hypot(b.x - x, b.y - y) * s.mmpx > (Radius(b.vol) + rMm) * 0.8f) continue;
            if (Radius(b.vol + vol) > (g_cur->drip ? 8.0f : 5.0f)) continue;  // a puddle that big: the new cream lies beside it instead
            const float total = b.vol + vol;
            b.x = (b.x * b.vol + x * vol) / total;
            b.y = (b.y * b.vol + y * vol) / total;
            b.vol = total;
            b.stretch = 1.0f + (b.stretch - 1.0f) * 0.5f;
            b.holdUntil = std::max(b.holdUntil, hold);
            DrawBlob(b);  // stamps are max-blended: the old shape stays under the grown one
            const auto here = At(b.x, b.y);
            if (g_cur->drip && g_s.t >= b.holdUntil && here.steep > 0.04f && here.inner && Radius(b.vol) >= CriticalRadiusMm(here.steep) && static_cast<int>(g_cur->runners.size()) < RunnerCap()) StartRunner(i);
            return;
        }
        if (static_cast<int>(g_cur->blobs.size()) >= BlobCap()) return;
        Blob b{ x, y, vol, stretch, ang, U(0.0f, 6.283f), hold };
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
            if (Chance() < std::clamp((ratio - 0.9f) * 2.0f, 0.0f, 1.0f) * 0.5f) StartRunner(i);
        }
    }

    Surf* Find(const char* name)
    {
        for (auto& surf : g_surfs) {
            if (surf.name == name) return &surf;
        }
        return nullptr;
    }

    // ---------------------------------------------------------------- the shot: strands cast as rays against the posed skin
    struct SkinHit
    {
        std::size_t surf;  // index into g_surfs
        DripSim::Hit hit;
    };

    // does the segment from + dir * [0, maxDist] touch the box (grown by a margin)? The raycast itself tests every triangle of a surface.
    bool TouchesBox(const float f[3], const float d[3], float maxDist, const float lo[3], const float hi[3])
    {
        float t0 = 0.0f;
        float t1 = maxDist;
        for (int k = 0; k < 3; ++k) {
            const float a = lo[k] - 2.0f;
            const float b = hi[k] + 2.0f;
            if (std::abs(d[k]) < 1e-8f) {
                if (f[k] < a || f[k] > b) return false;
                continue;
            }
            float ta = (a - f[k]) / d[k];
            float tb = (b - f[k]) / d[k];
            if (ta > tb) std::swap(ta, tb);
            t0 = std::max(t0, ta);
            t1 = std::min(t1, tb);
            if (t0 > t1) return false;
        }
        return true;
    }

    // the nearest skin a ray meets, over every surface of this run
    bool CastSkin(V3 from, V3 dir, float maxDist, SkinHit* out)
    {
        bool any = false;
        const float f[3] = { from.x, from.y, from.z };
        const float d[3] = { dir.x, dir.y, dir.z };
        for (std::size_t i = 0; i < g_surfs.size(); ++i) {
            if (g_surfs[i].hasBounds && !TouchesBox(f, d, maxDist, g_surfs[i].lo, g_surfs[i].hi)) continue;
            DripSim::Hit h{};
            if (!g_host->SurfaceRaycast(g_surfs[i].index, f, d, maxDist, &h)) continue;
            if (!any || h.dist < out->hit.dist) {
                out->surf = i;
                out->hit = h;
                any = true;
            }
        }
        return any;
    }

    // Which way a surface faces now, around height z: horizontal rays from all sides at its middle; the hits on skin that faces forward in the
    // standing pose (+y in skin space) give it by their world normals.
    bool Facing(const Surf& surf, V3 centre, float reach, V3* front)
    {
        V3 sum{};
        int n = 0;
        for (int i = 0; i < 48; ++i) {
            const float th = 6.283f * (static_cast<float>(i) + U(0.0f, 1.0f)) / 48.0f;
            const float from[3] = { centre.x + std::cos(th) * reach, centre.y + std::sin(th) * reach, centre.z + U(-0.1f, 0.1f) * reach };
            const float dir[3] = { -std::cos(th), -std::sin(th), 0.0f };
            DripSim::Hit h{};
            if (!g_host->SurfaceRaycast(surf.index, from, dir, reach * 2.0f, &h)) continue;
            const auto tri = surf.field->TriAt(static_cast<int>(h.x), static_cast<int>(h.y));
            if (tri < 0 || surf.field->NormalY(tri) < 0.5f) continue;
            sum = sum + V3{ h.normal[0], h.normal[1], 0.0f };
            ++n;
        }
        if (n < 2 || Len(sum) < 1e-4f) return false;
        *front = Norm(sum);
        return true;
    }

    // one point of a strand that met the skin
    struct Drop
    {
        std::size_t surf;
        float x, y;        // atlas
        float rMm;         // radius of the strand there
        float thick;       // its relative thickness (1 = average)
        float tx, ty;      // its travel along the skin (atlas, unit)
        float slant;       // 0 head-on .. 1 grazing
        float s;           // position along the strand (-1 .. 1)
    };

    // a strand in flight: its points meet the skin one after another within a few hundredths of a second
    struct Strand
    {
        std::vector<Drop> drops;  // in the order they land
        std::size_t landed = 0;
        float age = 0;
        float dur = 0.06f;
        float compact = 0;        // 1 = it flew end-on (all of it lands on one spot)
        float delay = 0;          // seconds until it arrives (the strands of a shot come one after another)
    };
    std::vector<Strand> g_strands;

    // The thickness along a strand (1 = its average): an uneven base, two or three lumps, now and then a thin neck, and sometimes thicker ends
    // (a strand that is thick at both ends and thin in the middle runs off at both ends: an upside-down U). Unevenness scales all of it.
    std::vector<float> ThicknessProfile(int n)
    {
        struct Bump
        {
            float at, height, width;
        };
        const float u = g_cfg.unevenness;
        std::vector<Bump> bumps;
        const int lumps = RandInt(2, 3);
        for (int i = 0; i < lumps; ++i) bumps.push_back({ U(-0.85f, 0.85f), U(0.5f, 1.3f) * u, U(0.08f, 0.22f) });
        const int necks = RandInt(0, 2);
        for (int i = 0; i < necks; ++i) bumps.push_back({ U(-0.7f, 0.7f), -U(0.3f, 0.6f) * u, U(0.06f, 0.15f) });
        if (Chance() < 0.45f) bumps.push_back({ -1.0f, U(0.4f, 1.0f) * u, 0.18f });
        if (Chance() < 0.45f) bumps.push_back({ 1.0f, U(0.4f, 1.0f) * u, 0.18f });
        const float ph1 = U(0.0f, 6.283f);
        const float ph2 = U(0.0f, 6.283f);
        std::vector<float> t(static_cast<std::size_t>(n));
        float sum = 0.0f;
        for (int i = 0; i < n; ++i) {
            const float s = -1.0f + 2.0f * static_cast<float>(i) / static_cast<float>(n - 1);
            float v = 1.0f - 0.55f * u + 0.15f * u * std::sin(s * 9.0f + ph1) + 0.1f * u * std::sin(s * 17.0f + ph2);
            for (const auto& b : bumps) v += b.height * std::exp(-((s - b.at) / b.width) * ((s - b.at) / b.width));
            v *= 1.0f - 0.35f * std::pow(std::abs(s), 6.0f);  // the very ends taper
            v = std::max(v, 0.12f);
            t[static_cast<std::size_t>(i)] = v;
            sum += v;
        }
        const float mean = sum / static_cast<float>(n);
        for (auto& v : t) v /= std::max(mean, 1e-3f);  // the same amount of cream, only shared out unevenly
        return t;
    }

    // One strand aimed at point p (world) along dir: its points are cast as parallel rays; where they meet the skin they become drops.
    bool MakeStrand(V3 p, V3 dir, float strength, Strand* out)
    {
        constexpr int kPoints = 32;
        const float length = g_cfg.strandLength * 10.0f / kUnitMm * U(0.6f, 1.4f) * (0.7f + 0.3f * std::min(strength, 1.5f));  // units
        const bool compact = Chance() < g_cfg.blobShare;
        const float lateral = compact ? U(0.0f, 0.12f) : U(0.3f, 1.0f);  // how much of its length lies across the flight
        // two directions across the flight: t along the strand's tilt, t2 for its bend
        V3 any = std::abs(dir.z) < 0.9f ? V3{ 0, 0, 1 } : V3{ 1, 0, 0 };
        const float spin = U(0.0f, 6.283f);
        const V3 a = Norm(Cross(dir, any));
        const V3 b = Cross(dir, a);
        const V3 t = a * std::cos(spin) + b * std::sin(spin);
        const V3 t2 = Cross(dir, t);
        const float bend = length * U(-0.3f, 0.3f) * std::max(lateral, 0.15f);
        const auto thick = ThicknessProfile(kPoints);
        const float rBase = g_cfg.strandWidth * 0.5f * g_cfg.size * (0.8f + 0.2f * std::min(strength, 1.5f)) * U(0.8f, 1.2f);  // mm
        const bool reverse = Chance() < 0.5f;  // which end arrives first
        out->drops.clear();
        out->compact = 1.0f - lateral;
        for (int k = 0; k < kPoints; ++k) {
            const int i = reverse ? kPoints - 1 - k : k;
            const float s = -1.0f + 2.0f * static_cast<float>(i) / static_cast<float>(kPoints - 1);
            const V3 q = p + t * (s * length * lateral * 0.5f) + t2 * (bend * (1.0f - s * s));
            SkinHit sh{};
            if (!CastSkin(q - dir * 60.0f, dir, 120.0f, &sh)) continue;  // this part of the strand flies past
            g_cur = &g_surfs[sh.surf];
            if (!g_cur->field->Inner(static_cast<int>(sh.hit.x), static_cast<int>(sh.hit.y))) continue;
            Drop d;
            d.surf = sh.surf;
            d.x = sh.hit.x;
            d.y = sh.hit.y;
            d.thick = thick[static_cast<std::size_t>(i)];
            d.rMm = rBase * std::pow(d.thick, 0.75f);  // a lump is wider and higher, a neck narrower and thinner
            d.tx = sh.hit.tx;
            d.ty = sh.hit.ty;
            d.slant = sh.hit.slant;
            d.s = s;
            out->drops.push_back(d);
        }
        return out->drops.size() >= 3;
    }

    // a drop of a strand on the skin, joined to the one before it when they are close (one line, not a row of dots)
    void DepositDrop(const Drop& d, const Drop* prev)
    {
        g_cur = &g_surfs[d.surf];
        const auto s = At(d.x, d.y);
        const float r = d.rMm / std::max(s.mmpx, 1e-3f);
        const float h = std::clamp(0.35f + 0.6f * d.thick, 0.3f, 1.45f);  // thin necks see-through, lumps thick
        const float smear = 1.0f + 1.4f * d.slant;  // a grazing strand smears along its travel
        const float ang = std::atan2(d.ty, d.tx);
        auto stamp = [&](float x, float y, float rr, float hh) { g_cur->layer->Stamp(x, y, rr * smear, rr, ang, hh, 0.5f, true); };
        if (prev && prev->surf == d.surf) {
            const auto ps = At(prev->x, prev->y);
            const float pr = prev->rMm / std::max(ps.mmpx, 1e-3f);
            const float dist = std::hypot(d.x - prev->x, d.y - prev->y);
            if (dist < 3.5f * (r + pr) && dist > 0.4f * std::min(r, pr)) {  // close: fill the gap (far apart: a seam or a ridge in between)
                const int pieces = static_cast<int>(std::ceil(dist / (0.4f * std::min(r, pr))));
                const float ph = std::clamp(0.35f + 0.6f * prev->thick, 0.3f, 1.45f);
                for (int i = 1; i < pieces; ++i) {
                    const float f = static_cast<float>(i) / static_cast<float>(pieces);
                    stamp(prev->x + (d.x - prev->x) * f, prev->y + (d.y - prev->y) * f, pr + (r - pr) * f, ph + (h - ph) * f);
                }
            }
        }
        stamp(d.x, d.y, r, h);
    }

    // When a strand has landed: its cream gathers where it is thickest. Every local peak of its thickness (its ends too, when they are thicker
    // than next to them) becomes a blob that sits for HoldTime, then creeps when it is heavy enough. A strand that flew end-on dumped all of
    // itself on one spot: one big blob.
    void StrandBlobs(const Strand& st)
    {
        const auto& d = st.drops;
        const double hold = g_s.t + static_cast<double>(g_cfg.holdTime * U(0.7f, 1.3f));
        for (std::size_t i = 0; i < d.size(); ++i) {
            const float here = d[i].thick;
            const float before = i > 0 ? d[i - 1].thick : -1.0f;
            const float after = i + 1 < d.size() ? d[i + 1].thick : -1.0f;
            if (here < before || here < after || here < 1.1f) continue;  // the lumps (above average) gather into blobs
            g_cur = &g_surfs[d[i].surf];
            const float rMm = std::min(d[i].rMm * (1.1f + 1.4f * st.compact) * std::sqrt(here), 8.0f);
            Land(d[i].x, d[i].y, rMm, 1.0f + 0.6f * d[i].slant, std::atan2(d[i].ty, d[i].tx), hold);
        }
    }

    // a shot in progress: its strands, each waiting for its turn
    void UpdateStrands(float dt)
    {
        for (std::size_t k = 0; k < g_strands.size();) {
            auto& st = g_strands[k];
            if (st.delay > 0.0f) {
                st.delay -= dt;
                ++k;
                continue;
            }
            st.age += dt;
            const std::size_t due = std::min(st.drops.size(), static_cast<std::size_t>(std::ceil(st.age / st.dur * static_cast<float>(st.drops.size()))));
            for (; st.landed < due; ++st.landed) DepositDrop(st.drops[st.landed], st.landed > 0 ? &st.drops[st.landed - 1] : nullptr);
            if (st.landed < st.drops.size()) {
                ++k;
                continue;
            }
            StrandBlobs(st);
            g_strands.erase(g_strands.begin() + static_cast<std::ptrdiff_t>(k));
        }
    }

    // A shot: a target (face or upper torso), a source in front of it and below, a sweep of aim points across the target, and a strand for each.
    void SpawnShot(float strength)
    {
        Surf* face = g_cfg.face ? Find("face") : nullptr;
        Surf* body = Find("body");
        const bool atFace = face && Chance() < g_cfg.faceShare;
        Surf* target = atFace ? face : body;
        if (!target) return;
        for (auto& surf : g_surfs) surf.hasBounds = g_host->SurfaceBounds(surf.index, surf.lo, surf.hi);
        float lo[3];
        float hi[3];
        if (!g_host->SurfaceBounds(target->index, lo, hi)) return;
        const V3 up{ 0, 0, 1 };
        V3 centre{ (lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f };
        float height = hi[2] - lo[2];
        float width = std::max(hi[0] - lo[0], hi[1] - lo[1]);
        if (!atFace) {  // the torso: the chest, not the whole body (its bounds include arms and legs)
            centre.z = lo[2] + height * g_cfg.aimHeight;
            height *= 0.14f;
            width *= 0.22f;
        } else {
            height *= 0.8f;  // the face mesh reaches over the scalp and down the neck a little
            width *= 0.6f;
        }
        V3 front{};
        if (!Facing(*target, centre, std::max(width, height) * 2.0f + 40.0f, &front)) {
            logger::info("Cream: shot skipped: cannot tell which way the {} faces", target->name);
            return;
        }
        const V3 side = Norm(Cross(up, front));
        // the source: in front, below, a little to one side; it stays put for the whole shot
        const float dist = U(g_cfg.shotDistanceMin, g_cfg.shotDistanceMax) * 10.0f / kUnitMm;
        const float el = U(g_cfg.shotElevationMin, g_cfg.shotElevationMax) * 3.14159f / 180.0f;
        const float az = U(-g_cfg.shotSide, g_cfg.shotSide) * 3.14159f / 180.0f;
        const V3 source = centre + (front * (std::cos(el) * std::cos(az)) + side * (std::cos(el) * std::sin(az)) - up * std::sin(el)) * dist;
        // the sweep: from low to high across the target, at a slant (a narrow cone from below that crosses the face)
        const float sw = g_cfg.sweep;
        const V3 from = centre + up * (-0.5f * height * sw) + side * (U(-0.35f, 0.35f) * width);
        const V3 to = centre + up * (0.5f * height * sw) + side * (U(-0.35f, 0.35f) * width);
        const int n = RandInt(g_cfg.strandsMin, g_cfg.strandsMax);
        int made = 0;
        for (int i = 0; i < n; ++i) {
            const float f = n > 1 ? static_cast<float>(i) / static_cast<float>(n - 1) : 0.5f;
            const V3 aim = from + (to - from) * f + side * (Gauss() * 0.04f * width) + up * (Gauss() * 0.04f * height);
            Strand st;
            if (!MakeStrand(aim, Norm(aim - source), strength, &st)) continue;
            st.delay = static_cast<float>(i) * g_cfg.strandInterval * U(0.7f, 1.3f);
            st.dur = 0.04f + 0.04f * Chance();
            g_strands.push_back(std::move(st));
            ++made;
        }
        logger::info("Cream: shot {:.2f} at the {} from {:.0f} cm, {:.0f} deg below, {:.0f} deg aside: {} of {} strands met the skin", strength, target->name, dist * kUnitMm / 10.0f,
            el * 180.0f / 3.14159f, az * 180.0f / 3.14159f, made, n);
    }

    // ---------------------------------------------------------------- pour
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

    // ---------------------------------------------------------------- creeping blobs and their streaks
    void StepRunner(Runner& r)
    {
        const auto s = At(r.x, r.y);
        const float rMm = Radius(r.vol);
        // it slows as it sets; when it hardly moves any more, or the skin flattens out or ends, the rest of it stays as a hanging bulb
        const float setting = 1.0f / (1.0f + r.age / g_cfg.setTime);
        const float pulse = (std::sin(r.age * 2.1f + r.ph) + 1.0f) * 0.5f;  // a slow, uneven creep
        const float vmm = 1.6f * g_cfg.flowSpeed * rMm * s.steep * setting * (0.45f + 0.55f * pulse) / std::sqrt(g_cfg.viscosity);  // mm per second
        // it stops when it has all but stopped (relative to how fast it was set to go), or after FlowTime
        const bool stop = !s.inner || s.steep < 0.05f || r.vol < 0.8f || (r.age > 1.5f && vmm < 0.12f * g_cfg.flowSpeed) || r.age > g_cfg.flowTime;
        if (stop) {
            CheckClimb(r);
            if (s.inner && r.vol >= 0.3f && static_cast<int>(g_cur->blobs.size()) < BlobCap()) {
                const float a = std::atan2(r.vy, r.vx);
                Blob b{ r.x, r.y, std::max(r.vol, 0.6f), 1.15f, a, a };
                g_cur->blobs.push_back(b);
                DrawBlob(b);
                // the streak points under the blob that stays must not thin it (they erase up to 1.8 of their width around them)
                const float keep = Radius(b.vol) / s.mmpx * 1.3f;
                std::erase_if(g_cur->trail, [&](const TrailPt& p) { return std::hypot(p.x - r.x, p.y - r.y) < keep + 1.8f * p.r; });
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
        r.vol -= 0.05f * rMm * stepMm;  // the streak takes its share
        // the streak: as wide as the blob and thick right behind it; it thins out later (FadeTrails)
        const float tr = std::max(rMm * 0.85f / s.mmpx, 0.9f);
        const int pieces = std::max(1, static_cast<int>(std::ceil(stepPx / (0.4f * tr))));
        for (int i = 1; i <= pieces; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(pieces);
            const float px = r.x - r.vx * stepPx * (1.0f - t);
            const float py = r.y - r.vy * stepPx * (1.0f - t);
            const float wob = 1.0f + 0.08f * std::sin((r.age + t * kStep) * 9.0f + r.ph);
            g_cur->layer->Stamp(px, py, tr * wob, tr * wob, 0, 1.05f, 0.5f, true);
            if (std::hypot(px - r.lastX, py - r.lastY) >= 0.4f * tr) {  // points a fixed share of the width apart, so the thinning is even
                g_cur->trail.push_back({ px, py, tr, g_s.t });
                r.lastX = px;
                r.lastY = py;
            }
        }
        // blobs on the way flow into it
        for (std::size_t i = 0; i < g_cur->blobs.size();) {
            const Blob& b = g_cur->blobs[i];
            if (std::hypot(b.x - r.x, b.y - r.y) * s.mmpx < (Radius(b.vol) + rMm) * 0.75f) {
                r.vol += b.vol * 0.6f;
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
        g_cur->layer->Stamp(r.x, r.y, rh * 1.15f, rh * 1.0f, ang, 1.35f, 0.5f, false);  // the round, heavy head: compact
        g_cur->layer->Stamp(r.x - r.vx * rh * 0.9f, r.y - r.vy * rh * 0.9f, rh * 0.95f, rh * 0.85f, ang, 1.15f, 0.5f, false);
    }

    // Streaks thin out with age: every half second each point that is older than a moment is erased a little, until it has had its share of
    // passes (TrailFade). Erase takes most at its centre (keep = 1 - sqrt(q) * 1.6 * strength), so it is made almost twice as wide as the
    // streak: then its edges thin nearly as fast as its middle (a narrow erase left the edges thick and white). The points lie ~0.4 of the
    // width apart, so about 9 overlap at a pixel, at ~0.8 of the full strength on average.
    void FadeTrails()
    {
        constexpr float kPass = 0.5f;
        constexpr float kWide = 1.8f;
        constexpr float kOverlap = 2.0f * kWide / 0.4f;
        const int passes = std::max(1, static_cast<int>(g_cfg.trailFade / kPass));
        const float keepPerPass = std::pow(g_cfg.trailFloor, 1.0f / (static_cast<float>(passes) * kOverlap));
        const float strength = (1.0f - keepPerPass) / (1.6f * 0.8f);
        for (auto& p : g_cur->trail) {
            if (g_s.t - p.born < 0.6) continue;
            g_cur->layer->Erase(p.x, p.y, p.r * kWide, p.r * kWide, 0, strength);
            ++p.passes;
        }
        std::erase_if(g_cur->trail, [&](const TrailPt& p) { return p.passes >= passes; });
    }

    // ---------------------------------------------------------------- requests
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
        if (splash > 0.0f) SpawnShot(splash);
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
            g_strands.clear();
            std::string names;
            for (std::int32_t i = 0; i < host->SurfaceCount(); ++i) {
                Surf surf;
                surf.name = host->SurfaceName(i);
                surf.index = i;
                surf.layer = host->SurfaceLayer(i);
                surf.field = host->SurfaceField(i);
                if (!surf.layer || !surf.field) continue;
                surf.drip = surf.name == "body" ? g_cfg.dripBody : surf.name == "hands" ? g_cfg.dripHands : surf.name == "feet" ? g_cfg.dripFeet : g_cfg.dripFace;
                names += std::format("{}{}{}", names.empty() ? "" : ", ", surf.name, surf.drip ? "" : " (no dripping)");
                g_surfs.push_back(std::move(surf));
            }
            if (g_surfs.empty()) return false;
            g_s = State{};
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
            if (g_s.lookAcc >= 1.0f) {  // the slow thinning out of the look
                g_s.lookAcc = 0.0f;
                ApplyLook();
            }
            if (!g_strands.empty()) {
                UpdateStrands(dt);
                g_s.since = 0;
            }
            if (g_s.pourLeft > 0.0f) {
                g_s.pourLeft -= dt;
                g_s.pourAcc += g_cfg.pourRate * g_s.pourStr * dt;
                const int n = static_cast<int>(g_s.pourAcc);
                g_s.pourAcc -= static_cast<float>(n);
                // the stream falls on the face first (when it is there and gets some), the rest runs onto the body
                const bool hasFace = std::ranges::any_of(g_surfs, [](const Surf& s) { return s.name == "face" && s.pourX >= 0.0f; });
                for (auto& surf : g_surfs) {
                    if (surf.pourX < 0.0f) continue;
                    const float share = !hasFace ? 1.0f : surf.name == "face" ? 0.4f : 0.6f;
                    PourOn(surf, static_cast<int>(std::round(static_cast<float>(n) * share + Chance() - 0.5f)));
                }
                g_s.since = 0;
            }

            g_s.checkAcc += dt;
            const bool check = g_s.checkAcc >= 0.25f;
            if (check) g_s.checkAcc = 0.0f;
            g_s.trailAcc += dt;
            const bool fade = g_s.trailAcc >= 0.5f;
            if (fade) g_s.trailAcc = 0.0f;
            std::size_t blobs = 0;
            std::size_t runners = 0;
            std::size_t trail = 0;
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
                if (fade) FadeTrails();
                blobs += g_cur->blobs.size();
                runners += g_cur->runners.size();
                trail += g_cur->trail.size();
            }
            if (runners > 0) g_s.since = std::min(g_s.since, 1.0);

            // it stays, then fades out as a whole and the mode ends
            if (g_cfg.stayTime > 0.0f && g_s.since > g_cfg.stayTime) {
                g_s.alpha = std::max(0.0f, 1.0f - static_cast<float>(g_s.since - g_cfg.stayTime) / g_cfg.fadeTime);
                for (auto& surf : g_surfs) surf.layer->SetAlpha(g_s.alpha);
                if (g_s.alpha <= 0.0f) host->Finish();
            }
            if (g_s.frames % 600 == 0) logger::info("Cream: t={:.0f}s blobs {} runners {} streak points {} since {:.0f}s", g_s.t, blobs, runners, trail, g_s.since);
        }

        void Stop(DripSim::IHost*, DripSim::StopReason) override
        {
            logger::info("Cream: stopped after {:.0f} s", g_s.t);
            g_s = State{};
            g_surfs.clear();
            g_strands.clear();
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
