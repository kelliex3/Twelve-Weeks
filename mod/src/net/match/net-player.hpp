#pragma once

#include "net-common.hpp"
#include "net-protocol.hpp"
#include "net-replication.hpp"

struct HealthLedger {
    float predHp = -1.0f;           // our running estimate of this owner's real health. -1 = unset
    // (no packet yet) -> fall back to the raw replicated value.
    float lastReportedHp = -1.0f;   // last health THEY reported; a rise means they healed/respawned.
    float lastLocalHitAt = -1000.0f;// NowSec() of our last hit on them. If we have not shot them
    // recently the ledger is resynced to their report, so damage
    // that never landed on their end (packet loss, engine
    // mitigation, spawn invuln) cannot drift us wrong forever.
    bool  killPredicted = false;    // kill reticle already flashed for this life -> a burst that
    // crosses zero flashes ONCE. Cleared on respawn, on a heal,
    // and on a ledger resync (they are alive, so we were wrong).

// How long after our last hit on someone we keep trusting our own running estimate
// over the value they report. Comfortably longer than any playable RTT (so a burst is
// never cut short mid-fight) and short enough that a wrong ledger self-corrects within
// about a second of us stopping fire.
    static constexpr float kLedgerResyncSec = 1.5f;

    // What one of our hits did to the estimate (for the shooter's log + reticle).
    struct HitPrediction { float beforeHp; float afterHp; bool lethal; };

    // Shooter side, from hk_BulletHitVictim: fold our own damage into the estimate.
    // NOT computed off reportedHp alone -- that is the last value they SENT (see the
    // struct essay). Returns lethal=true exactly when THIS hit takes the estimate
    // across zero for the first time this life (the killPredicted latch), which is
    // the shooter's cue to force the kill + draw the red reticle.
    HitPrediction OnDamageDealt(float damage, float reportedHp, float nowSec)
    {
        const float beforeHp = (predHp >= 0.0f) ? predHp : reportedHp;
        const float afterHp = beforeHp - damage;
        const bool  lethal = !killPredicted && beforeHp > 0.0f && afterHp <= 0.0f;
        if (lethal) killPredicted = true;
        predHp = (afterHp > 0.0f) ? afterHp : 0.0f;
        lastLocalHitAt = nowSec;
        return { beforeHp, afterHp, lethal };
    }

    // Same prediction as OnDamageDealt, without touching the ledger: lets the melee
    // execution decide to HOLD the killing blow before anything is reported.
    bool WouldBeLethal(float damage, float reportedHp) const
    {
        const float beforeHp = (predHp >= 0.0f) ? predHp : reportedHp;
        return !killPredicted && beforeHp > 0.0f && (beforeHp - damage) <= 0.0f;
    }

    // Per state packet, from ApplyRemote -- runs on EVERY packet, not just health
    // changes: "their number stopped moving" is itself information (our damage is
    // not landing). Our own hits drive predHp DOWN in OnDamageDealt; here we decide
    // when their report should win instead. Caller gates on a sane health value
    // (> 0 and < 1e6), same as the puppet health mirror.
    void OnStateReport(float reportedHp, float nowSec)
    {
        // We have not shot them in a while, so nothing of ours should still be in flight.
        // Their report is the truth -- take it. This is what stops a permanent pessimistic
        // drift when damage we counted never actually landed on their machine (lost damage
        // event, engine mitigation, spawn invulnerability); without it we would flash a
        // kill reticle on every subsequent hit for the rest of their life.
        const bool ledgerStale = (nowSec - lastLocalHitAt) > kLedgerResyncSec;

        if (predHp < 0.0f || ledgerStale) {
            predHp = reportedHp;
            killPredicted = false;          // they are demonstrably alive -> re-arm the reticle
        }
        else if (reportedHp < predHp) {
            predHp = reportedHp;            // ahead of our guess: someone ELSE damaged them
        }
        else if (reportedHp > lastReportedHp) {
            predHp = reportedHp;            // their health went UP: healed or respawned
            killPredicted = false;
        }
        // else: their report is simply older than our ledger -> keep ours.

        lastReportedHp = reportedHp;
    }

