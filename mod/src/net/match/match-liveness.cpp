#include "match-liveness.hpp"

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
        l.live = net::gui2::InMatch();
        l.over = net::gui2::MatchExpired() || net::gui2::ScoreboardShowing();
        // One frame of grace is already in wasLive: the frame the relay session is
        // torn down InMatch() can be false while the scoreboard is still up.
        l.hold = (l.live || l.wasLive) && l.over;
    }

    bool Live()        { return g_latch.live; }
    bool WasLive()     { return g_latch.wasLive; }
    bool MatchEndHold(){ return g_latch.hold; }

    void Reset()
    {
        g_latch = Latch{};
    }

}
