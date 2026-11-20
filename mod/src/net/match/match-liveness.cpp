#include "match-liveness.hpp"

#include "engine/lobby.hpp"
#include "net-gui2.hpp"

namespace net::match::liveness {

    namespace {
        struct Latch {
            bool live = false;
            bool wasLive = false;
            bool over = false;
            bool hold = false;
        } g_latch;
    }

    void Tick()
    {
        Latch& l = g_latch;
        l.wasLive = l.live;
        // Ext MatchSubstate() == 6, falling back to the mod's own phase only when the
        // engine byte is not readable (e.g. before the net context exists).
        const uint8_t sub = engine::lobby::Substate();
        l.live = sub ? sub == engine::lobby::kSubstateInMatch : net::gui2::InMatch();
        // The ext's LobbyObj()[0x2ca4] is the end banner / fade hold. Keep the mod's
        // end-screen gates in the OR so a lobby that is not up yet cannot lose an arm.
        l.over = engine::lobby::MatchOver() || net::gui2::MatchExpired()
            || net::gui2::ScoreboardShowing();
        // One frame of grace is already in wasLive: the frame the relay session is
        // torn down InMatch() can be false while the scoreboard is still up.
        l.hold = (l.live || l.wasLive) && l.over;
    }

    bool Live()        { return g_latch.live; }
    bool WasLive()     { return g_latch.wasLive; }
    bool MatchOver()   { return g_latch.over; }
    bool MatchEndHold(){ return g_latch.hold; }

    void Reset()
    {
        g_latch = Latch{};
    }

}
