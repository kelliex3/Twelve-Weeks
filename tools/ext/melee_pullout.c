// melee_pullout.c - the PRONE PULL-OUT of the MP melee (in SP NPCs pull the player out of prone cover (like under a truck) into a grapple
// sequence; here that has a VERY short counter-attack timing and also applies to players prone under trucks, only from behind). Included after
// melee_duel_pin.c in t2r-ext.c. Log prefix "pullout:". Opt-out: t2r-pullout-off.txt. Test aid: t2r-pullout-any.txt (no prone check on the victim: the clips play on a standing body).
//  TRIGGER  a STANDING attacker (not crouched / prone / downed / in a pair, a duel, a pin) presses melee within PUL_RANGE of a PRONE, NOT-downed enemy puppet and BEHIND it (the feet side: a
//           PUL_CONE_DEG cone around the victim's backward direction). The press is ours (the swing it also started is cancelled).
//  SEQUENCE intro (attacker-only SP clip, ~1 s, the victim is still free = the TELL; the victim's screen plays it on our puppet) -> DRAG pair (psync, SP's pullout-prone-bw: the victim is
//           pulled out by the ankles) -> the COUNTER window on the victim's screen (PUL_WIN_MS, E / triangle / Y, "ESCAPE!" over the attacker; a press before it = Too early = the counter
//           fails, anti-mash like the pin QTE) -> SUCCESS pair (the victim breaks free and hits back) or FAIL pair (the victim is downed). SP's 143-frame struggle is left out.
//  CLOCK    the window is judged on the VICTIM's screen against its own pair move clock (the START arrived one-way late, so the latency is already subtracted, as for the pin QTE); the verdict
//           goes to the attacker (PUL_OP_RES), which starts the outcome pair at the drag's hand-off (no verdict within PUL_RES_WAIT_MS = fail).
//  NET      MSG_MEX op 5 sub-ops 11 INTRO {set} (attacker -> victim), 12 RES {ok, rel lo, hi} (victim -> attacker), 13 END. Older clients route them into mpn_recv, which ignores unknown ops.
//  PATH     the drag pair goes through pg_gate (wall / floor / drop on its spots) - under a truck the attacker stands at the victim's feet and no level spline exists in MP; blocked = refused.
//  Later additions (prone under a truck: the pull must be detectable, work from all sides, and the attacker also gets a cinematic camera):
//   COVER   pul_cover_tick: per prone enemy puppet within ~4.5 m an async world ray straight UP from 0.15 m over its pelvis to +1.6 m (every ~170 ms, kept with a timestamp); a hit = under cover
//           (truck, car, low roof). t2r-pullout-cover-off.txt = off.
//   FIND    a COVERED victim is accepted from ANY bearing (no feet-side cone) and its range is measured to the NEAREST of root / head / ankles (the root sits deep under the vehicle); an
//           uncovered one keeps the feet-side cone. The TRIGGER line logs the covered flag.
//   SIDES   SP's other drag clips live in the same anim-melee-*-npc-t2 paks as the bw one: prone-l / -r / -fw (+ their -intro--npc) -> rows pu-drag-l/r/fw-<sex> (pullout_rows.h); the
//           outcome pairs (success / fail) are shared by every side. A covered victim tries the rows by the attacker's bearing around the victim's body axis (gx_body_side: fw / sd); the
//           gate refuses a row whose authored attacker spot is far from us (or blocked) and the next row is tried. t2r-pullout-sides-off.txt = the bw row only.
//   CAMERA  where SP's authored shot is blocked / absent (sides have no track) BOTH roles get the procedural pull-out camera (pul_proc_frame): no cut, 2.7 s swing-in, fov 70 -> 73.7,
//           held through the struggle, 1.5 s ease out; composition from SP's victim shot (see pul_proc_*). t2r-pullout-proccam-off.txt = the old pin-camera fallback.
//   pulfix  the VICTIM's camera replays SP's recorded camera 1:1 (pullout_spcam.h, psc_frame; t2r-pullout-vcam-fit.txt = the fitted shot).
//   GRAPPLE (pull out, grapple, then a window to hit F while the victim is already pulled out and grappling with the attacker, and if it fails,
//           death punch): drag -> GRAPPLE pair (pu-grap-<sex>: SP's npc-prone-struggle clips <set>-pullout-struggle--npc / --<set>, started by the attacker the moment the drag
//           hands off, solved on the attacker like the outcome pairs) -> the counter window runs on the GRAPPLE's pair clock (600 ms before its 3.40 s hand-off, 400 ms + 30 ms
//           grace; a press during the drag or the grapple before the window = Too early) -> pu-ok (escape -> supine) or pu-fail + the DEATH PUNCH (the fail blow downs, a
//           lethal 250 follows once the down protection is over - "downed: executed by a hit"). The INTRO's set byte carries bit 3 = grapple flow, so the victim holds its
//           window until the grapple. Victim camera: SP's recorded STRUGGLE track (ep 5 after the feet drag, ep 8 after the left drag); the attacker's camera runs on.
//           t2r-pullout-nograpple.txt (attacker side) = the old drag-window flow.
#define PUL_OP_INTRO 11
#define PUL_OP_RES 12
#define PUL_OP_END 13
#define PUL_OP_CAM 14           // attacker -> victim: the SP camera index its clean ray test chose (0 = none: the pin camera) - both screens play the same world shot
#define PUL_RANGE 2.5f          // within ~2.5 m
#define PUL_COV_SCAN 4.5f       // the cover ray runs for prone enemies this close
#define PUL_COV_MS 170          // ... every this often
#define PUL_COV_KEEP_MS 1500    // a cover result older than this is stale (the victim crawled / the ray was never answered)
#define PUL_GATE_FAR_COV 2.6f   // the authored attacker spot may be this far from a covered victim's attacker (uncovered: 2.2 m)
#define PUL_CONE_DEG 55.0f      // half angle (110 deg cone) around the victim's backward direction
#define PUL_PRONE_HEAD 0.60f    // a puppet's head above its root below this = prone (footsteps.c FS_PRONE_HEAD)
#define PUL_WIN_MS 400          // VERY short counter window (250 ms = one reaction time: a press on seeing ESCAPE! landed after it)
#define PUL_WIN_LEAD_MS 600     // the window opens this long before the drag's hand-off
#define PUL_GRACE_MS 30
#define PUL_RES_WAIT_MS 220     // the attacker waits this long past the drag's hand-off for the victim's verdict
#define PUL_COOL_FAIL_MS 3000
#define PUL_COOL_OK_MS 8000     // (like MPN_ESC_COOL_MS) a victim that countered can't be pulled again for this long
#define PUL_LATE_MS 1200
#define PUL_INTRO_LEAD_S 0.10f  // pulfix: the drag starts this long before the intro clip ends (its blend-in covers the intro's last frames, never the base pose)
#define PULLOG(...) do { if (LOGCAP(g_pul.nlog, 700)) Xlog("pullout: " __VA_ARGS__); } while (0)
#define PUS_SWING_S 2.7f        // SP: the victim camera swings round over ~2.7 s (yaw 28 -> 85 deg), fov 70 -> 73.7
enum { PUL_IDLE, PUL_GATE, PUL_INTRO, PUL_DRAG, PUL_WAITRES, PUL_OUT, PUL_GRAP, PUL_KILL };     // PUL_GRAP: the grapple pair runs (after the drag, before the outcome); PUL_KILL: the prone-kill pair (variants a / b)
static struct {
    int st, slot, id, set, nlog, verdict, n_started, n_ok, n_fail, n_refused, row[3];   // row[]: drag / ok / fail CD_PAIRS rows of the picked set
    int dir, ndir, gi, cov, farspot, dirs[4];   // drag direction 0 bw / 1 l / 2 r / 3 fw; the dirs to try in order (gi = current); cov = covered victim; far = the last gate refusal was the spot distance
    uint64_t intro_end, drag_t0, drag_end, out_t0, out_end, res_ms, uid,
        gate_t0, cool[T2R_MAX_PEERS];
    float vpos[3], hit_s;
    int hit_done;
    float vend[8];
    int vend_ok;                // pulblend: the drag victim half's exact end pose (world locator), captured as the drag runs out
    int shortd;                 // an UNCOVERED prone victim: intro + the first PUL_SHORT_DRAG_S of the drag (one tug), then the grapple pair
    int pounce;                 // an UNCOVERED prone victim - no intro, no drag: the attacker pounces straight into the grapple pair
    int var, krow, kskip, fb, ncyc, kpose;
    uint64_t kill_t0, kill_end; // ps45 prone: variant (PULV_*), the prone-kill row (-1 none), it starts PUL_B_SKIP_S in, a fallback was taken, cycle count, pose checked
    int grap, grow;
    uint64_t grap_t0, grap_end, hand_end, kill_at;      // grapple: flow on, its row, start / hand-off; hand_end = the hand-off the outcome waits on; kill_at = the death punch's lethal follow-up
    PadinjEdgeT edge;
} g_pul = {.slot = -1,.krow = -1 };

// ---- the victim side
static struct {
    int on, slot, id, burnt, decided, ok, shown, row, out;      // out: 0 the drag, 1 success pair, 2 fail pair
    uint64_t uid, last_pair, late_until, hit_at;
    uint64_t out_ms, sup_ms;
    int sup, sup_try;           // pulfix: the success pair's start / the go-supine hand-off (pul_go_supine)
    float end_s;
    double shown_t;             // shown_t: pair-clock ms the ESCAPE! prompt first showed (the window runs from there)
    int grap;                   // 1 = the drag runs and a grapple follows (no window yet), 2 = the grapple pair runs (the window is on it)
} g_pulv = {.slot = -1 };

static struct {
    int slot;
    uint64_t ms;
    int pounce;
} g_pul_igrap = { -1, 0, 0 };   // the last INTRO's grapple-flow bit (victim side)

static int PulPuppetBusy(uint32_t s)
{                               // a pull-out with slot s runs (attacker or victim side, intro to outcome) - no puppet re-spawn under it
    return (g_pul.st != PUL_IDLE && g_pul.slot == (int) s) || (g_pulv.on
                                                               && g_pulv.
                                                               slot ==
                                                               (int) s);
}

static int PulFlag(const char *f, int *v, uint64_t *at)
{
    uint64_t now = GetTickCount64();
    if (*v < 0 || now - *at >= 2000) {
        *at = now ? now : 1;
        *v = GetFileAttributesA(f) != INVALID_FILE_ATTRIBUTES;
    }
    return *v;
}

static int PulOff(void);
static int PulIntrocamOn(void);
static int PulCamRole(int *slot)
{                               // the pin camera (melee_spcam.c spc_tick5) for the pull-out: attacker = 2 from the intro to the outcome's end, victim = 1 while a pair of it plays on us
    if (PulOff()
        || GetFileAttributesA("t2r-pullout-nocam.txt") !=
        INVALID_FILE_ATTRIBUTES)
        return 0;
    if (g_pul.st >= PUL_INTRO && g_pul.slot >= 0) {
        if (PulIntrocamOn())
            return 0;
        *slot = g_pul.slot;
        return 2;
    }
    if (g_pulv.on && g_pulv.slot >= 0) {
        *slot = g_pulv.slot;
        return 1;
    }
    return 0;
}

static int PulAttActive(void)
{
    return g_pul.st != PUL_IDLE;
}                               // melee_duel.c: no duel swing while our pull-out runs

static int PulOff(void)
{
    static int v = -1;
    static uint64_t at;
    return PulFlag("t2r-pullout-off.txt", &v, &at);
}

static int PulCoverOff(void)
{
    static int v = -1;
    static uint64_t at;
    return PulFlag("t2r-pullout-cover-off.txt", &v, &at);
}

static int PulSidesOff(void)
{
    static int v = -1;
    static uint64_t at;
    return PulFlag("t2r-pullout-sides-off.txt", &v, &at);
}

static int PulProcOff(void)
{
    static int v = -1;
    static uint64_t at;
    return PulFlag("t2r-pullout-proccam-off.txt", &v, &at);
}

static int PulAny(void)
{
    static int v = -1;
    static uint64_t at;
    return PulFlag("t2r-pullout-any.txt", &v, &at);
}

// the drag exists in four directions (SP: where the NPC stands round the prone player): 0 bw = at the feet, 1 l = on its left, 2 r = on its right, 3 fw = at the head. The outcome pairs are shared.
static const char *const PUL_DRAG_NAMES[2][4] =
    { { "pu-drag-abby", "pu-drag-l-abby", "pu-drag-r-abby",
       "pu-drag-fw-abby" }, { "pu-drag-ellie", "pu-drag-l-ellie",
                             "pu-drag-r-ellie", "pu-drag-fw-ellie" } };
static const char *const PUL_OUT_NAMES[2][2] =
    { { "pu-ok-abby", "pu-fail-abby" }, { "pu-ok-ellie",
                                         "pu-fail-ellie" } };
static const char *const PUL_GRAP_NAMES[2] =
    { "pu-grap-abby", "pu-grap-ellie" };
// pulblend (pulling out went to the grapple instantly; in SP the attacker pounces in on the victim, ours teleported into it). SP evidence (re/pullout-pounce-1102.md,
// the SP melee recorder log of ep 5 / 8): there is NO transition clip. npc-prone-pullout-*-drag-sync runs to its last frame (phase 0.9976), the next frame npc-prone-struggle starts
// (a new sync move, its ap re-solved), and the VICTIM does not move at all across the cut (x/y/z/yaw identical on both sides); the pounce is the drag clip's own root motion - the
// puller closes 2.1 m -> 1.0 m -> 0.22 m from the victim over the clip. Ours solved the grapple pair on the ATTACKER's pose, so any difference between the two clips' authored
// relation and the bodies' end poses landed on the victim (it is put on the authored relation within 1-3 frames = the teleport). Now the grapple pair is solved on the VICTIM's end
// pose (the drag victim half's clip end, computed from the pair itself = identical on every client, no net lag) so the victim stays put like SP's and the attacker glides in
// (psync's SP pull, ease-out). t2r-pullout-blend-old.txt = the old attacker-anchored solve.
static int PulBlendOld(void)
{
    static int v = -1;
    static uint64_t at;
    return PulFlag("t2r-pullout-blend-old.txt", &v, &at);
}

static int PulPounceOff(void)
{
    static int v = -1;
    static uint64_t at;
    return PulFlag("t2r-pullout-pounce-off.txt", &v, &at);
}

static int PulPounceDirect(void)
{
    static int v = -1;
    static uint64_t at;
    return PulFlag("t2r-pullout-pounce-direct.txt", &v, &at);
}                               // 38add6cd behaviour: no intro, no drag

// Going straight to the pounce is not wanted: some part of that animation is needed, just without the long pull. An uncovered prone victim gets the
// intro (the grab at the ankles) and only the first PUL_SHORT_DRAG_S of the drag clip, handed off there (the pair's own end_s - the victim receives the same end_s). SP's drag clip
// (3.33 s) pulls the puller in 2.1 m -> 1.56 m by 0.6 s -> 1.23 m by 1.6 s; 0.85 s is the end of the first pull cycle (the arms are fully loaded, the body has leaned in once) and the
// grapple pair (solved on the victim's pose there, pulblend) closes the remaining gap like SP's cut does. t2r-pullout-pounce-off.txt = the full pull, ...-direct.txt = 38add6cd.
#define PUL_SHORT_DRAG_S 0.85f
static int PulNograpple(void)
{
    static int v = -1;
    static uint64_t at;
    return PulFlag("t2r-pullout-nograpple.txt", &v, &at);
}

#define PUL_KILL_AFTER_MS 1300  // the death punch's lethal follow-up: the fail blow downs, the downed body ignores hits for its first 1000 ms (down protection) + network jitter
// ---- prone pull-out. Observed: the long drag, then a POP into the grapple (the victim instantly on its back, the attacker behind it, no lunge), the dead victim kneeling
// ~0.5 s. Goal: the full SP animation except the transition part. SP's TRANSITION, measured in the SP recorder (re/review/pullout-1031/
// sp-pullout-ep5-ep8.log, victim rows "R,5,<frame>,...,<clip>,2,<phase>,0.000,<layer weight>,<clip s>"; ep 5 F17462-17790 feet drag, ep 8 F32113-32430 left drag):
//   * the drag (npc-prone-pullout-feet / -left-drag-sync) plays to its LAST frame (phase 0.9976 / 0.9957 of 3.333 s), npc-prone-struggle starts the next frame (#MSTART,
//     its ap re-solved, macro melee-npc-grab-struggle-one-shot) - no transition clip;
//   * the struggle layer's weight ramps 0.002 0.024 0.089 0.186 0.306 0.441 0.582 0.719 0.839 0.932 0.988 1.000 over 0.50 s (ep 8 0.51 s) = a 0.50 s SMOOTHSTEP crossfade
//     on top of the drag's held last pose; the intro -> drag start ramps the same way (0.014 0.051 0.110 0.183 0.270 0.366 0.468 at 22 .. 225 ms);
//   * the victim root does not move across the cut (52.455 8.475 79.763 yaw 140.80 on both sides), the attacker root is 0.22 m from it at the struggle's frame 0 and moves
//     1-2 mm per frame after it (the lunge is the drag clip's own last ~0.9 s: 1.0 m at phase 0.73 -> 0.22 m).
// Ours: the drag handed off at clip length - 0.13 s (pullout_rows.h: the end of the lunge cut), every pull-out pair started over psync's 0.20 s SP-pull blend, and on the
// victim's screen the attacker puppet's half started with a CUT (paired.c ps_pup_att_blend); pul_play also re-solved the grapple on the victim's LIVE locator after
// solving it on the drag-end pose (the second solve overwrote the first: a lagging / blocked victim puppet moved the grapple). Now (variant 'sp', the default): the drag of
// a grapple flow runs to its clip end - PUL_DRAG_TAIL_S, the grapple pair is solved on the victim's drag-end pose (and kept), and every pull-out pair (drag / grapple)
// starts with SP's 0.50 s smoothstep crossfade + smoothstep drive on BOTH halves and every screen (paired.c psync_play / ps_pup_att_blend ask pul_ps_xfade). Uncovered
// victims get the full drag too (the one-tug short drag = t2r-pullout-shortdrag.txt). t2r-pullout-spblend-off.txt = the old hand-off + blends.
// VARIANTS (t2r-prone-variant.txt next to the exe = sp / a / b / c / cycle, re-read every 2 s; missing = sp; cycle = sp, a, b, c in turn per pull-out; bot.c 'pullvar' overrides):
//   sp  the SP chain above (direction rules as before: the feet for an uncovered victim, any side for a covered one).
//   a   NO pull-out: npc-vs-player-prone-kill-left / -right (the side whose authored attacker spot, the pair solved on the victim's body, is nearer; <= PUL_A_FAR) 0 ..
//       PUL_KILL_END_S = the grab + the roll onto the back (psync glides us in) -> the grapple, solved on the kill victim half's end pose. Any bearing.
//   b   the intro (lunge + ankle grab) of the nearer side l / r -> its drag 0 .. PUL_B_DRAG_S (one tug) -> pu-kill-<side> PUL_B_SKIP_S .. PUL_KILL_END_S solved on the drag's
//       victim end pose (the side re-picked there by the nearer authored attacker spot) -> the grapple.
//   c   intro-l / -r -> the full side drag to its end -> the grapple at SP's cut (= sp on the side rows). Any bearing.
//   Every segment after the first is solved on the previous segment's victim end pose (computed from the pair itself = the same on every client); every pull-out pair
//   start crossfades (kill: PUL_KILL_XFADE_S). Fallbacks (logged "pullout: variant ..."): a -> c when no kill side fits (spot > PUL_A_FAR, gate refused, clips not
//   resident) or the victim is under cover; b / c -> a when every side drag was refused and the victim is in the open; a / b with a MALE victim: the kill clips only
//   have an Ellie victim half - played, and checked 0.2 s in ("pullout: kill pose"); a bind pose there (hands > 1 m up and > 1.2 m apart) turns a / b into c for male
//   victims for the session; t2r-prone-kill-male-off.txt = c for male victims at once. The INTRO byte carries the variant in bits 5-6 (a: no intro clip on the puppet).
//   Outcome pairs stay solved on the attacker (t2r-pullout-vend-out.txt = on the grapple's victim end pose too).
// DEAD UPRIGHT: the fail pair handed off at 1.75 s while the death punch lands PUL_KILL_AFTER_MS after its 1.00 s blow (the down protection) = ~0.55 s of the downed kneel.
// The fail pair of a grapple flow now hands off PUL_FAILHOLD_PAD_S after the death punch: the victim half plays / holds its lying end pose through the death (ctxdown's
// dead-at-the-hand-off holds take it from there). t2r-pullout-failhold-off.txt = the old 1.75 s.
#define PUL_SP_XFADE_S 0.50f    // SP measured (above)
#define PUL_KILL_XFADE_S 0.30f  /* [TUNING ps45] */
#define PUL_KILL_END_S 0.45f    /* [TUNING ps45] (research: the kill's victim pose within 0.12 m of the struggle's frame 0 at 0.4-0.6 s) */
#define PUL_B_DRAG_S 0.80f      /* [TUNING ps45] one tug of the side drag */
#define PUL_B_SKIP_S 0.15f      /* [TUNING ps45] b plays the kill clip from here (past its own approach) */
#define PUL_A_FAR 2.2f          /* [TUNING ps45] m: the kill's authored attacker spot from us */
#define PUL_DRAG_TAIL_S 0.02f   // SP: phase 0.9976 of 3.333 s = 8 ms before the clip's end; ours ~one frame
#define PUL_FAILHOLD_PAD_S 0.25f        /* [TUNING ps45] the fail pair's hand-off past the death punch (its 250 crossing the network) */
enum { PULV_SP, PULV_A, PULV_B, PULV_C };
static const char *const PULV_TAG[4] = { "sp", "a", "b", "c" };

static const char *const PULV_NAME[4] =
    { "sp = the full SP drag + SP's 0.5 s crossfade into the struggle",
"a = no pull-out: prone-kill grab + roll -> grapple",
    "b = side intro + one tug of the side drag -> prone-kill roll -> grapple",
        "c = side intro + the full side drag -> grapple at SP's cut"
};
static const char *const PUL_KILL_NAMES[2] = { "pu-kill-l", "pu-kill-r" };

static int g_pulVarForce = -2;        // bot.c 'pullvar': -2 = the file decides, -1 = cycle, 0..3 = that variant
static int g_pulKillMaleBad; // a male victim showed a bind pose under the kill clip: a / b -> c for male victims (this session)
static int PulVarCfg(void)
{                               // -1 = cycle, 0..3
    static int v = -3;
    static uint64_t at;
    if (g_pulVarForce >= -1)
        return g_pulVarForce;
    uint64_t now = GetTickCount64();
    if (v == -3 || now - at >= 2000) {
        at = now ? now : 1;
        int nv = PULV_SP;
        char w[16] = "";
        FILE *f = ExeSideOpen("t2r-prone-variant.txt", "r");
        int had = f != NULL;
        if (f) {
            if (fscanf(f, "%15s", w) != 1)
                w[0] = 0;
            fclose(f);
        }
        for (char *c = w; *c; c++)
            if (*c >= 'A' && *c <= 'Z')
                *c = (char) (*c + 32);
        if (!strcmp(w, "a"))
            nv = PULV_A;
        else if (!strcmp(w, "b"))
            nv = PULV_B;
        else if (!strcmp(w, "c"))
            nv = PULV_C;
        else if (!strcmp(w, "cycle"))
            nv = -1;
        if (nv != v)
            PULLOG("variant switch: t2r-prone-variant.txt %s -> %s",
                   had ? (w[0] ? w : "(empty)") : "missing",
                   nv < 0 ? "cycle (sp, a, b, c in turn)" : PULV_NAME[nv]);
        v = nv;
    }
    return v;
}

static int PulVarPick(void)
{
    int c = PulVarCfg();
    return c >= 0 ? c : g_pul.ncyc % 4;
}

static int PulSpxOff(void)
{
    static int v = -1;
    static uint64_t at;
    return PulFlag("t2r-pullout-spblend-off.txt", &v, &at);
}

static int PulShortdragOn(void)
{
    static int v = -1;
    static uint64_t at;
    return PulFlag("t2r-pullout-shortdrag.txt", &v, &at);
}

// (the enemy was just knocked down instead of dying): the 2.55 s fail hold kept the victim in the pair when the death punch's lethal 250 landed -
// the pair aborted and the victim stayed downed. Back to the table hand-off (the death lands on the downed body as before); t2r-pullout-failhold-on.txt = the ps45 hold.
static int PulFailholdOff(void)
{
    static int v = -1;
    static uint64_t at;
    return !PulFlag("t2r-pullout-failhold-on.txt", &v, &at);
}

static int PulVendoutOn(void)
{
    static int v = -1;
    static uint64_t at;
    return PulFlag("t2r-pullout-vend-out.txt", &v, &at);
}

static int PulKillMaleOff(void)
{
    static int v = -1;
    static uint64_t at;
    return PulFlag("t2r-prone-kill-male-off.txt", &v, &at);
}

static int PulKillRow(int row)
{
    return row >= 0 && !strncmp(CD_PAIRS[row].name, "pu-kill", 7);
}

// paired.c psync_play / ps_pup_att_blend: the crossfade a pull-out pair starts with (every client answers it from the row name)
static float PulPsXfade(uint64_t move)
{
    int p = CdPairOfMove(move);
    if (p < 0 || CD_PAIRS[p].kind != 8 || PulSpxOff())
        return 0.0f;
    const char *n = CD_PAIRS[p].name;
    if (!strncmp(n, "pu-drag", 7) || !strncmp(n, "pu-grap", 7))
        return PUL_SP_XFADE_S;
    if (!strncmp(n, "pu-kill", 7))
        return PUL_KILL_XFADE_S;
    return 0.0f;
}

// the hand-off of `row` in the running sequence (attacker side; the victim receives it in the START)
// (the characters were teleported around; the position should have stayed relative): the victim's screen
// released its own body at the drag's hand-off ("ABORT (contextual down hand-off)", pelvis 0.12 -> 0.96, the body back to the engine) BEFORE the grapple START
// arrived one-way later, then the grapple placed it 2.55 m / 180 deg off and pulled it over 1 s. The attacker now starts the grapple PUL_LEAD_MS before its drag
// hand-off (grapple flow only), so the START lands on the victim while its drag half still plays; SP's 0.5 s crossfade covers the drag's last frames.
// t2r-pullout-lead-ps46-old.txt = the start at the hand-off.
// The lead did nothing - drag_end moved, but the grapple waited for psync to end OUR drag pair at the unchanged pair end
// (`ours` stayed true until pul_dend), so it started at the old moment. Now, at drag_end (PUL_LEAD_MS before the pair end) the attacker ends its OWN drag
// copy locally (psync_abort, no ABORT sent - the victim's drag half plays on) and starts the grapple in the same frame; the victim's (and a bystander's)
// psync_begin_remote lets the same pair's pull-out grapple / kill START replace the running drag copy (paired.c "pull-out chain") instead of refusing it
// as BUSY. Proof lines "ps46b lead". A one-way above PUL_LEAD_MS = the START lands after the victim's hand-off as before (no worse).
static uint64_t g_pulLeadOn;  // the lead the running drag was started with (ms), 0 = none
#define PUL_LEAD_MS 150         /* [TUNING ps46] ms: > one-way (14 ms LAN, 60 ms typical online) + one frame */
static uint64_t PulLeadMs(void)
{
    static int v = -1;
    static uint64_t at;
    if (!g_pul.grap || g_pul.var == PULV_B || g_pul.shortd
        || PulFlag("t2r-pullout-lead-ps46-old.txt", &v, &at))
        return 0;
    PULLOG
        ("ps46b lead: the grapple starts %d ms before the drag's hand-off (our drag copy ends locally then, the START reaches the victim inside its drag half)",
         PUL_LEAD_MS);
    return PUL_LEAD_MS;
}

static float PulDend(int row)
{
    if (row < 0)
        return 0.0f;
    if (row == g_pul.krow)
        return PUL_KILL_END_S;
    if (row == g_pul.row[0]) {
        if (g_pul.var == PULV_B)
            return PUL_B_DRAG_S;
        if (g_pul.shortd)
            return PUL_SHORT_DRAG_S;
        if (g_pul.grap && !PulSpxOff()) {     // SP: the drag runs to its last frame, the struggle crossfades in on top of it
            float L = AnimLen(SID_PLAYER, CD_VIC0(row)), La =
                AnimLen(SID_PLAYER, CD_ATT0(row));
            if (La > 0.2f && La < L)
                L = La;
            if (L > CD_PAIRS[row].end_s && L < 10.0f)
                return L - PUL_DRAG_TAIL_S;
        }
    }
    if (row == g_pul.row[2] && g_pul.grap && !PulFailholdOff()) {     // the fail pair keeps the victim half past the death punch
        float want =
            CD_PAIRS[row].dmax + PUL_KILL_AFTER_MS / 1000.0f +
            PUL_FAILHOLD_PAD_S;
        if (want > CD_PAIRS[row].end_s && want < 5.5f)
            return want;
    }
    return CD_PAIRS[row].end_s;
}

// ps46 item 3 (ps45a A [8872] 'FAIL blow ... the death punch's lethal follow-up in 1.3 s', [8951] 'DEATH PUNCH: lethal 250 (downed 0)'; B [10777] 'pair hit down ...
// 1 hp, the pair plays on, down ... at its hand-off', [10856] 'finisher guard: hit 250 during the finisher dropped', [10867] 'downed: down 1 ... bleed-out 15.0 s',
// [11766] 'bleed-out kill' = clip 22.53 s; the same on all 3 pulls): the victim goes down at the fail pair's HAND-OFF, not at the blow, and t2r-ext.c recv-hit drops
// every hit in the first 1000 ms of a down (g_down_ms). PUL_KILL_AFTER_MS (1.3 s after the 1.0 s blow = 2.3 s) fell inside the 2.55 s hold (ps45: dropped by the
// finisher guard) and, with the 1.75 s table hand-off (ps46 default), 0.55 s into the down protection (dropped again) - the victim only bled out. The death punch now
// goes out PUL_DOWNPROT_MS + PUL_KILL_NET_PAD_MS after the fail pair's hand-off (never sooner than PUL_KILL_AFTER_MS after the blow).
// t2r-pullout-killtime-ps46-old.txt = the 1.3 s after the blow. Proof line "ps46 kill time".
#define PUL_DOWNPROT_MS 1000    // t2r-ext.c recv-hit: the down protection (read from the code, not tuned)
#define PUL_KILL_NET_PAD_MS 250 /* [TUNING ps46] one-way (14 ms LAN, ~60 ms online) + jitter + the victim's END -> down frames */
static uint64_t PulKillAfterMs(void)
{
    static int v = -1;
    static uint64_t at;
    if (PulFlag("t2r-pullout-killtime-ps46-old.txt", &v, &at)
        || g_pul.row[2] < 0)
        return PUL_KILL_AFTER_MS;
    float hand = PulDend(g_pul.row[2]), hit = CD_PAIRS[g_pul.row[2]].dmax;
    long ms =
        (long) ((hand - hit) * 1000.0f) + PUL_DOWNPROT_MS +
        PUL_KILL_NET_PAD_MS;
    if (ms < PUL_KILL_AFTER_MS)
        ms = PUL_KILL_AFTER_MS;
    if (ms > 4000)
        ms = 4000;
    static long last = -1;
    if (ms != last) {
        last = ms;
        PULLOG
            ("ps46 kill time: the death punch goes out %ld ms after the fail blow (hand-off %.2f s - blow %.2f s + down protection %d ms + %d ms) - it lands on the downed body past its protection",
             ms, (double) hand, (double) hit, PUL_DOWNPROT_MS,
             PUL_KILL_NET_PAD_MS);
    }
    return (uint64_t) ms;
}

