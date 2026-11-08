// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 AntiArc. This program is free software: see the LICENSE file (GNU GPL version 3 or later). It comes with NO WARRANTY.
#include "Spike.h"

#include "Body.h"
#include "Field.h"
#include "Params.h"

// The plugin-owned overlay textures: per surface a 2048x2048 RGBA8 ID3D11Texture2D + view that is put into an existing overlay texture object and
// rewritten from the frame-end hook, with no DDS load and no skee call (S3). The pixels come from the caller (TextureSetFrame).
//
// A surface's texture object comes one of two ways; the shader samples BSGraphics::Texture::resourceView, so replacing that pointer re-skins every
// shape that uses the object, and restoring it on stop puts everything back:
//  - donor: the NiSourceTexture of another overlay of the same surface (a tattoo, a spank mark) becomes ours; our free overlay slot is pointed at it.
//  - placeholder: when the surface has no used overlay to borrow, our free slot's texture set gets a tiny placeholder DDS
//    (Data\Textures\DripSim\surface_<name>.dds, shipped with the framework) loaded by the engine, and that unique texture object is then replaced.
namespace
{
    constexpr std::uint32_t kSize = DS::Atlas::kFilm;
    constexpr double kLoadTimeoutSeconds = 3.0;

    using REX::W32::D3D11_BIND_SHADER_RESOURCE;
    using REX::W32::D3D11_SUBRESOURCE_DATA;
    using REX::W32::D3D11_TEXTURE2D_DESC;
    using REX::W32::D3D11_USAGE_DEFAULT;
    using REX::W32::ID3D11Resource;
    using REX::W32::ID3D11ShaderResourceView;
    using REX::W32::ID3D11Texture2D;

    constexpr std::uint32_t kTilesPerSide = kSize / DS::Atlas::kTile;

    enum class Phase { Idle, Loading, Running };
    enum class Method { Donor, Placeholder };

    struct Slot
    {
        int surface = 0;
        int owner = 0;
        Phase phase = Phase::Idle;
        Method method = Method::Donor;
        RE::NiPointer<RE::BSGeometry> shape;          // our free overlay slot
        RE::NiPointer<RE::NiSourceTexture> original;  // its diffuse before we started
        RE::NiPointer<RE::BSTextureSet> originalSet;  // its texture set before we started
        float originalAlpha = 1.0f;
        RE::NiPointer<RE::NiSourceTexture> object;    // the texture object we replaced the contents of (donor or placeholder)
        ID3D11Resource* objectTexture = nullptr;      // its engine texture + view before we replaced them
        ID3D11ShaderResourceView* objectView = nullptr;
        ID3D11Texture2D* texture = nullptr;  // ours
        ID3D11ShaderResourceView* view = nullptr;
        std::vector<std::uint32_t> pixels;  // the next frame, RGBA8 (R in the low byte)
        bool fresh = false;                 // pixels changed since the last upload
        std::vector<std::uint8_t> dirty;  // 1 per film tile (row-major) changed since the last upload: only these are copied to the GPU
        std::chrono::steady_clock::time_point loadStarted;
        bool specSaved = false;  // the material's own specular settings, saved before the first change
        float origSpecPower = 1.0f;
        float origSpecScale = 1.0f;
        float lastPower = -1.0f;
        float lastScale = -1.0f;
        bool refrSaved = false;  // spike: the overlay's own refraction flag and power, saved before the first change
        bool origRefrFlag = false;
        float origRefrPower = 0.0f;
        float lastRefr = -1.0f;
        std::chrono::steady_clock::time_point attachChecked{};  // the last look at whether our overlay shape is still part of the player's 3D
        bool attached = true;
        std::uint64_t uploads = 0;
        double uploadMs = 0;
        // the optional normal map: a second owned texture swapped into the overlay's normal slot (a unique placeholder DDS is loaded there first)
        int normalState = 0;  // 0 off, 1 loading, 2 running
        bool normalTried = false;
        RE::NiPointer<RE::NiSourceTexture> originalNormal;
        RE::NiPointer<RE::NiSourceTexture> nObject;
        ID3D11Resource* nObjectTexture = nullptr;
        ID3D11ShaderResourceView* nObjectView = nullptr;
        ID3D11Texture2D* nTexture = nullptr;
        ID3D11ShaderResourceView* nView = nullptr;
        std::vector<std::uint32_t> nPixels;
        bool nFresh = false;
        std::vector<std::uint8_t> nDirty;
        std::chrono::steady_clock::time_point nStarted;
    };
    std::array<Slot, DS::Surface::Count> g_slots;
    bool g_normalMaps = false;
    std::mutex g_mutex;  // the main-thread hook (start/stop/pixels) and the frame-end hook may be different threads

