// immersion.c - TLOU2 single-player breath / effort voice for OUR player.
// Included by t2r-ext.c after feel.c (uses feel_flag, local_hp, g_wg_max, match_substate, dc_call, SID_PLAYER). Log prefix "immersion:".
//
// The SP player is never silent between fights: she pants after a sprint, breathes hard when hurt, exhales after a heal, holds her
// breath crouched in cover, grunts on a throw. MP had none of it (only the executions' effort lines). The game's own vox events
// (sidbase: vox-eff-gen-breath-*, -effort-*) are played with the `say` native on our own body, exactly as the execution grunts do
// (gx_play). LOCAL ONLY: nothing is sent, nobody hears our breathing through a wall (fairness; 2013 had no breath tell).
// Every voice is one switch (opt-out file next to the exe, polled every ~3 s):
//   t2r-no-sprint-breath.txt   panting after a long sprint (breath-sprint-recover-1/2, fallback breath-exhausted-recover)
//   t2r-no-wounded-breath.txt  labored breathing below 30 % health (breath-fight-injured moving / breath-idle-sore standing)
//   t2r-no-heal-breath.txt     the exhale when a heal finishes (breath-heal-finish-combat / breath-heal-finish)
//   t2r-no-stealth-breath.txt  quiet breaths while crouched and still (breath-stealth-mouth-closed / -open)
//   t2r-no-throw-effort.txt    a short effort grunt on our own throw (effort-sml)
// One breath at a time (IMM_GAP_MS between any two); the heal exhale and the throw grunt always play. Female looks are skipped: the
// generic events resolve in the speaker's own bank, which for the female looks is not proven (fem_effort_sid).
// An event whose bank is not loaded is silent at worst (say returns a handle anyway); the first lines log what `say` answered.

#define IMM_GAP_MS 2600
#define IMM_N 6
enum { IMM_SPRINT, IMM_WOUND, IMM_HEAL, IMM_STEALTH, IMM_THROW,
        IMM_HEART };
static const char *const IMM_SWITCH[IMM_N] =
    { "t2r-no-sprint-breath.txt", "t2r-no-wounded-breath.txt",
"t2r-no-heal-breath.txt",
    "t2r-no-stealth-breath.txt", "t2r-no-throw-effort.txt",
        "t2r-no-heartbeat.txt"
};
static int g_immOff[IMM_N] = { -1, -1, -1, -1, -1, -1 };

static uint64_t g_immOffAt[IMM_N];
static uint64_t g_immLastMs;

static int ImmEnabled(int k)
{
    return !FeelFlag(IMM_SWITCH[k], &g_immOffAt[k], &g_immOff[k], 180);
}

// plays the first of `ev` that `say` accepts (a ~0 answer = the engine refused the event); remembers which one worked
static int ImmSay(int k, const char *const *ev, int n, int force)
{
    static int pick[IMM_N] = { -1, -1, -1, -1, -1, -1 }, dead[IMM_N], nl;
    uint64_t now = GetTickCount64();
    if (dead[k] || OwnFemale())
        return 0;
    if (!force && g_immLastMs && now - g_immLastMs < IMM_GAP_MS)
        return 0;
    for (int i = pick[k] >= 0 ? pick[k] : 0; i < n; i++) {
        uint64_t a[4] = { SID_PLAYER, FNV64(ev[i]), 2, 0 };
        uint64_t r = DcCall(FNV64("say"), 4, a);
        if (nl < 50) {
            nl++;
            Xlog("immersion: %s -> %llx", ev[i], (unsigned long long) r);
        }
        if (r != ~0ULL) {
            pick[k] = i;
            g_immLastMs = now;
            return 1;
        }
        if (pick[k] >= 0) {
            pick[k] = -1;
            return 0;
        }
    }
    dead[k] = 1;
    Xlog("immersion: nothing plays for %s (events not in this build)",
         IMM_SWITCH[k]);
    return 0;
}

// ---- explosion tinnitus + low-health heartbeat: TLOU2's own waves (gbl-hud.bnk heartbeat / tinnitus-tone, tools/rip_t2_hud_audio.py) through the
// PCM mixer, 2D, on our player only. The engine events (sfx-heartbeat, sfx-tinnitus) exist but a TLOU2 sound event is silent in MP unless its bank is
// loaded, so, as for the death sting, the wave itself is played.
//   t2r-no-tinnitus.txt   a nail bomb / placed bomb that bursts within 5 m of our body (any team, any thrower): the SP ear ring, the first
//                         3.6 s of the SP tinnitus tone fading out; 2.5 m or closer plays at full level, 5 m at 0.25; not twice within 4 s.
//                         The world muffle + camera shake of a close blast are the pass below (t2r-no-blast-muffle.txt / t2r-no-cam-shake.txt).
//   t2r-no-heartbeat.txt  below 30 % health (until 40 %) one lub-dub per beat, every 1.25 s at 30 % health down to 0.55 s near death
//                         (SP: the pulse speeds up as you get worse). Nothing while downed / dying / outside a match.
#include "sp_hud_audio.h"
__asm__
    (".section .rdata,\"dr\"\n.balign 4\n.globl sh_blob\nsh_blob:\n.incbin \"sp_hud_audio.pcm\"\n.text\n");
extern const int16_t sh_blob[];
#define SH_BEAT_N (SH_RATE * 95 / 100)  // the lub-dub ends at ~0.7 s, the file's tail is not played

// ---- SP-feel pass, each its own opt-out file next to the exe (feel_flag, polled ~3 s), log prefix "immersion:":
//   t2r-no-near-miss.txt    an enemy's shot that passes within ~1.5 m of us without hitting: TLOU2's bullet pass-by whizz from the side it passed (waves of
//                           gbl-bullet.bnk bullet-pass-by-NN through the PCM mixer, tools/rip_t2_bullet_by.py) + a very small camera flinch
//   t2r-no-health-screen.txt  TLOU2's cut multiplayer render presets *zgame-mp-player-health-02-low* (red vignette, grain, aberration) by health band,
//                           blended in / out, plus *zgame-mp-player-pain-kill* as a short pulse when we are hurt; cleared at once off-live
//   t2r-no-blast-muffle.txt a close blast (<= 5 m): the world is ducked for 1.5 s by the mix snapshot mix-listen-mode-abbie-ellie-fight (rewritten by
//                           dc_data_patches.patch_blast_muffle), under the tinnitus (PCM voices are outside the game's mix)
//   t2r-no-cam-shake.txt    SP-style camera shake (render camera only, gsh_add in t2r-ext.c) on a nearby blast and on nearby heavy gunfire
static int ImmNoflag(const char *file, uint64_t *at, int *val)
{
    return FeelFlag(file, at, val, 180);
}

static const char IMM_F_NM[] = "t2r-no-near-miss.txt", IMM_F_HS[] =
    "t2r-no-health-screen.txt", IMM_F_MF[] =
    "t2r-no-blast-muffle.txt", IMM_F_CS[] = "t2r-no-cam-shake.txt";
static int g_immNmV = -1, g_imm_hs_v = -1, g_imm_mf_v = -1, g_imm_cs_v =
    -1;
static uint64_t g_imm_nm_a, g_imm_hs_a, g_imm_mf_a, g_imm_cs_a;
#define IMM_NM_ON() (!ImmNoflag(IMM_F_NM, &g_imm_nm_a, &g_immNmV))
#define IMM_HS_ON() (!ImmNoflag(IMM_F_HS, &g_imm_hs_a, &g_imm_hs_v))
#define IMM_MF_ON() (!ImmNoflag(IMM_F_MF, &g_imm_mf_a, &g_imm_mf_v))
#define IMM_CS_ON() (!ImmNoflag(IMM_F_CS, &g_imm_cs_a, &g_imm_cs_v))

static volatile uint64_t g_immTnMs;   // GetTickCount64 of the last tinnitus
static float g_immTnDist[2];  // [0] last blast distance, [1] its gain (log)
static volatile uint64_t g_imm_mf_from, g_imm_mf_until; // blast muffle window (GetTickCount64): the snapshot is applied every frame inside it
#define IMM_MUFFLE_ON_MS 1100   // + the snapshot's 0.4 s release = 1.5 s
static void ImmBlastNote(int kind, const float *pos)
{
    static int nl, nm, ns;
    if (!pos || !__builtin_isfinite(pos[0] + pos[1] + pos[2]))
        return;
    uint64_t now = GetTickCount64();
    float lb[16];
    const float *l = Uc4MyLoc(lb);
    if (!l || !__builtin_isfinite(l[0]) || LocalHp() <= 0.0f)
        return;
    float dx = pos[0] - l[0], dy = pos[1] - (l[1] + 0.9f), dz =
        pos[2] - l[2], d = __builtin_sqrtf(dx * dx + dy * dy + dz * dz);
    // camera shake: any blast within 25 m, quadratic fall-off (2.4 deg peak at the feet, 0 at 25 m), 0.9 s (SP's explosion shake amplitude is not
    // decoded - the numbers are ours)
    // a molotov bursting next to you shook the screen in 2013 too - a lighter, shorter-range shake (no tinnitus / muffle below)
    float cs_r = kind == EX13_MOLO ? 10.0f : 25.0f, cs_a =
        kind == EX13_MOLO ? 1.2f : 2.4f;
    if (d < cs_r && IMM_CS_ON()) {
        float u = 1.0f - d / cs_r, amp = cs_a * u * u;
        GshAdd(amp, 0.9f, 9.0f);
        if (LOGCAP(ns, 20))
            Xlog("immersion: camera shake - blast %.1f m away, %.2f deg for 0.9 s", (double) d, (double) amp);
    }
    if (kind == EX13_MOLO)
        return;                 // (molotov: shake only)
    // world muffle: within 5 m, 1.1 s of the ducking snapshot + its 0.4 s release (a re-hit within the window just extends it)
    if (d <= 5.0f && IMM_MF_ON()) {
        g_imm_mf_from = g_imm_mf_from
            && now < g_imm_mf_until ? g_imm_mf_from : now;
        g_imm_mf_until = now + IMM_MUFFLE_ON_MS;
        if (LOGCAP(nm, 20))
            Xlog("immersion: blast muffle - blast %.1f m away, world ducked for %d ms + 400 ms release", (double) d, IMM_MUFFLE_ON_MS);
    }
    static int off = -1;
    static uint64_t off_at;
    if (FeelFlag("t2r-no-tinnitus.txt", &off_at, &off, 180))
        return;
    if (g_immTnMs && now - g_immTnMs < 4000)
        return;
    if (d > 5.0f)
        return;
    float g = d <= 2.5f ? 1.0f : 1.0f - 0.75f * (d - 2.5f) / 2.5f;
    int ok =
        PcmPlayStereo(sh_blob + 2 * SH_HB_N,
                        sh_blob + 2 * SH_HB_N + SH_TN_N, SH_TN_N,
                        0.55f * g, SH_RATE);
    if (ok)
        g_immTnMs = now;
    g_immTnDist[0] = d;
    g_immTnDist[1] = g;
    if (LOGCAP(nl, 20))
        Xlog("immersion: tinnitus - blast %.1f m away, gain %.2f -> %s",
             (double) d, (double) g, ok ? "playing" : "mixer full");
}

// quieter (0.5 -> 0.3), and a heal fades it out over ~2.5 s (slowing beats, falling gain) instead of stopping it dead
#define HB_GAIN 0.30f
#define HB_FADE_MS 2500
static void ImmHeartbeat(float hp, float mx, int on)
{
    static uint64_t next_ms, fade_from;
    static int nl, was_on, beats;
    static float last_iv = 1.25f, arm_hp;
    if (!ImmEnabled(IMM_HEART)) {
        next_ms = 0;
        was_on = 0;
        fade_from = 0;
        return;
    }
    uint64_t now = GetTickCount64();
    if (on)
        fade_from = 0;
    // the heartbeat warns, it does not nag - 4 beats at full level, then it fades over 2 more and stays quiet while health stays
    // where it is; a fresh hit (8 % of max lower than where it last started) brings it back
    if (on
        && (!was_on || hp < arm_hp - 0.08f * (mx > 10.0f ? mx : 100.0f))) {
        beats = 0;
        arm_hp = hp;
    } else if (was_on && !fade_from)
        fade_from = now;        // healed out of the low band: fade
    was_on = on;
    float g = HB_GAIN, iv;
    if (on) {
        float frac = hp / (mx > 10.0f ? mx : 100.0f);   // 0.30 .. 0
        iv = 0.55f + (1.25f - 0.55f) * (frac < 0.0f ? 0.0f : frac >
                                        0.30f ? 1.0f : frac / 0.30f);
    } else if (fade_from && now - fade_from < HB_FADE_MS) {
        float u = (float) (now - fade_from) / (float) HB_FADE_MS;       // 0 -> 1
        g = HB_GAIN * (1.0f - u) * (1.0f - u);
        iv = last_iv + (1.6f - last_iv) * u;    // the pulse calms down as it fades
    } else {
        next_ms = 0;
        fade_from = 0;
        return;
    }
    if (now < next_ms)
        return;
    if (on)
        last_iv = iv;
    next_ms = now + (uint64_t) (iv * 1000.0f);
    if (on) {
        int b = beats++;
        if (b >= 6)
            return;             // faded out: quiet until a fresh hit
        if (b == 4)
            g *= 0.5f;
        else if (b == 5)
            g *= 0.2f;
    } else if (beats >= 6) {
        next_ms = 0;
        fade_from = 0;
        return;
    }                           // already faded: no heal tail
    int ok =
        PcmPlayStereo(sh_blob, sh_blob + SH_HB_N, SH_BEAT_N, g, SH_RATE);
    if (LOGCAP(nl, 12))
        Xlog("immersion: heartbeat at hp %.0f, gain %.2f%s, next in %.2f s -> %s", (double) hp, (double) g, on ? "" : " (fading)", (double) iv, ok ? "playing" : "mixer full");
}

// ---- blast muffle: the ducking snapshot, applied every frame inside the window imm_blast_note opens (the engine drops it on its own once the calls stop,
// over the snapshot's 0.4 s release; same mechanism as imm_end_duck / mix_under_black). Only while we are live (a death / down has its own mix).
static void ImmMuffleTick(int live, int stunned)
{
    static int nl, ns;
    static uint64_t sid;
    uint64_t now = GetTickCount64();
    // a stunned player gets a guaranteed audio blur - the shock effect, hearing everything muffled: the same snapshot for the
    // whole stun lock, burst near or far (set-per-frame: gone the frame the stun ends)
    if (stunned && IMM_MF_ON()) {
        // the 9..16 dB blast duck was not heard as a stun. Own snapshot, 24..28 dB deep (mix-tut-slowdown, rewritten by
        // dc_data_patches.patch_duck_snapshot; snapshots are gain tables only, no low-pass exists), our own effort voice and the HUD kept
        static uint64_t ssid;
        if (!ssid)
            ssid = FNV64("mix-tut-slowdown");
        uint64_t a[1] = { ssid };
        uint64_t r = DcCall(FNV64("set-per-frame-mixer-override"), 1, a);
        if (ns < 6 && !(g_frame % 30)) {
            ns++;
            Xlog("immersion: stun muffle snapshot applied (mix-tut-slowdown -> %llx)", (unsigned long long) r);
        }
        return;
    }
    if (!live || now >= g_imm_mf_until)
        return;
    if (!sid)
        sid = FNV64("mix-listen-mode-abbie-ellie-fight");
    uint64_t a[1] = { sid };
    uint64_t r = DcCall(FNV64("set-per-frame-mixer-override"), 1, a);
    if (nl < 6) {
        nl++;
        Xlog("immersion: blast muffle snapshot applied, %llu ms into the window (set-per-frame-mixer-override -> %llx)", (unsigned long long) (now - g_imm_mf_from), (unsigned long long) r);
    }
}

