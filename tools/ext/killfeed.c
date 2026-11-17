// ---------------------------------------------------------------- kill feed as icons: "KILLER [weapon icon] VICTIM"
// Included by t2r-ext.c right after death_ui.c (one translation unit; uses its du_weapon icon table). Log prefix 'killfeed:'.
// Off: t2r-old-killfeed.txt next to the exe (polled every 2 s) = the client's debug-draw text feed ("X killed Y" / "X died") as before.
//
// Capture (no new network traffic): winmm's death handler ApplyConfirmedDeath (FUN_1800379b0: ecx victim slot, edx killer slot,
// r8 weapon SID, r9d headshot) is hooked only to remember the weapon SID for the duration of its call; the two feed entry points it
// calls are hooked too - AddKillFeedEntry (FUN_1800606d0: rcx killer std::string*, rdx victim std::string*, r8b killer friendly,
// r9b victim friendly) and AddDeathFeedEntry (FUN_180060910: rcx victim std::string*, dl friendly). Both are NOT passed on to winmm
// (its text feed stays empty = nothing to suppress in DrawKillFeed) unless the fallback file exists or our widgets are missing
// from hud-root (then the old feed shows). The names / friendliness come from the winmm call itself, so they match the old feed exactly.
// The hooks only queue the event; feature_killfeed (every frame) writes the widgets.
//
// Widgets (killfeed_widgets.py, baked into hud-root): t2rKf0..t2rKf4 = row groups (3840 x 78 canvas units, top-left anchored, moved with
// gui2-rxfm-set-pivot-as; the group's alpha fades the whole row) with kb / kr (killer name, blue = friendly / red = enemy, right aligned,
// ends at the icon), ic (weapon icon, StretchAspect in a fixed box, centred on the icon column), vb / vr (victim name, left aligned,
// starts at the icon). The icon column is fixed so no text width is needed. A row's strings are written while its group is hidden and
// shown one frame later (a gui2 text whose string changes draws nothing for a frame). Newest entry at the bottom; when the oldest
// expires the others glide up; a sixth entry replaces the oldest.
#define KF_ROWS      5
#define KF_PT        44         // = killfeed_widgets.KF_PT (the name texts' pointSize)
#define KF_LIFE_MS   6000
#define KF_FADEIN_MS 150
#define KF_FADEOUT_MS 1000
#define KF_TOP       150.0f     // canvas units (3840 x 2160): 50 px of the 1280 x 720 text space, as the client's feed
#define ROW_H_UNITS  78.0f      // = killfeed_widgets.ROW_H
#define KF_PITCH     84.0f      // 28 px
// backdrop geometry = killfeed_widgets.py (ICX 3075, ICON_W 270, GAP 20, VICTIM_W 520, killer box 1100, SH_Y / SH_CAP / SH_MID_W0)
#define KF_ICON_L    2940.0f
#define KF_KILLER_END 2920.0f
#define KF_KILLER_W  1100.0f
#define KF_VICTIM_X  3230.0f
#define KF_VICTIM_W  520.0f
#define KF_SH_PAD    36.0f
#define KF_SH_Y      (-21.0f)
#define KF_SH_CAP    240.0f
#define KF_SH_MID_W0 1000.0f
static float FeedTextW(const char *s, float scale);
#define KF_SKULL    "streaming/t2r/icons/killfeed-skull.png"
#define KF_FISTS     "streaming/t2r/icons/fist.png"
enum { KFW_GRP, KFW_KB, KFW_KR, KFW_IC, KFW_VB, KFW_VR, KFW_SHL, KFW_SHM,
        KFW_SHR, KFW_KBS, KFW_KRS, KFW_ICS, KFW_VBS, KFW_VRS, KFW_FI,
        KFW_N };
