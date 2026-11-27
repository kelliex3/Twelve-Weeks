// Standalone harness for the pure near-miss geometry / wire helpers (no game headers):
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -DTO1_SLOTS4=1 -I mod/src
//       -I mod/src/net/audio -I mod/src/net/protocol
//       mod/tests/near_miss_core_test.cpp -o /tmp/near_miss_core_test
//       && /tmp/near_miss_core_test

#include "near-miss.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>

namespace {

    bool Near(float a, float b, float eps = 1e-4f)
    {
        return std::fabs(a - b) <= eps;
    }

    void TestClosestApproach()
    {
        using namespace net::audio::near_miss;

        // A line along +X passing 2 m to the side of our head.
        const float o[3] = { 0.0f, 0.0f, 0.0f };
        const float d[3] = { 1.0f, 0.0f, 0.0f };
        const float head[3] = { 10.0f, 2.0f, 0.0f };
        ClosestApproach a = ClosestPointOnSegment(o, d, 120.0f, head);
        assert(Near(a.t, 10.0f));
        assert(Near(a.lat, 2.0f));
        assert(Near(a.cp[0], 10.0f) && Near(a.cp[1], 0.0f));
        assert(ShotIsNear(a));

        // Behind / at the muzzle (t < 1) is rejected even though the lateral is small.
        const float behind[3] = { 0.5f, 2.0f, 0.0f };
        a = ClosestPointOnSegment(o, d, 120.0f, behind);
        assert(Near(a.t, 0.5f) && Near(a.lat, 2.0f));
        assert(!ShotIsNear(a));

        // Too far to the side (> 2.5 m) is rejected.
        const float wide[3] = { 10.0f, 3.0f, 0.0f };
        a = ClosestPointOnSegment(o, d, 120.0f, wide);
        assert(Near(a.lat, 3.0f));
        assert(!ShotIsNear(a));

        // The segment's upper clamp: a known short line stops at len, so the closest point
        // is the end and the lateral grows to the rest of the distance.
        a = ClosestPointOnSegment(o, d, 5.0f, head);
        assert(Near(a.t, 5.0f));
        assert(Near(a.cp[0], 5.0f));
        assert(Near(a.lat, std::sqrt(25.0f + 4.0f)));
        assert(!ShotIsNear(a));

        // Diagonal line, head off in all three axes.
        const float d2[3] = { 0.6f, 0.8f, 0.0f };          // unit length
        const float head2[3] = { 6.0f, 8.0f, 1.5f };      // t = 10, lateral = 1.5
        a = ClosestPointOnSegment(o, d2, 120.0f, head2);
        assert(Near(a.t, 10.0f));
        assert(Near(a.lat, 1.5f));
        assert(ShotIsNear(a));
    }

    void TestWire()
    {
        using namespace net::audio::near_miss;

        const float vals[] = { -1.0f, -0.75f, -0.5f, 0.0f, 0.5f, 0.75f, 1.0f };
        for (float v : vals) {
            const int8_t q = QuantDir(v);
            assert(Near(DequantDir(q), v, 1.0f / 127.0f + 1e-6f));
        }
        assert(QuantDir(-2.0f) == -127);
        assert(QuantDir(2.0f) == 127);

        const float good[3] = { 0.6f, 0.8f, 0.0f };       // |d|^2 == 1
        assert(DirValid(good));
        const float zero[3] = { 0.0f, 0.0f, 0.0f };
        assert(!DirValid(zero));
        const float huge[3] = { 1.0f, 1.0f, 1.0f };       // |d|^2 == 3
        assert(!DirValid(huge));

        assert(sizeof(StunnedEvent) == 1);
        assert(sizeof(ShotRayEvent) == 17);
    }

