// sp_parity.c - make MP sound / behave like TLOU2 single player where the SP sandbox recorded the difference (re/sp-parity.md).
// Included by t2r-ext.c after footsteps.c (uses bp_me, match_substate, t2r_peer, xlog, exe_side_open, dc_call).
//
//  1. OWN FOLEY = SP PLAYER FOLEY (opt-out: t2r-sp-ownsteps-off.txt, checked every 2 s; switching it off restores the engine's configs).
//     SP (re/sp-sandbox-runs/*round3*: 6816 foot-effect sounds on the player): Abby's own steps, scuffs, plants, turns, landings, hand
//     touches and body falls are sfx-abby-<event>-<surface> (category foley-abby, mix-default -12 dB, the player's own recordings); the
//     enemies' are sfx-mlb-* (category foley-npc). EffectControl::FireEffect builds the name as "sfx-" + prefix + event + "-" + surface
//     (exe 0x14038bdfb, "sfx-%s%s-%s%s"), the prefix being +0x10 of the highest-priority mesh-audio-config of the body
//     (0x140389e90: EffectControl +0x218 config pointers, +0x2b8 types, +0x308 count <= 20, type 3 = backpack skipped). MP bodies are
//     NPC looks: every mesh says "mlb-", so OUR steps were NPC foley too - same events the enemies make, at the foley-npc mix the data
//     build raises for the 2013 enemy-step range (+8 dB, 2.25x distance: dc_data_patches.NPC_FOLEY_MIX), i.e. ~6 dB over SP's own steps.
//     Fix: only in OUR player's EffectControl, every config whose prefix is "mlb-" is pointed at a private copy saying "abby-" (all 87
//     mlb event kinds exist as abby events on every surface, same resident srf-* banks). Remote players keep the mlb set on everyone's
//     screen (SP: enemies sound like enemies). The copy keeps the mesh sid at +8, which is what OnPropRemoved (0x140393e78) matches.
//     Log: "sp parity: own foley ..." (each swap / restore, first 60 lines of all kinds).
//     Puppets' worn bag (opt-in: t2r-sp-puppet-bag.txt): SP enemies never play bag foley; our puppets replay player clips (bp-effect) and
//     their bag now has a config + bank (dc_data_patches.SP_BAG_FOLEY) - so their type-3 "backpack-*" config is pointed at a copy with
//     an empty prefix (no event: silent) on this screen. Log: "sp parity: puppet worn bag muted ...".
//  1b. OWN BAG FOLEY GATE (no SP sound difference on the bag rattle after the bag configs + banks landed). FireEffect's
//     bp-effect case (exe 0x14038b293) plays only for an object that IS a ProcessBackpack (the TAB prop) or whose has-backpack answer is
//     true (0x14038b2c1..0x14038b3e2: profile flag 0x141a8f9b0, then Player::HasBackpack = vtable 0x142cfc988 +0x3d0 = 0x140daf5d0,
//     directly or through the "has-backpack" event, whose handler 0x140b76dfa calls the same slot). HasBackpack asks the draw control
//     for the bag mesh of 0x140da7100: [player+0x5a60], else IsAbby ? "abby-backpack" : "ellie-backpack" - SP's player bags. An MP body
//     wears an NPC / rigged bag (militia-t2r-bp-rigged-11, scar-t2r-bp-rigged, militia-male-backpack-08 ...), never those two: the find
//     fails, HasBackpack is false (true only while the TAB prop is out) and EVERY bp-effect of our own clips (SP sandbox round 3: dodges,
//     landings, melee swings, stops / settles, pickups - 165 bag sounds) was dropped before the sound name was built; the type-3 configs
//     and the banks were never reached. Fix: the gate's `je` (0x14038b3e2, 6 bytes) -> nop6, one aligned 8-byte store (job threads run
//     FireEffect), so bp-effect always goes on to the prefix pick: the type-3 (worn bag) config -> sfx-backpack-<militia|abby>-<event>.
//     A body without a bag config gets the empty prefix ("sfx-bp-..." does not exist: silent, as before); the puppets' bag configs are
//     muted (above), so only OUR bag sounds, on the same clip frames as SP's. HasBackpack itself is untouched (its other users see what
//     they saw before). t2r-sp-bag-gate-off.txt next to the exe at the first match = not patched. Log: "sp parity: bag foley gate ...".
//     Diagnosis (always, cheap): own bp-effects FireEffect sees -> "sp parity: own bag foley #n ..." (first 24 per match), our
//     EffectControl's configs once per body -> "sp parity: own configs ..." (type 3 = the worn bag).
//  2. Global levels (always): 8 s into a match, is-level-loaded? of the exe's hard-wired global levels (hud: gbl-bullet, wpn-weapon-foley,
//     gbl-fight, gbl-blood, fol-cloth-*, srf-weapon-* ...; misc-fx; particle*) -> "sp parity: global levels ..." (the SP-vs-MP sound audit,
//     tools/sp_parity/sound_audit.py, assumes them loaded in MP too).
//  3. MP sound census (opt-in: t2r-sp-snd-census.txt at match start): the MP side of the sandbox's SND lines. The sound request build
//     0x141dd7630 (rdx = event sid, r8 = &owner handle) is probed; each distinct (owner class, event) is logged once per match:
//     "sp parity: snd own|puppet|other <sid>" (resolve with tools/sid.py; compare with the SP logs: tools/sp_parity/sound_audit.py --mp).
//     Also the anim effect events FireEffect sees (clipfx_fe_hook): "sp parity: fx own|puppet|other <event type sid>" (foot-effect,
//     bp-effect, haptic-effect, ...): whether MP bodies fire SP's bp-effect / haptic events at all.

#define SPP_PREFIX_OFF 0x10
#define SPP_EC_CFG 0x218
#define SPP_EC_N 0x308
#define SPP_CFG_SIZE 0x90
#define SPP_POOL 24
#ifndef SPP_FILE_MS
#define SPP_FILE_MS 2000        // opt-out file re-check period
#endif
#ifndef SPP_TICK_MS
#define SPP_TICK_MS 250         // own foley re-check period (a look rebuild re-adds the engine's configs)
#endif

// a pool of private copies of engine configs, each with another prefix (+0x10); never freed (static), so a slot pointing at one stays valid
typedef struct {
    const char *prefix;
    int n;
    const uint8_t *orig[SPP_POOL];
    __attribute__((aligned(16))) uint8_t copy[SPP_POOL][SPP_CFG_SIZE];
} SppPool;
static SppPool g_sppOwn = { "abby-" }, g_spp_mute = { "" }, g_spp_agi = { "" }, g_spp_agi_soft = { "abby-" }; // own foley: SP player set; puppets' worn bag: no prefix = silent

static int g_sppLogN;

static int SppFile(const char *name, int *cache, uint64_t *at,
                    uint64_t every_ms)
{
    uint64_t now = GetTickCount64();
    if (*cache < 0 || now - *at >= every_ms) {
        *at = now;
        FILE *f = ExeSideOpen(name, "r");     // (next to the exe, as every t2r-*.txt switch)
        int v = f != NULL;
        if (f)
            fclose(f);
        if (*cache >= 0 && v != *cache)
            Xlog("sp parity: %s %s", name, v ? "present" : "gone");
        *cache = v;
    }
    return *cache;
}

static int SppOwnstepsOff(void)
{
    static int c = -1;
    static uint64_t at;
    return SppFile("t2r-sp-ownsteps-off.txt", &c, &at, SPP_FILE_MS);
}

static int SppPuppetBagOn(void)
{
    static int c = -1;
    static uint64_t at;
    return SppFile("t2r-sp-puppet-bag.txt", &c, &at, SPP_FILE_MS);
}

static int SppInPool(const SppPool *P, const uint8_t *cfg)
{
    return cfg >= &P->copy[0][0] && cfg < &P->copy[0][0] + sizeof P->copy;
}

static uint8_t *SppCopyOf(SppPool *P, const uint8_t *orig)
{
    for (int i = 0; i < P->n; i++)
        if (P->orig[i] == orig) {       // (same address, other content = the DC module was reloaded: refresh the copy)
            if (memcmp(P->copy[i], orig, SPP_PREFIX_OFF)
                || memcmp(P->copy[i] + SPP_PREFIX_OFF + 8,
                          orig + SPP_PREFIX_OFF + 8,
                          SPP_CFG_SIZE - SPP_PREFIX_OFF - 8)) {
                memcpy(P->copy[i], orig, SPP_CFG_SIZE);
                *(const char **) (P->copy[i] + SPP_PREFIX_OFF) = P->prefix;
            }
            return P->copy[i];
        }
    if (P->n >= SPP_POOL)
        return NULL;
    uint8_t *c = P->copy[P->n];
    memcpy(c, orig, SPP_CFG_SIZE);
    *(const char **) (c + SPP_PREFIX_OFF) = P->prefix;
    P->orig[P->n++] = orig;
    return c;
}

