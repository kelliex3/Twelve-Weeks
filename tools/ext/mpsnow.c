// mpsnow.c - snow accumulation on the MP characters of the winter map (prototype; like SP winter Jackson (Ellie / Dina)).
// Included from t2r-ext.c after bloodbuild.c; mpsnow_tick() is called from the main tick.
//
// What the build ships (tools/features/mp_snow.py, T2R_MP_SNOW=1 builds only, mp_snow_built.h tells us): the snow garments' materials (look paks
// whose garment techniques are the snow-capable wlf-patch one, tools/models/mp_snow_pak.py) and the g_snowStrength rows of SP-only material-param
// tables REUSED for them (each row re-pointed at one garment's material sid + g_snowStrength04, value 1.0; mp_snow_built.h lists the tables). SP's own snow script (ss-pat-chalet-ellie-snow, state set-snow) drives its table with
// wait-set-table-shader-material-values-param(*patrol-ellie-snow*, 0.0, 0.3, 0.1, 1); the two-argument native set-table-shader-material-values-param(table, x)
// (SP: *ellie-flush*, *ellie-dirt-seattle* ...) sets it at once. The table acts on the MATERIAL (by sid), so every body wearing the garment - ours
// and the puppets' - shows the same snow cover on every screen without any message.
// On the winter map (Supermarket = to1-jackson-market) while a match runs: strength MPSNOW_LEVEL, wetness 0, re-asserted every 2 s (a level / look
// reload may reset the material); everywhere else 0. Test override: t2r-mp-snow-force.txt (one float 0..1) = that strength on EVERY map.
// Cross-pak test (T2R_MP_SNOW_XPAK=1 builds, tools/features/mp_snow.py XPAK): t2r-snow-xpak-test.txt = the force file with default strength 0.5 (winter looks too).
// Opt-out: t2r-no-mp-snow.txt next to the exe. Log prefix "mpsnow: ".
#if __has_include("mp_snow_built.h")
#include "mp_snow_built.h"
#endif
#ifndef T2R_MP_SNOW_BUILT
#define T2R_MP_SNOW_BUILT 0
#endif
#define MPSNOW_LEVEL 0.20f      // A/B: 0.30 left the Seraphite white (snow_mask.MASK_BOOST puts ~33 % of an island above 0.7); 0.16 = ~14 %, Ellie's own coverage (~12 % above 0.75 at her 0.3). One value per material table: no per-character melt (re/mp-snow.md 8.26)
#define MPSNOW_COVER_CAP 0.10f  // runtime accumulation ceiling (rolling in snow). A/B: 0.30 / task-begin 0.30 (we RAISED the file's 0.25 / 0.10) = sleeves paint-white, nothing else; SP's lowest is 0.10 with task-begin 0
#define MPSNOW_TASK_BEGIN 0.0f  // snow-task-begin-coverage: SP writes 0 at the start of a scene (T2R-SP t2r-sp.log FXN) = bodies start bare; it is the START level of the accumulation task, never raise it to the cap
#define MPSNOW_MELT_S 20.0f     // seconds indoors until the garment snow is gone (strength ramps LEVEL -> 0)
#define MPSNOW_REGROW_S 60.0f   // seconds outdoors until it is back (falling snow)
#ifndef MP_SNOW_WINTER_LOOKS
#define MP_SNOW_WINTER_LOOKS 0
#endif
#ifndef MP_SNOW_NTABLES
#define MP_SNOW_NTABLES 0
#define MP_SNOW_TABLES {0}
#endif
static const uint64_t mpsnow_tables[] = MP_SNOW_TABLES; // the material-param tables whose g_snowStrength rows the build re-pointed at our garments
static int MpsnowOff(void)
{
    static int c = -1;
    static uint64_t k;
    return CfxeFile("t2r-no-mp-snow.txt", &c, &k);
}

static int MpsnowWinter(const char *map)
{
    return map && !strcmp(map, "Supermarket");
}

static int MpsnowSet(uint64_t table, float v, uint64_t *ret)
{
    uint32_t b;
    memcpy(&b, &v, 4);
    uint64_t a[2] = { table, b };
    *ret = DcCall(FNV64("set-table-shader-material-values-param"), 2, a);
    return *ret != ~0ULL;
}

// Winter crew looks (FACTION_LOOKS_WINTER): the winter map (or t2r-mp-snow-force.txt, which every client of a test must have) in a build that carries the looks.
static int MpsnowWinterLooksNow(void)
{
#if T2R_MP_SNOW_BUILT && MP_SNOW_WINTER_LOOKS
    if (MpsnowOff())
        return 0;
    {
        static int c = -1;
        static uint64_t k;
        if (CfxeFile("t2r-no-winter-looks.txt", &c, &k))
            return 0;
    }                           // bisect switch (night22): the normal WLF / Seraphite looks, snow rows etc. unchanged
    FILE *ff = ExeSideOpen("t2r-mp-snow-force.txt", "r");
    if (!ff)
        ff = ExeSideOpen("t2r-snow-xpak-test.txt", "r");
    if (ff) {
        fclose(ff);
        return 1;
    }
    uint8_t *lc = LobbyObj();
    return lc && !IsBadReadPtr(lc, 0x20)
        && MpsnowWinter(MfMapName(lc[0x18]));
#else
    return 0;
#endif
}

