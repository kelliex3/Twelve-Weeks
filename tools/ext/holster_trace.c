// holster_trace.c - HOLSTER TRACER (analyzes all calls to fix the "Putting Weapon Back On Sling" bug
// once and for all). Included by t2r-ext.c after paired.c / ctxdown.c (uses g_ps, g_gx, the hfix gates, the puppet stow helpers).
//
// Every engine entry that starts a weapon put-away / holster / draw on ANY body (our player and every puppet) is recorded with: frame, body
// (own / puppet slot), the weapon, the request kind, a symbolized caller stack (exe+off / winmm+off / t2r-ext+off + nearest known
// function) and the paired-move state at that instant (psync role / kind / uid / ms into the move, the gun-execution role, our unarm /
// fist-takedown flags, the holster-fix scope, downed). Sources:
//   engine (trace-only stubs: the args are logged, the function runs unchanged)
//     ProcessWeaponBase::PutInHolster 0x141168060 (any body: player, NPC, puppet stow), PlayerItemController::PutWeaponInHolster 0x140bdd9d0,
//     ReparentWeapons 0x140afc740 (backpack), the NPC weapon-grip controller (puppets): holster / unholster 0x140680f90 (vfunc 0xb8),
//     grip set 0x14067e230 (vfunc 0xd8, npc-holster-weapon mode 4), the holster / unholster gestures 0x14067f230 / 0x14067f890, the quick /
//     slow holster gesture 0x1406800a0, the player weapon state's begin-holster 0x140d7f8d0
//   engine (our existing holster-fix hooks report into it, gated or not, for every body): HolsterItemFromAnimActionPhase1 / Phase2, the
//     Holster item-state request (any state machine; every state request of our own item controller), the PlayerHolster(Finish)WeaponAnimAction
//     begin + body, the character anim events reparent-weapon-to-holster / to-hand, the weapon attach calls 0x141cd2210 / 0x141cd67e0
//   overlay lookups (0x1414ebf50) of every put-away / holster / draw anim name (holster_sids.h) on any body's overlay state
//   natives (the DC record's handler wrapped): holster-player-weapon(-instantly / -physically), player-(instant-)draw-weapon(-by-name),
//     npc-holster-weapon, holster-npc-weapon-physically, player-disable-(melee-)unholster(ing), give-player-weapon, cancel-melee-action,
//     npc-give-weapon, npc-destroy-weapon - ours (a t2r-ext frame on the stack, "ours") and the game scripts'
//   winmm: the puppet weapon switch (give block 0x180035eeb -> pstow_switch), and per frame: every peer's synced held weapon (peer+0x91),
//     our own held weapon (desired/current id + the item controller's held handle), each puppet's current weapon process
// The events go to a ring (HTR_N); per frame the new ones are printed rate-limited ("htr~ #seq ..."), and around every paired-move start
// (psync: executions, finishers, ctxdown, shiv, revive) the whole ring window [-1.5 s, +1.5 s] is dumped in full ("htr: #seq ..." between
// "htr: MOVE ... dump begin/end" lines). tools/win/trace_holster.py turns a log into a per-move timeline.
// On: dev builds (t2r-no-trace-holster.txt = off), release builds only with t2r-trace-holster.txt next to the exe.

#if __has_include("holster_sids.h")
#include "holster_sids.h"
#else
#define HTR_HSID_N 1
static const uint64_t HTR_HSID[1] = { 0 };
#endif

#define HTR_N 8192
#define HTR_PRE 90              // frames before the move start in the dump
#define HTR_POST 90             // frames after
// (the event kinds HK_*: enum near the top of t2r-ext.c, the hooks there report with them)
static const char *const HK_NAME[HK_N] =
    { "?", "PutInHolster", "PutWeaponInHolster", "ReparentWeapons",
"npc-grip-holster/unholster",
    "npc-grip-set", "npc-holster-gesture", "npc-unholster-gesture",
        "npc-quick/slow-holster-gesture", "player-begin-holster",
    "HolsterFromAnimActionPhase1", "HolsterFromAnimActionPhase2",
        "Holster-state-request", "item-state-request(other)",
        "HolsterAnimAction-body",
    "HolsterAnimAction-begin", "anim-event", "weapon-attach", "native",
        "winmm-give", "overlay-lookup", "peer-weapon", "own-held",
        "puppet-weapon", "MOVE"
};

#define HF_GATED 1              // our hfix gate refused it (the engine call did not run)
#define HF_OURS 2               // a t2r-ext.dll frame is on its stack (our own call made it)
typedef struct {
    volatile LONG64 seq;        // written last; 0 = empty / being written
    uint64_t frame;
    double ms;
    uint16_t kind;
    int8_t body;
    uint8_t fl;
    uint64_t a[4];
    uint64_t stk[10];
    int8_t role, pkind, gxrole, scope;
    uint8_t unarm, cdh, down, upg;
    uint32_t uid;
    int32_t mvms;
} HtrEvT;
static HtrEvT g_htr[HTR_N];
static volatile LONG64 g_htrSeq;
static uint64_t g_htrLo[3], g_htr_hi[3];       // code ranges: exe, winmm, t2r-ext
// per-frame body cache (main thread writes, hooks read)
static uint8_t *volatile g_htr_me, *volatile g_htr_ctrl,
    *volatile g_htr_wac;
static uint8_t *volatile g_htrPup[T2R_MAX_PEERS],
    *volatile g_htr_pwc[T2R_MAX_PEERS];
static volatile uint64_t g_htrPuph[T2R_MAX_PEERS];
static volatile uint64_t g_htrOvs[T2R_MAX_PEERS + 1][3];       // [slot] puppets, [T2R_US] own: overlay state + the 2 weapon-holster-overlays holders

static int HtrHsid(uint64_t s)
{
    if (!s)
        return 0;
    int lo = 0, hi = HTR_HSID_N - 1;
    while (lo <= hi) {
        int m = (lo + hi) >> 1;
        if (HTR_HSID[m] == s)
            return 1;
        if (HTR_HSID[m] < s)
            lo = m + 1;
        else
            hi = m - 1;
    }
    return 0;
}

// -1 own, 0..7 puppet slot, -2 not one of our bodies / unknown. Also a pointer inside a body's object (embedded controllers).
static int HtrBodyObj(const void *p)
{
    uint64_t v = (uint64_t) p;
    if (!v)
        return -2;
    uint64_t me = (uint64_t) g_htr_me;
    if (me && v == me)
        return -1;
    for (int s = 0; s < T2R_MAX_PEERS; s++)
        if (g_htrPup[s] && v == (uint64_t) g_htrPup[s])
            return s;
    if (v == (uint64_t) g_htr_ctrl || v == (uint64_t) g_htr_wac)
        return -1;
    for (int s = 0; s < T2R_MAX_PEERS; s++)
        if (g_htr_pwc[s] && v == (uint64_t) g_htr_pwc[s])
            return s;
    if (me && v > me && v < me + 0x10000)
        return -1;
    for (int s = 0; s < T2R_MAX_PEERS; s++) {
        uint64_t q = (uint64_t) g_htrPup[s];
        if (q && v > q && v < q + 0x8000)
            return s;
    }
    return -2;
}

static int HtrBodyHandle(uint64_t h)
{
    if (!h)
        return -2;
    if (h == SID_PLAYER)
        return -1;
    for (int s = 0; s < T2R_MAX_PEERS; s++)
        if (g_htrPuph[s] == h)
            return s;
    return -2;
}

static uint64_t HtrWsid(uint8_t *w)
{
    return w
        && !IsBadReadPtr(w + 0x840, 8) ? *(uint64_t *) (w + 0x840) : 0;
}

static int HtrBodyWeapon(uint8_t *w)
{
    if (!w || IsBadReadPtr(w + 0x4b0, 8))
        return -2;
    return HtrBodyObj(SstowParent(w));
}

