#include "loadout.hpp"

#include "../perks/perk-registry.hpp"
#include "match.hpp"

namespace gameplay::loadout {

    using weapons::Slot;

    int SelectedIndex(Slot s)
    {
        auto& M = net::match::Get();
        switch (s) {
        case Slot::Primary:     return M.loadoutPrim;
        case Slot::Secondary:   return M.loadoutSec;
        case Slot::Purchasable: return M.loadoutPurch;
        }
        return weapons::kEmpty;
    }

    weapons::View Selected(Slot s) { return weapons::Get(s, SelectedIndex(s)); }

    bool IsEquipped(Slot s) { return weapons::Valid(s, SelectedIndex(s)); }

    int PointsSpent()
    {
        int total = 0;
        total += static_cast<int>(Selected(Slot::Primary).lpCost);
        total += static_cast<int>(Selected(Slot::Secondary).lpCost);
        total += static_cast<int>(Selected(Slot::Purchasable).lpCost);
        for (int i = 0; i < kMaxEquippedPerks; ++i)
            if (const Perk* p = perks::Find(perks::EquippedAt(i)))
                total += static_cast<int>(p->lpCost);
        return total;
    }

    int PointsRemaining() { return kMaxLoadoutPoints - PointsSpent(); }

    bool CanSelectWeapon(Slot s, int index)
    {
        if (index == weapons::kEmpty) return weapons::CanBeEmpty(s);
        if (!weapons::Valid(s, index)) return false;

        const int freed = static_cast<int>(Selected(s).lpCost);
        const int cost = static_cast<int>(weapons::Get(s, index).lpCost);
        return PointsSpent() - freed + cost <= kMaxLoadoutPoints;
    }

    bool SelectWeapon(Slot s, int index)
    {
        if (!CanSelectWeapon(s, index)) return false;
        auto& M = net::match::Get();
        switch (s) {
        case Slot::Primary:     M.loadoutPrim = index; break;
        case Slot::Secondary:   M.loadoutSec = index; break;
        case Slot::Purchasable: M.loadoutPurch = index; break;
        }
        return true;
    }

    bool CanEquipPerk(int slot, PerkId id)
    {
        const Perk* p = perks::Find(id);
        if (!p) return false;
        const Perk* cur = perks::Find(perks::EquippedAt(slot));
        const int freed = cur ? static_cast<int>(cur->lpCost) : 0;
        return PointsSpent() - freed + static_cast<int>(p->lpCost) <= kMaxLoadoutPoints;
    }
}