// ---- low-health screen: the cut multiplayer render presets of render-settings/zgame.bin, applied with the SP native
// override-render-settings(rs, blend-in, blend-out, 0, priority, 0, 1, 0, strength) (9 values; SP's workbench / Jackson-workbench state scripts call it in
// their update block every frame with (sid, 1.0, -1.0 | 0, 0, 32767, 0, 1, 0, 1.0); winmm's menu lock every frame with priority 1000). The presets are
// SPARSE (only post:* keys: *-02-low* = red vignette + grain + chromatic aberration; *-00-high* and *-01-mid* hold no keys at all, so "mid" is the
// low preset at a part strength). Called every frame while wanted, not at all otherwise: nothing is left set (the call is per-frame), so every exit
// (death, down, respawn, match end, opt-out) is an immediate hard clear.
static int DuDeadNow(void);   // death_ui.c: the death screen runs / its kill signal is fresh
static uint64_t ImmF2u(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static volatile int g_rslOurs; // rslog.c: this render-settings call is ours
static uint64_t ImmRsCall(uint64_t sid, int prio, float strength)
{
    uint64_t a[9] =
        { sid, ImmF2u(0.0f), ImmF2u(0.0f), 0, (uint64_t) prio, 0, 1, 0,
ImmF2u(strength) };
    g_rslOurs = 1;
    uint64_t r_ = DcCall(FNV64("override-render-settings"), 9, a);
    g_rslOurs = 0;
    return r_;
}

// strength by health fraction: 0 above 60 %, 0..0.5 over the mid band 60..30 %, 0.5..1 over the low band 30..0 % (30 % = the heartbeat / wounded-breath edge)
static float ImmHsTarget(float frac)
{
    if (frac >= 0.60f)
        return 0.0f;
    if (frac > 0.30f)
        return 0.5f * (0.60f - frac) / 0.30f;
    return 0.5f + 0.5f * (0.30f - (frac < 0.0f ? 0.0f : frac)) / 0.30f;
}

static void ImmScreenTick(int live)
{
    static double t_prev;
    static float env, prev_hp = -1.0f, pain;
    static int active, nl, band_prev = -1, ncall;
    static uint64_t sid_low, sid_pain, sid_dark;
    double t = GshNow();
    float dt = t_prev ? (float) (t - t_prev) : 0.016f;
    t_prev = t;
    dt = dt < 0.0f ? 0.0f : dt > 0.1f ? 0.1f : dt;
    float hp = live ? LocalHp() : 0.0f, mx =
        g_wgMax > 10.0f ? g_wgMax : 100.0f;
    if (!sid_low) {
        sid_low = FNV64("*zgame-mp-player-health-02-low*");
        sid_pain = FNV64("*zgame-mp-player-pain-kill*");
        sid_dark = FNV64("*zgame-player-peek-mode*");
    }
    // downed = a DARK edge vignette (tunnel vision: the pure-vignette SP preset *zgame-player-peek-mode*, density 0.6, softness 1.0, no
    // tint keys = black) at priority 999, with the red bleedout vignette (*zgame-mp-player-health-02-low* at 0.45) layered over it at priority 1000, plus
    // the pain pulse that was running. Both fade in over ~0.2 s on the down, out over ~0.5 s on a revive, and carry through a special execution ON US
    // (psync role 2 / gx role 2: the latch remembers we were downed when it began); gone at once the moment the death screen / kill / death anim shows
    // and at respawn / match end. Per-frame call, no call = cleared.
    static float denv, kenv;
    static int dlog, dlatch;
    int dead = DuDeadNow() || (live ? 0 : (LocalHp() <= 0.0f))
        || g_downDying || g_uc4DyingUntil;
    int xvic = (PsyncActive() && g_ps.role == 2 && g_ps.kind != PS_REVIVE)
        || GxVictimActive();
    int want = 0;
    {
        int in_m = MatchSubstate() == 6 && IMM_HS_ON() && !dead;
        want = in_m && ((g_downed && (!PsyncActive() || xvic))
                        || (dlatch && xvic));
        dlatch = want;
        float dt_ = want ? 0.45f : 0.0f, ds =
            (dt_ > denv ? 2.5f : -0.9f) * dt, nd = denv + ds;
        if ((ds > 0.0f && nd > dt_) || (ds < 0.0f && nd < dt_))
            nd = dt_;
        float kt_ = want ? 1.0f : 0.0f, ks =
            (kt_ > kenv ? 4.0f : -2.0f) * dt, nk = kenv + ks;
        if ((ks > 0.0f && nk > kt_) || (ks < 0.0f && nk < kt_))
            nk = kt_;
        if (dead || !in_m) {
            nd = 0.0f;
            nk = 0.0f;
        }                       // death screen / death anim / respawn / match end / opt-out: an instant cut, no fade
        if ((nd > 0.02f) != (denv > 0.02f) && dlog < 60) {
            dlog++;
            Xlog("immersion: downed vignette %s (dark edge + red bleedout%s)", nd > 0.02f ? "fading in" : dead ? "cut (death)" : "off", xvic ? ", during a special execution on us" : "");
        }
        denv = nd;
        kenv = nk;
    }
    if (!(live && hp > 0.0f && IMM_HS_ON())) {
        if (kenv > 0.02f)
            ImmRsCall(sid_dark, 999, kenv > 1.0f ? 1.0f : kenv);
        if (denv > 0.02f)
            ImmRsCall(sid_low, 1000, denv);
        if (want || denv > 0.02f) {     // the pain pulse (blood screen) runs on through the down and the execution
            pain -= dt / 0.5f;
            if (pain > 0.02f)
                ImmRsCall(sid_pain, 1001, pain);
            else
                pain = 0.0f;
        } else
            pain = 0.0f;
        if (active && nl < 80) {
            nl++;
            Xlog("immersion: health screen cleared (%s)",
                 !live ? "not live: down / death / respawn / match end" :
                 hp <= 0.0f ? "no health" : "opt-out file");
        }
        env = 0.0f;
        prev_hp = -1.0f;
        active = 0;
        band_prev = -1;
        return;
    }
    float frac = hp / mx, tgt = ImmHsTarget(frac);
    if (g_xb.on && tgt < 0.6f)
        tgt = 0.6f;             // (was 1.0: the low preset near full strength flashed black after a couple of seconds)          // impaled by a crossbow bolt = the full low-health screen, whatever the health
    {                           // blend in over ~0.7 s per half band, out slower (~1.4 s from full)
        float step = (tgt > env ? 1.5f : -0.7f) * dt, ne = env + step;
        if ((step > 0.0f && ne > tgt) || (step < 0.0f && ne < tgt))
            ne = tgt;
        env = ne;
    }
    if (prev_hp > 0.0f && prev_hp - hp >= 5.0f) {       // hurt: the pain preset as a short pulse (0.5 s)
        float p = 0.45f + (prev_hp - hp) / mx * 2.0f;
        if (p > pain)
            pain = p > 1.0f ? 1.0f : p;
    }
    pain -= dt / 0.5f;
    if (pain < 0.0f)
        pain = 0.0f;
    {
        float bl = MhfxPainLevel();
        if (bl > pain)
            pain = bl;
    }                           // melee_hitfx.c: a blade cut's red overlay
    prev_hp = hp;
    int band = frac >= 0.60f ? 0 : frac > 0.30f ? 1 : 2;
    if (band != band_prev) {
        if (nl < 80) {
            nl++;
            Xlog("immersion: health screen band %s (hp %.0f / %.0f = %.0f%%), target strength %.2f", band == 0 ? "high" : band == 1 ? "mid" : "low", (double) hp, (double) mx, (double) (frac * 100.0f), (double) tgt);
        }
        band_prev = band;
    }
    if (kenv > 0.02f)
        ImmRsCall(sid_dark, 999, kenv > 1.0f ? 1.0f : kenv);
    if (env > 0.02f || denv > 0.02f) {
        float Sv = env > denv ? env : denv;
        uint64_t r = ImmRsCall(sid_low, 1000, Sv > 1.0f ? 1.0f : Sv);
        if (!active) {
            active = 1;
            Xlog("immersion: health screen on (override-render-settings *zgame-mp-player-health-02-low* strength %.2f -> %llx)", (double) env, (unsigned long long) r);
        } else if (ncall < 3)
            Xlog("immersion: health screen call -> %llx (strength %.2f)",
                 (unsigned long long) r, (double) env);
        ncall++;
    } else if (active) {
        active = 0;
        Xlog("immersion: health screen faded out");
    }
    if (pain > 0.02f) {
        static int np;
        uint64_t r = ImmRsCall(sid_pain, 1001, pain);
        if (LOGCAP(np, 4))
            Xlog("immersion: pain pulse %.2f (override-render-settings *zgame-mp-player-pain-kill* -> %llx)", (double) pain, (unsigned long long) r);
    }
}

// ---- stun blur (a blur effect that fades in and slowly fades out over the stun, being 0 around 0.1 s before the character gets back
// input). Engine route: the same override-render-settings call as the health screen, with SP's depth-of-field presets of zgame.bin (keys decoded
// with the sidbase; the record holds {sid, f32 value, i32 -1} per key):
//   *zgame-player-approach-mortal-state*  post:enable-depth-of-field, dof-focus-plane-dist 1.3 m, dof-background-blur-scale 7.0 - everything beyond 1.3 m
//                                         blurred strongly (the SP "about to die" focus pull); strength = the envelope, priority 1002
//   *zgame-player-damage-punch*           the SP punched-in-the-face daze: dof-foreground-blur-scale 1, f/11, focal 38, focus 1.8 m, mblur-blur-scale 2,
//                                         saturation 0.185, fisheye -0.095, grain 0.83, chromatic aberration 0.006; at 0.4 x the envelope, priority 1001
// Envelope: in over 0.2 s, held to 0.8 s, then a raised-cosine fade to exactly 0 at STUN_SECONDS - 0.1 s (the lock ends at g_stun_until = input back).
// The first version showed nothing visible: the priorities were 1001 / 1002. SP's own calls of these presets (every melee-hit script of
// melee/npc-*.dcpl: override-render-settings(*zgame-player-damage-punch*, 0, 4, 0, 32767, 0, 1, 2, 1.0)) use the engine maximum 32767, i.e. they
// outrank every level / MP render-settings layer that tunes the same dof / saturation / mblur keys; a level layer above 1002 would hide ours (the
// health vignette keys sit in no level layer, which is why those rendered at 1000). Now punch = 32767, mortal-state = 32766 (its focus plane 1.3 m
// and background blur 7 win over the punch's 1.8 m; the punch brings f/11 + 38 mm + foreground blur + the SP daze: the mortal-state alone has no
// f-number / focal length, so its blur depends on the camera defaults). Punch strength 0.6 x the envelope (saturation 0.19, grain 0.83, fisheye show
// even where the DoF does not). With Settings > Graphics > Depth of Field OFF (PCGraphicsSettings_DepthOfField) only those daze keys can show.
// Per-frame call, none outside the stun = hard clear on any exit (down, death, match end). t2r-no-stun-blur.txt: off. UNTESTED in game: the log shows what the engine answered.
static void ImmStunBlurTick(int stunned)
{
    static int nl, on_prev;
    static uint64_t sid_dof, sid_punch;
    static int off = -1;
    static uint64_t off_at;
    if (!stunned || FeelFlag("t2r-no-stun-blur.txt", &off_at, &off, 180)
        || !g_stun_total) {
        if (on_prev) {
            on_prev = 0;
            Xlog("immersion: stun blur off");
        }
        return;
    }
    if (!sid_dof) {
        sid_dof = FNV64("*zgame-player-approach-mortal-state*");
        sid_punch = FNV64("*zgame-player-damage-punch*");
    }
    float total = (float) g_stun_total / 60.0f, left =
        (float) (g_stun_until - g_t60) / 60.0f, el = total - left;
    float end = total - 0.1f, env;
    if (el >= end)
        env = 0.0f;
    else if (el < 0.2f)
        env = el / 0.2f;
    else if (el < 0.8f)
        env = 1.0f;
    else {
        float u = (el - 0.8f) / (end - 0.8f);
        env = 0.5f * (1.0f + __builtin_cosf(3.14159265f * u));
    }
    if (env < 0.01f) {
        if (on_prev) {
            on_prev = 0;
            Xlog("immersion: stun blur faded out at %.2f s of %.2f s",
                 (double) el, (double) total);
        }
        return;
    }
    uint64_t r1 = ImmRsCall(sid_dof, 32766, env), r2 =
        ImmRsCall(sid_punch, 32767, 0.6f * env);
    if (!on_prev) {
        on_prev = 1;
        Xlog("immersion: stun blur on (override-render-settings approach-mortal-state -> %llx, damage-punch -> %llx, total %.2f s)", (unsigned long long) r1, (unsigned long long) r2, (double) total);
    } else if (LOGCAP(nl, 6))
        Xlog("immersion: stun blur %.2f at %.2f s", (double) env,
             (double) el);
}

// ---- near miss + heavy gunfire shake (the NEAR MISS part now uses the replicated shot lines, MSG_SHOTRAY below; the facing-based inference described here is gone, the sound request signal only drives the gunfire shake). Signal = the engine's own sound request for an enemy puppet's gun shot: the sound request build FUN_141dd7630
// (rdx = event sid, r8 = &owner handle, hooked by sp_parity.c's spp_snd_hook_install, the census hook; the remote isFiring byte peer+0x89 is almost
// never set, see the combat music notes) filtered to the gun events *-npc* below. The shot line is INFERRED, not replicated: the puppet's own position
// and body facing (net quaternion, forward = +Z of the root: the same formula the back-shiv test uses); no aim pitch is known, so only the horizontal
// distance of us from that line counts (1.5 m), plus the shooter must see us (los_gate) and the shot must not be the one that hit us (a damaging hit
// from that slot within 0.25 s cancels the whizz). A teammate's / our own shots are no near miss (own body skipped).
#include "sp_bullet_by.h"
__asm__
    (".section .rdata,\"dr\"\n.balign 4\n.globl sb_blob\nsb_blob:\n.incbin \"sp_bullet_by.pcm\"\n.text\n");
extern const int16_t sb_blob[];
static int SppSndHookInstall(void);  // sp_parity.c
static uint8_t *SppHandleObj(const uint8_t * h);      // sp_parity.c
// class: 0 pistol / smg, 1 rifle, 2 shotgun (heavy), 3 magnum / bolt / lever (heavy-ish)
// being shot = any damaging hit on us in the last 3 s: no gunfire shake then (the render camera moving off the aim cost fights; blasts keep theirs)
static int ImmBeingShot(uint64_t now)
{
    for (int i = 0; i < T2R_MAX_PEERS; i++)
        if (g_hitByMs[i] && now - g_hitByMs[i] < 3000)
            return 1;
    return 0;
}

static const struct {
    const char *ev;
    int cls;
} IMM_SHOT_EV[] = {
    { "sfx-pistol-beretta-npc", 0 }, { "sfx-pistol-beretta-npc-mp", 0 },
        { "sfx-pistol-beretta-npc-suppressed", 0 },
        { "sfx-pistol-beretta-npc-suppressed-mp", 0 },
    { "sfx-pistol-browning-npc", 0 }, { "sfx-pistol-browning-scar-npc",
                                       0 },
        { "sfx-pistol-colt-1911-manny-npc", 0 },
        { "sfx-pistol-colt-1911-tommy-npc", 0 },
    { "sfx-pistol-cz75-dina-npc", 0 }, { "sfx-pistol-deagle-npc-mp", 3 },
        { "sfx-pistol-deagle-npc-suppressed-mp", 3 },
        { "sfx-pistol-glock-burst-npc-fac", 0 },
    { "sfx-pistol-glock-burst-npc-suppressed-fac", 0 },
        { "sfx-pistol-glock-npc", 0 }, { "sfx-pistol-glock-npc-suppressed",
                                        0 },
    { "sfx-pistol-revolver-357-npc", 3 },
        { "sfx-pistol-revolver-357-npc-mp", 3 },
        { "sfx-pistol-revolver-357-npc-suppressed-mp", 3 },
    { "sfx-pistol-revolver-scar-npc", 3 },
        { "sfx-pistol-revolver-taurus-npc", 3 },
        { "sfx-pistol-revolver-taurus-npc-joel", 3 },
        { "sfx-pistol-ruger-npc", 0 },
    { "sfx-pistol-shotgun-npc-mp", 2 }, { "sfx-pistol-xcaliber-npc", 0 },
        { "sfx-rifle-bolt-scar-npc", 3 }, { "sfx-rifle-dina-npc", 1 },
    { "sfx-rifle-lever-action-npc", 3 }, { "sfx-rifle-m14-npc", 1 },
        { "sfx-rifle-m4-npc", 1 }, { "sfx-rifle-mini-14-npc", 1 },
        { "sfx-rifle-mini-14-npc-mp", 1 },
    { "sfx-rifle-mini-14-npc-suppressed-mp", 1 }, { "sfx-rifle-mpx5-npc",
                                                   1 },
        { "sfx-rifle-remington-bolt-npc", 3 },
        { "sfx-rifle-remington-bolt-npc-mp", 3 },
    { "sfx-rifle-tommy-npc", 1 }, { "sfx-rifle-vepr-npc", 1 },
        { "sfx-rifle-vepr-npc-mp", 1 },
        { "sfx-shotgun-baserri-over-under-npc", 2 },
    { "sfx-shotgun-basic-npc-mp", 2 },
        { "sfx-shotgun-basic-npc-suppressed-mp", 2 },
        { "sfx-shotgun-dbl-barrel-abby-npc", 2 },
    { "sfx-shotgun-dbl-barrel-scar-npc", 2 }, { "sfx-shotgun-pump-npc",
                                               2 },
        { "sfx-shotgun-remington-pump-npc", 2 },
        { "sfx-shotgun-remington-pump-npc-joel", 2 },
    { "sfx-shotgun-remington-pump-npc-mel", 2 },
        { "sfx-shotgun-remington-pump-npc-mp", 2 }
};

#define IMM_SHOT_N ((int)(sizeof IMM_SHOT_EV / sizeof IMM_SHOT_EV[0]))
static struct {
    uint64_t sid;
    int cls;
} g_imm_shot[IMM_SHOT_N];       // sorted by sid (the probe bsearches it on the job threads)
static volatile int g_immShotN;
static volatile LONG g_immSndOn;
static struct {
    volatile uint64_t sid, ms;
    volatile uint8_t *obj;
    volatile uint32_t cls, seq;
} g_imm_ring[16];
static volatile LONG g_immRi;
static void ImmSndProbe(uint64_t sid, uint64_t r8)
{                               // job threads: lock-free, a binary search and a ring store
    int n = g_immShotN;
    if (!g_immSndOn || n <= 0)
        return;
    int lo = 0, hi = n - 1, c = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_imm_shot[mid].sid < sid)
            lo = mid + 1;
        else if (g_imm_shot[mid].sid > sid)
            hi = mid - 1;
        else {
            c = g_imm_shot[mid].cls;
            break;
        }
    }
    if (c < 0)
        return;
    LONG i = InterlockedIncrement(&g_immRi);
    int k = i & 15;
    g_imm_ring[k].seq = 0;
    g_imm_ring[k].sid = sid;
    g_imm_ring[k].cls = (uint32_t) c;
    g_imm_ring[k].ms = GetTickCount64();
    g_imm_ring[k].obj =
        (volatile uint8_t *) SppHandleObj((const uint8_t *) r8);
    g_imm_ring[k].seq = (uint32_t) i;
}

// ---- REAL SHOT LINES (whizz-bys for NPC bullet trails). The old near miss INFERRED the line from the shooter's body facing and
// fired 0 times in a night of A/B logs. Now the shooter's own client sends the line it really shot along: MSG_SHOTRAY 0xAF, 17 bytes
//   [u8 0 reserved][f32 x3 origin][i8 x3 direction * 127][u8 length in metres, 0 = unknown (the receiver uses 120 m)].
// Sender: the shot is the held gun's magazine count going DOWN (same weapon, player-is-firing? seen in the last 300 ms: a reload / weapon swap never counts);
// the line is the engine's own camera ray (aim helper player+0x3278 via FUN_140c72440, the call the bow arc uses: origin out+0x10, direction out+0x20), the
// origin moved along it to the point nearest our chest (the bullet leaves the muzzle toward the reticle point, the camera sits behind us). At most 15 a second.
// Receiver (imm_nm_tick): enemy, not us, 3-D closest approach of the segment to our head within 2.5 m, at least 1 m from the muzzle, the shooter's line of
// sight to us not blocked (los_gate), no damaging hit from that slot within 250 ms of the line (that shot hit us) -> the SP bullet-by wave at the closest point.
// t2r-no-shotray.txt = neither sent nor played. UNVERIFIED in game. No tracer (see the note after imm_sray_send_tick).
#define MSG_SHOTRAY 0xAF        // (0x96 is MSG_UPGSYNC)
#define SRAY_RING 16
static struct {
    volatile uint32_t seq;
    uint32_t slot;
    uint64_t ms;
    float o[3], d[3], len;
} g_sray_ring[SRAY_RING];
static volatile LONG g_srayRi;
static int SrayOff(void)
{
    static int off = -1;
    static uint64_t at;
    return FeelFlag("t2r-no-shotray.txt", &at, &off, 180);
}

static void SrayRecv(uint32_t from, const uint8_t *b, uint64_t len)
{
    static int nl;
    if (from >= T2R_MAX_PEERS || !b || len < 17)
        return;
    float o[3];
    memcpy(o, b + 1, 12);
    float d[3] =
        { (float) (int8_t) b[13] / 127.0f, (float) (int8_t) b[14] / 127.0f,
   (float) (int8_t) b[15] / 127.0f };
    float dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    if (!__builtin_isfinite(o[0] + o[1] + o[2])
        || __builtin_fabsf(o[0]) > 1e5f || __builtin_fabsf(o[1]) > 1e5f
        || __builtin_fabsf(o[2]) > 1e5f || dd < 0.5f || dd > 1.5f)
        return;
    float inv = 1.0f / __builtin_sqrtf(dd);
    LONG i = InterlockedIncrement(&g_srayRi);
    int k = i & (SRAY_RING - 1);
    g_sray_ring[k].seq = 0;
    g_sray_ring[k].slot = from;
    g_sray_ring[k].ms = GetTickCount64();
    for (int j = 0; j < 3; j++) {
        g_sray_ring[k].o[j] = o[j];
        g_sray_ring[k].d[j] = d[j] * inv;
    }
    g_sray_ring[k].len = b[16] ? (float) b[16] : 120.0f;
    g_sray_ring[k].seq = (uint32_t) i;
    if (LOGCAP(nl, 20))
        Xlog("shotray: received from slot %u origin (%.1f %.1f %.1f) dir (%.2f %.2f %.2f) length %s", from, (double) o[0], (double) o[1], (double) o[2], (double) d[0] * inv, (double) d[1] * inv, (double) d[2] * inv, b[16] ? "known" : "unknown");
}

