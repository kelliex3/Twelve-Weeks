// Standalone harness for the pure sbteam placement core:
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -I mod/src/net/ui
//       mod/tests/sbteam_core_test.cpp -o /tmp/sbteam_core_test
//   /tmp/sbteam_core_test
//
// Keeps the ext's physical-swap rule explicit and verifies that the native
// fill already owns the order (so the owner never runs ExtBlockY).

#include "sbteam-core.hpp"

#include <cmath>
#include <cstdio>

namespace {

    using namespace net::ui::sbteam::core;

    int fails = 0;
    void Check(bool ok, const char* what)
    {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++fails;
    }

    bool Near(float a, float b) { return std::fabs(a - b) < 0.001f; }

    void PlacementTests()
    {
        Check(WantsPhysicalSwap(0) == false && WantsPhysicalSwap(1) == true,
              "only a team-1 player needed the ext's block swap");
        Check(NativeFillOwnTeamOnTop(0, 0) && NativeFillOwnTeamOnTop(1, 1) &&
              !NativeFillOwnTeamOnTop(1, 0),
              "the native fill puts the local team in display block 0");
        Check(PlaceColoursFixed(0) && PlaceColoursFixed(1),
              "the blue / red bands follow the place, not the team");

        Check(Near(ExtBlockY(0, 800.0f, false), 800.0f) &&
              Near(ExtBlockY(1, 1274.0f, false), 1274.0f),
              "with no swap the ext keeps the template positions");
        Check(Near(ExtBlockY(0, 800.0f, true), 1274.0f) &&
              Near(ExtBlockY(1, 1274.0f, true), 800.0f),
              "with a swap the ext exchanges the two blocks");
        Check(Near(ExtBlockY(0, 1274.0f, false), 800.0f) &&
              Near(ExtBlockY(1, 800.0f, false), 1274.0f),
              "the ext placement is absolute and repeatable");
        Check(NeedsMove(1274.0f, 800.0f) && !NeedsMove(800.0f, 800.4f),
              "only a half-unit-or-more difference triggers a pivot write");
    }

    void LeafTests()
    {
        Check(sizeof(kBands) / sizeof(*kBands) == 7 &&
              sizeof(kCells) / sizeof(*kCells) == 11 && kRows == 4,
              "the ext's band / row leaf sets and row count are preserved");
    }

} // namespace

int main()
{
    PlacementTests();
    LeafTests();
    if (fails) {
        std::printf("%d failure(s)\n", fails);
        return 1;
    }
    std::printf("sbteam core: all checks passed\n");
    return 0;
}
