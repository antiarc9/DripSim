// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 AntiArc. This program is free software: see the LICENSE file (GNU GPL version 3 or later). It comes with NO WARRANTY.
#pragma once

// Engine access (hooks, keys, owned textures, surface capture) and the debug probes that proved it (see docs/spikes.md).
namespace DS::Spike
{
    // Installs the hooks and the keyboard sink. Keys: Ctrl+H = S1 shape dump, Ctrl+Shift+H = S4 body dump (DripSim_body.bin),
    // Ctrl+Shift+G = M1 gravity view, F10 = reload settings; the modes register their own keys.
    void Install();
    // S1: logs every geometry under the player's 3D (name, hidden flag, parent, skin vertex count, shader property flags, alpha, texture paths).
    void DumpPlayerShapes();

    // Once per game frame on the main thread (Main::Update hook).
    void OnFrame();

    // The frame-end hook (render call) and the owned textures, one per surface (DS::Surface ids) on a free "<Prefix> [Ovl#]" slot.
    // A surface's texture has ONE owner at a time (kOwnerCore = the modes, kOwnerGravity = the debug view): the other cannot start on it, stop it,
    // or write pixels into it.
    constexpr int kOwnerCore = 1;
    constexpr int kOwnerGravity = 2;
    void InstallTextureHook();
    // 0 = failed (or owned by someone else), 1 = running, 2 = loading (the placeholder texture is being loaded by the engine: call TexturePoll).
    int TextureStart(int surface, int owner);
    int TexturePoll(int surface, int owner);
    void TextureStop(int surface, int owner);  // restores everything (only the owner can)
    bool TextureRunning(int surface, int owner);
    bool TextureBusy(int surface, int owner);  // loading or running, and owned by `owner`
    void TextureSetFrame(int surface, int owner, const std::uint32_t* pixels);  // 1024 x 1024 RGBA8, copied whole; uploaded before the next present
    // Same, but only the 64 px tiles flagged in `tiles` (16 x 16 flags, row-major) are copied and uploaded.
    void TextureSetTiles(int surface, int owner, const std::uint32_t* pixels, const std::uint8_t* tiles);
    // The optional normal map (model-space, same size): switched on by the framework's NormalMaps setting; the tiles are those of TextureSetTiles.
    void TextureSetNormalMaps(bool on);
    // Spike S7 (OverlayDecal=1 in DripSim.ini): the overlay shapes get the decal flags (depth bias), against z-fighting with the skin. Logs the overlay's flags once.
    void TextureSetDecal(bool on);
    // Debug (KeyFilmDump): the pixels of every running surface as DripSim_film_<surface>.tga in the SKSE log folder.
    void TextureDumpFilm();
    void TextureSetRefraction(float power);  // spike S5: screen-space refraction of the overlay (0 = off); needs the normal map running
    void TextureSetNormalTiles(int surface, int owner, const std::uint32_t* pixels, const std::uint8_t* tiles);
    // The optional specular map in the overlay's slot 7 (SpecMaps=1; it needs NormalMaps=1: it is loaded with the normal map): the gloss of each pixel, RGBA8 grey.
    void TextureSetSpecMaps(bool on);
    // PremultipliedAlpha=1: the overlays blend as colour + skin x (1 - alpha), so the engine's shine on the film is not scaled down by its opacity. The film's pixels must
    // then be premultiplied (TexturePremultiplied says whether a surface's overlay is in that mode now).
    void TextureSetPremultiplied(bool on);
    bool TexturePremultiplied(int surface);
    bool TextureSpecRunning(int surface);  // the surface's own gloss map is in its overlay's specular slot now
    void TextureSetSpecTiles(int surface, int owner, const std::uint32_t* pixels, const std::uint8_t* tiles);
    // What the end-of-frame uploads cost on the render thread (for the PERF log line); taking it resets the counters.
    struct TextureUploadStats
    {
        std::uint64_t frames = 0;  // frames that uploaded anything
        std::uint64_t calls = 0;   // UpdateSubresource calls
        double megabytes = 0;
        double totalMs = 0;
        double worstMs = 0;
        double lockWaitMs = 0;  // render thread waiting for the main thread's hold of the texture lock
    };
    TextureUploadStats TextureTakeUploadStats();
    void TextureOnFrame();
    // Sets the overlay material's gloss exponent and specular strength (the originals come back on stop); has = false puts the originals back now.
    void TextureSetSpec(int surface, int owner, bool has, float power, float scale);

    // S4 (Ctrl+Shift+H): dumps the live body's skin data (GPU vertex/index buffers, bones, bind + world transforms) to DripSim_body.bin.
    void RequestBodyDump();
    void BodyRenderWork();  // render thread, from the frame-end hook: performs the pending surface captures

    // M1 (Ctrl+Shift+G): per-triangle gravity on the live body as an animated debug texture.
    void ToggleGravityView();
    bool GravityViewActive();
    void GravityOnFrame();
}
