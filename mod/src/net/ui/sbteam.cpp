// ported from tools/ext/sbteam.c (build-201 working tree, 102-line version).
// The ext's block move fixed winmm's team-0-on-top fill. The native fill already
// puts our team on top, so moving the blocks here would invert a correct board;
// only the place-colour band pair is owned natively. No settext hook is needed:
// the mod's scoreboard writes go straight through engine::widget, and the
// observer slot is still used by search-rows.

#include "sbteam.hpp"

#include "boot.hpp"
#include "engine/flag.hpp"
#include "engine/lobby.hpp"
#include "engine/widget.hpp"
#include "log.hpp"
#include "net/ui/net-gui2.hpp"

#include <cstdint>

namespace net::ui::sbteam {

    namespace {

        struct Owned {
            uint64_t page = 0;
            uint64_t blue = 0, red = 0;
            uint64_t stock0 = 0, stock1 = 0;
            bool     applied = false;
        } g;

        bool Disabled()
        {
            return boot::ExtLoaded() ||
                   engine::flag::Present("t2r-no-sbteam.txt") ||
                   engine::lobby::Substate() < 3 ||
                   (!net::gui2::ScoreboardShowing() && !net::gui2::PauseMenuOpen());
        }

        void Restore()
        {
            if (!g.applied) return;
            if (g.stock0) engine::widget::Visible(g.stock0, true);
            if (g.stock1) engine::widget::Visible(g.stock1, true);
            if (g.blue)   engine::widget::Visible(g.blue, false);
            if (g.red)    engine::widget::Visible(g.red, false);
            g.applied = false;
            g.page = 0;
            g.blue = g.red = g.stock0 = g.stock1 = 0;
        }

    } // namespace

    void Tick(uint64_t)
    {
        if (Disabled()) {
            Restore();
            return;
        }

        const uint64_t page = engine::widget::TopPage();
        if (!page) return;

        const uint64_t blue = engine::widget::Page(
            "anim-group.scoreboard.sbBlue");
        const uint64_t red = engine::widget::Page(
            "anim-group.scoreboard.sbRed");
        const uint64_t stock0 = engine::widget::Page(
            "anim-group.scoreboard.team0_band");
        const uint64_t stock1 = engine::widget::Page(
            "anim-group.scoreboard.team1_band");
        if (!blue || !red || !stock0 || !stock1) {
            Restore();   // old gui2-widgets.pak: keep the stock bands
            return;
        }
        if (g.applied && g.page == page) return;
        Restore();

        g.page = page;
        g.blue = blue;
        g.red = red;
        g.stock0 = stock0;
        g.stock1 = stock1;

        // The ext would swap both blocks here when our roster team is 1. The
        // native fill already used our team for display team 0, so the blocks
        // stay put and only the colours follow the place.
        engine::widget::Visible(stock0, false);
        engine::widget::Visible(stock1, false);
        engine::widget::Visible(blue, true);
        engine::widget::Visible(red, true);
        engine::widget::Alpha(blue, 1.0f);
        engine::widget::Alpha(red, 1.0f);
        g.applied = true;
        TO1_LOG("sbteam: our team %d - native fill owns the order; %s; sbBlue / sbRed placed on the two bands\n",
            static_cast<int>(engine::lobby::OwnTeam()),
            core::WantsPhysicalSwap(static_cast<int>(engine::lobby::OwnTeam()))
                ? "ext would have swapped blocks"
                : "no block swap needed");
    }

    bool Active()
    {
        return g.applied && !Disabled();
    }

    void Reset()
    {
        Restore();
    }

} // namespace net::ui::sbteam
