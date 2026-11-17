// crashdiag.h - crash-proof capture for the three OPEN engine-side crash families.
//
// One fixed block (g_cdiag, magic "T2RCDG01") in the ext's writable data. Release-enabled, zero behaviour change: every writer only stores into
// the block (no engine call, no lock, no log line); the readers run in the crash handler (cd_report -> the ext log) and offline
// (tools/reports/dumps.py --rings finds the magic in a WER dump, whose type includes the module data segments; the game's own .mdmp is
// MiniDumpNormal = stacks only and does not hold it, so the crash handler ALSO prints everything into t2r-ext.log).
//
//   cm[]   one entry per ProcessMgr::CompactProcessMemory (once per frame): frame, time, duration, whether it moved the local Player,
//          the Player / item controller before and after, and the index of the native ring and of the body-event ring at entry. cm_active /
//          cm_* say a compaction is RUNNING right now (a fault with cm_active = 1 on that thread is a fault inside the relocator).
//   be[]   body events: only the natives that create / destroy / toggle engine objects or physics (kill-entity, spawn-object, collider toggles,
//          kill-process, ragdoll, explosion ...). One entry per call, with the frame and the first argument.
//   hk[]   every 30 frames in a match: the first 0x200 bytes of the Havok world (exe+0x92a2190) as 64 qwords. Counters / pointers that move
//          between the samples before a crash show up in the diff (cd_report prints the changed words).
//   pup[]  puppet lifecycle: a puppet process seen new / moved by the heap / gone per net slot, attach-table registration and rewrites
//          (the snapshot-table crash reads a puppet's attach table).
//
// Cost per event: one atomic increment + 4..8 plain stores (tens of ns); cm: 2 rdtsc + the stores, once per frame; hk: one 512-byte copy per
// 30 frames. Nothing here is in a hot loop.
#ifndef T2R_CRASHDIAG_H
#define T2R_CRASHDIAG_H
#include <stdint.h>
#include <string.h>
#include <x86intrin.h>

#define CD_CM_N 64
#define CD_BE_N 128
#define CD_HK_N 32
#define CD_HK_W 64
#define CD_PUP_N 64
#define CD_BE_SET 128           // open-addressing set of the body-event native hashes (power of two)

enum { CD_PUP_NEW = 1, CD_PUP_MOVED = 2, CD_PUP_GONE = 3, CD_PUP_REG =
        4, CD_PUP_PATCH = 5, CD_PUP_AOUA = 6 };

typedef struct {
    uint64_t frame;
    uint32_t ms, dur_k, dcr_in, be_in, tid, flags;
    uint64_t me0, me1, ct0, ct1;
} CdCmT;                      // 64 B; dur_k = tsc ticks / 1024
typedef struct {
    uint64_t frame;
    uint32_t ms, tid;
    uint64_t sid, a0;
    uint32_t dcr_i, pad;
} CdBeT;                      // 40 B
typedef struct {
    uint64_t frame;
    uint32_t ms, pad;
    uint64_t w[CD_HK_W];
} CdHkT;                      // 528 B
typedef struct {
    uint64_t frame;
    uint32_t ms, tid;
    uint16_t kind;
    int16_t slot;
    uint32_t pad;
    uint64_t a, b, c;
} CdPupT;                     // 48 B

typedef struct {
    char magic[8];              // "T2RCDG01"
    uint32_t ver, size;         // 1, sizeof
    uint64_t self, exe_base, ext_base;  // &g_cdiag (checks a relocated / copied block), module bases (pointers in the rings are absolute)
    uint64_t dcr_addr, xr_addr; // the ext's native ring (g_dcr) and log-line ring (g_xring): addresses, so the tool can read them too
    uint64_t dcr_i_addr, xr_i_addr;     // their int32 write indexes (g_dcr_i, g_xring_i)
    uint32_t dcr_n, dcr_stride, xr_n, xr_w;
    volatile uint64_t frame;    // last tick frame
    volatile uint32_t tick_tid, pad0;
    // compaction in progress
    volatile uint32_t cm_active, cm_tid;
    volatile uint64_t cm_frame, cm_tsc, cm_calls, cm_moves;
    volatile uint32_t cm_ms, cm_dcr, cm_be, pad1;
    volatile int32_t cm_i, be_i, hk_i, pup_i;
    uint64_t be_set[CD_BE_SET]; // fnv64 of the natives that go to be[]
    CdCmT cm[CD_CM_N];
    CdBeT be[CD_BE_N];
    CdHkT hk[CD_HK_N];
    CdPupT pup[CD_PUP_N];
} T2rCdiagT;

static T2rCdiagT g_cdiag = {.magic =
        { 'T', '2', 'R', 'C', 'D', 'G', '0', '1' },.ver = 1,.size =
    sizeof(T2rCdiagT) };