// the ap of `row` solved so its VICTIM half sits on `anchor` at clip second p0 (psync_solve_ap = p0 0; yaw-only like it)
static int PulSolveApAt(uint64_t h, int row, const float *anchor,
                           float p0, float *ap7)
{
    if (!(p0 > 0.0005f))
        return PsyncSolveAp(h, Fnv64Rt(CD_VIC0(row)), anchor, ap7);
    float c[8], ic[8], r[8];
    if (!PsLocOk(anchor) || !CdOffsetAt(h, CD_VIC0(row), p0, c))
        return 0;
    float yc = Uc4Yaw(c) * 0.0174533f, ya = Uc4Yaw(anchor) * 0.0174533f;
    float cu[8] =
        { c[0], c[1], c[2], 1.0f, 0, __builtin_sinf(yc * 0.5f), 0,
  __builtin_cosf(yc * 0.5f) };
    float au[8] =
        { anchor[0], anchor[1], anchor[2], 1.0f, 0,
  __builtin_sinf(ya * 0.5f), 0, __builtin_cosf(ya * 0.5f) };
    PsInverse(cu, ic);
    PsCompose(au, ic, r);
    PsAp7(r, ap7);
    return 1;
}

// the prone-kill side whose authored attacker spot (at clip second p0, the pair solved on `anchor` = the victim's pose) is nearer to us. -1 = none resident / readable
static int PulKillPick(uint64_t h, const float *anchor, float p0,
                         float *da_out, const char *tag)
{
    float mb[16];
    const float *me = PlayerLocator(mb);
    int best = -1;
    float bd = 1e9f;
    *da_out = 1e9f;
    if (!me || !PsLocOk(me) || !anchor || !PsLocOk(anchor))
        return -1;
    for (int k = 0; k < 2; k++) {
        int r = CdPairOfMove(Fnv64Rt(PUL_KILL_NAMES[k]));
        if (r < 0 || !CdPairLoaded(r)) {
            PULLOG("%s: %s not resident here", tag, PUL_KILL_NAMES[k]);
            continue;
        }
        float ap[7], mv, da, sa[3], vy, V[3] =
            { anchor[0], anchor[1], anchor[2] };
        if (!PulSolveApAt(h, r, anchor, p0, ap)
            || !CdSpotsAt(r, h, ap, V, me, p0, &mv, &da, sa, &vy)) {
            PULLOG("%s: %s spots unreadable", tag, PUL_KILL_NAMES[k]);
            continue;
        }
        PULLOG
            ("%s: %s - its authored attacker spot (clip %.2f s, the pair solved on the victim's pose) is %.2f m from us",
             tag, CD_PAIRS[r].name, (double) p0, (double) da);
        if (da < bd) {
            bd = da;
            best = r;
        }
    }
    *da_out = bd;
    return best;
}
static const char *const PUL_INTRO_CLIPS[2][4] =
    { { "abby-pullout-prone-bw-intro--npc",
       "abby-pullout-prone-l-intro--npc",
       "abby-pullout-prone-r-intro--npc",
       "abby-pullout-prone-fw-intro--npc" },
{ "ellie-pullout-prone-bw-intro--npc", "ellie-pullout-prone-l-intro--npc",
 "ellie-pullout-prone-r-intro--npc", "ellie-pullout-prone-fw-intro--npc" }
};
static const char *const PUL_DIR_NAME[4] =
    { "feet (bw)", "left (l)", "right (r)", "head (fw)" };
static const float PUL_DIR_ANG[4] = { 180.0f, 90.0f, -90.0f, 0.0f };    // the attacker's bearing round the victim's body axis (0 = past its head, + = its left) each drag direction was authored for

static const char *PulIntroClip(int set, int dir)
{
    return PUL_INTRO_CLIPS[set & 1][dir & 3];
}

// the three rows of a set (CD_PAIRS rows, all clips resident) for the drag direction `dir`; the victim's own character first (male = abby, female = ellie), else the other set. -1 = none loaded
// (int108 A [8824]: the TRIGGER frame spent 5.7 ms in 797 natives - the residency checks below, up to 4 directions x 2 sets): the answer per
// (set wanted, direction) is kept for 120 s (the paks of a level do not change mid-match), warmed one pair per cover scan while a prone enemy is near
static int PulPickSetRaw(int want, int dir, int *rows);
static int PulPickSet(int want, int dir, int *rows)
{
    static struct {
        uint64_t at;
        int set, rows[3];
    } c[2][4];
    if (want < 0 || want > 1 || dir < 0 || dir > 3)
        return PulPickSetRaw(want, dir, rows);
    uint64_t now = GetTickCount64();
    if (c[want][dir].at && now - c[want][dir].at < 120000) {
        memcpy(rows, c[want][dir].rows, sizeof c[want][dir].rows);
        return c[want][dir].set;
    }
    int set = PulPickSetRaw(want, dir, rows);
    c[want][dir].at = now;
    c[want][dir].set = set;
    memcpy(c[want][dir].rows, rows, sizeof c[want][dir].rows);
    return set;
}

static int PulPickSetRaw(int want, int dir, int *rows)
{
    for (int pass = 0; pass < 2; pass++) {
        int set = pass ? 1 - want : want, ok =
            AnimLen(SID_PLAYER, PulIntroClip(set, dir)) > 0.2f;
        rows[0] = CdPairOfMove(Fnv64Rt(PUL_DRAG_NAMES[set][dir]));
        rows[1] = CdPairOfMove(Fnv64Rt(PUL_OUT_NAMES[set][0]));
        rows[2] = CdPairOfMove(Fnv64Rt(PUL_OUT_NAMES[set][1]));
        for (int k = 0; k < 3 && ok; k++)
            if (rows[k] < 0 || !CdPairLoaded(rows[k]))
                ok = 0;
        if (ok)
            return set;
    }
    return -1;
}

static uint8_t *PulPeer(int s)
{
    uint8_t *pe = MdgPeer((uint32_t) s);
    return pe && pe[4] && !pe[5] && !pe[0x4d]
        && *(uint64_t *) (pe + 0x60) ? pe : NULL;
}

// (every TRIGGER ended "GATE refused: the pair's authored attacker spot is 47-65 m from us"):
// the pull-out was found on a RESPAWNING body - the respawn grant replaces the puppet ("respawn puppet: slot N new body", "dead here 1 -> 0"), and for
// ~3 frames the new body sits on the corpse's spot in its lying pose (pelvis 0.10 m: our head-height prone test passes), then the net locator snaps it to
// the picked spawn point ("visdiag ... pos-jump ... moved 47.02 m" = the spawn pick's distance). The GATE frame solved the pair on that new locator, so
// the clip spots came out 47-65 m away. Not a clip / align / ap fault (the victim's align C -0.02 0 -1.93 yaw -180 is the same as live). Now: a slot in
// its respawn grace (the window where hits on its puppet are ignored: t2r-ext.c g_grace_until, until it reached its spawn point + RESPAWN_GRACE) is no
// pull-out victim. t2r-pullout-gatefix-ps45-old.txt = the old test. Proof line "pullout: ps45 gatefix".
static int PulGatefixOld(void)
{
    static int v = -1;
    static uint64_t at;
    return PulFlag("t2r-pullout-gatefix-ps45-old.txt", &v, &at);
}

static int PulRespawning(int s)
{
    if (PulGatefixOld()
        || !(g_graceDead[s] || g_t60 < g_graceUntil[s]))
        return 0;
    static uint64_t last[T2R_MAX_PEERS];
    uint64_t now = GetTickCount64();
    if (g_t60 < g_graceUntil[s] && now - last[s] > 3000) {
        last[s] = now;
        Xlog("pullout: ps45 gatefix - slot %d is respawning (dead %d, waiting for its spawn point %d, grace %llu frames left): its body is no prone victim (t2r-pullout-gatefix-ps45-old.txt = old)", s, g_graceDead[s], g_gracePend[s], (unsigned long long) (g_graceUntil[s] > g_t60 ? g_graceUntil[s] - g_t60 : 0));
    }
    return 1;
}

static int PulSlotOk(int s)
{
    return s >= 0 && s < T2R_MAX_PEERS && s != (int) PsyncSelf()
        && PulPeer(s) && !g_peerDowned[s] && !g_puppetDeadHere[s]
        && !IsTeammate((uint32_t) s) && !PulRespawning(s);
}

static float PulWrap(float a)
{
    while (a > 180.0f)
        a -= 360.0f;
    while (a < -180.0f)
        a += 360.0f;
    return a;
}

static int PulLocalProne(void)
{
    uint64_t n = 0, p = DcCall(FNV64("player-is-prone?"), 0, &n), su =
        DcCall(FNV64("player-is-supine?"), 0, &n);
    return (p != ~0ULL && (p & 0xff)) || (su != ~0ULL && (su & 0xff));
}

static int PulLocalCrouched(void)
{
    uint64_t n = 0, c = DcCall(FNV64("player-is-crouched?"), 0, &n);
    return c != ~0ULL && (c & 0xff);
}

// the attacker may start a pull-out: standing, alive, free of every pair / duel / pin / execution. NULL = free
static const char *PulAttBusy(void)
{
    if (MatchSubstate() != 6)
        return "no match";
    if (g_downed || g_downDying || g_execUntil || g_uc4DyingUntil
        || g_downAt || LocalHp() <= 0.0f)
        return "we are down / dying / in an execution";
    if (PsyncOwn() || g_dcm.on_att || g_dcm.on_vic || g_msq.on
        || GxRunning())
        return "in a paired move";
    if (g_mpn.on || MduInPair())
        return "in a duel pair / pin";  // (an engaged duel no longer blocks - the pull-out takes over from it at the trigger)
    if (PulLocalProne() || PulLocalCrouched())
        return "not standing";
    return NULL;
}

// ---- COVER: is a prone enemy under a vehicle / low roof? One async world ray per prone puppet near us, straight up from 0.15 m above its pelvis to 1.6 m above it (a truck's underside, a car,
// a table, a low overhang all hit; the puppet itself is ignored). Kicked every PUL_COV_MS, read on a LATER frame (ray_t waits for the job), the answer kept with its time.
static struct {
    uint64_t job, kick_ms, ts_ms;
    uint32_t kick_f;
    int covered, valid;
} g_pulcov[T2R_MAX_PEERS];
static int PulProneHead(uint64_t h, const float *vl)
{
    float hd[3];
    return GxJointPos(h, "headb", hd) && hd[1] - vl[1] < PUL_PRONE_HEAD;
}

static int PulCovered(int s, uint64_t now)
{
    if (PulCoverOff() || s < 0 || s >= T2R_MAX_PEERS)
        return 0;
    return g_pulcov[s].valid && g_pulcov[s].covered
        && now - g_pulcov[s].ts_ms < PUL_COV_KEEP_MS;
}

static void PulCoverTick(uint64_t now)
{
    static uint64_t nxt;
    static int nlog;
    if (PulCoverOff()) {
        for (int s = 0; s < T2R_MAX_PEERS; s++)
            if (g_pulcov[s].job) {
                RayT(g_pulcov[s].job);
                g_pulcov[s].job = 0;
            }
        return;
    }
    float mb[16];
    const float *me = PlayerLocator(mb);
    for (int s = 0; s < T2R_MAX_PEERS; s++) {
        uint64_t *job = &g_pulcov[s].job;
        if (*job && g_t60 != g_pulcov[s].kick_f) {      // answered on a later frame
            float t = RayT(*job);
            *job = 0;
            if (!g_rayErr) {
                // (pulling opponents that were not under cover; Bookstore: 10 x "UNDER COVER ... up ray t 0.00", flipping open / covered
                // within frames): a hit at t 0 means the ray STARTED inside something - the prone body's own bag / long guns / attached props (only its
                // character is ignored), not a roof. A real underside is hit above the start. TUNING: 0.03 of 1.45 m = 4 cm.
                int c = t >= 0.03f && t < 0.99f;
                if (!g_pulcov[s].valid || g_pulcov[s].covered != c)
                    PULLOG
                        ("cover: slot %d is %s (up ray t %.2f over 1.6 m)",
                         s,
                         c ?
                         "UNDER COVER (vehicle / roof) - any side, nearest-body range"
                         : "in the open", (double) t);
                g_pulcov[s].covered = c;
                g_pulcov[s].valid = 1;
                g_pulcov[s].ts_ms = now;
            }
        }
    }
    if (!me || !PsLocOk(me) || now < nxt)
        return;
    nxt = now + PUL_COV_MS;
    for (int s = 0; s < T2R_MAX_PEERS; s++) {
        uint64_t *job = &g_pulcov[s].job;
        if (*job)
            continue;
        if (!PulSlotOk(s)) {
            g_pulcov[s].valid = 0;
            continue;
        }
        uint8_t *pe = PulPeer(s);
        uint64_t h = pe ? *(uint64_t *) (pe + 0x60) : 0;
        float lb[16];
        const float *vl = h ? PuppetLocator(h, lb) : NULL;
        if (!vl || !PsLocOk(vl)) {
            g_pulcov[s].valid = 0;
            continue;
        }
        float dx = me[0] - vl[0], dz = me[2] - vl[2];
        if (dx * dx + dz * dz > PUL_COV_SCAN * PUL_COV_SCAN
            || __builtin_fabsf(me[1] - vl[1]) > 3.0f) {
            g_pulcov[s].valid = 0;
            continue;
        }
        if (!PulAny() && !PulProneHead(h, vl)) {
            g_pulcov[s].valid = 0;
            continue;
        }                       // (only a prone body can be pulled: no ray for the standing)
        float pel[3], a[3], b[3];
        if (!GxJointPos(h, "pelvis", pel)
            || __builtin_fabsf(pel[0] - vl[0]) + __builtin_fabsf(pel[2] -
                                                                 vl[2]) >
            2.0f) {
            pel[0] = vl[0];
            pel[1] = vl[1] + 0.15f;
            pel[2] = vl[2];
        }
        a[0] = pel[0];
        a[1] = pel[1] + 0.15f;
        a[2] = pel[2];
        b[0] = a[0];
        b[1] = pel[1] + 1.6f;
        b[2] = a[2];
        *job = WorldRayKickIgn(a, b, h);
        g_pulcov[s].kick_ms = now;
        g_pulcov[s].kick_f = g_t60;
        {
            static int wk;
            int rows[3], k = wk++ & 7;
            PulPickSet(k >> 2, k & 3, rows);
        }                       // warm one (set, direction) residency answer per scan: the TRIGGER frame reads the cache
        if (!*job && LOGCAP(nlog, 5))
            PULLOG("cover: the up ray for slot %d was not kicked", s);
    }
}

// horizontal range from the attacker to the NEAREST of the victim's root / head / ankles (a covered victim's root is deep under the vehicle)
static float PulNear(const float *me, uint64_t h, const float *vl)
{
    float best =
        __builtin_sqrtf((me[0] - vl[0]) * (me[0] - vl[0]) +
                        (me[2] - vl[2]) * (me[2] - vl[2])), p[3];
    static const char *const J[3] = { "headb", "l_ankle", "r_ankle" };
    for (int k = 0; k < 3; k++)
        if (GxJointPos(h, J[k], p)
            && __builtin_fabsf(p[0] - vl[0]) + __builtin_fabsf(p[2] -
                                                               vl[2]) <
            3.0f) {
            float d =
                __builtin_sqrtf((me[0] - p[0]) * (me[0] - p[0]) +
                                (me[2] - p[2]) * (me[2] - p[2]));
            if (d < best)
                best = d;
        }
    return best;
}

// the attacker's bearing round the victim's body axis (0 = past its head, +90 = its left, 180 = at its feet), from the joints (gx_body_side)
static float PulBearing(uint64_t h, const float *vl, const float *me)
{
    float fw, sd;
    if (GxBodySide(h, vl, me, &fw, &sd))
        return __builtin_atan2f(sd, fw) * 57.29578f;
    return PulWrap(__builtin_atan2f(me[0] - vl[0], me[2] - vl[2]) *
                    57.29578f - Uc4Yaw(vl) - 180.0f) + 180.0f;
}

// the nearest live enemy puppet that is prone and whose feet side we stand on (a COVERED one: from any side), within PUL_RANGE. vb receives its locator; *cov its covered flag; *dist the range used
static int PulFind(float *vb, float *mb, const char **why, int *cov,
                    float *dist, int anyside)
{
    const float *me = PlayerLocator(mb);
    *why = "no enemy near";
    *cov = 0;
    *dist = 0.0f;
    if (!me || !PsLocOk(me)) {
        *why = "no own locator";
        return -1;
    }
    if (me != mb)
        memcpy(mb, me, sizeof(float) * 8);
    int best = -1;
    float bd = 1e9f;
    uint64_t now = GetTickCount64();
    for (int s = 0; s < T2R_MAX_PEERS; s++) {
        if (!PulSlotOk(s) || now < g_pul.cool[s])
            continue;
        uint8_t *pe = PulPeer(s);
        uint64_t h = *(uint64_t *) (pe + 0x60);
        float lb[16];
        const float *vl = PuppetLocator(h, lb);
        if (!vl || !PsLocOk(vl))
            continue;
        int c = PulCovered(s, now);
        float dx = me[0] - vl[0], dz = me[2] - vl[2], d =
            __builtin_sqrtf(dx * dx + dz * dz);
        if (c)
            d = PulNear(me, h, vl);
        if (d > PUL_RANGE || __builtin_fabsf(me[1] - vl[1]) > 1.2f)
            continue;
        float hd[3];
        if (!PulAny()
            && (!GxJointPos(h, "headb", hd)
                || hd[1] - vl[1] >= PUL_PRONE_HEAD)) {
            *why = "enemy not prone";
            continue;
        }                       // (no stance flag for a puppet: the head sits close above the root)
        float bearing = __builtin_atan2f(dx, dz) * 57.29578f, back =
            Uc4Yaw(vl) + 180.0f;
        if (!c && !anyside
            && __builtin_fabsf(PulWrap(bearing - back)) > PUL_CONE_DEG) {
            *why = "not behind the prone enemy (feet side only)";
            continue;
        }                       // (ps45 a / b / c: any side)
        if (d < bd) {
            bd = d;
            best = s;
            *cov = c;
            *dist = d;
            memcpy(vb, vl, sizeof(float) * 8);
        }
    }
    return best;
}

// the melee press (keyboard F / the engine's melee-attack command / the bot), the same sources as the pin's blow press
static int PulPressEdge(uint64_t now)
{
    int foc = GameFocused(), fk =
        (GetAsyncKeyState(0x46) & 0x8000) != 0, cmd =
        PadCmdOn("melee-attack");
    int kb = (foc && fk) || cmd, bot = g_botMeleeMs
        && now - g_botMeleeMs < 80;
    const char *src = "-";
    int e = PadinjMelEdge(&g_pul.edge, now, kb, bot, &src);
    return e == 1 && !InteractDown();  // (E held = the contextual shiv / execution, not ours)
}

static void PulSend(int slot, int op, int id, const uint8_t *p, int n)
{
    if (!g_sendTramp || slot < 0)
        return;
    uint8_t b[16] = { 5, (uint8_t) op, (uint8_t) id, 0 };
    if (n > 12)
        n = 12;
    if (n > 0)
        memcpy(b + 4, p, (size_t) n);
    g_sendTramp(MSG_MEX, b, (uint64_t) (4 + n), (uint64_t) slot);
}

// ---- TLOU2 SP's own cameras (tools/pullout_cam_gen.py -> pullout_cam_tracks.h; SP cameras are required): the player halves' apReference-camera1..4 channels of the drag / success /
// fail clips, in the pair's ap space, played by the execution camera (xcam) on BOTH screens (the victim's role-2 xcam maps onto the same role-1 track, xcam_track_pick). One xcam runs from the
// drag to the outcome's end: at the outcome pair's start the track is swapped in place (the same camera index, a new ap, a 0.4 s crossfade from the last pose: g_xph) - a restart would show
// the gameplay camera for the camera-choice ticks. The intro (attacker-only, the victim is still free) keeps the v5 pin camera (pul_cam_role) and so does every client with no track.
// t2r-pullout-pincam.txt = the old pin-camera framing for the whole sequence.
static struct {
    int on, slot, role, judged, dropped, peer_cam, intro, sent;
    uint64_t t0;
} g_pulc = {.slot = -1,.peer_cam = -1 };        // intro: our attacker camera started at the intro (v2)

static int PulPincamOld(void)
{
    static int v = -1;
    static uint64_t at;
    return PulFlag("t2r-pullout-pincam.txt", &v, &at);
}

// (the attacker camera did not follow the player down in a cinematic way, and the victim did not get the SP
// camera at all; A frames s5 6.5-9.0: a hard cut at the intro to a wide static shot (the pin camera framing the pair from 2+ m), then a second snap ~2.5 s later
// when the procedural camera's 2.7 s swing finally showed; B: the first second of its 2.7 s smoothstep swing barely moved, so it read as the gameplay camera):
// v2 = the attacker's procedural camera starts AT THE INTRO (no pin camera ever, nothing to cut from), swings in over 1.6 s, sits at the puller's chest height
// (+0.3 m) instead of a fixed 1.7 m over the root - it goes down with the body into the grab - and 0.35 m closer; the victim's swing is 1.6 s too.
// t2r-pullout-cam-v2-off.txt = the old cameras.
#define PUL_V2_SWING_S 1.6f
static int PulIntrocamOn(void)
{
    return g_pulc.intro;
}

static int PulCamV2(void)
{
    static int v = -1;
    static uint64_t at;
    return !PulFlag("t2r-pullout-cam-v2-off.txt", &v, &at)
        && !PulPincamOld();
}

static float PulCamOff(float end_s)
{                               // the track clock runs on the move's clock: a victim joins a few ms late (its START skip), as ctxdown.c cd_cam_tracks
    float off =
        g_ps.role ? (float) ((PsyncMs() - g_ps.t0 + g_ps.chain_shift) /
                             1000.0) : 0.0f;
    return off > 0.0f && off < end_s - 0.5f ? off : 0.0f;
}

static void PullstingGrab(void);       // pullout_sting.c (victim music)
static void PullstingResult(int ok);
static void PullstingCut(int fade_ms, const char *why);
static void PullstingTick(void);
static void PullstingOutcome(int ok, float blow_s);
// ---- the PROCEDURAL pull-out camera (the attacker also has a cinematic camera in all cases, using the same rules as the victim camera, composition included).
// Runs wherever SP's authored shot is blocked (prone under a truck: every authored camera sat in the vehicle / the bodies) or absent (the l / r / fw drag rows have no track), on BOTH screens, as an
// xcam (g_xcam_pulproc, t2r-ext.c xcam_tick): no cut (2.7 s smoothstep swing-in from the live camera), fov 70 -> 73.7 over the same 2.7 s (g_xcam_fov0 x 0.95 -> x 1), held through the struggle and the
// outcome pair, 1.5 s ease out (the attacker's hand-back). Each player sees ITS OWN role's shot.
// COMPOSITION = SP's victim shot, measured from the player half of pullout-prone-bw, camera 3 (the calm camera the victim plays by default: pullout_cam_tracks.h PUK_14 / PUK_2), in the pair's ap
// space against the victim's align (the victim travels +z toward the puller; 13 samples 0.267 s apart, 3.2 s):
//   distance (horizontal, camera -> victim)  1.35 1.29 1.27 1.28 1.34 1.36 1.17 1.24 1.21 1.28 1.27 1.19 1.17 m   (a slow 0.2 m push-in)
//   bearing round the victim, off straight BEHIND it (head side), i.e. -travel:  22 21 23 22 19 16 12 14 11 8 2 1 8 deg   (the camera drifts in behind the victim's head, along the drag line)
//   height over the victim's root           0.28 0.25 0.25 0.24 0.22 0.24 0.20 0.20 0.18 0.21 0.32 0.49 0.51 m   (a floor-level shot, rising at the end)
//   victim's screen position: horizontally centred (-12 .. +6 deg off the view axis), vertically at the view axis +-8 deg: voff (victim elevation - view pitch) -2 -1 -2 -2 1 3 6 3 1 0 -1 -5 -8 deg
//   view pitch +3 .. +5 deg, -4 at the end (level, the puller standing up the line beyond the victim: the victim is the foreground, the puller the far figure); hfov 83 (the user's FOV scaled);
//   roll 1 .. 8 deg (left out). Success / fail cameras (PUK_18 / 22) sit 1.0 m away, 0.5 m up, pitch -6 .. +5 (fail: a top-down 0.3 m shot, roll 40-54; not copied) - the drag's end pose is held.
// VICTIM shot = that composition as is (subject = us). ATTACKER shot = the mirror (subject = us = the puller): the same distance / push-in / bearing drift / voff, camera BEHIND the puller on the
// open side looking along the drag line, so the victim is the far figure dragged TOWARD the lens like the puller is for the victim; the height scales to a standing body (1.15 m + 0.8 x the drag's
// rise - a low over-the-shoulder shot) and the aim is 70 % of the way from the puller's chest to the victim's pelvis (a centred puller's back would hide the victim; the puller sits at the screen edge).
// COLLISION: 7 candidate camera spots (the current side at 0 / +35 / +70 deg wider, the other side the same, straight back), each tested with two async rays (subject -> lens and lens -> aim point,
// refreshed every 0.4 s); the first clear one wins (the current one is kept while it stays clear - no flipping); none clear = the least blocked with the arm pulled short. The side the lens starts on is
// the live gameplay camera's. t2r-pullout-proccam-off.txt = off (the old pin-camera fallback).
static const float PCP_DT = 0.2667f;
static const float PCP_D[13] =
    { 1.35f, 1.29f, 1.27f, 1.28f, 1.34f, 1.36f, 1.17f, 1.24f, 1.21f, 1.28f,
1.27f, 1.19f, 1.17f };
static const float PCP_OFF[13] =
    { 22, 21, 23, 22, 19, 16, 12, 14, 11, 8, 2, 1, 8 };
static const float PCP_H[13] =
    { 0.28f, 0.25f, 0.25f, 0.24f, 0.22f, 0.24f, 0.20f, 0.20f, 0.18f, 0.21f,
0.32f, 0.49f, 0.51f };
static const float PCP_VOFF[13] =
    { -2, -1, -2, -2, 1, 3, 6, 3, 1, 0, -1, -5, -8 };
static float PcpAt(const float *tb, float t)
{
    float x = t / PCP_DT;
    if (!(x > 0.0f))
        return tb[0];
    if (x >= 12.0f)
        return tb[12];
    int i = (int) x;
    float f = x - (float) i;
    return tb[i] + (tb[i + 1] - tb[i]) * f;
}

#define PCP_NC 7
static struct {
    int on, role, side, chosen, init, uinit, pend;
    uint64_t job[PCP_NC][2];
    uint32_t kick_f;
    double kick_ms;
    float shrink, lift, u[2], pos[3], look[3];
    int nlog;                   // lift: m over H when every spot was blocked
    uint64_t cj;
    uint32_t cj_f;
    float cl;
    int cl_n;                   // pulfix: the per-frame lens collision (subject -> lens ray, arm clamp)
} g_pulp;
// pulfix (the victim camera clipped through the truck a lot, which does not happen in SP, where the victim camera is over the right
// shoulder looking at the grabber): A frames 260-336 (4.3-5.6 s) the victim lens sat inside the truck's chassis - log A [16457] / [16480] / [16650] "every spot
// blocked - lift +0.45 / +0.90 m, arm x0.30": the victim lens rode 0.85 m over the prone root (a truck's underside is ~0.5-0.7 m up) and every blocked batch
// LIFTED it another 0.45 m - straight into the vehicle. SP (the SP sandbox recorder log, episode 5, ellie-pullout-prone-bw--ellie, CAM columns, measured
// against the player root and the NPC): the gameplay camera stays LOW and close behind the victim's head, on its RIGHT shoulder, aimed at the victim with the
// puller in frame: horizontal 0.9 -> 1.3-1.37 -> 1.2 m from the root, 0.37 m up at the grab -> 0.19 at 1.9 s -> 0.02-0.11 m at the drag's end, 18-37 deg off
// straight behind to the victim's right, look ~8 deg past the victim (the puller ~13-20 deg the other side of centre), pitch -10 -> +13 deg as the puller
// closes (puller-chest elevation - ~14 deg), fov 70 -> 73.7. Victim role now: that shot (side = our right shoulder, from the shoulder joints), no lift ever
// (a blocked victim spot pulls the arm IN, under the vehicle, never up into it), and for both roles a per-frame subject -> lens ray clamps the arm the
// frame after a hit (the 0.4 s spot batches alone let the lens pass through the chassis between batches). t2r-pullout-vcam-old.txt = the old victim shot.
static int PulVcamOld(void)
{
    static int v = -1;
    static uint64_t at;
    return PulFlag("t2r-pullout-vcam-old.txt", &v, &at);
}
static const float PVC_H[10] = { 0.37f, 0.33f, 0.26f, 0.20f, 0.16f, 0.13f, 0.12f, 0.15f, 0.30f, 0.45f };        // SP lens height over the victim root, every 0.45 s (floored at 0.12 m: grass / near clip; the outcome rises to SP struggle / success 0.3-0.5)
static const float PVC_D[10] = { 0.95f, 1.15f, 1.30f, 1.35f, 1.30f, 1.22f, 1.22f, 1.20f, 1.18f, 1.18f };        // SP horizontal arm from the victim root, every 0.45 s

// pulfix (match SP 1:1 from the recorded camera data): the VICTIM camera no longer FITS curves to SP
// (the PVC_* / 18-37 deg shot above = t2r-pullout-vcam-fit.txt) - it REPLAYS SP's recorded camera frame by frame. tools/sp_pullout_cam.py turns the SP
// sandbox recorder's render-camera columns (camx..cqw = the CameraLocation pos + quat of the render camera hook, fov = camera-get-fov-degrees) of the two
// pull-out episodes (SP recorder log: ep 5 bw drag -> fail, ep 8 left drag -> success) into pullout_spcam.h; tools/sp_pullout_cam_check.py
// re-applies the header to SP's own anchors: every frame within 0.2 mm / 0.01 deg of what SP rendered.
//  ANCHOR  position = our live root (SP's camera is the victim's own gameplay camera, it orbits that body), axes = the pair ap's yaw (psync_ap, the same
//          upright frame SP's #MSTART ap gives). Not the ap alone: SP solves it on the attacker and plays the defender 0.35-0.37 m further along the
//          drag line than the authored align for the whole bw drag, psync puts our body ON the authored align - camera-to-ap would sit 0.37 m off us.
//          Not our root's yaw: the body rolls over 180 deg mid-drag, SP's camera does not.
//  TIME    the pair clock (psync_ms - t0 + chain_shift = s into the clip), SP's keys are its victim clip's phase x duration - the same clip names.
//  ROWS    pu-drag-<sex> (bw) = SP's feet drag (ellie-pullout-prone-bw--ellie, exact); pu-drag-l = SP's left drag (exact); pu-drag-r = the left
//          recording MIRRORED (x, view x, roll) and time-scaled by the clip lengths (no SP recording; t2r-pullout-vcam-nomirror.txt = the fitted shot);
//          pu-drag-fw = no recording -> the fitted shot; pu-ok / pu-fail = SP's success / fail (struggle-success / -finisher). Abby rows play
//          Ellie's recording (the same choreography). SP's 4.7 s struggle has no MP pair (left out) - its track is in the header, unused.
//  BLENDS  measured in the same frames: in = PSC_BLEND_IN_S smoothstep from our live camera (SP's gameplay camera needs 1.24 s into the drag to reach
//          the pull-out framing / lens); success = PSC_XFADE_OK_S crossfade from the drag's last frame (SP: continuous, re-settles in 0.24 s); fail =
//          a CUT (SP: the finisher camera jumps 80 deg / 0.83 m in one frame); out = PSC_OUT_S from PSC_OUT_AT_S of the success clip.
//  WALLS   SP's camera shows no pull-in in these frames (the arm never changes faster than 1.8 m/s; both episodes are at the same truck, so that is not
//          proof it never collides) - the per-frame lens ray (0acc3837) stays, as a PULL-IN only where the recorded spot is blocked here.
// Log "pullout cam: SP REPLAY ...". t2r-pullout-vcam-fit.txt = the fitted victim shot (and t2r-pullout-vcam-old.txt the one before it).
#include "pullout_spcam.h"
#define PSC_OUT_AT_S 1.68f      // SP: the success camera holds its last pose until 1.68 s of the clip, then hands back over PSC_OUT_S (sp_pullout_cam.py)
static int PulVcamFit(void)
{
    static int v = -1;
    static uint64_t at;
    return PulFlag("t2r-pullout-vcam-fit.txt", &v, &at);
}