static void ImmSraySendTick(int live, uint64_t now)
{
    typedef uint8_t *(*fn_me)(void);
    typedef void *(*fn_aim)(void *, void *);
    static int prev = -1, nbad, nsent;
    static uint64_t pw, last_fire, last_send;
    if (!live || SrayOff() || !g_sendTramp) {
        prev = -1;
        return;
    }
    uint64_t w = CurrentWeapon();
    if (!w || w == ~0ULL || !BaseWeaponIsGun(BaseWeapon(w))) {
        prev = -1;
        return;
    }
    uint64_t none = 0, fr = DcCall(FNV64("player-is-firing?"), 0, &none);
    if (fr != ~0ULL && (fr & 0xff))
        last_fire = now;
    int clip = HeldClipRounds();
    if (clip < 0) {
        prev = -1;
        return;
    }
    int shot = prev >= 0 && w == pw && clip < prev && last_fire
        && now - last_fire < 300;
    prev = clip;
    pw = w;
    if (!shot || now - last_send < 66)
        return;
    uint8_t *exe = (uint8_t *) GetModuleHandleW(NULL);
    uint8_t *pl = ((fn_me) (exe + 0xdabea0)) ();
    if (!pl || IsBadReadPtr(pl, 0x5530))
        return;
    uint8_t *aimo = *(uint8_t **) (pl + 0x3278);
    if (!aimo || ((uintptr_t) aimo & 7) || (uintptr_t) aimo < 0x10000
        || IsBadReadPtr(aimo, 0x40) || *(uint8_t **) aimo != pl) {
        if (LOGCAP(nbad, 4))
            Xlog("shotray: no aim helper (player+0x3278) - shot line not sent");
        return;
    }
    float out[12] __attribute__((aligned(16))) = { 0 };
    ((fn_aim) (exe + 0xc72440)) (aimo, out);
    const float *org = out + 4, *dir = out + 8;
    float dd = dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2];
    if (!__builtin_isfinite
        (org[0] + org[1] + org[2] + dir[0] + dir[1] + dir[2]) || dd < 0.25f
        || dd > 4.0f) {
        if (LOGCAP(nbad, 4))
            Xlog("shotray: camera ray invalid (org %.1f %.1f %.1f dir %.2f %.2f %.2f) - shot line not sent", (double) org[0], (double) org[1], (double) org[2], (double) dir[0], (double) dir[1], (double) dir[2]);
        return;
    }
    float inv = 1.0f / __builtin_sqrtf(dd), d[3] =
        { dir[0] * inv, dir[1] * inv, dir[2] * inv }, o[3] =
        { org[0], org[1], org[2] };
    float mb[16];
    const float *mp = Uc4MyLoc(mb);
    if (mp && __builtin_isfinite(mp[0] + mp[1] + mp[2])) {      // the camera ray's point nearest our chest = where the bullet leaves
        float t =
            (mp[0] - o[0]) * d[0] + (mp[1] + 0.6f - o[1]) * d[1] + (mp[2] -
                                                                    o[2]) *
            d[2];
        if (t < 0.0f)
            t = 0.0f;
        if (t > 6.0f)
            t = 6.0f;
        for (int j = 0; j < 3; j++)
            o[j] += t * d[j];
    }
    uint8_t m[17] = { 0 };
    memcpy(m + 1, o, 12);
    for (int j = 0; j < 3; j++)
        m[13 + j] =
            (uint8_t) (int8_t) (int) (d[j] * 127.0f +
                                      (d[j] < 0 ? -0.5f : 0.5f));
    m[16] = 0;                  // hit distance unknown (no cheap synchronous ray): the receiver uses 120 m
    g_sendTramp(MSG_SHOTRAY, m, sizeof m, 0xff);
    last_send = now;
    if (LOGCAP(nsent, 16))
        Xlog("shotray: sent - weapon %llx clip %d, origin (%.1f %.1f %.1f) dir (%.2f %.2f %.2f)", (unsigned long long) w, clip, (double) o[0], (double) o[1], (double) o[2], (double) d[0], (double) d[1], (double) d[2]);
}

// TRACERS: not implemented. tracer-smoke-basic / tracer-smoke-tommy (t2r-ext.c CFX_DRESSING) are compute RIBBON groups the engine's bullet path drives
// (and they occupy the 23-entry compute table that the blood / gore code already fights over); the only world spawns the ext has are spawn-particles-at-joint /
// spawn-particles-at-bound-frame, which need a joint or locator handle, not an origin + direction, so there is no reliable way to draw a line along an
// arbitrary ray from here.
static void ImmNmTick(int live, uint64_t now)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    typedef void *(*fn_proc)(uint64_t);
    typedef uint8_t *(*fn_me)(void);
    static uint32_t last_seq, sr_last;
    static int inited, hook_try, nl, nls, nlh;
    static uint64_t last_whiz, last_shake, next_los;
    static struct {
        uint64_t due, shot;
        uint32_t slot;
        float lat, along, cp[3];
    } pend[4];
    static int npend;
    if (!inited) {
        inited = 1;
        for (int i = 0; i < IMM_SHOT_N; i++) {
            g_imm_shot[i].sid = FNV64(IMM_SHOT_EV[i].ev);
            g_imm_shot[i].cls = IMM_SHOT_EV[i].cls;
        }
        for (int i = 1; i < IMM_SHOT_N; i++) {
            typeof(g_imm_shot[0]) v = g_imm_shot[i];
            int j = i - 1;
            while (j >= 0 && g_imm_shot[j].sid > v.sid) {
                g_imm_shot[j + 1] = g_imm_shot[j];
                j--;
            }
            g_imm_shot[j + 1] = v;
        }
        g_immShotN = IMM_SHOT_N;
    }
    int in_match = MatchSubstate() == 6, want_nm = in_match
        && IMM_NM_ON(), want_cs = in_match && IMM_CS_ON();
    InterlockedExchange(&g_immSndOn, want_nm || want_cs);
    if (!want_nm && !want_cs) {
        npend = 0;
        last_seq = (uint32_t) g_immRi;
        sr_last = (uint32_t) g_srayRi;
        return;
    }
    if (!hook_try) {
        hook_try = 1;
        int ok = SppSndHookInstall();
        Xlog("immersion: puppet shot signal (sound request hook, %d gun events) %s", IMM_SHOT_N, ok ? "installed" : "NOT available - near miss and gunfire shake stay off");
    }
    // whizzes that waited out the hit check (a hit from that slot in the meantime = that shot hit us, no whizz)
    for (int i = 0; i < npend;) {
        if (now < pend[i].due) {
            i++;
            continue;
        }
        int hit = g_hitByMs[pend[i].slot]
            && g_hitByMs[pend[i].slot] + 250 >= pend[i].shot;
        if (live && !hit && now - last_whiz >= 150) {
            float g, pan, dist, bp[3];
            const char *why = "";
            float cp[3] = { pend[i].cp[0], pend[i].cp[1], pend[i].cp[2] };
            if (!SfxSpatial(0, cp, &g, &pan, &dist, bp, &why))
                pan = (T2rRand() & 1) ? 0.7f : -0.7f;
            float lat = pend[i].lat, along = pend[i].along;
            float lf = (lat - 0.25f) / 2.25f;
            if (lf < 0.0f)
                lf = 0.0f;
            if (lf > 1.0f)
                lf = 1.0f;
            float gain =
                0.95f * (1.0f - 0.55f * lf) * (along <
                                               10.0f ? 1.0f : 1.0f -
                                               (along - 10.0f) / 130.0f);
            int w = (int) (T2rRand() % SB_COUNT), ok;
            float l, r;
            SfxLr(1.0f, gain, pan, &l, &r);
            ok = PcmPlayLr(sb_blob + SB_OFF[w], SB_LEN[w], l, r,
                             SB_RATE);
            if (ok)
                last_whiz = now;
            AlertstingCombat("near miss out of the quiet");
            if (LOGCAP(nl, 24))
                Xlog("immersion: near miss - slot %u, passed %.2f m from us at %.1f m range, pan %+.2f gain %.2f wave %d -> %s", pend[i].slot, (double) lat, (double) along, (double) pan, (double) gain, w, ok ? "playing" : "mixer full");
            // (the near-miss camera flinch is gone: render-camera shake while being shot moved the picture off the aim)
        } else if (hit && nlh++ < 10)
            Xlog("immersion: near miss dropped - slot %u hit us within 250 ms", pend[i].slot);
        pend[i] = pend[--npend];
    }
    uint32_t end = (uint32_t) g_immRi;
    if (end - last_seq > 16)
        last_seq = end - 16;
    uint32_t me = *(uint32_t *) T2R_SYM(kMySlot);
    uint8_t *meobj = ((fn_me) T2R_SYM(kFnLocalPlayer)) ();
    float mb[16];
    const float *mp = live ? Uc4MyLoc(mb) : NULL;
    int mp_ok = mp && __builtin_isfinite(mp[0] + mp[1] + mp[2]);
    if (live && mp_ok && now >= next_los) {     // keep the line-of-sight verdicts of enemies in range warm (rays are async)
        next_los = now + 300;
        for (uint32_t s = 0; s < T2R_MAX_PEERS; s++) {
            if (s == me || IsTeammate(s))
                continue;
            uint8_t *pe = ((fn_peer) T2rPeer) (s);
            if (!pe || IsBadReadPtr(pe, 0x90) || !pe[4] || pe[5])
                continue;
            uint64_t h = *(uint64_t *) (pe + 0x60);
            float pb[16];
            const float *pl = h ? PuppetLocator(h, pb) : NULL;
            if (!pl)
                continue;
            float dx = pl[0] - mp[0], dz = pl[2] - mp[2];
            if (dx * dx + dz * dz < 45.0f * 45.0f)
                LosGate(h, mp, pl);
        }
    }
    for (uint32_t q = last_seq + 1; q - 1 != end; q++) {
        int k = q & 15;
        if (g_imm_ring[k].seq != q)
            continue;
        uint64_t sid = g_imm_ring[k].sid, ms = g_imm_ring[k].ms;
        uint32_t cls = g_imm_ring[k].cls;
        uint8_t *obj = (uint8_t *) g_imm_ring[k].obj;
        (void) sid;
        if (!live || !mp_ok || now - ms > 400 || !obj || obj == meobj)
            continue;
        // whose shot: the owner object is the puppet itself, or (a weapon / child object) the puppet standing next to it
        int slot = -1;
        for (uint32_t s = 0; s < T2R_MAX_PEERS && slot < 0; s++) {
            if (s == me)
                continue;
            uint8_t *pe = ((fn_peer) T2rPeer) (s);
            if (!pe || IsBadReadPtr(pe, 0x90) || !pe[4] || pe[5])
                continue;
            uint64_t h = *(uint64_t *) (pe + 0x60);
            if (h && ((fn_proc) T2R_SYM(kFnEntityFromHandle)) (h) == obj)
                slot = (int) s;
        }
        float op[3] = { 0, 0, 0 };
        if (slot < 0 && ProcPos(obj, op)) {    // not the body itself: a weapon / child object, the puppet within 3 m of it
            float bestd = 3.0f;
            for (uint32_t s = 0; s < T2R_MAX_PEERS; s++) {
                if (s == me)
                    continue;
                uint8_t *pe = ((fn_peer) T2rPeer) (s);
                if (!pe || IsBadReadPtr(pe, 0x90) || !pe[4] || pe[5])
                    continue;
                uint64_t h = *(uint64_t *) (pe + 0x60);
                float pb[16];
                const float *pl = h ? PuppetLocator(h, pb) : NULL;
                if (!pl)
                    continue;
                float dx = pl[0] - op[0], dy = pl[1] - op[1], dz =
                    pl[2] - op[2], dd =
                    __builtin_sqrtf(dx * dx + dy * dy + dz * dz);
                if (dd < bestd) {
                    bestd = dd;
                    slot = (int) s;
                }
            }
        }
        if (slot < 0)
            continue;
        uint8_t *pe = ((fn_peer) T2rPeer) ((uint32_t) slot);
        uint64_t h = pe ? *(uint64_t *) (pe + 0x60) : 0;
        if (!h)
            continue;
        float pb[16];
        const float *pl = PuppetLocator(h, pb);
        if (!pl || !__builtin_isfinite(pl[0] + pl[1] + pl[2]))
            continue;
        float dx = mp[0] - pl[0], dy = mp[1] - pl[1], dz =
            mp[2] - pl[2], d3 =
            __builtin_sqrtf(dx * dx + dy * dy + dz * dz);
        if (want_cs && cls >= 2 && d3 <= 16.0f && now - last_shake >= 140 && !ImmBeingShot(now)) {    // heavy gun fired near: SP-style shake, fall-off to 16 m
            float u = 1.0f - d3 / 16.0f, amp =
                (cls == 2 ? 0.9f : 0.55f) * u * __builtin_sqrtf(u);
            GshAdd(amp, 0.28f, 13.0f);
            last_shake = now;
            if (LOGCAP(nls, 16))
                Xlog("immersion: gunfire shake - slot %d class %u at %.1f m, %.2f deg", slot, cls, (double) d3, (double) amp);
        }
    }
    // the replicated shot lines (MSG_SHOTRAY): closest approach of each enemy line to our head
    uint32_t send = (uint32_t) g_srayRi;
    if (send - sr_last > SRAY_RING)
        sr_last = send - SRAY_RING;
    for (uint32_t q = sr_last + 1; q - 1 != send; q++) {
        int k = q & (SRAY_RING - 1);
        if (g_sray_ring[k].seq != q)
            continue;
        uint32_t slot = g_sray_ring[k].slot;
        uint64_t ms = g_sray_ring[k].ms;
        float o[3] =
            { g_sray_ring[k].o[0], g_sray_ring[k].o[1],
       g_sray_ring[k].o[2] }, d[3] =
{ g_sray_ring[k].d[0], g_sray_ring[k].d[1], g_sray_ring[k].d[2] }, L = g_sray_ring[k].len;
        if (g_sray_ring[k].seq != q || !live || !mp_ok || !want_nm
            || now - ms > 400 || slot == me || IsTeammate(slot)
            || npend >= 4 || now - last_whiz < 150)
            continue;
        float hd[3] = { mp[0], mp[1] + 0.6f, mp[2] };
        float t =
            (hd[0] - o[0]) * d[0] + (hd[1] - o[1]) * d[1] + (hd[2] -
                                                             o[2]) * d[2];
        if (t < 1.0f)
            continue;           // behind the muzzle / right at it
        if (t > L)
            t = L;
        float cp[3] =
            { o[0] + t * d[0], o[1] + t * d[1], o[2] + t * d[2] };
        float lat =
            __builtin_sqrtf((hd[0] - cp[0]) * (hd[0] - cp[0]) +
                            (hd[1] - cp[1]) * (hd[1] - cp[1]) + (hd[2] -
                                                                 cp[2]) *
                            (hd[2] - cp[2]));
        if (lat > 2.5f)
            continue;
        uint8_t *pe = ((fn_peer) T2rPeer) (slot);
        if (!pe || IsBadReadPtr(pe, 0x90) || !pe[4] || pe[5])
            continue;
        uint64_t h = *(uint64_t *) (pe + 0x60);
        float pb[16];
        const float *pl = h ? PuppetLocator(h, pb) : NULL;
        if (pl && __builtin_isfinite(pl[0] + pl[1] + pl[2])
            && !LosGate(h, mp, pl))
            continue;           // a wall between the shooter and us: that shot struck the wall
        pend[npend].due = now + 120;
        pend[npend].shot = ms;
        pend[npend].slot = slot;
        pend[npend].lat = lat;
        pend[npend].along = t;
        pend[npend].cp[0] = cp[0];
        pend[npend].cp[1] = cp[1];
        pend[npend].cp[2] = cp[2];
        npend++;
    }
    sr_last = send;
    last_seq = end;
}

// ---- TEAM callouts (vox kinds VX_RELOAD / VX_NOAMMO / VX_THROWWARN, tools/features/vox_lines.py): the same team-line path as every other
// kind (vx_event: our voice letter, teammates only, cooldowns VX_CD_MS, a female speaker gets the male lines). TLOU2's human enemies have no
// reload bark, so the nearest real lines are used: "Cover me!" / "Watch my back!" (WLF; Seraphite: "Cover me!" / "Keep them down!").
//   t2r-no-reload-call.txt   reload started on an EMPTY magazine with a teammate alive within 25 m: "Cover me!" (cooldown 25 s)
//   t2r-no-noammo-call.txt   held gun dry (clip 0, reserve 0) with a teammate alive within 25 m (cooldown 15 s)
//   t2r-no-throw-warn.txt    an ENEMY throws a molotov / pipe bomb / smoke / stun bomb that lands within 14 m of us (or leaves the hand within 10 m):
//                            molotov = the see-mltv barks ("Molotov!" / "Fire bomb!" / "Get down!"), smoke = see-smoke-bmb, bombs = warn-ally barks (cooldown 4 s)
//   t2r-no-throw-call.txt    OUR OWN throw in a fight: molotov "Light 'em up!" / "Purge them!", pipe bomb "Take them out!", smoke / stun "Move in!" (cooldown 9 s)
// The calls only fire in a match; a teammate within range is needed for the first two (nobody to cover you = no line).
static int ImmTeamNear(float range)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    uint32_t me = *(uint32_t *) T2R_SYM(kMySlot);
    float mb[16], pb[16];
    const float *mp = Uc4MyLoc(mb);
    if (!mp)
        return 0;
    for (uint32_t s = 0; s < T2R_MAX_PEERS; s++) {
        if (s == me || !IsTeammate(s))
            continue;
        uint8_t *pe = ((fn_peer) T2rPeer) (s);
        if (!pe || IsBadReadPtr(pe, 0x90) || !pe[4] || pe[5]
            || g_peerDowned[s])
            continue;
        uint64_t h = *(uint64_t *) (pe + 0x60);
        const float *pp = h ? PuppetLocator(h, pb) : NULL;
        if (!pp)
            continue;
        float dx = pp[0] - mp[0], dz = pp[2] - mp[2];
        if (dx * dx + dz * dz <= range * range)
            return 1;
    }
    return 0;
}
static const char *const IMM_CALL_SWITCH[4] =
    { "t2r-no-reload-call.txt", "t2r-no-noammo-call.txt",
"t2r-no-throw-warn.txt", "t2r-no-throw-call.txt" };
static int g_immCallOff[4] = { -1, -1, -1, -1 };

static uint64_t g_immCallAt[4];
// an active fight = combat music tier up, or enemy contact (shot / hit / shot heard) in the last 8 s (the gate of the reload call and the own-throw calls)
static int ImmFight(uint64_t now)
{
    return g_msTier > 0 || (g_mus_contact_ms
                             && now - g_mus_contact_ms < 8000);
}

static volatile uint64_t g_imm_own_sid, g_imm_own_ms;   // our last throw (send_hook, type 2): handled by imm_calls on the next poll, never from inside the send hook
static void ImmNoteOwnThrow(uint64_t sid)
{
    g_imm_own_sid = sid;
    g_imm_own_ms = GetTickCount64();
}

static int ImmCallOn(int k)
{
    return !FeelFlag(IMM_CALL_SWITCH[k], &g_immCallAt[k],
                      &g_immCallOff[k], 180);
}

// ---- throwable reactions with perception (opt-out t2r-no-vox-rules.txt = the old instant warning below):
// a thrown molotov / bomb is reacted to only when it could be perceived - after it LANDS (the copy comes to rest, or is gone = burst), within 8 m of us - and
// after a human delay of 0.4-0.9 s, never at throw time (the old bark came the frame the throw message arrived and told the target to run). Our own team's
// throwable is silent unless it lands within 4.5 m of us (then the same reaction). One voice: only the teammate nearest the landing point speaks (lowest slot on a tie).
#define IMM_TW_N 6
static struct {
    int on;
    int rp;
    uint64_t born_ms, due;
    uint32_t slot;
    int kind, mate, landed, still;
    float pos[3];
    const char *nm;
} g_imm_tw[IMM_TW_N];
static int ImmNearestTeammateIsMe(const float *at)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    uint32_t me = *(uint32_t *) T2R_SYM(kMySlot);
    float mb[16], pb[16];
    const float *mp = Uc4MyLoc(mb);
    if (!mp)
        return 1;
    float md =
        (mp[0] - at[0]) * (mp[0] - at[0]) + (mp[2] - at[2]) * (mp[2] -
                                                               at[2]);
    for (uint32_t s = 0; s < T2R_MAX_PEERS; s++) {
        if (s == me || !IsTeammate(s))
            continue;
        uint8_t *pe = ((fn_peer) T2rPeer) (s);
        if (!pe || IsBadReadPtr(pe, 0x90) || !pe[4] || pe[5]
            || g_peerDowned[s])
            continue;
        uint64_t h = *(uint64_t *) (pe + 0x60);
        const float *pp = h ? PuppetLocator(h, pb) : NULL;
        if (!pp)
            continue;
        float d =
            (pp[0] - at[0]) * (pp[0] - at[0]) + (pp[2] - at[2]) * (pp[2] -
                                                                   at[2]);
        if (d < md || (d == md && s < me))
            return 0;
    }
    return 1;
}