static int HtrCode(uint64_t a)
{                               // 1 = a return address in one of our three modules, preceded by a call
    int in = 0;
    for (int m = 0; m < 3; m++)
        if (a > g_htrLo[m] + 8 && a < g_htr_hi[m])
            in = 1;
    if (!in)
        return 0;
    const uint8_t *c = (const uint8_t *) a;
    if (c[-5] == 0xE8)
        return 1;               // call rel32
    if (c[-6] == 0xFF && ((c[-5] >> 3) & 7) == 2)
        return 1;               // call [rip / reg + disp32]
    if (c[-7] == 0xFF && ((c[-6] >> 3) & 7) == 2)
        return 1;               // call [reg + sib + disp32]
    if (c[-3] == 0xFF && ((c[-2] >> 3) & 7) == 2)
        return 1;               // call [reg + disp8]
    if (c[-4] == 0xFF && ((c[-3] >> 3) & 7) == 2)
        return 1;               // call [reg + sib + disp8]
    if (c[-2] == 0xFF && ((c[-1] >> 3) & 7) == 2)
        return 1;               // call reg / [reg]
    return 0;
}

static int HtrFromUs(const uint64_t *stk, int n)
{
    for (int i = 0; i < n; i++)
        if (stk[i] >= g_htrLo[2] && stk[i] < g_htr_hi[2])
            return 1;
    return 0;
}

// the paired-move state at this instant (read from any thread: plain globals)
static void HtrSnap(HtrEvT *e)
{
    e->role = (int8_t) g_ps.role;
    e->pkind = (int8_t) g_ps.kind;
    e->uid = g_ps.uid;
    e->mvms =
        g_ps.role ? (int32_t) (PsyncMs() - g_ps.t0 +
                               g_ps.chain_shift) : 0;
    e->gxrole = (int8_t) (g_gx.until
                          && g_t60 < g_gx.until ? g_gx.role : 0);
    e->scope = (int8_t) (g_hfix_on ? HfixScope() : -1);
    e->unarm = (uint8_t) g_unarmExec;
    e->cdh = (uint8_t) g_cdHandsOn;
    e->down = (uint8_t) g_downed;
    e->upg = (uint8_t) g_gx.upg;
}

// per-signature dedup at the source (a per-frame repeat - the item update's Holster request, an attach every frame - must not flood the ring)
static int HtrDup(uint64_t sig, int frames)
{
    static volatile uint64_t k[512], f[512];
    unsigned i = (unsigned) ((sig ^ (sig >> 17) ^ (sig >> 41)) & 511);
    uint64_t now = g_frame;
    if (k[i] == sig && now - f[i] < (uint64_t) frames)
        return 1;
    k[i] = sig;
    f[i] = now;
    return 0;
}

static HtrEvT *HtrBegin(int kind, int body)
{
    LONG64 s = InterlockedIncrement64(&g_htrSeq);
    HtrEvT *e = &g_htr[(uint64_t) s % HTR_N];
    e->seq = 0;
    MemoryBarrier();
    e->frame = g_frame;
    e->ms = PsyncMs();
    e->kind = (uint16_t) kind;
    e->body = (int8_t) body;
    e->fl = 0;
    e->a[0] = e->a[1] = e->a[2] = e->a[3] = 0;
    memset(e->stk, 0, sizeof e->stk);
    HtrSnap(e);
    e->stk[9] = (uint64_t) s;   // (the sequence number travels in stk[9] until commit)
    return e;
}

static void HtrCommit(HtrEvT *e)
{
    LONG64 s = (LONG64) e->stk[9];
    e->stk[9] = 0;
    MemoryBarrier();
    e->seq = s;
}

// a C hook's context: ra = the hook's own return address (__builtin_return_address(0) in a hook entered by a jmp from the patched
// prologue = the hooked function's caller; in a native thunk = the native's caller); the captured stack is listed from that frame on, so
// none of the tracer's / the hook's own frames is in it (and HF_OURS really means: our code made the call)
static __attribute__((noinline))
void htr_rec(int kind, int body, int fl, uint64_t a0, uint64_t a1,
             uint64_t a2, uint64_t a3, void *ra, int dedup)
{
    if (!g_htrOn)
        return;
    if (dedup
        && HtrDup(((uint64_t) kind << 56) ^ (uint64_t) (body & 0xff) << 48
                   ^ a0 * 31 ^ a1 *
                   1009 ^ (uint64_t) ra ^ (uint64_t) fl << 40, dedup))
        return;
    void *bt[24];
    USHORT n = RtlCaptureStackBackTrace(1, 24, bt, NULL);
    int from = -1;
    for (USHORT i = 0; i < n; i++)
        if (bt[i] == ra) {
            from = i;
            break;
        }
    HtrEvT *e = HtrBegin(kind, body);
    e->a[0] = a0;
    e->a[1] = a1;
    e->a[2] = a2;
    e->a[3] = a3;
    int m = 0;
    if (from < 0) {
        e->stk[m++] = (uint64_t) ra;
        from = n;
    }
    for (int i = from; i < n && m < 9; i++)
        e->stk[m++] = (uint64_t) bt[i];
    e->fl = (uint8_t) (fl | (HtrFromUs(e->stk, m) ? HF_OURS : 0));
    HtrCommit(e);
}

// ---- trace-only stubs on engine entries (args saved, htr_stub_cb, the relocated prologue, back into the function)
// 8 patched sites. The 9th entry (exe+d7f8d0, the player's BeginHolster, HK_PBEGIN) is NOT patched here any more: the holster fix
// (hfix_patch 'player begin-holster') owns those bytes and reports every call through htr_pbegin() below (hook audit: two patches on one site).
#define HTR_NSITE 8
static const struct {
    uint32_t rva;
    uint8_t len, jat, kind;
    uint8_t b[24];
} HTR_SITES[HTR_NSITE] = {
    { 0x1168060, 14, 3, HK_PUTIN,
     { 0x4d, 0x85, 0xc0, 0x0f, 0x84, 0, 0, 0, 0, 0x4c, 0x8b, 0xdc, 0x56,
      0x57} },
    { 0xbdd9d0, 20, 3, HK_PUTW,
     { 0x48, 0x85, 0xd2, 0x0f, 0x84, 0, 0, 0, 0, 0x55, 0x56, 0x57, 0x48,
      0x8d, 0xac, 0x24, 0x60, 0xff, 0xff, 0xff} },
    { 0xafc740, 17, 0xff, HK_REPARENT,
     { 0x40, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48,
      0x81, 0xec, 0xf8, 0x03, 0x00, 0x00} },
    { 0x680f90, 15, 0xff, HK_NHOLST,
     { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48,
      0x89, 0x74, 0x24, 0x20} },
    { 0x67e230, 15, 0xff, HK_NGRIP,
     { 0x48, 0x89, 0x5c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57,
      0x41, 0x56, 0x41, 0x57} },
    { 0x67f230, 15, 0xff, HK_NGEST_H,
     { 0x40, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x48, 0x81, 0xec,
      0x00, 0x01, 0x00, 0x00} },
    { 0x67f890, 15, 0xff, HK_NGEST_U,
     { 0x40, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x48, 0x81, 0xec,
      0x00, 0x01, 0x00, 0x00} },
    { 0x6800a0, 14, 0xff, HK_NQUICK,
     { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x56,
      0x57, 0x41, 0x56} },
};

