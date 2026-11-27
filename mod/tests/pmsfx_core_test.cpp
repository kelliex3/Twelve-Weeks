// Standalone test for the pure post-match score-sound rules; no engine, PCM,
// GUI or private-data headers.
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -I mod/src
//       mod/tests/pmsfx_core_test.cpp -o /tmp/opencode/pmsfx_check
//   /tmp/opencode/pmsfx_check

#include "net/audio/pmsfx-core.hpp"

#include <cassert>
#include <cstdio>

namespace {

    using namespace net::audio::pmsfx::core;

    int Find(const Result& r, CueKind kind, int cue = -1)
    {
        for (int i = 0; i < r.count; ++i)
            if (r.cues[i].kind == kind && (cue < 0 || r.cues[i].cue == cue))
                return i;
        return -1;
    }

    void TestDurationAndSamples()
    {
        assert(CountDur(0) == 700);
        assert(CountDur(20) == 700);
        assert(CountDur(100) == 1500);
        assert(CountDur(200) == 1800);   // ext's 1.8 s cap
        assert(CountDur(-50) == 700);

        assert(CountSamples(1500, 48000, 100000) == 76800);
        // ext clamps to the wave first, then to the 100 ms minimum (which wins
        // for a wave shorter than the minimum; the real wave is much longer).
        assert(CountSamples(1500, 48000, 1000) == 4800);
        assert(CountSamples(0, 48000, 100000) == 4800);
        assert(CountSamples(-100, 48000, 100000) == 4800);
        assert(TailFrames(72150, 48000) == 2880);
        assert(TailFrames(1000, 48000) == 1000);
    }

    void TestFlow()
    {
        State s;
        Result r;
        Enter(s, r);
        assert(s.armed && !s.finished);
        assert(Find(r, CueKind::kPlay, static_cast<int>(Kind::kEnter)) >= 0);

        // The count starts exactly once, only in the window, only when ready.
        r = Result{};
        Tick(s, r, false, 2300, 1200);
        assert(r.count == 0 && !s.countStarted);
        Tick(s, r, true, 2199, 1200);
        assert(r.count == 0 && !s.countStarted);
        Tick(s, r, true, 3400, 1200);
        assert(r.count == 0 && !s.countStarted);
        Tick(s, r, true, 2300, 1200);
        int i = Find(r, CueKind::kCountStart);
        assert(i >= 0 && r.cues[i].ms == 1200 && s.countStarted);
        r = Result{};
        Tick(s, r, true, 2500, 1200);
        assert(r.count == 0);

        // The land cuts the loop and plays the accent + stamp once.
        Finish(s, r);
        assert(s.finished);
        i = Find(r, CueKind::kCountCut);
        assert(i >= 0 && r.cues[i].ms == 60);
        assert(Find(r, CueKind::kPlay, static_cast<int>(Kind::kEnd)) >= 0);
        assert(Find(r, CueKind::kPlay, static_cast<int>(Kind::kStamp)) >= 0);
        const int n = r.count;
        r = Result{};
        Finish(s, r);
        assert(r.count == 0);
        (void)n;

        r = Result{};
        Page(r);
        assert(Find(r, CueKind::kPlay, static_cast<int>(Kind::kPage)) >= 0);
    }

    void TestSkip()
    {
        State s;
        Result r;
        Enter(s, r);
        r = Result{};
        Skip(s, r);
        int i = Find(r, CueKind::kCountCut);
        assert(i >= 0 && r.cues[i].ms == 50 && s.skipped);
        Tick(s, r, true, 2300, 1200);
        assert(Find(r, CueKind::kCountStart) < 0);
        r = Result{};
        Skip(s, r);
        assert(r.count == 0);
        Finish(s, r);
        assert(Find(r, CueKind::kPlay, static_cast<int>(Kind::kStamp)) >= 0);
    }

    void TestReset()
    {
        State s;
        Result r;
        Enter(s, r);
        Tick(s, r, true, 2300, 1200);
        Finish(s, r);
        Reset(s);
        assert(!s.armed && !s.countStarted && !s.skipped && !s.finished);
        r = Result{};
        Tick(s, r, true, 2300, 1200);
        assert(r.count == 0);
        Enter(s, r);
        assert(Find(r, CueKind::kPlay, static_cast<int>(Kind::kEnter)) >= 0);
    }

} // namespace

int main()
{
    TestDurationAndSamples();
    TestFlow();
    TestSkip();
    TestReset();
    std::puts("pmsfx core: ok");
    return 0;
}