static void ImmThrowReact(uint64_t now)
{
    static uint64_t seen[MAX_REMOTE_PROJ];
    static int nw;
    uint32_t me = *(uint32_t *) T2R_SYM(kMySlot);
    float mb[16];
    const float *mp = Uc4MyLoc(mb);
    for (int i = 0; i < MAX_REMOTE_PROJ; i++) { // new throws: noted only, nobody reacts yet
        if (!g_rproj[i].born || !g_rproj[i].born_ms
            || g_rproj[i].born_ms == seen[i])
            continue;
        seen[i] = g_rproj[i].born_ms;
        if (now - g_rproj[i].born_ms > 1500 || g_rproj[i].slot == me
            || g_rproj[i].slot >= T2R_MAX_PEERS)
            continue;
        uint64_t sid = g_rproj[i].sid;
        int k = -1;
        for (int t = 0; t < IMM_TW_N; t++)
            if (!g_imm_tw[t].on) {
                k = t;
                break;
            }
        if (k < 0)
            continue;
        memset(&g_imm_tw[k], 0, sizeof g_imm_tw[k]);
        g_imm_tw[k].on = 1;
        g_imm_tw[k].rp = i;
        g_imm_tw[k].born_ms = g_rproj[i].born_ms;
        g_imm_tw[k].slot = g_rproj[i].slot;
        g_imm_tw[k].mate = IsTeammate(g_rproj[i].slot);
        g_imm_tw[k].kind =
            g_rproj[i].molotov ? VX_THROWWARN : sid ==
            0xa2aa922866d0479dULL ? VX_SMOKEWARN : VX_BOMB;
        g_imm_tw[k].nm =
            g_rproj[i].molotov ? "molotov" : sid ==
            0xa2aa922866d0479dULL ? "smoke bomb" : sid ==
            0x8502a9c43b674354ULL ? "stun bomb" : "pipe bomb";
        memcpy(g_imm_tw[k].pos, g_rproj[i].last, 12);
        if (LOGCAP(g_vxrNlog, 300))
            Xlog("vox: rule throw seen - slot %u's %s (%s): no reaction until it lands", g_rproj[i].slot, g_imm_tw[k].nm, g_imm_tw[k].mate ? "our team's" : "enemy");
    }
    for (int t = 0; t < IMM_TW_N; t++) {
        if (!g_imm_tw[t].on)
            continue;
        uint64_t age = now - g_imm_tw[t].born_ms;
        if (age > 8000) {
            g_imm_tw[t].on = 0;
            continue;
        }
        if (!g_imm_tw[t].landed) {
            int i = g_imm_tw[t].rp, gone = !(g_rproj[i].born
                                             && g_rproj[i].born_ms ==
                                             g_imm_tw[t].born_ms);
            if (!gone) {
                const float *l = g_rproj[i].last;
                float mx = l[0] - g_imm_tw[t].pos[0], my =
                    l[1] - g_imm_tw[t].pos[1], mz =
                    l[2] - g_imm_tw[t].pos[2];
                if (mx * mx + my * my + mz * mz < 0.0025f && age > 250)
                    g_imm_tw[t].still++;
                else
                    g_imm_tw[t].still = 0;
                memcpy(g_imm_tw[t].pos, l, 12);
                // (the stunned player shouted "watch out, smoke!" after she finished coughing, when she should be shouting
                // that on seeing the bomb fly): a throwable we can SEE in flight (on screen, within 12 m - 4.5 m for our own team's) is called at once,
                // after a 0.2-0.4 s human delay, instead of waiting for it to land / burst
                float cp[3], cf[3];
                if (mp && age >= 250 && RcamGet(cp, cf)) {
                    float dx = l[0] - mp[0], dy = l[1] - mp[1], dz =
                        l[2] - mp[2], d =
                        __builtin_sqrtf(dx * dx + dy * dy + dz * dz);
                    float vx = l[0] - cp[0], vy = l[1] - cp[1], vz =
                        l[2] - cp[2], vl =
                        __builtin_sqrtf(vx * vx + vy * vy + vz * vz);
                    float dot =
                        vl >
                        0.01f ? (vx * cf[0] + vy * cf[1] +
                                 vz * cf[2]) / vl : 0.0f;
                    if (d <= (g_imm_tw[t].mate ? 4.5f : 12.0f)
                        && dot >= 0.55f && !g_downed
                        && LocalHp() > 0.0f) {
                        g_imm_tw[t].landed = 1;
                        g_imm_tw[t].due =
                            now + 200 + (uint64_t) (T2rRand() % 201u);
                        if (LOGCAP(nw, 60))
                            Xlog("vox: rule throw seen in flight - slot %u's %s %.1f m from us (view %.2f), reaction in %llu ms", g_imm_tw[t].slot, g_imm_tw[t].nm, (double) d, (double) dot, (unsigned long long) (g_imm_tw[t].due - now));
                        continue;
                    }
                }
            }
            if (!(gone || g_imm_tw[t].still >= 2 || age > 3000))
                continue;
            g_imm_tw[t].landed = 1;
            float d =
                mp ? __builtin_sqrtf((g_imm_tw[t].pos[0] - mp[0]) *
                                     (g_imm_tw[t].pos[0] - mp[0]) +
                                     (g_imm_tw[t].pos[1] -
                                      mp[1]) * (g_imm_tw[t].pos[1] -
                                                mp[1]) +
                                     (g_imm_tw[t].pos[2] -
                                      mp[2]) * (g_imm_tw[t].pos[2] -
                                                mp[2])) : 999.0f;
            float lim = g_imm_tw[t].mate ? 4.5f : 8.0f;
            if (d > lim || g_downed || LocalHp() <= 0.0f) {
                if (LOGCAP(nw, 60))
                    Xlog("vox: rule throw landed - slot %u's %s %.1f m from us (perceived within %.1f m%s): no reaction", g_imm_tw[t].slot, g_imm_tw[t].nm, (double) d, (double) lim, g_downed ? ", we are down" : "");
                g_imm_tw[t].on = 0;
                continue;
            }
            g_imm_tw[t].due = now + 400 + (uint64_t) (T2rRand() % 501u);
            if (LOGCAP(nw, 60))
                Xlog("vox: rule throw landed - slot %u's %s %.1f m from us, reaction in %llu ms", g_imm_tw[t].slot, g_imm_tw[t].nm, (double) d, (unsigned long long) (g_imm_tw[t].due - now));
            continue;
        }
        if (now < g_imm_tw[t].due)
            continue;
        g_imm_tw[t].on = 0;
        if (g_downed || LocalHp() <= 0.0f)
            continue;
        if (g_stun_until > g_t60) {
            if (LOGCAP(nw, 60))
                Xlog("vox: rule throw reaction skipped - we are stunned (too late to warn anyone)");
            continue;
        }
        int kind = g_imm_tw[t].kind;
        if (now - g_vx_kind_ms[kind] < VX_CD_MS[kind]) {
            if (LOGCAP(nw, 60))
                Xlog("vox: rule throw reaction kind %d skipped - cooldown",
                     kind);
            continue;
        }
        if (!ImmNearestTeammateIsMe(g_imm_tw[t].pos)) {
            if (LOGCAP(nw, 60))
                Xlog("vox: rule throw reaction kind %d skipped - a teammate is nearer the landing point and speaks", kind);
            continue;
        }
        VxEventEx(kind,
                    g_imm_tw[t].
                    mate ? "our team's bomb landed beside us" :
                    "enemy throwable landed near us", 1);
    }
}

static void ImmCalls(uint64_t now)
{
    static int was_rel, dry_was, nl;
    static uint64_t next_ammo, rp_seen[MAX_REMOTE_PROJ];
    uint32_t me = *(uint32_t *) T2R_SYM(kMySlot);
    uint64_t none = 0;
    uint64_t rr = DcCall(FNV64("player-is-reloading?"), 0, &none);
    int rel = rr != ~0ULL && (rr & 0xff);
    if (rel && !was_rel && ImmCallOn(0)) {
        int clip = AmmoClip();
        if (LOGCAP(nl, 20))
            Xlog("immersion: reload started, clip %d", clip);
        // only in an active fight - combat music tier up, or enemy contact (shot / hit / shot heard) in the last 8 s
        int fight = ImmFight(now);
        if (clip == 0 && fight && ImmTeamNear(25.0f))
            VxEvent(VX_RELOAD, "reload on an empty magazine");
        else if (clip == 0 && !fight && nl < 20)
            Xlog("immersion: reload call skipped - no active fight (tier %d)", g_msTier);
    }
    was_rel = rel;
    if (now >= next_ammo) {
        next_ammo = now + 400;
        uint64_t w = CurrentWeapon();
        int gun = w && w != ~0ULL && BaseWeaponIsGun(BaseWeapon(w));
        int dry = 0;
        if (gun && !rel) {
            int clip = HeldClipRounds();
            dry = clip == 0 && g_heldRes == 0;
        }
        if (dry && !dry_was && ImmCallOn(1) && ImmTeamNear(25.0f))
            VxEvent(VX_NOAMMO, "gun dry, no reserve");
        dry_was = dry;
    }
    // ENEMY throws (molotov lines must actually play): every new remote throwable of a non-teammate within range gets its warning bark
    // (molotov: see-mltv "Molotov!" / "Fire bomb!"; smoke: see-smoke-bmb "Smoke, watch it!"; pipe / nail / stun: the warn-ally barks, kind bomb). It is in
    // range when it LANDS within 14 m of us (the thrower's target point) or leaves the hand within 10 m. A warning is urgent, so it skips the 2.5 s any-kind
    // gap (force) and only keeps its own per-kind cooldown; every decision is logged (the old single log budget of vx_event hid them all).
    if (ImmCallOn(2) && VxrOn())
        ImmThrowReact(now);   // perceived (landed, within 8 m) + human delay; the old branch below fires at throw time
    else if (ImmCallOn(2)) {
        float mb[16];
        const float *mp = Uc4MyLoc(mb);
        static int nw;
        for (int i = 0; i < MAX_REMOTE_PROJ; i++) {
            if (!g_rproj[i].born || !g_rproj[i].born_ms
                || g_rproj[i].born_ms == rp_seen[i])
                continue;
            rp_seen[i] = g_rproj[i].born_ms;
            if (now - g_rproj[i].born_ms > 1500)
                continue;
            if (g_rproj[i].slot == me || g_rproj[i].slot >= T2R_MAX_PEERS
                || IsTeammate(g_rproj[i].slot))
                continue;
            uint64_t sid = g_rproj[i].sid;
            int kind =
                g_rproj[i].molotov ? VX_THROWWARN : sid ==
                0xa2aa922866d0479dULL ? VX_SMOKEWARN : VX_BOMB;
            const char *nm =
                g_rproj[i].molotov ? "molotov" : sid ==
                0xa2aa922866d0479dULL ? "smoke bomb" : sid ==
                0x8502a9c43b674354ULL ? "stun bomb" : "pipe bomb";
            float dstart = -1.0f, dland = -1.0f;
            if (mp) {
                dstart =
                    __builtin_sqrtf((g_rproj[i].last[0] -
                                     mp[0]) * (g_rproj[i].last[0] -
                                               mp[0]) +
                                    (g_rproj[i].last[2] -
                                     mp[2]) * (g_rproj[i].last[2] -
                                               mp[2]));
                for (int k = 0; k < MAX_THROWS; k++)    // the thrower's target point of this very throw (same frame, slot, sid)
                    if (g_throws[k].frame == g_rproj[i].born
                        && g_throws[k].slot == g_rproj[i].slot
                        && g_throws[k].sid == sid) {
                        dland =
                            __builtin_sqrtf((g_throws[k].at[0] -
                                             mp[0]) * (g_throws[k].at[0] -
                                                       mp[0]) +
                                            (g_throws[k].at[2] -
                                             mp[2]) * (g_throws[k].at[2] -
                                                       mp[2]));
                        break;
                    }
            }
            int in_rng = mp && (dstart <= 10.0f
                                || (dland >= 0.0f ? dland <=
                                    14.0f : dstart <= 20.0f));
            if (!in_rng) {
                if (LOGCAP(nw, 60))
                    Xlog("immersion: throw warn skipped - slot %u's %s too far (leaves the hand %.1f m, lands %.1f m from us)", g_rproj[i].slot, nm, (double) dstart, (double) dland);
                continue;
            }
            if (now - g_vx_kind_ms[kind] < VX_CD_MS[kind]) {
                if (LOGCAP(nw, 60))
                    Xlog("immersion: throw warn skipped - slot %u's %s, kind %d on cooldown (%llu ms ago)", g_rproj[i].slot, nm, kind, (unsigned long long) (now - g_vx_kind_ms[kind]));
                continue;
            }
            if (LOGCAP(nw, 60))
                Xlog("immersion: throw warn - slot %u's %s (leaves the hand %.1f m, lands %.1f m from us) -> kind %d", g_rproj[i].slot, nm, (double) dstart, (double) dland, kind);
            VxEventEx(kind,
                        g_rproj[i].
                        molotov ? "enemy molotov thrown near us" :
                        "enemy bomb thrown near us", 1);
        }
    }
    // OWN throws: a fitting TLOU2 bark when WE throw a molotov / pipe bomb / smoke / stun - ONLY in a fight (the reload call's gate), with the
    // normal gap + per-kind cooldown; heard by the team like every callout (vx_event). t2r-no-throw-call.txt = off. send_hook only records the throw.
    {
        static uint64_t own_seen;
        static int no;
        uint64_t osid = g_imm_own_sid, oms = g_imm_own_ms;
        if (osid && oms && oms != own_seen) {
            own_seen = oms;
            int kind =
                osid == 0x2a3164cb4a1c95d9ULL ? VX_THROWMOLO : osid ==
                0xcf576d288d014eacULL ? VX_THROWBOMB : (osid ==
                                                        0x8502a9c43b674354ULL
                                                        || osid ==
                                                        0xa2aa922866d0479dULL)
                ? VX_THROWSMOKE : -1;
            if (kind >= 0 && now - oms < 1500) {
                if (!ImmCallOn(3)) {
                    if (LOGCAP(no, 40))
                        Xlog("immersion: own throw call off (t2r-no-throw-call.txt)");
                } else if (!ImmFight(now)) {
                    if (LOGCAP(no, 40))
                        Xlog("immersion: own throw %llx - no line, no active fight (tier %d)", (unsigned long long) osid, g_msTier);
                } else {
                    if (LOGCAP(no, 40))
                        Xlog("immersion: own throw %llx in a fight (tier %d, contact %llu ms ago) -> kind %d", (unsigned long long) osid, g_msTier, (unsigned long long) (g_mus_contact_ms ? now - g_mus_contact_ms : 0), kind);
                    VxEvent(kind, "our own throw in a fight");
                }
            }
        }
    }
}

// ---- end-of-match mix (t2r-no-end-duck.txt): SP / No Return post-match pages play under the game's own `mix-rl-post-match-ducking` snapshot
// (world ambience and weapons down, music and UI up; re/no-return-analysis.md 4.3). Applied every frame with set-per-frame-mixer-override (the call
// mix_under_black uses) from 0.4 s after winmm's end tick while the end hold runs; the engine drops the override on its own once the calls stop.
static void ImmEndDuck(void)
{
    static int off = -1;
    static uint64_t off_at, nl;
    uint8_t *lc = LobbyObj();
    if (!lc || IsBadReadPtr(lc + 0x2ca4, 1) || !lc[0x2ca4]
        || MatchSubstate() != 6)
        return;
    if (FeelFlag("t2r-no-end-duck.txt", &off_at, &off, 180))
        return;
    uint64_t el = GetTickCount64() - *(uint64_t *) (lc + 0x2ca8);
    if (el < 400 || el > 9000)
        return;
    uint64_t a[1] = { FNV64("mix-rl-post-match-ducking") };
    uint64_t r = DcCall(FNV64("set-per-frame-mixer-override"), 1, a);
    if (!nl++)
        Xlog("immersion: end-of-match ducking mix applied (set-per-frame-mixer-override -> %llx)", (unsigned long long) r);
}

// ---- Part I Bookstore exposure: the converted Part I levels sit outside every render-settings region the engine gathers (the 23 regions of
// hun-checkpoint-bookstore-rs are logged into the region manager but the camera probe still reads the defaults), so the
// baked interior renders ~2^4 too dark. Region-free route: the same per-frame native the health screen uses, override-render-settings(sym, 0, 0, 0, prio, 0, 1, 0, 1.0), with
// Part I's own symbols (registered by the map's t1x module: *t1-rs-hun-const-base* = whole city, *hun-rs-hun-checkpoint-bookstore-base* = the interior;
// tonemap light range 4 .. 4.85, set point 1.558, sun, sky). Only while hun-checkpoint-bookstore is loaded and the match runs.
//   t2r-no-p1-rs-override.txt   off (compare with the region route alone)
//   t2r-p1-rs-override.txt      optional text "<symbol> <priority> <strength>" for the interior layer (default *hun-rs-hun-checkpoint-bookstore-base* 100 1.0)
static int g_p1oV = -1;
static uint64_t g_p1oA;
// override layers: default = const base (prio 50) + the interior base (prio 100). t2r-p1-rs-override.txt (re-read every 2 s, so a tuning run needs no
// restart) replaces them with one layer per line: "<symbol> <priority> <strength>" (up to 8; '#' lines ignored). A layer may be any TLOU2 symbol
// the level set registers (e.g. *flo-rs-100-bookstore*, *sea-rs-sea-hub-global*). Every 2 s (first 12 samples) the log gets the render settings at the camera
// (immersion: P1 rs probe ...) so a run shows whether the layers reach the renderer at all.
typedef struct {
    uint64_t sid;
    int prio;
    float str;
    char name[96];
} ImmP1Layer;
static void ImmP1Probe(int *n)
{
    static const char *const K[] =
        { "tonemap:tonemap-light-range-min",
"tonemap:tonemap-light-range-max", "tonemap:tonemap-set-point",
        "lighting:ambient-background-scale",
            "lighting:ambient-foreground-scale",
            "lighting:ibl-scale-background",
            "lighting:bg-dual-lightmap-blend",
        "lighting:basis3-spec-scale", "lighting:sun-light:rgbi-i",
            "lighting:ssao-intensity-scale", "lighting:sun-light:dir-x",
            "lighting:sun-light:dir-z"
    };
    char out[640];
    size_t at = 0;
    for (size_t i = 0; i < sizeof K / sizeof K[0]; i++) {
        uint64_t one = FNV64(K[i]), r =
            DcCall(FNV64("get-render-setting-value-float"), 1, &one);
        float f;
        uint32_t l = (uint32_t) r;
        memcpy(&f, &l, 4);
        at +=
            snprintf(out + at, sizeof out - at, " %s=%g",
                     strchr(K[i], ':') + 1, (double) f);
    }
    Xlog("immersion: P1 rs probe #%d:%s", ++*n, out);
}

// the same per-frame override for other pack levels that need it (lvl 1 = the winter Supermarket, pat-jackson-supermarket: SP lights
// its interior with Ellie's flashlight, MP has none). Layers come from t2r-rs-override-<level>.txt (same format); a level with no defaults and
// no file gets no override.
#include "sp_lighting.h"        // GENERATED (tools/maps/sp_lighting.py header): per SP map the pack level, interior boxes, forced EVs, override layers (sp_maps.json "sp_lighting")
#include "p1_lighting.h"        // GENERATED (tools/part1/p1_trace_zones.py header): Part I Bookstore render-settings regions (exact volumes, prio, blend) for imm_p1_regions_tick
static const char *const IMM_RS_LEVELS[] = { "hun-checkpoint-bookstore" SPL_LEVEL_ITEMS };      // [0] Part I Bookstore, [1 + k] = g_spl[k].level

// third layer = *t2r-p1-ambient-fill* (sp_maps_build, T2R_P1_FILL=0 drops it): ambient fill + tonemap-exposure-method 1 (auto). Part I's symbols leave the
// method unset and the engine default is 0 = manual EV 0 (exe+13f92fa), i.e. black and deaf to force-exposure-value; this layer fixes that without a reconvert.
static const char *const IMM_RS_DEF[][3] =
    { { "*t1-rs-hun-const-base*", "*hun-rs-hun-checkpoint-bookstore-base*",
       "*t2r-p1-ambient-fill*" }, { 0, 0, 0 } };
