// melee_hitfx.c - what a melee blow does to OUR screen and ears (SP's heavy-pickaxe-hit feedback for every BLUNT weapon, a lighter blade version
// for BLADE weapons). Included after alert_sting.c (uses als_v2 / the stun ring wave), driven from melee_feel.c mfk_victim_hit (note) and immersion_tick (tick).
// Log prefix "hitfx:". Opt-out t2r-no-melee-hitfx.txt (the camera jolt keeps its own t2r-no-melee-shake.txt).
//
// Weapon class (mhfx_class, by the attacker's weapon id; the MP melee table is MELEE_TAB, extra SP names are listed too, anything else = fists = no hitfx):
//   BLUNT 1h: crowbar lead-pipe short-pipe pipe-wrench hammer-modern hammer-scar club-1h-police club-1h-scar
//   BLUNT 2h (heavier): baseball-bat two-by-four club-2h-scar sledgehammer pickaxe
//   BLADE   : machete machete-scar tomahawk hatchet-modern fireman-axe (+ knife / switchblade / shiv names if a build ever sends them)
// BLUNT = the SP concussion: a stronger camera jolt (melee_feel.c), the SP melee-hit render presets (*zgame-player-damage-punch* = what every SP melee hit script
// calls at priority 32767, plus a light *zgame-player-approach-mortal-state* focus pull), SP's tinnitus ring (stun_ring.pcm) for 1.5 .. 2.5 s by damage with a
// fade out, and the world ducked under it (the blast mix snapshot). A hit during the effect refreshes it (no second ring, no stacking).
// BLADE = TLOU2's slash cue (blade_sfx.h M_SLASH, 2D), a slight camera shake (melee_feel.c) and the red pain overlay (*zgame-mp-player-pain-kill*,
// via immersion.c imm_screen_tick) that fades in ~0.5 s. No ring, no blur.
// Skipped while stunned (the stun owns blur / ring), downed, dying, in a paired move or execution (the callers' filters); everything is cleared on those at once.
enum { MHFX_NONE, MHFX_BLUNT, MHFX_BLADE };
static const struct {
    const char *n;
    int cls;
    int two_h;
} MHFX_TAB[] = {
    { "crowbar", MHFX_BLUNT, 0 }, { "lead-pipe", MHFX_BLUNT, 0 },
        { "short-pipe", MHFX_BLUNT, 0 }, { "pipe-wrench", MHFX_BLUNT, 0 },
    { "hammer-modern", MHFX_BLUNT, 0 }, { "hammer-scar", MHFX_BLUNT, 0 },
        { "club-1h-police", MHFX_BLUNT, 0 }, { "club-1h-scar", MHFX_BLUNT,
                                              0 },
    { "baseball-bat", MHFX_BLUNT, 1 }, { "two-by-four", MHFX_BLUNT, 1 },
        { "club-2h-scar", MHFX_BLUNT, 1 }, { "sledgehammer", MHFX_BLUNT,
                                            1 }, { "pickaxe", MHFX_BLUNT,
                                                  1 },
    { "machete", MHFX_BLADE, 0 }, { "machete-scar", MHFX_BLADE, 0 },
        { "tomahawk", MHFX_BLADE, 0 }, { "hatchet-modern", MHFX_BLADE, 0 },
        { "hatchet", MHFX_BLADE, 0 }, { "fireman-axe", MHFX_BLADE, 1 },
    { "knife", MHFX_BLADE, 0 }, { "switchblade", MHFX_BLADE, 0 }, { "shiv",
                                                                   MHFX_BLADE,
                                                                   0 },
    { "knife-survival-mel-player", MHFX_BLADE, 0 }, { "dagger-scar-lev-player", MHFX_BLADE, 0 },        // v5-knife: a female's knife (t2r-ext.c fkr_victim_w): slash cue + red overlay, -shrp grunts
    { "abby-unarmed", MHFX_BLUNT, 0 },  // fists (6ac05fba9c921900): a LIGHT concussion, see mhfx_note
};

