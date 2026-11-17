// mv3_net.c - melee v3 net (spec re/melee-v3.md): the bubble session + rollback netcode for the ext. The engine-free core is mv3_net_core.h
// (shared with the offline test tools/mv3/nettest.c); this file is its engine side: MSG_MV3 through g_send_tramp, the QPC clock, our slot / teams /
// psync RTT, the mv3_glue_* callbacks, a lock (recv and tick may come from different threads), and the Net interface of re/melee-v3-brief.md.
// Included by t2r-ext.c after the melee modules (uses xlog, LOGCAP, VMA, g_send_tramp, g_ps_rtt (paired.c), team_of_slot, match_substate).
// Log lines: "mv3 net: ..." - session start (delay, t0, seed) / join / leave / END with the reason and totals, a 5 s summary (rollbacks: count, frames,
// longest; late inputs; stalls; desyncs; rtt; clock offset + trim), desyncs + resyncs, stalls.
#ifndef MSG_MV3
#define MSG_MV3 0x9E            // peer msg (ours): melee v3 bubble / inputs / hash (0x9D is MSG_HELPCALL)
#endif
#define MV3N_LOGCAP(c, n) LOGCAP(c, n)
#include "mv3_net_core.h"
static void Mv3GlueInput(Mv3In * out);       // mv3_glue.c
static void Mv3GlueEvents(const Mv3Evbuf * ev, int resim);
static void Mv3GlueRolledBack(uint32_t to_frame);
static void Mv3GlueFrame(const Mv3World * w);
static int16_t Mv3GlueHp10(uint32_t slot);

static Mv3nCtx g_mv3n;
static CRITICAL_SECTION g_mv3nCs;
static volatile LONG g_mv3nOnce;       // 0 none, 1 initialising, 2 ready
static uint32_t g_mv3nMe = 0xff;
static int64_t g_mv3nGtcOff;
static uint64_t g_mv3nGtcAt;  // GetTickCount64 ms * 1000 - our QPC us (best = largest seen: GetTickCount lags up to 15.6 ms)

static uint64_t Mv3nQpcUs(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER q;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q);
    return (uint64_t) (q.QuadPart / f.QuadPart) * 1000000ull +
        (uint64_t) (q.QuadPart % f.QuadPart) * 1000000ull /
        (uint64_t) f.QuadPart;
}

static void Mv3nESend(void *u, uint32_t to, const uint8_t *b, int len)
{
    (void) u;
    if (g_sendTramp && to < T2R_MAX_PEERS)
        g_sendTramp(MSG_MV3, (uint8_t *) b, (uint64_t) len, to);
}

static void Mv3nEInput(void *u, Mv3In *out)
{
    (void) u;
    Mv3GlueInput(out);
}

static void Mv3nEEvents(void *u, const Mv3Evbuf *ev, int resim)
{
    (void) u;
    Mv3GlueEvents(ev, resim);
}

static void Mv3nERolled(void *u, uint32_t f)
{
    (void) u;
    Mv3GlueRolledBack(f);
}

static void Mv3nEFrame(void *u, const Mv3World *w)
{
    (void) u;
    Mv3GlueFrame(w);
}

static int16_t Mv3nEHp(void *u)
{
    (void) u;
    return Mv3GlueHp10(g_mv3nMe);
}

static int Mv3nETeam(void *u, uint32_t s)
{
    (void) u;
    return TeamOfSlot(s);
}

static float Mv3nERtt(void *u, uint32_t s)
{
    (void) u;
    return s < T2R_MAX_PEERS ? g_psRtt[s] : 0.0f;
}

static void Mv3nELog(void *u, const char *m)
{
    (void) u;
    Xlog("%s", m);
}

static int Mv3nLock(void)
{
    LONG st = InterlockedCompareExchange(&g_mv3nOnce, 1, 0);
    if (st == 0) {
        InitializeCriticalSection(&g_mv3nCs);
        Mv3nIo io =
            { NULL, Mv3nESend, Mv3nEInput, Mv3nEEvents,
       Mv3nERolled, Mv3nEFrame, Mv3nEHp, Mv3nETeam, Mv3nERtt,
       Mv3nELog, NULL };
        Mv3nInit(&g_mv3n, &io, 0xff);
        InterlockedExchange(&g_mv3nOnce, 2);
    } else
        while (g_mv3nOnce != 2)
            Sleep(0);
    EnterCriticalSection(&g_mv3nCs);
    return 1;
}

static void Mv3nUnlock(void)
{
    LeaveCriticalSection(&g_mv3nCs);
}

static void Mv3nSlotCheck(uint64_t now)
{                               // our session slot: a change (rejoin, host migration) ends whatever we had
    uint32_t me = *(uint32_t *) T2R_SYM(kMySlot);
    if (me >= T2R_MAX_PEERS)
        me = 0xff;
    if (me == g_mv3nMe)
        return;
    if (g_mv3n.state != MV3N_IDLE)
        Mv3nHardEnd(&g_mv3n, MV3N_END_ABORT, "our slot changed", now);
    Xlog("mv3 net: our slot %u -> %u", g_mv3nMe, me);
    g_mv3nMe = me;
    g_mv3n.me = (uint8_t) me;
}

