#pragma once

#include "weapons.hpp"

// In match weapon purchasing. Entirely local. Nothing here is replicated
namespace gameplay::purchase {

    void OnMatchLive(); // clear spend + ownership

    int  PartsEarned(); // scoreboard total, never decreases
    int  PartsBalance();
    bool CanAfford(int cost);

    bool PurchasableOwned();

    bool CanBuyWeapon();
    bool BuyWeapon();
    bool CanBuyAmmo(weapons::Slot s);
    bool AmmoFull(weapons::Slot s);
    int  CurrentAmmo(weapons::Slot s); // total carried, -1 if unknown
    bool BuyAmmo(weapons::Slot s);

    void OnDeath(); // snapshot the purchasable's remaining ammo
    int  PurchasableAmmo();

    void GrantParts(int parts); // debug only
}