#define CD_NEXT(i) ((uint32_t)__atomic_add_fetch(&(i), 1, __ATOMIC_RELAXED))

static inline void CdBeAdd(uint64_t h)
{
    if (!h)
        return;
    for (uint32_t k = 0; k < CD_BE_SET; k++) {
        uint32_t j = ((uint32_t) (h >> 7) + k) & (CD_BE_SET - 1);
        if (g_cdiag.be_set[j] == h)
            return;
        if (!g_cdiag.be_set[j]) {
            g_cdiag.be_set[j] = h;
            return;
        }
    }
}

static inline int CdBeIs(uint64_t h)
{
    if (!h)
        return 0;
    for (uint32_t k = 0; k < 4; k++) {  // the set is built sparse (<= 40 of 128): a hit or an empty slot within 4 probes
        uint64_t v =
            g_cdiag.be_set[((uint32_t) (h >> 7) + k) & (CD_BE_SET - 1)];
        if (v == h)
            return 1;
        if (!v)
            return 0;
    }
    return 0;
}

// a native that changes engine objects / physics (called from dc_call for every native; cd_be_is is 1-2 loads for the others)
static inline void CdBeNote(uint64_t frame, uint32_t ms, uint32_t tid,
                              uint64_t sid, uint64_t a0, uint32_t dcr_i)
{
    uint32_t i = CD_NEXT(g_cdiag.be_i) & (CD_BE_N - 1);
    CdBeT *e = &g_cdiag.be[i];
    e->frame = frame;
    e->ms = ms;
    e->tid = tid;
    e->sid = sid;
    e->a0 = a0;
    e->dcr_i = dcr_i;
}

static inline void CdPupNote(uint64_t frame, uint32_t ms, uint32_t tid,
                               int kind, int slot, uint64_t a, uint64_t b,
                               uint64_t c)
{
    uint32_t i = CD_NEXT(g_cdiag.pup_i) & (CD_PUP_N - 1);
    CdPupT *e = &g_cdiag.pup[i];
    e->frame = frame;
    e->ms = ms;
    e->tid = tid;
    e->kind = (uint16_t) kind;
    e->slot = (int16_t) slot;
    e->a = a;
    e->b = b;
    e->c = c;
}

// ProcessMgr::CompactProcessMemory entry / exit (cm_hook)
static inline uint64_t CdCmBegin(uint64_t frame, uint32_t ms,
                                   uint32_t tid, uint32_t dcr_i)
{
    g_cdiag.cm_frame = frame;
    g_cdiag.cm_ms = ms;
    g_cdiag.cm_tid = tid;
    g_cdiag.cm_dcr = dcr_i;
    g_cdiag.cm_be = (uint32_t) g_cdiag.be_i;
    uint64_t t = __rdtsc();
    g_cdiag.cm_tsc = t;
    __atomic_store_n(&g_cdiag.cm_active, 1, __ATOMIC_RELEASE);
    return t;
}

static inline void CdCmEnd(uint64_t t0, int moved, uint64_t me0,
                             uint64_t me1, uint64_t ct0, uint64_t ct1)
{
    uint64_t t1 = __rdtsc();
    uint32_t i = CD_NEXT(g_cdiag.cm_i) & (CD_CM_N - 1);
    CdCmT *e = &g_cdiag.cm[i];
    e->frame = g_cdiag.cm_frame;
    e->ms = g_cdiag.cm_ms;
    e->dur_k = (uint32_t) ((t1 - t0) >> 10);
    e->dcr_in = g_cdiag.cm_dcr;
    e->be_in = g_cdiag.cm_be;
    e->tid = g_cdiag.cm_tid;
    e->flags = (uint32_t) moved;
    e->me0 = me0;
    e->me1 = me1;
    e->ct0 = ct0;
    e->ct1 = ct1;
    g_cdiag.cm_calls++;
    if (moved)
        g_cdiag.cm_moves++;
    __atomic_store_n(&g_cdiag.cm_active, 0, __ATOMIC_RELEASE);
}

// every N frames in a match: the Havok world header
static inline void CdHkPut(uint64_t frame, uint32_t ms,
                             const uint64_t *w)
{
    uint32_t i = CD_NEXT(g_cdiag.hk_i) & (CD_HK_N - 1);
    CdHkT *e = &g_cdiag.hk[i];
    e->frame = frame;
    e->ms = ms;
    memcpy(e->w, w, sizeof e->w);
}

