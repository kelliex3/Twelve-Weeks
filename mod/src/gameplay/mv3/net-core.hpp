// Ported verbatim from tools/ext/mv3_net_core.h (melee-v3 bubble session + rollback
// netcode, the engine-free part). Kept header-only with the ext's static functions and
// pinned contract names (the ext includes this in mv3_net.c and tools/mv3/nettest.c); the
// namespace is the only change. Every engine contact is through the Mv3nIo callbacks, so
// the module stays inert until a bridge supplies them. Wire id NEVT_MV3 = 0x9E is reserved
// in net-protocol.hpp. Pads / animation / FX / camera and the ext cutover stay gated.
#pragma once

#include "types.hpp"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace melee::mv3 {

#ifndef MV3N_LOGCAP
#define MV3N_LOGCAP(c, n) ((c)++ < (n))
#endif

#define MV3N_SLOTS 8
#define MV3N_IH 256             // per-slot input ring (frames): > MV3_HIST + MV3_DELAY_MAX + catch-up
#define MV3N_PKT 120            // max payload
#define MV3N_K 7                // input frames per INPUT packet: 34 + 12 * 7 = 118 bytes
#define MV3N_HDR 34
#define MV3N_HASH_EVERY 30
#define MV3N_CTLQ 32
#define MV3N_CHUNK 100          // SNAP fragment payload
#define MV3N_INLEN 20
#define MV3N_FRAME_US(f) ((int64_t)(f) * 50000 / 3)     // 1e6 / 60
#define MV3N_CATCHUP 8          // max sim steps per tick going forward
#define MV3N_STALL_END_US 2500000       // stalled this long (a member's inputs stopped) -> the session ends
#define MV3N_SILENT_US 3000000
#define MV3N_FAR_MM 4000        // leave rule: nearest enemy > 4 m ...
#define MV3N_FAR_FRAMES 60      // ... for 1 s
#define MV3N_JOIN_LEAD 30       // a join lands >= 30 frames after the master's current frame

enum { MV3N_OP_INPUT =
        1, MV3N_OP_PROPOSE, MV3N_OP_ACCEPT, MV3N_OP_START,
        MV3N_OP_REDIRECT, MV3N_OP_JOINREQ, MV3N_OP_JOIN, MV3N_OP_SNAP,
        MV3N_OP_CACK, MV3N_OP_END,
    MV3N_OP_PING, MV3N_OP_PONG
};
enum { MV3N_IDLE, MV3N_PROPOSING, MV3N_ACCEPTED, MV3N_JOINREQ,
        MV3N_JOINWAIT, MV3N_ACTIVE, MV3N_LINGER };
enum { MV3N_END_OVER =
        1, MV3N_END_LEFT, MV3N_END_MATCH, MV3N_END_STALL, MV3N_END_SILENT,
        MV3N_END_DESYNC, MV3N_END_ABORT };
static const char *const MV3N_END_NAME[] =
    { "?", "bubble over", "left", "match end", "stall", "peer silent",
"desync storm", "aborted" };

typedef struct {
    void *user;
    void (*send)(void *user, uint32_t to, const uint8_t * b, int len);
    void (*input)(void *user, Mv3In * out);    // mv3_glue_input
    void (*events)(void *user, const Mv3Evbuf * ev, int resim);        // mv3_glue_events
    void (*rolled_back)(void *user, uint32_t to_frame); // mv3_glue_rolled_back
    void (*frame)(void *user, const Mv3World * w);     // mv3_glue_frame
     int16_t(*hp) (void *user); // our hp x10 now (bubble start / join)
    int (*team_of)(void *user, uint32_t slot);
    float (*rtt_ms)(void *user, uint32_t slot); // prior RTT (psync pings), <= 0 unknown
    void (*log)(void *user, const char *msg);
    void (*hash_note)(void *user, uint32_t frame, uint32_t h);  // optional (tests): every confirmed hash computed
} Mv3nIo;

// hp_set / hp: the owner's engine hp x10 changed at this frame (mv3_net_set_hp) - every sim sets the fighter's hp before stepping it; never predicted
typedef struct {
    uint32_t f;
    uint8_t known, leave, used_ok, hp_set, used_hp_set, pad[3];
    int16_t hp, used_hp;
    Mv3In in, used;
} Mv3nIent;

typedef struct {
    Mv3nIent r[MV3N_IH];
    uint32_t first, contig, hi; // inputs first..contig all known; hi = highest known
    Mv3In base;
    uint8_t has_base;           // prediction when nothing is known yet
    // network (this slot as a peer)
    uint8_t dest;               // our INPUT / PING go to it
    uint32_t acked;             // it holds our inputs up to here
    uint32_t sent_hi;
    uint64_t last_tx_us, last_rx_us, last_ping_us, left_us;
    uint32_t left_f;
    uint32_t alt;
    struct {
        int64_t d, rtt;
    } smp[16];
    int smp_i, smp_n;
    int64_t d_us, rtt_us;
    int have_d;
    int64_t their_off;
    float adv_e;
    int8_t my_adv;
    uint32_t rx_hf, rx_h;
    uint8_t rx_have;
    uint32_t cmp_hf, cmp_floor;
    uint64_t last_snap_us, invite_us;
    uint16_t seen[32];
    uint8_t seen_i;
} Mv3nPeer;

typedef struct {
    Mv3nIo io;
    uint8_t me;
    int state;
    uint16_t sess, prop_id, prop_ctr, cseq;
    uint8_t prop_slot, prop_master, delay, my_delay;
    uint64_t prop_us;
    uint32_t seed;
    int64_t t0, off, trim;      // us
    uint32_t cur, conf, floor, end_f, leave_f, own_hi, hp_chg_f;
    int16_t own_hp, hp_sent;    // our engine hp as last reported / as last put in our input stream (-1 unknown)
    int end_why;
    char leave_why[32];
    uint64_t start_us, end_wait_us, stall_us, linger_us, act_us;
    Mv3World w;
    Mv3World snap[MV3_HIST];
    uint32_t snap_f[MV3_HIST];
    Mv3Evbuf ev;
    Mv3nPeer p[MV3N_SLOTS];
    struct {
        uint32_t f;
        uint8_t slot, team, on, pad;
        int16_t hp;
        Mv3In in;
    } j[MV3_MAX_F];
    struct {
        uint32_t slot, f;
        uint8_t on;
    } jsnap[MV3_MAX_F];         // master: the joiner waits for our world at f
    struct {
        uint32_t f, h;
    } myh[8];
    uint32_t next_hf;
    uint32_t rb_to;
    uint8_t rb_pending;
    uint32_t far_n;
    struct {
        uint8_t on, dest, len, ts_off;
        uint16_t cq;
        uint64_t next_us;
        int tries;
        uint8_t b[MV3N_PKT];
    } q[MV3N_CTLQ];
    struct {
        uint8_t from, cnt, reason;
        uint32_t f, mask;
        uint8_t d[sizeof(Mv3World) + MV3N_CHUNK];
    }
    srx;
    // stats: s_* session totals, w_* the current 5 s window
    uint32_t s_rb, s_rbf, s_rbmax, s_stall, s_desync, s_resync, s_steps;
    uint32_t w_rb, w_rbf, w_rbmax, w_stall, w_desync, w_late;
    uint64_t w_us;
    uint32_t n_logs[16];
}
Mv3nCtx;

// ---------------------------------------------------------------- small helpers
static void Mv3nLog(Mv3nCtx *c, const char *fmt, ...)
{
    char b[400];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    if (c->io.log)
        c->io.log(c->io.user, b);
}

static uint32_t Mv3nRec16(uint16_t x, uint32_t ref)
{
    return ref + (uint32_t) (int32_t) (int16_t) (uint16_t) (x -
                                                            (uint16_t)
                                                            ref);
}

static void Mv3nW16(uint8_t *b, uint16_t v)
{
    memcpy(b, &v, 2);
}

static void Mv3nW32(uint8_t *b, uint32_t v)
{
    memcpy(b, &v, 4);
}

static void Mv3nW64(uint8_t *b, uint64_t v)
{
    memcpy(b, &v, 8);
}

static uint16_t Mv3nR16(const uint8_t *b)
{
    uint16_t v;
    memcpy(&v, b, 2);
    return v;
}

static uint32_t Mv3nR32(const uint8_t *b)
{
    uint32_t v;
    memcpy(&v, b, 4);
    return v;
}

static uint64_t Mv3nR64(const uint8_t *b)
{
    uint64_t v;
    memcpy(&v, b, 8);
    return v;
}

static void Mv3nInPut(uint8_t *b, const Mv3In *in)
{
    b[0] = in->btn;
    b[1] = (uint8_t) in->sx;
    b[2] = (uint8_t) in->sy;
    b[3] = in->wset;
    Mv3nW32(b + 4, (uint32_t) in->px);
    Mv3nW32(b + 8, (uint32_t) in->pz);
    Mv3nW32(b + 12, (uint32_t) in->py);
    Mv3nW16(b + 16, (uint16_t) in->yaw);
    Mv3nW16(b + 18, in->flags);
}

static void Mv3nInGet(Mv3In *in, const uint8_t *b)
{
    memset(in, 0, sizeof *in);
    in->btn = b[0];
    in->sx = (int8_t) b[1];
    in->sy = (int8_t) b[2];
    in->wset = b[3];
    in->px = (int32_t) Mv3nR32(b + 4);
    in->pz = (int32_t) Mv3nR32(b + 8);
    in->py = (int32_t) Mv3nR32(b + 12);
    in->yaw = (int16_t) Mv3nR16(b + 16);
    in->flags = Mv3nR16(b + 18);
}

static int Mv3nInEq(const Mv3In *a, const Mv3In *b)
{
    return a->btn == b->btn && a->sx == b->sx && a->sy == b->sy
        && a->wset == b->wset && a->px == b->px && a->pz == b->pz
        && a->py == b->py && a->yaw == b->yaw && a->flags == b->flags;
}

static int32_t Mv3nClamp16(int32_t d)
{
    return d > 32767 ? 32767 : d < -32767 ? -32767 : d;
}

// the canonical form of our own input: exactly what the receivers decode (wset 3 bits, flags 4 bits, pose delta <= 32 m per frame)
static void Mv3nCanon(Mv3In *in, const Mv3In *prev)
{
    in->wset &= 7;
    in->flags &= 0xF;
    if (in->sx == -128)
        in->sx = -127;
    if (in->sy == -128)
        in->sy = -127;
    if (prev) {
        in->px = prev->px + Mv3nClamp16(in->px - prev->px);
        in->pz = prev->pz + Mv3nClamp16(in->pz - prev->pz);
        in->py = prev->py + Mv3nClamp16(in->py - prev->py);
    }
}

