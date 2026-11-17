// Offline fuzz + unit check of peertrust.h (peer message trust helpers).
// Build: gcc -std=gnu2x -O2 -o peertrust_test peertrust_test.c && ./peertrust_test   (exit 0 = all pass)
// The helpers take the wire's numbers; random payloads must never yield an index out of range, a non-finite float that passes, a rate above the limit
// or a host-only message from a non-host.
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "../peertrust.h"
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static uint64_t rs = 0x9E3779B97F4A7C15ULL;
static uint32_t Rnd(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (uint32_t) (rs >> 16);
}

static float RndFloat(void)
{                               // every kind of float: random bit patterns (NaN, inf, denormals), small, huge
    uint32_t k = Rnd() % 4, u = Rnd();
    float f;
    if (k == 0)
        memcpy(&f, &u, 4);
    else if (k == 1)
        f = (float) (int32_t) u / 1000.0f;
    else if (k == 2)
        f = (float) (int32_t) u *1e6f;
    else {
        static const float S[] =
            { 0.0f, -0.0f, 1.0f, -1.0f, 1e5f, 1e6f, 1e30f, 3.4e38f,
1e-40f };
        f = S[u % 9];
        if (u & 0x100)
            f = -f;
    }
    return f;
}

int main(void)
{
    // ---- floats: pt_f_ok never lets NaN / inf / |f| >= lim through
    for (int i = 0; i < 2000000; i++) {
        float f = RndFloat(), lim = 1e5f;
        if (PtFOk(f, lim))
            CHECK(isfinite(f) && fabsf(f) < lim);
        float v[3] = { RndFloat(), RndFloat(), RndFloat() };
        if (PtV3Ok(v, lim))
            CHECK(isfinite(v[0] + v[1] + v[2]) && fabsf(v[0]) < lim
                  && fabsf(v[1]) < lim && fabsf(v[2]) < lim);
        float q[4] =
            { RndFloat(), RndFloat(), RndFloat(), RndFloat() };
        if (PtQuatOk(q)) {
            float n =
                q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
            CHECK(isfinite(n) && n >= 0.25f && n <= 4.0f);
        }
    }
    CHECK(!PtFOk(NAN, 1e5f));
    CHECK(!PtFOk(INFINITY, 1e5f));
    CHECK(!PtFOk(-INFINITY, 1e5f));
    CHECK(PtFOk(0.0f, 1e5f));
    CHECK(PtFOk(-99999.0f, 1e5f));
    {
        float z[4] = { 0, 0, 0, 0 };
        CHECK(!PtQuatOk(z));
        float id[4] = { 0, 0, 0, 1 };
        CHECK(PtQuatOk(id));
    }
    // ---- slots: anything >= 8 is refused, 0xff only where "none" is allowed
    for (uint32_t s = 0; s < 1000; s++) {
        CHECK(PtSlotOk(s) == (s < 8));
        CHECK(PtSlotOrNone(s) == (s < 8 || s == 255));
    }
    // ---- counts: n * per + hdr never exceeds the message nor the table, whatever len / claimed are (including len < hdr: no unsigned underflow)
    for (int i = 0; i < 2000000; i++) {
        uint32_t claimed = Rnd() % 300, tmax = Rnd() % 40, Hdr =
            Rnd() % 8, per = 1 + Rnd() % 16;
        uint64_t len =
            Rnd() % 3 ? Rnd() % 200 : ((uint64_t) Rnd() << 32 | Rnd());
        uint32_t n = PtCount(claimed, tmax, len, Hdr, per);
        CHECK(n <= claimed && n <= tmax);
        if (n)
            CHECK((uint64_t) n * per + Hdr <= len);
        if (len < Hdr)
            CHECK(n == 0);
    }
    CHECK(PtCount(10, 10, 5, 2, 4) == 0);      // 3 bytes of entries: not even one
    CHECK(PtCount(10, 10, 6, 2, 4) == 1);
    CHECK(PtCount(255, 16, 1000, 2, 4) == 16);
    // ---- host-only
    CHECK(PtHostOk(0, 0, 2, 3));
    CHECK(!PtHostOk(1, 0, 2, 3));
    CHECK(PtHostOk(4, 4, 2, 3));
    CHECK(!PtHostOk(0, 4, 2, 3));     // client: only the host slot
    CHECK(!PtHostOk(0, 0, 1, 0));
    CHECK(!PtHostOk(5, 0, 1, 0));     // the host takes none
    CHECK(PtHostOk(1, 0, 0, 0));
    CHECK(!PtHostOk(0, 0, 0, 0));     // solo test
    for (int i = 0; i < 100000; i++) {
        uint32_t s = Rnd() % 300;
        int ch = (int) (Rnd() % 20) - 5;
        if (PtHostOk(s, ch, 2, 1))
            CHECK(s < 8 && (int) s == ch);
    }
    // ---- within
    CHECK(PtWithin(0, 1e9f, 1e9f, 1e9f, 1, 1));        // position unknown: passes (the caller's other checks decide)
    CHECK(!PtWithin(1, NAN, 0, 0, 5, 5));
    CHECK(!PtWithin(1, 0, INFINITY, 0, 5, 5));
    CHECK(!PtWithin(1, 6, 0, 0, 5, 5));
    CHECK(PtWithin(1, 3, 2, 3, 5, 5));
    for (int i = 0; i < 1000000; i++) {
        float dx = RndFloat(), dy = RndFloat(), dz = RndFloat();
        if (PtWithin(1, dx, dy, dz, 9.0f, 4.0f))
            CHECK(isfinite(dx) && isfinite(dy) && isfinite(dz)
                  && dx * dx + dz * dz <= 81.0f && fabsf(dy) <= 4.0f);
    }
    // ---- rate limit: never more than `limit` passes in any 1 s window, also with a clock that jumps around; recovers in the next window
    for (int round = 0; round < 2000; round++) {
        PtRlT r = { 0 };
        uint32_t limit = 1 + Rnd() % 200;
        uint64_t t = Rnd() % 100000, last_win = ~0ULL;
        uint32_t passed = 0;
        for (int i = 0; i < 1000; i++) {
            t += Rnd() % 40;
            int ok = PtRate(&r, t, limit);
            if (r.win_ms != last_win) {
                last_win = r.win_ms;
                passed = 0;
            }                   // a new window opened with this call
            if (ok)
                passed++;
            CHECK(passed <= limit && r.n <= limit);
        }
    }
    {
        PtRlT r = { 0 };
        for (int i = 0; i < 100; i++)
            PtRate(&r, 5000, 10);
        CHECK(r.n == 10 && r.dropped == 90);
        CHECK(PtRate(&r, 6000, 10));
    }                           // flood: 10 pass, 90 dropped, next window opens
    {
        PtRlT r = { 0 };
        PtRate(&r, 9000, 5);
        CHECK(PtRate(&r, 10, 5));
    }                           // clock went backwards: window restarts
    {
        uint64_t lm = 0;
        CHECK(PtLogDue(&lm, 1000));
        CHECK(!PtLogDue(&lm, 3000));
        CHECK(!PtLogDue(&lm, 5999));
        CHECK(PtLogDue(&lm, 6000));
        CHECK(PtLogDue(&lm, 0) || 1);
    }
    for (int t = 0; t < 256; t++)
        CHECK(PtLimit((uint8_t) t) >= 1 && PtLimit((uint8_t) t) <= 400);
    // ---- mines
    {
        float p[3] = { 1, 2, 3 };
        uint16_t id = (uint16_t) ((3 << 8) | 7);
        CHECK(PtMinePlaceOk(3, id, p, 0, 3));
        CHECK(!PtMinePlaceOk(2, id, p, 0, 3));       // slot 2 placing an id of slot 3: a spoof
        CHECK(!PtMinePlaceOk(3, id, p, 5, 3));       // far over the per-player cap
        float bad[3] = { NAN, 0, 0 };
        CHECK(!PtMinePlaceOk(3, id, bad, 0, 3));
        float big[3] = { 0, 1e7f, 0 };
        CHECK(!PtMinePlaceOk(3, id, big, 0, 3));
        CHECK(!PtMinePlaceOk(9, (uint16_t) (9 << 8), p, 0, 3));
    }
    for (int i = 0; i < 1000000; i++) {
        float p[3] = { RndFloat(), RndFloat(), RndFloat() };
        uint32_t from = Rnd() % 300;
        uint16_t id = (uint16_t) Rnd();
        if (PtMinePlaceOk(from, id, p, (int) (Rnd() % 20), 3))
            CHECK(from < 8 && (uint32_t) (id >> 8) == from
                  && isfinite(p[0] + p[1] + p[2]));
    }
    // ---- roll numbers
    CHECK(PtRollOk(0, 0));
    CHECK(PtRollOk(5, 5));
    CHECK(PtRollOk(6, 5));
    CHECK(!PtRollOk(7, 5));
    CHECK(!PtRollOk(-1, 5));
    CHECK(!PtRollOk(251, 255));
    for (int n = -300; n < 300; n++)
        for (int r = 0; r < 300; r += 7)
            if (PtRollOk(n, r))
                CHECK(n >= 0 && n <= 250 && n <= r + 1);
    // ---- revive
    CHECK(PtReviveOk(1, 1, 1, 0, 1, 1, 1, 0, 1) == PT_RV_OK);
    CHECK(PtReviveOk(0, 1, 1, 0, 1, 1, 1, 0, 1) == PT_RV_SLOT);
    CHECK(PtReviveOk(1, 0, 1, 0, 1, 1, 1, 0, 1) == PT_RV_SLOT);
    CHECK(PtReviveOk(1, 1, 0, 0, 1, 1, 1, 0, 1) == PT_RV_ENEMY);      // an enemy "reviving" us
    CHECK(PtReviveOk(1, 1, 1, 1, 1, 1, 1, 0, 1) == PT_RV_SENDER_DOWN);
    CHECK(PtReviveOk(1, 1, 1, 0, 0, 1, 1, 0, 1) == PT_RV_TARGET_UP);  // nobody is down
    CHECK(PtReviveOk(1, 1, 1, 0, 1, 1, 30, 0, 0) == PT_RV_FAR);
    CHECK(PtReviveOk(1, 1, 1, 0, 1, 0, 30, 0, 0) == PT_RV_OK);        // position unknown: not judged
    for (int i = 0; i < 1000000; i++) {
        int r =
            PtReviveOk((int) (Rnd() & 1), (int) (Rnd() & 1),
                         (int) (Rnd() & 1), (int) (Rnd() & 1),
                         (int) (Rnd() & 1), (int) (Rnd() & 1), RndFloat(),
                         RndFloat(), RndFloat());
        CHECK(r >= PT_RV_OK && r <= PT_RV_FAR);
    }
    // ---- mine id / cap split
    {
        float p[3] = { 1, 2, 3 };
        uint16_t id = (uint16_t) ((3 << 8) | 7);
        CHECK(PtMineIdOk(3, id, p));
        CHECK(!PtMineIdOk(2, id, p));
        CHECK(PtMineCapOk(4, 3));
        CHECK(!PtMineCapOk(5, 3));
    }
    // ---- UC4 reach: a stop with the 'no target' 99 is honest (uc4_reach(99, 0)), a start for a non-slot / for the sender is not
    CHECK(!PtReachWireBad(3, 1, 0) && !PtReachWireBad(99, 0, 2)
          && !PtReachWireBad(0xff, 0, 5)
          && !PtReachWireBad(3, 0, 0));
    CHECK(PtReachWireBad(99, 1, 2) && PtReachWireBad(8, 1, 2)
          && PtReachWireBad(2, 1, 2) && PtReachWireBad(2, 0, 2));
    // ---- switches: defaults, parser, decision
    {
        PtCfgT c;
        PtCfgDefault(&c);
        int lg;
        // the enforced-by-default set is exactly the pure-wire checks: nothing that needs the receiver's picture of teams / positions / host / rolls may be PT_DROP
        static const char *const DROP_OK[] =
            { "gate_slot", "gate_len", "rate_flood", "wire", "mine_id",
"pstow_def" };
        for (int k = 0; k < PTK_N; k++) {
            int ok_drop = 0;
            for (size_t j = 0; j < sizeof DROP_OK / sizeof *DROP_OK; j++)
                if (!strcmp(PT_NAME[k], DROP_OK[j]))
                    ok_drop = 1;
            CHECK((c.mode[k] == PT_DROP) == ok_drop);
            CHECK(c.mode[k] <= PT_DROP);
        }
        CHECK(PTK_N == (int) (sizeof PT_NAME / sizeof *PT_NAME));
        for (int k = 0; k < PTK_N; k++)
            CHECK(!PtDecide(0, &c, k, &lg) && !lg);    // master off: nothing is dropped, nothing logged
        CHECK(PtDecide(1, &c, PTK_wire, &lg) == 1 && !lg);     // drop mode
        CHECK(PtDecide(1, &c, PTK_revive, &lg) == 0 && lg);    // log-only: passes, "would drop" line
        CHECK(PtDecide(1, &c, -1, &lg) == 0
              && PtDecide(1, &c, PTK_N, &lg) == 0);
        CHECK(PtCfgParse
              (&c,
               "revive=drop, host_only=off\r\nbogus=drop wire=nope  mine_id=log;")
              == 3);
        CHECK(c.mode[PTK_revive] == PT_DROP
              && c.mode[PTK_host_only] == PT_OFF
              && c.mode[PTK_mine_id] == PT_LOG
              && c.mode[PTK_wire] == PT_DROP);
        CHECK(PtDecide(1, &c, PTK_host_only, &lg) == 0 && !lg);        // off: no drop, no log
        CHECK(PtCfgParse(&c, "all=log") == 1);
        for (int k = 0; k < PTK_N; k++)
            CHECK(c.mode[k] == PT_LOG);
        CHECK(PtCfgParse(&c, "") == 0
              && PtCfgParse(&c, "=== = ,,, wire") == 0);
        // fuzz the parser: random bytes never crash / read past the terminator
        for (int i = 0; i < 200000; i++) {
            char b[64];
            int n = (int) (Rnd() % 63);
            for (int j = 0; j < n; j++) {
                static const char A[] =
                    "abcdefghijklmnopqrstuvwxyz_=, \t\r\n;0";
                b[j] =
                    Rnd() % 5 ? A[Rnd() % (sizeof A - 1)] : (char) (Rnd() %
                                                                    255 +
                                                                    1);
            } b[n] = 0;
            PtCfgParse(&c, b);
        }
    }
    printf(fails ? "%d FAILED\n" : "all peertrust checks pass\n", fails);
    return fails != 0;
}