static int g_immRsLevel = -1; // index into IMM_RS_LEVELS of the loaded pack level (-1 none), set by imm_p1_bookstore_tick
// Winter Supermarket: SP lights the store interior with Ellie's flashlight; in MP it is near black under auto exposure, and
// override-render-settings symbols of the pat set do not move the tonemap range. force-exposure-value (absolute EV, latched by the engine
// until released, see imm_exposure_release) does: 0.0 looks like auto exposure in the snow lot, 4.0 makes the store and the post office readable. So while the level is up we force
// it every frame, blended over ~0.8 s (eye adaptation) by whether the camera is inside the store block. Tuning: t2r-winter-exposure.txt =
// "x0 x1 z0 z1 ev_in ev_out" (re-read every 2 s); t2r-no-winter-exposure.txt = off (then the last forced value stays until the level is left).
// force-exposure-value (exe+d400c0) stores the EV at table+0x234 and sets the flag byte +0x238 = 1 for screen idx 0/1 (table exe+8334e40,
// stride 0x23c); the tonemap setup (exe+13f92ff) reads it only under auto exposure. Nothing in the engine ever clears the flag, so a forced
// EV would follow us into the next map: we clear it ourselves (outside the store once the blend is done, and when the level is left).
static void ImmExposureRelease(void)
{
    uint8_t *t = (uint8_t *) GetModuleHandleA(NULL) + 0x8334e40;
    for (int i = 0; i < 2; i++) {
        uint8_t *f = t + i * 0x23c + 0x238;
        if (!IsBadWritePtr(f, 1))
            *f = 0;
    }
}

// The forced-EV volume is the map's INTERIOR only (sp_maps.json "sp_lighting".interior = SP's own building regions, 3D boxes): the first version was one
// x/z rectangle around the whole building block (x -540..-495, z 234..322) which also held the lot's front strip and the courtyard, so those outdoor spots
// went white at EV 4 (blinding white in front of the store, ~10 m, and in the open area behind it). t2r-winter-exposure.txt still replaces
// the boxes by one x/z rectangle (any height) for tuning.
static int ImmSplInside(const SplMap *s, const float *R, int use_file,
                          const float *cp)
{
    if (use_file)
        return cp[0] >= R[0] && cp[0] <= R[1] && cp[2] >= R[2]
            && cp[2] <= R[3];
    for (int i = 0; i < s->nbox; i++) {
        const float *b = s->box[i];
        if (cp[0] >= b[0] && cp[0] <= b[1] && cp[1] >= b[2]
            && cp[1] <= b[3] && cp[2] >= b[4] && cp[2] <= b[5])
            return 1;
    }
    return 0;
}

// SP's own exposure, no invented numbers (match SP's inside/outside balance, not only its boundaries). SP never calls force-exposure-value: the
// engine's auto exposure runs with the tonemap values of the render-settings REGIONS the camera is in (tonemap-light-range-min/max, set-point, adaption-time,
// histogram position ...; "partial" regions, each blends in over its own render-settings-blend-time, usually 1 s; higher priority wins per value). sp_lighting.h
// lists those regions of the map (g_spl_reg, from the SP data); here we do what the region manager does, with the same symbols, priorities and blend times:
// every frame override-render-settings(symbol, prio, strength) per region, strength ramping 0..1 while the camera is inside its box. If the engine's own
// region gather already applies them (ungated ones) the values are identical. t2r-no-sp-regions.txt = off. Log "immersion: sp regions ..." (+ a probe of the
// resulting tonemap range at the camera) shows entering / leaving.
// SP-exact lighting. SP's own render-settings regions (exact convex volumes from the level paks, gating cut by the build) are applied NATIVELY by the engine; this
// tick no longer replays them. Every deviation we used to have is now an opt-in that RESTORES the old behaviour (flag file next to the exe, present = old):
//   t2r-spl-replay.txt  ext replay of the tonemap regions in the legacy AABB boxes (only sensible with a legacy build: T2R_SPL_GATES=old T2R_SPL_DROP=old)
//   t2r-spl-hold.txt    "consistent outdoors" hold: replayed regions other than the base look are 0 outdoors, indoor weight from the hold
//   t2r-spl-hyst.txt    1 m exit margin + 0.3 s flip limit on the interior / replay edges
//   t2r-spl-aabb.txt    interior test = the legacy AABB boxes instead of the exact region volumes
//   t2r-spl-windhold.txt  wind-intensity-multiplier / cloth-wind-intensity-multiplier held at the outdoor value on every region symbol
//   t2r-spl-gustcap.txt   blizzard gust capped at 8 (t2r-gust-cap.txt) and its direction pinned
// What stays always: the wind TIME-SCALE hold (joint-wind-speed / flutter-speed constant: shake fix).
static float g_immIndoorW;    // 0..1 camera interior weight of the winter map's interior regions (set by imm_spl_regions_tick; weather.c snow melt)
// camera state of the last imm_spl_regions_tick for weather.c (storm fog seen through doorways): position, map index, raw interior hit, teleport, time
static float g_immCam[3];
static int g_immCamK = -1, g_imm_cam_in, g_imm_cam_jump;
static uint64_t g_immCamT;
static float g_immStorm;       // 0..1 strength of weather.c's random blizzard (set by weather_tick; the door sun blends the door rooms' outdoor values to the storm's)
static int ImmSplHit(const SplMap *sm, const float *cp, int aabb,
                       float m)
{
    if (aabb) {
        for (int q = 0; q < sm->nbox; q++) {
            const float *b = sm->box[q];
            if (cp[0] >= b[0] - m && cp[0] <= b[1] + m && cp[1] >= b[2] - m
                && cp[1] <= b[3] + m && cp[2] >= b[4] - m
                && cp[2] <= b[5] + m)
                return 1;
        }
        return 0;
    }
    for (int q = 0; q < sm->nvol; q++) {
        const SplVol *v = &sm->vol[q];
        int in = 1;
        for (int f = 0; f < v->nface && in; f++)
            if (v->f[f][0] * cp[0] + v->f[f][1] * cp[1] +
                v->f[f][2] * cp[2] > v->f[f][3] + m)
                in = 0;
        if (in)
            return 1;
    }
    return 0;
}

// per-body interior test (mpsnow.c per-body melt): is this world position inside one of the winter map's SP interior volumes (same exact-volume test as the camera's)
static int ImmBodyIndoor(const float *p)
{
    int k = g_immCamK;
    return k >= 0 && k < SPL_N && ImmSplHit(&g_spl[k], p, 0, 0.0f);
}

static void ImmSplRegionsTick(int k, const float *cp)
{
    static float env[SPL_NREG_MAX];
    static uint64_t last_t;
    static int nl, nprobe, last_in = -1;
    static uint64_t a_rep, a_hold, a_hyst, a_aabb;
    static int v_rep, v_hold, v_hyst, v_aabb;
    uint64_t t = GetTickCount64();
    float dt = last_t
        && t - last_t < 500 ? (float) (t - last_t) / 1000.0f : 0.016f;
    int replay = ImmNoflag("t2r-spl-replay.txt", &a_rep, &v_rep), hold =
        ImmNoflag("t2r-spl-hold.txt", &a_hold, &v_hold);
    int hyst = ImmNoflag("t2r-spl-hyst.txt", &a_hyst, &v_hyst), aabb =
        ImmNoflag("t2r-spl-aabb.txt", &a_aabb, &v_aabb);
    // a teleport (respawn, match start, long gap) is not a walk across a region edge: SP puts the player in the region state of the new spot at once
    static float last_cp[3];
    static int have_cp;
    int jump = !last_t || t - last_t >= 500 || !have_cp
        || (cp[0] - last_cp[0]) * (cp[0] - last_cp[0]) + (cp[1] -
                                                          last_cp[1]) *
        (cp[1] - last_cp[1]) + (cp[2] - last_cp[2]) * (cp[2] -
                                                       last_cp[2]) >
        8.0f * 8.0f;
    last_cp[0] = cp[0];
    last_cp[1] = cp[1];
    last_cp[2] = cp[2];
    have_cp = 1;
    last_t = t;
    static uint8_t was_in[SPL_NREG_MAX];
    static uint64_t flip_t[SPL_NREG_MAX];
    int nin = 0, ncall = 0;
    char names[300];
    size_t at = 0;
    names[0] = 0;
    // interior = camera inside one of SP's interior region volumes (main-bld-low-1, side-blds-1, hunting-1): drives the snow-melt weight only, the lighting is the engine's
    static uint8_t ind_was;
    static uint64_t ind_flip;
    int ind = 0;
    if (k >= 0 && k < SPL_N) {
        const SplMap *sm = &g_spl[k];
        int hit = ImmSplHit(sm, cp, aabb, 0.0f);
        if (hyst) {
            if (!hit && ind_was && !jump)
                hit = ImmSplHit(sm, cp, aabb, 1.0f);
            if (jump) {
                ind_was = (uint8_t) hit;
                ind_flip = 0;
            } else if (hit != ind_was) {
                if (t - ind_flip < 300)
                    hit = ind_was;
                else {
                    ind_was = (uint8_t) hit;
                    ind_flip = t;
                }
            }
        }
        ind = hit;
    }
    g_immCam[0] = cp[0];
    g_immCam[1] = cp[1];
    g_immCam[2] = cp[2];
    g_immCamK = k;
    g_imm_cam_in = ind;
    g_imm_cam_jump = jump;
    g_immCamT = t;
    g_immIndoorW += (ind ? dt : -dt) / 1.0f;  // the same 1 s doorway blend as SP's regions (0 outdoors .. 1 inside)
    if (g_immIndoorW < 0.0f)
        g_immIndoorW = 0.0f;
    else if (g_immIndoorW > 1.0f)
        g_immIndoorW = 1.0f;
    if (jump)
        g_immIndoorW = ind ? 1.0f : 0.0f;
    for (int i = 0; replay && i < SPL_NREG && i < SPL_NREG_MAX; i++) {
        const SplReg *r = &g_splReg[i];
        if (r->map != k)
            continue;
        const float *b = r->box;
        int in = cp[0] >= b[0] && cp[0] <= b[1] && cp[1] >= b[2]
            && cp[1] <= b[3] && cp[2] >= b[4] && cp[2] <= b[5];
        if (hyst) {
            if (jump) {
                was_in[i] = in;
                flip_t[i] = 0;
            } else {
                if (!in && was_in[i])
                    in = cp[0] >= b[0] - 1.0f && cp[0] <= b[1] + 1.0f
                        && cp[1] >= b[2] - 1.0f && cp[1] <= b[3] + 1.0f
                        && cp[2] >= b[4] - 1.0f && cp[2] <= b[5] + 1.0f;
                if (in != was_in[i]) {
                    if (t - flip_t[i] < 300)
                        in = was_in[i];
                    else {
                        was_in[i] = in;
                        flip_t[i] = t;
                    }
                }
            }
        }
        if (hold && !ind && !r->always)
            in = 0;
        float bl = in ? r->blend : r->blend_out;
        float step = r->always
            && in ? 1.0f : dt / (bl > 0.05f ? bl : 0.05f);
        if (jump)
            env[i] = in ? 1.0f : 0.0f;
        else
            env[i] += in ? step : -step;
        if (env[i] > 1.0f)
            env[i] = 1.0f;
        if (env[i] < 0.0f)
            env[i] = 0.0f;
        if (in) {
            nin++;
            if (r->prio > 0 && at < sizeof names - 60)
                at +=
                    snprintf(names + at, sizeof names - at, " %s[%d]",
                             r->name, r->prio);
        }
        if (env[i] > 0.01f) {
            ImmRsCall(FNV64(r->sym), r->prio, env[i]);
            ncall++;
        }
    }
    // wind: the region symbols' joint-wind-speed / flutter-speed are TIME SCALES (phase = time x speed): every change of them jumps the wind phase by time x delta = a violent
    // shake while they ramp (SP interior regions ramp speed to 0 and back; the blizzard 3.8 -> 1.8). Held constant (1.8 / 1.0, the storm's final values) on every region symbol of
    // the map and on the blizzard symbol - always on. Optional restores of the old extras: t2r-spl-windhold.txt, t2r-spl-gustcap.txt.
    {
        static uint64_t ww_a, gc_a, gc_t, gf_t;
        static int ww_v, gc_v, wh_n, gc_n;
        static float cap = 8.0f;
        uint64_t fn = FNV64("change-render-setting-value-float"), gnow =
            GetTickCount64();
        int wind = ImmNoflag("t2r-spl-windhold.txt", &ww_a, &ww_v);
        float wv = 1.0f, cv = 1.0f;
        uint32_t wb, cb, sb, fb;
        float Sv = 1.8f, fv = 1.0f;
        if (wind) {
            uint64_t kf = FNV64("fog:distance-start"), fr =
                DcCall(FNV64("get-render-setting-value-float"), 1, &kf);
            uint32_t fu = (uint32_t) fr;
            float fg;
            memcpy(&fg, &fu, 4);
            if (fr != ~0ULL && fg < -1.0f)
                cv = 1.5f;
        }
        memcpy(&wb, &wv, 4);
        memcpy(&cb, &cv, 4);
        memcpy(&sb, &Sv, 4);
        memcpy(&fb, &fv, 4);
        uint64_t kw = FNV64("wind:wind-intensity-multiplier"), kc =
            FNV64("wind:cloth-wind-intensity-multiplier");
        for (int i = 0; i < SPL_NHOLD; i++) {
            if (g_splHold[i].map != k)
                continue;
            uint64_t g = FNV64(g_splHold[i].sym);
            uint64_t a3[3] =
                { g, FNV64("wind:joint-wind-speed"), sb }, a4[3] =
                { g, FNV64("wind:joint-wind-flutter-speed"), fb };
            DcCall(fn, 3, a3);
            DcCall(fn, 3, a4);
            if (wind) {
                uint64_t a1[3] = { g, kw, wb }, a2[3] = { g, kc, cb };
                DcCall(fn, 3, a1);
                DcCall(fn, 3, a2);
            }
            if (wh_n < 3) {
                wh_n++;
                Xlog("immersion: wind time-scale hold %s%s",
                     g_splHold[i].sym, wind ? " (+ intensity hold)" : "");
            }
        }
        if (gnow - gc_t >= 2000) {
            gc_t = gnow;
            uint64_t g = FNV64("*pat-rs-pat-forest-blizzard*");
            if (ImmNoflag("t2r-spl-gustcap.txt", &gc_a, &gc_v)) {
                if (gnow - gf_t >= 10000) {
                    gf_t = gnow;
                    FILE *f = ExeSideOpen("t2r-gust-cap.txt", "r");
                    if (f) {
                        float v;
                        if (fscanf(f, "%f", &v) == 1 && v >= 1.0f
                            && v <= 20.0f)
                            cap = v;
                        fclose(f);
                    }
                }
                float vx = 0.0f, vz = 1.0f;
                uint32_t xb, zb, capb;
                memcpy(&capb, &cap, 4);
                memcpy(&xb, &vx, 4);
                memcpy(&zb, &vz, 4);
                uint64_t a1[3] =
                    { g, FNV64("wind:joint-wind-gust-intensity"), capb },
                    a2[3] =
                    { g, FNV64("wind:joint-wind-direction-x"), xb },
                    a3[3] =
                    { g, FNV64("wind:joint-wind-direction-z"), zb };
                DcCall(fn, 3, a1);
                DcCall(fn, 3, a2);
                DcCall(fn, 3, a3);
                if (gc_n++ < 3)
                    Xlog("immersion: storm gust cap %.1f, direction (0,1) (flag t2r-spl-gustcap.txt)", (double) cap);
            }
            for (int i = 0; i < 2; i++) {
                float c2 = i ? 1.0f : 1.8f;
                uint32_t vb;
                memcpy(&vb, &c2, 4);
                uint64_t a[3] =
                    { g,
            FNV64(i ? "wind:joint-wind-flutter-speed" :
                  "wind:joint-wind-speed"), vb };
                DcCall(fn, 3, a);
            }
        }
    }
    // door sun (a deviation from SP): three openings we made that SP keeps shut - (1) the post office door that was covered by snow, (2) the boarded
    // supermarket -> courtyard door, (3) the supermarket garage / loading door. The rooms that see out through them (door volumes: main-bld-low-1 store, main-bld-4 dock,
    // side-blds-1 post office; the hunting store and every other door stay pure SP) switch sun / fog / cloud shadows camera-globally in SP, so the yard seen through the opening
    // would lose its sun. While the camera is in those volumes (and 2 s after, the engine's 1 s zone blend) the zone symbols carry the OUTDOOR value of those keys
    // (sun-light / fog / cloud shadow = *pat-rs-pat-cloudy* over *pat-rs-pat*), the outdoor shadow cascades (17 / 50 m: roof and walls shadow the room while the sun is on) and
    // tonemap-light-range-min capped at 0.5 (yard not blown out by the interior eye adaptation); elsewhere the symbols hold SP's original values again.
    // Hood / ambient / bounce / flash-light keys are never touched. Flag t2r-spl-door-sun.txt = SP's original everywhere.
    {
        static uint64_t ds_a, ds_t, ds_in_t;
        static int ds_v, ds_log, ds_was, ds_state = -1;
        int off = ImmNoflag("t2r-spl-door-sun.txt", &ds_a, &ds_v), din =
            0;
        if (!off && k >= 0 && k < SPL_N) {
            const SplMap *sm = &g_spl[k];
            for (int q = 0; q < sm->ndvol && !din; q++) {
                const SplVol *v = &sm->dvol[q];
                int in = 1;
                for (int f = 0; f < v->nface && in; f++)
                    if (v->f[f][0] * cp[0] + v->f[f][1] * cp[1] +
                        v->f[f][2] * cp[2] > v->f[f][3])
                        in = 0;
                din = in;
            }
        }
        if (din)
            ds_in_t = t;
        int want = !off && (din || (ds_in_t && t - ds_in_t < 2000));
        // storm: the outdoor value of a key is the storm layers' value at the storm strength (hs entries, sp_lighting.py STORM_LAYERS) - with
        // the clear-weather values the yard / courtyard seen from the door rooms stayed sunlit (sun 2.8 inside, 0 outside) while the storm was on; rewritten every
        // 0.5 s while the camera is in a door room, so the 20 s storm ramp follows
        static float ds_storm = -1.0f;
        float stw =
            g_immStorm < 0.0f ? 0.0f : g_immStorm >
            1.0f ? 1.0f : g_immStorm;
        if (want && fabsf(stw - ds_storm) > 0.02f)
            ds_t = 0;
        if (want != ds_state || (want && t - ds_t >= 500)) {
            ds_storm = stw;
            ds_t = t;
            uint64_t fl = FNV64("change-render-setting-value-float"), ii =
                FNV64("change-render-setting-value-int32");
            for (int i = 0; i < SPL_NDOOR; i++) {
                const SplDoor *d = &g_splDoor[i];
                if (d->map != k)
                    continue;
                float val =
                    want ? (d->hs ? d->v +
                            (d->storm - d->v) * stw : d->v) : d->orig;
                if (d->isint)
                    val =
                        val <
                        0.0f ? (float) (int) (val -
                                              0.5f) : (float) (int) (val +
                                                                     0.5f);
                uint32_t vb;
                memcpy(&vb, &val, 4);
                uint64_t a[3] =
                    { FNV64(d->sym), FNV64(d->key),
            d->
            isint ? (uint64_t) (uint32_t) (int32_t) val : (uint64_t) vb };
                uint64_t r = DcCall(d->isint ? ii : fl, 3, a);
                if (want && din && !ds_was && ds_log < 12)
                    Xlog("immersion: door sun %s %s -> %g (SP %g, clear %g, storm %.2f)%s (%s, rc %llx)", d->sym, d->key, (double) val, (double) d->orig, (double) d->v, (double) stw, d->isint ? " int" : "", d->kind == 1 ? "outdoor cascade" : d->kind == 2 ? "exposure cap" : d->kind == 3 ? "storm value" : "outdoor value", (unsigned long long) r);
            }
            if (want && din && !ds_was)
                ds_log++;
            if (want != ds_state)
                Xlog("immersion: door sun %s (camera %.1f %.1f %.1f)",
                     want ? "ON: outdoor values in the door rooms" :
                     "off: SP values", (double) cp[0], (double) cp[1],
                     (double) cp[2]);
            ds_state = want;
        }
        ds_was = din;
    }
    static int ind_last = -1;
    if (ind != ind_last && nl < 60) {
        nl++;
        ind_last = ind;
        nprobe = 0;
        Xlog("immersion: sp interior %s: camera %.1f %.1f %.1f (%s volumes)%s", ind ? "IN" : "out", (double) cp[0], (double) cp[1], (double) cp[2], aabb ? "legacy AABB" : "exact", names);
    }
    (void) last_in;
    static uint64_t last_pr;
    if (nprobe < 3 && nl && t - last_pr >= 1500) {
        last_pr = t;
        ImmP1Probe(&nprobe);
    }                           // tonemap range / set point at the camera after the engine's own blend
    (void) ncall;
    (void) nin;
}

