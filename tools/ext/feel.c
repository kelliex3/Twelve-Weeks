// TLOU2 single-player "feel" features, cosmetic only (re/feature-ideas-tlou2-feel.md items 2, 5, 7). Included by t2r-ext.c before t2r_tick.
//
//  1. Blood trail + pools (off: t2r-no-blood.txt): a downed player leaves a smear while crawling and a pool under the lying / dead
//     body. Rendered locally on every client from state we already sync: our own body (g_downed, locator) and every puppet
//     (g_peer_downed, peer+0x68 net position). Particle effect groups of the game's own part-blood-and-gore module (names verified
//     by hashing against the module's sid table, re/tlou2-feel-notes.md): blood-pool-01..08 (random per pool), blood-splatter-trail
//     (the smear), blood-drips. Spawned with the same pair of natives the armour sparks use (armor_fx_spawn: spawn-particles-at-bound-frame
//     at the get-joint-locator of "root", else spawn-particles-at-joint). Override: t2r-blood-fx.txt lines "pool|trail|drips <particle>
//     [particle...]" ("-" = none). Rate limits: one spawn per BLOOD_GAP frames over everything, trail every 0.45 s of crawl (cap 16 per
//     down), pools at most 2 per down (+1 at death), nothing beyond BLOOD_RANGE metres.
//  2. Wounded gait (off: t2r-no-wounded-gait.txt): below 30 % health our own walk type is walk-uneasy (the stun lock uses the same
//     set-walk-type sid); restored to walk/run/sprint on heal, down, death, match end. Puppets replay our animations, so peers see it.
//     t2r-wounded-gait.txt "walk=<sid> run=<sid> sprint=<sid> pct=<n>" overrides (default run / sprint unchanged: only the walk is
//     lame, a walk-class clip at run speed would skate).
//  3. Hit reactions, visual only (off: t2r-no-hit-react.txt; t2r-hit-react-no-net.txt = own body only): when a bullet / blow lands on us an additive
//     upper-body gesture (the game's gesture-ellie-idle-ub-hr-*-add set, zone x direction x crouch / run) plays on our body, and on our puppet on every
//     other screen (MSG_HITREACT). No input lock, no speed change, no root motion. Details in the section below and re/hit-reactions.md.
// Everything logs "feel: ..." lines (first ones only).

#define BLOOD_GAP SecsFrames(0.17f)    // frames between any two blood spawns
#define BLOOD_RANGE 45.0f       // metres from us
#define BLOOD_TRAIL_FRAMES SecsFrames(0.45f)   // 0.45 s in real time
#define BLOOD_TRAIL_MAX 16
#define BLOOD_POOL_MAX 2

static int FeelFlag(const char *file, uint64_t *at, int *val, int every)
{
    if (!*at || g_frame - *at >= (uint64_t) every) {
        *at = g_frame ? g_frame : 1;
        FILE *f = ExeSideOpen(file, "r");
        int v = f != NULL;
        if (f)
            fclose(f);
        // (opt-out files switch a feature off, opt-in files on: the caller knows which)
        if (v != *val) {
            Xlog("feel: %s %s", file, v ? "present" : "gone");
            *val = v;
        }
    }
    return *val;
}

// world locator (position + rotation, 8 floats) of a puppet handle; NULL if unavailable
static const float *FeelLoc(uint64_t h, float *b16)
{
    typedef void *(*fn_proc)(uint64_t);
    typedef const float *(*fn_loc)(void *, float *);
    uint8_t *pr = h ? ((fn_proc) T2R_SYM(kFnEntityFromHandle)) (h) : NULL;
    if (!pr || IsBadReadPtr(pr + 0x100, 0x40))
        return NULL;
    return ((fn_loc) ((uint8_t *) GetModuleHandleW(NULL) + 0x133db30)) (pr
                                                                        +
                                                                        0x100,
                                                                        b16);
}

// ---------------------------------------------------------------- 1. blood
typedef struct {
    int state;                  // last seen: 0 up, 1 downed, 2 dying, 3 dead
    float px, pz;
    int pos_valid;
    float speed;
    uint64_t still_since, last_trail, down_at, dead_at;
    int trails, pools, dead_pool;
    float pool_x, pool_z;
    int pool_valid;
} BloodSt;
static BloodSt g_blood[T2R_MAX_PEERS + 1];     // 0..3 puppets, 4 = us
static uint64_t g_bloodLast;   // frame of the last spawn
static int g_bloodOff = -1;
static uint64_t g_bloodOffAt;
#define BLOOD_FX_MAX 8
static uint64_t g_bfx[3][BLOOD_FX_MAX];
static char g_bfxNm[3][BLOOD_FX_MAX][40];
static int g_bfxN[3], g_bfx_ready;
static const char *const BFX_KEY[3] = { "pool", "trail", "drips" };

static const char *const BFX_DEF[3] = {
    "blood-pool-01 blood-pool-02 blood-pool-03 blood-pool-04 blood-pool-05 blood-pool-06 blood-pool-07 blood-pool-08",
    "blood-splatter-trail", "blood-drips"
};

static void BloodFxParse(int k, const char *list)
{
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", list);
    g_bfxN[k] = 0;
    for (char *t = strtok(tmp, " \t\r\n"); t && g_bfxN[k] < BLOOD_FX_MAX;
         t = strtok(NULL, " \t\r\n")) {
        if (!strcmp(t, "-"))
            continue;
        snprintf(g_bfxNm[k][g_bfxN[k]], 40, "%s", t);
        g_bfx[k][g_bfxN[k]++] = FNV64(t);
    }
}

static void BloodFxLoad(void)
{
    if (g_bfx_ready)
        return;
    g_bfx_ready = 1;
    for (int k = 0; k < 3; k++)
        BloodFxParse(k, BFX_DEF[k]);
    FILE *f = ExeSideOpen("t2r-blood-fx.txt", "r");
    if (f) {
        char ln[512];
        while (fgets(ln, sizeof ln, f))
            for (int k = 0; k < 3; k++) {
                size_t kl = strlen(BFX_KEY[k]);
                if (!strncmp(ln, BFX_KEY[k], kl)
                    && (ln[kl] == ' ' || ln[kl] == '\t'))
                    BloodFxParse(k, ln + kl);
            }
        fclose(f);
        Xlog("feel: blood: t2r-blood-fx.txt read");
    }
    for (int k = 0; k < 3; k++)
        Xlog("feel: blood: %s = %d effect(s), first %s", BFX_KEY[k],
             g_bfxN[k], g_bfxN[k] ? g_bfxNm[k][0] : "(none)");
}