static int Mv3nIdxOf(const Mv3World *w, uint32_t slot)
{
    for (int i = 0; i < MV3_MAX_F; i++)
        if (w->f[i].slot == slot && slot != 0xff)
            return i;
    return -1;
}

static int Mv3nContested(const Mv3World *w)
{                               // >= 2 fighters on >= 2 teams
    int n = 0, t = -1, two = 0;
    for (int i = 0; i < MV3_MAX_F; i++) {
        if (w->f[i].slot == 0xff)
            continue;
        n++;
        if (t < 0)
            t = w->f[i].team;
        else if (w->f[i].team != t)
            two = 1;
    }
    return n >= 2 && two;
}

static int64_t Mv3nShared(const Mv3nCtx *c, uint64_t now)
{
    return (int64_t) now + c->off + c->trim;
}

static int64_t Mv3nTarget(const Mv3nCtx *c, uint64_t now)
{
    int64_t d = Mv3nShared(c, now) - c->t0;
    return d < 0 ? -1 : d * 3 / 50000;
}

static uint64_t Mv3nFrameLocalUs(const Mv3nCtx *c, uint32_t f)
{
    return (uint64_t) (c->t0 + MV3N_FRAME_US(f) - c->off - c->trim);
}

static int Mv3nDelayFor(Mv3nCtx *c, uint32_t slot)
{
    float r = c->io.rtt_ms ? c->io.rtt_ms(c->io.user, slot) : 0.0f;
    if (slot < MV3N_SLOTS && c->p[slot].have_d)
        r = (float) c->p[slot].rtt_us / 1000.0f;
    if (!(r > 0.0f))
        r = 150.0f;
    int d = (int) (r * 0.5f / (1000.0f / MV3_HZ) + 0.5f);
    return d < MV3_DELAY_MIN ? MV3_DELAY_MIN : d >
        MV3_DELAY_MAX ? MV3_DELAY_MAX : d;
}

static int64_t Mv3nOnewayUs(Mv3nCtx *c, uint32_t slot)
{
    if (slot < MV3N_SLOTS && c->p[slot].have_d)
        return c->p[slot].rtt_us / 2;
    float r = c->io.rtt_ms ? c->io.rtt_ms(c->io.user, slot) : 0.0f;
    return (int64_t) ((r > 0.0f ? r : 150.0f) * 500.0f);
}

// ---------------------------------------------------------------- input history
static Mv3nIent *Mv3nEnt(Mv3nCtx *c, uint32_t s, uint32_t f, int create)
{
    Mv3nIent *e = &c->p[s].r[f % MV3N_IH];
    if (e->f == f && (e->known || e->used_ok))
        return e;
    if (!create)
        return NULL;
    memset(e, 0, sizeof *e);
    e->f = f;
    return e;
}

static void Mv3nInpSet(Mv3nCtx *c, uint32_t s, uint32_t f,
                         const Mv3In *in, int leave, int hp_set,
                         int16_t hp)
{
    Mv3nPeer *p = &c->p[s];
    if (f < p->first || f + MV3N_IH / 2 < p->contig + 1
        || f > p->contig + MV3N_IH / 2)
        return;                 // outside the ring window
    Mv3nIent *e = Mv3nEnt(c, s, f, 1);
    if (e->known)
        return;
    e->in = *in;
    e->leave = (uint8_t) (leave != 0);
    e->known = 1;
    e->hp_set = (uint8_t) (hp_set != 0);
    e->hp = hp_set ? hp : 0;
    if (f > p->hi)
        p->hi = f;
    while (1) {
        Mv3nIent *n = Mv3nEnt(c, s, p->contig + 1, 0);
        if (!n || !n->known)
            break;
        p->contig++;
    }
    if ((c->state == MV3N_ACTIVE) && f <= c->cur && f > c->floor
        && s != c->me) {
        int rb = 0;
        if (e->used_ok
            && (!Mv3nInEq(&e->used, in) || e->used_hp_set != e->hp_set
                || e->used_hp != e->hp))
            rb = 1;
        if (leave && Mv3nIdxOf(&c->w, s) >= 0)
            rb = 1;             // its fighter should have gone at f
        if (rb) {
            if (!c->rb_pending || f < c->rb_to)
                c->rb_to = f;
            c->rb_pending = 1;
            c->w_late++;
        }
    }
}

static void Mv3nPredict(Mv3nCtx *c, uint32_t s, uint32_t f, Mv3In *out)
{
    Mv3nPeer *p = &c->p[s];
    Mv3nIent *e = Mv3nEnt(c, s, f, 0);
    if (e && e->known) {
        *out = e->in;
        return;
    }
    int64_t lo = (int64_t) f - 48;
    if (lo < (int64_t) p->first)
        lo = p->first;
    for (int64_t kk = (int64_t) f - 1; kk >= lo; kk--) {
        uint32_t k = (uint32_t) kk;
        Mv3nIent *a = Mv3nEnt(c, s, k, 0);
        if (!a || !a->known)
            continue;
        *out = a->in;
        Mv3nIent *b = k ? Mv3nEnt(c, s, k - 1, 0) : NULL;
        if (b && b->known && f - k <= 10) {     // keep walking the way it walked
            int32_t n = (int32_t) (f - k);
            out->px += (a->in.px - b->in.px) * n;
            out->pz += (a->in.pz - b->in.pz) * n;
            out->py += (a->in.py - b->in.py) * n;
        }
        return;
    }
    if (p->has_base)
        *out = p->base;
    else
        memset(out, 0, sizeof *out);
    out->btn = 0;
}

static void Mv3nBaseFromFighter(Mv3In *in, const Mv3Fighter *f)
{
    memset(in, 0, sizeof *in);
    in->wset = f->wset;
    in->px = f->px;
    in->pz = f->pz;
    in->py = f->py;
    in->yaw = f->yaw;
    in->flags = 1;
}

// ---------------------------------------------------------------- the sim driver
static void Mv3nSnapPut(Mv3nCtx *c)
{
    c->snap[c->cur % MV3_HIST] = c->w;
    c->snap_f[c->cur % MV3_HIST] = c->cur;
}

static void Mv3nStep(Mv3nCtx *c, int resim)
{
    uint32_t f = c->cur + 1;
    for (int k = 0; k < MV3_MAX_F; k++) {       // joins agreed for this frame
        if (!c->j[k].on || c->j[k].f != f
            || Mv3nIdxOf(&c->w, c->j[k].slot) >= 0)
            continue;
        int idx =
            Mv3SimAdd(&c->w, c->j[k].slot, c->j[k].team, &c->j[k].in);
        if (idx >= 0 && idx < MV3_MAX_F && c->j[k].hp >= 0)
            c->w.f[idx].hp = c->j[k].hp;
    }
    for (uint32_t s = 0; s < MV3N_SLOTS; s++) { // leaves carried in the owner's input stream
        int idx = Mv3nIdxOf(&c->w, s);
        if (idx < 0)
            continue;
        Mv3nIent *e = Mv3nEnt(c, s, f, 0);
        if (e && e->known && e->leave) {
            Mv3SimRemove(&c->w, idx);
            int first_time = c->p[s].left_f != f;
            c->p[s].left_f = f;
            if (first_time && s != c->me)
                Mv3nLog(c,
                         "mv3 net: sess %04x slot %u left the bubble at frame %u",
                         c->sess, s, f);
        }
    }
    if (!c->end_f && !Mv3nContested(&c->w))
        c->end_f = f;
    Mv3In in[MV3_MAX_F];
    memset(in, 0, sizeof in);
    for (int i = 0; i < MV3_MAX_F; i++) {
        uint32_t s = c->w.f[i].slot;
        if (s >= MV3N_SLOTS)
            continue;
        Mv3nPredict(c, s, f, &in[i]);
        Mv3nIent *e = Mv3nEnt(c, s, f, 1);
        e->used = in[i];
        e->used_ok = 1;
        e->used_hp_set = e->known && e->hp_set;
        e->used_hp = e->used_hp_set ? e->hp : 0;
        if (e->used_hp_set)
            c->w.f[i].hp = e->hp;       // the owner's engine hp, at the same frame in every sim
    }
    c->ev.n = 0;
    Mv3SimStep(&c->w, in, &c->ev);
    if (c->w.frame != f) {
        static uint32_t nl;
        if (MV3N_LOGCAP(nl, 4))
            Mv3nLog(c,
                     "mv3 net: sim step left frame %u, expected %u - forced",
                     c->w.frame, f);
        c->w.frame = f;
    }
    c->cur = f;
    Mv3nSnapPut(c);
    c->s_steps++;
    if (c->io.events)
        c->io.events(c->io.user, &c->ev, resim);
}

static void Mv3nHashResetFrom(Mv3nCtx *c, uint32_t from)
{
    for (int i = 0; i < 8; i++)
        if (c->myh[i].f >= from)
            c->myh[i].f = 0xffffffffu;
    uint32_t n =
        (from + MV3N_HASH_EVERY - 1) / MV3N_HASH_EVERY * MV3N_HASH_EVERY;
    if (!n)
        n = MV3N_HASH_EVERY;
    if (c->next_hf > n)
        c->next_hf = n;
}

static int Mv3nRollback(Mv3nCtx *c, uint32_t to)
{                               // redo frames to..cur
    if (to > c->cur || to <= c->floor)
        return 1;
    uint32_t base = to - 1;
    if (c->snap_f[base % MV3_HIST] != base) {
        static uint32_t nl;
        if (MV3N_LOGCAP(nl, 10))
            Mv3nLog(c,
                     "mv3 net: sess %04x rollback to %u impossible (snapshot gone, cur %u conf %u)",
                     c->sess, to, c->cur, c->conf);
        return 0;
    }
    uint32_t old = c->cur;
    c->w = c->snap[base % MV3_HIST];
    c->cur = base;
    if (c->end_f > base)
        c->end_f = 0;
    if (base < c->next_hf)
        Mv3nHashResetFrom(c, to);
    if (c->io.rolled_back)
        c->io.rolled_back(c->io.user, base);
    while (c->cur < old)
        Mv3nStep(c, 1);
    uint32_t len = old - base;
    c->s_rb++;
    c->s_rbf += len;
    if (len > c->s_rbmax)
        c->s_rbmax = len;
    c->w_rb++;
    c->w_rbf += len;
    if (len > c->w_rbmax)
        c->w_rbmax = len;
    return 1;
}