static void ImmWinterExposureTick(int in_match)
{
    static float R[6] = { 0, 0, 0, 0, 4.0f, 0.0f }, ev = 0.0f;
    static int on, nl, from_file;
    static uint64_t last_rd;
    if (!in_match || g_immRsLevel < 1 || g_immRsLevel > SPL_N) {
        if (on) {
            on = 0;
            ImmExposureRelease();
            if (nl < 8) {
                nl++;
                Xlog("immersion: winter exposure off (level left): forced EV released (auto exposure)");
            }
        }
        return;
    }
    {
        float cp0[3], cd0[3];
        if (CamPosDir(cp0, cd0))
            ImmSplRegionsTick(g_immRsLevel - 1, cp0);
    }
    static uint64_t nf_a;
    static int nf_v;
    if (ImmNoflag("t2r-no-winter-exposure.txt", &nf_a, &nf_v)) {
        if (on) {
            on = 0;
            ImmExposureRelease();
        }
        return;
    }
    uint64_t t = GetTickCount64();
    if (t - last_rd >= 2000) {
        last_rd = t;
        FILE *f = ExeSideOpen("t2r-winter-exposure.txt", "r");
        if (f) {
            float N[6];
            if (fscanf
                (f, "%f %f %f %f %f %f", &N[0], &N[1], &N[2], &N[3], &N[4],
                 &N[5]) == 6) {
                memcpy(R, N, sizeof R);
                from_file = 1;
            }
            fclose(f);
        }
    }
    const SplMap *sp = &g_spl[g_immRsLevel - 1];
    if (!from_file) {
        R[4] = sp->ev_in;
        R[5] = sp->ev_out;
    }
    if (!from_file && !sp->forced) {    // SP's way: no forced EV (spec has no ev_in / ev_out), the regions above drive the engine's auto exposure
        if (on) {
            on = 0;
            ImmExposureRelease();
        }
        return;
    }
    float cp[3], cd[3];
    if (!CamPosDir(cp, cd))
        return;
    int in = ImmSplInside(sp, R, from_file, cp);
    float tgt = in ? R[4] : R[5];
    static int was_in = -1, nsw;
    static uint64_t last_pl;
    if ((in != was_in && nsw < 40) || (t - last_pl >= 3000 && nsw < 6)) {
        nsw++;
        last_pl = t;
        Xlog("immersion: winter exposure: camera %.1f %.1f %.1f %s the store (EV now %.2f -> %.2f)", cp[0], cp[1], cp[2], in ? "IN" : "outside", ev, tgt);
    }
    was_in = in;
    if (!on) {
        on = 1;
        ev = tgt;
        if (nl < 8) {
            nl++;
            Xlog("immersion: winter exposure on: %d interior box(es) of %s%s, EV in %.2f out %.2f", from_file ? 1 : sp->nbox, sp->level, from_file ? " (t2r-winter-exposure.txt rectangle)" : "", R[4], R[5]);
        }
    }
    float step = (R[4] - R[5]) / 48.0f;
    if (step < 0)
        step = -step;
    if (step < 0.01f)
        step = 0.01f;           // ~0.8 s at 60 fps
    ev = ev < tgt ? (ev + step > tgt ? tgt : ev + step) : (ev - step <
                                                           tgt ? tgt : ev -
                                                           step);
    static int released;
    if (!in && ev == tgt) {
        if (!released) {
            released = 1;
            ImmExposureRelease();
        }
        return;
    }                           // out in the lot: the engine's own auto exposure
    released = 0;
    uint64_t a = ImmF2u(ev);
    DcCall(FNV64("force-exposure-value"), 1, &a);
}

// Part I's own per-zone lighting. The p1tracer walk of the Part I Bookstore (outside, ground floor, top floor) showed that Part I's
// renderer at the camera = exactly its hun-const render-settings regions (camera inside the region's convex volumes, highest priority wins per key, region blend
// times): 97 % of every logged float key predicted, the rest are blends / death-camera frames. Those are the 23 regions of our rs level (gating dropped), whose symbols
// the map's t1x module registers. The engine does not apply them natively here (camera probe = defaults), so the ext replays them the way the region manager
// would: every frame override-render-settings(symbol, Part I prio, ramp 0..1 over the region's blend-in / blend-out time) while the camera is in one of its volumes
// (p1_lighting.h, generated from the shipped rs pak). This REPLACES the two camera-global default layers (const base 50 + bookstore base 100, which put the interior's
// tonemap 4 .. 4.85 / set point 1.558 on the street, courtyard, top-floor exit and entrance too). Default ON; t2r-no-p1-regions.txt = the old two global layers.
// The ambient-fill layer is OFF in replay mode: the rs level is reconverted so every Part I symbol carries tonemap-exposure-method = auto
// (p1_rsenv.with_auto_exposure; TLOU2's default 0 = manual EV 0 was the fill's other job) and the build no longer adds the fill symbol (sp_maps_build.fill_on,
// T2R_P1_FILL=1 = old). t2r-p1-fill.txt re-adds the layer (prio 200; needs a T2R_P1_FILL=1 build). A t2r-p1-rs-override.txt still wins over both.
// Test: the replay's override calls are made (rslog) but the live tonemap / sun stay at the engine default (-5 / 5 / 0, dir 0,-0.82,-0.57),
// so neither the native region tree of hun-checkpoint-bookstore-rs nor our overrides take a single t1x symbol. Diagnostic: is the symbol bound in the
// loaded DC data (exe+0x1357750 = the DC symbol lookup skills.c / the arc art params use)? "p1 sym" lines: pointer + first words of the object.
static int ImmDcBound(uint64_t sid)
{                               // the DC symbol is bound in the loaded data (exe+0x1356d60 slot non-NULL and filled)
    typedef uint8_t **(*fn_sym)(uint64_t);
    uint8_t **slot =
        ((fn_sym) ((uint8_t *) GetModuleHandleW(NULL) + 0x1356d60)) (sid);
    return slot && !IsBadReadPtr(slot, 8) && *slot;
}

static void ImmP1SymDiag(void)
{
    static int n;
    static uint64_t last;
    uint64_t t = GetTickCount64();
    if (n >= 4 || (last && t - last < 10000))
        return;
    last = t;
    n++;
    // exe+0x1357750 = { p = exe+0x1356d60(sid); return *p; } - the inner lookup returns NULL for an UNBOUND symbol and the wrapper then
    // reads [0] (night60: crashed A and B on base-2 = our t1x symbols are not bound). Call the inner one and test it.
    typedef uint8_t **(*fn_sym)(uint64_t);
    fn_sym look =
        (fn_sym) ((uint8_t *) GetModuleHandleW(NULL) + 0x1356d60);
    static const struct {
        const char *nm;
        uint64_t sid;
    } S[] = {
        { "*hun-rs-hun-checkpoint-bookstore-base-2*",
         0x7f841655d2b206a8ULL }, { "*t1-rs-hun-const-base*",
                                   0x403a3259d39c40faULL },
        { "*t2r-p1-ambient-fill*", 0 }, { "stock t1x[0]",
                                         0x3616fb1faa20b723ULL },
            { "stock sea[0]", 0x1123f1700c784e2ULL }, { "stock pat[0]",
                                                       0x3771903b13dffcULL
                                                       }
    };
    char out[900];
    size_t at = 0;
    for (size_t i = 0; i < sizeof S / sizeof *S; i++) {
        uint64_t sid = S[i].sid ? S[i].sid : FNV64(S[i].nm);
        uint8_t **slot = look(sid);
        uint8_t *o = slot && !IsBadReadPtr(slot, 8) ? *slot : NULL;
        uint64_t w0 = 0, w1 = 0, w2 = 0;
        if (o && !IsBadReadPtr(o, 24)) {
            memcpy(&w0, o, 8);
            memcpy(&w1, o + 8, 8);
            memcpy(&w2, o + 16, 8);
        }
        at +=
            snprintf(out + at, sizeof out - at, " %s=%s%p[%llx %llx %llx]",
                     S[i].nm, slot ? "" : "UNBOUND ", (void *) o,
                     (unsigned long long) w0, (unsigned long long) w1,
                     (unsigned long long) w2);
        if (at > sizeof out - 140)
            break;
    }
    Xlog("immersion: p1 sym #%d:%s", n, out);
}

// priority added to Part I's own region priorities (0..11) when replayed: TLOU2's override stack is shared with every other caller (SP state scripts use
// 32767, winmm's menu lock 1000); +100 keeps Part I's order and stays under the ambient fill (200). t2r-p1-region-prio.txt "<n>" = another offset (re-read 2 s).
static int g_p1PrioAdd = 100;
// night61: 23 / 23 symbols bound, calls made, live values unchanged. Self-test once per match, 20 s after the Bookstore is up: 4 phases of 2 s,
// each one symbol at priority 32766 (the route t2r-rs-test proved on the Supermarket, night48: APPLIED), read back at the end of the phase:
//   base-2 (Part I, sets min 4 / max 4.85 / set point 1.558 / sun rgbi-i 1.1), *t1-rs-hun-const-base* (min -1 / set point 0.18), stock t1x[0], stock sea[0].
// "p1 selftest" lines: values per phase. Moves for stock but not for ours = our records are not read (format); moves for ours = priority loses below 32766.
static uint64_t g_p1prT0;
static int g_p1prPhase = -1, g_p1pr_done;      // per match (reset by imm_p1_bookstore_tick out of a match)
static void ImmP1Selftest(void)
{
    uint64_t t0 = g_p1prT0;
    int phase = g_p1prPhase, done = g_p1pr_done;
    static const uint64_t SID[4] =
        { 0x7f841655d2b206a8ULL, 0x403a3259d39c40faULL,
0x3616fb1faa20b723ULL, 0x1123f1700c784e2ULL };
    static const char *const NM[4] =
        { "base-2", "t1-const-base", "stock-t1x0", "stock-sea0" };
    if (done)
        return;
    // Flat light, then a white fog burst ~20 s into every Bookstore match: this self-test forced four lighting sets
    // on screen for 2 s each in every match. Dev only now: t2r-p1-selftest.txt next to the exe.
    // The four sets on screen are what brings the Part I sun in (all 1234 logged render values are identical
    // before and after, so it is engine state the full-symbol overrides kick, not a value). So the sequence runs at once when the level is loaded, 150 ms
    // per set, inside the match intro's black hold, every match. t2r-no-p1-prime.txt = off; t2r-p1-selftest.txt = the old on-screen run (20 s in, 2 s each).
    static uint64_t na;
    static int nv;
    if (FeelFlag("t2r-no-p1-prime.txt", &na, &nv, 180))
        return;
    static uint64_t sa;
    static int Sv;
    int slow = FeelFlag("t2r-p1-selftest.txt", &sa, &Sv, 180);
    const uint64_t delay = slow ? 20000 : 0, step = slow ? 2000 : 150;
    uint64_t t = GetTickCount64();
    if (!t0) {
        g_p1prT0 = t;
        Xlog("immersion: p1 prime: %s (delay %llu ms, %llu ms per set)",
             slow ? "on-screen self-test" : "at level load",
             (unsigned long long) delay, (unsigned long long) step);
        return;
    }
    if (t - t0 < delay)
        return;
    // (flat light again, log "base-2" then "stock-t1x0" 2 ms apart): the phase came from the clock, so a level-load hitch over 150 ms
    // skipped t1-const-base. Now the sets run in order: the next one starts only after the current one ran >= step ms AND >= 6 frames.
    static uint64_t ph_t;
    static int ph_n;
    int ph = phase;
    if (phase < 0) {
        ph = 0;
    } else if (t - ph_t >= step && ph_n >= 6)
        ph = phase + 1;
    if (ph > 4)
        ph = 4;
    if (ph != phase) {
        ph_t = t;
        ph_n = 0;
        if (phase >= 0) {
            static const char *const K[] =
                { "tonemap:tonemap-light-range-min",
"tonemap:tonemap-light-range-max", "tonemap:tonemap-set-point",
                "lighting:sun-light:rgbi-i", "lighting:sun-light:dir-x",
                    "lighting:ambient-background-scale",
                    "shadow:cascaded-level-2-distance"
            };
            char out[400];
            size_t at = 0;
            for (size_t i = 0; i < sizeof K / sizeof K[0]; i++) {
                uint64_t one = FNV64(K[i]), r =
                    DcCall(FNV64("get-render-setting-value-float"), 1,
                            &one);
                float f;
                uint32_t l = (uint32_t) r;
                memcpy(&f, &l, 4);
                at +=
                    snprintf(out + at, sizeof out - at, " %s=%g",
                             strrchr(K[i], ':') + 1, (double) f);
            }
            Xlog("immersion: p1 prime %s @32766 (bound %d):%s", NM[phase],
                 ImmDcBound(SID[phase]), out);
        }
        g_p1prPhase = ph;
        if (ph == 4)
            g_p1pr_done = 1;
    }
    if (ph < 4)
        ph_n++;
    if (ph < 4 && ImmDcBound(SID[ph]))
        ImmRsCall(SID[ph], 32766, 1.0f);
}

static void ImmP1RegionsTick(void)
{
    static float env[P1L_NREG];
    static uint64_t sid[P1L_NREG], last_t;
    static uint8_t was[P1L_NREG];
    static float last_cp[3];
    static int have_cp, nl, init;
    static uint8_t bound[P1L_NREG];
    static uint64_t bound_at;
    if (!init) {
        init = 1;
        for (int i = 0; i < P1L_NREG; i++)
            sid[i] = FNV64(g_p1lReg[i].sym);
    }
    if (!bound_at || GetTickCount64() - bound_at >= 3000) {     // re-test every 3 s: only bound symbols are overridden (night60)
        int nb = 0;
        bound_at = GetTickCount64();
        for (int i = 0; i < P1L_NREG; i++) {
            bound[i] = (uint8_t) ImmDcBound(sid[i]);
            nb += bound[i];
        }
        static int lb = -1;
        if (nb != lb && nl < 80) {
            nl++;
            lb = nb;
            Xlog("immersion: p1 regions: %d of %d region symbols bound in the DC data (unbound ones skipped)", nb, P1L_NREG);
        }
    }
    float cp[3], cd[3];
    if (!CamPosDir(cp, cd))
        return;
    uint64_t t = GetTickCount64();
    float dt = last_t
        && t - last_t < 500 ? (float) (t - last_t) / 1000.0f : 0.016f;
    float dx = cp[0] - last_cp[0], dy = cp[1] - last_cp[1], dz =
        cp[2] - last_cp[2];
    int jump = !last_t || t - last_t >= 500 || !have_cp || dx * dx + dy * dy + dz * dz > 8.0f * 8.0f;   // respawn / match start: settle at once (Part I snaps on a load too)
    last_cp[0] = cp[0];
    last_cp[1] = cp[1];
    last_cp[2] = cp[2];
    have_cp = 1;
    last_t = t;
    char names[400];
    size_t at = 0;
    int changed = 0;
    names[0] = 0;
    for (int i = 0; i < P1L_NREG; i++) {
        const P1lReg *r = &g_p1lReg[i];
        int in = r->always;
        for (int q = r->v0; !in && q < r->v0 + r->nv && q < P1L_NVOL; q++) {
            const P1lVol *v = &g_p1lVol[q];
            int ok = 1;
            for (int f = 0; f < (int) (sizeof v->f / sizeof v->f[0]) && ok;
                 f++)
                if (v->f[f][0] * cp[0] + v->f[f][1] * cp[1] +
                    v->f[f][2] * cp[2] > v->f[f][3])
                    ok = 0;
            in = ok;
        }
        float bl = in ? r->blend : r->blend_out;
        if (jump || r->always || bl < 0.02f)
            env[i] = in ? 1.0f : 0.0f;
        else {
            env[i] += (in ? dt : -dt) / bl;
            if (env[i] > 1.0f)
                env[i] = 1.0f;
            else if (env[i] < 0.0f)
                env[i] = 0.0f;
        }
        if (env[i] > 0.001f && bound[i])
            ImmRsCall(sid[i], g_p1PrioAdd + r->prio, env[i]);
        if (in != was[i]) {
            changed = 1;
            was[i] = (uint8_t) in;
        }
        if (in && !r->always && at < sizeof names - 48)
            at +=
                snprintf(names + at, sizeof names - at, " %s[%d]",
                         r->name +
                         (strncmp(r->name, "rr-hun-", 7) ? 0 : 7),
                         r->prio);
    }
    if (changed && nl < 80) {
        nl++;
        Xlog("immersion: p1 regions at camera %.1f %.1f %.1f:%s",
             (double) cp[0], (double) cp[1], (double) cp[2],
             at ? names : " (city base only)");
    }
}

// 2nd-floor barrel "orange ball" (bisected in game, t2r-rs-test.txt on every Bookstore region + *t1-rs-hun-const-base*): fog density 0 removed it,
// then new-volumetrics:fog-run-time-light-contribution 0 alone removed it with the fog kept; the fire light's own volumetric word
// (+0x4c 0 in the light record) did not. So every Bookstore region gets that key at 0 (the stored region
// value, no extra override layer: our region replay carries it), re-applied every 2 s. t2r-p1-fog-rtlight.txt next to the exe = Part I's value (1).
static void ImmP1FogRtlTick(void)
{
    static uint64_t last;
    uint64_t now = GetTickCount64();
    if (now - last < 2000)
        return;
    last = now;
    static uint64_t fa;
    static int fv;
    if (FeelFlag("t2r-p1-fog-rtlight.txt", &fa, &fv, 180))
        return;
    const uint64_t key =
        FNV64("new-volumetrics:fog-run-time-light-contribution"), fn =
        FNV64("change-render-setting-value-float");
    float z = 0.0f;
    uint32_t zb;
    memcpy(&zb, &z, 4);
    int n = 0;
    for (int i = 0; i <= P1L_NREG; i++) {
        const char *sym =
            i < P1L_NREG ? g_p1lReg[i].sym : "*t1-rs-hun-const-base*";
        uint64_t a3[3] = { FNV64(sym), key, zb };
        if (DcCall(fn, 3, a3) != ~0ULL)
            n++;
    }
    static int logged;
    if (logged < 3) {
        logged++;
        uint64_t kk = key, br =
            DcCall(FNV64("get-render-setting-value-float"), 1, &kk);
        uint32_t bu = (uint32_t) br;
        float bf;
        memcpy(&bf, &bu, 4);
        Xlog("immersion: p1 fog: runtime-light contribution 0 in %d / %d Bookstore regions, live value now %g", n, P1L_NREG + 1, (double) bf);
    }
}

