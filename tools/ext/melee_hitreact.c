// melee_hitreact.c - TLOU2's own melee hit reactions for a landed blow (melee must feel impactful: a short hit reaction taken from the game, not a random head snap, with a good blend back into whatever the controller is doing; while the victim's short impact reaction plays the
// ATTACKER cannot swing again). Included after melee_pace.c in t2r-ext.c. Notes: re/melee-hitreact-1026.md. Log prefix "hitreact:". Opt-out: t2r-no-sp-hitreact.txt next to
// the exe (re-read every 3 s): the old behaviour (additive upper-body flinch of feel.c + the command lock of melee_stagger.c) is back.
//
// Before: a blow that hurt a standing player played only the ADDITIVE upper-body gesture of feel.c (gesture-ellie-idle-ub-hr-<zone>-add, no root motion) and melee_stagger.c took
// aim / fire / melee / sprint-jump off for 0.55 s (fists) / 0.7 s (weapons) with the body free to run on - a punch read as nothing but a camera jolt.
// Now (VICTIM's client decides, as for every hit; mhstun_note calls mhr_victim_start): the full-body SP clip ellie-1h-hr-<zone>-<dir/angle>-<variant> (SP "player-standing-hit-reaction"
// move set, anim-melee-player-shared-t2.pak - the very clips SP plays when an NPC hits Ellie, re/sp-sandbox-runs/knife-1020*.log MSTART lines) plays on the victim's own body
// (animate_ on the player layer, streams to everybody like the stun / dodge clips): 0.06 s blend in, cut after 0.30 / 0.38 / 0.45 s (light / medium / heavy blow = the first push of
// the recoil, authored root motion in melee_hitreact_rm.h), blend out 0.18 s (0.12 s with the move stick held) and the engine's locomotion / the player's stick take over - no long
// lock. The clips' authored root motion would carry the body 0.4-0.9 m in that time: it is pinned every frame to a collision-checked path along the clip's own recoil direction
// (the dodge's pin; rays for walls / ground, cap 0.25 / 0.32 / 0.40 m). melee_stagger.c's command lock shrinks to the same window (ONE reaction system); the additive flinch is skipped
// for a melee blow (and for 1.0 s after one: the 2013 grazing protection). The victim tells everybody MSG_HITREACT2 0x9A [u8 flags][u64 clip sid][u16 cut ms]: other clients play the
// same clip at once on his puppet (phase advanced by the one-way latency, like mdg_puppet_dodge), and the ATTACKER's pace lock (melee_pace.c mpace_note_victim) holds the next swing
// start until that cut (provisional lock from the confirmed blow, corrected / released by the message, max 480 ms).
// Pick: direction = where the attacker stands around the victim's facing (front / behind / left / right, dir_around), strength = weapon (fists 0, 1h 1, 2h 2; a hard fist blow +1,
// a hard 1h blow +1), angle / hand from the announced swing (g_mdg.sw) when it is fresh. Never: crouched victims (the additive set stays), a victim mid-swing or dodging, downed / dying /
// execution / paired move / stun / revive / backpack / intro, a clip that is not loaded (get-animation-duration), a blow inside the grazing window. Executions, paired moves, downs unchanged.
#include "melee_hitreact_rm.h"
#define MHR_BLEND_IN 0.06f
#define MHR_BLEND_OUT 0.45f     // (the hit reaction cut to the regular pose instead of blending nicely - was 0.18 / 0.12, cut 300-450 ms)
#define MHR_BLEND_OUT_STICK 0.32f
#define MHR_ATK_MAX_MS 480
#define MHR_GRAZE_MS 1000
#define MSG_HITREACT2 0x9A      // peer msg (ours): [u8 flags: bit0 reaction][u64 clip sid][u16 cut ms] the sender took a melee blow - 0 sid = no reaction (releases the attacker's lock)
static int MhrPunchOld(void);
static uint64_t MhrPunchAt(uint64_t ent, int str, const char *who);
static int MhrFistW(uint64_t w);
static void MduSwingHitCancel(uint32_t slot);        // melee_duel.c: the blow ends our duel swing record too
static void MhrPainSay(uint32_t who, uint64_t ent, int str, int sharp);
static uint8_t MhrPainBits(int str);
static int MhrPainSharp(void);
static const uint32_t MHR_CUT_MS[3] = { 360, 440, 520 };
static const float MHR_CAP_M[3] = { 0.32f, 0.42f, 0.52f };

// SP push-back (hit reactions should feel like SP in animation, sound and fluidity; SP recording
// re/sp-melee-1027): the victim of ONE landed punch is carried back ~0.4-1.0 m (clean single hits: head-front-c 0.88-1.03 m, hip-front-left-from-right 0.69 / 0.74 m,
// chest-front-horizontal 0.43 / 0.50 m, a lone left jab only 0.01-0.08 m; peak 1.9-3.9 m/s) while the reaction lasts ~1.2 s, then walks on. The MP control lock stays
// the short cut above (fairness: MHS_* in melee_stagger.c untouched); the rest of the push is an ADDITIVE shove on top of whatever the stick does, easing out over
// <= 0.7 s after the cut: total target 0.45 / 0.65 / 0.85 m (light / medium / heavy) incl. the pinned part, collision-checked with the dodge's rays (0x1b07). t2r-no-sp-push.txt = off.
static const float MHR_PUSH_M[3] = { 0.45f, 0.65f, 0.85f };

#define MHR_SHOVE_MIN_S 0.25f
#define MHR_SHOVE_MAX_S 0.70f
static struct {
    int on;
    uint64_t t0, lt, jf, job;
    float d, T, v0, u[2], done, y0;
    double t0q, ltq;
} g_msh;                        // t0q / ltq: psync_ms (GetTickCount64 steps 15.6 ms: frames alternated 0 / 2 steps)
static int MhrPushOff(void)
{
    static int off = -1;
    static uint64_t at;
    return FeelFlag("t2r-no-sp-push.txt", &at, &off, 180);
}

// pool: dir 0 front / 1 behind / 2 left / 3 right of the VICTIM (where the attacker stands), str 0 light / 1 medium / 2 heavy, two_h = only for a 2-handed weapon
static const struct {
    const char *n;
    uint8_t dir, str, two_h;
} MHR_POOL[] = {
    { "hip-front-left-from-left-a", 0, 0, 0 },
        { "hip-front-left-from-right-a", 0, 0, 0 },
        { "hip-front-left-from-right-b", 0, 0, 0 },
        { "hip-front-right-from-left-a", 0, 0, 0 },
    { "hip-front-right-from-left-b", 0, 0, 0 },
        { "hip-front-right-from-left-c", 0, 0, 0 },
        { "hip-front-right-from-right-a", 0, 0, 0 },
        { "hip-front-right-from-right-b", 0, 0, 0 },
    { "chest-front-horizontal-a", 0, 1, 0 }, { "chest-front-upward-a", 0,
                                              1, 0 },
        { "rt-ft-chest-front-horizontal-a", 0, 1, 0 },
        { "chest-front-downward-a", 0, 1, 0 }, { "chest-front-downward-b",
                                                0, 1, 0 },
    { "head-front-a", 0, 2, 0 }, { "head-front-b", 0, 2, 0 },
        { "head-front-c", 0, 2, 0 }, { "head-front-d", 0, 2, 0 },
        { "chest-front-downward-c", 0, 2, 0 },
    { "hip-back-left-from-left-a", 1, 0, 0 },
        { "hip-back-left-from-left-b", 1, 0, 0 },
        { "hip-back-left-from-right-a", 1, 0, 0 },
        { "hip-back-left-from-right-b", 1, 0, 0 },
    { "hip-back-left-from-right-c", 1, 0, 0 },
        { "hip-back-left-from-right-d", 1, 0, 0 },
        { "hip-back-right-from-left-a", 1, 0, 0 },
        { "hip-back-right-from-right-a", 1, 0, 0 },
    { "back-left-shoulder-from-left-a", 1, 1, 0 },
        { "back-left-shoulder-from-right-a", 1, 1, 0 },
        { "back-left-shoulder-from-right-b", 1, 1, 0 },
        { "back-left-shoulder-from-right-c", 1, 1, 0 },
    { "back-right-shoulder-from-left-a", 1, 1, 0 },
        { "back-right-shoulder-from-right-a", 1, 1, 0 },
        { "back-right-shoulder-from-right-b", 1, 1, 0 },
        { "chest-back-upward-a", 1, 1, 0 },
    { "back-center-a", 1, 2, 0 }, { "back-center-b", 1, 2, 0 },
        { "back-center-c", 1, 2, 0 }, { "back-center-upward-a", 1, 2, 0 },
    { "head-back-center-a", 1, 2, 0 }, { "head-back-center-b", 1, 2, 0 },
        { "head-back-center-c", 1, 2, 0 },
    { "left-upward-a", 2, 0, 0 }, { "left-upward-b", 2, 0, 0 },
    { "left-horizontal-a", 2, 1, 0 }, { "left-horizontal-b", 2, 1, 0 },
        { "left-horizontal-c", 2, 1, 0 }, { "left-horizontal-d", 2, 1, 0 },
        { "left-downward-a", 2, 1, 0 },
    { "head-left-a", 2, 2, 0 },
    { "right-upward-a", 3, 0, 0 }, { "right-shoulder-from-left-a", 3, 0,
                                    0 }, { "right-shoulder-from-left-b", 3,
                                          0, 0 },
        { "right-shoulder-from-right-a", 3, 0, 0 },
        { "right-shoulder-from-right-b", 3, 0, 0 },
    { "right-horizontal-a", 3, 1, 0 },
    { "head-right-a", 3, 2, 0 }, { "head-right-b", 3, 2, 0 },
};

#define MHR_POOL_N ((int)(sizeof MHR_POOL / sizeof *MHR_POOL))
// the 2-handed hr clips (only the "back-center" ones exist in the data): extra heavy candidates behind a 2-handed blow
static const char *const MHR_2H[] =
    { "ellie-2h-hr-back-center-a", "ellie-2h-hr-back-center-b",
"ellie-2h-hr-back-center-c" };
static const char *const MHR_DIRN[4] =
    { "front", "behind", "left", "right" };
static const char *const MHR_STRN[3] = { "light", "medium", "heavy" };

static struct {
    int on, str, dir, stage, loaded_logged;
    uint64_t sid, t0, cut_ms, arm_f, last_start_ms, start_frame, last_sid,
        rm_logged;
    float from[3], u[2], total, cap, dist_cut, blend_out, push, push_ext,
        ldist, vend;
    uint64_t jw[2], jg, jg2, lt;
    const MhrRm *rm;
    float fwd[2], left[2], net[2];
    int stick_prev;
    struct {
        uint32_t slot;
        uint64_t ms;
        int str;
    } atk;                      // attacker side: our last confirmed blow
    struct {
        uint32_t slot;
        uint64_t ms, w;
        int str;
    } snd;                      // attacker side: our last fist contact, its punch sound waits for the victim's confirmation
    struct {
        uint64_t ent, sid, end;
    } pd[T2R_MAX_PEERS];        // the clip we forced on a puppet (blended out at its cut)
    struct {
        uint64_t due, sid, played, last, punch_due;
        uint32_t cut;
        int str, sharp, pain, punch_str;
    } pr[T2R_MAX_PEERS];        // attacker side: the PREDICTED reaction of our victim (mhr_pred_*)
    float ys[8];
    uint64_t yt[8];
    int yi;                     // our root height, sampled every tick (vertical motion = ladder / mantle / jump / fall)
    double t0q;                 // psync_ms() at t0 (the pin path's clock)
} g_mhr;
// (7 fist blows on a passive bat holder: 'hitreact: SP reaction ... cut after 360 ms' + 'reaction over after 375 ms (impact part over) ...
// clip blended out over 0.45 s' on every blow, knock-back 'capped 0.32 m'; SP re/sp-melee-spec-1110.md 2 / 7: a reaction runs until the next hit or its own end
// 616-1383 ms, control loss 631-1413 ms, knock-back 0.15 / 0.27 / 0.40 / 0.62 m at 100 / 200 / 300 / 500 ms): the reaction has two parts. Up to its EXIT POINT
// (cut_ms, unchanged: 360 / 440 / 520 ms) the controls are locked - mhr_running() is that part only, so the stagger lock, the holster hold and the pin veto last
// exactly as long as before. After it the clip is NOT cut: it plays on from its own pose until the next blow replaces it, our own input (stick / swing / dodge / aim)
// leaves it, or its exit at exit_ms; the body follows the clip's own root path (no cap), limited only by the wall / ground probes. t2r-hr-length-ps24.txt = the old cut.
static struct {
    int full, ctl_back;
    uint64_t exit_ms;
    int turn, turn_f;
    float yaw_t, yaw_c, yaw_0;
} g_mhx;                        // turn (ps29): 1 = our body is turning to face the attacker, 2 = turned (yaw held to the exit point)
static int MhrRunning(void)
{
    return g_mhr.on && !g_mhx.ctl_back;
}

static uint64_t MhrPathMs(void)
{
    return g_mhx.full ? g_mhx.exit_ms : g_mhr.cut_ms;
}

// t2r-ext.c feature_downed (the 2013 fall pick): our momentum right now is a hit reaction's knock-back (the pin / push-back wrote it), not our own running
static int MhrKnockbackRecent(void)
{
    uint64_t n = GetTickCount64();
    return g_mhr.on || g_msh.on || (g_mhr.last_start_ms
                                    && n - g_mhr.last_start_ms <
                                    MHR_GRAZE_MS);
}

static int MhrOff(void)
{
    static int off = -1;
    static uint64_t at;
    return FeelFlag("t2r-no-sp-hitreact.txt", &at, &off, 180);
}

static uint64_t MhrSid(int i)
{
    static uint64_t h[MHR_POOL_N];
    if (!h[0])
        for (int k = 0; k < MHR_POOL_N; k++) {
            char b[96];
            snprintf(b, sizeof b, "ellie-1h-hr-%s", MHR_POOL[k].n);
            h[k] = Fnv64Rt(b);
        }
    return h[i];
}

// a full-body override of the clip on one body (own: phase 0; puppet: phase advanced by the one-way latency)
static uint64_t MhrAnimate(uint64_t ent, uint64_t sid, float phase,
                            float blend)
{
    int n = phase > 0.001f ? 2 : 1;
    uint64_t aa[4] = { FNV64("single-frame"), 16, (uint64_t) n, 1 };
    uint64_t arr = DcCall(FNV64("#%alloc-array"), 4, aa);
    if (!arr || arr == ~0ULL || IsBadReadPtr((void *) arr, 8))
        return ~0ULL;
    uint64_t *tab = *(uint64_t **) arr;
    if (!tab || IsBadReadPtr(tab, 8 * (size_t) n))
        return ~0ULL;
    for (int i = 0; i < n; i++)
        if (!tab[i] || AllocEntryBad((void *) arr, (void *) tab[i], 16))
            return ~0ULL;
    uint8_t *e0 = (uint8_t *) tab[0];
    blend = MelMinBlend(blend);       // no instant clip starts (paired.c MEL_MIN_BLEND)
    *(int32_t *) e0 = 28;
    memcpy(e0 + 8, &blend, 4);
    if (n == 2) {
        uint8_t *e1 = (uint8_t *) tab[1];
        *(int32_t *) e1 = 58;
        memcpy(e1 + 8, &phase, 4);
    }
    uint64_t a[3] = { ent, sid, arr };
    return DcCall(FNV64("animate_"), 3, a);
}

static void MhrStop(uint64_t ent, uint64_t sid, float blend)
{
    uint64_t a[5] = { ent, ANIM_LAYER, 0, sid, 0 };
    memcpy(&a[2], &blend, 4);
    DcCall(FNV64("stop-animating"), 5, a);
}

// ---- the weapon hold is released for the reaction (the bat stays in a two-hand grip through every reaction,
// the arms never open; SP: the hit enemy holds its weapon in ONE hand and both arms fling out - npc-1h-hr-* even for a 2h-blunt holder). Our reaction is a clip on the
// base layer; the player's anim control also runs the layers WeaponPartial / WeaponPartialAlt ('uc4 own: anim layers (64): ... db55d7d8c61fcf45
// fb872a70daf7adfe ...' = sid WeaponPartialAlt / WeaponPartial, tools/sid.py) ABOVE it - the weapon's hold pose (both arms for a two-handed item) stays layered over
// every reaction clip. For the reaction those layers are faded to 1 % with the engine's own layer fade (the fields uc4_gik_restore / the prone ground-IK pin in
// t2r-ext.c write: +0x2c timer, +0x30 length, +0x34 start, +0x48 target; never 0 - a blend of exactly 0 drops the layer's state, see the crash note there) and faded
// back when the reaction is over. The same on a puppet that plays a reaction clip (its own anim control, when it has those layers - the log line lists the
// puppet's layer sids so the next recording names its hold layer if it is another one). NOT proven in game: that WeaponPartial is the layer that carries the
// two-hand grip is read from its name + the layer list, no recording shows it faded yet. t2r-hr-weapon-partial-ps24.txt = the layers are left alone.
#define MHR_WP_FADE_S 0.10f     // [TUNING ps25] fade of the hold layers at the reaction start (the reaction clip's own blend-in is 0.06-0.10 s)
#define MHR_WP_BACK_S 0.25f     // [TUNING ps25] fade back when the reaction is over
static const uint64_t MHR_WP_SIDS[3] =
    { 0xfb872a70daf7adfeULL /* WeaponPartial */ ,
0xdb55d7d8c61fcf45ULL /* WeaponPartialAlt */ , 0 };
// (the attacker's screen: 'hitreact: puppet slot 0 weapon hold for its reaction - 0 WeaponPartial / WeaponPartialAlt layer(s) ... -1.00 / -1.00 ...
// its anim layers: 81b46a69304ee2c9 ... 9da0551daebb873 932f90f311514ea0 27756a2441539c7c ... 76add18493720b5 ...' = hands, facial-base, speech-layer, blink, base,
// idle-randomness, melee-partial, weapon-additive, melee-hands, stairs, weapon-partial, facial-partial, hit-reaction-partial, hit-reaction-partial-override,
// hit-reaction-additive, gesture-1, gesture-2 (tools/sid.py): the puppet is an NPC-class body with the NPC's layer set - no WeaponPartial; its weapon hold sits in
// weapon-partial / melee-partial / melee-hands above the base layer our reaction clip plays on. Those three are faded for the puppet's reaction. Which of them
// carries the two-hand grip is not known - the log line prints each one's weight before the fade (-1 = not a state layer / absent).
static const uint64_t MHR_WP_PUP_SIDS[3] =
    { 0x076add18493720b5ULL /* weapon-partial */ ,
0x09da0551daebb873ULL /* melee-partial */ , 0x27756a2441539c7cULL /* melee-hands */  };
typedef struct {
    uint8_t *ly[3];
    float w[3];
    int on;
    uint64_t until, ent;
    const uint64_t *sids;
    int scan;
} MhrWpT;                     // scan (ps30): 1 = layers by the table walk (any class), 0 = FindLayer (state layers)
static MhrWpT g_mhr_wp, g_mhr_wpp[T2R_MAX_PEERS];
static uint64_t MduVicMelee(int s);   // melee_duel.c: the melee item in a peer's hand, 0 = none
static int MhrWpOld(void)
{
    static int off = -1;
    static uint64_t at;
    return FeelFlag("t2r-hr-weapon-partial-ps24.txt", &at, &off, 180);
}