// kind 0 pool, 1 trail, 2 drips; chr = SID_PLAYER or a puppet handle. Returns 1 if spawned.
static int BloodSpawn(int kind, uint64_t chr, const char *why)
{
    static int nl;
    static uint64_t ok_n, fail_n;
    BloodFxLoad();
    if (kind != 0 && !OldBloodParticles())
        return 0;               // the crawl trail splat / drips are not SP blood (SP leaves one blood-pool per kill); t2r-old-blood-particles.txt = back
    if (!g_bfxN[kind]
        || (g_bloodLast && g_frame - g_bloodLast < BLOOD_GAP))
        return 0;
    g_bloodLast = g_frame ? g_frame : 1;
    int i = kind == 0 ? (int) ((g_frame * 2654435761u >> 7) % (uint32_t) g_bfxN[0]) : 0;       // pools: a random variant
    uint64_t r = ArmorFxSpawn(g_bfx[kind][i], chr, "root");
    int ok = r && r != ~0ULL;
    if (ok)
        ok_n++;
    else
        fail_n++;
    if (nl < 40 || (!ok && fail_n < 6)) {
        nl++;
        Xlog("feel: blood %s (%s) %s on %llx -> %llx (ok %llu fail %llu)",
             BFX_KEY[kind], why, g_bfxNm[kind][i],
             (unsigned long long) chr, (unsigned long long) r,
             (unsigned long long) ok_n, (unsigned long long) fail_n);
    }
    return ok;
}

// one body: who 0..3 puppet, 4 us. st: 0 up, 1 downed, 2 dying, 3 dead; pos = world xz of the body; chr its handle
static void BloodBody(int who, int st, uint64_t chr, const float *pos,
                       const float *me)
{
    BloodSt *b = &g_blood[who];
    uint64_t now = g_frame ? g_frame : 1;
    if (st != b->state) {
        if (st == 1 && b->state == 0) {
            b->trails = b->pools = 0;
            b->still_since = now;
            b->down_at = now;
            b->pos_valid = 0;
            b->pool_valid = 0;
            b->dead_pool = 0;
        }
        if (st == 3 && b->state != 3) {
            b->dead_at = now;
            b->dead_pool = 0;
        }
        if (st == 0) {
            b->dead_pool = 0;
            b->pool_valid = 0;
        }
        b->state = st;
    }
    if (st == 0)
        return;
    if (!chr || !pos)
        return;
    float dxm = pos[0] - me[0], dzm = pos[1] - me[1];
    if (dxm * dxm + dzm * dzm > BLOOD_RANGE * BLOOD_RANGE) {
        b->pos_valid = 0;
        return;
    }
    if (b->pos_valid) {
        float dx = pos[0] - b->px, dz = pos[1] - b->pz, v =
            __builtin_sqrtf(dx * dx + dz * dz) * 1000.0f / FrameMsC();
        if (v > 8.0f)
            v = 0;              // teleport / network snap
        b->speed += (v - b->speed) * 0.12f;
    }
    b->px = pos[0];
    b->pz = pos[1];
    b->pos_valid = 1;
    if (st == 3) {              // dead: one pool once the body has settled
        if (!b->dead_pool && now - b->dead_at > SecsFrames(1.25f)
            && g_frame - g_bloodLast >= BLOOD_GAP) {
            if (!BloodSpawn(0, chr, "death")) {
                g_bloodLast = 0;
                BloodSpawn(1, chr, "death, pool fallback");
            }
            b->dead_pool = 1;
        }
        return;
    }
    if (st == 2)
        return;
    int crawl = b->speed > 0.35f;
    if (crawl) {
        b->still_since = now;
        if (now - b->last_trail >= BLOOD_TRAIL_FRAMES
            && b->trails < BLOOD_TRAIL_MAX
            && now - b->down_at > SecsFrames(0.75f)) {
            if (BloodSpawn(1, chr, "crawl")) {
                b->last_trail = now;
                b->trails++;
            }
        }
        return;
    }
    // lying still: the pool grows under a body that stayed 1.2 s; a second one if it crawled >2.5 m off the first
    if (now - b->still_since > SecsFrames(1.2f)
        && b->pools < BLOOD_POOL_MAX) {
        float dx = pos[0] - b->pool_x, dz = pos[1] - b->pool_z;
        if (!b->pool_valid || dx * dx + dz * dz > 2.5f * 2.5f) {
            // the blood-pool-0x groups return 0 here (not spawnable at a bound frame) and were retried every 10 frames
            // forever - one try per spot; on failure the trail splat (which spawns) marks the spot instead, then no more tries
            if (g_frame - g_bloodLast >= BLOOD_GAP) {
                int okp = BloodSpawn(0, chr, "lying");
                if (!okp) {
                    g_bloodLast = 0;
                    BloodSpawn(1, chr, "lying, pool fallback");
                }
                b->pools++;
                b->pool_x = pos[0];
                b->pool_z = pos[1];
                b->pool_valid = 1;
            }
        }
    }
}

// Blood part modules held resident for the match. MP levels don't list them, so without a reference the particle
// ModulePool (exe 142080b40) streams them into a pool slot when an effect first asks and unloads them again once no
// effect uses them: a load hitch per bleed, and (a crash, before install_script_unload_fix) a pool-slot module
// unload at the level change. Same module reference API the level particle-module defs use (exe 141cca720): sid of
// "part-module/<name>", acquire 1420803a0 / release 142080430 on pool = [mgr 1492d9720 + 0x70], only while the pool
// isn't mid-load (+0x300 == 0). blood-splatter-trail is in part-blood-and-gore, blood-pool-01..08 and blood-drips in
// part-kit-blood-and-gore (sid tables of re/gamebin/dc1/part-module).
// SP-exact takedown blood (clipfx.c): the takedown clips' part-effect groups that are in neither blood module live in the melee anim
// actors' own part modules (the engine loads "part-actor-<actor>" with an SP actor; sid tables of re/gamebin/dc1/part-module):
// blood-splatter-wall-spray-eff, blood-switchblade-splatter-line-curved / -wall-eff, blood-spread-wall-messy-eff, bm-blood-spill,
// bm-abby-fist-blood-right, bm-ellie-* skin blood ... -> part-actor-anim-melee-ellie-t2 (Ellie's knife set, 346 KB) and
// part-actor-anim-melee-player-shared-t2 (Abby's fists + shared, 422 KB); held the same way so the first takedown has them.
// + part-actor-trap-mine (the placed pipe bomb goes off with SP's "trap-mine" particles, placedbomb2013.c pb_trap_blast): resident, so
// the first blast of a match does not ask the pool for a module still streaming in (the spawn would fail -> pipe-bomb explode fallback).
// + part-npc-fire (the fire-* body groups of the NPC burn effect: a flamethrower kill burns on every screen, deathreact.c bd_*): resident, so the first burning death does not stream it.
#define BLOOD_MODS_N 6
static const char *const BLOOD_MODS[BLOOD_MODS_N] =
    { "part-module/part-blood-and-gore",
"part-module/part-kit-blood-and-gore",
    "part-module/part-actor-anim-melee-ellie-t2",
        "part-module/part-actor-anim-melee-player-shared-t2",
    "part-module/part-actor-trap-mine", "part-module/part-npc-fire"
};

