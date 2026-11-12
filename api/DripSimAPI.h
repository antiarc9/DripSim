// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 AntiArc. This program is free software: see the LICENSE file (GNU GPL version 3 or later). It comes with NO WARRANTY.
#pragma once

// DripSim framework API (version 5: IHost::SurfaceRaycast / SurfaceBounds; version 4: IWorld::Exertion; version 3: ILayer::SetWetFilm; version 2: IWorld::WaterDepth / IsSwimming). A "mode" is a separate SKSE plugin that simulates one kind of fluid on the player's skin
// (rain water, ...) and draws it into a film layer that the framework composites onto one overlay texture.
//
// How a mode plugin connects (see DripSimMode.h): at kPostLoad it calls DripSim::RequestAPI(), gets an IDripSim*, and registers
// one IMode with RegisterMode(). From then on the framework calls the mode's Start / Update / Stop on the game's main thread.
//
// ABI rules (the framework and the modes are separate DLLs):
//  - the virtual function ORDER of every interface below is the ABI: add new functions only at the end, bump kApiVersion;
//  - only plain data crosses the boundary: no std::string, std::vector, exceptions or ownership transfer;
//  - everything runs on the game's main thread; a pointer received in Start (layer, field, world) is valid until Stop returns.
#include <cstdint>

namespace DripSim
{
    constexpr std::uint32_t kApiVersion = 5;
    constexpr std::uint32_t kApiVersionMin = 3;  // the framework also serves modes built against these older versions (new functions are only ever added at the end)
    constexpr const char* kFrameworkModule = "DripSim.dll";

    // What the framework knows about the surface at a pixel of the body's UV atlas (1024 x 1024, x right, y down).
    struct Sample
    {
        float dx;           // unit downhill direction in the atlas; 0, 0 where the surface is flat
        float dy;
        float steep;        // 0 flat .. 1 vertical (from the live pose)
        float mmpx;         // millimetres per atlas pixel here
        float up;           // how much the surface faces the sky now (0 facing away / sideways .. 1 straight up)
        std::int32_t inner; // 1 when at least 3 px away from the edge of a UV island
    };

    // Colours are 0..255, alphas 0..1. Defaults (see DefaultLook) are a glassy, translucent look.
    struct Look
    {
        float bodyColor[3];  // the fluid body
        float rimColor[3];   // bright meniscus along edges and ridges
        float haloColor[3];  // wet-darkened skin around the fluid
        float bodyAlpha;     // opacity of a thick film
        float rimAlpha;      // strength of the rim
        float haloAlpha;     // strength of the wet halo (0 = none)
        float hole;          // thickness at which the body is ~63 % opaque (smaller = fills in faster)
        float glint;         // specular highlight on bulges
        float mottle;        // colour mottling of the body (0 = flat)
        float brightness;    // multiplies the colours
        float shade;         // 0..1: darkens and thickens the side of a bulge facing away from the light (a dome look: clear water, thick gel)
        float specPower;     // the overlay material's gloss exponent where the film is (0 = leave the material alone); ~60-150 = wet and tight
        float specScale;     // the overlay material's specular strength (0 = leave alone); the engine's own lights then make view-dependent highlights
        float rimDirection;  // 0..1: 0 = the rim glows all around an edge, 1 = only the edges facing the light shine (a bright crescent, the rest dark): water drops
        float smooth;        // 0..1: extra blur of the thickness map, so overlapping lobes and stamps merge into one soft liquid surface
        float filmAlpha;     // 0 = off. Two-fluid look: thin film (any thickness above ~0.02) gets this small constant opacity: a clear glossy film, with a bright edge line
        float creamStart;    // with filmAlpha: the thickness where the opaque cream body starts to come in (it is full ~0.5 above this); 0 = the plain thickness curve
        float coreColor[3];  // with creamStart: the colour of the thick cream (the warm subsurface glow); bodyColor is then the thin film. All 0 = no difference
        float normalStrength;  // 0 = no normal map; otherwise how strongly the film's thickness bends the surface normal the engine's lights see (1 = physical-ish, 2-3 = glossy and lumpy)
    };