    void TestGain()
    {
        using namespace net::audio::near_miss;
        assert(Near(WhizzGain(0.25f, 5.0f), 0.95f));
        assert(Near(WhizzGain(2.5f, 5.0f), 0.95f * 0.45f));
        assert(Near(WhizzGain(2.5f, 140.0f), 0.0f)); // roll-off reaches zero at 140 m
        assert(WhizzGain(0.1f, 5.0f) > WhizzGain(2.0f, 5.0f));
        // The ext does not floor the tail past 140 m; keep that divergence visible.
        assert(WhizzGain(2.5f, 200.0f) < 0.0f);
    }

    void TestClipAndAim()
    {
        using namespace net::audio::near_miss;
        using namespace net::audio::near_miss::core;

        const uint64_t w = 0x1234, other = 0x5678, now = 5000;
        assert(!ClipDecrementShot(-1, w, w, 5, now, now));       // first clip sight
        assert(!ClipDecrementShot(6, w, other, 5, now, now));    // weapon swap
        assert(!ClipDecrementShot(6, w, w, 5, 0, now));          // no fire stamp
        assert(!ClipDecrementShot(6, w, w, 5, now - kFireStampMs, now));
        assert(ClipDecrementShot(6, w, w, 5, now - kFireStampMs + 1, now));
        assert(!ClipDecrementShot(5, w, w, 5, now, now));        // no decrease
        assert(!ClipDecrementShot(6, w, w, 7, now, now));        // reload

        const float o[3] = { 1.0f, 2.0f, 3.0f };
        const float unit[3] = { 0.6f, 0.8f, 0.0f };
        assert(AimRayValid(o, unit));
        const float small[3] = { 0.4f, 0.0f, 0.0f };            // |d|^2 = 0.16
        const float big[3] = { 2.5f, 0.0f, 0.0f };              // |d|^2 = 6.25
        assert(!AimRayValid(o, small) && !AimRayValid(o, big));
        const float nan[3] = { std::nanf(""), 0.0f, 0.0f };
        assert(!AimRayValid(o, nan));

        float slid[3] = { 0.0f, 0.0f, 0.0f };
        const float along[3] = { 1.0f, 0.0f, 0.0f };
        const float chest[3] = { 3.0f, 2.0f, 0.0f };
        SlideOriginToChest(slid, along, chest);
        assert(Near(slid[0], 3.0f) && Near(slid[1], 0.0f));
        float far[3] = { 0.0f, 0.0f, 0.0f };
        const float farChest[3] = { 20.0f, 2.0f, 0.0f };
        SlideOriginToChest(far, along, farChest);
        assert(Near(far[0], kAimSlideMax));
        float behind[3] = { 0.0f, 0.0f, 0.0f };
        const float behindChest[3] = { -5.0f, 2.0f, 0.0f };
        SlideOriginToChest(behind, along, behindChest);
        assert(Near(behind[0], 0.0f));
    }