// DOWNS are in the feed too: "A [fist] [weapon] B" (fist icon = the status icon "fi", left of the weapon icon). The same attacker finishing
// the same victim turns that entry's fist into a skull (shake / pop), someone else's finish is a NEW "C [skull] [weapon] B" entry. Every kill
// with a killer shows [skull] [weapon]; "X died" (no killer) stays the single skull in the weapon box. fi = widget "fi" of killfeed_widgets.py
// (older gui2-widgets.pak without it: kills as before, no down entries).
#define KF_FI_W      66.0f      // = killfeed_widgets.FI_W (square status icon)
#define KF_FI_X      2864.0f    // = ICX - ICON_W/2 - 10 - FI_W
#define KF_FI_Y      6.0f       // = (ROW_H - FI_W) / 2
#define KF_IC_W      270.0f     // = killfeed_widgets.ICON_W x ICON_H (the weapon icon box, top-left KF_ICON_L, KF_FI_Y)
#define KF_IC_H      66.0f
#define KF_FI_SPAN   76.0f      // the killer name ends this much further left when the row has the status icon
#define KF_POP_MS    200        // shared icon pop-in: fist on a new down, skull on a new kill, skull replacing the fist
#define KF_OUT_MS    90         // the fist shrinks / fades out before the skull pops in
#define MSG_KFWEAPON_DOWN 1     // MSG_KFWEAPON byte 9: 1 = the weapon that DOWNED the sender (absent / 0 = the death weapon)
// the feed showed a fist but the death screen showed a shiv - winmm's weapon SID is often empty / generic (executions, shivs, melee
// downs); the victim's death card knows better (du_resolve: execution timeline, ext lethal hits, downer / hitter credit, held gun). The VICTIM
// resolves its own death the same way and sends it (MSG_KFWEAPON 0x94 [u8 victim slot][u64 weapon]); every other client holds the row up to
// KF_WAIT_MS for it, then shows winmm's weapon. Our own death: resolved here at once.
#define MSG_KFWEAPON 0x94
#define KF_WAIT_MS   700
typedef struct {
    char killer[44], victim[44];
    int have_killer, kf, vf, vslot, kslot, own, is_down, icon_known;
    const char *icon;
    uint64_t wsid, wait_until, born;
} KfEvT;
static KfEvT g_kfQ[8];
static int g_kfQn;
static SRWLOCK g_kfLk = SRWLOCK_INIT;  // guards g_kf_q / g_kf_qn / g_kf_vw: the death hooks and MSG_KFWEAPON may run off the main thread; never held across an engine / network call
static uint64_t g_kfw[KF_ROWS][KFW_N];
static int g_kf_ok, g_kf_fi_ok; // widgets resolved + present; the status icon "fi" present
static struct {
    int st;
    uint64_t t0, seq, set_frame;
    float y, ly, la, a, size, kw, vw;
    char killer[44], victim[44];
    int have_killer, kf, vf;
    uint64_t born, fa_t0;
    int fi_kind, is_down, fa_mode, vslot;
    float kend;
    const char *wicon, *next_wicon;
    int ic_anim;
} g_kfs[KF_ROWS];               // st 0 free, 1 written (hidden, waits a frame), 2 shown
static uint64_t g_kfSeq;
// the feed is two layers - icon / backdrop rows are hud-root widgets (above the death screen), the names come from our HUD
// draw hook (under every gui2 widget) - so the pause menu got the names drawn over its MISSION bar and the death screen covered the
// names but not the icons. The whole feed now follows one visibility: 0 while to1/pause-menu is on the stack (winmm's own HUD gate,
// 0x180061F70), 1 - the death screen's black while that is up (back as the black lifts for spectating / respawn). Entries keep ageing.
static int g_kfPaused;
static float g_kfVis = 1.0f;
static int g_kfOldFlag;
static uint64_t g_kfOldChk;
static volatile uint64_t g_kfClose[T2R_MAX_PEERS];     // a revive of that victim (down entries older than this can no longer be finished into a skull)
static uint64_t g_kfPw;
static int g_kf_pending, g_kf_pv = -1, g_kf_pk = -1;
static struct {
    uint64_t w, ms;
} g_kf_vw[T2R_MAX_PEERS], g_kf_dw[T2R_MAX_PEERS];       // g_kf_dw: the weapon that downed the victim (kind 1)
static uint64_t g_kfDnMs[T2R_MAX_PEERS];      // last down note per victim (a re-down desync resends state 1)
static struct {
    char name[44];
    uint64_t ms;
} g_kf_rk[8];
static int g_kfRkn;            // victims of the last kills (a late down must not follow its own kill)    // the victim's own answer (MSG_KFWEAPON), by victim slot
static void (*g_kf_acd_tramp)(int, int, uint64_t, uint32_t);
static void (*g_kf_kill_tramp)(const void *, const void *, uint8_t,
                               uint8_t, uint32_t);
static void (*g_kf_died_tramp)(const void *, uint8_t, uint32_t);

static const char *KfStr(const uint8_t *s)
{                               // MSVC std::string: SSO buffer in place when capacity (+0x18) < 16
    if (!s || IsBadReadPtr(s, 0x20))
        return "";
    const char *p =
        *(const uint64_t *) (s + 0x18) <
        0x10 ? (const char *) s : *(const char *const *) s;
    return p && !IsBadReadPtr(p, 1) ? p : "";
}

static int KfResolve(void)
{
    static uint64_t last_try;
    if (g_kf_ok)
        return 1;
    uint64_t now = GetTickCount64();
    if (last_try && now - last_try < 2000)
        return 0;
    last_try = now;
    static const char *const SUF[KFW_N] =
        { "", ".kb", ".kr", ".ic", ".vb", ".vr", ".shl", ".shm", ".shr",
".kbs", ".krs", ".ics", ".vbs", ".vrs", ".fi" };
    for (int r = 0; r < KF_ROWS; r++)
        for (int i = 0; i < KFW_N; i++) {
            char p[32];
            snprintf(p, sizeof p, "t2rKf%d%s", r, SUF[i]);
            g_kfw[r][i] = Hudw(p);
        }
    uint64_t g[1] = { g_kfw[0][KFW_GRP] };
    uint64_t xf = g[0] ? DcCall(SID_GUI2_GET_WIDGET_TRANSFORM, 1, g) : 0;
    if (!xf || xf == ~0ULL || IsBadReadPtr((void *) (uintptr_t) xf, 0x28)) {
        static int nl;
        if (LOGCAP(nl, 3))
            Xlog("killfeed: t2rKf0 not in hud-root (older gui2-widgets.pak) - the stock text feed stays");
        return 0;
    }
    g_kf_ok = 1;
    g_kf_fi_ok = g_kfw[0][KFW_FI] != 0;
    Xlog("killfeed: widgets found (%d rows, status icon %s) - icon feed on, stock text feed suppressed", KF_ROWS, g_kf_fi_ok ? "yes" : "NO (older pak: no down entries)");
    return 1;
}

static int KfUseOld(void)
{
    if (OvOff("t2r-old-killfeed.txt", &g_kfOldFlag, &g_kfOldChk))
        return 1;
    return !g_kf_ok;            // (the hooks may run off the main thread - no gui2 natives there; feature_killfeed resolves)
}