static int PulVcamNomirror(void)
{
    static int v = -1;
    static uint64_t at;
    return PulFlag("t2r-pullout-vcam-nomirror.txt", &v, &at);
}

// [SP ep5/ep8] (re/review/pullout-1031/sp-cam-1111.md): the SP recording is the VICTIM's camera only (SP's player is always the one pulled; the NPC
// pulls), so the attacker's ps45 prone camera (S45P_*) has no SP data and stays as is. The victim replay above covered only a sequence that STARTS with a drag:
// a pounce (the grapple pair first) and ps45 variant a (the prone-kill first, no SP recording) ran the fitted shot through the grapple and the outcome, although
// pu-grap plays the recorded clip (ellie-pullout-struggle--ellie = SP's PSC_STRUGGLE5 / 8). Now: a pu-grap row maps to SP's ep 5 struggle track, and a fitted
// victim shot that reaches the grapple / outcome pair joins SP's replay with a PSC_BLEND_IN_S (SP: 1.24 s) smoothstep from the shown camera.
// t2r-prone-cam-sp-old.txt = the old behaviour (fitted shot to the end). Proof line "pullout cam: SP 10-31 prone".
static int PulProneCamSpOld(void)
{
    static int v = -1;
    static uint64_t at;
    return PulFlag("t2r-prone-cam-sp-old.txt", &v, &at);
}

static struct {
    int on, trk, mir, nlog, xf_on, have, ph;    // trk = the PSC_TRACKS entry playing, mir = mirrored (r), have = out_* valid
    float ts, xf_s;
    double xf_t0, log_ms;
    float fy, fp;
    int fn;                     // the attacker-framing correction (yaw / pitch, deg, smoothed)
    float xf_p[3], xf_l[3], xf_r, xf_f, out_p[3], out_l[3], out_r, out_f;
    long lp_n;
    float lp_max;
    double lp_log;
    int lp_nl;                  // ps46 item 2: the lens push (frames, max m, last log ms, log count)
} g_psc = {.trk = -1 };

static int PulContOld(void);  // ps46 item 1 (the note at PUL_VEND_MAXD)
#define PUL_LPUSH_PAD 0.10f     /* [TUNING ps46] m clearance round the attacker's upper arms / thighs (as xcam_cine_push's +0.10) */
#define PUL_LPUSH_MAX 0.60f     /* [TUNING ps46] m the longest step back along the view */
// the recording for a pull-out row (-1 = none): *mir = mirror x, *ts = track seconds per pair second
static int PscPick(int row, int *mir, float *ts)
{
    const char *n = CD_PAIRS[row].name;
    *mir = 0;
    *ts = 1.0f;
    if (!strncmp(n, "pu-ok", 5))
        return PSC_OK;
    if (!strncmp(n, "pu-fail", 7))
        return PSC_FAIL;
    if (!strncmp(n, "pu-grap-", 8))
        return PulProneCamSpOld()? -1 : PSC_STRUGGLE5;      // [SP ep5/ep8] the grapple = SP's struggle clip (the note at pul_prone_cam_sp_old)
    if (!strncmp(n, "pu-drag-l-", 10))
        return PSC_DRAG_L;
    if (!strncmp(n, "pu-drag-fw-", 11))
        return -1;
    if (!strncmp(n, "pu-drag-r-", 10)) {
        if (PulVcamNomirror())
            return -1;
        float lr = AnimLen(SID_PLAYER, CD_VIC0(row));
        if (!(lr > 0.5f && lr < 10.0f))
            lr = CD_PAIRS[row].end_s + 0.13f;   // (pullout_rows.h: hand-off = clip length - 0.13 s)
        *mir = 1;
        *ts = PSC_TRACKS[PSC_DRAG_L].len / lr;
        return PSC_DRAG_L;
    }
    if (!strncmp(n, "pu-drag-", 8))
        return PSC_DRAG_BW;
    return -1;
}

// the track at time t (s): p (camera - victim root), l (view), roll, fov, v (victim root in the ap), all in the ap's axes - linear between SP's raw frames
static void PscEval(const psc_track *tk, float t, int mir, float *p,
                     float *l, float *roll, float *fov, float *v)
{
    const psc_key *k = tk->k, *a, *b;
    int n = tk->n;
    float f = 0.0f;
    if (t <= k[0].t)
        a = b = &k[0];
    else if (t >= k[n - 1].t)
        a = b = &k[n - 1];
    else {
        int lo = 0, hi = n - 1;
        while (hi - lo > 1) {
            int mid = (lo + hi) / 2;
            if (k[mid].t <= t)
                lo = mid;
            else
                hi = mid;
        }
        a = &k[lo];
        b = &k[hi];
        f = (t - a->t) / (b->t - a->t);
    }
    for (int i = 0; i < 3; i++) {
        p[i] = a->p[i] + (b->p[i] - a->p[i]) * f;
        l[i] = a->l[i] + (b->l[i] - a->l[i]) * f;
        v[i] = a->v[i] + (b->v[i] - a->v[i]) * f;
    }
    float ll = __builtin_sqrtf(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
    if (ll > 1e-4f)
        for (int i = 0; i < 3; i++)
            l[i] /= ll;
    *roll = a->roll + (b->roll - a->roll) * f;
    *fov = a->fov + (b->fov - a->fov) * f;
    if (mir) {
        p[0] = -p[0];
        l[0] = -l[0];
        v[0] = -v[0];
        *roll = -*roll;
    }
}

static void PscYawrot(float c, float s, const float *a, float *o)
{
    o[0] = a[0] * c + a[2] * s;
    o[1] = a[1];
    o[2] = -a[0] * s + a[2] * c;
}                               // about +y: (0,0,1) -> (sin, 0, cos), sp_pullout_cam.py yawrot

static void PscHold(float *C, float *L, const char *why)
{                               // no live pair (the gap before the outcome, the release): the last shown pose
    memcpy(C, g_psc.out_p, 12);
    memcpy(L, g_psc.out_l, 12);
    if (g_psc.ph != -2 && LOGCAP(g_psc.nlog, 60))
        PULLOG("pullout cam: SP REPLAY holds its last frame (%s)", why);
    g_psc.ph = -2;
}

static int PscFrame(const float *me, const float *pv, float dt)
{
    int want = g_pulv.out == 1 ? PSC_OK : g_pulv.out == 2 ? PSC_FAIL : g_pulv.grap == 2 ? (g_psc.trk == PSC_DRAG_L || g_psc.trk == PSC_STRUGGLE8 ? PSC_STRUGGLE8 : PSC_STRUGGLE5) : g_psc.trk;  // SP's struggle recording through the grapple
    int live = g_ps.role == 2 && g_ps.kind == PS_CTXDOWN
        && g_ps.uid == g_pulv.uid;
    double now = PsyncMs();
    if (want != g_psc.trk && live) {    // the outcome pair started on us: SP's success crossfades (re-settles), SP's fail cuts
        int cut = want == PSC_FAIL ? PSC_CUT_FAIL : PSC_CUT_OK;
        PULLOG
            ("pullout cam: SP REPLAY -> %s (SP ep %d, %d keys, %.2f s): %s",
             PSC_TRACKS[want].name, PSC_TRACKS[want].ep,
             PSC_TRACKS[want].n, (double) PSC_TRACKS[want].len,
             cut ?
             "a CUT as in SP (the finisher camera jumps in one frame)" :
             want == PSC_STRUGGLE5
             || want ==
             PSC_STRUGGLE8 ?
             "the grapple: crossfade from the drag's last frame" :
             "crossfade from the grapple's / drag's last frame as SP's camera re-settles");
        g_psc.xf_on = !cut && g_psc.have;
        g_psc.xf_t0 = now;
        g_psc.xf_s = PSC_XFADE_OK_S;
        if (g_psc.xf_on) {
            memcpy(g_psc.xf_p, g_psc.out_p, 12);
            memcpy(g_psc.xf_l, g_psc.out_l, 12);
            g_psc.xf_r = g_psc.out_r;
            g_psc.xf_f = g_psc.out_f;
        }
        g_psc.trk = want;
        g_psc.mir = 0;
        g_psc.ts = 1.0f;
    }
    float C[3], L[3], roll = g_psc.out_r, fov = g_psc.out_f;
    int kroll = live && PulKillRow(CdPairOfMove(g_ps.move));       // ps45 b: the prone-kill roll between the drag and the grapple (no SP recording): the last frame holds
    if (!live || want != g_psc.trk || kroll) {
        if (!g_psc.have)
            return 0;
        PscHold(C, L,
                 !live ? "no pull-out pair on us" : kroll ?
                 "the prone-kill roll (ps45 b: no SP recording)" :
                 "the outcome pair is not ours yet");
    } else {
        const float *ap = g_ps.ap;
        float fx = 2.0f * (ap[3] * ap[5] + ap[6] * ap[4]), fz =
            1.0f - 2.0f * (ap[3] * ap[3] + ap[4] * ap[4]), fl =
            __builtin_sqrtf(fx * fx + fz * fz);
        if (fl < 1e-4f) {
            if (!g_psc.have)
                return 0;
            PscHold(C, L, "degenerate ap");
            goto out;
        }
        float cy = fz / fl, sy = fx / fl;
        float tc =
            (float) ((now - g_ps.t0 + g_ps.chain_shift) / 1000.0), t =
            tc * g_psc.ts;
        float p[3], l[3], v[3], wp[3], wv[3];
        PscEval(&PSC_TRACKS[g_psc.trk], t, g_psc.mir, p, l, &roll, &fov,
                 v);
        PscYawrot(cy, sy, p, wp);
        PscYawrot(cy, sy, l, L);
        int rooted = PsLocOk(me);
        if (rooted)
            for (int i = 0; i < 3; i++)
                C[i] = me[i] + wp[i];
        else {
            PscYawrot(cy, sy, v, wv);
            for (int i = 0; i < 3; i++)
                C[i] = ap[i] + wv[i] + wp[i];
        }                       // (fallback anchor: SP's victim spot in the ap)
        if (g_psc.xf_on) {
            float u =
                (float) ((now - g_psc.xf_t0) / 1000.0) / (g_psc.xf_s >
                                                          0.01f ? g_psc.
                                                          xf_s : 0.01f);
            if (u >= 1.0f)
                g_psc.xf_on = 0;
            else {
                u = u * u * (3.0f - 2.0f * u);
                // ps46 item 1: the view is SLERPED (B camcont [10660] 'rotation jump 169.6 deg in 1 frame': the normalised lerp of two near-opposite views passes
                // through zero and flips in one frame); t2r-prone-cam-cont-ps46-old.txt = the lerp
                float d0 =
                    g_psc.xf_l[0] * L[0] + g_psc.xf_l[1] * L[1] +
                    g_psc.xf_l[2] * L[2];
                d0 = d0 > 1.0f ? 1.0f : d0 < -1.0f ? -1.0f : d0;
                float th = __builtin_acosf(d0), sn = __builtin_sinf(th);
                int sl = !PulContOld() && th > 0.02f && sn > 0.02f;
                float ka =
                    sl ? __builtin_sinf((1.0f - u) * th) / sn : 1.0f - u,
                    kb = sl ? __builtin_sinf(u * th) / sn : u;
                for (int i = 0; i < 3; i++) {
                    C[i] = g_psc.xf_p[i] + (C[i] - g_psc.xf_p[i]) * u;
                    L[i] = g_psc.xf_l[i] * ka + L[i] * kb;
                }
                float ll =
                    __builtin_sqrtf(L[0] * L[0] + L[1] * L[1] +
                                    L[2] * L[2]);
                if (ll > 1e-4f)
                    for (int i = 0; i < 3; i++)
                        L[i] /= ll;
                if (sl && th > 1.0f && LOGCAP(g_psc.fn, 20))
                    PULLOG
                        ("ps46 cam cont: SP REPLAY crossfade slerps the view (%.0f deg between the drag's last frame and the grapple's track)",
                         (double) (th * 57.29578f));
                roll = g_psc.xf_r + (roll - g_psc.xf_r) * u;
                fov = g_psc.xf_f + (fov - g_psc.xf_f) * u;
            }
        }
        if (!XvFin3(C) || !XvFin3(L)) {
            if (!g_psc.have)
                return 0;
            PscHold(C, L, "non-finite pose");
            roll = g_psc.out_r;
            fov = g_psc.out_f;
            goto out;
        }
        memcpy(g_psc.out_p, C, 12);
        memcpy(g_psc.out_l, L, 12);
        g_psc.out_r = roll;
        g_psc.out_f = fov;
        g_psc.have = 1;
        if (g_psc.ph != g_psc.trk || now - g_psc.log_ms >= 500.0) {
            g_psc.ph = g_psc.trk;
            g_psc.log_ms = now;
            if (LOGCAP(g_psc.nlog, 60))
                PULLOG
                    ("pullout cam: SP REPLAY %s%s t %.2f s (pair %.2f): cam %.2f %.2f %.2f = %s + (%.2f %.2f %.2f) in the ap (yaw %.0f), view %.2f %.2f %.2f roll %.1f fov %.1f, lens pull-in x%.2f%s",
                     PSC_TRACKS[g_psc.trk].name,
                     g_psc.mir ? " (mirrored)" : "", (double) t,
                     (double) tc, (double) C[0], (double) C[1],
                     (double) C[2],
                     rooted ? "our root" : "ap + SP victim spot",
                     (double) p[0], (double) p[1], (double) p[2],
                     (double) (__builtin_atan2f(sy, cy) * 57.29578f),
                     (double) L[0], (double) L[1], (double) L[2],
                     (double) roll, (double) fov,
                     (double) (g_pulp.cl > 0.0f ? g_pulp.cl : 1.0f),
                     g_psc.xf_on ? " (crossfading)" : "");
        }
    }
  out:;
    // the lens ray (0acc3837) as a PULL-IN only: subject -> recorded spot every frame (read the next frame); a hit pulls the lens in to 90 % of it at once, a clear ray
    // lets it back out to the recorded spot over ~0.3 s. 0x1b07 = walls / vehicles, not grass; our own body ignored. t2r-pullout-camclamp-off.txt = off.
    float Cf[3];
    memcpy(Cf, C, 12);
    // (camera clipping): the 1:1 replay ran through the truck's chassis (escape re-settle, cam y 13.40 under the chassis) and
    // through both bodies (the attacker's legs, our own back). Back ON by default (t2r-pullout-spclamp-off.txt = the 1:1 replay):
    // (1) bodies: the lens is pushed out of a capsule round each body (attacker pelvis-head 0.35 m, ours 0.26 m) - the ray mask has no characters;
    // (2) world: ray from our CHEST (the old pelvis + 0.2 origin sat in the ground / chassis under the truck: hit at 0.00 every few frames = the
    //     pumping), a hit in the first 6 % of the arm = the origin is inside geometry -> ignored, not a pull-in. Pull in at once, ease out ~0.3 s.
    if (GetFileAttributesA("t2r-pullout-spclamp-off.txt") ==
        INVALID_FILE_ATTRIBUTES
        && GetFileAttributesA("t2r-pullout-camclamp-off.txt") ==
        INVALID_FILE_ATTRIBUTES) {
        float sp[3], sc[3], sh[3];
        XvBody(SID_PLAYER, me, sp, sc, sh);
        float ap_[3], ac_[3], ah_[3];
        int att_ok = 0;
        if (pv && XvFin3(pv)) {
            XvBody(g_xcam_h, pv, ap_, ac_, ah_);
            att_ok = XvFin3(ap_) && XvFin3(ah_);
        }
        for (int pass = 0; pass < 2; pass++) {  // push out of the attacker's capsule, then ours (twice: a push out of one can enter the other)
            for (int w = 0; w < 2; w++) {
                const float *c0 = w == 0 ? ap_ : sp, *c1 =
                    w == 0 ? ah_ : sh;
                float rad = w == 0 ? 0.35f : 0.26f;
                if (w == 0 && !att_ok)
                    continue;
                if (w == 1 && (!XvFin3(sp) || !XvFin3(sh)))
                    continue;
                float d[3] =
                    { c1[0] - c0[0], c1[1] - c0[1], c1[2] - c0[2] }, e[3] =
                    { C[0] - c0[0], C[1] - c0[1], C[2] - c0[2] };
                float dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2], u =
                    dd >
                    1e-4f ? (e[0] * d[0] + e[1] * d[1] +
                             e[2] * d[2]) / dd : 0.0f;
                if (u < 0.0f)
                    u = 0.0f;
                else if (u > 1.0f)
                    u = 1.0f;
                float q[3] =
                    { c0[0] + d[0] * u, c0[1] + d[1] * u,
               c0[2] + d[2] * u }, r[3] =
{ C[0] - q[0], C[1] - q[1], C[2] - q[2] };
                float rl =
                    __builtin_sqrtf(r[0] * r[0] + r[1] * r[1] +
                                    r[2] * r[2]);
                if (rl < rad) {
                    if (rl < 1e-3f) {
                        r[0] = -L[0];
                        r[1] = -L[1];
                        r[2] = -L[2];
                        rl = 1.0f;
                    }           // (dead centre: back along the view)
                    for (int i = 0; i < 3; i++)
                        C[i] = q[i] + r[i] / rl * rad;
                    if (LOGCAP(g_pulp.cl_n, 40))
                        PULLOG
                            ("pullout cam: SP REPLAY lens was %.2f m inside %s body - pushed out to %.2f m",
                             (double) (rad - rl),
                             w == 0 ? "the attacker's" : "our",
                             (double) rad);
                }
            }
        }
        memcpy(Cf, C, 12);
        float s1[3] = { sc[0], sc[1] + 0.1f, sc[2] };
        if (XvFin3(s1)) {
            if (g_pulp.cl <= 0.0f)
                g_pulp.cl = 1.0f;
            if (g_pulp.cj && g_t60 != g_pulp.cj_f) {
                float tr = RayT(g_pulp.cj);
                g_pulp.cj = 0;
                float want2 = (!g_rayErr && tr >= 0.06f
                               && tr < 1.0f) ? tr * 0.9f : 1.0f;
                if (want2 < 0.15f)
                    want2 = 0.15f;
                if (want2 < g_pulp.cl) {
                    if (g_pulp.cl - want2 > 0.1f
                        && LOGCAP(g_pulp.cl_n, 40))
                        PULLOG
                            ("pullout cam: SP REPLAY lens ray hit at %.2f of the arm - the recorded spot is blocked here, pulled in",
                             (double) tr);
                    g_pulp.cl = want2;
                } else
                    g_pulp.cl += (want2 - g_pulp.cl) * XcamK(0.06f, dt);
            }
            if (!g_pulp.cj) {
                g_pulp.cj = WorldRayKickM(s1, C, SID_PLAYER, 0x1b07);
                g_pulp.cj_f = g_t60;
            }
            for (int i = 0; i < 3; i++)
                Cf[i] = s1[i] + (C[i] - s1[i]) * g_pulp.cl;
        }
    }
    // ps46 item 2 (ps45a sheets B 5.93-6.70 s (the fail replay, B [10718] PSC_FAIL) and 35.50-36.60 s: the attacker's sleeve / jacket / backpack / hip fill the lens;
    // B.log has no 'lens was ... inside' line in the whole recording - the pelvis-head capsule above (r 0.35) never fired: the arms and legs are outside it and our
    // attacker is ~9 % taller than SP's NPC). The SHOWN lens (after the world pull-in) is pushed out of both bodies with xcam_cine_push (the ps45 X2 capsules: heads,
    // torsos +0.10 m, the attacker's forearms / hands) plus the attacker's upper arms and thighs, back along the view (the framing kept), at most PUL_LPUSH_MAX.
    // The held frames (psc_hold: between pairs and at the release, the source of the hand-back blend) pass here too. t2r-prone-lenspush-ps46-old.txt = off.
    // Proof line "ps46 lens push".
    {
        static int lv = -1;
        static uint64_t lat;
        if (!PulFlag("t2r-prone-lenspush-ps46-old.txt", &lv, &lat) && pv
            && XvFin3(pv) && PsLocOk(me)) {
            float up_[3], uc_[3], uh_[3], op_[3], oc_[3], oh_[3];
            XvBody(SID_PLAYER, me, up_, uc_, uh_);
            XvBody(g_xcam_h, pv, op_, oc_, oh_);
            float ll =
                __builtin_sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
            if (XvFin3(up_) && XvFin3(uc_) && XvFin3(uh_)
                && XvFin3(op_) && XvFin3(oc_) && XvFin3(oh_)
                && ll > 1e-3f) {
                float F[3] = { L[0] / ll, L[1] / ll, L[2] / ll }, C0[3] =
                    { Cf[0], Cf[1], Cf[2] };
                float aim[3] =
                    { Cf[0] + F[0] * 1.7f, Cf[1] + F[1] * 1.7f,
             Cf[2] + F[2] * 1.7f };
                float pc =
                    XcamCinePush(Cf, aim, g_xcam_h, up_, uc_, uh_, oc_,
                                   oh_, op_);
                // the limbs xcam_cine_push leaves out: the attacker's upper arms (shoulder -> elbow) and thighs (pelvis -> knee)
                static const char *const JA[4][2] =
                    { { "l_shoulder", "l_elbow" }, { "r_shoulder",
                                                    "r_elbow" },
                    { "pelvis", "l_knee" }, { "pelvis", "r_knee" } };
                static const float JR[4] =
                    { 0.10f + PUL_LPUSH_PAD, 0.10f + PUL_LPUSH_PAD,
 0.13f + PUL_LPUSH_PAD, 0.13f + PUL_LPUSH_PAD };
                float ja[4][3], jb[4][3];
                int jok[4];
                for (int k = 0; k < 4; k++)
                    jok[k] = GxJointPos(g_xcam_h, JA[k][0], ja[k])
                        && GxJointPos(g_xcam_h, JA[k][1], jb[k])
                        && XvFin3(ja[k]) && XvFin3(jb[k]);
                for (int it = 0; it < 16 && pc < PUL_LPUSH_MAX; it++) {
                    int in = 0;
                    for (int k = 0; k < 4 && !in; k++)
                        if (jok[k]
                            && XcamSegDist(Cf, ja[k], jb[k]) < JR[k])
                            in = 1;
                    if (!in)
                        break;
                    for (int i = 0; i < 3; i++)
                        Cf[i] -= F[i] * 0.05f;
                    pc += 0.05f;
                }
                if (pc > PUL_LPUSH_MAX) {
                    for (int i = 0; i < 3; i++)
                        Cf[i] = C0[i] - F[i] * PUL_LPUSH_MAX;
                    pc = PUL_LPUSH_MAX;
                }
                if (pc > 0.0f) {
                    g_psc.lp_n++;
                    if (pc > g_psc.lp_max)
                        g_psc.lp_max = pc;
                    double nw = PsyncMs();
                    if (nw - g_psc.lp_log >= 1000.0
                        && LOGCAP(g_psc.lp_nl, 40)) {
                        g_psc.lp_log = nw;
                        PULLOG
                            ("ps46 lens push: SP REPLAY%s lens %.2f m inside a body (heads / torsos / the attacker's arms / legs) - stepped back along the view, %ld frame(s) so far, max %.2f m",
                             g_psc.ph == -2 ? " (held frame)" : "",
                             (double) pc, g_psc.lp_n,
                             (double) g_psc.lp_max);
                    }
                }
            }
        }
    }
    // (the attacker was not framed): SP's recorded track is anchored on OUR root, but in MP the puller does not stand where
    // SP's NPC stood (the attacker half is solved on the attacker, the victim half placed on the authored align) - the puller drifted to the frame's edge / out
    // of it. Keep SP's shot and turn the view only as far as needed to hold the puller's chest within 22 deg (yaw) / 14 deg (pitch) of the centre, smoothed
    // (0.15 s). t2r-pullout-vframe-off.txt = off.
    if (pv && XvFin3(pv)
        && GetFileAttributesA("t2r-pullout-vframe-off.txt") ==
        INVALID_FILE_ATTRIBUTES) {
        float op[3], oc[3], oh[3];
        XvBody(g_xcam_h, pv, op, oc, oh);
        float ox = oc[0] - Cf[0], oy = oc[1] - Cf[1], oz =
            oc[2] - Cf[2], oh2 = __builtin_sqrtf(ox * ox + oz * oz);
        float wy = 0.0f, wp = 0.0f;
        if (XvFin3(oc) && oh2 > 0.4f && oh2 < 8.0f) {
            float lh = __builtin_sqrtf(L[0] * L[0] + L[2] * L[2]);
            float dy =
                XvWrap((__builtin_atan2f(ox, oz) -
                         __builtin_atan2f(L[0], L[2])) * 57.2957795f);
            float dp =
                (__builtin_atan2f(oy, oh2) -
                 __builtin_atan2f(L[1],
                                  lh > 1e-4f ? lh : 1e-4f)) * 57.2957795f;
            wy = dy > 22.0f ? dy - 22.0f : dy < -22.0f ? dy + 22.0f : 0.0f;
            wp = dp > 14.0f ? dp - 14.0f : dp < -14.0f ? dp + 14.0f : 0.0f;
        }
        float k = XcamK(0.15f, dt);
        g_psc.fy += (wy - g_psc.fy) * k;
        g_psc.fp += (wp - g_psc.fp) * k;
        if ((__builtin_fabsf(g_psc.fy) > 3.0f
             || __builtin_fabsf(g_psc.fp) > 3.0f) && LOGCAP(g_psc.fn, 20))
            PULLOG
                ("pullout cam: SP REPLAY framing: the puller is off the shot - view turned %.1f deg yaw, %.1f deg pitch to hold it in frame",
                 (double) g_psc.fy, (double) g_psc.fp);
        if (__builtin_fabsf(g_psc.fy) > 0.01f
            || __builtin_fabsf(g_psc.fp) > 0.01f) {
            float lh = __builtin_sqrtf(L[0] * L[0] + L[2] * L[2]);
            float yw =
                __builtin_atan2f(L[0], L[2]) + g_psc.fy * 0.0174532925f,
                pt =
                __builtin_atan2f(L[1],
                                 lh >
                                 1e-4f ? lh : 1e-4f) +
                g_psc.fp * 0.0174532925f;
            if (pt > 1.2f)
                pt = 1.2f;
            else if (pt < -1.2f)
                pt = -1.2f;
            L[0] = __builtin_sinf(yw) * __builtin_cosf(pt);
            L[1] = __builtin_sinf(pt);
            L[2] = __builtin_cosf(yw) * __builtin_cosf(pt);
        }
    }
    for (int i = 0; i < 3; i++) {
        g_xcamPos[i] = Cf[i];
        g_xcam_look[i] = L[i];
    }
    g_xcam_valid = 1;
    g_xcam_th = __builtin_atan2f(L[0], L[2]) * 57.2957795f;
    g_xcamRoll = roll;
    g_xcamFovT = fov;         // SP's own roll and lens (hfov; the render filter scales it for the user's FOV, mf_scale)
    return 1;
}

static void PulProcBegin(int role, int slot, const char *why)
{
    memset(&g_pulp, 0, sizeof g_pulp);
    g_psc.on = 0;
    g_pulp.on = 1;
    g_pulp.role = role;
    g_pulp.shrink = 1.0f;
    g_pulp.side = 0;
    g_pulp.chosen = -1;
    g_xcamTrk = NULL;
    g_xcam_ncand = 0;
    g_xcam_choice_state = 2;
    g_xcamRoll = 0.0f;
    g_xcamPulproc = 1;
    g_xcamSpbl = 1;
    // an SP shot already on screen (the victim gets the attacker's CAM 0 after its own shot started) must not drop back to the gameplay
    // camera for the new swing-in: blend from the frozen last shown camera (v5-cam6 g_xfz), as a downed start does
    if (g_xcamShown) {
        g_xfz_on = 1;
        g_xfz_gen++;
        g_xfz_used = 0;
        g_xfz_n = 0;
    }
    g_xph.on = 0;
    g_xph.last_ok = 0;
    g_xcam_valid = 0;
    g_xcamShown = 0;
    // blends: the victim's SP swing (2.7 s smoothstep) for BOTH roles, then the 1.5 s ease out; the duration grows by the longer out
    float bin =
        GetFileAttributesA("t2r-pullout-camswing-off.txt") ==
        INVALID_FILE_ATTRIBUTES ? (PulCamV2()? PUL_V2_SWING_S :
                                   PUS_SWING_S) : 0.45f;
    float old_out = g_xcam_bout_f;
    g_xcamBinF = bin * 60.0f;
    g_xcam_bout_f = 90.0f;
    if (g_xcam_bout_f > old_out)
        g_xcam_dur += (uint64_t) (g_xcam_bout_f - old_out);
    g_pulc.on = 1;
    g_pulc.slot = slot;
    g_pulc.role = role;
    g_pulc.judged = 1;
    g_pulc.dropped = 0;
    g_pulc.t0 = GetTickCount64();
    PULLOG
        ("pullout cam: PROCEDURAL camera, role %d (%s): SP victim composition (camera 3 of the drag): %.2f m behind the subject drifting %.0f -> %.0f deg off the drag line, %.2f m up at the start (x%s), "
         "victim voff %.0f deg; swing-in %.1f s, fov x0.95 -> x1, out 1.5 s",
         role, why, (double) PCP_D[0], (double) PCP_OFF[0],
         (double) PCP_OFF[12], (double) PCP_H[0],
         role ==
         1 ?
         " the puller's mirror: 1.15 m + 0.8 x rise, aim 70 % to the victim"
         : "", (double) PCP_VOFF[0], (double) bin);
}

