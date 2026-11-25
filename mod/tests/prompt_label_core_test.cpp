// Standalone harness for the four native prompt-label state machines:
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -I mod/src/net/ui
//       mod/tests/prompt_label_core_test.cpp -o /tmp/prompt_label_core_test
//   /tmp/prompt_label_core_test
//
// Mirrors the ext's pullout / duel-pin / gift / revive writers (working-tree
// tools/ext sources); no engine headers are pulled in.

#include "prompt-label-core.hpp"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

    using namespace net::ui::prompt_label::core;

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

    void RingTests()
    {
        Check(RingAt(1000, 1500, 200, 30) == 1270,
              "the pin ring starts 230 ms before the window ends");
        Check(RingAt(1400, 1500, 200, 30) == 1400,
              "a short window never puts the ring before its open");
    }

    void PulloutTests()
    {
        const uint64_t open = 1000, end = 1600;   // 600 ms lead, 400 ms window

        CounterState st;
        Label first = Counter(900, true, open, end, open, false, false,
                              false, Style::Pullout, st);
        Check(first.valid && first.kind == Kind::Wait && first.progress == 0.0f,
              "the pullout wait ring starts empty before the window");
        Label mid = Counter(950, true, open, end, open, false, false,
                            false, Style::Pullout, st);
        Check(mid.valid && mid.kind == Kind::Wait && Near(mid.progress, 0.5f),
              "the wait ring fills to the window open");
        Label press = Counter(1200, true, open, end, open, false, false,
                              false, Style::Pullout, st);
        Check(press.valid && press.kind == Kind::Press &&
              std::string(press.text) == "ESCAPE!",
              "the open pullout window asks for ESCAPE!");

        CounterState old;
        Label oldWait = Counter(900, true, open, end, open, false, true,
                                false, Style::Pullout, old);
        Check(oldWait.valid && std::string(oldWait.text) == "...wait",
              "the old-animation sidecar selects the ...wait text");

        CounterState burnt;
        Label early = Counter(900, true, open, end, open, true, false,
                              false, Style::Pullout, burnt);
        Check(early.valid && std::string(early.text) == "Too early",
              "a press before the window burns the pullout counter");
        Label stillEarly = Counter(1500, true, open, end, open, false, false,
                                   false, Style::Pullout, burnt);
        Check(stillEarly.valid && std::string(stillEarly.text) == "Too early",
              "the burnt press owns the label through the window");

        CounterState late;
        Counter(1000, true, open, end, open, false, false, false,
                Style::Pullout, late);
        Label tooLate = Counter(1700, true, open, end, open, false, false,
                                false, Style::Pullout, late);
        Check(tooLate.valid && std::string(tooLate.text) == "Too late",
              "a timed-out pullout shows Too late for the ext's 1200 ms");
        Check(Counter(2900, true, open, end, open, false, false, false,
                      Style::Pullout, late).valid == false,
              "the Too late linger expires at 1200 ms");
    }

    void PinTests()
    {
        const uint64_t open = 1000, end = 1500;
        const uint64_t ring = RingAt(open, end, 200, 30);

        CounterState st;
        Counter(1000, true, open, end, ring, false, false, false,
                Style::Pin, st);
        Label wait = Counter(1100, true, open, end, ring, false, false,
                             false, Style::Pin, st);
        Check(wait.valid && wait.kind == Kind::Wait &&
              Near(wait.progress, 100.0f / 270.0f),
              "the pin wait ring fills toward the ring-at time");
        Label press = Counter(1300, true, open, end, ring, false, false,
                              false, Style::Pin, st);
        Check(press.valid && press.kind == Kind::Press &&
              std::string(press.text) == "ESCAPE!",
              "after ring-at the pin window asks for ESCAPE!");

        CounterState lenient;
        Label oldPress = Counter(1300, true, open, end, ring, false, false,
                                 true, Style::Pin, lenient);
        Check(std::string(oldPress.text) == "Escape",
              "the old / lenient pin branch keeps the plain Escape label");

        CounterState burnt;
        Counter(1100, true, open, end, ring, true, false, false,
                Style::Pin, burnt);
        Label early = Counter(1400, true, open, end, ring, false, false,
                              false, Style::Pin, burnt);
        Check(std::string(early.text) == "Too early",
              "an early pin press burns the window instead of showing ESCAPE!");

        CounterState late;
        Counter(1300, true, open, end, ring, false, false, false,
                Style::Pin, late);
        Label tooLate = Counter(1550, true, open, end, ring, false, false,
                                false, Style::Pin, late);
        Check(std::string(tooLate.text) == "Too late",
              "a closed pin window shows Too late for 150 ms");
        Check(!Counter(1701, true, open, end, ring, false, false, false,
                       Style::Pin, late).valid,
              "the pin late linger expires after 150 ms");
    }

    void GiftAndReviveTests()
    {
        Check(!Gift(true, true, false, true).valid,
              "no peer stock message means no guessed gift prompt");
        Label give = Gift(true, true, true, true);
        Check(give.valid && give.kind == Kind::Hold &&
              std::string(give.text) == "Give gift" && give.progress == 0.0f,
              "a giftable teammate gets the empty-ring Give gift hold");
        Label full = Gift(true, true, true, false);
        Check(full.valid && full.kind == Kind::Text &&
              std::string(full.text) == "Can't carry more",
              "a full teammate gets the grey Can't carry more text");
        Check(!Gift(false, true, true, true).valid,
              "a gift not in the hand produces no prompt");
        Check(!Gift(true, false, true, true).valid,
              "no teammate in reach produces no prompt");

        Label idle = Revive(true, false, 0.0f);
        Check(idle.valid && idle.kind == Kind::Hold &&
              std::string(idle.text) == "Revive" && idle.progress == 0.0f,
              "a downed teammate gets the idle Revive hold");
        Label active = Revive(true, true, 0.42f);
        Check(active.valid && Near(active.progress, 0.42f),
              "holding interact fills the revive ring from RvProg");
        Label capped = Revive(true, true, 1.5f);
        Check(capped.progress == 1.0f,
              "multi-reviver progress over 1 is clamped for the ring");
        Check(!Revive(false, true, 0.5f).valid,
              "no downed target means no revive prompt");
    }

    void ReachTests()
    {
        Check(HealReach(1.4f, 0.5f, 0.0f),
              "the gift hand-out uses the ext's 1.5 x 1.2 m start box");
        Check(!HealReach(1.6f, 0.0f, 0.0f) && !HealReach(0.0f, 1.3f, 0.0f),
              "outside the hand-out box there is no gift target");
        Check(ReviveReach(2.0f, 0.0f, 0.0f, false),
              "a new revive starts inside 2.1213 m");
        Check(!ReviveReach(2.5f, 0.0f, 0.0f, false) &&
              ReviveReach(2.5f, 0.0f, 0.0f, true),
              "a running revive keeps going to 3 m");
    }

} // namespace

int main()
{
    RingTests();
    PulloutTests();
    PinTests();
    GiftAndReviveTests();
    ReachTests();
    if (fails) {
        std::printf("%d failure(s)\n", fails);
        return 1;
    }
    std::printf("prompt label core: all checks passed\n");
    return 0;
}