// ---- (the generic reactions were wrong: hitting a player without a response should
// feel exactly like hitting an NPC in No Return when they do not parry): an ARMED duel victim takes the blow as the synced -hit pair again (melee_duel.c mdu_armed_pairs,
// default on), with the weapon kept in ONE hand through the victim half on every screen. SP (a recorded SP clip, a hammer holder punched
// by Abby; melee-cur.log F=121948: the machete NPC's own move is 'fist-light-attack-1-hit' = it IS the pair's defender; game data re/melee-dc: defender_weapons mask 0x1 =
// any, no weapon / hand flag on the move): the victim half <swing>-hit--npc-f plays on the armed enemy, the weapon stays gripped in the main hand, both arms fling.
// What the layer logs show (tools/sid.py on the tables of 'uc4 own: anim layers' / 'hitreact: puppet slot ... its anim layers', test logs):
//   OUR body:   backpack-bindpose, Face, Hands (a STATE layer, weight 1, one instance: 'psync layers ... own victim' L2 t2 f1.00 n1), breath, braid-collider, hands (simple,
//               empty), facial-base, CharacterSpeech, blink, base, gesture-1 / -2, WeaponPartial (state layer, target 1.00 with a melee item: 'weapon hold released ...
//               before 1.00'), WeaponPartialAlt, FireAnim0, BandagePartial.
//   a PUPPET:   hands (a simple layer that holds a hand-pose partial: 'psync layers ... L1 t1 ... n1 23115de6ef739f1a' = npc-m-nw-mm-aggr-hands-idle--part), facial-base,
//               speech-layer, blink, base, idle-randomness, melee-partial, weapon-additive, melee-hands, stairs, weapon-partial, facial-partial, hit-reaction-partial
//               (-override, -additive), gesture-1 / -2. The upper ones are SIMPLE layers (t1) in that log - the ps29 fade looks them up with FindLayer (state layers only).
// So the fingers have their own layer on both body classes (Hands / hands, + melee-hands on the NPC class) and the ARM hold is a separate one (WeaponPartial /
// weapon-partial). INFERRED from the names + those weights, not seen in a recording: Hands / hands / melee-hands = the grip of the fingers, WeaponPartial / weapon-partial
// (/ melee-partial) = the arms' carry / two-hand hold. For the pair half: the finger layers are never touched (full weight above the half), the arm hold fades to 1 % (both
// arms follow the half: the off hand of a two-handed item lets go, the weapon stays attached to the main hand's joint) and comes back when the half is over.
// The put-away ("the right hand opens flat with the 2x4 lying across the palm, swings low by the hip, then grips it again") fits the engine's begin-holster, not the
// clip: PlayerWeaponState::BeginHolster swaps the weapon overlay to s_weapon-holster = the put-away ARM clip (t2r-ext.c pbegin_hook notes), while the item itself never
// left the hand because the holster anim actions / state requests of a victim half are refused ("stow fix: ... not begun (victim half)") - the arms mimed a put-away around
// an item that stayed. begin-holster was gated for the attacker half, the finisher windup and our generic reaction (exe+2c4c8d on the reaction's first
// frame), never for the victim half. NOT proven for the victim half by a log (nothing logged an unskipped begin-holster): the END line counts the skips, 0 = this guess
// was wrong and the open palm is the clip's own hand pose.
// One switch file per sub-change (re-read every 2 s):
//   t2r-duel-armed-plain-ps29.txt   the armed victim takes the blow PLAIN again (generic reaction, ps29)
//   t2r-ps30-vic-pbegin-old.txt     begin-holster passes in the victim half (the old put-away overlay)
//   t2r-ps30-own-arms-old.txt       our WeaponPartial / WeaponPartialAlt stay at full weight over the half
//   t2r-ps30-pup-arms-old.txt       a puppet's weapon-partial / melee-partial stay at full weight over the half
//   t2r-ps30-pup-scan-old.txt       the puppet's layers are looked up with FindLayer (state layers only, the ps29 way) instead of the layer table
//   t2r-ps30-hr-hands-fade.txt      the generic reaction on a puppet fades melee-hands too (ps29)
// Log: "ps30 armed pair: ..." (START with the layers before, the layers 8 frames in, END with the item's state).
#define P30_MAX_MS 4000         // a duel -hit pair hands off within 3.0 s (mdu_victim_begin refuses longer ones): the arm release never outlives this
#define P30_GRACE_F 3           // [TUNING ps30] frames without the pair before the arm hold fades back (the next blow's pair starts on the abort's frame)
static const uint64_t MHR_WP_PUP_ARM_SIDS[3] =
    { 0x076add18493720b5ULL /* weapon-partial */ ,
0x09da0551daebb873ULL /* melee-partial */ , 0 };
static struct {
    int on, own, slot, n_wp, pb_skips, snap, dead;
    uint32_t uid;
    uint64_t ent, held, t0, f0;
    MhrWpT wp;
} g_p30;
static int P30File(const char *f, int *v, uint64_t *at)
{
    uint64_t n = GetTickCount64();
    if (*v < 0 || n - *at > 2000) {
        *at = n;
        int o = GetFileAttributesA(f) != INVALID_FILE_ATTRIBUTES;
        if (o != *v)
            Xlog("ps30 armed pair: switch file %s %s", f,
                 o ? "PRESENT" : "absent");
        *v = o;
    }
    return *v;
}

static int P30PbeginOld(void)
{
    static int v = -1;
    static uint64_t at;
    return P30File("t2r-ps30-vic-pbegin-old.txt", &v, &at);
}

static int P30OwnArmsOld(void)
{
    static int v = -1;
    static uint64_t at;
    return P30File("t2r-ps30-own-arms-old.txt", &v, &at);
}

static int P30PupArmsOld(void)
{
    static int v = -1;
    static uint64_t at;
    return P30File("t2r-ps30-pup-arms-old.txt", &v, &at);
}

static int P30PupScanOld(void)
{
    static int v = -1;
    static uint64_t at;
    return P30File("t2r-ps30-pup-scan-old.txt", &v, &at);
}

static int P30HrHandsFade(void)
{
    static int v = -1;
    static uint64_t at;
    return P30File("t2r-ps30-hr-hands-fade.txt", &v, &at);
}

static uint8_t *MhrOwnObj(void)
{
    typedef uint8_t *(*mhr_fn_me)(void);
    return ((mhr_fn_me) T2R_SYM(kFnLocalPlayer)) ();
}

static uint8_t *MhrWpFind(uint8_t *obj, uint64_t sid)
{
    typedef uint8_t *(*mhr_fn_fl)(uint8_t * anim, uint64_t sid);
    static const uint8_t fl_pro[8] =
        { 0x48, 0x85, 0xd2, 0x74, 0x4a, 0x44, 0x8b, 0x49 };
    uint8_t *exe = (uint8_t *) GetModuleHandleW(NULL);
    if (!obj || !exe || IsBadReadPtr(obj + 0x1b8, 8)
        || memcmp(exe + 0x14447d0, fl_pro, 8))
        return NULL;
    uint8_t *anim = *(uint8_t **) (obj + 0x1b8);
    if (!anim || IsBadReadPtr(anim + 0x20, 0x10))
        return NULL;
    uint8_t *ly = ((mhr_fn_fl) (exe + 0x14447d0)) (anim, sid);  // FindLayer: state layers only ([obj+8] == 2), the class whose fade fields are known
    return ly && !IsBadWritePtr(ly, 0x50) ? ly : NULL;
}

// ps30: a layer of ANY class by its name, from the anim control's layer table (paired.c vdf_layers' walk: count +0x20, 0x20-byte entries at [+0x28]: +0 name, +8 layer,
// +0x14 in use). The fade fields (+0x2c / +0x30 / +0x34 / +0x44 / +0x48) sit in the layer base: the 'psync layers' lines read them on simple layers (t1) with sane
// values (a puppet's hands: w-0.00/0.10/0.00 f1.00 r1.00 = faded in over 0.10 s). Only simple (1) and state (2) layers are returned. NOT proven in game: that a
// direct fade write behaves on a simple layer as it does on a state layer (t2r-ps30-pup-scan-old.txt = FindLayer only).
static uint8_t *MhrWpScan(uint8_t *obj, uint64_t sid)
{
    if (!obj || IsBadReadPtr(obj + 0x1b8, 8))
        return NULL;
    uint8_t *anim = *(uint8_t **) (obj + 0x1b8);
    if (!anim || IsBadReadPtr(anim + 0x20, 0x10))
        return NULL;
    uint32_t cnt = *(uint32_t *) (anim + 0x20);
    uint8_t *t = *(uint8_t **) (anim + 0x28);
    if (!cnt || cnt >= 256 || !t || IsBadReadPtr(t, (size_t) cnt * 0x20))
        return NULL;
    for (uint32_t i = 0; i < cnt; i++) {
        uint8_t *e = t + (size_t) i * 0x20;
        if (!e[0x14] || *(uint64_t *) e != sid)
            continue;
        uint8_t *ly = *(uint8_t **) (e + 8);
        if (!ly || IsBadWritePtr(ly, 0x50))
            return NULL;
        int ty = *(int32_t *) (ly + 8);
        return ty == 1 || ty == 2 ? ly : NULL;
    }
    return NULL;
}

static uint8_t *MhrWpGet(const MhrWpT *p, uint8_t *obj, uint64_t sid)
{
    return p->scan ? MhrWpScan(obj, sid) : MhrWpFind(obj, sid);
}

static void MhrWpFade(uint8_t *ly, float to, float secs)
{
    *(float *) (ly + 0x34) = *(float *) (ly + 0x44);
    *(float *) (ly + 0x30) = secs;
    *(float *) (ly + 0x2c) = secs;
    *(float *) (ly + 0x48) = to;
}

static int MhrWpBegin(MhrWpT *p, uint8_t *obj, float *was)
{
    int n = 0;
    for (int i = 0; i < 3; i++) {
        uint8_t *ly = p->sids[i] ? MhrWpGet(p, obj, p->sids[i]) : NULL;
        p->ly[i] = NULL;
        was[i] = -1.0f;
        if (!ly || ly[0x3d])
            continue;
        float tgt = *(float *) (ly + 0x48), cur = *(float *) (ly + 0x44);
        if (!__builtin_isfinite(tgt) || !__builtin_isfinite(cur))
            continue;
        was[i] = tgt;
        if (!(tgt > 0.02f))
            continue;
        p->ly[i] = ly;
        p->w[i] = tgt > 1.0f ? 1.0f : tgt;
        MhrWpFade(ly, 0.01f, MHR_WP_FADE_S);
        n++;
    }
    p->on = n > 0;
    return n;
}

static void MhrWpHold(MhrWpT *p, uint8_t *obj)
{                               // every frame of the reaction: the engine raised the layer again (a weapon state change) -> faded again; a new layer object is left alone
    if (!p->on)
        return;
    for (int i = 0; i < 3; i++) {
        if (!p->ly[i])
            continue;
        uint8_t *ly = MhrWpGet(p, obj, p->sids[i]);
        if (ly != p->ly[i] || ly[0x3d]) {
            p->ly[i] = NULL;
            continue;
        }
        if (*(float *) (ly + 0x48) > 0.02f)
            MhrWpFade(ly, 0.01f, MHR_WP_FADE_S);
    }
}

static int MhrWpEnd(MhrWpT *p, uint8_t *obj)
{
    int n = 0;
    if (!p->on)
        return 0;
    for (int i = 0; i < 3; i++) {
        uint8_t *ly = p->ly[i] ? MhrWpGet(p, obj, p->sids[i]) : NULL;
        if (ly && ly == p->ly[i] && !ly[0x3d]
            && *(float *) (ly + 0x48) < 0.02f) {
            MhrWpFade(ly, p->w[i], MHR_WP_BACK_S);
            n++;
        }                       // (only the same, still live layer that still sits at our 1 %)
        p->ly[i] = NULL;
    }
    p->on = 0;
    return n;
}

static void MhrWpOwnBegin(void)
{
    if (g_mhr_wp.on || MhrWpOld() || (g_p30.on && g_p30.own))
        return;                 // (the next blow's reaction: still released; ps30: an armed pair half on us holds the release - one writer)
    uint64_t cw = CurrentWeapon();
    if (!cw || cw == ~0ULL
        || !(MeleeClassOf(cw, NULL) >= 0
             || MeleeClassOf(BaseWeapon(cw), NULL) >= 0))
        return;                 // a melee item in the hand only (the evidence: a baseball bat)
    float was[3];
    g_mhr_wp.sids = MHR_WP_SIDS;
    int n = MhrWpBegin(&g_mhr_wp, MhrOwnObj(), was);
    static int nl;
    if (LOGCAP(nl, 60))
        Xlog("hitreact: weapon hold released for the reaction - %d of our WeaponPartial / WeaponPartialAlt layers fade to 1%% over %.2f s (target weights before %.2f / %.2f, -1 = no such layer; held %llx): the reaction clip's own arms show", n, (double) MHR_WP_FADE_S, (double) was[0], (double) was[1], (unsigned long long) cw);
}

static void MhrWpOwnEnd(const char *why)
{
    if (!g_mhr_wp.on)
        return;
    int n = MhrWpEnd(&g_mhr_wp, MhrOwnObj());
    static int nl;
    if (LOGCAP(nl, 60))
        Xlog("hitreact: weapon hold back (%s) - %d layer(s) fade in over %.2f s", why, n, (double) MHR_WP_BACK_S);
}

static void MhrWpPupBegin(uint32_t slot, uint64_t ent, uint64_t keep_ms)
{
    if (slot >= T2R_MAX_PEERS || !ent || MhrWpOld()
        || !MduVicMelee((int) slot))
        return;
    if (g_p30.on && !g_p30.own && g_p30.slot == (int) slot
        && g_p30.ent == ent)
        return;                 // ps30: an armed pair half on this puppet holds the release - one writer
    MhrWpT *p = &g_mhr_wpp[slot];
    uint64_t now = GetTickCount64();
    if (p->on && p->ent == ent) {
        p->until = now + keep_ms;
        return;
    }                           // the next blow's reaction: still released
    uint8_t *obj = ObProc(ent);
    // SP's hit enemy keeps its weapon GRIPPED through a generic reaction too (npc-1h-hr-* on a 2h-blunt holder: one hand on the weapon, arms flung) - the
    // finger layer melee-hands is not faded any more, only the arm hold weapon-partial / melee-partial (inferred from the layer names). t2r-ps30-hr-hands-fade.txt = ps29.
    float was[3];
    p->sids = P30HrHandsFade()? MHR_WP_PUP_SIDS : MHR_WP_PUP_ARM_SIDS;
    int n = MhrWpBegin(p, obj, was);
    p->ent = ent;
    p->until = now + keep_ms;
    static int nl;
    if (LOGCAP(nl, 40)) {
        char b[700];
        int o = 0;
        b[0] = 0;
        uint8_t *anim = obj
            && !IsBadReadPtr(obj + 0x1b8,
                             8) ? *(uint8_t **) (obj + 0x1b8) : NULL;
        if (anim && !IsBadReadPtr(anim + 0x20, 0x10)) {
            uint32_t cnt = *(uint32_t *) (anim + 0x20);
            uint8_t *t = *(uint8_t **) (anim + 0x28);
            if (cnt && cnt < 256 && t
                && !IsBadReadPtr(t, (size_t) cnt * 0x20))
                for (uint32_t i = 0; i < cnt && o < 640; i++) {
                    uint8_t *e = t + (size_t) i * 0x20;
                    if (e[0x14])
                        o += snprintf(b + o, SNLEFT(b, o), " %llx",
                                      (unsigned long long) *(uint64_t *)
                                      e);
                }
        }
        Xlog("hitreact: puppet slot %u weapon hold for its reaction (ps29) - %d of its weapon-partial / melee-partial / melee-hands layers fade to 1%% (target weights before %.2f / %.2f / %.2f, -1 = no such state layer on the puppet / ps30: melee-hands not looked up); its anim layers:%s", slot, n, (double) was[0], (double) was[1], (double) was[2], o ? b : " (table unreadable)");
    }
}

static void MhrWpPupTick(uint64_t now)
{
    for (uint32_t k = 0; k < T2R_MAX_PEERS; k++) {
        MhrWpT *p = &g_mhr_wpp[k];
        if (!p->on)
            continue;
        uint8_t *obj = PuppetHandle(k) == p->ent ? ObProc(p->ent) : NULL;
        if (!obj) {
            p->on = 0;
            p->ly[0] = p->ly[1] = p->ly[2] = NULL;
            continue;
        }                       // (the puppet is gone: a new one has its own layers)
        if (now < p->until)
            MhrWpHold(p, obj);
        else
            MhrWpEnd(p, obj);
    }
}

// ---- ps30: the armed victim's pair half (the notes sit above g_p30). One state for the pair that runs here (g_ps): the victim is our own body or a puppet.
static int P30HitPair(uint64_t move)
{
    int p = CdPairOfMove(move);
    return p >= 0 && CD_PAIRS[p].kind == 7
        && !strncmp(CD_PAIRS[p].name, "md-", 3);
}                               // the duel's -hit rows only (not pins / parries / finishers)

static int P30MeleeId(uint64_t w)
{
    return w && w != ~0ULL && (MeleeClassOf(w, NULL) >= 0
                               || MeleeClassOf(BaseWeapon(w),
                                                 NULL) >= 0);
}

static const char *P30Lname(uint64_t s)
{
    static const struct {
        uint64_t sid;
        const char *n;
    } T[] = {
        { 0x417451cfa2643529ULL, "Hands" }, { 0x81b46a69304ee2c9ULL,
                                             "hands" },
            { 0x9a7ce19baa54c278ULL, "base" }, { 0xf9e528522ec06a5eULL,
                                                "gesture-1" },
            { 0xf9e527522ec068abULL, "gesture-2" },
        { 0xfb872a70daf7adfeULL, "WeaponPartial" },
            { 0xdb55d7d8c61fcf45ULL, "WeaponPartialAlt" },
            { 0x60471316fd45629cULL, "FireAnim0" },
            { 0x0dad5ec8f74c3606ULL, "BandagePartial" },
        { 0xdb7f04d3dcbcc1acULL, "character-prone-ground-ik" },
            { 0x4b7953355b75313aULL, "idle-randomness" },
            { 0x09da0551daebb873ULL, "melee-partial" },
            { 0x932f90f311514ea0ULL, "weapon-additive" },
        { 0x27756a2441539c7cULL, "melee-hands" }, { 0xe176e46d7db9034fULL,
                                                   "stairs" },
            { 0x076add18493720b5ULL, "weapon-partial" },
            { 0x764e07dbc79175baULL, "hit-reaction-partial" },
        { 0x26aa6d884613382dULL, "hit-reaction-partial-override" },
            { 0x953b9b5b3bf4ed65ULL, "hit-reaction-additive" },
    };
    for (size_t i = 0; i < sizeof T / sizeof *T; i++)
        if (T[i].sid == s)
            return T[i].n;
    return NULL;
}

static int P30Lskip(uint64_t s)
{                               // face / speech / breath / hair / backpack layers: nothing to do with the hands, left out of the proof lines
    static const uint64_t K[] =
        { 0x19b15a8518073d5cULL /* Face */ ,
0xafc72554d0f6393dULL /* facial-base */ , 0x9c9635fed3081ef6ULL /* CharacterSpeech */ ,
0x8f18a8e1bc204ce3ULL /* speech-layer */ ,
        0x061ca0ab95393e89ULL /* blink */ ,
            0xc88dc7d7a4aefc87ULL /* breath */ ,
            0xc3d1f8b078d74080ULL /* braid-collider */ ,
            0x643bfefaa09ea2e0ULL /* backpack-bindpose */ ,
        0x49c0b8c1e4935641ULL /* facial-partial */
    };
    for (size_t i = 0; i < sizeof K / sizeof *K; i++)
        if (K[i] == s)
            return 1;
    return 0;
}

// proof line(s): every used layer of the body - name, class (1 simple, 2 state), weight now > its fade target, instance count, and the clip (simple) / current state (state layer)
static void P30Layers(const char *when, uint8_t *obj)
{
    static int nl;
    if (!LOGCAP(nl, 150))
        return;
    const char *who = g_p30.own ? "our body" : "the puppet";
    uint8_t *anim = obj
        && !IsBadReadPtr(obj + 0x1b8,
                         8) ? *(uint8_t **) (obj + 0x1b8) : NULL;
    uint32_t cnt = 0;
    uint8_t *t = NULL;
    if (anim && !IsBadReadPtr(anim + 0x20, 0x10)) {
        cnt = *(uint32_t *) (anim + 0x20);
        t = *(uint8_t **) (anim + 0x28);
    }
    if (!cnt || cnt >= 256 || !t || IsBadReadPtr(t, (size_t) cnt * 0x20)) {
        Xlog("ps30 armed pair: u%08x layers of %s %s: table unreadable",
             (unsigned) g_p30.uid, who, when);
        return;
    }
    uint8_t *exe = (uint8_t *) GetModuleHandleW(NULL);
    char b[900];
    int o = 0;
    b[0] = 0;
    for (uint32_t i = 0; i < cnt; i++) {
        uint8_t *e = t + (size_t) i * 0x20;
        uint64_t nm = *(uint64_t *) e;
        if (!e[0x14] || P30Lskip(nm))
            continue;
        uint8_t *ly = *(uint8_t **) (e + 8);
        if (!ly || IsBadReadPtr(ly, 0x98))
            continue;
        int ty = *(int32_t *) (ly + 8), n = -1;
        uint64_t what = 0;
        if (ty == 1) {
            n = *(int32_t *) (ly + 0x94);
            uint8_t *in = n ? *(uint8_t **) (ly + 0x58) : NULL;
            if (in && !IsBadReadPtr(in, 0x80))
                what = *(uint64_t *) (in + 0x78);
        } else if (ty == 2 && !IsBadReadPtr(ly, 0x648)) {
            n = *(int32_t *) (ly + 0x640);
            if (n > 0) {
                typedef uint64_t *(*p30_fn_st)(void *, uint64_t *);
                ((p30_fn_st) (exe + 0x16017d0)) (ly, &what);
            }
        }
        const char *ln = P30Lname(nm);
        char hx[20];
        if (!ln) {
            snprintf(hx, sizeof hx, "%llx", (unsigned long long) nm);
            ln = hx;
        }
        o += snprintf(b + o, SNLEFT(b, o), " | %s t%d %.2f>%.2f n%d", ln,
                      ty, (double) *(float *) (ly + 0x44),
                      (double) *(float *) (ly + 0x48), n);
        if (what)
            o += snprintf(b + o, SNLEFT(b, o), " %llx",
                          (unsigned long long) what);
        if (o > 760) {
            Xlog("ps30 armed pair: u%08x layers of %s %s (name class weight>target instances [clip / state]):%s", (unsigned) g_p30.uid, who, when, b);
            o = 0;
            b[0] = 0;
        }
    }
    if (o)
        Xlog("ps30 armed pair: u%08x layers of %s %s (name class weight>target instances [clip / state]):%s", (unsigned) g_p30.uid, who, when, b);
}