static int SppPrefixIs(const uint8_t *cfg, const char *want, int whole)
{
    if (!cfg || IsBadReadPtr(cfg, SPP_CFG_SIZE))
        return 0;
    const char *p = *(const char *const *) (cfg + SPP_PREFIX_OFF);
    size_t n = strlen(want);
    if (!p || IsBadReadPtr(p, n + 1))
        return 0;
    return !memcmp(p, want, whole ? n + 1 : n);
}

// an object's EffectControl, checked (vtable = the game's EffectControl 0x142b27490, count in range)
static uint8_t *SppEcOf(uint8_t *obj)
{
    if (!obj || IsBadReadPtr(obj + 0x1c8, 8))
        return NULL;
    uint8_t *ec = *(uint8_t **) (obj + 0x1c8);
    if (!ec || IsBadReadPtr(ec, SPP_EC_N + 4))
        return NULL;
    if (*(uintptr_t *) ec !=
        (uintptr_t) GetModuleHandleW(NULL) + 0x2b27490) {
        static int warned;
        if (!warned++)
            Xlog("sp parity: an EffectControl with vtable exe+%llx, not exe+2b27490: left alone", (unsigned long long) (*(uintptr_t *) ec - (uintptr_t) GetModuleHandleW(NULL)));
        return NULL;
    }
    if (*(uint32_t *) (ec + SPP_EC_N) > 20)
        return NULL;
    return ec;
}

// every config of ec that `want` picks -> its copy in P (restore = 1: every copy of P back to the engine's config). Returns the swaps.
static int SppSwap(uint8_t *ec, SppPool *P, int restore,
                    int (*want)(const uint8_t *cfg, uint32_t type),
                    const char *what)
{
    int done = 0;
    uint32_t n = *(uint32_t *) (ec + SPP_EC_N);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t **slot = (uint8_t **) (ec + SPP_EC_CFG + 8 * i);
        uint8_t *cfg = *slot;
        if (!cfg)
            continue;
        if (restore) {
            if (!SppInPool(P, cfg))
                continue;
            int k = (int) ((cfg - &P->copy[0][0]) / SPP_CFG_SIZE);
            InterlockedExchangePointer((void *volatile *) slot,
                                       (void *) P->orig[k]);
            if (LOGCAP(g_sppLogN, 60))
                Xlog("sp parity: %s - slot %u back to the engine's config",
                     what, i);
            done++;
            continue;
        }
        uint32_t type = *(uint32_t *) (ec + 0x2b8 + 4 * i);
        if (SppInPool(&g_sppOwn, cfg) || SppInPool(&g_spp_mute, cfg)
            || SppInPool(&g_spp_agi, cfg)
            || SppInPool(&g_spp_agi_soft, cfg) || !want(cfg, type))
            continue;
        uint8_t *c = SppCopyOf(P, cfg);
        if (!c) {
            static int full;
            if (!full++)
                Xlog("sp parity: %s - copy pool full (%d), slot %u left",
                     what, SPP_POOL, i);
            continue;
        }
        InterlockedExchangePointer((void *volatile *) slot, c);
        if (LOGCAP(g_sppLogN, 60))
            Xlog("sp parity: %s - EffectControl %p slot %u/%u (mesh %016llx, type %u): %s -> \"%s\"", what, (void *) ec, i, n, (unsigned long long) *(uint64_t *) (cfg + 8), type, *(const char **) (cfg + SPP_PREFIX_OFF), P->prefix);
        done++;
    }
    return done;
}

static int SppWantMlb(const uint8_t *cfg, uint32_t type)
{
    (void) type;
    return SppPrefixIs(cfg, "mlb-", 1);
}

static int SppWantBag(const uint8_t *cfg, uint32_t type)
{
    return type == 3 && SppPrefixIs(cfg, "backpack-", 0);
}

static int SppWantFoley(const uint8_t *cfg, uint32_t type)
{
    return type != 3 && SppPrefixIs(cfg, "mlb-", 1);
}

// Agility II on a PUPPET (2013: "nearly impossible to hear while moving"): the engine's NPC foley of that body (steps, scuffs, plants,
// landings, hand touches: every "mlb-" config) is silenced on this screen unless he is sprinting (>= LISTEN_SPRINT_MPS, held 1 s) - the
// same split as the listen-mode reveal. Uses the peer's EFFECTIVE Agility (MSG_SK_SYNC byte 8: Lone Wolf's alone-grant included).
static struct {
    volatile uint8_t *obj;
    volatile uint8_t quiet;
    volatile uint8_t soft;
    volatile float pos[3];
    volatile uint8_t pos_ok;
} g_spp_agi_st[T2R_MAX_PEERS];  // per slot: its body + "quiet right now" + its position (read by FireEffect on job threads)
static volatile float g_sppListenPos[3];
static volatile uint8_t g_sppListenOk;        // our own position (the listener), set every tick
// An enemy heard a player despite running Agility 2: the old test took the speed over ONE 250 ms tick against 3.6 m/s. Puppet positions
// arrive at the network rate (steps of 0 / two ticks of travel), so a jogging player crossed 3.6 m/s now and then and every crossing un-muted the
// body for a full second (the log shows the mute swapped on / off every ~100-300 ms). Now: speed over a >= 0.6 s window of samples against the
// footsteps' own sprint boundary (g_fs_v_sprint, 4.6 m/s: the run / sprint split of the step tiers), hold 0.6 s. 2013 silent-running = quiet
// while moving; only a real sprint is audible (the task's rule).
#define SPP_AGI_WIN 8
// ---- Agility II silence, the other half (the characters were still loud: vaulting, walking, crouch walking, sprinting should be
// silent; falling quiet with only some of the impact audible). The prefix swap above only covers the foot-effect sounds (sfx-mlb-<heel|toe|lift|scuff|
// plant|turn|settle|slide|wall-*|bodyfall|dland>-<surface>). The SP sandbox's own SND lines (re/sp-sandbox-runs, round 3, re/agility2-silence-1019.md)
// show a SECOND per-step layer: the clothing rustle, gear-effect-upper / -lower "sfx-gear-move-(upper|lower)(-stealth)-<suffix>" (~1900 per 3600 foot
// events: it fires on every step / body movement), whose name comes from the config's SUFFIX strings, not the prefix - the swap never touched it.
// So FireEffect itself is filtered (clipfx_fe_hook asks spp_agility_fx_drop): for a quiet puppet every foot-effect except the landing "dland" and every
// gear-effect (except the sidearm draw / put-away events) is dropped before the engine builds the sound. A dead or downed body is left alone.
// t2r-agility-fxdrop-off.txt next to the exe = the previous behaviour (prefix mute only, no drop, no landing).
static int SppAgiSprintAudible(void)
{
    static int c = -1;
    static uint64_t at;
    return SppFile("t2r-agility-sprint-audible.txt", &c, &at,
                    SPP_FILE_MS);
}

static int SppAgiFxdropOn(void)
{                               // the FireEffect wrap must be installed (else dropping is impossible and the quiet set would only be -6 dB)
    static int c = -1;
    static uint64_t at;
    if (SppFile("t2r-agility-fxdrop-off.txt", &c, &at, SPP_FILE_MS))
        return 0;
    const uint64_t *slot =
        (const uint64_t *) ((uint8_t *) GetModuleHandleW(NULL) +
                            0x2b274d8);
    return !IsBadReadPtr(slot, 8)
        && *slot == (uint64_t) (uintptr_t) ClipfxFeHook;
}

static struct {
    volatile LONG foot, gear, grunt, land, touch;
    uint8_t was;
    uint64_t chg_at, sum_at;
} g_spp_agi_n[T2R_MAX_PEERS];
// one line per peer when the mute state changes (>= 1 s apart, 80 lines in all), and a 10 s tally of what the mute dropped (the A/B proof)
static void SppAgiReport(uint32_t s, int mode, uint64_t now)
{
    static LONG total, sums;
    if (s >= T2R_MAX_PEERS)
        return;
    if (mode != g_spp_agi_n[s].was && now - g_spp_agi_n[s].chg_at >= 1000
        && total < 80) {
        total++;
        Xlog("agility: slot %u foley %s (Agility II peer%s)", s,
             mode ? "MUTED" : "audible again",
             mode ==
             2 ?
             ": foot + cloth events dropped, landing impact kept at the quiet player level"
             : mode == 1 ? ": prefix mute only, no event drop" : "");
        g_spp_agi_n[s].was = (uint8_t) mode;
        g_spp_agi_n[s].chg_at = now;
    }
    if (!mode) {
        g_spp_agi_n[s].sum_at = 0;
        return;
    }
    if (!g_spp_agi_n[s].sum_at)
        g_spp_agi_n[s].sum_at = now;
    if (now - g_spp_agi_n[s].sum_at >= 10000) {
        g_spp_agi_n[s].sum_at = now;
        LONG f = InterlockedExchange(&g_spp_agi_n[s].foot, 0), g =
            InterlockedExchange(&g_spp_agi_n[s].gear, 0), v =
            InterlockedExchange(&g_spp_agi_n[s].grunt, 0), l =
            InterlockedExchange(&g_spp_agi_n[s].land, 0), t =
            InterlockedExchange(&g_spp_agi_n[s].touch, 0);
        if ((f || g || v || l || t) && sums++ < 300)
            Xlog("agility: slot %u quiet - last 10 s dropped %d foot-effects, %d cloth gear-effects, %d hand/arm touches, %d grunts; %d landing impacts let through", s, (int) f, (int) g, (int) t, (int) v, (int) l);
    }
}

