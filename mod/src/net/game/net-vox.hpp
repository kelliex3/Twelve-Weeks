#pragma once

#include <cstdint>

namespace net::vox {

    enum class Scenario : uint8_t {
        MatchIntro,
        MarkedEnemy,
        HealFinish,
        EnemyKill,
    };

    void PlayForLocal(Scenario s);
    void OnMatchLive();
    void Tick(float dt);
    void HandleLine(uint8_t speakerSlot, uint64_t lineSid);
    void Reset();

}