// (the prone pull-out / grapple on the ATTACKER's screen in tall grass - the camera far behind and high; wanted: SP-style, over the
// shoulder, close, the victim's upper body / head in frame through the grab and the grapple, smooth in and out). SP has no attacker camera for this pair (the
// NPC pulls in SP; pullout_cam_tracks.h holds the player = victim halves' cameras only), so the attacker keeps the procedural camera (from the intro, v2), keyed
// on the pull-out system (g_xcam_pulproc), not on a clip name - the A / B / C variants of kind 8 get it too. Its v2 composition was 1.55 + 0.35 x gap m
// behind us (up to 2.4 m), 33-54 deg off straight behind, the lens at our chest + 0.3 + up to 0.9 m of 'every spot blocked' lift, aimed 65 % of the way
// to the victim's CHEST lowered 0.25 m. Now: S45P_D (+0.15 x gap) behind us = the ps45 A1 body clearance, S45P_UP over our head (over the shoulder), the
// lift capped at S45P_LIFT, aimed S45P_AIM of the way from our chest to the victim's upper body (chest-head midpoint); the side angle starts at S45P_OFF0
// and widens 8 deg at a time (to S45P_OFFMAX, eased) while the line to the victim's head or chest crosses our torso (ps45 A1's occlusion rule), narrows
// back when clear. The swing-in (PUL_V2_SWING_S smoothstep from the live camera), the collision candidates / rays and the 1.5 s hand-back are unchanged.
// t2r-ps45-cam-prone-old.txt = the v2 composition. Proof line "pullout cam: ps45". UNVERIFIED: no pull-out in the recording; the numbers are [TUNING ps45].
#define S45P_D 1.25f            /* m behind us (horizontal) [TUNING ps45] */
#define S45P_UP 0.10f           /* m over our head joint [TUNING ps45] */
#define S45P_LIFT 0.30f         /* m [TUNING ps45] */
#define S45P_AIM 0.80f          /* [TUNING ps45] */
#define S45P_OFF0 26.0f         /* deg off straight behind [TUNING ps45] */
#define S45P_OFFMAX 66.0f       /* deg [TUNING ps45] */
static struct {
    double ms0;
    float off, tgt;
    long n, nw;
    float mx;
    uint64_t lt;
} g_s45p;
static int PulProcFrame(const float *me, const float *pv, float dt,
                          float t)
{
    if (!g_pulp.on)
        return 0;
    if (g_psc.on)
        return PscFrame(me, pv, dt);   // pulfix: the victim camera replays SP's recording (above)
    float sp[3], sc[3], sh[3], op[3], oc[3], oh[3];
    XvBody(SID_PLAYER, me, sp, sc, sh);
    XvBody(g_xcam_h, pv, op, oc, oh);
    int att = g_xcam_role == 1;
    // subject -> other line (horizontal), held while the bodies are on top of each other
    float dx = op[0] - sp[0], dz = op[2] - sp[2], dl =
        __builtin_sqrtf(dx * dx + dz * dz);
    if (!g_pulp.uinit) {
        if (dl > 0.2f) {
            g_pulp.u[0] = dx / dl;
            g_pulp.u[1] = dz / dl;
        } else {
            g_pulp.u[0] = -g_xcam_gdir[0];
            g_pulp.u[1] = -g_xcam_gdir[2];
            float l =
                __builtin_sqrtf(g_pulp.u[0] * g_pulp.u[0] +
                                g_pulp.u[1] * g_pulp.u[1]);
            if (l > 1e-3f) {
                g_pulp.u[0] /= l;
                g_pulp.u[1] /= l;
            } else {
                g_pulp.u[0] = 0;
                g_pulp.u[1] = 1;
            }
        }
        g_pulp.uinit = 1;
    } else if (dl > 0.5f) {
        float k = XcamK(0.15f, dt), nx =
            g_pulp.u[0] + (dx / dl - g_pulp.u[0]) * k, nz =
            g_pulp.u[1] + (dz / dl - g_pulp.u[1]) * k, nl =
            __builtin_sqrtf(nx * nx + nz * nz);
        if (nl > 1e-3f) {
            g_pulp.u[0] = nx / nl;
            g_pulp.u[1] = nz / nl;
        }
    }
    float yb = XcamDeg(-g_pulp.u[0], -g_pulp.u[1]);    // straight BEHIND the subject (away from the other body)
    int vsp = !att && !PulVcamOld();  // pulfix: SP's victim shot (above)
    if (!g_pulp.side && vsp) {  // SP: over the victim's RIGHT shoulder - the candidate side nearer our right shoulder joint
        float rs[3], ls[3], rx, rz, ya =
            (yb + 25.0f) * 0.0174532925f, yc2 =
            (yb - 25.0f) * 0.0174532925f;
        if (GxJointPos(SID_PLAYER, "r_shoulder", rs)
            && GxJointPos(SID_PLAYER, "l_shoulder", ls) && XvFin3(rs)
            && XvFin3(ls)
            && __builtin_fabsf(rs[0] - ls[0]) + __builtin_fabsf(rs[2] -
                                                                ls[2]) >
            0.05f) {
            rx = rs[0] - ls[0];
            rz = rs[2] - ls[2];
        } else {
            float yy = Uc4Yaw(me) * 0.0174532925f;
            rx = __builtin_cosf(yy);
            rz = -__builtin_sinf(yy);
        }                       // (no joints: the root's right, as SP's side was measured)
        g_pulp.side =
            (__builtin_sinf(ya) * rx + __builtin_cosf(ya) * rz) >=
            (__builtin_sinf(yc2) * rx + __builtin_cosf(yc2) * rz) ? 1 : -1;
        PULLOG
            ("pullout cam: victim shot (SP): the lens goes over our right shoulder - side %d",
             g_pulp.side);
    }
    if (!g_pulp.side) {         // the side the live gameplay camera is on
        float th = XcamDeg(-g_xcam_gdir[0], -g_xcam_gdir[2]);
        g_pulp.side = XvWrap(th - yb) >= 0.0f ? 1 : -1;
    }
    // (int101 A/B frames: the SP victim-track numbers put the lens 1.35 m from the ROOT at 0.28 m up - the prone body filled the
    // frame, the puller never showed; the attacker's mirror saw only grass). SP's victim view in the run (re/sp-melee-run-1031.md) is the gameplay
    // camera swung behind the player (~1.2 m) under *camera-remap-prone-tight-space*, so: a two-shot over the subject's shoulder that frames BOTH
    // bodies, keeping SP's motion - the 2.7 s swing-in, the push-in (PCP_D 1.35 -> 1.17 = -13 %) and the side drift (PCP_OFF 22 -> 1 deg, never under
    // 18 deg so the subject's own body does not hide the other). The arm grows with the gap between the bodies so both stay in frame.
    // Victim: chest-low (0.85 m over its root), aim 40 % from its chest to the puller's chest. Attacker: shoulder height (1.55 m), aim 65 % toward the victim.
    float tt = t < 0.0f ? 0.0f : t, push = PcpAt(PCP_D, tt) / PCP_D[0], off = PcpAt(PCP_OFF, tt) + (att ? 32.0f : 18.0f);     // (attacker: int102 frames - the puller's back filled the frame at 18)
    float gap = dl > 2.5f ? 2.5f : dl;
    int v2 = att && PulCamV2();
    float D =
        (att ? (v2 ? 1.55f : 1.9f) + 0.35f * gap : 1.45f +
         0.3f * gap) * push;
    float base_y = me[1];
    float H_use = (att ? 1.7f : 0.85f) + g_pulp.lift;
    if (vsp) {                  // SP's victim lens: low, close, 30 -> 22 deg to our right (the measured 18-37), no growth with the gap, no lift
        float x = tt / 0.45f;
        int i0 = x >= 9.0f ? 9 : (int) x;
        float f = x >= 9.0f ? 0.0f : x - (float) i0;
        int i1 = i0 < 9 ? i0 + 1 : 9;
        D = PVC_D[i0] + (PVC_D[i1] - PVC_D[i0]) * f;
        H_use = PVC_H[i0] + (PVC_H[i1] - PVC_H[i0]) * f;
        off = 22.0f + 8.0f * (tt < 2.0f ? 1.0f - tt / 2.0f : 0.0f);
    }
    if (v2) {
        float ch = sc[1] - me[1];
        ch = ch < 0.45f ? 0.45f : ch > 1.5f ? 1.5f : ch;
        H_use = ch + 0.3f + g_pulp.lift;
    }                           // (v2: the lens rides the puller's chest down into the grab)
    int p45 = v2 && g_ps45pr && XvFin3(sh) && XvFin3(sc) && XvFin3(sp) && XvFin3(oc) && XvFin3(oh);        // ps45 item 7 (the note at S45P_*)
    if (p45) {
        if (g_s45p.ms0 != g_xcamMs0) {
            memset(&g_s45p, 0, sizeof g_s45p);
            g_s45p.ms0 = g_xcamMs0;
            g_s45p.off = g_s45p.tgt = S45P_OFF0;
            PULLOG
                ("pullout cam: ps45 - the attacker's pull-out camera over our shoulder: %.2f m (+0.15 x gap) behind us, %.2f m over our head, side %.0f deg (widened to %.0f while the victim hides behind us), aim %.0f %% to the victim's upper body, lift <= %.2f m (t2r-ps45-cam-prone-old.txt = the v2 composition)",
                 (double) S45P_D, (double) S45P_UP, (double) S45P_OFF0,
                 (double) S45P_OFFMAX, (double) (S45P_AIM * 100.0f),
                 (double) S45P_LIFT);
        }
        float hy = sh[1] - me[1];
        hy = hy < 0.55f ? 0.55f : hy > 1.75f ? 1.75f : hy;
        D = (S45P_D + 0.15f * gap) * push;
        H_use =
            hy + S45P_UP + (g_pulp.lift <
                            S45P_LIFT ? g_pulp.lift : S45P_LIFT);
        float vu[3] =
            { (oc[0] + oh[0]) * 0.5f, (oc[1] + oh[1]) * 0.5f,
      (oc[2] + oh[2]) * 0.5f };
        int sd = g_pulp.side ? g_pulp.side : 1;
        float yc = (yb + (float) sd * g_s45p.off) * 0.0174532925f, L0[3] =
            { me[0] + __builtin_sinf(yc) * D, me[1] + H_use,
me[2] + __builtin_cosf(yc) * D };
        int hid = S45aHidden(L0, sp, sc, sh, oh)
            || S45aHidden(L0, sp, sc, sh, vu);
        if (hid && g_s45p.tgt < S45P_OFFMAX) {
            g_s45p.tgt += 8.0f;
            g_s45p.nw++;
        } else if (!hid && g_s45p.tgt > S45P_OFF0) {
            float yn =
                (yb + (float) sd * (g_s45p.tgt - 8.0f)) * 0.0174532925f,
                L1[3] =
                { me[0] + __builtin_sinf(yn) * D, me[1] + H_use,
             me[2] + __builtin_cosf(yn) * D };
            if (!S45aHidden(L1, sp, sc, sh, oh)
                && !S45aHidden(L1, sp, sc, sh, vu))
                g_s45p.tgt -= 8.0f;
        }
        g_s45p.off += (g_s45p.tgt - g_s45p.off) * XcamK(0.35f, dt);
        if (g_s45p.off > g_s45p.mx)
            g_s45p.mx = g_s45p.off;
        off = g_s45p.off;
        g_s45p.n++;
        uint64_t nw = GetTickCount64();
        if (nw - g_s45p.lt > 1500 && LOGCAP(g_pulp.nlog, 40)) {
            g_s45p.lt = nw;
            PULLOG
                ("pullout cam: ps45 - over the shoulder %.2f m behind us at %.2f m, side %.0f deg (max %.0f, widened %ld time(s) for the victim behind our torso), %ld frame(s)",
                 (double) D, (double) H_use, (double) off,
                 (double) g_s45p.mx, g_s45p.nw, g_s45p.n);
        }
    }
    float voff_use = 0.0f;
    float A[3];
    for (int i = 0; i < 3; i++)
        A[i] = sc[i] + (oc[i] - sc[i]) * (att ? 0.65f : 0.40f);
    if (att)
        A[1] -= 0.25f;          // the dragged body lies low: aim a little under the chest line
    if (p45)
        for (int i = 0; i < 3; i++)
            A[i] = sc[i] + ((oc[i] + oh[i]) * 0.5f - sc[i]) * S45P_AIM; // ps45 item 7: the victim's upper body
    // candidates (preference order): [cur side +0, +35, +70], [other side +0, +35, +70], straight back
    static const float ADD[PCP_NC] = { 0, 35, 70, 0, 35, 70, 0 };
    float cx[PCP_NC], cz[PCP_NC];
    for (int k = 0; k < PCP_NC; k++) {
        float sg =
            k < 3 ? (float) g_pulp.side : k <
            6 ? -(float) g_pulp.side : 0.0f, ang =
            k == 6 ? 0.0f : sg * (off + ADD[k]);
        float yc = (yb + ang) * 0.0174532925f;
        cx[k] = me[0] + __builtin_sinf(yc) * D;
        cz[k] = me[2] + __builtin_cosf(yc) * D;
    }
    double now_ms = PsyncMs();
    // read the last batch (a later frame), then kick the next every 0.4 s
    if (g_pulp.pend && g_t60 != g_pulp.kick_f) {
        float sc_k[PCP_NC];
        int first = -1;
        for (int k = 0; k < PCP_NC; k++) {
            float m = 1.0f;
            for (int r = 0; r < 2; r++)
                if (g_pulp.job[k][r]) {
                    float tr = RayT(g_pulp.job[k][r]);
                    g_pulp.job[k][r] = 0;
                    if (r == 1 && tr >= 0.0f) {
                        tr /= 0.8f;
                        if (tr > 1.0f)
                            tr = 1.0f;
                    }           // lens -> aim ends inside the other body (0x1b07 stops on bodies): its surface is no blocker
                    if (!g_rayErr && tr >= 0.0f && tr < m)
                        m = tr;
                }
            sc_k[k] = m;
            if (m >= 0.97f && first < 0)
                first = k;
        }
        g_pulp.pend = 0;
        int cur_ok = g_pulp.chosen >= 0 && g_pulp.init
            && sc_k[g_pulp.chosen] >= 0.97f, np =
            cur_ok ? g_pulp.chosen : first;
        if (np >= 0) {
            if (np != g_pulp.chosen && LOGCAP(g_pulp.nlog, 40))
                PULLOG
                    ("pullout cam: procedural role %d spot %d -> %d (rays: %.2f %.2f %.2f %.2f %.2f %.2f %.2f)",
                     g_pulp.role, g_pulp.chosen, np, (double) sc_k[0],
                     (double) sc_k[1], (double) sc_k[2], (double) sc_k[3],
                     (double) sc_k[4], (double) sc_k[5], (double) sc_k[6]);
            g_pulp.chosen = np;
            g_pulp.shrink = 1.0f;
        } else {
            int bk = 0;
            for (int k = 1; k < PCP_NC; k++)
                if (sc_k[k] > sc_k[bk])
                    bk = k;
            g_pulp.chosen = bk;
            // (int107 B, side pull out from under the truck: every chest-high spot hit the truck, arm x0.30 = the puller's torso filled the frame):
            // first rise over the obstacle (+0.45 m per 0.4 s batch, up to +0.9 m, kept for the rest of the sequence); the arm shrinks only once fully raised
            if (vsp)
                g_pulp.shrink = sc_k[bk] * 0.9f < 0.3f ? 0.3f : sc_k[bk] * 0.9f;        // (pulfix: the victim lens never rises into the vehicle - the arm comes in under it)
            else if (g_pulp.lift < 0.85f) {
                g_pulp.lift += 0.45f;
                if (g_pulp.shrink < 0.75f)
                    g_pulp.shrink = 0.75f;
            } else
                g_pulp.shrink = sc_k[bk] < 0.3f ? 0.3f : sc_k[bk];
            if (LOGCAP(g_pulp.nlog, 40))
                PULLOG
                    ("pullout cam: procedural role %d: every spot blocked - least blocked %d, lift +%.2f m, arm x%.2f",
                     g_pulp.role, bk, (double) g_pulp.lift,
                     (double) g_pulp.shrink);
        }
    }
    if (!g_pulp.pend
        && (g_pulp.kick_ms == 0.0 || now_ms - g_pulp.kick_ms >= 400.0)) {
        float s0[3] = { sp[0], sp[1] + (att ? 0.5f : 0.2f), sp[2] };
        for (int k = 0; k < PCP_NC; k++) {
            float C[3] = { cx[k], base_y + H_use, cz[k] };
            // (int100 B: 0x207 stops on tall grass ~0.45 m up - every low victim spot read "blocked" and the arm shrank to x0.30 into the body.
            //  0x1b07 = winmm's shot mask: walls / vehicles yes, grass no; it stops on bodies, so our own body is ignored)
            // the attacker's aim point (65 % to the victim, lowered) sat UNDER the truck - every spot's lens -> aim ray hit the chassis,
            // the camera lifted / shrank into the wheel. Attacker: test toward a chest-high point 35 % of the way to the victim (still out in the open).
            float A2[3];
            if (att)
                for (int i = 0; i < 3; i++)
                    A2[i] = sc[i] + (oc[i] - sc[i]) * 0.35f;
            else
                memcpy(A2, A, 12);
            if (att && oc[1] < sc[1])
                A2[1] = sc[1] - 0.1f;
            g_pulp.job[k][0] = WorldRayKickM(s0, C, SID_PLAYER, 0x1b07);
            g_pulp.job[k][1] = WorldRayKickM(C, A2, SID_PLAYER, 0x1b07);
        }
        g_pulp.pend = 1;
        g_pulp.kick_f = g_t60;
        g_pulp.kick_ms = now_ms;
        if (g_pulp.chosen < 0 || !g_pulp.init)
            g_pulp.chosen = 0;
    }
    int ch = g_pulp.chosen < 0 ? 0 : g_pulp.chosen;
    float C[3] =
        { me[0] + (cx[ch] - me[0]) * g_pulp.shrink, base_y + H_use,
   me[2] + (cz[ch] - me[2]) * g_pulp.shrink };
    if (!XvFin3(C))
        return 0;
    // the view: toward the aim, the pitch = the aim's elevation - voff (SP: the victim sits voff deg off the view axis)
    float ax = A[0] - C[0], az = A[2] - C[2], ah =
        __builtin_sqrtf(ax * ax + az * az);
    if (ah < 0.05f) {
        ax = -g_pulp.u[0];
        az = -g_pulp.u[1];
        ah = 1.0f;
    }
    float elev = __builtin_atan2f(A[1] - C[1], ah) * 57.2957795f, pit =
        elev - voff_use;
    pit = pit < -50.0f ? -50.0f : pit > 35.0f ? 35.0f : pit;
    float yw = __builtin_atan2f(ax, az), pr = pit * 0.0174532925f;
    if (vsp) {                  // SP: aimed ~8 deg past the victim, away from the puller (the puller 13-20 deg the other side of centre, kept within 26 deg); pitch = puller chest elevation - 14
        float yv = XcamDeg(sp[0] - C[0], sp[2] - C[2]), yo =
            XcamDeg(oc[0] - C[0], oc[2] - C[2]), dvo = XvWrap(yo - yv);
        float yl = yv - 0.6f * dvo;
        if (XvWrap(yo - yl) > 26.0f)
            yl = yo - 26.0f;
        else if (XvWrap(yo - yl) < -26.0f)
            yl = yo + 26.0f;
        float oh2 =
            __builtin_sqrtf((oc[0] - C[0]) * (oc[0] - C[0]) +
                            (oc[2] - C[2]) * (oc[2] - C[2]));
        float eo =
            __builtin_atan2f(oc[1] - C[1],
                             oh2 > 0.05f ? oh2 : 0.05f) * 57.2957795f;
        pit = eo - 14.0f;
        pit = pit < -10.0f ? -10.0f : pit > 15.0f ? 15.0f : pit;
        yw = yl * 0.0174532925f;
        pr = pit * 0.0174532925f;
    }
    float L[3] =
        { __builtin_sinf(yw) * __builtin_cosf(pr), __builtin_sinf(pr),
   __builtin_cosf(yw) * __builtin_cosf(pr) };
    if (!g_pulp.init) {
        memcpy(g_pulp.pos, C, 12);
        memcpy(g_pulp.look, L, 12);
        g_pulp.init = 1;
        PULLOG
            ("pullout cam: procedural role %d starts: side %d, subject-to-other %.2f m, camera %.2f m %s",
             g_pulp.role, g_pulp.side, (double) dl, (double) D,
             att ? "behind the puller" : "behind the victim");
    } else {
        float k = XcamK(0.2f, dt), k2 = XcamK(0.25f, dt);
        for (int i = 0; i < 3; i++) {
            g_pulp.pos[i] += (C[i] - g_pulp.pos[i]) * k;
            g_pulp.look[i] += (L[i] - g_pulp.look[i]) * k2;
        }
        float ll =
            __builtin_sqrtf(g_pulp.look[0] * g_pulp.look[0] +
                            g_pulp.look[1] * g_pulp.look[1] +
                            g_pulp.look[2] * g_pulp.look[2]);
        if (ll > 1e-3f)
            for (int i = 0; i < 3; i++)
                g_pulp.look[i] /= ll;
        else
            memcpy(g_pulp.look, L, 12);
    }
    // pulfix: per-frame lens collision - a subject -> lens ray every frame (read the next frame): a hit pulls the arm in at once to 90 % of the hit, a clear ray
    // lets it out again over ~0.3 s. (0x1b07: walls / vehicles, not grass; our own body ignored.) t2r-pullout-camclamp-off.txt = off.
    float Cf[3];
    memcpy(Cf, g_pulp.pos, 12);
    if (GetFileAttributesA("t2r-pullout-camclamp-off.txt") ==
        INVALID_FILE_ATTRIBUTES) {
        float s1[3] = { sp[0], sp[1] + (att ? 0.5f : 0.2f), sp[2] };
        if (g_pulp.cl <= 0.0f)
            g_pulp.cl = 1.0f;
        if (g_pulp.cj && g_t60 != g_pulp.cj_f) {
            float tr = RayT(g_pulp.cj);
            g_pulp.cj = 0;
            float want = (!g_rayErr && tr >= 0.05f && tr < 1.0f) ? tr * 0.9f : 1.0f;   // a hit at the very start = the ray began inside the ground / vehicle, not a blocked lens
            if (want < 0.15f)
                want = 0.15f;
            if (want < g_pulp.cl) {
                if (g_pulp.cl - want > 0.1f && LOGCAP(g_pulp.cl_n, 20))
                    PULLOG
                        ("pullout cam: role %d lens ray hit at %.2f of the arm - pulled in",
                         g_pulp.role, (double) tr);
                g_pulp.cl = want;
            } else
                g_pulp.cl += (want - g_pulp.cl) * XcamK(0.06f, dt);
        }
        if (!g_pulp.cj) {
            g_pulp.cj =
                WorldRayKickM(s1, g_pulp.pos, SID_PLAYER, 0x1b07);
            g_pulp.cj_f = g_t60;
        }
        for (int i = 0; i < 3; i++)
            Cf[i] = s1[i] + (g_pulp.pos[i] - s1[i]) * g_pulp.cl;
    }
    for (int i = 0; i < 3; i++) {
        g_xcamPos[i] = Cf[i];
        g_xcam_look[i] = g_pulp.look[i];
    }
    g_xcam_valid = 1;
    g_xcam_th = yw * 57.2957795f;
    // the SP lens: fov 70 -> 73.7 over the swing (x0.95 -> x1 of the game fov), held
    {
        float u = tt / PUS_SWING_S;
        u = u > 1.0f ? 1.0f : u;
        u = u * u * (3.0f - 2.0f * u);
        g_xcamFovT = g_xcamFov0 * (0.95f + 0.05f * u);
    }
    return 1;
}

// pulfix: the victim's SP replay camera on the running xcam (pul_cam_start). 0 = no recording for this row / switched off (the fitted shot runs)
static int PulRepBegin(int slot, int row)
{
    int mir = 0;
    float ts = 1.0f;
    int tk = PscPick(row, &mir, &ts);
    if (PulVcamFit() || PulVcamOld()) {
        PULLOG
            ("pullout cam: SP REPLAY off (t2r-pullout-vcam-fit.txt / -vcam-old.txt) - the fitted victim shot");
        return 0;
    }
    if (tk < 0) {
        PULLOG
            ("pullout cam: SP REPLAY: no SP recording for %s%s - the fitted victim shot",
             CD_PAIRS[row].name, !strncmp(CD_PAIRS[row].name, "pu-drag-r-",
                                          10) ?
             " (t2r-pullout-vcam-nomirror.txt)" : "");
        return 0;
    }
    PulProcBegin(2, slot, "SP replay");
    memset(&g_psc, 0, sizeof g_psc);
    g_psc.on = 1;
    g_psc.trk = tk;
    g_psc.mir = mir;
    g_psc.ts = ts;
    g_psc.ph = -1;
    // SP's blends (sp_pullout_cam.py measure()): in = SP's 1.24 s into the pull-out framing, smoothstep (g_xcam_spbl) from our live camera; out = SP's hand-back
    float obi = g_xcamBinF, ob = g_xcam_bout_f;
    g_xcamBinF = PSC_BLEND_IN_S * 60.0f;
    float nb = PSC_OUT_S * 60.0f, d = nb - g_xcam_bout_f;
    g_xcam_bout_f = nb;
    if (d >= 0.0f)
        g_xcam_dur += (uint64_t) d;
    else
        g_xcam_dur =
            g_xcam_dur >
            (uint64_t) (-d) ? g_xcam_dur - (uint64_t) (-d) : 1;
    PULLOG
        ("pullout cam: SP REPLAY victim camera for %s: %s (SP episode %d, %d recorded frames, %.2f s)%s%s, anchor = our root + the pair ap's yaw, blend in %.2f s (was %.2f), out %.2f s (was %.2f) - measured in SP",
         CD_PAIRS[row].name, PSC_TRACKS[tk].name, PSC_TRACKS[tk].ep,
         PSC_TRACKS[tk].n, (double) PSC_TRACKS[tk].len,
         mir ? ", MIRRORED (no SP recording of the right drag)" : "",
         ts != 1.0f ? ", time-scaled" : "", (double) PSC_BLEND_IN_S,
         (double) (obi / 60.0f), (double) PSC_OUT_S,
         (double) (ob / 60.0f));
    if (ts != 1.0f)
        PULLOG
            ("pullout cam: SP REPLAY time scale x%.3f (the left recording %.2f s over this clip)",
             (double) ts, (double) PSC_TRACKS[tk].len);
    return 1;
}

// [SP ep5/ep8] the fitted victim shot (no SP recording for the first pair: pu-kill) joins SP's replay at the grapple / outcome pair (the note at pul_prone_cam_sp_old)
static void PulRepJoin(int row)
{
    if (g_psc.on || PulProneCamSpOld() || PulVcamFit()
        || PulVcamOld() || PulProcOff())
        return;
    int mir = 0;
    float ts = 1.0f, P0[3], L0[3];
    int tk = PscPick(row, &mir, &ts);
    for (int i = 0; i < 3; i++) {
        P0[i] = g_xcamPos[i];
        L0[i] = g_xcam_look[i];
    }
    if (tk < 0 || !g_xcam_valid || !XvFin3(P0) || !XvFin3(L0)) {
        PULLOG("pullout cam: SP 10-31 prone - no join for %s (%s)",
               CD_PAIRS[row].name,
               tk < 0 ? "no SP recording" : "no shown camera");
        return;
    }
    memset(&g_psc, 0, sizeof g_psc);
    g_psc.on = 1;
    g_psc.trk = tk;
    g_psc.mir = mir;
    g_psc.ts = ts;
    g_psc.ph = -1;
    memcpy(g_psc.xf_p, P0, 12);
    memcpy(g_psc.xf_l, L0, 12);
    g_psc.xf_r = g_xcamRoll;
    g_psc.xf_f = g_xcamFovT;
    g_psc.xf_on = 1;
    g_psc.xf_t0 = PsyncMs();
    g_psc.xf_s = PSC_BLEND_IN_S;
    PULLOG
        ("pullout cam: SP 10-31 prone - the fitted victim shot joins SP's replay at %s: %s (SP ep %d, %d frames), %.2f s smoothstep from the shown camera (t2r-prone-cam-sp-old.txt = the fitted shot to the end)",
         CD_PAIRS[row].name, PSC_TRACKS[tk].name, PSC_TRACKS[tk].ep,
         PSC_TRACKS[tk].n, (double) PSC_BLEND_IN_S);
}

static void PulCamStart(int role, int slot, int row)
{                               // the drag pair just started on us: role 1 attacker / 2 victim, slot = the other player
#ifdef XCAM_HAVE_PUL
    if (PulPincamOld()) {
        PULLOG
            ("pullout cam: t2r-pullout-pincam.txt - the pin camera framing stays");
        return;
    }
    if (role == 1 && g_pulc.intro) {
        if (g_pulc.on && g_xcamOn && g_xcamPulproc && !strncmp(g_xcamName, "ctxdown:pu-", 11)) {     // v2: the camera that started at the intro runs on (no cut at the grab)
            double now2 = PsyncMs();
            float t2 = (float) ((now2 - g_xcamMs0) * 0.001), end2 =
                CD_PAIRS[row].end_s;
            g_xcam_dur =
                (uint64_t) (t2 * 60.0f + (end2 + 1.7f) * 60.0f +
                            g_xcam_bout_f);
            g_xcam_kill = t2 + end2;
            PULLOG
                ("pullout cam: v2 - the intro's procedural camera runs on through the drag (%.2f s in, the drag ends %.2f s from now)",
                 (double) t2, (double) end2);
            return;
        }
        g_pulc.intro = 0;       // it stopped (a body gone / another camera): the drag starts its own below
        PULLOG
            ("pullout cam: v2 - the intro's camera is no longer running - the drag starts one");
    }
    if (g_pulc.dropped) {
        PULLOG
            ("pullout cam: the drag's SP camera was dropped (blocked) - the pin camera framing stays");
        return;
    }
    int force_proc = 0;
    if (role == 2 && g_pulc.peer_cam == 0) {
        if (PulProcOff()) {
            g_pulc.dropped = 1;
            PULLOG
                ("pullout cam: the attacker found no clean SP camera - the pin camera framing (t2r-pullout-proccam-off.txt)");
            return;
        }
        force_proc = 1;
    }
    uint8_t *pe = MdgPeer((uint32_t) slot);
    uint64_t h = pe ? *(uint64_t *) (pe + 0x60) : 0;
    char nm[48];
    snprintf(nm, sizeof nm, "ctxdown:%s", CD_PAIRS[row].name);
    float end_s = CD_PAIRS[row].end_s, off = PulCamOff(end_s);
    if (!h) {
        PULLOG
            ("pullout cam: %s: no puppet for slot %d - the pin camera framing",
             nm, slot);
        return;
    }
    // (+1.7 s past the hand-off: the outcome pair starts up to PUL_RES_WAIT_MS + the network later and swaps the track in place; the camera must not be easing out by then)
    if (!XcamStart(h, end_s - off + 1.7f, 0, -1, end_s - off, role, nm)) {
        PULLOG
            ("pullout cam: %s: xcam_start refused (exec camera off / no render hook) - the pin camera framing",
             nm);
        return;
    }
    if (role == 2 && !PulProcOff() && PulRepBegin(slot, row))
        return;                 // pulfix: the victim replays SP's recorded camera (no authored-track choice, no attacker answer needed)
    if (!g_xcamTrk || force_proc) {
        if (PulProcOff()) {
            XcamStop("no authored track");
            PULLOG
                ("pullout cam: %s: no authored track - the pin camera framing",
                 nm);
            return;
        }
        PulProcBegin(role, slot,
                       force_proc ? "the attacker's SP shot is blocked" :
                       "no authored track for this drag");
        return;
    }
    g_xcamTrkOff = off;
    // SP victim (re/sp-melee-run-1031.md "Prone pull-out on the player"): no cut - the gameplay camera swings round behind the victim over ~2.7 s and holds through the struggle.
    // The authored track's own 0.45 s blend-in reads as a cut for the victim, so its blend-in becomes the SP 2.7 s (smoothstep from the live gameplay camera); the lens ramp
    // (fov 70 -> 73.7 = x0.95 -> x1 of the game fov) is driven by pullout_sting.c. t2r-pullout-camswing-off.txt = the old 0.45 s blend.
    if (role == 2
        && GetFileAttributesA("t2r-pullout-camswing-off.txt") ==
        INVALID_FILE_ATTRIBUTES) {
        g_xcamBinF = PUS_SWING_S * 60.0f;
        PULLOG
            ("pullout cam: victim swing - blend-in %.1f s (SP gameplay camera swing, t2r-pullout-camswing-off.txt = off)",
             (double) PUS_SWING_S);
    }
    g_pulc.on = 1;
    g_pulc.slot = slot;
    g_pulc.role = role;
    g_pulc.judged = 0;
    g_pulc.t0 = GetTickCount64();
    PULLOG
        ("pullout cam: %s role %d: TLOU2 SP's authored camera (apReference-camera%d of the player half, %d candidates, %d keys, %.2f s), track clock +%.0f ms",
         nm, role, XcamCamIx(g_xcamTrk), g_xcam_ncand, g_xcamTrk->n,
         (double) g_xcamTrk->k[g_xcamTrk->n - 1].t,
         (double) off * 1000.0);
#else
    (void) role;
    (void) slot;
    (void) row;
#endif
}

static void PulCamIntro(int slot, int row, float il)
{                               // v2: our attacker camera from the intro on (pul_cam_v2)
#ifdef XCAM_HAVE_PUL
    if (!PulCamV2() || PulProcOff())
        return;
    uint8_t *pe = MdgPeer((uint32_t) slot);
    uint64_t h = pe ? *(uint64_t *) (pe + 0x60) : 0;
    if (!h) {
        PULLOG
            ("pullout cam: v2 intro: no puppet for slot %d - the pin camera framing",
             slot);
        return;
    }
    char nm[48];
    snprintf(nm, sizeof nm, "ctxdown:%s", CD_PAIRS[row].name);
    float end_s = il + CD_PAIRS[row].end_s;
    if (!XcamStart(h, end_s + 1.7f, 0, -1, end_s, 1, nm)) {
        PULLOG
            ("pullout cam: v2 intro: xcam_start refused - the pin camera framing");
        return;
    }
    PulProcBegin(1, slot, "v2: from the intro, no pin camera");
    g_pulc.intro = 1;
    g_pulc.sent = 0;
#else
    (void) slot;
    (void) row;
    (void) il;
#endif
}

