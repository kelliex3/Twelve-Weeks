// Standalone harness for the pure thrown-bomb rules (no game / engine headers):
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -I mod/src -I mod/src/net/audio
//       mod/tests/bomb_roll_core_test.cpp -o /tmp/bomb_roll_core_test
//       && /tmp/bomb_roll_core_test

#include "bomb-roll-core.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace {

    namespace core = net::audio::bomb_roll::core;
    using core::FrameResult;
    using core::Kind;
    using core::State;
    using core::WaveGroup;

    bool Near(float a, float b, float eps = 1e-3f)
    {
        return std::fabs(a - b) <= eps;
    }

    const core::Tracker* Find(const State& s, uint64_t h)
    {
        for (const core::Tracker& t : s.slots)
            if (t.h == h)
                return &t;
        return nullptr;
    }

    // Gain after the ext's uniform 0.7 impact factor.
    float G(float str) { return core::Gain(str) * core::kImpactGain; }

    void TestConstants()
    {
        static_assert(core::kRing == 12);
        static_assert(core::kMaster == 0.55f);
        static_assert(core::kStaleMs == 2500);
        static_assert(core::kDtMin == 0.003);
        static_assert(core::kDtMax == 0.2);
        static_assert(core::kMaxStepSq == 16.0f);
        static_assert(core::kSkipFrames == 2);
        static_assert(core::kImpactCdMs == 80);
        static_assert(core::kRollCdMs == 220);
        static_assert(core::kRollAfterImpactMs == 120);
        static_assert(core::kGroupCount == 10);
        static_assert(static_cast<int>(WaveGroup::SbHard) == 0);
        static_assert(static_cast<int>(WaveGroup::SbSoft) == 1);
        static_assert(static_cast<int>(WaveGroup::NbHard) == 2);
        static_assert(static_cast<int>(WaveGroup::NbMid) == 3);
        static_assert(static_cast<int>(WaveGroup::NbSoft) == 4);
        static_assert(static_cast<int>(WaveGroup::NbRattle) == 5);
        static_assert(static_cast<int>(WaveGroup::MoHit) == 6);
        static_assert(static_cast<int>(WaveGroup::SbRoll) == 7);
        static_assert(static_cast<int>(WaveGroup::NbRoll) == 8);
        static_assert(static_cast<int>(WaveGroup::MoRoll) == 9);
        static_assert(static_cast<int>(Kind::PipeBomb) == 1);
        static_assert(static_cast<int>(Kind::StunSmoke) == 2);
        static_assert(static_cast<int>(Kind::Molotov) == 3);
        assert(Near(core::kMinStrength, 0.9f));
        assert(Near(core::kMidStrength, 1.5f));
        assert(Near(core::kHardStrength, 4.0f));
        assert(Near(core::kRollStepM, 0.30f));
    }

    void TestGainRangeEarshot()
    {
        assert(Near(core::Gain(0.0f), 0.12f));
        assert(Near(core::Gain(9.0f), 1.0f));
        assert(Near(core::Gain(18.0f), 1.0f));        // min(str / 9, 1)
        assert(Near(core::Gain(4.5f),
                    0.12f + 0.88f * std::pow(0.5f, 0.7f)));
        assert(core::Gain(8.0f) > core::Gain(4.0f));

        assert(core::Rnear(8.0f) == 1.0f);
        assert(core::Rnear(10.0f) == 1.0f);
        assert(core::Rnear(10.01f) == 2.0f);
        assert(core::Rnear(24.0f) == 2.0f);

        const float listener[3] = { 0.0f, 0.0f, 0.0f };
        const float inside[3] = { 13.0f, 0.0f, 0.0f };   // exactly rfar + 3
        const float outside[3] = { 13.01f, 0.0f, 0.0f };
        assert(!core::OutOfEarshot(inside, listener, 10.0f));
        assert(core::OutOfEarshot(outside, listener, 10.0f));
        const float above[3] = { 0.0f, 11.1f, 0.0f };   // rfar 8 -> (8+3)^2
        assert(core::OutOfEarshot(above, listener, 8.0f));
        assert(!core::OutOfEarshot(above, nullptr, 8.0f));  // no locator = no drop
    }

    void TestRngPickAndPitch()
    {
        uint64_t s = 0;
        const uint32_t first = core::NextRand(s, 1234);
        assert(s != 0);
        uint64_t expected = (1234ULL * 0x9E3779B97F4A7C15ULL) | 1ULL;
        expected ^= expected << 13;
        expected ^= expected >> 7;
        expected ^= expected << 17;
        assert(first == static_cast<uint32_t>(expected >> 33));
        uint64_t copy = s;
        assert(core::NextRand(copy, 9999) == core::NextRand(s, 9999));
        assert(copy == s);                               // nowMs only seeds state == 0

        const uint64_t seed = 0x123456789abcdefULL;
        uint64_t x = seed;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        uint64_t st = seed;
        assert(core::NextRand(st, 777) == static_cast<uint32_t>(x >> 33));
        assert(st == x);

        State p;
        core::SetWaveCount(p, WaveGroup::NbHard, 2);
        int prev = -1;
        for (int i = 0; i < 40; ++i) {
            const int w = core::PickWave(p, WaveGroup::NbHard, 2, 1000 + i);
            assert(w == 0 || w == 1);
            assert(w != prev);                           // ext's g_br_last bypass
            prev = w;
        }
        const uint64_t before = p.rng;
        assert(core::PickWave(p, WaveGroup::MoHit, 0, 1000) == -1);
        assert(p.rng == before);                         // an empty group consumes no RNG
        assert(p.waveCount[static_cast<int>(WaveGroup::NbHard)] == 2);

        // Exact pitch construction / RNG order: one draw, +-6 %, truncated to int.
        State pitch;
        uint64_t probe = 0;
        const uint32_t jitterRand = core::NextRand(probe, 1000); // the one draw PitchRate uses
        const int rate = core::PitchRate(pitch, 44100, 1000);
        const float factor = 0.94f
            + 0.12f * static_cast<float>(jitterRand & 1023u) / 1023.0f;
        assert(rate == static_cast<int>(44100.0f * factor));
        assert(rate >= static_cast<int>(44100.0f * 0.94f));
        assert(rate <= static_cast<int>(44100.0f * 1.06f));
        assert(pitch.rng == probe);
    }

    void TestFirstSeedAndClocks()
    {
        State s;
        const float p0[3] = { 1.0f, 2.0f, 3.0f };
        FrameResult r = core::Feed(s, 0xA1ULL, static_cast<int>(Kind::StunSmoke),
                                   -1, p0, 1000, 0.05);
        assert(r.type == FrameResult::Type::None && r.n == 0);
        const core::Tracker* t = Find(s, 0xA1ULL);
        assert(t && t->seenMs == 1000 && t->kind == static_cast<int>(Kind::StunSmoke)
               && t->slot == -1 && t->nimp == 0 && t->skip == 0);
        assert(Near(t->last[0], 1.0f) && Near(t->last[1], 2.0f) && Near(t->last[2], 3.0f));

        // dt == 0.003 (exclusive) is a stall: skip two frames and zero the velocity.
        const float stalled[3] = { 1.0f, 2.0f, 3.2f };
        r = core::Feed(s, 0xA1ULL, static_cast<int>(Kind::StunSmoke),
                       -1, stalled, 1100, core::kDtMin);
        assert(r.type == FrameResult::Type::None);
        t = Find(s, 0xA1ULL);
        assert(t->skip == 2 && t->v[0] == 0.0f && t->v[1] == 0.0f && t->v[2] == 0.0f);
        assert(Near(t->last[2], 3.2f));

        const float f1[3] = { 1.0f, 2.0f, 3.4f };       // 4 m/s
        assert(core::Feed(s, 0xA1ULL, 2, -1, f1, 1150, 0.05).type
               == FrameResult::Type::None);
        t = Find(s, 0xA1ULL);
        assert(t->skip == 1 && Near(t->v[2], 4.0f));
        const float f2[3] = { 1.0f, 2.0f, 3.6f };
        assert(core::Feed(s, 0xA1ULL, 2, -1, f2, 1200, 0.05).type
               == FrameResult::Type::None);
        t = Find(s, 0xA1ULL);
        assert(t->skip == 0 && Near(t->v[2], 4.0f));

        // A > 4 m step is a re-place, not a contact.
        const float jumped[3] = { 20.0f, 2.0f, 3.6f };
        assert(core::Feed(s, 0xA1ULL, 2, -1, jumped, 1300, 0.05).type
               == FrameResult::Type::None);
        t = Find(s, 0xA1ULL);
        assert(t->skip == 2 && t->v[0] == 0.0f);

        core::Steer(s, 0xA1ULL);
        assert(Find(s, 0xA1ULL)->skip == 2);
        core::Steer(s, 0xDEADULL);                       // unknown handle is harmless
        assert(Find(s, 0xA1ULL)->skip == 2);

        const float bad[3] = { std::nanf(""), 0.0f, 0.0f };
        const uint64_t rng = s.rng;
        r = core::Feed(s, 0xBEEFULL, 2, -1, bad, 1400, 0.05);
        assert(r.type == FrameResult::Type::None && !Find(s, 0xBEEFULL));
        assert(s.rng == rng);
        assert(core::Feed(s, 0ULL, 2, -1, p0, 1400, 0.05).type
               == FrameResult::Type::None);
        assert(core::Feed(s, 0xBEEFULL, 0, -1, p0, 1400, 0.05).type
               == FrameResult::Type::None);
    }

    void TestRingEviction()
    {
        State s;
        for (int i = 0; i < core::kRing; ++i) {
            const float p[3] = { static_cast<float>(i), 0.0f, 0.0f };
            core::Feed(s, 100ULL + static_cast<uint64_t>(i), 1, i, p,
                       1000 + static_cast<uint64_t>(i), 0.0);
        }
        for (int i = 0; i < core::kRing; ++i)
            assert(Find(s, 100ULL + static_cast<uint64_t>(i)));

        // No free / stale slot yet: the least recently seen one is reused.
        const float p[3] = { 0.0f, 0.0f, 0.0f };
        core::Feed(s, 200ULL, 1, -1, p, 3000, 0.0);     // 3000 - 1000 < 2500
        assert(!Find(s, 100ULL) && Find(s, 200ULL));
        assert(Find(s, 200ULL) == &s.slots[0]);

        // Past 2500 ms every slot is stale: the first one is reused.
        State stale;
        for (int i = 0; i < core::kRing; ++i)
            core::Feed(stale, 300ULL + static_cast<uint64_t>(i), 2, i, p, 1000, 0.0);
        core::Feed(stale, 400ULL, 2, -1, p, 3600, 0.0);
        assert(Find(stale, 400ULL) == &stale.slots[0]);

        // An exact handle still wins even when its record is stale.
        State exact;
        core::Feed(exact, 99ULL, 3, 4, p, 1000, 0.0);
        core::Feed(exact, 99ULL, 3, 4, p, 4000, 0.05);
        assert(Find(exact, 99ULL) == &exact.slots[0]);
        assert(Find(exact, 99ULL)->seenMs == 4000);
        core::Forget(exact, 99ULL);
        assert(!Find(exact, 99ULL));

        core::Reset(s);
        for (const core::Tracker& t : s.slots)
            assert(t.h == 0 && t.skip == 0 && t.armed == 0 && t.nimp == 0);
        assert(s.rng == 0 && s.last[0] == 0);
    }

    // Seed a bomb with a -2 m/s fall, then reverse / stop it by `dvy` on the next
    // judged frame. That is exactly the ext's floor-contact verdict.
    FrameResult FloorImpact(State& s, int kind, float dvy, uint64_t now = 1000)
    {
        const uint64_t h = 0xB0B0ULL;
        const float p0[3] = { 0.0f, 0.0f, 0.0f };
        core::Feed(s, h, kind, -1, p0, now, 0.05);
        const float p1[3] = { 0.0f, -0.1f, 0.0f };
        assert(core::Feed(s, h, kind, -1, p1, now + 50, 0.05).type
               == FrameResult::Type::None);
        const float p2[3] = { 0.0f, -0.1f + (dvy - 2.0f) * 0.05f, 0.0f };
        return core::Feed(s, h, kind, -1, p2, now + 100, 0.05);
    }

    struct Expect { WaveGroup group; float mul; float rfar; bool primary; };

    void CheckImpact(int kind, float dvy, const Expect* exp, int n)
    {
        State s;
        s.baseRate = 44100;
        FrameResult r = FloorImpact(s, kind, dvy);
        assert(r.type == FrameResult::Type::Impact);
        assert(r.n == n);
        assert(Near(r.strength, dvy));
        const core::Tracker* t = Find(s, 0xB0B0ULL);
        assert(t && t->armed == 1 && t->nimp == 1 && t->impMs == 1100
               && t->rollD == 0.0f);
        for (int i = 0; i < n; ++i) {
            assert(r.cues[i].group == exp[i].group);
            assert(Near(r.cues[i].gain, G(dvy) * exp[i].mul));
            assert(Near(r.cues[i].rfar, exp[i].rfar));
            assert(r.cues[i].primary == exp[i].primary);
            assert(r.cues[i].wave >= 0);
            assert(r.cues[i].rate > 0);
            assert(r.cues[i].pos[0] == 0.0f);
        }
    }

    void TestFloorAndCueMatrix()
    {
        const Expect sbHard[] = { { WaveGroup::SbHard, 1.3f, 24.0f, true } };
        const Expect sbMid[] = { { WaveGroup::SbSoft, 1.5f, 16.0f, true },
                                 { WaveGroup::SbHard, 0.35f, 16.0f, false } };
        const Expect sbSoft[] = { { WaveGroup::SbSoft, 1.1f, 10.0f, true } };
        const Expect nbHard[] = { { WaveGroup::NbHard, 1.0f, 24.0f, true },
                                  { WaveGroup::NbRattle, 0.45f, 16.0f, false } };
        const Expect nbMid[] = { { WaveGroup::NbMid, 1.1f, 16.0f, true },
                                 { WaveGroup::NbRattle, 0.3f, 11.0f, false } };
        const Expect nbSoft[] = { { WaveGroup::NbSoft, 1.3f, 16.0f, true },
                                  { WaveGroup::NbRattle, 0.2f, 8.0f, false } };
        const Expect moHard[] = { { WaveGroup::MoHit, 1.2f, 24.0f, true } };
        const Expect moMid[] = { { WaveGroup::MoHit, 1.2f, 16.0f, true } };
        const Expect moSoft[] = { { WaveGroup::MoHit, 1.2f, 10.0f, true } };

        CheckImpact(static_cast<int>(Kind::StunSmoke), 4.0f, sbHard, 1);
        CheckImpact(static_cast<int>(Kind::StunSmoke), 2.0f, sbMid, 2);
        CheckImpact(static_cast<int>(Kind::StunSmoke), 1.2f, sbSoft, 1);
        CheckImpact(static_cast<int>(Kind::PipeBomb), 4.0f, nbHard, 2);
        CheckImpact(static_cast<int>(Kind::PipeBomb), 2.0f, nbMid, 2);
        CheckImpact(static_cast<int>(Kind::PipeBomb), 1.2f, nbSoft, 2);
        CheckImpact(static_cast<int>(Kind::Molotov), 4.0f, moHard, 1);
        CheckImpact(static_cast<int>(Kind::Molotov), 2.0f, moMid, 1);
        CheckImpact(static_cast<int>(Kind::Molotov), 1.2f, moSoft, 1);

        // A 0.8 m/s reversal is below the 0.9 m/s minimum: no impact.
        State weak;
        assert(FloorImpact(weak, static_cast<int>(Kind::PipeBomb), 0.8f).type
               == FrameResult::Type::None);
        assert(Find(weak, 0xB0B0ULL)->nimp == 0);
    }

    void TestSideImpact()
    {
        const uint64_t h = 0xC1C1ULL;
        State s;
        const float p0[3] = { 0.0f, 0.0f, 0.0f };
        core::Feed(s, h, static_cast<int>(Kind::PipeBomb), -1, p0, 1000, 0.05);
        const float p1[3] = { 0.0f, 0.0f, 0.15f };       // hp = 3 m/s
        assert(core::Feed(s, h, 1, -1, p1, 1050, 0.05).type == FrameResult::Type::None);
        const float p2[3] = { 0.30f, 0.0f, 0.30f };      // v2 = (6, 0, 3): dvh = 6 -> str 5.4
        const FrameResult r = core::Feed(s, h, 1, -1, p2, 1100, 0.05);
        assert(r.type == FrameResult::Type::Impact);
        assert(Near(r.strength, 5.4f));
        assert(r.n == 2 && r.cues[0].group == WaveGroup::NbHard
               && r.cues[1].group == WaveGroup::NbRattle);

        // hp <= 2.5 vetoes the side verdict even with a large velocity change.
        State slow;
        core::Feed(slow, h, 1, -1, p0, 1000, 0.05);
        const float q1[3] = { 0.0f, 0.0f, 0.10f };       // hp = 2 m/s
        core::Feed(slow, h, 1, -1, q1, 1050, 0.05);
        assert(core::Feed(slow, h, 1, -1, p2, 1100, 0.05).type
               == FrameResult::Type::None);
        assert(Find(slow, h)->nimp == 0);
    }

    void TestImpactCooldown()
    {
        State s;
        const uint64_t h = 0xB0B0ULL;
        assert(FloorImpact(s, static_cast<int>(Kind::StunSmoke), 2.0f).type
               == FrameResult::Type::Impact);
        assert(Find(s, h)->impMs == 1100);

        // The impact frame ended at y = -0.1 with v = 0.
        const float down[3] = { 0.0f, -0.12f, 0.0f };    // 10 ms -> v = -2
        const float back[3] = { 0.0f, -0.11f, 0.0f };    // 10 ms -> v = +1
        assert(core::Feed(s, h, 2, -1, down, 1110, 0.01).type
               == FrameResult::Type::None);
        assert(core::Feed(s, h, 2, -1, back, 1120, 0.01).type
               == FrameResult::Type::None);             // 20 ms after the first impact
        assert(Find(s, h)->nimp == 1 && Find(s, h)->impMs == 1100);

        const float down2[3] = { 0.0f, -0.23f, 0.0f };   // 60 ms -> v = -2
        const float back2[3] = { 0.0f, -0.205f, 0.0f };  // 10 ms -> v = +2.5
        assert(core::Feed(s, h, 2, -1, down2, 1180, 0.06).type
               == FrameResult::Type::None);
        const FrameResult r = core::Feed(s, h, 2, -1, back2, 1190, 0.01);
        assert(r.type == FrameResult::Type::Impact);    // 90 ms >= 80 ms
        assert(Find(s, h)->nimp == 2 && Find(s, h)->impMs == 1190);
    }

    void TestRollRules()
    {
        const uint64_t h = 0xB0B0ULL;
        State s;
        assert(FloorImpact(s, static_cast<int>(Kind::StunSmoke), 2.0f).type
               == FrameResult::Type::Impact);

        // 3 m/s along X, vy == 0: 0.15 m per 50 ms frame. The impact frame ended
        // at y = -0.1, so the roll frames must stay on that ground plane.
        const float p1[3] = { 0.15f, -0.1f, 0.0f };
        assert(core::Feed(s, h, 2, -1, p1, 1150, 0.05).type == FrameResult::Type::None);
        assert(Near(Find(s, h)->rollD, 0.15f));
        const float p2[3] = { 0.30f, -0.1f, 0.0f };
        assert(core::Feed(s, h, 2, -1, p2, 1200, 0.05).type == FrameResult::Type::None);
        assert(Near(Find(s, h)->rollD, 0.30f));          // only 100 ms after impact

        const float p3[3] = { 0.45f, -0.1f, 0.0f };
        const FrameResult r = core::Feed(s, h, 2, -1, p3, 1250, 0.05);
        assert(r.type == FrameResult::Type::Roll && r.n == 1);
        assert(r.cues[0].group == WaveGroup::SbRoll);
        assert(Near(r.cues[0].gain, (0.05f + 0.25f * 0.75f) * 0.9f));
        assert(Near(r.cues[0].rfar, 8.0f) && r.cues[0].primary);
        assert(Find(s, h)->rollMs == 1250 && Find(s, h)->rollD == 0.0f);

        // 220 ms between grains: 1450 is early, 1500 is due.
        const float p4[3] = { 0.60f, -0.1f, 0.0f };
        const float p5[3] = { 0.75f, -0.1f, 0.0f };
        const float p6[3] = { 0.90f, -0.1f, 0.0f };
        const float p7[3] = { 1.05f, -0.1f, 0.0f };
        assert(core::Feed(s, h, 2, -1, p4, 1300, 0.05).type == FrameResult::Type::None);
        assert(core::Feed(s, h, 2, -1, p5, 1350, 0.05).type == FrameResult::Type::None);
        assert(core::Feed(s, h, 2, -1, p6, 1400, 0.05).type == FrameResult::Type::None);
        assert(core::Feed(s, h, 2, -1, p7, 1450, 0.05).type == FrameResult::Type::None);
        const float p8[3] = { 1.20f, -0.1f, 0.0f };
        assert(core::Feed(s, h, 2, -1, p8, 1500, 0.05).type == FrameResult::Type::Roll);
        assert(Find(s, h)->rollMs == 1500);

        // The other two kinds pick their own roll group.
        State nb;
        assert(FloorImpact(nb, static_cast<int>(Kind::PipeBomb), 2.0f).type
               == FrameResult::Type::Impact);
        const float n1[3] = { 0.15f, -0.1f, 0.0f };
        const float n2[3] = { 0.30f, -0.1f, 0.0f };
        const float n3[3] = { 0.45f, -0.1f, 0.0f };
        core::Feed(nb, h, 1, -1, n1, 1150, 0.05);
        core::Feed(nb, h, 1, -1, n2, 1200, 0.05);
        const FrameResult nr = core::Feed(nb, h, 1, -1, n3, 1250, 0.05);
        assert(nr.type == FrameResult::Type::Roll
               && nr.cues[0].group == WaveGroup::NbRoll);

        State mo;
        assert(FloorImpact(mo, static_cast<int>(Kind::Molotov), 2.0f).type
               == FrameResult::Type::Impact);
        core::Feed(mo, h, 3, -1, n1, 1150, 0.05);
        core::Feed(mo, h, 3, -1, n2, 1200, 0.05);
        const FrameResult mr = core::Feed(mo, h, 3, -1, n3, 1250, 0.05);
        assert(mr.type == FrameResult::Type::Roll
               && mr.cues[0].group == WaveGroup::MoRoll);

        // Too slow / too fast / airborne never accumulates roll distance.
        State slow;
        assert(FloorImpact(slow, static_cast<int>(Kind::StunSmoke), 2.0f).type
               == FrameResult::Type::Impact);
        const float tiny[3] = { 0.02f, -0.1f, 0.0f };    // hs = 0.4 < 0.45
        core::Feed(slow, h, 2, -1, tiny, 1150, 0.05);
        assert(Find(slow, h)->rollD == 0.0f);
        const float airborne[3] = { 0.15f, 0.0f, 0.0f }; // left the ground: vy = 2
        core::Feed(slow, h, 2, -1, airborne, 1200, 0.05);
        assert(Find(slow, h)->rollD == 0.0f);
    }

} // namespace

int main()
{
    TestConstants();
    TestGainRangeEarshot();
    TestRngPickAndPitch();
    TestFirstSeedAndClocks();
    TestRingEviction();
    TestFloorAndCueMatrix();
    TestSideImpact();
    TestImpactCooldown();
    TestRollRules();
    std::printf("BOMB_ROLL_CORE_TEST_OK\n");
    return 0;
}
