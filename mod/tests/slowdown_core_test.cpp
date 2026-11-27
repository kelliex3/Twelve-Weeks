// Standalone harness for the pure slowdown rules; no engine or file headers.
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -I mod/src
//       mod/tests/slowdown_core_test.cpp -o /tmp/opencode/slowdown_check
//   /tmp/opencode/slowdown_check

#include "gameplay/slowdown-core.hpp"

#include <cstdio>

namespace {

    using namespace gameplay::slowdown::core;

    int fails = 0;
    void Check(bool ok, const char* what)
    {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++fails;
    }

} // namespace

int main()
{
    Check(kMove == 0.90f,
          "the ext's locomotion factor is 0.90 (no silent retune)");
    Check(Mul(false) == 0.90f && Mul(true) == 1.0f,
          "the opt-out restores the original speed");
    Check(CacheDue(0, 0),
          "the sidecar is read on the first poll");
    Check(!CacheDue(1999, 1000) && CacheDue(3000, 1000),
          "the sidecar is re-read only every 2 s");

    if (fails) {
        std::printf("%d slowdown core checks failed\n", fails);
        return 1;
    }
    std::puts("all slowdown core checks passed");
    return 0;
}