static uint32_t Mv3nConfirmedCalc(Mv3nCtx *c)
{
    uint32_t cf = c->cur;
    for (int i = 0; i < MV3_MAX_F; i++) {
        uint32_t s = c->w.f[i].slot;
        if (s >= MV3N_SLOTS)
            continue;
        if (c->p[s].contig < cf)
            cf = c->p[s].contig;
    }
    for (int k = 0; k < MV3_MAX_F; k++) // a join agreed for a frame we simulated: its inputs count from there
        if (c->j[k].on && c->j[k].f <= c->cur
            && c->p[c->j[k].slot].contig < cf)
            cf = c->p[c->j[k].slot].contig;
    for (uint32_t s = 0; s < MV3N_SLOTS; s++)   // a leaver whose leave bit came before the frames ahead of it: those frames are not final yet
        if (c->p[s].left_f && c->p[s].contig + 1 < c->p[s].left_f
            && c->p[s].contig < cf)
            cf = c->p[s].contig;
    if (cf < c->floor)
        cf = c->floor;
    return cf;
}

// ---------------------------------------------------------------- control messages (reliable)
static void Mv3nCtl(Mv3nCtx *c, uint32_t dest, uint8_t *b, int len,
                     int ts_off, uint64_t now)
{
    if (dest >= MV3N_SLOTS || len > MV3N_PKT || len < 5)
        return;
    uint16_t cq = ++c->cseq;
    if (!cq)
        cq = ++c->cseq;
    Mv3nW16(b + 3, cq);
    if (ts_off)
        Mv3nW64(b + ts_off, (uint64_t) Mv3nShared(c, now));
    c->io.send(c->io.user, dest, b, len);
    for (int i = 0; i < MV3N_CTLQ; i++)
        if (!c->q[i].on) {
            c->q[i].on = 1;
            c->q[i].dest = (uint8_t) dest;
            c->q[i].len = (uint8_t) len;
            c->q[i].ts_off = (uint8_t) ts_off;
            c->q[i].cq = cq;
            c->q[i].tries = 0;
            c->q[i].next_us = now + 100000;
            memcpy(c->q[i].b, b, (size_t) len);
            return;
        }
}

static void Mv3nCtlTick(Mv3nCtx *c, uint64_t now)
{
    for (int i = 0; i < MV3N_CTLQ; i++) {
        if (!c->q[i].on || now < c->q[i].next_us)
            continue;
        if (++c->q[i].tries > 12) {
            static uint32_t nl;
            if (MV3N_LOGCAP(nl, 20))
                Mv3nLog(c,
                         "mv3 net: control op %u to slot %u never acked - given up",
                         c->q[i].b[0], c->q[i].dest);
            c->q[i].on = 0;
            continue;
        }
        if (c->q[i].ts_off)
            Mv3nW64(c->q[i].b + c->q[i].ts_off, (uint64_t) Mv3nShared(c, now));       // a resent START / JOIN carries the time it really left
        c->io.send(c->io.user, c->q[i].dest, c->q[i].b, c->q[i].len);
        int64_t rtt =
            c->p[c->q[i].dest].have_d ? c->p[c->q[i].dest].rtt_us : 200000;
        int64_t iv = rtt + rtt / 4;
        if (iv < 80000)
            iv = 80000;
        c->q[i].next_us = now + (uint64_t) iv;
    }
}

static void Mv3nCtlDrop(Mv3nCtx *c, int keep_end)
{
    for (int i = 0; i < MV3N_CTLQ; i++)
        if (c->q[i].on && !(keep_end && c->q[i].b[0] == MV3N_OP_END))
            c->q[i].on = 0;
}

static int Mv3nCtlSeen(Mv3nCtx *c, uint32_t from, uint16_t cq)
{                               // 1 = a duplicate (the CACK went out anyway)
    Mv3nPeer *p = &c->p[from];
    for (int i = 0; i < 32; i++)
        if (p->seen[i] == cq)
            return 1;
    p->seen[p->seen_i++ % 32] = cq;
    return 0;
}

static int Mv3nPutInit(Mv3nCtx *c, uint8_t *b, Mv3In *in_out)
{                               // [u8 team][i16 hp][in 20]
    Mv3In in;
    memset(&in, 0, sizeof in);
    if (c->io.input)
        c->io.input(c->io.user, &in);
    in.btn = 0;
    Mv3nCanon(&in, NULL);
    b[0] =
        (uint8_t) (c->io.team_of ? c->io.team_of(c->io.user, c->me) : 0);
    int16_t hp = c->io.hp ? c->io.hp(c->io.user) : 1000;
    Mv3nW16(b + 1, (uint16_t) hp);
    Mv3nInPut(b + 3, &in);
    if (in_out)
        *in_out = in;
    return 3 + MV3N_INLEN;
}

static void Mv3nSendInput(Mv3nCtx * c, uint32_t s, uint64_t now);
// ---------------------------------------------------------------- session lifecycle
static void Mv3nSessionClear(Mv3nCtx *c)
{
    uint8_t me = c->me;
    Mv3nIo io = c->io;
    uint16_t pc = c->prop_ctr, cq = c->cseq;
    // control queue survives (an END in flight)
    uint8_t q[sizeof c->q];
    memcpy(q, c->q, sizeof q);
    uint32_t nl[16];
    memcpy(nl, c->n_logs, sizeof nl);
    uint16_t seen[MV3N_SLOTS][32];
    uint8_t seen_i[MV3N_SLOTS];
    for (int s = 0; s < MV3N_SLOTS; s++) {
        memcpy(seen[s], c->p[s].seen, sizeof seen[s]);
        seen_i[s] = c->p[s].seen_i;
    }
    memset(c, 0, sizeof *c);
    c->me = me;
    c->io = io;
    c->prop_ctr = pc;
    c->cseq = cq;
    memcpy(c->q, q, sizeof q);
    memcpy(c->n_logs, nl, sizeof nl);
    for (int s = 0; s < MV3N_SLOTS; s++) {
        memcpy(c->p[s].seen, seen[s], sizeof seen[s]);
        c->p[s].seen_i = seen_i[s];
    }
    for (int i = 0; i < MV3_HIST; i++)
        c->snap_f[i] = 0xffffffffu;
    for (int i = 0; i < 8; i++)
        c->myh[i].f = 0xffffffffu;
    c->next_hf = MV3N_HASH_EVERY;
    c->own_hp = c->hp_sent = -1;
}

static void Mv3nPeerFresh(Mv3nCtx *c, uint32_t s, uint32_t first,
                            const Mv3In *base)
{
    Mv3nPeer *p = &c->p[s];
    uint16_t seen[32];
    uint8_t si = p->seen_i;
    memcpy(seen, p->seen, sizeof seen);
    memset(p, 0, sizeof *p);
    memcpy(p->seen, seen, sizeof seen);
    p->seen_i = si;
    p->first = first;
    p->contig = first - 1;
    p->hi = first - 1;
    for (int i = 0; i < MV3N_IH; i++)
        p->r[i].f = 0xffffffffu;
    if (base) {
        p->base = *base;
        p->has_base = 1;
    }
    p->cmp_hf = 0;
    p->cmp_floor = 0;
    p->rx_hf = 0xffffffffu;
}

static void Mv3nOwnFill(Mv3nCtx *c, uint32_t upto, const Mv3In *in)
{                               // our own inputs up to frame upto (never past our leave frame)
    Mv3nPeer *p = &c->p[c->me];
    while (c->own_hi < upto) {
        if (c->leave_f && c->own_hi >= c->leave_f)
            break;
        uint32_t f = c->own_hi + 1;
        Mv3In x;
        Mv3nIent *prev = Mv3nEnt(c, c->me, f - 1, 0);
        if (in)
            x = *in;
        else {
            memset(&x, 0, sizeof x);
            if (c->io.input)
                c->io.input(c->io.user, &x);
        }
        Mv3nCanon(&x, prev && prev->known ? &prev->in : NULL);
        int lv = 0, hs = 0;
        if (c->leave_why[0] && !c->leave_f) {
            c->leave_f = f;
            lv = 1;
            if (prev && prev->known)
                x = prev->in;
        }
        if (c->own_hp >= 0 && c->own_hp != c->hp_sent
            && (!c->hp_chg_f || f >= c->hp_chg_f + MV3N_K)) {
            hs = 1;
            c->hp_sent = c->own_hp;
            c->hp_chg_f = f;
        }
        Mv3nInpSet(c, c->me, f, &x, lv, hs, c->hp_sent);
        c->own_hi = f;
        (void) p;
    }
}

static void Mv3nActivate(Mv3nCtx *c, uint64_t now)
{
    c->state = MV3N_ACTIVE;
    c->act_us = now;
    c->w_us = now;
    c->conf = c->cur;
    c->floor = c->cur;
    Mv3nSnapPut(c);
    for (uint32_t s = 0; s < MV3N_SLOTS; s++)
        if (c->p[s].dest && !c->p[s].last_rx_us)
            c->p[s].last_rx_us = now;
    uint32_t n = (c->cur / MV3N_HASH_EVERY + 1) * MV3N_HASH_EVERY;
    c->next_hf = n;
}

static void Mv3nFinish(Mv3nCtx *c, int why, const char *detail,
                        int send_end, uint64_t now)
{
    int nf = 0;
    for (int i = 0; i < MV3_MAX_F; i++)
        if (c->w.f[i].slot != 0xff)
            nf++;
    uint32_t ef =
        why == MV3N_END_LEFT ? c->leave_f : why ==
        MV3N_END_OVER ? c->end_f : c->cur;
    Mv3nLog(c,
             "mv3 net: sess %04x END (%s%s%s) at frame %u (now %u, confirmed %u), %.1f s: %u steps, rollbacks %u (%u frames, max %u), stalls %u, desyncs %u / resyncs %u, %d fighters left",
             c->sess, MV3N_END_NAME[why], detail
             && *detail ? ": " : "", detail ? detail : "", ef, c->cur,
             c->conf, (double) (now - c->act_us) / 1e6, c->s_steps,
             c->s_rb, c->s_rbf, c->s_rbmax, c->s_stall, c->s_desync,
             c->s_resync, nf);
    Mv3nCtlDrop(c, 0);
    if (c->state == MV3N_ACTIVE)        // one last INPUT each: its ack lets a lingering leaver stop at once
        for (uint32_t s = 0; s < MV3N_SLOTS; s++)
            if (c->p[s].dest && s != c->me)
                Mv3nSendInput(c, s, now);
    if (send_end) {
        for (uint32_t s = 0; s < MV3N_SLOTS; s++) {
            if (!c->p[s].dest || s == c->me)
                continue;
            uint8_t b[8] = { MV3N_OP_END };
            Mv3nW16(b + 1, c->sess);
            b[5] = (uint8_t) why;
            Mv3nCtl(c, s, b, 6, 0, now);
        }
    }
    if (why == MV3N_END_LEFT && c->leave_f) {
        c->state = MV3N_LINGER;
        c->linger_us = now;
        return;
    }                           // the tail of our inputs (the leave bit) still has to reach them
    Mv3nSessionClear(c);
}

