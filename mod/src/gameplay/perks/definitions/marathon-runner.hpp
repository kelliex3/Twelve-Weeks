#pragma once

#include "../perk.hpp"

namespace gameplay {

    void MarathonRunnerTick(float dt);

    inline constexpr Perk MarathonRunner = {
        .id = PerkId::MARATHONRUNNER,
        .name = "MARATHON RUNNER",
        .description = "Sprint 10% faster at all times.",
        .icon = "hud/player-upgrades/branches/player-upgrade-generic.png",
        .lpCost = 1,   // was winmm_patches.py: lp-marathon-1
        .OnTick = &MarathonRunnerTick,
    };
}