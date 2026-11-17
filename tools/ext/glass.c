// Included from t2r-ext.c after meleeenv.c (uses xlog / g_send_tramp / g_frame / t2r_peer / exe_side_open).
// Synced breakable glass / windows. Destruction is local: a window shot or smashed on one
// screen stayed whole on the others. TLOU2's windows are BigBreakable (a SimpleBreakable subclass; spawner types "BigBreakable" /
// "BigBreakableLarge", type hash 0xbfc75a7abb5d8524 / ctor 0x140848410, SimpleBreakable 0xf7ccab701b37e966 / ctor 0x14085f090).
//
// Engine break path (static RE of tlou-ii.exe 1.6.0 PC, exe addresses):
//  - every event sent to a breakable ends in the base handler 0x14084a820(obj, Event*) (SimpleBreakable vtable +0x70 = 0x14085f1e0 -> jmp
//    0x14084a820; BigBreakable's 0x140848970 handles a few events and tail-jumps to it). Event layout (generic engine Event): +0 hash,
//    +8 sender handle, +0x18 result slot, +0x28 + 0x10*i args (value u64, aux u32, tag u16 at +0xC), +0x128 int16 arg count.
//  - the break itself is the event 0x2ccc09d462355d58 (built inline by the Havok destruction code, exe 0x1420fdfa0): arg 2 (+0x48) = the
//    broken piece index (read through 0x1414cf020); the handler sets obj+0x640 bit0 (= broken), runs the break fx events (0x55359e1822896248,
//    random pick from the object's fx list), vfunc 0xb28(obj, 1) and 0x141aff2c0(obj, 0) (collision / simulation off).
//  - the stock net path for this (NetEventBreakableAttackProjectile / Explosion / SyncBreakable: send fns 0x140931fa0 / 0x140931cb0 /
//    0x140938cd0, receive Execute 0x140924030 / 0x140923e20 / 0x14092aa80, objects found by obj+0xc0 through 0x141b0a970) is dead here: the
//    send fns have no caller in the exe, and the attack handler only calls 0x1409316a0 when the net game byte 0x144082fbb is set (it is 0).
// Mod: hook 0x14084a820. A break event on OUR screen (not one we replay ourselves) -> MSG_GLASS to every peer: the pane's id (obj+0xc0, the
// object's level name sid; the receiver finds it with 0x141b0a970, else with a registry of the panes the hook has seen), the piece index and the
// event's three args. The receiver replays the same event on that pane through the engine's own event sender 0x1415734d0 (the one the coat
// reboot uses), so the pane runs its own break handler: state, fx, sound, collision off. Pieces already broken here (our own physics got
// there first) are skipped; the replay sets a flag so the hook never echoes it. A peer that joins (slot becomes active) gets the list of
// the panes broken on our screen. Opt-out: t2r-no-glass-sync.txt next to the exe. Logs "glass: ...".
// registry cap fixed (see gl_reg_get), queue 256, per-piece joiner list, send log cap 300.
// MSG_GLASS 0x7A: [u8 1][u64 pane id][u32 piece][u8 nargs][nargs x 16 bytes event arg slots]   one break
//                 [u8 2][u8 n][n x ([u64 pane id][u32 piece])]                                  panes broken here (to a joiner)
#define MSG_GLASS 0x7A
#define GL_BREAK 0x2ccc09d462355d58ULL
#define GL_REG_MAX 2048
#define GL_HASH 8192            // open-addressing index over g_gl_reg (by object address)
#define GL_Q 256                // received-message queue (a shattering pane sends one message per piece: dozens in one burst)
typedef void (*gl_fn_ev)(uint8_t * obj, uint8_t * ev);
typedef uint8_t *(*gl_fn_find)(uint64_t id);
typedef void (*gl_fn_sendev)(uint8_t * out, uint8_t * sender, uint64_t sid,
                             uint8_t * target, uint8_t * a0, uint8_t * a1,
                             uint8_t * a2, uint8_t * a3);
