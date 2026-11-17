#include "net-vox.hpp"
#include "net-common.hpp"
#include "net-protocol.hpp"
#include "net-player.hpp"
#include "match.hpp"
#include "net-session.hpp"
#include "net-matchmaking.hpp"
#include "net-debug.hpp"

#include <random>

namespace net::vox {

    static const char* const kSeraphIntro[] = {
        "vox-igc-to1-scrma-intro-002", "vox-igc-to1-scrma-intro-003",
        "vox-igc-to1-scrma-intro-004", "vox-igc-to1-scrma-intro-005",
        "vox-igc-to1-scrma-intro-006", "vox-igc-to1-scrma-intro-007",
        "vox-igc-to1-scrma-intro-008",
    };
    static const char* const kWlfIntro[] = {
        "vox-igc-to1-mltma-intro-002", "vox-igc-to1-mltma-intro-003",
        "vox-igc-to1-mltma-intro-004", "vox-igc-to1-mltma-intro-005",
        "vox-igc-to1-mltma-intro-006", "vox-igc-to1-mltma-intro-007",
        "vox-igc-to1-mltma-intro-008", "vox-igc-to1-mltma-intro-009",
        "vox-igc-to1-mltma-intro-010", "vox-igc-to1-mltma-intro-011",
        "vox-igc-to1-mltma-intro-012", "vox-igc-to1-mltma-intro-013",
    };
    static const char* const kSeraphMarked[] = {
        "vox-igc-to1-scrma-marked-001", "vox-igc-to1-scrma-marked-002",
        "vox-igc-to1-scrma-marked-003", "vox-igc-to1-scrma-marked-004",
        "vox-igc-to1-scrma-marked-005", "vox-igc-to1-scrma-marked-006",
        "vox-igc-to1-scrma-marked-007", "vox-igc-to1-scrma-marked-008",
        "vox-igc-to1-scrma-marked-009", "vox-igc-to1-scrma-marked-010",
        "vox-igc-to1-scrma-marked-011", "vox-igc-to1-scrma-marked-012",
        "vox-igc-to1-scrma-marked-013", "vox-igc-to1-scrma-marked-014",
        "vox-igc-to1-scrma-marked-015", "vox-igc-to1-scrma-marked-016",
    };
    static const char* const kWlfMarked[] = {
        "vox-igc-to1-mltma-marked-001", "vox-igc-to1-mltma-marked-002",
        "vox-igc-to1-mltma-marked-003", "vox-igc-to1-mltma-marked-004",
        "vox-igc-to1-mltma-marked-005", "vox-igc-to1-mltma-marked-006",
        "vox-igc-to1-mltma-marked-007", "vox-igc-to1-mltma-marked-008",
        "vox-igc-to1-mltma-marked-009", "vox-igc-to1-mltma-marked-010",
        "vox-igc-to1-mltma-marked-011", "vox-igc-to1-mltma-marked-012",
        "vox-igc-to1-mltma-marked-013", "vox-igc-to1-mltma-marked-014",
        "vox-igc-to1-mltma-marked-015",
    };
    static const char* const kSeraphHealFinish[] = {
        "vox-igc-to1-scrma-heal-finish-001", "vox-igc-to1-scrma-heal-finish-002",
        "vox-igc-to1-scrma-heal-finish-003", "vox-igc-to1-scrma-heal-finish-004",
        "vox-igc-to1-scrma-heal-finish-005", "vox-igc-to1-scrma-heal-finish-006",
    };
    static const char* const kWlfHealFinish[] = {
        "vox-igc-to1-mltma-heal-finish-001", "vox-igc-to1-mltma-heal-finish-002",
        "vox-igc-to1-mltma-heal-finish-003", "vox-igc-to1-mltma-heal-finish-004",
        "vox-igc-to1-mltma-heal-finish-005", "vox-igc-to1-mltma-heal-finish-006",
    };
    static const char* const kSeraphKill[] = {
        "vox-igc-to1-scrma-kill-001", "vox-igc-to1-scrma-kill-002",
        "vox-igc-to1-scrma-kill-003", "vox-igc-to1-scrma-kill-004",
        "vox-igc-to1-scrma-kill-005", "vox-igc-to1-scrma-kill-006",
        "vox-igc-to1-scrma-kill-007", "vox-igc-to1-scrma-kill-008",
        "vox-igc-to1-scrma-kill-009",
    };
    static const char* const kWlfKill[] = {
        "vox-igc-to1-mltma-kill-001", "vox-igc-to1-mltma-kill-002",
        "vox-igc-to1-mltma-kill-003", "vox-igc-to1-mltma-kill-004",
        "vox-igc-to1-mltma-kill-005", "vox-igc-to1-mltma-kill-006",
        "vox-igc-to1-mltma-kill-007",
    };

    struct LineList { const char* const* items; int count; };

    template <size_t N>
    static constexpr LineList Lines(const char* const (&a)[N]) { return LineList{ a, static_cast<int>(N) }; }

