// weather.c - random blizzard on the winter map (Supermarket), for t2r-ext. Included from t2r-ext.c after immersion.c (uses imm_rs_call / imm_noflag).
//
// The storm picks up with fog, heavier snow, etc, as a random effect (happens a couple of times per game, always
// on snowy maps). SP reference (re/sp_rsz_1103/NOTES.md, storm.txt): SP's blizzard is three task-gated render-settings regions of the patrol set,
//   rr-pat-forest-blizzard-start-1   *pat-rs-pat-forest-blizzard*             prio 300
//   rr-pat-forest-blizzard-height-1  *pat-rs-pat-forest-blizzard-height*      prio 350
//   rr-pat-forest-blizzard-cloth-wind-1 *pat-rs-pat-forest-blizzard-cloth-wind* prio 5
// blended in linearly over ~20 s (fog:distance-start 0 -> -91, fog:color-i 1 -> 1.332, wind 0 -> 1, cloth wind 0 -> 1.5, joint gust 2 -> 20, snow direction
// x 0.4 -> 1.1, bloom 0.035 -> 0). MP never runs the patrol task, so those regions stay gated; here the same symbols are applied with the per-frame
// override-render-settings native at a strength we ramp ourselves (0..1 over WX_RAMP_S, hold, ramp out).
//
// Authority: the HOST decides. At match start it rolls 1..2 storms (first one WX_FIRST_MIN..MAX s in, the next one WX_GAP_MIN..MAX s after the previous one
// ended, hold WX_HOLD_MIN..MAX s) and sends its current storm strength target every second (MSG_WEATHER 0xAC: [u8 ver 1][u8 on][u16 strength x 10000]).
// Clients ramp toward the host's value at the same speed, so a late joiner, a lost packet or a host change only shifts the blend a little.
// Test switches next to the exe: t2r-weather-force.txt (optional float 0..1, default 1) = that storm strength now, on every map, no host needed;
// t2r-no-weather.txt = off. Log prefix "weather: ".
#define MSG_WEATHER 0xAC
#define WX_RAMP_S 20.0f
#define WX_FIRST_MIN 90
#define WX_FIRST_MAX 300
#define WX_GAP_MIN 150
#define WX_GAP_MAX 360
#define WX_HOLD_MIN 60
#define WX_HOLD_MAX 120
static const struct {
    const char *sym;
    int prio;
} WX_LAYERS[] = {
    { "*pat-rs-pat-forest-blizzard*", 300 },
        { "*pat-rs-pat-forest-blizzard-height*", 350 },
        { "*pat-rs-pat-forest-blizzard-cloth-wind*", 5 }
};

static struct {
    float env, host_tgt, dopen, shifted;
    uint64_t host_at;
    int nstorm, planned;
    uint64_t t0, next_on, next_off;
    uint32_t rng;
} g_wx;
static uint32_t WxRand(uint32_t lo, uint32_t hi)
{
    uint32_t x =
        g_wx.rng ? g_wx.rng : (uint32_t) GetTickCount64() * 2654435761u +
        1u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_wx.rng = x;
    return lo + x % (hi - lo + 1);
}

// camera-distance keys of the blizzard symbol (stock values, pat.bin) that move out by the doorway distance indoors (see weather_tick)
#define WX_OPEN_SLEW 12.0f
static const struct {
    const char *key;
    float v;
} WX_DIST[] = {
    { "new-volumetrics:fog-start-distance", 3.0f },
        { "new-volumetrics:layer-fog-0:fog-horizontal-end-dist", 50.0f },
    { "new-volumetrics:sky-fog-horizontal-start-dist", 15.0f },
        { "new-volumetrics:sky-fog-horizontal-end-dist", 400.0f },
    { "fog:distance-start", -91.0f }, { "post:dof-focus-plane-dist",
                                       14.0f }, { "misc:far-cull-distance",
                                                 100.0f }
};

static void WxShift(float d)
{                               // writes the symbol's values (persistent): stock + d, only when d changed
    if (fabsf(d - g_wx.shifted) < 0.05f
        && (d > 0.0f || g_wx.shifted == 0.0f))
        return;
    uint64_t fn = FNV64("change-render-setting-value-float"), g =
        FNV64(WX_LAYERS[0].sym);
    for (size_t i = 0; i < sizeof WX_DIST / sizeof *WX_DIST; i++) {
        float v = WX_DIST[i].v + d;
        uint32_t vb;
        memcpy(&vb, &v, 4);
        uint64_t a[3] = { g, FNV64(WX_DIST[i].key), vb };
        DcCall(fn, 3, a);
    }
    g_wx.shifted = d;
}

static int WxIsHost(void)
{
    return *(volatile uint8_t *) T2R_SYM(kRoleByte) != 2;
}

static int WxWinterMap(void)
{
    uint8_t *lc = LobbyObj();
    return lc && !IsBadReadPtr(lc, 0x20)
        && MpsnowWinter(MfMapName(lc[0x18]));
}