static void Mv3nStartWorld(Mv3nCtx *c, int n, const uint8_t *slots,
                             const uint8_t *teams, const int16_t *hps,
                             const Mv3In *ins)
{
    Mv3SimInit(&c->w, c->seed);
    c->w.frame = 0;
    c->cur = 0;
    for (int i = 0; i < n; i++) {
        int idx = Mv3SimAdd(&c->w, slots[i], teams[i], &ins[i]);
        if (idx >= 0 && idx < MV3_MAX_F && hps[i] >= 0)
            c->w.f[idx].hp = hps[i];
        if (slots[i] == c->me) {
            c->own_hp = c->hp_sent = hps[i];
        }
        Mv3nPeerFresh(c, slots[i], 1, &ins[i]);
        if (slots[i] != c->me)
            c->p[slots[i]].dest = 1;
    }
}

// ---------------------------------------------------------------- API
static void Mv3nInit(Mv3nCtx *c, const Mv3nIo *io, uint8_t me)
{
    memset(c, 0, sizeof *c);
    c->io = *io;
    c->me = me;
    Mv3nSessionClear(c);
}

static int Mv3nActive(const Mv3nCtx *c)
{
    return c->state == MV3N_ACTIVE;
}

static uint32_t Mv3nMaster(const Mv3nCtx *c)
{                               // the lowest slot in the bubble: arbitrates joins, sends the joiner its world
    uint32_t m = 0xff;
    for (int i = 0; i < MV3_MAX_F; i++)
        if (c->w.f[i].slot < m)
            m = c->w.f[i].slot;
    return m;
}

static int Mv3nPropose(Mv3nCtx *c, uint32_t slot, uint64_t now)
{
    if (slot >= MV3N_SLOTS || slot == c->me)
        return 0;
    if (c->state == MV3N_ACTIVE) {
        if (Mv3nIdxOf(&c->w, slot) >= 0)
            return 1;
        for (int k = 0; k < MV3_MAX_F; k++)
            if (c->j[k].on && c->j[k].slot == slot)
                return 1;
        if (c->leave_f)
            return 0;
        Mv3nPeer *p = &c->p[slot];
        if (p->invite_us && now - p->invite_us < 500000)
            return 1;           // invite already out
        uint8_t b[MV3N_PKT];
        memset(b, 0, sizeof b);
        b[0] = MV3N_OP_PROPOSE;
        Mv3nW16(b + 1, c->sess);
        b[5] = 1;
        b[6] = c->delay;
        int n = 7 + Mv3nPutInit(c, b + 7, NULL);
        b[n++] = (uint8_t) Mv3nMaster(c);
        Mv3nW16(b + n, c->sess);
        n += 2;
        Mv3nCtl(c, slot, b, n, 0, now);
        p->invite_us = now;
        Mv3nLog(c,
                 "mv3 net: sess %04x invites slot %u to join (master %u)",
                 c->sess, slot, Mv3nMaster(c));
        return 1;
    }
    if (c->state == MV3N_PROPOSING || c->state == MV3N_ACCEPTED
        || c->state == MV3N_JOINREQ || c->state == MV3N_JOINWAIT)
        return c->prop_slot == slot;
    if (c->state != MV3N_IDLE)
        return 0;
    if (c->io.team_of
        && c->io.team_of(c->io.user, slot) == c->io.team_of(c->io.user,
                                                            c->me))
        return 0;
    c->state = MV3N_PROPOSING;
    c->prop_slot = (uint8_t) slot;
    c->prop_us = now;
    c->prop_id = (uint16_t) ((c->me << 12) | (++c->prop_ctr & 0xfff));
    c->my_delay = (uint8_t) Mv3nDelayFor(c, slot);
    uint8_t b[MV3N_PKT];
    memset(b, 0, sizeof b);
    b[0] = MV3N_OP_PROPOSE;
    Mv3nW16(b + 1, c->prop_id);
    b[5] = 0;
    b[6] = c->my_delay;
    int n = 7 + Mv3nPutInit(c, b + 7, NULL);
    b[n++] = 0xff;
    Mv3nW16(b + n, 0);
    n += 2;
    Mv3nCtl(c, slot, b, n, 0, now);
    Mv3nLog(c,
             "mv3 net: propose %04x to slot %u (delay wish %u, rtt %.0f ms)",
             c->prop_id, slot, c->my_delay,
             c->io.rtt_ms ? (double) c->io.rtt_ms(c->io.user, slot) : 0.0);
    return 1;
}

static void Mv3nEnd(Mv3nCtx *c, const char *why, uint64_t now)
{                               // our own leave: a bit in our input stream at an agreed frame
    (void) now;
    if (c->state == MV3N_PROPOSING || c->state == MV3N_ACCEPTED
        || c->state == MV3N_JOINREQ || c->state == MV3N_JOINWAIT) {
        Mv3nLog(c, "mv3 net: %s cancelled (%s)",
                 c->state ==
                 MV3N_PROPOSING ? "proposal" : "pending bubble",
                 why ? why : "?");
        Mv3nCtlDrop(c, 1);
        Mv3nSessionClear(c);
        return;
    }
    if (c->state != MV3N_ACTIVE || c->leave_why[0])
        return;
    snprintf(c->leave_why, sizeof c->leave_why, "%s", why
             && *why ? why : "glue");
}

static void Mv3nSetHp(Mv3nCtx *c, int16_t hp10)
{                               // our engine hp x10: goes into our input stream at the next frame we sample (>= K frames after the last change)
    if (hp10 < 0)
        return;
    c->own_hp = hp10;
}

static void Mv3nHardEnd(Mv3nCtx *c, int why, const char *detail,
                          uint64_t now)
{                               // no agreed frame: match end, peer silent, desync storm
    if (c->state == MV3N_IDLE)
        return;
    if (c->state == MV3N_LINGER) {
        Mv3nSessionClear(c);
        return;
    }
    if (c->state != MV3N_ACTIVE) {
        Mv3nCtlDrop(c, 1);
        Mv3nSessionClear(c);
        return;
    }
    Mv3nFinish(c, why, detail, 1, now);
}

// ---------------------------------------------------------------- sending
static void Mv3nSendInput(Mv3nCtx *c, uint32_t s, uint64_t now)
{
    Mv3nPeer *p = &c->p[s], *mp = &c->p[c->me];
    uint32_t newest = c->own_hi, lo = mp->first;
    uint32_t a = p->acked + 1 > lo ? p->acked + 1 : lo;
    uint32_t f0 = a, n = 0;
    if (newest + 1 > a) {
        uint32_t span = newest - a + 1;
        if (span <= MV3N_K)
            n = span;
        else {
            n = MV3N_K;
            f0 = (p->alt++ & 1) ? a : newest - MV3N_K + 1;
        }                       // a hole older than K frames: every other packet repairs it from the oldest end
    }
    uint8_t b[MV3N_PKT];
    b[0] = MV3N_OP_INPUT;
    Mv3nW16(b + 1, c->sess);
    Mv3nW16(b + 3, (uint16_t) f0);
    b[5] = (uint8_t) n;
    Mv3nW16(b + 6, (uint16_t) p->contig);
    Mv3nW16(b + 8, (uint16_t) c->cur);
    b[10] = (uint8_t) p->my_adv;
    uint32_t hf = 0xffff, hh = 0;
    for (int i = 0; i < 8; i++)
        if (c->myh[i].f != 0xffffffffu
            && (hf == 0xffff || c->myh[i].f > hf)) {
            hf = c->myh[i].f;
            hh = c->myh[i].h;
        }
    Mv3nW16(b + 11, (uint16_t) (hf == 0xffff ? 0xffff : hf));
    Mv3nW32(b + 13, hh);
    Mv3In prev;
    memset(&prev, 0, sizeof prev);
    Mv3nIent *e0 = n ? Mv3nEnt(c, c->me, f0, 0) : NULL;
    if (e0 && e0->known)
        prev = e0->in;
    Mv3nW32(b + 17, (uint32_t) prev.px);
    Mv3nW32(b + 21, (uint32_t) prev.pz);
    Mv3nW32(b + 25, (uint32_t) prev.py);
    Mv3nW16(b + 29, (uint16_t) prev.yaw);
    Mv3nW16(b + 31, 0);
    b[33] = 0xff;
    int len = MV3N_HDR;
    for (uint32_t i = 0; i < n; i++) {
        Mv3nIent *e = Mv3nEnt(c, c->me, f0 + i, 0);
        if (!e || !e->known) {
            n = i;
            break;
        }
        uint8_t *q = b + len;
        q[0] = e->in.btn;
        q[1] = (uint8_t) e->in.sx;
        q[2] = (uint8_t) e->in.sy;
        q[3] =
            (uint8_t) ((e->in.wset & 7) | ((e->in.flags & 0xF) << 3) | (e->
                                                                        leave
                                                                        ?
                                                                        0x80
                                                                        :
                                                                        0));
        Mv3nW16(q + 4, (uint16_t) (int16_t) (e->in.px - prev.px));
        Mv3nW16(q + 6, (uint16_t) (int16_t) (e->in.pz - prev.pz));
        Mv3nW16(q + 8, (uint16_t) (int16_t) (e->in.py - prev.py));
        Mv3nW16(q + 10, (uint16_t) (int16_t) (e->in.yaw - prev.yaw));
        if (e->hp_set && b[33] == 0xff) {
            b[33] = (uint8_t) i;
            Mv3nW16(b + 31, (uint16_t) e->hp);
        }
        prev = e->in;
        len += 12;
    }
    b[5] = (uint8_t) n;
    c->io.send(c->io.user, s, b, len);
    p->last_tx_us = now;
    if (n && f0 + n - 1 > p->sent_hi)
        p->sent_hi = f0 + n - 1;
}

