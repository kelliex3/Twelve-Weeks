// Standalone harness for the pure execution-victim music rules (no game headers):
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -I mod/src -I mod/src/net/audio
//       mod/tests/exec_music_core_test.cpp -o /tmp/exec_music_core_test
//       && /tmp/exec_music_core_test

#include "exec-music-core.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

    namespace core = net::audio::exec_music::core;

    bool Near(double a, double b, double eps = 1e-6)
    {
        return std::fabs(a - b) <= eps;
    }

    core::Input InputAt(uint64_t now, bool inPlay, bool cueLive,
                        float volume = 1.0f, bool mixerLive = true)
    {
        core::Input in;
        in.nowMs     = now;
        in.inPlay    = inPlay;
        in.cueLive   = cueLive;
        in.mixerLive = mixerLive;
        in.volume    = volume;
        return in;
    }

    // Start -> due play -> successful voice start, like the native owner does it.
    uint64_t PlayCue(core::State& s, uint64_t now, float killS, int pick)
    {
        core::Start(s, now, killS, pick, 0);
        const uint64_t at = s.pendingAt;
        const core::Output out = core::Tick(s, InputAt(at, true, false));
        assert(out.startCue && out.gain == core::kLevel);
        core::CommitStart(s, at, true);
        return at;
    }

    void TestConstants()
    {
        static_assert(core::kLevel == 0.85f);
        static_assert(core::kCapMs == 12500);
        static_assert(core::kFadeMs == 300);
        static_assert(core::kResumeMs == 1500);
        static_assert(core::kDeathDelayMs == 153);
        static_assert(core::kCues == 5);
        assert(core::kHit[0] == 2.44f && core::kHit[1] == 2.35f
            && core::kHit[2] == 2.31f && core::kHit[3] == 2.35f
            && core::kHit[4] == 2.39f);
        assert(core::ClampPick(-1) == 0 && core::ClampPick(5) == 0
            && core::ClampPick(4) == 4);
    }

    void TestStartTiming()
    {
        core::State s;
        const uint64_t hold = core::Start(s, 100000, 2.5f, 0, 0);
        assert(s.pick == 0 && s.pendingAt == 100212 && s.seek == 0.0);
        assert(hold == 100212 + core::kCapMs + core::kResumeMs);
        assert(s.pendingCut);                     // now >= holdUntil (0)
        assert(Near(core::Lead(2.5f, 0), 0.213));

        // ext EXMUS_HIT order: each hit lands 153 ms after the move starts.
        // EXMUS_HIT is a float: 2.5 + 0.153 - 2.44f truncates to 212 ms in the
        // ext's uint64 cast, the others to 303 / 343 / 303 / 262 ms.
        const uint64_t expectAt[core::kCues] = { 212, 303, 343, 303, 262 };
        for (int p = 0; p < core::kCues; ++p) {
            core::State q;
            core::Start(q, 1000, 2.5f, p, 0);
            assert(q.pendingAt == 1000 + expectAt[p]);
            assert(q.seek == 0.0);
        }

        // Late join: kill 2.0 s with hit 2.44 s -> start now, seek 0.287 s in.
        core::State mid;
        core::Start(mid, 1000, 2.0f, 0, 0);
        assert(mid.pendingAt == 1000);
        assert(Near(mid.seek, 0.287));
        assert(Near(core::StartFrame(mid.seek, 44100), mid.seek * 44100.0, 1e-9));
        assert(core::StartFrame(mid.seek, 0) == 0.0);

        // The combat-music cut is decided from the hold as it was at Start.
        core::State blocked;
        core::Start(blocked, 999, 2.5f, 0, 1000);
        assert(!blocked.pendingCut);
        core::State free;
        core::Start(free, 1000, 2.5f, 0, 1000);
        assert(free.pendingCut);
    }

    void TestPendingStartOrder()
    {
        core::State s;
        core::Start(s, 1000, 2.5f, 0, 0);
        const uint64_t at = s.pendingAt;
        assert(at == 1212);

        core::Output early = core::Tick(s, InputAt(at - 1, true, false));
        assert(!early.startCue && s.pendingAt == at);

        // ext consumes pending.at BEFORE checking match_substate(): a due cue
        // outside play is dropped, not deferred.
        core::Output out = core::Tick(s, InputAt(at, false, false));
        assert(!out.startCue && s.pendingAt == 0);
        out = core::Tick(s, InputAt(at + 1, true, false));
        assert(!out.startCue && !s.haveVoice);
    }

    void TestPlayAndCap()
    {
        core::State s;
        const uint64_t when = 1000;
        const uint64_t at = PlayCue(s, when, 2.5f, 0);
        assert(at == 1212 && s.haveVoice && s.t0 == at);
        assert(!s.fading);

        core::Output out = core::Tick(s, InputAt(at + core::kCapMs - 1, true, true));
        assert(!out.cutCue && !s.fading);
        out = core::Tick(s, InputAt(at + core::kCapMs, true, true));
        assert(std::string(out.why) == "cap reached");
        assert(out.cutCue && out.fadeMs == 300 && out.hold);
        assert(out.holdUntil == at + core::kCapMs + core::kFadeMs + core::kResumeMs);
        assert(out.elapsedMs == core::kCapMs && s.fading);
        out = core::Tick(s, InputAt(at + core::kCapMs + 1, true, true));
        assert(!out.cutCue && !out.hold);         // one fade per cue

        // A new start cuts the old live pair and re-cuts combat music if the
        // hold had expired at the new Start (here it is still blocking).
        core::Start(s, at + core::kCapMs + 2000, 1.9f, 1, 0);
        const uint64_t next = s.pendingAt;
        out = core::Tick(s, InputAt(next, true, true));
        assert(out.startCue && out.cutOld && out.musicCut);
        assert(out.holdUntil == next + core::kCapMs + core::kResumeMs);
        assert(!s.fading && !s.haveVoice);       // ext a = b = -1 until CommitStart
        core::CommitStart(s, next, true);
        assert(s.haveVoice && s.t0 == next);

        // Zero volume still consumes the pending cue and holds the resume; no
        // voice is committed.
        core::State quiet;
        core::Start(quiet, 1000, 2.5f, 0, 0);
        core::Input in = InputAt(quiet.pendingAt, true, false, 0.0f);
        out = core::Tick(quiet, in);
        assert(out.startCue && out.noVolume && out.gain == 0.0f);
        core::CommitStart(quiet, in.nowMs, false);
        assert(!quiet.haveVoice && quiet.t0 == 0);
    }

    void TestMatchLeftAndFinished()
    {
        core::State s;
        const uint64_t at = PlayCue(s, 1000, 2.5f, 0);
        core::Output out = core::Tick(s, InputAt(at + 10, false, true));
        assert(out.cutCue && out.fadeMs == 300);
        assert(std::string(out.why) == "match left the playing state");
        assert(out.holdUntil == at + 10 + core::kFadeMs + core::kResumeMs);

        // The cap wins over the match-left reason at the same frame.
        core::State both;
        const uint64_t t = PlayCue(both, 1000, 2.5f, 0);
        out = core::Tick(both, InputAt(t + core::kCapMs, false, true));
        assert(std::string(out.why) == "cap reached");

        // The mixer finishing the cue clears the handles and holds only RESUME.
        core::State done;
        const uint64_t d = PlayCue(done, 1000, 2.5f, 0);
        out = core::Tick(done, InputAt(d + 20, true, false));
        assert(out.stopped && out.clearVoices && !out.cutCue);
        assert(out.holdUntil == d + 20 + core::kResumeMs);
        assert(!done.haveVoice && !done.fading);

        // Mixer-off is treated like a finished cue.
        core::State off;
        const uint64_t o = PlayCue(off, 1000, 2.5f, 0);
        out = core::Tick(off, InputAt(o + 5, true, false, 1.0f, false));
        assert(out.stopped && out.holdUntil == o + 5 + core::kResumeMs);
    }

    void TestFadeCancelsPending()
    {
        core::State s;
        core::Start(s, 1000, 2.5f, 0, 0);
        const core::FadeOutput pending = core::Fade(s, 1100);
        assert(!pending.acted && pending.holdUntil == 0 && s.pendingAt == 0);
        const core::Output late = core::Tick(s, InputAt(5000, true, false));
        assert(!late.startCue && !s.haveVoice);

        core::State live;
        const uint64_t at = PlayCue(live, 1000, 2.5f, 0);
        const core::FadeOutput f = core::Fade(live, at + 50);
        assert(f.acted && f.elapsedMs == 50);
        assert(f.holdUntil == at + 50 + core::kFadeMs + core::kResumeMs);
        assert(live.fading && !core::Fade(live, at + 60).acted);
    }

    void TestDuck()
    {
        core::Duck d;
        const bool live[2] = { true, true };
        const float raw[2][2] = { { 0.85f, 0.0f }, { 0.0f, 0.85f } };

        core::DuckOutput out = core::DuckStep(d, 0.35f, raw, live);
        assert(out.apply && d.captured && Near(out.gains[0][0], 0.85f * 0.35f));
        assert(Near(out.gains[0][1], 0.0) && Near(out.gains[1][0], 0.0));
        assert(Near(out.gains[1][1], 0.85f * 0.35f));

        // Later duck levels scale the CAPTURED base, not the scaled target.
        const float scaled[2][2] = { { 0.2975f, 0.0f }, { 0.0f, 0.2975f } };
        out = core::DuckStep(d, 0.5f, scaled, live);
        assert(out.apply && Near(out.gains[0][0], 0.425f));
        assert(Near(out.gains[1][1], 0.425f));

        out = core::DuckStep(d, 1.0f, scaled, live);
        assert(out.apply && Near(out.gains[0][0], 0.85f));
        assert(Near(out.gains[1][1], 0.85f) && !d.captured && d.value == 1.0f);
        assert(!core::DuckStep(d, 1.0f, raw, live).apply);

        const bool none[2] = { false, false };
        assert(!core::DuckStep(d, 0.35f, raw, none).apply && !d.captured);
    }

    void TestReset()
    {
        core::State s;
        const uint64_t at = PlayCue(s, 1000, 2.5f, 0);
        core::Fade(s, at + 10);
        const float raw[2][2] = { { 0.85f, 0.0f }, { 0.0f, 0.85f } };
        const bool live[2] = { true, true };
        (void)core::DuckStep(s.duck, 0.35f, raw, live);
        assert(s.duck.captured);
        core::Reset(s);
        assert(s.pendingAt == 0 && s.t0 == 0 && s.pick == -1 && s.seek == 0.0);
        assert(!s.pendingCut && !s.haveVoice && !s.fading);
        assert(!s.duck.captured && s.duck.value == 1.0f);
    }

} // namespace

int main()
{
    TestConstants();
    TestStartTiming();
    TestPendingStartOrder();
    TestPlayAndCap();
    TestMatchLeftAndFinished();
    TestFadeCancelsPending();
    TestDuck();
    TestReset();
    std::printf("EXEC_MUSIC_CORE_TEST_OK\n");
    return 0;
}