static void SppAgilityQuiet(uint32_t s, uint64_t h, uint8_t *pec,
                              uint64_t now)
{
    static struct {
        uint64_t h, sprint_until;
        uint64_t t[SPP_AGI_WIN];
        float x[SPP_AGI_WIN], z[SPP_AGI_WIN];
        int n, head;
    } st[T2R_MAX_PEERS];
    if (s >= T2R_MAX_PEERS)
        return;
    float lb[16];
    const float *l = PuppetLocator(h, lb);
    if (st[s].h != h) {
        memset(&st[s], 0, sizeof st[s]);
        st[s].h = h;
    }
    if (l && __builtin_isfinite(l[0]) && __builtin_isfinite(l[2])) {
        int hd = st[s].head;
        st[s].t[hd] = now;
        st[s].x[hd] = l[0];
        st[s].z[hd] = l[2];
        st[s].head = (hd + 1) % SPP_AGI_WIN;
        if (st[s].n < SPP_AGI_WIN)
            st[s].n++;
        int cur = hd, oi = -1;
        for (int k = 1; k < st[s].n; k++) {     // the newest sample that is at least 0.6 s old
            int i = (hd + SPP_AGI_WIN - k) % SPP_AGI_WIN;
            if (now - st[s].t[i] >= 600) {
                oi = i;
                break;
            }
        }
        if (oi >= 0) {
            float dx = st[s].x[cur] - st[s].x[oi], dz =
                st[s].z[cur] - st[s].z[oi], dt =
                (float) (now - st[s].t[oi]) * 0.001f;
            float v = __builtin_sqrtf(dx * dx + dz * dz) / dt;
            if (v > g_fs_v_sprint * SdMul() && v < 15.0f)
                st[s].sprint_until = now + 600;
        }
        g_spp_agi_st[s].pos[0] = l[0];
        g_spp_agi_st[s].pos[1] = l[1];
        g_spp_agi_st[s].pos[2] = l[2];
        g_spp_agi_st[s].pos_ok = 1;
    } else
        g_spp_agi_st[s].pos_ok = 0;
    // 2013 Agility II is silent at EVERY
    // gait including the sprint. t2r-agility-sprint-audible.txt next to the exe = the previous rule (sprinting is heard).
    int quiet = SkPeer(s, SK_AGILITY) >= 2 && (!SppAgiSprintAudible()
                                                || now >=
                                                st[s].sprint_until);
    int soft = quiet && SppAgiFxdropOn();
    g_spp_agi_st[s].quiet = 0;
    g_spp_agi_st[s].obj =
        (volatile uint8_t *) ((void *(*)(uint64_t))
                              T2R_SYM(kFnEntityFromHandle))(h);
    g_spp_agi_st[s].soft = (uint8_t) soft;
    g_spp_agi_st[s].quiet = (uint8_t) quiet;
    // the config pointers of that body: quiet + fx drop = the quiet "abby-" set (only the landing impact survives the drop in the FireEffect
    // wrap, at the player-foley level: 6 dB under the NPC set and the shorter player range); quiet without the wrap = the empty prefix (every
    // foot sound silent, the earlier mute); not quiet = the engine's configs
    if (soft) {
        SppSwap(pec, &g_spp_agi, 1, SppWantFoley,
                 "Agility II puppet foley muted (quiet unless sprinting)");
        SppSwap(pec, &g_spp_agi_soft, 0, SppWantFoley,
                 "Agility II puppet foley quiet (landing impact only)");
    } else if (quiet) {
        SppSwap(pec, &g_spp_agi_soft, 1, SppWantFoley,
                 "Agility II puppet foley quiet (landing impact only)");
        SppSwap(pec, &g_spp_agi, 0, SppWantFoley,
                 "Agility II puppet foley muted (quiet unless sprinting)");
    } else {
        SppSwap(pec, &g_spp_agi, 1, SppWantFoley,
                 "Agility II puppet foley muted (quiet unless sprinting)");
        SppSwap(pec, &g_spp_agi_soft, 1, SppWantFoley,
                 "Agility II puppet foley quiet (landing impact only)");
    }
    SppAgiReport(s, quiet ? (soft ? 2 : 1) : 0, now);
}

// The movement effort GRUNTS of an Agility II puppet (2013 had no such grunts). Puppets replay
// SP player clips whose `voice-effect` anim events (effect type 0x36b7cf45b65a6305, FireEffect case 0x14038b61a; name pair = vox-eff-gen-...) make
// the body say the line (the MP voices got these lines from dc_data_patches.SP_VOX_SUBS). clipfx_fe_hook asks here before the engine fires one:
// 1 = drop it (a movement effort of a body whose peer is quiet - same state as the foley mute above). Pain / death / melee / callout lines never match.
static uint64_t g_sppEffort[256];
static int g_sppEffortN;
// the movement efforts of a body (vault / mantle (climb) / recover / land / jump / dodge / slide / stance changes) are the engine's
// voice-effect anim events; the list is the exact set of the game's sidbase.bin "vox-eff-gen-*" names of those actions (the old generated list
// held only 64 names - every "jump-*" and half of "land-*" - so vault / climb / dodge / recover grunts were never matched, neither by the Agility
// mute nor by anything else).
static const char *const SPP_EFFORT_NAMES[] = {
    "climb-0-5m", "climb-1-0m", "climb-1-5m", "climb-2-0m", "climb-finish",
        "climb-finish-relaxed", "climb-into", "climb-into-relaxed",
        "climb-into-sml",
    "climb-into-xsml", "climb-ledge", "climb-med", "climb-sml",
        "climb-strggl", "climb-struggle", "climb-struggle-relaxed",
        "climb-xsml",
    "crouch", "crouch-enter", "crouch-enter-combat", "crouch-exit",
        "crouch-exit-combat", "crouch-run", "crouch-run-combat",
    "dodge", "dodge-roll", "drop", "fight-dodge", "fight-dodge-hvy",
        "fight-dodge-lrg-long", "fight-dodge-lrg-short", "fight-dodge-med",
        "fight-dodge-sml",
    "fight-recover-med", "fight-recover-sml", "jump-lrg",
        "jump-lrg-combat", "jump-med", "jump-med-always-play",
        "jump-med-combat", "jump-small", "jump-sml",
    "jump-sml-combat", "jump-xsml", "land-injrd", "land-injure",
        "land-injured", "land-lrg", "land-lrg-combat", "land-lrg-panic",
        "land-med",
    "land-med-combat", "land-med-panic", "land-ml", "land-sml",
        "land-sml-combat", "land-sml-panic", "land-xsml",
        "lose-balance-med", "lose-balance-sml",
    "prone-crawl-exit", "prone-crawl-exit-combat", "prone-enter",
        "prone-enter-combat", "recover-lrg", "recover-sml", "slide",
        "slide-finish",
    "vault-crouch", "vault-jump", "vault-jump-combat", "vault-land",
        "vault-land-combat", "vault-sprint", "vault-stand",
        "vault-wounded",
    "vault-wounded-close", "vault-wounded-crouch", "wounded-recover-med",
        "wounded-recover-sml", "breath-exhausted-recover",
        "breath-sprint-recover-1",
    "breath-sprint-recover-2", "carry-stumble", "pull-into-sml",
        "climb-fatigued",
    // the rest of the movement voice events the SP player clips carry (anim-ellie-player-traversal / -movement / -movement-combat / -ladder /
    // -mm-*, anim-player-gun-shared: the prone crawl breaths (~40 events: every crawl step), the combat breaths, the throw effort)
    "prone-crawl-mouth-closed", "prone-crawl-mouth-open",
        "breath-fight-med", "breath-fight-sml", "throw-object",
        "swim-drowning-panicked"
};

