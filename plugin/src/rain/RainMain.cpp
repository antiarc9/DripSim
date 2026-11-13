// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 AntiArc. This program is free software: see the LICENSE file (GNU GPL version 3 or later). It comes with NO WARRANTY.
#include "RainMode.h"
#include "RotateLog.h"

// Plugin entry of DripSimRain.dll (DSW_KIND 1: the "rain" mode and its toggle key) and of DripSimSoak.dll (DSW_KIND 2: the "soak" mode, its key and the
// DripSim_Soak mod event). Both register with the DripSim framework; the water code (RainMode.cpp) is the same source.
#if DSW_KIND == 2
#define DSW_NAME "DripSimSoak"
#define DSW_MODE "soak"
#else
#define DSW_NAME "DripSimRain"
#define DSW_MODE "rain"
#endif
namespace
{
    DripSim::IDripSim* g_api = nullptr;

    // The toggle key forces rain (for testing, or for a rainy scene indoors); pressing it again stops it.
    [[maybe_unused]] void Toggle(void*)
    {
        if (g_api->IsActive("rain")) g_api->Stop("rain");
        else g_api->Start("rain", 1);
    }

    // A soak on demand: the whole body is wet, as after a swim. A running rain mode takes it on its next frame, otherwise one starts.
    void Soak(float strength, float depth)
    {
        if (!RainSoakEnabled()) return;
        RainRequestSoak(strength, depth);
        if (!g_api->IsActive(DSW_MODE)) g_api->Start(DSW_MODE, 2);
    }

    // The mod event DripSim_Soak (or DripSim_Wet), sent by other mods (a bath, a shower, a waterfall, a potion): numArg = how drenched, 0..1 (0 = 1);
    // strArg is ignored. Papyrus: akRef.SendModEvent("DripSim_Soak", "", 0.7)
    struct Triggers final : RE::BSTEventSink<SKSE::ModCallbackEvent>
    {
        RE::BSEventNotifyControl ProcessEvent(const SKSE::ModCallbackEvent* event, RE::BSTEventSource<SKSE::ModCallbackEvent>*) override
        {
            if (!event || !g_api) return RE::BSEventNotifyControl::kContinue;
            const std::string name = event->eventName.c_str();
            if (name == "DripSim_Soak" || name == "DripSim_Wet") {
                const float strength = event->numArg > 0.0f ? std::min(event->numArg, 1.0f) : 1.0f;
                logger::info("{}: mod event {} strength {:.2f}", DSW_MODE, name, strength);
                Soak(strength, 1000.0f);
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    Triggers g_triggers;

    [[maybe_unused]] void Bind(const DripSim::Ini& ini, const char* key, const char* fallback, void (*callback)(void*))
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
            logger::error("DripSim framework not found (DripSim.dll missing or a different API version): the {} mode is disabled", DSW_MODE);
            return;
        }
        RainLoadSettings();
        if (!g_api->RegisterMode(RainModeInstance())) {
            logger::error("Could not register the {} mode", DSW_MODE);
            return;
        }
#if DSW_KIND != 2
        const DripSim::Ini ini(DSW_NAME);
        Bind(ini, "KeyToggle", "Shift+G", Toggle);  // (the soak has no key of its own: leaving water and the mod event start it; F10 reloads its INI)
#endif
        if (RainSoakEnabled()) {
            if (auto* source = SKSE::GetModCallbackEventSource()) source->AddEventSink(&g_triggers);
            logger::info("Triggers: DripSim_Soak mod event");
        } else {
            logger::info("Soak is off ({})", DSW_KIND == 2 ? "Enabled=0 in DripSimSoak.ini" : "DripSimSoak.dll does the soak");
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    DS::RotateLog(DSW_NAME);
    SKSE::Init(skse);
    logger::info("{} {} loading", DSW_NAME, DSR_VERSION);
    if (!SKSE::GetMessagingInterface()->RegisterListener(OnMessage)) {
        logger::error("Could not register for SKSE messages");
        return false;
    }
    return true;
}