    inline Look DefaultLook()
    {
        return Look{ { 188, 190, 191 }, { 238, 240, 240 }, { 150, 118, 98 }, 0.58f, 0.72f, 0.10f, 0.5f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, { 0.0f, 0.0f, 0.0f }, 0.0f };
    }

    // One film layer = one thickness map the mode draws into (heights ~0..1.4, 1 = a thick film). Layers are composited bottom to top in
    // the order the modes were started.
    class ILayer
    {
    public:
        virtual void Clear() = 0;
        // Max-blends a soft ellipse (radii in atlas pixels, angle in radians, peak height, profile exponent ~0.5..0.7).
        // permanent = stays (until Erase / Clear); otherwise it is drawn for this frame only (moving things: redraw them every frame).
        virtual void Stamp(float x, float y, float rx, float ry, float angle, float height, float power, bool permanent) = 0;
        // Removes film under a soft ellipse (strength 0..1, 1 = gone at the centre): for beads that merge or run off.
        virtual void Erase(float x, float y, float rx, float ry, float angle, float strength) = 0;
        virtual void SetLook(const Look* look) = 0;
        virtual void SetAlpha(float alpha) = 0;  // whole layer, 0..1 (fade-out)
        virtual std::int32_t Size() const = 0;   // atlas size in pixels
        // API 3: a thin clear film of water over all the skin below a height (in the skin's own space, the same units as IField::CenterZ), a uniform wet sheen without drawing it:
        // amount 0..1 (0 = none, scales the film), soft waterline. Changes are applied in steps (the film is re-rendered), so call it every frame with a slowly changing amount.
        virtual void SetWetFilm(float belowZ, float amount) = 0;
    };

    // One skin surface (the body, the hands, the feet) as a UV atlas. Positions are atlas pixels. tri ids are -1 outside the atlas islands.
    class IField
    {
    public:
        virtual std::int32_t Size() const = 0;
        virtual void At(float x, float y, Sample* out) const = 0;  // smoothed over a few pixels
        virtual bool Inner(std::int32_t x, std::int32_t y) const = 0;
        virtual std::int32_t TriAt(std::int32_t x, std::int32_t y) const = 0;
        // standing-pose (anatomical) values of a triangle: height, front/back facing (+y = front) and how vertical it is when standing
        virtual float CenterZ(std::int32_t tri) const = 0;
        virtual float NormalY(std::int32_t tri) const = 0;
        virtual float RestSteep(std::int32_t tri) const = 0;
        virtual float ZMin() const = 0;
        virtual float ZMax() const = 0;
    };

    // The world around the player.
    class IWorld
    {
    public:
        virtual float RainIntensity() const = 0;  // 0 dry .. 1 downpour: how heavy the current weather's rain really is (particle density, wind, thunder), blended while it changes
        virtual bool IsInterior() const = 0;
        virtual bool SkyExposed() const = 0;       // nothing above the player's head (roof, tree, overhang): sampled about twice a second
        virtual const char* WeatherName() const = 0;  // editor id of the current weather ("" when unknown): for logs and tuning
        // API 2: water the player stands or swims in
        virtual float WaterDepth() const = 0;  // game units of water above the player's feet (0 = dry): wading ~20-60, swimming 100+ (70 units = 1 m)
        virtual bool IsSwimming() const = 0;
        // API 4: how hard the player works right now, 0 resting .. 1 sprinting or fighting flat out (sprint, running, a melee swing, a drawn bow, low stamina after effort).
        // It is the instant value: a mode that follows how hot the body is keeps its own slowly moving sum of it.
        virtual float Exertion() const = 0;
    };

    // Skin surfaces a mode may ask for (body, hands, feet, face/head) (IMode::Surfaces is a mask of these). Each has its own UV atlas, field, film layer and texture.
    constexpr std::int32_t kSurfaceBody = 1;
    constexpr std::int32_t kSurfaceHands = 2;
    constexpr std::int32_t kSurfaceFeet = 4;
    constexpr std::int32_t kSurfaceFace = 8;

