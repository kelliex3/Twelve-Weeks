// Standalone harness for the native prompt-producer rules:
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -I mod/src/net/ui
//       mod/tests/prompt_producer_core_test.cpp -o /tmp/prompt_producer_core_test
//   /tmp/prompt_producer_core_test
//
// Covers the ext's capped wait-ring fill and the supply-box interact gate.

#include "prompt-producer-core.hpp"

#include <cmath>
#include <cstdio>
#include <limits>

namespace {

    using namespace net::ui::prompt_producer::core;

    int fails = 0;
    void Check(bool ok, const char* what)
    {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++fails;
    }

    bool Near(float a, float b, float eps = 0.001f)
    {
        return std::fabs(a - b) <= eps;
    }

    void WaitTests()
    {
        Check(WaitFill(1000, 1500, 500) == 0.0f,
              "a window first seen at its deadline start has an empty ring");
        Check(Near(WaitFill(1250, 1500, 500), 0.5f),
              "the ring is half way at the window's midpoint");
        Check(Near(WaitFill(1490, 1500, 500), 0.98f),
              "the ring contracts onto the key as the deadline arrives");
        Check(WaitFill(1501, 1500, 500) == kWaitCap,
              "the fill is capped once the deadline has passed");
        Check(WaitFill(1000, 0, 500) == 0.0f &&
              WaitFill(1000, 1500, 0) == 0.0f,
              "an unknown end or window length produces no fill");

        // Only the remaining time and the core's nominal window length are
        // known; a short / already-open window starts part way through.
        Check(Near(WaitFill(1400, 1500, 500), 0.8f),
              "a window first seen late starts at its elapsed share");
    }

    void SupplyTests()
    {
        Check(SupplyPrompt(0.0f, false, true), "a box at our feet is openable");
        Check(SupplyPrompt(1.5f, false, true), "the 1.5 m reach is inclusive");
        Check(!SupplyPrompt(1.51f, false, true), "beyond the reach there is no prompt");
        Check(!SupplyPrompt(1.0f, true, true), "aiming suppresses the prompt");
        Check(!SupplyPrompt(1.0f, false, false), "an opened / refilling box has no prompt");
        Check(!SupplyPrompt(std::numeric_limits<float>::quiet_NaN(), false, true),
              "a bad distance never shows a prompt");
        Check(!SupplyPrompt(-1.0f, false, true), "a negative distance never shows a prompt");
    }

} // namespace

int main()
{
    WaitTests();
    SupplyTests();
    if (fails) {
        std::printf("%d failure(s)\n", fails);
        return 1;
    }
    std::printf("prompt producer core: all checks passed\n");
    return 0;
}
