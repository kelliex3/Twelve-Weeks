// Native owner of build-191 tools/ext/sbteam.c. winmm fills the two team
// blocks by roster team; our team must be the upper one. The ext reads the
// CURRENT placement from the two team-name pivots and moves each block by the
// measured distance -- the pause page and the scoreboard page have different
// y, so the swap is relative, not an absolute 474-unit write. Colour follows
// the place: the blue / red bands are static and are never touched here.
// Yields while t2r-ext is loaded; no sidecar is written.

#include "sbteam.hpp"

#include "boot.hpp"
#include "engine/flag.hpp"
#include "engine/lobby.hpp"
#include "engine/widget.hpp"
#include "log.hpp"

#include <cstdint>
#include <cstdio>

namespace net::ui::sbteam {

    namespace {

        struct Owned {
            bool active = false;    // this page has resolvable team-name pivots
        } g;

        // ext SbTick: the local player's roster entry carries the team bit.
        // With no readable entry the ext leaves swap = 0.
        bool WantsSwap(bool& known)
        {
            known = false;
            const uint8_t me = engine::lobby::LocalSlot();
            if (me >= engine::lobby::kRosterMax) return false;
            const int n = engine::lobby::RosterCount();
            for (int i = 0; i < n; ++i) {
                const uint8_t* e = engine::lobby::RosterEntry(i);
                if (e && e[0] == me) {
                    known = true;
                    return core::WantsPhysicalSwap(e[0x31]);
                }
            }
            return false;
        }

        void Shift(int team, const char* leaf, float dy, int& moved)
        {
            char rel[128];
            std::snprintf(rel, sizeof rel,
                          "anim-group.scoreboard.team%d_%s", team, leaf);
            const uint64_t w = engine::widget::Page(rel);
            if (!w) return;
            float x = 0.0f, y = 0.0f;
            if (!engine::widget::PivotGet(w, &x, &y)) return;
            engine::widget::Pivot(w, x, y + dy);
            ++moved;
        }

        void PlaceBlocks(bool want)
        {
            const uint64_t n0 = engine::widget::Page("anim-group.scoreboard.team0_name");
            const uint64_t n1 = engine::widget::Page("anim-group.scoreboard.team1_name");
            float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
            g.active = n0 && n1 && engine::widget::PivotGet(n0, &x0, &y0)
                       && engine::widget::PivotGet(n1, &x1, &y1);
            if (!g.active) return;

            const core::SwapRead read = core::ReadSwap(y0, y1);
            if (!core::NeedsSwap(read, want)) return;
            int moved = 0;
            for (int team = 0; team < 2; ++team) {
                const float dy = core::ShiftDelta(read, want, team);
                for (const char* leaf : core::kBlockParts)
                    Shift(team, leaf, dy, moved);
                for (int row = 0; row < core::kRows; ++row)
                    for (const char* part : core::kRowParts) {
                        char leaf[48];
                        std::snprintf(leaf, sizeof leaf, "row%d_%s", row, part);
                        Shift(team, leaf, dy, moved);
                    }
            }
            TO1_LOG("sbteam: our team is team %d - %s block on top (%d widgets placed)\n",
                    want ? 1 : 0, want ? "team 1" : "team 0", moved);
        }

    } // namespace

    void Tick(uint64_t now)
    {
        (void)now;
        if (boot::ExtLoaded()) { g.active = false; return; }
        if (!engine::widget::TopPage()) { g.active = false; return; }

        bool known = false;
        const bool want = WantsSwap(known);
        // Opt-out and an unknown local team both put the template blocks back.
        const bool target = known && !engine::flag::Present("t2r-no-sbteam.txt") && want;
        PlaceBlocks(target);
    }

    bool Active() { return g.active && !boot::ExtLoaded(); }

    void Reset() { g.active = false; }

} // namespace net::ui::sbteam