// the item in the hand of the pair's victim body: which attach point carries it, and its draw-control visibility byte (0 = hidden)
static void P30Item(char *out, size_t n)
{
    typedef uint64_t *(*p30_fn_ap)(void *, void *);
    uint8_t *exe = (uint8_t *) GetModuleHandleW(NULL), *w = NULL;
    if (g_p30.own) {
        uint8_t *me = BpMe();
        w = me ? PlayerWeapon(me) : NULL;
    } else {
        uint8_t *pn = PstNpc(g_p30.ent);
        w = pn ? PstCurrent(pn) : NULL;
    }
    if (!w || IsBadReadPtr(w + 0x80, 0x18) || IsBadReadPtr(w + 0x1b0, 8)) {
        snprintf(out, n, "NO item process readable in the hand");
        return;
    }
    uint64_t o8[8] = { 0 };
    uint64_t *r = ((p30_fn_ap) (exe + 0x1cd4870)) (w, o8), ap = r ? *r : 0;
    uint8_t *dc = *(uint8_t **) (w + 0x1b0);
    int vis = dc && !IsBadReadPtr(dc + 0x48, 1) ? dc[0x48] : -1;
    snprintf(out, n,
             "item %llx attached to %s (%llx), visibility byte %02x%s",
             (unsigned long long) PstSidOf(w),
             ap ? BpApName(ap) : "nothing", (unsigned long long) ap,
             (unsigned) (vis & 0xff),
             vis == 0 ? " = HIDDEN" : vis < 0 ? " (unreadable)" : "");
}

static uint8_t *P30Obj(void)
{
    if (g_p30.own)
        return MhrOwnObj();
    return g_p30.slot >= 0 && g_p30.slot < T2R_MAX_PEERS
        && PuppetHandle((uint32_t) g_p30.slot) ==
        g_p30.ent ? ObProc(g_p30.ent) : NULL;
}

static void P30End(const char *why)
{
    if (!g_p30.on)
        return;
    uint8_t *obj = P30Obj();
    int n = obj ? MhrWpEnd(&g_p30.wp, obj) : 0;
    char it[200];
    if (obj) {
        P30Layers("at the END of the half", obj);
        P30Item(it, sizeof it);
    } else
        snprintf(it, sizeof it, "the body is gone");
    uint64_t held =
        g_p30.own ? CurrentWeapon() : MduVicMelee(g_p30.slot);
    static int nl;
    if (LOGCAP(nl, 80))
        Xlog("ps30 armed pair: u%08x END (%s) %d ms after its start on %s (slot %d): arm hold back on %d layer(s) over %.2f s; the hand: %s; held id %llx (%llx at the start)%s; begin-holster skipped %d time(s)%s", (unsigned) g_p30.uid, why, (int) (GetTickCount64() - g_p30.t0), g_p30.own ? "our body" : "the puppet", g_p30.slot, n, (double) MHR_WP_BACK_S, it, (unsigned long long) held, (unsigned long long) g_p30.held, held != g_p30.held ? " - CHANGED during the half" : "", g_p30.pb_skips, !g_p30.own ? " (a puppet: no such call)" : g_p30.pb_skips ? " (the engine asked for the put-away arm overlay in this half and was refused)" : " (the engine never asked for the put-away arm overlay in this half)");
    memset(&g_p30, 0, sizeof g_p30);
}

// paired.c psync_play, right before the victim half's animate_ (the layers still as they were before the half)
static void P30Start(void)
{
    if (g_ps.kind != PS_CTXDOWN || g_ps.nseg > 1 || !g_ps.ent_vic
        || !P30HitPair(g_ps.move))
        return;
    int own = g_ps.ent_vic == SID_PLAYER, slot = g_ps.vic;
    uint64_t held = own ? CurrentWeapon() : MduVicMelee(slot);
    if (!P30MeleeId(held))
        return;
    if (g_p30.on && (g_p30.own != own || g_p30.ent != g_ps.ent_vic))
        P30End("a pair on another body starts");
    int chain = g_p30.on, arms_old =
        own ? P30OwnArmsOld() : P30PupArmsOld();
    g_p30.on = 1;
    g_p30.own = own;
    g_p30.slot = slot;
    g_p30.ent = g_ps.ent_vic;
    g_p30.uid = g_ps.uid;
    g_p30.held = held;
    g_p30.t0 = GetTickCount64();
    g_p30.f0 = g_frame;
    g_p30.snap = 0;
    g_p30.dead = 0;
    uint8_t *obj = P30Obj();
    P30Layers("BEFORE the half", obj);
    float was[3] = { -1.0f, -1.0f, -1.0f };
    if (!chain) {
        memset(&g_p30.wp, 0, sizeof g_p30.wp);
        g_p30.n_wp = 0;
        if (!arms_old && obj) {
            // one writer per layer: a generic reaction's release of the same body is handed back first (its fade-back target is what we then record and release)
            if (own) {
                if (g_mhr_wp.on)
                    MhrWpOwnEnd
                        ("ps30: the pair half takes the arm hold over");
            } else if (slot >= 0 && slot < T2R_MAX_PEERS
                       && g_mhr_wpp[slot].on)
                MhrWpEnd(&g_mhr_wpp[slot], obj);
            g_p30.wp.sids = own ? MHR_WP_SIDS : MHR_WP_PUP_ARM_SIDS;
            g_p30.wp.scan = !own && !P30PupScanOld();
            g_p30.n_wp = MhrWpBegin(&g_p30.wp, obj, was);
        }
    }
    char it[200];
    P30Item(it, sizeof it);
    static int nl;
    if (LOGCAP(nl, 80))
        Xlog("ps30 armed pair: u%08x START %s on %s (slot %d, our role %d) holding %llx - the hand: %s; arm hold %s: %d layer(s) fade to 1%% over %.2f s (targets before %.2f / %.2f / %.2f, -1 = none / not looked up); fingers (%s) left at full weight; %s", (unsigned) g_ps.uid, PsyncName(g_ps.vic_sid), own ? "our body" : "the puppet", slot, g_ps.role, (unsigned long long) held, it, arms_old ? "LEFT ALONE (switch file)" : chain ? "still released from the last blow's pair" : own ? "WeaponPartial / WeaponPartialAlt" : g_p30.wp.scan ? "weapon-partial / melee-partial (layer table)" : "weapon-partial / melee-partial (FindLayer)", g_p30.n_wp, (double) MHR_WP_FADE_S, (double) was[0], (double) was[1], (double) was[2], own ? "Hands / hands" : "hands / melee-hands", !own ? "no hide / hands-emptied for a melee item of a kind 7 pair's victim (cd_pup_hands_empty)" : P30PbeginOld()? "begin-holster PASSES in this half (t2r-ps30-vic-pbegin-old.txt)" : "begin-holster is skipped in this half; the holster anim actions / state requests are refused as before (victim half scope)");
}

static void P30Tick(void)
{                               // mhr_tick, every frame
    if (!g_p30.on)
        return;
    uint8_t *obj = P30Obj();
    if (!obj) {
        P30End("the body is gone");
        return;
    }
    if (GetTickCount64() - g_p30.t0 > P30_MAX_MS) {
        P30End("longer than any -hit half");
        return;
    }
    int live = g_ps.role && g_ps.kind == PS_CTXDOWN && !g_ps.ended
        && g_ps.uid == g_p30.uid && g_ps.ent_vic == g_p30.ent;
    if (!live) {
        if (++g_p30.dead >= P30_GRACE_F)
            P30End("the pair is over");
        return;
    }
    g_p30.dead = 0;
    MhrWpHold(&g_p30.wp, obj);        // (the engine raised the arm hold again mid-half, e.g. a weapon state change: released again; the only writer of these layers while the half runs)
    if (!g_p30.snap && g_frame - g_p30.f0 >= 8) {
        g_p30.snap = 1;
        P30Layers("8 frames into the half", obj);
        char it[200];
        P30Item(it, sizeof it);
        static int nl;
        if (LOGCAP(nl, 80))
            Xlog("ps30 armed pair: u%08x 8 frames in - the hand: %s; fist-pair hide rule for this body %d (0 = the item stays shown, -1 = our own body: no hide rule); begin-holster skipped %d time(s) so far", (unsigned) g_p30.uid, it, g_p30.own ? -1 : CdPupHandsEmpty((uint32_t) g_p30.slot), g_p30.pb_skips);
    }
}

// t2r-ext.c pbegin_hook: 1 = we are the armed victim of a duel -hit pair right now - the engine's begin-holster (the put-away arm overlay) is not started
static int P30VicKeep(void)
{
    if (P30PbeginOld())
        return 0;
    int half = g_ps.role == 2 && g_ps.kind == PS_CTXDOWN && !g_ps.ended
        && g_ps.ent_vic == SID_PLAYER && g_ps.nseg <= 1
        && P30HitPair(g_ps.move);
    if (!half && !(g_p30.on && g_p30.own))
        return 0;
    return P30MeleeId(CurrentWeapon());
}

static void P30PbeginNote(void)
{
    g_p30.pb_skips++;
}

// ---- ps25 (3): the reaction's exit (see g_mhx above). SP: the player's control loss in a hit reaction ran to 1413 ms, an NPC's reaction ran out by itself after
// 616-1383 ms (re/sp-melee-spec-1110.md 2 / 7); the ellie-1h-hr clips are 2.2-3.5 s (their tail = the walk back to idle, SP leaves them with a 0.4-0.6 s blend).
#define MHR_EXIT_MAX_MS 1400    // SP: the longest reaction seen (1383 ms NPC, 1413 ms player control loss)
static int MhrLenOld(void)
{
    static int off = -1;
    static uint64_t at;
    return FeelFlag("t2r-hr-length-ps24.txt", &at, &off, 180);
}

// how long the reaction clip `sid` plays when nothing replaces it (both sides use it: the victim's body, the clip forced on its puppet); cut = its control-return time
static uint64_t MhrExitMsOf(uint64_t sid, uint64_t cut)
{
    if (MhrLenOld())
        return cut;
    float dur = MdgClipLen(sid);
    uint64_t dm = dur > 0.0f ? (uint64_t) (dur * 1000.0f) : 0, ex =
        MHR_EXIT_MAX_MS;
    if (dm < ex + 500)
        ex = dm > cut + 500 ? dm - 500 : cut;   // (a short clip: its blend-out starts 0.5 s before its end)
    return ex > cut ? ex : cut;
}