static void Mv3nSendPing(Mv3nCtx *c, uint32_t s, uint64_t now)
{
    uint8_t b[19];
    b[0] = MV3N_OP_PING;
    Mv3nW16(b + 1, c->sess);
    Mv3nW64(b + 3, now);
    Mv3nW64(b + 11, (uint64_t) c->off);
    c->io.send(c->io.user, s, b, sizeof b);
    c->p[s].last_ping_us = now;
}

// ---------------------------------------------------------------- receiving
static void Mv3nRxInput(Mv3nCtx *c, uint32_t s, const uint8_t *b,
                          uint64_t len, uint64_t now)
{
    if (len < MV3N_HDR)
        return;
    Mv3nPeer *p = &c->p[s];
    uint32_t n = b[5];
    if (n > 16 || len < MV3N_HDR + 12u * n)
        return;
    int16_t hpv = (int16_t) Mv3nR16(b + 31);
    uint8_t hpi = b[33];
    uint32_t ref =
        c->state == MV3N_ACTIVE ? c->cur : (p->contig >
                                            c->own_hi ? p->contig : c->
                                            own_hi);
    uint32_t f0 = Mv3nRec16(Mv3nR16(b + 3), ref);
    uint32_t ack = Mv3nRec16(Mv3nR16(b + 6), c->own_hi);
    uint32_t their_cur = Mv3nRec16(Mv3nR16(b + 8), ref);
    int8_t their_adv = (int8_t) b[10];
    p->last_rx_us = now;
    if (ack > p->acked && ack <= c->own_hi)
        p->acked = ack;
    Mv3In in;
    memset(&in, 0, sizeof in);
    in.px = (int32_t) Mv3nR32(b + 17);
    in.pz = (int32_t) Mv3nR32(b + 21);
    in.py = (int32_t) Mv3nR32(b + 25);
    in.yaw = (int16_t) Mv3nR16(b + 29);
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *q = b + MV3N_HDR + 12 * i;
        in.btn = q[0];
        in.sx = (int8_t) q[1];
        in.sy = (int8_t) q[2];
        in.wset = q[3] & 7;
        in.flags = (uint16_t) ((q[3] >> 3) & 0xF);
        in.px += (int16_t) Mv3nR16(q + 4);
        in.pz += (int16_t) Mv3nR16(q + 6);
        in.py += (int16_t) Mv3nR16(q + 8);
        in.yaw =
            (int16_t) (uint16_t) ((uint16_t) in.yaw + Mv3nR16(q + 10));
        Mv3nInpSet(c, s, f0 + i, &in, q[3] & 0x80, hpi == i, hpv);
    }
    if (c->state == MV3N_ACTIVE) {
        // GGPO frame advantage: where we are vs where they are now (their frame + the one-way); they report the same from their side
        float ow_f =
            p->have_d ? (float) p->rtt_us / 2.0f / 16666.7f : 0.0f;
        float la = (float) (int32_t) (c->cur - their_cur) - ow_f;
        int ila = (int) (la + (la >= 0 ? 0.5f : -0.5f));
        if (ila > 127)
            ila = 127;
        if (ila < -127)
            ila = -127;
        p->my_adv = (int8_t) ila;
        p->adv_e =
            p->adv_e * 0.9f + 0.1f * (la - (float) their_adv) * 0.5f;
        uint16_t hf16 = Mv3nR16(b + 11);
        if (hf16 != 0xffff) {
            p->rx_hf = Mv3nRec16(hf16, c->conf);
            p->rx_h = Mv3nR32(b + 13);
            p->rx_have = 1;
        }
    }
}

static void Mv3nSendSnap(Mv3nCtx *c, uint32_t s, uint32_t f, int reason,
                           uint64_t now)
{
    if (c->snap_f[f % MV3_HIST] != f)
        return;
    const uint8_t *d = (const uint8_t *) &c->snap[f % MV3_HIST];
    int total = (int) sizeof(Mv3World), cnt =
        (total + MV3N_CHUNK - 1) / MV3N_CHUNK;
    for (int i = 0; i < cnt; i++) {
        uint8_t b[MV3N_PKT];
        int l = total - i * MV3N_CHUNK;
        if (l > MV3N_CHUNK)
            l = MV3N_CHUNK;
        b[0] = MV3N_OP_SNAP;
        Mv3nW16(b + 1, c->sess);
        Mv3nW32(b + 5, f);
        b[9] = (uint8_t) i;
        b[10] = (uint8_t) cnt;
        b[11] = (uint8_t) reason;
        memcpy(b + 12, d + i * MV3N_CHUNK, (size_t) l);
        Mv3nCtl(c, s, b, 12 + l, 0, now);
    }
    c->p[s].last_snap_us = now;
}

static void Mv3nAdopt(Mv3nCtx *c, uint32_t f, const Mv3World *w,
                       uint64_t now)
{
    uint32_t old = c->cur;
    c->w = *w;
    c->w.frame = f;
    c->cur = f;
    for (int i = 0; i < MV3_HIST; i++)
        if (c->snap_f[i] != 0xffffffffu && c->snap_f[i] < f)
            c->snap_f[i] = 0xffffffffu; // never roll back into the old (wrong) past
    Mv3nSnapPut(c);
    c->floor = f;
    c->rb_pending = 0;
    if (c->end_f && c->end_f <= f)
        c->end_f = 0;
    if (!Mv3nContested(&c->w) && !c->end_f)
        c->end_f = f;
    for (uint32_t s = 0; s < MV3N_SLOTS; s++) {
        Mv3nPeer *p = &c->p[s];
        if (p->contig < f) {
            p->contig = f;
            while (1) {
                Mv3nIent *n = Mv3nEnt(c, s, p->contig + 1, 0);
                if (!n || !n->known)
                    break;
                p->contig++;
            }
        }
        if (p->cmp_floor < f)
            p->cmp_floor = f;
    }
    for (int i = 0; i < 8; i++)
        if (c->myh[i].f != 0xffffffffu && c->myh[i].f <= f)
            c->myh[i].f = 0xffffffffu;
    c->next_hf = (f / MV3N_HASH_EVERY + 1) * MV3N_HASH_EVERY;
    if (c->io.rolled_back)
        c->io.rolled_back(c->io.user, f);
    while (c->cur < old)
        Mv3nStep(c, 1);
    (void) now;
}

static void Mv3nRxSnap(Mv3nCtx *c, uint32_t s, const uint8_t *b,
                         uint64_t len, uint64_t now)
{
    if (len < 13)
        return;
    uint32_t f = Mv3nR32(b + 5);
    uint8_t idx = b[9], cnt = b[10], reason = b[11];
    int total = (int) sizeof(Mv3World);
    if (cnt != (total + MV3N_CHUNK - 1) / MV3N_CHUNK || idx >= cnt
        || cnt > 31)
        return;
    if (c->srx.from != s || c->srx.f != f || c->srx.cnt != cnt) {
        c->srx.from = (uint8_t) s;
        c->srx.f = f;
        c->srx.cnt = cnt;
        c->srx.mask = 0;
        c->srx.reason = reason;
    }
    int l = (int) len - 12;
    if (idx * MV3N_CHUNK + l > total)
        l = total - idx * MV3N_CHUNK;
    if (l <= 0)
        return;
    memcpy(c->srx.d + idx * MV3N_CHUNK, b + 12, (size_t) l);
    c->srx.mask |= 1u << idx;
    if (c->srx.mask != (1u << cnt) - 1)
        return;
    c->srx.mask = 0;
    Mv3World w;
    memcpy(&w, c->srx.d, sizeof w);
    if (c->state == MV3N_JOINWAIT && reason == 0) {
        uint32_t jf = 0;
        for (int k = 0; k < MV3_MAX_F; k++)
            if (c->j[k].on && c->j[k].slot == c->me)
                jf = c->j[k].f;
        if (!jf || f != jf - 1)
            return;
        c->w = w;
        c->w.frame = f;
        c->cur = f;
        for (int i = 0; i < MV3_MAX_F; i++) {
            uint32_t ss = w.f[i].slot;
            if (ss >= MV3N_SLOTS || ss == c->me)
                continue;
            Mv3In base;
            Mv3nBaseFromFighter(&base, &w.f[i]);
            if (!c->p[ss].first)
                Mv3nPeerFresh(c, ss, f + 1, &base);
            else {
                c->p[ss].base = base;
                c->p[ss].has_base = 1;
                if (c->p[ss].contig < f)
                    c->p[ss].contig = f;
            }
            c->p[ss].dest = 1;
        }
        Mv3nActivate(c, now);
        Mv3nLog(c,
                 "mv3 net: sess %04x JOINED at frame %u (world from slot %u, delay %u, %u fighters before us)",
                 c->sess, jf, s, c->delay, (unsigned) w.n);
        return;
    }
    if (c->state == MV3N_ACTIVE && reason == 1) {
        if (f + MV3N_IH / 2 < c->cur)
            return;             // far too old
        Mv3nLog(c,
                 "mv3 net: sess %04x RESYNC: adopting slot %u's world at frame %u (we are at %u, confirmed %u)",
                 c->sess, s, f, c->cur, c->conf);
        c->s_resync++;
        Mv3nAdopt(c, f, &w, now);
    }
}

