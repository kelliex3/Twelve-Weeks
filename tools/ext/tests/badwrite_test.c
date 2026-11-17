// Offline test + micro-benchmark of t2r_bad_write (stale-while-revalidate, re/ext-tick-cost-1019.md).
// The cache code is cut out of ../t2r-ext.c (from "#define BW_N" to the IsBadWritePtr macro) at build time, so this tests the shipped text.
// Build (from tools/ext/tests, MSYS gcc on PATH):
//   sed -n '/^#define BW_N/,/^#define IsBadWritePtr/p' ../t2r-ext.c | tr -d '\r' > bw_cut.h
//   gcc -std=gnu2x -O2 -o badwrite_test badwrite_test.c && ./badwrite_test        (exit 0 = all checks pass)
// Checks: fresh hit, stale hit answered without a synchronous query (and re-validated by the worker), a decommitted region turns "bad" within ~0.5 s,
// a level change (g_bw_gen) drops the cache, never "good" for a non-writable page; then times the call and VirtualQuery on big committed regions.
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "bw_cut.h"
static LARGE_INTEGER qf;
static double us(LARGE_INTEGER a, LARGE_INTEGER b)
{
    return (double) (b.QuadPart - a.QuadPart) * 1e6 / (double) qf.QuadPart;
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } else { printf("ok:   " __VA_ARGS__); printf("\n"); } } while (0)
int main(void)
{
    QueryPerformanceFrequency(&qf);
    uint8_t *r =
        VirtualAlloc(NULL, 1 << 20, MEM_COMMIT | MEM_RESERVE,
                     PAGE_READWRITE);
    uint8_t *ro =
        VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READONLY);
    CHECK(!T2rBadWrite(r + 100, 8), "writable page: good (sync query)");
    CHECK(g_bw_n_vq == 1, "one synchronous query so far (%lld)",
          (long long) g_bw_n_vq);
    CHECK(!T2rBadWrite(r + 5000, 8) && g_bw_n_vq == 1
          && g_bw_n_fresh == 1, "second call: fresh hit, no query");
    CHECK(T2rBadWrite(ro, 8), "read-only page: bad");
    CHECK(T2rBadWrite(NULL, 8)
          && !T2rBadWrite(r, 0), "NULL bad, zero length good");
    Sleep(300);
    LONG64 vq0 = g_bw_n_vq;
    CHECK(!T2rBadWrite(r + 100, 8) && g_bw_n_vq == vq0
          && g_bw_n_stale == 1,
          "after 300 ms: stale hit answered with no synchronous query");
    Sleep(150);
    CHECK(g_bw_n_reval >= 1, "worker re-validated the entry (%lld)",
          (long long) g_bw_n_reval);
    LONG64 fr0 = g_bw_n_fresh;
    CHECK(!T2rBadWrite(r + 100, 8)
          && g_bw_n_fresh == fr0 + 1,
          "entry fresh again after the worker's refresh");
    VirtualFree(r, 1 << 20, MEM_DECOMMIT);      // region decommitted: bad within TTL + worker latency
    int bad_at = -1;
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    for (int i = 0; i < 100; i++) {
        if (T2rBadWrite(r + 100, 8)) {
            bad_at = i;
            break;
        }
        Sleep(10);
    }
    QueryPerformanceCounter(&b);
    CHECK(bad_at >= 0
          && us(a, b) / 1000.0 < 600.0,
          "decommitted region reported bad after %.0f ms (limit: TTL 250 + worker 40 + query)",
          us(a, b) / 1000.0);
    VirtualAlloc(r, 1 << 20, MEM_COMMIT, PAGE_READWRITE);
    CHECK(!T2rBadWrite(r + 100, 8), "recommitted: good again");
    InterlockedIncrement(&g_bw_gen);
    LONG64 vq1 = g_bw_n_vq;
    CHECK(!T2rBadWrite(r + 100, 8)
          && g_bw_n_vq == vq1 + 1,
          "level change drops the cache: synchronous query again");
    Sleep(150);                 // prewarm: the worker re-validated the regions that were live at the drop, so this call is a fresh hit, not a synchronous query
    {
        LONG64 vq2 = g_bw_n_vq, fr2 = g_bw_n_fresh;
        CHECK(!T2rBadWrite(r + 100, 8) && g_bw_n_vq == vq2
              && g_bw_n_fresh == fr2 + 1,
              "after a level change the worker prewarmed the live region (no synchronous query)");
    }
    {
        VirtualFree(r, 1 << 20, MEM_DECOMMIT);
        InterlockedIncrement(&g_bw_gen);
        (void) T2rBadWrite(ro, 8);
        Sleep(150);
        CHECK(T2rBadWrite(r + 100, 8),
              "region decommitted before the drop is NOT prewarmed (bad)");
        VirtualAlloc(r, 1 << 20, MEM_COMMIT, PAGE_READWRITE);
    }
    LARGE_INTEGER q0, q1;
    int N = 2000000;
    volatile BOOL sink = 0;
    QueryPerformanceCounter(&q0);
    for (int i = 0; i < N; i++)
        sink |= T2rBadWrite(r + (i & 1023), 8);
    QueryPerformanceCounter(&q1);
    printf("fresh hit: %.1f ns / call\n", us(q0, q1) * 1000.0 / N);
    MEMORY_BASIC_INFORMATION mi;
    QueryPerformanceCounter(&q0);
    for (int i = 0; i < 100000; i++)
        VirtualQuery(r + (i & 1023), &mi, sizeof mi);
    QueryPerformanceCounter(&q1);
    printf("VirtualQuery, 1 MB region: %.2f us / call\n",
           us(q0, q1) / 100000);
    SIZE_T big = (SIZE_T) 3 << 30;      // what does VirtualQuery cost on a heap-like region?
    uint8_t *g =
        VirtualAlloc(NULL, big, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (g) {
        for (SIZE_T off = 0; off < big; off += (SIZE_T) 64 << 20)
            g[off] = 1;
        QueryPerformanceCounter(&q0);
        for (int i = 0; i < 20; i++)
            VirtualQuery(g + 4096 * (SIZE_T) i, &mi, sizeof mi);
        QueryPerformanceCounter(&q1);
        printf
            ("VirtualQuery, 3 GB committed region (64 pages touched): %.1f us / call (region %llu MB)\n",
             us(q0, q1) / 20, (unsigned long long) (mi.RegionSize >> 20));
        for (SIZE_T off = 0; off < ((SIZE_T) 256 << 20); off += 256 * 1024)
            VirtualFree(g + off, 64 * 1024, MEM_DECOMMIT);
        QueryPerformanceCounter(&q0);
        for (int i = 0; i < 20; i++)
            VirtualQuery(g + 128 * 1024 * (SIZE_T) i + 70000, &mi,
                         sizeof mi);
        QueryPerformanceCounter(&q1);
        printf
            ("VirtualQuery inside a fragmented 3 GB allocation: %.1f us / call (region %llu KB)\n",
             us(q0, q1) / 20, (unsigned long long) (mi.RegionSize >> 10));
    }
    printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails != 0;
}
