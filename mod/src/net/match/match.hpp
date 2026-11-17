#pragma once

#include "net-player.hpp"
#include "net-matchmaker-protocol.hpp"
#include "net-timer.hpp"
#include "gameplay/perks/perk.hpp"
#include "gameplay/loadout/weapons.hpp"

namespace net::match {

    inline constexpr int kStartLives = 20;
    inline constexpr float kMatchSeconds = 15.0f * 60.0f;   // 15:00
    inline constexpr float kForceKillDamage = 250.0f;

    struct MatchInfo {
        char          relayIp[16];          // relay IPv4, dotted, nul-terminated
        uint16_t      relayPort;
        uint32_t      sessionId;
        uint8_t       map;                  // server-selected map id for this match
        uint8_t       yourSlot;             // 0 = host
        uint8_t       playerCount;          // entries valid in roster[]
        uint8_t       yourSpawn;            // your assigned spawn point (1-indexed; 0 = none)
        uint8_t       yourTeam;             // your matchmaker-assigned team (0 = SERAPHITES, 1 = WLF)
        MMRosterEntry roster[MM_MAX_SLOTS]; // slot + name + loadout + team for each player
    };

    enum class Source : uint8_t { None, DirectIp, Matchmade };

    //     Idle               game's official main menu
    //     FrontEndLoading    MP menu LEVEL streaming in
    //     FrontEnd           MP front-end live in-level
    //     LoadingPlaceSpawn  map task loading; its exit places us on our spawn
    //     LoadingAssets      MP asset levels streaming
    //     LoadingWaitStart   at the server's load barrier
    //     Live               match live; HUD by replication

    enum class Phase : uint8_t {
        Idle, FrontEndLoading, FrontEnd,
        LoadingPlaceSpawn, LoadingAssets, LoadingWaitStart,
        Live, End
    };

    enum class EndSeq : uint8_t { Idle, Scoreboard };
    enum class ResetReason : uint8_t { LeaveToMenu, SessionLost, Dissolved, AbortToMenu };

    struct PlayerStats { int kills = 0; int deaths = 0; int parts = 0; };

    struct SlotMatchState {
        bool  dead = false;
        float respawnGrace = 0.0f;
        float grantCooldown = 0.0f;
    };

    struct LocalPlayerState {
        // ---- respawn countdown (NetTimer-driven; see net-timer.hpp) --------
        NetTimer respawnTimer;                // 10s countdown
        int      respawnLastSec = -1;         // last whole-second a tick beep played on
        bool     wasDead = false;             // previous-frame Death state (edge detector)
        bool     respawnPending = false;
        float    respawnPendingSec = 0.0f;    // seconds since ask (re-ask at kGrantTimeoutSec)

        float    healProgress = 0.0f; // last frame's player-get-heal-progress
        float    healPeak = 0.0f; // highest progress seen during this heal

        // ---- kill attribution (victim side) --------------------------------
        int      firstLethalSlot = -1;
        uint64_t firstLethalWeapon = 0;
        bool     firstLethalHead = false;
        int      lastDamagerSlot = -1;
        uint64_t lastDamageWeapon = 0;
        bool     lastDamageHead = false;
        float    lastDamageAt = -1000.0f;     // seconds, same clock as Match::NowSec()

        int      killerSlot = -1;

        // out of lives spectate
        bool     elimSpec = false;
        float    elimSpecTime = 0.0f;
        int      specTargetSlot = -1;
        float    specOrphan = 0.0f;
        bool     specStuckLogged = false;
    };

    class Match {
    public:
        MatchInfo info{};
        Source    source = Source::None;

        bool started = false;             // MM_START received (everyone loaded)
        bool aborted = false;             // MM_ABORT received (match torn down)
        int  lobbyPhase = -1;             // server lobby phase: -1 none, 0 found, 1 map, 2 countdown
        int  lobbyCountdown = 0;          // server countdown seconds during phase 2

        // ---- relay-handoff latch ----------------------------------------------
        bool relayStarted = false;

        NetPlayer players[kMaxNetPlayers];

        Phase phase = Phase::Idle;
        int teamLives[2] = { kStartLives, kStartLives };
        NetTimer matchTimer;
        PlayerStats stats[kMaxNetPlayers];
        SlotMatchState slotState[kMaxNetPlayers];
        LocalPlayerState local;

        int loadoutPrim = gameplay::weapons::kEmpty;
        int loadoutSec = 0;
        int loadoutPurch = gameplay::weapons::kEmpty; // purchasable starts unslotted

        // stats[].parts is the scoreboard total. Spendable balance is that minus partsSpent
        int  partsSpent = 0;
        bool purchasableOwned = false;
        int  purchasableAmmo = 0; // carried through death, so you dont get given ammo on spawn

        gameplay::PerkIds perkIds = gameplay::PerkId::NONE;
        gameplay::PerkId  perkSlots[gameplay::kMaxEquippedPerks] = {};

        float nowSec = 0.0f;

        // ---- result section -----------------------------------------------------
        // Final match state, latched by gui2::BeginMatchOver() so the scoreboard shows the
        // values at the instant the match ended - ticks/events keep arriving during the
        // 10s hold, and the clock is stopped there too. finalWinner is the absolute
        // winning team latched at match-over (0/1, 2=draw, -1=unknown).
        int      finalBlueLives = 0;
        int      finalRedLives  = 0;
        float    finalSeconds   = 0.0f;
        int      finalWinner    = -1;
        EndSeq   endSeq            = EndSeq::Idle;
        uint64_t scoreboardShownMs = 0;
        bool     matchArmed = false;
        uint64_t matchLiveMs = 0;

        bool IsMatchmade() const { return source == Source::Matchmade; }
        Phase GetPhase() const   { return phase; }
        void  SetPhase(Phase p)  { phase = p; }
        bool  InLiveMatch() const { return phase == Phase::Live; }

        int TeamOf(int slot);
        int MyTeam() const;
        int  LivesForTeam(int absTeam) const;
        void DecrementLivesForTeam(int absTeam);  // the ONLY way a life is ever spent
        void SetLivesAbsolute(int t0, int t1);
        int  BlueLives() const;                   // viewer relative (blue = OUR side)
        int  RedLives() const;

        // ---- clock (thin wrappers over matchTimer; start/teardown stay in gui2) -
        void         TickMatch(float dt)                   { matchTimer.Tick(dt); }
        bool         MatchRunning() const                  { return matchTimer.Running(); }
        bool         MatchExpired() const                  { return matchTimer.Expired(); }
        NetTimerSync MatchSnapshot() const                 { return matchTimer.Snapshot(); }
        void         ApplyMatchSync(const NetTimerSync& s) { matchTimer.ApplySync(s); }

        float NowSec() const { return nowSec; }
        int MapIndex() const;
        void ClearMatchmade();
        int SpawnCount() const;
        void TickRelayHandoff();
        void Reset(ResetReason reason);
        void FinishDeferredHookRemoval();
        void EnsurePlayersInit();

    private:
        bool playersInit = false;
    };

    Match& Get();

}