// ---------------------------------------------------------------- Net interface (re/melee-v3-brief.md)
static void Mv3NetTick(uint64_t now_ms)
{
    (void) now_ms;              // the glue's GetTickCount64: too coarse for the clock sync, QPC is used inside
    if (!g_winmm)
        return;
    Mv3nLock();
    uint64_t now = Mv3nQpcUs();
    {
        int64_t d = (int64_t) GetTickCount64() * 1000 - (int64_t) now;
        if (!g_mv3nGtcAt || d > g_mv3nGtcOff
            || now - g_mv3nGtcAt > 10000000) {
            g_mv3nGtcOff = d;
            g_mv3nGtcAt = now;
        }
    }
    Mv3nSlotCheck(now);
    if (MatchSubstate() != 6) {
        if (g_mv3n.state != MV3N_IDLE)
            Mv3nHardEnd(&g_mv3n, MV3N_END_MATCH, NULL, now);
    } else if (g_mv3nMe < T2R_MAX_PEERS)
        Mv3nTick(&g_mv3n, now);
    Mv3nUnlock();
}

static void Mv3Recv(uint32_t from, const uint8_t *buf, uint64_t len)
{
    if (!g_winmm || from >= T2R_MAX_PEERS || !buf || len < 3
        || len > MV3N_PKT)
        return;
    Mv3nLock();
    if (g_mv3nMe < T2R_MAX_PEERS && MatchSubstate() == 6)
        Mv3nRecv(&g_mv3n, from, buf, len, Mv3nQpcUs());
    Mv3nUnlock();
}

// 1 = a bubble with that slot runs, or the proposal / join request / invite is on its way (glue: check mv3_net_active() / idx_of_slot later); 0 = refused
// (teammate, busy with another proposal, leaving, not in a match)
static int Mv3NetPropose(uint32_t slot)
{
    if (!g_winmm || slot >= T2R_MAX_PEERS || MatchSubstate() != 6)
        return 0;
    Mv3nLock();
    int r =
        g_mv3nMe < T2R_MAX_PEERS ? Mv3nPropose(&g_mv3n, slot,
                                                 Mv3nQpcUs()) : 0;
    Mv3nUnlock();
    return r;
}

// our own leave: a bit in our input stream at the next frame we sample, so every machine drops our fighter at the same frame. A 1v1 bubble then ends on
// both machines at that frame ("bubble over"); in a 3+ bubble the others fight on.
static void Mv3NetEnd(const char *why)
{
    Mv3nLock();
    Mv3nEnd(&g_mv3n, why, Mv3nQpcUs());
    Mv3nUnlock();
}

static int Mv3NetActive(void)
{
    return g_mv3n.state == MV3N_ACTIVE;
}

static const Mv3World *Mv3NetWorld(void)
{
    return &g_mv3n.w;
}

static int Mv3NetLocalIdx(void)
{
    return g_mv3n.state == MV3N_ACTIVE ? Mv3nIdxOf(&g_mv3n.w,
                                                     g_mv3n.me) : -1;
}

static uint32_t Mv3NetConfirmed(void)
{
    return g_mv3n.conf;
}

static int Mv3NetIdxOfSlot(uint32_t slot)
{
    return g_mv3n.state == MV3N_ACTIVE ? Mv3nIdxOf(&g_mv3n.w, slot) : -1;
}

static int Mv3NetSlotOfIdx(int idx)
{
    return idx >= 0 && idx < MV3_MAX_F
        && g_mv3n.w.f[idx].slot != 0xff ? g_mv3n.w.f[idx].slot : -1;
}

static uint32_t Mv3NetFrameMs(uint32_t frame)
{                               // GetTickCount64 time (low 32 bits) of a world frame on this machine
    if (g_mv3n.state != MV3N_ACTIVE)
        return (uint32_t) GetTickCount64();
    return (uint32_t) (((int64_t) Mv3nFrameLocalUs(&g_mv3n, frame) +
                        g_mv3nGtcOff) / 1000);
}

// the owner's engine hp x10 (glue, at bubble start and on change). Only our own fighter's: it rides in our input stream (hp_set at one frame, applied by
// every sim before that step); a report for another fighter is ignored - its owner reports it.
static void Mv3NetSetHp(int idx, int16_t hp10)
{
    Mv3nLock();
    int li = Mv3NetLocalIdx();
    if (idx < 0 || idx == li)
        Mv3nSetHp(&g_mv3n, hp10);
    else {
        static int nl;
        if (LOGCAP(nl, 8))
            Xlog("mv3 net: hp report for fighter %d ignored (we are %d; each owner reports its own)", idx, li);
    }
    Mv3nUnlock();
}

// drop a fighter (downed) from the bubble. Ours: our leave bit (agreed frame = the next one we sample; `frame` is not needed). Another slot's: its owner
// leaves through its own stream (a remote can not be removed at an agreed frame from here), so this only logs.
static void Mv3NetRemove(uint32_t slot, uint32_t frame)
{
    Mv3nLock();
    if (slot == g_mv3nMe)
        Mv3nEnd(&g_mv3n, "removed (down)", Mv3nQpcUs());
    else {
        static int nl;
        if (LOGCAP(nl, 8))
            Xlog("mv3 net: remove of slot %u at frame %u ignored - its owner leaves through its own input stream", slot, frame);
    }
    Mv3nUnlock();
}

#ifdef MV3N_GLUE_STUBS
// inert callbacks so t2r-ext.c compiles without mv3_glue.c; the real ones live in mv3_glue.c.
static void Mv3GlueInput(Mv3In *out)
{
    if (out)
        memset(out, 0, sizeof *out);
}

static void Mv3GlueEvents(const Mv3Evbuf *ev, int resim)
{
    (void) ev;
    (void) resim;
}

static void Mv3GlueRolledBack(uint32_t to_frame)
{
    (void) to_frame;
}

static void Mv3GlueFrame(const Mv3World *w)
{
    (void) w;
}

static int16_t Mv3GlueHp10(uint32_t slot)
{
    (void) slot;
    return -1;
}
#endif