static int WxRecv(uint32_t slot, const uint8_t *buf, uint64_t len)
{
    if (!buf || len < 4 || buf[0] != 1)
        return 0;
    uint16_t s;
    memcpy(&s, buf + 2, 2);
    float v = buf[1] ? (float) s / 10000.0f : 0.0f;
    if (v > 1.0f)
        v = 1.0f;
    static int nl;
    if ((v > 0.0f) != (g_wx.host_tgt > 0.0f) && nl < 20) {
        nl++;
        Xlog("weather: host (slot %u) storm %s", slot,
             v > 0.0f ? "ON" : "off");
    }
    g_wx.host_tgt = v;
    g_wx.host_at = GetTickCount64();
    return 0;
}

static void WxSend(float tgt)
{
    if (!g_sendTramp)
        return;
    uint8_t m[4];
    uint16_t s = (uint16_t) (tgt * 10000.0f + 0.5f);
    m[0] = 1;
    m[1] = tgt > 0.0f;
    memcpy(m + 2, &s, 2);
    g_sendTramp(MSG_WEATHER, m, 4, 0xff);
}

// ---- audio (there are no weather sounds, like the actual wind outside; re/maps/jackson-market.md "Wind audio")
// SP's outdoor wind is not scripted: pat-const-audio's REGION_TREE holds the beds as positional regions (env-pat-ride-middle-ext, env-pat-ride-blizzard-ext,
// the amb-pat-ride-middle-wind-* interior bleeds, ...) whose tag dictionary carries start-task / end-task; the patrol task graph switches them on when the story
// reaches the blizzard. MP never runs that graph, so they stay off. Static evidence (exe 1.6.10721, RVAs): the region builder 0x1bc8490 reads the tags
// start-task, start-task-subnode, end-task, end-task-subnode of the record (+0x50 = tag dictionary, the pak's own record) and stores the node's subnode
// indices at +0x72 / +0x74 (0xffff = none); 0x1bc9dc0 is the gate (0xffff either -> ungated path, else "start <= the task's current subnode index <= end"), 0x1bc9d60
// wraps it (clears / sets bit 3 of the flag word at +0x70) and the sound-env / emitter region query 0x1d27a30 calls it for EVERY region on EVERY query
// (0x1d27daa; a region with bit 0 of +0x70 set is skipped before it) - so the gating is re-evaluated live, nothing is cached. We hook 0x1bc9dc0: a bed region of
// the list below answers "active" in addition to the engine's own answer while the storm (g_wx.env) is above its threshold; everything else, and calm
// weather, is the engine's answer unchanged (SP's own gating). Blends between beds are the engine's region blends. Log prefix "wx audio:".
//   t2r-no-weather-audio.txt   hook not installed / beds stay SP-gated (= off in MP)
static volatile int g_wxa_live, g_wxa_on[2];
static volatile LONG g_wxaHit[2];
static uint8_t(*g_wxa_orig) (uint8_t *);
static int g_wxaHooked = -1;
static struct {
    uint8_t *p;
    uint8_t c;
} g_wxa_cache[1024];
static int WxaClass(uint8_t *rg)
{                               // 0 = not a storm bed, 1 = middle (wind starts), 2 = blizzard (full storm)
    static const char *const MID[] =
        { "env-pat-ride-middle-ext", "amb-pat-ride-middle-wind-bleed-",
"amb-pat-ride-middle-wind-and-tree-bleed-",
        "amb-pat-ride-middle-wind-whistle-int-",
            "emt-pat-ride-middle-pine-tree-movement-lp-",
            "emt-quad-pat-ride-middle-pine-tree-movement-quad-lp-",
        "emt-pat-wood-window-rattle-cone-",
            "emt-pat-metal-fence-rattle-lp-",
            "emt-pat-razor-wire-movement-lp-",
            "emt-pat-wood-structure-ticks-lp-",
        "emt-pat-metal-vessel-ticks-lp-",
            "emt-pat-metal-thin-office-ceiling-a-interior-lp-"
    };
    static const char *const BLZ[] = { "env-pat-ride-blizzard-ext" };
    unsigned h = (unsigned) ((uintptr_t) rg >> 4) & 1023;
    if (g_wxa_cache[h].p == rg)
        return g_wxa_cache[h].c;
    int c = 0;
    const char *nm = NULL;
    if (!IsBadReadPtr(rg, 8)) {
        nm = *(const char *const *) rg;
        if (nm && IsBadReadPtr(nm, 64))
            nm = NULL;
    }
    if (nm) {
        for (size_t i = 0; i < sizeof MID / sizeof *MID && !c; i++)
            if (!strncmp(nm, MID[i], strlen(MID[i])))
                c = 1;
        for (size_t i = 0; i < sizeof BLZ / sizeof *BLZ && !c; i++)
            if (!strncmp(nm, BLZ[i], strlen(BLZ[i])))
                c = 2;
    }
    g_wxa_cache[h].p = rg;
    g_wxa_cache[h].c = (uint8_t) c;
    return c;
}

static uint8_t WxaGateHook(uint8_t *rg)
{                               // exe 0x1bc9dc0: region_task_gate_active(region) -> al
    uint8_t r = (uint8_t) g_wxa_orig(rg);
    if (r || !g_wxa_live)
        return r;
    int c = WxaClass(rg);
    if (c && g_wxa_on[c - 1]) {
        InterlockedIncrement(&g_wxaHit[c - 1]);
        return 1;
    }
    return r;
}

