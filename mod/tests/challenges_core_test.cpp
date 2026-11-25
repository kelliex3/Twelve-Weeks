// Standalone harness for the CHALLENGES grid core:
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -I mod/src/net/ui
//       mod/tests/challenges_core_test.cpp -o /tmp/challenges_core_test
//   /tmp/challenges_core_test
//
// Pins row building (OG / KELLIE / ACH items are not challenges), focus
// retention, equip validation and the first-visit tip latch.

#include "challenges-core.hpp"

#include <cstdio>

namespace {

    using namespace net::ui::customize::challenges::core;

    int fails = 0;
    void Check(bool ok, const char* what)
    {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++fails;
    }

    void GridTests()
    {
        Profile p;
        Defaults(p);
        Grid g;
        const int n = RowsOf(0, Sex(p, 0), p, false, g);
        Check(n > 0 && n <= kWearCats, "only non-empty challenge categories become rows");
        int done = 0, total = 0;
        Count(p, 0, true, false, done, total);
        Check(total > 0 && done <= total, "both-sex counts cover every challenge item");
        Check(PlainKind(CZK_OG) && PlainKind(CZK_KELLIE) && PlainKind(CZK_ACH) &&
              !PlainKind(CZK_WEEKS),
              "OG / KELLIE / ACH are plain lines, not challenge rows");
    }

    void FocusTests()
    {
        Profile p;
        Defaults(p);
        State s;
        Enter(s, p, 0, false, 0, 1000);
        Check(s.up && s.grid.nrow > 0 && s.fr == 0, "the page opens on the first category");
        Check(FadeStep(s, 1000) == 0 && FadeStep(s, 1000 + kEnterFadeMs) == 24,
              "the group alpha quantises to 24 steps over the fade-in");

        const int want = s.want;
        MoveRow(s, 1);
        if (s.grid.nrow > 1) {
            Check(s.fr == 1 && s.want == want,
                  "down keeps the absolute screen column between rows");
        }
        MoveRow(s, -1);
        Check(s.fr == 0, "up returns to the first row");
        if (s.grid.nrow > 2) {
            ScrollRows(s, 1);
            Check(s.row0 == 0, "scroll clamps at the top");
            ScrollRows(s, -s.grid.nrow);
            Check(s.row0 == s.grid.nrow - kScreenRows, "scroll clamps at the bottom");
        }

        Check(SetFac(s, p, 1, false) && s.fac == 1 && s.fr == 0,
              "Q / E switches faction and rebuilds the grid from the top");
        Check(!SetFac(s, p, 1, false), "selecting the current faction is a no-op");
        Check(!SetFac(s, p, -1, false) && SetFac(s, p, -2, false) && s.fac == 0,
              "a negative index wraps modulo the faction count");
    }

    void HoverEquipTests()
    {
        Profile p;
        Defaults(p);
        State s;
        Enter(s, p, 0, false, 0, 1000);
        ScanHover(s, -1.0f, -1.0f, false, false);
        Check(s.hvR == -1 && !ClickHit(s), "no cursor means no hover and no click gate");

        float x, y;
        TileXY(0, 0, x, y);
        ScanHover(s, x + 1.0f, y + 1.0f, false, true);
        Check(s.hvR == 0 && s.hvC == 0 && ClickHit(s),
              "the tile geometry resolves a cursor to a grid cell");

        if (s.grid.nrow > 0 && s.grid.rn[0] > 0) {
            const int cat = s.grid.rcat[0], item = s.grid.rk[0][0];
            EquipResult r = CanEquip(s, p, s.fac, cat, item);
            Check(r.denied && !r.changed,
                  "an unmet challenge is refused without changing the profile");
            (void)GrantAdd(p, Item(s.fac, Sex(p, s.fac), cat, item)->label);
            r = CanEquip(s, p, s.fac, cat, item);
            Check(r.changed || p.it[s.fac][Sex(p, s.fac)][cat] == item,
                  "a granted challenge item becomes equippable");
        }
    }

    void TipTests()
    {
        Profile p;
        Defaults(p);
        Check(TipWanted(p), "the tip is wanted on a fresh profile");
        Tip t;
        TipShow(t, 1000);
        Check(t.up, "showing the tip latches it up");
        TipDone(t, p);
        Check(!t.up && (p.flags & kFlagChTip) && !TipWanted(p),
              "dismissing the tip sets kFlagChTip so it never returns");
    }

} // namespace

int main()
{
    GridTests();
    FocusTests();
    HoverEquipTests();
    TipTests();
    if (fails) {
        std::printf("%d failure(s)\n", fails);
        return 1;
    }
    std::printf("challenges core: all checks passed\n");
    return 0;
}