    // A ray against the player's body (world space, game units): where it lands on the body's UV atlas and which way it was travelling along the surface.
    struct Hit
    {
        float x, y;        // atlas pixel (Size() of the body's field)
        float tx, ty;      // unit direction of the ray's travel along the surface, in atlas pixels (x right, y down)
        float slant;       // 0 = the ray hit head-on (no travel direction), 1 = grazing
        float dist;        // distance from the ray origin
        float point[3];    // world position
        float normal[3];   // world surface normal (unit, facing the ray)
    };

    // Handed to a mode in Start / Update / Stop.
    class IHost
    {
    public:
        virtual ILayer* Layer() = 0;                // the body's layer (surface 0)
        virtual const IField* Field() const = 0;    // the body's field
        virtual const IWorld* World() const = 0;
        virtual void Notify(const char* text) = 0;  // a line in the console
        virtual void Finish() = 0;                  // the mode is done: the framework stops it after this call returns
        // The surfaces this run got (the body is always 0; hands / feet / face only when asked for and available), by index 0 .. SurfaceCount() - 1.
        virtual std::int32_t SurfaceCount() const = 0;
        virtual const char* SurfaceName(std::int32_t index) const = 0;  // "body", "hands", "feet", "face"
        virtual ILayer* SurfaceLayer(std::int32_t index) = 0;
        virtual const IField* SurfaceField(std::int32_t index) const = 0;
        // Added 2026-10-15. Both use the live skinned body (surface 0).
        virtual bool Raycast(const float from[3], const float dir[3], float maxDist, Hit* out) const = 0;  // nearest hit of the segment, false when it misses
        virtual bool BodyBounds(float lo[3], float hi[3]) const = 0;                                      // world bounding box of the body skin
        // API 5: the same on any surface this run got (index 0 .. SurfaceCount() - 1; 0 = the body, as Raycast / BodyBounds): the face, the hands, ...
        // Hit.x / y are pixels of that surface's atlas.
        virtual bool SurfaceRaycast(std::int32_t index, const float from[3], const float dir[3], float maxDist, Hit* out) const = 0;
        virtual bool SurfaceBounds(std::int32_t index, float lo[3], float hi[3]) const = 0;
    };

    enum class StopReason : std::int32_t
    {
        Requested = 0,  // Stop() or Finish()
        BodyLost = 1,   // the player's body went away (load, re-equip, cell change)
        Shutdown = 2,
    };

    // Implemented by a mode plugin.
    class IMode
    {
    public:
        virtual const char* Name() const = 0;  // unique, e.g. "rain"
        // variant: whatever the mode defines (-1 = its default). Return false to refuse (nothing is started then).
        virtual bool Start(IHost* host, std::int32_t variant) = 0;
        virtual void Update(IHost* host, float dt) = 0;  // every game frame while running
        virtual void Stop(IHost* host, StopReason reason) = 0;
        // About twice a second while the mode is NOT running: a place for automatic starts (e.g. rain begins). May call api->Start.
        virtual void Idle(class IDripSim* api, float dt) = 0;
        virtual void Reload() = 0;  // the player pressed the reload-settings key: re-read your INI
        // Mask of kSurface* the mode wants besides the body (the body is always given). The framework only builds what some running mode asks for.
        virtual std::int32_t Surfaces() const = 0;
    };

    // The framework, returned by RequestPluginAPI.
    class IDripSim
    {
    public:
        virtual std::uint32_t Version() const = 0;
        virtual bool RegisterMode(IMode* mode) = 0;
        virtual bool Start(const char* mode, std::int32_t variant) = 0;  // runs on the next game frame; false if unknown
        virtual void Stop(const char* mode) = 0;
        virtual bool IsActive(const char* mode) const = 0;
        // Calls callback(context) on the game thread when the key is pressed in the game world (not in menus / console / dialogue).
        // scanCode: DirectInput (H = 0x23). Never bind a bare key Skyrim uses (J journal, ...).
        virtual bool RegisterHotkey(std::uint32_t scanCode, bool shift, bool ctrl, void (*callback)(void*), void* context) = 0;
        // "H", "Shift+H", "Ctrl+Shift+F6", "None". Returns false (and leaves the outputs) for unknown text.
        virtual bool ParseKey(const char* text, std::uint32_t* scanCode, bool* shift, bool* ctrl) const = 0;
        virtual void Notify(const char* text) = 0;
        virtual const IWorld* World() const = 0;
    };
}
