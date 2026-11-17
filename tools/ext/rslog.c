// rslog.c - the "every-logger": every script/engine call that can change the render settings (tonemap / exposure / sun / fog / wetness / snow / wind),
// with who called it. Why: the winter Supermarket changes character/scene lighting at spots SP leaves alone, and our own SP-region replay did not change what the
// user sees, so SOMETHING ELSE sets it. Included from immersion.c (after IMM_RSZ_KEYS). Default ON (dev), opt-out t2r-no-rslog.txt. Lines in t2r-ext.log:
//   "rslog: hooked N of M ..."   once, at the first match tick (+ hooked, - not registered in the engine)
//   "rslog: <native> (<args>) from <ours|tick-thread|script/engine> tid=N cam=x,y,z ra=<mod+off> bt=<mod+off ...>"   first call per (native, arg0, caller) per second
//   "rslog: repeat <native> (<last args>) x<N> <who>"   the repeats of that key, once its 1 s window ended
//   "RSZ ... | rslog<=2s: native(arg0)[who] xN ..."   the RSZ change lines point at the sources of the last 2 s
// Hooks the DcCallFunc at rec+8 of each native (the one the script interpreter and dc_call use); callers that jump to the typed impl at rec+0x10 are NOT seen.
// Cost per call: a 48-slot table probe and a spin lock; the stack walk and xlog only on a new key.
static volatile int g_rslOurs; // set by imm_rs_call around its dc_call
static volatile float g_rslCam[3];     // camera (imm_rsz_tick, 4x a second)
#define RSL_N 40
#define RSL_VIS0 24             // names from here on are level / streaming / visibility natives ("vislog:")
static const char *const RSL_NAMES[RSL_N] = {
    "override-render-settings", "set-render-settings",
        "change-render-setting-value-float",
        "change-render-setting-value-int32", "force-exposure-value",
    "render-settings-force-underwater/f", "set-max-character-wetness",
        "set-rain-state", "set-rain-ripples-state", "enable-fog-region",
        "disable-fog-region",
    "set-light-process-volumetric-intensity",
        "set-state-character-accumulates-snow-from-ground",
        "set-state-character-accumulates-snow-from-sky",
        "spawn-snow-disturb",
    "set-sun-direction", "set-time-of-day", "set-tonemap", "set-exposure",
        "force-exposure", "set-render-setting-value-float",
        "set-fog-settings", "set-sky-settings", "clear-render-settings",
    "load-level", "unload-level", "load-levels", "unload-levels",
        "show-level", "hide-level", "set-level-visible",
        "set-level-visibility", "set-level-hidden", "level-set-visible",
    "request-level", "request-level-load", "request-level-unload",
        "stream-in-level", "stream-out-level", "task-load-level"
};

static fn_dccall g_rslOrig[RSL_N];
static uint64_t g_rslSid[RSL_N];
static int g_rslOff = -1, g_vis_off = -1;
typedef struct {
    uint64_t key, t0;
    int n;
    int slot;
    uint64_t last[10];
    int argc;
    int ours;
} RslEnt;
static RslEnt g_rslTab[48];
static volatile LONG g_rslLock;
static volatile LONG64 g_rslTotal[RSL_N];

static const char *RslSym(uint64_t sid)
{
    static uint64_t *rh;
    static int init;
    if (!init) {
        init = 1;
        rh = (uint64_t *) calloc(RSD_N, 8);
        if (rh)
            for (unsigned i = 0; i < RSD_N; i++)
                rh[i] = FNV64(g_rsdRegions[i].sym);
    }
    for (size_t i = 0; i < DC_NAMES_N; i++)
        if (DC_NAMES[i].sid == sid)
            return DC_NAMES[i].name;
    if (rh)
        for (unsigned i = 0; i < RSD_N; i++)
            if (rh[i] == sid)
                return g_rsdRegions[i].sym;
    static uint64_t kh[IMM_RSZ_N];
    static int kinit;
    if (!kinit) {
        kinit = 1;
        for (int i = 0; i < IMM_RSZ_N; i++)
            kh[i] = FNV64(IMM_RSZ_KEYS[i]);
    }
    for (int i = 0; i < IMM_RSZ_N; i++)
        if (kh[i] == sid)
            return IMM_RSZ_KEYS[i];
    return NULL;
}