    Slot& SlotOf(int surface)
    {
        return g_slots[static_cast<std::size_t>(std::clamp(surface, 0, static_cast<int>(DS::Surface::Count) - 1))];
    }

    RE::BSLightingShaderMaterialBase* MaterialOf(RE::BSGeometry* geometry)
    {
        auto* lighting = geometry ? geometry->lightingShaderProp_cast() : nullptr;
        return lighting ? static_cast<RE::BSLightingShaderMaterialBase*>(lighting->GetBaseMaterial()) : nullptr;
    }


    // True virtual dispatch on the material's own vtable (slot 8 = OnLoadTextureSet, 9 = ClearTextures), like skee does. CommonLibSSE-NG's
    // wrappers of these two call the base class implementation (BSLightingShaderMaterialBase's vtable), which is wrong for derived materials.
    void LoadTextureSet(RE::BSLightingShaderMaterialBase* material, RE::BSTextureSet* set)
    {
        using Fn = void (*)(RE::BSLightingShaderMaterialBase*, std::uint64_t, RE::BSTextureSet*);
        auto** vtable = *reinterpret_cast<void***>(material);
        reinterpret_cast<Fn>(vtable[8])(material, 0, set);
    }

    void ClearTextures(RE::BSLightingShaderMaterialBase* material)
    {
        using Fn = void (*)(RE::BSLightingShaderMaterialBase*);
        auto** vtable = *reinterpret_cast<void***>(material);
        reinterpret_cast<Fn>(vtable[9])(material);
    }

    void SetRefractionFlag(RE::BSLightingShaderProperty* lighting, bool on)
    {
        if (on) lighting->flags.set(RE::BSShaderProperty::EShaderPropertyFlag::kRefraction);
        else lighting->flags.reset(RE::BSShaderProperty::EShaderPropertyFlag::kRefraction);
    }

    bool IsBlank(const char* path)
    {
        std::string lower = path ? path : "";
        std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lower.empty() || lower.find("default.dds") != std::string::npos;
    }

    void ReleaseD3D(Slot& s)
    {
        if (s.view) s.view->Release();
        if (s.texture) s.texture->Release();
        s.view = nullptr;
        s.texture = nullptr;
        if (s.nView) s.nView->Release();
        if (s.nTexture) s.nTexture->Release();
        s.nView = nullptr;
        s.nTexture = nullptr;
    }

    // Puts the texture object's own texture and view back, restores our slot's texture, releases ours. Caller holds g_mutex.
    void StopLocked(Slot& s, const char* why)
    {
        if (s.phase == Phase::Idle) return;
        if (auto* renderer = s.object ? s.object->rendererTexture : nullptr) {
            if (s.objectView) {  // only once it was swapped
                renderer->texture = s.objectTexture;
                renderer->resourceView = s.objectView;
            }
        }
        if (s.normalState == 2) {
            if (auto* renderer = s.nObject ? s.nObject->rendererTexture : nullptr) {
                renderer->texture = s.nObjectTexture;
                renderer->resourceView = s.nObjectView;
            }
        }
        auto* material = MaterialOf(s.shape.get());
        auto* lighting = s.shape ? s.shape->lightingShaderProp_cast() : nullptr;
        if (material && lighting && s.refrSaved) {  // the refraction spike: back to what the overlay had
            material->refractionPower = s.origRefrPower;
            SetRefractionFlag(lighting, s.origRefrFlag);
            lighting->DoClearRenderPasses();
            s.refrSaved = false;
            s.lastRefr = -1.0f;
        }
        if (material && s.specSaved) {
            material->specularPower = s.origSpecPower;
            material->specularColorScale = s.origSpecScale;
        }
        if (material && lighting) {
            if ((s.method == Method::Placeholder || s.normalState != 0) && s.originalSet) {
                ClearTextures(material);
                LoadTextureSet(material, s.originalSet.get());
            }
            material->diffuseTexture = s.original;
            lighting->SetMaterialAlpha(s.originalAlpha);
        }
        ReleaseD3D(s);
        logger::info("Texture {}: stopped ({}) after {} uploads", DS::Surface::Name(s.surface), why, s.uploads);
        const int surface = s.surface;
        s = Slot{};
        s.surface = surface;
    }