static void KfQueue(const char *killer, const char *victim,
                     int have_killer, int kf, int vf)
{
    KfEvT ev, *e = &ev;       // built outside the lock, copied in under it
    memset(e, 0, sizeof *e);
    snprintf(e->killer, sizeof e->killer, "%.40s", killer);
    snprintf(e->victim, sizeof e->victim, "%.40s", victim);
    e->have_killer = have_killer;
    e->kf = kf != 0;
    e->vf = vf != 0;
    e->wsid = g_kf_pending ? g_kfPw : 0;
    e->vslot = g_kf_pending ? g_kf_pv : -1;
    e->kslot = g_kf_pending ? g_kf_pk : -1;
    uint32_t me = *(uint32_t *) T2R_SYM(kMySlot);
    uint64_t now = GetTickCount64();
    AcquireSRWLockExclusive(&g_kfLk);
    if (have_killer && e->vslot == (int) me)
        e->own = 1;             // our own death: resolved in feature_killfeed (main thread - this hook may not be)
    else if (have_killer && e->vslot >= 0 && e->vslot < T2R_MAX_PEERS) {
        if (g_kf_vw[e->vslot].ms && now - g_kf_vw[e->vslot].ms < 3000) {
            e->wsid = g_kf_vw[e->vslot].w;
            g_kf_vw[e->vslot].ms = 0;
        } else
            e->wait_until = now + KF_WAIT_MS;   // the victim's answer may still be on its way
    }
    if (g_kfQn >= 0 && g_kfQn < (int) (sizeof g_kfQ / sizeof *g_kfQ))
        g_kfQ[g_kfQn++] = ev; // g_kf_qn only changes under the lock and stays in [0, 8]: index <= 7
    ReleaseSRWLockExclusive(&g_kfLk);
}

static void KfPickIcon(KfEvT *e)
{
    const char *nm = NULL, *ic = NULL, *src = NULL;
    if (!e->have_killer)
        e->icon = KF_SKULL;     // fall / suicide / environment: "X died" = skull
    else if (e->wsid && DuWeapon(e->wsid, &nm, &ic, &src) && ic) {
        e->icon = ic;
        e->icon_known = 1;
    } else
        e->icon = KF_FISTS;     // unknown weapon with a killer: the death screen's fallback too
    static int nl;
    if (LOGCAP(nl, 40))
        Xlog("killfeed: %s%s%s%s%s -> %s (weapon %llx = %s)",
             e->is_down ? "DOWN " : "", e->have_killer ? e->killer : "",
             e->have_killer ? " x " : "", e->victim,
             e->have_killer ? "" : " died", e->icon,
             (unsigned long long) e->wsid, nm ? nm : "-");
}

static void KfWeaponRecv(uint32_t from, const uint8_t *b, int len)
{                               // MSG_KFWEAPON from the victim (byte 9 = 1: the weapon that downed it)
    if (len < 9 || from >= T2R_MAX_PEERS || b[0] != from)
        return;
    int dn = len >= 10 && b[9] == MSG_KFWEAPON_DOWN;
    uint64_t w;
    memcpy(&w, b + 1, 8);
    if (dn)
        BkWeapon(from, w);     // t2r-ext.c: molotov / nail bomb kill bonus
    AcquireSRWLockExclusive(&g_kfLk);
    for (int i = 0; i < g_kfQn; i++)
        if (g_kfQ[i].wait_until && g_kfQ[i].vslot == (int) from
            && g_kfQ[i].is_down == dn) {
            g_kfQ[i].wsid = w;
            g_kfQ[i].wait_until = 0;
            ReleaseSRWLockExclusive(&g_kfLk);
            return;
        }
    if (dn) {
        g_kf_dw[from].w = w;
        g_kf_dw[from].ms = GetTickCount64();
    }                           // ahead of its MSG_DOWNED
    else {
        g_kf_vw[from].w = w;
        g_kf_vw[from].ms = GetTickCount64();
    }                           // ahead of winmm's death on this client
    ReleaseSRWLockExclusive(&g_kfLk);
}

static void KfDownPush(int vslot, int kslot, uint64_t w, int wait)
{
    KfEvT ev;
    memset(&ev, 0, sizeof ev);
    ev.is_down = 1;
    ev.have_killer = 1;
    ev.vslot = vslot;
    ev.kslot = kslot;
    ev.wsid = w;
    uint64_t now = GetTickCount64();
    AcquireSRWLockExclusive(&g_kfLk);
    if (!w && wait) {
        if (g_kf_dw[vslot].ms && now - g_kf_dw[vslot].ms < 3000) {
            ev.wsid = g_kf_dw[vslot].w;
            g_kf_dw[vslot].ms = 0;
        } else
            ev.wait_until = now + KF_WAIT_MS;   // the victim's MSG_KFWEAPON (kind 1) may still be on its way
    }
    if (g_kfQn >= 0 && g_kfQn < (int) (sizeof g_kfQ / sizeof *g_kfQ))
        g_kfQ[g_kfQn++] = ev;
    ReleaseSRWLockExclusive(&g_kfLk);
}

// A peer went down (their MSG_DOWNED state 1, t2r-ext.c): everyone, bystanders and both teams, books the entry from the same message.
static void KfDownNote(uint32_t victim, uint32_t downer)
{
    uint32_t me = *(uint32_t *) T2R_SYM(kMySlot);
    if (victim >= T2R_MAX_PEERS || downer >= T2R_MAX_PEERS || victim == me)
        return;                 // 0xFE / no downer (a fall): the death is the "X died" skull
    uint64_t now = GetTickCount64();
    if (g_kfDnMs[victim] && now - g_kfDnMs[victim] < 3000)
        return;                 // a re-down desync resend
    g_kfDnMs[victim] = now;
    KfDownPush((int) victim, (int) downer, 0, 1);
}