// Melt (the snow did not melt at all; rule: per character, never camera-wide).
// What the engine does per body: the runtime cover (render-settings particles:snow-*, natives set-state-character-accumulates-snow-from-sky / -from-ground,
// both GLOBAL switches without a character argument, disassembled: tlou-ii.exe+0xd62240 / 0xd621b0) is a per-body mask that accumulates from the sky / ground
// contact and decays at snow-decey-multiplier down to snow-minimum-coverage. SP applies decay only inside its interior render-settings region (pat.bin
// *rs-pat-chalet-snow-accum-interior*: min-coverage 0, decey-multiplier 1.2); outdoors the region defines neither key. Here that region is held ON for the whole
// match (weight 1): every body decays all the time, and a body in the open is re-fed by the snowfall faster (accumulation multiplier 100) while a body under a
// roof is not fed and melts - per body without any per-body code. (Inference from the native names; the log lines below show the live keys.)
// The garment-strength layer (g_snowStrength in the material tables) is per MATERIAL, shared by every body wearing it: it melts only while EVERY body in the
// match is inside (each by its own position, mpsnow_all_indoor), so nobody ever sees snow melt on a body that stands outside.
static int ImmBodyIndoor(const float *p);     // immersion.c
static uint64_t ImmRsCall(uint64_t sid, int prio, float strength);    // immersion.c
static float mpsnow_melt;       // 0 = fresh .. 1 = melted
static int mpsnow_on_winter;    // last slow tick: snow wanted on this map
static int mpsnow_nbody, mpsnow_nin;    // bodies seen / of those indoors (last slow tick)
static void MpsnowMeltFrame(void)
{
    static uint64_t sid;
    if (!sid)
        sid = FNV64("*rs-pat-chalet-snow-accum-interior*");
    static int nlog;
    if (!mpsnow_on_winter)
        return;
    uint64_t r = ImmRsCall(sid, 310, 1.0f);
    if (nlog < 3) {
        nlog++;
        Xlog("mpsnow: SP interior snow region (melt) held on for every body (-> %llx)", (unsigned long long) r);
    }
}

static void MpsnowCountBodies(void)
{
    mpsnow_nbody = mpsnow_nin = 0;
    float cp[3], cd[3];
    if (CamPosDir(cp, cd)) {
        mpsnow_nbody++;
        mpsnow_nin += ImmBodyIndoor(cp);
    }
    typedef uint8_t *(*mpn_peer_t)(uint32_t);
    uint32_t self = *(uint32_t *) T2R_SYM(kMySlot);
    for (uint32_t sl = 0; sl < T2R_MAX_PEERS; sl++) {
        if (sl == self)
            continue;
        uint8_t *peer = ((mpn_peer_t) T2rPeer) (sl);
        uint64_t h = peer ? *(uint64_t *) (peer + 0x60) : 0;
        float pp[3];
        if (h && PtPeerPos(sl, pp)) {
            mpsnow_nbody++;
            mpsnow_nin += ImmBodyIndoor(pp);
        }
    }
}

static void MpsnowLogKeys(const char *why)
{
    static const char *const K[] =
        { "particles:snow-accumulation-multiplier",
"particles:snow-decey-multiplier", "particles:snow-maximum-coverage",
        "particles:snow-minimum-coverage",
            "particles:snow-melt-time-on-skin",
            "particles:snow-task-begin-coverage"
    };
    char line[400];
    int q = 0;
    for (int i = 0; i < 6; i++) {
        uint64_t a = FNV64(K[i]), r =
            DcCall(FNV64("get-render-setting-value-float"), 1, &a);
        uint32_t u = (uint32_t) r;
        float f;
        memcpy(&f, &u, 4);
        q += snprintf(line + q, sizeof line - q, " %s=%g", K[i] + 15,
                      r == ~0ULL ? -1.0 : (double) f);
    }
    Xlog("mpsnow: engine snow keys (%s)%s", why, line);
}