    // Creates our D3D texture and swaps it into the texture object. Caller holds g_mutex.
    bool SwapIn(Slot& s)
    {
        auto* device = RE::BSGraphics::Renderer::GetDevice();
        if (!device || !s.object || !s.object->rendererTexture) return false;
        std::vector<std::uint32_t> blank(static_cast<std::size_t>(kSize) * kSize, 0u);
        D3D11_TEXTURE2D_DESC desc{};
        desc.width = kSize;
        desc.height = kSize;
        desc.mipLevels = 1;
        desc.arraySize = 1;
        desc.format = REX::W32::DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.sampleDesc.count = 1;
        desc.usage = D3D11_USAGE_DEFAULT;
        desc.bindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{};
        init.sysMem = blank.data();
        init.sysMemPitch = kSize * 4;
        ID3D11Texture2D* texture = nullptr;
        if (device->CreateTexture2D(&desc, &init, &texture) < 0 || !texture) {
            logger::error("Texture {}: CreateTexture2D failed", DS::Surface::Name(s.surface));
            return false;
        }
        ID3D11ShaderResourceView* view = nullptr;
        if (device->CreateShaderResourceView(texture, nullptr, &view) < 0 || !view) {
            logger::error("Texture {}: CreateShaderResourceView failed", DS::Surface::Name(s.surface));
            texture->Release();
            return false;
        }
        s.texture = texture;
        s.view = view;
        s.pixels = std::move(blank);
        s.dirty.assign(static_cast<std::size_t>(kTilesPerSide) * kTilesPerSide, 0);
        s.objectTexture = s.object->rendererTexture->texture;
        s.objectView = s.object->rendererTexture->resourceView;
        s.object->rendererTexture->texture = texture;
        s.object->rendererTexture->resourceView = view;
        return true;
    }

