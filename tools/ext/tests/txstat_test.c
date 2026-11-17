// Offline test of txstat.h (per-type tx counter). Build: gcc -std=gnu2x -O2 -o txstat_test txstat_test.c && ./txstat_test
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../txstat.h"
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
int main(void)
{
    TxsT s;
    memset(&s, 0, sizeof s);
    for (int i = 0; i < 10; i++)
        TxsNote(&s, 0x6A, 52, 0xff);   // 10 x pstow broadcast: 60 B each
    for (int i = 0; i < 30; i++)
        TxsNote(&s, 0x4B, 10, i & 1 ? 3 : 0xff);       // 30 psync: half unicast, 18 B each
    for (int i = 0; i < 5; i++)
        TxsNote(&s, 0x58, 7, 0xff);
    TxsNote(&s, 0x62, 15, 0xff);
    TxsNote(&s, 0x06, 0xdeadbeef00000040ULL, 0xff);    // garbage upper bits: the uint8 size (0x40) counts
    TxsNote(&s, 0x0d, 0x12345678ffULL, 0xff);  // 0xff -> 255 clamps to 200
    CHECK(s.b[0x6A] == 600
          && s.n[0x6A] == 10, "pstow %u/%u", s.b[0x6A], s.n[0x6A]);
    CHECK(s.b[0x4B] == 540
          && s.uni[0x4B] == 15, "psync %u uni %u", s.b[0x4B], s.uni[0x4B]);
    CHECK(s.b[0x06] == 8 + 0x40, "garbage len -> %u", s.b[0x06]);
    CHECK(s.b[0x0d] == 8 + 200, "clamp -> %u", s.b[0x0d]);
    uint32_t m, b;
    TxsTotals(&s, &m, &b);
    CHECK(m == 10 + 30 + 5 + 1 + 1 + 1, "msgs %u", m);
    CHECK(b == 600 + 540 + 75 + 23 + 72 + 208, "bytes %u", b);
    int ids[8];
    int n = TxsTop(&s, 3, ids);
    CHECK(n == 3 && ids[0] == 0x6A && ids[1] == 0x4B
          && ids[2] == 0x0d, "top3 %02x %02x %02x (n %d)", ids[0], ids[1],
          ids[2], n);
    n = TxsTop(&s, 6, ids);
    CHECK(n == 6 && ids[3] == 0x58 && ids[4] == 0x06
          && ids[5] == 0x62, "top6 order %02x %02x %02x", ids[3], ids[4],
          ids[5]);
    n = TxsTop(&s, 8, ids);
    CHECK(n == 6, "only 6 ids present, got %d", n);
    char line[400];
    TxsFmt(&s, 10.0, 6, line, sizeof line);
    CHECK(strstr(line, " 6a:1.00/s 60B 60B/s u0%") == line, "fmt: %s",
          line);
    CHECK(strstr(line, " 4b:3.00/s 18B 54B/s u50%") != NULL, "fmt: %s",
          line);
    // many ids: the top k really is the k largest
    memset(&s, 0, sizeof s);
    for (int t = 0; t < 256; t++)
        for (int i = 0; i <= (t * 37) % 11; i++)
            TxsNote(&s, (uint64_t) t, 0, 0xff);
    n = TxsTop(&s, 6, ids);
    for (int i = 1; i < n; i++)
        CHECK(s.b[ids[i - 1]] >= s.b[ids[i]], "sorted at %d", i);
    for (int t = 0; t < 256; t++) {
        int in = 0;
        for (int i = 0; i < n; i++)
            in |= ids[i] == t;
        CHECK(in || !s.n[t]
              || s.b[t] <= s.b[ids[n - 1]],
              "id %02x (%u) missing from the top 6", t, s.b[t]);
    }
    memset(&s, 0, sizeof s);
    CHECK(TxsTop(&s, 6, ids) == 0, "empty");
    printf(fails ? "txstat_test: %d FAILED\n" :
           "txstat_test: all passed\n", fails);
    return fails != 0;
}
