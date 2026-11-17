// stockpickup.c - ps50: the engine's own container pickups shown as an enemy gun -> the supply box ammo pickup. Included from t2r-ext.c
// after containers.c (uses box_spawn_like / b13_name / dc_call / CL_MAP / g_cl_off).
//
// A drawer held shotgun ammo for the Shorty, drawn as a full enemy double-barrel shotgun that lay
// through the drawer front. That is SP's enemy-weapon pickup (in SP you take ammo out of an enemy's gun). It should use the supply
// boxes instead. Our container layer is OFF, so the drawer hands out the SP level's own pickup (containers.c header).
// The log of that report has no spawn-item-pickup line for it: the engine spawns it on another path.
//
// What this does:
//  1. Hooks the other spawn-item-pickup natives (by-spawn-type / dynamic / static / by-obj-id). Each pickup they spawn is queued.
//  2. Asks every container of the map (CL_MAP, while our container layer is off) once a second for its spawned pickup
//     (get-openable-spawned-pickup). A new handle is queued.
//  3. 100 ms later, each queued pickup is checked: get-ammo-process-from-weapon-pickup != 0 = a weapon pickup that gives ammo (the
//     enemy-gun look). Its ammo item and amount are read. The pickup is killed, and the supply box ammo pickup (box_spawn_like) of the
//     same item and amount is spawned at its spot. Plain ammo / item pickups are left alone.
// Unknown: which of the paths the engine really uses for drawers. Every step logs ('stock pickup: ...'), so the first A/B run shows it.
// Opt-out: t2r-stock-pickup-ps50-old.txt next to the exe (read every 2 s) = the engine's pickups stay as they are.
#define SPX_N 4
static const char *const SPX_NAMES[SPX_N] =
    { "spawn-item-pickup-by-spawn-type", "spawn-item-pickup-dynamic",
"spawn-item-pickup-static",
    "spawn-item-pickup-by-obj-id"
};

static fn_native g_spxOrig[SPX_N];
#define SPX_Q 24
static struct {
    uint64_t h, due;
    int via;
} g_spx_q[SPX_Q];               // via: 0..3 = the native, 4 = a container probe
static uint64_t g_spx_cont_h[CL_MAX];   // the last pickup handle each container answered
static uint64_t g_spx_probe_ms, g_spx_live;
static int SpxOff(void)
{
    static int v = -1;
    static uint64_t at;
    uint64_t now = GetTickCount64();
    if (v < 0 || now - at > 2000) {
        at = now;
        FILE *f = ExeSideOpen("t2r-stock-pickup-ps50-old.txt", "r");
        int nv = f != NULL;
        if (f)
            fclose(f);
        if (nv != v)
            Xlog("stock pickup: ps50 %s",
                 nv ?
                 "OFF (t2r-stock-pickup-ps50-old.txt) - engine pickups as they are"
                 :
                 "ON - enemy-gun ammo pickups become the box ammo pickup");
        v = nv;
    }
    return v;
}

static void SpxQueue(uint64_t h, int via)
{
    if (!h || h == ~0ULL)
        return;
    for (int i = 0; i < SPX_Q; i++)
        if (g_spx_q[i].h == h)
            return;
    for (int i = 0; i < SPX_Q; i++)
        if (!g_spx_q[i].h) {
            g_spx_q[i].h = h;
            g_spx_q[i].due = GetTickCount64() + 100;
            g_spx_q[i].via = via;
            return;
        }
    static int nf;
    if (LOGCAP(nf, 4))
        Xlog("stock pickup: queue full - %llx not checked",
             (unsigned long long) h);
}

static void SpxAfter(int via, void *ret, uint64_t a0, uint64_t a1,
                      uint64_t a2)
{
    uint64_t h = ret ? *(uint64_t *) ret : 0;
    static int nl;
    if (LOGCAP(nl, 60))
        Xlog("stock pickup: %s (%s %llx, %llx, %llx) -> %llx",
             SPX_NAMES[via], B13Name(a0), (unsigned long long) a0,
             (unsigned long long) a1, (unsigned long long) a2,
             (unsigned long long) h);
    if (!SpxOff())
        SpxQueue(h, via);
}

#define SPX_HOOK(i) static void spx_hook##i(void *ret, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) { \
    g_spxOrig[i](ret, a0, a1, a2, a3, a4, a5, a6, a7); SpxAfter(i, ret, a0, a1, a2); }
SPX_HOOK(0) SPX_HOOK(1) SPX_HOOK(2) SPX_HOOK(3)
static void InstallStockPickupHooks(void)
{
    fn_native hk[SPX_N] = { spx_hook0, spx_hook1, spx_hook2, spx_hook3 };
    for (int i = 0; i < SPX_N; i++) {
        uint8_t *rec =
            (uint8_t *) ((fn_lookup)
                         T2R_SYM(kFnNativeLookup)) (FNV64(SPX_NAMES[i]));
        if (!rec || !*(void **) (rec + 0x10)
            || !IsCode(*(void **) (rec + 0x10))) {
            Xlog("stock pickup: %s not found - not hooked", SPX_NAMES[i]);
            continue;
        }
        g_spxOrig[i] = *(fn_native *) (rec + 0x10);
        DWORD old;
        VirtualProtect(rec + 0x10, 8, PAGE_READWRITE, &old);
        *(fn_native *) (rec + 0x10) = hk[i];
        VirtualProtect(rec + 0x10, 8, old, &old);
        Xlog("stock pickup: %s hooked", SPX_NAMES[i]);
    }
}

