// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 AntiArc. This program is free software: see the LICENSE file (GNU GPL version 3 or later). It comes with NO WARRANTY.
#include "SystemInfo.h"
#include "Core.h"
#include "RotateLog.h"
#include "Params.h"
#include "Spike.h"

namespace
{
    void OnMessage(SKSE::MessagingInterface::Message* message)
    {
        switch (message->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            DS::Params::Load();
            DS::Spike::Install();
            DS::Core::RegisterReportKey();
            break;
        case SKSE::MessagingInterface::kPreLoadGame:
        case SKSE::MessagingInterface::kNewGame:
            DS::Core::StopAll(DripSim::StopReason::BodyLost);
            break;
        default:
            break;
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    DS::RotateLog("DripSim");
    SKSE::Init(skse);
    SKSE::AllocTrampoline(64);
    logger::info("DripSim framework {} loading (API {})", DS_VERSION, DripSim::kApiVersion);
    const auto gameInfo = std::format("Game: Skyrim SE {}, SKSE {}", REL::Module::get().version().string("."), skse->SKSEVersion() ? std::to_string(skse->SKSEVersion()) : std::string("?"));
    logger::info("{}", gameInfo);
    DS::Core::SetGameInfo(gameInfo);
    const auto systemInfo = DS::SystemSummary();  // the player's machine, for the log and the report (nothing leaves the PC)
    logger::info("{}", systemInfo);
    DS::Core::SetSystemInfo(systemInfo);
    if (!SKSE::GetMessagingInterface()->RegisterListener(OnMessage)) {
        logger::error("Could not register for SKSE messages");
        return false;
    }
    return true;
}
