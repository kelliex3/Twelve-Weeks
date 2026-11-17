// Offline check of skheal.h (MSG_SK_HEAL receive validation).
// Build: gcc -std=gnu2x -O2 -o skheal_test skheal_test.c && ./skheal_test   (exit 0 = all pass)
#include <stdio.h>
#include "../skheal.h"
static int fails;
#define T(exp, ...) do { int r = SkhCheck(__VA_ARGS__); if (r != (exp)) { printf("FAIL line %d: got %d want %d\n", __LINE__, r, (exp)); fails++; } } while (0)
int main(void)
{
    //      slot teammate downed amt known lv pos  dx dy dz  now  last
    T(SKH_OK, 1, 1, 0, 10, 1, 2, 1, 1.0f, 0, 0.5f, 5000, 0);    // II, 10, close
    T(SKH_OK, 1, 1, 0, 20, 1, 3, 1, 0, 0, 0, 5000, 3900);       // III, 20, a tick later
    T(SKH_OK, 1, 1, 0, 10, 0, 0, 0, 99, 99, 99, 5000, 0);       // level + position unknown: only 10 / 20 allowed
    T(SKH_BAD_SLOT, 0, 1, 0, 10, 1, 2, 1, 0, 0, 0, 5000, 0);
    T(SKH_NOT_TEAMMATE, 1, 0, 0, 10, 1, 2, 1, 0, 0, 0, 5000, 0);        // an enemy cannot heal us
    T(SKH_SENDER_DOWNED, 1, 1, 1, 10, 1, 2, 1, 0, 0, 0, 5000, 0);
    T(SKH_BAD_AMOUNT, 1, 1, 0, 0, 1, 2, 1, 0, 0, 0, 5000, 0);
    T(SKH_BAD_AMOUNT, 1, 1, 0, 255, 1, 3, 1, 0, 0, 0, 5000, 0); // the full-heal cheat
    T(SKH_BAD_AMOUNT, 1, 1, 0, 21, 0, 0, 1, 0, 0, 0, 5000, 0);
    T(SKH_BAD_AMOUNT, 1, 1, 0, 15, 0, 0, 1, 0, 0, 0, 5000, 0);  // unknown level: still only 10 / 20
    T(SKH_BAD_AMOUNT, 1, 1, 0, 20, 1, 2, 1, 0, 0, 0, 5000, 0);  // II may only send 10
    T(SKH_BAD_AMOUNT, 1, 1, 0, 10, 1, 3, 1, 0, 0, 0, 5000, 0);  // III sends 20
    T(SKH_SKILL_LOW, 1, 1, 0, 10, 1, 1, 1, 0, 0, 0, 5000, 0);   // I cannot heal teammates
    T(SKH_SKILL_LOW, 1, 1, 0, 10, 1, 0, 1, 0, 0, 0, 5000, 0);   // no First-Aid at all
    T(SKH_TOO_FAR, 1, 1, 0, 10, 1, 2, 1, 6.0f, 0, 0, 5000, 0);  // across the map
    T(SKH_TOO_FAR, 1, 1, 0, 10, 1, 2, 1, 0, 4.0f, 0, 5000, 0);
    T(SKH_OK, 1, 1, 0, 10, 1, 2, 1, 2.9f, -1.9f, 0, 5000, 0);
    T(SKH_TOO_FAR, 1, 1, 0, 10, 1, 2, 1, 3.1f, 0, 0, 5000, 0);  // beyond the receiver slack (the sender keeps within 1.8 m)
    T(SKH_RATE, 1, 1, 0, 10, 1, 2, 1, 0, 0, 0, 5200, 5000);     // 200 ms after the last
    T(SKH_OK, 1, 1, 0, 10, 1, 2, 1, 0, 0, 0, 5400, 5000);
    printf(fails ? "%d FAILED\n" : "all skheal checks pass\n", fails);
    return fails != 0;
}