// We went down (downed_set): the entry here, and our weapon for everyone (we know the downing hit; the others only guess from the downer's held gun).
static void KfDownLocal(uint32_t downer, uint64_t w)
{
    uint32_t me = *(uint32_t *) T2R_SYM(kMySlot);
    if (me >= T2R_MAX_PEERS || downer >= T2R_MAX_PEERS)
        return;
    uint64_t now = GetTickCount64();
    if (g_kfDnMs[me] && now - g_kfDnMs[me] < 500)
        return;
    g_kfDnMs[me] = now;
    if (!w && g_dk.down == (int) downer)
        w = g_dk.down_w;
    BkWeapon(me, w);           // t2r-ext.c: molotov / nail bomb kill bonus (the downer is known: award_down ran first)
    if (g_sendTramp && w) {
        uint8_t m[10];
        m[0] = (uint8_t) me;
        memcpy(m + 1, &w, 8);
        m[9] = MSG_KFWEAPON_DOWN;
        g_sendTramp(MSG_KFWEAPON, m, sizeof m, 0xff);
    }
    KfDownPush((int) me, (int) downer, w, 0);
}

static void KfDownClosed(uint32_t victim)
{
    if (victim < T2R_MAX_PEERS)
        g_kfClose[victim] = GetTickCount64();
}                               // revived: that down is over, no death follows

// name + friendliness of a slot: the server roster (lobby object), else the peer table
static void KfSlotInfo(int slot, char *out, int *friendly)
{
    typedef uint8_t *(*fn_obj)(void);
    typedef uint8_t *(*fn_peer)(uint32_t);
    uint32_t me = *(uint32_t *) T2R_SYM(kMySlot);
    out[0] = 0;
    *friendly = slot == (int) me || SameTeam((uint32_t) slot, me);
    uint8_t *o = ((fn_obj) T2R_SYM(kFnNetPlayersCtx)) ();
    if (o && !IsBadReadPtr(o, 0x1d + 8 * 0x32)) {
        int n = o[0x1a] > 8 ? 8 : o[0x1a];
        for (int i = 0; i < n; i++) {
            const uint8_t *e = o + 0x1d + i * 0x32;
            if (e[0] == slot && e[1]) {
                snprintf(out, 44, "%.23s", (const char *) (e + 1));
                return;
            }
        }
    }
    uint8_t *p = ((fn_peer) T2rPeer) ((uint32_t) slot);
    if (p && !IsBadReadPtr(p + 6, 0x18))
        snprintf(out, 44, "%.23s", (const char *) (p + 6));
}

static void KfAcdHook(int victim, int killer, uint64_t w, uint32_t hs)
{
    g_kfPw = w;
    g_kf_pv = victim;
    g_kf_pk = killer;
    g_kf_pending = 1;
    g_kf_acd_tramp(victim, killer, w, hs);
    g_kf_pending = 0;
}

static void KfKillHook(const void *killer, const void *victim,
                         uint8_t kf, uint8_t vf, uint32_t color)
{
    if (KfUseOld()) {
        g_kf_kill_tramp(killer, victim, kf, vf, color);
        return;
    }
    KfQueue(KfStr(killer), KfStr(victim), 1, kf, vf);
}

static void KfDiedHook(const void *victim, uint8_t vf, uint32_t color)
{
    if (KfUseOld()) {
        g_kf_died_tramp(victim, vf, color);
        return;
    }
    KfQueue("", KfStr(victim), 0, 0, vf);
}

static void *KfPatch(uint64_t va, const uint8_t *expect, int ncopy,
                      void *hook, const char *what, void **tramp_out)
{
    uint8_t *f = (uint8_t *) T2R_LEGACY_VA(va);
    if (memcmp(f, expect, (size_t) ncopy)) {
        HOOK_MISS(f, (size_t) ncopy,
                  "killfeed: %s prologue differs, not hooked", what);
        return NULL;
    }
    uint8_t *t =
        VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE,
                     PAGE_EXECUTE_READWRITE);
    if (!t)
        return NULL;
    memcpy(t, f, (size_t) ncopy);
    t[ncopy] = 0xff;
    t[ncopy + 1] = 0x25;
    *(uint32_t *) (t + ncopy + 2) = 0;
    *(uint64_t *) (t + ncopy + 6) = (uint64_t) (f + ncopy);
    if (tramp_out)
        *tramp_out = t;         // published before the patch: the hook calls it the moment the jump is in
    DWORD old;
    VirtualProtect(f, (size_t) ncopy, PAGE_EXECUTE_READWRITE, &old);
    f[0] = 0x48;
    f[1] = 0xb8;
    *(uint64_t *) (f + 2) = (uint64_t) hook;
    f[10] = 0xff;
    f[11] = 0xe0;               // mov rax, hook; jmp rax
    memset(f + 12, 0x90, (size_t) (ncopy - 12));
    VirtualProtect(f, (size_t) ncopy, old, &old);
    FlushInstructionCache(GetCurrentProcess(), f, (size_t) ncopy);
    return t;
}

static void InstallKillfeed(void)
{
    static const uint8_t acd[12] =
        { 0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x56, 0x57,
0x53 };
    static const uint8_t kill[17] =
        { 0x55, 0x56, 0x48, 0x81, 0xec, 0x78, 0x01, 0, 0, 0x48, 0x8d, 0xac,
0x24, 0x80, 0, 0, 0 };
    static const uint8_t died[18] =
        { 0x55, 0x56, 0x57, 0x48, 0x81, 0xec, 0x60, 0x01, 0, 0, 0x48, 0x8d,
0xac, 0x24, 0x80, 0, 0, 0 };
    // all three checked before any is patched: a half-installed feed (weapon known, entries not hooked or the reverse) is harmless but pointless
    if (memcmp((void *) T2R_CODE(kCodeKillfeedBody), acd, sizeof acd)
        || memcmp((void *) T2R_CODE(kCodeKillfeedCall), kill, sizeof kill)
        || memcmp((void *) T2R_CODE(kCodeKillfeedPrologue), died,
                  sizeof died)) {
        HOOK_MISS((void *) T2R_CODE(kCodeKillfeedPrologue), sizeof died,
                  "killfeed: winmm prologues differ, not installed (stock text feed)");
        return;
    }
    g_kf_acd_tramp =
        KfPatch(0x1800379B0, acd, sizeof acd, (void *) KfAcdHook,
                 "ApplyConfirmedDeath", (void **) &g_kf_acd_tramp);
    g_kf_kill_tramp =
        KfPatch(0x1800606D0, kill, sizeof kill, (void *) KfKillHook,
                 "AddKillFeedEntry", (void **) &g_kf_kill_tramp);
    g_kf_died_tramp =
        KfPatch(0x180060910, died, sizeof died, (void *) KfDiedHook,
                 "AddDeathFeedEntry", (void **) &g_kf_died_tramp);
    Xlog("killfeed: hooks installed (death handler %s, kill entry %s, died entry %s)", g_kf_acd_tramp ? "ok" : "FAILED", g_kf_kill_tramp ? "ok" : "FAILED", g_kf_died_tramp ? "ok" : "FAILED");
}