    void TestRingAndPending()
    {
        using namespace net::audio::near_miss::core;

        State s;
        const float o[3] = { 1.0f, 2.0f, 3.0f };
        const float d[3] = { 1.0f, 0.0f, 0.0f };
        for (uint32_t i = 1; i <= 20; ++i)
            StoreShot(s, 1, 1000 + i, o, d, 120.0f);
        assert(RingEnd(s) == 20);

        // Records are keyed by their interlocked sequence, not by their slot index:
        // sequence 20 reused slot 4, so sequence 4 is no longer live in that slot.
        assert(At(s, 20).seq.load() == 20 && At(s, 4).seq.load() == 20);
        RewindSeen(s, RingEnd(s));
        assert(s.seen == 20 - kRing);         // only the last 16 are still readable
        assert(!RingLive(s, 4));
        for (uint32_t q = 5; q <= 20; ++q)
            assert(RingLive(s, q));
        assert(At(s, 20).ms == 1020 && At(s, 20).slot == 1);
        assert(At(s, 20).len == 120.0f);
        AdvanceSeen(s, RingEnd(s));
        assert(s.seen == 20);

        // ext's rewind: a gap of more than SRAY_RING snaps seen to end - 16.
        RewindSeen(s, 20 + kRing + 3);
        assert(s.seen == 23);
        AdvanceSeen(s, RingEnd(s));

        // The uint32 slot overload stores a peer slot verbatim.
        StoreShot(s, static_cast<uint32_t>(kMaxNetPlayers - 1), 2000, o, d, 60.0f);
        assert(RingEnd(s) == 21
               && At(s, 21).slot == static_cast<uint32_t>(kMaxNetPlayers - 1));
        RewindSeen(s, RingEnd(s));
        assert(RingLive(s, 21));
        AdvanceSeen(s, RingEnd(s));

        ShotRecord r{};
        r.slot = 1;
        r.ms = 1000;
        for (int j = 0; j < 3; ++j) { r.o[j] = o[j]; r.d[j] = d[j]; }
        r.len = 120.0f;
        assert(QueueReady(s, 1100, r, true));
        assert(!QueueReady(s, 1100, r, false));  // teammate / local producer
        assert(!QueueReady(s, 1000 + kFreshMs + 1, r, true));  // stale line

        const float cp[3] = { 10.0f, 2.0f, 3.0f };
        const ClosestApproach a{ 10.0f, 1.5f, { cp[0], cp[1], cp[2] } };
        Queue(s, r, a, 1100);
        assert(s.nPending == 1);
        assert(s.pending[0].due == 1100 + kDelayMs);
        assert(s.pending[0].shot == r.ms && s.pending[0].slot == 1);
        assert(Near(s.pending[0].lat, 1.5f) && Near(s.pending[0].along, 10.0f));
        assert(Near(s.pending[0].cp[0], 10.0f));

        for (int i = 1; i < kMaxPending; ++i)
            Queue(s, r, a, 1100);
        assert(s.nPending == kMaxPending);
        assert(!QueueReady(s, 1100, r, true));  // pend[4] is full

        DropPending(s, 42);
        assert(s.nPending == 0 && s.seen == 42);

        // Sender's 66 ms rate limit.
        assert(SendReady(s, 1000));
        NoteSent(s, 1000);
        assert(!SendReady(s, 1000 + kSendCdMs - 1));
        assert(SendReady(s, 1000 + kSendCdMs));

        Reset(s);
        assert(s.seq == 0 && s.seen == 0 && s.nPending == 0 && s.lastSend == 0);
    }

    void TestHitCancelAndWhizzCooldown()
    {
        using namespace net::audio::near_miss::core;

        State s;
        NoteHit(s, 1, 1200);
        assert(HitCancels(s, 1, 1000));          // hit 200 ms after the line
        assert(!HitCancels(s, 1, 1500));         // hit 300 ms before that line
        assert(!HitCancels(s, 2, 1000));         // different slot
        assert(!HitCancels(s, -1, 1000));
        assert(!ReadyToWhizz(s, 1300, 1, 1000));

        NoteHit(s, 3, 1001);
        assert(HitCancels(s, 3, 1000));          // 250 ms inclusive boundary
        assert(ReadyToWhizz(s, 1300, 3, 1252));

        // A failed PCM start does not arm the 150 ms whizz cooldown.
        NoteWhizz(s, 1500, false);
        assert(s.lastWhiz == 0);
        assert(ReadyToWhizz(s, 1600, 2, 1000));
        NoteWhizz(s, 1500, true);
        assert(s.lastWhiz == 1500);
        assert(!ReadyToWhizz(s, 1500 + kWhizCdMs - 1, 2, 1000));
        assert(ReadyToWhizz(s, 1500 + kWhizCdMs, 2, 1000));
    }