    // A second owned texture for the normal slot: the overlay's texture set is reloaded with a unique placeholder normal DDS (diffuse kept as it is), then,
    // once the engine has loaded it, that texture object's contents are replaced like the diffuse one. Only for model-space-normal overlays.
    void BeginNormalLocked(Slot& s)
    {
        if (s.normalTried || s.normalState != 0 || s.phase != Phase::Running) return;
        s.normalTried = true;
        auto* material = MaterialOf(s.shape.get());
        auto* lighting = s.shape ? s.shape->lightingShaderProp_cast() : nullptr;
        if (!material || !lighting || !s.originalSet) return;
        const bool modelSpace = lighting->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kModelSpaceNormals);
        const char* path = s.originalSet->GetTexturePath(RE::BSTextureSet::Texture::kNormal);
        logger::info("Texture {}: overlay '{}' normal map '{}', model-space flag {}", DS::Surface::Name(s.surface), s.shape->name.c_str(), path ? path : "-", modelSpace ? "set" : "NOT set");
        if (!modelSpace) {
            logger::warn("Texture {}: the overlay is not model-space-normal; the normal map is skipped (it would be read as tangent-space)", DS::Surface::Name(s.surface));
            return;
        }
        auto* set = RE::BSShaderTextureSet::Create();
        if (!set) return;
        for (int i = 0; i < 9; ++i) {
            const char* p = s.originalSet->GetTexturePath(static_cast<RE::BSTextureSet::Texture>(i));
            if (p && *p) set->SetTexturePath(static_cast<RE::BSTextureSet::Texture>(i), p);
        }
        const std::string placeholder = std::format("DripSim\\surface_{}_n.dds", DS::Surface::Name(s.surface));
        set->SetTexturePath(RE::BSTextureSet::Texture::kNormal, placeholder.c_str());
        RE::NiPointer<RE::BSTextureSet> keep(set);
        RE::NiPointer<RE::NiSourceTexture> diffuse = material->diffuseTexture;
        s.originalNormal = material->normalTexture;
        ClearTextures(material);
        LoadTextureSet(material, set);
        material->diffuseTexture = diffuse;  // ours stays
        s.lastPower = -1.0f;                 // the reload may have touched the specular values: apply them again
        s.lastScale = -1.0f;
        s.nStarted = std::chrono::steady_clock::now();
        s.normalState = 1;
        logger::info("Texture {}: loading the normal placeholder {}", DS::Surface::Name(s.surface), placeholder);
    }

    void PollNormalLocked(Slot& s)
    {
        if (s.normalState != 1) return;
        auto* material = MaterialOf(s.shape.get());
        auto* normal = material ? material->normalTexture.get() : nullptr;
        if (normal && normal != s.originalNormal.get() && normal->rendererTexture && normal->rendererTexture->resourceView) {
            auto* device = RE::BSGraphics::Renderer::GetDevice();
            if (!device) return;
            std::vector<std::uint32_t> flat(static_cast<std::size_t>(kSize) * kSize, 0xFFFF8080u);  // straight up in the game's model-space encoding
            D3D11_TEXTURE2D_DESC desc{};
            desc.width = kSize;
            desc.height = kSize;
            desc.mipLevels = 1;
            desc.arraySize = 1;
            desc.format = REX::W32::DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.sampleDesc.count = 1;
            desc.usage = D3D11_USAGE_DEFAULT;
            desc.bindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA init{};
            init.sysMem = flat.data();
            init.sysMemPitch = kSize * 4;
            ID3D11Texture2D* texture = nullptr;
            ID3D11ShaderResourceView* view = nullptr;
            if (device->CreateTexture2D(&desc, &init, &texture) < 0 || !texture || device->CreateShaderResourceView(texture, nullptr, &view) < 0 || !view) {
                if (texture) texture->Release();
                logger::error("Texture {}: could not create the normal texture", DS::Surface::Name(s.surface));
                s.normalState = 3;  // gave up; the set is restored on stop
                return;
            }
            s.nObject.reset(normal);
            s.nObjectTexture = normal->rendererTexture->texture;
            s.nObjectView = normal->rendererTexture->resourceView;
            normal->rendererTexture->texture = texture;
            normal->rendererTexture->resourceView = view;
            s.nTexture = texture;
            s.nView = view;
            s.nPixels = std::move(flat);
            s.nDirty.assign(static_cast<std::size_t>(kTilesPerSide) * kTilesPerSide, 0);
            s.normalState = 2;
            logger::info("Texture {}: normal map running ({}x{} owned texture)", DS::Surface::Name(s.surface), kSize, kSize);
            return;
        }
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - s.nStarted).count() > kLoadTimeoutSeconds) {
            logger::warn("Texture {}: the normal placeholder did not load (Data\\Textures\\DripSim\\surface_{}_n.dds missing?); no normal map", DS::Surface::Name(s.surface), DS::Surface::Name(s.surface));
            s.normalState = 3;
        }
    }

    // What the uploads cost on the render thread (summed since the last PERF line, read by TextureTakeUploadStats).
    std::atomic<std::uint64_t> g_upBytes{ 0 };
    std::atomic<std::uint64_t> g_upCalls{ 0 };
    std::atomic<std::uint64_t> g_upFrames{ 0 };
    std::atomic<std::int64_t> g_upNs{ 0 };
    std::atomic<std::int64_t> g_upWorstNs{ 0 };
    std::atomic<std::int64_t> g_upLockNs{ 0 };  // waiting for g_mutex

    // Copies the dirty tiles of `pixels` into `texture`: one UpdateSubresource per run of neighbouring dirty tiles in a tile row (gaps of up to 2 clean tiles are bridged), instead
    // of one box around everything: a soak changes tiles all over the atlas, and the bounding box of those is the whole 16 MB texture.
    template <class Context, class Texture>
    void UploadDirty(Context* context, Texture* texture, const std::uint32_t* pixels, std::vector<std::uint8_t>& dirty)
    {
        constexpr std::uint32_t kTile = DS::Atlas::kTile;
        for (std::uint32_t ty = 0; ty < kTilesPerSide; ++ty) {
            const std::uint8_t* row = &dirty[static_cast<std::size_t>(ty) * kTilesPerSide];
            std::uint32_t tx = 0;
            while (tx < kTilesPerSide) {
                if (!row[tx]) {
                    ++tx;
                    continue;
                }
                std::uint32_t last = tx;
                std::uint32_t gap = 0;
                for (std::uint32_t t = tx + 1; t < kTilesPerSide; ++t) {
                    if (row[t]) {
                        last = t;
                        gap = 0;
                    } else if (++gap > 2) {
                        break;
                    }
                }
                REX::W32::D3D11_BOX box{ tx * kTile, ty * kTile, 0, (last + 1) * kTile, (ty + 1) * kTile, 1 };
                const void* source = pixels + static_cast<std::size_t>(ty * kTile) * kSize + tx * kTile;  // the box top-left corner; the pitch steps rows
                context->UpdateSubresource(texture, 0, &box, source, kSize * 4, 0);
                g_upBytes += static_cast<std::uint64_t>(last + 1 - tx) * kTile * kTile * 4;
                ++g_upCalls;
                std::fill(dirty.begin() + static_cast<std::ptrdiff_t>(ty * kTilesPerSide + tx), dirty.begin() + static_cast<std::ptrdiff_t>(ty * kTilesPerSide + last + 1), std::uint8_t{ 0 });
                tx = last + 1;
            }
        }
    }

    // The render call at the end of each frame, same call Skygram hooks (SE 75461 + 0x9). Uploads this frame's pixels.
    struct FrameEndHook
    {
        static void thunk(std::uint32_t a_syncInterval)
        {
            Upload();
            DS::Spike::BodyRenderWork();
            func(a_syncInterval);
        }

        static void Upload()
        {
            const auto t0 = std::chrono::steady_clock::now();
            std::scoped_lock lock(g_mutex);
            const auto t1 = std::chrono::steady_clock::now();
            auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
            auto* context = renderer ? renderer->GetRuntimeData().context : nullptr;
            if (!context) return;
            bool any = false;
            for (auto& s : g_slots) {
                if (s.phase == Phase::Running && s.normalState == 2 && s.nTexture && s.nFresh) {
                    UploadDirty(context, s.nTexture, s.nPixels.data(), s.nDirty);
                    s.nFresh = false;
                    any = true;
                }
                if (s.phase != Phase::Running || !s.texture || !s.fresh) continue;
                UploadDirty(context, s.texture, s.pixels.data(), s.dirty);
                s.fresh = false;
                any = true;
                ++s.uploads;
            }
            if (!any) return;
            const auto t2 = std::chrono::steady_clock::now();
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();
            g_upNs += ns;
            g_upLockNs += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
            ++g_upFrames;
            auto worst = g_upWorstNs.load();
            while (ns > worst && !g_upWorstNs.compare_exchange_weak(worst, ns)) {
            }
        }

        static inline REL::Relocation<decltype(thunk)> func;
    };

    // Step 2 of the placeholder method: once the engine has loaded the placeholder DDS, replace its contents.
    int PollLocked(Slot& s)
    {
        if (s.phase != Phase::Loading) return s.phase == Phase::Running ? 1 : 0;
        auto* material = MaterialOf(s.shape.get());
        auto* diffuse = material ? material->diffuseTexture.get() : nullptr;
        if (diffuse && diffuse != s.original.get() && diffuse->rendererTexture && diffuse->rendererTexture->resourceView) {
            s.object.reset(diffuse);
            if (SwapIn(s)) {
                s.phase = Phase::Running;
                logger::info("Texture {}: placeholder loaded and replaced; running", DS::Surface::Name(s.surface));
                return 1;
            }
            StopLocked(s, "swap failed");
            return 0;
        }
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - s.loadStarted).count() > kLoadTimeoutSeconds) {
            logger::warn("Texture {}: the placeholder texture did not load (is Data\\Textures\\DripSim\\surface_{}.dds there?)", DS::Surface::Name(s.surface), DS::Surface::Name(s.surface));
            StopLocked(s, "placeholder did not load");
            return 0;
        }
        return 2;
    }
}