static int RslArgs(char *o, size_t cap, int argc, const uint64_t *a)
{
    int q = 0;
    for (int i = 0; i < argc && i < 10 && q < (int) cap - 60; i++) {
        const char *nm = RslSym(a[i]);
        uint32_t lo = (uint32_t) a[i];
        float f;
        memcpy(&f, &lo, 4);
        if (nm)
            q += snprintf(o + q, cap - (size_t) q, "%s%s", i ? ", " : "",
                          nm);
        else if ((a[i] >> 32) == 0 && lo > 0x00800000u && fabsf(f) > 1e-6f
                 && fabsf(f) < 1e6f)
            q += snprintf(o + q, cap - (size_t) q, "%s%g", i ? ", " : "",
                          (double) f);
        else if (a[i] < 100000)
            q += snprintf(o + q, cap - (size_t) q, "%s%llu", i ? ", " : "",
                          (unsigned long long) a[i]);
        else
            q += snprintf(o + q, cap - (size_t) q, "%s%llx", i ? ", " : "",
                          (unsigned long long) a[i]);
    }
    return q;
}

static void RslWhere(char *o, size_t cap, void *p)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(p, &mbi, sizeof mbi) && mbi.AllocationBase) {
        char nm[64] = "?";
        wchar_t w[MAX_PATH];
        if (GetModuleFileNameW((HMODULE) mbi.AllocationBase, w, MAX_PATH)) {
            wchar_t *s = wcsrchr(w, L'\\');
            s = s ? s + 1 : w;
            size_t k = 0;
            while (*s && k < sizeof nm - 1)
                nm[k++] = (char) *s++;
            nm[k] = 0;
        } else
            snprintf(nm, sizeof nm, "alloc%p", mbi.AllocationBase);
        snprintf(o, cap, "%s+%llx", nm,
                 (unsigned long long) ((uintptr_t) p -
                                       (uintptr_t) mbi.AllocationBase));
    } else
        snprintf(o, cap, "%p", p);
}

static const char *RslWho(int ours)
{
    return ours == 1 ? "ours" : ours ==
        2 ? "tick-thread" : "script/engine";
}

