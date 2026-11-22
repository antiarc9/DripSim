// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 AntiArc. This program is free software: see the LICENSE file (GNU GPL version 3 or later). It comes with NO WARRANTY.
#include "SweatMode.h"
#include "RotateLog.h"

// Plugin entry of DripSimSweat.dll: registers the "sweat" mode with the DripSim framework and binds its key and the mod event.
namespace
{
    DripSim::IDripSim* g_api = nullptr;

    // The toggle key forces sweat (for testing the look); pressing it again stops it.
    void Toggle(void*)
    {
        if (g_api->IsActive("sweat")) g_api->Stop("sweat");
        else g_api->Start("sweat", 1);
    }

    // The mod event DripSim_Sweat, sent by other mods (a survival mod, a hot room, a spell): numArg = how hot the body gets at once, 0..1 (0 = 1);
    // strArg is ignored. Papyrus: akRef.SendModEvent("DripSim_Sweat", "", 0.7)
    struct Triggers final : RE::BSTEventSink<SKSE::ModCallbackEvent>
    {
        RE::BSEventNotifyControl ProcessEvent(const SKSE::ModCallbackEvent* event, RE::BSTEventSource<SKSE::ModCallbackEvent>*) override
        {
            if (!event || !g_api) return RE::BSEventNotifyControl::kContinue;
            const std::string name = event->eventName.c_str();
            if (name == "DripSim_Sweat") {
                const float heat = event->numArg > 0.0f ? std::min(event->numArg, 1.0f) : 1.0f;
                logger::info("Sweat: mod event {} heat {:.2f}", name, heat);
                SweatRequestHeat(heat);
                if (!g_api->IsActive("sweat")) g_api->Start("sweat", 2);
            }
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
            logger::error("DripSim framework not found (DripSim.dll missing or too old: the sweat mode needs API 4): the sweat mode is disabled");
            return;
        }
        SweatLoadSettings();
        if (!g_api->RegisterMode(SweatModeInstance())) {
            logger::error("Could not register the sweat mode");
            return;
        }
        const DripSim::Ini ini("DripSimSweat");
        Bind(ini, "KeyToggle", "Shift+B", Toggle);
        if (auto* source = SKSE::GetModCallbackEventSource()) source->AddEventSink(&g_triggers);
        logger::info("Triggers: DripSim_Sweat mod event");
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    DS::RotateLog("DripSimSweat");
    SKSE::Init(skse);
    logger::info("DripSimSweat {} loading", DSS_VERSION);
    if (!SKSE::GetMessagingInterface()->RegisterListener(OnMessage)) {
        logger::error("Could not register for SKSE messages");
        return false;
    }
    return true;
}

// For other plugins (the skin wetness of DripSim AIO): the sweat's heat, so their shine can match the beads. Any thread.
extern "C" __declspec(dllexport) float DripSimSweatHeat()
{
    return SweatHeatNow();
}
