#pragma once

#include "net-common.hpp"

namespace net::engine {

    const char* KillProcess(uintptr_t process);        // engine kill (RVA 0x1341470)

    void ForceMeleeAction(uintptr_t player, StringId64 sid);
    void StopMeleeAction(StringId64 entity);
    void RequestState(uintptr_t player, uintptr_t descriptor);

    void ApplyWeaponSkeleton(uintptr_t player);

    StringId64 GetPlayerWeaponId(uintptr_t player);    // held weapon SID
    uintptr_t  GetHeldFirearm(uintptr_t process);      // puppet/NPC held firearm object
    bool       ArmLaunchVelocity(uintptr_t weapon, const float* vel3);
    StringId64 GetMeleeAction(uintptr_t player);       // current melee action SID

    bool FireAttack(uintptr_t target, uintptr_t attacker, uint64_t weapon, uint64_t ammo, float damage, bool headshot = false);

    constexpr uint32_t kStateDescDeath = 0x35BD410;

    // Fix for puppet weapon in hands
    float GetWeaponIkFade(uintptr_t player);
    uintptr_t GetNpcWeapon(uintptr_t npc);
    bool IsAimSideSwapped(uintptr_t player);
    bool AttachWeaponToCorrectHand(uintptr_t character, uint64_t weaponSid, bool mirrored = false);
}