// ---- widgets
static void KfHide(int r)
{
    if (g_kfs[r].st) {
        WVisible(g_kfw[r][KFW_GRP], 0);
    }
    g_kfs[r].st = 0;
}

static void KfReset(void)
{
    if (!g_kf_ok)
        return;
    for (int r = 0; r < KF_ROWS; r++)
        KfHide(r);
}

#define KF_TXT_PX 18.0f         // name line height in the 1280 x 720 text space (row pitch 28 px)
static void KfMeasure(const char *killer, const char *victim, float *size,
                       float *kw, float *vw)
{
    typedef uint64_t(*fn_meas) (const char *, float);
    static float lineh;
    if (!lineh) {               // the nametag code's reference: line height of "Mg" at size 2
        uint64_t m = ((fn_meas) T2R_SYM(kFnMeasureText)) ("Mg", 2.0f);
        float hh;
        uint32_t mh = (uint32_t) (m >> 32);
        memcpy(&hh, &mh, 4);
        lineh = hh > 1.0f && hh < 400.0f ? hh / 2.0f : 40.0f;
        Xlog("killfeed: nametag font line height %.2f px per size unit -> name size %.3f for %.0f px", lineh, KF_TXT_PX / lineh, KF_TXT_PX);
    }
    float s = KF_TXT_PX / lineh;
    *size = s;
    *kw = *vw = 0.0f;
    const char *n[2] = { killer, victim };
    float *o[2] = { kw, vw };
    for (int i = 0; i < 2; i++) {
        if (!n[i][0])
            continue;
        uint64_t m = ((fn_meas) T2R_SYM(kFnMeasureText)) (n[i], s);
        float w;
        uint32_t ml = (uint32_t) m;
        memcpy(&w, &ml, 4);
        *o[i] = w > 0.0f && w < 1280.0f ? w : 0.0f;
    }
}

static void KfShowEvent(const KfEvT *e, uint64_t now)
{
    int r = -1;
    for (int i = 0; i < KF_ROWS; i++)
        if (!g_kfs[i].st) {
            r = i;
            break;
        }
    if (r < 0) {
        r = 0;
        for (int i = 1; i < KF_ROWS; i++)
            if (g_kfs[i].seq < g_kfs[r].seq)
                r = i;
    }                           // full: the oldest gives way
    WVisible(g_kfw[r][KFW_GRP], 0);
    uint64_t *w = g_kfw[r];
    // names are drawn with the nametags' own text routine (winmm FUN_1800039d0: same font + built-in outline) from the HUD draw
    // hook (feature_killfeed_draw), not with gui2 text widgets - the name / shadow widgets stay hidden and unused. Only the icon is a widget.
    ((fn_settext_t) T2R_SYM(kFnSetImage)) (w[KFW_IC], e->icon);
    WScale(w[KFW_IC], 1.0f, 1.0f);
    WPivot(w[KFW_IC], KF_ICON_L, KF_FI_Y);
    WAlpha(w[KFW_IC], 1.0f);
    WVisible(w[KFW_KBS], 0);
    WVisible(w[KFW_KRS], 0);
    WVisible(w[KFW_ICS], 0);
    WVisible(w[KFW_VBS], 0);
    WVisible(w[KFW_VRS], 0);
    WVisible(w[KFW_KB], 0);
    WVisible(w[KFW_KR], 0);
    WVisible(w[KFW_IC], 1);
    WVisible(w[KFW_VB], 0);
    WVisible(w[KFW_VR], 0);
    int fik = e->have_killer && g_kf_fi_ok ? (e->is_down ? 1 : 2) : 0;  // status icon: 1 fist (down), 2 skull (kill), 0 none ("X died")
    if (w[KFW_FI]) {
        if (fik)
            ((fn_settext_t) T2R_SYM(kFnSetImage)) (w[KFW_FI],
                                                   fik ==
                                                   1 ? KF_FISTS :
                                                   KF_SKULL);
        WAlpha(w[KFW_FI], 0.0f);
        WVisible(w[KFW_FI], fik != 0);
    }
    g_kfs[r].fi_kind = fik;
    g_kfs[r].is_down = e->is_down;
    g_kfs[r].fa_mode = fik ? 1 : 0;
    g_kfs[r].fa_t0 = 0;
    g_kfs[r].vslot = e->vslot;
    g_kfs[r].kend = KF_KILLER_END - (fik ? KF_FI_SPAN : 0.0f);
    g_kfs[r].wicon = e->icon;
    g_kfs[r].next_wicon = NULL;
    g_kfs[r].ic_anim = 0;
    memcpy(g_kfs[r].killer, e->killer, sizeof e->killer);
    memcpy(g_kfs[r].victim, e->victim, sizeof e->victim);
    g_kfs[r].have_killer = e->have_killer;
    g_kfs[r].kf = e->kf;
    g_kfs[r].vf = e->vf;
    KfMeasure(g_kfs[r].have_killer ? g_kfs[r].killer : "",
               g_kfs[r].victim, &g_kfs[r].size, &g_kfs[r].kw,
               &g_kfs[r].vw);
    {                           // soft dark backdrop: flat part spans the measured names + icon + KF_SH_PAD, feathered caps (fixed width) outside it.
        // The names are measured with the feed's per-letter estimate (feed_text_w, scaled to the name size) with 12 % to spare - the caps
        // fade out any error. Written while the row is hidden like the strings.
        float kw = g_kfs[r].kw * 3.0f, vw = g_kfs[r].vw * 3.0f; // measured px of the 1280 x 720 text space -> canvas units
        if (kw > KF_KILLER_W)
            kw = KF_KILLER_W;
        if (vw > KF_VICTIM_W)
            vw = KF_VICTIM_W;
        float L =
            (e->have_killer ? g_kfs[r].kend - kw : KF_ICON_L) - KF_SH_PAD,
            R = KF_VICTIM_X + vw + KF_SH_PAD;
        if (L < KF_SH_CAP)
            L = KF_SH_CAP;
        WPivot(w[KFW_SHL], L - KF_SH_CAP, KF_SH_Y);
        WPivot(w[KFW_SHM], L, KF_SH_Y);
        WScale(w[KFW_SHM], (R - L) / KF_SH_MID_W0, 1.0f);
        WPivot(w[KFW_SHR], R, KF_SH_Y);
    }
    WAlpha(w[KFW_GRP], 0.0f);
    g_kfs[r].st = 1;
    g_kfs[r].t0 = g_kfs[r].born = now;
    g_kfs[r].seq = ++g_kfSeq;
    g_kfs[r].set_frame = g_frame;
    g_kfs[r].y = -1.0f;
    g_kfs[r].ly = -1.0f;
    g_kfs[r].la = -1.0f;        // y is placed on the first pass below
}