namespace DS::Spike
{
    void InstallTextureHook()
    {
        REL::Relocation<std::uintptr_t> frameEnd{ REL::VariantID(75461, 77246, 0xDBBDD0) };
        FrameEndHook::func = SKSE::GetTrampoline().write_call<5>(frameEnd.address() + REL::VariantOffset(0x9, 0x9, 0x15).offset(),
            FrameEndHook::thunk);
        logger::info("Texture: frame-end hook installed");
    }

    // Main thread, once per frame: stop a surface whose overlay or the player's 3D went away.
    void TextureOnFrame()
    {
        std::scoped_lock lock(g_mutex);
        auto* player = RE::PlayerCharacter::GetSingleton();
        const bool alive = player && player->Is3DLoaded() && player->Get3D(false);
        for (auto& s : g_slots) {
            if (s.phase == Phase::Idle) continue;
            if (alive && g_normalMaps && s.phase == Phase::Running) {
                BeginNormalLocked(s);
                PollNormalLocked(s);
            }
            if (!alive) {
                StopLocked(s, "player 3D gone");
            } else if (!s.shape || !s.shape->parent || !MaterialOf(s.shape.get())) {
                StopLocked(s, "overlay shape lost");
            } else if (s.phase == Phase::Running) {
                // A clothes change / overlay refresh (skee, QueueNiNodeUpdate) reloads the overlay's material: the shape stays but its textures are the engine's again
                // and our image is on an object nobody draws. Stop; the modes restart from a fresh capture (rain does it by itself while it rains).
                auto* material = MaterialOf(s.shape.get());
                if (s.object && material->diffuseTexture.get() != s.object.get()) StopLocked(s, "overlay textures were reset (clothes change or overlay refresh)");
                else if (s.normalState == 2 && s.nObject && material->normalTexture.get() != s.nObject.get()) StopLocked(s, "overlay normal map was reset (clothes change or overlay refresh)");
            }
        }
    }

