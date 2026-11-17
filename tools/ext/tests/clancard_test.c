// Offline check of clancard.h (generic corner card: status tail parse, moment gate, card text, event ids).
// Build: gcc -std=gnu2x -O2 -o clancard_test clancard_test.c && ./clancard_test   (exit 0 = all pass)
#include <stdio.h>
#include "../clancard.h"
static int fails;
#define CK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
int main(void)
{
    CcStatusT s;
    // the exact tail tools/clan_status.wire_text sends, behind a state text and a perk tail
    const char *t =
        "v=1 fac=1 st=1 day=9 lmh=77 pk=1 pkc0=1 pkn0=2 pkg0=1 pkl0=1 pkr0=1 pkm0=Lila_B sc=2 sci0=1 scd0=9 scg0=0 scl0=2 sci1=4 scd1=9 scg1=1 scl1=3 scm0=Cameron scm1=Ava_K";
    CcParseStatus(t, &s);
    CK(s.n == 2, "two updates (%d)", s.n);
    CK(s.e[0].code == CC_HUNGRY && s.e[0].day == 9 && s.e[0].look == 2
       && !s.e[0].female
       && !strcmp(s.e[0].name, "Cameron"), "first: %d %s", s.e[0].code,
       s.e[0].name);
    CK(s.e[1].code == CC_DEAD && s.e[1].female
       && !strcmp(s.e[1].name, "Ava K"), "second: %d %s", s.e[1].code,
       s.e[1].name);
    CcParseStatus("v=1 fac=1 day=9 pk=1 pkc0=1 pkn0=1", &s);
    CK(s.n == 0, "old daemon: no updates");
    CcParseStatus(NULL, &s);
    CK(s.n == 0, "NULL");
    CcParseStatus("x sc=9 sci0=9 scd0=1 sci1=3 scm1=Z sci7=1 scx=1", &s);
    CK(s.n == 1 && s.e[0].code == CC_SICK
       && !strcmp(s.e[0].name, "Z"),
       "bad code 9 skipped, bad index ignored (%d)", s.n);
    CcParseStatus
        (" sc=1 sci0=2 scd0=3 scm0=ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEF", &s);
    CK(s.n == 1 && strlen(s.e[0].name) == 23, "names cut to 23");
    CcParseStatus(" sc=1 sci0=2 scx0=8 scd0=3 scm0=Zed", &s);
    CK(s.n == 1
       && s.e[0].code == CC_DEPRESSED,
       "scx0=8: the extended code (depressed) wins over sci0=2");
    CcParseStatus(" sc=1 sci0=2 scx0=9 scd0=3 scm0=Zed", &s);
    CK(s.n == 1 && s.e[0].code == CC_SHOCK, "scx0=9: shock");
    CcParseStatus(" sc=1 sci0=2 scd0=3 scm0=Zed", &s);
    CK(s.n == 1 && s.e[0].code == CC_SAD, "an old daemon's sad stays sad");
    CcParseStatus(" sc=1 sci0=9 scx0=7 scd0=3", &s);
    CK(s.n == 0, "sci0=9 without a valid extended code is still dropped");
    CcParseStatus(" sc=1 sci0=2 scd0=3", &s);
    CK(s.n == 1 && !strcmp(s.e[0].name, "Someone"), "no name: Someone");
    // the gate
    CK(!CcAllowed(CC_HUNGRY, 0, 0) && !CcAllowed(CC_HUNGRY, 1, 0)
       && !CcAllowed(CC_DEAD, 1, 1),
       "status: never a card (ESC menu list only)");
    CK(!CcAllowed(CC_PERK, 0, 0) && !CcAllowed(CC_PERK, 0, 1)
       && CcAllowed(CC_PERK, 1, 1)
       && !CcAllowed(CC_PERK, 1, 0),
       "perk: only in the match at the spawn moment");
    // card text + expressions
    CcCardT c;
    CcStatusCard(&c, CC_SICK, "Rosa", 1, 0, 1);
    CK(!strcmp(c.line, "ZACHOROWALA")
       && c.expr == CC_EX_SICK, "polish female: %s", c.line);
    CcStatusCard(&c, CC_SICK, "Marek", 0, 0, 1);
    CK(!strcmp(c.line, "ZACHOROWAL"), "polish male");
    CcStatusCard(&c, CC_DEAD, "Ava", 1, 0, 0);
    CK(!strcmp(c.line, "DID NOT MAKE IT")
       && c.expr == CC_EX_DEAD, "english dead");
    CK(CcExprOf(CC_PERK) == CC_EX_HAPPY
       && CcExprOf(CC_NEW) == CC_EX_NEUTRAL,
       "perk = happy, new = neutral");
    CK(CcEventId("Ava", CC_DEAD, 9) != CcEventId("Ava", CC_SICK, 9)
       && CcEventId("Ava", CC_DEAD, 9) == CcEventId("Ava", CC_DEAD, 9)
       && CcEventId("Ava", CC_DEAD, 9) != CcEventId("Ava", CC_DEAD,
                                                        10),
       "event ids differ per code and day");
    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