    static LineList ListFor(Scenario s, int team)
    {
        const bool seraph = (team == MM_TEAM_SERAPHITES);
        const bool wlf    = (team == MM_TEAM_WLF);
        switch (s) {
        case Scenario::MatchIntro:
            if (seraph) return Lines(kSeraphIntro);
            if (wlf)    return Lines(kWlfIntro);
            break;
        case Scenario::MarkedEnemy:
            if (seraph) return Lines(kSeraphMarked);
            if (wlf)    return Lines(kWlfMarked);
            break;
        case Scenario::HealFinish:
            if (seraph) return Lines(kSeraphHealFinish);
            if (wlf)    return Lines(kWlfHealFinish);
            break;
        case Scenario::EnemyKill:
            if (seraph) return Lines(kSeraphKill);
            if (wlf)    return Lines(kWlfKill);
            break;
        }
        return LineList{ nullptr, 0 };
    }

    static const char* PickLine(Scenario s, int team)
    {
        const LineList l = ListFor(s, team);
        if (!l.items || l.count <= 0) return nullptr;
        static std::mt19937 rng{ std::random_device{}() };
        std::uniform_int_distribution<int> pick(0, l.count - 1);
        return l.items[pick(rng)];
    }

    static uint64_t AvatarSidForSlot(int slot)
    {
        if (slot < 0 || slot >= kMaxNetPlayers) return 0;
        if (slot == net::session::MySlot()) return SID("player");
        NetPlayer& p = NetPlayers::BySlot(slot);
        if (!p.IsActiveRemote() || !p.spawned || !p.npcSid) return 0;
        return p.npcSid;
    }

    static bool PlayLocal(int slot, uint64_t lineSid)
    {
        const uint64_t avatar = AvatarSidForSlot(slot);
        if (!avatar || !lineSid) return false;
        AUDIO::PLAY_VOICE_LINE(avatar, lineSid);
        TO1_LOG("[vox] play slot%d avatar=%llx line=%llx\n", slot,
                static_cast<unsigned long long>(avatar),
                static_cast<unsigned long long>(lineSid));
        return true;
    }

    static void Broadcast(int speakerSlot, uint64_t lineSid)
    {
        VoxLineEvent evt{ static_cast<uint8_t>(speakerSlot), lineSid };
        net::session::SendEvent(NEVT_VOX_LINE, evt);
    }

    enum class Audience : uint8_t { Everyone, TeamOnly };

    static Audience AudienceFor(Scenario s)
    {
        switch (s) {
        case Scenario::MatchIntro:  return Audience::TeamOnly;
        case Scenario::MarkedEnemy: return Audience::Everyone;
        case Scenario::HealFinish:  return Audience::TeamOnly;
        case Scenario::EnemyKill:   return Audience::Everyone;
        }
        return Audience::Everyone;
    }

    static void BroadcastToTeam(int speakerSlot, uint64_t lineSid)
    {
        VoxLineEvent evt{ static_cast<uint8_t>(speakerSlot), lineSid };
        for (NetPlayer& t : NetPlayers::Remotes()) {
            if (!t.active) continue;
            if (!NetPlayers::SlotIsFriendly(t.netSlot)) continue;
            net::session::SendEvent(NEVT_VOX_LINE, evt, static_cast<uint8_t>(t.netSlot));
        }
    }

    static float DelayFor(Scenario s)
    {
        switch (s) {
        case Scenario::MatchIntro:  return 0.0f;
        case Scenario::MarkedEnemy: return 0.0f;
        case Scenario::HealFinish:  return 0.0f;
        case Scenario::EnemyKill:   return 1.0f;
        }
        return 0.0f;
    }

    static void Emit(Scenario s, int slot, uint64_t lineSid)
    {
        PlayLocal(slot, lineSid);
        if (AudienceFor(s) == Audience::TeamOnly)
            BroadcastToTeam(slot, lineSid);
        else
            Broadcast(slot, lineSid);
    }

    // One slot rather than a queue: a second kill inside the delay window should not stack
    // a second taunt on top of the first. First one wins, the rest are dropped.
    struct Deferred { Scenario s; uint64_t lineSid; float left; bool armed; };
    static Deferred s_deferred{};

    void PlayForLocal(Scenario s)
    {
        const int mySlot = net::session::MySlot();
        const int myTeam = net::matchmaking::MyTeam();
        const char* name = PickLine(s, myTeam);
        if (!name) return; // team unresolved / no list -> stay silent

        const uint64_t lineSid = SID(name);
        const float    delay = DelayFor(s);

        if (delay <= 0.0f) {
            Emit(s, mySlot, lineSid);
            TO1_LOG("[vox] local slot%d team%d -> %s\n", mySlot, myTeam, name);
            return;
        }

        if (s_deferred.armed) {
            TO1_LOG("[vox] dropping %s, a line is already queued\n", name);
            return;
        }
        s_deferred = Deferred{ s, lineSid, delay, true };
        TO1_LOG("[vox] local slot%d team%d -> %s in %.2fs\n", mySlot, myTeam, name, delay);
    }

    static constexpr float kHeldLineMaxAgeSec = 4.0f;
    static constexpr int   kMaxHeldLines      = 8;

