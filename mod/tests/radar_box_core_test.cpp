// Standalone harness for the pure radar-box placement core:
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -I mod/src/net/ui
//       mod/tests/radar_box_core_test.cpp -o /tmp/radar_box_core_test
//   /tmp/radar_box_core_test
//
// Covers the ext's radial clamp, page / hud-root pivot mapping, 60-step rim
// arcs, floor fallback and fade rule.

#include "radar-box-core.hpp"

#include <cmath>
#include <cstdio>
#include <limits>

namespace {

    using namespace net::ui::radar_box::core;

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

    void AnchorTests()
    {
        float x = kRadarCx + 80.0f, y = kRadarCy + 0.0f;
        Check(Pinned(x, y) && ClampIcon(x, y, true),
              "a page icon outside the ring is clamped");
        Check(Near(x, kRadarCx + kClampD) && Near(y, kRadarCy),
              "the clamped icon sits at 60 units from the centre");
        Check(!ClampIcon(x, y, true), "an already-clamped icon is not moved again");

        x = 900.0f; y = 400.0f;
        Check(!ClampIcon(x, y, false), "the hud-root fallback is not clamped");
        Check(Near(x, 900.0f) && Near(y, 400.0f), "the fallback centre is unchanged");

        Check(Near(PivotX(104.0f, true), 104.0f * 3.0f - kWidgetW * 0.5f) &&
              Near(PivotY(179.0f), 179.0f * 3.0f - kWidgetH * 0.5f),
              "page pivots are 3x the text-space centre minus the icon half-size");
        Check(Near(PivotX(740.0f, false), 100.0f * 3.0f - kWidgetW * 0.5f),
              "hud-root pivots are relative to the 640 px screen centre");
    }

    void ArcTests()
    {
        Check(ArcIndex(kRadarCx, kRadarCy - 70.0f) == 0,
              "a blip above the centre is the first arc");
        Check(ArcIndex(kRadarCx + 70.0f, kRadarCy) == 15,
              "a blip to the right is 90 degrees / 15 steps");
        Check(ArcIndex(kRadarCx, kRadarCy + 70.0f) == 30,
              "a blip below is 180 degrees / 30 steps");
        Check(ArcIndex(kRadarCx - 70.0f, kRadarCy) == 45,
              "a blip to the left is 270 degrees / 45 steps");
        Check(ArcIndex(kRadarCx + 70.0f, kRadarCy + 70.0f) == 23,
              "a south-east blip rounds into the 23rd step");
        Check(ArcIndex(kRadarCx + 70.0f, kRadarCy, false) == 45,
              "the mirrored layout reverses the bearing");
        for (int k = 0; k < kArcSteps; ++k) {
            const float a = static_cast<float>(k) * 6.0f * 3.14159265f / 180.0f;
            const float x = kRadarCx + 70.0f * std::sin(a);
            const float y = kRadarCy - 70.0f * std::cos(a);
            Check(ArcIndex(x, y) == k, "arc step round-trips through its bearing");
        }
    }

    void FloorTests()
    {
        Check(FloorFromDelta(0.0f, Floor::Same, true) == Floor::Same &&
              FloorFromDelta(2.0f, Floor::Same, true) == Floor::Same &&
              FloorFromDelta(-2.0f, Floor::Same, true) == Floor::Same,
              "within 2.0 m is our floor (the ext's BXF_LEVEL)");
        Check(FloorFromDelta(2.01f, Floor::Same, true) == Floor::Up &&
              FloorFromDelta(-2.01f, Floor::Same, true) == Floor::Down,
              "more than 2.0 m above / below selects the arrow direction");
        Check(FloorFromDelta(1.9f, Floor::Up, true) == Floor::Up &&
              FloorFromDelta(-1.9f, Floor::Down, true) == Floor::Down,
              "1.9 m keeps the previous other-floor state (15 cm hysteresis)");
        Check(FloorFromDelta(1.84f, Floor::Up, true) == Floor::Same &&
              FloorFromDelta(-1.84f, Floor::Down, true) == Floor::Same,
              "below 1.85 m returns to the plain crate (BXF_BACK)");
        Check(FloorFromDelta(5.0f, Floor::Same, false) == Floor::Same,
              "the ext opt-out disables the floor state");
        Check(FloorFromDelta(std::numeric_limits<float>::quiet_NaN(),
                             Floor::Up, true) == Floor::Same,
              "a bad Y delta falls back to the plain crate");
    }

    void ArcStateTests()
    {
        Check(ArcBank(Floor::Same) == kArcBankSame &&
              ArcBank(Floor::Up) == kArcBankOther &&
              ArcBank(Floor::Down) == kArcBankOther,
              "white arc for our floor, grey arc for another floor");

        Blip inRange;
        inRange.x = kRadarCx + 40.0f; inRange.y = kRadarCy;
        Check(!ArcFor(inRange, true).visible,
              "an in-range box has no rim arc");

        Blip pinned;
        pinned.x = kRadarCx + 70.0f; pinned.y = kRadarCy;
        pinned.pinned = true;
        const ArcState white = ArcFor(pinned, true);
        Check(white.visible && white.index == 15 && white.bank == kArcBankSame,
              "a pinned box on our floor lights the east white arc");
        pinned.floor = Floor::Up;
        const ArcState grey = ArcFor(pinned, true);
        Check(grey.visible && grey.index == white.index &&
              grey.bank == kArcBankOther,
              "the same box one floor up uses the grey bank at the same bearing");
        Check(!ArcFor(pinned, false).visible,
              "an old pak without arcs leaves a pinned crate alone");
    }

    void StateTests()
    {
        Check(IconState(Floor::Up, false) == Floor::Same &&
              IconState(Floor::Down, false) == Floor::Same,
              "without floor widgets every icon is a plain crate");
        Check(IconState(Floor::Down, true) == Floor::Down &&
              StateSlot(IconState(Floor::Down, true)) == 2,
              "with the pak extras a down-floor crate selects the down widget");
        Check(StateSlot(Floor::Same) == 0 && StateSlot(Floor::Up) == 1,
              "state slots are plain / up / down");

        Check(IconAlpha(1.0f, 1.0f, true, false) == 1.0f,
              "page icons inherit the page fade");
        Check(IconAlpha(0.4f, 1.0f, false, false) == 0.4f,
              "hud-root icons take the smaller of radar and text fade");
        Check(IconAlpha(1.0f, 0.2f, false, false) == 0.2f,
              "the text fade dims the fallback too");
        Check(IconAlpha(1.0f, 1.0f, true, true) == 0.0f,
              "a hidden HUD draws no icons");
    }

} // namespace

int main()
{
    AnchorTests();
    ArcTests();
    FloorTests();
    ArcStateTests();
    StateTests();
    if (fails) {
        std::printf("%d failure(s)\n", fails);
        return 1;
    }
    std::printf("radar box core: all checks passed\n");
    return 0;
}
