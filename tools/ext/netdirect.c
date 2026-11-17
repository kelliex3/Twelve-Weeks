// netdirect.c - direct melee path (re/netdirect-1026.md). Included by t2r-ext.c after melee_hitreact.c (uses g_ps_rtt, xlog, VMA, HM_ROLE, exe_side_open).
// The game's traffic is a star: a client's ext message to another client goes client -> host -> client (two Steam legs). Live: direct Steam
// ping 140 ms, the melee path 181 + 88 ms. The melee-critical ext messages (hit-rel, dodge swing / press / res, hit reaction, psync START / ABORT / END)
// are ALSO sent straight to the peer through the Steam helper (t2r-steam.c, opcodes 05 / 06 / 07 on its loopback port, which winmm's relay address
// names: ctx+0 "127.0.0.1", ctx+0x10 port). The relay copy still goes out; the receiver keeps the first copy of each message (sender, type, bytes,
// 800 ms; a repeat on the SAME path is a deliberate resend and is kept). Per-peer ping / pong over the same direct path measures its RTT; a
// peer is used only while a pong is fresh and the direct RTT is below the psync RTT through the host. Old ext / old helper never answer = never used.
// Only a client uses it (the host's messages already go straight to everybody). Opt-out: t2r-no-direct-melee.txt next to the exe.
// Wire ext <-> helper: [05][dst slot or src slot][kind][...]; kind 1 [type][payload] message, 2 [f64 t] ping, 3 [f64 t] pong.
#define ND_WINDOW_MS 800
#define ND_RING 128
typedef int (WINAPI * nd_fn_connect) (uintptr_t, const void *, int);
typedef int (WINAPI * nd_fn_send) (uintptr_t, const char *, int, int);
typedef int (WINAPI * nd_fn_recv) (uintptr_t, char *, int, int);
typedef uintptr_t(WINAPI * nd_fn_socket) (int, int, int);
typedef int (WINAPI * nd_fn_ioctl) (uintptr_t, long, unsigned long *);
typedef int (WINAPI * nd_fn_close) (uintptr_t);
static struct {
    int off, inited, up;        // up = the helper answered our hello
    uintptr_t s;
    nd_fn_send send;
    nd_fn_recv recv;
    nd_fn_close close;
    uint64_t next_open, next_hello, next_ping;
    float rtt[T2R_MAX_PEERS];   // direct RTT EWMA, 0 = none
    uint64_t pong_ms[T2R_MAX_PEERS];
    int use[T2R_MAX_PEERS];     // last decision (for the change log)
    uint64_t log_ms[T2R_MAX_PEERS];
    uint32_t tx[T2R_MAX_PEERS], rx[T2R_MAX_PEERS], dup[T2R_MAX_PEERS];
    int delivering;
    struct {
        uint64_t ms;
        uint32_t h;
        uint8_t slot, type, path, matched;
    } ring[ND_RING];
    int ri;
} g_nd;

static int NdTypeOk(uint8_t type, const uint8_t *buf, uint64_t len)
{
    if (len < 1 || len > 120)
        return 0;
    switch (type) {
    case 0x6C:
    case 0x97:
    case 0x98:
    case 0x99:
    case 0x9A:
    case 0x9C:
        return 1;               // MSG_HIT_REL, DODGE_SWING / RES / PRESS, HITREACT2, MEX (melee_exchange.c)
    case 0x4B:
        return buf[0] != PS_OP_PING && buf[0] != PS_OP_PONG;
    case 0x9E:
        return buf[0] != 11 && buf[0] != 12;    // MSG_MV3 (melee v3 inputs / control; its PING / PONG measure the relay clock)      // MSG_PSYNC: START / ABORT / END / ACK (the ping measures the relay path)
    }
    return 0;
}

static uint32_t NdHash(uint8_t type, const uint8_t *b, uint64_t len)
{
    uint32_t h = 2166136261u ^ type;
    for (uint64_t i = 0; i < len; i++)
        h = (h ^ b[i]) * 16777619u;
    return h;
}

