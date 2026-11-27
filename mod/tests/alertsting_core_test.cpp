// Standalone harness for the pure alert-sting state machine (no game headers):
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -DTO1_SLOTS4=1 -I mod/src
//       -I mod/src/net/audio -I mod/src/net/protocol
//       mod/tests/alertsting_core_test.cpp -o /tmp/alertsting_core_test
//       && /tmp/alertsting_core_test

#include "alertsting-core.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>

namespace {

    namespace core = net::audio::alertsting::core;

    bool Near(float a, float b, float eps = 1e-5f)
    {
        return std::fabs(a - b) <= eps;
    }

    core::State CalmAt(uint64_t now)
    {
        core::State s;
        core::Tick(s, now, true, false, false);
        assert(s.calm == now);
        return s;
    }

    void TestThreatStamp()
    {
        core::State s = CalmAt(1000);

        // ext AlertstingCombat stamps g_als_threat_ms BEFORE every gate, so a rejected
        // event still starts the 15 s quiet window.
        assert(!core::Combat(s, 2000, false, false, false, "not live"));
        assert(s.threat == 2000);
        assert(s.due == 0);

        assert(!core::Combat(s, 3000, true, false, false, "shot / hit out of the quiet"));
        assert(s.threat == 3000);
        assert(s.due == 0);

        // Dead / downed also stamp, both at producer and at play time.
        assert(!core::Combat(s, 4000, true, true, false, "dead"));
        assert(s.threat == 4000);
        assert(!core::Combat(s, 5000, true, false, true, "downed"));
        assert(s.threat == 5000);
        assert(s.due == 0);
    }

    void TestQuietGates()
    {
        // The calm clock must be fresh (tier 0 within 1500 ms).
        core::State stale = CalmAt(1000);
        assert(!core::Combat(stale, 2501, true, false, false, "shot at"));
        assert(stale.due == 0);

        core::State fresh = CalmAt(1000);
        assert(core::Combat(fresh, 2500, true, false, false, "shot at"));
        assert(fresh.due == 2500 + core::kDelayMs);

        // An earlier threat inside ALS_QUIET_MS vetoes; at exactly 15 s it does not.
        core::State s = CalmAt(9000);
        s.threat = 1000;
        assert(!core::Combat(s, 1000 + core::kQuietMs - 1, true, false, false, "shot 1"));
        assert(s.due == 0);

        s = CalmAt(22000);
        s.threat = 7000;
        assert(core::Combat(s, 7000 + core::kQuietMs, true, false, false, "shot 2"));
        assert(s.due == 7000 + core::kQuietMs + core::kDelayMs);
    }

    void TestDelayedPlay()
    {
        core::State s = CalmAt(1000);
        assert(core::Combat(s, 2000, true, false, false, "near miss out of the quiet"));
        assert(s.due == 2100);

        core::Due due = core::Tick(s, 2099, false, false, false);
        assert(!due.fire);
        due = core::Tick(s, 2100, false, false, false);
        assert(due.fire);
        assert(due.why && std::string(due.why) == "near miss out of the quiet");
        assert(Near(due.level, 0.8f));
        assert(s.due == 0);                     // one shot only
        assert(!core::Tick(s, 2101, false, false, false).fire);

        // Dead / downed at the delayed tick cancel the cue without replaying it later.
        core::State cancelled = CalmAt(1000);
        assert(core::Combat(cancelled, 2000, true, false, false, "shot at"));
        assert(!core::Tick(cancelled, 2100, false, true, false).fire);
        assert(cancelled.due == 0);
        assert(!core::Tick(cancelled, 2101, false, false, false).fire);

        // Tier 0 continues to refresh the calm clock even while a cue is pending.
        core::State calm = CalmAt(1000);
        assert(core::Combat(calm, 2000, true, false, false, "shot at"));
        core::Tick(calm, 2050, true, false, false);
        assert(calm.calm == 2050);
    }

    void TestPlayGate()
    {
        core::State s;
        assert(core::CheckPlay(s, 1000, false, 0.8f) == core::PlayGate::kNotLive);
        assert(core::CheckPlay(s, 1000, true, 0.8f) == core::PlayGate::kOk);
        assert(core::CheckPlay(s, 1000, true, 0.0f) == core::PlayGate::kSilent);

        core::CommitPlay(s, 1000);
        assert(s.last == 1000);
        assert(core::CheckPlay(s, 1000 + core::kGapMs - 1, true, 0.8f) == core::PlayGate::kGap);
        assert(core::CheckPlay(s, 1000 + core::kGapMs, true, 0.8f) == core::PlayGate::kOk);

        core::Reset(s);
        assert(s.last == 0 && s.calm == 0 && s.threat == 0 && s.due == 0 && s.dueWhy == nullptr);
        assert(core::CheckPlay(s, 10, false, 0.8f) == core::PlayGate::kNotLive);
    }

} // namespace

int main()
{
    TestThreatStamp();
    TestQuietGates();
    TestDelayedPlay();
    TestPlayGate();
    std::printf("ALERTSTING_CORE_TEST_OK\n");
    return 0;
}
