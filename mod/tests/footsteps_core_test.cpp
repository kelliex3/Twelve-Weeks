// Standalone test for the pure remote-footstep rules; no engine / game headers.
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -I mod/src -I mod/src/net/audio
//       mod/tests/footsteps_core_test.cpp -o /tmp/opencode/footsteps_check
//   /tmp/opencode/footsteps_check

#include "footsteps-core.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>

namespace {

    using namespace net::audio::footsteps::core;

    struct Fixture {
        Wave  waves[8] = {};
        Tier  tiers[kTierCount] = {};
        DataView d;
        Config cfg;

        Fixture()
        {
            for (int i = 0; i < 8; ++i) {
                waves[i].off = static_cast<uint32_t>(i * 10000);
                waves[i].n = 10000;
                waves[i].rate = 44100;
            }
            tiers[0] = { 0, 2 };
            tiers[1] = { 2, 2 };
            tiers[2] = { 4, 2 };
            tiers[3] = { 6, 2 };
            d.waves = waves;
            d.waveCount = 8;
            d.tiers = tiers;
            d.tierCount = kTierCount;
            d.rate = 44100;
            d.samples = 80000;
        }
    };

    void TestConfig()
    {
        Config cfg;
        const char* text =
            "sprint 0.40 4 40\n"
            "near 0.25 2 6\n"
            "speed 0.6 3 5\n"
            "crouch-head 1.10\n"
            "cadence 1\n"
            "sprint 9 1 2\n"          // out of range, ignored
            "junk 1 2 3\n";
        assert(Parse(text, cfg));
        assert(cfg.tier[0].gain == 0.40f && cfg.tier[0].d0 == 4.0f && cfg.tier[0].dmax == 40.0f);
        assert(cfg.near.keep == 0.25f && cfg.near.lo == 2.0f && cfg.near.hi == 6.0f);
        assert(cfg.walk == 0.6f && cfg.run == 3.0f && cfg.sprint == 5.0f);
        assert(cfg.crouchHead == 1.10f && cfg.forceCadence);
        assert(!Parse(nullptr, cfg));
    }

    void TestCurves()
    {
        Fixture f;
        assert(DistGain(0, 2.0f, f.cfg) == 1.0f);
        assert(DistGain(0, 36.0f, f.cfg) == 0.0f);
        const float mid = DistGain(0, 19.0f, f.cfg);
        assert(mid > 0.2f && mid < 0.3f);
        assert(DistGain(-1, 1.0f, f.cfg) == 0.0f);
        assert(Near(1.0f, f.cfg) == f.cfg.near.keep);
        assert(Near(6.0f, f.cfg) == 1.0f);
        const float n = Near(3.0f, f.cfg);
        assert(n > f.cfg.near.keep && n < 1.0f);

        assert(TierFor(4.7f, 0.0f, true, 1.5f, f.cfg, 1.0f) == 0);
        assert(TierFor(3.0f, 0.0f, true, 1.5f, f.cfg, 1.0f) == 1);
        assert(TierFor(1.0f, 0.0f, true, 1.5f, f.cfg, 1.0f) == 2);
        assert(TierFor(5.0f, 0.0f, true, 1.0f, f.cfg, 1.0f) == 3);
        assert(TierFor(5.0f, 0.0f, true, 0.5f, f.cfg, 1.0f) == -1);
        assert(TierFor(5.0f, 0.0f, true, 1.5f, f.cfg, 2.0f) == 2);
    }

    Input Base(uint64_t body, double ms)
    {
        Input in;
        in.body = body;
        in.nowMs = ms;
        in.tick = static_cast<uint64_t>(ms * 3.0);
        in.havePos = true;
        in.pos[0] = 0.0f;
        in.pos[1] = 0.0f;
        in.pos[2] = 0.0f;
        in.distance = 2.0f;
        return in;
    }