// ---- reader (crash handler and the offline harness): `lg` is xlog-like, `nm` names a native hash, `now_ms` is (uint32)GetTickCount64() at the crash
typedef void (*cd_log_fn)(const char *fmt, ...);
typedef const char *(*cd_name_fn)(uint64_t sid);
static void CdReport(cd_log_fn lg, cd_name_fn nm, uint32_t now_ms)
{
    const T2rCdiagT *d = &g_cdiag;
#define CD_AGE(ms) ((long)(int32_t)(now_ms - (ms)))
    lg("  crash diag (cdiag v%u): frame %llu, tick tid %u", d->ver,
       (unsigned long long) d->frame, d->tick_tid);
    if (d->cm_active)
        lg("  crash diag: a heap COMPACTION IS RUNNING: started frame %llu, %ld ms ago, thread %u; since the previous compaction began %d natives / %d body events were issued", (unsigned long long) d->cm_frame, CD_AGE(d->cm_ms), d->cm_tid, (int) ((uint32_t) d->cm_dcr - (d->cm_i > 0 ? d->cm[(uint32_t) (d->cm_i) & (CD_CM_N - 1)].dcr_in : 0)), (int) ((uint32_t) d->cm_be - (d->cm_i > 0 ? d->cm[(uint32_t) (d->cm_i) & (CD_CM_N - 1)].be_in : 0)));
    else
        lg("  crash diag: no compaction running (%llu seen, %llu moved the local Player)", (unsigned long long) d->cm_calls, (unsigned long long) d->cm_moves);
    for (int k = 0; k < 8; k++) {
        int32_t n = d->cm_i - k;
        if (n <= 0)
            break;
        const CdCmT *e = &d->cm[(uint32_t) n & (CD_CM_N - 1)];
        if (!e->frame && !e->ms)
            break;
        lg("  crash diag: compaction -%d: frame %llu %ld ms ago, %u k-cycles, thread %u, natives %u..., body events %u...%s player %llx -> %llx ctrl %llx -> %llx", k, (unsigned long long) e->frame, CD_AGE(e->ms), e->dur_k, e->tid, e->dcr_in, e->be_in, e->flags ? " MOVED" : "", (unsigned long long) e->me0, (unsigned long long) e->me1, (unsigned long long) e->ct0, (unsigned long long) e->ct1);
    }
    {                           // body events newest first; the ones after the last completed compaction's entry are marked
        uint32_t since =
            d->cm_i >
            0 ? d->cm[(uint32_t) d->cm_i & (CD_CM_N - 1)].be_in : 0;
        lg("  crash diag: body events (newest first; '*' = issued after the last completed compaction began):");
        for (int k = 0; k < 24; k++) {
            int32_t n = d->be_i - k;
            if (n <= 0)
                break;
            const CdBeT *e = &d->be[(uint32_t) n & (CD_BE_N - 1)];
            if (!e->sid)
                break;
            lg("    %c-%d frame %llu %ld ms ago tid %u %s (%llx) a0 %llx",
               (uint32_t) n > since ? '*' : ' ', k,
               (unsigned long long) e->frame, CD_AGE(e->ms), e->tid,
               nm ? nm(e->sid) : "?", (unsigned long long) e->sid,
               (unsigned long long) e->a0);
        }
    }
    {
        static const char *const KN[] =
            { "?", "new", "moved by the heap", "gone",
"attach table registered", "attach entry patched",
"attach table rewritten by the engine updater" };
        lg("  crash diag: puppet events (newest first):");
        for (int k = 0; k < 24; k++) {
            int32_t n = d->pup_i - k;
            if (n <= 0)
                break;
            const CdPupT *e = &d->pup[(uint32_t) n & (CD_PUP_N - 1)];
            if (!e->kind)
                break;
            lg("    -%d frame %llu %ld ms ago tid %u slot %d %s: %llx %llx %llx", k, (unsigned long long) e->frame, CD_AGE(e->ms), e->tid, e->slot, e->kind < 7 ? KN[e->kind] : "?", (unsigned long long) e->a, (unsigned long long) e->b, (unsigned long long) e->c);
        }
    }
    {                           // Havok world header: newest sample in full for the first 12 words, then every word that differs between the newest and the oldest kept sample
        int32_t n = d->hk_i;
        if (n > 0) {
            const CdHkT *nw = &d->hk[(uint32_t) n & (CD_HK_N - 1)];
            int32_t old = n - (n >= CD_HK_N ? CD_HK_N - 1 : n - 1);
            const CdHkT *od =
                &d->hk[(uint32_t) (old < 1 ? 1 : old) & (CD_HK_N - 1)];
            lg("  crash diag: havok world samples: %d kept, newest frame %llu (%ld ms ago), oldest frame %llu (%ld ms ago)", n < CD_HK_N ? n : CD_HK_N, (unsigned long long) nw->frame, CD_AGE(nw->ms), (unsigned long long) od->frame, CD_AGE(od->ms));
            for (int j = 0; j < CD_HK_W; j++)
                if (nw->w[j] != od->w[j] || j < 6)
                    lg("    world+%03x: oldest %016llx newest %016llx%s",
                       j * 8, (unsigned long long) od->w[j],
                       (unsigned long long) nw->w[j],
                       nw->w[j] != od->w[j] ? "  CHANGED" : "");
        }
    }
#undef CD_AGE
}
#endif