static int MhfxClass(uint64_t w, int *two_h, const char **name)
{
    static uint64_t h[sizeof MHFX_TAB / sizeof *MHFX_TAB];
    if (!h[0])
        for (size_t i = 0; i < sizeof MHFX_TAB / sizeof *MHFX_TAB; i++)
            h[i] = FNV64(MHFX_TAB[i].n);
    for (size_t i = 0; i < sizeof MHFX_TAB / sizeof *MHFX_TAB; i++)
        if (h[i] == w) {
            if (two_h)
                *two_h = MHFX_TAB[i].two_h;
            if (name)
                *name = MHFX_TAB[i].n;
            return MHFX_TAB[i].cls;
        }
    return MHFX_NONE;
}

static struct {
    volatile int pending, cls;
    volatile uint64_t t0, until;        // GetTickCount64; until = end of the blunt effect
    volatile float dur, k;
    volatile uint64_t blade_t0;
    volatile int two, light;    // light = a fist blow: short punch + blur, no ring / whine
    AlsV2 tn;
    uint64_t tn_end;
    int tn_fading;
    AlsV2 hi;
} g_mhfx = {.tn = { -1, -1, NULL, NULL },.hi = { -1, -1, NULL, NULL } };

// A high-pitched tinnitus on heavy hits (SP brute pickaxe hits): SP has no melee ring
// at all and its sfx-tinnitus tone is a dull ~410 Hz. A heavy blunt blow adds a HIGH whine on top: a synthetic ~6 kHz ring (left 5.9 kHz, right
// 5.96 kHz: a slow 60 Hz-free binaural shimmer), in over 0.12 s, a slight swell, then an exponential fade over the rest of MHFX_HI_S.
// t2r-no-melee-hiring.txt = off. Built once into static buffers.
#define MHFX_HI_S 2.6f
#define MHFX_HI_RATE 48000
#define MHFX_HI_N ((int)(MHFX_HI_S * MHFX_HI_RATE))
static int16_t g_mhfxHiL[MHFX_HI_N], g_mhfx_hi_r[MHFX_HI_N];
static void MhfxHiBuild(void)
{
    static int built;
    if (built)
        return;
    built = 1;
    const float f[2] = { 5900.0f, 5960.0f };
    int16_t *o[2] = { g_mhfxHiL, g_mhfx_hi_r };
    for (int c = 0; c < 2; c++) {
        float ph = 0.0f, dp = 6.2831853f * f[c] / MHFX_HI_RATE;
        for (int i = 0; i < MHFX_HI_N; i++) {
            float t = (float) i / MHFX_HI_RATE;
            float env =
                t <
                0.12f ? t / 0.12f : __builtin_expf(-(t - 0.12f) * 1.15f);
            env *= 1.0f + 0.12f * __builtin_sinf(6.2831853f * 0.9f * t);        // a slow swell
            if (t > MHFX_HI_S - 0.3f)
                env *= (MHFX_HI_S - t) / 0.3f;  // no click at the end
            o[c][i] = (int16_t) (__builtin_sinf(ph) * env * 9000.0f);
            ph += dp;
            if (ph > 6.2831853f)
                ph -= 6.2831853f;
        }
    }
}

static int MhfxHiOff(void)
{
    static int off = -1;
    static uint64_t at;
    return FeelFlag("t2r-no-melee-hiring.txt", &at, &off, 180);
}

static int MhfxOff(void)
{
    static int off = -1;
    static uint64_t at;
    return FeelFlag("t2r-no-melee-hitfx.txt", &at, &off, 180);
}