static int g_htrSiteOk[HTR_NSITE];
// r: [0] r11 [1] r10 [2] rax [3] r9 [4] r8 [5] rdx [6] rcx [7] the return address, [8..] the caller's frame
static void HtrStubCb(uint64_t site, uint64_t *r)
{
    if (!g_htrOn || site >= HTR_NSITE)
        return;
    int kind = HTR_SITES[site].kind, body = -2;
    uint64_t a0 = 0, a1 = 0, a2 = 0, a3 = 0;
    uint64_t rcx = r[6], rdx = r[5], r8 = r[4], r9 = r[3];
    switch (kind) {
    case HK_PUTIN:             // (w, holster info, owner, float t)
        a0 = HtrWsid((uint8_t *) rcx);
        a1 = rcx;
        a2 = r8;
        a3 = rdx;
        body = HtrBodyObj((void *) r8);
        if (body == -2)
            body = HtrBodyWeapon((uint8_t *) rcx);
        break;
    case HK_PUTW:              // (PlayerItemController, w, slot, float t)
        a0 = HtrWsid((uint8_t *) rdx);
        a1 = rdx;
        a2 = rcx;
        a3 = r8;
        body =
            rcx ==
            (uint64_t) g_htr_ctrl ? -1 : HtrBodyWeapon((uint8_t *) rdx);
        break;
    default:                   // grip controller / gesture / state methods: this + 3 args
        a0 = rcx;
        a1 = rdx;
        a2 = r8;
        a3 = r9;
        body = HtrBodyObj((void *) rcx);
        break;
    }
    if (HtrDup
        (((uint64_t) kind << 56) ^ rcx ^ rdx * 7 ^ r8 * 13 ^ r[7],
         kind == HK_NGRIP || kind == HK_REPARENT ? 10 : 6))
        return;                 // a per-frame repeat
    HtrEvT *e = HtrBegin(kind, body);
    e->a[0] = a0;
    e->a[1] = a1;
    e->a[2] = a2;
    e->a[3] = a3;
    // callers: the exact return address, then return addresses found up the stack (call-site checked)
    int n = 0;
    e->stk[n++] = r[7];
    const uint64_t *sp = r + 8;
    uint64_t top = (uint64_t) ((NT_TIB *) NtCurrentTeb())->StackBase;   // (never read past this thread's stack)
    for (int i = 0; i < 700 && n < 9 && (uint64_t) (sp + i + 1) <= top;
         i++) {
        uint64_t v = sp[i];
        if (HtrCode(v) && v != e->stk[n - 1])
            e->stk[n++] = v;
    }
    e->fl = (uint8_t) (HtrFromUs(e->stk, n) ? HF_OURS : 0);
    HtrCommit(e);
}

// the holster fix's pbegin_hook (the only patch on exe+d7f8d0) reports here: this + 3 args, ra = the hook's own return address (the caller of BeginHolster)
static void HtrPbegin(uint64_t self, uint64_t a1, uint64_t a2,
                       uint64_t a3, void *ra)
{
    if (!g_htrOn)
        return;
    htr_rec(HK_PBEGIN, HtrBodyObj((void *) self), 0, self, a1, a2, a3,
            ra, 6);
}

static int HtrPatchSite(int i)
{
    uint8_t *exe = (uint8_t *) GetModuleHandleW(NULL), *f =
        exe + HTR_SITES[i].rva;
    int L = HTR_SITES[i].len, j =
        HTR_SITES[i].jat == 0xff ? -1 : HTR_SITES[i].jat;
    for (int k = 0; k < L; k++) {
        if (j >= 0 && k >= j + 2 && k < j + 6)
            continue;           // the jcc's rel32
        if (f[k] != HTR_SITES[i].b[k]) {
            HOOK_MISS(f, L, "htr: exe+%x differs at +%d - not traced (%s)",
                      HTR_SITES[i].rva, k, HK_NAME[HTR_SITES[i].kind]);
            return 0;
        }
    }
    uint8_t *jtarget =
        j >= 0 ? f + j + 6 + *(int32_t *) (f + j + 2) : NULL;
    uint8_t *t =
        VirtualAlloc(NULL, 256, MEM_COMMIT | MEM_RESERVE,
                     PAGE_EXECUTE_READWRITE);
    if (!t)
        return 0;
    int p = 0;
    static const uint8_t save[] = { 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x50, 0x41, 0x52, 0x41, 0x53,   // push rcx rdx r8 r9 rax r10 r11
        0x48, 0x83, 0xEC, 0x60, // sub rsp, 0x60
        0x0F, 0x11, 0x44, 0x24, 0x20, 0x0F, 0x11, 0x4C, 0x24, 0x30,     // movups [rsp+0x20], xmm0 / [rsp+0x30], xmm1
        0x0F, 0x11, 0x54, 0x24, 0x40, 0x0F, 0x11, 0x5C, 0x24, 0x50
    };                          // [rsp+0x40], xmm2 / [rsp+0x50], xmm3
    memcpy(t + p, save, sizeof save);
    p += sizeof save;
    t[p++] = 0x48;
    t[p++] = 0xB9;
    *(uint64_t *) (t + p) = (uint64_t) i;
    p += 8;                     // mov rcx, site
    t[p++] = 0x48;
    t[p++] = 0x8D;
    t[p++] = 0x54;
    t[p++] = 0x24;
    t[p++] = 0x60;              // lea rdx, [rsp+0x60]
    t[p++] = 0x48;
    t[p++] = 0xB8;
    *(uint64_t *) (t + p) = (uint64_t) (uintptr_t) HtrStubCb;
    p += 8;                     // mov rax, htr_stub_cb
    t[p++] = 0xFF;
    t[p++] = 0xD0;              // call rax
    static const uint8_t rest[] =
        { 0x0F, 0x10, 0x44, 0x24, 0x20, 0x0F, 0x10, 0x4C, 0x24, 0x30,
        0x0F, 0x10, 0x54, 0x24, 0x40, 0x0F, 0x10, 0x5C, 0x24, 0x50,
        0x48, 0x83, 0xC4, 0x60, // add rsp, 0x60
        0x41, 0x5B, 0x41, 0x5A, 0x58, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59
    };                          // pop r11 r10 rax r9 r8 rdx rcx
    memcpy(t + p, rest, sizeof rest);
    p += sizeof rest;
    if (j >= 0) {               // relocated prologue around the jcc rel32: inverse short jcc over an absolute jmp
        memcpy(t + p, f, (size_t) j);
        p += j;
        uint8_t cc = f[j + 1] & 0x0F;
        t[p++] = (uint8_t) (0x70 | (cc ^ 1));
        t[p++] = 14;
        t[p++] = 0xFF;
        t[p++] = 0x25;
        *(uint32_t *) (t + p) = 0;
        p += 4;
        *(uint64_t *) (t + p) = (uint64_t) jtarget;
        p += 8;
        memcpy(t + p, f + j + 6, (size_t) (L - j - 6));
        p += L - j - 6;
    } else {
        memcpy(t + p, f, (size_t) L);
        p += L;
    }
    t[p++] = 0xFF;
    t[p++] = 0x25;
    *(uint32_t *) (t + p) = 0;
    p += 4;
    *(uint64_t *) (t + p) = (uint64_t) (f + L);
    p += 8;
    FlushInstructionCache(GetCurrentProcess(), t, (SIZE_T) p);
    DWORD old;
    VirtualProtect(f, (SIZE_T) L, PAGE_EXECUTE_READWRITE, &old);
    f[0] = 0xFF;
    f[1] = 0x25;
    *(uint32_t *) (f + 2) = 0;
    *(uint64_t *) (f + 6) = (uint64_t) t;
    for (int k = 14; k < L; k++)
        f[k] = 0x90;
    VirtualProtect(f, (SIZE_T) L, old, &old);
    FlushInstructionCache(GetCurrentProcess(), f, (SIZE_T) L);
    g_htrSiteOk[i] = 1;
    return 1;
}

// the code checks of other features (spawn stow, puppet stow) compare these prologues: they read the original bytes through this
static int HtrExeEq(uint32_t rva, const uint8_t *want, size_t n)
{
    for (int i = 0; i < HTR_NSITE; i++)
        if (g_htrSiteOk[i] && HTR_SITES[i].rva == rva) {
            if (n > HTR_SITES[i].len)
                return 0;
            for (size_t k = 0; k < n; k++) {
                int j = HTR_SITES[i].jat == 0xff ? -1 : HTR_SITES[i].jat;
                if (j >= 0 && (int) k >= j + 2 && (int) k < j + 6)
                    continue;
                if (HTR_SITES[i].b[k] != want[k])
                    return 0;
            }
            return 1;
        }
    return !memcmp((uint8_t *) GetModuleHandleW(NULL) + rva, want, n);
}

