#pragma once

#include "net-common.hpp"
#include "net-protocol.hpp"

class NetPlayer;

namespace net::replication {

    struct LocalCapture {
        uint32_t shotCount = 0;
    };

    LocalCapture& Local();

    void AlwaysRun();
    bool CaptureLocalState(PlayerNetState& out);
    void Tick();
    void TickMatchAuthority(float dt);
    void PlaceLocalAtSpawn(int spawnIndex);
    void HandleNetEvent(uint8_t senderSlot, uint8_t type, const uint8_t* data, uint8_t size);
    void RequestRespawn();
    float NowSec();

    enum class MatchState : uint8_t { Winning, Losing, Contested };

    struct AmmoDropProfile {
        int baseTarget;
        int cycleLength;
        int stateStep;
    };

    AmmoDropProfile GetAmmoProfileFromAmmoSID(StringId64 ammoSID);
    int CalculateAmmoDrop(int currentAmmo, AmmoDropProfile ammoDropProfile, MatchState state);
    MatchState CalculateRubberbandingTeamState(int localSlot, int enemySlot);
    void SpawnItemFromKill(int killerSlot, int victimSlot, StringId64 weaponSID);
    void AwardParts(int partsAmount, int slot, std::string reason);

    void ResetRuntime();

}