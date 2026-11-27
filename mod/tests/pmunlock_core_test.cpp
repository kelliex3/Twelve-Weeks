#include "pmunlock-core.hpp"

#include <cstring>
#include <iostream>

using namespace net::ui::pmunlock::core;

namespace {

    int failed = 0;

    void Check(bool ok, const char* what)
    {
        if (!ok) ++failed;
        std::cout << (ok ? "ok   " : "FAIL ") << what << '\n';
    }

} // namespace

int main()
{
    std::cout << "pmunlock core:\n";

    {
        Grid now{};
        Snapshot snap;
        now[1][0][3][8] = true;              // unlocked before the match
        TakeSnapshot(now, snap);
        Check(snap.have, "a match-start copy is marked valid");
        Check(snap.was[Cell(1, 0, 3, 8)] == 1, "an existing unlock is captured");
        Check(snap.was[Cell(1, 0, 3, 9)] == 0, "a locked cell stays zero");
        now[1][0][3][8] = false;
        now[1][0][3][9] = true;
        Lists out;
        Grid ok{}, off{};
        ok[1][0][3][9] = true;
        const int sex[kFac] = { 1, 0 };
        Build(snap, now, ok, off, sex, nullptr, 0, 1, 100, out);
        Check(out.ng == 1 && out.g[0].f == 1 && out.g[0].k == 9,
              "a cell that was already unlocked is not shown again");
    }

    {
        Grid before{}, now{}, eligible{}, off{};
        now[1][0][3][8] = true;              // captured as already unlocked
        Snapshot snap;
        TakeSnapshot(before, snap);

        now[1][0][3][7] = true;              // newly unlocked WLF item
        now[1][0][2][5] = true;              // newly unlocked, but story-locked
        now[1][0][3][8] = true;              // a late table reply re-adds this cell
        now[0][1][1][1] = true;              // newly unlocked Seraphite item
        eligible[1][0][3][7] = true;
        eligible[1][0][2][5] = true;
        eligible[1][0][3][8] = true;
        eligible[0][1][1][1] = true;
        off[1][0][2][5] = true;

        const int sex[kFac] = { 1, 0 };
        Lists out;
        Build(snap, now, eligible, off, sex, nullptr, 0, 1, 500, out);
        Check(out.ng == 3 && out.ngt == 3, "only fresh, eligible gear is counted");
        Check(out.g[0].f == 1 && out.g[0].c == 3 && out.g[0].k == 7,
              "the faction-first item is the first row");
        Check(out.g[1].f == 1 && out.g[1].k == 8,
              "a late table reply is included in the next rebuild");
        Check(out.g[2].f == 0 && out.g[2].c == 1 && out.g[2].k == 1,
              "the other faction follows the worn-sex order");
        Check(out.facFirst == 1 && out.builtAt == 500,
              "the build records its faction and time");

        // 30 more WLF items on a later wear row: the cap must be reached while
        // the same faction is still being walked, so nothing from faction 0
        // can displace it.
        for (int k = 0; k < 30; ++k) {
            now[1][0][6][k] = true;
            eligible[1][0][6][k] = true;
        }
        Build(snap, now, eligible, off, sex, nullptr, 0, 1, 900, out);
        Check(out.ng == kMaxG && out.ngt == 33, "the gear rows cap at 24");
        Check(out.g[kMaxG - 1].f == 1, "the cap keeps faction-first ordering");

        // Flipping the clan faction rebuilds and reverses the order.
        Lists other;
        Build(snap, now, eligible, off, sex, nullptr, 0, 0, 1200, other);
        Check(other.g[0].f == 0, "a faction flip puts Seraphite gear first");
        Check(NeedRebuild(1201, other, 1), "a faction flip needs a rebuild");
        Check(!NeedRebuild(1400, other, 0), "a settled list is not rebuilt");
    }

    {
        std::array<Achievement, kMaxA + 4> many{};
        for (int i = 0; i < static_cast<int>(many.size()); ++i)
            many[i] = Achievement{ "id", "title", "desc" };
        Lists out;
        Grid grid{};
        Snapshot snap;
        const int sex[kFac] = { 0, 0 };
        Build(snap, grid, grid, grid, sex, many.data(),
              static_cast<int>(many.size()), 0, 0, out);
        Check(out.na == kMaxA, "achievements cap at eight");
    }

    {
        Check(RowAlpha(0, 0) == 0.0f, "row 0 starts hidden");
        Check(RowAlpha(kFadeMs, 0) == 1.0f, "row 0 finishes after PMU_FADE");
        Check(RowAlpha(kStagMs, 1) == 0.0f, "row 1 waits its stagger");
        Check(RowAlpha(kStagMs + kFadeMs / 2, 1) == 0.5f,
              "row 1 fades over PMU_FADE");
        Check(RowAlpha(AnimMs(), kRows - 1) == 1.0f,
              "the last row is up by AnimMs");
        Check(AnimMs() == 3 * kStagMs + kFadeMs, "AnimMs covers all four rows");
    }

    {
        char b[96];
        MoreText(4, b, sizeof b);
        Check(std::strcmp(b, "") == 0, "no more-line for a full grid");
        MoreText(9, b, sizeof b);
        Check(std::strcmp(b, "+5 MORE IN CLAN | ACHIEVEMENTS") == 0,
              "the achievement overflow names the clan");
        GearFootText(2, b, sizeof b);
        Check(std::strcmp(b, "EQUIP THEM IN CUSTOMIZE") == 0,
              "a short gear grid still invites equipping");
        GearFootText(7, b, sizeof b);
        Check(std::strcmp(b, "+3 MORE  |  EQUIP THEM IN CUSTOMIZE") == 0,
              "the gear overflow keeps its exact spacing");
        Check(GearFootY(4) == 1580.0f, "the gear foot sits below row 3");
    }

    if (failed) {
        std::cout << failed << " pmunlock core check(s) failed\n";
        return 1;
    }
    std::cout << "pmunlock core: all checks passed\n";
    return 0;
}