// ---- natives (the DC record's handler at +8, as dc_call / the script VM call it)
#define HTR_NNAT 16
static const char *const HTR_NAT_NAME[HTR_NNAT] =
    { "holster-player-weapon", "holster-player-weapon-instantly",
"holster-player-weapon-physically",
    "player-instant-draw-weapon-by-name", "player-instant-draw-weapon",
        "player-draw-weapon-by-name", "npc-holster-weapon",
    "holster-npc-weapon-physically", "player-disable-unholstering",
        "player-disable-melee-unholster", "player-disable-auto-unholster",
    "give-player-weapon", "cancel-melee-action", "npc-give-weapon",
        "npc-destroy-weapon", "holster-pistol-for-crafting"
};

// (the engine reads the handler's RETURN VALUE as the result pointer: exe 0x16135 9a 'call rax; mov r8, [rax]' - the wrappers must pass the
// original's rax back; a void thunk returned garbage and crashed a client on a draw, 'EXCEPTION at tlou-ii.exe+161359c (info 0 ca)')
typedef uint64_t *(*fn_dccall_r)(uint64_t * ret, int argc, uint64_t * argv,
                                 uint64_t sec);
static fn_dccall g_htrNatOrig[HTR_NNAT];
static __attribute__((noinline))
uint64_t *htr_nat(int i, uint64_t *ret, int argc, uint64_t *argv,
                  uint64_t sec, void *ra)
{
    uint64_t *r = ((fn_dccall_r) g_htrNatOrig[i]) (ret, argc, argv, sec);
    if (!g_htrOn)
        return r;
    uint64_t a0 = argc > 0 && argv ? argv[0] : 0, a1 = argc > 1
        && argv ? argv[1] : 0;
    int body = -1;
    if (!strncmp(HTR_NAT_NAME[i], "npc-", 4) || i == 7)
        body = HtrBodyHandle(a0);
    if (i == 8 || i == 9 || i == 10) {
        if (HtrDup
            (0x4e41540000000000ULL ^ (uint64_t) i ^ ((uint64_t) argc << 8),
             30))
            return r;
    }                           // per-frame re-asserts
    htr_rec(HK_NAT, body, 0, (uint64_t) i, a0, a1, ret ? ret[0] : 0, ra,
            0);
    return r;
}

#define HTR_NAT_THUNK(I) static uint64_t *htr_nat##I(uint64_t *ret, int argc, uint64_t *argv, uint64_t sec) { return htr_nat(I, ret, argc, argv, sec, __builtin_return_address(0)); }
HTR_NAT_THUNK(0) HTR_NAT_THUNK(1) HTR_NAT_THUNK(2) HTR_NAT_THUNK(3)
HTR_NAT_THUNK(4) HTR_NAT_THUNK(5) HTR_NAT_THUNK(6) HTR_NAT_THUNK(7)
    HTR_NAT_THUNK(8) HTR_NAT_THUNK(9) HTR_NAT_THUNK(10) HTR_NAT_THUNK(11)
HTR_NAT_THUNK(12) HTR_NAT_THUNK(13) HTR_NAT_THUNK(14) HTR_NAT_THUNK(15)
static int HtrWrapNatives(int last_try)
{                               // 1 = every native wrapped (retried while the records are not all there)
    static const fn_dccall_r TH[HTR_NNAT] =
        { htr_nat0, htr_nat1, htr_nat2, htr_nat3, htr_nat4, htr_nat5,
htr_nat6, htr_nat7,
        htr_nat8, htr_nat9, htr_nat10, htr_nat11, htr_nat12, htr_nat13,
            htr_nat14, htr_nat15
    };
    int n = 0, newly = 0;
    char miss[400];
    int q = 0;
    miss[0] = 0;
    for (int i = 0; i < HTR_NNAT; i++) {
        if (g_htrNatOrig[i]) {
            n++;
            continue;
        }
        uint64_t sid =
            i == 12 ? 0x394888944e949d5fULL : Fnv64Rt(HTR_NAT_NAME[i]);
        uint8_t *rec =
            (uint8_t *) ((fn_lookup) T2R_SYM(kFnNativeLookup)) (sid);
        fn_dccall fn = rec ? *(fn_dccall *) (rec + 8) : NULL;
        if (!fn || !IsCode((void *) fn) || IsBadWritePtr(rec + 8, 8)) {
            q += snprintf(miss + q, SNLEFT(miss, q), " %s",
                          HTR_NAT_NAME[i]);
            continue;
        }
        g_htrNatOrig[i] = fn;
        *(fn_dccall_r volatile *) (rec + 8) = TH[i];
        n++;
        newly++;
    }
    if (newly && (n == HTR_NNAT || last_try))
        Xlog("htr: %d of %d natives wrapped%s%s", n, HTR_NNAT,
             miss[0] ? ", not found:" : "", miss);
    else if (last_try && miss[0])
        Xlog("htr: natives not found:%s", miss);
    return n == HTR_NNAT;
}

// ---- hooks of other features report here (declared near install_holster_fix)
static void HtrHfix(int kind, uint64_t ctrl_or_action, uint64_t x,
                     int gated, void *ra)
{
    if (!g_htrOn)
        return;
    int body = -2;
    uint64_t clip = 0, which = 0;
    if (kind == HK_PH1 || kind == HK_PH2)
        body = ctrl_or_action == (uint64_t) g_htr_ctrl ? -1 : -2;
    else if (kind == HK_ABODY || kind == HK_ABEGIN) {
        uint8_t *own =
            ctrl_or_action
            ? ((uint8_t *
                (*)(uint64_t)) ((uint8_t *) GetModuleHandleW(NULL) +
                                0x1C18D20)) (ctrl_or_action) : NULL;
        body = HtrBodyObj(own);
        if (kind == HK_ABEGIN) {
            which = x >> 63;
            uint64_t anim = x & ~(1ULL << 63);
            const uint8_t *a = (const uint8_t *) anim;
            const uint8_t *p = a
                && !IsBadReadPtr(a + 0x140,
                                 8) ? *(const uint8_t * const *) (a +
                                                                  0x140) :
                NULL;
            if (p && !IsBadReadPtr(p + 0x20, 8))
                clip = *(const uint64_t *) (p + 0x20);
        }
    }
    htr_rec(kind, body, gated ? HF_GATED : 0, ctrl_or_action,
            kind == HK_ABEGIN ? which : x, clip, 0, ra, 0);
}

static void HtrSreq(uint64_t sm, uint64_t desc_rva, int holster,
                     int gated, void *ra)
{
    if (!g_htrOn)
        return;
    int body = g_htr_ctrl && sm == (uint64_t) g_htr_ctrl + 8 ? -1 : -2;
    htr_rec(holster ? HK_SREQ : HK_SREQ_O, body, gated ? HF_GATED : 0, sm,
            desc_rva, 0, 0, ra, 15);
}

static void HtrEvt(uint64_t chr, uint64_t name, int gated, void *ra)
{
    if (!g_htrOn)
        return;
    htr_rec(HK_EVT, HtrBodyObj((void *) chr), gated ? HF_GATED : 0, chr,
            name, 0, 0, ra, 0);
}

static void HtrAtt(int k, uint64_t proc, uint64_t owner_handle_ptr,
                    uint64_t ap, int gated, void *ra)
{
    if (!g_htrOn)
        return;
    uint8_t *own = NULL;
    if (owner_handle_ptr && !IsBadReadPtr((void *) owner_handle_ptr, 16)) {
        own = *(uint8_t **) owner_handle_ptr;
        uint8_t *op = TacHandleProc((uint8_t *) owner_handle_ptr);
        if (op)
            own = op;
    }
    int body = HtrBodyObj(own);
    if (body == -2)
        return;                 // only our bodies (every prop / NPC weapon attach would flood the ring)
    htr_rec(HK_ATT, body, gated ? HF_GATED : 0, HtrWsid((uint8_t *) proc),
            ap, proc, (uint64_t) k, ra, 20);
}

static void HtrGive(uint32_t slot, uint64_t nsid, uint64_t cursid,
                     void *ra)
{
    if (!g_htrOn)
        return;
    htr_rec(HK_GIVE, slot < T2R_MAX_PEERS ? (int) slot : -2, 0, nsid,
            cursid, 0, 0, ra, 0);
}