static void ImmP1BookstoreTick(int in_match)
{
    static int loaded = -1;
    static unsigned fr;
    static int nl, ncall, nprobe;
    static ImmP1Layer L[8];
    static int nL;
    static uint64_t last_cfg, last_probe;
    static int replay;
    if (!in_match) {
        loaded = -1;
        g_immRsLevel = -1;
        fr = 0;
        nL = 0;
        last_cfg = 0;
        nprobe = 0;
        g_p1prT0 = 0;
        g_p1prPhase = -1;
        g_p1pr_done = 0;
        return;
    }
    if (ImmNoflag("t2r-no-p1-rs-override.txt", &g_p1oA, &g_p1oV))
        return;
    if ((fr++ % 60) == 0) {
        int now = -1;
        for (int k = 0;
             k < (int) (sizeof IMM_RS_LEVELS / sizeof *IMM_RS_LEVELS)
             && now < 0; k++) {
            uint64_t sid = FNV64(IMM_RS_LEVELS[k]), l =
                DcCall(FNV64("is-level-loaded?"), 1, &sid);
            if (l && l != ~0ULL)
                now = k;
        }
        if (now != loaded && nl < 8) {
            nl++;
            Xlog("immersion: rs override level %s (%s)",
                 now >= 0 ? IMM_RS_LEVELS[now] : "none",
                 now >= 0 ? "on" : "off");
        }
        if (now != loaded) {
            nL = 0;
            last_cfg = 0;
        }
        loaded = now;
        g_immRsLevel = now;
    }
    if (loaded < 0)
        return;
    uint64_t tnow = GetTickCount64();
    if (!last_cfg || tnow - last_cfg >= 2000) {
        last_cfg = tnow;
        ImmP1Layer N[8];
        int nN = 0;
        char fn[96];
        snprintf(fn, sizeof fn, "t2r-rs-override-%s.txt",
                 IMM_RS_LEVELS[loaded]);
        FILE *f = fopen(fn, "rb");
        if (!f && loaded == 0)
            f = fopen("t2r-p1-rs-override.txt", "rb");
        if (f) {
            char ln[160];
            while (nN < 8 && fgets(ln, sizeof ln, f)) {
                char s2[96];
                int p2;
                float st2;
                if (ln[0] != '#'
                    && sscanf(ln, "%95s %d %f", s2, &p2, &st2) == 3) {
                    snprintf(N[nN].name, sizeof N[nN].name, "%s", s2);
                    N[nN].sid = FNV64(s2);
                    N[nN].prio = p2;
                    N[nN].str = st2;
                    nN++;
                }
            }
            fclose(f);
        }
        if (!nN && loaded >= 1 && loaded <= SPL_N) {    // the map's own layers from sp_maps.json "sp_lighting".layers
            for (int k = 0; k < g_spl[loaded - 1].nlayer && nN < 8; k++) {
                snprintf(N[nN].name, sizeof N[nN].name, "%s",
                         g_spl[loaded - 1].layer[k].sym);
                N[nN].sid = FNV64(N[nN].name);
                N[nN].prio = g_spl[loaded - 1].layer[k].prio;
                N[nN].str = g_spl[loaded - 1].layer[k].str;
                nN++;
            }
        }
        {
            FILE *pf = fopen("t2r-p1-region-prio.txt", "rb");
            int pv = 100;
            if (pf) {
                if (fscanf(pf, "%d", &pv) != 1)
                    pv = 100;
                fclose(pf);
            }
            if (pv != g_p1PrioAdd) {
                Xlog("immersion: Part I region priority offset %d -> %d (t2r-p1-region-prio.txt)", g_p1PrioAdd, pv);
                g_p1PrioAdd = pv;
            }
        }
        static uint64_t rp_a, nf_a;
        static int rp_v, nf_v;
        replay = 0;
        if (!nN && loaded == 0 && !ImmNoflag("t2r-no-p1-regions.txt", &rp_a, &rp_v)) { // Part I's per-zone regions (imm_p1_regions_tick) + the ambient fill
            replay = 1;
            if (ImmNoflag("t2r-p1-fill.txt", &nf_a, &nf_v)) {  // no fill by default (Part I ambient 1.0); the file re-adds it (build with T2R_P1_FILL=1)
                snprintf(N[0].name, sizeof N[0].name, "%s",
                         IMM_RS_DEF[loaded][2]);
                N[0].prio = 200;
                N[0].str = 1.0f;
                N[0].sid = FNV64(N[0].name);
                nN = 1;
            }
            static int rl;
            if (rl++ < 4)
                Xlog("immersion: Part I Bookstore: Part I region replay on (%d regions)%s", P1L_NREG, nN ? " + ambient fill" : ", no fill");
        }
        if (!nN && !replay && loaded == 0 && IMM_RS_DEF[loaded][0]) {
            snprintf(N[0].name, sizeof N[0].name, "%s",
                     IMM_RS_DEF[loaded][0]);
            N[0].prio = 50;
            N[0].str = 1.0f;
            snprintf(N[1].name, sizeof N[1].name, "%s",
                     IMM_RS_DEF[loaded][1]);
            N[1].prio = 100;
            N[1].str = 1.0f;
            snprintf(N[2].name, sizeof N[2].name, "%s",
                     IMM_RS_DEF[loaded][2]);
            N[2].prio = 200;
            N[2].str = 1.0f;
            N[0].sid = FNV64(N[0].name);
            N[1].sid = FNV64(N[1].name);
            N[2].sid = FNV64(N[2].name);
            nN = 3;
        }
        int same = nN == nL;
        for (int i = 0; same && i < nN; i++)
            same = N[i].sid == L[i].sid && N[i].prio == L[i].prio
                && N[i].str == L[i].str;
        if (!same) {
            memcpy(L, N, sizeof N);
            nL = nN;
            for (int i = 0; i < nL; i++)
                Xlog("immersion: Part I Bookstore override layer %d: %s prio %d strength %.2f", i, L[i].name, L[i].prio, (double) L[i].str);
        }
    }
    uint64_t r[8] = { 0 };
    static uint8_t lb_ok[8];
    static uint64_t lb_at;
    if (!lb_at || tnow - lb_at >= 3000) {
        lb_at = tnow;
        for (int i = 0; i < nL; i++)
            lb_ok[i] = (uint8_t) ImmDcBound(L[i].sid);
    }
    for (int i = 0; i < nL; i++)
        if (lb_ok[i])
            r[i] = ImmRsCall(L[i].sid, L[i].prio, L[i].str);
    if (replay && loaded == 0)
        ImmP1RegionsTick();
    if (loaded == 0) {
        ImmP1SymDiag();
        ImmP1Selftest();
    }
    if (loaded == 0)
        ImmP1FogRtlTick();
    if (ncall < 4) {
        ncall++;
        Xlog("immersion: Part I Bookstore override-render-settings -> %llx %llx %llx %llx (%d layers)", (unsigned long long) r[0], (unsigned long long) r[1], (unsigned long long) r[2], (unsigned long long) r[3], nL);
    }
    if (nprobe < 12 && tnow - last_probe >= 2000) {
        last_probe = tnow;
        ImmP1Probe(&nprobe);
    }
}

// dev (tuning): t2r-dc-dev.txt next to the exe = one native call per line, made EVERY match frame: "<fn-name> [arg ...]".
// Arg: "s:name" = symbol fnv64(name), a number with '.' = float bits, other numbers = int. Re-read every 2 s; the return value is logged on change.
// e.g. "force-exposure-value 2.5" or "change-render-setting-value-float s:*pat-rs-pat* s:tonemap:final-exposure-bias 1.0". Compiled out in release.
#ifndef T2R_RELEASE
#define DCDEV_SEP " \t\r\n"
static void ImmDcDevTick(int in_match)
{
    static struct {
        uint64_t fn;
        int n;
        uint64_t a[8];
        char name[64];
        uint64_t last;
        int logged;
    } C[6];
    static int nC;
    static uint64_t last_rd;
    uint64_t now = GetTickCount64();
    if (now - last_rd >= 2000) {
        last_rd = now;
        nC = 0;
        FILE *f = ExeSideOpen("t2r-dc-dev.txt", "r");
        if (f) {
            char ln[256];
            while (nC < 6 && fgets(ln, sizeof ln, f)) {
                char *tok = strtok(ln, DCDEV_SEP);
                if (!tok || tok[0] == '#')
                    continue;
                snprintf(C[nC].name, sizeof C[nC].name, "%s", tok);
                C[nC].fn = FNV64(tok);
                C[nC].n = 0;
                while (C[nC].n < 8 && (tok = strtok(NULL, DCDEV_SEP))) {
                    uint64_t v;
                    if (!strncmp(tok, "s:", 2))
                        v = FNV64(tok + 2);
                    else if (strchr(tok, '.')) {
                        float fv = (float) atof(tok);
                        uint32_t u;
                        memcpy(&u, &fv, 4);
                        v = u;
                    } else
                        v = (uint64_t) _strtoi64(tok, NULL, 0);
                    C[nC].a[C[nC].n++] = v;
                }
                nC++;
            }
            fclose(f);
        }
    }
    if (!in_match)
        return;
    for (int i = 0; i < nC; i++) {
        uint64_t r = DcCall(C[i].fn, C[i].n, C[i].a);
        if ((!C[i].logged || r != C[i].last) && C[i].logged < 20) {
            uint8_t *rec =
                (uint8_t *) ((fn_lookup) T2R_SYM(kFnNativeLookup)) (C[i].
                                                                    fn);
            uint64_t fp = rec ? *(uint64_t *) (rec + 8) : 0, base =
                (uint64_t) GetModuleHandleA(NULL);
            C[i].logged++;
            C[i].last = r;
            Xlog("dc dev: %s (%d args) -> %llx (native at exe+%llx)",
                 C[i].name, C[i].n, (unsigned long long) r,
                 (unsigned long long) (fp > base ? fp - base : fp));
        }
    }
}
#else
static void ImmDcDevTick(int in_match)
{
    (void) in_match;
}
#endif

// dev (localisation of the leftover dark patches where props were removed): t2r-rs-test.txt next to the exe = render-setting overrides, one per
// line "<render-setting key> <float> [region]" (e.g. "lighting:ssao-intensity-scale 0"); optional line "region <symbol>" = the default region for the lines without
// one (a file without it uses *pat-rs-pat-cloudy*). change-render-setting-value-float only EDITS A ROW THE REGION ALREADY HAS (the engine has no set-render-setting-
// value-float native: not in the exe's name table): *pat-rs-pat-cloudy* defines only sun-light / sun-shadow-cast-distance, *pat-rs-pat* (the level default; measured in
// pat.bin) defines lighting:ambient-shadow-intensity (1.0) and many symbols define ssao-intensity-scale, so a key the base region lacks needs its own region: "lighting:
// ambient-shadow-intensity 0 *pat-rs-pat*". Side effect to expect: that region is re-applied as a whole layer, so its other rows (sun direction -0.611 -0.504 0.610,
// sky rotation -105) override the cloudy ones while the line is in the file. Every used region is re-applied EVERY FRAME with override-render-settings(region, 0, 0, 0,
// prio 32766, 0, 1, 0, 1.0) (sun rgbi-i, sun-shadow-cast-distance, probe-occlusion, ssct APPLIED this way). File re-read every 2 s; each new / changed
// line is logged with the value before, both return codes and a read-back 0.5 s later (APPLIED / NOT applied). Delete the file to stop. Compiled out in release.
#ifndef T2R_RELEASE
#define RST_DEF_REGION "*pat-rs-pat-cloudy*"
static void ImmRsTestTick(int in_match)
{
    typedef struct {
        char key[96], region[96];
        float v, before;
        int fresh;
    } RstEnt;
    static RstEnt T[12];
    static int nT;
    static uint64_t last, rb_at;
    static int rb_pend;
    uint64_t now = GetTickCount64();
    if (!in_match)
        return;
    if (now - last >= 2000) {
        last = now;
        FILE *f = ExeSideOpen("t2r-rs-test.txt", "r");
        if (!f) {
            if (nT)
                Xlog("rs test: file gone, override off");
            nT = 0;
        } else {
            char reg[96] = RST_DEF_REGION, ln[256];
            int n = 0;
            RstEnt N[12];
            memset(N, 0, sizeof N);
            while (n < 12 && fgets(ln, sizeof ln, f)) {
                char *a = strtok(ln, DCDEV_SEP), *b =
                    a ? strtok(NULL, DCDEV_SEP) : NULL;
                if (!a || a[0] == '#' || !b)
                    continue;
                if (!strcmp(a, "region")) {
                    snprintf(reg, sizeof reg, "%s", b);
                    continue;
                }
                char *c = strtok(NULL, DCDEV_SEP);
                snprintf(N[n].key, sizeof N[n].key, "%s", a);
                snprintf(N[n].region, sizeof N[n].region, "%s",
                         c ? c : reg);
                N[n].v = (float) atof(b);
                N[n].fresh = 1;
                for (int i = 0; i < nT; i++)
                    if (!strcmp(T[i].key, a) && T[i].v == N[n].v
                        && !strcmp(T[i].region, N[n].region)) {
                        N[n].fresh = 0;
                        N[n].before = T[i].before;
                    }
                n++;
            }
            fclose(f);
            nT = n;
            memcpy(T, N, sizeof T);
            uint64_t fn = FNV64("change-render-setting-value-float");
            for (int i = 0; i < nT; i++) {
                uint64_t rg = FNV64(T[i].region), kk =
                    FNV64(T[i].key), br =
                    DcCall(FNV64("get-render-setting-value-float"), 1,
                            &kk);
                uint32_t bu = (uint32_t) br, vb;
                float bf;
                memcpy(&bf, &bu, 4);
                memcpy(&vb, &T[i].v, 4);
                if (T[i].fresh)
                    T[i].before = br == ~0ULL ? NAN : bf;
                uint64_t a3[3] = { rg, kk, vb }, r = DcCall(fn, 3, a3);
                if (T[i].fresh) {
                    Xlog("rs test: %s region %s before %g (get -> %llx) set %g (change -> %llx), override layer on", T[i].key, T[i].region, (double) T[i].before, (unsigned long long) br, (double) T[i].v, (unsigned long long) r);
                    rb_at = now + 500;
                    rb_pend = 1;
                }
            }
        }
    }
    if (!nT)
        return;
    for (int i = 0; i < nT; i++) {
        int dup = 0;
        for (int j = 0; j < i; j++)
            if (!strcmp(T[j].region, T[i].region))
                dup = 1;
        if (dup)
            continue;
        uint64_t r = ImmRsCall(FNV64(T[i].region), 32766, 1.0f);      // every frame, like SP's own region calls
        static int n_ov;
        if (n_ov < 6) {
            n_ov++;
            Xlog("rs test: override-render-settings(%s, prio 32766, 1.0) -> %llx", T[i].region, (unsigned long long) r);
        }
    }
    if (rb_pend && now >= rb_at) {
        rb_pend = 0;
        for (int i = 0; i < nT; i++) {
            uint64_t kk = FNV64(T[i].key), br =
                DcCall(FNV64("get-render-setting-value-float"), 1, &kk);
            uint32_t bu = (uint32_t) br;
            float bf;
            memcpy(&bf, &bu, 4);
            Xlog("rs test: %s readback %g (wanted %g, before %g)%s",
                 T[i].key, br == ~0ULL ? (double) NAN : (double) bf,
                 (double) T[i].v, (double) T[i].before, br != ~0ULL
                 && fabsf(bf - T[i].v) <
                 1e-4f ? " APPLIED" : " NOT applied");
        }
    }
}
#else
static void ImmRsTestTick(int in_match)
{
    (void) in_match;
}
#endif

// ---- RSZ: the SP sandbox's lighting / weather zone tracker (tools/sp-sandbox/t2r-sp.c rsz_tick), same keys and line format, so an MP run can be
// diffed against the SP reference at the same camera positions. Every 250 ms in a match: the renderer's live render settings at the camera
// (get-render-setting-value-float); "RSZ" in t2r-ext.log when a value changed, "RSZFULL" every 5 s. Opt-out: t2r-no-rsz.txt.
static const char *const IMM_RSZ_KEYS[] = {
    "tonemap:tonemap-light-range-min", "tonemap:tonemap-light-range-max",
        "tonemap:tonemap-set-point", "tonemap:final-exposure-bias",
        "tonemap:hdr-exposure-bias",
    "tonemap:tonemap-adaption-time", "lighting:sun-light:dir-x",
        "lighting:sun-light:dir-y", "lighting:sun-light:dir-z",
        "lighting:sun-light:rgbi-r",
    "lighting:sun-light:rgbi-g", "lighting:sun-light:rgbi-b",
        "lighting:sun-light:rgbi-i", "sky:intensity", "sky:rotation",
        "sky:gamma", "sky:clouds-coverage",
    "sky:clouds-speed", "sky:sun-altitude", "sky:sun-longitude",
        "sky:sun-disk-intensity", "fog:density", "fog:fog-density",
        "fog:fog-opacity", "fog:color-r",
    "fog:color-g", "fog:color-b", "fog:color-i", "fog:distance-start",
        "fog:distance-end", "fog:height-start", "fog:height-falloff",
        "fog:height-range",
    "fog:floor-fog-density-multiplier", "post:bloom-intensity",
        "post:bloom-threshold", "rain:rain-active", "rain:rain-type",
        "rain:rain-audio-intensity",
    "rain:character-wetness-accumulation",
        "rain:character-wetness-minimum", "rain:rain-direction-x",
        "rain:rain-direction-y", "rain:rain-direction-z",
    "rain:rain-num-quads", "rain:rain-material-id",
        "rain:rain-start-distance", "rain:rain-end-distance",
        "wind:wind-intensity-multiplier", "wind:joint-wind-intensity",
    "wind:joint-wind-gust-intensity", "wind:joint-wind-speed",
        "wind:joint-wind-flutter-speed",
        "wind:joint-wind-flutter-intensity", "wind:joint-wind-turbulence",
        "wind:joint-wind-gust-coverage", "wind:joint-wind-direction-x",
        "wind:joint-wind-direction-z",
        "wind:cloth-wind-intensity-multiplier", "misc:clear-color-r",
    "particles:snow-accumulation-multiplier",
        "particles:snow-decey-multiplier",
        "particles:snow-maximum-coverage",
        "particles:snow-minimum-coverage",
    "particles:snow-melt-time-on-skin",
        "particles:snow-task-begin-coverage"
};

#define IMM_RSZ_N ((int)(sizeof IMM_RSZ_KEYS / sizeof *IMM_RSZ_KEYS))
#include "rslog.c"              // "rslog:" every-logger of render-settings natives + callers (t2r-no-rslog.txt = off)
#include "rszl.c"               // "RSZL": interior-zone lighting keys the RSZ list lacks (hood light, bounce, probe blend ...; t2r-no-rszl.txt = off)
#include "rsdump.c"             // t2r-rsdump.txt: all 1059 float render-settings keys every 500 ms (changes) / 10 s (full) + VRAM (t2r-no-rsdump.txt = off)
#include "p1_fire_flicker.c"    // "p1 fire:": flicker of the Bookstore fire-barrel point lights (p1_fires.py; t2r-no-p1-fire-flicker.txt = off)
static void ImmRszTick(int in_match)
{
    static uint64_t at, full_at, nf_a, sids[IMM_RSZ_N];
    static float prev[IMM_RSZ_N];
    static int have, nf_v;
    if (!in_match) {
        have = 0;
        return;
    }
    uint64_t t = GetTickCount64();
    if (t - at < 250 || ImmNoflag("t2r-no-rsz.txt", &nf_a, &nf_v))
        return;
    at = t;
    float cp[3], cd[3];
    if (!CamPosDir(cp, cd))
        return;
    if (!sids[0])
        for (int i = 0; i < IMM_RSZ_N; i++)
            sids[i] = FNV64(IMM_RSZ_KEYS[i]);
    float v[IMM_RSZ_N];
    for (int i = 0; i < IMM_RSZ_N; i++) {
        uint64_t a = sids[i], r =
            DcCall(FNV64("get-render-setting-value-float"), 1, &a);
        uint32_t u = (uint32_t) r;
        float f;
        memcpy(&f, &u, 4);
        v[i] = r == ~0ULL ? NAN : f;
    }
    g_rslCam[0] = cp[0];
    g_rslCam[1] = cp[1];
    g_rslCam[2] = cp[2];
    char head[96];
    snprintf(head, sizeof head, "cam=%.2f,%.2f,%.2f", (double) cp[0],
             (double) cp[1], (double) cp[2]);
    if (!have || t - full_at >= 5000) {
        full_at = t;
        char line[4000];
        int q = 0;
        for (int i = 0; i < IMM_RSZ_N && q < (int) sizeof line - 80; i++)
            q += snprintf(line + q, sizeof line - q, " %s=%g",
                          IMM_RSZ_KEYS[i], (double) v[i]);
        Xlog("RSZFULL %s%s", head, line);
    }
    if (have) {
        char line[3000];
        int q = 0, nch = 0;
        for (int i = 0; i < IMM_RSZ_N && q < (int) sizeof line - 100; i++) {
            float a = prev[i], b = v[i];
            if (__builtin_isnan(a) && __builtin_isnan(b))
                continue;
            float tol = 0.002f * fabsf(a) + 1e-4f;
            if (!__builtin_isnan(a) && !__builtin_isnan(b)
                && fabsf(a - b) <= tol)
                continue;
            nch++;
            q += snprintf(line + q, sizeof line - q, " %s %g->%g",
                          IMM_RSZ_KEYS[i], (double) a, (double) b);
        }
        if (nch) {
            char src[700];
            RslogRecent(src, sizeof src);
            Xlog("RSZ %s n=%d%s | rslog<=2s:%s", head, nch, line,
                 src[0] ? src : " none");
        }
    }
    memcpy(prev, v, sizeof v);
    have = 1;
}