    void TestLosGate()
    {
        using namespace net::audio::near_miss::core;

        const float our[3] = { 0.0f, 0.0f, 0.0f };
        const float farTarget[3] = { 10.0f, 0.0f, 0.0f };
        const float closeTarget[3] = { 0.5f, 0.0f, 0.0f };
        int kicks = 0;
        float ray = -1.0f;
        float from[3] = {}, chest[3] = {}, head[3] = {};
        auto kick = [&](const float* a, const float* b) -> uint64_t {
            ++kicks;
            for (int j = 0; j < 3; ++j) {
                from[j] = a[j];
                if (kicks % 2) chest[j] = b[j];
                else head[j] = b[j];
            }
            return static_cast<uint64_t>(kicks);
        };
        auto rayT = [&](uint64_t) -> float { return ray; };
        constexpr uint64_t h = 0x1234;

        // Unknown verdict is CLOSED: the first call only kicks the two rays from root+1.5 m
        // to the target's +0.6 / +1.3 m.
        LosEntry e;
        assert(!LosGate(e, h, 100, 1, our, farTarget, kick, rayT));
        assert(kicks == 2);
        assert(Near(from[1], 1.5f));
        assert(Near(chest[1], 0.6f) && Near(head[1], 1.3f));

        ray = -1.0f;                             // both rays clear
        assert(LosGate(e, h, 105, 2, our, farTarget, kick, rayT));
        assert(e.verdict == 1 && kicks == 2);    // no re-kick inside 150 ms

        // Blocked needs two agreeing samples after a clear verdict.
        ray = 0.5f;
        assert(LosGate(e, h, 115, 3, our, farTarget, kick, rayT));
        assert(kicks == 4);                      // 150 ms since the last kick
        assert(LosGate(e, h, 116, 4, our, farTarget, kick, rayT));
        assert(e.verdict == 1 && e.streak == 1);
        assert(LosGate(e, h, 130, 5, our, farTarget, kick, rayT));
        assert(!LosGate(e, h, 131, 6, our, farTarget, kick, rayT));
        assert(e.verdict == 0);

        // A clear verdict flips back immediately once the new rays are read.
        ray = -1.0f;
        assert(!LosGate(e, h, 145, 7, our, farTarget, kick, rayT));  // kicks a fresh pair
        assert(LosGate(e, h, 146, 8, our, farTarget, kick, rayT));
        assert(e.verdict == 1 && e.streak == 0);

        // Both rays starting inside a collider (t < 0.02) are not a wall between us.
        LosEntry inside;
        ray = 0.01f;
        assert(!LosGate(inside, 0x55, 100, 1, our, farTarget, kick, rayT));
        assert(LosGate(inside, 0x55, 101, 2, our, farTarget, kick, rayT));
        assert(inside.verdict == 1 && inside.handle == 0x55);

        // Closer than 0.8 m horizontally is open without any ray.
        LosEntry close;
        const int before = kicks;
        assert(LosGate(close, 0x66, 100, 1, our, closeTarget, kick, rayT));
        assert(close.verdict == 1 && kicks == before);

        // A long-unused cache starts over with a closed verdict.
        LosEntry stale;
        stale.handle = h;
        stale.verdict = 1;
        stale.lastUse = 1;
        stale.j1 = 1;
        stale.j2 = 2;
        ray = 0.5f;
        const int staleKicks = kicks;
        assert(!LosGate(stale, h, 1 + kLosStaleT60 + 1, 1, our, farTarget, kick, rayT));
        assert(stale.verdict == -1 && kicks == staleKicks + 2);
    }

