#pragma once

#include "../perk.hpp"

namespace gameplay {

    void HawkEyedTick(float dt);
    void HawkEyedOnSpawn(const SpawnContext& ctx);
    void HawkEyedOnDeath(const DeathContext& ctx);
    void HawkEyedReset();

    void ApplyNetMark(int senderSlot, int markedSlot);
    bool IsSlotMarked(int slot);
    void ClearAimMarks();

    inline constexpr Perk HawkEyed = {
        .id          = PerkId::HAWKEYED,
        .name        = "HAWK-EYED",
        .description = "Press |gen:flashlight| when aiming at an opponent to highlight them for you and your allies.",
        .icon        = "hud/notification/notifications_icons/coverops-abby-notification.png",
        .lpCost      = 2,
        .OnTick      = &HawkEyedTick,
        .OnSpawn     = &HawkEyedOnSpawn,
        .OnDeath     = &HawkEyedOnDeath,
        .OnReset     = &HawkEyedReset,
        .alwaysOn    = true,   // was winmm_patches.py: mark-without-perk (everyone can mark)
    };
}