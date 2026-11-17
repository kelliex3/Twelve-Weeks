#pragma once

#include "../perk.hpp"

namespace gameplay {

    void SecondChanceTick(float dt);
    void SecondChanceOnDeath(const DeathContext& ctx);
    void SecondChanceReset();

    inline constexpr Perk SecondChance = {
        .id = PerkId::SECONDCHANCE,
        .name = "SECOND CHANCE",
        .description = "Get a free health kit when you have none and have less than 30 health.",
        .icon = "hud/notification/notifications_icons/healthkit-notification.png",
        .lpCost = 3,
        .OnTick = &SecondChanceTick,
        .OnDeath = &SecondChanceOnDeath,
        .OnReset = &SecondChanceReset,
    };

}