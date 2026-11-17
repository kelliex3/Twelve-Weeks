// Offline test of slotq.h (the early-send queue, re/netcode.md "slot 255") against a model of winmm's SendEventRaw / HandleNetEvent:
// the event is stamped with the sender's slot at ENQUEUE; a receiver drops sender >= 8. Build: gcc -std=gnu2x -O2 -o slotq_test slotq_test.c && ./slotq_test
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../slotq.h"
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
typedef struct {
    uint8_t sender, type, len, dst, d0;
} WireT;
static WireT wire[64];
static int nwire;
static uint32_t own_slot = 0xff;
static void WinmmSend(uint8_t type, const uint8_t *b, uint64_t len,
                       uint8_t dst)
{
    WireT *w = &wire[nwire++];
    w->sender = (uint8_t) own_slot;
    w->type = type;
    w->len = (uint8_t) len;
    w->dst = dst;
    w->d0 = len ? b[0] : 0;
}

static void cb(void *c, uint8_t t, const uint8_t *b, uint64_t l, uint8_t d)
{
    (void) c;
    WinmmSend(t, b, l, d);
}

static SqT q;
static void gate(uint8_t type, const uint8_t *b, uint64_t len, uint8_t dst,
                 uint64_t now, int sup)
{                               // = send_gate
    if (SqShouldHold(own_slot, type)
        && SqHold(&q, type, b, len, dst, now, sup))
        return;
    WinmmSend(type, b, len, dst);
}

static int Received(void)
{
    int ok = 0;
    for (int i = 0; i < nwire; i++)
        if (wire[i].sender < 8)
            ok++;
    return ok;
}

int main(void)
{
    uint8_t look[7] = { 1, 2, 3, 1, 0, 3, 1 };
    // 1. without the queue (old behaviour): an own-spawn look sent at slot 255 reaches nobody
    nwire = 0;
    q = (SqT) {
    0};
    WinmmSend(0x58, look, 7, 0xff);
    CHECK(Received() == 0,
          "old path must be dropped by the receivers (sender 255)");
    // 2. with the queue: held while 255, sent in order with the real slot
    nwire = 0;
    q = (SqT) {
    0};
    own_slot = 0xff;
    uint8_t a[2] = { 0xaa, 1 }, b[3] = { 0xbb, 2, 3 };
    gate(0x58, look, 7, 0xff, 1000, 1);
    gate(0x5B, a, 2, 0xff, 1001, 0);
    gate(0x43, b, 3, 2, 1002, 0);
    CHECK(nwire == 0 && q.held == 3, "3 messages held, none on the wire");
    gate(0x10, a, 2, 0xff, 1003, 0);    // winmm's own type (< 0x40) is never held
    CHECK(nwire == 1
          && wire[0].type == 0x10, "winmm type passes straight through");
    nwire = 0;
    own_slot = 2;               // session starts
    CHECK(SqFlush(&q, 1500, cb, NULL) == 3, "flush sends 3");
    CHECK(wire[0].type == 0x58 && wire[1].type == 0x5B
          && wire[2].type == 0x43 && wire[2].dst == 2 && wire[2].len == 3
          && wire[2].d0 == 0xbb, "order / dst / payload kept");
    CHECK(Received() == 3,
          "every replayed message carries slot 2 and is accepted");
    CHECK(SqFlush(&q, 1501, cb, NULL) == 0, "nothing sent twice");
    // 3. a slot that is valid from the start never queues
    nwire = 0;
    q = (SqT) {
    0};
    gate(0x58, look, 7, 0xff, 5, 1);
    CHECK(nwire == 1 && q.held == 0, "valid slot: straight through");
    // 4. supersede (look resent every second) keeps only the newest; exact duplicates are not stored twice
    own_slot = 0xff;
    nwire = 0;
    q = (SqT) {
    0};
    for (int i = 0; i < 5; i++) {
        look[6] = (uint8_t) (10 + i);
        gate(0x58, look, 7, 0xff, 100 + i, 1);
    }
    uint8_t h[4] = { 0 };
    for (int i = 0; i < 5; i++)
        gate(0x91, h, 4, 0xff, 200 + i, 0);
    int used = 0;
    for (int i = 0; i < SQ_CAP; i++)
        used += q.e[i].used;
    CHECK(used == 2
          && q.dup == 4, "1 look + 1 hello held, got %d (dup %u)", used,
          q.dup);
    int newest = 0;
    for (int i = 0; i < SQ_CAP; i++)
        if (q.e[i].used && q.e[i].type == 0x58)
            newest = q.e[i].data[6];
    CHECK(newest == 14, "the newest look copy survives, got %d", newest);
    own_slot = 3;
    nwire = 0;
    SqFlush(&q, 300, cb, NULL);
    CHECK(nwire == 2 && Received() == 2, "newest look + hello sent");
    // 5. expiry: a session that never started must not leak into the next one
    own_slot = 0xff;
    nwire = 0;
    q = (SqT) {
    0};
    gate(0x58, look, 7, 0xff, 1000, 0);
    gate(0x5B, a, 2, 0xff, 9000, 0);
    own_slot = 1;
    SqFlush(&q, 12000, cb, NULL);
    CHECK(nwire == 1 && wire[0].type == 0x5B
          && q.dropped_old == 1,
          "10 s old message dropped, young one sent");
    own_slot = 0xff;
    q = (SqT) {
    0};
    gate(0x58, look, 7, 0xff, 0, 0);
    CHECK(SqExpire(&q, 5000) == 0
          && SqExpire(&q, 10001) == 1, "expire w/o slot");
    // 6. full queue drops the oldest, oversize payload is refused (the caller then sends it straight)
    q = (SqT) {
    0};
    nwire = 0;
    for (int i = 0; i < SQ_CAP + 3; i++) {
        uint8_t x[2] = { (uint8_t) i, 7 };
        gate(0x70, x, 2, 0xff, 10 + i, 0);
    }
    CHECK(q.dropped_full == 3, "3 oldest dropped, got %u", q.dropped_full);
    own_slot = 4;
    nwire = 0;
    SqFlush(&q, 100, cb, NULL);
    CHECK(nwire == SQ_CAP && wire[0].d0 == 3
          && wire[SQ_CAP - 1].d0 == SQ_CAP + 2,
          "survivors are the newest, in order");
    uint8_t big[201] = { 0 };
    CHECK(SqHold(&q, 0x70, big, 201, 0xff, 1, 0) == 0,
          "oversize refused");
    // 7. the host (slot 0) is a valid slot: never held
    own_slot = 0;
    q = (SqT) {
    0};
    nwire = 0;
    gate(0x58, look, 7, 0xff, 1, 1);
    CHECK(nwire == 1 && wire[0].sender == 0
          && Received() == 1, "host slot 0 is a valid slot");
    printf(fails ? "slotq_test: %d FAILED\n" :
           "slotq_test: all checks passed\n", fails);
    return fails != 0;
}