static void RslThunkBody(int idx, int argc, uint64_t *argv, void *ra)
{
    InterlockedIncrement64(&g_rslTotal[idx]);
    uint64_t a0 = argc > 0 && argv ? argv[0] : 0;
    int ours = g_rslOurs ? 1 : GetCurrentThreadId() == g_tickTid ? 2 : 0;
    uint64_t key =
        (g_rslSid[idx] * 0x9E3779B97F4A7C15ULL) ^ (a0 *
                                                    0xC2B2AE3D27D4EB4FULL)
        ^ (uint64_t) (uintptr_t) ra ^ (uint64_t) ours;
    if (!key)
        key = 1;
    uint64_t now = GetTickCount64();
    int fresh = 0, slot = -1;
    RslEnt snap;
    memset(&snap, 0, sizeof snap);
    while (InterlockedExchange(&g_rslLock, 1))
        YieldProcessor();
    for (int i = 0; i < 48; i++)
        if (g_rslTab[i].key == key && now - g_rslTab[i].t0 < 1000) {
            slot = i;
            break;
        }
    if (slot < 0) {
        int oldest = 0;
        for (int i = 0; i < 48; i++) {
            if (now - g_rslTab[i].t0 >= 1000) {
                oldest = i;
                break;
            }
            if (g_rslTab[i].t0 < g_rslTab[oldest].t0)
                oldest = i;
        }
        RslEnt *e = &g_rslTab[oldest];
        if (e->n > 1)
            snap = *e;          // repeats of the displaced entry are flushed below
        e->key = key;
        e->t0 = now;
        e->n = 1;
        e->slot = idx;
        e->argc = argc > 10 ? 10 : argc;
        e->ours = ours;
        memset(e->last, 0, sizeof e->last);
        for (int i = 0; i < e->argc && argv; i++)
            e->last[i] = argv[i];
        fresh = 1;
    } else {
        RslEnt *e = &g_rslTab[slot];
        e->n++;
        for (int i = 0; i < e->argc && argv; i++)
            e->last[i] = argv[i];
    }
    InterlockedExchange(&g_rslLock, 0);
    if (snap.n > 1) {
        char ab[300];
        RslArgs(ab, sizeof ab, snap.argc, snap.last);
        Xlog("%s: repeat %s (%s) x%d %s",
             snap.slot >= RSL_VIS0 ? "vislog" : "rslog",
             RSL_NAMES[snap.slot], ab, snap.n, RslWho(snap.ours));
    }
    if (fresh) {
        char ab[300], w[96], bt[500];
        int q = 0;
        void *st[8];
        RslArgs(ab, sizeof ab, argc, argv);
        RslWhere(w, sizeof w, ra);
        USHORT nf = RtlCaptureStackBackTrace(2, 7, st, NULL);
        for (USHORT i = 0; i < nf && q < (int) sizeof bt - 100; i++) {
            char t[96];
            RslWhere(t, sizeof t, st[i]);
            q += snprintf(bt + q, sizeof bt - (size_t) q, "%s%s",
                          i ? " " : "", t);
        }
        bt[q] = 0;
        Xlog("%s: %s (%s) from %s tid=%lu cam=%.1f,%.1f,%.1f ra=%s bt=%s",
             idx >= RSL_VIS0 ? "vislog" : "rslog", RSL_NAMES[idx], ab,
             RslWho(ours), (unsigned long) GetCurrentThreadId(),
             (double) g_rslCam[0], (double) g_rslCam[1],
             (double) g_rslCam[2], w, bt);
    }
}

#define RSLT(i) static void rsl_thunk##i(uint64_t *ret, int argc, uint64_t *argv, uint64_t sec) { RslThunkBody(i, argc, argv, __builtin_return_address(0)); g_rslOrig[i](ret, argc, argv, sec); }
RSLT(0) RSLT(1) RSLT(2) RSLT(3) RSLT(4) RSLT(5) RSLT(6) RSLT(7) RSLT(8)
RSLT(9) RSLT(10) RSLT(11) RSLT(12) RSLT(13) RSLT(14) RSLT(15) RSLT(16)
RSLT(17) RSLT(18) RSLT(19) RSLT(20) RSLT(21) RSLT(22) RSLT(23) RSLT(24)
RSLT(25) RSLT(26) RSLT(27) RSLT(28) RSLT(29) RSLT(30) RSLT(31) RSLT(32)
RSLT(33) RSLT(34) RSLT(35) RSLT(36) RSLT(37) RSLT(38) RSLT(39)
static const fn_dccall g_rslThunks[RSL_N] =
    { rsl_thunk0, rsl_thunk1, rsl_thunk2, rsl_thunk3, rsl_thunk4,
rsl_thunk5, rsl_thunk6, rsl_thunk7, rsl_thunk8, rsl_thunk9, rsl_thunk10, rsl_thunk11,
    rsl_thunk12, rsl_thunk13, rsl_thunk14, rsl_thunk15, rsl_thunk16,
        rsl_thunk17, rsl_thunk18, rsl_thunk19, rsl_thunk20, rsl_thunk21,
        rsl_thunk22, rsl_thunk23,
    rsl_thunk24, rsl_thunk25, rsl_thunk26, rsl_thunk27, rsl_thunk28,
        rsl_thunk29, rsl_thunk30, rsl_thunk31, rsl_thunk32, rsl_thunk33,
        rsl_thunk34, rsl_thunk35, rsl_thunk36, rsl_thunk37, rsl_thunk38,
        rsl_thunk39
};

