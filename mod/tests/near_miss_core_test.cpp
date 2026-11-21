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
    }

} // namespace

int main()
{
    TestClosestApproach();
    TestWire();
    TestGain();
    std::printf("NEAR_MISS_CORE_TEST_OK\n");
    return 0;
}
