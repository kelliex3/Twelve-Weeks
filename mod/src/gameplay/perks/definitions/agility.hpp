#pragma once

#include "../perk.hpp"

namespace gameplay {

    void AgilityTick(float dt);
    void AgilityReset();

    inline constexpr Perk Agility = {
        .id = PerkId::AGILITY,
        .name = "AGILITY",
        .description = "Climb and crouch walk very quickly.",
        .icon = "hud/player-upgrades/branches/player-upgrade-generic.png",
        .lpCost = 4,
        .OnTick = &AgilityTick,
        .OnReset = &AgilityReset,
    };
}