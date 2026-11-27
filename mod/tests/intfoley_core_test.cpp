// Standalone test for the pure interrogation-foley rules; no engine / game
// headers. A synthetic explus DataView stands in for the guarded private rows.
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -I mod/src -I mod/src/net/audio
//       mod/tests/intfoley_core_test.cpp -o /tmp/opencode/intfoley_check
//   /tmp/opencode/intfoley_check

#include "intfoley-core.hpp"

#include <cassert>
#include <cstdio>

namespace {

    using namespace net::audio::intfoley::core;

    struct Fixture {
        net::audio::explus::core::Row rows[8] = {};
        net::audio::explus::core::DataView d;

        Fixture(int struggleMs = -1)
        {
            if (struggleMs >= 0) {
                rows[0] = { 1, 21, static_cast<uint8_t>(net::audio::explus::core::Layer::kStrugTorso),
                            1, 2, 100, static_cast<uint16_t>(struggleMs) };
            }
            d.rows = rows;
            d.rowCount = struggleMs >= 0 ? 1 : 0;
        }
    };

    void TestGating()
    {
        assert(Pair("fist-interrogate-enter-front") == PairKind::kFist);
        assert(Pair("shiv-interrogate-enter-front") == PairKind::kShiv);
        assert(Pair("fist-execute-enter-front") == PairKind::kNone);
        assert(Pair(nullptr) == PairKind::kNone);
        assert(Gate(1, "fist-interrogate-enter-front", true));
        assert(!Gate(1, "fist-interrogate-enter-front", false));
        assert(!Gate(0, "fist-interrogate-enter-front", true));
        assert(!Gate(1, "pipe-execute", true));
    }

    void TestVoiceYield()
    {
        LineFeed line;
        assert(!VoxClash(line, 1000.0, 800.0, 1, "fist-interrogate-enter-front"));
        line.on = true;
        line.t0 = 1000.0;
        line.busy[0] = 500.0;
        assert(VoxClash(line, 1000.0, 550.0, 1, "fist-interrogate-enter-front"));
        assert(!VoxClash(line, 1000.0, 650.0, 1, "fist-interrogate-enter-front"));
        line.busy[0] = 0.0;
        line.step = 0;
        line.count = 1;
        line.steps[0].at = 1400.0;
        assert(VoxClash(line, 1000.0, 1100.0, 1, "shiv-interrogate-enter-front"));
        assert(!VoxClash(line, 1000.0, 1100.0, 1, "pipe-execute"));
        line.shift = 900.0;
        assert(!VoxClash(line, 1000.0, 1100.0, 1, "shiv-interrogate-enter-front"));
        line = LineFeed{};
        line.on = true;
        line.t0 = 1000.0;
        line.vline[0] = 2000.0;
        line.vline[1] = 2400.0;
        assert(VicLine(line, 1000.0, 2200.0, 1, "fist-interrogate-enter-front"));
        assert(!VicLine(line, 1000.0, 2600.0, 1, "fist-interrogate-enter-front"));
        assert(!VicLine(line, 1000.0, 2200.0, 1, "pipe-execute"));
    }

    void TestShaking()
    {
        Fixture f;
        State s;
        assert(!Begin(s, 0, "fist-interrogate-enter-front", 1000.0, 4600.0, 0xAA, 0xBB));
        assert(Begin(s, 1, "fist-interrogate-enter-front", 1000.0, 4600.0, 0xAA, 0xBB));
        assert(s.on && s.fist && s.a == 2400.0 && s.b == 4300.0 && s.next == 2400.0);
        LineFeed line;
        Result r;
        for (double ems = 2350.0; ems <= 4500.0; ems += 50.0)
            Tick(s, ems, f.d, 21, line, false, r);
        assert(r.count >= 2 && r.count < kMaxCues);
        double prev = 0.0;
        for (int i = 0; i < r.count; ++i) {
            assert(r.cues[i].fist && r.cues[i].att == 0xAA && r.cues[i].vic == 0xBB);
            assert(r.cues[i].atMs >= s.a && r.cues[i].atMs < s.b);
            assert(r.cues[i].k == i);
            if (i) {
                const double gap = r.cues[i].atMs - prev;
                assert(gap >= 560.0 && gap <= 800.0);
                assert(gap <= r.cues[i - 1].atMs - (i > 1 ? r.cues[i - 2].atMs : 0.0) + 1.0
                       || i < 3);
            }
            prev = r.cues[i].atMs;
            assert(net::audio::explus::core::IntFoleyRecipe(r.cues[i].k, true).count == 3);
        }
        assert(s.finished);

        State q;
        assert(Begin(q, 1, "shiv-interrogate-enter-front", 2000.0, 2550.0, 0xAA, 0xBB));
        assert(!q.fist && q.a == 700.0 && q.b == 2250.0);
        Result qr;
        for (double ems = 650.0; ems <= 2350.0; ems += 50.0)
            Tick(q, ems, f.d, 23, line, false, qr);
        assert(qr.count >= 2 && !qr.cues[0].fist);
        assert(net::audio::explus::core::IntFoleyRecipe(qr.cues[0].k, false).count >= 3);

        // A late join skips the moments already gone.
        State late;
        assert(Begin(late, 1, "fist-interrogate-enter-front", 3000.0, 7600.0, 0xAA, 0xBB));
        Result lr;
        for (double ems = 3150.0; ems <= 4400.0; ems += 50.0)
            Tick(late, ems, f.d, 21, line, false, lr);
        assert(lr.count >= 1 && lr.cues[0].k > 0 && lr.cues[0].atMs >= 2400.0);

        // The sidecar switch stops the shaking.
        State off;
        assert(Begin(off, 1, "fist-interrogate-enter-front", 1000.0, 4600.0, 0xAA, 0xBB));
        Result orr;
        Tick(off, 9999.0, f.d, 21, line, true, orr);
        assert(orr.count == 0);
    }

    void TestStruggleGuard()
    {
        Fixture f(2500);
        assert(StruggleNear(f.d, 21, 2550.0));
        assert(!StruggleNear(f.d, 21, 3000.0));
        assert(!StruggleNear(f.d, 22, 2500.0));

        State s;
        assert(Begin(s, 1, "fist-interrogate-enter-front", 1000.0, 4600.0, 0xAA, 0xBB));
        LineFeed line;
        Result r;
        for (double ems = 2350.0; ems <= 4500.0; ems += 50.0)
            Tick(s, ems, f.d, 21, line, false, r);
        assert(r.count > 0);
        for (int i = 0; i < r.count; ++i)
            assert(!StruggleNear(f.d, 21, r.cues[i].atMs));
    }

} // namespace

int main()
{
    TestGating();
    TestVoiceYield();
    TestShaking();
    TestStruggleGuard();
    std::puts("INTFOLEY_CORE_TEST_OK");
    return 0;
}
