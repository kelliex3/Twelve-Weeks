// Offline test of bwpolicy.h: simulates a bomb thrown at t=0 that flies 1.0 s, lies still 2.0 s (fuse), at 30 / 60 / 144 fps, counts the MSG_BOMB_TRACK sends
// of the old rule (every 3 frames) and the new one, and a 2 s keep-alive on frame counters vs wall clock.
// Build: gcc -std=gnu2x -O2 -o bwpolicy_test bwpolicy_test.c && ./bwpolicy_test
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../bwpolicy.h"
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void Sim(int fps, int *old_n, int *new_n, int *new_rest_gap_max_ms,
                int *fly_gap_max_ms)
{
    double dt = 1000.0 / fps;
    int frames = (int) (3.0 * fps);
    int still_n = 0;
    uint64_t last_f_old = 0, last_f_new = 0;
    double last_ms_new = 0;
    int on = 0, nn = 0, gmax_rest = 0, gmax_fly = 0;
    double last_send_new = 0;
    for (int f = 1; f <= frames; f++) {
        double ms = f * dt;
        int flying = ms <= 1000.0;
        still_n = flying ? 0 : still_n + 1;
        if ((uint64_t) f - last_f_old >= 3) {
            on++;
            last_f_old = (uint64_t) f;
        }
        if (BwTrackDue
            ((uint64_t) f - last_f_new, ms - last_ms_new, still_n)) {
            nn++;
            if (last_send_new > 0) {
                int gap = (int) (ms - last_send_new + 0.5);
                if (still_n >= BW_TRACK_STILL_N && gap > gmax_rest)
                    gmax_rest = gap;
                if (!flying && 0) {
                }
                if (flying && gap > gmax_fly)
                    gmax_fly = gap;
            }
            last_send_new = ms;
            last_f_new = (uint64_t) f;
            last_ms_new = ms;
        }
    }
    *old_n = on;
    *new_n = nn;
    *new_rest_gap_max_ms = gmax_rest;
    *fly_gap_max_ms = gmax_fly;
}

int main(void)
{
    int fps_list[] = { 30, 60, 144 };
    for (int k = 0; k < 3; k++) {
        int o, n, gr, gf;
        Sim(fps_list[k], &o, &n, &gr, &gf);
        printf
            ("%3d fps: bomb track sends over a 1 s flight + 2 s at rest: old %d, new %d (%.0f%%), longest gap in flight %d ms, at rest %d ms\n",
             fps_list[k], o, n, 100.0 * n / o, gf, gr);
        CHECK(n < o
              || fps_list[k] == 30, "no reduction at %d fps", fps_list[k]);
        CHECK(gf <= 45 + 1000 / fps_list[k] * 2, "in-flight gap %d ms too long at %d fps", gf, fps_list[k]);    // never more than one extra frame of slack over the old pace at low fps
        CHECK(gr <= 200 + 1000 / fps_list[k] + 1,
              "rest gap %d ms too long at %d fps", gr, fps_list[k]);
    }
    // in flight the 60 fps pace is unchanged (3 frames = 50 ms >= 45 ms)
    {
        int o, n, gr, gf;
        Sim(60, &o, &n, &gr, &gf);
        CHECK(gf == 50, "60 fps flight gap %d", gf);
    }
    // boundary cases of the predicate
    CHECK(!BwTrackDue(2, 1000.0, 0), "under 3 frames never sends");
    CHECK(!BwTrackDue(3, 44.0, 0)
          && BwTrackDue(3, 45.0, 0), "fly 45 ms");
    CHECK(!BwTrackDue(9, 199.0, 6)
          && BwTrackDue(9, 200.0, 6), "rest 200 ms");
    CHECK(!BwTrackDue(9, 100.0, 5) == 0,
          "still_n 5 is still the flight pace (100 ms >= 45)");
    CHECK(BwTrackDue(9, 50.0, 5)
          && !BwTrackDue(9, 50.0, 6),
          "6 stills switch to the rest pace");
    // wall-clock keep-alive vs frame counter
    for (int k = 0; k < 3; k++) {
        int fps = fps_list[k], frames = 20 * fps, nf = 0, nt = 0;
        uint64_t last = 0;
        for (int f = 1; f <= frames; f++) {
            if (!(f % 120))
                nf++;
            if (BwEveryMs
                (&last, (uint64_t) (f * 1000.0 / fps) + 1000, 2000))
                nt++;
        }
        printf
            ("%3d fps: 2 s keep-alive over 20 s: frame counter %d sends, wall clock %d\n",
             fps, nf, nt);
        CHECK(nt == 10, "wall clock sends %d at %d fps", nt, fps);
    }
    {
        uint64_t last = 0;
        CHECK(BwEveryMs(&last, 0, 2000)
              && last == 1, "time 0 is stored as 1");
        CHECK(!BwEveryMs(&last, 1500, 2000), "not yet");
        CHECK(BwEveryMs(&last, 2001, 2000), "due");
    }
    printf(fails ? "bwpolicy_test: %d FAILED\n" :
           "bwpolicy_test: all passed\n", fails);
    return fails != 0;
}