// shared icon pop (fist on a new down, skull on a new kill, skull replacing the fist): overshoot scale + short horizontal shake, eased
static void KfFiApply(int r, float sc, float dx, float al)
{
    uint64_t fi = g_kfw[r][KFW_FI];
    WScale(fi, sc, sc);        // scales about the widget's top-left: moved back so the icon's centre stays put
    WPivot(fi, KF_FI_X + dx + (1.0f - sc) * KF_FI_W * 0.5f,
            KF_FI_Y + (1.0f - sc) * KF_FI_W * 0.5f);
    WAlpha(fi, al);
    // a finish with another weapon than the down's (downed with the Frontier, executed with a pistol / fists ...) swaps the weapon icon
    // with the same shrink-out / pop-in / shake as the fist -> skull, in step with it
    uint64_t ic = g_kfs[r].ic_anim ? g_kfw[r][KFW_IC] : 0;
    if (ic) {
        WScale(ic, sc, sc);
        WPivot(ic, KF_ICON_L + dx + (1.0f - sc) * KF_IC_W * 0.5f,
                KF_FI_Y + (1.0f - sc) * KF_IC_H * 0.5f);
        WAlpha(ic, al);
    }
}

static void KfFiAnim(int r, uint64_t now)
{
    if (!g_kfs[r].fi_kind || !g_kfs[r].fa_mode || !g_kfw[r][KFW_FI])
        return;
    if (!g_kfs[r].fa_t0)
        g_kfs[r].fa_t0 = now;
    if (g_kfs[r].fa_mode == 2) {        // the fist shrinks and fades, then the skull pops in
        float u = (float) (now - g_kfs[r].fa_t0) / KF_OUT_MS;
        if (u < 1.0f) {
            KfFiApply(r, 1.0f - 0.45f * u * u, 0.0f, 1.0f - u);
            return;
        }
        ((fn_settext_t) T2R_SYM(kFnSetImage)) (g_kfw[r][KFW_FI], KF_SKULL);
        if (g_kfs[r].next_wicon) {
            ((fn_settext_t) T2R_SYM(kFnSetImage)) (g_kfw[r][KFW_IC],
                                                   g_kfs[r].next_wicon);
            g_kfs[r].wicon = g_kfs[r].next_wicon;
            g_kfs[r].next_wicon = NULL;
        }
        g_kfs[r].fa_mode = 1;
        g_kfs[r].fa_t0 += KF_OUT_MS;
    }
    float u = (float) (now - g_kfs[r].fa_t0) / KF_POP_MS;
    if (u >= 1.0f) {
        KfFiApply(r, 1.0f, 0.0f, 1.0f);
        g_kfs[r].fa_mode = 0;
        g_kfs[r].ic_anim = 0;
        return;
    }
    float t = u - 1.0f, eb = 1.0f + 4.0f * t * t * t + 3.0f * t * t;    // easeOutBack, c1 = 3
    float dx = 12.0f * __builtin_sinf(u * 15.7f) * (1.0f - u);  // 2.5 shakes, dying out
    float al = u * 4.0f;
    if (al > 1.0f)
        al = 1.0f;
    KfFiApply(r, 0.4f + 0.6f * eb, dx, al);
}

