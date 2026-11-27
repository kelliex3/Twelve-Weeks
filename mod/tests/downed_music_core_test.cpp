// Standalone harness for the pure downed-music rules (no game headers):
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -I mod/src -I mod/src/net/audio
//       mod/tests/downed_music_core_test.cpp -o /tmp/downed_music_core_test
//       && /tmp/downed_music_core_test

#include "downed-music-core.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>

namespace {

    namespace core = net::audio::downed_music::core;

    constexpr double kRate = 48000.0;
    constexpr int kLoop = 96000;                 // 2 s bar at 48 kHz

    bool Near(double a, double b, double eps = 1e-4)
    {
        return std::fabs(a - b) <= eps;
    }

    core::Input Base(uint64_t now, uint64_t token)
    {
        core::Input in;
        in.nowMs = now;
        in.downToken = token;
        in.downed = true;
        in.inPlay = true;
        in.hp = 1.0f;
        in.fraction = 0.0f;
        in.volume = 1.0f;
        in.rate = kRate;
        in.stages = core::kStageCount;
        for (int s = 0; s < core::kStageCount; ++s) {
            in.rmsDb[s] = -27.0f;
            in.loopFrames[s] = kLoop;
        }
        in.mixerLive = true;
        in.tailFrames = 7 * static_cast<int>(kRate);
        return in;
    }

    void Live(core::Input& in, const core::State& s, double pos)
    {
        in.haveVoice = true;
        in.voiceStage = s.stage;
        in.voicePos = pos;
        in.oldHaveVoice = false;
        in.oldVoiceStage = -1;
    }

    // Tick from a fresh down through the 300 ms delay and the 250 ms start fade.
    core::Input Start(core::State& s, uint64_t downAt)
    {
        core::Input in = Base(downAt, downAt);
        assert(!core::Tick(s, in).start);
        in.nowMs = downAt + core::kStartMs - 1;
        assert(!core::Tick(s, in).start);
        in.nowMs = downAt + core::kStartMs;
        const core::Output out = core::Tick(s, in);
        assert(out.start && out.stage == 0);
        assert(out.fadeMs == static_cast<int>(core::kStartFadeMs) && !out.linear);
        assert(s.running == 1 && s.f == 0.0f);

        Live(in, s, 0.0);
        in.nowMs += core::kStartFadeMs;
        core::Tick(s, in);                       // half-way through the start fade
        in.nowMs += core::kStartFadeMs;
        core::Tick(s, in);                       // fade done, no outgoing pair
        assert(!s.xf);
        return in;
    }

    // Raise the fraction in <=0.06 steps so no hit-quick window opens.
    void Glide(core::Input& in, core::State& s, float from, float to, double pos)
    {
        constexpr int kSteps = 20;
        for (int i = 1; i <= kSteps; ++i) {
            in.fraction = from + (to - from) * static_cast<float>(i) / kSteps;
            Live(in, s, pos);
            ++in.nowMs;
            core::Tick(s, in);
        }
        assert(!in.nowMs || s.quickUntil <= in.nowMs);
    }

    void TestStageOf()
    {
        assert(core::StageOf(-1.0f) == 0);
        assert(core::StageOf(0.0f) == 0);
        assert(core::StageOf(0.299f) == 0);
        assert(core::StageOf(0.30f) == 1);
        assert(core::StageOf(0.519f) == 1);
        assert(core::StageOf(0.52f) == 2);
        assert(core::StageOf(0.759f) == 2);
        assert(core::StageOf(0.76f) == 3);
        assert(core::StageOf(2.0f) == 3);
        assert(core::StageOf(0.52f, 2) == 1);
    }

    void TestDb()
    {
        assert(Near(core::Db(-5.0f), -27.0));
        assert(Near(core::Db(0.0f), -27.0));
        assert(Near(core::Db(0.15f), -25.0));      // (0,-27) -> (0.30,-23)
        assert(Near(core::Db(0.30f), -23.0));
        assert(Near(core::Db(0.41f), -20.75));     // (0.30,-23) -> (0.52,-18.5)
        assert(Near(core::Db(0.52f), -18.5));
        assert(Near(core::Db(0.70f), -14.5));
        assert(Near(core::Db(0.80f), -11.5));
        assert(Near(core::Db(0.90f), -10.5));
        assert(Near(core::Db(1.0f), -10.5));
        assert(Near(core::Db(9.0f), -10.5));
    }

