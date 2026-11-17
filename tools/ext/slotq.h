// Early-send queue (re/netcode.md "slot 255"). winmm stamps every event it queues with the sender's own session slot AT ENQUEUE
// (net-session.cpp SendEventRaw: e.sender = g_mySlot). That slot (0x180138090) is 0xFF until the session starts, one frame after the match
// start - after the intro spawn, where the ext already announces itself (faction look, stow, backpack, ...). The receivers get sender 255
// (winmm's own HandleNetEvent drops senderSlot >= max; the ext's receivers range-check the slot) and the message is lost; only features
// that resend (look every second) recover. This holds our own ext messages (type >= SQ_MIN_TYPE) while the slot is unset and sends them
// in order once it is known; a held message older than SQ_TTL_MS is dropped (a session that never started must not leak into the next).
// Pure C: tools/ext/tests/slotq_test.c compiles this file.
#ifndef SLOTQ_H
#define SLOTQ_H
#include <stdint.h>
#include <string.h>
#define SQ_MIN_TYPE 0x40u       // ext message ids start at 0x40; winmm's own events (< 0x40) keep their path
#define SQ_CAP 24
#define SQ_MAX_LEN 200          // kMaxEventPayload
#define SQ_TTL_MS 10000u
#define SQ_NO_SLOT 0xffu
typedef struct {
    uint8_t used, type, dst;
    uint8_t len;
    uint64_t ms;
    uint8_t data[SQ_MAX_LEN];
} SqEntT;
typedef struct {
    SqEntT e[SQ_CAP];
    uint32_t held, dropped_full, dropped_old, replayed, dup;
} SqT;
static inline int SqShouldHold(uint32_t own_slot, uint64_t type)
{
    return own_slot == SQ_NO_SLOT && (uint8_t) type >= SQ_MIN_TYPE;
}

// Hold one message. Returns 1 held. An exact duplicate of a held entry (same type / dst / bytes: a resend inside the same
// slot-less window) is not stored twice; `supersede` replaces every held copy of that type / dst by this one (counted in dup, still returns 1). A full queue drops the OLDEST entry.
static inline int SqHold(SqT *q, uint8_t type, const uint8_t *buf,
                          uint64_t len, uint8_t dst, uint64_t now_ms,
                          int supersede)
{
    if (len > SQ_MAX_LEN)
        return 0;
    if (supersede)              // a state message that is resent every second: only the newest copy is worth sending
        for (int i = 0; i < SQ_CAP; i++)
            if (q->e[i].used && q->e[i].type == type && q->e[i].dst == dst)
                q->e[i].used = 0;
    for (int i = 0; i < SQ_CAP; i++)
        if (q->e[i].used && q->e[i].type == type && q->e[i].dst == dst
            && q->e[i].len == len && (!len
                                      || (buf
                                          && !memcmp(q->e[i].data, buf,
                                                     len)))) {
            q->dup++;
            return 1;
        }
    int slot = -1, oldest = 0;
    for (int i = 0; i < SQ_CAP; i++) {
        if (!q->e[i].used) {
            slot = i;
            break;
        }
        if (q->e[i].ms < q->e[oldest].ms)
            oldest = i;
    }
    if (slot < 0) {
        slot = oldest;
        q->dropped_full++;
    }
    SqEntT *e = &q->e[slot];
    e->used = 1;
    e->type = type;
    e->dst = dst;
    e->len = (uint8_t) len;
    e->ms = now_ms;
    if (len && buf)
        memcpy(e->data, buf, len);
    q->held++;
    return 1;
}

typedef void (*sq_send_fn)(void *ctx, uint8_t type, const uint8_t * buf,
                           uint64_t len, uint8_t dst);
// The slot is known: send everything held, oldest first (entries older than SQ_TTL_MS are dropped). Returns how many were sent.
static inline int SqFlush(SqT *q, uint64_t now_ms, sq_send_fn send,
                           void *ctx)
{
    int n = 0;
    for (;;) {
        int best = -1;
        for (int i = 0; i < SQ_CAP; i++)
            if (q->e[i].used && (best < 0 || q->e[i].ms < q->e[best].ms))
                best = i;
        if (best < 0)
            break;
        SqEntT e = q->e[best];
        q->e[best].used = 0;
        if (now_ms - e.ms > SQ_TTL_MS) {
            q->dropped_old++;
            continue;
        }
        send(ctx, e.type, e.data, e.len, e.dst);
        n++;
        q->replayed++;
    }
    return n;
}

// Expire without a slot (nothing ever starts): keeps the queue from holding stale state. Returns the number dropped.
static inline int SqExpire(SqT *q, uint64_t now_ms)
{
    int n = 0;
    for (int i = 0; i < SQ_CAP; i++)
        if (q->e[i].used && now_ms - q->e[i].ms > SQ_TTL_MS) {
            q->e[i].used = 0;
            q->dropped_old++;
            n++;
        }
    return n;
}
#endif