static void RslogInstall(void)
{
    char list[1400];
    int q = 0, nh = 0;
    for (int i = 0; i < RSL_N; i++) {
        g_rslSid[i] = FNV64(RSL_NAMES[i]);
        if (i >= RSL_VIS0 ? g_vis_off : g_rslOff)
            continue;
        uint8_t *rec =
            (uint8_t *) ((fn_lookup)
                         T2R_SYM(kFnNativeLookup)) (g_rslSid[i]);
        fn_dccall fn = rec ? *(fn_dccall *) (rec + 8) : NULL;
        if (!fn || !IsCode((void *) fn)) {
            if (q < (int) sizeof list - 80)
                q += snprintf(list + q, sizeof list - (size_t) q, " -%s",
                              RSL_NAMES[i]);
            continue;
        }
        g_rslOrig[i] = fn;
        DWORD old;
        VirtualProtect(rec + 8, 8, PAGE_READWRITE, &old);
        *(fn_dccall volatile *) (rec + 8) = g_rslThunks[i];
        nh++;
        if (q < (int) sizeof list - 80)
            q += snprintf(list + q, sizeof list - (size_t) q, " +%s",
                          RSL_NAMES[i]);
    }
    Xlog("rslog/vislog: hooked %d of %d render-settings + level/streaming natives at rec+8 (+ hooked, - not registered):%s", nh, RSL_N, list);
}