static int SpxAmmoItem(uint64_t id)
{
    const char *n = B13Name(id);
    return n && !strncmp(n, "ammo-", 5);
}

static void SpxCheck(int qi)
{
    uint64_t h = g_spx_q[qi].h;
    int via = g_spx_q[qi].via;
    g_spx_q[qi].h = 0;
    void *pr = ((void *(*)(uint64_t)) T2R_SYM(kFnEntityFromHandle))(h);
    if (!pr) {
        Xlog("stock pickup: %llx (%s) gone before the check",
             (unsigned long long) h,
             via < SPX_N ? SPX_NAMES[via] : "container");
        return;
    }
    uint64_t a[1] = { h };
    uint64_t ap =
        DcCall(FNV64("get-ammo-process-from-weapon-pickup"), 1, a);
    uint64_t iid = DcCall(FNV64("item-pickup-get-item-id"), 1, a), amt =
        DcCall(FNV64("item-pickup-get-amount"), 1, a);
    uint64_t aiid = 0, aamt = 0;
    int weapon = ap && ap != ~0ULL;
    if (weapon) {
        uint64_t b[1] = { ap };
        aiid = DcCall(FNV64("item-pickup-get-item-id"), 1, b);
        aamt = DcCall(FNV64("item-pickup-get-amount"), 1, b);
    }
    float lb[16];
    const float *l =
        ((const float *(*)(void *, float *))( (uint8_t *)
                                             GetModuleHandleW(NULL) +
                                             0x133db30))( (uint8_t *) pr +
                                                         0x100, lb);
    Xlog("stock pickup: %llx via %s: item %s x%lld, ammo process %llx (%s x%lld) at %.2f %.2f %.2f", (unsigned long long) h, via < SPX_N ? SPX_NAMES[via] : "container probe", B13Name(iid), (long long) amt, (unsigned long long) ap, weapon ? B13Name(aiid) : "-", (long long) aamt, l ? l[0] : 0.0f, l ? l[1] : 0.0f, l ? l[2] : 0.0f);
    if (!weapon)
        return;                 // a plain pickup: it already looks like what it is
    uint64_t item =
        SpxAmmoItem(aiid) ? aiid : SpxAmmoItem(iid) ? iid : 0;
    uint64_t cnt = SpxAmmoItem(aiid) ? aamt : amt;
    if (!item) {
        Xlog("stock pickup: %llx is a weapon pickup without an ammo item - left as it is", (unsigned long long) h);
        return;
    }
    if (!(cnt >= 1 && cnt <= 60))
        cnt = 1;
    if (!l || !KillPosOk(l)) {
        Xlog("stock pickup: %llx has no position - left as it is",
             (unsigned long long) h);
        return;
    }
    float p[3] = { l[0], l[1], l[2] };
    uint64_t k[1] = { h };
    DcCall(FNV64("kill-entity"), 1, k);
    if (((void *(*)(uint64_t)) T2R_SYM(kFnEntityFromHandle))(ap)) {
        uint64_t k2[1] = { ap };
        DcCall(FNV64("kill-entity"), 1, k2);
    }
    uint64_t ret[4] = { 0 };
    BoxSpawnLike(ret, item, p, cnt);
    Xlog("stock pickup: ps50 enemy-gun pickup %llx (%s x%llu) -> the box ammo pickup at %.2f %.2f %.2f -> %llx", (unsigned long long) h, B13Name(item), (unsigned long long) cnt, p[0], p[1], p[2], (unsigned long long) ret[0]);
}

static void SpxTick(void)
{
    if (MatchSubstate() != 6) {
        if (g_spx_live) {
            memset(g_spx_q, 0, sizeof g_spx_q);
            memset(g_spx_cont_h, 0, sizeof g_spx_cont_h);
            g_spx_live = 0;
        }
        return;
    }
    if (SpxOff() || !g_spawnPickupOrig)
        return;
    uint64_t now = GetTickCount64();
    if (!g_spx_live)
        g_spx_live = now;
    for (int i = 0; i < SPX_Q; i++)
        if (g_spx_q[i].h && now >= g_spx_q[i].due)
            SpxCheck(i);
    // the containers of this map (our container layer is off: the engine's own contents are what the player finds)
    if (g_clOff == 1 && now - g_spx_live > 2000
        && now - g_spx_probe_ms >= 1000) {
        g_spx_probe_ms = now;
        uint8_t *lo = LobbyObj();
        int map = lo && !IsBadReadPtr(lo + 0x18, 1) ? lo[0x18] : 255;
        int n = map < CL_NMAPS ? CL_MAP[map].n : 0;
        if (n > CL_MAX)
            n = CL_MAX;
        for (int i = 0; i < n; i++) {
            uint64_t a[1] = { FNV64(CL_MAP[map].c[i].name) };
            uint64_t h =
                DcCall(FNV64("get-openable-spawned-pickup"), 1, a);
            if (!h || h == ~0ULL || h == g_spx_cont_h[i])
                continue;
            g_spx_cont_h[i] = h;
            Xlog("stock pickup: container %s holds pickup %llx",
                 CL_MAP[map].c[i].name, (unsigned long long) h);
            SpxQueue(h, 4);
        }
    }
}
