// Standalone harness for the pure sbteam placement core:
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -I mod/src/net/ui
//       mod/tests/sbteam_core_test.cpp -o /tmp/sbteam_core_test
//   /tmp/sbteam_core_test
//
// Build-191 sbt_swap reads the two team-name pivots and moves each block by
// their measured distance, so the same helper works on the pause page and the
// scoreboard page (their baselines differ).

#include "sbteam-core.hpp"

#include <cmath>
#include <cstdio>
#include <initializer_list>

namespace {

    using namespace net::ui::sbteam::core;

    int fails = 0;
    void Check(bool ok, const char* what)
    {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++fails;
    }

    bool Near(float a, float b) { return std::fabs(a - b) < 0.001f; }

    void SwapTests()
    {
        Check(!WantsPhysicalSwap(0) && WantsPhysicalSwap(1) &&
              !WantsPhysicalSwap(2) && WantsPhysicalSwap(3),
              "the roster team bit decides the physical swap");

        // Pause-menu s0 = 868, scoreboard s0 = 616 (PNR_D = 520).
        for (float base : {582.0f, 834.0f}) {
            const float y0 = base, y1 = base + 520.0f;
            SwapRead r = ReadSwap(y0, y1);
            Check(r.valid && !r.swapped && Near(r.distance, 520.0f),
                  "the two name pivots describe a template-order board");
            Check(NeedsSwap(r, true) && !NeedsSwap(r, false),
                  "a team-1 player wants a swap; a team-0 player does not");
            const float d0 = ShiftDelta(r, true, 0);
            const float d1 = ShiftDelta(r, true, 1);
            Check(Near(d0, 520.0f) && Near(d1, -520.0f),
                  "each block moves by the measured distance, in opposite directions");
            const SwapRead after = ReadSwap(y0 + d0, y1 + d1);
            Check(after.swapped && !NeedsSwap(after, true),
                  "the read-back makes the swapped placement idempotent");
            Check(NeedsSwap(after, false) && Near(ShiftDelta(after, false, 0), -520.0f),
                  "an opt-out or unknown team restores the template order");
        }

        SwapRead tooClose = ReadSwap(600.0f, 620.0f);
        Check(!NeedsSwap(tooClose, true),
              "a collapsed or missing block pair is left alone");
    }

    void LeafTests()
    {
        Check(sizeof(kBlockParts) / sizeof(*kBlockParts) == 8 &&
              sizeof(kRowParts) / sizeof(*kRowParts) == 13 && kRows == 4,
              "the ext's block / row leaf sets and row count are preserved");
    }

} // namespace

int main()
{
    SwapTests();
    LeafTests();
    if (fails) {
        std::printf("%d failure(s)\n", fails);
        return 1;
    }
    std::printf("sbteam core: all checks passed\n");
    return 0;
}
