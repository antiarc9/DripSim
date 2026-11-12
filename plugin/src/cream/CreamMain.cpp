// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 AntiArc. This program is free software: see the LICENSE file (GNU GPL version 3 or later). It comes with NO WARRANTY.
#include "CreamMode.h"

// Plugin entry of DripSimCream.dll: registers the "cream" mode with the DripSim framework and binds its keys and the mod event.
namespace
{
    DripSim::IDripSim* g_api = nullptr;

    void Ask(CreamKind kind, float strength)
    {
        CreamRequest(kind, strength);
        if (!g_api->IsActive("cream")) g_api->Start("cream", static_cast<std::int32_t>(kind));
    }

    void Splash(void*) { Ask(CreamKind::Splash, 1.0f); }
    void Pour(void*) { Ask(CreamKind::Pour, 1.0f); }
    void Clean(void*)
    {
        if (g_api->IsActive("cream")) g_api->Stop("cream");
    }

    // The mod event DripSim_Cream, sent by other mods: strArg "pour" or "splash" (default), numArg = strength 0..1 (0 = 1); strArg "clean" wipes it off.
    // Papyrus: akRef.SendModEvent("DripSim_Cream", "pour", 0.8)
    struct Triggers final : RE::BSTEventSink<SKSE::ModCallbackEvent>
    {
        RE::BSEventNotifyControl ProcessEvent(const SKSE::ModCallbackEvent* event, RE::BSTEventSource<SKSE::ModCallbackEvent>*) override
        {
            if (!event || !g_api) return RE::BSEventNotifyControl::kContinue;
            if (std::string_view(event->eventName.c_str()) != "DripSim_Cream") return RE::BSEventNotifyControl::kContinue;
            const std::string what = event->strArg.c_str();
            const float strength = event->numArg > 0.0f ? std::min(event->numArg, 1.0f) : 1.0f;
            logger::info("Cream: mod event '{}' strength {:.2f}", what, strength);
            // the game's event thread: the framework calls are queued for the next frame, the request itself is atomic
            SKSE::GetTaskInterface()->AddTask([what, strength]() {
                if (what == "clean") Clean(nullptr);
                else Ask(what == "pour" ? CreamKind::Pour : CreamKind::Splash, strength);
            });
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    Triggers g_triggers;

    void Bind(const DripSim::Ini& ini, const char* key, const char* fallback, void (*callback)(void*))
    {
        std::uint32_t code = 0;
        bool shift = false;
        bool ctrl = false;
        const auto text = ini.Text(key, fallback);
        if (g_api->ParseKey(text.c_str(), &code, &shift, &ctrl) && code && g_api->RegisterHotkey(code, shift, ctrl, callback, nullptr)) {
            logger::info("Hotkey {} = {}", key, text);
        } else {
            logger::warn("{}={} is not a usable key; no hotkey", key, text);
        }
    }

    void OnMessage(SKSE::MessagingInterface::Message* message)
    {
        if (message->type != SKSE::MessagingInterface::kPostLoad) return;
        g_api = DripSim::RequestAPI();
        if (!g_api) {
            logger::error("DripSim framework not found (DripSim.dll missing or a different API version): the cream mode is disabled");
            return;
        }
        CreamLoadSettings();
        if (!g_api->RegisterMode(CreamModeInstance())) {
            logger::error("Could not register the cream mode");
            return;
        }
        const DripSim::Ini ini("DripSimCream");
        Bind(ini, "KeySplash", "Shift+C", Splash);
        Bind(ini, "KeyPour", "Shift+V", Pour);
        Bind(ini, "KeyClean", "Ctrl+Shift+C", Clean);
        if (auto* source = SKSE::GetModCallbackEventSource()) source->AddEventSink(&g_triggers);
        logger::info("Triggers: DripSim_Cream mod event");
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    SKSE::Init(skse);
    logger::info("DripSimCream {} loading", DSC_VERSION);
    if (!SKSE::GetMessagingInterface()->RegisterListener(OnMessage)) {
        logger::error("Could not register for SKSE messages");
        return false;
    }
    return true;
}