// The same attacker finishes the victim he downed: the newest down entry of that victim becomes the kill (fist -> skull, lifetime refreshed).
static int KfTryConvert(const KfEvT *e, uint64_t now)
{
    if (!e->have_killer || !g_kf_fi_ok)
        return 0;
    int b = -1;
    for (int r = 0; r < KF_ROWS; r++)
        if (g_kfs[r].st && g_kfs[r].fi_kind
            && !strcmp(g_kfs[r].victim, e->victim) && (b < 0
                                                       || g_kfs[r].seq >
                                                       g_kfs[b].seq))
            b = r;
    if (b < 0 || !g_kfs[b].is_down || strcmp(g_kfs[b].killer, e->killer))
        return 0;               // someone else's finish (or none down): a new entry
    if (g_kfs[b].vslot >= 0 && g_kfs[b].vslot < T2R_MAX_PEERS
        && g_kfClose[g_kfs[b].vslot] > g_kfs[b].born)
        return 0;               // revived since
    g_kfs[b].is_down = 0;
    g_kfs[b].fi_kind = 2;
    g_kfs[b].t0 = now;
    if (e->icon_known && e->icon != g_kfs[b].wicon)
        g_kfs[b].next_wicon = e->icon;
    g_kfs[b].ic_anim = g_kfs[b].next_wicon && g_kfs[b].st != 1; // shown already: the weapon icon swaps with the fist's animation
    if (g_kfs[b].st == 1) {     // not on screen yet: it just opens as the kill
        ((fn_settext_t) T2R_SYM(kFnSetImage)) (g_kfw[b][KFW_FI], KF_SKULL);
        if (g_kfs[b].next_wicon) {
            ((fn_settext_t) T2R_SYM(kFnSetImage)) (g_kfw[b][KFW_IC],
                                                   g_kfs[b].next_wicon);
            g_kfs[b].wicon = g_kfs[b].next_wicon;
            g_kfs[b].next_wicon = NULL;
        }
    } else {
        g_kfs[b].fa_mode = 2;
        g_kfs[b].fa_t0 = now;
    }
    Xlog("killfeed: %s finished %s - entry %d fist -> skull", e->killer,
         e->victim, b);
    return 1;
}

static void FeatureKillfeed(void)
{
    static uint64_t prev_ms;
    uint64_t now = GetTickCount64();
    if (MatchSubstate() != 6) {
        KfReset();
        AcquireSRWLockExclusive(&g_kfLk);
        g_kfQn = 0;
        ReleaseSRWLockExclusive(&g_kfLk);
        prev_ms = 0;
        g_kf_ok = 0;
        return;
    }                           // (re-resolved each match: hud-root is rebuilt per level)
    if (!g_kf_ok && !KfResolve()) {
        AcquireSRWLockExclusive(&g_kfLk);
        g_kfQn = 0;
        ReleaseSRWLockExclusive(&g_kfLk);
        return;
    }
    {
        int n = 0, nr = 0;
        KfEvT ready[8];
        AcquireSRWLockExclusive(&g_kfLk);      // take the due events out under the lock; the waiting ones stay (compacted); the work below runs unlocked
        for (int i = 0; i < g_kfQn && i < 8; i++) {
            if (g_kfQ[i].wait_until && now < g_kfQ[i].wait_until) {
                g_kfQ[n++] = g_kfQ[i];
                continue;
            }                   // still waiting for the victim's answer
            if (!g_kfQ[i].is_down && g_kfQ[i].vslot >= 0) {   // a kill never overtakes the down of the same victim that is still waiting for its weapon
                int hold = 0;
                for (int j = 0; j < g_kfQn && j < 8; j++)
                    if (g_kfQ[j].is_down
                        && g_kfQ[j].vslot == g_kfQ[i].vslot
                        && g_kfQ[j].wait_until
                        && now < g_kfQ[j].wait_until)
                        hold = 1;
                if (hold) {
                    g_kfQ[n++] = g_kfQ[i];
                    continue;
                }
            }
            ready[nr++] = g_kfQ[i];
        }
        g_kfQn = n;
        ReleaseSRWLockExclusive(&g_kfLk);
        for (int i = 0; i < nr; i++) {
            if (ready[i].own) { // our own death: the death card's answer, and to everyone
                int k = -1;
                uint64_t w = 0;
                const char *why = "-";
                uint32_t me = *(uint32_t *) T2R_SYM(kMySlot);
                DuResolve(now, &k, &w, &why);
                if (w) {
                    Xlog("killfeed: our death - winmm weapon %llx, death card says %llx (%s)", (unsigned long long) ready[i].wsid, (unsigned long long) w, why);
                    ready[i].wsid = w;
                }
                if (g_sendTramp && w) {
                    uint8_t m[9];
                    m[0] = (uint8_t) me;
                    memcpy(m + 1, &w, 8);
                    g_sendTramp(MSG_KFWEAPON, m, sizeof m, 0xff);
                }
            }
            if (ready[i].is_down) {
                int f;
                if (!g_kf_fi_ok)
                    continue;   // older pak without the status icon: no down entries
                KfSlotInfo(ready[i].kslot, ready[i].killer, &f);
                ready[i].kf = f;
                KfSlotInfo(ready[i].vslot, ready[i].victim, &f);
                ready[i].vf = f;
                if (!ready[i].killer[0] || !ready[i].victim[0])
                    continue;
                int late = 0;   // its own kill already came (late packet): no down entry after the kill
                for (int k = 0; k < g_kfRkn; k++)
                    if (!strcmp(g_kf_rk[k].name, ready[i].victim)
                        && now - g_kf_rk[k].ms < 1500)
                        late = 1;
                if (late) {
                    Xlog("killfeed: down of %s arrived after its kill - skipped", ready[i].victim);
                    continue;
                }
                if (!ready[i].wsid) {   // the victim's answer never came: the downer's held gun, else fists
                    typedef uint8_t *(*fn_peer)(uint32_t);
                    uint8_t *kp =
                        ready[i].kslot !=
                        (int) *(uint32_t *) T2R_SYM(kMySlot) ? ((fn_peer)
                                                                T2rPeer) ((uint32_t) ready[i].kslot) : NULL;
                    uint64_t hw = kp
                        && !IsBadReadPtr(kp + 0x91,
                                         8) ? *(uint64_t *) (kp +
                                                             0x91) : 0;
                    ready[i].wsid = hw && hw != ~0ULL
                        && BaseWeaponIsGun(hw) ? hw : FNV64("fists");
                }
                KfPickIcon(&ready[i]);
                KfShowEvent(&ready[i], now);
                continue;
            }
            KfPickIcon(&ready[i]);
            if (ready[i].have_killer) {
                if (g_kfRkn < 8)
                    g_kfRkn++;
                memmove(g_kf_rk + 1, g_kf_rk,
                        (size_t) (g_kfRkn - 1) * sizeof *g_kf_rk);
                snprintf(g_kf_rk[0].name, sizeof g_kf_rk[0].name, "%s",
                         ready[i].victim);
                g_kf_rk[0].ms = now;
                if (KfTryConvert(&ready[i], now))
                    continue;
            }
            KfShowEvent(&ready[i], now);
        }
    }
    float dt = prev_ms && now > prev_ms ? (float) (now - prev_ms) : 16.0f;
    prev_ms = now;
    {
        g_kfPaused = HUD_MENU_UP();    // (was the pause page alone: the options sub-pages / backpack / TAB showed the feed - hud_menu_up, t2r-ext.c)
        float blk = g_du.phase ? DuVal(&g_du.tr[DT_BLACK], now) : 0.0f;
        if (blk < 0.0f)
            blk = 0.0f;
        else if (blk > 1.0f)
            blk = 1.0f;
        g_kfVis = g_kfPaused ? 0.0f : 1.0f - blk;
    }
    for (int r = 0; r < KF_ROWS; r++)
        if (g_kfs[r].st && now - g_kfs[r].t0 >= KF_LIFE_MS) {
            if (g_kfw[r][KFW_FI])
                WVisible(g_kfw[r][KFW_FI], 0);
            KfHide(r);
        }
    int live = 0;
    for (int r = 0; r < KF_ROWS; r++) {
        if (!g_kfs[r].st)
            continue;
        int rank = 0;           // position from the top = number of older live entries
        for (int o = 0; o < KF_ROWS; o++)
            if (g_kfs[o].st && g_kfs[o].seq < g_kfs[r].seq)
                rank++;
        live++;
        float ty = KF_TOP + (float) rank * KF_PITCH;
        if (g_kfs[r].y < 0.0f)
            g_kfs[r].y = ty;    // a new row starts in its place; the others glide
        else
            g_kfs[r].y +=
                (ty - g_kfs[r].y) * (1.0f - __builtin_expf(-dt / 70.0f));
        if (__builtin_fabsf(g_kfs[r].y - ty) < 0.5f)
            g_kfs[r].y = ty;
        if (__builtin_fabsf(g_kfs[r].y - g_kfs[r].ly) > 0.4f) {
            WPivot(g_kfw[r][KFW_GRP], 0.0f, g_kfs[r].y);
            g_kfs[r].ly = g_kfs[r].y;
        }
        if (g_kfs[r].st == 1) {
            if (g_frame <= g_kfs[r].set_frame)
                continue;       // the strings have had no frame yet
            WVisible(g_kfw[r][KFW_GRP], 1);
            g_kfs[r].st = 2;
        }
        uint64_t age = now - g_kfs[r].t0, born_age = now - g_kfs[r].born;       // (t0 is refreshed by a finish; the fade-in counts from the birth)
        float a = 1.0f;
        if (born_age < KF_FADEIN_MS)
            a = 0.1f + 0.9f * (float) born_age / KF_FADEIN_MS;
        if (age + KF_FADEOUT_MS > KF_LIFE_MS) {
            float o = (float) (KF_LIFE_MS - age) / KF_FADEOUT_MS;
            if (o < a)
                a = o;
        }
        if (a < 0.0f)
            a = 0.0f;
        a *= g_kfVis;
        g_kfs[r].a = a;
        KfFiAnim(r, now);
        if (__builtin_fabsf(a - g_kfs[r].la) > 0.01f
            || (a == 0.0f && g_kfs[r].la != 0.0f)) {
            WAlpha(g_kfw[r][KFW_GRP], a);
            g_kfs[r].la = a;
        }
    }
    (void) live;
}