// ---- vislog: loaded / fully-spawned / visible state of every level of the map's level set, 4x a second, logged on change with the camera. The visibility
// getter is used only if the engine registers one. NOT decoded: the camera's vis cell / portal / occluder state (no RE of the exe's visibility system yet).
static void VislogTick(void)
{
    static const char *const SETS[] =
        { "tsk-to1-jackson-market-1", "tsk-to1-train-station-1",
"tsk-to1-downtown-1", "tsk-to1-jackson-suburbs-1", "tsk-to1-outpost-1", "tsk-to1-lodge-1",
        "tsk-to1-hardware-1", "tsk-to1-motel-1",
            "tsk-to1-hillcrest-stores-1", "tsk-to1-tv-station-1",
            "tsk-to1-p1-bookstore-1", "tsk-to1-wilds-houses-1",
            "tsk-to1-capitol-shops-1",
        "tsk-to1-into-the-city-1", "tsk-to1-overpass-1", "tsk-to1-chase-1"
    };
    static uint64_t at, pick_at;
    static int pick = -2;
    static uint8_t prev[64];
    static uint64_t prev_sid[64];
    static int np;
    static const char *const VIS_GETTERS[] =
        { "is-level-visible?", "level-visible?", "is-level-active?" };
    static uint64_t vg;
    if (g_vis_off)
        return;
    uint64_t now = GetTickCount64();
    if (now - at < 250)
        return;
    at = now;
    typedef uint8_t *(*fn_l)(uint64_t);
    if (pick == -2 || now - pick_at > 5000) {   // the first set with a loaded level
        pick_at = now;
        int np2 = -1;
        for (int k = 0; k < (int) (sizeof SETS / sizeof *SETS) && np2 < 0;
             k++) {
            uint8_t *rec =
                ((fn_l) T2R_SYM(kFnNativeLookup)) (FNV64(SETS[k]));
            uint8_t *ls = rec
                && !IsBadReadPtr(rec + 8,
                                 8) ? *(uint8_t **) (rec + 8) : NULL;
            uint8_t *arr = ls
                && !IsBadReadPtr(ls,
                                 0x18) ? *(uint8_t **) (ls + 0x10) : NULL;
            if (!arr || IsBadReadPtr(arr, 0x60))
                continue;
            uint32_t n = *(uint32_t *) (arr + 8);
            uint8_t **lst = *(uint8_t ***) arr;
            if (!lst || n > 64 || IsBadReadPtr(lst, 8ull * n))
                continue;
            for (uint32_t i = 0; i < n; i++) {
                uint8_t *e = lst[i];
                if (!e || IsBadReadPtr(e, 0x10))
                    continue;
                uint64_t sid = *(uint64_t *) (e + 8), l =
                    DcCall(FNV64("is-level-loaded?"), 1, &sid);
                if (l && l != ~0ULL) {
                    np2 = k;
                    break;
                }
            }
        }
        if (np2 != pick) {
            Xlog("vislog: level set %s",
                 np2 >= 0 ? SETS[np2] : "none loaded");
            np = 0;
        }
        pick = np2;
        if (!vg)
            for (int i = 0; i < 3; i++)
                if (DcCallable(FNV64(VIS_GETTERS[i]))) {
                    vg = FNV64(VIS_GETTERS[i]);
                    Xlog("vislog: visibility getter %s registered",
                         VIS_GETTERS[i]);
                    break;
                }
    }
    if (pick < 0)
        return;
    uint8_t *rec = ((fn_l) T2R_SYM(kFnNativeLookup)) (FNV64(SETS[pick]));
    uint8_t *ls = rec
        && !IsBadReadPtr(rec + 8, 8) ? *(uint8_t **) (rec + 8) : NULL;
    uint8_t *arr = ls
        && !IsBadReadPtr(ls, 0x18) ? *(uint8_t **) (ls + 0x10) : NULL;
    if (!arr || IsBadReadPtr(arr, 0x60))
        return;
    uint32_t n = *(uint32_t *) (arr + 8);
    uint8_t **lst = *(uint8_t ***) arr;
    if (!lst || n > 64 || IsBadReadPtr(lst, 8ull * n))
        return;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *e = lst[i];
        if (!e || IsBadReadPtr(e, 0x10))
            continue;
        const char *nm = *(const char **) e;
        uint64_t sid = *(uint64_t *) (e + 8);
        uint64_t l = DcCall(FNV64("is-level-loaded?"), 1, &sid), s =
            DcCall(FNV64("is-level-fully-spawned?"), 1, &sid), v =
            vg ? DcCall(vg, 1, &sid) : 0;
        uint8_t st =
            (uint8_t) ((l
                        && l != ~0ULL) | ((s && s != ~0ULL) << 1) | ((v
                                                                      && v
                                                                      !=
                                                                      ~0ULL)
                                                                     <<
                                                                     2));
        int k = 0;
        while (k < np && prev_sid[k] != sid)
            k++;
        if (k == np) {
            if (np >= 64)
                continue;
            np++;
            prev_sid[k] = sid;
            prev[k] = 0xff;
        }
        if (prev[k] == st)
            continue;
        Xlog("vislog: level %s %s%s%s -> %s%s%s (loaded/spawned/visible) cam=%.1f,%.1f,%.1f", nm && !IsBadReadPtr(nm, 8) ? nm : "?", prev[k] == 0xff ? "?" : (prev[k] & 1) ? "L" : "-", prev[k] == 0xff ? "" : (prev[k] & 2) ? "S" : "-", prev[k] == 0xff || !vg ? "" : (prev[k] & 4) ? "V" : "-", (st & 1) ? "L" : "-", (st & 2) ? "S" : "-", vg ? ((st & 4) ? "V" : "-") : "", (double) g_rslCam[0], (double) g_rslCam[1], (double) g_rslCam[2]);
        prev[k] = st;
    }
}

