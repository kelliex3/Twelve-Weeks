#pragma once

#include "../perk.hpp"

namespace gameplay {

    void AdrenalineTick(float dt);

    inline constexpr Perk Adrenaline = {
        .id = PerkId::ADRENALINE,
        .name = "ADRENALINE",
        .description = "Crouch and Sprint 20% faster while below 40 health.",
        .icon = "hud/player-upgrades/branches/player-upgrade-generic.png",
        .lpCost = 3,
        .OnTick = &AdrenalineTick,
    };
}