static void DtlReactOver(uint32_t slot, uint64_t sid);        // melee_duel_tl.c
// ---- the reaction BY THE ATTACKER'S MOVE, from SP. Every npc-standing-hit-reaction MSTART of the SP melee run (melee-prev.log + melee-cur.log,
// 102 reactions on the frame of the player's <move>-hit), per player move: the reactions SP played with their counts. (MP punch 3 played
// ellie-1h-hr-right-horizontal-a - which SP plays for punch 4 (4 of 6); SP's punch 3 is rt-ft-chest-front-horizontal-a / head-front-a (4 + 4 of 10);
// the old pick took the directional pool by where the attacker stood, 'from the right' on all 7 blows.) The pick stays inside the move's set: a blow from the
// front arc takes its front members, from behind its back members (none = the old directional pool), from a side the members named for that side
// (-from-left / -from-right, else the zone name left- / right- / head-left / head-right; none = the front members), weighted by SP's counts, no clip twice in a row.
// A move SP has no sample of keeps the old pick. t2r-hr-pick-ps24.txt = the old pick (the earlier three-move table + wrist + directional pool).
// ---- (the reactions were wrong). What the frames
// show on the bat holder: after each punch it rears up with the bat over its head in both hands, then stumbles bent double with its head near the ground, 0.9-1.3 m
// per blow (A.log 'reaction over after 750 / 719 / 812 / 1188 ms ... moved 1.05 / 0.92 / 1.30 / 1.25 m'); SP's hit enemy stays upright, facing the player, arms
// flung, and is knocked 0.15 / 0.27 / 0.40 / 0.62 m at 100 / 200 / 300 / 500 ms (re/sp-melee-spec-1110.md 2). Causes, each from the code + logs:
// (a) THE CLIP. SP plays npc-1h-hr-<name> (set npc-standing-hit-reaction) on the enemy; we played ellie-1h-hr-<name> - the PLAYER's reaction of the same name
//     (melee_hitreact_rm.h: '60 TLOU2 player melee hit-reaction clips', root travel 1.2-2.2 m; SP spec 7: the player's own reactions travel 0.53-1.77 m), a
//     different, much bigger animation. A table pick now plays the npc-1h-hr clip itself when it is loaded here (NPC clips already play on player bodies: the -hit
//     pairs' --npc-f victim halves); not loaded = the ellie clip as before, and the log says so. The NPC clips have no root table: the knock-back is SP's measured
//     median curve (MHR_RM_SPNPC), straight away from the attacker. t2r-hr-npc-clips-ps28.txt = the ellie clips.
// (b) THE FACING. The victim stood 30-84 deg off the attacker (A.log 'from -30 / -63 / -84 / -42 / -52 deg'); SP's enemy turns to the player in the first frames
//     after the contact (yaw step median 10.3 deg/frame over 6 frames, p90 28.7) and SP's table was measured front-on. Our body now turns to face the attacker,
//     at most MHR_TURN_DEG_F a frame, through the reaction's own placement write (hitreact.pin - no new writer), and the reaction is picked from the move's FRONT
//     set (the facing after the turn); the recoil runs along the line away from the attacker. An attacker behind us (beyond MHR_SP_BACK_DEG) keeps the back set, no
//     turn. The yaw is held to the exit point, then it is the player's again. t2r-hr-turn-ps28.txt = no turn, the pick by the side as in ps25.
// (c) THE ONSET. mhr_animate floors every blend at MEL_MIN_BLEND 0.10 s, so 'blend in 0.06 s' was 0.10 s; SP's hit enemy changes pose on the contact frame. An NPC
//     reaction clip starts over MEL_HIT_BLEND (2 frames), like the -hit halves. t2r-hr-blend-ps28.txt = 0.10 s.
#define MHR_TURN_DEG_F 15.0f    // [TUNING ps29] deg per frame of the victim's turn (SP: median 10.3, p90 28.7 deg/frame over the first 6 frames)
// (a blow from behind over 90 deg = SP back reaction clip, then the victim turns around to face the attacker after it): the
// attacker more than MHR_BACK44_DEG off our facing = the BACK reaction - SP's back set of the attacker's move (mhr_sp_pick at 180 deg: clips named
// back / hip-back / back-*-shoulder / head-back-center, weighted by SP's counts), none in the move's set = the directional pool's 'behind' row (dir 1); the clip plays
// with our facing kept (its own root path carries the body forward, away from the blow; no ps29 turn first). At the control return (the exit point) our body turns to
// face the attacker through the same hitreact.pin write, at most MHR_BACK44_TURN_F deg a frame (no snap); the clip's own exit waits for that turn (inputs still end it).
// melee_duel.c sends such a blow PLAIN (MDUO_PLAIN) instead of the -hit pair that turned the victim 170 deg in 2 frames. t2r-hr-back-ps44-old.txt = old (re-read every 2 s).
#define MHR_BACK44_DEG 90.0f    // over 90 deg from the victim's facing
#define MHR_BACK44_TURN_F 10.3f // SP's median turn of a hit enemy toward the player (re/sp-melee-spec-1110.md: 10.3 deg/frame); it is used AFTER the reaction
static int MhrBack44Off(void)
{
    static int off = -1;
    static uint64_t at;
    uint64_t n = GetTickCount64();
    if (off < 0 || n - at > 2000) {
        at = n;
        off =
            GetFileAttributesA("t2r-hr-back-ps44-old.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    return off;
}

static struct {
    int on;
    uint32_t slot;
    float ang;
    int f;
} g_mhb44;                      // on: the running reaction is a ps44 back reaction from slot (the turn after it is still owed)
static const MhrRm MHR_RM_SPNPC = { 0, 1.15f, 0.45f, 0.57f, 24, { 0 }, { 0, -8, -15, -21, -27, -34, -40, -46, -51, -57, -62, -62, -62, -62, -62, -62, -62, -62, -62, -62, -62, -62, -62, -62 } };       // SP's hit NPC: 0.15 / 0.27 / 0.40 / 0.62 m at 100 / 200 / 300 / 500 ms (median), not measured past 500 ms: flat

/* the game data (re/melee-dc/melee-moves.json.xz) says every npc-1h-hr-* MOVE plays the anim ellie-1h-hr-* (56 of 56; no anim is named npc-1h-hr-*), so SP's enemy plays the same clips we do - the npc-clip path is OFF unless t2r-hr-npc-clips-on.txt exists */
    static int mhr_npc_old(void)
{
    static int v = -1;
    static uint64_t at;
    uint64_t n = GetTickCount64();
    if (v < 0 || n - at > 3000) {
        at = n;
        v = GetFileAttributesA("t2r-hr-npc-clips-on.txt") ==
            INVALID_FILE_ATTRIBUTES;
    }
    return v;
}

static int MhrTurnOld(void)
{
    static int off = -1;
    static uint64_t at;
    return FeelFlag("t2r-hr-turn-ps28.txt", &at, &off, 180);
}

static uint64_t MhrNpcSidOf(int k)
{
    static uint64_t h[MHR_POOL_N];
    if (k < 0 || k >= MHR_POOL_N)
        return 0;
    if (!h[k]) {
        char b[96];
        snprintf(b, sizeof b, "npc-1h-hr-%s", MHR_POOL[k].n);
        h[k] = Fnv64Rt(b);
    }
    return h[k];
}

static int MhrNpcSid(uint64_t sid)
{
    for (int k = 0; k < MHR_POOL_N; k++)
        if (MhrNpcSidOf(k) == sid)
            return 1;
    return 0;
}

// (a kick into a wall should trigger a wall reaction, acting like any other hit reaction;
// logs: 'WALL 0.30 m behind the victim', 'recoil path checked - pinned 0.10 m of 0.56 m planned' = the
// kick's reaction ran into the wall and stopped dead): when a KICK's reaction recoil is cut short by a WALL (not a ledge / step), the reaction becomes TLOU2's own
// single-body wall impact (names from the game's sidbase: npc-m-nw-idle-wall-bump-hr-fw-chest, npc-wall-push-a01-impact-hr, wall-knock-stand-l-nw, being-pushed-
// high-wall-fw - the first one loaded in MP), plus the body-on-wall impact sound. Same control return, same lock, same message to the other screens (MSG_HITREACT2).
// t2r-kick-wall-ps48-old.txt = old. Proof line "ps48 wall reaction".
static const char *const MHR48_WALL[] =
    { "npc-m-nw-idle-wall-bump-hr-fw-chest", "npc-wall-push-a01-impact-hr",
"wall-knock-stand-l-nw", "being-pushed-high-wall-fw" };
#define MHR48_WALL_N ((int)(sizeof MHR48_WALL / sizeof *MHR48_WALL))
#define MHR48_ROOM_M 0.40f      /* [TUNING ps48] the wall within this along the recoil (and the recoil cut by >= 0.15 m) = a wall reaction */
static int Mhr48Off(void)
{
    static int v = -1;
    static uint64_t at;
    uint64_t n = GetTickCount64();
    if (v < 0 || n - at > 2000) {
        at = n;
        v = GetFileAttributesA("t2r-kick-wall-ps48-old.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    return v;
}

static int Mhr48IsWall(uint64_t sid)
{
    for (int i = 0; i < MHR48_WALL_N; i++)
        if (Fnv64Rt(MHR48_WALL[i]) == sid)
            return 1;
    return 0;
}

static uint64_t Mhr48WallSid(const char **nm)
{
    static int probed;
    if (!probed) {
        probed = 1;
        char b[512];
        int o = 0;
        for (int i = 0; i < MHR48_WALL_N; i++)
            o += snprintf(b + o, SNLEFT(b, o), " %s=%.2f", MHR48_WALL[i],
                          (double) MdgClipLen(Fnv64Rt(MHR48_WALL[i])));
        Xlog("hitreact: ps48 wall reaction clips on our body (s, 0 = not loaded in MP):%s", b);
    }
    for (int i = 0; i < MHR48_WALL_N; i++) {
        uint64_t sd = Fnv64Rt(MHR48_WALL[i]);
        if (MdgClipLen(sd) > 0.3f) {
            if (nm)
                *nm = MHR48_WALL[i];
            return sd;
        }
    }
    return 0;
}

typedef struct {
    const char *move;
    struct {
        const char *clip;
        uint8_t n;
    } r[8];
} MhrSpRowT;
static const MhrSpRowT MHR_SP[] = {
    { "abby-fists-sprinting-attack-1",
     { { "head-front-c", 1}, { "hip-front-left-from-right-b", 1},
      { "left-horizontal-d", 1}, { "rt-ft-chest-front-horizontal-a", 1},
      { "back-left-shoulder-from-right-a", 1}} },
    { "fist-light-attack-left-1",
     { { "hip-front-right-from-left-a", 2},
      { "chest-front-horizontal-a", 1}, { "hip-back-left-from-right-c", 1},
      { "chest-front-upward-a", 1}, { "hip-front-left-from-right-a", 1},
      { "head-front-c", 1}} },
    { "fist-light-attack-1",
     { { "left-horizontal-d", 3}, { "rt-ft-chest-front-horizontal-a", 2},
      { "back-left-shoulder-from-left-a", 1},
      { "hip-front-left-from-right-b", 1}, { "head-right-a", 1},
      { "head-front-c", 1}, { "hip-front-left-from-right-a", 1}} },
    { "fist-light-attack-2",
     { { "chest-front-horizontal-a", 5}, { "head-front-c", 4},
      { "head-right-a", 2}, { "right-shoulder-from-right-a", 2},
      { "right-horizontal-a", 1}, { "hip-back-left-from-left-a", 1},
      { "right-shoulder-from-right-b", 1}, { "head-back-center-c", 1}} },
    { "fist-light-attack-3",
     { { "rt-ft-chest-front-horizontal-a", 4}, { "head-front-a", 4},
      { "head-front-c", 1}, { "back-right-shoulder-from-right-b", 1}} },
    { "fist-light-attack-4-punch",
     { { "right-horizontal-a", 4}, { "head-front-c", 1},
      { "head-right-b", 1}} },
    { "fist-light-attack-4-kick",
     { { "chest-front-downward-a", 1},
      { "rt-ft-chest-front-horizontal-a", 1}} },
    { "fist-dodge-back-attack-1",
     { { "hip-front-left-from-right-b", 2}, { "head-left-a", 1},
      { "head-front-b", 1}, { "hip-front-right-from-right-b", 1},
      { "left-downward-a", 1}, { "hip-front-left-from-right-a", 1},
      { "left-horizontal-b", 1}, { "rt-ft-chest-front-horizontal-a", 1}} },
    { "abby-1h-blunt-light-1",
     { { "rt-ft-chest-front-horizontal-a", 1},
      { "hip-front-left-from-right-b", 1}, { "hip-front-left-from-right-a",
                                            1}} },
    { "abby-1h-blunt-light-2",
     { { "head-front-b", 1}, { "hip-front-left-from-right-a", 1}} },
    { "abby-1h-blunt-light-3", { { "head-front-c", 1}} },
    { "abby-1h-blunt-dodge-back-attack-1",
     { { "head-front-c", 1}, { "left-horizontal-d", 1}} },
    { "ellie-1h-blunt-light-1", { { "chest-front-downward-c", 1}} },
    { "ellie-1h-blunt-sprinting-attack-1",
     { { "hip-front-left-from-right-a", 1}} },
    { "ellie-1h-blade-light-1",
     { { "hip-front-left-from-right-a", 3}, { "chest-front-downward-a", 1},
      { "head-front-c", 1}, { "rt-ft-chest-front-horizontal-a", 1}} },
    { "ellie-1h-blade-light-2",
     { { "right-shoulder-from-right-b", 1}, { "chest-front-downward-c", 1},
      { "head-right-a", 1}, { "left-horizontal-d", 1},
      { "chest-front-horizontal-a", 1}} },
    { "ellie-1h-blade-sprinting-attack-1",
     { { "back-left-shoulder-from-right-a", 1}} },
    { "ellie-1h-blade-dodge-back-attack-1",
     { { "hip-front-left-from-right-a", 1}} },
};

#define MHR_SP_N ((int)(sizeof MHR_SP / sizeof *MHR_SP))
#define MHR_SP_FRONT_DEG 60.0f  // [TUNING ps25] the attacker within this of our facing = a front blow (the duel's own front arc, MDU_SIDE_DEG)
#define MHR_SP_BACK_DEG 120.0f  // [TUNING ps25] beyond this = from behind
static const char *MduPlainMove(uint32_t slot);       // melee_duel.c: the SP move of the partner's plain blow on us, NULL = not a plain duel blow
static int MhrPickOld(void)
{
    static int off = -1;
    static uint64_t at;
    return FeelFlag("t2r-hr-pick-ps24.txt", &at, &off, 180);
}

static int MhrSpSide(const char *n)
{                               // +1 = named for our left, -1 = our right, 0 = no side in the name
    if (strstr(n, "-from-left"))
        return 1;
    if (strstr(n, "-from-right"))
        return -1;
    if (!strncmp(n, "left-", 5) || !strncmp(n, "head-left", 9))
        return 1;
    if (!strncmp(n, "right-", 6) || !strncmp(n, "head-right", 10))
        return -1;
    return 0;
}

// the pool index of the reaction for the move `mv`, the attacker standing `ang` degrees around our facing (0 = in front, + = on our left); -1 = the old pick
static int MhrSpPick(const char *mv, float ang)
{
    const MhrSpRowT *row = NULL;
    size_t bl = 0;
    for (int i = 0; i < MHR_SP_N; i++) {
        size_t l = strlen(MHR_SP[i].move);
        if (l > bl && !strncmp(mv, MHR_SP[i].move, l)
            && (mv[l] == 0 || mv[l] == '-')) {
            row = &MHR_SP[i];
            bl = l;
        }
    }
    static int nl;
    if (!row) {
        if (LOGCAP(nl, 80))
            Xlog("hitreact: SP has no reaction sample for the move %s - the old pick", mv);
        return -1;
    }
    int zone = (ang > -MHR_SP_FRONT_DEG
                && ang < MHR_SP_FRONT_DEG) ? 0 : (ang >= MHR_SP_BACK_DEG
                                                  || ang <=
                                                  -MHR_SP_BACK_DEG) ? 1 :
        2, side = ang > 0.0f ? 1 : -1;
    int cand[8], wt[8], n = 0, tot = 0, pass_used = 0;
    for (int pass = 0; pass < 2 && !n; pass++) {
        if (pass == 1 && zone != 2)
            break;              // (front: pass 0 was the front set; behind with no back member: the directional pool)
        pass_used = pass;
        for (int j = 0; j < 8 && row->r[j].clip; j++) {
            const char *c = row->r[j].clip;
            int back = strstr(c, "back") != NULL;
            int ok =
                pass == 1 ? !back : zone == 0 ? !back : zone ==
                1 ? back : (!back && MhrSpSide(c) == side);
            if (!ok)
                continue;
            int q = -1;
            for (int k = 0; k < MHR_POOL_N && q < 0; k++)
                if (!strcmp(MHR_POOL[k].n, c))
                    q = k;
            if (q < 0 || !(MdgClipLen(MhrSid(q)) > 0.2f))
                continue;
            cand[n] = q;
            wt[n] = row->r[j].n;
            tot += wt[n];
            n++;
        }
    }
    if (!n) {
        if (LOGCAP(nl, 80))
            Xlog("hitreact: SP's reactions for %s have no loaded member for a blow from %+.0f deg - the old pick", row->move, (double) ang);
        return -1;
    }
    if (n > 1)
        for (int i = 0; i < n; i++)
            if (MhrSid(cand[i]) == g_mhr.last_sid) {
                tot -= wt[i];
                cand[i] = cand[n - 1];
                wt[i] = wt[n - 1];
                n--;
                break;
            }
    int r = (int) (HrxRnd() % (uint32_t) tot), k = cand[0];
    for (int i = 0; i < n; i++) {
        if (r < wt[i]) {
            k = cand[i];
            break;
        }
        r -= wt[i];
    }
    if (LOGCAP(nl, 80))
        Xlog("hitreact: SP reaction table (ps25) - %s from %+.0f deg around our facing (%s): npc-1h-hr-%s, 1 of %d candidate(s) of the move's %s, SP weight %d", row->move, (double) ang, zone == 0 ? "front" : zone == 1 ? "behind" : side > 0 ? "our left" : "our right", MHR_POOL[k].n, n, pass_used ? "front set (SP has no member named for that side)" : zone == 0 ? "front set" : zone == 1 ? "back set" : "set named for that side", tot);
    return k;
}

// ---- the pick
static int MhrStrength(uint64_t w, float dmg, int *two)
{
    int t2 = 0;
    MhfxClass(w, &t2, NULL);
    int wpn = MeleeClassOf(w, NULL) >= 0;
    int str = !wpn ? 0 : t2 ? 2 : 1;
    if (!wpn && dmg >= 28.0f)
        str = 1;                // an upgraded / hard fist blow
    else if (wpn && !t2 && dmg >= 45.0f)
        str = 2;
    *two = t2;
    return str;
}

// the clip for this blow; returns its pool index (>= 0), 100 + n for a 2h clip, or -1; the victim's facing / attacker's place from the two locators
static int MhrPick(uint32_t slot, int dir, int str, int two_h,
                    int *used_str)
{
    uint64_t now = GetTickCount64();
    int ang = -1, hand = -1;    // the announced swing (melee_dodge.c): 0 horizontal / 1 downward / 2 upward, 0 left / 1 right hand
    if (slot < T2R_MAX_PEERS && g_mdg.sw[slot].ms
        && now - g_mdg.sw[slot].ms < 1500) {
        ang = g_mdg.sw[slot].cls;
        if (g_mdg.sw[slot].hand_real)
            hand = g_mdg.sw[slot].hand;
    }
    static const char *const AN[3] =
        { "horizontal", "downward", "upward" };
    static const int ORD[3][3] = { { 0, 1, 2 }, { 1, 0, 2 }, { 2, 1, 0 } };
    for (int oi = 0; oi < 3; oi++) {
        int s = ORD[str][oi];
        int cand[80], score[80], n = 0, best = -1;
        for (int k = 0; k < MHR_POOL_N; k++) {
            if (MHR_POOL[k].dir != dir || MHR_POOL[k].str != s
                || MHR_POOL[k].two_h > two_h)
                continue;
            uint64_t sid = MhrSid(k);
            if (!(MdgClipLen(sid) > 0.2f) || sid == g_mhr.last_sid)
                continue;
            int sc = 0;
            if (ang >= 0 && strstr(MHR_POOL[k].n, AN[ang]))
                sc += 2;
            if (hand >= 0
                && strstr(MHR_POOL[k].n,
                          hand ? "from-left" : "from-right"))
                sc += 1;        // his right hand swings in from our left
            cand[n] = k;
            score[n] = sc;
            if (sc > best)
                best = sc;
            n++;
        }
        if (s == 2 && two_h && dir == 1)
            for (int k = 0; k < 3; k++) {
                uint64_t sid = Fnv64Rt(MHR_2H[k]);
                if (MdgClipLen(sid) > 0.2f && sid != g_mhr.last_sid
                    && n < 79) {
                    cand[n] = 100 + k;
                    score[n] = 1;
                    if (best < 1)
                        best = 1;
                    n++;
                }
            }
        if (!n)
            continue;
        int m = 0, pick[80];
        for (int i = 0; i < n; i++)
            if (score[i] == best)
                pick[m++] = cand[i];
        *used_str = s;
        return pick[HrxRnd() % (uint32_t) m];
    }
    return -1;
}

// ---- the victim
static void MhrEnd(const char *why, int release)
{
    if (!g_mhr.on)
        return;
    uint64_t now = GetTickCount64();
    float lb[16];
    const float *l = Uc4MyLoc(lb);
    float moved =
        l ? __builtin_sqrtf((l[0] - g_mhr.from[0]) *
                            (l[0] - g_mhr.from[0]) + (l[2] -
                                                      g_mhr.from[2]) *
                            (l[2] - g_mhr.from[2])) : 0.0f;
    // ('reaction over after 500 ms (a dodge owns the body) ... clip blended out over 0.45 s': the last blow's reaction clip faded for
    // 0.45 s UNDER the dodge clip, so the first half of the dodge still showed a hit pose). SP: the dodge takes the body over its own fade-in, 11 frames
    // (re/sp-melee-spec-1110.md 6) - the reaction leaves over the same 0.183 s. t2r-react-dodge-blend-ps34.txt = the reaction's own blend-out.
    if (release && why && !strcmp(why, "a dodge owns the body")
        && g_mhr.blend_out > 0.183f
        && GetFileAttributesA("t2r-react-dodge-blend-ps34.txt") ==
        INVALID_FILE_ATTRIBUTES)
        g_mhr.blend_out = 0.183f;
    if (release)
        MhrStop(SID_PLAYER, g_mhr.sid, g_mhr.blend_out);
    // ps25: every screen keeps the clip on our puppet up to its exit (mhr_exit_ms_of) - when we leave it earlier they are told (flags 0x80, the clip): blend-out there too
    if (release && g_mhx.full && g_sendTramp) {
        uint8_t xb[11] = { 0x80 };
        memcpy(xb + 1, &g_mhr.sid, 8);
        g_sendTramp(MSG_HITREACT2, xb, 11, 0xff);
    }
    if (g_mhb44.on && g_mhx.turn == 3) {
        static int nc4;
        if (LOGCAP(nc4, 40))
            Xlog("hitreact: ps44 back reaction's turn to the attacker cut after %d frame(s) (%s)", g_mhb44.f, why ? why : "?");
    }
    g_mhb44.on = 0;
    g_mhx.ctl_back = 0;
    g_mhx.turn = 0;
    Xlog("hitreact: reaction over after %llu ms (%s) - moved %.2f m, clip blended out over %.2f s, locomotion / stick back", (unsigned long long) (now - g_mhr.t0), why, (double) moved, release ? (double) g_mhr.blend_out : 0.0);
    g_mhr.on = 0;
}

// mhstun_note (melee_stagger.c): a melee blow from `slot` hurt us and left us standing, all its filters passed. 1 = the SP clip plays; *ms = the window the command lock should last
static int g_mhrPunchThud;    // tentative: defined again below
static int MduOwnsReact(uint32_t slot);
static const char *MduPlainSpClip(uint32_t slot, const char **mv);   // melee_duel.c: engaged in a duel with that slot - its -hit pair is the reaction
static int MhrVictimStart(uint32_t slot, uint64_t w, float dmg,
                            uint64_t *ms)
{
    if (MhrOff())
        return 0;
    uint64_t now = GetTickCount64();
    const char *skip = NULL;
    uint64_t none = 0, cr =
        DcCall(FNV64("player-is-crouched?"), 0, &none);
    float vb[16], ab[16];
    const float *vl = Uc4MyLoc(vb);
    uint64_t ah = PuppetHandle(slot);
    const float *al = ah ? PuppetLocator(ah, ab) : NULL;
    // v5-armed: an armed duel victim takes every blow of the combo PLAIN (melee_duel.c mdu_contact) - the next blow (~0.4-0.6 s after the last) restarts the reaction
    // the way SP's npc-standing-hit-reaction does on each blow, instead of being dropped by the running one / the 1 s grazing window
    int plain = MduPlainStr(slot) >= 0;
    // ps25: a running reaction is REPLACED by the next plain duel blow (as before) and, past its exit point, by any blow (SP: 67 of 100 reactions were replaced by the
    // next hit). It is ended only once the new clip is certain (below, before its geometry is set): a blow that gets no reaction leaves the running one untouched
    int repl = g_mhr.on && (plain || g_mhx.ctl_back);
    if (g_mhr.on && !repl)
        skip = "a reaction is already running";
    else if (!plain && !Mv2On() && g_mhr.last_start_ms
             && now - g_mhr.last_start_ms < MHR_GRAZE_MS)
        skip = "grazing window (1.0 s since the last reaction)";
    else if (MduOwnsReact(slot)) {
        skip =
            "in a duel with the attacker: its SP -hit pair is the reaction";
        if (LocalHp() <= 1.0f && MhrFistW(w)) {
            skip =
                "in a duel: the lethal blow (finisher punch, the down takes it)";
            g_mhrPunchThud = 1;
            MhrPunchAt(SID_PLAYER, 2, "us (duel lethal blow)");
            g_mhrPunchThud = 0;
        }
    }                           // both played - the victim swerved sideways
    else if (MdgBusy())
        skip =
            "busy (downed / stun / execution / paired move / revive / backpack / intro)";
    else if (cr != ~0ULL && (cr & 0xff))
        skip = "crouched (the additive set stays)";
    else if (g_msAtk && !Mv2On())
        skip = "we are mid melee swing (the swing is extended instead)";
    else if (g_mdg.cmb_on || g_md_fb.on)
        skip = "a dodge owns the body";
    else if (!vl || !al || !__builtin_isfinite(vl[0])
             || !__builtin_isfinite(al[0]))
        skip = "no locators";
    else {                      // the full-body clip + pin would replace a ladder climb / mantle / jump / fall mid-move - skip when our root moved > 0.12 m vertically in the last 250 ms
        float lo = vl[1], hi = vl[1];
        for (int i = 0; i < 8; i++)
            if (g_mhr.yt[i] && now - g_mhr.yt[i] <= 250) {
                if (g_mhr.ys[i] < lo)
                    lo = g_mhr.ys[i];
                if (g_mhr.ys[i] > hi)
                    hi = g_mhr.ys[i];
            }
        if (hi - lo > 0.12f)
            skip = "moving vertically (ladder / mantle / jump / fall)";
    }
    static int nl;
    if (skip) {
        if (LOGCAP(nl, 60))
            Xlog("hitreact: blow from slot %u (weapon %llx, dmg %.0f) - no SP reaction: %s", slot, (unsigned long long) w, (double) dmg, skip);
        return 0;
    }
    if (g_msAtk && Mv2On()) { // melee v2 (SP): the blow interrupts our swing - cancel it, the reaction owns the body
        uint64_t me0 = SID_PLAYER, r =
            DcCall(0x394888944e949d5fULL /* cancel-melee-action */ , 1,
                    &me0);
        static int nc;
        if (LOGCAP(nc, 40))
            Xlog("melee v2: hit mid swing by slot %u - our swing cancelled (-> %llx), reaction plays", slot, (unsigned long long) r);
    }
    if (Mv2On())
        MduSwingHitCancel(slot);     // the reaction owns the body - the duel's swing record (warp + contact) stops with the engine swing
    int dir = DirAround(vl, al), two = 0, str =
        MhrStrength(w, dmg, &two), used = str;
    int back44 = 0;
    float ang44 = 0.0f;         // ps44 T9: the attacker over MHR_BACK44_DEG off our facing
    {
        float vfx4 = 2 * (vl[4] * vl[6] + vl[7] * vl[5]), vfz4 =
            1 - 2 * (vl[4] * vl[4] + vl[5] * vl[5]), adx4 =
            al[0] - vl[0], adz4 = al[2] - vl[2];
        ang44 = __builtin_atan2f(vfz4 * adx4 - vfx4 * adz4, vfx4 * adx4 + vfz4 * adz4) * 57.29578f;     // + = the attacker on our left
        if (!MhrBack44Off()
            && (ang44 > MHR_BACK44_DEG || ang44 < -MHR_BACK44_DEG)) {
            back44 = 1;
            if (dir != 1) {
                static int nd;
                if (LOGCAP(nd, 40))
                    Xlog("hitreact: ps44 back blow from %+.0f deg - the 'behind' row (dir_around said %s)", (double) ang44, MHR_DIRN[dir]);
            }
            dir = 1;
        }
    }
    {
        int fl = MduPlainStr(slot);
        if (fl > str) {
            str = fl;
            used = str;
        }
    }                           // v5-var: a plain duel blow - SP's combo 3 / 4 blows hit harder (medium / heavy reaction)
    // The character winced in the wrong direction of the punch: the reaction follows WHERE the blow landed - the attacker's striking wrist
    // (the one nearer our head at the hit) in our own frame: at head height a head reaction toward the side it came from (head-left / head-right / head-front),
    // lower down the body pool with its from-left / from-right taken from the wrist instead of the announced (often guessed) hand.
    int k = -1;
    char why[96] = "";
    int sp_tab = 0, turn = 0, npc = 0;
    float turn_t = 0.0f;        // ps29
    {                           // ps25: a plain duel blow - SP's reaction set for the attacker's MOVE, picked by where the attacker stands (mhr_sp_pick above); the strength (= the control-return
        // time) stays the blow's own, a head clip no longer raises it (the victim is never locked longer than before)
        const char *pm = MhrPickOld()? NULL : MduPlainMove(slot);
        float vfx = 2 * (vl[4] * vl[6] + vl[7] * vl[5]), vfz =
            1 - 2 * (vl[4] * vl[4] + vl[5] * vl[5]), adx =
            al[0] - vl[0], adz = al[2] - vl[2];
        float ang = __builtin_atan2f(vfz * adx - vfx * adz, vfx * adx + vfz * adz) * 57.29578f; // + = the attacker on our left (dir_around's side)
        int turn_ok = pm && !MhrTurnOld() && ang > -MHR_SP_BACK_DEG && ang < MHR_SP_BACK_DEG && !back44;      // ps29 (b): we will face the attacker - the front set (ps44: not for a back blow - it turns AFTER)
        int q = pm ? MhrSpPick(pm, turn_ok ? 0.0f : back44 ? (ang > 0.0f ? 180.0f : -180.0f) : ang) : -1;     // (ps44: a back blow takes the move's BACK set)
        if (q >= 0) {
            k = q;
            used = str;
            sp_tab = 1;
            turn = turn_ok && (ang > 3.0f || ang < -3.0f);
            turn_t = __builtin_atan2f(adx, adz);
            snprintf(why, sizeof why,
                     " [SP table ps25: %s, attacker %+.0f deg]", pm,
                     (double) ang);
        }
    }
    if (k < 0 && dir == 0 && MhrFistW(w)) {   // a plain duel fist blow on an armed victim = SP's reaction for that move (mdu_plain_sp_clip), not the wrist guess
        const char *mv = NULL, *sc = MduPlainSpClip(slot, &mv);
        if (sc)
            for (int q = 0; q < MHR_POOL_N && k < 0; q++)
                if (!strcmp(MHR_POOL[q].n, sc)
                    && MdgClipLen(MhrSid(q)) > 0.2f)
                    k = q;
        if (k >= 0) {
            used = (!strncmp(sc, "head", 4) && str < 1) ? 1 : str;
            if (!strncmp(sc, "chest", 5) || !strncmp(sc, "rt-ft", 5))
                used = 1;
            snprintf(why, sizeof why, " [SP table: %s -> npc-1h-hr-%s]",
                     mv, sc);
            Xlog("hitreact: fist blow %s on an armed victim - SP's reaction for that move is %s (not the wrist pick)", mv, sc);
        }
    }

    {
        float hb[3], wl[3], wr[3], pv[3];
        float fx0 = 2 * (vl[4] * vl[6] + vl[7] * vl[5]), fz0 =
            1 - 2 * (vl[4] * vl[4] + vl[5] * vl[5]), fl0 =
            __builtin_sqrtf(fx0 * fx0 + fz0 * fz0);
        int okw = k < 0 && fl0 > 1e-3f && dir == 0
            && GetFileAttributesA("t2r-no-hr-wrist.txt") ==
            INVALID_FILE_ATTRIBUTES
            && GxJointPos(SID_PLAYER, "headb", hb)
            && GxJointPos(SID_PLAYER, "pelvis", pv);
        int hl = okw && GxJointPos(ah, "l_wrist", wl), hr = okw
            && GxJointPos(ah, "r_wrist", wr);
        if (hl || hr) {
            fx0 /= fl0;
            fz0 /= fl0;
            float dl =
                hl ? (wl[0] - hb[0]) * (wl[0] - hb[0]) + (wl[1] -
                                                          hb[1]) * (wl[1] -
                                                                    hb[1])
                + (wl[2] - hb[2]) * (wl[2] - hb[2]) : 1e9f;
            float dr =
                hr ? (wr[0] - hb[0]) * (wr[0] - hb[0]) + (wr[1] -
                                                          hb[1]) * (wr[1] -
                                                                    hb[1])
                + (wr[2] - hb[2]) * (wr[2] - hb[2]) : 1e9f;
            const float *wp = dl <= dr ? wl : wr;
            float lat = (wp[0] - hb[0]) * fz0 + (wp[2] - hb[2]) * -fx0; // + = on our left
            int head = wp[1] >= hb[1] - 0.35f, from_left = lat > 0.0f;
            // SP recording: front fist blows to the head played head-front-c in 7 of 8 (the 8th, a wall punch, -a); chest-height fist blows chest-front-horizontal-a / rt-ft-chest-front-horizontal-a
            static const char *const HL[] =
                { "head-left-a" }, *const HR[] =
                { "head-right-a", "head-right-b" }, *const HF[] =
                { "head-front-a", "head-front-b", "head-front-c",
"head-front-d" }, *const HFC[] = { "head-front-c" }, *const CH[] =
{ "chest-front-horizontal-a", "rt-ft-chest-front-horizontal-a" };
            int fistw = MhrFistW(w);
            const char *const *L = NULL;
            int nL = 0;
            if (head) {
                if (lat > 0.10f) {
                    L = HL;
                    nL = 1;
                } else if (lat < -0.10f) {
                    L = HR;
                    nL = 2;
                } else if (fistw) {
                    L = HFC;
                    nL = 1;
                } else {
                    L = HF;
                    nL = 4;
                }
            }
            // v5-hrside (stomach punches played to the left; B 18518: a right-hand gut punch 0.17 m left of our head played
            // rt-ft-chest-front-horizontal-a, whose recoil steps off to the side): the rt-ft variant only for a blow from our right. t2r-hr-chest-both.txt = old.
            else if (fistw && wp[1] >= pv[1] + 0.12f) {
                L = CH;
                nL = from_left
                    && GetFileAttributesA("t2r-hr-chest-both.txt") ==
                    INVALID_FILE_ATTRIBUTES ? 1 : 2;
            }
            if (L) {
                int cand[4], n = 0, chest = L == CH;
                for (int i = 0; i < nL; i++)
                    for (int q = 0; q < MHR_POOL_N; q++)
                        if (!strcmp(MHR_POOL[q].n, L[i])) {
                            uint64_t sd = MhrSid(q);
                            if (MdgClipLen(sd) > 0.2f
                                && sd != g_mhr.last_sid)
                                cand[n++] = q;
                        }
                // v5-hrrep (B 18632: a left jab at the head, head-left-a was the last reaction = no candidate, it fell through to the hip pool -
                // hip-front-right-from-left-a for a face jab): a one-clip list repeats its clip instead
                if (!n
                    && GetFileAttributesA("t2r-hr-no-repeat.txt") ==
                    INVALID_FILE_ATTRIBUTES)
                    for (int i = 0; i < nL; i++)
                        for (int q = 0; q < MHR_POOL_N; q++)
                            if (!strcmp(MHR_POOL[q].n, L[i])
                                && MdgClipLen(MhrSid(q)) > 0.2f
                                && n < 4)
                                cand[n++] = q;
                if (n) {
                    k = cand[HrxRnd() % (uint32_t) n];
                    used = str < 1 ? 1 : str;
                    if (chest)
                        used = 1;
                }
            } else {            // a body blow: the hip / chest pool, side from the wrist
                if (slot < T2R_MAX_PEERS) {
                    g_mdg.sw[slot].hand = (uint8_t) from_left;
                    g_mdg.sw[slot].hand_real = 1;
                }
            }
            snprintf(why, sizeof why,
                     " [%s wrist %s, %.2f m %s of our head, %.2f m below it]",
                     dl <= dr ? "left" : "right",
                     head ? "at the head" : "at the body",
                     (double) __builtin_fabsf(lat),
                     from_left ? "left" : "right",
                     (double) (hb[1] - wp[1]));
        }
    }
    if (k < 0)
        k = MhrPick(slot, dir, str, two, &used);
    if (k < 0) {
        if (LOGCAP(nl, 60))
            Xlog("hitreact: blow from slot %u - no SP hit-reaction clip loaded for %s blows from the %s (the old flinch + lock stay)", slot, MHR_STRN[str], MHR_DIRN[dir]);
        return 0;
    }
    char nm[96];
    if (k >= 100)
        snprintf(nm, sizeof nm, "%s", MHR_2H[k - 100]);
    else
        snprintf(nm, sizeof nm, "ellie-1h-hr-%s", MHR_POOL[k].n);
    uint64_t sid = Fnv64Rt(nm);
    const MhrRm *rm = mhr_rm_find(sid);
    if (sp_tab && k < 100 && !mhr_npc_old()) {  // ps29 (a): SP's own clip for the hit enemy
        uint64_t ns = MhrNpcSidOf(k);
        if (ns && MdgClipLen(ns) > 0.2f) {
            snprintf(nm, sizeof nm, "npc-1h-hr-%s", MHR_POOL[k].n);
            sid = ns;
            rm = &MHR_RM_SPNPC;
            npc = 1;
        } else {
            static int nn;
            if (LOGCAP(nn, 40))
                Xlog("hitreact: npc-1h-hr-%s is NOT loaded here - the player's clip %s plays instead (ps29: SP's enemy reaction needs its pak in the MP build)", MHR_POOL[k].n, nm);
        }
    }
    float dur = MdgClipLen(sid);
    uint64_t cut = MHR_CUT_MS[used];
    int full = !MhrLenOld();
    uint64_t exit_ms = full ? MhrExitMsOf(sid, cut) : cut;   // ps25: see g_mhx
    float fx = 2 * (vl[4] * vl[6] + vl[7] * vl[5]), fz =
        1 - 2 * (vl[4] * vl[4] + vl[5] * vl[5]), fl =
        __builtin_sqrtf(fx * fx + fz * fz);
    if (!(fl > 1e-3f) || !rm) {
        if (LOGCAP(nl, 60))
            Xlog("hitreact: %s - no root-motion data / facing, not played",
                 nm);
        return 0;
    }
    fx /= fl;
    fz /= fl;
    float yaw0 = __builtin_atan2f(fx, fz);
    if (turn) {
        fx = __builtin_sinf(turn_t);
        fz = __builtin_cosf(turn_t);
    }                           // ps29 (b): the recoil frame = the facing after the turn (toward the attacker)
    if (repl)
        MhrEnd(plain ? "the next plain duel blow restarts it" :
                "the next blow replaces it (past its exit point, ps25)",
                0);
    g_mhr.fwd[0] = fx;
    g_mhr.fwd[1] = fz;
    g_mhr.left[0] = fz;
    g_mhr.left[1] = -fx;
    float ox, oz;
    mhr_rm_at(rm, (float) cut / 1000.0f, &ox, &oz);
    float wx = oz * fx + ox * g_mhr.left[0], wz =
        oz * fz + ox * g_mhr.left[1], wd =
        __builtin_sqrtf(wx * wx + wz * wz);
    g_mhr.total = wd;
    g_mhr.cap = full ? wd : MHR_CAP_M[used];    // ps25: no cap on the clip's own recoil (SP 0.40 m at 300 ms, 0.62 m at 500 ms); the wall / ground probes below still limit it
    if (wd > 0.02f) {
        g_mhr.u[0] = wx / wd;
        g_mhr.u[1] = wz / wd;
    } else {
        g_mhr.u[0] = fx;
        g_mhr.u[1] = fz;
        g_mhr.total = 0.0f;
    }
    g_mhr.dist_cut = wd;
    float total = g_mhr.total < g_mhr.cap ? g_mhr.total : g_mhr.cap;
    // wall / ground probes along the recoil (the dodge mover's rays: walls at 0.5 / 1.0 m height, ground at the end spot); judged next frame
    int push_on = !MhrPushOff() && g_mhr.total > 0.02f;
    float full_t = total;       // ps25: the clip's own travel along the recoil direction by its exit (the probes cover the whole path)
    if (full && g_mhr.total > 0.02f) {
        float ex, ez;
        mhr_rm_at(rm, (float) exit_ms / 1000.0f, &ex, &ez);
        float pr =
            (ez * fx + ex * g_mhr.left[0]) * g_mhr.u[0] + (ez * fz +
                                                           ex *
                                                           g_mhr.left[1]) *
            g_mhr.u[1];
        if (pr > full_t)
            full_t = pr;
    }
    float target = push_on ? (full ? full_t : MHR_PUSH_M[used]) : total;
    if (push_on) {
        float adx = al[0] - vl[0], adz = al[2] - vl[2], adl =
            __builtin_sqrtf(adx * adx + adz * adz);
        if (adl > 0.05f
            && (adx * g_mhr.u[0] + adz * g_mhr.u[1]) / adl > 0.35f) {
            push_on = 0;
            target = total;
        }
    }                           // (a clip whose recoil leans toward the attacker: no push into him)
    if (target < total)
        target = total;
    g_mhr.push = push_on ? target : 0.0f;
    g_mhr.push_ext = 0.0f;
    g_mhr.ldist = 0.0f;
    g_mhr.vend = 0.0f;
    g_mhr.lt = 0;
    float far_ = target + 0.4f;
    float a1[3] = { vl[0], vl[1] + 0.5f, vl[2] }, b1[3] =
        { vl[0] + g_mhr.u[0] * far_, vl[1] + 0.5f,
vl[2] + g_mhr.u[1] * far_ };
    float a2[3] = { vl[0], vl[1] + 1.0f, vl[2] }, b2[3] =
        { b1[0], vl[1] + 1.0f, b1[2] };
    float c1[3] =
        { vl[0] + g_mhr.u[0] * total, vl[1] + 0.6f,
  vl[2] + g_mhr.u[1] * total }, e1[3] = { c1[0], vl[1] - 0.9f, c1[2] };
    // live logs: "recoil path checked - 0.00 m of 0.32 m planned" in 6 of 7 reactions - world_ray_kick's mask 0x207 stops on tall grass (~0.5 m up), the dodge fixed that with 0x1b07
    float c2[3] =
        { vl[0] + g_mhr.u[0] * target, vl[1] + 0.6f,
  vl[2] + g_mhr.u[1] * target }, e2[3] = { c2[0], vl[1] - 0.9f, c2[2] };
    g_mhr.jw[0] = WorldRayKickM(a1, b1, 0, 0x1b07);
    g_mhr.jw[1] = WorldRayKickM(a2, b2, 0, 0x1b07);
    g_mhr.jg = total > 0.02f ? WorldRayKickM(c1, e1, 0, 0x1b07) : 0;
    g_mhr.jg2 = push_on
        && target > total + 0.05f ? WorldRayKickM(c2, e2, 0,
                                                     0x1b07) : 0;
    memcpy(g_mhr.from, vl, 12);
    int stick = 0; {
        float sx, sy;
        MdgStickDir(&sx, &sy);
        stick = sx * sx + sy * sy >= 0.3f * 0.3f;
    }
    float bin = MHR_BLEND_IN;
    if (npc
        && GetFileAttributesA("t2r-hr-blend-ps28.txt") ==
        INVALID_FILE_ATTRIBUTES) {
        bin = MEL_HIT_BLEND;
        g_melFloorOnce = MEL_HIT_BLEND;
    }                           // ps29 (c)
    uint64_t r = MhrAnimate(SID_PLAYER, sid, 0.0f, bin);
    g_melFloorOnce = -1.0f;
    if (r == ~0ULL) {
        if (LOGCAP(nl, 60))
            Xlog("hitreact: %s - animate_ failed, the old flinch + lock stay", nm);
        if (repl)
            MhrStop(SID_PLAYER, g_mhr.sid, MHR_BLEND_OUT);
        g_mhr.jw[0] = g_mhr.jw[1] = g_mhr.jg = g_mhr.jg2 = 0;
        return 0;
    }
    g_mhx.full = full;
    g_mhx.ctl_back = 0;
    g_mhx.exit_ms = exit_ms;
    MhrWpOwnBegin();
    g_mhx.turn = turn;
    g_mhx.turn_f = 0;
    g_mhx.yaw_t = turn_t;
    g_mhx.yaw_c = g_mhx.yaw_0 = yaw0;
    g_mhb44.on = back44;
    g_mhb44.slot = slot;
    g_mhb44.ang = ang44;
    g_mhb44.f = 0;
    if (back44) {
        static int nb4;
        if (LOGCAP(nb4, 80))
            Xlog("hitreact: ps44 back reaction %s for a blow from slot %u at %+.0f deg off our facing (> %.0f) - facing kept while it plays, the turn to the attacker comes at the control return (%llu ms, <= %.1f deg a frame)", nm, slot, (double) ang44, (double) MHR_BACK44_DEG, (unsigned long long) cut, (double) MHR_BACK44_TURN_F);
    }
    if (sp_tab) {
        static int n29;
        if (LOGCAP(n29, 80))
            Xlog("hitreact: ps29 - %s clip %s (%.2f s), blend-in %.3f s%s; %s; knock-back %s", npc ? "SP's NPC" : "the player's", nm, (double) dur, (double) bin, bin < 0.099f ? "" : " (mhr_animate floors it at 0.10 s)", turn ? "our body turns to face the attacker (<= 15 deg a frame, hitreact.pin)" : "no turn (facing it already / attacker behind / opt-out)", npc ? "SP's measured NPC curve away from the attacker (0.15 / 0.27 / 0.40 / 0.62 m at 100 / 200 / 300 / 500 ms)" : "the player clip's own root path");
    }
    g_mhr.on = 1;
    g_mhr.stage = 0;
    g_mhr.sid = sid;
    g_mhr.t0 = now;
    g_mhr.t0q = PsyncMs();
    g_mhr.cut_ms = cut;
    g_mhr.arm_f = g_frame ? g_frame : 1;
    g_mhr.rm = rm;
    g_mhr.str = used;
    g_mhr.dir = dir;
    {
        int mc = MeleeClassOf(w, NULL);
        BbMeleeNote(T2R_US, mc == MC_1HBLADE || mc == MC_MACHETE);
    }
    g_mhr.last_start_ms = now;
    g_mhr.start_frame = g_frame ? g_frame : 1;
    g_mhr.last_sid = sid;
    g_mhr.blend_out = stick ? MHR_BLEND_OUT_STICK : MHR_BLEND_OUT;
    g_mhr.stick_prev = stick;
    *ms = cut;
    uint8_t b[11] = { 1 };
    b[0] |= MhrPainBits(used);
    if (MhrFistW(w) && !MhrPunchOld()) {
        b[0] |= 0x40;
        MhrPunchAt(SID_PLAYER, used, "us (victim)");
    }
    if (b[0] & 4)
        MhrPainSay(T2R_MAX_PEERS, SID_PLAYER, used, MhrPainSharp());
    memcpy(b + 1, &sid, 8);
    uint16_t c16 = (uint16_t) cut;
    memcpy(b + 9, &c16, 2);
    if (g_sendTramp)
        g_sendTramp(MSG_HITREACT2, b, 11, 0xff);
    Xlog("hitreact: SP reaction %s (%.2f s clip) for a %s blow from slot %u (weapon %llx, dmg %.0f -> strength %s%s) from the %s - blend in %.2f s, cut after %llu ms, recoil %.2f m in the clip by then, capped %.2f m, animate_ -> %llx", nm, (double) dur, MHR_STRN[str], slot, (unsigned long long) w, (double) dmg, MHR_STRN[used], why[0] ? why : used != str ? " (nearest loaded)" : "", MHR_DIRN[dir], (double) MHR_BLEND_IN, (unsigned long long) cut, (double) wd, (double) g_mhr.cap, (unsigned long long) r);
    if (full) {
        static int nf;
        if (LOGCAP(nf, 80))
            Xlog("hitreact: ps25 full reaction - control back at %llu ms (the exit point, as before); the clip plays on until the next blow / our input / its exit at %llu ms; knock-back = the clip's own root path: %.2f m by the control return, up to %.2f m by the exit (wall / ground probes next frame; no cap, no additive push)", (unsigned long long) cut, (unsigned long long) exit_ms, (double) wd, (double) target);
    }
    return 1;
}

// ---- the victim's pain grunt (MP melee felt empty without SP's pain effect). SP, every standing
// melee hit on Ellie (t2r-sp.log): the hit reaction + voice-effect vox-eff-gen-fight-take-med (heavier: -take-lrg) + a pain face. Here the grunt, through `say`
// on the hurt body: the victim on its own body when it judges the blow, everybody else on its puppet from MSG_HITREACT2 bit2 (bits 3-4 = strength 0..2;
// an old client ignores them). Not for downs / deaths / pairs (their own voices). t2r-no-melee-pain-vox.txt = off.
static int MhrPainOff(void)
{
    static int off = -1;
    static uint64_t at;
    uint64_t n = GetTickCount64();
    if (off < 0 || n - at > 2000) {
        at = n;
        off =
            GetFileAttributesA("t2r-no-melee-pain-vox.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    return off;
}

static void DmSay(uint64_t ent, const char *ev, int vxe);
static int MhfxClass(uint64_t w, int *two_h, const char **name);       // melee_hitfx.c: 2 = blade
static int MhrPainSharp(void)
{
    return MhfxClass(g_mhrPainW, NULL, NULL) == 2;
}

static void MhrPainSay(uint32_t who, uint64_t ent, int str, int sharp)
{                               // who: slot, or T2R_MAX_PEERS = us; sharp = a blade (SP: -shrp takes, blunt / fists -blnt)
    static uint64_t last[T2R_MAX_PEERS + 1];
    static int nl;
    uint64_t now = GetTickCount64();
    if (!ent || ent == ~0ULL || who > T2R_MAX_PEERS || MhrPainOff()
        || now - last[who] < 350)
        return;
    last[who] = now;
    // SP trace: a hatchet cut on Ellie -> vox-eff-gen-fight-take-lrg-shrp, a stalker fist -> -take-med (+ -take-med-blnt)
    static const char *const SH[3] = { "vox-eff-gen-fight-take-sml-shrp", "vox-eff-gen-fight-take-med-shrp", "vox-eff-gen-fight-take-med-shrp" };       // (-lrg-shrp: no takes for scrma / mltfa - own voices only)
    // SP Abby fist capture (re/sp-abby-fist-sounds-1026.md): every face punch -> take-med-blnt + take-face 35 ms later (the face line is what is heard), the gut
    // punch -> take-med-blnt-gut, a killing blow -> death-melee-blunt-large (= take-hvy-blnt takes). Every MP voice has take-face / take-med-blnt-gut
    static const char *const BL[3] =
        { "vox-eff-gen-fight-take-face", "vox-eff-gen-fight-take-face",
"vox-eff-gen-fight-take-lrg-blnt" };
    str = str < 0 ? 0 : str > 2 ? 2 : str;
    const char *ev = sharp ? SH[str] : BL[str];
    DmSay(ent, ev, VXE_PAIN);
    if (LOGCAP(nl, 60))
        Xlog("hitreact: pain grunt %s on %s", ev,
             who == T2R_MAX_PEERS ? "us" : "a puppet");
}

// Many hits played no sounds, neither for the victim nor for bystanders: winmm's fist hit list is muted (g_hit_sfx_fist_net) and only the
// ATTACKER played a punch, ~340 ms late, on the victim's confirmation - the victim and every bystander heard nothing. Now: the attacker plays it at the
// contact (unless the victim's dodge press already arrived), the victim on its own head when the blow lands, bystanders on MSG_HITREACT2 bit 0x40
// (= a fist blow landed; an old client never sets it). t2r-punch-sfx-old.txt = the old attacker-only confirmation path.
static int g_mhrPunchThud;
static int MduOwnsPunch(uint64_t ent);
static uint64_t g_mhrAtkPunchMs;     // when we (attacker) played the punch at our own contact: the victim's 0x40 notice must not play it again
static int MhrPunchOld(void)
{
    static int off = -1;
    static uint64_t at;
    uint64_t n = GetTickCount64();
    if (off < 0 || n - at > 2000) {
        at = n;
        off =
            GetFileAttributesA("t2r-punch-sfx-old.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    return off;
}

static uint64_t MhrPunchAt(uint64_t ent, int str, const char *who)
{
    static const char *const PN[3] =
        { "sfx-unarmed-hit-face-skin", "sfx-unarmed-hit-face-skin",
"sfx-unarmed-finisher-face-skin" };
    if (!ent || ent == ~0ULL)
        return 0;
    if (!g_mhrPunchThud && MduOwnsPunch(ent))
        return 1;               // the duel's thud + this one's random (often flammy, "slappy") wave doubled up
    const char *pn = PN[str < 0 ? 0 : str > 2 ? 2 : str];
    // Punches sometimes missed sounds in sequences: every blow of a combo played the SAME event on the SAME head joint, and the
    // sound engine keeps one instance of an event per emitter - the next blow inside the last one's tail was swallowed (the log showed each call).
    // A blow on a head that took a punch in the last 1.5 s alternates to TLOU2's breaker variant (sfx-unarmed-breaker-face-skin, the counter's layer).
    // t2r-punch-sfx-same.txt = always the same event.
    static uint64_t last_ent, last_ms;
    static int alt;
    static int same = -1;
    if (same < 0)
        same =
            GetFileAttributesA("t2r-punch-sfx-same.txt") !=
            INVALID_FILE_ATTRIBUTES;
    uint64_t now_ = GetTickCount64();
    if (!same && str < 2 && ent == last_ent && now_ - last_ms < 1500) {
        alt ^= 1;
        if (alt)
            pn = "sfx-unarmed-breaker-face-skin";
    } else
        alt = 0;
    last_ent = ent;
    last_ms = now_;
    static int nl;
    // The sfx-unarmed-* events return a handle but are SILENT in MP (their layers are
    // not loaded - same finding as the execution punches, t2r-ext.c exec sfx). Every landed fist blow (exchange blows, sequence hits,
    // bystanders, the victim's own head) logged a handle and the recording had no sound there; only the dodge counter's PCM punch was heard.
    // Default now: the ripped TLOU2 punch waves through our PCM mixer, like the counter / executions. t2r-native-punch-hit.txt = the native events.
    static int nat = -1;
    static uint64_t nat_chk;
    if (nat < 0 || now_ - nat_chk > 2000) {
        nat_chk = now_;
        nat =
            GetFileAttributesA("t2r-native-punch-hit.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    if (!nat) {
        static unsigned rp;
        unsigned w = str >= 2 ? 13u + (rp % 5u) : (rp % 13u);   // jab: flammy 0-5 + thud 6-12; heavy: punch-face-finisher 13-17
        if (g_mhrPunchThud && str < 2)
            w = 6u + (rp % 7u); // (duel): the flammy waves sound "slappy" next to SP's hits - thuds only

        rp += 5;
        float master = 0.45f;
        FILE *gf = ExeSideOpen("t2r-melee-sfx-gain.txt", "r");
        if (gf) {
            int pc;
            if (fscanf(gf, "%d", &pc) == 1 && pc >= 0 && pc <= 400)
                master = pc / 100.0f;
            fclose(gf);
        }
        int ok =
            PcmPlayAt2(t2m_blob + T2M_WAVE[w].off, (int) T2M_WAVE[w].n,
                         master, T2M_RATE, ent, NULL);
        if (LOGCAP(nl, 400))
            Xlog("hitreact: punch wave %u (PCM, was %s) at %s -> %d", w,
                 pn, who, ok);
        if (ok)
            return 1;
    }
    uint64_t a[5] = { FNV64(pn), ent, FNV64("heada"), 0, 0 };
    uint64_t r = DcCall(FNV64("play-sound-at-joint"), 5, a);
    if (LOGCAP(nl, 400))
        Xlog("hitreact: punch %s at the head of %s -> %llx", pn, who,
             (unsigned long long) r);
    return r;
}

static int MhrFistW(uint64_t w)
{
    return w == 0x6ac05fba9c921900ULL;
}                               // abby-unarmed

static uint8_t MhrPainBits(int str)
{
    return MhrPainOff()? 0 : (uint8_t) (4 |
                                          ((str < 0 ? 0 : str >
                                            2 ? 2 : str) << 3) |
                                          (MhrPainSharp()? 0x20 : 0));
}

// a no-reaction notice: the attacker's lock must not wait for a reaction that never plays (blow inside the grazing window, victim busy ...)
static void MhrVictimNone2(uint32_t slot, int landed)
{                               // landed: the blow did hit us (bit1: the attacker plays its impact sound); 2 = and it hurt us standing (pain grunt)
    if (MhrOff() || !g_sendTramp)
        return;
    uint8_t b[11] = { 0 };
    b[0] = landed ? 2 : 0;
    if (landed && MhrFistW(g_mhrPainW) && !MhrPunchOld()) {
        b[0] |= 0x40;
        MhrPunchAt(SID_PLAYER, 1, "us (victim)");
    }
    if (landed >= 2) {
        b[0] |= MhrPainBits(1);
        if (b[0] & 4)
            MhrPainSay(T2R_MAX_PEERS, SID_PLAYER, 1, MhrPainSharp());
    }
    (void) slot;
    g_sendTramp(MSG_HITREACT2, b, 11, 0xff);
}

static void MhrVictimNone(uint32_t slot)
{
    MhrVictimNone2(slot, 0);
}

// feel.c hrx_victim_hit: this melee blow is covered by the SP reaction (running, or inside its 1.0 s grazing window): no additive flinch on top
static int MhrCoversBlow(void)
{
    if (MhrOff())
        return 0;
    // A 2nd blow 550 ms after the first landed after the SP clip was cut (453 ms) but inside
    // the 1 s graze window - it showed NOTHING (no clip, no flinch, no grunt). Now only a clip that is really playing covers a blow; otherwise the additive flinch plays.
    return g_mhr.on;
}

static void MhgTick(void);
static void MhrPredTick(uint64_t now);
// the additive push-back after the cut: eased-out velocity v0 (1 - t/T)^2 along the recoil direction, summed on top of the body's own motion (stick stays live);
// ends on a dodge / downed / execution / paired move, a new reaction, a vertical move, a wall one frame ahead, or its time
static int MduWarpOwns(void); // melee_duel.c
static void MhrShoveTick(uint64_t now)
{
    if (!g_msh.on)
        return;
    const char *why = NULL;
    const char *busy = MdgBusy();
    float lb[16];
    const float *l = Uc4MyLoc(lb);
    if (g_mhr.on)
        why = "a new reaction started";
    else if (busy)
        why = busy;
    else if (MhrOff() || MhrPushOff())
        why = "opt-out";
    else if (g_mdg.cmb_on || g_md_fb.on)
        why = "a dodge owns the body";
    // ('YAW SNAP 113 deg ... writers: hitreact.shove', 59 deg, 48 deg - each on the first frame of our own duel
    // swing: the push-back writes the position with the yaw the engine left, so it carried the engine's swing-start snap, and its step ran beside the swing's
    // placement): our duel swing places the body from its first frame - the push-back ends there. t2r-shove-in-swing-old.txt = it runs on.
    else if (MduWarpOwns()
             && GetFileAttributesA("t2r-shove-in-swing-old.txt") ==
             INVALID_FILE_ATTRIBUTES)
        why = "our duel swing places the body";
    else if (MatchSubstate() != 6)
        why = "not in a match";
    else if (!l || !__builtin_isfinite(l[0]))
        why = "no locator";
    else if (__builtin_fabsf(l[1] - g_msh.y0) > 0.2f)
        why = "vertical move (jump / step / fall)";
    double nq = PsyncMs();
    float el = (float) ((nq - g_msh.t0q) / 1000.0);
    if (!why && g_msh.job && g_frame > g_msh.jf) {
        g_rayErr = 0;
        float r = RayT(g_msh.job);
        g_msh.job = 0;
        if (g_rayErr || (r >= 0.0f && r < 0.99f))
            why = "wall / body one step ahead";
    }
    if (!why && (el >= g_msh.T || g_msh.done >= g_msh.d - 0.003f))
        why = "done";
    if (why) {
        Xlog("hitreact: SP push-back over after %.0f ms (%s) - %.2f of %.2f m", (double) (el * 1000.0f), why, (double) g_msh.done, (double) g_msh.d);
        g_msh.on = 0;
        return;
    }
    float dt = (float) ((nq - g_msh.ltq) / 1000.0);
    g_msh.lt = now;
    g_msh.ltq = nq;
    if (dt > 0.05f)
        dt = 0.05f;
    if (dt < 0.0f)
        dt = 0.0f;
    float k = 1.0f - el / g_msh.T, step = g_msh.v0 * k * k * dt;
    if (g_msh.done + step > g_msh.d)
        step = g_msh.d - g_msh.done;
    if (step <= 0.0f)
        return;
    float l8[8] =
        { l[0] + g_msh.u[0] * step, l[1], l[2] + g_msh.u[1] * step, 1.0f,
  l[4], l[5], l[6], l[7] };
    static PsyncBf bf;
    PsyncBfFromLoc(&bf, l8);
    mo_write(SID_PLAYER, &bf, MO_SRC("hitreact.shove"), 0);
    g_msh.done += step;
    if (!g_msh.job) {           // the next probe: 0.5 m up, 0.3 m + two steps ahead of where we are now (read on the next frame)
        float la = 0.3f + 2.0f * step, a[3] =
            { l8[0], l[1] + 0.5f, l8[2] }, b[3] =
            { l8[0] + g_msh.u[0] * la, l[1] + 0.5f,
l8[2] + g_msh.u[1] * la };
        g_msh.job = WorldRayKickM(a, b, 0, 0x1b07);
        g_msh.jf = g_frame;
    }
}

static void MhrTick(void)
{
    MhgTick();
    MhrShoveTick(GetTickCount64());
    uint64_t now = GetTickCount64();
    MhrPredTick(now);
    MhrWpPupTick(now);       // ps25: the hold layers of puppets in a reaction
    P30Tick();                 // ps30: the arm hold of an armed victim's synced -hit half (our body / its puppet)
    {
        float yb[16];
        const float *yl = Uc4MyLoc(yb);
        if (yl && __builtin_isfinite(yl[1])) {
            g_mhr.yi = (g_mhr.yi + 1) & 7;
            g_mhr.ys[g_mhr.yi] = yl[1];
            g_mhr.yt[g_mhr.yi] = now;
        }
    }
    // the engine's own SP hit-reaction move on us (diagnostic: would double ours)
    if (g_msCurmove && mhr_rm_find(g_msCurmove) && g_mhr.on) {
        static int ne;
        if (LOGCAP(ne, 8))
            Xlog("hitreact: the engine's own hit-reaction move %llx runs under ours (ours plays on top)", (unsigned long long) g_msCurmove);
    }
    // puppets: blend out the clips we forced
    for (uint32_t k = 0; k < T2R_MAX_PEERS; k++) {
        if (!g_mhr.pd[k].end || now < g_mhr.pd[k].end)
            continue;
        MhrStop(g_mhr.pd[k].ent, g_mhr.pd[k].sid, MHR_BLEND_OUT);
        g_mhr.pd[k].end = 0;
    }
    if (!g_mhr.on) {
        if (g_mhr_wp.on)
            MhrWpOwnEnd("the reaction is over");
        return;
    }
    if (g_mhr_wp.on)
        MhrWpHold(&g_mhr_wp, MhrOwnObj());
    if (MhrOff()) {
        MhrEnd("opt-out", 1);
        return;
    }
    const char *busy = MdgBusy();
    if (busy) {
        MhrEnd(busy, 1);
        return;
    }
    // (a reaction pin and a dodge pin both wrote our body for 22 frames - 'hitreact.pin + dodge.pin: 55 frames' in the motion table - SNAP 0.24-0.97 m,
    // YAW SNAP +-46-49, the snap-cross check teleporting back every other frame): a running dodge pin owns the body, the reaction's absolute path stops writing it (the shove already did).
    // t2r-react-dodge-old.txt = both write.
    if (g_md_fb.on && g_md_fb.pin) {
        static int old_ = -1;
        static uint64_t oc_;
        if (old_ < 0 || GetTickCount64() - oc_ > 3000) {
            oc_ = GetTickCount64();
            old_ =
                GetFileAttributesA("t2r-react-dodge-old.txt") !=
                INVALID_FILE_ATTRIBUTES;
        }
        if (!old_) {
            static int nl_;
            if (LOGCAP(nl_, 30))
                Xlog("hitreact: reaction pin ended - the dodge pin owns the body (one writer per body)");
            MhrEnd("a dodge owns the body", 1);
            return;
        }
    }
    float lb[16];
    const float *l = Uc4MyLoc(lb);
    if (!l || !__builtin_isfinite(l[0])) {
        MhrStop(SID_PLAYER, g_mhr.sid, g_mhr.blend_out);
        g_mhr.on = 0;
        return;
    }                           // (was left playing unpinned)
    if (g_mhr.stage == 0) {
        if (g_frame <= g_mhr.arm_f) {
            goto pin;
        }                       // (rays resolve next frame)
        float total =
            g_mhr.total < g_mhr.cap ? g_mhr.total : g_mhr.cap, cap = total;
        float target = g_mhr.push > total ? g_mhr.push : total, far_ =
            target + 0.4f, room_w = target, tw[2] = { -2.0f, -2.0f };
        int rok = 1;
        for (int k = 0; k < 2; k++) {
            uint64_t jb = g_mhr.jw[k];
            g_rayErr = 0;
            float r = RayT(jb);
            g_mhr.jw[k] = 0;
            tw[k] = r;
            if (!jb || g_rayErr) {
                rok = 0;
                cap = 0.0f;
                room_w = 0.0f;
            }                   // (a probe that never ran is not open space, as in the dodge)
            else if (r >= 0.0f && r < 0.99f) {
                float room = r * far_ - 0.4f;
                if (room < cap)
                    cap = room;
                if (room < room_w)
                    room_w = room;
            }
        }
        float rg = RayT(g_mhr.jg);
        g_mhr.jg = 0;
        int gok = !(total > 0.02f
                    && (rg < 0.0f || rg > 0.99f || rg < 0.2f
                        || rg > 0.62f));
        if (!gok)
            cap = 0.0f;
        if (cap < 0.0f)
            cap = 0.0f;
        float rg2 = g_mhr.jg2 ? RayT(g_mhr.jg2) : -2.0f;
        g_mhr.jg2 = 0;
        float ext = 0.0f;
        if (g_mhr.push > 0.0f && rok && gok && rg2 >= 0.3f && rg2 <= 0.62f
            && room_w > cap + 0.05f)
            ext = (room_w < target ? room_w : target) - cap;
        g_mhr.push_ext = ext > 0.05f ? ext : 0.0f;
        g_mhr.cap = cap;
        g_mhr.stage = 1;
        if (g_mhx.full) {       // ps25: one path for the whole reaction = the clip's root curve up to the probed room; no additive push-back after it
            g_mhr.cap = cap + g_mhr.push_ext;
            g_mhr.push_ext = 0.0f;
            static int nr;
            if (LOGCAP(nr, 80))
                Xlog("hitreact: ps25 knock-back room %.2f m along the recoil (the clip's own root path is followed up to there: %.2f m by the control return, %.2f m by the exit)", (double) g_mhr.cap, (double) total, (double) target);
        }
        Xlog("hitreact: recoil path checked - pinned %.2f m of %.2f m planned%s; SP push-back after the cut +%.2f m (target %.2f m total; wall t %.2f / %.2f, ground %.2f / %.2f)", (double) cap, (double) total, cap + 0.001f < total ? " (wall / edge / step ahead)" : "", (double) g_mhr.push_ext, (double) g_mhr.push, (double) tw[0], (double) tw[1], (double) rg, (double) rg2);
        {                       // ps48 kick into a wall: the wall reaction (see MHR48_WALL)
            const char *pm48 =
                g_mhb44.slot <
                T2R_MAX_PEERS ? MduPlainMove(g_mhb44.slot) : NULL;
            int wall48 = (tw[0] >= 0.0f && tw[0] < 0.99f) || (tw[1] >= 0.0f
                                                              && tw[1] <
                                                              0.99f);
            if (pm48 && strstr(pm48, "kick") && !Mhr48Off() && wall48
                && gok && g_mhr.cap < MHR48_ROOM_M
                && total > g_mhr.cap + 0.15f) {
                const char *wn = NULL;
                uint64_t ws = Mhr48WallSid(&wn);
                if (!ws) {
                    static int nw;
                    if (LOGCAP(nw, 20))
                        Xlog("hitreact: ps48 wall reaction - %s hit us %.2f m from a wall, no wall clip loaded in MP: the normal reaction stays", pm48, (double) g_mhr.cap);
                } else if (MhrAnimate(SID_PLAYER, ws, 0.0f, 0.08f) !=
                           ~0ULL) {
                    g_mhr.sid = ws;
                    g_mhr.last_sid = ws;
                    uint8_t wb[11] = { 1 };
                    memcpy(wb + 1, &ws, 8);
                    uint16_t wc =
                        (uint16_t) (g_mhr.cut_ms <
                                    150 ? 150 : g_mhr.cut_ms >
                                    600 ? 600 : g_mhr.cut_ms);
                    memcpy(wb + 9, &wc, 2);
                    if (g_sendTramp)
                        g_sendTramp(MSG_HITREACT2, wb, 11, 0xff);
                    uint64_t sa[5] =
                        { FNV64("wall-impact-body-med"), SID_PLAYER,
               FNV64("spinea"), 0, 0 };
                    uint64_t sr =
                        DcCall(FNV64("play-sound-at-joint"), 5, sa);
                    Xlog("hitreact: ps48 wall reaction - %s from slot %u, the wall %.2f m along the recoil (%.2f m planned): %s (%.2f s) replaces the reaction, sent to the other screens; wall-impact-body-med -> %llx", pm48, g_mhb44.slot, (double) g_mhr.cap, (double) total, wn, (double) MdgClipLen(ws), (unsigned long long) sr);
                }
            }
        }
    }
  pin:;
    uint64_t el = (uint64_t) (PsyncMs() - g_mhr.t0q);  // QPC: GetTickCount64's 15.6 ms steps moved the pinned body 0 / 2 frames' worth in turn (SNAP 0.25 m)
    // path: the clip's own curve projected on the recoil direction, scaled so the cut lands on the capped distance; pinned every frame (the clip's root motion must not carry the body)
    float ox, oz;
    mhr_rm_at(g_mhr.rm,
              (float) (el > MhrPathMs()? MhrPathMs() : el) / 1000.0f,
              &ox, &oz);
    float wx = oz * g_mhr.fwd[0] + ox * g_mhr.left[0], wz =
        oz * g_mhr.fwd[1] + ox * g_mhr.left[1];
    float prog =
        g_mhr.dist_cut >
        0.02f ? (wx * g_mhr.u[0] +
                 wz * g_mhr.u[1]) / g_mhr.dist_cut : 0.0f;
    static float pmax;
    if (el < 60)
        pmax = 0.0f;
    if (prog > pmax)
        pmax = prog;
    if (pmax > 1.0f && !g_mhx.full)
        pmax = 1.0f;
    float dist = !g_mhr.stage ? 0.0f : g_mhx.full ? fminf(g_mhr.dist_cut * pmax, g_mhr.cap) : g_mhr.cap * pmax; // ps25: the clip's own metres (not scaled onto a cap), limited by the probed room
    if (g_mhr.lt && now > g_mhr.lt) {
        float v =
            (dist - g_mhr.ldist) / ((float) (now - g_mhr.lt) / 1000.0f);
        g_mhr.vend = 0.5f * g_mhr.vend + 0.5f * v;
    }
    g_mhr.ldist = dist;
    g_mhr.lt = now;
    float l8[8] =
        { g_mhr.from[0] + g_mhr.u[0] * dist, l[1],
  g_mhr.from[2] + g_mhr.u[1] * dist, 1.0f, l[4], l[5], l[6], l[7] };
    if (g_mhx.turn == 3) {      // ps44 T9: the turn after a back reaction follows the attacker where it is now
        uint64_t ah4 = PuppetHandle(g_mhb44.slot);
        float ab4[16];
        const float *al4 = ah4 ? PuppetLocator(ah4, ab4) : NULL;
        if (al4 && __builtin_isfinite(al4[0])) {
            float dx4 = al4[0] - l[0], dz4 = al4[2] - l[2];
            if (dx4 * dx4 + dz4 * dz4 > 0.04f)
                g_mhx.yaw_t = __builtin_atan2f(dx4, dz4);
        }
        g_mhb44.f++;
    }
    if (g_mhx.turn) {           // ps29 (b): the victim faces the attacker - the yaw goes through this same write, <= MHR_TURN_DEG_F a frame, then held to the exit point
        float dy = g_mhx.yaw_t - g_mhx.yaw_c, cap =
            (g_mhx.turn ==
             3 ? MHR_BACK44_TURN_F : MHR_TURN_DEG_F) * 0.01745329f;
        while (dy > 3.14159265f)
            dy -= 6.2831853f;
        while (dy < -3.14159265f)
            dy += 6.2831853f;
        int last = dy <= cap && dy >= -cap;
        g_mhx.yaw_c += dy > cap ? cap : dy < -cap ? -cap : dy;
        if (g_mhx.turn == 1)
            g_mhx.turn_f++;
        l8[4] = 0.0f;
        l8[5] = __builtin_sinf(g_mhx.yaw_c * 0.5f);
        l8[6] = 0.0f;
        l8[7] = __builtin_cosf(g_mhx.yaw_c * 0.5f);
        if (last && g_mhx.turn == 1) {
            float tot = (g_mhx.yaw_t - g_mhx.yaw_0) * 57.29578f;
            while (tot > 180.0f)
                tot -= 360.0f;
            while (tot < -180.0f)
                tot += 360.0f;
            g_mhx.turn = 2;
            static int nt;
            if (LOGCAP(nt, 80))
                Xlog("hitreact: ps29 turn - our body turned %+.0f deg to face the attacker in %d frame(s) (<= %.0f deg a frame, written by hitreact.pin); held to the exit point", (double) tot, g_mhx.turn_f, (double) MHR_TURN_DEG_F);
        }
        if (last && g_mhx.turn == 3) {
            float tot = (g_mhx.yaw_t - g_mhx.yaw_0) * 57.29578f;
            while (tot > 180.0f)
                tot -= 360.0f;
            while (tot < -180.0f)
                tot += 360.0f;
            static int nt4;
            if (LOGCAP(nt4, 80))
                Xlog("hitreact: ps44 back reaction over - our body turned %+.0f deg to face slot %u in %d frame(s) after the control return (<= %.1f deg a frame, hitreact.pin; no snap)", (double) tot, g_mhb44.slot, g_mhb44.f, (double) MHR_BACK44_TURN_F);
            g_mhx.turn = 0;
            g_mhb44.on = 0;
        }
    }
    static PsyncBf bf;
    PsyncBfFromLoc(&bf, l8);
    mo_write(SID_PLAYER, &bf, MO_SRC("hitreact.pin"), 0);
    {
        float sx, sy;
        MdgStickDir(&sx, &sy);
        int st = sx * sx + sy * sy >= 0.3f * 0.3f;
        if (st)
            g_mhr.blend_out = MHR_BLEND_OUT_STICK;
        g_mhr.stick_prev = st;
    }
    if (g_mhx.full) {           // ps25: the exit point gives the controls back; the clip is left only by the next blow (mhr_victim_start), our input, or its exit
        if (el >= g_mhr.cut_ms && !g_mhx.ctl_back) {
            g_mhx.ctl_back = 1;
            g_mhx.turn = 0;     // (ps29: the facing is the player's again)
            if (g_mhb44.on && !MhrBack44Off()) {      // ps44 T9: the back reaction is over - now the turn to the attacker
                g_mhx.yaw_c = g_mhx.yaw_0 =
                    __builtin_atan2f(2 * (l[4] * l[6] + l[7] * l[5]),
                                     1 - 2 * (l[4] * l[4] + l[5] * l[5]));
                g_mhx.yaw_t = g_mhx.yaw_c;
                g_mhx.turn = 3;
                g_mhb44.f = 0;
                static int ns4;
                if (LOGCAP(ns4, 80))
                    Xlog("hitreact: ps44 back reaction - control back at %llu ms: our body turns to face slot %u now (blow came from %+.0f deg)", (unsigned long long) el, g_mhb44.slot, (double) g_mhb44.ang);
            }
            static int nb;
            if (LOGCAP(nb, 80))
                Xlog("hitreact: control back after %llu ms (the exit point) - the reaction clip plays on from its own pose (next blow / our input / its exit at %llu ms), %.2f m travelled so far", (unsigned long long) el, (unsigned long long) g_mhx.exit_ms, (double) dist);
        }
        if (g_mhx.ctl_back) {
            const char *why = el >= g_mhx.exit_ms && !(g_mhx.turn == 3
                                                       && g_mhb44.f <
                                                       40) ?
                "the clip's exit" : g_mhr.
                stick_prev ? "left at the exit point: stick held" :
                g_msAtk ? "left at the exit point: our swing" : (g_mdg.
                                                                  cmb_on
                                                                  ||
                                                                  g_md_fb.
                                                                  on) ?
                "left at the exit point: our dodge" : PadCmdOn("aim") ?
                "left at the exit point: aim" : NULL;
            if (why)
                MhrEnd(why, 1);
        }
    } else if (el >= g_mhr.cut_ms) {
        if (g_mhr.push_ext > 0.05f) {   // SP push-back: the rest of the carry, additive and with the controls already back
            g_msh.on = 1;
            g_msh.t0 = now;
            g_msh.lt = now;
            g_msh.t0q = g_msh.ltq = PsyncMs();
            g_msh.jf = 0;
            g_msh.job = 0;
            g_msh.done = 0.0f;
            g_msh.y0 = l[1];
            g_msh.d = g_mhr.push_ext;
            g_msh.u[0] = g_mhr.u[0];
            g_msh.u[1] = g_mhr.u[1];
            float ve = g_mhr.vend > 0.3f ? g_mhr.vend : 0.0f;
            g_msh.T = ve > 0.0f ? 3.0f * g_msh.d / ve : 0.45f;
            if (g_msh.T < MHR_SHOVE_MIN_S)
                g_msh.T = MHR_SHOVE_MIN_S;
            if (g_msh.T > MHR_SHOVE_MAX_S)
                g_msh.T = MHR_SHOVE_MAX_S;
            g_msh.v0 = 3.0f * g_msh.d / g_msh.T;
            Xlog("hitreact: SP push-back starts - +%.2f m over %.2f s, v0 %.2f m/s (pinned part ended at %.2f m/s), direction %.2f %.2f", (double) g_msh.d, (double) g_msh.T, (double) g_msh.v0, (double) ve, (double) g_msh.u[0], (double) g_msh.u[1]);
        }
        MhrEnd(g_mhr.
                stick_prev ? "impact part over, stick held" :
                "impact part over", 1);
    }
}

// ---- everybody else: MSG_HITREACT2
static void MhrPuppetPlayAge(uint32_t slot, uint64_t sid,
                                uint32_t cut_ms, float age);
static void MhrPuppetPlay(uint32_t slot, uint64_t sid, uint32_t cut_ms)
{
    MhrPuppetPlayAge(slot, sid, cut_ms,
                        g_psRtt[slot] >
                        0.0f ? g_psRtt[slot] * 0.5f : 40.0f);
}

static int DtlPupRoute(uint32_t slot, int src, uint64_t sid, uint32_t cut, float age);        // melee_duel_tl.c: the duel partner's puppet clips (1 = handled there)
static int DtlPartner(uint32_t slot);
static void MhrPuppetPlayAge(uint32_t slot, uint64_t sid,
                                uint32_t cut_ms, float age)
{
    if (DtlPupRoute(slot, 2 /* DTL_S_REACT */ , sid, cut_ms, age))
        return;                 // duel timeline: queued / crossfaded, phase 0 + catch-up (no trimmed start)
    if (g_peerDowned[slot] || g_puppetDeadHere[slot]
        || PsyncBusySlot(slot) || HrxBusyPeer(slot))
        return;
    uint64_t ent = PuppetHandle(slot);
    if (!ent)
        return;
    float dur = MdgClipLen(sid);
    if (!(dur > 0.2f)) {
        static int nl;
        if (LOGCAP(nl, 10))
            Xlog("hitreact: puppet slot %u: reaction clip %llx not loaded here - the streamed pose only", slot, (unsigned long long) sid);
        return;
    }
    float ph = age / 1000.0f / dur;
    if (ph > 0.5f)
        ph = 0.5f;
    uint64_t r = MhrAnimate(ent, sid, ph, MHR_BLEND_IN);
    uint64_t now = GetTickCount64(), kx = MhrExitMsOf(sid, cut_ms), keep = kx > (uint64_t) age ? kx - (uint64_t) age : 60;   // ps25: the clip stays to its exit (the victim's 0x80 notice ends it earlier)
    g_mhr.pd[slot].ent = ent;
    g_mhr.pd[slot].sid = sid;
    g_mhr.pd[slot].end = now + keep;
    MhrWpPupBegin(slot, ent, keep);
    // (A's swing was cut by our blow, yet B's dodge window for it opened 500 ms later and the bot hopped 0.62 m at
    // nothing): a puppet that plays a hit reaction has no swing coming any more - its pending swing window is closed (landed).
    if (slot < T2R_MAX_PEERS && g_mdg.sw[slot].ms)
        g_mdg.sw[slot].landed_ms = GetTickCount64();
    static int nl2;
    if (LOGCAP(nl2, 60))
        Xlog("hitreact: puppet slot %u plays reaction clip %llx at phase %.2f (age %.0f ms), cut in %llu ms -> %llx", slot, (unsigned long long) sid, (double) ph, (double) age, (unsigned long long) keep, (unsigned long long) r);
}

static void MhrRecv(uint32_t slot, const uint8_t *buf, uint64_t len)
{
    if (slot >= T2R_MAX_PEERS || slot == *(uint32_t *) T2R_SYM(kMySlot)
        || !buf || len < 11 || MhrOff() || MatchSubstate() != 6)
        return;
    if (Mv3Stand
        ("hitreact: peer's MSG_HITREACT2 (puppet reaction + swing lock)",
         slot))
        return;                 // melee v3: the reaction is the sim's
    uint64_t sid;
    uint16_t c16;
    memcpy(&sid, buf + 1, 8);
    memcpy(&c16, buf + 9, 2);
    uint64_t now = GetTickCount64();
    int react = (buf[0] & 1) && sid;
    if ((buf[0] & 0x80) && !react) {    // ps25: the victim left its reaction (exit point / its input) - the clip we keep on its puppet blends out now
        if (sid && g_mhr.pd[slot].end && g_mhr.pd[slot].sid == sid) {
            MhrStop(g_mhr.pd[slot].ent, sid, MHR_BLEND_OUT);
            g_mhr.pd[slot].end = 0;
            DtlReactOver(slot, sid);
            static int nx;
            if (LOGCAP(nx, 80))
                Xlog("hitreact: slot %u left its reaction - clip %llx on its puppet blends out over %.2f s", slot, (unsigned long long) sid, (double) MHR_BLEND_OUT);
        }
        if (g_mhr_wpp[slot].on)
            g_mhr_wpp[slot].until = now;
        return;
    }
    if (react
        && ((!mhr_rm_find(sid) && !MhrNpcSid(sid) && !Mhr48IsWall(sid))
            || c16 < 150 || c16 > 600))
        return;                 // (ps29: SP's npc-1h-hr clips are ours too)           // not one of ours / out of range
    if (react) {
        int mc = g_mhr.atk.ms
            && g_mhr.atk.slot == slot ? MeleeClassOf(DcWeapon(),
                                                       NULL) : -1;
        BbMeleeNote((int) slot, mc == MC_1HBLADE || mc == MC_MACHETE);
    }                           // bloodbuild: head masks for a blunt blow
    // the attacker: this is the victim of our confirmed blow - hold the next swing start until its impact part is over (or release the provisional lock)
    if (g_mhr.atk.ms && g_mhr.atk.slot == slot && now - g_mhr.atk.ms < 900) {
        float age = g_psRtt[slot] > 0.0f ? g_psRtt[slot] * 0.5f : 40.0f;
        uint64_t until = react ? g_mhr.atk.ms + (uint64_t) age + c16 : now;
        if (until > g_mhr.atk.ms + MHR_ATK_MAX_MS)
            until = g_mhr.atk.ms + MHR_ATK_MAX_MS;
        MpaceNoteVictim(until > now ? until : 0);
        static int nl;
        if (LOGCAP(nl, 80))
            Xlog("hitreact: victim slot %u %s - our next swing start %s (%lld ms from now)", slot, react ? "reacts" : "does not react", react ? "waits for its impact part" : "is free", (long long) (until > now ? until - now : 0));
        g_mhr.atk.ms = 0;
    }
    // Punch sounds are disabled when a dodge is detected (they still played if the fist contacted the body despite the dodge). winmm's local fist hit
    // list is silent (melee_hit_sfx_swap, g_hit_sfx_fist_net); our punch plays here when the victim confirms the blow landed (reaction / bit1), at its head
    if ((react || (buf[0] & 2)) && g_hitSfxFistNet && g_mhr.snd.ms
        && g_mhr.snd.slot == slot && now - g_mhr.snd.ms < 900) {
        uint64_t ent = PuppetHandle(slot);
        // SP Abby fist capture: light / medium / sprint punches all play sfx-unarmed-hit-face-skin at the victim's heada (no -head-skin in SP fist fights),
        // kills sfx-unarmed-finisher-face-skin
        static const char *const PN[3] =
            { "sfx-unarmed-hit-face-skin", "sfx-unarmed-hit-face-skin",
"sfx-unarmed-finisher-face-skin" };
        const char *pn =
            PN[g_mhr.snd.str < 0 ? 0 : g_mhr.snd.str >
               2 ? 2 : g_mhr.snd.str];
        uint64_t a[5] = { FNV64(pn), ent, FNV64("heada"), 0, 0 };
        uint64_t r = ent ? DcCall(FNV64("play-sound-at-joint"), 5, a) : 0;
        static int ns;
        if (LOGCAP(ns, 60))
            Xlog("hitreact: slot %u confirmed our fist blow - %s at its head %lld ms after the contact -> %llx", slot, pn, (long long) (now - g_mhr.snd.ms), (unsigned long long) r);
        g_mhr.snd.ms = 0;
    } else if ((react || (buf[0] & 2)) && (buf[0] & 0x40)
               && g_hitSfxFistNet && !MhrPunchOld()
               && !(g_mhr.snd.slot == slot
                    && now - g_mhrAtkPunchMs < 900))
        MhrPunchAt(PuppetHandle(slot), react ? 1 : 0,
                     "a puppet (bystander)");
    if (react || (buf[0] & 2))
        BfxConfirm(slot);      // our deferred blade / blunt cut (t2r-ext.c bfx_defer)
    // bystander (a blade / blunt hit between two other players made no impact sound or blood on our screen at all): the swinger = the enemy of the
    // victim whose announced swing (MSG_DODGE_SWING, broadcast) is freshest within 900 ms and 2.5 m; its weapon's cut plays on the victim's puppet
    if ((react || (buf[0] & 2)) && !(buf[0] & 0x40)
        && !(g_mhr.atk.slot == slot && now - g_mhrAtkPunchMs < 900)
        && !(g_mhr.pr[slot].last && now - g_mhr.pr[slot].last < 900)) {
        uint64_t vh = PuppetHandle(slot);
        float vb[16];
        const float *vl = vh ? PuppetLocator(vh, vb) : NULL;
        int bs = -1;
        uint64_t bt = 0;
        for (uint32_t q = 0; vl && q < T2R_MAX_PEERS; q++) {
            if (q == slot || !g_mdg.sw[q].ms || now - g_mdg.sw[q].ms > 900
                || g_mdg.sw[q].ms <= bt)
                continue;
            uint64_t qh = PuppetHandle(q);
            float qb[16];
            const float *ql = qh ? PuppetLocator(qh, qb) : NULL;
            if (!ql
                || (ql[0] - vl[0]) * (ql[0] - vl[0]) + (ql[2] -
                                                        vl[2]) * (ql[2] -
                                                                  vl[2]) >
                2.5f * 2.5f)
                continue;
            bs = (int) q;
            bt = g_mdg.sw[q].ms;
        }
        uint8_t *wpl = bs >= 0 ? T2rPeer((uint32_t) bs) : NULL;
        uint64_t bw = wpl
            && !IsBadReadPtr(wpl, 0x99) ? *(uint64_t *) (wpl + 0x91) : 0;
        if (bw && MeleeClassOf(bw, NULL) >= 0) {
            MeleeHitFx(vh, bw);
            static int nbx;
            if (LOGCAP(nbx, 30))
                Xlog("hitreact: bystander - slot %d's melee blow landed on slot %u, its cut plays on the victim's puppet", bs, slot);
        }
    }
    // client-side prediction (mhr_pred_tick): the reaction already plays on the puppet since our contact - the victim's word only confirms or undoes it
    g_mhr.pr[slot].due = 0;     // (not played yet: the real message wins)
    if (g_mhr.pr[slot].played && now - g_mhr.pr[slot].played < 900) {
        uint64_t pl = g_mhr.pr[slot].played;
        int pain = g_mhr.pr[slot].pain;
        g_mhr.pr[slot].played = 0;
        static int np;
        if (react) {
            if (g_mhr.pd[slot].end
                && g_mhr.pd[slot].sid == g_mhr.pr[slot].sid)
                g_mhr.pd[slot].end = pl + MhrExitMsOf(sid, c16);
            if ((buf[0] & 4) && !pain)
                MhrPainSay(slot, PuppetHandle(slot), (buf[0] >> 3) & 3,
                             (buf[0] & 0x20) != 0);
            if (LOGCAP(np, 60))
                Xlog("hitreact: slot %u confirmed the predicted reaction %lld ms after it started on its puppet", slot, (long long) (now - pl));
        } else {
            if (g_mhr.pd[slot].end
                && g_mhr.pd[slot].sid == g_mhr.pr[slot].sid) {
                MhrStop(g_mhr.pd[slot].ent, g_mhr.pd[slot].sid, 0.15f);
                g_mhr.pd[slot].end = 0;
            }
            // landed but no clip on the victim (mid swing / crouched / grazing / dodging) - its body did not react, ours must not either (the grunt stays when it landed)
            if ((buf[0] & 4) && !pain)
                MhrPainSay(slot, PuppetHandle(slot), (buf[0] >> 3) & 3,
                             (buf[0] & 0x20) != 0);
            if (LOGCAP(np, 60))
                Xlog("hitreact: slot %u %s - predicted reaction blended out after %lld ms", slot, (buf[0] & 2) ? "took the blow without a reaction clip" : "says our blow did not land", (long long) (now - pl));
        }
        return;
    }
    if (buf[0] & 4)
        MhrPainSay(slot, PuppetHandle(slot), (buf[0] >> 3) & 3,
                     (buf[0] & 0x20) != 0);
    if (react)
        MhrPuppetPlay(slot, sid, c16);
}

// ---- client-side prediction, attacker side (melee sequences made more client-side, visually and audibly; live log: our contact 36619,
// the victim's reaction on its puppet 36641 = ~230 ms of a puppet standing still after the fist connected, and the blade / blunt cut waited the same round trip).
// At our contact the victim's reaction clip (the pick the victim makes: where we stand around it, the blow's strength), its pain grunt and our blade / blunt cut play
// MHR_PRED_GRACE_MS later on our screen - unless its dodge is known by then. The victim's MSG_HITREACT2 confirms (cut timing) or undoes (blend out) it.
// t2r-no-melee-predict.txt = wait for the victim again.
#define MHR_PRED_GRACE_MS 50
static int MhrPredOff(void)
{
    static int off = -1;
    static uint64_t at;
    return FeelFlag("t2r-no-melee-predict.txt", &at, &off, 180);
}

static void MhrPredArm(uint32_t slot, uint64_t w, float dmg)
{
    if (MhrPredOff() || slot >= T2R_MAX_PEERS || MduOwnsReact(slot))
        return;                 // the duel's pair / lethal punch covers it
    if (DtlPartner(slot))
        return;                 // duel timeline: only CONFIRMED reactions play on the duel partner (no speculative clip that the victim's word may replace)
    if (g_mhr.pr[slot].last
        && GetTickCount64() - g_mhr.pr[slot].last < MHR_GRAZE_MS)
        return;                 // the victim's 1 s grazing window: no clip there either
    uint64_t ah = PuppetHandle(slot);
    float vb[16], ab[16];
    const float *vl = ah ? PuppetLocator(ah, vb) : NULL, *al =
        Uc4MyLoc(ab);
    if (!vl || !al || !__builtin_isfinite(vl[0])
        || !__builtin_isfinite(al[0]))
        return;
    int dir = DirAround(vl, al), two = 0, str =
        MhrStrength(w, dmg, &two), used = str, k = -1;
    if (dir == 0) {             // the victim's own front pick: fists -> head-front-c (SP 7 of 8), weapons -> the head-front set
        static const char *const HF[] =
            { "head-front-a", "head-front-b", "head-front-c",
"head-front-d" };
        int i0 = MhrFistW(w) ? 2 : (int) (HrxRnd() % 4u);
        for (int q = 0; q < MHR_POOL_N && k < 0; q++)
            if (!strcmp(MHR_POOL[q].n, HF[i0])
                && MdgClipLen(MhrSid(q)) > 0.2f)
                k = q;
        if (k >= 0)
            used = str < 1 ? 1 : str;
    }
    if (k < 0)
        k = MhrPick(T2R_MAX_PEERS, dir, str, two, &used);
    if (k < 0)
        return;
    char nm[96];
    if (k >= 100)
        snprintf(nm, sizeof nm, "%s", MHR_2H[k - 100]);
    else
        snprintf(nm, sizeof nm, "ellie-1h-hr-%s", MHR_POOL[k].n);
    g_mhr.pr[slot].sid = Fnv64Rt(nm);
    g_mhr.pr[slot].cut = MHR_CUT_MS[used];
    g_mhr.pr[slot].str = used;
    g_mhr.pr[slot].sharp = MhfxClass(w, NULL, NULL) == 2;
    g_mhr.pr[slot].played = 0;
    g_mhr.pr[slot].due = GetTickCount64() + MHR_PRED_GRACE_MS;
}

static void MhrPredTick(uint64_t now)
{
    for (uint32_t k = 0; k < T2R_MAX_PEERS; k++) {
        if (g_mhr.pr[k].punch_due && now >= g_mhr.pr[k].punch_due) {
            g_mhr.pr[k].punch_due = 0;
            if (!MdgSlotImmune(k))
                MhrPunchAt(PuppetHandle(k), g_mhr.pr[k].punch_str,
                             "the victim (our contact, predicted)");
        }
        if (!g_mhr.pr[k].due || now < g_mhr.pr[k].due)
            continue;
        g_mhr.pr[k].due = 0;
        const char *skip = MdgSlotImmune(k) || (g_mdgPupAnimMs[k]
                                                  && now -
                                                  g_mdgPupAnimMs[k] <
                                                  700) ?
            "its dodge is known / its puppet is dodging" : g_peerDowned[k]
            || g_puppetDeadHere[k] ? "down / dead" : PsyncBusySlot(k) ?
            "a paired move" : HrxBusyPeer(k) ? "busy" : NULL;
        static int nl;
        if (skip) {
            if (LOGCAP(nl, 60))
                Xlog("hitreact: predicted reaction on slot %u not played - %s", k, skip);
            continue;
        }
        MhrPuppetPlayAge(k, g_mhr.pr[k].sid, g_mhr.pr[k].cut, 0.0f);
        g_mhr.pr[k].played = g_mhr.pr[k].last = now ? now : 1;
        g_mhr.pr[k].pain = !MhrPainOff();
        if (g_mhr.pr[k].pain)
            MhrPainSay(k, PuppetHandle(k), g_mhr.pr[k].str,
                         g_mhr.pr[k].sharp);
        if (LOGCAP(nl, 60))
            Xlog("hitreact: PREDICTED reaction %llx on slot %u's puppet %d ms after our contact (strength %s, pain grunt %d) - the victim's message confirms / undoes it", (unsigned long long) g_mhr.pr[k].sid, k, MHR_PRED_GRACE_MS, MHR_STRN[g_mhr.pr[k].str], g_mhr.pr[k].pain);
    }
    // our blade / blunt cut (t2r-ext.c bfx_defer): played after the same grace unless the dodge is known - the victim's confirmation no longer gates it
    if (g_bfxPend.ms && !MhrPredOff()
        && now - g_bfxPend.ms >= MHR_PRED_GRACE_MS) {
        uint32_t sl = g_bfxPend.slot;
        if (!MdgSlotImmune(sl) && PuppetHandle(sl) == g_bfxPend.ent) {
            if (g_bfxPend.knife)
                KnifeHitNative(g_bfxPend.ent, g_bfxPend.lethal);
            else
                MeleeHitFx(g_bfxPend.ent, g_bfxPend.w);
            static int nb;
            if (LOGCAP(nb, 40))
                Xlog("blade fx: our %s blow on slot %u - cut PREDICTED %llu ms after the contact", g_bfxPend.knife ? "knife" : "melee", sl, (unsigned long long) (now - g_bfxPend.ms));
        }
        g_bfxPend.ms = 0;
    }
}

// ---- the attacker glides WITH the victim (on a successful hit the attacker glides WITH the movement of the victim, so if they are walking
// away the punch still visually connects on the face). Our own body, from our swing's start until the contact (MHG_PRE_MS): every frame it moves by the target's
// own motion (its locator delta), and a gap over MHG_REACH closes at up to MHG_CLOSE m/s (SP's melee magnetism). After a confirmed blow it keeps following the
// victim's recoil for MHG_POST_MS, fading out, so the fist stays on the face while the reaction plays. Not during a dodge of that target, paired moves, downs.
// t2r-no-melee-glide.txt = off.
#define MHG_PRE_MS 650
#define MHG_POST_MS 380
#define MHG_REACH 0.95f
#define MHG_MIN 0.65f
#define MHG_CLOSE 2.6f
#define MHG_STEP_MAX 0.12f
#define MHG_POST_STEP_MAX 0.04f // (2h combo-1 victim thrown 1.5 m): following the recoil at the 0.12 m/frame cap slid the attacker 1.2 m in 10 frames (SNAP x5); ~2.4 m/s now
static struct {
    int slot;
    uint64_t swing_ms;
    float prev[3];
    int prev_ok;
    float moved;
    int logged;
} g_mhg = { -1 };

static int MhgTarget(const float *ml)
{
    float fx = 2.0f * (ml[4] * ml[6] + ml[7] * ml[5]), fz =
        1.0f - 2.0f * (ml[4] * ml[4] + ml[5] * ml[5]), best = 2.2f;
    int bs = -1;
    for (uint32_t s = 0; s < T2R_MAX_PEERS; s++) {
        uint8_t *pe = MdgPeer(s);
        if (!pe || !pe[0x4c] || pe[0x4d] || !*(uint64_t *) (pe + 0x60)
            || IsTeammate(s) || g_peerDowned[s] || g_puppetDeadHere[s])
            continue;
        float pb[16];
        const float *pl = PuppetLocator(*(uint64_t *) (pe + 0x60), pb);
        if (!pl || !__builtin_isfinite(pl[0])
            || __builtin_fabsf(pl[1] - ml[1]) > 0.8f)
            continue;
        float dx = pl[0] - ml[0], dz = pl[2] - ml[2], d =
            __builtin_sqrtf(dx * dx + dz * dz);
        if (d < best && d > 0.05f && (dx * fx + dz * fz) / d > 0.6f) {
            best = d;
            bs = (int) s;
        }
    }
    return bs;
}

static int MduWarpOwns(void); // melee_duel.c
static void MhgTick(void)
{
    static int off = -1;
    static uint64_t at;
    if (FeelFlag("t2r-no-melee-glide.txt", &at, &off, 180))
        return;
    uint64_t now = GetTickCount64();
    float mb[16];
    const float *ml = Uc4MyLoc(mb);
    int pre = g_msAtk && g_mdg.my_swing_ms
        && now - g_mdg.my_swing_ms < MHG_PRE_MS;
    if (pre && MduWarpOwns())
        pre = 0;                // the duel warp places us until the contact
    int post = g_mhr.atk.ms && now - g_mhr.atk.ms < MHG_POST_MS;
    // ('glide with slot 1 (our swing to the contact) ... we moved 1.43 m with it', [17443]-[17464] 1.10 m, both AFTER the duel's
    // contact - the swing was 485 ms long, MHG_PRE_MS is 650 - while the duel's plain-blow rule held the same body in place; 0.81 m in another recording): inside a duel
    // the duel places our body (the warp to the contact, then the -hit pair or the plain blow's in-place rule) - no glide. t2r-duel-glide-ps22.txt = the glide runs.
    {
        static int dg = -1;
        static uint64_t dgat;
        if (dg < 0 || now - dgat > 3000) {
            dgat = now;
            dg = GetFileAttributesA("t2r-duel-glide-ps22.txt") !=
                INVALID_FILE_ATTRIBUTES;
        }
        if (!dg && MduEngaged())
            pre = post = 0;
    }
    if (!ml || !__builtin_isfinite(ml[0]) || (!pre && !post)
        || MatchSubstate() != 6 || PsyncOwn() || MdgBusy()
        || g_mdg.cmb_on) {
        if (g_mhg.slot >= 0 && g_mhg.logged)
            Xlog("hitreact: glide with slot %d over - we moved %.2f m with it", g_mhg.slot, (double) g_mhg.moved);
        g_mhg.slot = -1;
        g_mhg.prev_ok = 0;
        g_mhg.logged = 0;
        return;
    }
    if (pre && g_mhg.swing_ms != g_mdg.my_swing_ms) {
        g_mhg.swing_ms = g_mdg.my_swing_ms;
        g_mhg.slot = MhgTarget(ml);
        g_mhg.prev_ok = 0;
        g_mhg.moved = 0.0f;
        g_mhg.logged = 0;
    }
    if (post && !pre) {
        if (g_mhg.slot != (int) g_mhr.atk.slot) {
            g_mhg.slot = (int) g_mhr.atk.slot;
            g_mhg.prev_ok = 0;
        }
    }
    int s = g_mhg.slot;
    if (s < 0 || s >= T2R_MAX_PEERS || g_peerDowned[s]
        || g_puppetDeadHere[s] || g_mdg.imm_until[s] > now) {
        g_mhg.prev_ok = 0;
        return;
    }
    uint64_t h = PuppetHandle((uint32_t) s);
    float pb[16];
    const float *pl = h ? PuppetLocator(h, pb) : NULL;
    if (!pl || !__builtin_isfinite(pl[0])) {
        g_mhg.prev_ok = 0;
        return;
    }
    float mx = 0.0f, mz = 0.0f;
    if (g_mhg.prev_ok) {
        float dx = pl[0] - g_mhg.prev[0], dz = pl[2] - g_mhg.prev[2];
        float w = post
            && !pre ? 1.0f - (float) (now -
                                      g_mhr.atk.ms) /
            (float) MHG_POST_MS : 1.0f;
        if (dx * dx + dz * dz < 0.5f * 0.5f) {
            mx += dx * w;
            mz += dz * w;
        }                       // (a net snap of the puppet is not followed)
    }
    memcpy(g_mhg.prev, pl, 12);
    g_mhg.prev_ok = 1;
    if (pre) {
        float gx = pl[0] - (ml[0] + mx), gz = pl[2] - (ml[2] + mz), d =
            __builtin_sqrtf(gx * gx + gz * gz);
        if (d > MHG_REACH && d < 2.2f) {
            float st = fminf(MHG_CLOSE / 60.0f, d - MHG_REACH);
            mx += gx / d * st;
            mz += gz / d * st;
        } else if (d < MHG_MIN && d > 0.05f) {
            float st = fminf(0.02f, MHG_MIN - d);
            mx -= gx / d * st;
            mz -= gz / d * st;
        }
    }
    float ml_ = __builtin_sqrtf(mx * mx + mz * mz);
    if (ml_ < 0.0005f)
        return;
    {
        float cap = pre ? MHG_STEP_MAX : MHG_POST_STEP_MAX;
        if (ml_ > cap) {
            mx *= cap / ml_;
            mz *= cap / ml_;
            ml_ = cap;
        }
    }
    float l8[8] =
        { ml[0] + mx, ml[1], ml[2] + mz, 1.0f, ml[4], ml[5], ml[6],
  ml[7] };
    static PsyncBf bf;
    PsyncBfFromLoc(&bf, l8);
    if (g_snapxBlockMs && GetTickCount64() - g_snapxBlockMs < 700
        && GetFileAttributesA("t2r-snapx-block-old.txt") ==
        INVALID_FILE_ATTRIBUTES) {
        static int nlb;
        if (LOGCAP(nlb, 30))
            Xlog("attacker glide: held - snapcross threw our body back off geometry %llu ms ago (no pull through it)", (unsigned long long) (GetTickCount64() - g_snapxBlockMs));
        return;
    }
    mo_write(SID_PLAYER, &bf, MO_SRC("hitreact.attacker_glide"), 0);
    g_mhg.moved += ml_;
    if (!g_mhg.logged) {
        g_mhg.logged = 1;
        Xlog("hitreact: glide with slot %d (%s) - our body follows its motion%s", s, pre ? "our swing to the contact" : "its recoil after our blow", pre ? ", a gap over 0.95 m closes" : "");
    }
}

// ---- the attacker: a blow of ours was confirmed (hitsend_hook / directhit_hook, melee weapon or fists): provisional lock
static void MhrAtkHit(uint32_t slot, uint64_t w, float dmg, int heavy)
{
    if (slot < T2R_MAX_PEERS
        &&
        Mv3Stand
        ("hitreact: attacker confirm + predicted puppet reaction + punch",
         slot))
        return;                 // melee v3: the sim's MV3_E_HIT / REACT show it
    // v5-knife (A [18867] "punch wave 4 ... at the victim (our contact, predicted)" next to "knife hit sfx: sfx-switchblade-hit-face-skin"): our female knife swing hits with
    // the fists' id - no punch on top of the knife's cut (directhit / hitsend play it), the predicted reaction / grunt as a blade's. t2r-fem-knife-fist-react.txt = old.
    if (w == 0x6ac05fba9c921900ULL && g_femEllieOn && !FkrOff())
        w = g_femKnifeCur ? g_femKnifeCur :
            FNV64("knife-survival-mel-player");
    if (dmg > 0.0f && slot < T2R_MAX_PEERS && MhsIsBlow(w)
        && MeleeClassOf(w, NULL) < 0 && !KnifeWeaponId(w)) {
        int t2 = 0;
        g_mhr.snd.slot = slot;
        g_mhr.snd.ms = GetTickCount64();
        g_mhr.snd.w = w;
        g_mhr.snd.str = heavy ? 2 : MhrStrength(w, dmg, &t2);
        if (g_hitSfxFistNet && !MhrPunchOld()
            && !MdgSlotImmune(slot) && !IsTeammate(slot)) {
            // the punch rides the prediction's grace (a dodge press arriving in it turns it into the whiff) - t2r-no-melee-predict.txt: at the contact again
            if (MhrPredOff())
                MhrPunchAt(PuppetHandle(slot), g_mhr.snd.str,
                             "the victim (our contact)");
            else {
                g_mhr.pr[slot].punch_due =
                    GetTickCount64() + MHR_PRED_GRACE_MS;
                g_mhr.pr[slot].punch_str = g_mhr.snd.str;
            }
            g_mhr.snd.ms = 0;
            g_mhrAtkPunchMs = GetTickCount64();
        }
    }
    if (heavy || !(dmg > 0.0f) || slot >= T2R_MAX_PEERS || MhrOff()
        || !MhsIsBlow(w) || IsTeammate(slot) || g_peerDowned[slot]
        || g_puppetDeadHere[slot])
        return;
    if (MatchSubstate() != 6 || PsyncOwn() || MdgBusy())
        return;
    int two = 0, str = MhrStrength(w, dmg, &two);
    uint64_t now = GetTickCount64();
    g_mhr.atk.slot = slot;
    g_mhr.atk.ms = now;
    g_mhr.atk.str = str;
    if (!MdgSlotImmune(slot))
        MhrPredArm(slot, w, dmg);     // client-side: the victim's reaction + grunt on its puppet now, not a round trip later
    MpaceNoteVictim(now +
                      (MHR_CUT_MS[str] <
                       MHR_ATK_MAX_MS ? MHR_CUT_MS[str] : MHR_ATK_MAX_MS));
    static int nl;
    if (LOGCAP(nl, 80))
        Xlog("hitreact: our %s blow on slot %u confirmed - next swing start held %u ms until its reaction's impact part is over (the victim's message corrects it)", MHR_STRN[str], slot, MHR_CUT_MS[str]);
}