// 1 = a copy of this message already came over the OTHER path (drop this one)
static int NdDup(uint32_t slot, uint8_t type, const uint8_t *buf,
                  uint64_t len, int path)
{
    if (slot >= T2R_MAX_PEERS)
        return 0;
    uint64_t now = GetTickCount64();
    uint32_t h = NdHash(type, buf, len);
    for (int k = 0; k < ND_RING; k++) {
        int i = (g_nd.ri - 1 - k + ND_RING * 2) % ND_RING;
        if (!g_nd.ring[i].ms || now - g_nd.ring[i].ms > ND_WINDOW_MS)
            break;
        if (g_nd.ring[i].slot == slot && g_nd.ring[i].type == type
            && g_nd.ring[i].h == h && g_nd.ring[i].path != path
            && !g_nd.ring[i].matched) {
            g_nd.ring[i].matched = 1;
            g_nd.dup[slot]++;
            return 1;
        }
    }
    int i = g_nd.ri;
    g_nd.ri = (g_nd.ri + 1) % ND_RING;
    g_nd.ring[i].ms = now;
    g_nd.ring[i].h = h;
    g_nd.ring[i].slot = (uint8_t) slot;
    g_nd.ring[i].type = type;
    g_nd.ring[i].path = (uint8_t) path;
    g_nd.ring[i].matched = 0;
    return 0;
}

// recv_hook_body, first thing for our message types: the relayed copy (path 0)
static int NdRelayIn(uint64_t slot, uint64_t type, const uint8_t *buf,
                       uint64_t len)
{
    if (!g_nd.up || g_nd.delivering || !buf)
        return 0;
    if (!NdTypeOk((uint8_t) type, buf, len))
        return 0;
    return NdDup((uint32_t) (uint8_t) slot, (uint8_t) type, buf, len, 0);
}

static int NdUse(int slot)
{
    if (!g_nd.up || slot <= 0 || slot >= T2R_MAX_PEERS
        || g_nd.rtt[slot] <= 0.0f)
        return 0;
    uint64_t now = GetTickCount64();
    float rel = g_psRtt[slot];
    int ok = now - g_nd.pong_ms[slot] < 3000 && (rel <= 0.0f
                                                 || g_nd.rtt[slot] <
                                                 rel * 0.9f);
    if (ok != g_nd.use[slot] || (ok && now - g_nd.log_ms[slot] > 20000)) {
        Xlog("direct melee path slot %d: rtt %.0f vs relay %.0f ms -> %s (sent %u, got %u, relay copies dropped %u)", slot, g_nd.rtt[slot], rel, ok ? "ON" : "off", g_nd.tx[slot], g_nd.rx[slot], g_nd.dup[slot]);
        g_nd.use[slot] = ok;
        g_nd.log_ms[slot] = now;
    }
    return ok;
}

static void NdRaw(int dst, uint8_t kind, const uint8_t *pl, int plen)
{
    uint8_t m[160];
    if (!g_nd.s || plen > 150)
        return;
    m[0] = 0x05;
    m[1] = (uint8_t) dst;
    m[2] = kind;
    if (plen)
        memcpy(m + 3, pl, (size_t) plen);
    g_nd.send(g_nd.s, (const char *) m, 3 + plen, 0);
}

// send_gate_body, after the hold check: an ext message of ours goes direct as well
static void NdOut(uint64_t type, const uint8_t *buf, uint64_t len,
                   uint64_t dst)
{
    if (!g_nd.up || !NdTypeOk((uint8_t) type, buf, len)
        || *(volatile uint8_t *) T2R_SYM(kRoleByte) != 2)
        return;
    uint32_t me = *(uint32_t *) T2R_SYM(kMySlot);
    for (int s = 1; s < T2R_MAX_PEERS; s++) {
        if ((uint8_t) dst != 0xff && (uint8_t) dst != s)
            continue;
        if ((uint32_t) s == me || !NdUse(s))
            continue;
        uint8_t m[130];
        m[0] = (uint8_t) type;
        memcpy(m + 1, buf, (size_t) len);
        NdRaw(s, 1, m, (int) len + 1);
        g_nd.tx[s]++;
    }
}