static void PulCamSwap(int role, int slot, int row)
{                               // the outcome pair just started: the running camera moves on to its track (or starts one if the drag's did not run)
#ifdef XCAM_HAVE_PUL
    if (PulPincamOld() || g_pulc.dropped)
        return;
    if (g_pulc.on && g_xcamOn && g_xcamPulproc && !strncmp(g_xcamName, "ctxdown:pu-", 11)) { // the procedural camera runs on through the outcome pair (no cut): only its clock grows
        char nm2[48];
        snprintf(nm2, sizeof nm2, "ctxdown:%s", CD_PAIRS[row].name);
        if (role == 2)
            PulRepJoin(row);  // [SP ep5/ep8] (before the clock: a joined success gets SP's 1.68 s hold below)
        double now2 = PsyncMs();
        float t2 = (float) ((now2 - g_xcamMs0) * 0.001), end2 =
            CD_PAIRS[row].end_s, off2 = PulCamOff(end2);
        g_xcam_dur =
            (uint64_t) (t2 * 60.0f + (end2 - off2) * 60.0f +
                        g_xcam_bout_f);
        g_xcam_kill = end2;
        if (g_psc.on && !strncmp(CD_PAIRS[row].name, "pu-ok", 5) && PSC_OUT_AT_S > end2) {      // pulfix: SP holds the success camera to 1.68 s of the clip before handing back
            g_xcam_dur += (uint64_t) ((PSC_OUT_AT_S - end2) * 60.0f);
            end2 = PSC_OUT_AT_S;
        }
        snprintf(g_xcamName, sizeof g_xcamName, "%s", nm2);
        PULLOG
            ("pullout cam: %s: the procedural camera runs on through the outcome pair, ends %.2f s from now",
             nm2, (double) (end2 - off2));
        return;
    }
    if (!
        (g_pulc.on && g_xcamOn && g_xcamTrk
         && !strncmp(g_xcamName, "ctxdown:pu-", 11))) {
        PULLOG
            ("pullout cam: no pull-out camera running at the outcome - starting one");
        g_pulc.on = 0;
        PulCamStart(role, slot, row);
        return;
    }
    char nm[48];
    snprintf(nm, sizeof nm, "ctxdown:%s", CD_PAIRS[row].name);
    const XcamTrack *nt = NULL, *any = NULL;
    int cix = XcamCamIx(g_xcamTrk);
    for (size_t i = 0;
         i < sizeof PUL_XCAM_TRACKS / sizeof PUL_XCAM_TRACKS[0]; i++)
        if (!strcmp(PUL_XCAM_TRACKS[i].move, nm)
            && PUL_XCAM_TRACKS[i].role == 1) {
            if (!any)
                any = &PUL_XCAM_TRACKS[i];
            if (XcamCamIx(&PUL_XCAM_TRACKS[i]) == cix)
                nt = &PUL_XCAM_TRACKS[i];
        }
    if (!nt)
        nt = any;
    float tap[7];
    if (!nt && !strncmp(CD_PAIRS[row].name, "pu-grap", 7)) {    // no SP authored camera for the struggle - the drag's track holds its last key through the grapple (its clock grows)
        g_xcam_dur += (uint64_t) (CD_PAIRS[row].end_s * 60.0f);
        g_xcam_kill += CD_PAIRS[row].end_s;
        PULLOG
            ("pullout cam: %s has no authored track - the drag's camera holds through the grapple (+%.2f s)",
             nm, (double) CD_PAIRS[row].end_s);
        return;
    }
    if (!nt) {
        PULLOG("pullout cam: %s has no track - the drag's camera runs out",
               nm);
        return;
    }
    if (!PsyncAp(tap)) {
        PULLOG
            ("pullout cam: %s: no pair ap yet - the drag's camera runs out",
             nm);
        return;
    }
    double now_ms = PsyncMs();
    float t = (float) ((now_ms - g_xcamMs0) * 0.001), end_s =
        CD_PAIRS[row].end_s, off = PulCamOff(end_s);
    if (g_xph.last_ok) {
        memcpy(g_xph.c, g_xph.lc, 12);
        memcpy(g_xph.l, g_xph.ll, 12);
        g_xph.f = g_xph.lf;
        g_xph.r = g_xph.lr;
        g_xph.t0 = now_ms;
        g_xph.on = 1;
    }
    g_xcamTrk = nt;
    g_xcamCands[0] = nt;
    g_xcam_ncand = 1;
    g_xcam_choice_state = 2;
    g_xcamApOk = 0;           // latched again from the outcome pair's ap on the next eval
    g_xcamTrkOff = off - t;   // the track clock reads `off` now (0, or the victim's START skip)
    g_xcam_dur =
        (uint64_t) (t * 60.0f + (end_s - off) * 60.0f + g_xcam_bout_f);
    g_xcam_kill = end_s;
    snprintf(g_xcamName, sizeof g_xcamName, "%s", nm);
    PULLOG
        ("pullout cam: %s role %d: the camera moves on to the outcome's track (camera %d kept, %d keys, %.2f s), crossfade %.0f ms%s, ends %.2f s from now",
         nm, role, XcamCamIx(nt), nt->n, (double) nt->k[nt->n - 1].t,
         XPH_S * 1000.0, g_xph.on ? "" : " (no pose to blend from: cut)",
         (double) (end_s - off));
#else
    (void) role;
    (void) slot;
    (void) row;
#endif
}

// (prone under a truck): every authored camera of the drag sat in the truck / in the bodies (victim: all 4 DROPPED, the least blocked kept;
// attacker: the right-side rule kept one). The pull-out only plays an SP camera that survived the ray test clean; otherwise the pin camera (pul_cam_role,
// framing both bodies) runs the whole sequence - the outcome does not start a new SP camera either (no cut mid-sequence)
static void PulCamJudge(void)
{
#ifdef XCAM_HAVE_PUL
    if (!g_pulc.on || g_pulc.judged || !g_xcamOn
        || g_xcam_choice_state != 2
        || strncmp(g_xcamName, "ctxdown:pu-", 11))
        return;
    int ci = -1;
    for (int i = 0; i < g_xcam_ncand; i++)
        if (g_xcamCands[i] == g_xcamTrk)
            ci = i;
    int clean = g_xcamTrk && ci >= 0 && g_xcamCandOk[ci];
    if (g_pulc.role == 2) {
        // the victim plays the attacker's choice (the victim's own ray test dropped all 4 SP shots on the bodies, which those close
        // SP shots frame on purpose, while the attacker's test of the same world shots was clean) - its own test only when no answer comes (an older client)
        if (g_pulc.peer_cam < 0 && GetTickCount64() - g_pulc.t0 < 450)
            return;
        g_pulc.judged = 1;
        if (g_pulc.peer_cam > 0) {
            for (int i = 0; i < g_xcam_ncand; i++)
                if (XcamCamIx(g_xcamCands[i]) == g_pulc.peer_cam) {
                    g_xcamTrk = g_xcamCands[i];
                    PULLOG
                        ("pullout cam: %s: the attacker's camera %d (same world shot on both screens)",
                         g_xcamName, g_pulc.peer_cam);
                    return;
                }
        }
        if (g_pulc.peer_cam < 0 && clean)
            return;
        if (!PulProcOff()) {
            PulProcBegin(2, g_pulc.slot,
                           g_pulc.peer_cam ==
                           0 ? "the attacker's SP shot is blocked" :
                           "no clean SP camera here");
            return;
        }
        PULLOG("pullout cam: %s: %s - dropped for the pin camera framing",
               g_xcamName,
               g_pulc.peer_cam ==
               0 ? "the attacker found no clean SP camera" :
               "no clean SP camera here");
        g_pulc.on = 0;
        g_pulc.dropped = 1;
        XcamStop("pullout: blocked SP camera");
        return;
    }
    g_pulc.judged = 1;
    uint8_t p[1] = { (uint8_t) (clean ? XcamCamIx(g_xcamTrk) : 0) };
    PulSend(g_pulc.slot, PUL_OP_CAM, g_pul.id, p, 1);
    if (clean) {
        PULLOG("pullout cam: %s: camera %d clean - sent to the victim",
               g_xcamName, (int) p[0]);
        return;
    }
    if (!PulProcOff()) {
        PULLOG
            ("pullout cam: %s: no clean SP camera (chosen %d, %s) - the procedural camera takes over (the victim's too)",
             g_xcamName, g_xcamTrk ? XcamCamIx(g_xcamTrk) : -1,
             ci >= 0 ? "blocked" : "none");
        PulProcBegin(1, g_pulc.slot, "no clean SP camera");
        return;
    }
    PULLOG
        ("pullout cam: %s: no clean SP camera (chosen %d, %s) - dropped for the pin camera framing (the victim too)",
         g_xcamName, g_xcamTrk ? XcamCamIx(g_xcamTrk) : -1,
         ci >= 0 ? "blocked" : "none");
    g_pulc.on = 0;
    g_pulc.dropped = 1;
    XcamStop("pullout: blocked SP camera");
#endif
}

static void PulCamEnd(const char *why, int normal)
{                               // normal = the sequence ran to its end: the camera eases out on its own duration
    g_pulc.dropped = 0;
    g_pulc.peer_cam = -1;
    g_pulc.intro = 0;
    g_pulc.sent = 0;
    // (the SP replay keeps g_psc.on while the camera eases out: the hold frame needs it; pul_proc_begin clears it for the next camera)
    if (!g_pulc.on)
        return;
    g_pulc.on = 0;
    if (!normal && g_xcamOn && !strncmp(g_xcamName, "ctxdown:pu-", 11)) {
        PULLOG("pullout cam: stopped (%s)", why);
        XcamStop(why);
    } else if (g_xcamOn)
        PULLOG("pullout cam: ends with the sequence (%s)", why);
}

static void PulAttEnd(const char *why, int notify)
{
    if (g_pul.st == PUL_IDLE)
        return;
    PulCamEnd(why, g_pul.st == PUL_OUT
                && (!strncmp(why, "countered", 9)
                    || !strcmp(why, "the victim was downed")));
    PULLOG
        ("END (%s) in state %d on slot %d: pulls started %d, countered %d, downed %d, refused %d",
         why, g_pul.st, g_pul.slot, g_pul.n_started, g_pul.n_ok,
         g_pul.n_fail, g_pul.n_refused);
    if (g_pul.st == PUL_INTRO)
        MhrStop(SID_PLAYER,
                 Fnv64Rt(PulIntroClip(g_pul.set, g_pul.dir)), 0.15f);
    if (notify && g_pul.slot >= 0)
        PulSend(g_pul.slot, PUL_OP_END, g_pul.id, NULL, 0);
    if (g_ps.role == 1 && g_pul.uid && g_ps.uid == g_pul.uid)
        PsyncAbort("pullout over", 1);
    PadinjBlock("melee-attack", 0);
    g_pul.st = PUL_IDLE;
    g_pul.slot = -1;
    g_pul.uid = 0;
    g_pul.verdict = 0;
    g_pul.gate_t0 = 0;
    g_pul.grap = 0;
    g_pul.pounce = 0;
    g_pul.shortd = 0;
    g_pul.kill_at = 0;
    g_pul.vend_ok = 0;
    g_pul.krow = -1;
    g_pul.kskip = 0;
    g_pul.fb = 0;
}

// the path gate on a pair's spots (wall / floor / drop; 2 = the rays are in flight, ask again). ap solved on the victim's body from its own half
static int PulGate(int row, int s, uint64_t h, float *ap_out,
                    const char *tag)
{
    float vb[16], mb[16];
    const float *vl = PuppetLocator(h, vb), *me = PlayerLocator(mb);
    if (!vl || !me || !PsLocOk(vl) || !PsLocOk(me)) {
        PULLOG("%s refused: no locators", tag);
        return 0;
    }
    float v8[8], ap[7];
    memcpy(v8, vl, sizeof v8);
    if (!PsyncSolveAp(h, Fnv64Rt(CD_VIC0(row)), v8, ap)) {
        PULLOG("%s refused: %s has no readable align channels", tag,
               CD_VIC0(row));
        return 0;
    }
    float mv, da, sa[3], vy, V[3] = { vl[0], vl[1], vl[2] };
    if (!CdSpotsAt(row, h, ap, V, me, 0.0f, &mv, &da, sa, &vy)) {
        PULLOG("%s refused: the attacker spot of %s is unreadable", tag,
               CD_ATT0(row));
        return 0;
    }
    g_pul.farspot = 0;
    float dmax = g_pul.pounce ? PUL_RANGE + 0.6f : g_pul.cov ? PUL_GATE_FAR_COV : 2.2f; // (a covered victim: the attacker may stand a bit further from the authored spot; the path gate below still tests the way)
    if (da > dmax) {
        g_pul.farspot = 1;
        PULLOG
            ("%s refused: the pair's authored attacker spot is %.2f m from us (max %.1f)",
             tag, (double) da, (double) dmax);
        return 0;
    }
    float vd = PsyncDur(h, Fnv64Rt(CD_VIC0(row)));
    g_pgEndPh = vd > 0.2f ? PulDend(row) / vd : 0.0f;
    int g =
        PgGate((uint32_t) s, CD_ATT0(row), CD_VIC0(row), ap, vl, me, 0,
                "prone pull-out");
    g_pgEndPh = 0.0f;
    if (g == 1) {
        memcpy(ap_out, ap, sizeof ap);
        PULLOG("%s gate ok: authored attacker spot %.2f m from us", tag,
               (double) da);
    }
    return g;
}

// start the pair `row` of the sequence on the victim (the ap is solved on the victim's body from its own half: it starts where it lies). 1 = started
static int PulPlay(int row, int s, uint64_t h, const char *tag)
{
    float vb[16], v8[8], ap[7];
    const float *vl = PuppetLocator(h, vb);
    if (!vl || !PsLocOk(vl)) {
        PULLOG("%s refused: no victim locator", tag);
        return 0;
    }
    memcpy(v8, vl, sizeof v8);
    // (prone under a truck): the outcome pair was solved on the victim's live locator, still under the truck (the dragged puppet's
    // net body lags / is held by the truck: victim error 1.42 m at the hand-off) -> the attacker was pulled 2.9 m back under the truck and popped onto it.
    // The drag ends with OUR body on its clip end spot (handed to the engine there), so the outcome pair is solved on the attacker instead.
    int on_att = 0, on_vend = 0;
    float skip_s = row == g_pul.krow && g_pul.kskip ? PUL_B_SKIP_S : 0.0f;
    // ps45: every segment after the first (the kill / the grapple; the outcomes with t2r-pullout-vend-out.txt) is solved on the previous segment's victim end pose
    if ((row == g_pul.grow || row == g_pul.krow
         || (PulVendoutOn()
             && (row == g_pul.row[1] || row == g_pul.row[2])))
        && g_pul.grap && g_pul.vend_ok && !PulBlendOld()) {
        float cur[16];
        const float *lv = PuppetLocator(h, cur);
        on_vend = PulSolveApAt(h, row, g_pul.vend, skip_s, ap);
        if (on_vend) {
            float mb[16];
            const float *me = PlayerLocator(mb);
            PULLOG
                ("%s: pair solved on the victim's drag end pose (the victim's live locator is %.2f m from it, we are %.2f m from it) - the victim stays put, the attacker glides in",
                 tag,
                 lv ? (double) __builtin_sqrtf((lv[0] - g_pul.vend[0]) *
                                               (lv[0] - g_pul.vend[0]) +
                                               (lv[2] -
                                                g_pul.vend[2]) * (lv[2] -
                                                                  g_pul.
                                                                  vend[2]))
                 : -1.0,
                 me ? (double) __builtin_sqrtf((me[0] - g_pul.vend[0]) *
                                               (me[0] - g_pul.vend[0]) +
                                               (me[2] -
                                                g_pul.vend[2]) * (me[2] -
                                                                  g_pul.
                                                                  vend[2]))
                 : -1.0);
        }
    }
    if (!on_vend && row != g_pul.row[0] && row != g_pul.krow
        && !g_pul.pounce) {
        float mb[16];
        const float *me = PlayerLocator(mb);
        if (me && PsLocOk(me)) {
            float m8[8];
            memcpy(m8, me, sizeof m8);
            on_att =
                PsyncSolveAp(SID_PLAYER, Fnv64Rt(CD_ATT0(row)), m8, ap);
        }
        if (on_att)
            PULLOG
                ("%s: pair solved on the attacker (the victim's live locator is %.2f m from us)",
                 tag,
                 (double) __builtin_sqrtf((vl[0] - me[0]) *
                                          (vl[0] - me[0]) + (vl[2] -
                                                             me[2]) *
                                          (vl[2] - me[2])));
    }
    // (ps45: was `!on_att` only - the vend solve above was overwritten by a solve on the victim's LIVE locator; t2r-pullout-spblend-off.txt = that old order)
    if (!on_att && (!on_vend || PulSpxOff())
        && !PulSolveApAt(h, row, v8, skip_s, ap)) {
        PULLOG("%s refused: %s has no readable align channels", tag,
               CD_VIC0(row));
        return 0;
    }
    {
        uint64_t me0 = SID_PLAYER;
        DcCall(0x394888944e949d5fULL /* cancel-melee-action */ , 1, &me0);
    }
    g_psStartSkipMs = skip_s * 1000.0f;
    if (!PsyncStart
        (PS_CTXDOWN, (uint32_t) s, CD_ATT0(row), CD_VIC0(row), ap,
         PulDend(row), 0, 0xff, 0, Fnv64Rt(CD_PAIRS[row].name))) {
        PULLOG("%s refused by psync", tag);
        return 0;
    }
    g_pul.uid = g_ps.uid;
    CdHandsBegin(row);
    PULLOG("%s: pair %s = %s on us + %s on slot %d, hand-off %.2f s%s",
           tag, CD_PAIRS[row].name, CD_ATT0(row), CD_VIC0(row), s,
           (double) PulDend(row), g_pul.shortd
           && row ==
           g_pul.row[0] ? " (SHORT drag, uncovered victim)" : "");
    if (row == g_pul.row[0] && g_pul.grap && g_pul.var != PULV_B
        && PulDend(row) > CD_PAIRS[row].end_s + 0.01f)
        PULLOG
            ("SP transition (ps45): the drag runs to %.2f s of its clip (table hand-off %.2f s; SP: phase 0.9976), then the grapple crossfades in over %.2f s smoothstep on its victim end pose",
             (double) PulDend(row), (double) CD_PAIRS[row].end_s,
             (double) PUL_SP_XFADE_S);
    if (row == g_pul.row[2] && PulDend(row) > CD_PAIRS[row].end_s + 0.01f)
        PULLOG
            ("FAIL hold (ps45): hand-off %.2f s (table %.2f s, clip %.2f s) - the victim half stays on through the death punch at %.2f s, no downed kneel before the death",
             (double) PulDend(row), (double) CD_PAIRS[row].end_s,
             (double) AnimLen(SID_PLAYER, CD_VIC0(row)),
             (double) (CD_PAIRS[row].dmax + PUL_KILL_AFTER_MS / 1000.0f));
    if (skip_s > 0.0f)
        PULLOG
            ("%s: the kill clip from %.2f s (past its own approach), solved %s",
             tag, (double) skip_s,
             on_vend ? "on the drag's victim end pose" :
             "on the victim's live pose");
    g_pul.vend_ok = 0;          // (consumed: the next segment captures its own)
    return 1;
}

// ps46 item 1 (ps45a A [8721] 'pulblend: the drag victim half ends at 108.45 -0.96 286.63 yaw -135 (clip 3.31 s, phase 0.994)' = EXACTLY the drag's START anchor
// ([8546] 'ap solved on the anchor 108.45 -0.96 286.63 yaw -135'), while the victim half stood at 110.19 288.52 yaw 45 ([8741] placed -0.13 0.81 yaw 0, err 0.00):
// psync_expected at phase 0.994 of the 3.33 s drag returned the clip's phase-0 align (the sample wrapped). The grapple was solved on it ([8746] 'victim's live
// locator is 2.56 m from it', 'attacker 3.06 m / 180 deg, victim 2.56 m / 180 deg off their spots') - both bodies swung 180 deg over 1 s and SP's grapple camera
// (anchored on that ap, B [10652] ap yaw -106) looked the opposite way of the drag's last frame (B camcont [10660] 'rotation jump 169.6 deg in 1 frame') = the
// drag -> grapple CUT on both screens (clip A 4.47, B 4.53). The kill segments (phase 0.314) and pulls 2 / 3 (0.34 m) were fine. The end pose is now checked
// against the victim half's live placement (psync puts the puppet on the clip: err 0.00): a sample more than PUL_VEND_MAXD / PUL_VEND_MAXYAW off it steps the
// phase back 0.01 at a time (<= 8); none fits = the live placement itself. t2r-prone-cam-cont-ps46-old.txt = the raw sample. Proof line "ps46 vend".
#define PUL_VEND_MAXD 0.60f     /* [TUNING ps46] m (the drag's victim root moves <= ~0.1 m over its last 0.4 s: B [10630]-[10645]) */
#define PUL_VEND_MAXYAW 60.0f   /* [TUNING ps46] deg */
static int PulContOld(void)
{
    static int v = -1;
    static uint64_t at;
    return PulFlag("t2r-prone-cam-cont-ps46-old.txt", &v, &at);
}

static int PulVendSample(const float *a8, const PsyncBf *ib, float vph,
                           float *c)
{
    if (PulContOld())
        return PsyncExpected(g_ps.ent_vic, g_ps.vic_sid, ib, vph, c);
    float lb[16], lv8[8];
    const float *lv =
        g_ps.ent_vic ? PuppetLocator(g_ps.ent_vic, lb) : NULL;
    int live = lv && PsLocOk(lv);
    if (live)
        memcpy(lv8, lv, sizeof lv8);
    for (int i = 0; i <= 8; i++) {
        float ph = vph - 0.01f * (float) i, w[8];
        if (ph < 0.5f)
            break;
        if (!PsyncExpected(g_ps.ent_vic, g_ps.vic_sid, ib, ph, c))
            continue;
        if (!live)
            return 1;
        PsCompose(a8, c, w);
        float dx = w[0] - lv8[0], dz = w[2] - lv8[2], d =
            __builtin_sqrtf(dx * dx + dz * dz), dy =
            __builtin_fabsf(PulWrap(Uc4Yaw(w) - Uc4Yaw(lv8)));
        if (d <= PUL_VEND_MAXD && dy <= PUL_VEND_MAXYAW) {
            PULLOG
                ("ps46 vend: the victim half's end pose sampled at phase %.3f%s - %.2f m / %.0f deg from its live placement",
                 (double) ph,
                 i ? " (stepped back: the later sample wrapped)" : "",
                 (double) d, (double) dy);
            return 1;
        }
        PULLOG
            ("ps46 vend: phase %.3f sample %.2f %.2f %.2f yaw %.0f is %.2f m / %.0f deg off the victim half's live placement %.2f %.2f %.2f yaw %.0f - rejected (a wrapped clip sample)",
             (double) ph, (double) w[0], (double) w[1], (double) w[2],
             (double) Uc4Yaw(w), (double) d, (double) dy, (double) lv8[0],
             (double) lv8[1], (double) lv8[2], (double) Uc4Yaw(lv8));
    }
    if (!live)
        return 0;
    float ia[8];
    PsInverse(a8, ia);
    PsCompose(ia, lv8, c);
    PULLOG
        ("ps46 vend: no clip sample fits - the next segment is solved on the victim half's live placement");
    return 1;
}

static void PulBeginDrag(uint64_t now)
{
    int s = g_pul.slot;
    uint8_t *pe = PulPeer(s);
    uint64_t h = pe ? *(uint64_t *) (pe + 0x60) : 0;
    if (!h || !PulSlotOk(s)) {
        PulAttEnd("the victim is gone / down", 1);
        return;
    }
    const char *busy = PulAttBusy();
    if (busy) {
        PulAttEnd(busy, 1);
        return;
    }
    // pulfix (B frames 198-200 = 3.30-3.34 s, log B [12359]: the grabber reset its pose for a frame when the grab started - a full
    // standing pose for 3 frames between the intro and the drag): the intro was stopped (stop-animating, 0.1 s) on the frame the drag's half was started
    // on the same layer with its 0.24 s blend-in, so the drag faded in over the BASE locomotion pose (standing) instead of over the intro. The drag's
    // animate_ now replaces the intro on the layer and crossfades from its pose (the intro is stopped only when the drag is refused: pul_att_end), and
    // the drag starts PUL_INTRO_LEAD_S before the intro clip's end so the intro never runs out first. t2r-pul-intro-stop-old.txt = old.
    if (GetFileAttributesA("t2r-pul-intro-stop-old.txt") !=
        INVALID_FILE_ATTRIBUTES)
        MhrStop(SID_PLAYER,
                 Fnv64Rt(PulIntroClip(g_pul.set, g_pul.dir)), 0.1f);
    if (!PulPlay(g_pul.row[0], s, h, "DRAG")) {
        g_pul.n_refused++;
        g_pul.cool[s] = now + 1500;
        PulAttEnd("drag refused", 1);
        return;
    }
    PulCamStart(1, s, g_pul.row[0]);
    g_pul.vend_ok = 0;
    g_pulLeadOn = PulLeadMs();
    g_pul.st = PUL_DRAG;
    g_pul.drag_t0 = now;
    g_pul.drag_end =
        now + (uint64_t) (PulDend(g_pul.row[0]) * 1000.0f) -
        g_pulLeadOn;
    g_pul.verdict = 0;
    g_pul.hand_end = g_pul.drag_end;
    PadinjBlock("melee-attack", g_pul.drag_end + 1500);
    EvHitNote((uint32_t) s);
}

// ps45: the PRONE-KILL pair (variant a: the first segment, no pull-out; variant b: after one tug of the side drag, its side re-picked on the drag's victim end pose)
static void PulBeginKill(uint64_t now)
{
    int s = g_pul.slot, first = g_pul.st == PUL_GATE;
    uint8_t *pe = PulPeer(s);
    uint64_t h = pe ? *(uint64_t *) (pe + 0x60) : 0;
    if (!h || !PulSlotOk(s)) {
        PulAttEnd("the victim is gone / down", 1);
        return;
    }
    if (!first) {
        float lb[16], da = 0.0f;
        const float *vl = PuppetLocator(h, lb), *anc =
            g_pul.vend_ok ? g_pul.vend : vl;
        int kr =
            PulKillPick(h, anc, PUL_B_SKIP_S, &da,
                          "variant b: kill side");
        if (kr < 0) {
            g_pul.cool[s] = now + 1500;
            PulAttEnd
                ("variant b: no prone-kill pair here - the pull-out ends",
                 1);
            return;
        }
        g_pul.krow = kr;
    }
    if (!PulPlay
        (g_pul.krow, s, h,
         first ? "KILL (variant a)" : "KILL (variant b)")) {
        g_pul.n_refused += first;
        g_pul.cool[s] = now + 1500;
        PulAttEnd("prone-kill pair refused", 1);
        return;
    }
    if (first)
        PulCamStart(1, s, g_pul.krow);
    else
        PulCamSwap(1, s, g_pul.krow);
    float skip = g_pul.kskip ? PUL_B_SKIP_S : 0.0f;
    if (first) {
        g_pul.drag_t0 = now;
        g_pul.drag_end = now;
    }
    g_pul.st = PUL_KILL;
    g_pul.kill_t0 = now;
    g_pul.kill_end = now + (uint64_t) ((PUL_KILL_END_S - skip) * 1000.0f);
    g_pul.hand_end = g_pul.kill_end;
    g_pul.kpose = 0;
    PadinjBlock("melee-attack", g_pul.kill_end + 1500);
    EvHitNote((uint32_t) s);
    PULLOG
        ("variant %s: KILL %s from %.2f s to its %.2f s hand-off (%s) - the grapple follows, solved on its victim end pose",
         PULV_TAG[g_pul.var & 3], CD_PAIRS[g_pul.krow].name, (double) skip,
         (double) PUL_KILL_END_S,
         first ? "no pull-out before it" : "after one tug of the drag");
}

// the GRAPPLE pair, the moment the drag hands off (solved on the attacker like the outcome pairs - pul_play). Refused = the whole pull-out ends
// (the victim never got its window: no fail without a chance)
static void PulBeginGrap(uint64_t now)
{
    int s = g_pul.slot;
    uint8_t *pe = PulPeer(s);
    uint64_t h = pe ? *(uint64_t *) (pe + 0x60) : 0;
    if (!h || !PulSlotOk(s)) {
        PulAttEnd("the victim is gone / down", 1);
        return;
    }
    if (!PulPlay(g_pul.grow, s, h, "GRAPPLE")) {
        g_pul.cool[s] = now + 1500;
        PulAttEnd
            ("grapple pair refused - the pull-out ends (no window, no fail)",
             1);
        return;
    }
    if (g_pul.pounce)
        PulCamStart(1, s, g_pul.grow);
    else
        PulCamSwap(1, s, g_pul.grow);
    g_pul.st = PUL_GRAP;
    g_pul.grap_t0 = now;
    g_pul.grap_end =
        now + (uint64_t) (CD_PAIRS[g_pul.grow].end_s * 1000.0f);
    g_pul.hand_end = g_pul.grap_end;
    PadinjBlock("melee-attack", g_pul.grap_end + 1500);
    PULLOG
        ("GRAPPLE %s: %.0f ms after the drag started, hand-off %.2f s - the victim's counter window opens %d ms before it (on its pair clock)",
         CD_PAIRS[g_pul.grow].name, (double) (now - g_pul.drag_t0),
         (double) CD_PAIRS[g_pul.grow].end_s, PUL_WIN_LEAD_MS);
}

static void PulStartOutcome(uint64_t now)
{
    int s = g_pul.slot, ok = g_pul.verdict == 1, row =
        g_pul.row[ok ? 1 : 2];
    uint8_t *pe = PulPeer(s);
    uint64_t h = pe ? *(uint64_t *) (pe + 0x60) : 0;
    if (!h) {
        PulAttEnd("the victim is gone", 1);
        return;
    }
    PULLOG("OUTCOME: %s (verdict %s, %lld ms past the %s's hand-off)",
           ok ? "COUNTER - the victim breaks free" : g_pul.
           grap ? "FAIL - the death punch" : "FAIL - the victim is downed",
           g_pul.verdict == 1 ? "the victim's press" : g_pul.verdict ==
           2 ? "the victim's window closed" : "none in time",
           (long long) (now -
                        (g_pul.hand_end ? g_pul.hand_end : g_pul.
                         drag_end)), g_pul.grap ? "grapple" : "drag");
    if (!PulPlay(row, s, h, ok ? "SUCCESS" : "FAIL")) {
        PulAttEnd("outcome pair refused", 1);
        return;
    }
    PulCamSwap(1, s, row);
    g_pul.st = PUL_OUT;
    g_pul.out_t0 = now;
    g_pul.out_end = now + (uint64_t) (PulDend(row) * 1000.0f);
    g_pul.hit_s = CD_PAIRS[row].dmax;
    g_pul.hit_done = 0;
    g_pul.kill_at = 0;
    if (!ok && g_pul.grap && g_pul.out_end < g_pul.out_t0 + (uint64_t) (g_pul.hit_s * 1000.0f) + PulKillAfterMs() + 100)     // (the sequence stays ours until the death punch's lethal follow-up went out; ps46: its new time)
        g_pul.out_end =
            g_pul.out_t0 + (uint64_t) (g_pul.hit_s * 1000.0f) +
            PulKillAfterMs() + 100;
    if (ok)
        g_pul.n_ok++;
    else
        g_pul.n_fail++;
}

// the fail pair's blow: the victim goes down (the net hp as damage through the normal hit path, like the pin's lethal blow)
static void PulFailBlow(int s)
{
    uint8_t *peer = MdgPeer((uint32_t) s);
    if (!peer)
        return;
    float nh =
        !IsBadReadPtr(peer + 0xa5, 4) ? *(float *) (peer + 0xa5) : -1.0f,
        dmg = nh > 0.0f ? nh + 1.0f : 100.0f;
    g_mdu.synthing = 1;
    g_mdmgAuthored = 1;
    DirecthitHook(peer, FNV64("abby-unarmed"), 0, dmg, 0);
    g_mdu.synthing = 0;
    g_mdmgAuthored = 0;
    EvHitNote((uint32_t) s);
    PULLOG("FAIL blow: %.0f damage to slot %d (net hp %.0f) - down%s",
           (double) dmg, s, (double) nh,
           g_pul.
           grap ? "; the death punch's lethal follow-up in 1.3 s" : "");
}