static void HtrOvl(const uint64_t *st, const uint64_t *q,
                    const uint64_t *r, void *ra)
{
    if (!g_htrOn || !q)
        return;
    uint64_t to = 0;
    if (!HtrHsid(q[0])) {      // (most lookups: one binary search, no read)
        if (r && r[2])
            to = ((const uint64_t *) r[2])[1];  // the engine's entry {from, to} (scar_draw_answer reads it the same way)
        if (!HtrHsid(to))
            return;
    } else if (r && r[2])
        to = ((const uint64_t *) r[2])[1];
    int body = -2, hov = 0;
    for (int i = 0; i <= T2R_MAX_PEERS && body == -2; i++)
        for (int k = 0; k < 3; k++)
            if (g_htrOvs[i][k] && g_htrOvs[i][k] == (uint64_t) st) {
                body = i == T2R_US ? -1 : i;
                hov = k;
                break;
            }
    if (body == -2)
        return;                 // NPCs of the level / UI states: not ours
    if (HtrDup
        (0x4f564c0000000000ULL ^ (uint64_t) st ^ q[0] * 3 ^ to * 5, 20))
        return;
    htr_rec(HK_OVL, body, 0, q[0], to, (uint64_t) hov, q[1], ra, 0);
}

// ---- per frame (main thread): the body cache, the polled weapon states, printing, the move dumps
static struct {
    uint32_t uid;
    uint64_t start, dump_at;
    int role, kind, att, vic;
    uint64_t att_sid, vic_sid, move;
} g_htr_mv[4];
static int g_htrLines;
static const struct {
    uint32_t lo, hi;
    const char *name;
} HTR_EXE_SYM[] = {
    { 0x4f29b0, 0x4f29f0, "Npc::CurrentWeapon" }, { 0x4f2ce0, 0x4f2e3a,
                                                   "Npc::GiveWeapon" },
        { 0x63dc40, 0x63df5d, "native npc-holster-weapon" },
    { 0x67d820, 0x67d91f, "NpcWeaponGrip::vf_a0" }, { 0x67dbb0, 0x67dfff,
                                                     "NpcWeaponGrip::AttachForGrip(vf 0x218)"
                                                     }, { 0x67e230,
                                                         0x67e3eb,
                                                         "NpcWeaponGrip::SetGrip(vf 0xd8)"
                                                         },
    { 0x67ebd0, 0x67ecef, "NpcWeaponGrip::vf_08" }, { 0x67f230, 0x67f513,
                                                     "NpcWeaponGrip::HolsterGesture"
                                                     }, { 0x67f890,
                                                         0x67fb73,
                                                         "NpcWeaponGrip::UnholsterGesture"
                                                         },
    { 0x6800a0, 0x68042c, "NpcWeaponGrip::QuickSlowGesture" }, { 0x680f90,
                                                                0x6812ef,
                                                                "NpcWeaponGrip::HolsterUnholster(vf 0xb8)"
                                                                },
    { 0x8c09a0, 0x8c0ab7, "Npc holster -> PutInHolster" }, { 0x90a860,
                                                            0x90bb41,
                                                            "Character::OnAnimEvent"
                                                            },
    { 0xafc740, 0xafd772, "ReparentWeapons" }, { 0xaff010, 0xaff04e,
                                                "Backpack::Enter" },
        { 0xaff400, 0xaff774, "Backpack::Leave" },
    { 0xbd6304, 0xbd6705, "ItemController holster helper (bd6304)" },
        { 0xbd7590, 0xbd75c4, "PlayerItemController::RequestHolster" },
    { 0xbd8010, 0xbd81c2, "ItemController::Holster::OnEnter" }, { 0xbd8b80,
                                                                 0xbd92f0,
                                                                 "PlayerItemController::UpdateItem"
                                                                 },
    { 0xbda150, 0xbda1d1, "PlayerItemController::ClearHeld" }, { 0xbda2b0,
                                                                0xbda4fa,
                                                                "ItemController holster helper (bda2b0)"
                                                                },
    { 0xbdb7a0, 0xbdba35, "PlayerItemController::HolsterWeapon" },
        { 0xbdbc40, 0xbdbcc7, "PlayerItemController::HeldWeapon" },
    { 0xbdc1f0, 0xbdc23a, "PlayerItemController::CurrentWeapon" },
        { 0xbdc5a0, 0xbdc5fa, "ItemController holster helper (bdc5a0)" },
    { 0xbdd340, 0xbdd6a0, "HolsterItemFromAnimActionPhase1" }, { 0xbdd6a0,
                                                                0xbdd7b7,
                                                                "HolsterItemFromAnimActionPhase2"
                                                                },
    { 0xbdd9d0, 0xbddc1e, "PlayerItemController::PutWeaponInHolster" },
        { 0xbddec0, 0xbde026, "holster-item-physically" },
    { 0xbdea45, 0xbdead4,
     "PlayerItemController::HolsterNow (instant / melee)" }, { 0xbe036c,
                                                              0xbe0698,
                                                              "PlayerItemController::Update (auto holster)"
                                                              },
    { 0xc93640, 0xc93704, "native holster-player-weapon-instantly" },
        { 0xd79320, 0xd7937c, "HolsterFinishAction::OnEvent" },
    { 0xd79380, 0xd793c3, "HolsterFinishAction -> Phase2" }, { 0xd79570,
                                                              0xd7972d,
                                                              "PlayerWeaponAnimAction::SetWeaponLayer"
                                                              },
    { 0xd79960, 0xd79b10, "PlayerWeaponAnimAction::Begin" }, { 0xd7b220,
                                                              0xd7b3ea,
                                                              "PlayerHolsterFinishWeaponAnimAction::OnBegin"
                                                              },
    { 0xd7b3f0, 0xd7b600, "PlayerHolsterWeaponAnimAction::OnBegin" },
        { 0xd7bf90, 0xd7bffd, "HolsterFinishAction::OnEnd" },
    { 0xd7f8d0, 0xd7fca8, "PlayerWeaponState::BeginHolster" }, { 0xd9ac60,
                                                                0xd9acd6,
                                                                "anim action dispatch caller"
                                                                },
    { 0xee01cd, 0xee0709, "PlayerMelee (chunk ee01cd)" }, { 0xee1030,
                                                           0xeecfcc,
                                                           "PlayerMelee::Update"
                                                           },
    { 0x1161f20, 0x11621c4, "ProcessWeapon::HolsterInfo" }, { 0x1168060,
                                                             0x116830f,
                                                             "ProcessWeaponBase::PutInHolster"
                                                             },
    { 0x14eb820, 0x14eba99, "AnimOverlaySnapshot::Copy" }, { 0x14ebf50,
                                                            0x14ec096,
                                                            "AnimOverlay::FindEntry"
                                                            },
    { 0x1608b50, 0x1608c0c, "StateMachine::Request" }, { 0x1c18d20,
                                                        0x1c18d5f,
                                                        "AnimAction::Owner"
                                                        },
    { 0x1cd2140, 0x1cd2208, "WeaponAttachInstant" }, { 0x1cd2210,
                                                      0x1cd22f7,
                                                      "WeaponAttach" },
        { 0x1cd25a0, 0x1cd263a, "WeaponAttach(anim event)" },
    { 0x1cd4870, 0x1cd4909, "WeaponAttachPoint" }, { 0x1cd67e0, 0x1cd6925,
                                                    "WeaponAttach2" },
        { 0x1d57570, 0x1d575d5, "NpcHolsterWeapon" },
    { 0x1df8bf0, 0x1df8f41, "AnimActionDispatcher" },
};

static const struct {
    uint32_t off;
    const char *name;
} HTR_WINMM_SYM[] = {
    { 0x26ff0, "CreateAnimCmds hook" }, { 0x2c9e0,
                                         "puppet weapon give / hand attach"
                                         }, { 0x2ce70, "direct hit send" },
        { 0x35e00, "puppet weapon switch (give block)" },
    { 0x50390, "LocalPlayer" }, { 0x0a200, "HandleToProc" }, { 0x0a190,
                                                              "NativeLookup"
                                                              },
};