    void TestLosCache()
    {
        using namespace net::audio::near_miss::core;

        State s;
        // Fill all eight handles with distinct last-use times; the ninth insert must
        // evict the least recently used (slot 0) and clear its verdict.
        for (uint64_t i = 1; i <= static_cast<uint64_t>(kLosEntries); ++i) {
            LosEntry& e = LosFor(s, i * 10);
            e.lastUse = 100 + i;
            e.verdict = 1;
            e.j1 = i;
        }
        assert(LosFor(s, 10).handle == 10 && LosFor(s, 10).verdict == 1);
        LosEntry& evicted = LosFor(s, 999);
        assert(&evicted == &s.los[0]);
        assert(evicted.handle == 999 && evicted.verdict == -1
               && evicted.lastUse == 0 && evicted.j1 == 0 && evicted.j2 == 0
               && evicted.streak == 0);

        const float our[3] = { 0.0f, 0.0f, 0.0f };
        const float far[3] = { 10.0f, 0.0f, 0.0f };
        int kicks = 0;
        auto kick = [&](const float*, const float*) -> uint64_t { return ++kicks; };
        auto rayT = [](uint64_t) -> float { return -1.0f; };

        // Cache form: unknown verdict is closed until the first async sample arrives.
        assert(!LosGate(s, 0xABC, 200, 1, our, far, kick, rayT));
        LosEntry& cached = LosFor(s, 0xABC);
        assert(cached.handle == 0xABC && cached.verdict == -1);
        assert(kicks >= 2);
        assert(LosGate(s, 0xABC, 201, 2, our, far, kick, rayT));
        assert(cached.verdict == 1 && cached.j1 == 0 && cached.j2 == 0);

        // A second handle is independent; a zero handle is open and does not evict.
        assert(!LosGate(s, 0xDEF, 202, 3, our, far, kick, rayT));
        assert(LosGate(s, 0, 203, 4, our, far, kick, rayT));   // unknown handle is open
        assert(LosFor(s, 0xABC).verdict == 1);
    }

    void TestPanAndRng()
    {
        using namespace net::audio::near_miss::core;

        const float listener[3] = { 0.0f, 0.0f, 0.0f };
        const float fwd[3] = { 0.0f, 0.0f, 1.0f };  // right = (+1, 0, 0)
        const float right[3] = { 5.0f, 0.0f, 0.0f };
        const float left[3] = { -5.0f, 0.0f, 0.0f };
        const float ahead[3] = { 0.0f, 0.0f, 5.0f };
        assert(Near(PanOf(right, listener, fwd), 0.85f));
        assert(Near(PanOf(left, listener, fwd), -0.85f));
        assert(Near(PanOf(ahead, listener, fwd), 0.0f));

        // ext normalises its camera-right vector before the dot product.
        const float fwd2[3] = { 0.0f, 0.0f, 2.0f };
        const float diag[3] = { 1.0f, 0.0f, 1.0f };
        assert(Near(PanOf(diag, listener, fwd2), 1.0f / std::sqrt(2.0f), 1e-3f));

        float l = 0.0f, r = 0.0f;
        PanLr(1.0f, 0.0f, l, r);                    // centre = 1.0 on both channels
        assert(Near(l, 1.0f, 1e-3f) && Near(r, 1.0f, 1e-3f));
        PanLr(1.0f, 0.85f, l, r);
        assert(r > l && l > 0.0f);

        // ext t2r_rand: xorshift32, seeded from the clock on first use.
        uint64_t rng = 0;
        const uint64_t now = 12345;
        assert(NextRand(rng, now) == 1181626438u);
        assert(NextRand(rng, now) == 1655520032u);
        assert(NextRand(rng, now) == 652381818u);
        assert(NextRand(rng, now) == 1438594835u);  // fourth draw, used by PickWave below
        assert(PickWave(rng, now, 4) == 2);       // fifth draw, modulo 4
        const uint64_t before = rng;
        assert(PickWave(rng, now, 0) == 0 && rng == before);  // no draw with no waves
    }

} // namespace

int main()
{
    TestClosestApproach();
    TestWire();
    TestGain();
    TestClipAndAim();
    TestRingAndPending();
    TestHitCancelAndWhizzCooldown();
    TestLosGate();
    TestLosCache();
    TestPanAndRng();
    std::printf("NEAR_MISS_CORE_TEST_OK\n");
    return 0;
}
