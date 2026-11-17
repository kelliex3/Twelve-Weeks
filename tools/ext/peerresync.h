// "New peer seen" resync scheduler (re/jip-state-resync-1019.md). Many ext features send their state only on a change, or a few times at the
// match start; a player who JOINS a running match (JIP), a client after a HOST MIGRATION (winmm resets every event channel there) and a peer that
// came back after a drop never gets the current state. This is the one generic hook: the tick tells it which slots are "established" (peer record
// active + puppet handle set), it notices the 0 -> 1 edge of each slot (also the first observation after a match start / a process start) and, after
// a short delay (the newcomer's receivers need their puppet / session first) and once more later (loss), hands the slot out through rs_next(); the
// caller then asks every feature to re-send its CURRENT state to that slot (unicast). rs_all() = the same for every established slot (host migration
// done). Receivers must be idempotent (all of them are: look / downed / armour / skills carry epochs or are plain "current value" messages).
// Rate limits: one slot handed out per rs_next() call (the caller calls it once per frame), a schedule is two steps only and a re-arm (a flapping link)
// REPLACES the pending schedule (never adds to it), so a slot costs at most two resyncs per 0.7 s however often it flaps. Pure C: tools/ext/tests/peerresync_test.c compiles this file.
#ifndef PEERRESYNC_H
#define PEERRESYNC_H
#include <stdint.h>
#include <string.h>
#define RS_N 8                  // = T2R_MAX_PEERS
#define RS_STEPS 2
#define RS_GAP_MS 90u           // minimum time between two handed-out slots: a host migration / a JIP joiner with 7 peers is spread over ~0.6 s instead of 7 frames
static const uint32_t RS_DELAY_MS[RS_STEPS] = { 700u, 3500u };  // after the slot turned up: first resync / second resync (loss insurance)

typedef struct {
    uint8_t was[RS_N], left[RS_N];
    uint64_t next[RS_N], last;  // last = when the previous slot was handed out (RS_GAP_MS spacing)
    uint32_t started, handed, bulk;
} RsT;
static inline void RsReset(RsT *r)
{
    memset(r, 0, sizeof *r);
}

static inline void RsArm(RsT *r, uint32_t s, uint64_t now)
{
    r->left[s] = RS_STEPS;
    r->next[s] = now + RS_DELAY_MS[0];
}

// Report one slot's state once per tick. Returns 1 when a new schedule started for it.
static inline int RsSeen(RsT *r, uint32_t s, int established,
                          uint64_t now, uint32_t own_slot)
{
    if (s >= RS_N)
        return 0;
    int edge = established && !r->was[s] && s != own_slot;
    r->was[s] = (uint8_t) (established && s != own_slot);
    if (!edge) {
        if (!established)
            r->left[s] = 0;
        return 0;
    }                           // a slot that left drops its pending resyncs
    RsArm(r, s, now);
    r->started++;
    return 1;
}

// Host migration finished (or any "everybody may have lost our state" event): schedule every slot that is currently established.
static inline void RsAll(RsT *r, uint64_t now)
{
    for (uint32_t s = 0; s < RS_N; s++)
        if (r->was[s]) {
            RsArm(r, s, now);
            r->bulk++;
        }
}

// The next slot whose resync is due (at most one per call, and at most one per RS_GAP_MS: after a host migration every client
// re-sends ~10 messages to each of up to 7 peers; handed out one per FRAME that was ~70 messages from each of 8 clients inside 120 ms while the
// Steam sessions are still settling). Returns 1 and fills *slot, 0 = nothing due.
static inline int RsNext(RsT *r, uint64_t now, uint32_t *slot)
{
    if (r->last && now < r->last + RS_GAP_MS)
        return 0;
    for (uint32_t s = 0; s < RS_N; s++) {
        if (!r->left[s] || !r->was[s] || now < r->next[s])
            continue;
        uint32_t step = RS_STEPS - r->left[s];
        r->left[s]--;
        if (r->left[s])
            r->next[s] = now + (RS_DELAY_MS[step + 1] - RS_DELAY_MS[step]);
        r->handed++;
        r->last = now ? now : 1;
        *slot = s;
        return 1;
    }
    return 0;
}
#endif