static void SppEffortInit(void)
{
    if (g_sppEffortN)
        return;
    uint64_t t[256];
    int n = 0;
    for (size_t a = 0;
         a < sizeof SPP_EFFORT_NAMES / sizeof *SPP_EFFORT_NAMES && n < 256;
         a++) {
        char b[96];
        snprintf(b, sizeof b, "vox-eff-gen-%s", SPP_EFFORT_NAMES[a]);
        uint64_t h = Fnv64Rt(b);
        int dup = 0;
        for (int i = 0; i < n; i++)
            dup |= t[i] == h;
        if (!dup)
            t[n++] = h;
    }
    memcpy(g_sppEffort, t, sizeof t);
    MemoryBarrier();
    g_sppEffortN = n;
}

// How far a puppet's movement grunt is heard (they were heard from much farther than they should be, similar to running footsteps).
// The engine's voice-effect lines are dialogue-bus lines with a long carry (25-35 m measured, footsteps.c header); the footsteps of a runner are
// audible to ~20 m (foley-npc +8 dB, 2.25x). The per-event range is fixed in the data, so the event is not fired at all for a body farther
// than SPP_EFFORT_RANGE_M from us (t2r-effort-range.txt next to the exe: one number, metres, re-read every 2 s; 0 = no limit).
#ifndef SPP_EFFORT_RANGE_M
#define SPP_EFFORT_RANGE_M 20.0f
#endif
static float SppEffortRange(void)
{
    static float r = SPP_EFFORT_RANGE_M;
    static uint64_t at;
    uint64_t now = GetTickCount64();
    if (!at || now - at > 2000) {
        at = now;
        FILE *f = ExeSideOpen("t2r-effort-range.txt", "r");
        r = SPP_EFFORT_RANGE_M;
        if (f) {
            float v;
            if (fscanf(f, "%f", &v) == 1 && v >= 0.0f && v < 1000.0f)
                r = v;
            fclose(f);
        }
    }
    return r;
}

static int SppAgilityVoxDrop(uint8_t *obj, uint64_t name)
{
    if (!obj || !name)
        return 0;
    SppEffortInit();
    int hit = 0;
    for (int i = 0; i < g_sppEffortN && !hit; i++)
        hit = g_sppEffort[i] == name;
    if (!hit) {                 // A/B proof: every voice-effect a peer's body fires that is NOT a movement effort (pain / death / melee lines stay by design)
        static LONG nl;
        for (uint32_t s = 0; s < T2R_MAX_PEERS; s++)
            if ((uint8_t *) g_spp_agi_st[s].obj == obj
                && g_spp_agi_st[s].quiet
                && InterlockedIncrement(&nl) <= 40)
                Xlog("agility: voice-effect %016llx of slot %u kept (not a movement effort)", (unsigned long long) name, s);
        return 0;
    }
    for (uint32_t s = 0; s < T2R_MAX_PEERS; s++) {
        if ((uint8_t *) g_spp_agi_st[s].obj != obj)
            continue;
        if (g_spp_agi_st[s].quiet) {
            static int c = -1;
            static uint64_t at;
            if (!SppFile("t2r-agility-grunts.txt", &c, &at, SPP_FILE_MS)) {    // opt-out
                static LONG nl;
                if (InterlockedIncrement(&nl) <= 24)
                    Xlog("agility: dropped %016llx of slot %u (Agility II, not sprinting)", (unsigned long long) name, s);
                InterlockedIncrement(&g_spp_agi_n[s].grunt);
                return 1;
            }
        }
        float rng = SppEffortRange();
        if (rng > 0.0f && g_spp_agi_st[s].pos_ok && g_sppListenOk) {
            float dx = g_spp_agi_st[s].pos[0] - g_sppListenPos[0], dy =
                g_spp_agi_st[s].pos[1] - g_sppListenPos[1], dz =
                g_spp_agi_st[s].pos[2] - g_sppListenPos[2];
            float d = __builtin_sqrtf(dx * dx + dy * dy + dz * dz);
            if (d > rng) {
                static LONG nl;
                if (InterlockedIncrement(&nl) <= 24)
                    Xlog("effort range: dropped %016llx of slot %u at %.1f m (limit %.1f m)", (unsigned long long) name, s, (double) d, (double) rng);
                return 1;
            }
        }
        return 0;
    }
    return 0;
}

// The value of a clip event pair by key sid (weapon-effect names its event with `suffix`, not `name`: cfxe_entry_name never saw it)
static uint64_t SppFxPair(const CfxeEntry *e, uint64_t key)
{
    if (!e || IsBadReadPtr(e, sizeof *e) || !e->pairs || !e->n
        || IsBadReadPtr(e->pairs, 16 * (e->n < 64 ? e->n : 64)))
        return 0;
    for (uint32_t i = 0; i < e->n && i < 64; i++)
        if (e->pairs[i][0] == key)
            return e->pairs[i][1];
    return 0;
}

// clipfx_fe_hook's filter for a quiet puppet (see above): 1 = do not run FireEffect for this foot / cloth / hand-touch event
static uint8_t *SppHandleObj(const uint8_t * h);
static int SppAgilityFxDrop(const uint8_t *info)
{
    static uint64_t T_FOOT, T_GU, T_GL, T_WPN, T_HL, T_HR, T_AL, T_AR,
        T_BI, N_LAND, K_SUFFIX, W_TOUCH[6];
    if (!T_FOOT) {
        N_LAND = Fnv64Rt("dland");
        T_GU = Fnv64Rt("gear-effect-upper");
        T_GL = Fnv64Rt("gear-effect-lower");
        T_WPN = Fnv64Rt("weapon-effect");
        T_HL = Fnv64Rt("hand-effect-left");
        T_HR = Fnv64Rt("hand-effect-right");
        T_AL = Fnv64Rt("arm-effect-left");
        T_AR = Fnv64Rt("arm-effect-right");
        T_BI = Fnv64Rt("body-impact-effect");
        K_SUFFIX = Fnv64Rt("suffix");
        W_TOUCH[0] = Fnv64Rt("wpn-pat");
        W_TOUCH[1] = Fnv64Rt("wpn-lift");
        W_TOUCH[2] = Fnv64Rt("wpn-grab");
        W_TOUCH[3] = Fnv64Rt("wpn-brush");
        W_TOUCH[4] = Fnv64Rt("prone-wpn-pat");
        W_TOUCH[5] = Fnv64Rt("pat");
        MemoryBarrier();
        T_FOOT = Fnv64Rt("foot-effect");
    }
    uint64_t ty = *(const uint64_t *) (info + 0x30);
    // (steps are silent now, vaulting / climbing / landing / crawling still heard): the player clips' HAND / ARM touch events - the
    // slaps, lifts, grabs and brushes of every vault, climb, mantle, crouch plant and prone crawl - are not foot-effects. weapon-effect with a "wpn-*"
    // suffix ("sfx-weapon-unarmed-wpn-pat-<surface>", the SP census's 176 weapon-effect SND), hand-effect-*, arm-effect-* (prone-arm-pat / drag) and
    // body-impact-effect (wall-impact-body) played untouched: this filter only knew foot-effect and gear-effect.
    int foot = ty == T_FOOT, gear = ty == T_GU
        || ty == T_GL, touch = ty == T_HL || ty == T_HR || ty == T_AL
        || ty == T_AR || ty == T_BI, wpn = ty == T_WPN;
    if (!foot && !gear && !touch && !wpn)
        return 0;
    uint8_t *obj = SppHandleObj(info + 0x58);
    if (!obj)
        return 0;
    uint32_t s = 0;
    while (s < T2R_MAX_PEERS && (uint8_t *) g_spp_agi_st[s].obj != obj)
        s++;
    if (s >= T2R_MAX_PEERS || !g_spp_agi_st[s].quiet
        || !g_spp_agi_st[s].soft)
        return 0;
    if (g_puppetDeadHere[s] || g_peerDowned[s])
        return 0;               // a body that is dying / crawling keeps its sounds
    const CfxeEntry *e = *(const CfxeEntry * const *) (info + 0x388);
    if (wpn) {                  // weapon handling (grab / holster / reload parts) stays; the unarmed hand contacts go
        uint64_t sfx = SppFxPair(e, K_SUFFIX);
        int hit = 0;
        for (int k = 0; sfx && k < 6; k++)
            hit |= W_TOUCH[k] == sfx;
        if (!hit)
            return 0;
        InterlockedIncrement(&g_spp_agi_n[s].touch);
        return 1;
    }
    if (touch) {
        InterlockedIncrement(&g_spp_agi_n[s].touch);
        return 1;
    }
    uint64_t nm = e && !IsBadReadPtr(e, sizeof *e) && e->pairs && e->n
        && !IsBadReadPtr(e->pairs,
                         16 * (e->n <
                               64 ? e->n : 64)) ? CfxeEntryName(e) : 0;
    if (foot) {
        if (nm == N_LAND) {
            InterlockedIncrement(&g_spp_agi_n[s].land);
            return 0;
        }                       // the landing impact (quiet "abby-" set)
        InterlockedIncrement(&g_spp_agi_n[s].foot);
        return 1;
    }
    for (int k = 0; nm && k < SASFX_NK; k++)
        if (SASFX_NAMES[k].sid == nm)
            return 0;           // sidearm draw / put-away: weapon handling, not movement
    InterlockedIncrement(&g_spp_agi_n[s].gear);
    return 1;
}

