#pragma once

#include "../perk.hpp"

namespace gameplay {

    void FirstAidTrainingTick(float dt);
    void FirstAidTrainingReset();

    inline constexpr Perk FirstAidTraining = {
        .id = PerkId::FIRSTAIDTRAINING,
        .name = "FIRST AID TRAINING",
        .description = "Use health kits significantly faster.",
        .icon = "hud/player-upgrades/upgrades/upgrade-health-kit-speed-boost.png",
        .lpCost = 2,
        .OnTick = &FirstAidTrainingTick,
        .OnReset = &FirstAidTrainingReset,
    };
}