// Bed lifecycle probe (exe 1.6.10721): the region aux spawner 0x1bc9ab0 (rcx = aux block, rdx = region record; for aux type SfxProcess it builds the process with
// ctor 0x1dd7460 and returns it; called from the region OnEnter path 0x1e16430) and the aux stop 0x1bc8420 (rcx = aux block; sets the process' kill byte +0x32 through
// 0x1413414c0; called from the region OnExit path 0x1e16300..0x1e1637a). So a positional bed is spawned at full gain when the listener enters its region and killed when it
// leaves - the engine has no gain ramp of its own there (interior ambience is the ENVIRONMENT's loop: audio/environment.bin env-pat-supermarket-*-int [0x010] sfx-amb-pat-ride-
// middle-*-int-lp, switched by the sound-env query 0x1d27a30 with its portal fade; env-pat-ride-blizzard-ext carries mix-pat-blizzard, fade 3.5 s in / 1.5 s out). These two hooks
// only LOG every start / stop of a region process with the region name, camera, storm strength and the interior test, so the next run says which region toggles at each cut.
// "wx audio: bed START|STOP <region> ..." (first 300). t2r-no-weather-audio.txt = no hooks.
static void *(*g_wxa_spawn_orig)(uint8_t *, uint8_t *);
static void (*g_wxa_stop_orig)(uint8_t *, uint64_t);
static volatile LONG g_wxaPn;
static void WxaProbeLog(const char *what, const char *nm, uint8_t *aux)
{
    LONG n = InterlockedIncrement(&g_wxaPn);
    if (n > 300)
        return;
    Xlog("wx audio: bed %s %s aux %p | camera %.1f %.1f %.1f interior-test %d indoor-weight %.2f storm %.2f beds %d/%d frame %llu", what, nm ? nm : "?", (void *) aux, (double) g_immCam[0], (double) g_immCam[1], (double) g_immCam[2], g_imm_cam_in, (double) g_immIndoorW, (double) g_wx.env, g_wxa_on[0], g_wxa_on[1], (unsigned long long) g_frame);
}

static const char *WxaName(uint8_t *rg)
{
    const char *nm = NULL;
    if (rg && !IsBadReadPtr(rg, 8)) {
        nm = *(const char *const *) rg;
        if (nm && IsBadReadPtr(nm, 64))
            nm = NULL;
    }
    return nm;
}

// ---- bed crossfade (the wind hard-cuts at the supermarket door, in both directions: bed START / STOP pairs of the gusty / tree-movement / roof-cone beds
// at the door line, camera x -503 / -497). The engine starts a region bed at full gain and kills it with no ramp (see above), so:
//  STOP  - the process is told what the script message "fade-out-and-stop" (SID 0x1dcbcdbf989d638d, handler 0x141dd9428) tells it: AudioManager voice command 0x141aa5ba0
//          (mgr, voice, mode 6, time s, value 0) = ramp the voice to silence over `time` and stop it, flag bit 1 of +0x180 set as the handler does; the aux stop (kill byte) runs
//          after the ramp (time + 0.15 s) from wxa_tick. The process ends itself when its voice is gone (update 0x141dd8816 -> vtable +0x1e8).
//  START - the SfxProcess update (0x141dd83a5) sends command mode 0xa (time = float +0x490, default -1 = none) right after the voice starts: +0x490 is set to the fade-in time on
//          the fresh process (resolved from the aux's handle {+0x18 slot, +0x20 id}); a process that already started gets the same command directly.
// Storm beds (class 1 / 2 above) fade over WXA_STORM_FADE s while the storm is ramping (they follow the snow ramp), every other case WXA_FADE s. Opt-out t2r-no-bed-fade.txt.
// Lines: "wx audio: bed fade-out|fade-in ..." (first 120).
#define WXA_FADE 1.5f
#define WXA_STORM_FADE 6.0f
static struct {
    uint8_t *aux;
    uint64_t a2, kill_at;
} g_wxa_pend[64];
static struct {
    uint8_t *aux;
    uint64_t t0;
    int tries;
    float fade;
} g_wxa_in[64];
static struct {
    uint8_t *aux;
    char nm[96];
    int cls;
} g_wxa_nm[128];
static int g_wxaNmi;
static volatile LONG g_wxaFl;
static int WxaFadeOff(void)
{
    static uint64_t a;
    static int v;
    return ImmNoflag("t2r-no-bed-fade.txt", &a, &v);
}

static uint8_t *WxaProc(uint8_t *aux)
{                               // the aux's SfxProcess (handle {slot, id}), NULL if not (yet) live
    if (!aux || IsBadReadPtr(aux, 0x28)
        || *(uint64_t *) aux != 0x01386ce5b3a92451ull)
        return NULL;
    uint8_t *h = *(uint8_t **) (aux + 0x18);
    uint32_t id = *(uint32_t *) (aux + 0x20);
    if (!h || !id || IsBadReadPtr(h, 16))
        return NULL;
    uint8_t *pr = *(uint8_t **) (h + 8);
    if (!pr || IsBadReadPtr(pr, 0x4a0) || *(uint32_t *) (pr + 0x94) != id)
        return NULL;
    return pr;
}

static void *WxaMgr(void)
{
    return ((void *(*)(int, int))( (uint8_t *) GetModuleHandleW(NULL) +
                                  0x1270a80))(0xb, 0);
}