static int g_bpin, g_bpin_bad;
static uint8_t *g_bpinPool;    // the pool our references were taken on (a level change may rebuild it: never release on another one)
static void BloodPin(int want)
{
    if (want == g_bpin || g_bpin_bad)
        return;
    uint8_t *exe = (uint8_t *) GetModuleHandleW(NULL);
    static const uint8_t pro[8] = { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x57, 0x48, 0x83 };   // 1420803a0 / 142080430
    static const uint8_t thunk[9] = { 0x48, 0x8b, 0x49, 0x70, 0xe9, 0x57, 0x5f, 0x52, 0x00 };   // 141b5a440: mov rcx,[rcx+70]; jmp 1420803a0
    if (memcmp(exe + 0x20803a0, pro, 8) || memcmp(exe + 0x2080430, pro, 8)
        || memcmp(exe + 0x1b5a440, thunk, 9)) {
        g_bpin_bad = 1;
        HOOK_FAIL
            ("feel: blood modules: particle module pool code differs - not held");
        return;
    }
    uint8_t *pool = *(uint8_t **) (exe + 0x92d9720 + 0x70);
    if (!pool || IsBadReadPtr(pool, 0x400))
        return;
    if (*(volatile int *) (pool + 0x300))
        return;                 // mid-load: next frame
    typedef void (*fn_ref)(uint8_t *, uint64_t);
    typedef char (*fn_has)(uint8_t *, uint64_t);
    fn_ref f = (fn_ref) (exe + (want ? 0x20803a0 : 0x2080430));
    if (!want && pool != g_bpinPool) {
        Xlog("feel: blood modules: pool %p is not the one they were held on (%p) - nothing to release", pool, g_bpinPool);
        g_bpin = 0;
        return;
    }
    for (int i = 0; i < BLOOD_MODS_N; i++) {
        uint64_t sid = FNV64(BLOOD_MODS[i]);
        if (!want && !((fn_has) (exe + 0x207cff0)) (pool, sid))
            continue;           // not requested any more (pool reset): a release would underflow its count
        f(pool, sid);
        int req = ((fn_has) (exe + 0x207cff0)) (pool, sid) != 0, in =
            ((fn_has) (exe + 0x207cfc0)) (pool, sid) != 0;
        Xlog("feel: blood module %s %s (requested %d, in a pool slot %d)",
             BLOOD_MODS[i], want ? "held" : "released", req, in);
    }
    g_bpin = want;
    g_bpinPool = want ? pool : NULL;
}

static void BloodTick(void)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    static int off = -1;
    static uint64_t off_at;
    int in_match = MatchSubstate() == 6;
    if (FeelFlag("t2r-no-blood.txt", &off_at, &off, 180)) {
        BloodPin(0);
        return;
    }                           // default on: the crash was ScriptModule::Unload (install_script_unload_fix)
    BloodPin(in_match);
    if (!in_match) {
        for (int i = 0; i <= T2R_US; i++)
            g_blood[i].state = 0;
        return;
    }
    float lb[16];
    const float *ml = Uc4MyLoc(lb);
    if (!ml)
        return;
    float me[2] = { ml[0], ml[2] };
    // ourselves
    {
        int st = g_downed ? (g_downDying
                             || g_uc4DyingUntil ? 2 : 1) : 0;
        float p[2] = { ml[0], ml[2] };
        BloodBody(4, st, SID_PLAYER, p, me);
    }
    for (uint32_t sl = 0; sl < T2R_MAX_PEERS; sl++) {
        uint8_t *peer = ((fn_peer) T2rPeer) (sl);
        if (!peer || IsBadReadPtr(peer, 0x90) || !peer[4]) {
            g_blood[sl].state = 0;
            continue;
        }
        uint64_t h = *(uint64_t *) (peer + 0x60);
        int st =
            peer[5] ? (g_blood[sl].state ? 3 : 0) : g_peerDowned[sl] ==
            1 ? 1 : g_peerDowned[sl] == 2 ? 2 : 0;
        if (peer[5] && g_blood[sl].state == 0 && g_peerDowned[sl])
            st = 3;
        if (!h)
            continue;
        const float *np = (const float *) (peer + 0x68);
        if (np[0] != np[0] || np[2] != np[2]
            || __builtin_fabsf(np[0]) > 1e5f
            || __builtin_fabsf(np[2]) > 1e5f)
            continue;
        float p[2] = { np[0], np[2] };
        BloodBody(sl, st, h, p, me);
    }
}

// ---------------------------------------------------------------- 2. wounded gait
static int g_wgOn;             // we changed the move types
static uint64_t g_wgAt;
static uint64_t g_wg_walk, g_wg_run, g_wg_sprint;
static int g_wgPct = 30;
static float g_wgMax = 100.0f;
static uint64_t g_wgMaxAt;
static void WgSet(uint64_t w, uint64_t r, uint64_t s)
{
    DcCall(FNV64("set-walk-type"), 1, &w);
    DcCall(FNV64("set-run-type"), 1, &r);
    if (g_skExhausted)
        s = FNV64("run");       // skills.c sprint meter empty: no sprint animation / speed until it recovered
    DcCall(FNV64("set-sprint-type"), 1, &s);
}

