// Standalone harness for the pure loadseq core (no game headers / no Windows):
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -I mod/src/menu
//       mod/tests/loadseq_core_test.cpp -o /tmp/loadseq_core_test && /tmp/loadseq_core_test
//
// Covers the ext entry-sequence fade timings, the monotonic progress fill and
// the session-volume ramp from tools/ext/loadseq.c (build-201).

#include "loadseq-core.hpp"

#include <cmath>
#include <cstdio>

namespace {

    using namespace menu::loadseq::core;

    int fails = 0;
    void Check(bool ok, const char* what)
    {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++fails;
    }

    bool Near(float a, float b, float eps = 1e-4f)
    {
        return std::fabs(a - b) <= eps;
    }

    void SmoothTests()
    {
        Check(Clamp01(-1.0f) == 0.0f && Clamp01(2.0f) == 1.0f && Clamp01(0.4f) == 0.4f,
              "Clamp01 clamps both ends");
        Check(Smooth(-1.0f) == 0.0f && Smooth(2.0f) == 1.0f
              && Near(Smooth(0.5f), 0.5f),
              "smootherstep is clamped and symmetric at the midpoint");
        Check(Smooth(0.1f) < 0.1f && Smooth(0.9f) > 0.9f,
              "smootherstep eases in and out");
    }

    void FadeTests()
    {
        Check(Fade(1000, 0, kBlackMs, 300) == 0.0f,
              "no start time means no fade");
        Check(Fade(1000 + kBlackMs - 1, 1000, kBlackMs, 300) == 0.0f,
              "the fade waits for its delay");
        Check(Fade(1000 + kBlackMs + 150, 1000, kBlackMs, 300) > 0.49f
              && Fade(1000 + kBlackMs + 150, 1000, kBlackMs, 300) < 0.51f,
              "the fade is halfway through at the midpoint");
        Check(Fade(1000 + kBlackMs + 300, 1000, kBlackMs, 300) == 1.0f,
              "the fade reaches one at its duration");
        Check(Fade(1000 + kTitleAtMs + 100, 1000, kTitleAtMs, kTitleMs) > 0.0f,
              "the title fade starts later than the black fade");
        Check(Fade(1000 + kBarAtMs - 1, 1000, kBarAtMs, kBarMs) == 0.0f,
              "the bar fade is later still");
    }

    void FillTests()
    {
        Check(Target(0) == 0.0f, "zero elapsed has no fill");
        Check(Target(kFillTauMs) > 0.57f && Target(kFillTauMs) < 0.59f,
              "the fill is 58% of the cap after one time constant");
        Check(Target(1000000) <= kFillCap && Target(1000000) > kFillCap - 1e-6f,
              "the fill asymptotically approaches the cap");

        float v = 0.0f;
        for (int i = 0; i < 100; ++i)
            v = Advance(v, 500, 16, false);
        Check(v > 0.0f && v < kFillCap,
              "the fill rises monotonically toward the target");
        const float before = v;
        v = Advance(v, 1, 16, false);
        Check(v >= before, "the fill never moves backwards");

        v = Advance(0.2f, 1000, 10, true);
        Check(v > 0.79f && v < 0.81f,
              "the fade-out fill moves faster but still has a per-frame cap");

        Check(Near(BottomBarScaleX(0.0f), 0.0f)
              && Near(BottomBarScaleX(1.0f), 3840.0f / 2800.0f),
              "the bottom bar maps fill onto the widget scale");
        Check(Near(kBarScaleY, 24.0f / 760.0f), "the bar's y scale is unchanged");
    }

    void OutroTests()
    {
        uint64_t lastCheck = 0;
        Check(ReadyForFade(1000, false, lastCheck, 5000) == false && lastCheck == 0,
              "the world-not-ready signal never advances the check stamp");
        Check(ReadyForFade(kTitleAtMs + kTitleMs - 1, true, lastCheck, 5000) == false,
              "the title cannot leave before its minimum time");
        Check(ReadyForFade(kTitleAtMs + kTitleMs, true, lastCheck, 5000) == true
              && lastCheck == 5000,
              "the title can leave once the world is ready");
        Check(ReadyForFade(kTitleAtMs + kTitleMs, true, lastCheck, 5200) == false,
              "the readiness check is rate limited");

        Check(OutMultiplier(1000, 0) == 1.0f && OutMultiplier(5000, 6000) == 1.0f,
              "the outro multiplier is one before the out time");
        Check(OutMultiplier(1000 + kOutMs / 2, 1000) > 0.49f
              && OutMultiplier(1000 + kOutMs / 2, 1000) < 0.51f,
              "the outro fades through its midpoint");
        Check(OutMultiplier(1000 + kOutMs, 1000) == 0.0f,
              "the outro finishes at zero");
        Check(ResumeMultiplier(1000 + kResumeOutMs / 2, 1000) > 0.49f
              && ResumeMultiplier(1000 + kResumeOutMs, 1000) == 0.0f,
              "the resume out uses its longer duration");

        Check(SessionVolume(1000, 0, false) == 1.0f && SessionVolume(1000, 0, true) == 1.0f,
              "without a mute time the session volume is untouched");
        Check(SessionVolume(1000 + kMuteMs / 2, 1000, false) > 0.49f
              && SessionVolume(1000 + kMuteMs, 1000, false) == 0.0f,
              "the session mutes over its ramp");
        Check(SessionVolume(1000 + kMusicRampMs / 2, 1000, true) > 0.49f
              && SessionVolume(1000 + kMusicRampMs, 1000, true) == 1.0f,
              "the session unmutes over its longer ramp");
    }

} // namespace

int main()
{
    SmoothTests();
    FadeTests();
    FillTests();
    OutroTests();
    std::printf("%s\n", fails ? "loadseq core: FAILED" : "loadseq core: all tests passed");
    return fails ? 1 : 0;
}
