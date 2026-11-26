// Standalone harness for the pure CHOOSE YOUR SIDE core:
//   g++ -std=c++20 -Wall -Wextra -Werror -pedantic -I mod/src/net/ui
//       mod/tests/choose_core_test.cpp -o /tmp/opencode/choose_core_test
//   /tmp/opencode/choose_core_test
//
// Pins the stage machine, focus / hover rules, the authored tweens and the
// key-art / intro curves; the owner maps them onto widgets, camera and mixer
// without changing their timing.

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
    bool Near(float a, float b) { return std::fabs(a - b) < 0.02f; }

    void TweenTests()
    {
        Tween z;
        z.Reset(0.0f, 1000);
        z.To(2.0f, 1000, kZoomMs);
        Check(Near(z.Val(1000), 0.0f) && Near(z.Val(1450), 1.0f) &&
              Near(z.Val(1900), 2.0f),
              "a tween eases from -> to over its authored duration");
        Check(z.Val(500) == 0.0f, "a tween queried before its start holds `from`");
        z.To(0.0f, 1450, kZoomMs);
        Check(Near(z.Val(1450), 1.0f) && Near(z.Val(1450 + kZoomMs), 0.0f),
              "re-targeting starts from the value in flight (no focus snap)");
        Check(SideEase(0.0f) == 0.0f && SideEase(1.0f) == 1.0f &&
              SideEase(0.5f) == 0.5f && SideEase(0.25f) < Ease(0.25f),
              "the page curve is the authored smootherstep");
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

    void CameraTests()
    {
        Check(kInMs == 560 && kStaggerMs == 140 && kOutMs == 300 &&
              kPickOutMs == 1800 && Near(kBgA, 0.90f) && kSelectGateMs == 350,
              "the page timings pin side_tick's SIDE_* constants");
        Check(Near(kCameraY0, 5.75f) && Near(kDepth[0], 3.82f) &&
              Near(kDepth[4], 150.0f),
              "the intro starts 5.75 m up over the authored layer depths");
        Check(IntroCameraYAt(0) > 5.7f && IntroCameraYAt(IntroDropMs()) > 5.7f &&
              IntroCameraYAt(IntroDropMs() + kDropMs) == 0.0f,
              "the camera holds the tarp then drops to the overview");
        const float mid = IntroCameraYAt(IntroDropMs() + kDropMs / 2);
        Check(mid > 0.0f && mid < kCameraY0, "the drop eases over CHI_DROP_MS");

        const LayerPose near = LayerAt(0, 0.0f, -1.0f, 0.0f, kCameraY0);
        const LayerPose far = LayerAt(4, 0.0f, -1.0f, 0.0f, kCameraY0);
        Check(near.y > far.y && near.y > 1080.0f,
              "the intro drop shifts near layers more than the sky");
        const LayerPose z = LayerAt(0, 1.0f, -1.0f, 0.0f, 0.0f);
        Check(z.scale > near.scale, "the focused camp zooms the foreground layer");
        const LayerPose deck = DeckAt(0.0f, -1.0f, 0.0f, kCameraY0);
        Check(deck.y < 1080.0f, "the tarp starts above the overview framing");

        Check(IntroLineAlpha(0, 0) == 0.0f &&
              IntroLineAlpha(kLines[0].tin + kFadeMs, 0) > 0.99f &&
              IntroLineAlpha(kTextOut + kOutMs2, 0) == 0.0f,
              "the intro lines fade in at their beat and out together");
        Check(IntroBeatAlpha(kLogoAt + kLogoFadeMs, 2) > 0.99f &&
              IntroBeatAlpha(kBeatAt[0], 0) == 0.0f,
              "the logo beats have their own word fades");
        Check(IntroSkipAlpha(1000, true) > 0.0f && IntroSkipAlpha(1000, false) == 0.0f &&
              IntroSkipAlpha(IntroDropMs() - 1, true) < 0.001f,
              "SKIP is only shown after a watched-through intro, before the drop");
        Check(IntroUiAlpha(IntroUiMs() - 1) == 0.0f &&
              IntroUiAlpha(IntroUiMs() + kUiInMs) > 0.99f,
              "the picker UI fades in over CHI_UI_IN_MS");
        Check(SkipDownAlpha(kSkipDownMs) > 0.99f && SkipUpAlpha(kSkipUpMs) == 0.0f,
              "the skip dips to black and comes back up");
    }

    void StageTests()
    {
        Page p;
        Enter(p, false, true, false, 1000);
        Check(p.stage == kPick, "the key-art path shows the faction pick directly");
        Check(IntroInputOk(true, 1000, 1000) == false &&
              IntroInputOk(true, 1000 + static_cast<uint64_t>(IntroUiMs() + kInputMs), 1000),
              "intro select is skipped until the UI has settled");

        Page q;
        Enter(q, false, false, false, 1000);
        Check(q.stage == kIntro && IntroSelect(q, 1200) && q.stage == kCrossfade,
              "SELECT on the old intro card starts the crossfade");
        Check(!CrossfadeDone(q, 1200 + 219) && CrossfadeDone(q, 1200 + 220) &&
              q.stage == kPick,
              "the 220 ms crossfade hands over to the pick");
        Check(CrossfadeAlpha(q, 1200 + 110) > 0.0f &&
              CrossfadeAlpha(q, 1200 + 110) < 1.0f,
              "a short crossfade stays linear");
        Page e;
        Enter(e, true, false, false, 1000);
        e.stage = kCrossfade; e.fadeAt = 1000; e.fadeMs = 800;
        Check(CrossfadeAlpha(e, 1100) < 0.5f,
              "the end-screen crossfade uses the smootherstep curve");
        Confirm(q, 1, false, false, 5000);
        Check(q.stage == kFadeOut && q.pickSide == 1,
              "a confirmed side fades out with the pick's longer deadline");
        Check(!FadeOutDone(q, 5000 + kPickOutMs - 1) &&
              FadeOutDone(q, 5000 + kPickOutMs), "the pick fade-out lasts SIDE_PICK_OUT_MS");
        Check(ExitAmount(q, 5000 + kPickOutMs) == 0.0f &&
              Near(BackgroundOut(q, 5000 + kPickOutMs), 1.0f),
              "a confirmed pick fades the content to black");
        Popped(q);
        Check(q.stage == kPopped && q.pickSide == -1, "popping clears the pending pick");

        Page b;
        Enter(b, false, true, false, 9000);
        Check(!BackOrClose(b, false, 100, 9100) && b.backFrameAt == 0,
              "a clan with no side has no back target");
        b.living = true;
        Check(!BackOrClose(b, false, 100, 9100) && b.backFrameAt == 100 &&
              BackOrClose(b, false, 107, 9200) && b.stage == kPopped,
              "a living clan's back closes the page after six frames");

        Page r;
        Enter(r, false, true, true, 1000);
        Check(ShowRechoiceNote(r), "the re-choice note is live until a side is confirmed");
        Confirm(r, 0, true, false, 2000);
        Check(!ShowRechoiceNote(r), "a confirmed re-choice never restores the old warning");
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
        Check(ChalkAlpha(p, 0, 999) == 0.0f &&
              ChalkAlpha(p, 0, 1000 + 327) > 0.99f &&
              ChalkAlpha(p, 0, 1000 + 100000) == 1.0f,
              "each chalk group's alpha is clamped to its show window");
        Check(PayoffTextAlpha(p, 999) == 1.0f &&
              PayoffTextAlpha(p, 1000 + kPTextMs) == 0.0f,
              "the description and hints fade out over CHP_TEXT_MS");
        Check(PayoffSoundDue(p, 1000 + kPLiftMs + kPSlamMs - kSelectSoundLeadMs) &&
              !PayoffSoundDue(p, 1000 + kPLiftMs + kPSlamMs),
              "the select sound fires once, on the slam");
    }

} // namespace

int main()
{
    TweenTests();
    ArtTests();
    CameraTests();
    StageTests();
    PayoffTests();
    if (fails) {
        std::printf("%d failure(s)\n", fails);
        return 1;
    }
    std::printf("choose core: all checks passed\n");
    return 0;
}
