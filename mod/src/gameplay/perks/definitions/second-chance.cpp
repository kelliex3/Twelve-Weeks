#include "second-chance.hpp"

#include "net-common.hpp"
#include "net-gui2.hpp"
#include "net-debug.hpp"

namespace gameplay {

    static constexpr int kTriggerHp = 30;

    static bool s_used = false; // once per life, re-armed by OnDeath

    void SecondChanceTick(float dt)
    {
        if (s_used) return;

        const int hp = PLAYER::GET_HEALTH();
        if (hp <= 0 || hp > kTriggerHp) return; // <= 0 also covers being dead

        uint64_t healthKitCount = DC::Call<"player-inventory-get-item-quantity">(SID("health-kit")); // TODO, THIS CALL DOESNT WORK, RETURNS 0
        if (healthKitCount > 0) return;

        DC::Call<"give-player-weapon">(SID("health-kit"), 1, 0, 1);
        s_used = true;
        TO1_LOG("[secondchance] health kit granted\n");
    }

    void SecondChanceOnDeath(const DeathContext&) { s_used = false; }
    void SecondChanceReset() { s_used = false; }
}