static struct {
    uint64_t a;
    const char *name;
} g_htr_ext_sym[64];
static int g_htrExtN;
static void HtrExtSym(void *fn, const char *name)
{
    if (g_htrExtN < 64) {
        g_htr_ext_sym[g_htrExtN].a = (uint64_t) fn;
        g_htr_ext_sym[g_htrExtN++].name = name;
    }
}

static void HtrSym(uint64_t a, char *out, size_t n)
{
    uint64_t exe = g_htrLo[0];
    if (a >= g_htrLo[0] && a < g_htr_hi[0]) {
        uint32_t r = (uint32_t) (a - exe);
        for (size_t i = 0; i < sizeof HTR_EXE_SYM / sizeof HTR_EXE_SYM[0];
             i++)
            if (r >= HTR_EXE_SYM[i].lo && r < HTR_EXE_SYM[i].hi) {
                snprintf(out, n, "exe+%x(%s)", r, HTR_EXE_SYM[i].name);
                return;
            }
        snprintf(out, n, "exe+%x", r);
        return;
    }
    if (a >= g_htrLo[1] && a < g_htr_hi[1]) {
        uint32_t r = (uint32_t) (a - g_htrLo[1]);
        const char *best = NULL;
        uint32_t bo = 0;
        for (size_t i = 0;
             i < sizeof HTR_WINMM_SYM / sizeof HTR_WINMM_SYM[0]; i++)
            if (r >= HTR_WINMM_SYM[i].off
                && r - HTR_WINMM_SYM[i].off < 0x600 && (!best
                                                        ||
                                                        HTR_WINMM_SYM[i].
                                                        off > bo)) {
                best = HTR_WINMM_SYM[i].name;
                bo = HTR_WINMM_SYM[i].off;
            }
        if (best)
            snprintf(out, n, "winmm+%x(~%s)", r, best);
        else
            snprintf(out, n, "winmm+%x", r);
        return;
    }
    if (a >= g_htrLo[2] && a < g_htr_hi[2]) {
        const char *best = NULL;
        uint64_t bo = 0;
        for (int i = 0; i < g_htrExtN; i++)
            if (g_htr_ext_sym[i].a <= a && a - g_htr_ext_sym[i].a < 0x6000
                && g_htr_ext_sym[i].a > bo) {
                bo = g_htr_ext_sym[i].a;
                best = g_htr_ext_sym[i].name;
            }
        if (best)
            snprintf(out, n, "ext+%llx(~%s+%llx)",
                     (unsigned long long) (a - g_htrLo[2]), best,
                     (unsigned long long) (a - bo));
        else
            snprintf(out, n, "ext+%llx",
                     (unsigned long long) (a - g_htrLo[2]));
        return;
    }
    ModOff(a, out, n);
}

static const char *HtrBodyName(int b, char *buf)
{
    if (b == -1)
        return "own";
    if (b >= 0 && b < T2R_MAX_PEERS) {
        snprintf(buf, 8, "pup%d", b);
        return buf;
    }
    return "?";
}

static void HtrPrint(const HtrEvT *e, const char *tag, const char *rel)
{
    char line[1400], b[8], w[96];
    int k =
        snprintf(line, sizeof line, "%s #%lld f%llu%s %s %s", tag,
                 (long long) e->seq, (unsigned long long) e->frame,
                 rel ? rel : "",
                 e->kind < HK_N ? HK_NAME[e->kind] : "?",
                 HtrBodyName(e->body, b));
    if (e->fl & HF_GATED)
        k += snprintf(line + k, SNLEFT(line, k), " GATED");
    if (e->fl & HF_OURS)
        k += snprintf(line + k, SNLEFT(line, k), " ours");
    switch (e->kind) {
    case HK_PUTIN:
        k += snprintf(line + k, SNLEFT(line, k),
                      " w=%016llx proc=%llx owner=%llx info=%llx",
                      (unsigned long long) e->a[0],
                      (unsigned long long) e->a[1],
                      (unsigned long long) e->a[2],
                      (unsigned long long) e->a[3]);
        break;
    case HK_PUTW:
        k += snprintf(line + k, SNLEFT(line, k),
                      " w=%016llx proc=%llx ctrl=%llx slot=%lld",
                      (unsigned long long) e->a[0],
                      (unsigned long long) e->a[1],
                      (unsigned long long) e->a[2], (long long) e->a[3]);
        break;
    case HK_SREQ:
    case HK_SREQ_O:
        k += snprintf(line + k, SNLEFT(line, k), " sm=%llx state=exe+%llx",
                      (unsigned long long) e->a[0],
                      (unsigned long long) e->a[1]);
        break;
    case HK_ABEGIN:
        k += snprintf(line + k, SNLEFT(line, k),
                      " action=%llx %s clip=%016llx",
                      (unsigned long long) e->a[0],
                      e->
                      a[1] ?
                      "put-away-layer(PlayerHolsterWeaponAnimAction)" :
                      "holster(PlayerHolsterFinishWeaponAnimAction)",
                      (unsigned long long) e->a[2]);
        break;
    case HK_EVT:
        k += snprintf(line + k, SNLEFT(line, k), " chr=%llx event=%s",
                      (unsigned long long) e->a[0],
                      e->a[1] ==
                      0xa8721093c8919ab5ULL ? "reparent-weapon-to-holster"
                      : "reparent-weapon-to-other-hand");
        break;
    case HK_ATT:
        k += snprintf(line + k, SNLEFT(line, k),
                      " w=%016llx ap=%016llx(%s) proc=%llx via=%lld",
                      (unsigned long long) e->a[0],
                      (unsigned long long) e->a[1], BpApName(e->a[1]),
                      (unsigned long long) e->a[2], (long long) e->a[3]);
        break;
    case HK_NAT:
        k += snprintf(line + k, SNLEFT(line, k), " %s(%llx, %llx) -> %llx",
                      e->a[0] < HTR_NNAT ? HTR_NAT_NAME[e->a[0]] : "?",
                      (unsigned long long) e->a[1],
                      (unsigned long long) e->a[2],
                      (unsigned long long) e->a[3]);
        break;
    case HK_GIVE:
        k += snprintf(line + k, SNLEFT(line, k),
                      " to=%016llx from=%016llx",
                      (unsigned long long) e->a[0],
                      (unsigned long long) e->a[1]);
        break;
    case HK_OVL:
        k += snprintf(line + k, SNLEFT(line, k),
                      " q=%016llx -> %016llx%s layer=%lld",
                      (unsigned long long) e->a[0],
                      (unsigned long long) e->a[1],
                      e->a[2] ? " (weapon-holster-overlays)" : "",
                      (long long) (int32_t) (e->a[3] & 0xffffffff));
        break;
    case HK_PEERW:
    case HK_HELD:
    case HK_PUPW:
        k += snprintf(line + k, SNLEFT(line, k),
                      " %016llx -> %016llx (aux %llx -> %llx)",
                      (unsigned long long) e->a[0],
                      (unsigned long long) e->a[1],
                      (unsigned long long) e->a[2],
                      (unsigned long long) e->a[3]);
        break;
    case HK_MOVE:
        k += snprintf(line + k, SNLEFT(line, k), " uid=%08llx",
                      (unsigned long long) e->a[0]);
        break;
    default:
        k += snprintf(line + k, SNLEFT(line, k), " a=%llx %llx %llx %llx",
                      (unsigned long long) e->a[0],
                      (unsigned long long) e->a[1],
                      (unsigned long long) e->a[2],
                      (unsigned long long) e->a[3]);
        break;
    }
    k += snprintf(line + k, SNLEFT(line, k),
                  " | ps r%d k%d u%08x %+dms gx%d unarm%d cdh%d sc%d dn%d upg%d |",
                  e->role, e->pkind, e->uid, e->mvms, e->gxrole, e->unarm,
                  e->cdh, e->scope, e->down, e->upg);
    for (int i = 0; i < 9 && e->stk[i] && k < 1300; i++) {
        HtrSym(e->stk[i], w, sizeof w);
        k += snprintf(line + k, SNLEFT(line, k), " %s", w);
    }
    Xlog("%s", line);
}