static gl_fn_ev g_glOrig;
static gl_fn_find g_glFind;
static gl_fn_sendev g_glSendev;
static int g_glOn;             // hook installed and not opted out
#ifndef T2R_RELEASE
static int g_glDevOwn;        // t2r-dev-glass.txt: gl_replay delivers the break as OUR break (the hook sends MSG_GLASS)
#endif
static volatile int g_glReplay;        // we are delivering a peer's break: the hook must not send it back
static struct {
    uint8_t *obj;
    uint64_t id, vt, done[4];
    uint32_t piece;
} g_gl_reg[GL_REG_MAX];         // panes seen by the hook (done = bit mask of pieces 0..255 that got their break event here)
static int16_t g_glHash[GL_HASH];      // index + 1 into g_gl_reg, 0 = empty
static int g_glNreg;
static uint32_t g_gl_nsent, g_gl_nrecv, g_gl_ndrop, g_gl_npurge;        // stats, logged as 'glass: stats'
static struct {
    uint8_t slot, op, nargs;
    uint64_t id;
    uint32_t piece;
    uint8_t args[48];
} g_gl_q[GL_Q];                 // received, applied on the game thread
static volatile uint32_t g_gl_qh, g_gl_qt;

static int GlLive(const uint8_t *obj, uint64_t id)
{
    return obj && !IsBadReadPtr(obj, 0x700)
        && *(const uint64_t *) (obj + 0xc0) == id
        && *(const uint64_t *) obj != 0;
}

// the old registry was a 256-entry linear table that was never cleared. The level has far more
// breakables than that (windows, car glass, bookshelves, dishes, bottles; every event any of them gets registered it) and the entries of freed
// objects from earlier rounds stayed forever, so after a few matches the table was full, gl_reg_get(create) returned -1 and the hook sent NOTHING
// any more (the receiver side kept working, hence 'a handful of breaks' in the logs). Now: hashed by object address, 2048 entries, and when it
// fills up only the live BROKEN panes (the joiner list needs them) are kept - unbroken ones are re-learned from their next event.
static unsigned GlHidx(const uint8_t *obj)
{
    return (unsigned) (((uintptr_t) obj >> 4) *
                       2654435761u >> 7) & (GL_HASH - 1);
}

static void GlDoneClear(int i)
{
    memset(g_gl_reg[i].done, 0, sizeof g_gl_reg[i].done);
}

static int GlDoneHas(int i, uint32_t piece)
{
    return piece < 256
        && (g_gl_reg[i].done[piece >> 6] >> (piece & 63) & 1);
}

static void GlDoneSet(int i, uint32_t piece)
{
    if (piece < 256)
        g_gl_reg[i].done[piece >> 6] |= 1ULL << (piece & 63);
}

static int GlDoneAny(int i)
{
    return (g_gl_reg[i].done[0] | g_gl_reg[i].done[1] | g_gl_reg[i].
            done[2] | g_gl_reg[i].done[3]) != 0;
}

static void GlRegPurge(void)
{
    int k = 0;
    for (int i = 0; i < g_glNreg; i++) {
        uint8_t *o = g_gl_reg[i].obj;
        if (GlLive(o, g_gl_reg[i].id) && *(uint64_t *) o == g_gl_reg[i].vt
            && (o[0x640] & 1)) {
            if (k != i)
                g_gl_reg[k] = g_gl_reg[i];
            k++;
        }
    }
    Xlog("glass: registry full (%d panes) - kept the %d live broken ones",
         g_glNreg, k);
    g_glNreg = k;
    g_gl_npurge++;
    memset(g_glHash, 0, sizeof g_glHash);
    for (int i = 0; i < k; i++) {
        unsigned h = GlHidx(g_gl_reg[i].obj);
        while (g_glHash[h])
            h = (h + 1) & (GL_HASH - 1);
        g_glHash[h] = (int16_t) (i + 1);
    }
}