static void WxaVoiceCmd(uint32_t voice, uint32_t mode, float t)
{                               // 0x141aa5ba0 (mgr, voice, mode, time, value)
    void *m = WxaMgr();
    if (!m || !voice)
        return;
    ((void (*)(void *, uint32_t, uint32_t, float, float))
     ((uint8_t *) GetModuleHandleW(NULL) + 0x1aa5ba0)) (m, voice, mode, t,
                                                        0.0f);
}

static int WxaNmFind(uint8_t *aux)
{
    for (int i = 0; i < 128; i++)
        if (g_wxa_nm[i].aux == aux)
            return i;
    return -1;
}

static float WxaFadeS(int cls, int stopping)
{
    float e = g_wx.env;
    if (cls && ((!stopping && e < 0.9f) || (stopping && e < 0.9f)))
        return WXA_STORM_FADE;
    return WXA_FADE;
}

static void WxaFlushOne(int i)
{
    uint8_t *aux = g_wxa_pend[i].aux;
    g_wxa_pend[i].aux = NULL;
    if (WxaProc(aux))
        g_wxa_stop_orig(aux, g_wxa_pend[i].a2); // (a level that unloaded meanwhile: the aux is gone, nothing to kill)
}

static void WxaTick(void)
{                               // every frame, also outside matches: due kills, fade-in of fresh beds
    uint64_t now = GetTickCount64();
    for (int i = 0; i < 64; i++)
        if (g_wxa_pend[i].aux
            && (!g_wxa_live || now >= g_wxa_pend[i].kill_at))
            WxaFlushOne(i);
    for (int i = 0; i < 64; i++) {
        if (!g_wxa_in[i].aux)
            continue;
        uint8_t *o = WxaProc(g_wxa_in[i].aux);
        if (!o) {
            if (++g_wxa_in[i].tries > 40 || !g_wxa_live)
                g_wxa_in[i].aux = NULL;
            continue;
        }
        uint32_t voice = *(uint32_t *) (o + 0x1d0);
        float f = g_wxa_in[i].fade;
        int how = 0;
        if (!voice && !(*(uint64_t *) (o + 0x180) & 1)) {
            *(float *) (o + 0x490) = f;
            how = 1;
        }                       // not started: the update sends the command itself
        else if (voice && now - g_wxa_in[i].t0 < 400) {
            WxaVoiceCmd(voice, 0xa, f);
            how = 2;
        }                       // just started: same command by hand
        if (how && InterlockedIncrement(&g_wxaFl) <= 120) {
            int k = WxaNmFind(g_wxa_in[i].aux);
            Xlog("wx audio: bed fade-in %.1f s %s (%s) %s storm %.2f",
                 (double) f, k >= 0 ? g_wxa_nm[k].nm : "?",
                 how == 1 ? "field +0x490" : "voice cmd 0xa",
                 (void *) g_wxa_in[i].aux, (double) g_wx.env);
        }
        g_wxa_in[i].aux = NULL;
    }
}

static void *WxaSpawnHook(uint8_t *aux, uint8_t *rg)
{
    int mine = g_wxa_live && aux && !IsBadReadPtr(aux, 16)
        && *(uint64_t *) aux == 0x01386ce5b3a92451ull;
    if (mine) {
        for (int i = 0; i < 64; i++)
            if (g_wxa_pend[i].aux == aux)
                WxaFlushOne(i);       // re-entered while fading out: the old bed goes now
        WxaProbeLog("START", WxaName(rg), aux);
    }
    void *r = g_wxa_spawn_orig(aux, rg);
    if (mine) {
        const char *nm = WxaName(rg);
        int cls = WxaClass(rg);
        int k = WxaNmFind(aux);
        if (k < 0)
            k = (g_wxaNmi++) & 127;
        g_wxa_nm[k].aux = aux;
        g_wxa_nm[k].cls = cls;
        snprintf(g_wxa_nm[k].nm, sizeof g_wxa_nm[k].nm, "%s",
                 nm ? nm : "?");
        if (!WxaFadeOff())
            for (int i = 0; i < 64; i++)
                if (!g_wxa_in[i].aux) {
                    g_wxa_in[i].aux = aux;
                    g_wxa_in[i].t0 = GetTickCount64();
                    g_wxa_in[i].tries = 0;
                    g_wxa_in[i].fade = WxaFadeS(cls, 0);
                    break;
                }
    }
    return r;
}