static void Mv3nBeginMaster(Mv3nCtx *c, uint32_t s,
                              const uint8_t *init_b, int their_delay,
                              uint64_t now)
{                               // we proposed, s accepted: START
    uint8_t slots[2], teams[2];
    int16_t hps[2];
    Mv3In ins[2];
    uint8_t my_init[3 + MV3N_INLEN];
    Mv3nPutInit(c, my_init, &ins[0]);
    // canonical order: lower slot first (every machine adds the fighters in the same order)
    int lo = c->me < s ? 0 : 1;
    slots[lo] = c->me;
    teams[lo] = my_init[0];
    hps[lo] = (int16_t) Mv3nR16(my_init + 1);
    Mv3nInGet(&ins[lo], my_init + 3);
    slots[1 - lo] = (uint8_t) s;
    teams[1 - lo] = init_b[0];
    hps[1 - lo] = (int16_t) Mv3nR16(init_b + 1);
    Mv3nInGet(&ins[1 - lo], init_b + 3);
    uint16_t sess = c->prop_id;
    unsigned my_delay = c->my_delay;
    int delay = c->my_delay > their_delay ? c->my_delay : their_delay;
    if (delay < MV3_DELAY_MIN)
        delay = MV3_DELAY_MIN;
    if (delay > MV3_DELAY_MAX)
        delay = MV3_DELAY_MAX;
    uint32_t seed =
        (uint32_t) (now *
                    2654435761u) ^ ((uint32_t) sess << 8) ^ 0x9e3779b9u;
    if (!seed)
        seed = 1;
    int64_t ow = Mv3nOnewayUs(c, s);
    Mv3nCtlDrop(c, 1);
    Mv3nSessionClear(c);
    c->sess = sess;
    c->delay = (uint8_t) delay;
    c->seed = seed;
    c->off = 0;
    c->t0 = Mv3nShared(c, now) + ow + 8000;    // frame 0 when START has just landed on their side
    Mv3nStartWorld(c, 2, slots, teams, hps, ins);
    Mv3In me_in = ins[lo];
    Mv3nOwnFill(c, delay, &me_in);
    uint8_t b[MV3N_PKT];
    memset(b, 0, sizeof b);
    b[0] = MV3N_OP_START;
    Mv3nW16(b + 1, sess);
    Mv3nW64(b + 13, (uint64_t) c->t0);
    Mv3nW32(b + 21, seed);
    b[25] = (uint8_t) delay;
    b[26] = 2;
    int n = 27;
    for (int i = 0; i < 2; i++) {
        b[n] = slots[i];
        b[n + 1] = teams[i];
        Mv3nW16(b + n + 2, (uint16_t) hps[i]);
        Mv3nInPut(b + n + 4, &ins[i]);
        n += 4 + MV3N_INLEN;
    }
    Mv3nCtl(c, s, b, n, 5, now);
    Mv3nActivate(c, now);
    Mv3nLog(c,
             "mv3 net: sess %04x START with slot %u: delay %d frames (wishes: ours %u, theirs %d), t0 in %.0f ms, seed %08x",
             sess, s, delay, my_delay, their_delay,
             (double) (c->t0 - Mv3nShared(c, now)) / 1000.0, seed);
}