static void HtrPollEv(int kind, int body, uint64_t a0, uint64_t a1,
                        uint64_t a2, uint64_t a3)
{
    HtrEvT *e = HtrBegin(kind, body);
    e->a[0] = a0;
    e->a[1] = a1;
    e->a[2] = a2;
    e->a[3] = a3;
    HtrCommit(e);
}

static void HtrCacheTick(void)
{
    typedef uint8_t *(*fn_me)(void);
    typedef uint8_t *(*fn_peer)(uint32_t);
    uint8_t *me = ((fn_me) T2R_SYM(kFnLocalPlayer)) ();
    g_htr_me = me;
    g_htr_ctrl = me
        && !IsBadReadPtr(me + 0x2F60,
                         8) ? *(uint8_t **) (me + 0x2F60) : NULL;
    g_htrCtrl0 = g_htr_ctrl;
    g_htr_wac = me
        && !IsBadReadPtr(me + 0x2fc8,
                         8) ? *(uint8_t **) (me + 0x2fc8) : NULL;
    uint8_t *exe = (uint8_t *) GetModuleHandleW(NULL);
    for (int b = 0; b <= T2R_MAX_PEERS; b++) {
        uint8_t *chr = NULL;
        if (b == T2R_US)
            chr = me;
        else {
            uint8_t *peer = ((fn_peer) T2rPeer) ((uint32_t) b);
            uint64_t h = peer && peer[4] ? *(uint64_t *) (peer + 0x60) : 0;
            chr =
                h ? ((uint8_t * (*)(uint64_t))
                     T2R_SYM(kFnEntityFromHandle)) (h) : NULL;
            if (chr && chr == me)
                chr = NULL;
            g_htrPuph[b] = chr ? h : 0;
            uint8_t *wc = chr == g_htrPup[b] && (g_frame % 30) ? g_htr_pwc[b] : NULL;  // (the same body: re-read twice a second)
            if (!wc && chr && PstNpc(h) && !IsBadReadPtr(chr + 0x1bc8, 8)) {   // the NPC weapon-grip controller: (npc+0x1bc8)->vfunc 0x48 (as npc-holster-weapon gets it)
                uint8_t *comp = *(uint8_t **) (chr + 0x1bc8);
                void **vt = comp
                    && !IsBadReadPtr(comp, 8) ? *(void ***) comp : NULL;
                if (vt && !IsBadReadPtr(vt + 9, 8) && vt[9]
                    && (uint64_t) vt[9] > (uint64_t) exe
                    && (uint64_t) vt[9] < (uint64_t) exe + 0x2a8c000)
                    wc = ((uint8_t * (*)(void *)) vt[9]) (comp);
            }
            g_htr_pwc[b] = wc;
            g_htrPup[b] = chr;
        }
        uint64_t ov[3] = { 0, 0, 0 };
        if (chr && !IsBadReadPtr(chr + 0x1b8, 8)) {
            const uint8_t *anim = *(const uint8_t * const *) (chr + 0x1b8);
            const uint64_t *holder = anim
                && !IsBadReadPtr(anim + 0x60,
                                 8) ? *(const uint64_t * const *) (anim +
                                                                   0x60) :
                NULL;
            if (holder && !IsBadReadPtr(holder, 8))
                ov[0] = holder[0];
            if (ov[0] && !IsBadReadPtr(chr + 0x2fc8, 8)) {
                const uint8_t *wac =
                    *(const uint8_t * const *) (chr + 0x2fc8);
                if (wac && !IsBadReadPtr(wac, 0x330)
                    && *(const uint64_t *) wac ==
                    (uint64_t) (exe + 0x2cf9f90))
                    for (int k = 0; k < 2; k++) {
                        const uint64_t *hh =
                            *(const uint64_t * const *) (wac + 0x320 +
                                                         8 * k);
                        if (hh && !IsBadReadPtr(hh, 8))
                            ov[1 + k] = hh[0];
                    }
            }
        }
        for (int k = 0; k < 3; k++)
            g_htrOvs[b][k] = ov[k];
    }
}

static void HtrPollTick(void)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    static uint64_t peerw[T2R_MAX_PEERS], pupw[T2R_MAX_PEERS], held_id,
        held_h;
    uint32_t self = PsyncSelf();
    for (int s = 0; s < T2R_MAX_PEERS; s++) {
        uint8_t *peer = ((fn_peer) T2rPeer) ((uint32_t) s);
        uint64_t w = peer && peer[4] ? *(uint64_t *) (peer + 0x91) : 0; // the synced held weapon (ours: what we send)
        if (w != peerw[s]) {
            if (peer && peer[4])
                HtrPollEv(HK_PEERW, (uint32_t) s == self ? -1 : s,
                            peerw[s], w, (uint64_t) s, 0);
            peerw[s] = w;
        }
        uint8_t *npc = g_htrPuph[s] ? PstNpc(g_htrPuph[s]) : NULL;
        uint8_t *cur = npc ? PstCurrent(npc) : NULL;
        uint64_t cs = HtrWsid(cur);
        if (cs != pupw[s]) {
            if (npc)
                HtrPollEv(HK_PUPW, s, pupw[s], cs, 0, (uint64_t) cur);
            pupw[s] = cs;
        }
    }
    uint64_t id = CurrentWeapon();
    uint64_t h = g_htr_ctrl
        && !IsBadReadPtr(g_htr_ctrl + 0x70,
                         8) ? *(uint64_t *) (g_htr_ctrl + 0x70) : 0;
    if (id == ~0ULL)
        id = 0;
    if (id != held_id || (!h) != (!held_h)) {
        HtrPollEv(HK_HELD, -1, held_id, id, held_h, h);
        held_id = id;
        held_h = h;
    }
}

static int HtrInWindow(uint64_t f)
{
    for (int i = 0; i < 4; i++)
        if (g_htr_mv[i].dump_at && f + HTR_PRE >= g_htr_mv[i].start
            && f <= g_htr_mv[i].start + HTR_POST)
            return 1;
    return 0;
}

static void HtrDump(int i)
{
    char b1[8], b2[8];
    int ab = g_htr_mv[i].role == 1 ? -1 : g_htr_mv[i].att, vb =
        g_htr_mv[i].role == 2 ? -1 : g_htr_mv[i].vic;
    LONG64 cur = g_htrSeq;
    int n = 0, lost = 0;
    Xlog("htr: MOVE u%08x dump begin (role %d kind %d, attacker body %s, victim body %s, clips %s / %s, move %016llx, start f%llu, window f%llu..f%llu)", g_htr_mv[i].uid, g_htr_mv[i].role, g_htr_mv[i].kind, HtrBodyName(ab, b1), HtrBodyName(vb, b2), PsyncName(g_htr_mv[i].att_sid), PsyncName(g_htr_mv[i].vic_sid), (unsigned long long) g_htr_mv[i].move, (unsigned long long) g_htr_mv[i].start, (unsigned long long) (g_htr_mv[i].start - HTR_PRE), (unsigned long long) (g_htr_mv[i].start + HTR_POST));
    for (LONG64 s = cur - HTR_N + 1; s <= cur; s++) {
        if (s <= 0)
            continue;
        const HtrEvT *e = &g_htr[(uint64_t) s % HTR_N];
        if (e->seq != s) {
            if (e->seq < s)
                lost++;
            continue;
        }
        if (e->frame + HTR_PRE < g_htr_mv[i].start
            || e->frame > g_htr_mv[i].start + HTR_POST)
            continue;
        char rel[32];
        snprintf(rel, sizeof rel, " [%+lldf]",
                 (long long) e->frame - (long long) g_htr_mv[i].start);
        HtrPrint(e, "htr:", rel);
        n++;
    }
    Xlog("htr: MOVE u%08x dump end - %d events%s", g_htr_mv[i].uid, n,
         lost ? " (some ring slots overwritten / being written)" : "");
    g_htr_mv[i].dump_at = 0;
}

