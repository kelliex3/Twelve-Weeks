// hostbw.h unit test (build from tools/ext/tests: gcc -std=gnu2x -O2 -o hostbw_test hostbw_test.c). Exit 0 = pass.
#include <stdio.h>
#include "../hostbw.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

// drive one pair at `hz` for `secs` seconds with a fake tx counter growing at `tx_Bps`; returns lists passed
static int drive(HbwT *h, double *t, uint64_t *tx, double tx_Bps, int src,
                 int dst, float dist, double hz, double secs)
{
    int pass = 0;
    double dt = 1000.0 / hz;
    for (double e = 0; e < secs * 1000.0; e += dt) {
        *t += dt;
        *tx += (uint64_t) (tx_Bps * dt * 0.001);
        pass += HbwMsg7(h, *t, src, dst, dist, 637, *tx);
    }
    return pass;
}

int main(void)
{
    HbwT h;
    double t = 1000.0;
    uint64_t tx = 0;
    // off = everything passes
    HbwInit(&h, 0, 600);
    CHECK(drive(&h, &t, &tx, 5e6, 1, 2, 80.0f, 90, 2) >= 178);
    // below budget: level stays 0, everything passes
    HbwInit(&h, 1, 600);
    CHECK(drive(&h, &t, &tx, 300 * 1024, 1, 2, 80.0f, 90, 5) == 450
          && h.level == 0);
    // far pair above budget -> level >= 1, far pair paced to ~30 Hz or below, near pair untouched
    HbwInit(&h, 1, 600);
    t = 1000.0;
    tx = 0;
    int far = drive(&h, &t, &tx, 900 * 1024, 1, 2, 80.0f, 90, 10);
    CHECK(h.level >= 1);
    CHECK(far < 90 * 10 * 0.45);        // > 55 % of a 90 Hz stream dropped over 10 s (level 1 needs the first window)
    CHECK(far > 15 * 8);        // never below the 15 Hz floor
    int near_ = drive(&h, &t, &tx, 900 * 1024, 3, 4, 5.0f, 90, 2);
    CHECK(near_ >= 178);
    // unknown distance and out-of-range slots pass
    CHECK(HbwMsg7(&h, t + 1, 1, 2, -1.0f, 637, tx) == 1);
    CHECK(HbwMsg7(&h, t + 2, 9, 2, 80.0f, 637, tx) == 1);
    // closed loop: base 300 KB/s + one far pair of 6370 B lists at 90 Hz (560 KB/s offered): demand 860 -> level 2; tx falls as lists are dropped,
    // but demand includes the dropped bytes, so the level must not oscillate
    HbwInit(&h, 1, 600);
    t = 1000.0;
    tx = 0;
    int l0 = 0, ch0 = 0;
    for (int sec = 0; sec < 60; sec++) {
        if (sec == 30) {
            l0 = h.level;
            ch0 = h.lvl_changes;
        }
        for (int k = 0; k < 90; k++) {
            t += 1000.0 / 90.0;
            tx += (uint64_t) (300 * 1024 / 90);
            if (HbwMsg7(&h, t, 1, 2, 80.0f, 6370, tx))
                tx += 6370;
        }
    }
    printf("closed loop: level %d, changes %d -> %d, demand %.0f KB/s\n",
           l0, ch0, h.lvl_changes, h.demand_Bps / 1024);
    CHECK(l0 == 2 && h.lvl_changes == ch0);
    // load disappears: level comes back to 0 (after two quiet windows each step)
    for (int i = 0; i < 30; i++)
        drive(&h, &t, &tx, 50 * 1024, 1, 2, 80.0f, 90, 1);
    CHECK(h.level == 0);
    printf(fails ? "hostbw_test FAILED (%d)\n" : "hostbw_test ok\n",
           fails);
    return fails != 0;
}