static void SppOwnFoleyTick(void)
{
    static uint64_t next;
    uint64_t now = GetTickCount64();
    if (now < next)
        return;
    next = now + SPP_TICK_MS;
    uint8_t ms = MatchSubstate();
    if (ms < 3 || ms > 7) {
        for (int i = 0; i < T2R_MAX_PEERS; i++)
            g_spp_agi_st[i].quiet = 0;
        return;
    }
    uint8_t *ec = SppEcOf(BpMe());
    {                           // our position (the listener) for the effort range test
        float lb2[16];
        const float *lp = PlayerLocator(lb2);
        if (lp && __builtin_isfinite(lp[0]) && __builtin_isfinite(lp[2])) {
            g_sppListenPos[0] = lp[0];
            g_sppListenPos[1] = lp[1];
            g_sppListenPos[2] = lp[2];
            g_sppListenOk = 1;
        } else
            g_sppListenOk = 0;
    }
    {                           // once per EffectControl (= per body): what our configs are (the type-3 one = the worn bag, bp-effect's prefix)
        static uint8_t *dumped;
        if (ec && ec != dumped) {
            dumped = ec;
            char out[600];
            size_t at = 0;
            uint32_t n = *(uint32_t *) (ec + SPP_EC_N);
            for (uint32_t i = 0; i < n && at < 520; i++) {
                const uint8_t *cfg =
                    *(const uint8_t * const *) (ec + SPP_EC_CFG + 8 * i);
                const char *pre = cfg
                    && !IsBadReadPtr(cfg,
                                     SPP_CFG_SIZE) ? *(const char *const
                                                       *) (cfg +
                                                           SPP_PREFIX_OFF)
                    : NULL;
                at +=
                    snprintf(out + at, SNLEFT(out, at),
                             " [%u type %u mesh %016llx \"%s\"]", i,
                             *(uint32_t *) (ec + 0x2b8 + 4 * i),
                             cfg ? (unsigned long long) *(const uint64_t
                                                          *) (cfg +
                                                              8) : 0ULL,
                             pre && !IsBadReadPtr(pre, 1) ? pre : "?");
            }
            Xlog("sp parity: own configs (EffectControl %p, %u):%s",
                 (void *) ec, n, out);
        }
    }
    if (ec)
        SppSwap(ec, &g_sppOwn, SppOwnstepsOff(), SppWantMlb,
                 "own foley (SP player steps / landings / hands, foley-abby mix)");
    // puppets' worn bag: SP enemies never play bag foley (their clips have no bp-effect); our puppets replay player clips, whose
    // bp-effect now finds their bag's config + bank (dc_data_patches.SP_BAG_FOLEY) -> muted on this screen unless t2r-sp-puppet-bag.txt
    typedef uint8_t *(*fn_peer)(uint32_t);
    uint32_t me = *(uint32_t *) T2R_SYM(kMySlot);
    int on = SppPuppetBagOn();
    for (uint32_t s = 0; s < T2R_MAX_PEERS; s++) {
        uint8_t *peer = s != me ? ((fn_peer) T2rPeer) (s) : NULL;
        uint64_t h = peer ? *(uint64_t *) (peer + 0x60) : 0;
        uint8_t *obj =
            h ? (uint8_t *) ((void *(*)(uint64_t))
                             T2R_SYM(kFnEntityFromHandle))(h) : NULL;
        uint8_t *pec = SppEcOf(obj);
        if (pec)
            SppSwap(pec, &g_spp_mute, on, SppWantBag,
                     "puppet worn bag muted (SP: enemies have no bag foley)");
        if (pec)
            SppAgilityQuiet(s, h, pec, now);
        else if (g_spp_agi_st[s].obj) { // a peer that left / our own slot / a body that is gone must not keep its object pointer: FireEffect matches it on job threads and the engine may reuse the address (our own body)
            g_spp_agi_st[s].quiet = 0;
            g_spp_agi_st[s].soft = 0;
            g_spp_agi_st[s].obj = NULL;
            g_spp_agi_st[s].pos_ok = 0;
            SppAgiReport(s, 0, now);
        }
    }
}

// ---------------------------------------------------------------- 1b. own bag foley gate + watcher
#define SPP_BP_EFFECT 0x0f65d48cdf39b41bULL     // fnv64("bp-effect") (the t2r-ext.c fnv64_rt: FNV-1a 64, checked by test_sp_parity.py)
static volatile LONG g_sppCensus;      // (section 3)
static uint8_t *SppHandleObj(const uint8_t * h);
static volatile uint8_t *g_sppMe;      // our player object (set every tick; FireEffect runs on job threads)
static volatile LONG g_sppBpi;
static struct {
    volatile uint64_t name;
    volatile uint32_t seq;
} g_spp_bpr[16];
static void SppBagGateTick(void)
{
    static int done;
    if (done || MatchSubstate() != 6)
        return;
    done = 1;
    FILE *f = ExeSideOpen("t2r-sp-bag-gate-off.txt", "r");
    if (f) {
        fclose(f);
        Xlog("sp parity: bag foley gate NOT patched (t2r-sp-bag-gate-off.txt): own bp-effect plays only while the TAB prop is out");
        return;
    }
    uint8_t *exe = (uint8_t *) GetModuleHandleW(NULL);
    volatile uint64_t *w = (volatile uint64_t *) (exe + 0x38b3e0);      // 8-aligned: 84 f6 | 0f 84 57 2b 00 00 (test sil, sil ; je exit)
    static const uint8_t want[9] =
        { 0x40, 0x84, 0xf6, 0x0f, 0x84, 0x57, 0x2b, 0x00, 0x00 };
    static const uint8_t nop6[6] = { 0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00 };
    if (((uintptr_t) w & 7) || memcmp(exe + 0x38b3df, want, 9)) {
        Xlog("sp parity: bag foley gate %s",
             !memcmp(exe + 0x38b3e2, nop6,
                     6) ? "already patched" :
             "code differs - not patched (own bag foley stays gated by HasBackpack)");
        return;
    }
    uint64_t new8 = *w;
    memcpy((uint8_t *) & new8 + 2, nop6, 6);
    DWORD op;
    if (!VirtualProtect((void *) w, 8, PAGE_EXECUTE_READWRITE, &op)) {
        HOOK_FAIL("sp parity: bag foley gate not writable");
        return;
    }
    InterlockedExchange64((volatile LONG64 *) w, (LONG64) new8);
    VirtualProtect((void *) w, 8, op, &op);
    FlushInstructionCache(GetCurrentProcess(), (void *) w, 8);
    Xlog("sp parity: bag foley gate patched (exe 0x14038b3e2 je -> nop): bp-effect uses the worn bag's type-3 config like SP's player " "(HasBackpack only knows the abby- / ellie-backpack meshes)");
}

static void SppBpNote(const uint8_t *info)
{                               // (FireEffect, any thread) an own bp-effect -> ring
    if (!info || *(const uint64_t *) (info + 0x30) != SPP_BP_EFFECT)
        return;
    const uint8_t *me = (const uint8_t *) g_sppMe;
    if (!me || SppHandleObj(info + 0x58) != me)
        return;
    const CfxeEntry *e = *(const CfxeEntry * const *) (info + 0x388);
    uint64_t nm = e && !IsBadReadPtr(e, sizeof *e) && e->pairs && e->n
        && !IsBadReadPtr(e->pairs,
                         16 * (e->n <
                               64 ? e->n : 64)) ? CfxeEntryName(e) : 0;
    LONG i = InterlockedIncrement(&g_sppBpi);
    int k = i & 15;
    g_spp_bpr[k].seq = 0;
    g_spp_bpr[k].name = nm ? nm : 1;
    g_spp_bpr[k].seq = (uint32_t) i;
}