// death punch: the lethal marker (250) through the normal hit path on the (now downed) victim - its downed rule executes ("downed: executed by a hit")
static void PulDeathPunch(int s)
{
    uint8_t *peer = MdgPeer((uint32_t) s);
    if (!peer || g_puppetDeadHere[s]) {
        PULLOG("DEATH PUNCH: slot %d already dead / gone", s);
        return;
    }
    g_mdu.synthing = 1;
    g_mdmgAuthored = 1;
    g_pulDp47 = 1;             // ps47: past the downed-melee filter (t2r-ext.c directhit_hook)
    DirecthitHook(peer, FNV64("abby-unarmed"), 0, 250.0f, 0);
    g_mdu.synthing = 0;
    g_mdmgAuthored = 0;
    g_pulDp47 = 0;
    EvHitNote((uint32_t) s);
    PULLOG("DEATH PUNCH: lethal 250 to slot %d (downed %d)", s,
           (int) g_peerDowned[s]);
}

// ---- ps45: the drag directions to try for the running variant (b / c: the side rows, the nearer first; sp: as before)
static void PulDirsFor(float bear, int ok)
{
    g_pul.ndir = 0;
    if ((g_pul.var == PULV_B || g_pul.var == PULV_C) && ok) {
        int l1 =
            __builtin_fabsf(PulWrap(bear - PUL_DIR_ANG[1])) <=
            __builtin_fabsf(PulWrap(bear - PUL_DIR_ANG[2]));
        g_pul.dirs[g_pul.ndir++] = l1 ? 1 : 2;
        g_pul.dirs[g_pul.ndir++] = l1 ? 2 : 1;
    } else if (g_pul.var == PULV_SP && g_pul.cov && !PulSidesOff() && ok) {
        float key[4];
        int ord[4] = { 0, 1, 2, 3 };
        for (int k = 0; k < 4; k++)
            key[k] = __builtin_fabsf(PulWrap(bear - PUL_DIR_ANG[k]));
        for (int i = 1; i < 4; i++)
            for (int j = i; j > 0 && key[ord[j]] < key[ord[j - 1]]; j--) {
                int t = ord[j];
                ord[j] = ord[j - 1];
                ord[j - 1] = t;
            }
        for (int k = 0; k < 4; k++)
            g_pul.dirs[g_pul.ndir++] = ord[k];
    } else
        g_pul.dirs[g_pul.ndir++] = 0;
}

// ps45: the variant's own needs (a / b: the grapple + the kill clips; a: an open victim and a kill side within PUL_A_FAR, picked here). Returns the variant to play (c on a miss)
static int PulVarSettle(int want, uint64_t h, const float *vl)
{
    int male = !SlotFemale((uint32_t) g_pul.slot), grap_ok =
        !PulNograpple() && g_pul.grow >= 0 && CdPairLoaded(g_pul.grow);
    const char *why = NULL;
    if (want == PULV_A || want == PULV_B) {
        if (!grap_ok)
            why =
                "no grapple pair (t2r-pullout-nograpple.txt / not resident)";
        else if (male && (PulKillMaleOff() || g_pulKillMaleBad))
            why =
                g_pulKillMaleBad ?
                "a male victim showed a bind pose under the Ellie kill half this session"
                : "t2r-prone-kill-male-off.txt (male victim)";
        else if (want == PULV_A && g_pul.cov)
            why =
                "the victim is under cover (a needs open space round it)";
        else if (want == PULV_A) {
            float da = 0.0f;
            int kr = h
                && vl ? PulKillPick(h, vl, 0.0f, &da,
                                      "variant a: kill side") : -1;
            if (kr < 0)
                why = "no prone-kill pair resident / readable";
            else if (da > PUL_A_FAR)
                why =
                    "the nearer kill side's authored attacker spot is too far";
            else
                g_pul.krow = kr;
        } else {
            int k0 = CdPairOfMove(Fnv64Rt(PUL_KILL_NAMES[0])), k1 =
                CdPairOfMove(Fnv64Rt(PUL_KILL_NAMES[1]));
            if (!
                ((k0 >= 0 && CdPairLoaded(k0))
                 || (k1 >= 0 && CdPairLoaded(k1))))
                why = "no prone-kill pair resident";
        }
    }
    if (why) {
        PULLOG("variant %s -> c: %s", PULV_TAG[want & 3], why);
        return PULV_C;
    }
    PULLOG("variant %s (%s)%s", PULV_TAG[want & 3], PULV_NAME[want & 3],
           (want == PULV_A || want == PULV_B)
           && male ?
           " - MALE victim: the kill clip's victim half is Ellie's (pose check 0.2 s in)"
           : "");
    return want;
}

// ps45 a -> c at the gate: the side drags instead
static int PulToC(uint64_t h, const char *why)
{
    float lb[16], mb[16];
    const float *vl = PuppetLocator(h, lb), *me = PlayerLocator(mb);
    if (!vl || !me || !PsLocOk(vl) || !PsLocOk(me))
        return 0;
    g_pul.var = PULV_C;
    g_pul.krow = -1;
    g_pul.kskip = 0;
    g_pul.fb = 1;
    PulDirsFor(PulBearing(h, vl, me), 1);
    for (int gi = 0; gi < g_pul.ndir; gi++) {
        int rows[3], st2 = PulPickSet(g_pul.set, g_pul.dirs[gi], rows);
        if (st2 < 0)
            continue;
        g_pul.set = st2;
        g_pul.gi = gi;
        g_pul.dir = g_pul.dirs[gi];
        memcpy(g_pul.row, rows, sizeof rows);
        g_pul.gate_t0 = 0;
        g_pul.farspot = 0;
        PULLOG("variant a -> c: %s - trying the %s side drag", why,
               PUL_DIR_NAME[g_pul.dir]);
        return 1;
    }
    PULLOG("variant a -> c: %s - but no side drag is resident", why);
    return 0;
}

// ps45: the kill clip's victim half is Ellie's - what a male (or any) victim body shows 0.2 s into it (bind pose = hands > 1 m over the root, > 1.2 m apart)
static void PulKillPoseCheck(void)
{
    int s = g_pul.slot;
    uint64_t h = s >= 0 ? PuppetHandle((uint32_t) s) : 0;
    float lb[16], lh[3], rh[3];
    const float *l = h ? PuppetLocator(h, lb) : NULL;
    if (!l || g_pul.krow < 0)
        return;
    int okl = GxJointPos(h, "l_hand_prop_attachment", lh), okr =
        GxJointPos(h, "r_hand_prop_attachment", rh), male =
        !SlotFemale((uint32_t) s);
    float span = okl
        && okr ? __builtin_sqrtf((lh[0] - rh[0]) * (lh[0] - rh[0]) +
                                 (lh[1] - rh[1]) * (lh[1] - rh[1]) +
                                 (lh[2] - rh[2]) * (lh[2] -
                                                    rh[2])) : -1.0f;
    float hl = okl ? lh[1] - l[1] : -1.0f, hr = okr ? rh[1] - l[1] : -1.0f;
    int bind = okl && okr && span > 1.2f && hl > 1.0f && hr > 1.0f;
    PULLOG
        ("kill pose: %s victim (slot %d) 0.2 s into %s: hands %.2f m apart at %.2f / %.2f m over its root%s",
         male ? "MALE" : "female", s, CD_PAIRS[g_pul.krow].name,
         (double) span, (double) hl, (double) hr,
         bind ? " - BIND POSE (the Ellie half does not fit this body)" :
         okl && okr ? " - posed" : " - joints unreadable");
    if (bind && male && !g_pulKillMaleBad) {
        g_pulKillMaleBad = 1;
        PULLOG
            ("variant: a / b -> c for male victims from now on (this session)");
    }
}

// ---- the attacker, every frame
static void PulAttTick(uint64_t now)
{
    int e = PulPressEdge(now);
    if (g_pul.st == PUL_IDLE) {
        if (!e)
            return;
        const char *busy = PulAttBusy();
        if (busy) {
            static int nl;
            if (LOGCAP(nl, 40))
                Xlog("pullout: press ignored: %s", busy);
            return;
        }
        float vb[16], mb[16];
        const char *why = "";
        int cov = 0;
        float rng = 0.0f;
        int pvar = PulVarPick();
        int s = PulFind(vb, mb, &why, &cov, &rng, pvar != PULV_SP);
        if (s < 0) {
            static int nl;
            if (strcmp(why, "no enemy near") && LOGCAP(nl, 60))
                Xlog("pullout: press: %s", why);
            return;
        }
        // the drag directions to try: a covered victim, any side (nearest bearing first - the gate drops a row whose authored attacker spot is far or blocked); otherwise the feet row
        uint8_t *pe0 = PulPeer(s);
        uint64_t h0 = pe0 ? *(uint64_t *) (pe0 + 0x60) : 0;
        float lb0[16];
        const float *vl0 = h0 ? PuppetLocator(h0, lb0) : NULL;
        float bear = vl0 ? PulBearing(h0, vl0, mb) : 180.0f;
        // ps45: the variant first (its checks pick the kill side for a), then its drag directions
        g_pul.slot = s;
        g_pul.cov = cov;
        g_pul.krow = -1;
        g_pul.kskip = 0;
        g_pul.fb = 0;
        g_pul.ncyc++;
        g_pul.grow =
            CdPairOfMove(Fnv64Rt
                            (PUL_GRAP_NAMES
                             [SlotFemale((uint32_t) s) ? 1 : 0]));
        g_pul.var = PulVarSettle(pvar, h0, vl0);
        PulDirsFor(bear, vl0 != NULL);
        int set = -1, gi = 0;
        for (; gi < g_pul.ndir; gi++) {
            set =
                PulPickSet(SlotFemale((uint32_t) s) ? 1 : 0,
                             g_pul.dirs[gi], g_pul.row);
            if (set >= 0)
                break;
        }
        if (set < 0) {
            g_pul.slot = -1;
            g_pul.krow = -1;
            static int nl;
            if (LOGCAP(nl, 3))
                Xlog("pullout: no pull-out clip set is resident here (the SP-common paks / pullout_rows.h): nothing to play");
            return;
        }
        g_pul.set = set;
        g_pul.gi = gi;
        g_pul.dir = g_pul.dirs[gi];
        g_pul.farspot = 0;
        // a prone player NOT under a truck / collision is only pounced on (the pull-out is the way OUT from cover): no intro, no drag - the grapple pair from the pounce
        g_pul.grow = CdPairOfMove(Fnv64Rt(PUL_GRAP_NAMES[set & 1]));
        int unc = !cov && !PulPounceOff() && !PulNograpple()
            && !PulCoverOff() && g_pul.grow >= 0
            && CdPairLoaded(g_pul.grow);
        // sp plays the full drag for an uncovered victim too; the one tug = t2r-pullout-shortdrag.txt
        g_pul.pounce = g_pul.var == PULV_SP && unc && PulPounceDirect();
        g_pul.shortd = g_pul.var == PULV_SP && unc && !g_pul.pounce
            && PulShortdragOn();
        g_pul.kskip = g_pul.var == PULV_B;
        g_pul.id = (g_pul.id + 1) & 0xff;
        g_pul.gate_t0 = 0;
        g_pul.verdict = 0;
        memcpy(g_pul.vpos, vb, sizeof g_pul.vpos);
        {
            uint64_t me0 = SID_PLAYER;
            DcCall(0x394888944e949d5fULL /* cancel-melee-action */ , 1,
                    &me0);
        }                       // (the press also started a normal swing)
        PadinjBlock("melee-attack", now + 3500);
        float dx = mb[0] - vb[0], dz = mb[2] - vb[2];
        if (MduEngaged())
            MduEnd("a pull-out took over", 1, MDUO_END);
        PULLOG
            ("TRIGGER on slot %d (%s set, %s victim): covered=%d, %.2f m away (root %.2f m), %.0f deg off its back, bearing round its body %.0f deg -> first drag %s of %d to try",
             s, set ? "Ellie's" : "Abby's",
             SlotFemale((uint32_t) s) ? "female" : "male", cov,
             (double) rng, (double) __builtin_sqrtf(dx * dx + dz * dz),
             (double) PulWrap(__builtin_atan2f(dx, dz) * 57.29578f -
                               Uc4Yaw(vb) - 180.0f), (double) bear,
             PUL_DIR_NAME[g_pul.dir], g_pul.ndir);
        PULLOG("variant %s for this pull-out (%s)%s",
               PULV_TAG[g_pul.var & 3], g_pul.var == PULV_A
               && g_pul.krow >=
               0 ? CD_PAIRS[g_pul.krow].name : PULV_NAME[g_pul.var & 3],
               pvar != g_pul.var ? " - a fallback" : "");
        g_pul.st = PUL_GATE;
    }
    if (g_pul.st == PUL_GATE) { // the path gate first (a blocked pull-out never shows its intro); 2 = rays in flight
        int s = g_pul.slot;
        uint8_t *pe = PulPeer(s);
        uint64_t h = pe ? *(uint64_t *) (pe + 0x60) : 0;
        if (!h || !PulSlotOk(s)) {
            PulAttEnd("the victim is gone / down", 0);
            return;
        }
        const char *busy = PulAttBusy();
        if (busy) {
            PulAttEnd(busy, 0);
            return;
        }
        float ap[7];
        int g =
            PulGate(g_pul.var ==
                     PULV_A ? g_pul.krow : g_pul.pounce ? g_pul.
                     grow : g_pul.row[0], s, h, ap, "GATE");
        if (g == 2) {
            if (!g_pul.gate_t0)
                g_pul.gate_t0 = now;
            if (now - g_pul.gate_t0 < 300)
                return;
            PULLOG("path gate unanswered after 300 ms - refused");
            g = 0;
        }
        if (!g) {
            if (g_pul.var == PULV_A) {
                if (PulToC
                    (h,
                     g_pul.
                     farspot ?
                     "the kill pair's authored attacker spot is too far" :
                     "the kill pair's path is blocked"))
                    return;
            }
            // a covered victim (ps45: or the side rows of b / c): the next drag direction (its row must be resident; the outcome rows are shared)
            while ((g_pul.cov || g_pul.ndir > 1)
                   && g_pul.gi + 1 < g_pul.ndir) {
                int ni = ++g_pul.gi, rows[3], st2 =
                    PulPickSet(g_pul.set, g_pul.dirs[ni], rows);
                if (st2 < 0)
                    continue;
                PULLOG("GATE: drag %s refused (%s) - trying %s",
                       PUL_DIR_NAME[g_pul.dir],
                       g_pul.
                       farspot ? "its attacker spot is too far" :
                       "path blocked", PUL_DIR_NAME[g_pul.dirs[ni]]);
                g_pul.set = st2;
                g_pul.dir = g_pul.dirs[ni];
                memcpy(g_pul.row, rows, sizeof rows);
                g_pul.gate_t0 = 0;
                g_pul.farspot = 0;
                return;         // (the next frame gates the new row)
            }
            if ((g_pul.var == PULV_B || g_pul.var == PULV_C) && !g_pul.cov && !g_pul.fb && g_pul.var != PULV_A) {       // ps45 b / c -> a: an open victim, every side drag refused
                float lb[16];
                const float *vl = PuppetLocator(h, lb);
                int was = g_pul.var;
                if (vl && PsLocOk(vl)
                    && PulVarSettle(PULV_A, h, vl) == PULV_A) {
                    g_pul.var = PULV_A;
                    g_pul.fb = 1;
                    g_pul.kskip = 0;
                    g_pul.gate_t0 = 0;
                    g_pul.farspot = 0;
                    PULLOG
                        ("variant %s -> a: every side drag was refused, the victim is in the open - %s",
                         PULV_TAG[was & 3], CD_PAIRS[g_pul.krow].name);
                    return;
                }
                g_pul.var = was;
                g_pul.krow = -1;
            }
            g_pul.n_refused++;
            g_pul.cool[s] = now + 1500;
            PulAttEnd(g_pul.
                        farspot ? "no drag direction fits where we stand" :
                        "path blocked (wall / drop / the vehicle itself)",
                        0);
            return;
        }
        if (g_pul.var == PULV_A) {      // ps45 a: no pull-out - the prone-kill pair at once (the grab + the roll), then the grapple
            g_pul.grap = 1;
            g_pul.n_started++;
            g_pul.gate_t0 = 0;
            g_pul.vend_ok = 0;
            g_pul.verdict = 0;
            uint8_t pa[1] =
                { (uint8_t) (g_pul.
                             set | (g_pul.dir << 1) | 8 | (PULV_A << 5)) };
            PulSend(s, PUL_OP_INTRO, g_pul.id, pa, 1); // (bits 5-6 = the variant: the victim plays no intro clip)
            PulBeginKill(now);
            return;
        }
        if (g_pul.pounce) {     // the pounce: the grapple pair at once (solved on the victim's body, the attacker glides in - pul_play)
            g_pul.grap = 1;
            g_pul.n_started++;
            g_pul.gate_t0 = 0;
            g_pul.drag_t0 = now;
            g_pul.drag_end = now;
            g_pul.vend_ok = 0;
            g_pul.verdict = 0;
            uint8_t pp[1] =
                { (uint8_t) (g_pul.set | (g_pul.dir << 1) | 8 | 16) };
            PulSend(s, PUL_OP_INTRO, g_pul.id, pp, 1); // (bit 4 = pounce: the victim takes the grapple pair with no drag before it)
            PULLOG
                ("POUNCE on slot %d: not under cover - no pull-out, the grapple pair straight from the pounce",
                 s);
            PulBeginGrap(now);
            return;
        }
        float il =
            AnimLen(SID_PLAYER, PulIntroClip(g_pul.set, g_pul.dir));
        if (!(il > 0.5f))
            il = 1.05f;
        if (il > 1.5f)
            il = 1.5f;
        uint64_t r =
            MhrAnimate(SID_PLAYER,
                        Fnv64Rt(PulIntroClip(g_pul.set, g_pul.dir)),
                        0.0f, 0.15f);
        float lead = GetFileAttributesA("t2r-pul-intro-stop-old.txt") == INVALID_FILE_ATTRIBUTES ? PUL_INTRO_LEAD_S : 0.0f;     // (pul_begin_drag: the drag crossfades from the intro's last frames)
        g_pul.st = PUL_INTRO;
        g_pul.intro_end = now + (uint64_t) ((il - lead) * 1000.0f);
        g_pul.n_started++;
        g_pul.gate_t0 = 0;
        PulCamIntro(s, g_pul.row[0], il);
        g_pul.grow =
            CdPairOfMove(Fnv64Rt(PUL_GRAP_NAMES[g_pul.set & 1]));
        g_pul.grap = !PulNograpple() && g_pul.grow >= 0
            && CdPairLoaded(g_pul.grow);
        PULLOG("grapple flow %s (%s%s)", g_pul.grap ? "ON" : "off",
               PUL_GRAP_NAMES[g_pul.set & 1],
               PulNograpple()? ", t2r-pullout-nograpple.txt" : g_pul.
               grow <
               0 ? ", no row" : !g_pul.grap ? ", clips not resident" : "");
        if (g_pul.var == PULV_B && !g_pul.grap) {
            g_pul.var = PULV_C;
            g_pul.kskip = 0;
            PULLOG("variant b -> c: no grapple flow");
        }
        uint8_t p[1] =
            { (uint8_t) (g_pul.
                         set | (g_pul.dir << 1) | (g_pul.
                                                   grap ? 8 : 0) | ((g_pul.
                                                                     var &
                                                                     3) <<
                                                                    5)) };
        PulSend(s, PUL_OP_INTRO, g_pul.id, p, 1);
        PULLOG
            ("INTRO %s on us (%.2f s, animate -> %llx): the tell - the victim is still free; the drag follows",
             PulIntroClip(g_pul.set, g_pul.dir), (double) il,
             (unsigned long long) r);
        return;
    }
    if (g_pul.st == PUL_INTRO) {
        const char *busy = PulAttBusy();
        if (busy) {
            PulAttEnd(busy, 1);
            return;
        }
        if (!PulSlotOk(g_pul.slot)) {
            PulAttEnd("the victim is gone / down", 1);
            return;
        }
        if (now >= g_pul.intro_end) {   // re-validate: the victim may have stood up / crawled off during the tell
            uint8_t *pe = PulPeer(g_pul.slot);
            uint64_t h = pe ? *(uint64_t *) (pe + 0x60) : 0;
            float vb[16], mb[16], hd[3];
            const float *vl = h ? PuppetLocator(h, vb) : NULL, *me =
                PlayerLocator(mb);
            if (!vl || !me || !PsLocOk(vl) || !PsLocOk(me)) {
                PulAttEnd("no locators", 1);
                return;
            }
            float dx = me[0] - vl[0], dz = me[2] - vl[2], d =
                __builtin_sqrtf(dx * dx + dz * dz);
            float moved =
                __builtin_sqrtf((vl[0] - g_pul.vpos[0]) * (vl[0] -
                                                           g_pul.vpos[0]) +
                                (vl[2] - g_pul.vpos[2]) * (vl[2] -
                                                           g_pul.vpos[2]));
            if (!PulAny()
                && (!GxJointPos(h, "headb", hd)
                    || hd[1] - vl[1] >= PUL_PRONE_HEAD)) {
                g_pul.cool[g_pul.slot] = now + 1500;
                PulAttEnd("the victim stood up during the intro", 1);
                return;
            }
            if (g_pul.cov)
                d = PulNear(me, h, vl);
            if (d > PUL_RANGE + 0.5f
                || (!g_pul.cov && g_pul.var == PULV_SP
                    &&
                    __builtin_fabsf(PulWrap
                                    (__builtin_atan2f(dx, dz) * 57.29578f -
                                     Uc4Yaw(vl) - 180.0f)) >
                    PUL_CONE_DEG + 20.0f)) {
                g_pul.cool[g_pul.slot] = now + 1500;
                PulAttEnd("the victim left the cone during the intro",
                            1);
                return;
            }
            if (moved > 0.3f)
                PULLOG
                    ("the victim crawled %.2f m during the intro - the drag is placed on its body now",
                     (double) moved);
            PulBeginDrag(now);
        }
        return;
    }
    if (g_pul.st == PUL_DRAG || g_pul.st == PUL_WAITRES
        || g_pul.st == PUL_OUT || g_pul.st == PUL_GRAP
        || g_pul.st == PUL_KILL) {
        int ours = g_ps.role == 1 && g_ps.uid == g_pul.uid;
        // pulblend (ps45: every segment): the running drag / kill victim half's pose at its hand-off, in its ap - the next segment is solved on it
        if (g_pul.grap && (g_pul.st == PUL_DRAG || g_pul.st == PUL_KILL)
            && ours && !g_pul.vend_ok
            && now + 400 >= (g_pul.st ==
                             PUL_KILL ? g_pul.kill_end : g_pul.drag_end)
            && !PulBlendOld()) {
            static const float id7[7] = { 0, 0, 0, 0, 0, 0, 1 };
            PsyncBf ib;
            PsyncBfMake(&ib, id7);
            float a8[8], c[8];
            PsAp8(g_ps.ap, a8);
            float vph = 0.995f, vdur =
                PsyncDur(g_ps.ent_vic, g_ps.vic_sid), hs =
                PulDend(g_pul.st == PUL_KILL ? g_pul.krow : g_pul.row[0]);
            if (vdur > 0.2f && hs / vdur < vph)
                vph = hs / vdur;
            if (PulVendSample(a8, &ib, vph, c)) {     // ps46 item 1 (the note at PUL_VEND_MAXD)
                PsCompose(a8, c, g_pul.vend);
                g_pul.vend_ok = 1;
                PULLOG
                    ("pulblend: the %s victim half ends at %.2f %.2f %.2f yaw %.0f (clip %.2f s, phase %.3f) - the next segment is solved on it, not on the attacker",
                     g_pul.st == PUL_KILL ? "kill" : "drag",
                     (double) g_pul.vend[0], (double) g_pul.vend[1],
                     (double) g_pul.vend[2], (double) Uc4Yaw(g_pul.vend),
                     (double) hs, (double) vph);
            }
        }
        if (g_pul.st == PUL_KILL && !g_pul.kpose
            && now >= g_pul.kill_t0 + 200) {
            g_pul.kpose = 1;
            PulKillPoseCheck();
        }
        // (at 150 ms: the drag pair's own hand-off (2.85 s) ended psync on the frame the drag ended (2.84 s) and this check ran first - "pair aborted",
        //  the verdict that came in the same frame was dropped and the victim was never downed. The drag's end is taken first (100 ms slack), and after it an
        //  ended pair is normal: the verdict wait runs on. t2r-pul-gone-old.txt = old order.)
        int gone_new =
            GetFileAttributesA("t2r-pul-gone-old.txt") ==
            INVALID_FILE_ATTRIBUTES;
        // grapple: the drag's hand-off starts the grapple pair once psync ended the drag pair at its own hand-off (a psync_start over a running pair would
        // abort it to the victim first); 150 ms past the hand-off it starts anyway. Its hand-off -> the verdict wait
        // lead (the note at PUL_LEAD_MS): our drag copy ends here, locally, PUL_LEAD_MS before its pair end - the grapple START goes out while the victim's half still plays
        if (g_pul.grap && g_pul.st == PUL_DRAG && g_pulLeadOn
            && g_pul.var != PULV_B && ours && now >= g_pul.drag_end) {
            PULLOG
                ("ps46b lead: drag u%08x ended locally %llu ms before its pair end (no ABORT sent: the victim's drag half plays on) - the grapple START goes out now",
                 g_ps.uid, (unsigned long long) g_pulLeadOn);
            g_pulLeadOn = 0;
            PsyncAbort("pull-out lead: the grapple takes over the drag",
                        0);
            PulBeginGrap(now);
            return;
        }
        if (g_pul.grap && g_pul.st == PUL_DRAG
            && ((!ours && now + 300 >= g_pul.drag_end)
                || now >= g_pul.drag_end + 150)) {
            if (g_pul.var == PULV_B)
                PulBeginKill(now);
            else
                PulBeginGrap(now);
            return;
        }
        if (g_pul.st == PUL_KILL
            && ((!ours && now + 300 >= g_pul.kill_end)
                || now >= g_pul.kill_end + 150)) {
            PulBeginGrap(now);
            return;
        }                       // ps45 a / b: the kill's hand-off -> the grapple
        if (g_pul.st == PUL_GRAP
            && (now + 100 >= g_pul.grap_end
                || (!ours && now + 300 >= g_pul.grap_end))) {
            g_pul.st = PUL_WAITRES;
            g_pul.res_ms = now;
        }
        if (gone_new && !g_pul.grap && g_pul.st == PUL_DRAG
            && (now + 100 >= g_pul.drag_end
                || (!ours && now + 300 >= g_pul.drag_end))) {
            g_pul.st = PUL_WAITRES;
            g_pul.res_ms = now;
        }
        if (!ours && !g_pul.kill_at
            && !(gone_new && g_pul.st == PUL_WAITRES)
            && now - (g_pul.st == PUL_OUT ? g_pul.out_t0 : g_pul.st ==
                      PUL_GRAP ? g_pul.grap_t0 : g_pul.st ==
                      PUL_KILL ? g_pul.kill_t0 : g_pul.drag_t0) > 400
            && !(g_pul.st == PUL_OUT && now >= g_pul.out_end)) {
            PULLOG
                ("the pair is gone (the victim refused / aborted, psync role %d)",
                 g_ps.role);
            g_pul.cool[g_pul.slot] = now + 2000;
            PulAttEnd("pair aborted", 1);
            return;
        }
        if (g_pul.st == PUL_DRAG && !g_pul.grap && now >= g_pul.drag_end) {
            g_pul.st = PUL_WAITRES;
            g_pul.res_ms = now;
        }
        if (g_pulc.intro && !g_pulc.sent && now - g_pul.drag_t0 >= 150) {       // v2: our camera is procedural - so is the victim's (it waits for this, else tests SP's shots itself)
            uint8_t p0[1] = { 0 };
            PulSend(g_pul.slot, PUL_OP_CAM, g_pul.id, p0, 1);
            g_pulc.sent = 1;
        }
        // (the victim's window runs on its pair clock (one-way late) and its verdict travels one-way back - a valid press late in the window reaches us
        //  up to the hand-off + 140 ms + the round trip; a flat 220 ms wait judged it FAIL here while the victim played the escape. t2r-pul-wait-flat.txt = old.)
        if (g_pul.st == PUL_WAITRES) {
            uint64_t wait = PUL_RES_WAIT_MS;
            if (GetFileAttributesA("t2r-pul-wait-flat.txt") ==
                INVALID_FILE_ATTRIBUTES && g_pul.slot >= 0
                && g_psRtt[g_pul.slot] > 0.0f) {
                wait += (uint64_t) g_psRtt[g_pul.slot];
                if (wait > 900)
                    wait = 900;
            }
            if (g_pul.verdict || now - g_pul.res_ms >= wait)
                PulStartOutcome(now);
        } else if (g_pul.st == PUL_OUT) {
            if (g_pul.verdict != 1 && !g_pul.hit_done
                && now >=
                g_pul.out_t0 + (uint64_t) (g_pul.hit_s * 1000.0f)) {
                g_pul.hit_done = 1;
                PulFailBlow(g_pul.slot);
                if (g_pul.grap)
                    g_pul.kill_at = now + PulKillAfterMs();
            }
            if (g_pul.kill_at && now >= g_pul.kill_at) {
                g_pul.kill_at = 0;
                PulDeathPunch(g_pul.slot);
            }
            if (now >= g_pul.out_end) {
                g_pul.cool[g_pul.slot] =
                    now + (g_pul.verdict ==
                           1 ? PUL_COOL_OK_MS : PUL_COOL_FAIL_MS);
                PulAttEnd(g_pul.verdict ==
                            1 ? "countered - the victim got away" :
                            "the victim was downed", 1);
            }
        }
    }
}

// ---- the victim
static void PulHandBody(void);
static void PulVicEnd(const char *why)
{
    if (!g_pulv.on)
        return;
    PulCamEnd(why, g_pulv.out
                && (!strcmp(why, "success pair over")
                    || !strcmp(why, "fail pair over")));
    PULLOG("victim side END (%s): slot %d, counter %s", why, g_pulv.slot,
           g_pulv.ok ? "valid" : g_pulv.
           burnt ? "too early" : "none / late");
    MdgPromptWithdraw("pullout over");
    // after a successful escape melee stayed locked the victim lock's whole 240 frames (~2 s of dead F presses after the pair) -
    // the escape is over when the success pair is, like a pin escape (melee_duel_pin.c)
    if (g_pulv.ok && g_meleeLockVictim && g_meleeLockFrom) {
        g_meleeLockFrom = 0;
        PULLOG("escaped - melee input unlocked at the pair's end");
    }
    if (!g_pulv.decided)
        PullstingCut(200, "drag ended with no verdict");
    // psync placed our live body on the clips (paired.c psync_own_vic_needs_drive): the engine's player state is still where the controller stopped
    // (under the vehicle) - move it to where the body is now, once, or it rubber-bands back at the first step
    PulHandBody();
    g_pulv.on = 0;
    g_pulv.slot = -1;
    g_pulv.grap = 0;
}