static void WoundedGaitTick(void)
{
    static int off = -1;
    static uint64_t off_at;
    static int cfg;
    if (FeelFlag("t2r-no-wounded-gait.txt", &off_at, &off, 180)) {
        if (g_wgOn) {
            WgSet(FNV64("walk"), FNV64("run"), FNV64("sprint"));
            g_wgOn = 0;
            Xlog("feel: wounded gait OFF - restored");
        }
        return;
    }
    if (!cfg) {
        cfg = 1;
        g_wg_walk = FNV64("walk-uneasy");
        g_wg_run = FNV64("run");
        g_wg_sprint = FNV64("sprint");
        FILE *f = ExeSideOpen("t2r-wounded-gait.txt", "r");
        if (f) {
            char ln[256];
            while (fgets(ln, sizeof ln, f)) {
                char v[64];
                if (sscanf(ln, " walk=%63s", v) == 1)
                    g_wg_walk = FNV64(v);
                else if (sscanf(ln, " run=%63s", v) == 1)
                    g_wg_run = FNV64(v);
                else if (sscanf(ln, " sprint=%63s", v) == 1)
                    g_wg_sprint = FNV64(v);
                else
                    sscanf(ln, " pct=%d", &g_wgPct);
            }
            fclose(f);
            Xlog("feel: wounded gait: t2r-wounded-gait.txt read (pct %d)",
                 g_wgPct);
        }
        Xlog("feel: wounded gait ready: below %d%% health walk-type walk-uneasy", g_wgPct);
    }
    if (g_frame - g_wgAt < SecsFrames(0.33f))
        return;
    g_wgAt = g_frame;
    int live = MatchSubstate() == 6 && !g_downed && !g_downDying
        && !g_uc4DyingUntil && !(g_stun_until && g_frame < g_stun_until);
    float hp = live ? LocalHp() : -1.0f;
    if (!g_wgMaxAt || g_frame - g_wgMaxAt > SecsFrames(30.0f)) {
        g_wgMaxAt = g_frame ? g_frame : 1;
        uint64_t none = 0, r =
            DcCall(FNV64("get-player-max-health"), 0, &none);
        uint32_t lo = (uint32_t) r;
        float f;
        memcpy(&f, &lo, 4);
        if (r != ~0ULL && lo > 0x10000 && f >= 10.0f && f < 100000.0f)
            g_wgMax = f;
        else if (r != ~0ULL && (int32_t) lo >= 10 && (int32_t) lo < 100000)
            g_wgMax = (float) (int32_t) lo;
    }
    int low = hp > 0.0f
        && hp <= g_wgMax * g_wgPct * 0.01f, high =
        hp > g_wgMax * (g_wgPct + 8) * 0.01f || hp < 0.0f;
    if (low && !g_wgOn) {
        g_wgOn = 1;
        WgSet(g_wg_walk, g_wg_run, g_wg_sprint);
        Xlog("feel: wounded gait ON (hp %.0f / %.0f)", hp, g_wgMax);
    } else if (g_wgOn && high) {
        g_wgOn = 0;
        if (!(g_stun_until && g_frame < g_stun_until))
            WgSet(FNV64("walk"), FNV64("run"), FNV64("sprint"));
        Xlog("feel: wounded gait restored (hp %.0f)", hp);
    } else if (g_wgOn) {
        WgSet(g_wg_walk, g_wg_run, g_wg_sprint);       // re-assert: scripts reset the types on mode changes
    }
}

// ---------------------------------------------------------------- 2a. injured body language (hand on the wound)
// SP: the player's "injured overlay" is an engine overlay set (anim-overlays-player, renames like ellie-look-poi-tense-gesture ->
// ellie-look-poi-injured-gesture, s_motion-match-locomotion -> ellie-stand-tense-injured-{left,right}-grab-add/-part, ellie-mm-injured-run-*, crouch
// variants; SP sandbox log OV lines). ss-t2/ss-idle-controller asks `player-is-injured-overlay?`; the engine natives that drive it are
// `player-force-injured` (handler exe+0xc9d510, no args), `player-clear-injured-overlay` and the per-frame `player-disable-injured/f` (scripts hold it
// off). No script runs that for an MP match, so below the wounded-gait health the same natives are called from here: the hand-on-wound idle / walk
// overlays and the injured run come from the engine's own tables. Cleared on heal, down, death, match end. Off: t2r-no-injured-overlay.txt.
// Puppets: the natives address the local player only; whether the overlay reaches other screens (anim cmd list) is read from the log lines below.
static int g_ioOn;
static uint64_t g_ioAt;
static void InjuredOverlayTick(void)
{
    static int off = -1;
    static uint64_t off_at;
    static int nl;
    uint64_t none = 0;
    if (FeelFlag("t2r-no-injured-overlay.txt", &off_at, &off, 180)) {
        if (g_ioOn) {
            DcCall(FNV64("player-clear-injured-overlay"), 0, &none);
            g_ioOn = 0;
            Xlog("feel: injured overlay OFF - cleared");
        }
        return;
    }
    if (g_frame - g_ioAt < SecsFrames(0.33f))
        return;
    g_ioAt = g_frame;
    int live = MatchSubstate() == 6 && !g_downed && !g_downDying
        && !g_uc4DyingUntil && !(g_stun_until && g_frame < g_stun_until);
    float hp = live ? LocalHp() : -1.0f;
    int low = hp > 0.0f
        && hp <= g_wgMax * g_wgPct * 0.01f, high =
        hp > g_wgMax * (g_wgPct + 8) * 0.01f || hp < 0.0f;
    if (low && !g_ioOn) {
        uint64_t before =
            DcCall(FNV64("player-is-injured-overlay?"), 0, &none);
        DcCall(FNV64("player-force-injured"), 0, &none);
        g_ioOn = 1;
        uint64_t after =
            DcCall(FNV64("player-is-injured-overlay?"), 0, &none);
        if (nl < 12) {
            nl++;
            Xlog("feel: injured overlay ON (hp %.0f / %.0f): player-is-injured-overlay? %llx -> %llx", hp, g_wgMax, (unsigned long long) before, (unsigned long long) after);
        }
    } else if (g_ioOn && high) {
        g_ioOn = 0;
        DcCall(FNV64("player-clear-injured-overlay"), 0, &none);
        if (nl < 12) {
            nl++;
            Xlog("feel: injured overlay cleared (hp %.0f)", hp);
        }
    }
}

// ---------------------------------------------------------------- 2b. slow walk modifier (hold Ctrl on keyboard)
// SP: the state script ss-t2/ss-pc-partial-movement-controller (module in startup.bin) scales the move stick every frame while the bound
// "partial movement" command is held: (joypad-set-stick-scale 'move flat-scale) with flat-scale 0.85 (var default of the .bin), hold or toggle
// mode by (get-partial-move-input-mode); the commands are partial-move-hold / partial-move-toggle in joypad.bin (PC keyboard default: the walk
// key, Left Ctrl). MP levels never start that script (only SP level scripts do; startup.bin just lists the module) and the ext never read Ctrl
// (VK_CONTROL appears only in freecam / debug), so the key did nothing in a match. Fix: the same per-frame call from here, asking the engine's own
// command state, so the user's binds (and a pad bind, exactly as in SP) behave as in SP.
// Not while downed / dying / stunned. t2r-no-walk-key.txt = off. Log prefix "feel: walk key".
static int WalkkeyCmd(const char *name)
{
    uint64_t c = FNV64(name), r =
        DcCall(FNV64("joypad-command-active-and-enabled?"), 1, &c);
    return r != ~0ULL && (r & 0xff);
}

static void WalkkeyTick(void)
{
    static int off = -1;
    static uint64_t off_at;
    static int toggled, tog_prev, on_prev;
    if (FeelFlag("t2r-no-walk-key.txt", &off_at, &off, 180)) {
        toggled = 0;
        on_prev = 0;
        return;
    }
    int live = MatchSubstate() == 6 && !g_downed && !g_downDying
        && !g_uc4DyingUntil && !(g_stun_until && g_frame < g_stun_until)
        && GameFocused();
    int active = 0;
    if (live) {
        uint64_t none = 0, mode =
            DcCall(FNV64("get-partial-move-input-mode"), 0, &none);
        if (mode != ~0ULL && mode == FNV64("toggle-mode")) {
            int t = WalkkeyCmd("partial-move-toggle");
            if (t && !tog_prev)
                toggled = !toggled;
            tog_prev = t;
            active = toggled;
        } else {
            toggled = 0;
            tog_prev = 0;
            active = WalkkeyCmd("partial-move-hold");
        }
    } else {
        toggled = 0;
        tog_prev = 0;
    }
    if (active != on_prev) {
        on_prev = active;
        Xlog("feel: walk key %s (move stick scale 0.85, like SP's partial movement)", active ? "held" : "released");
    }
    if (!active)
        return;
    float sc = 0.85f;
    uint64_t ss[2] = { FNV64("move"), 0 };
    memcpy(&ss[1], &sc, sizeof sc);
    DcCall(FNV64("joypad-set-stick-scale"), 2, ss);
}