    struct HeldLine { int slot; uint64_t lineSid; float age; };
    static HeldLine s_pending[kMaxHeldLines];
    static int      s_pendingCount = 0;

    static void RetryPending(float dt)
    {
        int out = 0;
        for (int i = 0; i < s_pendingCount; ++i) {
            HeldLine& h = s_pending[i];
            h.age += dt;
            if (PlayLocal(h.slot, h.lineSid)) continue;
            if (h.age >= kHeldLineMaxAgeSec) {
                TO1_LOG("[vox] dropping held line for slot%d (no avatar after %.1fs)\n",
                        h.slot, h.age);
                continue;
            }
            s_pending[out++] = h;
        }
        s_pendingCount = out;
    }

    void HandleLine(uint8_t speakerSlot, uint64_t lineSid)
    {
        const int slot = static_cast<int>(speakerSlot);
        if (PlayLocal(slot, lineSid)) return;

        if (s_pendingCount >= kMaxHeldLines) {
            TO1_LOG("[vox] held-line queue full, dropping line for slot%d\n", slot);
            return;
        }
        s_pending[s_pendingCount++] = HeldLine{ slot, lineSid, 0.0f };
        TO1_LOG("[vox] holding line for slot%d — no avatar yet\n", slot);
    }

    // match intro - one speaker per team
    static constexpr float kIntroSettleSec = 2.0f;
    static constexpr float kIntroGiveUpSec = 15.0f;

    static bool  s_introArmed = false;
    static float s_introAge   = 0.0f;
    static bool  s_introTeamDone[2] = { false, false };

    void OnMatchLive()
    {
        if (s_introArmed) return;
        s_introArmed = true;
        s_introAge   = 0.0f;
        s_introTeamDone[0] = s_introTeamDone[1] = false;
        TO1_LOG("[vox] intro armed\n");
    }

    static bool TryIntroForTeam(int team)
    {
        int slots[kMaxNetPlayers];
        int n = 0;
        bool anyOnTeam = false;
        for (int slot = 0; slot < kMaxNetPlayers && n < kMaxNetPlayers; ++slot) {
            NetPlayer& p = NetPlayers::BySlot(slot);
            const bool isMe = (slot == net::session::MySlot());
            if (!isMe && !p.IsActiveRemote()) continue;
            if (isMe && !p.active) continue;
            const int t = isMe ? ((p.team >= 0) ? p.team : net::matchmaking::MyTeam())
                               : net::match::Get().TeamOf(slot);
            if (t != team) continue;
            anyOnTeam = true;
            if (AvatarSidForSlot(slot)) slots[n++] = slot;
        }

        if (!anyOnTeam) return true;    // nobody on this side at all -> nothing to say, ever
        if (n == 0)     return false;   // players exist but none has spawned yet -> retry

        static std::mt19937 rng{ std::random_device{}() };
        std::uniform_int_distribution<int> pick(0, n - 1);
        const int speaker = slots[pick(rng)];

        const char* name = PickLine(Scenario::MatchIntro, team);
        if (!name) return false;

        const uint64_t lineSid = SID(name);

        PlayLocal(speaker, lineSid);
        Broadcast(speaker, lineSid);
        TO1_LOG("[vox] intro team%d: slot%d of %d speaks -> %s\n", team, speaker, n, name);
        return true;
    }

    void Tick(float dt)
    {
        RetryPending(dt);

        if (s_deferred.armed) {
            if (PLAYER::IS_IN_STATE(SID("Death"))) {
                s_deferred.armed = false; // died before it landed -> no enemykill voice line
            }
            else {
                s_deferred.left -= dt;
                if (s_deferred.left <= 0.0f) {
                    s_deferred.armed = false;
                    Emit(s_deferred.s, net::session::MySlot(), s_deferred.lineSid);
                }
            }
        }

        if (!s_introArmed) return;
        s_introAge += dt;

        // Host-authoritative on purpose. If each client rolled its own speaker they would disagree
        // and you would hear several people deliver the same team's intro, which is precisely the
        // behaviour being replaced. One machine choosing and announcing is the only way every
        // listener agrees on who spoke.
        if (!net::session::IsHost()) { s_introArmed = false; return; }

        if (s_introAge < kIntroSettleSec) return;

        if (s_introAge >= kIntroGiveUpSec) {
            if (!s_introTeamDone[0] || !s_introTeamDone[1])
                TO1_LOG("[vox] intro gave up (team0 done=%d, team1 done=%d)\n",
                        s_introTeamDone[0], s_introTeamDone[1]);
            s_introArmed = false;
            return;
        }

        for (int team = 0; team < 2; ++team)
            if (!s_introTeamDone[team])
                s_introTeamDone[team] = TryIntroForTeam(team);

        if (s_introTeamDone[0] && s_introTeamDone[1])
            s_introArmed = false; // both sides handled -> stop ticking the intro
    }

    void Reset()
    {
        s_introArmed = false;
        s_introAge   = 0.0f;
        s_introTeamDone[0] = s_introTeamDone[1] = false;
        s_pendingCount = 0;
        s_deferred = Deferred{};
    }

}