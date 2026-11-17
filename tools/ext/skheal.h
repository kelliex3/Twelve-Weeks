// skheal.h - receive-side validation of MSG_SK_HEAL. Pure (no engine calls) so tools/ext/tests/skheal_test.c can check it.
// The sender's rules (skills.c sk_marks): First-Aid Training II = +10 per tick, III = +20, one tick per FAT_TICK_MS (1000 ms), a teammate within
// 1.5 m horizontally / 1.2 m vertically to start a hold, 1.8 / 1.5 m to keep it (the heal plays the revive hand-out gesture; was 2.0 / 1.5 m), healer up and
// not downed. The receiver allows network / movement slack (3 m / 2 m, was 5 / 3; ticks no closer than 400 ms).
#ifndef SKHEAL_H
#define SKHEAL_H
#include <stdint.h>
#define SKH_HOLD_XZ    1.5f     // sender: start a heal hold within this (horizontal / vertical), was 2.0 / 1.5 m
#define SKH_HOLD_Y     1.2f
#define SKH_KEEP_XZ    1.8f     // sender: a running hold (and its hand-out anim) ends beyond this
#define SKH_KEEP_Y     1.5f
#define SKH_RANGE_XZ   3.0f     // receiver: network / movement slack on top of SKH_KEEP_XZ (was 5 m; old clients send within 2.0 m, so they stay accepted)
#define SKH_RANGE_Y    2.0f     // (was 3 m)
#define SKH_MIN_GAP_MS 400
enum { SKH_OK =
        0, SKH_BAD_SLOT, SKH_NOT_TEAMMATE, SKH_SENDER_DOWNED,
        SKH_BAD_AMOUNT, SKH_SKILL_LOW, SKH_TOO_FAR, SKH_RATE };
// lv_known = the sender's MSG_SK_SYNC has arrived (else the level check is skipped: only the 10 / 20 values are allowed); lv = their First-Aid level
// have_pos = both positions known (else the range check is skipped); dx/dy/dz = sender - us; last_ms = time of the last accepted tick from them (0 = none)
static inline int SkhCheck(int slot_ok, int teammate, int sender_downed,
                            int amount, int lv_known, int lv, int have_pos,
                            float dx, float dy, float dz, uint64_t now,
                            uint64_t last_ms)
{
    if (!slot_ok)
        return SKH_BAD_SLOT;
    if (!teammate)
        return SKH_NOT_TEAMMATE;
    if (sender_downed)
        return SKH_SENDER_DOWNED;
    if (amount < 1 || amount > 20)
        return SKH_BAD_AMOUNT;
    if (lv_known) {
        if (lv < 2)
            return SKH_SKILL_LOW;
        if (amount != (lv >= 3 ? 20 : 10))
            return SKH_BAD_AMOUNT;      // exactly the skill's tick (the sender only ever sends 10 or 20)
    } else if (amount != 10 && amount != 20)
        return SKH_BAD_AMOUNT;
    if (have_pos
        && (dx * dx + dz * dz > SKH_RANGE_XZ * SKH_RANGE_XZ
            || dy < -SKH_RANGE_Y || dy > SKH_RANGE_Y))
        return SKH_TOO_FAR;
    if (last_ms && now - last_ms < SKH_MIN_GAP_MS)
        return SKH_RATE;
    return SKH_OK;
}
#endif
