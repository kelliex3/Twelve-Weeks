#pragma once

#include <cstdint>

namespace gameplay::weapons {

    enum class Slot : std::uint8_t { Primary, Secondary, Purchasable };

    inline constexpr int kEmpty = -1;
    constexpr bool CanBeEmpty(Slot s) { return s != Slot::Secondary; }

    struct Weapon {
        const char* name;
        const char* icon;
        const char* sid;
        std::uint32_t lpCost;
        std::uint32_t startingAmmoAmount;
        std::uint32_t ammoCost;    // parts per ammo refill (backpack)
        std::uint32_t ammoAmount;  // rounds granted per refill
        const char* desc;        // backpack blurb for the weapon
        const char* ammoDesc;    // backpack blurb when buying its ammo
    };

    struct WeaponPurchasable {
        const char* name;
        const char* icon;
        const char* sid;
        std::uint32_t lpCost;      // loadout points to carry the slot
        std::uint32_t startingAmmoAmount;
        std::uint32_t buyCost;     // parts to buy the weapon in match
        std::uint32_t ammoCost;    // parts per ammo refill
        std::uint32_t ammoAmount;  // rounds granted per refill
        const char* desc;
        const char* ammoDesc;
    };

    inline constexpr Weapon kPrimary[] = {
        {
            .name = "VEPR RIFLE",
            .icon = "hud/weapons/guns/rifle-vepr/vepr.png",
            .sid = "rifle-vepr",
            .lpCost = 2,
            .startingAmmoAmount = 9,
            .ammoCost = 150,
            .ammoAmount = 5,
            .desc = "Full-auto rifle. Steady damage at mid range.",
            .ammoDesc = "Rifle rounds for your Vepr.",
        },
        {
            .name = "HUNTING RIFLE",
            .icon = "hud/weapons/guns/rifle-remington-bolt/remington-bolt.png",
            .sid = "rifle-remington-bolt",
            .lpCost = 2,
            .startingAmmoAmount = 5,
            .ammoCost = 250,
            .ammoAmount = 5,
            .desc = "Bolt-action. One shot, one kill at range.",
            .ammoDesc = "Long rifle rounds for your hunting rifle.",
        },
        {
            .name = "DOUBLE BARREL",
            .icon = "hud/weapons/guns/shotgun-baserri-over-under/baserri.png",
            .sid = "shotgun-baserri-over-under",
            .lpCost = 2,
            .startingAmmoAmount = 4,
            .ammoCost = 280,
            .ammoAmount = 4,
            .desc = "Two barrels, devastating up close.",
            .ammoDesc = "Shells for your double barrel.",
        },
        {
            .name = "BOW",
            .icon = "hud/weapons/guns/bow-ellie/bow.png",
            .sid = "bow-ellie",
            .lpCost = 2,
            .startingAmmoAmount = 7,
            .ammoCost = 200,
            .ammoAmount = 4,
            .desc = "Silent and lethal. Arrows can be recovered.",
            .ammoDesc = "Arrows for your bow.",
        },
    };

    inline constexpr Weapon kSecondary[] = {
        {
            .name = "BERETTA",
            .icon = "hud/weapons/guns/pistol-beretta/beretta.png",
            .sid = "pistol-beretta",
            .lpCost = 0,   // was winmm_patches.py: lp-9mm-free
            .startingAmmoAmount = 10,
            .ammoCost = 150,
            .ammoAmount = 5,
            .desc = "Reliable sidearm with a fast draw.",
            .ammoDesc = "Pistol rounds for your Beretta.",
        },
        {
            .name = "REVOLVER",
            .icon = "hud/weapons/guns/pistol-revolver-taurus/revolver.png",
            .sid = "pistol-revolver-taurus",
            .lpCost = 0,   // was winmm_patches.py: lp-revolver-free
            .startingAmmoAmount = 7,
            .ammoCost = 180,
            .ammoAmount = 4,
            .desc = "Heavy hitting, slow to reload.",
            .ammoDesc = "Revolver rounds for your revolver.",
        },
        {
            .name = "GLOCK",
            .icon = "hud/weapons/guns/pistol-glock/glock.png",
            .sid = "pistol-glock",
            .lpCost = 1,
            .startingAmmoAmount = 10,
            .ammoCost = 150,
            .ammoAmount = 5,
            .desc = "Larger magazine, lighter per shot.",
            .ammoDesc = "Pistol rounds for your Glock.",
        },
    };