// ---------------------------------------------------------------- 3. Hit reactions, VISUAL ONLY
// The heavy hit stagger is visual only and does not restrict player movement at all (the SP wound grab). The reaction is an ADDITIVE UPPER-BODY
// GESTURE (gesture_ + alloc-gesture-play-params, the layer the cough and the pickup / revive hand-out already use): no root motion, no input lock, no
// speed change, the locomotion keeps running underneath. The clips are the game's own player-hit-reactions.bin entries of animation type 3 (gesture,
// "gesture-ellie-idle-ub-hr-<zone>-add" + the crouch set) and type 2 (the PvP additives mp-lg-hr-hi-*--add); the full-body SP clips (ellie-run-fw-fb-hr-*,
// the actual run + grab-the-wound ones) cannot be masked from here and are not played; the PvP additives (mp-lg-hr-hi-*--add) are not gesture defs
// and no pak we know of holds them, so they are not played either (re/hit-reactions.md has the decoded lists and the reasons).
// The VICTIM decides everything (it knows the shot, its stance and its speed): recv_hit_hook -> hrx_victim_hit queues the hit, hitreact_tick (t2r_tick
// context) picks zone / direction / stance, plays the gesture on our own body and tells everyone with MSG_HITREACT [u8 clip id][u8 flags], which plays
// the same clip on our puppet (hrx_recv). Off: t2r-no-hit-react.txt (everything), t2r-hit-react-no-net.txt (own body only, nothing sent / played on
// puppets). Never during executions / downed / paired moves / stun / revive / the TAB backpack / a crossbow bolt. Log prefix "hitreact:".
#define HRX_COOLDOWN 0.25f      // seconds between two reactions on one puppet (the victim's own screen already spaces them, HRX_GRAZE_DELAY)
// 2013 PvP hit reactions are the additive `mp-*-hr-*--add` entries; after each one the engine arms *shot-reaction-grazing-protection-delay* = 1.0 s (PS4 v1.11
// ReactToAttack 0x540400 -> FUN_00a75080(delay, player+0x128); the 2.3 s / 3.9 s (crouch) protection belongs to the full-body list, 0x53fd80): no second
// reaction on the victim's body for 1.0 s, heavy or light (re/factions2013/ps4/logic/gunplay.md section 5). Was 0.25 s heavy / 0.7 s light.
#define HRX_GRAZE_DELAY 1.0f
#define HRX_LIGHT_GAP HRX_GRAZE_DELAY   // light hits (pistol / SMG / rifle bullets): same protection
#define HRX_CLEAR_AT 0.50f      // a clip longer than HRX_MAX_LEN is let go at this age ...
#define HRX_CLEAR_BLEND 0.25f   // ... with this blend-out
#define HRX_MAX_LEN 0.65f
enum { HZ_FL_SHLDR, HZ_FR_SHLDR, HZ_FL_HIP, HZ_FR_HIP, HZ_STOMACH,
        HZ_BL_SHLDR, HZ_BR_SHLDR, HZ_BL_HIP, HZ_BR_HIP, HZ_BACK_HIGH,
        HZ_BACK_LOW, HZ_SIDE_L, HZ_SIDE_R, HZ_N };
// clip ids (sent over the wire): 0..12 standing / moving upper-body additives by zone, 13..18 crouched ones. Each is a gesture-def of
// gestures/ellie-gestures.bin (blend-in 0.10 s, blend-out 0.50 s in the def); the clip it plays is the name without "gesture-"
// (anim-ellie-player-movement-combat.pak: ellie-idle-ub-hr-<zone>-add, ellie-idle-crouch-ub-hr-<zone>-add)
static const char *const HRX_CLIP[] = {
    "gesture-ellie-idle-ub-hr-front-l-shldr-add",
        "gesture-ellie-idle-ub-hr-front-r-shldr-add",
        "gesture-ellie-idle-ub-hr-front-l-hip-add",
    "gesture-ellie-idle-ub-hr-front-r-hip-add",
        "gesture-ellie-idle-ub-hr-front-stomach-add",
        "gesture-ellie-idle-ub-hr-back-l-shldr-add",
    "gesture-ellie-idle-ub-hr-back-r-shldr-add",
        "gesture-ellie-idle-ub-hr-back-l-hip-add",
        "gesture-ellie-idle-ub-hr-back-r-hip-add",
    "gesture-ellie-idle-ub-hr-back-high-add",
        "gesture-ellie-idle-ub-hr-back-low-add",
        "gesture-ellie-idle-ub-hr-side-l-shldr-add",
    "gesture-ellie-idle-ub-hr-side-r-shldr-add",
    "gesture-ellie-idle-crouch-ub-hr-front-l-shldr-add",
        "gesture-ellie-idle-crouch-ub-hr-front-r-shldr-add",
    "gesture-ellie-idle-crouch-ub-hr-back-l-shldr-add",
        "gesture-ellie-idle-crouch-ub-hr-back-r-shldr-add",
    "gesture-ellie-idle-crouch-ub-hr-side-l-hip-add",
        "gesture-ellie-idle-crouch-ub-hr-side-r-hip-add"
};

#define HRX_N ((int)(sizeof HRX_CLIP / sizeof *HRX_CLIP))
#define HRX_CROUCH0 13
static const char *const HRX_ZONE_NM[HZ_N] =
    { "front-l-shldr", "front-r-shldr", "front-l-hip", "front-r-hip",
"front-stomach", "back-l-shldr", "back-r-shldr",
    "back-l-hip", "back-r-hip", "back-high", "back-low", "side-l", "side-r"
};
static const char *const HRX_DIR_NM[4] =
    { "front", "behind", "left", "right" };
// heavy weapons: the bigger reaction every time (shotgun, hunting / sniper rifle, revolver, El Diablo, Frontier, bow; melee too).
// The crossbow has its own bolt-in-the-shoulder reaction (xb_*) and is left out.
static const char *HrxClipOf(int id)
{
    return HRX_CLIP[id] + 8;
}                               // the anim behind the gesture def: "gesture-" cut off

