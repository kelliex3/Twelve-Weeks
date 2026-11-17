// Offline check of clanperk.h (clan supply perks: parse of the daemon's tail, spawn plan, report packing, card text, summary).
// Build: gcc -std=gnu2x -O2 -o clanperk_test clanperk_test.c && ./clanperk_test   (exit 0 = all pass)
#include <stdio.h>
#include "../clanperk.h"
static int fails;
#define CK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
int main(void)
{
    CpStateT s;
    // the exact tail tools/clan_perks.wire_text sends, behind a normal state text
    const char *t =
        "v=1 fac=1 st=1 day=9 h=8 hu=0 s=1 lmh=77 ag1=3 ag2=6 ag3=9 pk=3 pkc0=1 pkn0=2 pkg0=1 pkl0=1 pkr0=1 pkc1=2 pkn1=9 pkg1=0 pkl1=3 pkr1=5 pkc2=4 pkn2=1 pkg2=0 pkl2=1 pkr2=2 pkm0=Lila_B pkm1=Wendy pkm2=Arnaldo";
    CpParseTail(t, &s);
    CK(s.n == 3, "three perks (%d)", s.n);
    CK(s.p[CP_MEDKIT].cat == 1 && s.p[CP_MEDKIT].cap == 2
       && s.p[CP_MEDKIT].female == 1
       && !strcmp(s.p[CP_MEDKIT].name, "Lila B"), "medkit: %d %d %s",
       s.p[0].cat, s.p[0].cap, s.p[0].name);
    CK(s.p[CP_CLIP].cat == 2
       && s.p[CP_CLIP].cap == 3,
       "clip cap clamped to the per-match maximum 3 (daemon sent 9): %d",
       s.p[1].cap);
    CK(s.p[CP_STAMINA].cat == 4 && s.p[CP_STAMINA].cap == 1
       && s.p[CP_STAMINA].look == 1, "stamina in its own slot");
    CK(!s.p[CP_PARTS].cat
       && !s.p[CP_BLEED].cat, "perks not granted stay empty");
    // an old daemon: no keys at all -> nothing, never garbage
    CpParseTail("v=1 fac=1 st=1 day=9 lmh=77 ag1=3", &s);
    CK(s.n == 0 && !s.p[0].cat, "old daemon: no perks");
    CpParseTail(NULL, &s);
    CK(s.n == 0, "NULL text");
    // malformed: count above 5, category 9, cap 0, a name with no number, a huge token
    CpParseTail
        ("pk=99 pkc0=9 pkn0=1 pkc1=2 pkn1=0 pkc2=3 pkn2=2 pkm2=X pkx=1 pkc7=1",
         &s);
    CK(s.n == 1 && s.p[CP_PARTS].cat == 3
       && !strcmp(s.p[CP_PARTS].name, "X"),
       "malformed tokens skipped, valid kept (%d)", s.n);
    char big[400];
    memset(big, 'a', sizeof big - 1);
    big[sizeof big - 1] = 0;
    CpParseTail(big, &s);
    CK(s.n == 0, "a giant token is ignored");
    CpParseTail
        ("pk=1 pkc0=1 pkn0=2 pkm0=ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEF", &s);
    CK(s.n == 1 && strlen(s.p[0].name) == 23, "names are cut to 23 chars");
    CpParseTail("pk=1 pkc0=1 pkn0=2", &s);
    CK(!strcmp(s.p[0].name, "Your clan"), "no name sent: 'Your clan'");
    // when the help comes: struggling = 2 lives lost in a row, or 1 while 4+ behind / the clan lost 2+ matches, or the first spawn after 3+ lost matches
    CK(CpStruggling(0, 0, 0, 1) == CP_WHY_NONE
       && CpStruggling(1, 0, 0, 2) == CP_WHY_NONE
       && CpStruggling(1, 3, 1, 2) == CP_WHY_NONE, "no help while fine");
    CK(CpStruggling(2, 0, 0, 3) == CP_WHY_BAD2
       && CpStruggling(5, 0, 0, 9) == CP_WHY_BAD2,
       "2 lives lost in a row: help");
    CK(CpStruggling(1, 4, 0, 2) == CP_WHY_BEHIND
       && CpStruggling(1, 0, 2, 2) == CP_WHY_BEHIND
       && CpStruggling(0, 9, 9, 2) == CP_WHY_NONE,
       "1 life lost + behind / loss streak: help; none without a lost life");
    CK(CpStruggling(0, 0, 3, 1) == CP_WHY_STREAK
       && CpStruggling(0, 0, 3, 2) == CP_WHY_NONE
       && CpStruggling(0, 0, 2, 1) == CP_WHY_NONE,
       "3 lost matches: the first spawn of the match");
    CK(CpGrantDue(1, 1000, 0) && !CpGrantDue(0, 1000, 0)
       && !CpGrantDue(1, 50000, 1000)
       && CpGrantDue(1, 91001, 1000),
       "one grant per streak, 90 s apart");
    CpParseTail
        ("pk=3 pks=4 pkc0=1 pkn0=2 pkc1=2 pkn1=3 pkc2=3 pkn2=2 pkm0=Tyler",
         &s);
    CK(s.n == 3 && s.streak == 4, "pks = the loss streak");
    CpParseTail("pk=1 pks=77 pkc0=1 pkn0=2", &s);
    CK(s.streak == 9, "streak clamps to 9");
    CpParseTail("pk=1 pkc0=1 pkn0=2", &s);
    CK(s.streak == 0, "no pks: 0");
    {
        int used0[CP_N] = { 0 }, used1[CP_N] = { 1, 0, 0, 0, 0 };
        CpParseTail("pk=3 pkc0=1 pkn0=2 pkc1=2 pkn1=3 pkc2=3 pkn2=2",
                      &s);
        CK(CpPickItem(&s, used0, 7) == CP_MEDKIT, "medkit first");
        CK(CpPickItem(&s, used1, 7) == CP_CLIP,
           "then the one used least (clip)");
        CK(CpPickItem(&s, used0, 6) == CP_CLIP,
           "medkit not applicable (carrying 2): clip");
        CK(CpPickItem(&s, used0, 0) == -1, "nothing applicable");
        int full[CP_N] = { 2, 3, 2, 0, 0 };
        CK(CpPickItem(&s, full, 7) == -1, "all caps used");
        CpParseTail("pk=1 pkc0=4 pkn0=1", &s);
        CK(CpPickItem(&s, used0, 7) == -1,
           "stamina is passive: never a streak grant");
    }
    CK(CpClipExtra(30, 40, 0) == 30 && CpClipExtra(0, 0, 0) == 6
       && CpClipExtra(30, 50, 60) == 10 && CpClipExtra(30, 60, 60) == 0
       && CpClipExtra(30, 70, 60) == 0,
       "clip: one magazine, capped by the reserve cap");
    // packing
    int u[CP_N] = { 2, 3, 1, 1, 2 }, l31, l32;
    CpPackUses(u, &l31, &l32);
    CK(l31 == (2 | (3 << 4) | (1 << 8))
       && l32 == (1 | (2 << 4)), "labels 31/32: %x %x", l31, l32);
    int u2[CP_N] = { 99, -4, 0, 0, 0 };
    CpPackUses(u2, &l31, &l32);
    CK(l31 == 15 && l32 == 0, "uses clamp to a nibble");
    // card text
    char b[120];
    CpCardText(b, sizeof b, "Tyler", 1, 0, 0);
    CK(!strcmp(b, "TYLER PROVIDED YOU WITH A MEDKIT"), "%s", b);
    CpCardText(b, sizeof b, "Dina", 2, 1, 1);
    CK(!strcmp(b, "DINA PRZYSLALA CI ZAPASOWY MAGAZYNEK"), "%s", b);
    CpCardText(b, sizeof b, "Tyler", 1, 0, 1);
    CK(!strcmp(b, "TYLER PRZYSLAL CI APTECZKE"), "%s", b);
    CpCardText(b, sizeof b, NULL, 5, 0, 0);
    CK(!strcmp(b, "YOUR CLAN BOUGHT YOU TIME"), "%s", b);
    CpCardText(b, 12, "Tyler", 1, 0, 0);
    CK(strlen(b) == 11, "truncates safely");
    // summary
    CpParseTail(t, &s);
    CpSummary(b, sizeof b, &s, 0);
    CK(!strcmp
       (b,
        "medkit x2 (Lila B), clip x3 (Wendy), sprint recovery x1 (Arnaldo)"),
       "%s", b);
    CpSummary(b, 20, &s, 0);
    CK(strlen(b) < 20, "summary fits its buffer");
    // voice gate
    CK(CpVoiceDue(5000, 0) && !CpVoiceDue(19999, 0 + 1)
       && CpVoiceDue(21001, 1000)
       && !CpVoiceDue(20999, 1000), "one voice line per 20 s");
    printf(fails ? "%d FAILED\n" : "all clanperk checks pass\n", fails);
    return fails != 0;
}