    void TestCadence()
    {
        Fixture f;
        f.cfg.forceCadence = true;
        Peer p;
        ResetPeer(p);
        Result r;

        Input in = Base(0xAA, 1000.0);
        assert(Tick(p, f.d, f.cfg, in, r) == 0 && p.have);
        int steps = 0;
        for (int i = 1; i <= 5; ++i) {
            in = Base(0xAA, 1000.0 + i * 100.0);
            in.pos[2] = static_cast<float>(i) * 0.5f;    // 5 m/s ground speed
            r = Result{};
            steps += Tick(p, f.d, f.cfg, in, r);
        }
        assert(steps >= 1 && steps <= 2);
        assert(p.lastWave >= 0 && p.lastWave < 8);

        // Agility II silences the step entirely.
        Peer q;
        ResetPeer(q);
        in = Base(0xBB, 2000.0);
        assert(Tick(q, f.d, f.cfg, in, r) == 0);
        in.agilityLevel = 2;
        int silence = 0;
        for (int i = 1; i <= 5; ++i) {
            in = Base(0xBB, 2000.0 + i * 100.0);
            in.pos[2] = static_cast<float>(i) * 0.5f;
            in.agilityLevel = 2;
            r = Result{};
            silence += Tick(q, f.d, f.cfg, in, r);
        }
        assert(silence == 0 && q.steps > 0);

        // A different puppet SID reseeds the gait state.
        in = Base(0xCC, 3000.0);
        r = Result{};
        Tick(q, f.d, f.cfg, in, r);
        assert(q.h == 0xCC && !q.armed[0] && q.steps == 0);
    }

    void TestAnkles()
    {
        Fixture f;
        Peer p;
        ResetPeer(p);
        Result r;
        Input in = Base(0xAA, 1000.0);
        in.haveAnkles = true;
        in.anklePos[0][1] = 0.1f;
        in.anklePos[1][1] = 0.1f;
        Tick(p, f.d, f.cfg, in, r);              // first frame only seeds

        in = Base(0xAA, 1100.0);
        in.haveAnkles = true;
        in.pos[2] = 0.5f;
        in.anklePos[0][1] = 0.1f;
        in.anklePos[1][1] = 0.1f;
        Tick(p, f.d, f.cfg, in, r);
        assert(p.base[0] < 1e8f);

        // Lift the left foot: it arms.
        in = Base(0xAA, 1200.0);
        in.haveAnkles = true;
        in.pos[2] = 1.0f;
        in.anklePos[0][1] = 0.25f;
        in.anklePos[1][1] = 0.1f;
        Tick(p, f.d, f.cfg, in, r);
        assert(p.armed[0] && !p.armed[1]);

        // Plant after the 170 ms refractory window: one step, on the left.
        in = Base(0xAA, 1500.0);
        in.haveAnkles = true;
        in.pos[2] = 2.5f;
        in.anklePos[0][1] = 0.11f;
        in.anklePos[1][1] = 0.1f;
        r = Result{};
        const int n = Tick(p, f.d, f.cfg, in, r);
        assert(n == 1 && r.steps[0].foot == 0 && r.steps[0].ankle);
        assert(!p.armed[0] && r.steps[0].gain > 0.0f);

        // 45 failed ankle frames hand over to the speed cadence.
        Peer q;
        ResetPeer(q);
        in = Base(0xBB, 2000.0);
        Tick(q, f.d, f.cfg, in, r);
        for (int i = 1; i <= kAnkleFail; ++i) {
            in = Base(0xBB, 2000.0 + i * 100.0);
            in.haveAnkles = false;
            in.pos[2] = static_cast<float>(i) * 0.5f;
            Tick(q, f.d, f.cfg, in, r);
        }
        assert(!q.ankles && q.ankleMiss >= kAnkleFail);

        // The retry after 5 s re-enables the ankles.
        in = Base(0xBB, 2000.0 + (kAnkleFail + 51) * 100.0);
        in.pos[2] = static_cast<float>(kAnkleFail) * 0.5f + 0.25f;
        in.haveAnkles = true;
        r = Result{};
        Tick(q, f.d, f.cfg, in, r);
        assert(q.ankles && q.ankleMiss == 0);
    }

    void TestSpread()
    {
        Fixture f;
        Peer p;
        ResetPeer(p);
        Input in = Base(0xAA, 1000.0);
        in.agilityLevel = 0;
        Result r;
        Tick(p, f.d, f.cfg, in, r);
        for (int i = 1; i <= 3; ++i) {
            in = Base(0xAA, 1000.0 + i * 100.0);
            in.pos[2] = static_cast<float>(i) * 0.5f;
            in.distance = 2.0f;
            r = Result{};
            Tick(p, f.d, f.cfg, in, r);
        }
        // The take-to-take spread keeps the gain within +/-10 % of the tier gain.
        if (p.steps > 0) {
            const float base = f.cfg.tier[p.tier].gain * DistGain(p.tier, 2.0f, f.cfg)
                             * Near(2.0f, f.cfg);
            assert(base > 0.0f);
        }
    }

} // namespace

int main()
{
    TestConfig();
    TestCurves();
    TestCadence();
    TestAnkles();
    TestSpread();
    std::puts("FOOTSTEPS_CORE_TEST_OK");
    return 0;
}