// called from mfk_victim_hit once a blow is judged to hurt us and leave us standing (all busy / teammate / reach filters already applied)
static void MhfxNote(uint32_t slot, uint64_t w, float dmg)
{
    static int nl;
    int two = 0;
    const char *nm = NULL;
    int cls = MhfxClass(w, &two, &nm);
    if (cls == MHFX_NONE || MhfxOff())
        return;
    uint64_t now = GetTickCount64();
    if (g_mhfxDcUntil && now < g_mhfxDcUntil)
        return;                 // the counter's own heavy concussion runs (mhfx_counter)
    int fist = nm && !strcmp(nm, "abby-unarmed");
    if (fist
        && GetFileAttributesA("t2r-no-fist-hitfx.txt") !=
        INVALID_FILE_ATTRIBUTES)
        return;
    if (fist) {                 // light concussion for punches: 0.9 s, ~half strength, no tinnitus - a punch every second must not ring constantly
        if (g_mhfx.until && now < g_mhfx.until && !g_mhfx.light)
            return;             // a heavier effect is running: keep it
        g_mhfx.dur = 0.9f;
        g_mhfx.k = 0.45f;
        g_mhfx.two = 0;
        g_mhfx.light = 1;
        g_mhfx.t0 = now;
        g_mhfx.until = now + 900;
        GshAdd(0.8f, 1.2f, 5.0f);
    } else if (cls == MHFX_BLUNT) {
        g_mhfx.light = 0;
        float f = (dmg - 20.0f) / 20.0f;
        f = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
        float dur = 1.5f + f * 0.7f + (two ? 0.3f : 0.0f);      // 1.5 .. 2.5 s by damage, 2-handed a bit longer
        g_mhfx.dur = dur;
        g_mhfx.k = two ? 1.0f : 0.8f;
        g_mhfx.two = two;
        g_mhfx.t0 = now;
        g_mhfx.until = now + (uint64_t) (dur * 1000.0f);
        GshAdd(1.4f * g_mhfx.k, 1.6f, 5.0f);   // (the camera sways ~2 s in the SP clip concussion.mp4)                              // the after-wobble under the jolt
    } else
        g_mhfx.blade_t0 = now;
    g_mhfx.cls = cls;
    g_mhfx.pending = 1;
    if (LOGCAP(nl, 60))
        Xlog("hitfx: %s blow (%s%s) from slot %u, dmg %.1f -> %s",
             fist ? "FIST" : cls == MHFX_BLUNT ? "BLUNT" : "BLADE",
             nm ? nm : "?", two ? ", 2h" : "", slot, (double) dmg,
             fist ?
             "light concussion: damage-punch + focus pull at 0.45, 0.9 s, no ring"
             : cls ==
             MHFX_BLUNT ?
             "concussion: damage-punch + focus pull + tinnitus ring + mix duck"
             : "slash cue + red overlay");
}

// Counterattacks (knife and punch) use the tinnitus and sledge-type blow: the dodge-counter blow on us (dcounter.c dc_note_hit)
// is always the heaviest concussion, whatever the weapon: BLUNT, two-handed (damage-sledge preset + the ~6 kHz whine + SP tinnitus tone + duck + sway),
// full 2.5 s. It plays on through the counter pair and into the down it causes (the concussion goes through, it is not cut),
// immersion_tick keeps it live while g_mhfx_dc_until runs; only death clears it early.
static void MhfxCounter(uint32_t slot, uint64_t w)
{
    static int nl;
    if (MhfxOff())
        return;
    uint64_t now = GetTickCount64();
    g_mhfx.dur = 3.0f;
    g_mhfx.k = 1.0f;
    g_mhfx.two = 1;
    g_mhfx.light = 0;
    g_mhfx.t0 = now;
    g_mhfx.until = now + 3000;  // (3 s: on through the collapse into the down)
    g_mhfxDcUntil = now + 3000;
    GshAdd(1.4f, 1.6f, 5.0f);
    g_mhfx.cls = MHFX_BLUNT;
    g_mhfx.pending = 1;
    if (LOGCAP(nl, 40))
        Xlog("hitfx: COUNTER blow from slot %u (weapon %llx) -> heavy concussion: sledge + punch presets, high whine + tinnitus, 2.5 s", slot, (unsigned long long) w);
}

