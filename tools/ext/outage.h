// outage.h - every ext -> daemon request in one place (re/ext-daemon-outage-1019.md). Included by t2r-ext.c before clan_worker; the offline
// harness tests/outage_probe.c + tests/outage_test.py compile and run this same header against a fake daemon.
//
// t2r_rpc: one short TCP request/response, [u16 len][op][payload] both ways. Bounded in EVERY phase: non-blocking connect (conn_ms), send
// (SO_SNDTIMEO io_ms), and the whole reply within reply_ms (not per recv: a daemon that trickles one byte per timeout can no longer hold a
// worker - and the busy flag it owns - for minutes). A reply whose header says it cannot fit the buffer is cut at once (no waiting for bytes
// that would be thrown away). The workers run on their own threads; none of this is called from the tick.
// Also: jittered exponential backoff (t2r_backoff_ms) and a "the daemon proved it is alive" stamp (srv_mark_alive / srv_alive_within), so
// "no answer" can be told apart from "an older server that never answers this op" - counters that give a feature up for the session
// (notice 0xBA, perf 0xCA, hwid 0xB2) only count while the daemon is known to be up.
#ifndef T2R_OUTAGE_H
#define T2R_OUTAGE_H
#include <windows.h>
#include <stdint.h>

typedef struct {
    uint16_t family, port;
    uint32_t addr;
    char zero[8];
} T2rSin;
typedef struct {
    HMODULE ws;
     uintptr_t(WINAPI * socket) (int, int, int);
    int (WINAPI * send) (uintptr_t, const char *, int, int);
    int (WINAPI * recv) (uintptr_t, char *, int, int);
    int (WINAPI * closesocket) (uintptr_t);
    int (WINAPI * setsockopt) (uintptr_t, int, int, const char *, int);
} T2rNet;
static int T2rNetLoad(T2rNet *n, HMODULE ws)
{                               // 1 = every function found
    n->ws = ws;
    if (!ws)
        return 0;
    n->socket = (void *) GetProcAddress(ws, "socket");
    n->send = (void *) GetProcAddress(ws, "send");
    n->recv = (void *) GetProcAddress(ws, "recv");
    n->closesocket = (void *) GetProcAddress(ws, "closesocket");
    n->setsockopt = (void *) GetProcAddress(ws, "setsockopt");
    return n->socket && n->send && n->recv && n->closesocket
        && n->setsockopt;
}

// connect() with a timeout (the plain blocking connect to an unreachable server holds a worker, and g_clan_busy / g_hm_have with it, for
// ~21 s): non-blocking connect + select. 0 = connected (socket back in blocking mode), -1 = failed / timed out. ws = ws2_32.
static int T2rConnectTo(HMODULE ws, uintptr_t s, const T2rSin *a,
                          int ms)
{
    typedef int (WINAPI * fn_connect) (uintptr_t, const void *, int);
    typedef int (WINAPI * fn_ioctl) (uintptr_t, long, unsigned long *);
    typedef int (WINAPI * fn_select) (int, void *, void *, void *,
                                      const void *);
    typedef int (WINAPI * fn_getopt) (uintptr_t, int, int, char *, int *);
    fn_connect cn = (fn_connect) GetProcAddress(ws, "connect");
    fn_ioctl io = (fn_ioctl) GetProcAddress(ws, "ioctlsocket");
    fn_select sel = (fn_select) GetProcAddress(ws, "select");
    fn_getopt go = (fn_getopt) GetProcAddress(ws, "getsockopt");
    if (!cn)
        return -1;
    if (!io || !sel || !go)
        return cn(s, a, sizeof *a) ? -1 : 0;    // (no select: the old blocking connect)
    unsigned long nb = 1;
    io(s, (long) 0x8004667E /* FIONBIO */ , &nb);
    int r = cn(s, a, sizeof *a), ok = r == 0;
    if (r) {
        struct {
            unsigned count;
            uintptr_t arr[2];
        } wr = { 1, { s, 0 } }, er = { 1, { s, 0 } };
        struct {
            long sec, usec;
        } tv = { ms / 1000, (ms % 1000) * 1000 };
        int n = sel(0, NULL, &wr, &er, &tv);
        if (n > 0 && wr.count) {
            int err = 1, len = 4;
            ok = go(s, 0xffff, 0x1007 /* SO_ERROR */ , (char *) &err,
                    &len) == 0 && err == 0;
        }
    }
    nb = 0;
    io(s, (long) 0x8004667E, &nb);
    return ok ? 0 : -1;
}