// registry entry of a pane object (created on first sight); a recycled address (other id / vtable) starts fresh
static int GlRegGet(uint8_t *obj, int create)
{
    uint64_t id = *(uint64_t *) (obj + 0xc0), vt = *(uint64_t *) obj;
    unsigned h = GlHidx(obj);
    while (g_glHash[h]) {
        int i = g_glHash[h] - 1;
        if (g_gl_reg[i].obj == obj) {
            if (g_gl_reg[i].id != id || g_gl_reg[i].vt != vt) {
                g_gl_reg[i].id = id;
                g_gl_reg[i].vt = vt;
                GlDoneClear(i);
                g_gl_reg[i].piece = 0xffffffffu;
            }
            return i;
        }
        h = (h + 1) & (GL_HASH - 1);
    }
    if (!create)
        return -1;
    if (g_glNreg >= GL_REG_MAX) {
        GlRegPurge();
        return GlRegGet(obj, create);
    }
    int i = g_glNreg++;
    g_gl_reg[i].obj = obj;
    g_gl_reg[i].id = id;
    g_gl_reg[i].vt = vt;
    GlDoneClear(i);
    g_gl_reg[i].piece = 0xffffffffu;
    g_glHash[h] = (int16_t) (i + 1);
    return i;
}

static uint8_t *GlFindPane(uint64_t id, int *via_reg)
{
    *via_reg = 0;
    uint8_t *o = g_glFind ? g_glFind(id) : NULL;
    if (GlLive(o, id))
        return o;
    for (int i = 0; i < g_glNreg; i++)
        if (g_gl_reg[i].id == id && GlLive(g_gl_reg[i].obj, id)
            && *(uint64_t *) g_gl_reg[i].obj == g_gl_reg[i].vt) {
            *via_reg = 1;
            return g_gl_reg[i].obj;
        }
    return NULL;
}

static uint32_t GlArgInt(const uint8_t *a)
{                               // an event arg as 0x1414cf020 reads it (tag 1 bool, 2-5 / 10 u32, 6 float)
    uint16_t tag = *(const uint16_t *) (a + 0xc);
    if (tag == 1)
        return a[0] != 0;
    if (tag == 6)
        return (uint32_t) (int32_t) * (const float *) a;
    return *(const uint32_t *) a;
}

static int GlTagScalar(uint16_t t)
{
    return t == 1 || t == 2 || t == 3 || t == 6 || t == 10;
}

static int GlTagInt(uint16_t t)
{
    return t == 2 || t == 3 || t == 4 || t == 5 || t == 10;
}

// perf: this hook sits on the base handler of EVERY event sent to any breakable (SimpleBreakable / BigBreakable), so the common case - a
// pane we already know, an event that is not the break - must cost a few loads: a 64-entry direct-mapped cache of {obj, id, vtable} skips the two
// IsBadReadPtr probes and the 256-entry registry scan (those run only for a pane's first event, a recycled address and the break itself).
static struct {
    uint8_t *obj;
    uint64_t id, vt;
} g_gl_fast[64];
static void GlPre(uint8_t *obj, uint8_t *ev)
{
    if (g_glOn && obj && ev && !IsBadReadPtr(ev, 0x130)
        && !IsBadReadPtr(obj, 0x700)) {
        if (*(uint64_t *) ev == GL_BREAK) {
            int ri = GlRegGet(obj, 1);
            uint64_t id = *(uint64_t *) (obj + 0xc0);
            int16_t nargs = *(int16_t *) (ev + 0x128);
            uint32_t piece =
                nargs >= 3 ? GlArgInt(ev + 0x48) : 0xffffffffu;
            if (ri >= 0 && !(obj[0x640] & 1))
                GlDoneClear(ri);      // a reset pane (new round): forget old pieces
            if (g_glReplay) {
                static int n;
                if (LOGCAP(n, 40))
                    Xlog("glass: pane %016llx piece %u - the replayed peer break reached the pane's handler (not echoed)", (unsigned long long) id, piece);
            } else if (MatchSubstate() == 6) { // the level breaks ~12-25 panes itself while it loads - the same on every client, nothing to send
                // a pane without a registry slot is still sent (ri < 0 only if the table could not take it)
                if (ri >= 0 && GlDoneHas(ri, piece)) {
                    // this piece's break was already handled here (a repeat of the physics event): nothing new for the others
                } else {
                    if (ri >= 0) {
                        GlDoneSet(ri, piece);
                        g_gl_reg[ri].piece = piece;
                    }
                    g_gl_nsent++;
                    static int nlog;
                    uint8_t *chk = g_glFind ? g_glFind(id) : NULL;
                    if (LOGCAP(nlog, 300)) {    // was 12 per process: the log showed 12 sends however many were made
                        const char *nm = "?";
                        void **vt = *(void ***) obj;
                        if (vt && !IsBadReadPtr(vt + 5, 8) && vt[5]
                            && !IsBadReadPtr(vt[5], 16)) {
                            typedef const char *(*fn_name)(uint8_t *);
                            const char *s = ((fn_name) vt[5]) (obj);
                            if (s && !IsBadReadPtr(s, 4))
                                nm = s;
                        }
                        Xlog("glass: our break - pane %016llx '%.60s' %p piece %d args %d tags %u/%u/%u find(id) %s", (unsigned long long) id, nm, (void *) obj, (int) (int32_t) piece, (int) nargs, nargs > 0 ? *(uint16_t *) (ev + 0x28 + 0xc) : 0, nargs > 1 ? *(uint16_t *) (ev + 0x38 + 0xc) : 0, nargs > 2 ? *(uint16_t *) (ev + 0x48 + 0xc) : 0, chk == obj ? "= this pane (id ok)" : chk ? "= ANOTHER pane (twin / id mismatch)" : "NOT found (registry fallback)");
                    }
                    if (g_sendTramp) {
                        uint8_t m[1 + 8 + 4 + 1 + 48];
                        int n3 = nargs > 3 ? 3 : nargs < 0 ? 0 : nargs;
                        m[0] = 1;
                        memcpy(m + 1, &id, 8);
                        memcpy(m + 9, &piece, 4);
                        m[13] = (uint8_t) n3;
                        for (int i = 0; i < n3; i++)
                            memcpy(m + 14 + 16 * i, ev + 0x28 + 0x10 * i,
                                   16);
                        g_sendTramp(MSG_GLASS, m,
                                     (uint64_t) (14 + 16 * n3), 0xff);
                    }
                }
            }
        } else {
            GlRegGet(obj, 1); // learn the pane from any event it gets (the fallback lookup for the receiver side)
        }
        int rf = GlRegGet(obj, 0);
        if (rf >= 0) {
            unsigned fi = (unsigned) (((uintptr_t) obj >> 5) & 63);
            g_gl_fast[fi].id = g_gl_reg[rf].id;
            g_gl_fast[fi].vt = g_gl_reg[rf].vt;
            g_gl_fast[fi].obj = obj;
        }
    }
}