static void MpsnowTick(void)
{
#if T2R_MP_SNOW_BUILT
    static float applied = -1.0f;
    static uint64_t at;
    static int fails, announced;
    MpsnowMeltFrame();
    if (g_frame % 15)
        return;
    float want = 0.0f;
    const char *why = "off";
    if (!MpsnowOff() && MatchSubstate() == 6) {
        uint8_t *lc = LobbyObj();
        const char *mn = lc
            && !IsBadReadPtr(lc, 0x20) ? MfMapName(lc[0x18]) : NULL;
        FILE *ff = ExeSideOpen("t2r-mp-snow-force.txt", "r");
        const char *fname = "t2r-mp-snow-force.txt";
        if (!ff) {
            ff = ExeSideOpen("t2r-snow-xpak-test.txt", "r");
            fname = "t2r-snow-xpak-test.txt";
        }                       // the cross-pak test: same as the force file (default 0.5)
        float fv = 0.0f;
        if (ff) {
            if (fscanf(ff, "%f", &fv) != 1 || fv < 0.0f || fv > 1.0f)
                fv = !strcmp(fname,
                             "t2r-snow-xpak-test.txt") ? 0.5f :
                    MPSNOW_LEVEL;
            fclose(ff);
            want = fv;
            why = fname;
        } else if (MpsnowWinter(mn)) {
            want = MPSNOW_LEVEL;
            why = "winter map";
        } else
            why = "not a winter map";
    } else if (!MpsnowOff())
        why = "no match";
    // Runtime snow accumulation (A/B: rolling in the snow turned the sleeves paint-white). The winter map's ungated render region "pat-rs-pat-snow-accum"
    // (prio 300, rsdiag) is live in MP at its file defaults (snow-maximum-coverage 1.0 = the accumulation mask may fill to full white); SP's own script caps
    // it with change-render-setting-value-float(*pat-rs-pat-snow-accum*, particles:snow-maximum-coverage / snow-task-begin-coverage, 0.1 / 0.175 / 0.25
    // (SP recorder log FXN, 0.25 most often). Same call here, re-asserted every 2 s while snow is wanted.
    mpsnow_on_winter = want > 0.0f;
    {                           // melt state, advanced by wall time between slow ticks
        static uint64_t mt;
        uint64_t mn = GetTickCount64();
        float dt = mt ? (float) (mn - mt) / 1000.0f : 0.0f;
        mt = mn;
        if (dt > 1.0f)
            dt = 1.0f;
        if (mpsnow_on_winter)
            MpsnowCountBodies();
        float w = mpsnow_on_winter && mpsnow_nbody > 0 && mpsnow_nin == mpsnow_nbody ? 1.0f : 0.0f;     // all bodies inside
        static int was_in = -1;
        mpsnow_melt +=
            w * dt / MPSNOW_MELT_S - (1.0f - w) * dt / MPSNOW_REGROW_S;
        if (mpsnow_melt < 0.0f)
            mpsnow_melt = 0.0f;
        else if (mpsnow_melt > 1.0f)
            mpsnow_melt = 1.0f;
        if (!mpsnow_on_winter)
            mpsnow_melt = 0.0f;
        int in = w > 0.5f;
        if (mpsnow_on_winter && in != was_in) {
            was_in = in;
            MpsnowLogKeys(in ? "went indoors" : "went outdoors");
            Xlog("mpsnow: %s (%d of %d bodies indoors), melt %.2f",
                 in ? "all indoors" : "someone outdoors", mpsnow_nin,
                 mpsnow_nbody, (double) mpsnow_melt);
        }
        static uint64_t kl;
        if (mpsnow_on_winter && mn - kl >= 30000) {
            kl = mn;
            MpsnowLogKeys("30 s");
        }
        want *= 1.0f - mpsnow_melt;
        static float lw;
        if ((int) (mpsnow_melt * 10) != (int) (lw * 10))
            Xlog("mpsnow: melt %.2f -> garment strength %.3f",
                 (double) mpsnow_melt, (double) want);
        lw = mpsnow_melt;
    }
    if (mpsnow_on_winter) {
        static uint64_t cap_at;
        static int cap_n;
        uint64_t cn = GetTickCount64();
        if (cn - cap_at >= 2000) {
            cap_at = cn;
            float cv = MPSNOW_COVER_CAP, tv = MPSNOW_TASK_BEGIN;
            uint32_t cb, tb;
            memcpy(&cb, &cv, 4);
            memcpy(&tb, &tv, 4);
            uint64_t g = FNV64("*pat-rs-pat-snow-accum*"), a1[3] =
                { g, FNV64("particles:snow-maximum-coverage"), cb },
                a2[3] =
                { g, FNV64("particles:snow-task-begin-coverage"), tb };
            uint64_t q1 =
                DcCall(FNV64("change-render-setting-value-float"), 3, a1),
                q2 =
                DcCall(FNV64("change-render-setting-value-float"), 3, a2);
            if (cap_n++ < 3)
                Xlog("mpsnow: snow accumulation cap %.2f, task-begin 0 (max-coverage -> %llx, task-begin -> %llx)", (double) cv, (unsigned long long) q1, (unsigned long long) q2);
        }
    }
    if (fails >= 5)
        return;                 // the calls do not work in this build: stop (logged once)
    uint64_t now = GetTickCount64();
    // (A/B: snow on B's sleeves / hood on Train Station. The build re-points the table rows with value 1.0, so an untouched table is FULL snow:
    // "never touched" is not "no snow". In a match every value, 0 included, is re-asserted every 2 s (a level load may restore the rows' 1.0);
    // outside a match 0 is written once.)
    if (want == applied
        && (now - at < 2000 || !strcmp(why, "no match")
            || !strcmp(why, "off")))
        return;
    uint64_t r1 = 0;
    int ok = 1;
    for (int i = 0; i < MP_SNOW_NTABLES && ok; i++)
        ok = MpsnowSet(mpsnow_tables[i], want, &r1);
    if (!ok) {
        if (++fails >= 5)
            Xlog("mpsnow: set-table-shader-material-values-param unavailable / failed (%llx) - off", (unsigned long long) r1);
        return;
    }
    if (want != applied || !announced) {
        Xlog("mpsnow: snow strength %.2f in %d tables (%s)", (double) want,
             (int) MP_SNOW_NTABLES, why);
        announced = 1;
    }
    applied = want;
    at = now;
#endif
}