static void ImmersionTick(void)
{
    ImmEndDuck();
    static uint64_t sprint_since, still_since, next_wound, next_stealth,
        last_poll, throw_seen;
    static float prev_hp = -1.0f;
    static int was_sprint, hp_low;
    uint64_t now = GetTickCount64();
    int in_match = MatchSubstate() == 6;
    int live = in_match && !g_downed && !g_downDying && !g_uc4DyingUntil
        && !(g_execUntil && g_t60 < g_execUntil) && !(g_stun_until
                                                        && g_t60 <
                                                        g_stun_until)
        && !PsyncActive();
    ImmScreenTick(live);
    ImmDcDevTick(in_match);
    ImmRsTestTick(in_match);
    ImmRszTick(in_match);
    ImmRszlTick(in_match);
    ImmRsdumpTick(in_match);
    RslogTick(in_match);
    ImmWinterExposureTick(in_match); // (reads the level the bookstore tick found last frame)
    ImmP1BookstoreTick(in_match);    // every frame: the override-render-settings call is per frame (and its absence = cleared)
    ImmP1FireTick(in_match); // every frame: the fire-barrel light flicker
    int stun_on = in_match && g_stun_until && g_t60 < g_stun_until
        && !g_downed && !g_downDying && !g_uc4DyingUntil
        && !PsyncActive();
    ImmMuffleTick(live, stun_on);
    ImmStunBlurTick(stun_on);
    MhfxTick(live || (in_match && g_mhfxDcUntil && now < g_mhfxDcUntil && !g_downDying), stun_on);        // (a counter's concussion plays through the pair)
    ImmNmTick(live, now);
    ImmSraySendTick(live, now);
    if (!live) {
        sprint_since = still_since = 0;
        was_sprint = 0;
        prev_hp = -1.0f;
        hp_low = 0;
        return;
    }
    if (now - last_poll < 100)
        return;
    last_poll = now;
    uint64_t none = 0;
    // --- sprint: how long it has run, and the stop that follows
    int sprinting = BpOwnSpeed() > OWN_SPRINT_MPS && !g_downed;       // (dc audit: player-sprinting? is not a native; own ground speed stands in)
    if (sprinting) {
        if (!sprint_since)
            sprint_since = now;
        was_sprint = 1;
    } else if (was_sprint) {
        if (sprint_since && now - sprint_since >= 3500 && ImmEnabled(IMM_SPRINT)) {    // SP: she only pants after a real run
            static const char *const E[] =
                { "vox-eff-gen-breath-sprint-recover-1",
"vox-eff-gen-breath-sprint-recover-2", "vox-eff-gen-breath-exhausted-recover" };
            static int flip;
            const char *pair[2] = { E[flip++ & 1], E[2] };
            ImmSay(IMM_SPRINT, pair, 2, 0);
        }
        was_sprint = 0;
        sprint_since = 0;
    }
    // --- health: wounded breathing + heal exhale
    float hp = LocalHp();
    if (hp > 0.0f) {
        float mx = g_wgMax > 10.0f ? g_wgMax : 100.0f;
        if (prev_hp >= 0.0f && hp - prev_hp > mx * 0.25f && ImmEnabled(IMM_HEAL)) {    // a heal lands as one step, not a regen trickle
            static const char *const E[] =
                { "vox-eff-gen-breath-heal-finish-combat",
"vox-eff-gen-breath-heal-finish" };
            ImmSay(IMM_HEAL, E, 2, 1);
        }
        prev_hp = hp;
        if (hp <= mx * 0.30f) {
            if (!hp_low) {
                hp_low = 1;
                next_wound = now + 1500;
            }
        } else if (hp > mx * 0.40f)
            hp_low = 0;
        ImmHeartbeat(hp, mx, hp_low);
        if (hp_low && now >= next_wound && ImmEnabled(IMM_WOUND)) {
            static const char *const E[] =
                { "vox-eff-gen-breath-fight-injured",
"vox-eff-gen-breath-idle-sore" };
            static const char *const F[] =
                { "vox-eff-gen-breath-idle-sore",
"vox-eff-gen-breath-fight-injured" };
            if (ImmSay(IMM_WOUND, sprinting ? E : F, 2, 0))
                next_wound = now + 5500 + (T2rRand() % 3500);
            else
                next_wound = now + 1000;
        }
    }
    // --- crouched and still: the stealth breath
    {
        float lb[16];
        const float *l = Uc4MyLoc(lb);
        static float px, pz;
        static int have;
        int still = 0;
        if (l && __builtin_isfinite(l[0]) && __builtin_isfinite(l[2])) {
            float dx = l[0] - px, dz = l[2] - pz;
            still = have && dx * dx + dz * dz < 0.05f * 0.05f;  // < 0.5 m/s over the 0.1 s poll
            px = l[0];
            pz = l[2];
            have = 1;
        }
        uint64_t c = DcCall(FNV64("player-is-crouched?"), 0, &none), pr =
            DcCall(FNV64("player-is-prone?"), 0, &none);
        int hidden = (c != ~0ULL && (c & 0xff)) || (pr != ~0ULL
                                                    && (pr & 0xff));
        if (still && hidden) {
            if (!still_since) {
                still_since = now;
                next_stealth = now + 2500 + (T2rRand() % 2500);
            }
            if (now >= next_stealth && ImmEnabled(IMM_STEALTH)) {
                static const char *const E[] =
                    { "vox-eff-gen-breath-stealth-mouth-closed",
"vox-eff-gen-breath-stealth-mouth-open" };
                static int flip;
                const char *pair[2] = { E[flip++ & 1], E[flip & 1] };
                ImmSay(IMM_STEALTH, pair, 2, 0);
                next_stealth = now + 7000 + (T2rRand() % 5000);
            }
        } else
            still_since = 0;
    }
    ImmCalls(now);
    // --- our own throw (molotov / bomb / bottle: send_hook type 2 sets g_own_throw_ms)
    if (g_ownThrowMs && g_ownThrowMs != throw_seen) {
        throw_seen = g_ownThrowMs;
        if (now - g_ownThrowMs < 400 && ImmEnabled(IMM_THROW)) {
            static const char *const E[] =
                { "vox-eff-gen-effort-sml", "vox-eff-gen-effort-med" };
            ImmSay(IMM_THROW, E, 2, 1);
        }
    }
}

// ---- Factions-style MATCH HONOURS on the end banner (t2r-no-honours.txt): while the VICTORY / DEFEAT card holds the screen (0.9 s - 4.9 s after
// winmm's end tick, the scoreboard page is hidden then) four lines come in one by one under it: the match's top player per column, from the
// scoreboard's own numbers (SB_STATS: downs + parts per slot, g_sb_exec executions, g_revives): MOST DOWNS / MOST EXECUTIONS / MOST REVIVES /
// MOST PARTS, "<name>  <n>" (a unique leader only, a tie shows nothing; the viewer's own honour is gold). Drawn with the HUD text call the award
// popups use, from the HUD draw hook. Every client books every award (award_hook), so all screens show the same names.
static struct {
    uint64_t end_tick;
    int done;
    int n;
    char who[4][32];
    char name[4][24];
    int val[4];
    int me[4];
} g_hon;
static const char *const HON_TITLE[4] =
    { "MOST DOWNS", "MOST EXECUTIONS", "MOST REVIVES", "MOST PARTS" };
static int ImmRosterName(uint32_t slot, char *out, size_t on)
{
    typedef uint8_t *(*fn_obj)(void);
    // the scoreboard showed PLAYER 0-8: the lobby roster is often emptied by the time the match ends, so every honour read
    // 'PLAYER n'. First the name the kill card / nametags use (the net player record, peer+6), our own from t2r-name, then the roster.
    if (slot == *(uint32_t *) T2R_SYM(kMySlot)) {
        char ln[16], lt[6];
        if (NeLocalLoad(ln, lt) && ln[0]) {
            snprintf(out, on, "%.15s", ln);
            return 1;
        }
    } else if (slot < T2R_MAX_PEERS) {
        typedef uint8_t *(*fn_peer_)(uint32_t);
        uint8_t *p = ((fn_peer_) T2rPeer) (slot);
        if (p && !IsBadReadPtr(p + 6, 0x18) && p[6]
            && strncmp((const char *) (p + 6), "PLAYER ", 7)
            && strncmp((const char *) (p + 6), "Player ", 7)) {
            snprintf(out, on, "%.23s", (const char *) (p + 6));
            return 1;
        }
    }
    uint8_t *o = ((fn_obj) T2R_SYM(kFnNetPlayersCtx)) ();
    if (o && !IsBadReadPtr(o, 0x1d + 8 * 0x32)) {
        int n = o[0x1a] > 8 ? 8 : o[0x1a];
        for (int i = 0; i < n; i++) {
            const uint8_t *e = o + 0x1d + i * 0x32;
            if (e[0] == slot && e[1]) {
                snprintf(out, on, "%.23s", (const char *) (e + 1));
                return 1;
            }
        }
    }
    snprintf(out, on, "PLAYER %u", slot + 1);
    return 0;
}

// Host-authoritative (the honours differed between clients - each one read its own counters; our own awards such as Match Won / Survived / Revive are
// booked only on their owner's screen). The host computes the list and sends it (msg 0xAD: [u8 n][n x {u8 column, u8 slot, i32 value}]); clients show the host's list.
static struct {
    int n;
    uint8_t col[4], slot[4];
    int32_t val[4];
} g_honRx;
static int g_honRxOk;
static void ImmHonFill(int k, int c, uint32_t who, int val, uint32_t me)
{
    g_hon.val[k] = val;
    g_hon.me[k] = who == me;
    ImmRosterName(who, g_hon.name[k], sizeof g_hon.name[k]);
    snprintf(g_hon.who[k], sizeof g_hon.who[k], "%s",
             LocTrDraw(HON_TITLE[c]));
}

static void ImmHonRecv(uint32_t slot, const uint8_t *buf, uint64_t len)
{
    if (!PtHostSlotOk(slot) || len < 1 || buf[0] > 4
        || len < 1 + (uint64_t) buf[0] * 6)
        return;
    g_honRx.n = buf[0];
    for (int k = 0; k < g_honRx.n; k++) {
        g_honRx.col[k] = buf[1 + k * 6] & 3;
        g_honRx.slot[k] = buf[2 + k * 6];
        memcpy(&g_honRx.val[k], buf + 3 + k * 6, 4);
    }
    g_honRxOk = 1;
    Xlog("mvp: received the host's list from slot %u (%d line(s))", slot,
         g_honRx.n);
}

// Every player is the authority for its OWN totals (msg 0xAE [u8 1][i32 downs][i32 parts][u32 executions][u32 revives]): parts paid only to `me` (Match Won, Survived,
// Revive, Supplies, skill bonuses) are booked on the owner's screen alone. Sent every 1.5 s during a match and 4x in the first 0.8 s after the end; every receiver writes the
// owner's values over its local count for that slot, so the scoreboard columns and the honours read the same numbers on every client.
static void ImmOwnSend(void)
{
    uint32_t me = *(uint32_t *) T2R_SYM(kMySlot);
    if (!g_sendTramp || me >= (uint32_t) T2R_ENGINE_PEERS)
        return;
    uint8_t m[17];
    int32_t d = *(int32_t *) (SB_STATS + me * 12), p =
        *(int32_t *) (SB_STATS + me * 12 + 8);
    uint32_t ex = g_sbExec[me * 3], rv = g_revives[me];
    m[0] = 1;
    memcpy(m + 1, &d, 4);
    memcpy(m + 5, &p, 4);
    memcpy(m + 9, &ex, 4);
    memcpy(m + 13, &rv, 4);
    g_sendTramp(0xAE, m, sizeof m, 0xff);
}

static void ImmOwnRecv(uint32_t slot, const uint8_t *buf, uint64_t len)
{
    uint32_t me = *(uint32_t *) T2R_SYM(kMySlot);
    if (len < 17 || buf[0] != 1 || slot >= (uint32_t) T2R_ENGINE_PEERS
        || slot == me || MatchSubstate() != 6)
        return;
    int32_t d, p;
    uint32_t ex, rv;
    memcpy(&d, buf + 1, 4);
    memcpy(&p, buf + 5, 4);
    memcpy(&ex, buf + 9, 4);
    memcpy(&rv, buf + 13, 4);
    *(int32_t *) (SB_STATS + slot * 12) = d;
    *(int32_t *) (SB_STATS + slot * 12 + 8) = p;
    g_sbExec[slot * 3] = ex;
    g_revives[slot] = rv;
}

static void ImmHonoursCompute(void)
{
    typedef uint8_t *(*fn_obj)(void);
    uint32_t me = *(uint32_t *) T2R_SYM(kMySlot);
    int cols[4][T2R_MAX_PEERS];
    int players = 0;
    uint8_t *o = ((fn_obj) T2R_SYM(kFnNetPlayersCtx)) ();
    uint32_t mask = 0;
    if (o && !IsBadReadPtr(o, 0x1d + 8 * 0x32)) {
        int n = o[0x1a] > 8 ? 8 : o[0x1a];
        for (int i = 0; i < n; i++)
            if (o[0x1d + i * 0x32] < T2R_MAX_PEERS)
                mask |= 1u << o[0x1d + i * 0x32];
    }
    for (uint32_t s = 0; s < T2R_MAX_PEERS; s++) {
        int in = s < (uint32_t) T2R_ENGINE_PEERS;
        if (mask & (1u << s))
            players++;
        cols[0][s] = in ? *(int32_t *) (SB_STATS + s * 12) : 0;
        cols[1][s] = (int) g_sbExec[s * 3];
        cols[2][s] = (int) g_revives[s];
        cols[3][s] = in ? *(int32_t *) (SB_STATS + s * 12 + 8) : 0;
    }
    g_hon.n = 0;
    if (*(volatile uint8_t *) T2R_SYM(kRoleByte) == 2) {        // client: the host's list (a late message is applied by imm_honours_draw)
        if (!g_honRxOk)
            return;
        for (int k = 0; k < g_honRx.n; k++) {
            ImmHonFill(k, g_honRx.col[k], g_honRx.slot[k],
                         g_honRx.val[k], me);
            g_hon.n = k + 1;
        }
        Xlog("mvp: client shows the host's list, %d line(s)", g_hon.n);
        for (int k = 0; k < g_hon.n; k++)
            Xlog("mvp: %s - %s %d%s", g_hon.who[k], g_hon.name[k],
                 g_hon.val[k], g_hon.me[k] ? " (us)" : "");
        return;
    }
    if (players < 2)
        return;
    for (int c = 0; c < 4; c++) {
        int best = 0, who = -1, tie = 0;
        for (uint32_t s = 0; s < T2R_MAX_PEERS; s++) {
            if (!(mask & (1u << s)))
                continue;
            if (cols[c][s] > best) {
                best = cols[c][s];
                who = (int) s;
                tie = 0;
            }                   // a tie goes to the lowest slot (same on every client)
        }
        if (who < 0 || tie || best <= 0)
            continue;
        int k = g_hon.n++;
        ImmHonFill(k, c, (uint32_t) who, best, me);
        g_honRx.col[k] = (uint8_t) c;
        g_honRx.slot[k] = (uint8_t) who;
        g_honRx.val[k] = best;
    }
    g_honRx.n = g_hon.n;
    if (g_sendTramp && *(volatile uint8_t *) T2R_SYM(kRoleByte) == 1) {        // host: tell everybody (twice, in case the first is lost)
        uint8_t m[1 + 4 * 6];
        m[0] = (uint8_t) g_hon.n;
        for (int k = 0; k < g_hon.n; k++) {
            m[1 + k * 6] = g_honRx.col[k];
            m[2 + k * 6] = g_honRx.slot[k];
            memcpy(m + 3 + k * 6, &g_honRx.val[k], 4);
        }
        g_sendTramp(0xAD, m, (uint64_t) (1 + g_hon.n * 6), 0xff);
        g_sendTramp(0xAD, m, (uint64_t) (1 + g_hon.n * 6), 0xff);
    }
    Xlog("immersion: match honours: %d line(s)", g_hon.n);
    for (int k = 0; k < g_hon.n; k++)
        Xlog("mvp: %s - %s %d%s", g_hon.who[k], g_hon.name[k],
             g_hon.val[k], g_hon.me[k] ? " (us)" : "");
    for (int k = 0; k < g_hon.n; k++)
        Xlog("immersion: honour %s - %s %d%s", g_hon.who[k], g_hon.name[k],
             g_hon.val[k], g_hon.me[k] ? " (us)" : "");
}

static void ImmTextCenter(float y, float scale, uint32_t col,
                            const char *t)
{
    t = LocTrDraw(t);         // localisation (diacritics folded: the raw text draw's font)
    uint64_t wh = ((fn_measure) T2R_SYM(kFnMeasureText)) (t, scale);
    float w;
    uint32_t lo = (uint32_t) wh;
    memcpy(&w, &lo, 4);
    ((fn_text_va) T2R_SYM(kFnDrawTextVa)) (Fbits(640.0f - w * 0.5f),
                                           Fbits(y), Fbits(scale),
                                           (uint64_t) col, "%s", t);
}

static void ImmHonoursDraw(void)
{
    static int off = -1;
    static uint64_t off_at;
    uint8_t *lc = LobbyObj();
    int ended = lc && !IsBadReadPtr(lc + 0x2ca4, 1) && lc[0x2ca4]
        && MatchSubstate() == 6;
    {
        static uint64_t last;
        uint64_t n0 = GetTickCount64();
        if (MatchSubstate() == 6
            && n0 - last >= (ended
                             && n0 - *(uint64_t *) (lc + 0x2ca8) <
                             800 ? 200u : 1500u)) {
            last = n0;
            ImmOwnSend();
        }
    }
    if (!ended) {
        g_hon.end_tick = 0;
        g_hon.done = 0;
        return;
    }
    if (FeelFlag("t2r-no-honours.txt", &off_at, &off, 180))
        return;
    uint64_t end_tick = *(uint64_t *) (lc + 0x2ca8), now =
        GetTickCount64();
    if (end_tick != g_hon.end_tick) {
        g_hon.end_tick = end_tick;
        g_hon.done = 0;
        g_honRxOk = 0;
        g_honRx.n = 0;
    }
    if (!g_hon.done && now >= end_tick + 600) {
        g_hon.done = 1;
        ImmHonoursCompute();
    }                           // after the owners' end-of-match totals arrived
    else if (!g_hon.n && g_honRxOk
             && *(volatile uint8_t *) T2R_SYM(kRoleByte) == 2)
        ImmHonoursCompute();  // the host's list came after the banner started
    if (!g_hon.n || now < end_tick)
        return;
    uint64_t el = now - end_tick;
    uint64_t to = g_me_hold_on ? (uint64_t) ME_FADE_AT : 2000;
    if (el < 900 || el >= to + 400)
        return;
    float out = el > to ? 1.0f - (float) (el - to) / 400.0f : 1.0f;
    float y = 470.0f;
    ImmTextCenter(y, 0.85f,
                    ((uint32_t) (255 * RampMs(el, 900, 1150) * out) << 24)
                    | 0x00b4b4b4u, "MATCH HONOURS");
    y += 30.0f;
    for (int k = 0; k < g_hon.n; k++, y += 30.0f) {
        int from = 1100 + 350 * k;
        float a = RampMs(el, from, from + 250) * out;
        if (a <= 0.0f)
            continue;
        char t[96];
        snprintf(t, sizeof t, "%s   %s  %d", g_hon.who[k], g_hon.name[k],
                 g_hon.val[k]);
        ImmTextCenter(y, 0.95f,
                        ((uint32_t) (255 * a) << 24) | (g_hon.
                                                        me[k] ? 0x00f0c850u
                                                        : 0x00e6e6e6u), t);
    }
}