static void GlHook(uint8_t *obj, uint8_t *ev)
{
    LARGE_INTEGER q0, q1;
    QueryPerformanceCounter(&q0);
    unsigned fi = (unsigned) (((uintptr_t) obj >> 5) & 63);
    if (!
        (g_glOn && obj && ev && g_gl_fast[fi].obj == obj
         && *(uint64_t *) ev != GL_BREAK
         && *(uint64_t *) (obj + 0xc0) == g_gl_fast[fi].id
         && *(uint64_t *) obj == g_gl_fast[fi].vt))
        GlPre(obj, ev);
    QueryPerformanceCounter(&q1);
    InterlockedIncrement64(&g_pl_gl_n);
    InterlockedExchangeAdd64(&g_pl_gl_q, q1.QuadPart - q0.QuadPart);    // perf30 (t2r-ext.c)
    g_glOrig(obj, ev);
}

// replay a peer's break on the pane through the engine's event sender (target handle = {obj+0x80 slot ptr, obj+0x94 id}, as the coat reboot does)
static int GlReplay(uint8_t *obj, uint32_t piece, const uint8_t *args,
                     int nargs)
{
    if (!g_glSendev || IsBadReadPtr(obj + 0x80, 8)
        || IsBadReadPtr(obj + 0x94, 4))
        return 0;
    uint8_t *slot = *(uint8_t **) (obj + 0x80);
    if (!slot || IsBadReadPtr(slot, 16) || *(uint8_t **) (slot + 8) != obj)
        return 0;
    uint8_t out[0x100] __attribute__((aligned(32))) =
        { 0 }, snd[16] __attribute__((aligned(16))) =
        { 0 }, tgt[16] __attribute__((aligned(16))) = { 0 };
    uint8_t al[4][16] __attribute__((aligned(16))) = { { 0 }
    };
    memcpy(tgt, &slot, 8);
    memcpy(tgt + 8, obj + 0x94, 4);
    for (int i = 0; i < nargs && i < 3; i++) {
        const uint8_t *a = args + 16 * i;
        uint16_t tag = *(const uint16_t *) (a + 0xc);
        if (i == 2) {           // the piece index: the sender's tag when it is an int kind, else a plain int
            uint16_t t = GlTagInt(tag) ? tag : 2;
            uint64_t v = piece;
            memcpy(al[i], &v, 8);
            memcpy(al[i] + 0xc, &t, 2);
        } else if (GlTagScalar(tag))
            memcpy(al[i], a, 16);       // scalars travel; anything pointer-like becomes int 0
        else {
            uint16_t t = 2;
            memcpy(al[i] + 0xc, &t, 2);
        }
    }
    for (int i = nargs < 0 ? 0 : nargs; i < 3; i++) {
        uint16_t t = 2;
        memcpy(al[i] + 0xc, &t, 2);
    }                           // missing args: int 0 (the piece goes in arg 2 below)
    if (nargs < 3) {
        uint64_t v = piece;
        memcpy(al[2], &v, 8);
    }
    g_glReplay = 1;
#ifndef T2R_RELEASE
    if (g_glDevOwn)
        g_glReplay = 0;
#endif
    g_glSendev(out, snd, GL_BREAK, tgt, al[0], al[1], al[2], al[3]);
    g_glReplay = 0;
    return 1;
}

