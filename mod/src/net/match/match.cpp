#include "match.hpp"
#include "anim.hpp"
#include "net-common.hpp"
#include "net-session.hpp"
#include "net-replication.hpp"
#include "net-matchmaking.hpp"
#include "net-relayping.hpp"
#include "net-tamper.hpp"
#include "net-config.hpp"
#include "net-gui2.hpp"
#include "net-hooks.hpp"
#include "menu/net-menu.hpp"
#include "kill-feed.hpp"
#include "net-debug.hpp"

bool multiplayer = false;

namespace net::match {

    static constinit Match g_theMatch;

    Match& Get() { return g_theMatch; }

    void Match::EnsurePlayersInit()
    {
        if (playersInit) return;
        for (int i = 0; i < kMaxNetPlayers; i++)
            players[i].netSlot = i;
        playersInit = true;
    }

    void Match::ClearMatchmade()
    {
        if (source == Source::Matchmade)
            source = Source::None;
        lobbyPhase = -1;
        lobbyCountdown = 0;
    }


    static constexpr int kSpawnPointsFallback = 8;

    int Match::SpawnCount() const
    {
        if (IsMatchmade()) {
            const int n = static_cast<int>(net::config::Map(info.map).spawns);
            if (n > 0) return n;
        }
        return kSpawnPointsFallback;
    }

    int Match::TeamOf(int slot)
    {
        if (slot < 0 || slot >= kMaxNetPlayers)
            return net::matchmaking::SlotTeam(slot);
        EnsurePlayersInit();
        const NetPlayer& p = players[slot];
        return (p.team >= 0) ? p.team : net::matchmaking::SlotTeam(slot);
    }

    int Match::MyTeam() const
    {
        return IsMatchmade() ? static_cast<int>(info.yourTeam) : -1;
    }

    void Match::DecrementLivesForTeam(int absTeam)
    {
        if (absTeam != 0 && absTeam != 1) return;
        if (teamLives[absTeam] > 0) teamLives[absTeam]--;
    }

    int  Match::LivesForTeam(int absTeam) const
    {
        return (absTeam == 0 || absTeam == 1) ? teamLives[absTeam] : 0;
    }

    void Match::SetLivesAbsolute(int t0, int t1)
    {
        teamLives[0] = (t0 < 0) ? 0 : (t0 > kStartLives ? kStartLives : t0);
        teamLives[1] = (t1 < 0) ? 0 : (t1 > kStartLives ? kStartLives : t1);
    }

    // Blue = OUR side, red = theirs
    int Match::BlueLives() const { return teamLives[(MyTeam() == 1) ? 1 : 0]; }
    int Match::RedLives() const  { return teamLives[(MyTeam() == 1) ? 0 : 1]; }

    int Match::MapIndex() const
    {
        const int m = static_cast<int>(info.map);
        return (m < net::config::MapCount()) ? m : 0;
    }

    void Match::TickRelayHandoff()
    {
        if (InLiveMatch()) {
            if (!relayStarted) {
                relayStarted = true;
                if (const net::matchmaking::MatchInfo* m = net::matchmaking::GetMatch()) {
                    if (m->yourSlot == 0)
                        net::session::HostGameRelay(m->relayIp, m->relayPort, m->sessionId);
                    else
                        net::session::JoinGameRelay(m->relayIp, m->relayPort, m->sessionId, m->yourSlot);
                }
            }
        }
        else {
            if (relayStarted) {             // match ended / reset -> tear the session down
                relayStarted = false;
                net::session::Leave();      // goodbye + despawn remotes + full session reset
            }
        }
    }


    void Match::Reset(ResetReason reason)
    {
        if (reason == ResetReason::Dissolved) {
            ClearMatchmade();
            return;
        }

        const bool leaveToMenu = (reason == ResetReason::LeaveToMenu);
        const bool frontEnd = (reason == ResetReason::AbortToMenu) ||
                              (leaveToMenu && InLiveMatch());

        if (leaveToMenu) {
            if (net::session::IsRunning()) {
                net::session::LeaveInternal(true);
                HUD::SHOW_NOTIFICATION("Left the session", 4.0f);
            }
        }
        else if (reason == ResetReason::SessionLost) {
            net::session::LeaveInternal(false);
        }

        if (source == Source::DirectIp)
            source = Source::None;

        if (!frontEnd)
            return;

        if (leaveToMenu)
            NetPlayers::Reset();

        if (leaveToMenu)
            FinishDeferredHookRemoval();

        if (!leaveToMenu)
            matchArmed = false;

        net::menu::ResetMpAssetState();

        if (leaveToMenu) {
            teamLives[0] = kStartLives;
            teamLives[1] = kStartLives;
            matchTimer.Clear();
            endSeq = EndSeq::Idle;
            matchArmed = false;
        }

        net::gui2::ResetHudLatches();
        ResetKillFeed();

        DEBUG::PLAY_TASK(SID("to1-levels"), SID("to1-main-menu"), 0);
        SetPhase(Phase::FrontEndLoading);
    }

    void Match::FinishDeferredHookRemoval()
    {
        net::hooks::RemoveMultiplayerHooks();
    }

}

void NetTick()
{
    NetAnimTickEarly();
    net::gui2::UpdateMenu();
    net::relayping::Tick();
    net::matchmaking::Tick();
    net::tamper::Tick();
    net::match::Get().TickRelayHandoff();

    if (!multiplayer || !net::session::IsRunning())
        return;

    net::session::Update();

    if (!net::session::IsConnected())
        return;

    net::replication::TickMatchAuthority(TIME::GET_DELTA_TIME());

    if (!ENTITY::DOES_EXIST(SID("player")))
        return;

    net::replication::Tick();
}
