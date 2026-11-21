// Standalone behavioural harness for the pure netsmooth core (net/game/net-smooth.*).
// No engine / game headers: compiles with any C++17 compiler, e.g. from the repo root:
//
/*   g++ -std=c++17 -Wall -Wextra -Werror -I mod/src/net/game
       mod/tests/netsmooth_core_test.cpp mod/src/net/game/net-smooth.cpp
       -o /tmp/netsmooth_core_test && /tmp/netsmooth_core_test */
//
// Mirrors the scenarios in tools/ext/tests/netsim_sim.c at the pure-core seam: pairing,
// playout bracketing, handover, teleport detection, sender-clock dequantisation/rebase and
// msg-7 loss counting. It does NOT exercise the native bridge (that needs frame::NowMs()).

#include "net-smooth.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>

using namespace net::netsmooth;

namespace {

    double g_now = 1000.0;

    void Feed(uint8_t slot, uint32_t seq, double stepMs, float x)
    {
        uint32_t send = static_cast<uint32_t>(seq * stepMs);
        OnAnim(slot, seq, send, g_now);
        float p[3] = { x, 0.0f, 0.0f };
        float q[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        OnState(slot, seq, p, q, g_now);
        g_now += stepMs;
    }

} // namespace

int main()
{
    ResetAll();
    {
        Transform t;
        assert(!Primed(0));
        assert(!Sample(0, g_now, 0.0, t));   // unprimed fallback
    }

    // 16 ms sender clock moving +1 m/s along X; every state pairs with an anim stamp.
    for (uint32_t seq = 1; seq <= 120; seq++)
        Feed(0, seq, 16.0, static_cast<float>(seq) * 0.016f);

    assert(Primed(0));
    const Stats& first = StatsFor(0);
    assert(first.states == 120 && first.anims == 120);
    assert(first.lost5 == 0 && first.lost7 == 0 && first.tele == 0 && first.rebase == 0);
    assert(first.frames == 0);

    // Keep feeding while sampling: the playout clock should bracket samples (A && B),
    // not free-run past the newest one.
    Transform prev{};
    for (uint32_t i = 0; i < 200; i++) {
        uint32_t seq = 121 + i;
        Feed(0, seq, 16.0, static_cast<float>(seq) * 0.016f);
        Transform out{};
        assert(Sample(0, g_now, 30.0, out));
        if (i) {
            float dx = out.pos[0] - prev.pos[0];
            assert(dx >= -0.01f && dx < 0.10f);   // no jump, no rewind
        }
        prev = out;
        assert(std::isfinite(out.pos[0]) && std::isfinite(out.quat[3]));
    }
    assert(StatsFor(0).frames == 200);
    assert(StatsFor(0).extrap < 40);              // mostly interpolation

    // Handover: first frame is the native placement, then it blends toward the smoother.
    Reset(1);
    for (uint32_t seq = 1; seq <= 40; seq++)
        Feed(1, seq, 16.0, static_cast<float>(seq) * 0.016f);
    Transform native{};
    native.pos[0] = 5.0f;
    Transform h0{};
    assert(Sample(1, g_now, 0.0, h0, &native));
    Transform h1{};
    assert(Sample(1, g_now + 16.0, 0.0, h1, &native));
    assert(fabsf(h0.pos[0] - 5.0f) < 0.01f);
    assert(fabsf(h1.pos[0] - 5.0f) > fabsf(h0.pos[0] - 5.0f));

    // A 20 m jump is a teleport (not interpolated through).
    ResetAll();
    g_now = 1000.0;
    Feed(2, 1, 16.0, 0.0f);
    Feed(2, 2, 16.0, 20.0f);
    assert(StatsFor(2).tele == 1);

    // 15.625 ms staircase: least-squares dequantisation must engage.
    ResetAll();
    g_now = 1000.0;
    for (uint32_t seq = 1; seq <= 40; seq++)
        Feed(3, seq, 15.625, static_cast<float>(seq) * 0.015625f);
    assert(StatsFor(3).dequant > 40);             // 40 stamps + quantised outputs

    // Three consecutive lists >= 2 s behind the old clock confirm a new sender domain.
    ResetAll();
    g_now = 1000.0;
    for (uint32_t seq = 1; seq <= 10; seq++)
        Feed(4, seq, 16.0, static_cast<float>(seq) * 0.016f);
    for (uint32_t seq = 11; seq <= 13; seq++) {
        float p[3] = { static_cast<float>(seq) * 0.016f, 0.0f, 0.0f };
        float q[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        OnAnim(4, seq, 160u - (2000u + (seq - 10) * 100u), g_now);
        OnState(4, seq, p, q, g_now);
        g_now += 16.0;
    }
    assert(StatsFor(4).rebase >= 1);

    // msg 7 is forwarded as received: a seq gap is real loss.
    ResetAll();
    g_now = 1000.0;
    OnAnim(5, 1, 16, g_now); g_now += 16.0;
    OnAnim(5, 5, 80, g_now); g_now += 16.0;
    assert(StatsFor(5).lost7 == 3);

    puts("NETSMOOTH_CORE_TEST_OK");
    return 0;
}