// BLADE overlay strength this frame (immersion.c imm_screen_tick raises its pain pulse to it): 0.9 at the cut, quadratic fade to 0 over 0.5 s
static float MhfxPainLevel(void)
{
    if (!g_mhfx.blade_t0)
        return 0.0f;
    float u = (float) (GetTickCount64() - g_mhfx.blade_t0) / 500.0f;
    if (u >= 1.0f || u < 0.0f) {
        g_mhfx.blade_t0 = 0;
        return 0.0f;
    }
    return 0.9f * (1.0f - u) * (1.0f - u);
}

// every frame from immersion_tick; live = in match, standing, not stunned / executed / paired
static void MhfxTick(int live, int stunned)
{
    static int nl, no_cue;
    static uint64_t sid_dof, sid_punch;
    uint64_t now = GetTickCount64();
    int off = MhfxOff();
    if (!live || off) {         // down / death / stun / execution / match end / opt-out: a hard clear
        g_mhfx.pending = 0;
        g_mhfx.until = 0;
        g_mhfx.blade_t0 = 0;
        if (g_mhfx.hi.a >= 0)
            AlsV2Fade(&g_mhfx.hi, 150);
        if (g_mhfx.tn.a >= 0) {
            AlsV2Fade(&g_mhfx.tn, 150);
            if (LOGCAP(nl, 40))
                Xlog("hitfx: cleared (%s)",
                     off ? "opt-out" : stunned ? "stunned" : "not live");
        }
        return;
    }
    if (g_mhfx.pending) {
        g_mhfx.pending = 0;
        if (g_mhfx.cls == MHFX_BLUNT && !g_mhfx.light) {
            if (g_mhfx.tn.a >= 0 && !AlsV2Live(&g_mhfx.tn))
                g_mhfx.tn.a = g_mhfx.tn.b = -1;
            if (g_mhfx.tn.a < 0
                && !(g_stt.v.a >= 0 && AlsV2Live(&g_stt.v))) {
                // A blunt hit rang like the stun bomb; it should be a softer high-pitched ring: the stun ring (sr_blob) is the
                // stun bomb's; a blow now rings with SP's own sfx-tinnitus tone (gbl-hud tinnitus-tone, the blast ring of immersion.c), quieter, fading out with the effect
                int ok =
                    AlsV2Play(&g_mhfx.tn, sh_blob + 2 * SH_HB_N,
                                sh_blob + 2 * SH_HB_N + SH_TN_N, SH_TN_N,
                                SH_RATE, 0.28f * SfxUserVolume());
                if (LOGCAP(nl, 40))
                    Xlog("hitfx: tinnitus ring started -> %d (%.1f s)", ok,
                         (double) g_mhfx.dur);
            } else if (LOGCAP(nl, 40))
                Xlog("hitfx: tinnitus refreshed (ring already running, ends in %.1f s)", (double) g_mhfx.dur);
            g_mhfx.tn_end = g_mhfx.until;
            g_mhfx.tn_fading = 0;
            if (!MhfxHiOff()) {       // the high whine on top (restarts on each heavy blow)
                MhfxHiBuild();
                if (g_mhfx.hi.a >= 0 && AlsV2Live(&g_mhfx.hi))
                    AlsV2Fade(&g_mhfx.hi, 40);
                int okh =
                    AlsV2Play(&g_mhfx.hi, g_mhfxHiL, g_mhfx_hi_r,
                                MHFX_HI_N, MHFX_HI_RATE,
                                0.22f * g_mhfx.k * SfxUserVolume());
                if (LOGCAP(nl, 40))
                    Xlog("hitfx: high tinnitus whine (~6 kHz, %.1f s) -> %d", (double) MHFX_HI_S, okh);
            }
            if (IMM_MF_ON()) {  // the world ducked under the ring (immersion.c blast-muffle window)
                g_imm_mf_from = g_imm_mf_from
                    && now < g_imm_mf_until ? g_imm_mf_from : now;
                uint64_t u =
                    now + (uint64_t) (g_mhfx.dur * 1000.0f * 0.6f);
                if (u > g_imm_mf_until)
                    g_imm_mf_until = u;
            }
        } else if (g_mhfx.cls == MHFX_BLADE) {
            if (g_mwsNativeT60 && g_t60 - g_mwsNativeT60 < 20) {    // v5-sfx: the weapon's own SP hit event already played on our body (ctxdown.c mws_native)
                if (LOGCAP(nl, 40))
                    Xlog("hitfx: blade slash cue skipped - the weapon's SP hit event played on our body");
            } else if (BladePcmOn()) {
                int ok =
                    BladeCuePlay(BC_M_SLASH, 0.7f, SID_PLAYER, NULL,
                                   "hitfx");
                if (LOGCAP(nl, 40))
                    Xlog("hitfx: blade slash cue (machete slash, 2D) -> %d", ok);
            } else if (!no_cue++)
                Xlog("hitfx: blade slash cue off (t2r-no-blade-pcm.txt)");
        }
    }
    if (g_mhfx.tn.a >= 0) {
        if (!AlsV2Live(&g_mhfx.tn))
            g_mhfx.tn.a = g_mhfx.tn.b = -1;
        else if (!g_mhfx.tn_fading && now + 600 >= g_mhfx.tn_end) {
            g_mhfx.tn_fading = 1;
            AlsV2Fade(&g_mhfx.tn, 600);
        }
    }
    if (g_mhfx.until && now < g_mhfx.until) {
        float el = (float) (now - g_mhfx.t0) / 1000.0f, dur =
            g_mhfx.dur, env;
        if (el < 0.08f)
            env = el / 0.08f;
        else if (el < 0.35f)
            env = 1.0f;
        else {
            float u = (el - 0.35f) / (dur - 0.35f);
            env =
                0.5f * (1.0f +
                        __builtin_cosf(3.14159265f *
                                       (u > 1.0f ? 1.0f : u)));
        }
        // (SP hammer on the player): the screen darkens at the edges, blurs hard with motion + radial blur and chromatic
        // aberration, colours wash out. damage-punch at full strength (SP's melee scripts call it with the engine-max priority; 0.7 read as nothing), the focus
        // pull stronger, plus the pure dark-vignette preset (peek-mode, as the downed screen) for the darkening.
        static uint64_t sid_vig;
        if (!sid_dof) {
            sid_dof = FNV64("*zgame-player-approach-mortal-state*");
            sid_punch = FNV64("*zgame-player-damage-punch*");
            sid_vig = FNV64("*zgame-player-peek-mode*");
        }
        uint64_t r1 =
            ImmRsCall(sid_dof, 32766, 0.55f * env * g_mhfx.k), r2 =
            ImmRsCall(sid_punch, 32767, 1.0f * env * g_mhfx.k);
        ImmRsCall(sid_vig, 32765, 0.55f * env * g_mhfx.k);
        // (SP brute pickaxe hits: blur halves the detail, colour drops ~35 %, edges darken, a doubled / ghosted image, back in ~1.5 s):
        // SP has a preset for exactly these blows, *zgame-player-damage-sledge* - layered for two-handed blunt weapons. t2r-no-melee-sledge-fx.txt = off
        static uint64_t sid_sl;
        static int sl_off = -1;
        static uint64_t sl_at;
        if (!sid_sl)
            sid_sl = FNV64("*zgame-player-damage-sledge*");
        if (g_mhfx.two
            && !FeelFlag("t2r-no-melee-sledge-fx.txt", &sl_at, &sl_off,
                          180)) {
            uint64_t r3 = ImmRsCall(sid_sl, 32764, 1.0f * env);
            if (el < 0.1f && LOGCAP(nl, 40))
                Xlog("hitfx: two-handed blow - damage-sledge preset -> %llx", (unsigned long long) r3);
        }
        if (el < 0.1f && LOGCAP(nl, 40))
            Xlog("hitfx: concussion render presets on (focus pull -> %llx, damage-punch -> %llx, %.1f s)", (unsigned long long) r1, (unsigned long long) r2, (double) dur);
    } else if (g_mhfx.until) {
        g_mhfx.until = 0;
        Xlog("hitfx: concussion over");
    }
}