    // Back to "no estimate" -- the next state packet seeds predHp from their report. Called from
    // Spawn / Respawn / Despawn so an estimate can never survive a life or a re-used slot.
    void Reset() {
        predHp = -1.0f; lastReportedHp = -1.0f; lastLocalHitAt = -1000.0f; killPredicted = false;
    }
};

namespace net::player 
{
    void Tick(float dt);
    void OnMatchLive();
}

class NetPlayer {
public:
    int   netSlot = -1;
    bool  active = false;
    bool  isLocal = false;
    char  name[kNetNameLen] = "Player";
    char  look[kNetLookLen] = "";
    int   team = -1;

    bool  spawned = false;
    bool  dead = false;
    bool     dying = false;
    bool     triedNpcKill = false;
    bool     animDetached = false;

    enum class EngineHold : uint8_t { None, Suspended, Releasing };
    EngineHold engineHold = EngineHold::None;

    bool EngineOwned()     const { return engineHold != EngineHold::None; }
    bool EngineReleasing() const { return engineHold == EngineHold::Releasing; }
    uint64_t dyingSinceMs = 0;
    StringId64 npcSid = 0;

    PlayerNetState stateNew{};
    PlayerNetState stateOld{};
    bool     hasState = false;
    bool     hasSequence = false;
    uint32_t lastSequence = 0;
    float    interpTimer = 0.0f;
    float    interpDuration = 0.033f;
    float    lastPacketTime = 0.0f;

    bool     announcedSpawn = false;
    float    lastSyncedHealth = -1.0f;
    StringId64 puppetWeapon = 0;
    int      appliedTeamRel = -2;
    HealthLedger ledger;

    bool  aimMarkActive = false;
    float aimMarkAge = 0.0f;

    bool  pendingLaunchValid = false;
    float pendingLaunchVel[3] = { 0.0f, 0.0f, 0.0f };
    float pendingLaunchAt = -1000.0f;

    float radarFade = 0.0f;

    static constexpr int      kPingSamples = 4;
    static constexpr uint16_t kPingUnknown = 0xFFFF;
    uint16_t pingRing[kPingSamples] = { kPingUnknown, kPingUnknown, kPingUnknown, kPingUnknown };
    int      pingRingPos = 0;
    uint16_t reportedPingMs = kPingUnknown;

    uint16_t tamperFlags = 0;
    uint32_t tamperPid = 0;
    char     tamperProcess[kNetProcessNameLen] = {};

    bool IsLocal() const { return isLocal; }
    bool IsActiveRemote() const { return active && !isLocal; }
    uintptr_t  Ptr() const;
    bool Exists() const { return Ptr() != 0; }
    StringId64 UserSid() const;
    const char* DisplayName() const { return name; }
    const char* LookOrDefault() const;

    const PlayerNetState& State() const { return stateNew; }
    const PlayerNetState& Prev()  const { return stateOld; }
    float InterpAlpha() const;
    float SecondsSinceLastPacket(float now) const { return hasState ? (now - lastPacketTime) : 1e9f; }

    void RecordPing(uint32_t rttMs);
    int  PingMs() const;
    int  ReportedPingMs() const { return reportedPingMs == kPingUnknown ? -1 : static_cast<int>(reportedPingMs); }

    bool IsSprinting() const;

    bool PushState(const PlayerNetState& s, uint32_t sequence, float now);

    void Spawn(Vector3 position, Quaternion rotation);
    void Die();
    void ReleaseToEngine();
    void SuspendForEngineAction();
    void ResumeFromEngineAction();
    void ReclaimFromEngine();
    void TickDeath();
    void Respawn(Vector3 position, Quaternion rotation);
    void Reanchor(Vector3 position, Quaternion rotation);
    void Despawn();

    void StopMelee();
    void ForceMelee(StringId64 actionSid);

    void ApplyTeamFaction();

    void ResetRuntime();

};

namespace NetPlayers {

    bool SlotIsFriendly(int netSlot);

    NetPlayer& BySlot(int netSlot);
    NetPlayer* Local();
    uintptr_t  LocalPtr();
    NetPlayer* FromEnginePtr(uintptr_t playerPtr);
    int        ActiveCount();

    // team0 = SERAPHITES,
    // team1 = WLF
    void       CountActivePerTeam(int& team0, int& team1);