// Names with the nametags' text routine, every frame from the HUD draw hook (t2r-ext.c, next to feature_msg_tag_draw). Team colours as before
// (ABGR), alpha = the row's fade; the killer ends where the icon column starts, the victim starts where it ends; vertically centred on the row.
static void FeatureKillfeedDraw(void)
{
    typedef void (*fn_text_va)(...);
    if (!g_kf_ok || MatchSubstate() != 6 || g_kfPaused)
        return;
    for (int r = 0; r < KF_ROWS; r++) {
        if (g_kfs[r].st != 2 || g_kfs[r].a <= 0.0f)
            continue;
        uint32_t al = (uint32_t) (g_kfs[r].a * 255.0f + 0.5f) << 24;
        float cy = (g_kfs[r].y + ROW_H_UNITS * 0.5f) / 3.0f, th =
            KF_TXT_PX;
        if (g_kfs[r].have_killer && g_kfs[r].kw > 0.0f)
            ((fn_text_va)
             T2R_SYM(kFnDrawTextVa)) (Fbits(g_kfs[r].kend / 3.0f -
                                            g_kfs[r].kw),
                                      Fbits(cy - th * 0.5f),
                                      Fbits(g_kfs[r].size),
                                      (uint64_t) ((g_kfs[r].
                                                   kf ? 0x00CCB08Fu :
                                                   0x007078CFu) | al),
                                      "%s", g_kfs[r].killer);
        if (g_kfs[r].vw > 0.0f)
            ((fn_text_va)
             T2R_SYM(kFnDrawTextVa)) (Fbits(KF_VICTIM_X / 3.0f),
                                      Fbits(cy - th * 0.5f),
                                      Fbits(g_kfs[r].size),
                                      (uint64_t) ((g_kfs[r].
                                                   vf ? 0x00CCB08Fu :
                                                   0x007078CFu) | al),
                                      "%s", g_kfs[r].victim);
    }
}