// 1 when [rep, got) holds a complete frame; *total = its size. -1 = the header says it can never fit in cap (stop waiting).
// 0 = more bytes needed. A frame is [u16 len][len bytes] with len >= 1 (the op byte); len 0 is garbage and never completes.
static int T2rFrameState(const char *rep, int got, int cap, int *total)
{
    if (got < 3)
        return 0;
    int ln = (rep[0] & 0xff) + ((rep[1] & 0xff) << 8);
    if (total)
        *total = ln + 2;
    if (ln < 1)
        return 0;
    if (ln + 2 > cap)
        return -1;
    return got >= ln + 2 ? 1 : 0;
}

// One request. Returns the bytes received (>= 0); *sent = 1 when connect + send went through (the daemon was reachable), 0 when not.
// The caller checks the frame itself (t2r_frame_state): a short / garbled / foreign reply is just bytes here.
static int T2rRpc(const T2rNet *n, const T2rSin *a, const void *frame,
                   int flen, char *rep, int cap, int conn_ms, int io_ms,
                   int reply_ms, int *sent)
{
    int got = 0;
    if (sent)
        *sent = 0;
    uintptr_t s = n->socket(2, 1, 6);
    if (s == (uintptr_t) ~ 0ULL)
        return 0;
    DWORD to = (DWORD) io_ms;
    n->setsockopt(s, 0xffff, 0x1005 /* SO_SNDTIMEO */ , (const char *) &to,
                  4);
    if (!T2rConnectTo(n->ws, s, a, conn_ms)
        && n->send(s, (const char *) frame, flen, 0) == flen) {
        if (sent)
            *sent = 1;
        ULONGLONG end = GetTickCount64() + (ULONGLONG) reply_ms;
        while (got < cap) {
            ULONGLONG now = GetTickCount64();
            if (now >= end)
                break;
            ULONGLONG rem = end - now;
            to = (DWORD) (rem <
                          (ULONGLONG) io_ms ? rem : (ULONGLONG) io_ms);
            n->setsockopt(s, 0xffff, 0x1006 /* SO_RCVTIMEO */ ,
                          (const char *) &to, 4);
            int r = n->recv(s, rep + got, cap - got, 0);
            if (r <= 0)
                break;
            got += r;
            if (T2rFrameState(rep, got, cap, NULL) != 0)
                break;
        }
    }
    n->closesocket(s);
    return got;
}

// ---- backoff + liveness -------------------------------------------------------------------------------------------------------------------
static uint32_t T2rRng(void)
{                               // xorshift32 (jitter only; threads racing on it is harmless)
    static uint32_t s;
    if (!s)
        s = ((uint32_t) GetTickCount() *
             2654435761u) ^ (uint32_t) GetCurrentProcessId() ^ 0x9e3779b9u;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

// ms to hold off after `fails` failures in a row: base, 2 base, 4 base ... capped, then +-25 % jitter. 0 failures = 0.
static unsigned T2rBackoffMs(int fails, unsigned base, unsigned cap)
{
    if (fails < 1)
        return 0;
    unsigned d = base;
    for (int i = 1; i < fails && d < cap; i++)
        d *= 2;
    if (d > cap)
        d = cap;
    unsigned j = d / 4;
    return d - j + (j ? T2rRng() % (2 * j + 1) : 0);
}

static volatile LONG g_srvAliveT;     // GetTickCount() of the last reply any ext request got from the daemon (0 = none yet)
static void SrvMarkAlive(void)
{
    LONG t = (LONG) GetTickCount();
    InterlockedExchange(&g_srvAliveT, t ? t : 1);
}

static int SrvAliveWithin(unsigned ms)
{
    LONG t = g_srvAliveT;
    return t && (uint32_t) GetTickCount() - (uint32_t) t < ms;
}
#endif
