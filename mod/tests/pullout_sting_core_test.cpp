// Standalone test for the pure prone pull-out sting rules; no engine, DC,
// network or sidecar headers.
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -I mod/src
//       mod/tests/pullout_sting_core_test.cpp -o /tmp/opencode/pullout_check
//   /tmp/opencode/pullout_check

#include "net/audio/pullout-sting-core.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>

namespace {

    using namespace net::audio::pullout_sting::core;

    int Find(const Result& r, Event event, int after = 0)
    {
        for (int i = after; i < r.count; ++i)
            if (r.out[i].event == event)
                return i;
        return -1;
    }

    int FindSay(const Result& r, Line line)
    {
        for (int i = 0; i < r.count; ++i)
            if (r.out[i].event == Event::kSay && r.out[i].line == line)
                return i;
        return -1;
    }

    int Says(const Result& r, Line line) { return FindSay(r, line) >= 0 ? 1 : 0; }

    void TestGrabAndBeats()
    {
        State s;
        Result r;
        Grab(s, r, 10000, false);
        assert(s.on && s.grabSayAt == 10150);
        int i = Find(r, Event::kShake);
        assert(i >= 0 && r.out[i].amp == 1.7f);
        i = Find(r, Event::kMeasuredShake);
        assert(i >= 0 && r.out[i].amp == 1.7f);
        assert(Find(r, Event::kRumble) >= 0);
        i = Find(r, Event::kMusic);
        assert(i >= 0 && r.out[i].music == Music::kHold);

        TickIn in;
        in.now = 10149;
        r = Result{};
        Tick(s, r, in);
        assert(Says(r, Line::kGrabbed) == 0);
        assert(Find(r, Event::kRender) >= 0);
        assert(Find(r, Event::kFov) >= 0);

        in.now = 10150;
        r = Result{};
        Tick(s, r, in);
        assert(Says(r, Line::kGrabbed) == 1);
        r = Result{};
        Tick(s, r, in);
        assert(Says(r, Line::kGrabbed) == 0);

        in.now = 10900;
        r = Result{};
        Tick(s, r, in);
        assert(Says(r, Line::kStruggle) == 1);
        assert(Find(r, Event::kDof) < 0);       // default opt-out
        in.now = 12500;
        r = Result{};
        Tick(s, r, in);
        assert(Says(r, Line::kChoked) == 1);
        r = Result{};
        Tick(s, r, in);
        assert(Says(r, Line::kChoked) == 0);
    }

    void TestDofAndLens()
    {
        TickIn in;
        in.now = 12950;
        in.dofOff = false;
        State s;
        Result r;
        Grab(s, r, 10000, false);
        r = Result{};
        Tick(s, r, in);
        assert(Find(r, Event::kDof) >= 0);
        r = Result{};
        Tick(s, r, in);
        assert(Find(r, Event::kDof) >= 0);      // SP asks every struggle frame
        in.dofOff = true;
        r = Result{};
        Tick(s, r, in);
        assert(Find(r, Event::kDof) < 0);       // sidecar opt-out

        assert(std::fabs(FovMul(10000, 10000, false) - 0.9498f) < 0.0001f);
        assert(std::fabs(FovMul(10000 + 2700, 10000, false) - 1.0f) < 0.0001f);
        assert(std::fabs(FovMul(10000 + 9000, 10000, false) - 1.0f) < 0.0001f);
        assert(FovMul(10000 + 900, 10000, true) == 0.0f);
        const float a = FovMul(10500, 10000, false);
        const float b = FovMul(10700, 10000, false);
        assert(b > a && a > 0.9498f && b < 1.0f);
        in.swingOff = true;
        r = Result{};
        Tick(s, r, in);
        assert(Find(r, Event::kFov) < 0);
    }

    void TestMeasuredCurve()
    {
        assert(MeasuredPitch(10000, 10000, 1.7f) == 0.0f);
        const float peak = MeasuredPitch(10167, 10000, 1.7f);
        assert(peak < -1.5f && peak > -2.3f);       // PS32 view-down peak, scaled
        assert(std::fabs(MeasuredPitch(10450, 10000, 1.7f)) < 0.0001f);
        assert(MeasuredPitch(10167, 10000, 0.0f) == 0.0f);
        assert(MeasuredPitch(9999, 10000, 1.7f) == 0.0f);

        // The native producer feeds the same lens: 0.9498 * game FOV ~= 70 deg.
        assert(std::fabs(kGameFov * kFov0 - 70.0f) < 0.1f);
        assert(std::fabs(kGameFov * FovMul(12700, 10000, false) - kGameFov) < 0.0001f);
    }

