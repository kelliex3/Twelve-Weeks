#include "adrenaline.hpp"

#include "net-common.hpp"
#include "net-gui2.hpp"

namespace gameplay {

    static constexpr int   kTriggerHp = 30;
    static constexpr float kSpeedScale = 1.20f;

    void AdrenalineTick(float dt)
    {
        const int hp = PLAYER::GET_HEALTH();
        if (hp <= 0 || hp > kTriggerHp) return; // <= 0 also covers being dead

        DC::Call<"player-set-motion-match-crouch-speed-multiplier/f">(kSpeedScale);
        DC::Call<"player-set-motion-match-sprint-speed-multiplier/f">(kSpeedScale);
    }
}