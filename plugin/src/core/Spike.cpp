// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 AntiArc. This program is free software: see the LICENSE file (GNU GPL version 3 or later). It comes with NO WARRANTY.
#include "Spike.h"

#include "Core.h"

namespace
{

    void Dump(RE::NiAVObject* object, int depth)
    {
        if (!object) return;
        const std::string indent(static_cast<std::size_t>(depth) * 2, ' ');
        const char* name = object->name.c_str();
        auto* geometry = object->AsGeometry();
        const bool hidden = object->GetFlags().any(RE::NiAVObject::Flag::kHidden);
        const char* parent = object->parent && object->parent->name.c_str() ? object->parent->name.c_str() : "-";
        if (!geometry) {
            logger::info("{}{} [node] hidden={} parent={}", indent, name ? name : "(null)", hidden, parent);
            if (auto* node = object->AsNode()) {
                for (auto& child : node->GetChildren()) Dump(child.get(), depth + 1);
            }
            return;
        }
        // BSTriShape's own vertexCount reads 0 on 1.5.97 for skinned shapes; the skin partition carries the real count.
        const auto& data = geometry->GetGeometryRuntimeData();
        auto* partition = data.skinInstance && data.skinInstance->skinPartition ? data.skinInstance->skinPartition.get() : nullptr;
        std::uint64_t desc = 0;
        std::memcpy(&desc, &data.vertexDesc, std::min(sizeof desc, sizeof data.vertexDesc));
        logger::info("{}{} [geometry] hidden={} parent={} skinVerts={} partitions={} bones={} raw={} vertexDesc={:#x}", indent, name ? name : "(null)", hidden, parent,
            partition ? partition->vertexCount : 0u, partition ? partition->numPartitions : 0u,
            data.skinInstance && data.skinInstance->bones ? 1 : 0, data.rendererData && data.rendererData->rawVertexData ? 1 : 0, desc);
        auto* shader = geometry->GetGeometryRuntimeData().shaderProperty.get();
        if (!shader) {
            logger::info("{}  no shader property", indent);
            return;
        }
        logger::info("{}  shader={} flags={:#x}", indent, shader->GetRTTI() ? shader->GetRTTI()->GetName() : "?",
            static_cast<std::uint64_t>(shader->flags.underlying()));
        if (auto* lighting = geometry->lightingShaderProp_cast()) {
            logger::info("{}  alpha={}", indent, lighting->QMaterialAlpha());
            if (auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(lighting->GetBaseMaterial())) {
                if (auto set = material->GetTextureSet()) {
                    for (int i = 0; i < 9; ++i) {
                        const char* path = set->GetTexturePath(static_cast<RE::BSTextureSet::Texture>(i));
                        if (path && *path) logger::info("{}  tex[{}]={}", indent, i, path);
                    }
                }
            }
        }
    }

    // Main::Update: once per frame on the main thread. (A self-re-queueing AddTask loops inside one frame: it crashed the game.)
    struct MainUpdateHook
    {
        static void thunk()
        {
            func();
            DS::Spike::OnFrame();
        }

        static inline REL::Relocation<decltype(thunk)> func;
    };

    class Sink final : public RE::BSTEventSink<RE::InputEvent*>
    {
    public:
        static Sink& Get()
        {
            static Sink singleton;
            return singleton;
        }

        RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* events, RE::BSTEventSource<RE::InputEvent*>*) override
        {
            if (!events) return RE::BSEventNotifyControl::kContinue;
            for (auto* event = *events; event; event = event->next) {
                if (event->GetDevice() != RE::INPUT_DEVICE::kKeyboard) continue;
                auto* button = event->AsButtonEvent();
                if (!button || !button->IsDown()) continue;
                const auto key = button->GetIDCode();
                if (!DS::Core::WantsKey(key)) continue;
                const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                if (auto* tasks = SKSE::GetTaskInterface()) tasks->AddTask([key, shift, ctrl] { DS::Core::OnKey(key, shift, ctrl); });
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };
}

namespace DS::Spike
{
    void Install()
    {
        auto* manager = RE::BSInputDeviceManager::GetSingleton();
        if (!manager) {
            logger::error("Input manager unavailable; hotkeys disabled");
            return;
        }
        REL::Relocation<std::uintptr_t> update{ REL::RelocationID(35565, 36564) };
        MainUpdateHook::func = SKSE::GetTrampoline().write_call<5>(update.address() + REL::VariantOffset(0x748, 0xC26, 0x7EE).offset(),
            MainUpdateHook::thunk);
        InstallTextureHook();
        static_cast<RE::BSTEventSource<RE::InputEvent*>*>(manager)->AddEventSink(std::addressof(Sink::Get()));
        logger::info("Hooks and key sink installed; debug keys: Ctrl+H, Ctrl+Shift+H, Ctrl+Shift+G, F10");
    }

    void DumpPlayerShapes()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* ui = RE::UI::GetSingleton();
        if (!player || !player->Is3DLoaded() || (ui && ui->GameIsPaused())) return;
        auto* root = player->Get3D(false);
        if (!root) {
            logger::warn("S1: player has no 3D");
            return;
        }
        logger::info("S1: ---- player 3D dump ----");
        Dump(root, 0);
        logger::info("S1: ---- end ----");
        if (auto* console = RE::ConsoleLog::GetSingleton()) console->Print("DripSim: shapes logged");
        RE::SendHUDMessage::ShowHUDMessage("DripSim: shapes logged");
    }
}