static void HtrTick(void)
{
    static LONG64 printed;
    static uint32_t last_uid;
    static int nat_done, nat_tries;
    if (!g_htrOn)
        return;
    if (!g_htrLo[1] && g_winmm) {
        uint8_t *wm = (uint8_t *) g_winmm;
        IMAGE_NT_HEADERS *wn =
            (IMAGE_NT_HEADERS *) (wm +
                                  ((IMAGE_DOS_HEADER *) wm)->e_lfanew);
        g_htrLo[1] = (uint64_t) wm;
        g_htr_hi[1] = (uint64_t) wm + wn->OptionalHeader.SizeOfImage;
    }
    if (!nat_done && g_winmm && !(g_frame % 60)) {
        nat_tries++;
        nat_done = HtrWrapNatives(nat_tries >= 20) || nat_tries >= 20;
    }                           // (records registered late: retried 20 s)
    HtrCacheTick();
    HtrPollTick();
    if (g_ps.role && g_ps.uid != last_uid) {    // a paired move starts on this client
        last_uid = g_ps.uid;
        int k = 0;
        for (int i = 0; i < 4; i++)
            if (!g_htr_mv[i].dump_at) {
                k = i;
                break;
            }
        if (g_htr_mv[k].dump_at)
            HtrDump(k);
        g_htr_mv[k].uid = g_ps.uid;
        g_htr_mv[k].start = g_frame;
        g_htr_mv[k].dump_at = g_frame + HTR_POST + 1;
        g_htr_mv[k].role = g_ps.role;
        g_htr_mv[k].kind = g_ps.kind;
        g_htr_mv[k].att = g_ps.att;
        g_htr_mv[k].vic = g_ps.vic;
        g_htr_mv[k].att_sid = g_ps.att_sid;
        g_htr_mv[k].vic_sid = g_ps.vic_sid;
        g_htr_mv[k].move = g_ps.move;
        HtrPollEv(HK_MOVE, g_ps.role == 1 ? -1 : g_ps.att, g_ps.uid, 0,
                    0, 0);
        Xlog("htr: MOVE u%08x start f%llu role %d kind %d attacker slot %d victim slot %d (%s / %s) - full dump at f%llu", g_ps.uid, (unsigned long long) g_frame, g_ps.role, g_ps.kind, g_ps.att, g_ps.vic, PsyncName(g_ps.att_sid), PsyncName(g_ps.vic_sid), (unsigned long long) g_htr_mv[k].dump_at);
    }
    int old = g_xlogNolimit;
    g_xlogNolimit = 1;         // (release: the dump is not cut by the per-site log limit)
    LONG64 cur = g_htrSeq;
    if (cur - printed > HTR_N)
        printed = cur - HTR_N;
    static struct {
        uint64_t sig, f;
    } rl[512];
    for (LONG64 s = printed + 1; s <= cur; s++) {
        const HtrEvT *e = &g_htr[(uint64_t) s % HTR_N];
        if (e->seq != s) {
            if (e->seq < s)
                break;
            continue;
        }                       // (still being written: printed next frame)
        printed = s;
        if (HtrInWindow(e->frame) || g_htrLines > 4000)
            continue;           // the dump prints it in full
        uint64_t sig =
            ((uint64_t) e->
             kind << 56) ^ ((uint64_t) (e->body & 0xff) << 48) ^ e->
            stk[0] ^ e->a[0] * 31;
        unsigned ix = (unsigned) ((sig ^ (sig >> 23)) & 511);
        if (rl[ix].sig == sig && g_frame - rl[ix].f < 60)
            continue;
        rl[ix].sig = sig;
        rl[ix].f = g_frame;
        HtrPrint(e, "htr~", NULL);
        g_htrLines++;
    }
    for (int i = 0; i < 4; i++)
        if (g_htr_mv[i].dump_at && g_frame >= g_htr_mv[i].dump_at)
            HtrDump(i);
    g_xlogNolimit = old;
}

static void HtrInstall(void)
{
    static int done;
    if (done)
        return;
    done = 1;
#ifdef T2R_RELEASE
    {
        FILE *f = ExeSideOpen("t2r-trace-holster.txt", "r");
        if (!f) {
            Xlog("htr: holster tracer off (opt-in: t2r-trace-holster.txt next to the exe)");
            return;
        }
        fclose(f);
    }
#else
    {
        FILE *f = ExeSideOpen("t2r-no-trace-holster.txt", "r");
        if (f) {
            fclose(f);
            Xlog("htr: holster tracer off (t2r-no-trace-holster.txt)");
            return;
        }
    }
#endif
    uint8_t *exe = (uint8_t *) GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS *nt =
        (IMAGE_NT_HEADERS *) (exe + ((IMAGE_DOS_HEADER *) exe)->e_lfanew);
    g_htrLo[0] = (uint64_t) exe;
    g_htr_hi[0] = (uint64_t) exe + nt->OptionalHeader.SizeOfImage;
    if (g_winmm) {
        uint8_t *wm = (uint8_t *) g_winmm;
        IMAGE_NT_HEADERS *wn =
            (IMAGE_NT_HEADERS *) (wm +
                                  ((IMAGE_DOS_HEADER *) wm)->e_lfanew);
        g_htrLo[1] = (uint64_t) wm;
        g_htr_hi[1] = (uint64_t) wm + wn->OptionalHeader.SizeOfImage;
    }
    IMAGE_NT_HEADERS *on =
        (IMAGE_NT_HEADERS *) ((uint8_t *) & __ImageBase +
                              __ImageBase.e_lfanew);
    g_htrLo[2] = (uint64_t) & __ImageBase;
    g_htr_hi[2] = g_htrLo[2] + on->OptionalHeader.SizeOfImage;
    HtrExtSym((void *) DcCall, "dc_call");
    HtrExtSym((void *) CdHandsBegin, "cd_hands_begin");
    HtrExtSym((void *) CdHandsTick, "cd_hands_tick");
    HtrExtSym((void *) CdSwingStart, "cd_swing_start");
    HtrExtSym((void *) CdUpgStart, "cd_upg_start");
    HtrExtSym((void *) Meleeexec2013, "meleeexec_2013");
    HtrExtSym((void *) Upg13Start, "upg13_start");
    HtrExtSym((void *) VicUnarmTick, "vic_unarm_tick");
    HtrExtSym((void *) FemUnarmEmptyHands, "fem_unarm_empty_hands");
    HtrExtSym((void *) FemUnarmTick, "fem_unarm_tick");
    HtrExtSym((void *) ExecEndTick, "exec_end_tick");
    HtrExtSym((void *) ShivExec, "shiv_exec");
    HtrExtSym((void *) PsyncStart, "psync_start");
    HtrExtSym((void *) PsyncTick, "psync_tick");
    HtrExtSym((void *) PstowSwitch, "pstow_switch");
    HtrExtSym((void *) PstowTick, "pstow_tick");
    HtrExtSym((void *) PstHolster, "pst_holster");
    HtrExtSym((void *) SpawnStowTick, "spawn_stow_tick");
    HtrExtSym((void *) FeatureDowned, "feature_downed");
    HtrExtSym((void *) FemaleKnifeItem, "female_knife_item");
    HtrExtSym((void *) HfixHook, "hfix_hook");
    HtrExtSym((void *) SreqHook, "sreq_hook");
    HtrExtSym((void *) AfixHook, "afix_hook");
    HtrExtSym((void *) BfixHook, "bfix_hook");
    HtrExtSym((void *) EvfixHook, "evfix_hook");
    HtrExtSym((void *) P2fixHook, "p2fix_hook");
    HtrExtSym((void *) WattHook0, "watt_hook0");
    HtrExtSym((void *) WattHook1, "watt_hook1");
    HtrExtSym((void *) OnetapSwingTick, "onetap_swing_tick");
    int n = 0;
    for (int i = 0; i < HTR_NSITE; i++)
        n += HtrPatchSite(i);
    g_htrOn = 1;
    Xlog("htr: holster tracer ON - %d/%d engine entries traced, %d holster / draw anim ids for the overlay lookups, ring %d events, dump [-%d, +%d] frames " "around every paired-move start (tools/win/trace_holster.py)", n, HTR_NSITE, HTR_HSID_N, HTR_N, HTR_PRE, HTR_POST);
}