static void SppBpLogTick(void)
{
    static uint32_t last;
    static int n, was6;
    int in6 = MatchSubstate() == 6;
    if (in6 && !was6) {
        n = 0;
        last = (uint32_t) g_sppBpi;
    }
    was6 = in6;
    g_sppMe = in6 ? BpMe() : NULL;
    uint32_t end = (uint32_t) g_sppBpi;
    if (end - last > 16)
        last = end - 16;
    for (uint32_t q = last + 1; q - 1 != end; q++) {
        int k = q & 15;
        if (g_spp_bpr[k].seq != q || ++n > 24)
            continue;
        const char *pre = "?";
        uint8_t *ec = SppEcOf((uint8_t *) g_sppMe);
        uint32_t c = ec ? *(uint32_t *) (ec + SPP_EC_N) : 0;
        for (uint32_t i = 0; i < c; i++)
            if (*(uint32_t *) (ec + 0x2b8 + 4 * i) == 3) {
                const uint8_t *cfg =
                    *(const uint8_t * const *) (ec + SPP_EC_CFG + 8 * i);
                const char *p = cfg
                    && !IsBadReadPtr(cfg,
                                     SPP_CFG_SIZE) ? *(const char *const
                                                       *) (cfg +
                                                           SPP_PREFIX_OFF)
                    : NULL;
                pre = p && !IsBadReadPtr(p, 1) ? p : "?";
                break;
            }
        Xlog("sp parity: own bag foley #%d bp-effect name %016llx -> sfx-%s<name> (the type-3 config's prefix; \"?\" = no bag config)", n, (unsigned long long) g_spp_bpr[k].name, pre);
    }
    last = end;
}

static void SppFxSeen(const uint8_t * info);
static void SppFdPut(int kind, uint64_t sid, uint64_t ra);
static uint8_t *SppHandleObj(const uint8_t * h);
static void SppFxCb(const uint8_t *info)
{                               // clipfx_fe_hook's watcher: the bag notes always, the census while it is on
    SppBpNote(info);
    if (g_sppCensus)
        SppFxSeen(info);
    if (g_fire_diag_until && GetTickCount64() < g_fire_diag_until && info) {    // fire diagnosis: anim effect events on our body
        uint8_t *o = SppHandleObj(info + 0x58);
        if (o && o == BpMe())
            SppFdPut(1, *(const uint64_t *) (info + 0x30), 0);
    }
}

// ---------------------------------------------------------------- 2. global levels
static void SppGlobalLevelsTick(void)
{
    static uint64_t t0;
    static int done;
    if (MatchSubstate() != 6) {
        t0 = 0;
        done = 0;
        return;
    }
    uint64_t now = GetTickCount64();
    if (!t0) {
        t0 = now;
        return;
    }
    if (done || now - t0 < 8000)
        return;
    done = 1;
    static const char *const LV[] =
        { "hud", "misc-fx", "particle", "particle-shrub",
"particle-override", "rain-snow-lens-flare-mats" };
    char out[400];
    size_t at = 0;
    for (size_t i = 0; i < sizeof LV / sizeof LV[0]; i++) {
        uint64_t sid = FNV64(LV[i]);
        uint64_t l = DcCall(FNV64("is-level-loaded?"), 1, &sid);
        at +=
            snprintf(out + at, SNLEFT(out, at), " %s[%s]", LV[i],
                     (l && l != ~0ULL) ? "L" : "-");
    }
    Xlog("sp parity: global levels (L = loaded; their banks: gbl-bullet, wpn-weapon-foley, gbl-blood, fol-cloth-*, srf-weapon-* ...):%s", out);
}

// ---------------------------------------------------------------- 3. census (opt-in)
#define SPP_RING 512
static volatile LONG g_spp_ri, g_sppCensus;
static struct {
    volatile uint64_t sid;
    volatile uint8_t *obj;
    volatile uint32_t kind;
    volatile uint32_t seq;
} g_spp_ring[SPP_RING];
static volatile uint8_t *g_sppMe;
static volatile uint8_t *g_sppPup[T2R_MAX_PEERS];

static uint8_t *SppHandleObj(const uint8_t *h)
{                               // {ptr, u32 id} -> object ([ptr+8], id == [obj+0x94]); NULL when unreadable
    if (!h || IsBadReadPtr(h, 12))
        return NULL;
    const uint8_t *p = *(const uint8_t * const *) h;
    uint32_t id = *(const uint32_t *) (h + 8);
    if (!p || !id || IsBadReadPtr(p + 8, 8))
        return NULL;
    uint8_t *o = *(uint8_t * const *) (p + 8);
    if (!o || IsBadReadPtr(o + 0x94, 4) || *(uint32_t *) (o + 0x94) != id)
        return NULL;
    return o;
}

static void SppRingPut(uint32_t kind, uint64_t sid, const uint8_t *obj)
{
    if (!g_sppCensus || !sid)
        return;
    LONG i = InterlockedIncrement(&g_spp_ri);
    int k = i & (SPP_RING - 1);
    g_spp_ring[k].seq = 0;
    g_spp_ring[k].sid = sid;
    g_spp_ring[k].obj = (volatile uint8_t *) obj;
    g_spp_ring[k].kind = kind;
    g_spp_ring[k].seq = (uint32_t) i;
}

// sound request build probe (decal_diag_hook stub: fn(rcx, rdx, r8, r9, entry rsp))
static void ImmSndProbe(uint64_t sid, uint64_t r8);   // immersion.c: enemy gun shots (near miss / gunfire shake)
static void EntSndProbe(uint64_t sid, uint64_t r8, uint64_t * esp);   // t2r-ext.c (entry window sound trace + filter)
static void SlSndProbe(int kind, uint64_t sid, uint64_t r8, uint64_t * esp);  // t2r-ext.c (broad sound logger)
static void VxSndProbe(uint64_t sid, uint64_t r8);    // t2r-ext.c (vox): the engine's own dead-ally callouts (diagnosis)
// Reload shouts played on every reload, constantly - worst for the 1-ammo bow / crossbow / hunting rifle: the engine's own
// reload callouts. Our team callout (VX_RELOAD, immersion.c) fired once in the log while the player reloaded many times, so the shouts are the
// SP player voices' "Reloading" lines: dialogue 'abb-/ell-gen-reload-weapon-(sp|wh)-NNN' (sidbase.bin: Abby 17, Ellie 14 lines; no MP / NPC voice has
// one) requested by the vox events vox-event-player-reload-weapon-(sp|wh) / -player-reload-end(-wh) / -friendly-reload-weapon on every reload.
// The sound request build (FUN_141dd7630 = the SfxProcess constructor, rcx this, rdx = sound / line sid, r8 = &owner handle) cannot be skipped,
// so a matching request has its sid REPLACED in the register-saving stub's saved-rdx slot (stack: rax rcx rdx r8 r9 r10 r11 pushed in that
// order below the entry rsp, i.e. rdx = esp[-3]) by a sound that does not exist: the same outcome as an unloaded bank (silent, "sfx play failed").
// t2r-reload-vox.txt next to the exe = the old behaviour (checked every 2 s). Log: "vox: reload shout dropped <name> (owner)".
#define SPP_RLD_MAX 128
static uint64_t g_sppRld[SPP_RLD_MAX];
static const char *g_sppRldN[SPP_RLD_MAX];
static volatile LONG g_sppRldCnt;
static void SppRldInit(void)
{
    if (g_sppRldCnt)
        return;
    static const char *const EV[] =
        { "vox-event-player-reload-weapon-sp",
"vox-event-player-reload-weapon-wh", "vox-event-player-reload-end",
        "vox-event-player-reload-end-wh",
            "vox-event-friendly-reload-weapon"
    };
    static const char *const VOICE[] = { "abb-", "ell-" };
    int n = 0;
    for (size_t i = 0; i < sizeof EV / sizeof *EV; i++) {
        g_sppRldN[n] = EV[i];
        g_sppRld[n++] = Fnv64Rt(EV[i]);
    }
    for (int v = 0; v < 2; v++)
        for (int k = 0; k < 2; k++)
            for (int i = 1; i <= 10 && n < SPP_RLD_MAX; i++) {
                char b[64];
                snprintf(b, sizeof b, "%sgen-reload-weapon-%s-%03d",
                         VOICE[v], k ? "wh" : "sp", i);
                g_sppRldN[n] = _strdup(b);
                g_sppRld[n++] = Fnv64Rt(b);
            }
    MemoryBarrier();
    g_sppRldCnt = n;
}

static void SppReloadDrop(uint64_t rdx, uint64_t r8, uint64_t *esp)
{
    if (!esp || !rdx)
        return;
    SppRldInit();
    int n = (int) g_sppRldCnt, hit = -1;
    for (int i = 0; i < n; i++)
        if (g_sppRld[i] == rdx) {
            hit = i;
            break;
        }
    if (hit < 0)
        return;
    static int c = -1;
    static uint64_t at;
    if (SppFile("t2r-reload-vox.txt", &c, &at, SPP_FILE_MS))
        return;
    if (esp[-3] != rdx)
        return;                 // not the stub layout we expect: leave the request alone
    static uint64_t silent;
    if (!silent)
        silent = Fnv64Rt("t2r-silenced-reload-vox");
    esp[-3] = silent;
    static LONG nl;
    if (InterlockedIncrement(&nl) <= 30) {
        uint8_t *o = SppHandleObj((const uint8_t *) r8);
        Xlog("vox: reload shout dropped %s (%016llx, owner %s)",
             g_sppRldN[hit] ? g_sppRldN[hit] : "?",
             (unsigned long long) rdx, o
             && o ==
             BpMe()? "our own body" : o ? "another body" : "none");
    }
}

