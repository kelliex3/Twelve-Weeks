// Standalone harness for the native pull-out rules:
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -I mod/src/gameplay
//       mod/tests/melee_pullout_core_test.cpp -o /tmp/melee_pullout_core_test
//   /tmp/melee_pullout_core_test
//
// Covers the INTRO bitfield, the ext's 600/400/30 counter window and the verdict
// state machine, plus the trigger cone and fail-blow damage.

#include "melee-pullout-core.hpp"

#include <cmath>
#include <cstdio>

namespace {

    using namespace gameplay::melee_pullout::core;

    int fails = 0;
    void Check(bool ok, const char* what)
    {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++fails;
    }

    void WireTests()
    {
        const uint8_t bits = IntroBits(true, 2, false, false, 3);
        const Bits d = DecodeIntro(bits);
        Check(d.set && d.dir == 2 && !d.grapple && !d.pounce && d.variant == 3,
              "INTRO bits round-trip set / direction / variant");
        Check(DecodeIntro(IntroBits(false, 0, true, true, 0)).grapple &&
                  DecodeIntro(IntroBits(false, 0, true, true, 0)).pounce,
              "INTRO bits round-trip grapple and pounce");
    }

    void WindowTests()
    {
        const Window w = CounterWindow(3200);
        Check(w.open == 2600 && w.close == 3030,
              "the counter opens 600 ms before the 3.2 s hand-off and closes after 430 ms");
        Check(CounterWindow(3200, 2800).close == 3230 - 0,
              "a late-shown window keeps its 430 ms span");

        PressState st;
        Check(Step(2500, w, true, false, st) == PressResult::kBurn && st.burnt,
              "a press before the window burns the counter");
        Check(Step(2750, w, true, false, st) == PressResult::kNone,
              "a burnt counter cannot be rescued inside the window");
        Check(Step(3200, w, false, true, st) == PressResult::kFail && st.decided,
              "a missed window books a fail verdict");

        PressState win;
        Check(Step(2800, w, true, false, win) == PressResult::kCounter &&
                  win.countered && win.relMs == 200,
              "a press in the window sends a counter with its 200 ms offset");
        Check(Step(3100, w, true, false, win) == PressResult::kNone,
              "the verdict is sent once");

        PressState late;
        Check(Step(3300, w, true, false, late) == PressResult::kFail &&
                  late.relMs == 700,
              "a late press books a fail with the elapsed offset");
    }

    void SpCameraTests()
    {
        Check(Smooth01(-1.0f) == 0.0f && Smooth01(2.0f) == 1.0f &&
                  Smooth01(0.5f) == 0.5f,
              "the SP camera blend is a clamped smoothstep");
        Check(PcpAt(kPcpD, 0.0f) == kPcpD[0] && PcpAt(kPcpD, 99.0f) == kPcpD[12],
              "the authored dolly table clamps at both ends");
        Check(PcpAt(kPcpD, kPcpDt) == kPcpD[1],
              "the authored dolly table samples its second key at 0.2667 s");

        const float me[3] = { 0.0f, 0.0f, 0.0f };
        const float other[3] = { 0.0f, 0.0f, -2.0f };
        const SpPose att = SpCamera(1, 0.0f, me, other);
        const SpPose vic = SpCamera(2, 4.0f, me, other);
        Check(att.fov >= 70.0f && att.fov < 71.0f && vic.fov > 73.0f,
              "the lens swings from 70 to 73.7 degrees over 1.6 s");
        Check(std::isfinite(att.pos[0] + att.pos[1] + att.pos[2] + att.yaw + att.pitch) &&
                  std::isfinite(vic.pos[0] + vic.pos[1] + vic.pos[2] + vic.yaw + vic.pitch),
              "both camera roles produce a finite pose");
        const SpPose bad = SpCamera(1, 0.0f, nullptr, other);
        Check(bad.pos[0] == 0.0f && bad.yaw == 0.0f && bad.fov == 70.0f,
              "a missing body falls back to the live camera rather than NaN");

        const float root[3] = { 1.0f, 0.0f, 2.0f };
        const PairPose pair = PulloutPairPose(root, 0.0f);
        Check(pair.vicPos[0] == 1.0f && pair.vicPos[2] == 2.0f &&
                  pair.attPos[0] == 1.0f && pair.attPos[2] == 1.05f,
              "the attacker root is placed 0.95 m behind the victim's feet");
        const PairPose turned = PulloutPairPose(root, 1.5707963f);
        Check(turned.attPos[0] < 1.0f && std::fabs(turned.attPos[2] - 2.0f) < 0.01f,
              "the pair rotates with the victim's yaw");
    }

    void GeometryTests()
    {
        // Victim faces +Z, so the feet side is -Z. The attacker stands at z = -2.
        Check(BehindCone(0.0f, -2.0f, 0.0f, 0.0f, 0.0f, -1.0f, false),
              "the feet side is inside the 55 degree cone");
        Check(!BehindCone(0.0f, 2.0f, 0.0f, 0.0f, 0.0f, -1.0f, false),
              "the head side is outside the cone");
        Check(BehindCone(0.0f, 2.0f, 0.0f, 0.0f, 0.0f, -1.0f, true),
              "a covered victim may be taken from either side");
        Check(FailDamage(100.0f) == 101.0f && FailDamage(-1.0f) == 100.0f,
              "the fail blow is replicated hp + 1 (100 fallback when unknown)");
        Check(CooldownReady(4000, 3000) && !CooldownReady(2999, 3000),
              "fail cooldown is 3 s from the ending frame");
    }

} // namespace

int main()
{
    WireTests();
    WindowTests();
    SpCameraTests();
    GeometryTests();
    if (fails) {
        std::printf("%d pull-out core checks failed\n", fails);
        return 1;
    }
    std::puts("all pull-out core checks passed");
    return 0;
}