static void Mv3nRecv(Mv3nCtx *c, uint32_t s, const uint8_t *b,
                      uint64_t len, uint64_t now)
{
    if (s >= MV3N_SLOTS || s == c->me || !b || len < 3)
        return;
    uint8_t op = b[0];
    uint16_t sess = Mv3nR16(b + 1);
    Mv3nPeer *p = &c->p[s];
    if (op == MV3N_OP_INPUT) {
        if (c->state == MV3N_ACTIVE || c->state == MV3N_JOINWAIT
            || c->state == MV3N_LINGER) {
            if (sess != c->sess)
                return;
            if (c->state == MV3N_LINGER) {
                uint32_t ack = Mv3nRec16(Mv3nR16(b + 6), c->own_hi);
                if (len >= MV3N_HDR && ack > p->acked && ack <= c->own_hi)
                    p->acked = ack;
                return;
            }
            Mv3nRxInput(c, s, b, len, now);
        }
        return;
    }
    if (op == MV3N_OP_PING || op == MV3N_OP_PONG) {
        if (sess != c->sess
            || (c->state != MV3N_ACTIVE && c->state != MV3N_JOINWAIT))
            return;
        if (op == MV3N_OP_PING && len >= 19) {
            uint8_t r[27];
            r[0] = MV3N_OP_PONG;
            Mv3nW16(r + 1, c->sess);
            memcpy(r + 3, b + 3, 8);
            Mv3nW64(r + 11, now);
            Mv3nW64(r + 19, (uint64_t) c->off);
            c->io.send(c->io.user, s, r, sizeof r);
            p->their_off = (int64_t) Mv3nR64(b + 11);
            return;
        }
        if (op == MV3N_OP_PONG && len >= 27) {
            uint64_t t1 = Mv3nR64(b + 3), t2 = Mv3nR64(b + 11);
            if (t1 > now || now - t1 > 2000000)
                return;
            int64_t rtt = (int64_t) (now - t1), d =
                (int64_t) t2 - (int64_t) (t1 + (uint64_t) rtt / 2);
            p->smp[p->smp_i++ % 16].d = d;
            p->smp[(p->smp_i - 1) % 16].rtt = rtt;
            if (p->smp_n < 16)
                p->smp_n++;
            int bi = 0;
            for (int i = 1; i < p->smp_n; i++)
                if (p->smp[i].rtt < p->smp[bi].rtt)
                    bi = i;
            p->d_us = p->smp[bi].d;
            p->rtt_us = p->smp[bi].rtt;
            p->have_d = 1;
            p->their_off = (int64_t) Mv3nR64(b + 19);
            p->last_rx_us = now;
        }
        return;
    }
    if (op == MV3N_OP_CACK) {
        if (len < 5)
            return;
        uint16_t cq = Mv3nR16(b + 3);
        for (int i = 0; i < MV3N_CTLQ; i++)
            if (c->q[i].on && c->q[i].dest == s && c->q[i].cq == cq)
                c->q[i].on = 0;
        return;
    }
    // ---- reliable control: ACK every copy, act on the first
    if (len < 5)
        return;
    uint16_t cq = Mv3nR16(b + 3);
    {
        uint8_t a[5] = { MV3N_OP_CACK };
        Mv3nW16(a + 1, sess);
        Mv3nW16(a + 3, cq);
        c->io.send(c->io.user, s, a, 5);
    }
    if (Mv3nCtlSeen(c, s, cq))
        return;
    p->last_rx_us = now;
    switch (op) {
    case MV3N_OP_PROPOSE:{
            if (len < 7 + 3 + MV3N_INLEN + 3)
                return;
            int come_join = b[5] & 1, their_delay = b[6];
            const uint8_t *init = b + 7;
            if (come_join) {    // s is in a bubble and asks us in
                if (c->state != MV3N_IDLE)
                    return;
                uint8_t master = b[7 + 3 + MV3N_INLEN];
                uint16_t rs = Mv3nR16(b + 7 + 3 + MV3N_INLEN + 1);
                if (master >= MV3N_SLOTS || master == c->me)
                    return;
                c->state = MV3N_JOINREQ;
                c->prop_slot = master;
                c->prop_us = now;
                c->sess = rs;
                uint8_t r[MV3N_PKT];
                r[0] = MV3N_OP_JOINREQ;
                Mv3nW16(r + 1, rs);
                int n = 5 + Mv3nPutInit(c, r + 5, NULL);
                Mv3nCtl(c, master, r, n, 0, now);
                Mv3nLog(c,
                         "mv3 net: slot %u invited us into sess %04x - join request to master %u",
                         s, rs, master);
                return;
            }
            if (c->state == MV3N_ACTIVE) {
                if (Mv3nIdxOf(&c->w, s) >= 0)
                    return;
                uint8_t r[8];
                r[0] = MV3N_OP_REDIRECT;
                Mv3nW16(r + 1, sess);
                r[5] = (uint8_t) Mv3nMaster(c);
                Mv3nW16(r + 6, c->sess);
                Mv3nCtl(c, s, r, 8, 0, now);
                return;
            }
            int ok = 1;
            if (c->state == MV3N_PROPOSING && c->prop_slot == s) {
                if (c->me < s)
                    return;     // both proposed: the lower slot's proposal stands, ours is answered
                Mv3nCtlDrop(c, 1);
            } else if (c->state != MV3N_IDLE)
                ok = 0;
            if (c->io.team_of
                && c->io.team_of(c->io.user,
                                 s) == c->io.team_of(c->io.user, c->me))
                ok = 0;
            uint8_t r[MV3N_PKT];
            memset(r, 0, sizeof r);
            r[0] = MV3N_OP_ACCEPT;
            Mv3nW16(r + 1, sess);
            r[5] = (uint8_t) ok;
            if (ok) {
                c->my_delay = (uint8_t) Mv3nDelayFor(c, s);
            }
            r[6] = c->my_delay;
            int n = 7 + Mv3nPutInit(c, r + 7, NULL);
            Mv3nCtl(c, s, r, n, 0, now);
            if (ok) {
                c->state = MV3N_ACCEPTED;
                c->prop_slot = (uint8_t) s;
                c->prop_id = sess;
                c->prop_us = now;
            }
            Mv3nLog(c,
                     "mv3 net: proposal %04x from slot %u %s (their delay wish %d)",
                     sess, s,
                     ok ? "accepted" : "refused (busy / teammate)",
                     their_delay);
            (void) init;
            return;
        }
    case MV3N_OP_ACCEPT:
        if (len < 7 + 3 + MV3N_INLEN || c->state != MV3N_PROPOSING
            || c->prop_slot != s || sess != c->prop_id)
            return;
        if (!b[5]) {
            Mv3nLog(c, "mv3 net: proposal %04x refused by slot %u", sess,
                     s);
            Mv3nCtlDrop(c, 1);
            Mv3nSessionClear(c);
            return;
        }
        Mv3nBeginMaster(c, s, b + 7, b[6], now);
        return;
    case MV3N_OP_START:{
            if (len < 27 || c->state != MV3N_ACCEPTED || c->prop_slot != s
                || sess != c->prop_id)
                return;
            int n = b[26];
            if (n < 2 || n > MV3_MAX_F
                || len < 27u + (uint64_t) n * (4 + MV3N_INLEN))
                return;
            uint8_t slots[MV3_MAX_F], teams[MV3_MAX_F];
            int16_t hps[MV3_MAX_F];
            Mv3In ins[MV3_MAX_F];
            int mine = -1;
            for (int i = 0; i < n; i++) {
                const uint8_t *q = b + 27 + i * (4 + MV3N_INLEN);
                slots[i] = q[0];
                teams[i] = q[1];
                hps[i] = (int16_t) Mv3nR16(q + 2);
                Mv3nInGet(&ins[i], q + 4);
                if (slots[i] >= MV3N_SLOTS)
                    return;
                if (slots[i] == c->me)
                    mine = i;
            }
            if (mine < 0)
                return;
            int64_t ts = (int64_t) Mv3nR64(b + 5), t0 =
                (int64_t) Mv3nR64(b + 13);
            int64_t ow = Mv3nOnewayUs(c, s);
            Mv3nCtlDrop(c, 1);
            Mv3nSessionClear(c);
            c->sess = sess;
            c->delay = b[25];
            c->seed = Mv3nR32(b + 21);
            c->t0 = t0;
            c->off = ts + ow - (int64_t) now;   // our first guess at the shared clock; PING / PONG refine it
            Mv3nStartWorld(c, n, slots, teams, hps, ins);
            Mv3nOwnFill(c, c->delay, &ins[mine]);
            Mv3nActivate(c, now);
            Mv3nLog(c,
                     "mv3 net: sess %04x START from slot %u: delay %u frames, t0 %+.0f ms from now (one-way est %.0f ms), seed %08x",
                     sess, s, c->delay,
                     (double) (t0 - Mv3nShared(c, now)) / 1000.0,
                     (double) ow / 1000.0, c->seed);
            return;
        }
    case MV3N_OP_REDIRECT:
        if (len < 8 || c->state != MV3N_PROPOSING || c->prop_slot != s
            || sess != c->prop_id)
            return;
        {
            uint8_t master = b[5];
            uint16_t rs = Mv3nR16(b + 6);
            if (master >= MV3N_SLOTS || master == c->me)
                return;
            Mv3nCtlDrop(c, 1);
            c->state = MV3N_JOINREQ;
            c->prop_slot = master;
            c->prop_us = now;
            c->sess = rs;
            uint8_t r[MV3N_PKT];
            r[0] = MV3N_OP_JOINREQ;
            Mv3nW16(r + 1, rs);
            int n = 5 + Mv3nPutInit(c, r + 5, NULL);
            Mv3nCtl(c, master, r, n, 0, now);
            Mv3nLog(c,
                     "mv3 net: slot %u is in sess %04x - join request to master %u",
                     s, rs, master);
        }
        return;
    case MV3N_OP_JOINREQ:{
            if (len < 5 + 3 + MV3N_INLEN || c->state != MV3N_ACTIVE
                || sess != c->sess || c->leave_f)
                return;
            if (Mv3nMaster(c) != c->me) {
                uint8_t r[8];
                r[0] = MV3N_OP_REDIRECT;
                Mv3nW16(r + 1, sess);
                r[5] = (uint8_t) Mv3nMaster(c);
                Mv3nW16(r + 6, c->sess);
                Mv3nCtl(c, s, r, 8, 0, now);
                return;
            }
            if (Mv3nIdxOf(&c->w, s) >= 0)
                return;
            int nf = 0, k = -1, enemy = 0, team = b[5];
            for (int i = 0; i < MV3_MAX_F; i++)
                if (c->w.f[i].slot != 0xff) {
                    nf++;
                    if (c->w.f[i].team != team)
                        enemy = 1;
                }
            for (int i = 0; i < MV3_MAX_F; i++) {
                if (c->j[i].on && c->j[i].slot == s)
                    return;
                if (!c->j[i].on && k < 0)
                    k = i;
            }
            if (nf >= MV3_MAX_F || k < 0 || !enemy) {
                Mv3nLog(c,
                         "mv3 net: sess %04x join request from slot %u refused (%s)",
                         c->sess, s,
                         !enemy ? "no enemy of theirs inside" : "full");
                return;
            }
            int64_t ow = 0;
            for (uint32_t m = 0; m < MV3N_SLOTS; m++)
                if (c->p[m].dest || m == s) {
                    int64_t o = Mv3nOnewayUs(c, m);
                    if (o > ow)
                        ow = o;
                }
            uint32_t J =
                c->cur + MV3N_JOIN_LEAD + (uint32_t) (ow * 2 / 16667);
            c->j[k].on = 1;
            c->j[k].f = J;
            c->j[k].slot = (uint8_t) s;
            c->j[k].team = b[5];
            c->j[k].hp = (int16_t) Mv3nR16(b + 6);
            Mv3nInGet(&c->j[k].in, b + 8);
            Mv3nPeerFresh(c, s, J, &c->j[k].in);
            c->p[s].dest = 1;
            c->p[s].last_rx_us = now;
            c->p[s].acked = J - 1;      // it needs our inputs from J on only
            uint8_t mask = 0;
            for (uint32_t m = 0; m < MV3N_SLOTS; m++)
                if (c->p[m].dest || m == c->me)
                    mask |= (uint8_t) (1u << m);
            uint8_t r[MV3N_PKT];
            memset(r, 0, sizeof r);
            r[0] = MV3N_OP_JOIN;
            Mv3nW16(r + 1, c->sess);
            Mv3nW32(r + 5, J);
            r[9] = (uint8_t) s;
            memcpy(r + 10, b + 5, 3 + MV3N_INLEN);      // [slot][team][hp][in]
            Mv3nW64(r + 41, (uint64_t) c->t0);
            Mv3nW32(r + 49, c->seed);
            r[53] = c->delay;
            r[54] = c->me;
            r[55] = mask;
            for (uint32_t m = 0; m < MV3N_SLOTS; m++)
                if (c->p[m].dest && m != c->me)
                    Mv3nCtl(c, m, r, 56, 33, now);
            for (int i = 0; i < MV3_MAX_F; i++)
                if (!c->jsnap[i].on) {
                    c->jsnap[i].on = 1;
                    c->jsnap[i].slot = s;
                    c->jsnap[i].f = J - 1;
                    break;
                }
            Mv3nLog(c,
                     "mv3 net: sess %04x slot %u JOINS at frame %u (now %u, team %u)",
                     c->sess, s, J, c->cur, b[5]);
            return;
        }
    case MV3N_OP_JOIN:{
            if (len < 56)
                return;
            uint32_t J = Mv3nR32(b + 5);
            uint8_t js = b[9];
            if (js >= MV3N_SLOTS)
                return;
            Mv3In jin;
            Mv3nInGet(&jin, b + 13);
            if (js == c->me) {
                if (c->state != MV3N_JOINREQ || sess != c->sess)
                    return;
                int64_t ts = (int64_t) Mv3nR64(b + 33);
                Mv3nSessionClear(c);
                c->sess = sess;
                c->t0 = (int64_t) Mv3nR64(b + 41);
                c->seed = Mv3nR32(b + 49);
                c->delay = b[53];
                c->off = ts + Mv3nOnewayUs(c, s) - (int64_t) now;
                c->state = MV3N_JOINWAIT;
                c->prop_slot = (uint8_t) s;
                c->prop_us = now;
                c->j[0].on = 1;
                c->j[0].f = J;
                c->j[0].slot = js;
                c->j[0].team = b[10];
                c->j[0].hp = (int16_t) Mv3nR16(b + 11);
                c->j[0].in = jin;
                Mv3nPeerFresh(c, c->me, J, &jin);
                c->own_hi = J - 1;
                c->own_hp = c->hp_sent = c->j[0].hp;
                for (uint32_t m = 0; m < MV3N_SLOTS; m++)
                    if ((b[55] >> m) & 1 && m != c->me) {
                        Mv3nPeerFresh(c, m, J - 0, NULL);
                        c->p[m].dest = 1;
                        c->p[m].last_rx_us = now;
                        c->p[m].contig = J - 1;
                    }
                Mv3nLog(c,
                         "mv3 net: sess %04x join granted by master %u: frame %u, delay %u - waiting for its world",
                         sess, s, J, c->delay);
                return;
            }
            if (c->state != MV3N_ACTIVE || sess != c->sess
                || Mv3nIdxOf(&c->w, js) >= 0)
                return;
            int k = -1;
            for (int i = 0; i < MV3_MAX_F; i++) {
                if (c->j[i].on && c->j[i].slot == js)
                    return;
                if (!c->j[i].on && k < 0)
                    k = i;
            }
            if (k < 0)
                return;
            c->j[k].on = 1;
            c->j[k].f = J;
            c->j[k].slot = js;
            c->j[k].team = b[10];
            c->j[k].hp = (int16_t) Mv3nR16(b + 11);
            c->j[k].in = jin;
            Mv3nPeerFresh(c, js, J, &jin);
            c->p[js].dest = 1;
            c->p[js].last_rx_us = now;
            c->p[js].acked = J - 1;
            if (J <= c->cur) {
                static uint32_t nl;
                if (MV3N_LOGCAP(nl, 10))
                    Mv3nLog(c,
                             "mv3 net: sess %04x join of slot %u at frame %u arrived late (we are at %u) - rollback",
                             c->sess, js, J, c->cur);
                if (!c->rb_pending || J < c->rb_to)
                    c->rb_to = J;
                c->rb_pending = 1;
                if (J <= c->floor)
                    c->floor = J - 1;
            }
            Mv3nLog(c,
                     "mv3 net: sess %04x slot %u joins at frame %u (master %u)",
                     c->sess, js, J, s);
            return;
        }
    case MV3N_OP_SNAP:
        if (sess != c->sess)
            return;
        Mv3nRxSnap(c, s, b, len, now);
        return;
    case MV3N_OP_END:
        if (sess != c->sess || c->state == MV3N_IDLE)
            return;
        {
            char d[48];
            snprintf(d, sizeof d, "slot %u ended it: %s", s, len >= 6
                     && b[5] < 8 ? MV3N_END_NAME[b[5]] : "?");
            if (c->state == MV3N_ACTIVE) {
                int others = 0;
                for (int i = 0; i < MV3_MAX_F; i++)
                    if (c->w.f[i].slot != 0xff && c->w.f[i].slot != c->me
                        && c->w.f[i].slot != s)
                        others++;
                (void) others;
                Mv3nFinish(c, MV3N_END_ABORT, d, 0, now);
            } else {
                Mv3nLog(c, "mv3 net: %s", d);
                Mv3nCtlDrop(c, 1);
                Mv3nSessionClear(c);
            }
        }
        return;
    }
}

// ---------------------------------------------------------------- tick
static void Mv3nClock(Mv3nCtx *c)
{
    int n = 0;
    int64_t sum = 0;
    for (uint32_t s = 0; s < MV3N_SLOTS; s++) {
        Mv3nPeer *p = &c->p[s];
        if (!p->dest || !p->have_d || s == c->me)
            continue;
        sum += p->d_us + p->their_off - c->off; // their shared clock minus ours
        n++;
    }
    if (n) {
        int64_t want = sum / (n + 1);   // move to the mean of everybody's shared clock (they move toward it too)
        if (want > 100000 || want < -100000)
            c->off += want;     // way off (a bad first guess): jump
        else
            c->off += want > 120 ? 120 : want < -120 ? -120 : want;
    }
    float adv = 0.0f;
    int na = 0;
    for (uint32_t s = 0; s < MV3N_SLOTS; s++)
        if (c->p[s].dest && s != c->me && c->p[s].have_d) {
            adv += c->p[s].adv_e;
            na++;
        }
    if (na) {
        adv /= (float) na;
        if (adv > 1.0f && c->trim > -60000)
            c->trim -= 40;      // we run ahead of them: a little slower
        else if (adv < -1.0f && c->trim < 60000)
            c->trim += 40;
    }
}

