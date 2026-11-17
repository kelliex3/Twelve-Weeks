#pragma once

#include "../perk.hpp"

namespace gameplay {

    void HeadhunterOnDealHit(const HitContext& ctx);

    inline constexpr Perk Headhunter = {
        .id          = PerkId::HEADHUNTER,
        .name        = "HEAD HUNTER",
        .description = "Landing a headshot on an opponent restores 15 health.",
        .icon        = "hud/notification/notifications_icons/player-upgrade-jesse-guerilla.png",
        .lpCost      = 4,
        .OnDealHit   = &HeadhunterOnDealHit,
    };

    inline constexpr Perk SharpEars = {
        .id = PerkId::SHARPEARS,
        .name = "SHARP EARS",
        .description = "Move quickly while in listen mode, the meter regenerates 35% faster and you can stay in listen mode 30% longer.", 
        .icon = "hud/notification/notifications_icons/player-upgrade-listen-mode-notification.png",
        .lpCost = 3,
    };
}