static struct {
    uint64_t until_ms, last_ms;
    uint64_t id;
    uint32_t piece;
    uint64_t ws0, frame0;
} g_glWatch;
static uint64_t GlWsMb(void)
{
    PROCESS_MEMORY_COUNTERS_EX pmc;
    memset(&pmc, 0, sizeof pmc);
    pmc.cb = sizeof pmc;
    return K32GetProcessMemoryInfo(GetCurrentProcess(),
                                   (PROCESS_MEMORY_COUNTERS *) & pmc,
                                   sizeof pmc) ? (uint64_t) (pmc.
                                                             WorkingSetSize
                                                             >> 20) : 0;
}

static void GlApply(uint8_t slot, uint64_t id, uint32_t piece,
                     const uint8_t *args, int nargs, const char *what)
{
    int via = 0;
    uint8_t *obj = GlFindPane(id, &via);
    if (!obj) {
        static int n;
        if (LOGCAP(n, 30))
            Xlog("glass: slot %u %s pane %016llx piece %d - NOT FOUND here (level object missing)", (unsigned) slot, what, (unsigned long long) id, (int) (int32_t) piece);
        return;
    }
    int ri = GlRegGet(obj, 1);
    if (ri >= 0 && !(obj[0x640] & 1))
        GlDoneClear(ri);
    if (ri >= 0 && GlDoneHas(ri, piece)) {
        static int n;
        if (LOGCAP(n, 60))
            Xlog("glass: slot %u %s pane %016llx piece %u - already broken here (skipped)", (unsigned) slot, what, (unsigned long long) id, piece);
        return;
    }
    if (ri < 0 && (obj[0x640] & 1)) {
        Xlog("glass: slot %u %s pane %016llx - already broken here (skipped)", (unsigned) slot, what, (unsigned long long) id);
        return;
    }
    g_glWatch.id = id;
    g_glWatch.piece = piece;
    g_glWatch.frame0 = g_frame;
    g_glWatch.ws0 = GlWsMb();
    g_glWatch.until_ms = GetTickCount64() + 4000;
    int ok = GlReplay(obj, piece, args, nargs);
    if (ri >= 0) {
        GlDoneSet(ri, piece);
        g_gl_reg[ri].piece = piece;
    }
    g_gl_nrecv++;
    Xlog("glass: slot %u %s pane %016llx piece %d - %s (%p%s) broken flag now %d", (unsigned) slot, what, (unsigned long long) id, (int) (int32_t) piece, ok ? "replayed through the engine event path" : "REPLAY FAILED", (void *) obj, via ? ", found via registry" : "", obj[0x640] & 1);
}