static void Mv3nHashes(Mv3nCtx *c, uint64_t now)
{
    while (c->next_hf <= c->conf) {
        uint32_t f = c->next_hf;
        c->next_hf += MV3N_HASH_EVERY;
        if (c->snap_f[f % MV3_HIST] != f || f <= c->floor)
            continue;
        uint32_t h = Mv3SimHash(&c->snap[f % MV3_HIST]);
        int k = (int) (f / MV3N_HASH_EVERY % 8);
        c->myh[k].f = f;
        c->myh[k].h = h;
        if (c->io.hash_note)
            c->io.hash_note(c->io.user, f, h);
    }
    for (uint32_t s = 0; s < MV3N_SLOTS; s++) {
        Mv3nPeer *p = &c->p[s];
        if (!p->dest || !p->rx_have || p->rx_hf <= p->cmp_hf
            || p->rx_hf <= p->cmp_floor)
            continue;
        int k = -1;
        for (int i = 0; i < 8; i++)
            if (c->myh[i].f == p->rx_hf)
                k = i;
        if (k < 0) {
            if (p->rx_hf + 8 * MV3N_HASH_EVERY < c->next_hf)
                p->cmp_hf = p->rx_hf;
            continue;
        }                       // ours not computed yet: later
        p->cmp_hf = p->rx_hf;
        if (c->myh[k].h == p->rx_h)
            continue;
        c->s_desync++;
        c->w_desync++;
        uint32_t nl = c->n_logs[0]++;
        if (nl < 20)
            Mv3nLog(c,
                     "mv3 net: sess %04x DESYNC with slot %u at frame %u (ours %08x, theirs %08x)%s",
                     c->sess, s, p->rx_hf, c->myh[k].h, p->rx_h,
                     c->me <
                     s ? " - sending our confirmed world" :
                     " - waiting for theirs");
        if (c->me < s && now - p->last_snap_us > 500000) {
            Mv3nSendSnap(c, s, c->conf, 1, now);
            p->cmp_floor = c->conf;
        }
    }
}

static void Mv3nTick(Mv3nCtx *c, uint64_t now)
{
    Mv3nCtlTick(c, now);
    switch (c->state) {
    case MV3N_IDLE:
        return;
    case MV3N_PROPOSING:
    case MV3N_ACCEPTED:
    case MV3N_JOINREQ:
        if (now - c->prop_us > 1500000) {
            Mv3nLog(c, "mv3 net: %s with slot %u timed out",
                     c->state == MV3N_PROPOSING ? "proposal" : c->state ==
                     MV3N_ACCEPTED ? "accepted proposal (no START)" :
                     "join request", c->prop_slot);
            Mv3nCtlDrop(c, 1);
            Mv3nSessionClear(c);
        }
        return;
    case MV3N_LINGER:{
            int left = 0;
            for (uint32_t s = 0; s < MV3N_SLOTS; s++) {
                Mv3nPeer *p = &c->p[s];
                if (!p->dest || s == c->me || p->acked >= c->leave_f)
                    continue;
                left++;
                if (now - p->last_tx_us > 30000)
                    Mv3nSendInput(c, s, now);
            }
            if (!left || now - c->linger_us > 700000)
                Mv3nSessionClear(c);
            return;
        }
    case MV3N_JOINWAIT:{
            int64_t T = Mv3nTarget(c, now);
            uint32_t J = c->j[0].f;
            if (T >= 0 && (uint32_t) T + c->delay >= J)
                Mv3nOwnFill(c, (uint32_t) T + c->delay, NULL);
            for (uint32_t s = 0; s < MV3N_SLOTS; s++) {
                if (!c->p[s].dest || s == c->me)
                    continue;
                if (c->own_hi > c->p[s].sent_hi
                    || now - c->p[s].last_tx_us > 50000)
                    Mv3nSendInput(c, s, now);
                if (now - c->p[s].last_ping_us > 200000)
                    Mv3nSendPing(c, s, now);
            }
            if (now - c->prop_us > 2500000) {
                Mv3nLog(c,
                         "mv3 net: sess %04x join: the world never came - given up",
                         c->sess);
                Mv3nCtlDrop(c, 1);
                Mv3nSessionClear(c);
            }
            return;
        }
    default:
        break;
    }
    // ---- ACTIVE
    Mv3nClock(c);
    if (c->rb_pending) {
        c->rb_pending = 0;
        if (!Mv3nRollback(c, c->rb_to)) {
            Mv3nFinish(c, MV3N_END_STALL, "rollback past the history", 1,
                        now);
            return;
        }
    }
    int64_t T = Mv3nTarget(c, now);
    int steps = 0, stalled = 0;
    while (T >= 0 && (int64_t) c->cur < T && steps < MV3N_CATCHUP) {
        Mv3nOwnFill(c, c->cur + 1 + c->delay, NULL);  // (stops by itself at our leave frame)
        c->conf = Mv3nConfirmedCalc(c);
        if (c->cur + 1 - c->conf > MV3_HIST - 1) {
            stalled = 1;
            break;
        }
        Mv3nStep(c, 0);
        steps++;
    }
    c->conf = Mv3nConfirmedCalc(c);
    if (stalled) {
        if (!c->stall_us) {
            c->stall_us = now;
            c->s_stall++;
            c->w_stall++;
        } else if (now - c->stall_us > MV3N_STALL_END_US) {
            Mv3nFinish(c, MV3N_END_STALL, "a member's inputs stopped", 1,
                        now);
            return;
        }
    } else
        c->stall_us = 0;
    Mv3nHashes(c, now);
    for (int i = 0; i < MV3_MAX_F; i++)
        if (c->jsnap[i].on && c->conf >= c->jsnap[i].f) {       // master: the joiner's world (confirmed J - 1)
            Mv3nSendSnap(c, c->jsnap[i].slot, c->jsnap[i].f, 0, now);
            c->jsnap[i].on = 0;
        }
    for (int k = 0; k < MV3_MAX_F; k++)
        if (c->j[k].on && c->j[k].f + MV3_HIST < c->conf)
            c->j[k].on = 0;     // applied and out of the rollback window
    // leave rule: our nearest enemy farther than 4 m for 1 s
    int mi = Mv3nIdxOf(&c->w, c->me);
    if (mi >= 0 && !c->leave_why[0]) {
        int64_t best = -1;
        for (int i = 0; i < MV3_MAX_F; i++) {
            if (i == mi || c->w.f[i].slot == 0xff
                || c->w.f[i].team == c->w.f[mi].team)
                continue;
            int64_t dx = c->w.f[i].px - c->w.f[mi].px, dz =
                c->w.f[i].pz - c->w.f[mi].pz, d2 = dx * dx + dz * dz;
            if (best < 0 || d2 < best)
                best = d2;
        }
        if (best > (int64_t) MV3N_FAR_MM * MV3N_FAR_MM)
            c->far_n += steps;
        else
            c->far_n = 0;
        if (c->far_n >= MV3N_FAR_FRAMES)
            Mv3nEnd(c, "distance > 4 m for 1 s", now);
    }
    // send
    for (uint32_t s = 0; s < MV3N_SLOTS; s++) {
        Mv3nPeer *p = &c->p[s];
        if (!p->dest || s == c->me)
            continue;
        int member = Mv3nIdxOf(&c->w, s) >= 0;
        for (int k = 0; k < MV3_MAX_F; k++)
            if (c->j[k].on && c->j[k].slot == s)
                member = 1;
        if (!member) {          // it left: keep feeding it until it holds our inputs to its leave frame (it waits for them to confirm), then stop
            if (!p->left_us)
                p->left_us = now;
            if (p->acked + 1 >= p->left_f || now - p->left_us > 1000000) {
                p->dest = 0;
                continue;
            }
        }
        if (c->own_hi > p->sent_hi || now - p->last_tx_us > 50000)
            Mv3nSendInput(c, s, now);
        if (now - p->last_ping_us > 200000)
            Mv3nSendPing(c, s, now);
        if (member && p->last_rx_us
            && now - p->last_rx_us > MV3N_SILENT_US) {
            char d[32];
            snprintf(d, sizeof d, "slot %u", s);
            Mv3nFinish(c, MV3N_END_SILENT, d, 1, now);
            return;
        }
    }
    if (c->io.frame)
        c->io.frame(c->io.user, &c->w);
    // end: the bubble is over on every machine at end_f (deterministic), or we left at our leave frame. Wait for the frames before it to confirm.
    uint32_t ef = c->end_f;
    int why = MV3N_END_OVER;
    if (c->leave_f && c->cur >= c->leave_f && (!ef || c->leave_f <= ef)) {
        ef = c->leave_f;
        why = MV3N_END_LEFT;
    }
    if (ef && c->cur >= ef && (c->conf + 1 >= ef || c->cur >= ef + 30)) {
        Mv3nFinish(c, why, why == MV3N_END_LEFT ? c->leave_why : NULL, 0,
                    now);
        return;
    }
    if (now - c->w_us >= 5000000) {
        float rtt = 0.0f;
        int nr = 0;
        for (uint32_t s = 0; s < MV3N_SLOTS; s++)
            if (c->p[s].dest && c->p[s].have_d) {
                rtt += (float) c->p[s].rtt_us / 1000.0f;
                nr++;
            }
        Mv3nLog(c,
                 "mv3 net: sess %04x f%u conf %u delay %u | 5 s: rollbacks %u (%u frames, max %u), late inputs %u, stalls %u, desyncs %u | rtt %.0f ms, clock %+.1f trim %+.1f ms",
                 c->sess, c->cur, c->conf, c->delay, c->w_rb, c->w_rbf,
                 c->w_rbmax, c->w_late, c->w_stall, c->w_desync,
                 nr ? (double) (rtt / (float) nr) : 0.0,
                 (double) c->off / 1000.0, (double) c->trim / 1000.0);
        c->w_us = now;
        c->w_rb = c->w_rbf = c->w_rbmax = c->w_stall = c->w_desync =
            c->w_late = 0;
    }
}

} // namespace melee::mv3