// ---- winter Seraphites: hood UP (prototype, T2R_MP_SNOW=1 builds only; re/mp-snow.md section 6) -------------------------------------------
// Every Seraphite look already carries both hood parts (look2 meshes scar-male-hoodup / hooddown, scar-female-...; the up one is hidden at spawn, +0x48).
// On the winter map every Seraphite body (ours and the puppets') shows the hood up and the hood down hidden; set-mesh-visible through the armour
// rig's gvis_set (same dedupe). While a body's HELMET shows, the armour rig itself hides the hood-up clash part: not touched then.
// Keyed by the look NAME the faction-look protocol knows (fl_name), so it needs the faction looks on; no protocol change.
static int MpsnowHoodBody(int idx, uint64_t chr, const char *look,
                            int helm)
{
    if (!look || strncmp(look, "scar-", 5))
        return 0;
    if (idx >= 0 && idx <= T2R_MAX_PEERS)
        g_hoodUp[idx] = 1;     // before the hide below: a rig pass in between must already see it (night57 B frame 9050: hidden, then re-shown by the rig the same frame)
    const char *fem =
        !strncmp(look, "scar-female", 11) ? "scar-female" : "scar-male";
    char up[40], down[40];
    snprintf(up, sizeof up, "%s-hoodup", fem);
    snprintf(down, sizeof down, "%s-hooddown", fem);
    GvisSet(idx, chr, down, 0, "winter hood up (mpsnow: hood down off)");
    GvisSet(idx, chr, up, helm ? 0 : 1,
             helm ? "winter hood (helmet shows: hood off)" :
             "winter hood up (mpsnow)");
    return 1;
}

static void MpsnowHoodTick(void)
{
#if T2R_MP_SNOW_BUILT
    if (g_frame % 10)
        return;
    uint8_t up[T2R_MAX_PEERS + 1] = { 0 };      // -> g_hood_up: the armour rig keeps these bodies' hood-down pair hidden (t2r-ext.c armor_rig_body)
    if (MpsnowOff() || MatchSubstate() != 6 || FactionLooksOff()) {
        memcpy((void *) g_hoodUp, up, sizeof up);
        return;
    }
    uint8_t *lc = LobbyObj();
    if (!lc || IsBadReadPtr(lc, 0x20)
        || !MpsnowWinter(MfMapName(lc[0x18]))) {
        memcpy((void *) g_hoodUp, up, sizeof up);
        return;
    }
    typedef uint8_t *(*mph_me_t)(void);
    typedef uint8_t *(*mph_peer_t)(uint32_t);
    uint8_t *me = ((mph_me_t) T2R_SYM(kFnLocalPlayer)) ();
    if (me && g_fl.seq)
        up[T2R_US] =
            (uint8_t) MpsnowHoodBody(T2R_US, SID_PLAYER,
                                       FlName(g_fl.team, g_fl.cur),
                                       g_helm > 0);
    uint32_t self = *(uint32_t *) T2R_SYM(kMySlot);
    for (uint32_t sl = 0; sl < T2R_MAX_PEERS; sl++) {
        if (sl == self)
            continue;
        uint8_t *peer = ((mph_peer_t) T2rPeer) (sl);
        uint64_t h = peer ? *(uint64_t *) (peer + 0x60) : 0;
        int team, idx = h ? FlCoreShown(&g_fl, sl, &team) : -1;
        if (idx >= 0)
            up[sl] =
                (uint8_t) MpsnowHoodBody((int) sl, h, FlName(team, idx),
                                           g_peerHelm[sl] > 0);
    }
    memcpy((void *) g_hoodUp, up, sizeof up);
#endif
}