    inline constexpr WeaponPurchasable kPurchasable[] = {
        {
            .name = "PUMP SHOTGUN",
            .icon = "hud/weapons/guns/shotgun-remington-pump/remington-pump.png",
            .sid = "shotgun-remington-pump",
            .lpCost = 4,
            .startingAmmoAmount = 2,
            .buyCost = 400,
            .ammoCost = 480,
            .ammoAmount = 2,
            .desc = "Pump action with a tighter spread.",
            .ammoDesc = "Shells for your pump shotgun.",
        },
        {
            .name = "HUNTING PISTOL",
            .icon = "hud/weapons/guns/pistol-xcaliber/xcaliber.png",
            .sid = "pistol-xcaliber",
            .lpCost = 3,
            .startingAmmoAmount = 3,
            .buyCost = 350,
            .ammoCost = 250,
            .ammoAmount = 6,
            .desc = "Precision sidearm, huge stopping power.",
            .ammoDesc = "Rounds for your hunting pistol.",
        },
        {
            .name = "GALIL RIFLE",
            .icon = "hud/weapons/guns/rifle-galil/galil.png",
            .sid = "rifle-galil",
            .lpCost = 3,
            .startingAmmoAmount = 15,
            .buyCost = 400,
            .ammoCost = 300,
            .ammoAmount = 25,
            .desc = "Full-auto rifle with a deep magazine.",
            .ammoDesc = "Rifle rounds for your Galil.",
        },
        {
            .name = "MPX5",
            .icon = "hud/weapons/guns/rifle-mpx5/mpx5.png",
            .sid = "rifle-mpx5",
            .lpCost = 3,   // was winmm_patches.py: lp-mpx5-3
            .startingAmmoAmount = 15,
            .buyCost = 400,
            .ammoCost = 250,
            .ammoAmount = 15,
            .desc = "High rate of fire, low recoil, close range.",
            .ammoDesc = "Submachine gun rounds for your MPX5.",
        },
        {
            .name = "CROSSBOW",
            .icon = "hud/weapons/guns/crossbow/crossbow.png",
            .sid = "crossbow",
            .lpCost = 3,   // was winmm_patches.py: lp-crossbow-3
            .startingAmmoAmount = 4,
            .buyCost = 350,
            .ammoCost = 285,
            .ammoAmount = 4,
            .desc = "Silent, and bolts can be recovered.",
            .ammoDesc = "Bolts for your crossbow.",
        },
    };

    inline constexpr int kPrimaryCount = int(sizeof(kPrimary) / sizeof(kPrimary[0]));
    inline constexpr int kSecondaryCount = int(sizeof(kSecondary) / sizeof(kSecondary[0]));
    inline constexpr int kPurchasableCount = int(sizeof(kPurchasable) / sizeof(kPurchasable[0]));

    constexpr int Count(Slot s)
    {
        switch (s) {
        case Slot::Primary:     return kPrimaryCount;
        case Slot::Secondary:   return kSecondaryCount;
        case Slot::Purchasable: return kPurchasableCount;
        }
        return 0;
    }

    constexpr bool Valid(Slot s, int index) { return index >= 0 && index < Count(s); }

    constexpr const WeaponPurchasable* AtPurchasable(int index)
    {
        return Valid(Slot::Purchasable, index) ? &kPurchasable[index] : nullptr;
    }

    struct View {
        const char* name;
        const char* icon;
        const char* sid;
        std::uint32_t lpCost;
        std::uint32_t startingAmmoAmount;
        std::uint32_t buyCost;
        std::uint32_t ammoCost;
        std::uint32_t ammoAmount;
        const char* desc;
        const char* ammoDesc;
    };

    constexpr View Get(Slot s, int index)
    {
        if (!Valid(s, index)) return { "", "", "", 0, 0, 0, 0, 0, "", "" };
        switch (s) {
        case Slot::Primary: {
            const Weapon& w = kPrimary[index];
            return { w.name, w.icon, w.sid, w.lpCost, w.startingAmmoAmount, 0, w.ammoCost, w.ammoAmount, w.desc, w.ammoDesc };
        }
        case Slot::Secondary: {
            const Weapon& w = kSecondary[index];
            return { w.name, w.icon, w.sid, w.lpCost, w.startingAmmoAmount, 0, w.ammoCost, w.ammoAmount, w.desc, w.ammoDesc };
        }
        case Slot::Purchasable: {
            const WeaponPurchasable& w = kPurchasable[index];
            return { w.name, w.icon, w.sid, w.lpCost, w.startingAmmoAmount, w.buyCost, w.ammoCost, w.ammoAmount, w.desc, w.ammoDesc };
        }
        }
        return { "", "", "", 0, 0, 0, 0, 0, "", "" };
    }
}