// ctxdown.c cd_victim_begin (kind 8): a pull-out pair on us. The drag needs us prone, the outcome pairs the running pull-out. 1 = accepted
static int PulVictimBegin(uint32_t slot, float end_s, uint64_t skipf,
                            int pr)
{
    uint64_t now = GetTickCount64();
    const char *no = NULL;
    int drag0 = !strncmp(CD_PAIRS[pr].name, "pu-drag", 7), ok =
        !strncmp(CD_PAIRS[pr].name, "pu-ok", 5), grap =
        !strncmp(CD_PAIRS[pr].name, "pu-grap", 7);
    int kill = !strncmp(CD_PAIRS[pr].name, "pu-kill", 7), kfirst = kill && !(g_pulv.on && g_pulv.slot == (int) slot), drag = drag0 || kfirst;   // ps45 a: the kill pair opens the sequence like a drag
    int pounce = grap && !g_pulv.on && g_pul_igrap.slot == (int) slot && g_pul_igrap.pounce && GetTickCount64() - g_pul_igrap.ms < 6000;        // the grapple pair with no drag before it
    if (PulOff())
        no = "switched off here";
    else if (slot > T2R_MAX_PEERS - 1 || MatchSubstate() != 6
             || !__builtin_isfinite(end_s) || end_s < (kill ? 0.25f : 0.5f)
             || end_s > 6.0f)
        no = "bad start";
    else if (g_downed || g_downDying || g_execUntil || g_uc4DyingUntil
             || g_downAt || LocalHp() <= 0.0f)
        no = "we are down / dying / in an execution";
    else if ((drag || pounce)
             && (g_dcm.on_att || g_dcm.on_vic || g_msq.on || g_pulv.on))
        no = "already in a paired move";
    else if (drag && !PulAny() && !PulLocalProne())
        no = "we are not prone";
    else if (!drag && !pounce && (!g_pulv.on || g_pulv.slot != (int) slot))
        no = "no pull-out is running on us";
    if (no) {
        PULLOG("%s on us by slot %u refused: %s", CD_PAIRS[pr].name, slot,
               no);
        return 0;
    }
    uint8_t *peer = MdgPeer(slot);
    uint64_t ah = peer ? *(uint64_t *) (peer + 0x60) : 0;
    uint64_t left = S60(end_s) > skipf + 3 ? S60(end_s) - skipf : 3;
    if (g_mhr.on)
        MhrEnd("the pull-out pair takes the body", 1);
    g_mhs.on = 0;
    g_mhs.react = 0;
    MeleeLockStart(1);
    Uc4OvOffExec();
    if (g_uc4_me.cur)
        Uc4Stop(&g_uc4_me);
    if (g_uc4_pup[slot].cur)
        Uc4Stop(&g_uc4_pup[slot]);
    FinposBegin(2, (int) slot, 0, left + 60, "prone pull-out on us");
    if (ah)
        XfaceStart(ah, SID_PLAYER, end_s + 0.6f);
    g_pulv.uid = g_ps.uid;
    g_pulv.row = pr;
    g_pulv.last_pair = now;
    if (drag || pounce) {
        g_pulv.on = 1;
        g_pulv.slot = (int) slot;
        g_pulv.end_s = end_s;
        g_pulv.shown_t = 0;
        g_pulv.burnt = 0;
        g_pulv.decided = 0;
        g_pulv.ok = 0;
        g_pulv.shown = -1;
        g_pulv.late_until = 0;
        g_pulv.out = 0;
        g_pulv.id = (g_pulv.id + 1) & 0xff;
        g_pulv.hit_at = 0;
        g_pulv.grap = g_pul_igrap.slot == (int) slot
            && now - g_pul_igrap.ms < 6000 ? 1 : 0;
        g_pul_igrap.slot = -1;
        g_pul_igrap.pounce = 0;
        if (!pounce)
            PulCamStart(2, (int) slot, pr);   // (a pounce: the grapple branch below starts the camera on its own pair)
        PullstingGrab();       // victim music: combat riser under the drag (pullout_sting.c)
        if (pounce)
            g_pulv.grap = 1;
        if (pounce)
            PULLOG
                ("POUNCE on us by slot %u: no drag, the grapple pair follows at once",
                 slot);
        else if (kfirst)
            PULLOG
                ("KILL on us by slot %u accepted (variant a, no pull-out): %s, hand-off %.2f s - the grab + the roll, the GRAPPLE follows (a press before its window = Too early)",
                 slot, CD_VIC0(pr), (double) end_s);
        else if (g_pulv.grap)
            PULLOG
                ("DRAG on us by slot %u accepted: %s, hand-off %.2f s - a GRAPPLE follows: the counter window waits for it (a press before = Too early)",
                 slot, CD_VIC0(pr), (double) end_s);
        else
            PULLOG
                ("DRAG on us by slot %u accepted: %s, hand-off %.2f s, the counter window opens %d ms before it for %d ms (+%d ms grace) on our pair clock",
                 slot, CD_VIC0(pr), (double) end_s, PUL_WIN_LEAD_MS,
                 PUL_WIN_MS, PUL_GRACE_MS);
    }
    if (pounce || (!drag && grap)) {    // the grapple pair - the window runs on ITS clock now (a press during the drag already burnt it: anti-mash)
        g_pulv.grap = 2;
        g_pulv.end_s = end_s;
        g_pulv.shown_t = 0;
        g_pulv.shown = -1;
        g_pulv.late_until = 0;
        g_pulv.decided = 0;
        g_pulv.ok = 0;
        if (pounce)
            PulCamStart(2, (int) slot, pr);
        else
            PulCamSwap(2, (int) slot, pr);
        PULLOG
            ("GRAPPLE on us by slot %u accepted: %s, hand-off %.2f s, the counter window opens %d ms before it for %d ms (+%d ms grace) on our pair clock%s",
             slot, CD_VIC0(pr), (double) end_s, PUL_WIN_LEAD_MS,
             PUL_WIN_MS, PUL_GRACE_MS,
             g_pulv.
             burnt ? " - already TOO EARLY (pressed during the drag)" :
             "");
    } else if (kill) {          // ps45 b: the prone-kill roll between the drag and the grapple (no window yet)
        if (!kfirst) {
            PulCamSwap(2, (int) slot, pr);
            PULLOG
                ("KILL on us by slot %u (variant b): %s, the roll onto our back, hand-off %.2f s - the GRAPPLE follows",
                 slot, CD_VIC0(pr), (double) end_s);
        }
    } else if (!drag) {
        // the success pair's counter blow lands on the attacker at the row's blow moment (our client sends the hit: the victim half wins the exchange)
        g_pulv.out = ok ? 1 : 2;
        g_pulv.out_ms = now;
        g_pulv.sup = 0;
        g_pulv.sup_try = 0;
        g_pulv.sup_ms = 0;
        g_pulv.hit_at =
            ok ? now + (uint64_t) (CD_PAIRS[pr].dmax * 1000.0f) -
            (skipf * 1000ull / 60ull) : 0;
        PulCamSwap(2, (int) slot, pr);
        PullstingOutcome(ok, ok ? 0.0f : CD_PAIRS[pr].dmax - (float) skipf / 60.0f);   // victim blow feel: spike + camera kick at the attacker's fail blow
        PULLOG("%s on us by slot %u accepted: %s, hand-off %.2f s",
               CD_PAIRS[pr].name, slot, CD_VIC0(pr), (double) end_s);
    }
    return 1;
}

static void PulVicVerdict(int ok, int rel)
{
    g_pulv.decided = 1;
    g_pulv.ok = ok;
    uint8_t p[3] = { (uint8_t) ok, (uint8_t) rel, (uint8_t) (rel >> 8) };
    PulSend(g_pulv.slot, PUL_OP_RES, g_pulv.id, p, 3);
    MdgPromptWithdraw("pullout verdict");
    PullstingResult(ok);       // victim music: escape loop / fail end tag (pullout_sting.c)
    PULLOG("COUNTER %s: verdict sent to slot %d (%d ms into the window)",
           ok ? "VALID" : "FAILED (the window closed with no valid press)",
           g_pulv.slot, rel);
}

// pulfix (after a successful escape the victim stays on their back - supine, can aim and fire - until they stand up by their own input).
// SP reference (SP recorder logs, episode 8, F=32797-32842): npc-prone-struggle-success / ellie-pullout-success--ellie
// (2.6 s) hands the player straight to the engine's supine state - OV ellie-supine-idle + s_prone-supine + s_prone-ground-ik at F=32803 - and the
// script then points the camera with force-camera-aim-point for 38 frames (F=32805-32842) while Ellie lies there aiming (ellie-prone-p-aim^supine).
// The engine route (exe 1.6.10721, disassembled; re/review/pullout-1031/ab-supine-1031.log = the failed first try):
//  * send-event go-supine -> the player's handler 0x140c6a5e0: base layer request-transition "to-supine" (0x1416048d0), then
//    PlayerStealthController (player+0x31f8, vtable 0x142ce4cf0) slot 0x1f0 = 0x140cdcf90 GoProne(mode 3, anim 1, 0, blend, 0): spawns a NEW
//    CharacterProne subsystem (type 0x86fd8673791cc07e) with spawn mode 3 - unless one is already the top subsystem (then only the transition).
//    (player-force-prone = the same call with mode 6.)  CharacterProne::Init 0x140a604a0: mode 3/4/5 -> supine flag (+0x1e1, set by
//    0x140a68270, which also switches the overlay set prone-position -> "supine") and the first state s_prone-supine (0x9da7cbfba0afe740).
//  * player-is-supine? = Player vfunc 0xb78 (0x140db5170) = that CharacterProne's +0x1e1 byte (it answered 0 in MP because no CharacterProne
//    was ever supine, not because the native is dead).
//  * WHY THE FIRST TRY FLIPPED TO THE BELLY: CharacterProne re-decides the flag every frame from the LOOK direction (0x140a5cdd0, called by
//    the update 0x140a69b90 when the action mode is 4): the camera's yaw round the body, smoothed, in degrees - < 67.5 or > 292.5 (looking
//    past the head) clears supine (= roll to the belly), 112.5..247.5 (looking down along the feet) sets it, between keeps it. Our camera at
//    the hand-off (the SP replay's held last frame) looked over the head. SP avoids it with its own force-camera-aim-point for 0.63 s right
//    after the hand-off. A second clearer (0x140a6c760: the idle action's entry check of the anim state's tag 0x1a027fc61283aa7) resets the
//    flag when the base layer did not reach a supine state.
// Now, the frame the outcome pair lets go of our body:
//  1. the engine player goes to the body with the locator turned to the FEET direction when the clip's locator disagrees by > 45 deg (supine-idle's
//     head lies behind its locator - supine_body_test's rule), so supine-idle continues the clip's body instead of turning it;
//  2. go-supine (mode 3 -> s_prone-supine directly, no belly);
//  3. force-camera-aim-point every frame for PUL_SUP_AIM_MS at a point down along the feet (SP's 38 frames), so the engine's own look rule keeps supine;
//  4. while the camera turns (PUL_SUP_HOLD_MS) a hook on 0x140a68270 keeps OUR CharacterProne's supine flag when the engine tries to clear it
//     (t2r-pullout-nosuphold.txt = no hold) - every write to it for 4 s is logged with its caller ("supine flag ... from exe+...").
// Afterwards the stance is the engine's: supine aim / fire / stand-up by the player's own input; turning the camera over the head rolls to the
// belly as in SP. Other screens: the puppet replays our AnimCmdList (supine-idle etc.), no extra sync. t2r-pullout-nosupine.txt = the old stand-up.
#define PUL_SUP_AIM_MS 650
#define PUL_SUP_HOLD_MS 1500
#define PUL_SUP_WATCH_MS 4000
static int PulSupineOff(void)
{
    static int v = -1;
    static uint64_t at;
    return PulFlag("t2r-pullout-nosupine.txt", &v, &at);
}

static int PulSupholdOff(void)
{
    static int v = -1;
    static uint64_t at;
    return PulFlag("t2r-pullout-nosuphold.txt", &v, &at);
}

// ---- the supine-flag hook (exe 0x140a68270 CharacterProne::SetSupine(this, u8 on)); may run on a job thread: no log here, a ring drained by pul_sup_tick
typedef void (*fn_supflag)(uint8_t * cp, uint8_t on);
static fn_supflag g_supflagTramp;
static volatile uint64_t g_supw_cp, g_supw_until, g_supw_hold_until;    // our player's CharacterProne (main thread, every tick of the window), watch / hold ends (GetTickCount64)
static volatile int g_supwHoldoff;
static struct {
    uint64_t ra, cp;
    uint8_t on, kept;
} g_supw_ring[32];
static volatile LONG g_supwW;
static LONG g_supwR;
static void SupflagHook(uint8_t *cp, uint8_t on)
{
    uint64_t until = g_supw_until;
    if (until && cp) {
        uint64_t now = GetTickCount64();
        if (now < until && ((uint64_t) cp == g_supw_cp || !g_supw_cp)) {
            uint8_t kept = 0;
            if (!on && g_supw_cp && (uint64_t) cp == g_supw_cp
                && now < g_supw_hold_until && !g_supwHoldoff) {
                on = 1;
                kept = 1;
            }
            LONG k = InterlockedIncrement(&g_supwW) - 1;
            if (k - g_supwR < 32) {
                int i = k & 31;
                g_supw_ring[i].ra = (uint64_t) __builtin_return_address(0);
                g_supw_ring[i].cp = (uint64_t) cp;
                g_supw_ring[i].on = on;
                g_supw_ring[i].kept = kept;
            }
        }
    }
    g_supflagTramp(cp, on);
}

static int g_supflagInst;      // 1 installed, -1 failed
static void PulSupflagInstall(void)
{
    if (g_supflagInst)
        return;
    g_supflagInst = -1;
    uint8_t *f = (uint8_t *) GetModuleHandleW(NULL) + 0xa68270;
    // mov [rsp+10h],rbx; mov [rsp+18h],rsi; push rdi; sub rsp,20h  (15 bytes, no rip-relative operand)
    static const uint8_t expect[15] =
        { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57,
0x48, 0x83, 0xec, 0x20 };
    if (memcmp(f, expect, sizeof expect)) {
        HOOK_MISS(f, sizeof expect,
                  "pullout: exe build differs at 0x140a68270, supine-flag hook not installed");
        return;
    }
    uint8_t *t =
        VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE,
                     PAGE_EXECUTE_READWRITE);
    if (!t)
        return;
    memcpy(t, f, 15);
    t[15] = 0xff;
    t[16] = 0x25;
    *(uint32_t *) (t + 17) = 0;
    *(uint64_t *) (t + 21) = (uint64_t) (f + 15);
    g_supflagTramp = (fn_supflag) t;
    DWORD old;
    VirtualProtect(f, 15, PAGE_EXECUTE_READWRITE, &old);
    f[0] = 0xff;
    f[1] = 0x25;
    *(uint32_t *) (f + 2) = 0;
    *(uint64_t *) (f + 6) = (uint64_t) SupflagHook;
    f[14] = 0x90;
    VirtualProtect(f, 15, old, &old);
    FlushInstructionCache(GetCurrentProcess(), f, 15);
    g_supflagInst = 1;
    PULLOG
        ("supine-flag hook installed (0x140a68270 CharacterProne::SetSupine)");
}

// (the victim was still prone on the stomach): go-supine's GoProne(mode 3) never made the CharacterProne - the engine spawned its own with
// mode 1 (belly) a few frames later and re-created it twice more (Init spawn mode 1 = s_prone first, so the supine flag alone left the body on its
// belly). Hook GoProne (PlayerStealthController slot 0x1f0 = exe 0x140cdcf90, (ctrl, int mode, u8, u8, float blend, u8)): while the escape's
// supine hold runs, a belly GoProne (mode 1) on OUR player's controller becomes mode 3 (supine spawn -> s_prone-supine). Every call in the watch
// window is logged with its caller.
typedef void (*fn_goprone)(uint8_t * ctrl, int mode, uint8_t a, uint8_t b,
                           float blend, uint8_t c);
static fn_goprone g_goproneTramp;
static volatile uint64_t g_gopCtrl;    // our player's PlayerStealthController (main thread, set at go-supine)
static struct {
    uint64_t ra;
    int mode, nmode;
} g_gop_ring[16];
static volatile LONG g_gopW;
static LONG g_gopR;
static void GoproneHook(uint8_t *ctrl, int mode, uint8_t a, uint8_t b,
                         float blend, uint8_t c)
{
    uint64_t until = g_supw_until;
    if (until && ctrl && (uint64_t) ctrl == g_gopCtrl) {
        uint64_t now = GetTickCount64();
        int nm = mode;
        if (now < until) {
            if (mode == 1 && now < g_supw_hold_until && !g_supwHoldoff)
                nm = 3;
            LONG k = InterlockedIncrement(&g_gopW) - 1;
            if (k - g_gopR < 16) {
                int i = k & 15;
                g_gop_ring[i].ra = (uint64_t) __builtin_return_address(0);
                g_gop_ring[i].mode = mode;
                g_gop_ring[i].nmode = nm;
            }
            mode = nm;
        }
    }
    g_goproneTramp(ctrl, mode, a, b, blend, c);
}

static int g_goproneInst;
static void PulGoproneInstall(void)
{
    if (g_goproneInst)
        return;
    g_goproneInst = -1;
    uint8_t *f = (uint8_t *) GetModuleHandleW(NULL) + 0xcdcf90;
    // push rbp/rbx/rsi/rdi/r12/r14/r15; lea rbp,[rsp-10h]  (16 bytes, no rip-relative operand)
    static const uint8_t expect[16] =
        { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57,
0x48, 0x8d, 0x6c, 0x24, 0xf0 };
    if (memcmp(f, expect, sizeof expect)) {
        HOOK_MISS(f, sizeof expect,
                  "pullout: exe build differs at 0x140cdcf90, GoProne hook not installed");
        return;
    }
    uint8_t *t =
        VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE,
                     PAGE_EXECUTE_READWRITE);
    if (!t)
        return;
    memcpy(t, f, 16);
    t[16] = 0xff;
    t[17] = 0x25;
    *(uint32_t *) (t + 18) = 0;
    *(uint64_t *) (t + 22) = (uint64_t) (f + 16);
    g_goproneTramp = (fn_goprone) t;
    DWORD old;
    VirtualProtect(f, 16, PAGE_EXECUTE_READWRITE, &old);
    f[0] = 0xff;
    f[1] = 0x25;
    *(uint32_t *) (f + 2) = 0;
    *(uint64_t *) (f + 6) = (uint64_t) GoproneHook;
    f[14] = 0x90;
    f[15] = 0x90;
    VirtualProtect(f, 16, old, &old);
    FlushInstructionCache(GetCurrentProcess(), f, 16);
    g_goproneInst = 1;
    PULLOG("GoProne hook installed (0x140cdcf90)");
}

static void PulGopDrain(void)
{
    while (g_gopR < g_gopW) {
        int i = g_gopR & 15;
        g_gopR++;
        PULLOG("GoProne on our player: mode %d%s from exe+%llx",
               g_gop_ring[i].mode,
               g_gop_ring[i].nmode !=
               g_gop_ring[i].mode ? " -> 3 (supine hold)" : "",
               (unsigned long long) (g_gop_ring[i].ra -
                                     (uint64_t) GetModuleHandleW(NULL)));
    }
}

// our player's live CharacterProne: Player (exe GetPlayer 0x140dabea0) +0x31f8 = PlayerStealthController, its +0x118 handle {ptr, +0x120 offset,
// +0x124 id} -> the subsystem (id at +0x78; the same walk GoProne 0x140cdcf90 does). NULL = not prone
static uint8_t *PulMyCprone(void)
{
    uint8_t *base = (uint8_t *) GetModuleHandleW(NULL);
    uint8_t *pl = ((uint8_t * (*)(void)) (base + 0xdabea0)) ();
    if (!pl || IsBadReadPtr(pl + 0x31f8, 8))
        return NULL;
    uint8_t *sc = *(uint8_t **) (pl + 0x31f8);
    if (!sc || IsBadReadPtr(sc + 0x118, 16))
        return NULL;
    uint8_t *h = *(uint8_t **) (sc + 0x118);
    uint32_t off = *(uint32_t *) (sc + 0x120), id =
        *(uint32_t *) (sc + 0x124);
    if (!h || !id || IsBadReadPtr(h + 8, 8))
        return NULL;
    uint8_t *b = *(uint8_t **) (h + 8);
    if (!b)
        return NULL;
    uint8_t *cp = b + off;
    if (IsBadReadPtr(cp, 0x1f0) || *(uint32_t *) (cp + 0x78) != id)
        return NULL;
    return cp;
}

static void PulSupwDrain(void)
{
    while (g_supwR < g_supwW) {
        int i = g_supwR & 31;
        g_supwR++;
        uint64_t ra = g_supw_ring[i].ra, rv =
            ra - (uint64_t) GetModuleHandleW(NULL);
        static uint64_t last_ra, last_cp;
        static int last_on = -1;        // (the look rule writes every frame: log changes only)
        if (ra == last_ra && g_supw_ring[i].cp == last_cp
            && g_supw_ring[i].on == last_on && !g_supw_ring[i].kept)
            continue;
        last_ra = ra;
        last_cp = g_supw_ring[i].cp;
        last_on = g_supw_ring[i].on;
        const char *who =
            rv == 0xa5d2f1 ? "the look-direction rule 0x140a5cdd0" : rv ==
            0xa6c81b ? "the idle action's anim-state check 0x140a6c760" :
            rv == 0xa606fb ? "CharacterProne::Init (spawn mode)" : rv ==
            0xa6419a ? "a float test 0x140a64100" : rv == 0xa65405
            || rv == 0xa67734 ? "a supine state entry" : "?";
        PULLOG
            ("supine flag -> %d from exe+%llx (%s)%s, CharacterProne %p (ours %p)",
             g_supw_ring[i].on, (unsigned long long) rv, who,
             g_supw_ring[i].kept ? " - a CLEAR, kept supine (hold)" : "",
             (void *) g_supw_ring[i].cp, (void *) g_supw_cp);
    }
}

static __attribute__((aligned(16)))
float g_pulSupAim[4];
static void PulHandBodyQ(const float *q);
static void PulHandBody(void)
{
    PulHandBodyQ(NULL);
}

static void PulHandBodyQ(const float *q)
{
    if (!g_psPulVdrv)
        return;
    g_psPulVdrv = 0;
    float b[16];
    const float *l = PsyncLoc(SID_PLAYER, b);
    if (l && !PsyncOwnDeadNow()) {
        float t[8];
        memcpy(t, l, sizeof t);
        if (q)
            memcpy(t + 4, q, 16);
        mo_teleport(t, t + 4, MO_SRC("pullout.hand_body"), 0);
        PULLOG
            ("victim body handed to the engine at %.2f %.2f %.2f%s (teleport-player)",
             t[0], t[1], t[2], q ? " facing the feet" : "");
    }
}

// the body's feet direction (neck -> pelvis, horizontal) and how far the clip's locator facing is from it (+ = locator left of the feet); 0 = no joints
static int PulSupFeet(float *fd, float *dev_deg)
{
    float P[3], N[3], b[16];
    const float *l = PsyncLoc(SID_PLAYER, b);
    if (!l || !GxJointPos(SID_PLAYER, "pelvis", P)
        || !GxJointPos(SID_PLAYER, "neck", N))
        return 0;
    float x = P[0] - N[0], z = P[2] - N[2], n =
        __builtin_sqrtf(x * x + z * z);
    if (!(n > 0.15f))
        return 0;               // (sitting up / degenerate: no lying axis)
    fd[0] = x / n;
    fd[1] = 0.0f;
    fd[2] = z / n;
    float qx = l[4], qy = l[5], qz = l[6], qw = l[7], fx =
        2.0f * (qx * qz + qw * qy), fz = 1.0f - 2.0f * (qx * qx + qy * qy);
    float d = fd[0] * fx + fd[2] * fz, c = fd[2] * fx - fd[0] * fz;
    *dev_deg = __builtin_atan2f(c, d) * 57.29578f;
    g_pulSupAim[0] = P[0] + fd[0] * 4.0f;
    g_pulSupAim[1] = P[1] + 0.25f;
    g_pulSupAim[2] = P[2] + fd[2] * 4.0f;
    g_pulSupAim[3] = 1.0f;
    // (the aim went off to the side instead of at the enemy): straight down the feet
    // is not where the attacker stands. Aim at the attacker's chest instead, its bearing clamped to +-60 deg of the feet (inside the engine's
    // +-67 deg supine band, so the stance still holds). No attacker position: down the feet as before. t2r-pullout-supaim-feet.txt = the old aim.
    if (g_pulv.slot >= 0
        && GetFileAttributesA("t2r-pullout-supaim-feet.txt") ==
        INVALID_FILE_ATTRIBUTES) {
        typedef uint8_t *(*fn_peer)(uint32_t);
        uint8_t *peer = ((fn_peer) T2rPeer) ((uint32_t) g_pulv.slot);
        float *ap = peer ? (float *) (peer + 0x68) : NULL;
        if (ap && __builtin_isfinite(ap[0] + ap[1] + ap[2])) {
            float ax = ap[0] - P[0], az = ap[2] - P[2], ad =
                __builtin_sqrtf(ax * ax + az * az);
            if (ad > 0.5f && ad < 15.0f) {
                float rel = __builtin_atan2f(fd[2] * ax - fd[0] * az, fd[0] * ax + fd[2] * az); // attacker bearing off the feet (rad, + = same sense as cy above)
                const float lim = 60.0f * 0.0174532925f;
                float cl = rel > lim ? lim : rel < -lim ? -lim : rel;
                float th0 = __builtin_atan2f(fd[0], fd[2]), dd =
                    ad < 2.0f ? 2.0f : ad;
                float th1 = th0 - cl, th2 = th0 + cl;   // (either rotation sense: keep the one that points closer to the attacker)
                float d1 =
                    __builtin_sinf(th1) * ax + __builtin_cosf(th1) * az,
                    d2 =
                    __builtin_sinf(th2) * ax + __builtin_cosf(th2) * az;
                float th = d1 >= d2 ? th1 : th2;
                g_pulSupAim[0] = P[0] + __builtin_sinf(th) * dd;
                g_pulSupAim[1] = ap[1] + 1.1f;
                g_pulSupAim[2] = P[2] + __builtin_cosf(th) * dd;
                PULLOG
                    ("escape -> supine: camera aim at the attacker (slot %d, %.1f m, %+.0f deg off the feet, aimed %+.0f)",
                     g_pulv.slot, (double) ad, (double) (rel * 57.29578f),
                     (double) (cl * 57.29578f));
            }
        }
    }
    return 1;
}

static void PulGoSupine(uint64_t now, const char *why)
{
    g_pulv.sup = 1;
    g_pulv.sup_ms = now;
    g_pulv.sup_try++;
    float fd[3] = { 0 }, dev = 0;
    int feet = g_psPulVdrv && PulSupFeet(fd, &dev);
    if (feet) {
        float cd[3], cp[3], cy = 0;
        int cam = CamPosDir(cp, cd);
        if (cam) {
            float h = __builtin_sqrtf(cd[0] * cd[0] + cd[2] * cd[2]);
            cy = h >
                1e-3f ? __builtin_atan2f(fd[2] * cd[0] - fd[0] * cd[2],
                                         fd[0] * cd[0] +
                                         fd[2] * cd[2]) * 57.29578f : 0;
        }
        PULLOG
            ("escape -> supine: the body's feet point %.2f %.2f, the clip locator faces %+.0f deg off them, camera %s %+.0f deg off the feet (the engine keeps supine within +-67 deg of the feet)",
             fd[0], fd[2], (double) dev, cam ? "looks" : "unknown,",
             (double) cy);
    }
    if (feet && __builtin_fabsf(dev) > 45.0f) {
        float th = __builtin_atan2f(fd[0], fd[2]), q[4] =
            { 0.0f, __builtin_sinf(th * 0.5f), 0.0f,
__builtin_cosf(th * 0.5f) };
        PulHandBodyQ(q);
    } else
        PulHandBody();
    if (PsyncOwnDeadNow() || LocalHp() <= 0.0f || g_downed
        || g_downDying) {
        PULLOG("escape -> supine skipped: we are down / dying");
        return;
    }
    PulSupflagInstall();
    PulGoproneInstall();
    {
        uint8_t *pl =
            ((uint8_t * (*)(void)) ((uint8_t *) GetModuleHandleW(NULL) +
                                    0xdabea0)) ();
        g_gopCtrl = pl
            && !IsBadReadPtr(pl + 0x31f8,
                             8) ? *(uint64_t *) (pl + 0x31f8) : 0;
    }
    g_supwHoldoff = PulSupholdOff();
    g_supw_cp = 0;
    g_supw_hold_until = now + PUL_SUP_HOLD_MS;
    g_supw_until = now + PUL_SUP_WATCH_MS;
    if (!feet && g_pulv.sup_try == 1)
        g_pulSupAim[3] = 0.0f;        // (no joints: no aim point; a retry keeps the first one)
    uint64_t ev[2] = { FNV64("go-supine"), SID_PLAYER };
    uint64_t r = DcCall(0x4277eb0d5f4c59f2ULL /* send-event */ , 2, ev);
    // (supine trace): send-event go-supine answers 0 and its handler (0x140c6a5e0) never runs in MP - no GoProne call at all; the engine
    // then spawned its OWN prone (mode 1 = the stand -> belly dive, the body turned 180 to lie head-first along the locator, ~1.7 s before our
    // flag finally rolled it over). Do what that handler does: PlayerStealthController (player
    // +0x31f8) vtable slot 0x1f0 GoProne(mode 3 = supine spawn -> s_prone-supine at once, 1, 0, blend -1 = the handler's default, 0).
    if (!PulMyCprone()
        && GetFileAttributesA("t2r-pullout-nogoprone.txt") ==
        INVALID_FILE_ATTRIBUTES) {
        uint8_t *pl =
            ((uint8_t * (*)(void)) ((uint8_t *) GetModuleHandleW(NULL) +
                                    0xdabea0)) ();
        uint8_t *ctrl = pl
            && !IsBadReadPtr(pl + 0x31f8,
                             8) ? *(uint8_t **) (pl + 0x31f8) : NULL;
        if (ctrl && !IsBadReadPtr(ctrl, 8)
            && !IsBadReadPtr(*(uint8_t **) ctrl + 0x1f0, 8)) {
            fn_goprone gp = *(fn_goprone *) (*(uint8_t **) ctrl + 0x1f0);
            gp(ctrl, 3, 1, 0, -1.0f, 0);
            uint8_t *c2 = PulMyCprone();
            PULLOG
                ("escape -> supine: GoProne(mode 3) called directly on our controller %p (vfunc exe+%llx) -> CharacterProne %p supine byte %d mode %d",
                 (void *) ctrl,
                 (unsigned long long) ((uint64_t) gp -
                                       (uint64_t) GetModuleHandleW(NULL)),
                 (void *) c2, c2 ? c2[0x1e1] : -1,
                 c2 ? *(int *) (c2 + 0x1e8) : -1);
        }
    }
    uint8_t *cp = PulMyCprone();
    if (cp)
        g_supw_cp = (uint64_t) cp;
    PULLOG
        ("escape -> supine (%s, try %d): send-event go-supine to the player -> %llx; CharacterProne %p supine byte %d mode %d, hold %s, camera aim %s",
         why, g_pulv.sup_try, (unsigned long long) r, (void *) cp,
         cp ? cp[0x1e1] : -1, cp ? *(int *) (cp + 0x1e8) : -1,
         g_supwHoldoff ? "OFF (t2r-pullout-nosuphold.txt)" :
         g_supflagInst > 0 ? "on" : "unavailable",
         g_pulSupAim[3] == 1.0f ? "down the feet" : "none (no joints)");
}