// ---- vistrace: the camera 10x a second, logged on change (> 5 cm), so a vis-cell / culling toggle in a clip can be placed exactly (the per-level
// loaded / spawned state changes of vislog carry the same camera). NOT done: the renderer's vis cell, portal states, drawn-mesh counts and a generic
// native-call tracer - they need the exe's visibility / dispatcher internals (no RE yet). The offline cell of a camera: level_edit.VisInfo.bitset_at(pos).
// Default ON, opt-out t2r-no-vistrace.txt.
static void VistraceTick(int in_match)
{
    static uint64_t at, nf_a;
    static int nf_v;
    static float prev[3] = { 1e9f, 0, 0 };
    if (!in_match) {
        prev[0] = 1e9f;
        return;
    }
    uint64_t t = GetTickCount64();
    if (t - at < 100 || ImmNoflag("t2r-no-vistrace.txt", &nf_a, &nf_v))
        return;
    at = t;
    float cp[3], cd[3];
    if (!CamPosDir(cp, cd))
        return;
    if (fabsf(cp[0] - prev[0]) + fabsf(cp[1] - prev[1]) +
        fabsf(cp[2] - prev[2]) < 0.05f)
        return;
    memcpy(prev, cp, sizeof prev);
    Xlog("vistrace: cam=%.2f,%.2f,%.2f dir=%.2f,%.2f,%.2f", (double) cp[0],
         (double) cp[1], (double) cp[2], (double) cd[0], (double) cd[1],
         (double) cd[2]);
}

static void RslogTick(int in_match)
{
    static int installed;
    static uint64_t last_sum;
    if (g_rslOff < 0) {
        FILE *f = ExeSideOpen("t2r-no-rslog.txt", "r");
        g_rslOff = f != NULL;
        if (f)
            fclose(f);
        Xlog("rslog: %s",
             g_rslOff ? "off (t2r-no-rslog.txt)" :
             "on (every render-settings native with its caller; t2r-no-rslog.txt = off)");
    }
    if (g_vis_off < 0) {
        FILE *f = ExeSideOpen("t2r-no-vislog.txt", "r");
        g_vis_off = f != NULL;
        if (f)
            fclose(f);
        Xlog("vislog: %s",
             g_vis_off ? "off (t2r-no-vislog.txt)" :
             "on (level load/unload/visibility natives + level-set state changes; t2r-no-vislog.txt = off)");
    }
    if (!in_match)
        return;
    if (!installed) {
        installed = 1;
        RslogInstall();
    }
    VislogTick();
    VistraceTick(in_match);
    uint64_t now = GetTickCount64();
    if (now - last_sum < 1000)
        return;
    last_sum = now;
    RslEnt out[8];
    int no = 0;                 // flush the repeat counts of entries whose 1 s window ended
    while (InterlockedExchange(&g_rslLock, 1))
        YieldProcessor();
    for (int i = 0; i < 48 && no < 8; i++)
        if (g_rslTab[i].n > 1 && now - g_rslTab[i].t0 >= 1000) {
            out[no++] = g_rslTab[i];
            g_rslTab[i].n = 0;
        }
    InterlockedExchange(&g_rslLock, 0);
    for (int i = 0; i < no; i++) {
        char ab[300];
        RslArgs(ab, sizeof ab, out[i].argc, out[i].last);
        Xlog("%s: repeat %s (%s) x%d %s",
             out[i].slot >= RSL_VIS0 ? "vislog" : "rslog",
             RSL_NAMES[out[i].slot], ab, out[i].n, RslWho(out[i].ours));
    }
}

// most recent rslog sources (<= 2 s) for an RSZ change line: "native(arg0)[who] xN ..."
static void RslogRecent(char *o, size_t cap)
{
    uint64_t now = GetTickCount64();
    size_t q = 0;
    o[0] = 0;
    if (g_rslOff != 0)
        return;
    while (InterlockedExchange(&g_rslLock, 1))
        YieldProcessor();
    for (int i = 0; i < 48 && q + 160 < cap; i++) {
        RslEnt *e = &g_rslTab[i];
        if (!e->key || now - e->t0 > 2000)
            continue;
        const char *nm = RslSym(e->last[0]);
        q += (size_t) snprintf(o + q, cap - q, " %s(%s)[%s] x%d",
                               RSL_NAMES[e->slot], nm ? nm : "-",
                               e->ours == 1 ? "ours" : e->ours ==
                               2 ? "tick" : "script", e->n);
    }
    InterlockedExchange(&g_rslLock, 0);
}
