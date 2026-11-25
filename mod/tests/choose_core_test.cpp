// Standalone harness for the pure CHOOSE YOUR SIDE core:
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -I mod/src/net/ui
//       mod/tests/choose_core_test.cpp -o /tmp/choose_core_test
//   /tmp/choose_core_test
//
// Pins the stage machine, focus / hover rules and authored tweens; the owner
// maps them onto widgets, camera and mixer without changing their timing.

#include "choose-core.hpp"

#include <cmath>
#include <cstdio>

namespace {

    using namespace net::ui::choose::core;

    int fails = 0;
    void Check(bool ok, const char* what)
    {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++fails;
    }
    bool Near(float a, float b) { return std::fabs(a - b) < 0.01f; }

    void TweenTests()
    {
        Tween z;
        z.Reset(0.0f, 1000);
        z.To(2.0f, 1000, kZoomMs);
        Check(Near(z.Val(1000), 0.0f) && Near(z.Val(1450), 1.0f) &&
              Near(z.Val(1900), 2.0f),
              "a tween eases from -> to over its authored duration");
        z.To(0.0f, 1450, kZoomMs);
        Check(Near(z.Val(1450), 1.0f) && Near(z.Val(1450 + kZoomMs), 0.0f),
              "re-targeting starts from the value in flight (no focus snap)");
    }

    void ArtTests()
    {
        Art a;
        Show(a, true, 1000);
        Check(a.on && a.shown && a.focus == -1, "show opens on the overview, nothing focused");
        Check(Input(a, true, false, 1010) == 0 && a.focus == 0,
              "left focuses WLF from the overview");
        Check(Near(a.logo[0].Val(1010 + kNameMs), kLogoUp),
              "the focused camp's logo scales up over CH_NAME_MS");
        Check(Near(a.logo[1].Val(1010 + kNameMs), kLogoDown),
              "the other camp's logo scales down over the same ramp");
        Check(Input(a, true, false, 1020) == 1 && a.focus == 1,
              "left again switches to the other camp");
        Check(Back(a, false, 1030) && a.focus == -1,
              "back from a focused camp returns to the overview");
        Check(!Back(a, false, 1040), "back with nothing focused belongs to the page");

        // Overview click on a camp focuses it; a click on SELECT in the focus picks.
        Check(Mouse(a, -1, 0, false, true, 1500.0f, 1100.0f, false, 1100) == 0 &&
              a.focus == 0, "an overview click focuses the hovered camp");
        Check(Mouse(a, 0, 0, true, true, 1500.0f, 1100.0f, false, 1600) == 1,
              "clicking SELECT on a focused camp picks it");
        Check(PayoffTextAlpha(Payoff{}, 9999) == 1.0f,
              "no payoff leaves the description at full alpha");
    }

    void StageTests()
    {
        Page p;
        Enter(p, false, true, false, 1000);
        Check(p.stage == kPick, "the key-art path shows the faction pick directly");
        Check(IntroInputOk(true, 1000, 1000) == false &&
              IntroInputOk(true, 1000 + static_cast<uint64_t>(IntroUiMs() + kInputMs), 1000),
              "intro select is skipped until the UI has settled");
        Check(IntroCameraY(1000 + IntroDropMs(), 1000) == 0.0f &&
              IntroCameraY(1000 + IntroDropMs() + kDropMs, 1000) > 0.99f,
              "the intro camera drops over CHI_DROP_MS after the black pause");

        Page q;
        Enter(q, false, false, false, 1000);
        Check(q.stage == kIntro && IntroSelect(q, 1200) && q.stage == kCrossfade,
              "SELECT on the old intro card starts the crossfade");
        Check(!CrossfadeDone(q, 1200 + 219) && CrossfadeDone(q, 1200 + 220) &&
              q.stage == kPick,
              "the 220 ms crossfade hands over to the pick");
        Confirm(q, 1, false, false, 5000);
        Check(q.stage == kFadeOut && q.pickSide == 1,
              "a confirmed side fades out with the pick's longer deadline");
        Check(!FadeOutDone(q, 5000 + kPickOutMs - 1) &&
              FadeOutDone(q, 5000 + kPickOutMs), "the pick fade-out lasts SIDE_PICK_OUT_MS");
        Popped(q);
        Check(q.stage == kPopped && q.pickSide == -1, "popping clears the pending pick");
    }

    void PayoffTests()
    {
        Payoff p;
        StartPayoff(p, 1, true, 1000);
        Check(p.on && p.side == 1, "the payoff starts only with the key art on");
        LogoPose lift = PayoffLogo(p, 1000 + kPLiftMs);
        Check(lift.scale > 1.11f && lift.rise > 25.0f,
              "the chosen logo lifts by CHP_LIFT_K before the slam");
        LogoPose slam = PayoffLogo(p, 1000 + kPLiftMs + kPSlamMs);
        Check(slam.scale < 1.0f && slam.rise < 0.0f,
              "the slam squashes below rest before settling");
        Check(!PayoffDone(p, 1000 + kChalkEndMs + kPHoldMs - 1) &&
              PayoffDone(p, 1000 + kChalkEndMs + kPHoldMs),
              "the page fades out after the last chalk piece plus the hold");
        Check(ChalkAlpha(p, 0, 1000) <= 0.0f && ChalkAlpha(p, 0, 1000 + 326) > 0.99f,
              "each chalk group fades in over its own show window");
        Check(PayoffSoundDue(p, 1000 + kPLiftMs + kPSlamMs - 50) &&
              !PayoffSoundDue(p, 1000 + kPLiftMs + kPSlamMs),
              "the select sound fires once, on the slam");
    }

} // namespace

int main()
{
    TweenTests();
    ArtTests();
    StageTests();
    PayoffTests();
    if (fails) {
        std::printf("%d failure(s)\n", fails);
        return 1;
    }
    std::printf("choose core: all checks passed\n");
    return 0;
}