    int TextureStart(int surface, int owner)
    {
        std::scoped_lock lock(g_mutex);
        auto& s = SlotOf(surface);
        if (s.phase != Phase::Idle && s.owner != owner) {
            logger::warn("Texture {}: already in use by another owner ({}); not started", DS::Surface::Name(surface), s.owner);
            return 0;
        }
        if (s.phase == Phase::Running) return 1;
        if (s.phase == Phase::Loading) return PollLocked(s);
        s.surface = surface;
        s.owner = owner;
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* root = player && player->Is3DLoaded() ? player->Get3D(false) : nullptr;
        if (!root || !RE::BSGraphics::Renderer::GetDevice()) {
            logger::warn("Texture {}: no player 3D or no D3D device", DS::Surface::Name(surface));
            return 0;
        }
        RE::BSGeometry* ours = nullptr;
        RE::NiSourceTexture* donor = nullptr;
        for (int i = 9; i >= 0; --i) {
            auto* object = root->GetObjectByName(RE::BSFixedString(std::format("{} [Ovl{}]", DS::Surface::Prefix(surface), i)));
            auto* geometry = object ? object->AsGeometry() : nullptr;
            auto* material = MaterialOf(geometry);
            if (!geometry || !material || !material->diffuseTexture) continue;
            auto set = material->GetTextureSet();
            const char* path = set ? set->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse) : nullptr;
            if (IsBlank(path)) {
                if (!ours) ours = geometry;
            } else if (DS::Params::Get().donor && !donor && material->diffuseTexture->rendererTexture && material->diffuseTexture->rendererTexture->resourceView) {
                donor = material->diffuseTexture.get();
                logger::info("Texture {}: donor texture {}", DS::Surface::Name(surface), path);
            }
        }
        if (!ours) {
            logger::warn("Texture {}: no free '{} [Ovl#]' overlay slot (all have a diffuse)", DS::Surface::Name(surface), DS::Surface::Prefix(surface));
            return 0;
        }
        auto* material = MaterialOf(ours);
        auto* lighting = ours->lightingShaderProp_cast();
        s.shape.reset(ours);
        s.original = material->diffuseTexture;
        s.originalSet = material->GetTextureSet();
        s.originalAlpha = lighting->QMaterialAlpha();

        if (donor && material->diffuseTexture.get() != donor) {
            s.method = Method::Donor;
            s.object.reset(donor);
            if (!SwapIn(s)) {
                s = Slot{};
                s.surface = surface;
                return 0;
            }
            // show it on our free slot (the donor's own overlay keeps its low alpha)
            material->diffuseTexture.reset(donor);
            lighting->SetMaterialAlpha(1.0f);
            s.phase = Phase::Running;
            logger::info("Texture {}: started (donor), {}x{} owned texture on '{}' (parent '{}', hidden {})", DS::Surface::Name(surface), kSize, kSize, ours->name.c_str(),
                ours->parent && ours->parent->name.c_str() ? ours->parent->name.c_str() : "-", ours->GetFlags().any(RE::NiAVObject::Flag::kHidden));
            return 1;
        }