// a peer message (recv thread): queue only
static void GlassRecv(uint32_t from, const uint8_t *buf, uint64_t len)
{
    if (!g_glOn || !buf || len < 2)
        return;
    uint32_t h = g_gl_qh;
    if (h - g_gl_qt >= GL_Q) {
        g_gl_ndrop++;
        return;
    }
    if (buf[0] == 1 && len >= 14) {
        uint32_t i = h & (GL_Q - 1);
        int n = buf[13] > 3 ? 3 : buf[13];
        if (len < (uint64_t) (14 + 16 * n))
            return;
        {
            uint32_t pc;
            memcpy(&pc, buf + 9, 4);
            if (!(pc < 4096u || pc == 0xffffffffu)
                && PtDeny(PTK_glass_piece, from))
                return;
        }                       // peer msg trust: a piece index of a pane (0xffffffff = none), not an arbitrary number for the break handler
        g_gl_q[i].slot = (uint8_t) from;
        g_gl_q[i].op = 1;
        g_gl_q[i].nargs = (uint8_t) n;
        memcpy(&g_gl_q[i].id, buf + 1, 8);
        memcpy(&g_gl_q[i].piece, buf + 9, 4);
        memcpy(g_gl_q[i].args, buf + 14, (size_t) (16 * n));
        MemoryBarrier();
        g_gl_qh = h + 1;
    } else if (buf[0] == 2 && len >= 2) {
        int n = buf[1];
        for (int k = 0; k < n && len >= (uint64_t) (2 + 12 * (k + 1)); k++) {
            h = g_gl_qh;
            if (h - g_gl_qt >= GL_Q) {
                g_gl_ndrop++;
                return;
            }
            {
                uint32_t pc;
                memcpy(&pc, buf + 10 + 12 * k, 4);
                if (!(pc < 4096u || pc == 0xffffffffu)
                    && PtDeny(PTK_glass_piece, from))
                    continue;
            }
            uint32_t i = h & (GL_Q - 1);
            g_gl_q[i].slot = (uint8_t) from;
            g_gl_q[i].op = 2;
            g_gl_q[i].nargs = 0;
            memcpy(&g_gl_q[i].id, buf + 2 + 12 * k, 8);
            memcpy(&g_gl_q[i].piece, buf + 10 + 12 * k, 4);
            MemoryBarrier();
            g_gl_qh = h + 1;
        }
    }
}

// game thread, every frame: apply queued breaks; a joiner (slot turned active) gets the panes broken here
// A framerate report: ONE of 34 logged glass replays was followed by a stall (frame 79317: 1041 ms, 519625 page faults, working set
// 10.5 -> 13.3 GB with private memory flat = 2.8 GB of committed pool touched for the first time, first break of pane b72bc48135ef225a; 8 earlier breaks of
// other panes there and 33 elsewhere had none), so glass is not proven to cause it. For the next report: the 4 s after every replay are watched, and
// a tick gap >= 150 ms or a working-set jump >= 256 MB is logged as 'glass: aftermath' with the pane (replay-start frame, gap, ws before -> now).
static void GlWatchTick(void)
{
    uint64_t now = GetTickCount64();
    if (g_glWatch.until_ms && now < g_glWatch.until_ms) {
        uint64_t gap =
            g_glWatch.last_ms ? now - g_glWatch.last_ms : 0, ws =
            GlWsMb();
        if (gap >= 150 || ws >= g_glWatch.ws0 + 256) {
            static int n;
            if (LOGCAP(n, 20))
                Xlog("glass: aftermath of pane %016llx piece %u (replayed at frame %llu): tick gap %llu ms, working set %llu -> %llu MB", (unsigned long long) g_glWatch.id, g_glWatch.piece, (unsigned long long) g_glWatch.frame0, (unsigned long long) gap, (unsigned long long) g_glWatch.ws0, (unsigned long long) ws);
            g_glWatch.ws0 = ws;
        }
    }
    g_glWatch.last_ms = now;
}

