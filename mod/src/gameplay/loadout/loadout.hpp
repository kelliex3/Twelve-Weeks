#pragma once

#include "weapons.hpp"
#include "../perks/perk.hpp"

namespace gameplay::loadout {

    int           SelectedIndex(weapons::Slot s);
    weapons::View Selected(weapons::Slot s);
    bool          IsEquipped(weapons::Slot s);

    int PointsSpent();
    int PointsRemaining();

    bool CanSelectWeapon(weapons::Slot s, int index); // index may be weapons::kEmpty
    bool SelectWeapon(weapons::Slot s, int index); // false = refused, nothing written
    bool CanEquipPerk(int slot, PerkId id);
}