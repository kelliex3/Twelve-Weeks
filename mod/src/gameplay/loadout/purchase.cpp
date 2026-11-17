#include "purchase.hpp"
#include "loadout.hpp"

#include "net-common.hpp"
#include "net-session.hpp"
#include "net-debug.hpp"
#include "match.hpp"

namespace gameplay::purchase {

    using weapons::Slot;

    void OnMatchLive()
    {
        auto& M = net::match::Get();
        M.partsSpent = 0;
        M.purchasableOwned = false;
        M.purchasableAmmo = 0;
    }

    // stats[].parts is the scoreboard figure and must keep climbing, spending comes off a separate counter
    int PartsEarned()
    {
        const int slot = net::session::MySlot();
        if (slot < 0 || slot >= kMaxNetPlayers) return 0;
        return net::match::Get().stats[slot].parts;
    }

    int  PartsBalance() { return PartsEarned() - net::match::Get().partsSpent; }
    bool CanAfford(int cost) { return cost >= 0 && PartsBalance() >= cost; }
    bool PurchasableOwned() { return net::match::Get().purchasableOwned; }

    static void Spend(int cost) { net::match::Get().partsSpent += cost; }

    bool CanBuyWeapon()
    {
        if (PurchasableOwned()) return false;
        if (!loadout::IsEquipped(Slot::Purchasable)) return false;
        return CanAfford(static_cast<int>(loadout::Selected(Slot::Purchasable).buyCost));
    }

    bool BuyWeapon()
    {
        if (!CanBuyWeapon()) return false;

        const weapons::View w = loadout::Selected(Slot::Purchasable);
        const StringId64 sid = SID(w.sid);
        DC::Call<"give-player-weapon">(sid, w.startingAmmoAmount, 0, 1);
        DC::Call<"player-instant-draw-weapon-by-name">(sid, 0, 1);

        Spend(static_cast<int>(w.buyCost));
        net::match::Get().purchasableOwned = true;
        net::match::Get().purchasableAmmo = static_cast<int>(w.ammoAmount);
        TO1_LOG("[buy] %s for %u parts | balance %d\n", w.name, w.buyCost, PartsBalance());
        return true;
    }

    bool CanBuyAmmo(Slot s)
    {
        if (!loadout::IsEquipped(s)) return false;
        if (s == Slot::Purchasable && !PurchasableOwned()) return false;
        if (AmmoFull(s)) return false;
        return CanAfford(static_cast<int>(loadout::Selected(s).ammoCost));
    }

    static int MaxAmmoFor(StringId64 weaponSid)
    {
        constexpr uint64_t kDcFail = static_cast<uint64_t>(-1);

        const uint64_t ammoSid = DC::Call<"weapon-get-default-ammo-id">(weaponSid);
        if (!ammoSid || ammoSid == kDcFail) return -1;

        const uint64_t def = DC::Call<"lookup-inv-item-def">(ammoSid);
        if (!def || def == kDcFail) return -1;

        return *reinterpret_cast<const int32_t*>(static_cast<uintptr_t>(def) + 136);
    }

    bool AmmoFull(Slot s)
    {
        if (!loadout::IsEquipped(s)) return false;

        const StringId64 sid = SID(loadout::Selected(s).sid);
        const int max = MaxAmmoFor(sid);
        if (max <= 0) return false;

        const uint64_t cur = DC::Call<"get-total-ammo">(sid);
        return static_cast<int64_t>(cur) >= max;
    }

    int CurrentAmmo(Slot s)
    {
        if (!loadout::IsEquipped(s)) return -1;
        const uint64_t cur = DC::Call<"get-total-ammo">(SID(loadout::Selected(s).sid));
        if (cur == static_cast<uint64_t>(-1)) return -1;
        return static_cast<int>(cur);
    }

    int PurchasableAmmo() { return net::match::Get().purchasableAmmo; }

    void OnDeath()
    {
        auto& M = net::match::Get();
        if (!M.purchasableOwned) return;
        const int carried = CurrentAmmo(Slot::Purchasable);
        M.purchasableAmmo = (carried >= 0) ? carried : 0;
        TO1_LOG("[buy] purchasable kept %d ammo through death\n", M.purchasableAmmo);
    }

    bool BuyAmmo(Slot s)
    {
        if (!CanBuyAmmo(s)) return false;

        const weapons::View w = loadout::Selected(s);
        const StringId64 sid = SID(w.sid);
        const uint64_t cur = DC::Call<"get-total-ammo">(sid);

        // Buying just under the cap tops you up rather than exceeding it
        uint64_t next = cur + static_cast<uint64_t>(w.ammoAmount);
        const int max = MaxAmmoFor(sid);
        if (max > 0 && next > static_cast<uint64_t>(max))
            next = static_cast<uint64_t>(max);

        DC::Call<"set-total-ammo">(sid, next);

        Spend(static_cast<int>(w.ammoCost));
        TO1_LOG("[buy] %s ammo %llu -> %llu for %u parts | balance %d\n",
            w.name, static_cast<unsigned long long>(cur),
            static_cast<unsigned long long>(next), w.ammoCost, PartsBalance());
        return true;
    }

    void GrantParts(int parts)
    {
        const int slot = net::session::MySlot();
        if (slot < 0 || slot >= kMaxNetPlayers) return;
        net::match::Get().stats[slot].parts += parts;
        TO1_LOG("[buy] granted %d parts | balance %d\n", parts, PartsBalance());
    }
}