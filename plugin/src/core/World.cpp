// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 AntiArc. This program is free software: see the LICENSE file (GNU GPL version 3 or later). It comes with NO WARRANTY.
#include "Core.h"

#include "Params.h"

// The world around the player for the modes: weather, interior, and whether anything is above the player's head.
namespace
{
    class World final : public DripSim::IWorld
    {
    public:
        // How heavy a rainy weather really is: the precipitation's particle density (relative to Params::rainDensityRef), a little for wind and for
        // thunder. A drizzle ends up around 0.3-0.5, a storm near 1. Not rainy = 0.
        static float Strength(const RE::TESWeather* weather)
        {
            if (!weather || !weather->data.flags.any(RE::TESWeather::WeatherDataFlag::kRainy)) return 0.0f;
            float density = 0.0f;
            if (auto* precip = weather->precipitationData) {
                try {
                    density = precip->GetSettingValue(RE::BGSShaderParticleGeometryData::DataID::kParticleDensity).f;
                } catch (...) {
                    density = 0.0f;
                }
            }
            const float d = density > 0.0f && density < 1e6f ? std::clamp(density / DS::Params::Get().rainDensityRef, 0.0f, 1.0f) : 0.5f;
            const float wind = static_cast<float>(weather->data.windSpeed) / 255.0f;
            const float thunder = weather->data.thunderLightningFrequency > 0 ? 0.12f : 0.0f;
            return std::clamp(0.25f + 0.65f * d + 0.1f * wind + thunder, 0.0f, 1.0f);
        }

        float RainIntensity() const override
        {
            auto* sky = RE::Sky::GetSingleton();
            if (!sky || sky->mode.get() != RE::Sky::Mode::kFull) return 0.0f;
            const float pct = std::clamp(sky->currentWeatherPct, 0.0f, 1.0f);
            // currentWeather fades in as pct goes 0 -> 1 while lastWeather fades out
            float intensity = Strength(sky->currentWeather) * pct + Strength(sky->lastWeather) * (1.0f - pct);
            if (!sky->IsRaining()) intensity *= 0.5f;  // rainy weather whose rain has not started falling yet
            return std::clamp(intensity, 0.0f, 1.0f);
        }

        const char* WeatherName() const override
        {
            auto* sky = RE::Sky::GetSingleton();
            const char* name = sky && sky->currentWeather ? sky->currentWeather->GetFormEditorID() : nullptr;
            return name ? name : "";
        }

        bool IsInterior() const override
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* cell = player ? player->GetParentCell() : nullptr;
            return cell && cell->IsInteriorCell();
        }

        bool SkyExposed() const override
        {
            return m_exposed;
        }

        float WaterDepth() const override
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !player->Is3DLoaded()) return 0.0f;
            const float level = player->GetWaterHeight();  // the water the player is over (-infinity when none)
            if (!(level > -1.0e8f)) return 0.0f;
            return std::max(0.0f, level - player->GetPositionZ());
        }

        bool IsSwimming() const override
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            return player && player->Is3DLoaded() && player->AsActorState()->IsSwimming();
        }

        float Exertion() const override
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !player->Is3DLoaded()) return 0.0f;
            const auto* state = player->AsActorState();
            float effort = 0.0f;
            if (state->IsSprinting()) effort = 1.0f;
            else if (state->IsRunning()) effort = 0.3f;
            switch (state->GetAttackState()) {
            case RE::ATTACK_STATE_ENUM::kSwing:
            case RE::ATTACK_STATE_ENUM::kHit:
            case RE::ATTACK_STATE_ENUM::kNextAttack:
            case RE::ATTACK_STATE_ENUM::kBash:
            case RE::ATTACK_STATE_ENUM::kBowDraw:
            case RE::ATTACK_STATE_ENUM::kBowDrawn:
                effort = std::max(effort, 0.7f);
                break;
            default:
                break;
            }
            if (state->IsWeaponDrawn() && player->IsInCombat()) effort = std::max(effort, 0.4f);
            if (auto* values = player->AsActorValueOwner()) {  // low stamina right after effort: the body is still working
                const float maximum = values->GetPermanentActorValue(RE::ActorValue::kStamina);
                if (maximum > 1.0f) {
                    const float missing = std::clamp(1.0f - values->GetActorValue(RE::ActorValue::kStamina) / maximum, 0.0f, 1.0f);
                    effort = std::max(effort, missing * 0.6f);
                }
            }
            return std::clamp(effort, 0.0f, 1.0f);
        }

        void Tick(float dt)
        {
            m_timer -= dt;
            if (m_timer > 0.0f) return;
            m_timer = 0.5f;
            m_exposed = Cast();
            LogWeatherChange();
        }

    private:
        // One line per weather the player meets: what the engine says about it, to calibrate Params::rainDensityRef.
        void LogWeatherChange()
        {
            auto* sky = RE::Sky::GetSingleton();
            const auto* weather = sky ? sky->currentWeather : nullptr;
            if (weather == m_loggedWeather) return;
            m_loggedWeather = weather;
            if (!weather) return;
            float density = 0.0f;
            if (auto* precip = weather->precipitationData) density = precip->GetSettingValue(RE::BGSShaderParticleGeometryData::DataID::kParticleDensity).f;
            logger::info("Weather: '{}' rainy {} density {:.0f} wind {} thunder {} -> strength {:.2f}", WeatherName(),
                weather->data.flags.any(RE::TESWeather::WeatherDataFlag::kRainy), density, weather->data.windSpeed, weather->data.thunderLightningFrequency, Strength(weather));
        }

        // A ray straight up from above the head: anything solid in the way (roof, tree, overhang) shelters the player.
        static bool Cast()
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* cell = player && player->Is3DLoaded() ? player->GetParentCell() : nullptr;
            auto* physics = cell ? cell->GetbhkWorld() : nullptr;
            if (!physics) return true;
            const auto position = player->GetPosition();
            const float scale = RE::bhkWorld::GetWorldScale();
            RE::bhkPickData pick;
            pick.rayInput.from = RE::hkVector4(position.x * scale, position.y * scale, (position.z + 150.0f) * scale, 0.0f);
            pick.rayInput.to = RE::hkVector4(position.x * scale, position.y * scale, (position.z + 4000.0f) * scale, 0.0f);
            pick.rayInput.filterInfo.SetCollisionLayer(RE::COL_LAYER::kLOS);
            RE::BSReadLockGuard lock(physics->worldLock);
            physics->PickObject(pick);
            return !pick.rayOutput.HasHit();
        }

        float m_timer = 0.0f;
        bool m_exposed = true;
        const RE::TESWeather* m_loggedWeather = nullptr;
    };

    World g_world;
}

namespace DS::World
{
    DripSim::IWorld* Get()
    {
        return &g_world;
    }

    void Tick(float dt)
    {
        g_world.Tick(dt);
    }
}
