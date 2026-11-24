// Standalone harness for the pure launch-notice core (no game headers / no Windows):
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -I mod/src/net/ui
//       mod/tests/launch_notice_core_test.cpp -o /tmp/launch_notice_core_test && /tmp/launch_notice_core_test
//
// Covers the once-per-run version latch, the title hand-off read clock, and the
// push / focus gates from tools/ext/launchnote.c.

#include "launch-notice-core.hpp"

#include <cmath>
#include <cstdio>

namespace {

    using namespace net::ui::launch_notice::core;

    int fails = 0;
    void Check(bool ok, const char* what)
    {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++fails;
    }

    void LatchTests()
    {
        Check(Seen(0) == false && Seen(kVersion) == true && Seen(kVersion + 1) == true,
              "the version latch sticks at this version and later");
        Check(Busy(Stage::Unknown) == false && Busy(Stage::Due) == true
              && Busy(Stage::Pushed) == true && Busy(Stage::Up) == true
              && Busy(Stage::Done) == false,
              "only the live notice stages are busy");
    }

    void ReadClockTests()
    {
        Lock l = ReadLock(1000, 0);
        Check(l.elapsed == 0 && l.locked && l.leftSec == 10,
              "without a title-clear time the read clock stays locked at ten seconds");

        l = ReadLock(1000 + 2500, 1000);
        Check(l.elapsed == 2500 && l.locked && l.leftSec == 8,
              "the read clock starts when the title clears and counts down");
        Check(TimerScale(l) > 0.74f && TimerScale(l) < 0.76f,
              "the timer bar shrinks with the read clock");

        l = ReadLock(1000 + kReadMs, 1000);
        Check(!l.locked && l.leftSec == 0 && TimerScale(l) == 0.0f,
              "the notice unlocks after ten seconds");
    }

    void GateTests()
    {
        uint64_t holdAt = 0;
        Check(HoldFade(Stage::Due, 1000, holdAt) && holdAt == 1000,
              "a due notice starts the hand-off hold");
        Check(HoldFade(Stage::Pushed, 1000 + kHoldMaxMs - 1, holdAt) == true,
              "the hand-off hold lasts up to four seconds");
        Check(HoldFade(Stage::Pushed, 1000 + kHoldMaxMs, holdAt) == false,
              "the hand-off hold cannot keep the title up forever");
        Check(HoldFade(Stage::Up, 9999, holdAt) == false,
              "an already up notice does not hold the title");

        Check(ShouldPush(Stage::Unknown, 1000, 0, 0) == false,
              "an unknown stage never pushes");
        Check(ShouldPush(Stage::Due, 1000, 0, 0) == true,
              "a due notice pushes at once");
        Check(ShouldPush(Stage::Pushed, 1000 + kRetryMs, 1000, 1) == false,
              "retries wait out the retry window");
        Check(ShouldPush(Stage::Pushed, 1000 + kRetryMs + 1, 1000, 1) == true,
              "a retry is allowed after the window");
        Check(ShouldPush(Stage::Pushed, 1000 + kRetryMs + 1, 1000, kMaxPushes) == false,
              "the push count is capped");
        Check(DropPushes(Stage::Pushed, 1000 + kRetryMs + 1, 1000, kMaxPushes) == true,
              "an exhausted notice drops its pushes");
        Check(DropPushes(Stage::Done, 99999, 1000, kMaxPushes) == false,
              "a completed notice is not dropped");

        Check(Ramp(1000, 1000, kFadeMs) == 0.0f && Ramp(1000 + kFadeMs, 1000, kFadeMs) == 1.0f,
              "the fade ramp saturates at its endpoints");
    }

    void FocusTests()
    {
        Check(FocusMove(false, 0, false, true, false, false) == 0,
              "focus does not move on an unticked frame");
        Check(FocusMove(true, 0, false, true, false, false) == 1,
              "down moves from the checkbox to the buttons");
        Check(FocusMove(true, 2, false, false, true, false) == 1,
              "left moves from the second button to the first");
        Check(FocusMove(true, 1, false, false, false, true) == 2,
              "right moves from the first button to the second");
        Check(FocusMove(true, 2, true, false, false, false) == 0,
              "up from a button returns to the checkbox");
        Check(FocusMove(true, 1, false, true, false, false) == 1,
              "down is ignored once focus is already on the buttons");
    }

} // namespace

int main()
{
    LatchTests();
    ReadClockTests();
    GateTests();
    FocusTests();
    std::printf("%s\n", fails ? "launch-notice core: FAILED" : "launch-notice core: all tests passed");
    return fails ? 1 : 0;
}
