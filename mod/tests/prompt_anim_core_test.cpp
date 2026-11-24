// Standalone harness for the pure prompt-anim timing / geometry core:
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -I mod/src/net/ui
//       mod/tests/prompt_anim_core_test.cpp -o /tmp/prompt_anim_core_test
//   /tmp/prompt_anim_core_test
//
// Covers the ext's appear / fade curves, the perimeter and ring geometry,
// mash flash window and the two-frame slot lifetime.

#include "prompt-anim-core.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

    using namespace net::ui::prompt_anim::core;

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

    void TimingTests()
    {
        Check(Clamp01(-1.0f) == 0.0f && Clamp01(2.0f) == 1.0f &&
              Clamp01(0.4f) == 0.4f, "Clamp01 bounds the input");

        State s;
        Check(!Frame(s, 1000, false, 0, 0, Kind::Press, "x", 0, false),
              "a slot that was never shown draws nothing");
        Check(Frame(s, 1000, true, 640, 300, Kind::Wait, "...wait", 0.25f, false),
              "an active slot draws");
        Check(s.vis && s.t0 == 1000 && s.last == 1000 &&
              Near(s.hold, 0.25f) && std::strcmp(s.label, "...wait") == 0,
              "the first active frame starts t0 and stores label / progress");
        Check(Elapsed(s, 1120) > 0.119f && Elapsed(s, 1120) < 0.121f,
              "Elapsed is in seconds");
        Check(Frame(s, 1150, true, 700, 310, Kind::Wait, "...wait", 0.5f, false) &&
              s.t0 == 1000 && Near(s.x, 700.0f) && Near(s.hold, 0.5f),
              "the same kind / label keeps t0 and follows a moving anchor");

        Check(Frame(s, 1200, true, 700, 310, Kind::Mash, "", 0, true) &&
              s.t0 == 1200 && s.flash == 1200,
              "a kind change restarts the animation and a mash press sets flash");
        Check(FadeOut(s, 1250) == 1.0f, "an active slot has full fade");
        Check(Frame(s, 1280, false, 0, 0, Kind::Mash, "", 0, false),
              "an inactive slot keeps fading for 100 ms");
        Check(Near(FadeOut(s, 1240), 0.6f), "the fade-out is linear over 100 ms from the last active frame");
        Check(!Frame(s, 1301, false, 0, 0, Kind::Mash, "", 0, false) && !s.vis,
              "the slot goes invisible 100 ms after the last active frame");

        Check(Frame(s, 2000, true, 640, 300, Kind::Text, "a", 0, false) &&
              Frame(s, 2010, true, 640, 300, Kind::Text, "b", 0, false) &&
              s.t0 == 2010 && std::strcmp(s.label, "b") == 0,
              "a label change restarts a plain-text prompt");

        State clipped;
        Frame(clipped, 10, true, 640, 300, Kind::Text, nullptr, 5.0f, false);
        Check(Near(clipped.hold, 1.0f) && clipped.label[0] == '\0',
              "progress is clamped and a null label is empty");
    }

    void CurveTests()
    {
        Check(Near(Grow(0.0f), 0.35f) && Near(Grow(0.12f), 1.0f) &&
              Near(Grow(1.0f), 1.0f), "the key grows from 0.35x in 120 ms");
        Check(OutlineAlpha(0.0f, 1.0f) == 0.0f &&
              Near(OutlineAlpha(0.05f, 1.0f), 1.0f),
              "the outline fades in over 50 ms");
        Check(LetterAlpha(0.12f, 1.0f) == 0.0f &&
              Near(LetterAlpha(0.20f, 1.0f), 1.0f),
              "the letter fades in after the outline");
        Check(Near(LetterLum(0.0f), 0.55f) && Near(LetterLum(0.35f), 1.0f),
              "the letter goes from grey to white");
        Check(Settle(0.0f) > 3.99f && Near(Settle(1.0f), 0.0f),
              "text settles upward from 4 px");
        Check(Near(TextAlpha(0.0f, 1.0f), 0.0f) &&
              Near(TextAlpha(0.15f, 1.0f), 1.0f),
              "plain text fades in over 150 ms");
        Check(Near(FaceAlpha(0.0f, 1.0f), 0.0f) &&
              FaceAlpha(0.35f, 1.0f) > 0.8f,
              "the key face brightens after the outline");
    }

    void GeometryTests()
    {
        float x = 0.0f, y = 0.0f;
        Perimeter(100, 100, 10, 0.0f, x, y);
        Check(Near(x, 100.0f) && Near(y, 90.0f), "u=0 is the top middle");
        Perimeter(100, 100, 10, 0.125f, x, y);
        Check(Near(x, 110.0f) && Near(y, 90.0f), "u=1/8 is the top right");
        Perimeter(100, 100, 10, 0.375f, x, y);
        Check(Near(x, 110.0f) && Near(y, 110.0f), "u=3/8 is the bottom right");
        Perimeter(100, 100, 10, -0.25f, x, y);
        Check(Near(x, 90.0f) && Near(y, 100.0f), "negative u wraps clockwise");
        Perimeter(100, 100, 10, 1.0f, x, y);
        Check(Near(x, 100.0f) && Near(y, 90.0f), "u=1 wraps to u=0");

        Check(Near(RingRadius(0.0f), 30.0f) &&
              Near(RingRadius(1.0f), 20.0f),
              "the ring contracts from 1.5x to the key border");
        Check(WaitRingAlpha(0.12f, 1.0f) == 0.0f &&
              Near(WaitRingAlpha(0.22f, 1.0f), 0.8f),
              "the wait ring fades in over 100 ms");

        State m;
        Frame(m, 500, true, 640, 300, Kind::Mash, "", 0, true);
        float p = 0.0f, a = 0.0f;
        Check(!FlashActive(m, 499, p, a), "a flash is not active before it starts");
        Check(FlashActive(m, 590, p, a) && p > 0.49f && p < 0.51f && a > 0.4f,
              "a mash flash contracts over 180 ms");
        Check(!FlashActive(m, 681, p, a), "the flash ends after 180 ms");

        float cx = 10.0f, cy = 700.0f;
        ClampAnchor(cx, cy);
        Check(Near(cx, 60.0f) && Near(cy, 640.0f),
              "the fallback anchor stays inside the HUD box");
    }

} // namespace

int main()
{
    TimingTests();
    CurveTests();
    GeometryTests();
    if (fails) {
        std::printf("%d failure(s)\n", fails);
        return 1;
    }
    std::printf("prompt anim core: all checks passed\n");
    return 0;
}