    void TestStageGain()
    {
        assert(Near(core::StageGain(0, 0.0f, -27.0f, 0.5f), 0.5));
        assert(Near(core::StageGain(1, 0.0f, -40.0f, 0.7f), 0.7)); // rms-matched cap
        const float expected = 0.8f * std::pow(10.0f, (-27.0f + 10.0f) / 20.0f);
        assert(Near(core::StageGain(0, 0.0f, -10.0f, 0.8f), expected));
        assert(Near(core::StageGain(-1, 0.0f, -27.0f, 1.0f), 0.0));
    }

    void TestSlewHitDuck()
    {
        assert(Near(core::SlewFraction(0.0f, 1.0f, 0.0f), 0.0));
        assert(Near(core::SlewFraction(0.0f, 1.0f, 0.02f), 0.25));
        assert(Near(core::SlewFraction(0.0f, 1.0f, 0.04f), 0.5));
        assert(Near(core::SlewFraction(0.0f, 1.0f, core::kSmoothS), 1.0));
        assert(Near(core::SlewFraction(0.0f, 1.0f, 1.0f), 1.0));
        assert(Near(core::SlewFraction(0.2f, 0.8f, -1.0f), 0.2));

        assert(core::HitJump(0.07f, 0.0f));
        assert(!core::HitJump(0.06f, 0.0f));       // strict, as in the ext
        assert(!core::HitJump(0.1f, 0.06f));
        assert(!core::HitJump(0.0f, 0.5f));

        assert(Near(core::Duck(0.0f), 1.0));
        assert(Near(core::Duck(0.15f), 0.575));
        assert(Near(core::Duck(0.30f), 0.15));
        assert(Near(core::Duck(1.0f), 0.15));
        assert(Near(core::Duck(-1.0f), 1.0));
    }

    void TestBarSwitch()
    {
        core::State s;
        core::Input in = Start(s, 1000);
        Glide(in, s, 0.0f, 0.40f, kLoop / 4);     // far from any bar line

        // dt smoothing leaves f just below the target; the stage decision uses
        // the caller's fraction with the 0.05 look-ahead, so stage 1 is wanted.
        Live(in, s, kLoop - 4000);               // 83 ms before the bar line
        ++in.nowMs;
        const core::Output out = core::Tick(s, in);
        assert(out.switchStage && out.stage == 1 && !out.start);
        assert(out.fadeMs == static_cast<int>(core::kXfMs) && !out.linear);
        assert(s.quickUntil <= in.nowMs);
    }

    void TestOneStagePerBar()
    {
        core::State s;
        core::Input in = Start(s, 1000);
        Glide(in, s, 0.0f, 0.90f, kLoop / 4);
        assert(s.quickUntil <= in.nowMs);

        Live(in, s, kLoop - 4000);
        ++in.nowMs;
        const core::Output out = core::Tick(s, in);
        assert(out.switchStage && out.stage == 1); // not stage 3: one step per bar
    }

    void TestHitBeatSwitch()
    {
        core::State s;
        core::Input in = Start(s, 1000);
        const uint64_t at = in.nowMs;

        in.fraction = 0.30f;                      // a >0.06 jump: beat-synced
        Live(in, s, kRate / 4);                   // 250 ms: far from a beat
        const core::Output hit = core::Tick(s, in);
        assert(hit.hit && s.quickUntil == at + core::kQuickMs);
        assert(!hit.switchStage);

        const double beat = core::kBeatS * kRate;
        Live(in, s, beat - 2000);                 // 42 ms before the beat
        ++in.nowMs;
        const core::Output out = core::Tick(s, in);
        assert(out.switchStage && out.stage == 1);
        assert(out.fadeMs == static_cast<int>(core::kXfHitMs) && !out.linear);
    }

    void TestLoopWrap()
    {
        core::State s;
        core::Input in = Start(s, 1000);
        Live(in, s, kLoop + 512);                 // inside the appended start
        ++in.nowMs;
        const core::Output out = core::Tick(s, in);
        assert(out.switchStage && out.stage == 0);
        assert(out.linear && out.fadeMs == static_cast<int>(core::kWrapMs));
        assert(Near(out.offset, 512.0));           // source frames, not seconds
    }