    void TestEscape()
    {
        State s;
        Result r;
        Grab(s, r, 1000, true);
        int i = Find(r, Event::kMusic);
        assert(i >= 0 && r.out[i].music == Music::kRiser);
        r = Result{};
        Verdict(s, r, 2000, true, true, false);
        i = Find(r, Event::kMusic);
        assert(i >= 0 && r.out[i].music == Music::kEscape);
        assert(Find(r, Event::kSfx) < 0 && s.escapeAt == 2300);

        TickIn in;
        in.now = 2299;
        r = Result{};
        Tick(s, r, in);
        assert(Says(r, Line::kEscape) == 0);
        in.now = 2300;
        r = Result{};
        Tick(s, r, in);
        assert(Says(r, Line::kEscape) == 1);
        r = Result{};
        Tick(s, r, in);
        assert(Says(r, Line::kEscape) == 0);
        r = Result{};
        Verdict(s, r, 2400, false, false, false);
        assert(r.count == 0);                  // one verdict per drag
    }

    void TestFailBlow()
    {
        State s;
        Result r;
        Grab(s, r, 0, false);
        r = Result{};
        Verdict(s, r, 1000, false, false, true);
        assert(Find(r, Event::kSfx) >= 0);
        assert(Says(r, Line::kFailDeath) == 1);
        int i = Find(r, Event::kMusic);
        assert(i >= 0 && r.out[i].music == Music::kFail);
        Outcome(s, r, 1000, false, 1.0f);
        assert(s.blowAt == 2000);
        Outcome(s, r, 1000, true, 1.0f);        // a counter never has a blow
        assert(s.blowAt == 2000);

        TickIn in;
        in.now = 1999;
        in.verdictVox = true;
        r = Result{};
        Tick(s, r, in);
        assert(Find(r, Event::kSpikeRender) < 0);
        in.now = 2000;
        r = Result{};
        Tick(s, r, in);
        i = Find(r, Event::kShake);
        assert(i >= 0 && r.out[i].amp == 2.2f);
        i = Find(r, Event::kMeasuredShake);
        assert(i >= 0 && r.out[i].amp == 2.2f);
        assert(Says(r, Line::kBlow) == 1);
        assert(r.out[FindSay(r, Line::kBlow)].effort == kPain);
        assert(Find(r, Event::kSpikeRender) >= 0);
        r = Result{};
        Tick(s, r, in);
        assert(Says(r, Line::kBlow) == 0);
        in.now = 2899;
        r = Result{};
        Tick(s, r, in);
        assert(Find(r, Event::kSpikeRender) >= 0);
        in.now = 2900;
        r = Result{};
        Tick(s, r, in);
        assert(Find(r, Event::kSpikeRender) < 0);

        in.spikeOff = true;
        State s2;
        Result r2;
        Grab(s2, r2, 0, false);
        Verdict(s2, r2, 1000, false, false, false);
        Outcome(s2, r2, 1000, false, 0.01f);    // clamped to 50 ms
        assert(s2.blowAt == 1050);
        in.now = 1050;
        r2 = Result{};
        Tick(s2, r2, in);
        assert(Find(r2, Event::kShake) < 0 && Find(r2, Event::kSpikeRender) < 0);
    }

    void TestCutAndReset()
    {
        State s;
        Result r;
        Grab(s, r, 0, false);
        Cut(s, r, "dead");
        assert(!s.on && s.grabSayAt == 0 && r.end);
        assert(Find(r, Event::kCut) >= 0);
        r = Result{};
        Tick(s, r, TickIn{});
        assert(r.count == 0);
        Cut(s, r, "again");
        assert(r.count == 0 && !r.end);

        Grab(s, r, 100, false);
        Reset(s);
        assert(!s.on && s.t0 == 0 && s.voxN == 0);
    }

} // namespace

int main()
{
    TestGrabAndBeats();
    TestDofAndLens();
    TestMeasuredCurve();
    TestEscape();
    TestFailBlow();
    TestCutAndReset();
    std::puts("pullout sting core: ok");
    return 0;
}
