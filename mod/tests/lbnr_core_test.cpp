// Standalone harness for tools/ext/lbnr.c row / cadence rules:
//   g++ -std=c++20 -Wall -Wextra -Werror -pedantic -I mod/src/net/ui
//       mod/tests/lbnr_core_test.cpp -o /tmp/opencode/lbnr_core_test
//   /tmp/opencode/lbnr_core_test

#include "lbnr-core.hpp"

#include <cstdio>
#include <cstring>

namespace {

    using namespace net::ui::lbnr::core;

    int fails = 0;
    void Check(bool ok, const char* what)
    {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++fails;
    }

} // namespace

int main()
{
    std::printf("lbnr core:\n");

    Check(kRows == 10 && kNameCap == 16, "ten rows and the cached local name cap");
    Check(kResolveMs == 3000 && kOwnMs == 250, "resolve / own-row cadences match ext");
    Check(kChipX == 1700.0f && kChipPt == 25.5f, "YOU chip stays on the generated x / point size");

    Check(IsMe("Player", "player"), "exact names match without case");
    Check(!IsMe("Player2", "player") && !IsMe("Player", "player2"),
          "extra characters are not an exact match");
    Check(IsMe("[T2R] Player", "player"), "a bracketed tag prefix is allowed");
    Check(IsMe("[T2R]Player", "player"), "a closing bracket is a valid separator");
    Check(IsMe("WLF Player", "PLAYER"), "a space-separated prefix is allowed");
    Check(!IsMe("xPlayer", "player"), "no separator means another name");
    Check(!IsMe("x-Player", "player"), "other separators are not a suffix match");
    Check(!IsMe("", "player") && !IsMe("Player", "") && !IsMe(nullptr, "player"),
          "empty names never match");

    Check(ShouldResolve(1000, 0, 1, 1, true), "the first resolve is always needed");
    Check(ShouldResolve(1000, 900, 2, 1, true), "a rebuilt page generation forces a resolve");
    Check(ShouldResolve(4000, 1000, 1, 1, true), "resolves repeat after 3000 ms");
    Check(!ShouldResolve(3999, 1000, 1, 1, true), "resolves do not repeat early");
    Check(ShouldResolve(10, 0, 0, 0, false), "a hidden page resolves on re-entry");
    Check(ShouldRefreshOwn(250, 0) && ShouldRefreshOwn(500, 250)
          && !ShouldRefreshOwn(499, 250),
          "own-row highlight checks run every 250 ms");

    char b[96];
    Check(RowPath(b, sizeof b, 0)
          && std::strcmp(b, "anim-group.table.row0_onlineid") == 0,
          "row paths use the native leaderboard table names");
    Check(RowPath(b, sizeof b, 9)
          && std::strcmp(b, "anim-group.table.row9_onlineid") == 0,
          "the tenth row is included");
    Check(!RowPath(b, sizeof b, 10) && !RowPath(b, sizeof b, -1),
          "out-of-range row paths are refused");
    Check(HighlightPath(b, sizeof b, 3)
          && std::strcmp(b, "anim-group.table.lbMe3") == 0,
          "highlight paths use the generated lbMe groups");
    Check(ChipPath(b, sizeof b, 7)
          && std::strcmp(b, "anim-group.table.lbMe7.lbMe7Yo") == 0,
          "chip paths use the generated per-row YOU chip");
    Check(!ChipPath(nullptr, 80, 0) && !ChipPath(b, sizeof b, 10),
          "null buffers and bad rows are refused");

    if (fails) {
        std::printf("lbnr core: %d check(s) failed\n", fails);
        return 1;
    }
    std::printf("lbnr core: all checks passed\n");
    return 0;
}