// every pul_tick (also after the victim side ended): the camera aim, our CharacterProne for the hook, the flag log
static void PulSupTick(uint64_t now)
{
    // t2r-supine-trace.txt = watch our player's prone / supine all the time
    // (no forcing): every SetSupine write and GoProne call with its caller, every CharacterProne change, its supine byte / mode, and the body probe.
    {
        static int tv = -1;
        static uint64_t tat;
        static uint64_t tcp;
        static int tsup = -2, tmode = -2;
        if (PulFlag("t2r-supine-trace.txt", &tv, &tat)) {
            PulSupflagInstall();
            PulGoproneInstall();
            uint8_t *pl =
                ((uint8_t *
                  (*)(void)) ((uint8_t *) GetModuleHandleW(NULL) +
                              0xdabea0)) ();
            g_gopCtrl = pl
                && !IsBadReadPtr(pl + 0x31f8,
                                 8) ? *(uint64_t *) (pl + 0x31f8) : 0;
            uint8_t *cp = PulMyCprone();
            if (!g_pulv.sup || now - g_pulv.sup_ms > PUL_SUP_WATCH_MS) {
                g_supw_until = now + 1000;
                g_supw_hold_until = 0;
                g_supw_cp = (uint64_t) cp;
            }
            int sb = cp ? cp[0x1e1] : -1, md =
                cp ? *(int *) (cp + 0x1e8) : -1;
            if ((uint64_t) cp != tcp || sb != tsup || md != tmode) {
                int back = cp ? SupineBodyTest("supine trace") : -1;
                float cd[3], cpp[3], b16[16];
                int cam = CamPosDir(cpp, cd);
                const float *l = PsyncLoc(SID_PLAYER, b16);
                float rel = 999.0f;
                if (cam && l) {
                    float qx = l[4], qy = l[5], qz = l[6], qw = l[7], fx =
                        2.0f * (qx * qz + qw * qy), fz =
                        1.0f - 2.0f * (qx * qx + qy * qy);
                    rel =
                        XvWrap((__builtin_atan2f(cd[0], cd[2]) -
                                 __builtin_atan2f(fx, fz)) * 57.29578f);
                }
                PULLOG
                    ("supine trace: CharacterProne %p supine byte %d mode %d (+0x1e2 %d), body on its back %d, camera yaw %+.0f deg off the locator facing",
                     (void *) cp, sb, md, cp ? cp[0x1e2] : -1, back,
                     (double) rel);
                tcp = (uint64_t) cp;
                tsup = sb;
                tmode = md;
            }
        }
    }
    if (!g_supw_until)
        return;
    PulSupwDrain();
    PulGopDrain();
    if (now >= g_supw_until) {
        g_supw_until = 0;
        g_supw_cp = 0;
        PULLOG("supine watch over");
        return;
    }
    uint8_t *cp = PulMyCprone();
    if ((uint64_t) cp != g_supw_cp) {
        PULLOG("supine: our CharacterProne %p -> %p (supine byte %d)",
               (void *) g_supw_cp, (void *) cp, cp ? cp[0x1e1] : -1);
        // (the victim went just prone instead of prone on their back): go-supine spawned a NEW CharacterProne whose Init wrote supine 0
        // (spawn mode) before we knew its address, and the engine re-created it twice more - the hook's hold only covered a known one. A new one
        // during the hold window restarts the hold.
        if (cp && g_supw_cp && now < g_supw_hold_until)
            g_supw_hold_until = now + PUL_SUP_HOLD_MS;
        g_supw_cp = (uint64_t) cp;
    }
    // ...and the flag is SET through the engine's own SetSupine (0x140a68270: flag + the "supine" overlay set) on our CharacterProne every tick of the hold
    // while it reads 0. t2r-pullout-nosuphold.txt = off.
    // trace (a player's own roll onto the back): the flip IS SetSupine(1) - it writes the flag AND switches the anim overlay set to
    // "supine" (0x140a68270: [anim ctrl +0xdd] + the overlay remap); the look rule calls nothing else. Each re-created CharacterProne's Init
    // switched the overlay back to prone while the copied flag still read 1 - so call it for every new CharacterProne and every 100 ms of the hold.
    static uint64_t sup_last_cp, sup_last_ms;
    if (cp && now < g_supw_hold_until && !g_supwHoldoff && g_supflagTramp
        && (!cp[0x1e1] || (uint64_t) cp != sup_last_cp
            || now - sup_last_ms >= 100)) {
        sup_last_cp = (uint64_t) cp;
        sup_last_ms = now;
        g_supflagTramp(cp, 1);
        static int nf;
        if (LOGCAP(nf, 8))
            PULLOG
                ("supine: our CharacterProne %p read belly - SetSupine(1) -> byte %d",
                 (void *) cp, cp[0x1e1]);
    }
    // After the escape the aim landed on the truck - force-camera-aim-point (always answers 0) and the
    // stick never turned MP's live camera. camera-aim-override-direction (0x140409ab0: two angles in degrees -> quaternion -> the player camera's
    // override slot, mode 2) sets the heading directly. Its argument order / yaw sense is not known yet: the first 4 x 5 frames try
    // (yaw,pitch) (pitch,yaw) (-yaw,pitch) (pitch,-yaw) and keep the one that turns the live camera toward the attacker; it ends with
    // camera-cancel-aim-override when the player aims or after PUL_SUP_OVR_MS. t2r-pullout-aimovr-off.txt = off.
    {
        static int ovr_on, var = 2, vtry, vn;
        static float e0;
        static uint64_t t0, last_sup;   // (A/B: variant 2 = (-yaw, pitch) turned it 23 -> 4 deg in 5 frames; (pitch, yaw) flipped the view)
        if (g_pulv.sup && g_pulv.sup_ms != last_sup) {
            last_sup = g_pulv.sup_ms;
            ovr_on = 0;
            vn = 0;
            t0 = now;
            if (var < 0) {
                vtry = 0;
            }
        }
        int want = g_pulv.sup && now - g_pulv.sup_ms < 900
            && g_pulSupAim[3] == 1.0f && !g_pul_oh_user
            && GetFileAttributesA("t2r-pullout-aimovr-off.txt") ==
            INVALID_FILE_ATTRIBUTES;
        if (want) {
            uint64_t nn = 0, am =
                DcCall(FNV64("player-is-aiming?"), 0, &nn);
            if (now - g_pulv.sup_ms > 150 && am != ~0ULL && (am & 0xff))
                want = 0;       // (the player's own aim: hand the camera over)
        }
        float P[3];
        uint64_t none = 0;
        if (want && GxJointPos(SID_PLAYER, "headb", P)) {
            float dx = g_pulSupAim[0] - P[0], dy =
                g_pulSupAim[1] - P[1], dz = g_pulSupAim[2] - P[2], dh =
                __builtin_sqrtf(dx * dx + dz * dz);
            float yaw = __builtin_atan2f(dx, dz) * 57.29578f, pit =
                __builtin_atan2f(dy, dh > 1e-3f ? dh : 1e-3f) * 57.29578f;
            uint64_t rd =
                DcCall(FNV64("camera-get-dir-through-reticle"), 0, &none);
            float c[3] = { 0, 0, 1 };
            if (rd && rd != ~0ULL && !IsBadReadPtr((void *) rd, 12))
                memcpy(c, (void *) rd, 12);
            float err =
                __builtin_fabsf(XvWrap
                                (__builtin_atan2f(c[0], c[2]) * 57.29578f -
                                 yaw));
            int v = var >= 0 ? var : vtry;
            if (var < 0) {      // calibration: 5 frames per variant
                if (vn == 0)
                    e0 = err;
                if (++vn > 5) {
                    PULLOG
                        ("supine aim override: variant %d yaw error %.0f -> %.0f deg",
                         vtry, (double) e0, (double) err);
                    if (e0 - err > 8.0f || err < 6.0f) {
                        var = vtry;
                        PULLOG
                            ("supine aim override: variant %d WORKS - kept",
                             var);
                    } else if (++vtry > 3) {
                        var = 99;
                        PULLOG
                            ("supine aim override: no variant turned the live camera - off");
                    }
                    vn = 0;
                    v = var >= 0 && var <= 3 ? var : vtry;
                }
            }
            if (v >= 0 && v <= 3) {
                float a =
                    v == 0 ? yaw : v == 1 ? pit : v == 2 ? -yaw : pit, b =
                    v == 0 ? pit : v == 1 ? yaw : v == 2 ? pit : -yaw;
                uint64_t ar[2] = { 0, 0 };
                memcpy(&ar[0], &a, 4);
                memcpy(&ar[1], &b, 4);
                DcCall(MoCamSid
                        ("camera-aim-override-direction",
                         FNV64("camera-aim-override-direction")), 2, ar);
                if (!ovr_on) {
                    ovr_on = 1;
                    PULLOG
                        ("supine aim override: ON toward the attacker (yaw %.0f pitch %.0f, live camera %.0f deg off, variant %d)",
                         (double) yaw, (double) pit, (double) err, v);
                }
            }
        } else if (ovr_on) {
            DcCall(MoCamSid
                    ("camera-cancel-aim-override",
                     FNV64("camera-cancel-aim-override")), 0, &none);
            ovr_on = 0;
            PULLOG
                ("supine aim override: released (%llu ms after the hand-off)",
                 (unsigned long long) (now - g_pulv.sup_ms));
        }
        g_pulOvrOn = ovr_on;
    }
    if (g_pulv.sup && now - g_pulv.sup_ms < PUL_SUP_AIM_MS && g_pulSupAim[3] == 1.0f) {       // the player's own aim ends it at once (it fought the player's aim)
        uint64_t none = 0, am =
            DcCall(FNV64("player-is-aiming?"), 0, &none);
        if (g_pul_oh_user || (am != ~0ULL && (am & 0xff))) {
            g_pulSupAim[3] = 0.0f;
            PULLOG
                ("supine: the player aims - camera aim point dropped (%llu ms in)",
                 (unsigned long long) (now - g_pulv.sup_ms));
        }
    }
    if (g_pulv.sup && now - g_pulv.sup_ms < PUL_SUP_AIM_MS && g_pulSupAim[3] == 1.0f && !g_pul_oh_on) {       // (not while the escape camera turns the live camera with the stick)
        uint64_t a = (uint64_t) g_pulSupAim, r =
            DcCall(MoCamSid
                    ("force-camera-aim-point",
                     FNV64("force-camera-aim-point")), 1, &a);
        static int nl;
        if (LOGCAP(nl, 6))
            PULLOG("supine: force-camera-aim-point %.2f %.2f %.2f -> %llx",
                   g_pulSupAim[0], g_pulSupAim[1], g_pulSupAim[2],
                   (unsigned long long) r);
    }
}

static void PulVicTick(uint64_t now)
{
    if (!g_pulv.on)
        return;
    int mine = g_ps.role == 2 && g_ps.uid == g_pulv.uid;
    int pairnow = g_ps.role == 2 && g_ps.kind == PS_CTXDOWN
        && CdPairOfMove(g_ps.move) >= 0
        && CD_PAIRS[CdPairOfMove(g_ps.move)].kind == 8;
    if (pairnow)
        g_pulv.last_pair = now; // (the outcome pair replaces the drag's uid)
    if (g_pulv.out == 1 && g_pulv.ok && !g_pulv.sup && !pairnow
        && now - g_pulv.out_ms > 200 && !PulSupineOff())
        PulGoSupine(now, "the success pair let go of our body");
    if (g_pulv.sup == 1 && now - g_pulv.sup_ms > 400) { // check 0.4 s on: player-is-supine? = our CharacterProne's supine byte (0x140db5170) + the chest-normal probe; one retry
        g_pulv.sup = 2;
        uint64_t none = 0, pr =
            DcCall(FNV64("player-is-prone?"), 0, &none), su =
            DcCall(FNV64("player-is-supine?"), 0, &none);
        int isup = su != ~0ULL
            && (su & 0xff), back = SupineBodyTest("pull-out escape");
        PULLOG
            ("escape -> supine check 0.4 s on: prone? %d supine? %d, body on its back %d -> %s",
             (int) (pr != ~0ULL && (pr & 0xff)), isup, back, isup
             && back ? "SUPINE (engine state + body)" : isup ?
             "engine supine, body not on its back yet" : "NOT supine");
        // retry: go-supine again - with a CharacterProne already on top it is only the base layer's "to-supine" transition (the prone -> back flip)
        if (!isup && g_pulv.sup_try < 2 && g_pulv.on) {
            g_pulv.sup = 0;
            PulGoSupine(now, "retry: not supine");
            g_pulv.sup = 2;
        }
    }
    if (!g_pulv.out && !mine && now - g_pulv.last_pair > 700) {
        PulVicEnd("the drag pair ended with no outcome");
        return;
    }
    if (g_pulv.out && now - g_pulv.last_pair > 500) {
        PulVicEnd(g_pulv.out ==
                    1 ? "success pair over" : "fail pair over");
        return;
    }
    if (g_pulv.out == 1 && g_pulv.hit_at && now >= g_pulv.hit_at) {     // the counter blow on the attacker: 2 x the fists' table damage through the normal hit path
        uint8_t *peer = MdgPeer((uint32_t) g_pulv.slot);
        g_pulv.hit_at = 0;
        if (peer && PulSlotOk(g_pulv.slot)) {
            float dmg = 2.0f * MeleeDmgW(FNV64("abby-unarmed"));
            g_mdu.synthing = 1;
            g_mdmgAuthored = 1;
            DirecthitHook(peer, FNV64("abby-unarmed"), 0, dmg, 0);
            g_mdu.synthing = 0;
            g_mdmgAuthored = 0;
            EvHitNote((uint32_t) g_pulv.slot);
            PULLOG("COUNTER blow: %.0f damage to the attacker (slot %d)",
                   (double) dmg, g_pulv.slot);
        }
    }
    if (!mine || g_pulv.out)
        return;
    if (g_pulv.grap == 1) {     // the drag of a grapple flow - no window yet; a press is Too early (anti-mash), the grey wait prompt shows
        uint64_t h1 = PuppetHandle((uint32_t) g_pulv.slot);
        if (MpnEEdge() && !g_pulv.burnt) {
            g_pulv.burnt = 1;
            int sr = MpnUiSound(1);
            PULLOG
                ("COUNTER press during the drag (before the grapple): TOO EARLY - this counter fails (FAILED sound: %s)",
                 MpnSndWord(sr));
        }
        if (h1 && g_pulv.burnt)
            WorldPromptGrey(h1, "Too early", 0);      // no E prompt through the drag - only the window shows one
        return;
    }
    // the drag (or the grapple): the window on our pair move clock (the START arrived one-way late: the latency is already inside it)
    double t = PsyncMs() - g_ps.t0, open = g_pulv.end_s * 1000.0 - PUL_WIN_LEAD_MS, close = (g_pulv.shown_t > open ? g_pulv.shown_t : open) + SkIronWinMs(PUL_WIN_MS) + PUL_GRACE_MS;       // Iron Grip: x1.5
    if (close > g_pulv.end_s * 1000.0 + PUL_RES_WAIT_MS - 80)
        close = g_pulv.end_s * 1000.0 + PUL_RES_WAIT_MS - 80;   // (a late-shown window still lands before the attacker's verdict wait runs out)
    int e = MpnEEdge();
    if (!g_pulv.decided && t >= open && SkIronFree("pull-out")) {
        PULLOG
            ("IRON GRIP: the counter is passed for us as the window opens");
        PulVicVerdict(1, 0);
        return;
    }                           // Iron Grip II: once per life
    if (e && !g_pulv.decided) {
        if (t < open) {         // anti-mash (the pin QTE rule): a press before the window fails this counter
            if (!g_pulv.burnt) {
                g_pulv.burnt = 1;
                int sr = MpnUiSound(1);
                PULLOG
                    ("COUNTER press %.0f ms before the window: TOO EARLY - this counter fails (FAILED sound: %s)",
                     open - t, MpnSndWord(sr));
            }
        } else if (t <= close && !g_pulv.burnt) {
            PulVicVerdict(1, (int) (t - open));
            return;
        }
    }
    uint64_t h = PuppetHandle((uint32_t) g_pulv.slot);
    if (!g_pulv.decided && t > close) {
        g_pulv.late_until = now + PUL_LATE_MS;
        PulVicVerdict(0, (int) (t - open));
    } else if (!g_pulv.decided && t < open && h && !g_pulv.burnt
               && !g_pulv.grap) {
        if (PaOld())
            WorldPromptGrey(h, "...wait", 1);
        else {
            double w0 = 0.0;
            WorldPromptGreyP(h, "", 1,
                                open >
                                w0 ? (float) ((t - w0) /
                                              (open - w0)) : 1.0f);
    }}                          // a grey E "...wait" through the drag, so the window's start is readable
    else if (!g_pulv.decided && t < open && h && g_pulv.burnt)
        WorldPromptGrey(h, "Too early", 0);
    else if (!g_pulv.decided && t >= open && h) {       // the prompt over the attacker for the window only
        if (g_pulv.burnt)
            WorldPromptGrey(h, "Too early", 0);
        else {
            int shown = WorldPrompt(h, "ESCAPE!", -1.0f);
            if (g_pulv.shown != 1) {
                g_pulv.shown = 1;
                g_pulv.shown_t = t;
                int sr = MpnUiSound(0);
                PULLOG("COUNTER window OPEN (%d ms): prompt %s, cue %s",
                       SkIronWinMs(PUL_WIN_MS),
                       shown ? "SHOWN" : "refused - text fallback",
                       MpnSndWord(sr));
            }
            if (!shown) {
                static uint64_t pf;
                if (g_t60 - pf >= 30) {
                    pf = g_t60;
                    T2R_PROMPT_MSG("PRESS E NOW TO ESCAPE", 0.3f, 0, 0);
                }
            }
        }
    } else if (g_pulv.decided && !g_pulv.ok && now < g_pulv.late_until
               && h)
        WorldPromptGrey(h, g_pulv.burnt ? "Too early" : "Too late", 0);       // grey, no icon
}

// ---- network (MSG_MEX op 5, sub-ops 11+; the dispatcher in t2r-ext.c)
static void PulRecv(uint32_t slot, const uint8_t *buf, uint64_t len)
{
    if (slot >= T2R_MAX_PEERS || !buf || len < 4 || PulOff())
        return;
    int op = buf[1];
    uint64_t now = GetTickCount64();
    if (op == PUL_OP_INTRO && len >= 5) {       // the attacker's tell: the SP intro clip on its puppet (the victim is still free)
        int set = buf[4] & 1, dir = (buf[4] >> 1) & 3;
        uint64_t ent = PuppetHandle(slot);
        g_pul_igrap.slot = (buf[4] & 8) ? (int) slot : -1;
        g_pul_igrap.ms = now;
        g_pul_igrap.pounce = (buf[4] & 16) != 0;        // bit 3 = a grapple follows the drag (the window waits for it)
        int pvar = (buf[4] >> 5) & 3;   // ps45: bits 5-6 = the variant (a: no intro clip - the prone-kill pair follows at once)
        if (pvar == PULV_A)
            PULLOG
                ("INTRO from slot %u: variant a - no intro clip, the prone-kill pair follows",
                 slot);
        else if (pvar)
            PULLOG("INTRO from slot %u: variant %s", slot, PULV_TAG[pvar]);
        if ((buf[4] & 16) || pvar == PULV_A || !ent || g_peerDowned[slot]
            || g_puppetDeadHere[slot])
            return;             // (bit 4 = pounce: no intro clip, the grapple pair follows at once)
        float age =
            g_psRtt[slot] > 0.0f ? g_psRtt[slot] * 0.5f : 40.0f, dur =
            AnimLen(SID_PLAYER, PulIntroClip(set, dir)), ph =
            dur > 0.2f ? age / 1000.0f / dur : 0.0f;
        if (ph > 0.5f)
            ph = 0.5f;
        if (!(dur > 0.2f)) {
            PULLOG
                ("INTRO from slot %u: clip %s not resident here - the puppet just stands there until the drag",
                 slot, PulIntroClip(set, dir));
            return;
        }
        g_mdg.sw[slot].ms = 0;
        g_mdg.sw[slot].open_ms = 0;
        if (g_mdg.window_open)
            MdgPromptWithdraw("pullout intro");       // (the press's cancelled swing had opened a Dodge prompt over the tell)
        uint64_t r =
            MhrAnimate(ent, Fnv64Rt(PulIntroClip(set, dir)), ph,
                        0.12f);
        g_mhr.pd[slot].ent = ent;
        g_mhr.pd[slot].sid = Fnv64Rt(PulIntroClip(set, dir));
        g_mhr.pd[slot].end = now + (uint64_t) (dur * 1000.0f) + 200;
        PULLOG
            ("INTRO from slot %u: %s on its puppet (phase %.2f) -> %llx - the tell; we are still free for ~%.1f s",
             slot, PulIntroClip(set, dir), (double) ph,
             (unsigned long long) r, (double) dur);
    } else if (op == PUL_OP_RES && len >= 5) {  // the victim's verdict: 1 = valid counter
        if (g_pul.st == PUL_IDLE || g_pul.slot != (int) slot)
            return;
        if (g_pul.grap
            && (g_pul.st == PUL_INTRO || g_pul.st == PUL_DRAG
                || g_pul.st == PUL_KILL)) {
            PULLOG
                ("verdict from slot %u during the DRAG ignored (grapple flow: the window is on the grapple - an older client?)",
                 slot);
            return;
        }
        g_pul.verdict = buf[4] ? 1 : 2;
        PULLOG
            ("verdict from slot %u: %s (its press %d ms into the window), %lld ms after the drag started",
             slot, buf[4] ? "COUNTER" : "no counter",
             len >= 7 ? (int) (buf[5] | (buf[6] << 8)) : -1,
             (long long) (now - g_pul.drag_t0));
    } else if (op == PUL_OP_CAM && len >= 5) {  // the attacker's SP camera choice for this pull-out
        if (!g_pulv.on || g_pulv.slot != (int) slot)
            return;
        g_pulc.peer_cam = buf[4];
        PULLOG("pullout cam: the attacker chose %s",
               buf[4] ? "an SP camera" : "the pin camera");
        if (!buf[4] && g_pulc.on && g_xcamOn && !g_xcamPulproc
            && !PulProcOff()
            && !strncmp(g_xcamName, "ctxdown:pu-", 11)) {
            PulProcBegin(2, (int) slot,
                           "the attacker's SP shot is blocked");
            return;
        }
        if (!buf[4] && g_pulc.on && g_xcamOn && !g_xcamPulproc
            && !strncmp(g_xcamName, "ctxdown:pu-", 11)) {
            g_pulc.on = 0;
            g_pulc.dropped = 1;
            g_pulc.judged = 1;
            XcamStop("pullout: the attacker's camera is blocked");
        }
    } else if (op == PUL_OP_END) {
        if (g_pul.st != PUL_IDLE && g_pul.slot == (int) slot)
            PulAttEnd("the partner ended it", 0);
        else if (g_pulv.on && g_pulv.slot == (int) slot && !g_pulv.out)
            PulVicEnd("the attacker ended it");
    }
}

static void PulRiseTick(void)
{
    uint64_t none = 0;
    // (pulfix30: the victim went straight to standing, then crouched): forced only from the blend-out, the crouch latched after the 0.25 s
    // blend had already reached standing. Now the stance is crouched under the whole fail clip (the clip owns the pose, the stance change is unseen),
    // so the blend-out lands in the crouch; PUL_RISE_MS after the pair the engine stands up from it.
    if (g_ps.role == 1 && g_ps.kind == PS_CTXDOWN
        && PulFailMove(g_ps.move) && !g_downed && LocalHp() > 0.0f
        && GetFileAttributesA("t2r-pullout-rise-off.txt") ==
        INVALID_FILE_ATTRIBUTES) {
        if (g_pulRiseUid != g_ps.uid) {
            g_pulRiseUid = g_ps.uid;
            Xlog("pullout: fail pair on us (attacker) - crouch held under the clip, stands %d ms after it", PUL_RISE_MS);
        }
        DcCall(FNV64("player-force-crouch"), 0, &none);
        g_pulRiseUntil = GetTickCount64() + PUL_RISE_MS;
        return;
    }
    // (the winner popped standing -> crouch -> standing): the crouch was only held from the FAIL pair on, but the sequence before it (drag -> grapple -> the
    // verdict wait) ends every pair with its own 0.25 s blend-out onto STANDING locomotion - the attacker stood up between the pairs (the standing), then the fail pair
    // crouched it (the crouch), then it stood again. The crouch is now held from the drag / pounce on through the grapple and the verdict wait, so every blend-out lands
    // in the pose the next clip starts from and the fail pair's rise is the only stand. A countered pull-out (the victim escapes): the stand goes out once as the success
    // pair starts (hidden under its clip), so ITS blend-out lands standing. t2r-pullout-rise-off.txt = off (also this).
    {
        static int hold;
        int on = (g_pul.st == PUL_GRAP || g_pul.st == PUL_WAITRES
                  || g_pul.st == PUL_KILL || (g_pul.st == PUL_DRAG
                                              && g_pul.grap)) && !g_downed
            && LocalHp() > 0.0f
            && GetFileAttributesA("t2r-pullout-rise-off.txt") ==
            INVALID_FILE_ATTRIBUTES;
        if (on) {
            if (!hold) {
                hold = 1;
                Xlog("pullout: attacker crouch held through the sequence (every pair's blend-out lands in it)");
            }
            DcCall(FNV64("player-force-crouch"), 0, &none);
            return;
        }
        if (hold) {
            hold = 0;
            if (g_pul.st == PUL_OUT && g_pul.verdict != 1) {    /* the fail pair holds it from here (first branch) */
            } else if (!g_pulRiseUntil) {
                if (!g_downed && LocalHp() > 0.0f)
                    DcCall(FNV64("player-force-stand"), 0, &none);
                Xlog("pullout: attacker crouch hold released (%s) - player-force-stand", g_pul.st == PUL_OUT ? "countered: the success pair starts standing" : "the sequence ended");
            }
        }
    }
    if (!g_pulRiseUntil)
        return;
    if (GetTickCount64() < g_pulRiseUntil && !g_downed
        && LocalHp() > 0.0f && !(g_ps.role
                                  && g_ps.uid != g_pulRiseUid)) {
        DcCall(FNV64("player-force-crouch"), 0, &none);
        return;
    }                           // (a new paired move owns the body)
    if (!g_downed && LocalHp() > 0.0f)
        DcCall(FNV64("player-force-stand"), 0, &none);
    Xlog("pullout: attacker rises from the crouch (player-force-stand)");
    g_pulRiseUntil = 0;
}

static void PulTick(void)
{
    PullstingTick();
    PulRiseTick();            // the attacker's crouch -> stand after a fail blow (ctxdown.c sets it at the blend-out)      // victim music: tier hold while the drag runs (pullout_sting.c)
    if (PulOff()) {
        if (g_pul.st != PUL_IDLE)
            PulAttEnd("switched off", 1);
        if (g_pulv.on)
            PulVicEnd("switched off");
        return;
    }
    uint64_t now = GetTickCount64();
    PulCoverTick(now);
    PulAttTick(now);
    PulSupTick(now);          // the supine hand-off's camera aim / flag watch (runs on after the victim side ended)
    PulVicTick(now);
    PulCamJudge();
    // (the player had no control of the camera for a while and it bent weirdly): the escape camera waited 1.5 s for the
    // live camera, which never turned (force-camera-aim-point does not move it: still -148 deg off, looking over the head at the truck), then
    // blended out through that whole 148 deg. Under the held escape camera (weight 1, not seen) the live camera is turned with the right stick
    // (force-camera-stick) until it faces our view; the sign is learnt from the first frames (the stick's yaw sense is not assumed).
    {
        static float sgy = 1.0f, sgp = 1.0f, d0, p0;
        static int n, was;
        static uint64_t ts;
        if (g_pul_oh_want
            && GetFileAttributesA("t2r-pullout-outhold-off.txt") ==
            INVALID_FILE_ATTRIBUTES) {
            uint64_t none = 0, rd =
                DcCall(FNV64("camera-get-dir-through-reticle"), 0, &none);
            float c[3] = { 0, 0, 0 }, L[3] =
                { g_xcam_look[0], g_xcam_look[1], g_xcam_look[2] };
            if (rd && rd != ~0ULL && !IsBadReadPtr((void *) rd, 12))
                memcpy(c, (void *) rd, 12);
            float ch = __builtin_sqrtf(c[0] * c[0] + c[2] * c[2]), lh =
                __builtin_sqrtf(L[0] * L[0] + L[2] * L[2]);
            if (ch > 0.05f && lh > 0.05f) {
                g_pul_oh_d =
                    XvWrap((__builtin_atan2f(c[0], c[2]) -
                             __builtin_atan2f(L[0], L[2])) * 57.29578f);
                g_pul_oh_dp =
                    (__builtin_atan2f(c[1], ch) -
                     __builtin_atan2f(L[1], lh)) * 57.29578f;
                g_pul_oh_ok = 1;
            }
            uint64_t am = DcCall(FNV64("player-is-aiming?"), 0, &none);
            if (!g_pul_oh_user && am != ~0ULL && (am & 0xff)) {
                g_pul_oh_user = 1;
                PULLOG
                    ("escape camera: the player aims - the escape camera lets go now (live camera %+.0f yaw %+.0f pitch off)",
                     (double) g_pul_oh_d, (double) g_pul_oh_dp);
            }
        }
        if (g_pul_oh_on && !g_pulOvrOn && !g_pul_oh_user
            && GetFileAttributesA("t2r-pullout-outhold-off.txt") ==
            INVALID_FILE_ATTRIBUTES) {
            float d = g_pul_oh_d, dp = g_pul_oh_dp;
            if (!was) {
                was = 1;
                n = 0;
                d0 = d;
                p0 = dp;
                ts = now;
                sgy = sgp = 1.0f;
                PULLOG
                    ("escape camera: live camera %+.0f deg yaw, %+.0f deg pitch off our view - turning it with the stick",
                     (double) d, (double) dp);
            }
            if (++n == 8) {     // after 8 frames of stick: each gap must have shrunk, else that axis' sense is the other way
                if (__builtin_fabsf(d0) > 10.0f
                    && __builtin_fabsf(d) > __builtin_fabsf(d0) - 1.0f)
                    sgy = -sgy;
                if (__builtin_fabsf(p0) > 10.0f
                    && __builtin_fabsf(dp) > __builtin_fabsf(p0) - 1.0f)
                    sgp = -sgp;
                PULLOG
                    ("escape camera: after 8 frames yaw %+.0f -> %+.0f, pitch %+.0f -> %+.0f (yaw sense %+.0f, pitch sense %+.0f)",
                     (double) d0, (double) d, (double) p0, (double) dp,
                     (double) sgy, (double) sgp);
            }
            float x =
                __builtin_fabsf(d) < 3.0f ? 0.0f : sgy * (d >
                                                          0.0f ? 1.0f :
                                                          -1.0f) *
                (__builtin_fabsf(d) >
                 45.0f ? 1.0f : __builtin_fabsf(d) / 45.0f);
            float y =
                __builtin_fabsf(dp) < 3.0f ? 0.0f : sgp * (dp >
                                                           0.0f ? 1.0f :
                                                           -1.0f) *
                (__builtin_fabsf(dp) >
                 30.0f ? 1.0f : __builtin_fabsf(dp) / 30.0f);
            uint64_t a[2] = { 0, 0 };
            memcpy(&a[0], &x, 4);
            memcpy(&a[1], &y, 4);
            DcCall(MoCamSid
                    ("force-camera-stick", FNV64("force-camera-stick")), 2,
                    a);
        } else if (was) {
            was = 0;
            PULLOG
                ("escape camera: stick turn over after %llu ms (live camera %+.0f yaw %+.0f pitch off)",
                 (unsigned long long) (now - ts), (double) g_pul_oh_d,
                 (double) g_pul_oh_dp);
        }
    }
    // (the death animation (getting punched) in the pull-out when failing still had Ellie sounds): the pull-out clips carry
    // `voice-effect` anim events with SP player lines; a male body resolves them to Ellie. While a pull-out runs (and 4 s after), the body of a
    // male victim (ours, or the puppet we pull) drops its clip voice events - pullout_sting.c says the male lines instead. clipfx_fe_hook asks.
    {
        uint8_t *mo = NULL, *po = NULL;
        // (Ellie's grunts were still audible in the QTE fail, as the victim): the ATTACKER half (abby-/ellie-pullout ... --abby / --ellie,
        // SP player clips) carries Ellie's / Abby's efforts too - both bodies of the pair drop them on every screen: ours (victim or attacker)
        // and the other half's puppet (the victim we pull, or the attacker pulling us).
        int att = g_pul.st >= PUL_INTRO && g_pul.slot >= 0;
        if (g_pulv.on || att || g_pul_vox_me_until > now)
            mo = BpMe();
        if (g_pulv.on || att)
            g_pul_vox_me_until = now + 4000;
        int oslot = att ? g_pul.slot : g_pulv.on ? g_pulv.slot : -1;
        if (oslot >= 0) {
            typedef uint8_t *(*fn_peer)(uint32_t);
            uint8_t *peer = ((fn_peer) T2rPeer) ((uint32_t) oslot);
            uint64_t h = peer ? *(uint64_t *) (peer + 0x60) : 0;
            if (h) {
                g_pulVoxPoKeep =
                    (uint8_t *) ((void *(*)(uint64_t))
                                 T2R_SYM(kFnEntityFromHandle))(h);
                g_pul_vox_po_until = now + 4000;
            }
        }
        if (g_pul_vox_po_until > now)
            po = g_pulVoxPoKeep;
        if (GetFileAttributesA("t2r-pullout-clipvox-on.txt") !=
            INVALID_FILE_ATTRIBUTES)
            mo = po = NULL;
        g_pul_vox_me = mo;
        g_pul_vox_po = po;
    }
}