        // no donor: give our slot a texture set with the placeholder DDS and wait for the engine to load it
        if (!s.originalSet) {
            logger::warn("Texture {}: the free overlay has no texture set to copy", DS::Surface::Name(surface));
            s = Slot{};
            s.surface = surface;
            return 0;
        }
        auto* set = RE::BSShaderTextureSet::Create();
        if (!set) {
            s = Slot{};
            s.surface = surface;
            return 0;
        }
        for (int i = 0; i < 9; ++i) {
            const char* path = s.originalSet->GetTexturePath(static_cast<RE::BSTextureSet::Texture>(i));
            if (path && *path) set->SetTexturePath(static_cast<RE::BSTextureSet::Texture>(i), path);
        }
        const std::string placeholder = std::format("DripSim\\surface_{}.dds", DS::Surface::Name(surface));
        set->SetTexturePath(RE::BSTextureSet::Texture::kDiffuse, placeholder.c_str());
        s.method = Method::Placeholder;
        RE::NiPointer<RE::BSTextureSet> keep(set);
        ClearTextures(material);
        LoadTextureSet(material, set);
        // (BSLightingShaderProperty::InvalidateTextures is not called: CommonLibSSE-NG 7.2.0 declares it with an absolute address as its id, which
        // makes the address library abort. The shader reads material->diffuseTexture->rendererTexture at draw time, which is all we replace.)
        lighting->SetMaterialAlpha(1.0f);
        s.loadStarted = std::chrono::steady_clock::now();
        s.phase = Phase::Loading;
        logger::info("Texture {}: no donor overlay; loading the placeholder {} on '{}'", DS::Surface::Name(surface), placeholder, ours->name.c_str());
        return 2;
    }

    int TexturePoll(int surface, int owner)
    {
        std::scoped_lock lock(g_mutex);
        auto& s = SlotOf(surface);
        return s.owner == owner ? PollLocked(s) : 0;
    }

    void TextureStop(int surface, int owner)
    {
        std::scoped_lock lock(g_mutex);
        auto& s = SlotOf(surface);
        if (s.owner == owner) StopLocked(s, "stopped by owner");
    }

    bool TextureRunning(int surface, int owner)
    {
        std::scoped_lock lock(g_mutex);
        auto& s = SlotOf(surface);
        if (s.phase != Phase::Running || s.owner != owner) return false;
        const auto now = std::chrono::steady_clock::now();
        if (now - s.attachChecked > std::chrono::milliseconds(400)) {  // a clothes change replaces the overlay shapes: ours would then be an orphan
            s.attachChecked = now;
            s.attached = DS::Field::ShapeStillAttached(s.shape.get());
            if (!s.attached) logger::info("Texture {}: our overlay shape is no longer part of the player's 3D (clothes change or rebuild)", DS::Surface::Name(surface));
        }
        return s.attached;
    }

    bool TextureBusy(int surface, int owner)
    {
        std::scoped_lock lock(g_mutex);
        const auto& s = SlotOf(surface);
        return s.phase != Phase::Idle && s.owner == owner;
    }

    void TextureSetFrame(int surface, int owner, const std::uint32_t* pixels)
    {
        std::scoped_lock lock(g_mutex);
        auto& s = SlotOf(surface);
        if (s.phase != Phase::Running || s.owner != owner || !pixels) return;
        std::copy_n(pixels, static_cast<std::size_t>(kSize) * kSize, s.pixels.begin());
        std::fill(s.dirty.begin(), s.dirty.end(), std::uint8_t{ 1 });
        s.fresh = true;
    }

    void TextureSetTiles(int surface, int owner, const std::uint32_t* pixels, const std::uint8_t* tiles)
    {
        std::scoped_lock lock(g_mutex);
        auto& s = SlotOf(surface);
        if (s.phase != Phase::Running || s.owner != owner || !pixels || !tiles || s.dirty.empty()) return;
        constexpr std::uint32_t kTile = DS::Atlas::kTile;
        bool any = false;
        for (std::uint32_t ty = 0; ty < kTilesPerSide; ++ty) {
            for (std::uint32_t tx = 0; tx < kTilesPerSide; ++tx) {
                if (!tiles[ty * kTilesPerSide + tx]) continue;
                for (std::uint32_t y = ty * kTile; y < (ty + 1) * kTile; ++y) {
                    std::copy_n(pixels + static_cast<std::size_t>(y) * kSize + tx * kTile, kTile, s.pixels.begin() + static_cast<std::ptrdiff_t>(y * kSize + tx * kTile));
                }
                s.dirty[ty * kTilesPerSide + tx] = 1;
                any = true;
            }
        }
        if (any) s.fresh = true;
    }

    void TextureSetNormalMaps(bool on)
    {
        std::scoped_lock lock(g_mutex);
        g_normalMaps = on;
    }

    void TextureSetNormalTiles(int surface, int owner, const std::uint32_t* pixels, const std::uint8_t* tiles)
    {
        std::scoped_lock lock(g_mutex);
        auto& s = SlotOf(surface);
        if (s.phase != Phase::Running || s.owner != owner || s.normalState != 2 || !pixels || !tiles || s.nDirty.empty()) return;
        constexpr std::uint32_t kTile = DS::Atlas::kTile;
        bool any = false;
        for (std::uint32_t ty = 0; ty < kTilesPerSide; ++ty) {
            for (std::uint32_t tx = 0; tx < kTilesPerSide; ++tx) {
                if (!tiles[ty * kTilesPerSide + tx]) continue;
                for (std::uint32_t y = ty * kTile; y < (ty + 1) * kTile; ++y) {
                    std::copy_n(pixels + static_cast<std::size_t>(y) * kSize + tx * kTile, kTile, s.nPixels.begin() + static_cast<std::ptrdiff_t>(y * kSize + tx * kTile));
                }
                s.nDirty[ty * kTilesPerSide + tx] = 1;
                any = true;
            }
        }
        if (any) s.nFresh = true;
    }

    // The upload cost on the render thread since the last call (reset by it).
    TextureUploadStats TextureTakeUploadStats()
    {
        TextureUploadStats out;
        out.frames = g_upFrames.exchange(0);
        out.calls = g_upCalls.exchange(0);
        out.megabytes = static_cast<double>(g_upBytes.exchange(0)) / 1.0e6;
        out.totalMs = static_cast<double>(g_upNs.exchange(0)) / 1.0e6;
        out.worstMs = static_cast<double>(g_upWorstNs.exchange(0)) / 1.0e6;
        out.lockWaitMs = static_cast<double>(g_upLockNs.exchange(0)) / 1.0e6;
        return out;
    }

    // Spike (docs/spikes.md S5): screen-space refraction through the overlay's lighting shader. power > 0 sets the refraction flag and the material's
    // refractionPower on every running overlay (the engine then bends the scene behind the film by its normal map); 0 restores the overlay's own values.
    void TextureSetRefraction(float power)
    {
        std::scoped_lock lock(g_mutex);
        for (auto& s : g_slots) {
            if (s.phase != Phase::Running || s.normalState != 2 || !s.shape) continue;
            auto* material = MaterialOf(s.shape.get());
            auto* lighting = s.shape->lightingShaderProp_cast();
            if (!material || !lighting || power == s.lastRefr) continue;
            if (!s.refrSaved) {
                if (power <= 0.0f) continue;
                s.origRefrFlag = lighting->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kRefraction);
                s.origRefrPower = material->refractionPower;
                s.refrSaved = true;
            }
            const bool on = power > 0.0f;
            material->refractionPower = on ? power : s.origRefrPower;
            SetRefractionFlag(lighting, on ? true : s.origRefrFlag);
            lighting->DoClearRenderPasses();  // the shader technique is chosen when the passes are built
            s.lastRefr = power;
            logger::info("Texture {}: refraction {} (power {:.2f}, flag was {}, power was {:.2f})", DS::Surface::Name(s.surface), on ? "on" : "off", power, s.origRefrFlag, s.origRefrPower);
        }
    }

    void TextureSetSpec(int surface, int owner, bool has, float power, float scale)
    {
        std::scoped_lock lock(g_mutex);
        auto& s = SlotOf(surface);
        if (s.phase != Phase::Running || s.owner != owner) return;
        auto* material = MaterialOf(s.shape.get());
        if (!material) return;
        if (!s.specSaved) {
            s.origSpecPower = material->specularPower;
            s.origSpecScale = material->specularColorScale;
            s.specSaved = true;
            logger::info("Texture {}: material specular power {:.1f} scale {:.2f} (saved)", DS::Surface::Name(surface), s.origSpecPower, s.origSpecScale);
        }
        const float wantPower = has ? power : s.origSpecPower;
        const float wantScale = has ? scale : s.origSpecScale;
        if (wantPower == s.lastPower && wantScale == s.lastScale) return;
        material->specularPower = wantPower;
        material->specularColorScale = wantScale;
        s.lastPower = wantPower;
        s.lastScale = wantScale;
    }
}