static int HrxHeavy(uint64_t w)
{
    static const char *const H[] =
        { "shotgun-remington-pump", "shotgun-baserri-over-under",
"shotgun-tactical", "shotgun-shorty", "rifle-remington-bolt",
        "rifle-milsniper", "pistol-revolver-taurus", "pistol-el-diablo",
            "rifle-frontier", "bow-ellie"
    };
    uint64_t b = BaseWeapon(w);
    for (size_t i = 0; i < sizeof H / sizeof *H; i++)
        if (FNV64(H[i]) == b)
            return 1;
    return !BaseWeaponIsGun(b);
}

static uint32_t HrxRnd(void)
{
    static uint32_t s;
    if (!s)
        s = (uint32_t) GetTickCount64() | 1;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

// our own movement speed (m/s, real time) from the position we sample every tick: running = the SP "run" reactions' stance
static struct {
    float x, z, speed;
    uint64_t ms;
    int ok;
} g_hrxMv;
static void HrxSampleMove(void)
{
    float lb[16];
    const float *l = Uc4MyLoc(lb);
    uint64_t now = GetTickCount64();
    if (!l) {
        g_hrxMv.ok = 0;
        return;
    }
    if (g_hrxMv.ok && now > g_hrxMv.ms && now - g_hrxMv.ms < 400) {
        float dt = (float) (now - g_hrxMv.ms) * 0.001f, dx =
            l[0] - g_hrxMv.x, dz = l[2] - g_hrxMv.z;
        float v = __builtin_sqrtf(dx * dx + dz * dz) / dt;
        if (v < 15.0f)
            g_hrxMv.speed += (v - g_hrxMv.speed) * 0.5f;      // (a teleport / respawn is not a run)
    } else
        g_hrxMv.speed = 0;
    g_hrxMv.x = l[0];
    g_hrxMv.z = l[2];
    g_hrxMv.ms = now;
    g_hrxMv.ok = 1;
}

// per body (0..3 puppets, 4 = us)
static struct {
    uint64_t pend_frame, last, clear_at, ent;
    int pend_id, pend_flags, cur;
    uint64_t last_light;
} g_hrx[T2R_MAX_PEERS + 1];
static struct {
    uint64_t frame, w;
    uint32_t slot;
    float dmg;
} g_hrxOwn;                    // the queued hit on us
static uint64_t g_hrxProbeAt[2][HRX_N];
static int8_t g_hrxHave[2][HRX_N];     // [puppet / own][clip]: 0 unknown, 1 ok, -1 missing (re-probed every 20 s)
static int HrxHave(uint64_t ent, int own, int id)
{
    if (g_hrxHave[own][id]
        && g_frame - g_hrxProbeAt[own][id] < SecsFrames(20.0f))
        return g_hrxHave[own][id] > 0;
    float len = AnimLen(ent, HrxClipOf(id));
    int8_t was = g_hrxHave[own][id];
    g_hrxHave[own][id] = len > 0 ? 1 : -1;
    g_hrxProbeAt[own][id] = g_frame ? g_frame : 1;
    if (was != g_hrxHave[own][id])
        Xlog("hitreact: clip %s (gesture %s) on %s: %s (%.2f s)",
             HrxClipOf(id), HRX_CLIP[id], own ? "our body" : "a puppet",
             len > 0 ? "available" : "NOT available", len);
    return len > 0;
}

static int HrxBusyOwn(void)
{                               // the layers / states a hit reaction must never touch on our body
    return g_downed || g_downDying || g_uc4DyingUntil || (g_stun_until
                                                             && g_t60 <
                                                             g_stun_until)
        || PsyncActive() || (g_execUntil && g_t60 < g_execUntil)
        || FinisherGuardOn() || GxVictimActive()
        || BpOwnPhaseNow() != 0 || g_rg[T2R_US].on || g_revBy != 99
        || g_xb.on || MatchSubstate() != 6;
}

static int HrxBusyPeer(uint32_t sl)
{
    if (g_peerDowned[sl] || g_puppetDeadHere[sl] || PsyncBusySlot(sl)
        || GxBusySlot(sl) || g_stunPupAt[sl] || g_rg[sl].on
        || g_bpp[sl].clip != BPC_NONE)
        return 1;
    if (g_uc4ReachPupUntil[sl] && g_t60 < g_uc4ReachPupUntil[sl])
        return 1;
    return 0;
}

static uint64_t HrxPlay(uint64_t ent, const char *clip)
{
    uint64_t none = 0;
    uint64_t pp = DcCall(FNV64("alloc-gesture-play-params"), 0, &none);
    if (!pp || pp == ~0ULL || IsBadReadPtr((void *) pp, 0x90))
        return ~0ULL;           // (defaults only: rg_play's crash note)
    ((uint8_t *) pp)[0x20] = 0;
    ((uint8_t *) pp)[0x21] = 0;
    uint64_t a[3] = { ent, FNV64(clip), pp };
    return DcCall(FNV64("gesture_"), 3, a);
}

// which clip: zone x direction x stance. Preference list, the first one the body knows is played.
static int HrxPick(uint64_t ent, int own, int crouched, int running,
                    int dir, int lat_left, int heavy, const char **why)
{
    int c[2], n = 0, z;
    uint32_t r = HrxRnd() % 100;
    switch (dir) {
    case 0:                    // from the front: heavy = the shoulder (mostly), light = hip / stomach
        z = heavy ? (r <
                     70 ? (lat_left ? HZ_FL_SHLDR : HZ_FR_SHLDR) :
                     HZ_STOMACH) : (r <
                                    45 ? (lat_left ? HZ_FL_HIP : HZ_FR_HIP)
                                    : r <
                                    75 ? HZ_STOMACH : (lat_left ?
                                                       HZ_FL_SHLDR :
                                                       HZ_FR_SHLDR));
        break;
    case 1:                    // from behind: the shoulder reach for heavy hits (the wound grab while running), hips / low back for light ones
        z = heavy ? (r < 75
                     || running ? (lat_left ? HZ_BL_SHLDR : HZ_BR_SHLDR) :
                     HZ_BACK_HIGH) : (r <
                                      60 ? (lat_left ? HZ_BL_HIP :
                                            HZ_BR_HIP) : HZ_BACK_LOW);
        break;
    case 2:
        z = HZ_SIDE_L;
        break;
    default:
        z = HZ_SIDE_R;
        break;
    }
    int id = z;
    if (crouched) {             // the crouch set only has these six
        switch (z) {
        case HZ_FL_SHLDR:
        case HZ_FL_HIP:
            id = HRX_CROUCH0 + 0;
            break;
        case HZ_FR_SHLDR:
        case HZ_FR_HIP:
        case HZ_STOMACH:
            id = HRX_CROUCH0 + 1;
            break;
        case HZ_BL_SHLDR:
        case HZ_BL_HIP:
        case HZ_BACK_HIGH:
            id = HRX_CROUCH0 + 2;
            break;
        case HZ_BR_SHLDR:
        case HZ_BR_HIP:
        case HZ_BACK_LOW:
            id = HRX_CROUCH0 + 3;
            break;
        case HZ_SIDE_L:
            id = HRX_CROUCH0 + 4;
            break;
        default:
            id = HRX_CROUCH0 + 5;
            break;
        }
    }
    c[n++] = id;
    if (crouched)
        c[n++] = z;             // the standing one over a crouched body, if the crouch set is not loaded
    *why = HRX_ZONE_NM[z];
    for (int i = 0; i < n; i++)
        if (HrxHave(ent, own, c[i]))
            return c[i];
    return -1;
}

static int HrxOff(void)
{
    static int off = -1;
    static uint64_t at;
    return FeelFlag("t2r-no-hit-react.txt", &at, &off, 180);
}

static int HrxNonet(void)
{
    static int off = -1;
    static uint64_t at;
    return FeelFlag("t2r-hit-react-no-net.txt", &at, &off, 180);
}

static void HrxSend(int id, int flags)
{
    if (HrxNonet())
        return;
    if (!g_sendTramp)
        return;
    uint8_t b[2] = { (uint8_t) id, (uint8_t) flags };
    g_sendTramp(MSG_HITREACT, b, 2, 0xff);
}

static void HrxClearDue(int w)
{
    if (!g_hrx[w].clear_at || g_frame < g_hrx[w].clear_at)
        return;
    g_hrx[w].clear_at = 0;
    if (w == T2R_US ? (g_stun_until && g_t60 < g_stun_until)
        || g_rg[T2R_US].on || BpOwnPhaseNow() : (g_stunPupAt[w]
                                                    || g_rg[w].on
                                                    || g_bpp[w].clip !=
                                                    BPC_NONE))
        return;                 // another gesture owns the layer now
    uint64_t ent = w == T2R_US ? SID_PLAYER : PuppetHandle((uint32_t) w);
    if (!ent || ent != g_hrx[w].ent)
        return;
    uint64_t c[4] = { ent, (uint64_t) - 1LL, 0, 0 };
    float bl = HRX_CLEAR_BLEND;
    memcpy(&c[2], &bl, 4);
    DcCall(FNV64("clear-gesture"), 4, c);
}

// play clip `id` on body w (0..3 puppet, 4 us); 1 = started
static int HrxStart(int w, uint64_t ent, int id, int heavy,
                     const char *how)
{
    float len = AnimLen(ent, HrxClipOf(id));
    uint64_t r = HrxPlay(ent, HRX_CLIP[id]);
    g_hrx[w].last = g_frame ? g_frame : 1;
    g_hrx[w].ent = ent;
    g_hrx[w].cur = id;
    g_hrx[w].clear_at =
        len > HRX_MAX_LEN ? g_frame + SecsFrames(HRX_CLEAR_AT) : 0;
    static int nl;
    if (LOGCAP(nl, 60))
        Xlog("hitreact: %s %s clip %s (%.2f s%s) layer gesture, additive upper body, no root / input / speed -> %llx", how, heavy ? "heavy" : "light", HRX_CLIP[id], len, g_hrx[w].clear_at ? ", released at 0.5 s / 0.25 s blend-out" : "", (unsigned long long) r);
    return r != ~0ULL;
}

// called from recv_hit_hook (a hit that reached us): queue it, the tick plays it
static void HrxVictimHit(uint32_t slot, uint64_t w, float dmg,
                           float hp_before, float hp_after)
{
    // melee v3: a blow of a bubble member (the sim's confirmed hit, or a stray engine one) - no stagger lock / SP reaction (melee_stagger.c, melee_hitreact.c),
    // camera jolt + overlay (melee_feel.c, melee_hitfx.c) or additive flinch here: mv3_anim plays the sim's reaction, mv3_fx its sound / blood / camera
    if (Mv3StandBlow
        ("victim: stagger + SP reaction + camera jolt + overlay + additive flinch",
         slot, w) || g_mv3Applying)
        return;
    w = FkrVictimW(slot, w);  // v5-knife (t2r-ext.c): a female's knife blow arrives as the fists' id - the stagger / SP reaction / jolt / hit fx below take it as her knife
    if (!(MduVicHold(slot, w, dmg, hp_before, hp_after) && hp_after > 0.0f))  // melee_duel.c (v5-var: a PLAIN duel blow - no pair - gets the SP reaction): the duel partner's blow plays its synced pair instead of the stagger / SP flinch
        MhstunNote(slot, w, dmg, hp_before, hp_after); // melee_stagger.c: the interrupt (independent of the visual flinch switch)
    MfkVictimHit(slot, w, dmg, hp_before, hp_after);  // melee_feel.c: the SP hit jolt on our camera (independent of both)
    if (HrxOff())
        return;
    if (!(dmg > 0.0f) || dmg >= 150.0f || !w || slot >= T2R_MAX_PEERS)
        return;
    {                           // aim-punch audit: the only aim disturbance a hit gives is the engine's hit-reaction aim wobble (one spring, refreshed per hit);
        // our gesture clips are visual-only and no camera shake runs while we are shot. Per hit: the multiplier the engine applies now, hits in the last second.
        static uint64_t hist[8];
        static int hn, nl;
        uint64_t t = GetTickCount64();
        int in1s = 1;
        for (int i = 0; i < 8; i++)
            if (hist[i] && t - hist[i] < 1000)
                in1s++;
        hist[hn++ & 7] = t;
        if (LOGCAP(nl, 300))
            Xlog("aimpunch: hit by slot %u weapon %llx dmg %.0f - engine aim wobble x%.3f (Sharpshooter %d; no skill x%.2f, stock record x0.50), %d hit(s) in 1 s share ONE wobble, our camera shake: none while shot", slot, (unsigned long long) w, dmg, (double) g_skWobbleNow, Skl(SK_SHARP), (double) SK_WOB_BASE, in1s);
    }
    if (!(hp_after > 0.0f) || hp_before - hp_after < 0.5f)
        return;                 // dead / downed by it, or the vest took all of it (armor-feedback-2013.md)
    if (!BaseWeaponIsGun(BaseWeapon(w)) && MeleeClassOf(w, NULL) < 0
        && !MhsIsBlow(w))
        return;                 // explosions, molotov, fall: not a bullet / blow (fists are a blow: mhs_is_blow)
    if (XbWeapon(w))
        return;                 // the crossbow has its own bolt reaction
    if (!BaseWeaponIsGun(BaseWeapon(w)) && MhrCoversBlow()) {
        static int nc;
        if (LOGCAP(nc, 20))
            Xlog("hitreact: melee blow from slot %u - the SP full-body reaction covers it, no additive flinch", slot);
        return;
    }                           // melee_hitreact.c: ONE reaction system
    g_hrxOwn.frame = g_frame ? g_frame : 1;
    g_hrxOwn.w = w;
    g_hrxOwn.slot = slot;
    g_hrxOwn.dmg = dmg;
}

static void HrxOwnTick(void)
{
    if (!g_hrxOwn.frame)
        return;
    uint64_t pf = g_hrxOwn.frame, w = g_hrxOwn.w;
    uint32_t slot = g_hrxOwn.slot;
    float dmg = g_hrxOwn.dmg;
    g_hrxOwn.frame = 0;
    if (g_frame - pf > SecsFrames(0.2f))
        return;
    const char *skip = NULL;
    int heavy = HrxHeavy(w), melee_blow =
        !BaseWeaponIsGun(BaseWeapon(w))
        && (MeleeClassOf(w, NULL) >= 0 || MhsIsBlow(w));
    if (HrxBusyOwn())
        skip =
            "busy (execution / downed / paired / stun / revive / backpack / bolt)";
    // players saw no animations at all while getting hit: a melee blow that got no SP clip (mid swing / dodging / graze / immune window)
    // showed NOTHING - the 1.0 s gun-hit gaps below swallowed its additive flinch too. A melee blow's flinch only waits 0.25 s (cosmetic, upper body, no lock).
    else if (melee_blow && g_hrx[T2R_US].last
             && g_frame - g_hrx[T2R_US].last < SecsFrames(0.25f))
        skip = "melee flinch 0.25 s after the last";
    else if (!melee_blow && g_hrx[T2R_US].last
             && g_frame - g_hrx[T2R_US].last <
             SecsFrames(HRX_GRAZE_DELAY))
        skip = "2013 grazing protection (1.0 s since the last reaction)";
    else if (!melee_blow && !heavy && g_hrx[T2R_US].last_light
             && g_frame - g_hrx[T2R_US].last_light <
             SecsFrames(HRX_LIGHT_GAP))
        skip = "light hit, one flinch per 1.0 s";
    if (skip) {
        static int ns;
        if (LOGCAP(ns, 30))
            Xlog("hitreact: hit by slot %u (weapon %llx, dmg %.0f) - no reaction: %s", slot, (unsigned long long) w, dmg, skip);
        return;
    }
    float vb[16], ab[16];
    const float *vl = Uc4MyLoc(vb);
    uint64_t ah = PuppetHandle(slot);
    const float *al = ah ? FeelLoc(ah, ab) : NULL;
    if (!vl || !al) {
        static int nn;
        if (LOGCAP(nn, 20))
            Xlog("hitreact: hit by slot %u - no locator (us %d, attacker %d), no reaction", slot, vl != NULL, al != NULL);
        return;
    }
    int dir = DirAround(vl, al);
    float fx = 2 * (vl[4] * vl[6] + vl[7] * vl[5]), fz =
        1 - 2 * (vl[4] * vl[4] + vl[5] * vl[5]);
    float dx = al[0] - vl[0], dz = al[2] - vl[2], side = fz * dx - fx * dz;     // > 0: the attacker is on our left
    uint64_t none = 0, cr =
        DcCall(FNV64("player-is-crouched?"), 0, &none);
    int crouched = cr != ~0ULL && (cr & 0xff), running = g_hrxMv.ok
        && g_hrxMv.speed > 2.5f * SdMul();
    const char *zn = "?";
    int id =
        HrxPick(SID_PLAYER, 1, crouched, running, dir, side > 0, heavy,
                 &zn);
    if (id < 0) {
        static int nm;
        if (LOGCAP(nm, 12))
            Xlog("hitreact: hit by slot %u (%s, dmg %.0f): zone %s dir %s stance %s - no clip available on our body", slot, heavy ? "heavy" : "light", dmg, zn, HRX_DIR_NM[dir], crouched ? "crouch" : running ? "run" : "stand");
        return;
    }
    Xlog("hitreact: hit by slot %u weapon %llx dmg %.0f - zone %s dir %s stance %s (%.1f m/s) -> clip id %d", slot, (unsigned long long) w, dmg, zn, HRX_DIR_NM[dir], crouched ? "crouch" : running ? "run" : "stand", g_hrxMv.speed, id);
    if (!HrxStart(T2R_US, SID_PLAYER, id, heavy, "own body"))
        return;
    if (!heavy)
        g_hrx[T2R_US].last_light = g_frame ? g_frame : 1;
    HrxSend(id, heavy);
}

// MSG_HITREACT from slot: that player took a hit - the same clip on its puppet
static void HrxRecv(uint32_t slot, const uint8_t *buf, uint64_t len)
{
    if (slot >= T2R_MAX_PEERS || !buf || len < 2 || buf[0] >= HRX_N)
        return;
    if (HrxOff() || HrxNonet())
        return;
    g_hrx[slot].pend_frame = g_frame ? g_frame : 1;
    g_hrx[slot].pend_id = buf[0];
    g_hrx[slot].pend_flags = buf[1];
}

static void HrxPeerTick(uint32_t sl)
{
    if (!g_hrx[sl].pend_frame)
        return;
    uint64_t pf = g_hrx[sl].pend_frame;
    int id = g_hrx[sl].pend_id, heavy = g_hrx[sl].pend_flags & 1;
    g_hrx[sl].pend_frame = 0;
    if (g_frame - pf > SecsFrames(0.25f))
        return;
    uint64_t h = PuppetHandle(sl);
    const char *skip = NULL;
    if (!h)
        skip = "no puppet";
    else if (MatchSubstate() != 6)
        skip = "not in a match";
    else if (HrxBusyPeer(sl))
        skip =
            "puppet downed / dead / paired move / stun / revive / backpack";
    else if (g_hrx[sl].last
             && g_frame - g_hrx[sl].last < SecsFrames(HRX_COOLDOWN))
        skip = "cooldown";
    else if (!HrxHave(h, 0, id))
        skip = "clip not available on the puppet";
    if (skip) {
        static int ns;
        if (LOGCAP(ns, 30))
            Xlog("hitreact: slot %u took a hit (clip id %d) - not shown: %s", sl, id, skip);
        return;
    }
    HrxStart((int) sl, h, id, heavy, "puppet");
}

// the old attacker-side puppet reaction (an animate_ of the --add clip, off by default since the NPC's own reaction doubled it) is gone: the victim tells
static void FeelHitNotify(uint32_t slot, uint64_t weapon, float dmg)
{
    (void) slot;
    (void) weapon;
    (void) dmg;
}

static void HitreactTick(void)
{
    HrxSampleMove();
    if (HrxOff())
        return;
    HrxOwnTick();
    for (uint32_t sl = 0; sl < T2R_MAX_PEERS; sl++) {
        HrxPeerTick(sl);
        HrxClearDue((int) sl);
    }
    HrxClearDue(T2R_US);
}

static void FeelTick(void)
{
    if (!g_frame)
        return;
    BloodTick();
    WoundedGaitTick();
    InjuredOverlayTick();
    WalkkeyTick();
    HitreactTick();
}
