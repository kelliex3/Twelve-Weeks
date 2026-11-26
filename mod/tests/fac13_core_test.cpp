// Standalone harness for the 2013 Factions header rules:
//   g++ -std=c++20 -Wall -Wextra -Werror -pedantic -I mod/src/net/ui
//       mod/tests/fac13_core_test.cpp -o /tmp/opencode/fac13_core_test
//   /tmp/opencode/fac13_core_test

#include "fac13-core.hpp"

#include <cstdio>
#include <cstring>

namespace {

    using namespace net::ui::fac13::core;

    int fails = 0;
    void Check(bool ok, const char* what)
    {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++fails;
    }

} // namespace

int main()
{
    std::printf("fac13 core:\n");
    Check(ClampDay(-4) == 0 && ClampDay(83) == 83 && ClampDay(84) == 83,
          "day clamps to 0..83");
    Check(ClampSurvived(-1) == 0 && ClampSurvived(42) == 42
          && ClampSurvived(84) == 84 && ClampSurvived(99) == 84,
          "survived days clamp to 0..84");
    Check(PipX(0) == 2896.0f && PipX(7) == 2964.0f && PipX(83) == 3692.0f,
          "day 0 / 7 / 83 map to x 2896 / 2964 / 3692");
    Check(PipX(-3) == PipX(0) && PipX(90) == PipX(83),
          "pip positions clamp through the day rule");
    Check(PipOn(0, 1) && !PipOn(1, 1) && PipOn(83, 84),
          "only survived days use the bright alpha");
    Check(kPipOn > kPipOff && kPipOn == 230.0f / 255.0f
          && kPipOff == 40.0f / 255.0f,
          "pip alpha values match clan_widgets.py");
    Check(TodayVisible(83) && !TodayVisible(84)
          && TodayX(0) == PipX(0) && TodayX(84) == PipX(83)
          && TodayY() == 194.0f,
          "the cyan marker sits on today, or the last day when all survived");
    Check(IconW(1) && !IconS(1) && !IconW(2) && IconS(2),
          "WLF and Seraphite pictograms are mutually exclusive");

    char b[64];
    Check(WeekDay(b, sizeof b, 0) && std::strcmp(b, "WEEK 1: DAY 1") == 0,
          "day 0 reads WEEK 1: DAY 1");
    Check(WeekDay(b, sizeof b, 83) && std::strcmp(b, "WEEK 12: DAY 7") == 0,
          "day 83 reads WEEK 12: DAY 7");
    Check(Survived(b, sizeof b, 84) && std::strcmp(b, "DAYS SURVIVED 84 | 84") == 0,
          "the survived caption keeps the | 84 suffix");
    Check(PipPath(b, sizeof b, 83) && std::strcmp(b, "anim-group.f13Pip83") == 0,
          "pip paths use the generated anim-group names");
    Check(!PipPath(b, sizeof b, 84) && !WeekDay(nullptr, 8, 0),
          "out-of-range paths and null buffers are refused");
    Check(std::strcmp(EventLabel(1), "INCOMING EVENT") == 0
          && std::strcmp(EventLabel(2), "EVENT RESULT") == 0,
          "event labels follow the event mode");

    if (fails) {
        std::printf("fac13 core: %d check(s) failed\n", fails);
        return 1;
    }
    std::printf("fac13 core: all checks passed\n");
    return 0;
}