    // Iteration over ACTIVE remotes
    struct RemoteRange {
        struct It {
            int slot;
            void Advance();
            It& operator++() { slot++; Advance(); return *this; }
            bool operator!=(const It& o) const { return slot != o.slot; }
            NetPlayer& operator*() const { return BySlot(slot); }
        };
        It begin() const { It it{ 0 }; it.Advance(); return it; }
        It end()   const { return It{ kMaxNetPlayers }; }
    };
    inline RemoteRange Remotes() { return {}; }

    // ---- session lifecycle ---------------------------------------------------
    void Reset();
    void SetupLocal(int netSlot, const char* name, const char* look);
    void ApplyRoster(const RosterPayload& roster, int mySlot);
    void OnPeerDropped(int netSlot);

    void RespawnLocal(Vector3 position, Quaternion rotation);
    void GiveLocalLoadout();

    // Pickupables
    void InitializeSupplyBoxes();
    void PollPlayerNeeds();
    void RequestSpawnItem();


    // Supply Boxes
    struct SupplyBox
    {
        StringId64 boxSID;
        StringId64 interactableSID;
        float refillTime = -1.0f;
        bool opened = false;
        std::vector<StringId64> itemSpawnPoints = {};
        std::vector<StringId64> spawnedItems = {};

        float GetDistanceToPlayer() 
        {
            return LOCATOR::GET_LOCATOR_DISTANCE(LOCATOR::GET_LOCATOR(SID("player")), LOCATOR::GET_LOCATOR(boxSID));
        }

        bool CanOpenSupplyBox(float currentTime)
        {
            return (currentTime >= refillTime && !opened);
        }

        void CreateSupplyBoxInteractable() 
        {
            if (!boxSID) return;

            uint64_t interactLocator = LOCATOR::GET_LOCATOR(boxSID);

            if (!interactLocator) return;

            Vector3 interactPos = LOCATOR::GET_LOCATOR_POSITION(interactLocator);
            Quaternion interactRot = LOCATOR::GET_LOCATOR_ROTATION(interactLocator);

            interactPos.y += 0.2f; //offset vertically a little

            StringId64 interactable = DC::Call<"spawn-entity-at">(SID("to1-interact-generic-1"), MEMORY::ALLOC_BOUND_FRAME(interactPos, interactRot), 0);

            MEMORY::ALLOC_BOUND_FRAME_OBJECT(interactable, boxSID, 0);

            interactableSID = interactable;
        }

        void EnableSupplyBoxInteraction()
        {
            DC::Call<"interactable-enable">(interactableSID);
            DC::Call<"set-high-contrast-mode-type">(boxSID, 2);
            DC::Call<"listen-mode-set-buddy-draw-mode">(boxSID, 1);
            DC::Call<"listen-mode-buddy-draw-mode-force-interactive">(boxSID, 1);
            DC::Call<"listen-mode-visible">(boxSID, 1, 1, -1.00);
        }

        void DisableSupplyBoxInteraction()
        {
            DC::Call<"interactable-disable">(interactableSID);
            DC::Call<"set-high-contrast-mode-type">(boxSID, 1);
            DC::Call<"listen-mode-set-buddy-draw-mode">(boxSID, 0);
            DC::Call<"listen-mode-buddy-draw-mode-force-interactive">(boxSID, 2);
            DC::Call<"listen-mode-visible">(boxSID, 0, 1, -1.00);
        }
    };

    inline std::vector<SupplyBox> supplyBoxes;
    inline std::vector<SupplyBox> sortedAvailableSupplyBoxes;

    StringId64 SpawnPickupableItem(StringId64 itemSID, int quantity, Vector3 pos, Quaternion rot);
    int CalculateItemAmountToSpawn(net::replication::MatchState state);

    void UpdateSupplyBoxes(float dt); 
    void UpdateSupplyBoxesInteractBridge();
    void OpenSupplyBox(SupplyBox& box);
    void OpenSupplyBoxByIndex(int boxIndex);
    void SpawnItemsFromSupplyBox(SupplyBox& box);
    void ResetSupplyBox(SupplyBox& box);

    // Looks
    const char* RandomLookForTeam(int team);
    const char* DefaultLook(int netSlot);

}