static void SppSndProbe(uint64_t rcx, uint64_t rdx, uint64_t r8,
                          uint64_t r9, uint64_t *esp)
{
    (void) rcx;
    (void) r9;
    SppReloadDrop(rdx, r8, esp);
    EntSndProbe(rdx, r8, esp);        // (entry window only: a clock check outside it)
    SlSndProbe(0, rdx, r8, esp);      // (broad sound logger: a volatile flag check outside its window)
    if (esp && esp[-3] != rdx)
        rdx = esp[-3];          // (the other probes see the request as it is now)
    LARGE_INTEGER q0, q1;
    QueryPerformanceCounter(&q0);       // perf30: how often this runs and what it costs (job threads, interlocked)
    ImmSndProbe(rdx, r8);
    VxSndProbe(rdx, r8);
    if (g_fire_diag_until && GetTickCount64() < g_fire_diag_until) {    // fire diagnosis: every sound request built for our body (sid + the caller of the build)
        uint8_t *o = SppHandleObj((const uint8_t *) r8);
        if (o && o == BpMe())
            SppFdPut(0, rdx, esp ? esp[0] : 0);
    }
    if (g_hit_diag_until && GetTickCount64() < g_hit_diag_until) {      // hit diagnosis: every request right after a hit on us (kind 2 = our body, 3 = other / none)
        uint8_t *o = SppHandleObj((const uint8_t *) r8);
        SppFdPut(o && o == BpMe()? 2 : 3, rdx, esp ? esp[0] : 0);
    }
    if (g_sppCensus)
        SppRingPut(0, rdx, SppHandleObj((const uint8_t *) r8));
    QueryPerformanceCounter(&q1);
    InterlockedIncrement64(&g_pl_snd_n);
    InterlockedExchangeAdd64(&g_pl_snd_q, q1.QuadPart - q0.QuadPart);
}

// FireEffect (clipfx_fe_hook calls this first): info +0x30 = the anim event type sid, +0x58 = the owner handle
static void SppFxSeen(const uint8_t *info)
{
    if (!g_sppCensus || !info)
        return;
    SppRingPut(1, *(const uint64_t *) (info + 0x30),
                 SppHandleObj(info + 0x58));
}

// register-saving probe like decal_diag_hook's (fn(rcx, rdx, r8, r9, entry rsp); volatile regs + xmm0-3 kept), but the 5-byte jmp goes in
// with ONE aligned 8-byte store: the sound request build runs on several job threads at once, a byte-wise patch could be half-seen
static int SppHook5(uint8_t *f, const uint8_t expect[5], void *fn,
                     const char *name)
{
    if (((uintptr_t) f & 7) || memcmp(f, expect, 5)) {
        HOOK_MISS(f, 5,
                  "sp parity: %s prologue differs / unaligned, not hooked",
                  name);
        return 0;
    }
    uint8_t *t = NULL;
    for (uint64_t a = ((uint64_t) f - 0x70000000ULL) & ~0xFFFFULL;
         a < (uint64_t) f + 0x70000000ULL && !t; a += 0x10000)
        t = VirtualAlloc((void *) a, 256, MEM_COMMIT | MEM_RESERVE,
                         PAGE_EXECUTE_READWRITE);
    if (!t || (uint64_t) (t > f ? t - f : f - t) > 0x7ff00000ULL) {
        HOOK_FAIL("sp parity: %s no stub memory near the exe", name);
        return 0;
    }
    static const uint8_t pre[] =
        { 0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53,
0x48, 0x83, 0xec, 0x70,
        0x0f, 0x11, 0x44, 0x24, 0x30, 0x0f, 0x11, 0x4c, 0x24, 0x40, 0x0f,
            0x11, 0x54, 0x24, 0x50, 0x0f, 0x11, 0x5c, 0x24, 0x60,
        0x48, 0x8d, 0x84, 0x24, 0xa8, 0x00, 0x00, 0x00, 0x48, 0x89, 0x44,
            0x24, 0x20, 0x48, 0xb8
    };
    static const uint8_t post[] =
        { 0xff, 0xd0, 0x0f, 0x10, 0x44, 0x24, 0x30, 0x0f, 0x10, 0x4c, 0x24,
0x40, 0x0f, 0x10, 0x54, 0x24, 0x50,
        0x0f, 0x10, 0x5c, 0x24, 0x60, 0x48, 0x83, 0xc4, 0x70, 0x41, 0x5b,
            0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5a, 0x59, 0x58
    };
    int k = 0;
    memcpy(t + k, pre, sizeof pre);
    k += sizeof pre;
    *(uint64_t *) (t + k) = (uint64_t) fn;
    k += 8;
    memcpy(t + k, post, sizeof post);
    k += sizeof post;
    memcpy(t + k, f, 5);
    k += 5;                     // the relocated prologue (mov [rsp+8], rbx: no rip-relative operand)
    t[k++] = 0xFF;
    t[k++] = 0x25;
    *(uint32_t *) (t + k) = 0;
    k += 4;
    *(uint64_t *) (t + k) = (uint64_t) (f + 5);
    FlushInstructionCache(GetCurrentProcess(), t, k);
    uint64_t old8 = *(volatile uint64_t *) f, new8 =
        (old8 & 0xFFFFFF0000000000ULL) | 0xE9ULL |
        ((uint64_t) (uint32_t) (int32_t) (t - (f + 5)) << 8);
    DWORD old;
    if (!VirtualProtect(f, 8, PAGE_EXECUTE_READWRITE, &old)) {
        HOOK_FAIL("sp parity: %s not writable", name);
        return 0;
    }
    InterlockedExchange64((volatile LONG64 *) f, (LONG64) new8);
    VirtualProtect(f, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), f, 8);
    Xlog("sp parity: %s probed", name);
    return 1;
}

// the sound request build probe, installed once (the census, or immersion.c's puppet shot signal, whichever asks first): 1 = hooked
static int g_sppSndHooked;
static int SppSndHookInstall(void)
{
    if (!g_sppSndHooked) {
        uint8_t *exe = (uint8_t *) GetModuleHandleW(NULL);
        static const uint8_t pro[5] = { 0x48, 0x89, 0x5c, 0x24, 0x08 }; // mov [rsp+8], rbx (SP sandbox H_SND, same exe)
        g_sppSndHooked =
            SppHook5(exe + 0x1dd7630, pro, SppSndProbe,
                      "sound request build FUN_141dd7630") ? 1 : -1;
    }
    return g_sppSndHooked == 1;
}

// broad sound logger: AudioManager::PlaySfx 0x141a83e20 (mgr, int *res, params (+0 = the sound NAME string), flags ...) = the voice start every SfxProcess goes through
static void SlPlayProbe(uint64_t rcx, uint64_t rdx, uint64_t r8,
                          uint64_t r9, uint64_t *esp)
{
    (void) rcx;
    (void) rdx;
    (void) r9;
    SlSndProbe(1, 0, r8, esp);
}

static int g_sppPlayHooked;
static int SppPlayHookInstall(void)
{
    if (!g_sppPlayHooked) {
        uint8_t *exe = (uint8_t *) GetModuleHandleW(NULL);
        static const uint8_t pro[5] = { 0x48, 0x8b, 0xc4, 0x53, 0x56 }; // mov rax, rsp; push rbx; push rsi (no rip-relative operand)
        g_sppPlayHooked =
            SppHook5(exe + 0x1a83e20, pro, SlPlayProbe,
                      "AudioManager::PlaySfx 0x141a83e20") ? 1 : -1;
    }
    return g_sppPlayHooked == 1;
}

static const char *SppWho(const uint8_t *o)
{
    if (!o)
        return "other";
    if (o == g_sppMe)
        return "own";
    for (int i = 0; i < T2R_MAX_PEERS; i++)
        if (o == g_sppPup[i])
            return "puppet";
    return "other";
}