static void WxaStopHook(uint8_t *aux, uint64_t a2)
{
    if (g_wxa_live && aux && !IsBadReadPtr(aux, 0x28)
        && *(uint64_t *) aux == 0x01386ce5b3a92451ull) {
        const char *nm = NULL;
        int k = WxaNmFind(aux), cls = 0;
        uint8_t *pr = WxaProc(aux);
        if (k >= 0) {
            nm = g_wxa_nm[k].nm;
            cls = g_wxa_nm[k].cls;
        } else if (pr)
            nm = WxaName(*(uint8_t **) (pr + 0xa8));
        WxaProbeLog("STOP", nm, aux);
        for (int i = 0; i < 64; i++)
            if (g_wxa_in[i].aux == aux)
                g_wxa_in[i].aux = NULL;
        uint32_t voice = pr ? *(uint32_t *) (pr + 0x1d0) : 0;
        if (pr && voice && !(*(uint64_t *) (pr + 0x180) & 2)
            && !WxaFadeOff()) {
            for (int i = 0; i < 64; i++)
                if (!g_wxa_pend[i].aux) {
                    float f = WxaFadeS(cls, 1);
                    WxaVoiceCmd(voice, 6, f);
                    *(uint64_t *) (pr + 0x180) |= 2;
                    g_wxa_pend[i].aux = aux;
                    g_wxa_pend[i].a2 = a2;
                    g_wxa_pend[i].kill_at =
                        GetTickCount64() + (uint64_t) (f * 1000.0f) + 150;
                    if (InterlockedIncrement(&g_wxaFl) <= 120)
                        Xlog("wx audio: bed fade-out %.1f s %s voice %u %s storm %.2f", (double) f, nm ? nm : "?", voice, (void *) aux, (double) g_wx.env);
                    return;
                }
        }
    }
    g_wxa_stop_orig(aux, a2);
}

static void WxaInstall(void)
{
    if (g_wxaHooked >= 0)
        return;
    static const uint8_t pro[14] = { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56 };      // mov [rsp+10h],rbx; mov [rsp+18h],rbp; push rsi; push rdi; push r14
    uint8_t *f = (uint8_t *) GetModuleHandleW(NULL) + 0x1bc9dc0;
    if (memcmp(f, pro, sizeof pro)) {
        HOOK_MISS(f, sizeof pro,
                  "wx audio: gate 0x141bc9dc0 prologue differs - storm wind beds stay SP-gated (off)");
        g_wxaHooked = 0;
        return;
    }
    g_wxaHooked =
        XbHookExe(0x1bc9dc0, pro, sizeof pro, (void *) WxaGateHook,
                    "wx audio gate", (void **) &g_wxa_orig) ? 1 : 0;
    Xlog("wx audio: region gate hook %s",
         g_wxaHooked ? "installed (exe 0x1bc9dc0)" : "NOT installed");
    if (g_wxaHooked == 1) {
        static const uint8_t sp[16] =
            { 0x48, 0x89, 0x5c, 0x24, 0x18, 0x55, 0x56, 0x57, 0x48, 0x8d,
0xac, 0x24, 0x80, 0xfc, 0xff, 0xff };
        static const uint8_t st[16] =
            { 0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x48, 0xb8, 0x64, 0xc9,
0x10, 0x17, 0x8d, 0x60, 0xb3, 0x05 };
        int a =
            XbHookExe(0x1bc9ab0, sp, 16, (void *) WxaSpawnHook,
                        "wx audio region aux spawn",
                        (void **) &g_wxa_spawn_orig) != NULL;
        int b =
            XbHookExe(0x1bc8420, st, 16, (void *) WxaStopHook,
                        "wx audio region aux stop",
                        (void **) &g_wxa_stop_orig) != NULL;
        Xlog("wx audio: bed lifecycle probe hooks spawn 0x1bc9ab0 %s, stop 0x1bc8420 %s", a ? "installed" : "NOT installed", b ? "installed" : "NOT installed");
    }
}

// Storm beds follow the snow (the storm wind used to cut in instantly and start ~10 s AFTER the heavy snow): the old switch was env >= 0.10 (middle) / 0.60 (blizzard)
// on the linear 20 s strength = 2 s / 12 s into the ramp, while the snow shows from the first seconds. Now the beds switch on as soon as the ramp starts (0.03 / 0.06, rising) and
// off when the strength has fallen to 0.30 (falling), each with the 6 s bed fade (wxa_fade_s) = the wind swells over the first third of the ramp and is gone as the ramp ends.
static void WxaUpdate(float env)
{                               // storm strength -> bed switches, log on change
    static int nl;
    static float prev = -1.0f;
    static const float ON[2] = { 0.03f, 0.06f }, OFF_FALL[2] =
        { 0.30f, 0.30f };
    static const char *const NM[2] = { "middle wind", "blizzard" };
    int falling = prev >= 0.0f && env < prev - 1e-5f;
    prev = env;
    for (int i = 0; i < 2; i++) {
        int was = g_wxa_on[i], now_on = was ? !(falling
                                                && env <= OFF_FALL[i])
            && env > 0.01f : (env >= ON[i] && !falling);
        if (now_on != was) {
            g_wxa_on[i] = now_on;
            if (nl < 60) {
                nl++;
                Xlog("wx audio: bed %s %s (storm %.2f, %s, gate answers forced %ld/%ld so far)", NM[i], now_on ? "on" : "off", (double) env, falling ? "falling" : "rising", (long) g_wxaHit[0], (long) g_wxaHit[1]);
            }
        }
    }
}

