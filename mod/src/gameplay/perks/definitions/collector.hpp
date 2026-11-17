#pragma once

#include "../perk.hpp"

namespace gameplay {

    void CollectorOnPartsAwarded(int parts);

    inline constexpr Perk Collector = {
        .id = PerkId::COLLECTOR,
        .name = "COLLECTOR",
        .description = "Increases the number of parts you earn by 10%.",
        .icon = "hud/player-upgrades/branches/player-upgrade-generic.png",
        .lpCost = 3,
        .OnPartsAwarded = &CollectorOnPartsAwarded,
    };
}