// ---- fire diagnosis (burning noises after walking through fire, even without taking damage - the sound is not named in any log). While a fire field is
// on our body (ex13_burn sets g_fire_diag_until = field-contact + 6 s) every sound request built for our body / anim effect event on it is queued here from the job
// threads and logged by spp_fire_diag_tick: "fire diag: snd|fx <sid> <name if known> caller exe+X, +N ms after the last fire tick (-1 = no tick yet)". Capped (200 / match).
// Tells the next A/B run which engine event (or ours) makes the burn noise, and whether it lands between ticks.
#define SPP_FD 128
static struct {
    volatile uint32_t seq;
    uint32_t kind;
    uint64_t sid, ra, ms;
} g_spp_fd[SPP_FD];
static volatile LONG g_sppFdI;
static void SppFdPut(int kind, uint64_t sid, uint64_t ra)
{
    LONG i = InterlockedIncrement(&g_sppFdI);
    int k = i & (SPP_FD - 1);
    g_spp_fd[k].seq = 0;
    g_spp_fd[k].kind = (uint32_t) kind;
    g_spp_fd[k].sid = sid;
    g_spp_fd[k].ra = ra;
    g_spp_fd[k].ms = GetTickCount64();
    g_spp_fd[k].seq = (uint32_t) i;
}

static const char *SppFdName(uint64_t sid)
{
    static const char *const N[] =
        { "vox-eff-gen-hit-by-fire", "vox-eff-gen-death-molotov-fast",
"vox-eff-gen-death-molotov-slow", "vox-eff-gen-pain-fire", "vox-eff-gen-burn",
        "vox-eff-gen-fire-reaction", "vox-eff-gen-hit-med",
            "vox-eff-gen-hit-lrg", "vox-eff-gen-hit-sml", "sfx-fire-burn",
            "sfx-fire-loop", "sfx-fire-player-burn", "foot-effect",
        "gear-effect-lower", "gear-effect-upper", "dialog-effect",
            "hand-effect", "body-effect", "body-impact-effect",
            "vox-event-player-on-fire", "vox-event-player-burning",
        "vox-event-hit-by-fire", "vox-event-player-hit-fire",
            "t2r-silenced-reload-vox"
    };
    static uint64_t H[sizeof N / sizeof *N];
    static int init;
    if (!init) {
        for (size_t i = 0; i < sizeof N / sizeof *N; i++)
            H[i] = Fnv64Rt(N[i]);
        init = 1;
    }
    for (size_t i = 0; i < sizeof N / sizeof *N; i++)
        if (H[i] == sid)
            return N[i];
    return "?";
}

static void SppFireDiagTick(void)
{
    static uint32_t last;
    static int hooked, nl;
    static uint64_t matchid;
    uint64_t now = GetTickCount64();
    if (MatchSubstate() != 6) {
        nl = 0;
        return;
    }
    int hitd = g_hit_diag_until && now < g_hit_diag_until + 500;
    if ((!g_fire_diag_until || now >= g_fire_diag_until + 500) && !hitd)
        return;
    if (!hooked) {
        hooked = SppSndHookInstall()? 1 : -1;
        Xlog("fire diag: sound request probe %s",
             hooked ==
             1 ?
             "armed (own-body requests are logged while a fire field is on us)"
             : "not available");
    }
    uint32_t end = (uint32_t) g_sppFdI;
    if (end - last > SPP_FD)
        last = end - SPP_FD;
    for (uint32_t q = last + 1; q - 1 != end; q++) {
        int k = q & (SPP_FD - 1);
        if (g_spp_fd[k].seq != q)
            continue;
        if (nl >= 200)
            break;
        nl++;
        uint64_t tk = g_fire_tick_ms, ra = g_spp_fd[k].ra, exe =
            (uint64_t) GetModuleHandleW(NULL);
        if (g_spp_fd[k].kind >= 2) {
            Xlog("hit diag: snd %016llx (%s) caller %s%llx, %lld ms after the hit on us", (unsigned long long) g_spp_fd[k].sid, g_spp_fd[k].kind == 2 ? "our body" : "other / 2D", ra >= exe && ra < exe + 0x4000000 ? "exe+" : "", (unsigned long long) (ra >= exe && ra < exe + 0x4000000 ? ra - exe : ra), (long long) (g_spp_fd[k].ms - g_hit_diag_ms));
            continue;
        }
        Xlog("fire diag: %s %016llx %s caller %s%llx, %lld ms after the last fire tick", g_spp_fd[k].kind ? "fx" : "snd", (unsigned long long) g_spp_fd[k].sid, SppFdName(g_spp_fd[k].sid), ra >= exe && ra < exe + 0x4000000 ? "exe+" : "", (unsigned long long) (ra >= exe && ra < exe + 0x4000000 ? ra - exe : ra), tk ? (long long) (g_spp_fd[k].ms - tk) : -1LL);
    }
    last = end;
    (void) matchid;
}

static void SppCensusTick(void)
{
    static int armed, hooked, lines, cam_n;
    static uint64_t seen[2048];
    static uint32_t last_seq;
    uint8_t ms = MatchSubstate();
    if (ms != 6) {
        if (armed) {
            Xlog("sp parity: census end (%d distinct lines)", lines);
            armed = 0;
        }
        g_sppCensus = 0;
        g_cfxe_seen_cb = NULL;
        return;
    }
    if (g_cfxe_seen_cb != SppFxCb)
        g_cfxe_seen_cb = SppFxCb;     // (the own bag foley notes: always while in a match)
    if (!armed) {
        armed = 1;
        lines = 0;
        cam_n = 0;
        memset(seen, 0, sizeof seen);
        last_seq = (uint32_t) g_spp_ri;
        FILE *f = ExeSideOpen("t2r-sp-snd-census.txt", "r");
        int on = f != NULL;
        if (f)
            fclose(f);
        if (on && !hooked)
            hooked = SppSndHookInstall()? 1 : -1;
        g_sppCensus = on && hooked == 1;
        // (g_cfxe_seen_cb = spp_fx_cb above: the census part runs while g_spp_census; clipfx_fe_hook calls it only while its slot wrap is installed)
        if (on)
            Xlog("sp parity: census %s (t2r-sp-snd-census.txt): distinct sound events / anim effect types per owner class this match", g_sppCensus ? "ON" : "not available");
    }
    if (!g_sppCensus)
        return;
    typedef uint8_t *(*fn_peer)(uint32_t);
    g_sppMe = BpMe();
    uint32_t me = *(uint32_t *) T2R_SYM(kMySlot);
    for (uint32_t s = 0; s < T2R_MAX_PEERS; s++) {
        uint8_t *peer = s != me ? ((fn_peer) T2rPeer) (s) : NULL;
        uint64_t h = peer ? *(uint64_t *) (peer + 0x60) : 0;
        g_sppPup[s] =
            h ? (uint8_t *) ((void *(*)(uint64_t))
                             T2R_SYM(kFnEntityFromHandle))(h) : NULL;
    }
    {                           // camera vs our head (the MP side of the sandbox's PAIRF C= frames; tools/sp_parity/camera_offsets.py --mp)
        static uint64_t cam_next;
        uint64_t now = GetTickCount64();
        if (now >= cam_next && cam_n < 1200) {
            cam_next = now + 250;
            float cp[3], cf[3], hd[3], lb[16];
            const float *al = PlayerLocator(lb);
            if (al && RcamGet(cp, cf)
                && GxJointPos(SID_PLAYER, "headb", hd)) {
                cam_n++;
                Xlog("sp parity: cam pos %.3f %.3f %.3f fwd %.4f %.4f %.4f head %.3f %.3f %.3f align %.3f", cp[0], cp[1], cp[2], cf[0], cf[1], cf[2], hd[0], hd[1], hd[2], al[1]);
            }
        }
    }
    uint32_t end = (uint32_t) g_spp_ri;
    if (end - last_seq > SPP_RING)
        last_seq = end - SPP_RING;
    for (uint32_t q = last_seq + 1; q - 1 != end && lines < 1500; q++) {
        int k = q & (SPP_RING - 1);
        if (g_spp_ring[k].seq != q)
            continue;
        uint64_t sid = g_spp_ring[k].sid;
        uint32_t kind = g_spp_ring[k].kind;
        const char *who = SppWho((const uint8_t *) g_spp_ring[k].obj);
        uint64_t key =
            sid ^ ((uint64_t) kind << 62) ^ ((uint64_t) (who[0]) << 54);
        uint32_t hsh = (uint32_t) (key ^ (key >> 29)) & 2047;
        int dup = 0;
        for (int p = 0; p < 2048; p++) {
            uint32_t j = (hsh + p) & 2047;
            if (seen[j] == key) {
                dup = 1;
                break;
            }
            if (!seen[j]) {
                seen[j] = key;
                break;
            }
        }
        if (dup)
            continue;
        lines++;
        Xlog("sp parity: %s %s %016llx", kind ? "fx" : "snd", who,
             (unsigned long long) sid);
    }
    last_seq = end;
}

static void SpParityTick(void)
{
    SppBagGateTick();
    SppBpLogTick();
    SppOwnFoleyTick();
    SppGlobalLevelsTick();
    SppCensusTick();
    SppFireDiagTick();
}
