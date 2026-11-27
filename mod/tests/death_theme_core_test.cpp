// Standalone harness for the pure death-drone rules (no game headers):
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -I mod/src -I mod/src/net/audio
//       mod/tests/death_theme_core_test.cpp -o /tmp/death_theme_core_test
//       && /tmp/death_theme_core_test

#include "death-theme-core.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

    namespace core = net::audio::death_theme::core;

    bool Near(float a, float b, float eps = 1e-5f)
    {
        return std::fabs(a - b) <= eps;
    }

    void TestConstants()
    {
        static_assert(core::kBed == 0.30f);
        static_assert(core::kFloor == 0.02f);
        static_assert(core::kDuckFull == 0.25f);
        static_assert(core::kDownS == 0.06f);
        static_assert(core::kUpS == 0.70f);
        static_assert(core::kFadeMs == 600);
        static_assert(core::kMaxInst == 3);
    }

    void TestCuePickNoRepeat()
    {
        core::State s;
        assert(s.cue == -1 && s.prev == -1);

        assert(core::PickCue(s, 4, 0) == 0);
        assert(s.cue == 0 && s.prev == 0);
        assert(core::PickCue(s, 4, 0) == 1);   // rand 0 now skips prev
        assert(core::PickCue(s, 4, 1) == 2);
        assert(core::PickCue(s, 4, 2) == 3);
        assert(core::PickCue(s, 4, 3) == 0);   // 3 % 3 == 0, still != prev

        for (uint32_t i = 0; i < 64; ++i) {
            const int prev = s.prev;
            const int cue = core::PickCue(s, 4, i * 7u + 1u);
            assert(cue >= 0 && cue < 4);
            assert(cue != prev);
            assert(s.cue == cue && s.prev == cue);
        }

        core::State fresh;
        assert(core::PickCue(fresh, 4, 1) == 1);   // no prev: plain rand % count
        assert(core::PickCue(fresh, 1, 99) == 0);  // a single wave is always it
        assert(core::PickCue(fresh, 0, 99) == 0);  // no waves must not divide by 0
    }

    void TestArmAndRng()
    {
        uint64_t seed = 12345u;
        const uint32_t a = core::NextRand(seed, 1000);
        const uint32_t b = core::NextRand(seed, 1000);
        assert(a != 0u && b != 0u && a != b);
        uint64_t zeroSeed = 0;
        assert(core::NextRand(zeroSeed, 12345) != 0u && zeroSeed != 0); // owner seeds with now

        core::State s;
        s.armed = true;
        s.fading = true;
        s.count = 3;
        s.cue = 2;
        s.prev = 2;
        s.armT0 = 55;
        s.firstT0 = 66;
        s.tickT = 77;
        s.lvl = 0.3f;
        s.fg = 0.5f;
        s.t0[0] = 111;
        s.t0[1] = 222;

        // Arm resets instances / timers, preserves the no-repeat history, and
        // picks from the supplied draw (prev 2: rand 0 -> cue 0).
        const int cue = core::Arm(s, 5000, 4, 0);
        assert(cue == 0 && cue != 2);
        assert(s.armed && !s.fading && s.count == 0);
        assert(s.cue == cue && s.prev == cue);
        assert(s.armT0 == 5000 && s.firstT0 == 0 && s.tickT == 0);
        assert(Near(s.lvl, core::kFloor) && Near(s.fg, 1.0f));
        for (uint64_t t : s.t0) assert(t == 0);

        // A new death cannot pick the same bed even though Arm reset the state.
        const int next = core::Arm(s, 6000, 4, 2);
        assert(next == 3 && next != cue);
    }

    void TestDuckTarget()
    {
        assert(Near(core::DuckTarget(0.0f), core::kBed));
        assert(Near(core::DuckTarget(-1.0f), core::kBed));   // clamp low
        assert(Near(core::DuckTarget(core::kDuckFull), core::kFloor));
        assert(Near(core::DuckTarget(2.0f), core::kFloor));  // clamp high
        const float half = core::kFloor + (core::kBed - core::kFloor) * 0.25f;
        assert(Near(core::DuckTarget(core::kDuckFull * 0.5f), half));
    }

    void TestApproachLevel()
    {
        assert(core::ApproachLevel(core::kBed, core::kFloor, 0.0f) == core::kBed);
        const float down = core::ApproachLevel(core::kBed, core::kFloor, core::kDownS);
        const float up = core::ApproachLevel(core::kFloor, core::kBed, core::kUpS);
        assert(Near(down, core::kFloor + (core::kBed - core::kFloor)
                                * std::exp(-1.0f)));
        assert(Near(up, core::kBed - (core::kBed - core::kFloor)
                             * std::exp(-1.0f)));
        assert(down > core::kFloor && up < core::kBed);
        // Same 60 ms step: duck attack moves much further than the release.
        assert(core::kBed - core::ApproachLevel(core::kBed, core::kFloor, 0.06f)
             > core::ApproachLevel(core::kFloor, core::kBed, 0.06f) - core::kFloor);
    }

    void TestLoopTiming()
    {
        const uint64_t t0 = 10000;
        // 1 s wave, 120 ms of baked crossfade -> overlap after 880 ms.
        assert(!core::LoopDue(t0 + 879, t0, 48000, 48000, 120));
        assert(core::LoopDue(t0 + 880, t0, 48000, 48000, 120));
        assert(core::LoopDue(t0 + 2000, t0, 48000, 48000, 120));

        assert(!core::LoopDue(t0 + 998, t0, 48000, 48000, 1));
        assert(core::LoopDue(t0 + 999, t0, 48000, 48000, 1));
        assert(!core::LoopDue(t0 + 999, t0, 48000, 48000, 0));
        assert(core::LoopDue(t0 + 1000, t0, 48000, 48000, 0));
        assert(!core::LoopDue(t0 - 1, t0, 48000, 48000, 0)); // latest in the future
        assert(core::LoopDue(t0 + 1, t0, 100, 1000, 500));   // fade longer than wave
        assert(!core::LoopDue(t0 + 1, t0, 0, 1000, 0));      // no data
    }

    void TestMaxCopies()
    {
        core::State s;
        core::Reset(s);
        std::array<uint64_t, core::kMaxInst> starts{};
        uint64_t now = 1000;
        int copies = 0;

        auto spawn = [&]() {
            for (int i = 0; i < core::kMaxInst; ++i) {
                if (starts[i] == 0) {
                    starts[i] = now;
                    s.t0[i] = now;
                    ++s.count;
                    ++copies;
                    return true;
                }
            }
            return false;
        };

        for (int i = 0; i < core::kMaxInst; ++i) {
            assert(spawn());
            now += 880;                          // ext's copy interval
        }
        assert(copies == core::kMaxInst && copies == 3);
        assert(!spawn());                        // no fourth slot

        assert(core::LatestT0(s) == starts[core::kMaxInst - 1]);
        assert(core::LoopDue(now, core::LatestT0(s), 48000, 48000, 120));
    }

    void TestFinalBeforeFirst()
    {
        assert(core::ShouldDropFinalBeforeFirst(true, 0));
        assert(!core::ShouldDropFinalBeforeFirst(false, 0));
        assert(!core::ShouldDropFinalBeforeFirst(true, 1));
    }

    void TestCutReasons()
    {
        assert(core::ShouldCut(1, false, true) == core::CutReason::kNone);
        assert(core::ShouldCut(1, false, false) == core::CutReason::kNotInPlay);
        assert(core::ShouldCut(0, true, true) == core::CutReason::kDeathScreenOff);
        assert(core::ShouldCut(2, true, true) == core::CutReason::kRespawned);
        assert(core::ShouldCut(3, true, true) == core::CutReason::kMatchEnded);
        assert(core::ShouldCut(4, false, true) == core::CutReason::kDeathScreenOff);
        assert(core::ShouldCut(1, true, true) == core::CutReason::kFinalFade);

        // Exact priority: losing the match beats every phase and the final flag.
        assert(core::ShouldCut(0, true, false) == core::CutReason::kNotInPlay);
        assert(core::ShouldCut(2, true, false) == core::CutReason::kNotInPlay);
        assert(core::ShouldCut(3, true, false) == core::CutReason::kNotInPlay);
        // Respawn / match end beat the countdown fade.
        assert(core::ShouldCut(2, true, true) == core::CutReason::kRespawned);
        assert(core::ShouldCut(3, true, true) == core::CutReason::kMatchEnded);

        assert(std::string(core::Why(core::CutReason::kNotInPlay))
            == "match left the playing state");
        assert(std::string(core::Why(core::CutReason::kRespawned)) == "respawned");
        assert(std::string(core::Why(core::CutReason::kFinalFade))
            == "countdown fade to black");
    }

    void TestReset()
    {
        core::State s;
        s.armed = true;
        s.fading = true;
        s.count = 2;
        s.cue = 1;
        s.prev = 3;
        s.armT0 = 10;
        s.firstT0 = 20;
        s.tickT = 30;
        s.lvl = core::kBed;
        s.fg = 0.5f;
        s.t0[0] = 40;
        s.t0[2] = 50;

        core::Reset(s);
        assert(!s.armed && !s.fading && s.count == 0);
        assert(s.cue == -1 && s.prev == -1);
        assert(s.armT0 == 0 && s.firstT0 == 0 && s.tickT == 0);
        assert(s.lvl == 0.0f && s.fg == 0.0f);
        for (uint64_t t : s.t0) assert(t == 0);
        assert(core::ShouldCut(1, false, true) == core::CutReason::kNone);
    }

} // namespace

int main()
{
    TestConstants();
    TestCuePickNoRepeat();
    TestArmAndRng();
    TestDuckTarget();
    TestApproachLevel();
    TestLoopTiming();
    TestMaxCopies();
    TestFinalBeforeFirst();
    TestCutReasons();
    TestReset();
    std::printf("DEATH_THEME_CORE_TEST_OK\n");
    return 0;
}
