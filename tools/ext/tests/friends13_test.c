// friends13_test.c - the pure core of the in-game FRIENDS panel (tools/ext/friends13.h) against the daemon's real op 0xD9 JSON
// (tools/tests/social_ingame_test.py writes the files and runs this).
//   gcc -std=gnu2x -O2 -o friends13_test friends13_test.c && friends13_test <state-A.json> <state-B-party.json> <refused.json>
#include "../friends13.h"
static int fails;
#define CHECK(c, ...) do { int ok_ = !!(c); printf("%s ", ok_ ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)
static char *Slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    static char buf[4][65536];
    static int k;
    char *b = buf[k++ & 3];
    *n = fread(b, 1, 65535, f);
    b[*n] = 0;
    fclose(f);
    return b;
}

int main(int argc, char **argv)
{
    static Fr13T d;
    size_t n;
    if (argc < 4) {
        puts("usage: friends13_test a.json b.json refused.json");
        return 2;
    }
    char *ta = Slurp(argv[1], &n);
    size_t na = n;
    CHECK(ta && Fr13Parse(&d, ta, na), "state A parses");
    CHECK(d.ok == 1 && d.me_name[0]
          && !strncmp(d.me_id, "s:", 2) + !strncmp(d.me_id, "t:", 2) == 1,
          "me: %s (%s)", d.me_name, d.me_id);
    CHECK(d.nf >= 1 && d.f[0].name[0] && d.f[0].id[0]
          && d.f[0].status[0], "friend 0: %s / %s / %s", d.f[0].name,
          d.f[0].id, d.f[0].status);
    CHECK(d.ni >= 1
          && d.in[0].name[0], "incoming request: %s", d.in[0].name);
    CHECK(d.no >= 1
          && d.out[0].name[0], "request sent: %s", d.out[0].name);
    CHECK(d.npi >= 1
          && d.pinv[0].name[0], "party invite from %s", d.pinv[0].name);
    CHECK(Fr13Badge(&d) == d.ni + d.npi
          && Fr13Badge(&d) >= 2, "badge %d", Fr13Badge(&d));
    FrRowT rows[300];
    int nr = Fr13Rows(&d, rows, 300);
    CHECK(nr == 1 + d.npi + d.ni + d.nf + d.no && rows[0].kind == FR_K_ADD
          && rows[1].kind == FR_K_PINV,
          "right list: %d rows, ADD first then the invites", nr);
    CHECK(!d.has_party, "no party in state A");
    char *tb = Slurp(argv[2], &n);
    size_t nb = n;
    CHECK(tb && Fr13Parse(&d, tb, nb), "state B (party) parses");
    CHECK(d.has_party && d.nm == 2
          && d.pmax >= 2, "party of %d / %d", d.nm, d.pmax);
    int lead = -1, me = -1;
    for (int i = 0; i < d.nm; i++) {
        if (d.m[i].leader)
            lead = i;
        if (d.m[i].me)
            me = i;
    }
    CHECK(lead >= 0 && me >= 0
          && lead != me ? !d.me_leader : d.me_leader,
          "leader %d, me %d, meLeader %d", lead, me, d.me_leader);
    CHECK(d.m[me].ready == 1, "my ready flag came through");
    char *tc = Slurp(argv[3], &n);
    size_t nc = n;
    CHECK(tc && Fr13Parse(&d, tc, nc) && d.ok == 0
          && d.err[0], "a refused write still carries the state: err '%s'",
          d.err);
    // escapes, a long name cut on a character boundary, unknown keys skipped, a null party
    const char *j =
        "{\"ok\":1,\"err\":\"\",\"v\":1,\"x\":{\"a\":[1,2,{\"b\":null}],\"c\":\"q\\\"\"},\"me\":{\"id\":\"s:1\",\"name\":\"Zażółć \u0105\u00f3 \\\"q\\\"\"},\"friends\":[{\"id\":\"s:2\",\"name\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAĄĄĄĄ\",\"status\":\"menu\",\"seen\":null,\"party\":false,\"since\":12}],\"in\":[],\"out\":[],\"party\":null,\"pinv\":[],\"max\":4}";
    CHECK(Fr13Parse(&d, j, strlen(j)), "hand-made reply parses");
    CHECK(!strcmp(d.me_name, "Zażółć ąó \"q\""),
          "escapes decoded: %s", d.me_name);
    int len = (int) strlen(d.f[0].name), okutf = 1;
    for (int i = 0; i < len; i++)
        if (((unsigned char) d.f[0].name[i] & 0xC0) == 0x80
            && (i == 0 || (unsigned char) d.f[0].name[i - 1] < 0x80))
            okutf = 0;
    CHECK(len > 0 && len < 36 && okutf
          && ((unsigned char) d.f[0].name[len - 1] < 0x80
              || ((unsigned char) d.f[0].name[len - 1] & 0xC0) == 0x80),
          "a long name is cut on a character boundary (%d bytes)", len);
    CHECK(Fr13Presence("search") == 3 && Fr13Presence("off") == 0
          && Fr13CanInvite(&d, &d.f[0]) == 1, "presence / can-invite");
    CHECK(!Fr13Parse(&d, "{\"ok\":1,\"me\":{\"id\":\"s:1\"", 24),
          "a truncated reply is refused");
    CHECK(!Fr13Parse(&d, "garbage", 7), "garbage is refused");
    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails != 0;
}
