#include "perk-registry.hpp"
#include "definitions/hawk_eyed.hpp"
#include "definitions/headhunter.hpp"
#include "definitions/second-chance.hpp"
#include "definitions/marathon-runner.hpp"
#include "definitions/steady-hands.hpp"
#include "definitions/adrenaline.hpp"
#include "definitions/agility.hpp"
#include "definitions/first-aid-training.hpp"
#include "definitions/collector.hpp"
#include "definitions/hunker-down.hpp"
#include "match.hpp"
#include "net-debug.hpp"
#include "../loadout/loadout.hpp"

namespace gameplay::perks {

    // The one place every perk is listed, and the order the skill menu shows them in.
    static constexpr const Perk* kAllPerks[] = {
        &HawkEyed,
        &Headhunter,
        &SteadyHands,
        &FirstAidTraining,
        &MarathonRunner,
        &Agility,
        &Adrenaline,
        &SharpEars,
        //&SecondChance,
        &Collector,
    };
    static constexpr int kPerkCount = int(sizeof(kAllPerks) / sizeof(kAllPerks[0]));

    int         Count() { return kPerkCount; }
    const Perk* At(int index) { return (index >= 0 && index < kPerkCount) ? kAllPerks[index] : nullptr; }

    static PerkIds ActiveSet() { return net::match::Get().perkIds; }
    static PerkId* Slots() { return net::match::Get().perkSlots; }

    const Perk* Find(PerkId id)
    {
        for (const Perk* p : kAllPerks)
            if (p->id == id) return p;
        return nullptr;
    }

    bool IsActive(PerkId id) { return HasPerk(ActiveSet(), id); }

    static void SyncMask()
    {
        PerkIds mask = PerkId::NONE;
        for (int i = 0; i < kMaxEquippedPerks; ++i) mask |= Slots()[i];
        net::match::Get().perkIds = mask;
    }

    PerkId EquippedAt(int slot)
    {
        return (slot >= 0 && slot < kMaxEquippedPerks) ? Slots()[slot] : PerkId::NONE;
    }

    int SlotOf(PerkId id)
    {
        if (id == PerkId::NONE) return -1;
        for (int i = 0; i < kMaxEquippedPerks; ++i)
            if (Slots()[i] == id) return i;
        return -1;
    }

    bool IsEquipped(PerkId id) { return SlotOf(id) >= 0; }

    bool CanEquipInSlot(int slot, PerkId id)
    {
        if (slot < 0 || slot >= kMaxEquippedPerks) return false;
        if (!Find(id)) return false;
        if (SlotOf(id) >= 0) return false;
        return loadout::CanEquipPerk(slot, id);
    }

    bool EquipInSlot(int slot, PerkId id)
    {
        if (!CanEquipInSlot(slot, id)) return false;
        Slots()[slot] = id;
        SyncMask();
        TO1_LOG("[perk] slot%d = %s (%u LP) | %d/%d LP\n",
            slot, Find(id)->name, Find(id)->lpCost, loadout::PointsSpent(), kMaxLoadoutPoints);
        return true;
    }

    void ClearSlot(int slot)
    {
        if (slot < 0 || slot >= kMaxEquippedPerks) return;
        if (Slots()[slot] == PerkId::NONE) return;
        const Perk* p = Find(Slots()[slot]);
        Slots()[slot] = PerkId::NONE;
        SyncMask();
        TO1_LOG("[perk] slot%d cleared (was %s) | %d/%d LP\n",
            slot, p ? p->name : "(unknown)", loadout::PointsSpent(), kMaxLoadoutPoints);
    }

    void Unequip(PerkId id) { ClearSlot(SlotOf(id)); }

    void ClearLoadout()
    {
        for (int i = 0; i < kMaxEquippedPerks; ++i) Slots()[i] = PerkId::NONE;
        SyncMask();
    }

    // Debug only, LP not charged.
    void Grant(PerkId id)
    {
        const Perk* p = Find(id);
        if (!p || IsEquipped(id)) return;
        for (int i = 0; i < kMaxEquippedPerks; ++i) {
            if (Slots()[i] != PerkId::NONE) continue;
            Slots()[i] = id;
            SyncMask();
            TO1_LOG("[perk] granted %s in slot%d (debug, LP not charged) | %d/%d LP\n",
                p->name, i, loadout::PointsSpent(), kMaxLoadoutPoints);
            return;
        }
        TO1_LOG("[perk] cannot grant %s: all %d slots full\n", p->name, kMaxEquippedPerks);
    }

    void Revoke(PerkId id) { Unequip(id); }

    void SetActive(PerkIds ids)
    {
        ClearLoadout();
        for (int i = 0; i < kPerkCount; ++i)
            if (HasPerk(ids, kAllPerks[i]->id)) Grant(kAllPerks[i]->id);
    }

    void Tick(float dt)
    {
        const PerkIds set = ActiveSet();
        for (const Perk* p : kAllPerks)
            // was winmm_patches.py: mark-without-perk (Hawk-eyed's tick runs even when not equipped)
            if (p->OnTick && (p->alwaysOn || HasPerk(set, p->id))) p->OnTick(dt);
    }

    void NotifySpawn(bool respawn)
    {
        SpawnContext ctx;
        ctx.respawn = respawn;

        const PerkIds set = ActiveSet();
        for (const Perk* p : kAllPerks)
            if (p->OnSpawn && HasPerk(set, p->id)) p->OnSpawn(ctx);
    }

    void NotifyDeath(const DeathContext& ctx)
    {
        const PerkIds set = ActiveSet();
        for (const Perk* p : kAllPerks)
            if (p->OnDeath && HasPerk(set, p->id)) p->OnDeath(ctx);
    }

    void NotifyDealHit(const HitContext& ctx)
    {
        const PerkIds set = ActiveSet();
        for (const Perk* p : kAllPerks)
            if (p->OnDealHit && HasPerk(set, p->id)) p->OnDealHit(ctx);
    }

    void Reset()
    {
        for (const Perk* p : kAllPerks)
            if (p->OnReset) p->OnReset();
    }

    void NotifyPartsAwarded(int parts)
    {
        const PerkIds set = ActiveSet();
        for (const Perk* p : kAllPerks)
            if (p->OnPartsAwarded && HasPerk(set, p->id)) p->OnPartsAwarded(parts);
    }

}