    void TestCutPrioritiesAndHold()
    {
        {
            core::State s;
            core::Input in = Start(s, 1000);
            in.inPlay = false;
            const uint64_t now = in.nowMs;
            const core::Output out = core::Tick(s, in);
            assert(out.cut && out.fadeMs == core::kMatchFadeMs);
            assert(out.hold && out.holdUntil == now + core::kMatchFadeMs + core::kHoldMs);
        }
        {
            core::State s;
            core::Input in = Start(s, 1000);
            in.exec = true;
            const core::Output out = core::Tick(s, in);
            assert(out.cut && out.fadeMs == core::kExecFadeMs);
            assert(out.hold);
        }
        {
            core::State s;
            core::Input in = Start(s, 1000);
            in.downed = false;                    // revived / up
            const core::Output out = core::Tick(s, in);
            assert(out.cut && out.fadeMs == core::kReviveFadeMs);
            assert(out.hold);
        }
        {
            core::State s;
            core::Input in = Start(s, 1000);
            in.downed = false;                    // killed while down: bad gate is open
            in.hp = 0.0f;                         // dead with no audible stage: 400 ms
            in.volume = 0.0f;                     // (no tail can start at zero volume)
            Live(in, s, 1000.0);
            const core::Output out = core::Tick(s, in);
            assert(out.cut && out.fadeMs == core::kDeathFadeMs);
            assert(!s.dying);
        }
    }

    void TestTailHoldAndDuck()
    {
        core::State s;
        core::Input in = Start(s, 1000);
        const double beat = core::kBeatS * kRate;
        Live(in, s, 1000.0);
        in.dying = true;
        in.hp = 0.0f;
        const uint64_t now = in.nowMs;
        const core::Output scheduled = core::Tick(s, in);
        assert(!scheduled.cut && !scheduled.tail);
        assert(s.dying && s.tailAt > now);
        const double rem = (beat - 1000.0) / kRate - core::kHalfGuardS;
        assert(Near(static_cast<double>(s.tailAt - now), rem * 1000.0, 1.0));

        {   // inside the ext's 30 ms near-beat guard the tail is due immediately
            core::State near;
            core::Input inNear = Start(near, 1000);
            Live(inNear, near, beat - 0.025 * kRate);
            inNear.dying = true;
            inNear.hp = 0.0f;
            const uint64_t at = inNear.nowMs;
            core::Tick(near, inNear);
            assert(near.dying && near.tailAt == at);
        }

        in.nowMs = s.tailAt;
        const core::Output tail = core::Tick(s, in);
        assert(tail.tail && tail.stage == core::kEndStage);
        assert(tail.cut && tail.fadeMs == core::kTailFadeMs);
        assert(tail.hold && tail.holdUntil == in.nowMs
               + static_cast<uint64_t>(in.tailFrames * 1000.0 / in.rate) + core::kHoldMs);

        in.tailLive = true;
        in.tailGain = 0.30f;
        in.tailPeak = 1.0f;
        ++in.nowMs;
        const core::Output ducked = core::Tick(s, in);
        assert(ducked.duckCapture && Near(ducked.duck, 0.15));
        assert(s.duckCapture);

        in.tailLive = false;
        ++in.nowMs;
        const core::Output released = core::Tick(s, in);
        assert(released.duckCapture && Near(released.duck, 1.0));
        assert(released.stopTail && !s.duckCapture);

        ++in.nowMs;
        assert(!core::Tick(s, in).duckCapture);
    }

    void TestFadeResetAndSecondDown()
    {
        core::State s;
        core::Input in = Start(s, 1000);
        in.downed = false;                        // revived: 800 ms cut
        assert(core::Tick(s, in).fadeMs == core::kReviveFadeMs);
        assert(s.fading);

        in.haveVoice = false;                     // the cut faded out
        in.oldHaveVoice = false;
        ++in.nowMs;
        core::Tick(s, in);
        assert(!s.fading && s.running == 0);      // reset outside the live block

        in.downed = true;
        in.downToken = 50000;
        in.nowMs = 50000;
        in.fraction = 0.0f;
        core::Tick(s, in);
        assert(s.running == -1 && s.f == 0.0f && s.tgtPrev == 0.0f
               && s.quickUntil == 0 && !s.dying && !s.tailAt);

        in.nowMs = 50000 + core::kStartMs;
        const core::Output again = core::Tick(s, in);
        assert(again.start && s.running == 1);
    }

} // namespace

int main()
{
    TestStageOf();
    TestDb();
    TestStageGain();
    TestSlewHitDuck();
    TestBarSwitch();
    TestOneStagePerBar();
    TestHitBeatSwitch();
    TestLoopWrap();
    TestCutPrioritiesAndHold();
    TestTailHoldAndDuck();
    TestFadeResetAndSecondDown();
    std::printf("DOWNED_MUSIC_CORE_TEST_OK\n");
    return 0;
}