static void WeatherTick(void)
{
    static uint64_t last_t, last_send, nf_a, nfa_a;
    static int nf_v, nfa_v, nl, nl_d;
    uint64_t now = GetTickCount64();
    float dt = last_t
        && now - last_t < 500 ? (float) (now - last_t) / 1000.0f : 0.016f;
    last_t = now;
    int in_match = MatchSubstate() == 6;
    WxaTick();                 // (before g_wxa_live is cleared: the due fade-out kills of the last frame)
    g_wxa_live = 0;
    if (!in_match || ImmNoflag("t2r-no-weather.txt", &nf_a, &nf_v)) {
        g_wxa_on[0] = g_wxa_on[1] = 0;
        if (g_wx.planned && nl < 40) {
            nl++;
            Xlog("weather: match over / off - storms reset (%d played), storm strength was %.2f%s", g_wx.nstorm, (double) g_wx.env, g_wx.env > 0.02f ? " - CUT OFF, not faded (match state left 6 / t2r-no-weather.txt)" : "");
        }
        if (in_match && g_wx.shifted != 0.0f)
            WxShift(0.0f);     // flag off mid-match: stock distances back (a match end reloads the levels anyway)
        memset(&g_wx, 0, sizeof g_wx);
        g_immStorm = 0.0f;
        return;
    }
    float tgt = 0.0f;
    const char *why = NULL;
    FILE *ff = ExeSideOpen("t2r-weather-force.txt", "r");
    if (ff) {
        float f = 1.0f;
        if (fscanf(ff, "%f", &f) != 1 || f < 0.0f || f > 1.0f)
            f = 1.0f;
        fclose(ff);
        tgt = f;
        why = "t2r-weather-force.txt";
    } else if (!WxWinterMap())
        return;
    else if (WxIsHost()) {
        if (!g_wx.planned) {
            g_wx.planned = 1;
            g_wx.t0 = now;
            g_wx.nstorm = 0;
            g_wx.next_on =
                now + 1000ull * WxRand(WX_FIRST_MIN, WX_FIRST_MAX);
            g_wx.next_off =
                g_wx.next_on + 1000ull * (uint64_t) (WX_RAMP_S +
                                                     WxRand(WX_HOLD_MIN,
                                                             WX_HOLD_MAX));
            if (nl < 40) {
                nl++;
                Xlog("weather: host plans storm 1 at +%llu s (until +%llu s)", (unsigned long long) ((g_wx.next_on - now) / 1000), (unsigned long long) ((g_wx.next_off - now) / 1000));
            }
        }
        if (g_wx.next_on && now >= g_wx.next_on && now < g_wx.next_off)
            tgt = 1.0f;
        else if (g_wx.next_on && now >= g_wx.next_off) {        // storm over: maybe one more (max 2 per match)
            g_wx.nstorm++;
            if (g_wx.nstorm < 2 && WxRand(0, 99) < 70) {
                g_wx.next_on =
                    now + 1000ull * (uint64_t) (WX_RAMP_S +
                                                WxRand(WX_GAP_MIN,
                                                        WX_GAP_MAX));
                g_wx.next_off =
                    g_wx.next_on + 1000ull * (uint64_t) (WX_RAMP_S +
                                                         WxRand
                                                         (WX_HOLD_MIN,
                                                          WX_HOLD_MAX));
                if (nl < 40) {
                    nl++;
                    Xlog("weather: host plans storm %d at +%llu s",
                         g_wx.nstorm + 1,
                         (unsigned long long) ((g_wx.next_on - now) /
                                               1000));
                }
            } else
                g_wx.next_on = 0;
        }
        why = "host";
        if (now - last_send >= 1000) {
            last_send = now;
            WxSend(tgt);
        }
    } else {
        tgt = g_wx.host_at && now - g_wx.host_at < 10000 ? g_wx.host_tgt : 0.0f;        // host silent 10 s: clear sky
        why = "host msg";
    }
    float step = dt / WX_RAMP_S;
    float prev = g_wx.env;
    g_wx.env =
        g_wx.env < tgt ? (g_wx.env + step >
                          tgt ? tgt : g_wx.env + step) : (g_wx.env - step <
                                                          tgt ? tgt : g_wx.
                                                          env - step);
    if (((prev <= 0.0f) != (g_wx.env <= 0.0f)
         || (prev < 1.0f) != (g_wx.env < 1.0f)) && nl < 40) {
        nl++;
        Xlog("weather: storm strength %.2f -> %.2f (target %.2f, %s)",
             (double) prev, (double) g_wx.env, (double) tgt, why);
    }
    if (!ImmNoflag("t2r-no-weather-audio.txt", &nfa_a, &nfa_v)) {      // storm wind beds (see wxa_gate_hook)
        WxaInstall();
        if (g_wxaHooked == 1) {
            g_wxa_live = 1;
            WxaUpdate(g_wx.env);
        }
    }
    // fade shape (the storm fog etc. ended abruptly instead of fading out): the 20 s ramp itself is linear and symmetric (ramp up
    // and ramp down both 1193 frames), but the engine blends every key LINEARLY in the strength while fog / visibility is not linear in its distances, and a linear ramp
    // has full slope at both ends: the storm snaps in / out of the last few percent. All layers and the door sun take the eased strength (smoothstep: zero slope at both
    // ends, 50 % at the middle) so the first and the last seconds are gentle and the ramps stay symmetric; t2r-weather-linear.txt = the old linear strength.
    static uint64_t lin_a;
    static int lin_v;
    float vis = g_wx.env;
    if (!ImmNoflag("t2r-weather-linear.txt", &lin_a, &lin_v))
        vis = g_wx.env * g_wx.env * (3.0f - 2.0f * g_wx.env);
    g_immStorm = vis;          // immersion.c door sun: the door rooms' outdoor values follow the storm
    // Tree shake (trees sway the same, but when the storm comes in / down they start shaking). The blizzard symbol rotates the gust direction
    // (0,1) -> (1,0) (re/sp_rsz_1103 clear.txt / storm.txt) and the engine blends it linearly in the strength, so for the whole 20 s ramp every joint's wind phase (position x
    // direction; the map sits at x ~ -500) sweeps: a fast phase scroll = shake, on top of the time-scale jump the speed hold already removed. The direction is held at the
    // storm's final value (1,0) on the blizzard, cloudy, base and zone symbols from the match start, so nothing blends: the gust amplitude (2 -> 20) still ramps = sway only.
    // Opt-out t2r-weather-dirpin.txt. Log "wx wind: direction pinned ...".
    {
        static uint64_t dp_a, dp_t;
        static int dp_v, dp_n;
        uint64_t tn = GetTickCount64();
        if (tn - dp_t >= 2000
            && !ImmNoflag("t2r-weather-dirpin.txt", &dp_a, &dp_v)) {
            dp_t = tn;          // (imm_noflag = the file EXISTS: this was once inverted, so the pin ran only WITH the opt-out file = never; direction-x swept 0 -> 1 over both ramps)
            uint64_t fn = FNV64("change-render-setting-value-float"), kx =
                FNV64("wind:joint-wind-direction-x"), kz =
                FNV64("wind:joint-wind-direction-z");
            float one = 1.0f, zero = 0.0f;
            uint32_t ob, zb;
            memcpy(&ob, &one, 4);
            memcpy(&zb, &zero, 4);
            static const char *const SY[] =
                { "*pat-rs-pat-forest-blizzard*", "*pat-rs-pat-cloudy*",
"*pat-rs-pat*" };
            int n = 0;
            for (size_t i = 0; i < sizeof SY / sizeof *SY; i++) {
                uint64_t g = FNV64(SY[i]), a[3] = { g, kx, ob }, b[3] =
                    { g, kz, zb };
                DcCall(fn, 3, a);
                DcCall(fn, 3, b);
                n++;
            }
            for (int i = 0; i < SPL_NHOLD; i++) {
                if (g_splHold[i].map != g_immCamK)
                    continue;
                uint64_t g = FNV64(g_splHold[i].sym), a[3] =
                    { g, kx, ob }, b[3] = { g, kz, zb };
                DcCall(fn, 3, a);
                DcCall(fn, 3, b);
                n++;
            }
            if (dp_n < 3) {
                dp_n++;
                Xlog("wx wind: direction pinned to (1,0) on %d symbols (storm %.2f) - no direction blend during the ramp", n, (double) g_wx.env);
            }
        }
    }
    if (g_wx.env <= 0.001f) {
        if (g_wx.shifted != 0.0f)
            WxShift(0.0f);
        g_wx.dopen = 0.0f;
        return;
    }
    // dev t2r-weather-layers.txt (bit mask, 1 = blizzard, 2 = height, 4 = cloth wind; default 7): which layer carries which keys (RSZ lines) -
    // trees shook violently for ~15 s of the ramp, then the right harsh sway (the wind keys at partial strength)
    int mask = 7;
    {
        static uint64_t mk;
        static int mv = 7;
        uint64_t t = GetTickCount64();
        if (t - mk > 2000) {
            mk = t;
            FILE *mf = ExeSideOpen("t2r-weather-layers.txt", "r");
            mv = 7;
            if (mf) {
                if (fscanf(mf, "%d", &mv) != 1)
                    mv = 7;
                fclose(mf);
            }
        }
        mask = mv;
    }
    // Indoors (the weather did not play when inside and only switched on when walking out of the zone, which was very jarring).
    // The earlier fix for the indoors getting a big fog scaled the blizzard + height layers by (1 - indoor weight), camera-global: from inside, the yard seen through
    // the doors was clear and the whole storm snapped on within 1 s of crossing the interior volume edge (log: "sp interior out: camera -501.2 19.3 257.9", the
    // front entrance). The storm's fog is camera-distance based (pat.bin *pat-rs-pat-forest-blizzard*: new-volumetrics fog-start-distance 3, layer-fog-0
    // horizontal-end 50, sky-fog horizontal start 15 / end 400, fog:distance-start -91, DOF focus 14, far cull 100), so now all three layers run at full storm
    // strength everywhere and, while the camera is inside an interior volume, those distances move out by d = the camera's distance to the nearest opened
    // doorway (sp_lighting.h g_spl_open, sp_maps.json sp_lighting.openings): the outside seen through that doorway is fogged exactly as for a camera standing in
    // it, the room between camera and doorway gets no storm volumetric fog, and d is 0 at the doorway = no pop. d is slewed (WX_OPEN_SLEW m/s) so a camera that
    // leaves the volume anywhere else does not jump. Snowfall stays the engine's: rain:generate-rain-occluders 1 (roof rain blockers) and SP's prio-1000
    // *pat-rs-pat-interior-snow* inside the store, exactly as for the clear-weather flurries that already run indoors.
    // The courtyard doors seen from inside showed no courtyard weather: the openings are every gap of the interior walls (boundary scan,
    // sp_maps.json openings_scan) - the hunting store's courtyard windows were missing, so from there d was the distance to its one door (up to 16 m: the courtyard
    // seen through a window 1 m away was fogged as from 16 m further out). The door rooms' sun / fog-sun keys follow the storm too (immersion.c door sun, g_imm_storm).
    // Opt-outs (flag files next to the exe): t2r-weather-gate.txt = the earlier gating (storm x (1 - indoor)); t2r-weather-indoor.txt = full storm indoors, no shift.
    static uint64_t ind_nf_a, gate_nf_a;
    static int ind_nf_v, gate_nf_v;
    int full_in =
        ImmNoflag("t2r-weather-indoor.txt", &ind_nf_a, &ind_nf_v), gate =
        !full_in
        && ImmNoflag("t2r-weather-gate.txt", &gate_nf_a, &gate_nf_v);
    float ind = gate ? g_immIndoorW : 0.0f;
    float d = 0.0f;
    if (!full_in && !gate && g_imm_cam_in && g_immCamK >= 0
        && now - g_immCamT < 500) {
        d = 1e9f;
        for (int i = 0; i < SPL_NOPEN; i++) {
            const SplOpen *o = &g_splOpen[i];
            if (o->map != g_immCamK)
                continue;
            float q = 0.0f;
            for (int a = 0; a < 3; a++) {
                float v = g_immCam[a], lo = o->box[2 * a], hi =
                    o->box[2 * a + 1], e =
                    v < lo ? lo - v : v > hi ? v - hi : 0.0f;
                q += e * e;
            }
            if (q < d)
                d = q;
        }
        d = d < 1e8f ? sqrtf(d) : 0.0f;
    }
    if (g_imm_cam_jump || dt > 0.25f)
        g_wx.dopen = d;
    else {
        float m = WX_OPEN_SLEW * dt;
        g_wx.dopen =
            d > g_wx.dopen + m ? g_wx.dopen + m : d <
            g_wx.dopen - m ? g_wx.dopen - m : d;
    }
    WxShift(g_wx.dopen);
    if (g_wx.dopen > 0.5f && !(nl_d & 1) && nl < 40) {
        nl++;
        nl_d |= 1;
        Xlog("weather: indoors - storm fog distances moved out %.1f m (nearest doorway; camera %.1f %.1f %.1f)", (double) g_wx.dopen, (double) g_immCam[0], (double) g_immCam[1], (double) g_immCam[2]);
    }
    if (g_wx.dopen <= 0.5f && (nl_d & 1))
        nl_d &= ~1;
    for (size_t i = 0; i < sizeof WX_LAYERS / sizeof *WX_LAYERS; i++)
        if (mask & (1 << i))
            ImmRsCall(FNV64(WX_LAYERS[i].sym), WX_LAYERS[i].prio,
                        i < 2 ? vis * (1.0f - ind) : vis);
    // Fog thins within 1-2 frames at ~11.7 s and snaps back at ~12.25 s while the storm strength is constant: every change of the camera-dependent
    // inputs of the storm (interior test, doorway distance shift) is logged with camera, frame and strengths so the snap can be matched to one of them ("wx fog:")
    {
        static int li = -1, nlf;
        static float ld = -1.0f;
        int cin = g_imm_cam_in != 0;
        if ((cin != li || fabsf(g_wx.dopen - ld) > 1.0f) && nlf < 400) {
            nlf++;
            Xlog("wx fog: frame %llu camera %.1f %.1f %.1f interior-test %d doorway-shift %.2f m (target %.2f) storm %.2f shown %.2f indoor-weight %.2f", (unsigned long long) g_frame, (double) g_immCam[0], (double) g_immCam[1], (double) g_immCam[2], cin, (double) g_wx.dopen, (double) d, (double) g_wx.env, (double) vis, (double) g_immIndoorW);
            li = cin;
            ld = g_wx.dopen;
        }
    }
    // in-game check of the wind keys (storm sway): the live values every 3 s while a storm runs, "wx wind: ..."; no writes
    {
        static uint64_t wl;
        if (now - wl >= 3000) {
            wl = now;
            static const char *const WK[] =
                { "wind:wind-intensity-multiplier",
"wind:joint-wind-gust-intensity", "wind:joint-wind-direction-x",
"wind:joint-wind-direction-z",
                "wind:joint-wind-speed", "wind:joint-wind-flutter-speed",
                    "wind:cloth-wind-intensity-multiplier",
                    "wind:joint-wind-intensity",
                    "wind:joint-wind-flutter-intensity",
                    "wind:joint-wind-turbulence",
                    "wind:joint-wind-gust-coverage"
            };
            char ln[600];
            int q = 0;
            for (int i = 0; i < 11; i++) {
                uint64_t a = FNV64(WK[i]), r =
                    DcCall(FNV64("get-render-setting-value-float"), 1,
                            &a);
                uint32_t u = (uint32_t) r;
                float f;
                memcpy(&f, &u, 4);
                q += snprintf(ln + q, sizeof ln - q, " %s=%.2f", WK[i] + 5,
                              r == ~0ULL ? -1.0 : (double) f);
            }
            static int nw;
            if (nw < 80) {
                nw++;
                Xlog("wx wind: storm %.2f (shown %.2f)%s",
                     (double) g_wx.env, (double) vis, ln);
            }
        }
    }
}