static void GlassTick(void)
{
    if (!g_glOn)
        return;
    GlWatchTick();
    while (g_gl_qt != g_gl_qh) {
        uint32_t i = g_gl_qt & (GL_Q - 1);
        GlApply(g_gl_q[i].slot, g_gl_q[i].id, g_gl_q[i].piece,
                 g_gl_q[i].args, g_gl_q[i].nargs,
                 g_gl_q[i].op == 2 ? "list" : "break");
        MemoryBarrier();
        g_gl_qt++;
    }
    static uint8_t was[T2R_MAX_PEERS];
    static uint64_t next_stats;
    if (g_frame >= next_stats) {
        next_stats = g_frame + 3600;
        if (g_gl_nsent || g_gl_nrecv || g_gl_ndrop)
            Xlog("glass: stats - sent %u breaks, replayed %u from peers, %u dropped (queue full), registry %d panes, %u purges", g_gl_nsent, g_gl_nrecv, g_gl_ndrop, g_glNreg, g_gl_npurge);
    }
    if (g_frame % 30 != 0)
        return;
    typedef uint8_t *(*fn_peer)(uint32_t);
    for (uint32_t s = 0; s < T2R_MAX_PEERS; s++) {
        uint8_t *peer = ((fn_peer) T2rPeer) (s);
        uint8_t act = peer && peer[4];
        if (act && !was[s] && g_sendTramp) {
            // every broken piece of every live broken pane (not just the last piece of 20 panes as before), 20 entries a message
            uint8_t m[2 + 12 * 20];
            int n = 0, total = 0;
            for (int i = 0; i < g_glNreg; i++) {
                if (!GlLive(g_gl_reg[i].obj, g_gl_reg[i].id)
                    || *(uint64_t *) g_gl_reg[i].obj != g_gl_reg[i].vt
                    || !(g_gl_reg[i].obj[0x640] & 1))
                    continue;
                uint32_t pcs[257];
                int np = 0;
                for (uint32_t pc = 0; pc < 256; pc++)
                    if (GlDoneHas(i, pc))
                        pcs[np++] = pc;
                if (!np)
                    pcs[np++] = g_gl_reg[i].piece;
                for (int k = 0; k < np; k++) {
                    memcpy(m + 2 + 12 * n, &g_gl_reg[i].id, 8);
                    memcpy(m + 10 + 12 * n, &pcs[k], 4);
                    n++;
                    total++;
                    if (n == 20) {
                        m[0] = 2;
                        m[1] = 20;
                        g_sendTramp(MSG_GLASS, m, (uint64_t) (2 + 12 * n),
                                     s);
                        n = 0;
                    }
                }
            }
            if (n) {
                m[0] = 2;
                m[1] = (uint8_t) n;
                g_sendTramp(MSG_GLASS, m, (uint64_t) (2 + 12 * n), s);
            }
            n = total;
            Xlog("glass: slot %u is here - sent %d broken pane(s)", s, n);
        }
        was[s] = act;
    }
}

static void InstallGlassSync(void)
{
    FILE *f = ExeSideOpen("t2r-no-glass-sync.txt", "r");
    if (f) {
        fclose(f);
        Xlog("glass: sync OFF (t2r-no-glass-sync.txt)");
        return;
    }
    uint8_t *exe = (uint8_t *) GetModuleHandleW(NULL);
    // mov [rsp+18h],rbx; push rbp,rsi,rdi,r12,r13,r14,r15 (16 bytes, no rip-relative operand)
    static const uint8_t e_ev[16] =
        { 0x48, 0x89, 0x5c, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41,
0x55, 0x41, 0x56, 0x41, 0x57 };
    // SendEvent 0x1415734d0: mov [rsp+10h],rbx; mov [rsp+18h],rsi; push rdi; sub rsp,190h
    static const uint8_t e_send[16] =
        { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57,
0x48, 0x81, 0xec, 0x90, 0x01 };
    if (memcmp(exe + 0x15734d0, e_send, 16)) {
        HOOK_MISS(exe + 0x15734d0, 16,
                  "glass: sync NOT installed - exe 0x1415734d0 (event sender) differs in this build");
        return;
    }
    g_glSendev = (gl_fn_sendev) (exe + 0x15734d0);
    g_glFind = (gl_fn_find) (exe + 0x1b0a970);
    g_glOrig =
        (gl_fn_ev) VdfInline(0x14084a820, e_ev, 16, (void *) GlHook,
                              "BigBreakable / SimpleBreakable HandleEvent",
                              (void **) &g_glOrig);
    g_glOn = g_glOrig != NULL;
    Xlog("glass: sync %s (break event 0x2ccc09d462355d58 at exe 0x14084a820, replay through 0x1415734d0, pane lookup 0x141b0a970 by obj+0xc0, MSG_GLASS 0x%02X)", g_glOn ? "on" : "NOT installed", MSG_GLASS);
}
