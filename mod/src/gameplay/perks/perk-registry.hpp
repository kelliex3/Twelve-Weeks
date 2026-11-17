#pragma once

#include "perk.hpp"

namespace gameplay::perks {

    void Tick(float dt);
    void NotifySpawn(bool respawn);
    void NotifyDeath(const DeathContext& ctx);
    void NotifyDealHit(const HitContext& ctx);
    void NotifyPartsAwarded(int parts);
    void Reset();

    bool IsActive(PerkId id);
    const Perk* Find(PerkId id);
    void SetActive(PerkIds ids);
    void Grant(PerkId id);
    void Revoke(PerkId id);

    int Count();
    const Perk* At(int index);

    bool   IsEquipped(PerkId id);
    int    SlotOf(PerkId id); // -1 if not equipped
    PerkId EquippedAt(int slot); // NONE if empty

    bool CanEquipInSlot(int slot, PerkId id);
    bool EquipInSlot(int slot, PerkId id);
    void ClearSlot(int slot);
    void Unequip(PerkId id);
    void ClearLoadout();
}