static void NdOpen(void)
{
    const char *ip = (const char *) T2R_SYM(kNetCtx);
    uint16_t port = *(uint16_t *) (T2R_SYM(kNetCtx) + 0x10);
    if (strncmp(ip, "127.0.0.1", 10) || !port)
        return;                 // not a Steam-helper match
    HMODULE ws = LoadLibraryA("ws2_32.dll");
    if (!ws)
        return;
    nd_fn_socket sk = (nd_fn_socket) GetProcAddress(ws, "socket");
    nd_fn_connect cn = (nd_fn_connect) GetProcAddress(ws, "connect");
    nd_fn_ioctl io = (nd_fn_ioctl) GetProcAddress(ws, "ioctlsocket");
    g_nd.send = (nd_fn_send) GetProcAddress(ws, "send");
    g_nd.recv = (nd_fn_recv) GetProcAddress(ws, "recv");
    g_nd.close = (nd_fn_close) GetProcAddress(ws, "closesocket");
    if (!sk || !cn || !io || !g_nd.send || !g_nd.recv || !g_nd.close)
        return;
    uintptr_t s = sk(2, 2, 17);
    if (s == (uintptr_t) ~ 0ULL || !s)
        return;
    T2rSin a = { 2, 0, 0, { 0 }
    };
    a.port = (uint16_t) ((port >> 8) | (port << 8));
    a.addr = 0x0100007F;
    unsigned long nb = 1;
    if (cn(s, &a, sizeof a) || io(s, (long) 0x8004667E, &nb)) {
        g_nd.close(s);
        return;
    }
    g_nd.s = s;
    g_nd.up = 0;
    g_nd.next_hello = 0;
    Xlog("direct melee path: socket to the helper on 127.0.0.1:%u",
         (unsigned) port);
}

static void NdClose(void)
{
    if (g_nd.s) {
        g_nd.close(g_nd.s);
        g_nd.s = 0;
    }
    g_nd.up = 0;
    memset(g_nd.rtt, 0, sizeof g_nd.rtt);
    memset(g_nd.pong_ms, 0, sizeof g_nd.pong_ms);
    memset(g_nd.use, 0, sizeof g_nd.use);
}

static void NdTick(void)
{
    if (!g_winmm || g_nd.off)
        return;
    if (!g_nd.inited) {
        g_nd.inited = 1;
        FILE *f = ExeSideOpen("t2r-no-direct-melee.txt", "r");
        if (f) {
            fclose(f);
            g_nd.off = 1;
            Xlog("direct melee path OFF (t2r-no-direct-melee.txt)");
            return;
        }
    }
    uint64_t now = GetTickCount64();
    int in_match = MatchSubstate() == 6
        && *(volatile uint8_t *) T2R_SYM(kRoleByte) == 2;
    if (!in_match) {
        if (g_nd.s)
            NdClose();
        return;
    }
    if (!g_nd.s) {
        if (now < g_nd.next_open)
            return;
        g_nd.next_open = now + 2000;
        NdOpen();
        if (!g_nd.s)
            return;
    }
    if (now >= g_nd.next_hello) {
        uint8_t h = 0x06;
        g_nd.send(g_nd.s, (const char *) &h, 1, 0);
        g_nd.next_hello = now + (g_nd.up ? 5000 : 1000);
    }
    if (g_nd.up && now >= g_nd.next_ping) {
        g_nd.next_ping = now + 500;
        double t = PsyncMs();
        uint32_t me = *(uint32_t *) T2R_SYM(kMySlot);
        for (int s = 1; s < T2R_MAX_PEERS; s++)
            if ((uint32_t) s != me)
                NdRaw(s, 2, (const uint8_t *) &t, 8);
    }
    for (int k = 0; k < 64; k++) {
        uint8_t b[400];
        int n = g_nd.recv(g_nd.s, (char *) b, sizeof b, 0);
        if (n <= 0)
            break;
        if (b[0] == 0x07) {
            if (!g_nd.up) {
                g_nd.up = 1;
                Xlog("direct melee path: helper supports it");
            }
            continue;
        }
        if (b[0] != 0x05 || n < 4)
            continue;
        uint32_t src = b[1];
        uint8_t kind = b[2];
        if (src >= T2R_MAX_PEERS)
            continue;
        if (kind == 2 && n >= 11) {
            NdRaw((int) src, 3, b + 3, 8);
            continue;
        }                       // ping -> pong straight back
        if (kind == 3 && n >= 11) {
            double t;
            memcpy(&t, b + 3, 8);
            double rtt = PsyncMs() - t;
            if (rtt > 0 && rtt < 3000) {
                g_nd.rtt[src] =
                    g_nd.rtt[src] >
                    0 ? g_nd.rtt[src] * 0.75f +
                    (float) rtt *0.25f : (float) rtt;
                g_nd.pong_ms[src] = now;
            }
            continue;
        }
        if (kind == 1 && n >= 5) {
            uint8_t type = b[3];
            if (!NdTypeOk(type, b + 4, (uint64_t) (n - 4)))
                continue;
            g_nd.rx[src]++;
            if (NdDup(src, type, b + 4, (uint64_t) (n - 4), 1))
                continue;       // the relay copy came first
            g_nd.delivering = 1;
            RecvHook(src, type, b + 4, (uint64_t) (n - 4));
            g_nd.delivering = 0;
        }
    }
}
