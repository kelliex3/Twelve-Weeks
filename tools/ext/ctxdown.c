// ctxdown.c - contextual melee downs, rebuilt on TLOU2's own environment melee (re/tlou2-env-melee.md). Included after paired.c in t2r-ext.c.
//
// The melee hit that DOWNS a standing enemy (Melee Down) looks at what is right behind the victim, the way TLOU2's single-player melee does
// for a fist "wall" / "low-cover" opener finisher:
//   * the pairs are the open (from a standing enemy, no contextual combo) fist finishers of player-fisurgeonguild-finishers.bin: wall head smash
//     and throat punch, low-cover left-hand face smash and left-hook-to-kick (CD_PAIRS; the lift-n-slam / elbow smash / combo head smash moves
//     of the old version are combo-only in the data: from-open? = 0);
//   * the selection test is the engine's own IsAgainstWallForMelee(MeleeProbeResult *, dist, ...) (exe 0x140e91720) called exactly like the
//     DC native against-wall-relative? calls it (0x140e69e70) with each move's own wall-dist-max (1.5 / 1.1 / 1.2 / 2.2 m), on the victim's
//     probes, in the sector of the attacker -> victim direction (0x140e8d580); it also hands back the move's apReference (bound frame 0);
//   * that frame IS the ap of the move: both halves are psync's, around it, at their authored offsets (the same START an E execution sends);
//   * camera: the clips' own authored animated cameras (apReference-camera1 / 2) through the finisher-camera hook, like TLOU2.
// The victim is not killed: at the pair's hand-off (the pelvis has reached the floor) it goes down through the normal downed state.
//
// No engine probe on the puppet (or t2r-cd-ray-probe.txt next to the exe) = a replica of the wall frame from static-world rays (a guess of
// the frame's convention, marked "src rays" in the logs). Default ON; t2r-no-contextual-downs.txt next to the exe = off (re-read every 2 s).
//
// Log lines ("ctxdown:" prefix), in the order they appear:
//   attacker  probes ...            the victim's MeleeProbeResult (raw flag bytes, sector distances) - once a second while a target is near
//             engine <pair> ...     IsAgainstWallForMelee result per pair, bf0 (the ap) and the authored spots (victim / attacker move m)
//             pick ...              the pair chosen
//             plan ready ...        the attacker's authored spot is reachable
//             START ...             psync START (kind 6), then the "psync:" lines
//             camera ...            finisher-camera ("finisher cam:" lines follow) or the xcam fallback
//             hand-off ...          the cut frame: both halves blend out
//   victim    on us ...             START accepted: guard, down timer; hand-off to downed ...
//   refusals  "ctxdown: ... fallback ..."  why the normal knockdown strike plays instead

static int g_cdOff = -1;
static uint64_t g_cdPoll;
static int CdEnabled(void)
{
    if (g_cdOff < 0 || g_frame - g_cdPoll > 120) {
        FILE *f = ExeSideOpen("t2r-no-contextual-downs.txt", "r");
        int off = f != NULL;
        if (f)
            fclose(f);
        if (off != g_cdOff)
            Xlog("ctxdown: %s",
                 off ? "OFF (t2r-no-contextual-downs.txt)" :
                 "on (default)");
        g_cdOff = off;
        g_cdPoll = g_frame ? g_frame : 1;
    }
    return !g_cdOff;
}

// Grass got detected as low cover / wall: the replica rays only answer t (fraction), no surface / material, so a hit cannot be asked
// what it is. Real surfaces are planes; vegetation / scattered clutter is not. The WALL / COVER verdicts need the rays to agree (cd_plane_ok);
// t2r-cd-no-plane-check.txt = the old unchecked verdicts.
static int g_cdPlaneOff = -1;
static int CdPlaneCheck(void)
{
    if (g_cdPlaneOff < 0) {
        FILE *f = ExeSideOpen("t2r-cd-no-plane-check.txt", "r");
        g_cdPlaneOff = f != NULL;
        if (f)
            fclose(f);
        if (g_cdPlaneOff)
            Xlog("ctxdown: plane check OFF (t2r-cd-no-plane-check.txt)");
    }
    return !g_cdPlaneOff;
}

static char g_cdSpWhy[128] = "not tried";     // the last result of SP's own MeleeProbe query on the victim puppet (logged with every replica decision)
static int g_cdRayFile = -1;  // t2r-cd-ray-probe.txt: use the ray replica of the wall frame instead of the engine probe (A/B comparison)
static int g_cdYawOver = -999;        // t2r-cd-wall-yaw.txt (degrees): turns the REPLICA frame only (the engine's frame is the engine's)
// Pins must work within 30-45 degrees of facing the wall head on:
// the surface rays were cast along the attack direction only, so a wall behind the victim at an angle was missed (open-ground verdicts, a face the rays
// slid past). The CONE SEARCH: when the cast along the attack direction finds no surface, the following scans cast along the attack direction turned
// by +22 / -22 / +45 / -45 deg (g_cd_rot), and take a surface whose normal is within CD_CONE_DEG (SP's own limit: the attacker within 45 deg of the
// frame's facing, re/tlou2-env-melee.md 3) of the reversed attack direction. The wall frame is the wall's own (victim back to the wall, attacker in front
// along the normal): the existing pair glide / pair-as-one rules ease both bodies onto it. t2r-pin-headon.txt next to the exe = the old head-on-only casts.
#define CD_CONE_DEG 45.0f
static const float CD_CONE_ROT[5] = { 0.0f, 22.0f, -22.0f, 45.0f, -45.0f };

static int g_cdRot;            // index into CD_CONE_ROT of the cast direction this scan uses (sticky while it keeps finding the surface)
static float g_cdUatt[2];      // the attack direction (attacker -> victim, xz) of the scan, un-rotated
static float g_cd_cand_ang, g_cd_plan_ang;      // the candidate's / the plan's surface normal, degrees off the reversed attack direction
static int g_cdHeadon = -1;    // t2r-pin-headon.txt present: the old head-on casts
static void CdFilesPoll(void)
{
    static uint64_t poll;
    if (g_frame - poll <= 120 && poll)
        return;
    poll = g_frame ? g_frame : 1;
    {
        FILE *hf = ExeSideOpen("t2r-pin-headon.txt", "r");
        int h = hf != NULL;
        if (hf)
            fclose(hf);
        if (h != g_cdHeadon) {
            g_cdHeadon = h;
            if (h)
                g_cdRot = 0;
            Xlog("ctxdown: surface cast cone %s",
                 h ? "OFF (t2r-pin-headon.txt: head-on casts only)" :
                 "ON (+-45 deg around the attack direction)");
        }
    }
    FILE *f = ExeSideOpen("t2r-cd-ray-probe.txt", "r");
    int r = f != NULL;
    if (f)
        fclose(f);
    if (r != g_cdRayFile) {
        g_cdRayFile = r;
        Xlog("ctxdown: wall frame source %s",
             r ? "RAYS (t2r-cd-ray-probe.txt)" :
             "the engine's probe (rays only if the engine probe cannot run)");
    }
    f = ExeSideOpen("t2r-cd-wall-yaw.txt", "r");
    int v = -999;
    if (f) {
        if (fscanf(f, "%d", &v) != 1)
            v = -999;
        fclose(f);
    }
    if (v != g_cdYawOver) {
        g_cdYawOver = v;
        Xlog("ctxdown: replica frame yaw offset %s",
             v == -999 ? "off" : "set");
    }
}

// kind 1 = wall (high wall probe, the "hwall" clips), 2 = low cover (low probe).
// dmax = the move's wall-dist-max: the 3rd float argument its start-func lambda passes to against-wall-relative? (data: lambdas of
//        player-fisurgeonguild-finishers.bin, re/tlou2-env-melee.md section 3); end_s = the move time the victim's pelvis has reached the
//        floor (pelvis < 0.25 m on the decoded victim clip, 30 fps), where the pair is cut and the victim goes down.
// measured contact times (ms) of the Abby fist finishers, from the decoded halves in ap space (hits.py): [0] = wrist-to-victim-head contacts (the first blow),
// [1] = the victim head's speed peak (the smash into the surface), counted from the START OF THE FINISHER's main clip
// [2] = when the attacker is released (ms from the finisher's start, 0 = the final blow + 0.45 s, the rule for the fists).
// Knife: SP's own release, SP sandbox round 3 E=65 (ellie-knife-low-cover-kill-gut-to-ice-pick-end): locomotion back 2.22 s into the -end clip = its last
// blade event (the curved-line flick, 1.73 s) + 0.49 s; every knife finisher is released at ITS last blade event + 0.45 s, the heavy blow = its blade-out
// ribbon (blood-ribbon-slash-eff), both read from the clips' own part-effect events. MP before: 0.70 x the hand-off time + 0.45 s = 3.0 s into the
// ice pick (the attacker kept playing ~1.5 s).
// Fists: SP E=28 (abby-fist-kill-hwall-combo-head-smash, head slam at 1.05 s) gave control back only at 2.75 s (+1.70 s); MP keeps +0.45 s
// after the slam (the victim needs time to escape; the attacker was stuck too long).
// [3] = 1: the first contact [0] is a GRAB (the hand closes on the head, no blow): the grab foley there, not a punch.
// wall-combo-head-smash (decoded halves): right hand on the forehead 0.23 s (wrist 0.15 m from the head from then to 1.6 s), head pulled back
// 0.7 m off the wall by 0.73 s, into the wall at 1.03 s (the clip's own headslam sound event 1.033 s; head 6.1 m/s, at the wall 1.07 s; SP E=28 1.05 s).
// A victim against a truck (A [68375]: face 0.47 m behind him, top 1.26 m -> 'no surface', normal knockdown): the band between a car hood (1.1) and the
// 1.7 m wall ray fell through both rules. Truck beds / SUV hoods / high counters up to 1.35 m are cover (the ledge finishers slam onto that top).
#define CD_COVER_MAX 1.35f
#define CD_HITS_DATA \
    {"wall-combo-head-smash", {230, 1030, 0, 1}}, {"wall-throat-punch", {630, 1030, 0}}, {"cover-face-smash", {500, 1330, 0}}, \
    {"cover-lift-n-slam", {470, 1230, 0}}, {"cover-combo-head-smash", {270, 1130, 0}}, \
    {"knife-cover-ice-pick", {470, 1070, 2180}}, {"knife-wall-chest-stab-long", {100, 670, 1620}}, {"knife-wall-left-chest-stab", {100, 330, 1180}}, \
    {"knife-wall-kneed-gut-slice", {30, 930, 1550}}, {"knife-cover-chest-stab", {30, 1370, 2380}}, \
    {"upg-crowbar-eye-jab", {30, 730, 1380}}, {"upg-blade-gut-chop-kick", {30, 900, 1450}}, \
    /* the upgraded 1h blunt one-shot = TLOU2's 1h blunt / hammer finishers (decoded halves): [0] = the blow the clip starts with (victim head speed */ \
    /* peak / weapon on the head f1-f4), [1] = the victim's head on the floor (its speed peak; the back head bonk: the knees down), [2] = the release = the */ \
    /* attacker's last wrist motion > 1.5 m/s + 0.1 s (the crowbar's 1380 ms is that rule: f42) */ \
    {"upg-blunt-head-stop", {30, 870, 900}}, {"upg-blunt-swing-through", {30, 970, 570}}, {"upg-blunt-upward-swing", {30, 1030, 500}}, \
    {"upg-blunt-quick-right", {30, 900, 770}}, {"upg-blunt-back-head-bonk", {130, 1400, 670}}, \
    /* weapon takedowns: ESTIMATES from the decoded halves - the weapon tip (wrist + 0.7 m along the forearm) within 0.28 m of the victim's head / chest at > 2 m/s, */ \
    /* ms into the finisher's main clip; 0 = no clean contact found (the default fractions of the hand-off time apply) */ \
    {"wpn-blade-wall-gut-chop", {300, 733, 0}}, {"wpn-blade-wall-neck-chop", {0, 1430, 0}}, {"wpn-blade-wall-skull-chop", {433, 633, 0}}, \
    {"wpn-blade-wall-shoulder-chop", {300, 733, 0}}, {"wpn-machete-wall-gut-stab", {400, 1000, 0}}, {"wpn-machete-wall-neck-slice", {0, 1770, 0}}, \
    {"wpn-blunt-wall-double-hit", {370, 0, 0}}, {"wpn-blunt-wall-left-sweep", {200, 0, 0}}, {"wpn-hammer-wall-neck-jab", {0, 1070, 0}}, {"wpn-2h-wall-head-bonk", {200, 730, 0}},
// THE POOL = ONLY the takedowns of the reference video (re/contextual-downs.md): knife (a female life) wall 02:20 / 02:25 / 02:30 and ledge / low
// cover 02:37 / 02:49, fists wall 04:56 / 05:08 and ledge / cover 05:15 / 05:35 / 05:43. Each is a CHAIN of TLOU2's own clips: OPEN moves = the finisher's
// intro (the run-in) + main clip, COMBO-only moves = the push pair cut at its best-matching frame + the finisher (see CD_PAIRS).
// Every segment is a pair of resident clips, played by psync (paired.c: chains) on every client on one clock; the pair is eligible only if ALL its clips are loaded.
// kind 1 = wall, 2 = low cover / ledge; dmax = the wall-dist-max of the move's start lambda (data: player-fisurgeonguild-finishers.bin / player-switchblade-finishers.bin; 2.5 for the
// finishers whose "open" start is the sequence's loop pieces); end_s = the finisher's victim pelvis on the floor (pelvis < 0.25 m on the decoded main / -end clip, 30 fps) + 0.05 s, counted
// from the START of the last segment; style = -1 unarmed fists, 3 = a female life's empty hands = Ellie's knife set (g_fem_knife_now).
typedef struct {
    const char *att, *vic;
    float spd, cut;
} CdSeg;                       // cut: clip seconds played before the switch to the next segment (0 = the shorter half's whole clip)
#define CD_MAXSEG 5
#define CD_SEG_QUICK 1.75f      // the ONE quick hit of a chain
#define CD_SEG_PUSHHIT 1.5f     // cover chains: the first combo-hit pair IS the push and the hit (the SP data has no separate enter pair for cover)
#define CD_ABBY_PAIR(base, spd, cut) {"abby-fist-" base "--abby", "abby-fist-" base "--npc-f", (spd), (cut)}
#define CD_ELLIE_PAIR(base, spd, cut) {"ellie-knife-" base "--ellie", "ellie-knife-" base "--npc-f", (spd), (cut)}
#define CD_RAW_PAIR(att, vic, spd, cut) {att, vic, (spd), (cut)}        // full clip names (the upgraded one-shots: crowbar / machete clips)
#define CD_WPN_PAIR(base, spd, cut) {"ellie-" base "--ellie", "ellie-" base "--npc-f", (spd), (cut)}    // the 1h / 2h melee weapon sets (anim-melee-player-shared-t2, Ellie's skeleton)
#define CD_WPNA_PAIR(base, spd, cut) {"abby-" base "--abby", "abby-" base "--npc-f", (spd), (cut)}      // (the 1h blunt right uppercut: anim-melee-abby-t2)
// The push is only used where the animation needs the victim already pushed into the wall / over the ledge (the finisher is chosen on the is-hit-gonna-down basis), and the chain is one continuous animation, 1:1 with the references: TLOU2 starts a finisher two ways (start lambdas, re/tlou2-env-melee.md 2-3):
//   OPEN (from-open? = 1: the enemy one hit from going down) - no push: the finisher's own -intro pair IS the run-in (attacker 1.2-1.8 m out, victim at the surface)
//         -> intro, then the main clip;
//   COMBO only (from-open? = 0: the victim must already be pinned) - the push pair first, CUT at the frame whose pose matches the finisher's first frame best
//         before the attacker lets go (tools/ctxcut.py on the decoded halves: mean joint gap 0.16-0.44 m; the old 0.9-1.0 s cuts were 0.3-1.1 m and past the release).
// Every segment plays at x1 (authored speed).
#include "mduel_moves.h"        // melee_duel.c: its -hit pair rows (kind 7, MDU_CD_ROWS)
#include "mduel_fin.h"          // melee_duel.c lethal blow: the SP finisher rows (kind 0 "mf-", MDF_CD_ROWS)
#include "mduel_pins.h"         // melee_duel_pin.c: the pin pair rows (kind 7, "mp-*", MPN_CD_ROWS)
#include "pullout_rows.h"       // melee_pullout.c: the prone pull-out pairs (kind 8, PUL_CD_ROWS, appended at the end of CD_PAIRS)
#include "mduel_parry.h"        // melee_duel_parry.c: the parry / clash pair rows (kind 7, MDP_CD_ROWS; tools/mv3/gen_duel_parry.py)
static const struct {
    const char *name;
    int kind;
    float dmax, end_s;
    int style, nseg;
    CdSeg seg[CD_MAXSEG];
} CD_PAIRS[] = {
    // ---- fists (Abby), wall, OPEN: 05:08 throat strike + head into the wall
    { "wall-throat-punch", 1, 1.6f, 2.20f, -1, 2,
     { CD_ABBY_PAIR("hwall-melee-throat-punch-intro", 1.0f, 0.0f),
      CD_ABBY_PAIR("hwall-melee-throat-punch", 1.0f, 0.0f)} },
    // ---- fists, wall, COMBO: 04:56 forehead grab, head PULLED BACK 0.7 m off the wall, then smashed into it (SP E=28's finisher; replaces
    //      the quick 1.5 s hwall-melee-head-smash shove - the slower one, fairer vs the other takedowns). fist-wall-right-combo-head-smash-finisher is
    //      combo-only (dmax 2.5), so it starts from the contextual wall opener fist-wall-attack-enter-right / -right-far (window 0.5 .. 2.0 m, cd_dmin):
    //      abby-fist-hwall-enter-npc-close-hit (victim authored 0.50 m out -> 0.18 m) / -npc-far-hit (1.72 m -> 0.17 m), each from frame 0, cut at the
    //      root match to the finisher intro's frame 0 (tools/ctxcut.py), then SP's intro (both halves, 0.23 s) and the finisher (hand grabs the head 0.23 s,
    //      pulled back to 0.73 s, head into the wall 1.07 s at 6.1 m/s, let go 1.6 s, pelvis on the floor 3.17 s)
    //      SP capture (re/sp-abby-fist-sounds-1026.log): the pinning hit plays 0.53 s before the finisher intro takes over (ours was 0.87 s) - the
    //      close push is cut at 0.55 s so the pin -> grab -> slam rhythm is SP's (slam 1.04 s into the finisher both ways)
    { "wall-combo-head-smash", 1, 2.4f, 3.22f, -1, 3,
     { CD_ABBY_PAIR("hwall-enter-npc-close-hit", 1.0f, 0.55f),
      CD_ABBY_PAIR("kill-hwall-combo-head-smash-intro", 1.0f, 0.0f),
      CD_ABBY_PAIR("kill-hwall-combo-head-smash", 1.0f, 0.0f)} },
    { "wall-combo-head-smash~far", 1, 2.4f, 3.22f, -1, 3,
     { CD_ABBY_PAIR("hwall-enter-npc-far-hit", 1.0f, 1.20f),
      CD_ABBY_PAIR("kill-hwall-combo-head-smash-intro", 1.0f, 0.0f),
      CD_ABBY_PAIR("kill-hwall-combo-head-smash", 1.0f, 0.0f)} },
    // ---- fists, low cover, OPEN: 05:15 grab + rotate + face onto the ledge (its intro is attacker-only: the main pair starts the run-in itself, 1.16 m out)
    { "cover-face-smash", 2, 1.7f, 2.80f, -1, 1,
     { CD_ABBY_PAIR("low-cover-kill-left-hand-face-smash", 1.0f, 0.0f)} },
    // ---- fists, low cover, COMBO: 05:35 pick up + head onto the ledge, 05:43 grab + head onto the ledge - push cut at the best match (0.16 / 0.33 m)
    //      (SP rules: a combo-only finisher needs the contextual low-cover swing first, whose opener window is the defender 0.5 .. 1.7 m from the
    //      cover frame (fist-low-cover-neck-attack-enter-close / -far: wall-min-range 0.5, wall-range 1.7, player-fisurgeonguild-contextual.bin) - cd_dmin)
    //      The old pushes low-cover-combo-2 / -3-hit move the victim 0.03 / 0.00 m (both bodies are
    //      already at the cover in them: they are the hits of a running combo). SP's push INTO the cover is the low-cover opener pair (contextual.bin
    //      fist-low-cover-neck-attack-enter-close / -shoulder-attack-enter-far): abby-fist-lc-enter-player-close-npc-close-neck-hit (victim authored
    //      0.59 m out -> 0.13 m, attacker 0.65 m) and -npc-far-shoulder-hit (2.02 m -> 0, attacker 2.18 m). Played from frame 0, cut where both bodies
    //      are nearest the finisher's start (root match: neck 0.93 s, 0.29 / 0.10 m; shoulder 1.50 s, 0.33 / 0.22 m). "~far" = the far variant of the
    //      same move: cd_pick plays, per move, the variant whose authored start is nearest the bodies (SP's "ideal wall distance" ranking).
    { "cover-lift-n-slam", 2, 1.9f, 2.05f, -1, 2,
     { CD_ABBY_PAIR
      ("lc-enter-player-close-npc-close-neck-hit", 1.0f, 0.93f),
      CD_ABBY_PAIR("kill-cover-combo-right-lift-n-slam", 1.0f, 0.0f)} },
    { "cover-lift-n-slam~far", 2, 2.4f, 2.05f, -1, 2,
     { CD_ABBY_PAIR
      ("lc-enter-player-close-npc-far-shoulder-hit", 1.0f, 1.50f),
      CD_ABBY_PAIR("kill-cover-combo-right-lift-n-slam", 1.0f, 0.0f)} },
    { "cover-combo-head-smash", 2, 1.9f, 2.10f, -1, 2,
     { CD_ABBY_PAIR
      ("lc-enter-player-close-npc-close-neck-hit", 1.0f, 0.93f),
      CD_ABBY_PAIR("kill-cover-combo-right-head-smash", 1.0f, 0.0f)} },
    { "cover-combo-head-smash~far", 2, 2.4f, 2.10f, -1, 2,
     { CD_ABBY_PAIR
      ("lc-enter-player-close-npc-far-shoulder-hit", 1.0f, 1.50f),
      CD_ABBY_PAIR("kill-cover-combo-right-head-smash", 1.0f, 0.0f)} },
    // ---- knife (Ellie), wall, COMBO: 02:25 / 02:20 gut stabs (push cut 0.24 / 0.31 m); OPEN: 02:30 kneed gut slash (intro = the run-in)
    //      (SP rules, player-switchblade-finishers.bin: EVERY knife wall / low-cover finisher of the pool is open-startable - the chest-stab / left-chest-stab /
    //      gut-ice-pick ones through their "-intro" move whose lambda only refuses a running contextual combo - with wall-dist-max 2.5 (neck-slice 1.5, not in the pool);
    //      the high probe wins: "(if against-wall-result (= low-cover? #f) (= low-cover? #t))")
    //      push variants by distance (contextual.bin switchblade-wall-attack-enter-right / -right-far / -left-enter): victim authored start
    //      stab-enter-short-hit 0.60 m (cut 0.37 s), enter-hit-left 1.33 m (0.55 s), enter-hit 1.77 m (0.75 / 0.87 s) - each played from frame 0
    //      The push was short and insta-cut to the wall death instead of a short fluid pause: SP's real combo
    //      path (re/sp-sandbox-runs/knife-1020b.named.log) is push -> combo-intro (attacker only; the victim keeps its slash-1 hit) 0.37-0.40 s -> combo-mid
    //      0.27-0.40 s (the pinned stab, both halves) -> end. The intro / mid / end joins are seamless (0.001-0.024 m); the pushes are cut where they match
    //      the intro's frame 0 (D 0.15-0.27 m; the old 0.37 s close cut was mid-lunge). Chain before the end: 1.67 s (long) / 1.50 s (left), was 0.37-0.87 s.
    { "knife-wall-chest-stab-long", 1, 2.5f, 1.20f, 3, 4,
     { CD_ELLIE_PAIR("wall-combo-enter-hit", 1.0f, 1.00f),
      CD_RAW_PAIR
      ("ellie-knife-kill-wall-chest-stab-long-combo-intro--ellie",
       "ellie-knife-wall-combo-slash-1-hit--npc-f", 1.0f, 0.0f),
      CD_ELLIE_PAIR("kill-wall-chest-stab-long-combo-mid", 1.0f, 0.0f),
      CD_ELLIE_PAIR("kill-wall-chest-stab-long-end", 1.0f, 0.0f)} },
    { "knife-wall-chest-stab-long~close", 1, 2.5f, 1.20f, 3, 4,
     { CD_ELLIE_PAIR("wall-combo-stab-enter-short-hit", 1.0f, 0.70f),
      CD_RAW_PAIR
      ("ellie-knife-kill-wall-chest-stab-long-combo-intro--ellie",
       "ellie-knife-wall-combo-slash-1-hit--npc-f", 1.0f, 0.0f),
      CD_ELLIE_PAIR("kill-wall-chest-stab-long-combo-mid", 1.0f, 0.0f),
      CD_ELLIE_PAIR("kill-wall-chest-stab-long-end", 1.0f, 0.0f)} },
    { "knife-wall-left-chest-stab", 1, 2.5f, 1.30f, 3, 4,
     { CD_ELLIE_PAIR("wall-combo-enter-hit-left", 1.0f, 0.73f),
      CD_RAW_PAIR
      ("ellie-knife-wall-kill-left-chest-stab-combo-intro--ellie",
       "ellie-knife-wall-combo-slash-1-hit--npc-f", 1.0f, 0.0f),
      CD_ELLIE_PAIR("wall-kill-left-chest-stab-combo-mid", 1.0f, 0.0f),
      CD_ELLIE_PAIR("wall-kill-left-chest-stab-end", 1.0f, 0.0f)} },
    { "knife-wall-left-chest-stab~close", 1, 2.5f, 1.30f, 3, 4,
     { CD_ELLIE_PAIR("wall-combo-stab-enter-short-hit", 1.0f, 0.60f),
      CD_RAW_PAIR
      ("ellie-knife-wall-kill-left-chest-stab-combo-intro--ellie",
       "ellie-knife-wall-combo-slash-1-hit--npc-f", 1.0f, 0.0f),
      CD_ELLIE_PAIR("wall-kill-left-chest-stab-combo-mid", 1.0f, 0.0f),
      CD_ELLIE_PAIR("wall-kill-left-chest-stab-end", 1.0f, 0.0f)} },
    { "knife-wall-left-chest-stab~far", 1, 2.5f, 1.30f, 3, 4,
     { CD_ELLIE_PAIR("wall-combo-enter-hit", 1.0f, 0.97f),
      CD_RAW_PAIR
      ("ellie-knife-wall-kill-left-chest-stab-combo-intro--ellie",
       "ellie-knife-wall-combo-slash-1-hit--npc-f", 1.0f, 0.0f),
      CD_ELLIE_PAIR("wall-kill-left-chest-stab-combo-mid", 1.0f, 0.0f),
      CD_ELLIE_PAIR("wall-kill-left-chest-stab-end", 1.0f, 0.0f)} },
    { "knife-wall-kneed-gut-slice", 1, 2.5f, 2.95f, 3, 2,
     { CD_ELLIE_PAIR("wall-kill-kneed-gut-slice-intro", 1.0f, 0.0f),
      CD_ELLIE_PAIR("wall-kill-kneed-gut-slice", 1.0f, 0.0f)} },
    // ---- knife, ledge / low cover: 02:37 COMBO (the close push matches best, 0.44 m), 02:49 OPEN (intro = the run-in)
    //      push variants (SP E=65 played enter-far + enter-close-hit): enter-close-hit 0.60 m (cut 0.50 s), enter-hit 1.17 m (0.87 s), enter-left-hit 1.56 m (0.93 s)
    { "knife-cover-ice-pick", 2, 2.5f, 3.65f, 3, 2,
     { CD_ELLIE_PAIR("low-cover-combo-enter-close-hit", 1.0f, 0.50f),
      CD_ELLIE_PAIR("low-cover-kill-gut-to-ice-pick-end", 1.0f, 0.0f)} },
    { "knife-cover-ice-pick~mid", 2, 2.5f, 3.65f, 3, 2,
     { CD_ELLIE_PAIR("low-cover-combo-enter-hit", 1.0f, 0.87f),
      CD_ELLIE_PAIR("low-cover-kill-gut-to-ice-pick-end", 1.0f, 0.0f)} },
    { "knife-cover-ice-pick~far", 2, 2.5f, 3.65f, 3, 2,
     { CD_ELLIE_PAIR("low-cover-combo-enter-left-hit", 1.0f, 0.93f),
      CD_ELLIE_PAIR("low-cover-kill-gut-to-ice-pick-end", 1.0f, 0.0f)} },
    { "knife-cover-chest-stab", 2, 2.5f, 3.70f, 3, 2,
     { CD_ELLIE_PAIR("low-cover-kill-chest-stab-intro", 1.0f, 0.0f),
      CD_ELLIE_PAIR("low-cover-kill-chest-stab", 1.0f, 0.0f)} },
    // ---- held MELEE WEAPONS. TLOU2's weapon wall / low-cover finishers
    //      (player-1h-blade / -1h-blunt / -2h-blunt-finishers.bin, macro melee-player-wall-finisher-with-intro, context wall / low-cover; re/contextual-downs.md "Melee weapons"):
    //      the start lambda's weapon mask picks who may play it (cd_weapon_mask): style 1 = every 1h blade (machete, tomahawk, hatchet), 4 = the machete's own two, 0 = every 1h
    //      blunt, 5 = the crowbar / hammer / club / wrench neck jab, 2 = the 2h blunt (bat, 2x4, lead pipe). OPEN moves (from-open? = 1: the enemy one hit from going down) =
    //      the finisher's own intro (the run-in, where both halves have it) + the main clip, exactly as authored; window = the lambda's wall-dist-max. The streamed clips
    //      (1h low-cover double bash, 2h knee break / baseball swing / left swing) never play through animate_ (re/melee-finishers.md) and are left out; the ledge kills are an
    //      edge probe, not a wall. Names "wpn-*": no native sound rows (their banks are not in the MP level: the PCM weapon cues play, blade_cd_act / cd_sfx_label).
    //      end_s = the victim's pelvis on the floor + 0.05 s (decoded halves, tools/ctxcam_poses.py), counted from the start of the last segment.
    { "wpn-blade-wall-gut-chop", 1, 2.5f, 2.15f, 1, 2,
     { CD_WPN_PAIR("1hblade-hwall-gut-chop-intro", 1.0f, 0.0f),
      CD_WPN_PAIR("1hblade-hwall-gut-chop", 1.0f, 0.0f)} },
    { "wpn-blade-wall-neck-chop", 1, 2.0f, 2.45f, 1, 2,
     { CD_WPN_PAIR("1hblade-hwall-kill-left-neck-chop-intro", 1.0f, 0.0f),
      CD_WPN_PAIR("1hblade-hwall-kill-left-neck-chop", 1.0f, 0.0f)} },
    { "wpn-blade-wall-skull-chop", 1, 2.0f, 0.82f, 1, 2,
     { CD_WPN_PAIR("1hblade-hwall-skull-chop-intro", 1.0f, 0.0f),
      CD_WPN_PAIR("1hblade-hwall-skull-chop", 1.0f, 0.0f)} },
    { "wpn-blade-wall-shoulder-chop", 1, 2.4f, 1.85f, 1, 2,
     { CD_WPN_PAIR("1hblade-hwall-kill-shoulder-chop-intro", 1.0f, 0.0f),
      CD_WPN_PAIR("1hblade-hwall-kill-shoulder-chop", 1.0f, 0.0f)} },
    { "wpn-machete-wall-gut-stab", 1, 2.5f, 2.68f, 4, 2,
     { CD_WPN_PAIR("machete-wall-kill-left-gut-stab-intro", 1.0f, 0.0f),
      CD_WPN_PAIR("machete-wall-kill-left-gut-stab", 1.0f, 0.0f)} },
    { "wpn-machete-wall-neck-slice", 1, 2.5f, 3.15f, 4, 2,
     { CD_WPN_PAIR
      ("machete-wall-kill-slash-2-neck-slice-intro", 1.0f, 0.0f),
      CD_WPN_PAIR("machete-wall-kill-slash-2-neck-slice", 1.0f, 0.0f)} },
    { "wpn-blunt-wall-double-hit", 1, 2.2f, 1.25f, 0, 1,
     { CD_WPN_PAIR("1hblunt-hwall-kill-double-hit", 1.0f, 0.0f)} },
    { "wpn-blunt-wall-head-bash", 1, 2.0f, 1.72f, 0, 2,
     { CD_WPN_PAIR("1hblunt-hwall-kill-head-bash-intro", 1.0f, 0.0f),
      CD_WPN_PAIR("1hblunt-hwall-kill-head-bash", 1.0f, 0.0f)} },
    { "wpn-blunt-wall-left-sweep", 1, 2.0f, 2.32f, 0, 2,
     { CD_WPN_PAIR("1hblunt-hwall-kill-left-sweep-intro", 1.0f, 0.0f),
      CD_WPN_PAIR("1hblunt-hwall-kill-left-sweep", 1.0f, 0.0f)} },
    { "wpn-blunt-wall-uppercut", 1, 2.5f, 1.38f, 0, 2,
     { CD_WPNA_PAIR("1hblunt-kill-uppercut-intro", 1.0f, 0.0f),
      CD_WPNA_PAIR("1hblunt-kill-uppercut", 1.0f, 0.0f)} },
    { "wpn-hammer-wall-neck-jab", 1, 2.5f, 1.42f, 5, 2,
     { CD_WPN_PAIR("hammer-hwall-kill-left-neck-jab-intro", 1.0f, 0.0f),
      CD_WPN_PAIR("hammer-hwall-kill-left-neck-jab", 1.0f, 0.0f)} },
    { "wpn-2h-wall-head-bonk", 1, 1.5f, 1.48f, 2, 2,
     { CD_WPN_PAIR("2hblunt-wall-kill-head-bonk-intro", 1.0f, 0.0f),
      CD_WPN_PAIR("2hblunt-wall-kill-head-bonk", 1.0f, 0.0f)} },
    //      low cover: the 1h blade hack finisher is combo-only (from-open? = 0): the push pair (the contextual low-cover opener's -hit pair, by distance: close / enter / left)
    //      cut at the best root match to the finisher, then the finisher (its intro is attacker-only: left out). The 1h blunt low-cover double bash is streamed: none.
    { "wpn-blade-cover-hack", 2, 2.5f, 2.15f, 1, 2,
     { CD_WPN_PAIR("1hblade-low-cover-combo-enter-close-hit", 1.0f, 1.13f),
      CD_WPN_PAIR("1hbade-cover-kill-combo-hack", 1.0f, 0.0f)} },
    { "wpn-blade-cover-hack~mid", 2, 2.5f, 2.15f, 1, 2,
     { CD_WPN_PAIR("1hblade-low-cover-combo-enter-hit", 1.0f, 0.73f),
      CD_WPN_PAIR("1hbade-cover-kill-combo-hack", 1.0f, 0.0f)} },
    { "wpn-blade-cover-hack~far", 2, 2.5f, 2.15f, 1, 2,
     { CD_WPN_PAIR("1hblade-low-cover-combo-enter-left-hit", 1.0f, 1.30f),
      CD_WPN_PAIR("1hbade-cover-kill-combo-hack", 1.0f, 0.0f)} },
    // ---- kind 0 = NO surface: the UPGRADED 1h melee one-shot (the 2013 pipe moves only make sense for the upgraded bat, 2x4 and pipe; the
    //      crowbar and tomahawk need their own: TLOU2's animations for both - the crowbar: Abby's head stab finisher; the tomahawk: the one that digs
    //      it into the side and kicks them away). Never picked by the scan (kind 0 matches no surface); cd_upg_start (from t2r-ext.c upg13_start, the
    //      connected one-shot swing) plays it on a STANDING victim, which is DOWNED at the hand-off, never killed (cd_victim_begin). One clip pair each,
    //      no front / back variant (the blade): the pair is anchored on the victim and turned so the authored attacker spot points at us. The attacker-only
    //      -intro clips are left out: our own connected swing IS the run-in, the main clips start with the weapon already in the body (decoded halves,
    //      30 fps: crowbar wrist 0.18 m from the victim's head at f0, sfx-crowbar-one-hand-finisher-stab-eye f1 / -stab-extract f22; machete blade in the
    //      gut f1 (bm-knife-wound-cloth, chest_holster_attach), Ellie's left foot 0.16 m from the victim's pelvis f25-30 = the kick (sfx-machete-impact-kick
    //      f27), extract f32-33, victim thrown 2.7 m). end_s = victim pelvis < 0.25 m (f56 = 1.87 s / f51 = 1.70 s) + 0.05 s.
    //      upg-crowbar-eye-jab = 1h-blunt-abby-left-crowbar-head-jab-finisher-sync (player-1h-blunt-finishers.bin; anim-melee-abby-t2, resident, 97 f each half)
    //      upg-blade-gut-chop-kick = machete-slash-3-finisher-sync / 1h-blade-ledge-left-finisher-sync (player-1h-blade-finishers.bin; anim-melee-player-shared-t2,
    //      resident, attacker 205 f / victim 260 f)
    //      The upgraded crowbar stab is replaced by TLOU2's hammer finishers (a one-hand blunt style, not a chop): the crowbar class (crowbar, hammers, 1h clubs, pipe wrench) plays Ellie's 1h blunt / hammer finishers below; the eye jab row stays
    //      only for t2r-upg-crowbar-stab.txt next to the exe (cd_upg_start never picks it otherwise).
    { "upg-crowbar-eye-jab", 0, 0.0f, 1.92f, 0, 1,
     { CD_RAW_PAIR
      ("abby-crowbar-kill-left-head-jab--abby",
       "abby-crowbar-kill-left-head-jab--npc-f", 1.0f, 0.0f)} },
    //      The 1h blunt / hammer finishers (player-1h-blunt-finishers.bin moves 1h-blunt-left-head-stop-finisher, -left-swing-through-finisher, -quick-left,
    //      -quick-right, -back-head-bonk-finisher; finisher_anims.h). All halves RESIDENT in anim-melee-player-shared-t2 (ClipData > 128 bytes, tools/mv3/gen_moves.py
    //      Clips.resident): head-stop 66 / 66 f, swing-through 71 / 71, upward-swing 59 / 59, quick-right 72 / 57, head-bonk 38 / 51. Every main clip starts
    //      AT the blow (victim head speed peak / hammer on the head f1-f4) like the crowbar's, so the connected swing is the run-in and the -intro clips stay out.
    //      Authored attacker spot (frame 0, decoded halves, tools/ctxcam_poses.py): the four front ones 0.90-1.20 m out, 3-15 deg off the victim's facing;
    //      the head bonk 0.89 m out at 148 deg = BEHIND (cd_upg_start picks by our side of the victim's facing, round robin among the side's rows; names
    //      with "-back-" = the back rows). end_s = victim pelvis < 0.25 m + 0.05 s: f25 0.88 / f29 1.02 / f31 1.08 / f27 0.95 s; the head bonk leaves the
    //      victim on its knees (pelvis 0.49 m from f45 to the clip's end): its hand-off is there, f45 + 0.05 = 1.55 s. Left out: the back knee double bash
    //      (its victim half is STREAMED, a 0-frame stub + chunks), the knee-face stun finisher (the victim starts kneeling, head 0.62 m), the E3 hammer
    //      face break (a claw STAB to the head, sfx-hammer-finisher-head-stab, and its victim half is not on the npc-f skeleton), wall / ledge / dog kills.
    { "upg-blunt-head-stop", 0, 0.0f, 0.88f, 0, 1,
     { CD_RAW_PAIR
      ("ellie-1hblunt-kill-left-head-stop--ellie",
       "ellie-1hblunt-kill-left-head-stop--npc-f", 1.0f, 0.0f)} },
    { "upg-blunt-swing-through", 0, 0.0f, 1.02f, 0, 1,
     { CD_RAW_PAIR
      ("ellie-1hblunt-kill-left-horizontal-swing-through--ellie",
       "ellie-1hblunt-kill-left-horizontal-swing-through--npc-f", 1.0f,
       0.0f)} },
    { "upg-blunt-upward-swing", 0, 0.0f, 1.08f, 0, 1,
     { CD_RAW_PAIR
      ("ellie-1hblunt-kill-left-upward-swing--ellie",
       "ellie-1hblunt-kill-left-upward-swing--npc-f", 1.0f, 0.0f)} },
    { "upg-blunt-quick-right", 0, 0.0f, 0.95f, 0, 1,
     { CD_RAW_PAIR
      ("ellie-1hblunt-kill-quick-right--ellie",
       "ellie-1hblunt-kill-quick-right--npc-f", 1.0f, 0.0f)} },
    { "upg-blunt-back-head-bonk", 0, 0.0f, 1.55f, 0, 1,
     { CD_RAW_PAIR
      ("ellie-1hblunt-sneak-kill-head-bonk--ellie",
       "ellie-1hblunt-sneak-kill-head-bonk--npc-f", 1.0f, 0.0f)} },
    { "upg-blade-gut-chop-kick", 0, 0.0f, 1.75f, 1, 1,
     { CD_RAW_PAIR
      ("ellie-machete-kill-slash-3-gut-chop--ellie",
       "ellie-machete-kill-slash-3-gut-chop--npc-f", 1.0f, 0.0f)} },
    // ---- kind 3 / 4 / 5 = the DODGE COUNTER (dcounter.c, re/scripted-melee-1026.md): the connected exchange after a successful dodge. dmax = the attacker half's hit moment (clip seconds, the
    //      weapon-trail event of the attack clip; the contextual rows: the -hit pair begins AT contact, so the hit is 0.30 s after the start to give the victim's escape window its telegraph), end_s = the hand-off (both halves blend out together; clip seconds from the clip START). Never picked by the surface scan (kind > 2).
    //      3 = open ground (SP's player dodge counter pairs: fist-dodge-back-attack-1 -> abby-fist-dodge-back-attack--abby + -hit--npc-f ...), 4 = high wall / 5 = low cover (TLOU2's contextual combo hit
    //      pairs of the same weapon sets: the engine's probe picks the variant, a wall never cancels the exchange).
    { "dc-fist-back", 3, 0.33f, 1.25f, -1, 1,
     { CD_RAW_PAIR
      ("abby-fist-dodge-back-attack--abby",
       "abby-fist-dodge-back-attack-hit--npc-f", 1.0f, 0.0f)} },
    // The counter punch has a bigger wind-up so it feels stronger and punchier: TLOU2's haymaker - its attacker-only intro (0.53 s, the
    // held pull-back) then the main pair from the contact, cut into the stagger (the victim collapses by ~1.0 s of the main clip: hand-off 0.62 s into it).
    // The intro ends exactly on the main clip's first placement in the same apReference (offline check, ctxcam_lib), so the chain is seamless; the victim is
    // held on the main clip's first frame through the intro (paired.c, dc_seg0_hold). Preferred for fists over the plain dodge-back counter when loaded.
    { "dc-fist-haymaker", 3, 0.53f, 1.15f, -1, 2,
     { CD_RAW_PAIR
      ("abby-fist-kill-right-haymaker-intro--abby",
       "abby-fist-kill-right-haymaker--npc-f", 1.0f, 0.0f),
      CD_RAW_PAIR("abby-fist-kill-right-haymaker--abby",
                  "abby-fist-kill-right-haymaker--npc-f", 1.0f, 0.0f)} },
    { "dc-fist-fw", 3, 0.57f, 1.50f, -1, 1,
     { CD_RAW_PAIR
      ("abby-fist-dodge-foward-attack--abby",
       "abby-fist-dodge-foward-attack-hit--npc-f", 1.0f, 0.0f)} },
    { "dc-1hblunt-back", 3, 0.43f, 1.30f, 0, 1,
     { CD_RAW_PAIR
      ("abby-1hblunt-dodge-back-attack--abby",
       "abby-1hblunt-dodge-back-attack-hit--npc-f", 1.0f, 0.0f)} },
    { "dc-1hblunt-fw", 3, 0.43f, 1.30f, 0, 1,
     { CD_RAW_PAIR
      ("abby-1hblunt-dodge-fw-attack--abby",
       "abby-1hblunt-dodge-fw-attack-hit--npc-f", 1.0f, 0.0f)} },
    // knife (Ellie hits the attacker in the face with the bottom of her palm while the knife's in the hand): TLOU2's knife stun handle bash 2.
    // Offline (ctxcam poses): right (knife) wrist 0.20 m from the victim's head at the contact 0.37 s (solo-to-pair pose match, RMS 0.015 m), 0.43 m pull-back
    // over 0.33 s, 15.9 m/s; the knife victim half staggers (head 1.04 m at +0.5 s) and stays up - the 1hblade twin collapses, so the knife clip is used.
    // Preferred for knives over the fist haymaker (dc_plan); no knife-pak clip -> the knife falls back to the fist rows as before.
    // The knife counter attack should be more of a backhand slash/hit than just a hit on the face: SP plays switchblade-stun-breaker-bash
    // (= handle-bash-2--ellie) for 368 ms only, then switchblade-stun-breaker-bash-hit (= handle-bash-2-hit--ellie) from the contact (SP trace MARK 17,
    // 3406426.7 -> 3406794.9); the plain clip played on past 0.37 s is its swing-through, which read as a backhand. Now the same chain: wind-up cut at
    // 0.37 s, the -hit pair from the contact, the victim held on its first frame through the wind-up (dc_seg0_hold). Contact +0.04 s into the -hit pair.
    { "dc-knife-palm", 3, 0.41f, 0.97f, 3, 2,
     { CD_RAW_PAIR
      ("ellie-knife-stun-handle-bash-2--ellie",
       "ellie-knife-stun-handle-bash-2-hit--npc-f", 1.0f, 0.37f),
      CD_RAW_PAIR("ellie-knife-stun-handle-bash-2-hit--ellie",
                  "ellie-knife-stun-handle-bash-2-hit--npc-f", 1.0f,
                  0.0f)} },
    { "dc-knife-back", 3, 0.47f, 1.35f, 3, 1, { CD_RAW_PAIR("ellie-knife-dodge-back-attack", "ellie-knife-dodge-back-attack-hit--npc-m", 1.0f, 0.0f)} },        // SP's knife dodge counter
    { "dc-1hblade-back", 3, 0.47f, 1.35f, 1, 1,
     { CD_RAW_PAIR
      ("ellie-1hblade-dodge-back-attack--ellie",
       "ellie-1hblade-dodge-back-attack-hit--npc-f", 1.0f, 0.0f)} },
    { "dc-1hblade-heavy", 3, 0.50f, 1.35f, 1, 1,
     { CD_RAW_PAIR
      ("ellie-1hblade-dodge-bk-heavy-atk",
       "ellie-1hblade-dodge-bk-heavy-atk-hit--npc-m", 1.0f, 0.0f)} },
    { "dc-2h-back", 3, 0.50f, 1.40f, 2, 1,
     { CD_RAW_PAIR
      ("ellie-2hblunt-dodge-back-attack--ellie",
       "ellie-2hblunt-dodge-back-attack-hit--npc-f", 1.0f, 0.0f)} },
    { "dc-fist-hwall", 4, 0.42f, 1.55f, -1, 1,
     { CD_RAW_PAIR
      ("abby-fist-hwall-combo-1-hit--abby",
       "abby-fist-hwall-combo-1-hit--npc-f", 1.0f, 0.0f)} },
    { "dc-fist-lcover", 5, 0.42f, 1.55f, -1, 1,
     { CD_RAW_PAIR
      ("abby-fist-low-cover-combo-1-hit--abby",
       "abby-fist-low-cover-combo-1-hit--npc-f", 1.0f, 0.0f)} },
    { "dc-1hblunt-hwall", 4, 0.42f, 1.55f, 0, 1,
     { CD_RAW_PAIR
      ("ellie-1hblunt-hwall-combo-strike-1-hit--ellie",
       "ellie-1hblunt-hwall-combo-strike-1-hit--npc-f", 1.0f, 0.0f)} },
    { "dc-1hblunt-lcover", 5, 0.42f, 1.55f, 0, 1,
     { CD_RAW_PAIR
      ("ellie-1hblunt-low-cover-combo-strike-1-hit--ellie",
       "ellie-1hblunt-low-cover-combo-strike-1-hit--npc-f", 1.0f, 0.0f)} },
    { "dc-1hblade-hwall", 4, 0.42f, 1.55f, 1, 1,
     { CD_RAW_PAIR
      ("ellie-1hblade-hwall-combo-slash-1-hit--ellie",
       "ellie-1hblade-hwall-combo-slash-1-hit--npc-f", 1.0f, 0.0f)} },
    { "dc-1hblade-lcover", 5, 0.42f, 1.55f, 1, 1,
     { CD_RAW_PAIR
      ("ellie-1hblade-low-cover-combo-slash-1-hit--ellie",
       "ellie-1hblade-low-cover-combo-slash-1-hit--npc-f", 1.0f, 0.0f)} },
    { "dc-2h-hwall", 4, 0.42f, 1.55f, 2, 1,
     { CD_RAW_PAIR
      ("ellie-2hblunt-hwall-combo-strike-1-hit--ellie",
       "ellie-2hblunt-hwall-combo-strike-1-hit--npc-f", 1.0f, 0.0f)} },
    // ---- melee SEQUENCE rows (melee_seq.c, kind 6): the synced hit pair of blow N of a melee exchange; the clips BEGIN AT THE CONTACT (dmax = the victim vox / blood event at 0.03 s), end_s = the
    //      default hand-off (melee_seq.c msq_end_s overrides it: 0.65 s fists / 0.75 s weapons); style as the takedown rows (-1 fists, 0 1h blunt, 1 1h blade, 2 2h, 3 a female life's knife)
    { "ms-fist-1", 6, 0.03f, 0.75f, -1, 1,
     { CD_RAW_PAIR
      ("abby-fist-combo-punch-1-hit--abby",
       "abby-fist-combo-punch-1-hit--npc-f", 1.0f, 0.0f)} },
    { "ms-fist-2", 6, 0.03f, 0.75f, -1, 1,
     { CD_RAW_PAIR
      ("abby-fist-combo-punch-2-hit--abby",
       "abby-fist-combo-punch-2-hit--npc-f", 1.0f, 0.0f)} },
    { "ms-fist-3", 6, 0.03f, 0.75f, -1, 1,
     { CD_RAW_PAIR
      ("abby-fist-combo-punch-3-hit--abby",
       "abby-fist-combo-punch-3-hit--npc-f", 1.0f, 0.0f)} },
    { "ms-1hblunt-1", 6, 0.03f, 0.75f, 0, 1,
     { CD_RAW_PAIR
      ("ellie-1hblunt-combo-strike-1-hit--ellie",
       "ellie-1hblunt-combo-strike-1-hit--npc-f", 1.0f, 0.0f)} },
    { "ms-1hblunt-2", 6, 0.03f, 0.75f, 0, 1,
     { CD_RAW_PAIR
      ("ellie-1hblunt-combo-strike-2-hit--ellie",
       "ellie-1hblunt-combo-strike-2-hit--npc-f", 1.0f, 0.0f)} },
    { "ms-1hblunt-3", 6, 0.03f, 0.75f, 0, 1,
     { CD_RAW_PAIR
      ("ellie-1hblunt-combo-strike-3-hit--ellie",
       "ellie-1hblunt-combo-strike-3-hit--npc-f", 1.0f, 0.0f)} },
    { "ms-1hblade-1", 6, 0.03f, 0.75f, 1, 1,
     { CD_RAW_PAIR
      ("ellie-machete-combo-slash-1-hit--ellie",
       "ellie-machete-combo-slash-1-hit--npc-m", 1.0f, 0.0f)} },
    { "ms-1hblade-2", 6, 0.03f, 0.75f, 1, 1,
     { CD_RAW_PAIR
      ("ellie-machete-combo-slash-2-hit--ellie",
       "ellie-machete-combo-slash-2-hit--npc-m", 1.0f, 0.0f)} },
    { "ms-1hblade-3", 6, 0.03f, 0.75f, 1, 1,
     { CD_RAW_PAIR
      ("ellie-machete-combo-slash-3-hit--ellie",
       "ellie-machete-combo-slash-3-hit--npc-m", 1.0f, 0.0f)} },
    { "ms-2h-1", 6, 0.03f, 0.75f, 2, 1,
     { CD_RAW_PAIR
      ("ellie-2hblunt-open-combo-1-hit--ellie",
       "ellie-2hblunt-open-combo-1-hit--npc-f", 1.0f, 0.0f)} },
    { "ms-2h-2", 6, 0.03f, 0.75f, 2, 1,
     { CD_RAW_PAIR
      ("ellie-2hblunt-open-combo-2-hit--ellie",
       "ellie-2hblunt-open-combo-2-hit--npc-f", 1.0f, 0.0f)} },
    { "ms-2h-3", 6, 0.03f, 0.75f, 2, 1,
     { CD_RAW_PAIR
      ("ellie-2hblunt-open-combo-3-hit--ellie",
       "ellie-2hblunt-open-combo-3-hit--npc-f", 1.0f, 0.0f)} },
    { "ms-knife-1", 6, 0.03f, 0.75f, 3, 1,
     { CD_RAW_PAIR
      ("ellie-knife-combo-slash-1-hit--ellie",
       "ellie-knife-combo-slash-1-hit--npc-m", 1.0f, 0.0f)} },
    { "ms-knife-2", 6, 0.03f, 0.75f, 3, 1,
     { CD_RAW_PAIR
      ("ellie-knife-combo-slash-2-hit--ellie",
       "ellie-knife-combo-slash-2-hit--npc-m", 1.0f, 0.0f)} },
    { "ms-knife-3", 6, 0.03f, 0.75f, 3, 1,
     { CD_RAW_PAIR
      ("ellie-knife-combo-slash-3-hit--ellie",
       "ellie-knife-combo-slash-3-hit--npc-m", 1.0f, 0.0f)} },
    // melee duel (melee_duel.c, kind 7): one row per SP -hit pair of every melee set, generated (tools/mv3/gen_duel_moves.py)
    MDU_CD_ROWS
        // melee duel finishers (kind 0 "mf-<SP move>": the lethal blow of a duel swing plays an SP player finisher, cd_fin_pick / cd_upg_go), generated (tools/mv3/gen_duel_fin.py)
        MDF_CD_ROWS MPN_CD_ROWS MPN_ESC_CD_ROWS
        // melee duel parry / clash (melee_duel_parry.c, kind 7): the SP parry pairs, generated (tools/mv3/gen_duel_parry.py)
        MDP_CD_ROWS
        // prone pull-out (melee_pullout.c, kind 8): SP's NPC-pulls-you-out-of-cover drag + the counter's success / fail pairs
    PUL_CD_ROWS
};

#define CD_N (sizeof CD_PAIRS / sizeof CD_PAIRS[0])
// "move~variant": the variants of one move differ only in their push (by distance); the base name is the move (CD_HITS, blade cues, no-repeat)
static size_t CdBaseLen(const char *n)
{
    const char *t = strchr(n, '~');
    return t ? (size_t) (t - n) : strlen(n);
}

static int CdSameMove(int a, int b)
{
    size_t la = CdBaseLen(CD_PAIRS[a].name);
    return la == CdBaseLen(CD_PAIRS[b].name)
        && !strncmp(CD_PAIRS[a].name, CD_PAIRS[b].name, la);
}

static int CdIsMove(const char *base, int p)
{
    size_t l = CdBaseLen(CD_PAIRS[p].name);
    return strlen(base) == l && !strncmp(base, CD_PAIRS[p].name, l);
}

// the first segment's clips = what psync_start is given; the bodies' authored start spots (carry limits) are read from them
#define CD_ATT0(p) (CD_PAIRS[p].seg[0].att)
#define CD_VIC0(p) (CD_PAIRS[p].seg[0].vic)
// a COMBO move: its first segment is the push (a "-hit" reaction pair), not the finisher's own intro / main clip
// (the push clips are the "-enter...-hit" pairs; the weapon finishers "kill-double-hit" / "...-single-hit-right" are open moves)
static int CdHasPush(int p)
{
    return CD_PAIRS[p].nseg > 1
        && strstr(CD_PAIRS[p].seg[0].att, "-enter") != NULL
        && strstr(CD_PAIRS[p].seg[0].att, "-hit") != NULL;
}

// SP's minimum defender distance from the surface frame: the fist combo-only finishers start from the contextual swing, whose opener needs
// wall-min-range 0.5 m (player-fisurgeonguild-contextual.bin); open finishers (fists' open ones, every knife one) have min 0.0 (the lambda's 5th argument)
// Most wall takedowns were throat punch anims: MP victims are mostly backed against the wall (< 0.5 m), where SP's opener window
// ruled the head smash out and left only the throat punch. The head smash's close push is authored with the victim 0.50 m out, within the push carry
// limit from the wall itself, so it may start from 0 m (t2r-cd-headsmash-dmin.txt = the old 0.5 m)
static int CdIsMove(const char *base, int p);
static float CdDmin(int p)
{
    if (CdIsMove("wall-combo-head-smash", p)
        && GetFileAttributesA("t2r-cd-headsmash-dmin.txt") ==
        INVALID_FILE_ATTRIBUTES)
        return 0.0f;
    return CD_PAIRS[p].style == -1 && CdHasPush(p) ? 0.5f : 0.0f;
}

// v5-hatch: SP's hatchet disarms are STREAMED clips in anim-melee-abby-t2 (a header stub + chunks in
// animstream97/*.stm: animate_ plays them as the bind pose). The data build makes them resident in place (tools/features/unstream_anims.py,
// build_dll.stage_unstream_disarms). get-animation-duration reads the stub's total length, so it cannot tell the two apart: the build renames one
// now-unused chunk ANIM to CD_UNSTREAM_MARKER, and a client without that pak (an ext-only update on the old data) treats these clips as NOT loaded
// (the borrowed 1h-blunt rows play for a hatchet, as before). t2r-duel-no-hatchet-dis.txt = never use them.
#define CD_UNSTREAM_MARKER "t2r-unstreamed-disarms-1"
static const char *const CD_UNSTREAMED[] = {
    "abby-disarm-kill-1hblade-iron-rhino--abby",
        "abby-disarm-kill-1hblade-iron-rhino--npc-f",
    "abby-disarm-kill-1hblade-punch-n-hack--abby",
        "abby-disarm-kill-1hblade-punch-n-hack--npc-f",
};

static int CdUnstreamOk(void)
{
    static int ok = -1, logged = -1;
    static uint64_t chk;
    if (ok < 0 || GetTickCount64() - chk > 5000) {
        chk = GetTickCount64();
        ok = AnimLen(SID_PLAYER, CD_UNSTREAM_MARKER) > 0.2f
            && GetFileAttributesA("t2r-duel-no-hatchet-dis.txt") ==
            INVALID_FILE_ATTRIBUTES;
        if (ok != logged) {
            logged = ok;
            Xlog("ctxdown: SP hatchet disarms %s (marker %s %s)",
                 ok ?
                 "RESIDENT - the real hatchet rows play, the 1h-blunt borrow for a hatchet is off"
                 :
                 "not available - the borrowed 1h-blunt rows play for a hatchet",
                 CD_UNSTREAM_MARKER, AnimLen(SID_PLAYER,
                                              CD_UNSTREAM_MARKER) >
                 0.2f ? "found" :
                 "missing (the game's own anim-melee-abby-t2: data not updated)");
        }
    }
    return ok;
}

static int CdUnstreamed(const char *clip)
{
    for (size_t k = 0; k < sizeof CD_UNSTREAMED / sizeof *CD_UNSTREAMED;
         k++)
        if (!strcmp(CD_UNSTREAMED[k], clip))
            return 1;
    return 0;
}

// ps33: SP's third front disarm of a 1h-blunt holder, fist-1h-blunt-disarm-arm-break-finisher, has streamed -sync halves like the
// hatchet rows. tools/features/unstream_anims.py makes them resident in the same pak and renames a second chunk ANIM to CD_UNSTREAM_MARKER2. The data deployed
// before ps33 has MARKER 1 only: there the row counts as NOT loaded (the pick skips it, the other 1h-blunt disarms play as before). t2r-duel-no-armbreak-dis.txt
// = never use the row.
#define CD_UNSTREAM_MARKER2 "t2r-unstreamed-disarms-2"
static int CdUnstream2Ok(void)
{
    static int ok = -1, logged = -1;
    static uint64_t chk;
    if (ok < 0 || GetTickCount64() - chk > 5000) {
        chk = GetTickCount64();
        ok = AnimLen(SID_PLAYER, CD_UNSTREAM_MARKER2) > 0.2f
            && GetFileAttributesA("t2r-duel-no-armbreak-dis.txt") ==
            INVALID_FILE_ATTRIBUTES;
        if (ok != logged) {
            logged = ok;
            Xlog("ctxdown: SP 1h-blunt arm-break disarm %s (marker %s; ps33)", ok ? "RESIDENT - mf-fist-1h-blunt-disarm-arm-break-finisher joins the disarm pool" : "not loaded - the row is skipped (data build without the ps33 unstream list, or t2r-duel-no-armbreak-dis.txt)", CD_UNSTREAM_MARKER2);
        }
    }
    return ok;
}

static int CdUnstreamed2(const char *clip)
{
    return !strcmp(clip, "abby-disarm-kill-1hblunt-arm-break--abby")
        || !strcmp(clip, "abby-disarm-kill-1hblunt-arm-break--npc-f");
}

// The first play ever of
// mf-fist-1h-blade-disarm-back-hack-finisher (fists from behind on a tomahawk holder) froze BOTH games ~0.4 s into its SP intro
// abby-disarm-kill-1hblade-sneak-hack-intro--abby (no EXCEPTION, no dump, no WER event; its pak clips are byte-identical to the game's). Until it is
// reproduced and understood, the row counts as not loaded (the pick logs it as missing; the next finisher / the plain down blow takes the kill).
// t2r-duel-back-hack-dis.txt = allowed again.
static int CdRowHeldBack(int p)
{
    static int ok = -1;
    static uint64_t chk;
    if (strcmp
        (CD_PAIRS[p].name, "mf-fist-1h-blade-disarm-back-hack-finisher"))
        return 0;
    if (ok < 0 || GetTickCount64() - chk > 2000) {
        chk = GetTickCount64();
        ok = GetFileAttributesA("t2r-duel-back-hack-dis.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    return !ok;
}

static int CdPairLoaded(int p)
{                               // every clip of the chain is resident here
    if (CdRowHeldBack(p))
        return 0;
    for (int i = 0; i < CD_PAIRS[p].nseg; i++) {
        if (AnimLen(SID_PLAYER, CD_PAIRS[p].seg[i].att) <= 0
            || AnimLen(SID_PLAYER, CD_PAIRS[p].seg[i].vic) <= 0)
            return 0;
        if ((CdUnstreamed(CD_PAIRS[p].seg[i].att)
             || CdUnstreamed(CD_PAIRS[p].seg[i].vic))
            && !CdUnstreamOk())
            return 0;
        if ((CdUnstreamed2(CD_PAIRS[p].seg[i].att)
             || CdUnstreamed2(CD_PAIRS[p].seg[i].vic))
            && !CdUnstream2Ok())
            return 0;           // ps33
    }
    return 1;
}

// real-time length (s) of segment i (the shorter half / speed): when psync switches to the next one
static float CdSegLen(int p, int i)
{
    float a = AnimLen(SID_PLAYER, CD_PAIRS[p].seg[i].att), v =
        AnimLen(SID_PLAYER, CD_PAIRS[p].seg[i].vic), s =
        CD_PAIRS[p].seg[i].spd > 1.001f ? CD_PAIRS[p].seg[i].spd : 1.0f;
    float l = a < v ? a : v;
    if (CD_PAIRS[p].seg[i].cut > 0.0f && CD_PAIRS[p].seg[i].cut < l)
        l = CD_PAIRS[p].seg[i].cut;
    return l / s;
}

// the finisher's real seconds to the hand-off: "pelvis on the floor" + CD_SLIDE_EXTRA (the victim went to the downed pose too
// instantly - the slide plays a bit longer), never past the last segment's shorter half
#define CD_SLIDE_EXTRA 0.70f
static float CdFinLen(int p)
{
    if (CD_PAIRS[p].kind >= 3)
        return CD_PAIRS[p].end_s;       // dodge counter rows (dcounter.c): the hand-off is the row's end
    int l = CD_PAIRS[p].nseg - 1;
    float a = AnimLen(SID_PLAYER, CD_PAIRS[p].seg[l].att), v =
        AnimLen(SID_PLAYER, CD_PAIRS[p].seg[l].vic), m = a < v ? a : v;
    float want = CD_PAIRS[p].end_s + CD_SLIDE_EXTRA;
    if (m > 0.0f && want > m - 0.05f)
        want = m - 0.05f;
    return want >
        CD_PAIRS[p].end_s + 0.10f ? want : CD_PAIRS[p].end_s + 0.10f;
}

// the whole takedown in real seconds: the segments before the last one + the finisher's hand-off (cd_fin_len)
static float CdChainTotal(int p)
{
    float t = 0.0f;
    for (int i = 0; i + 1 < CD_PAIRS[p].nseg; i++)
        t += CdSegLen(p, i);
    return t + CdFinLen(p);
}

static int CdPairOfMove(uint64_t move)
{
    for (size_t i = 0; i < CD_N; i++)
        if (FNV64(CD_PAIRS[i].name) == move)
            return (int) i;
    return -1;
}

// paired.c: the segments of the takedown whose move id this is (the same on every client)
static int CdChainOf(uint64_t move, uint64_t *att, uint64_t *vic,
                       float *spd, float *cut, int max)
{
    int p = CdPairOfMove(move);
    if (p < 0)
        return 0;
    int n = CD_PAIRS[p].nseg < max ? CD_PAIRS[p].nseg : max;
    for (int i = 0; i < n; i++) {
        att[i] = FNV64(CD_PAIRS[p].seg[i].att);
        vic[i] = FNV64(CD_PAIRS[p].seg[i].vic);
        spd[i] = CD_PAIRS[p].seg[i].spd;
        cut[i] = CD_PAIRS[p].seg[i].cut;
    }
    return n;
}

// (A/B: 1.1 / 1.8 let knife-wall-kneed-gut-slice start with the victim carried 0.99 m off the wall and the attacker 1.49 m - a visible
// slide and a pose pop in the blend-in; SP only picks a move whose spots fit where the bodies already are)
#define CD_VIC_MOVE_MAX 0.85f   // the victim's authored start spot may be this far (xz) from where it stands (psync blends it in)
#define CD_ATT_MOVE_MAX 1.4f    // same for the attacker
// A case (A [152450], no takedown at all): wall 1.18 m, every scan "2 over the carry limits" -
// wall-combo-head-smash victim 0.60 m (close push, authored ~0.6 m out) / 0.64 m (~far push, ~1.8 m out): the victim stood in the hole BETWEEN the
// two push variants, so neither fit and the down hit was a plain knockdown. The variants of a push move must cover its whole SP window (0.5 .. 2.0 m):
// the push's own shove carries the pull, so a push move's best variant may start up to 0.75 m from where the victim stands (half the widest gap
// between adjacent variants, 1.2 m, + margin). Open finishers (no push) keep 0.6 m.
#define CD_VIC_MOVE_MAX_PUSH 1.0f
static float CdVicMax(int p)
{
    return CdHasPush(p) ? CD_VIC_MOVE_MAX_PUSH : CD_VIC_MOVE_MAX;
}

static int32_t g_cdUpgSlot = -1;
static uint64_t g_cdUpgW;     // an upgraded one-shot of ours runs on this slot (cd_upg_start): an ABORT sends its own down hit
static int g_cdRelOk;
static float g_cdRelClip[8];  // the clip's attacker spot at our release (feature_stuck_guard)
#define CD_PLAN_STALE 1500      // a plan older than CD_PLAN_LIFE is still used at the down hit if the bodies are where they were (<= this many frames)
#define CD_PLAN_LIFE 150        // frames a good plan stays valid (a 5-punch combo outlived 45; cd_try re-checks the victim's authored spot)

#define CD_NJOB 14              // replica rays: 5 chest-level (1.15 / 1.4 / 1.7 m centre, 1.3 m at +-0.35 m), knee, 6 vertical top rays
// Grass still got treated as low cover for contextual takedowns: the static mask 0x207 stops on tall grass
// ~0.45 m up, so a grass field reads as a low wall and the shape tests (foliage / clutter rejections) cannot catch every clump. The cover rays use
// winmm's shot mask instead (0x1b07: passes tall grass, stops on walls, cars, counters - the floor bake and the paired.c path gate use it
// for the same reason); the victim's own capsule stays ignored.
#define CD_MASK 0x1b07u
// the contextual takedown is a little more frequent: longer legal ranges, longer pushes (old -> new in re/melee-gap-1025.md):
#define CD_TARGET_REACH 2.5f    // was 2.0: the standing hostile the scan looks at may be this far in front
#define CD_TARGET_CONE 0.40f    // was 0.50 (60 deg): cos of the half angle of the cone in front of us
#define CD_ATT_ANGLE_MAX 55.0f  // was 45 (SP lambda): the attacker may stand this far off the surface frame facing
#define CD_RAYLEN 2.6f          // (SP's low-cover opener reaches 1.7 m (fists) / knife wall-dist-max 2.5 - the far pushes start a victim 1.2-2.0 m out)
static struct {
    int stage;                  // 0 idle, 1 replica surface rays out, 2 attacker-spot ray out
    uint64_t job[CD_NJOB], kick_frame;
    int slot, kind;
    float V[3], u[2], dist, surf_h, tk, tc, tt;
    float ref_dn;               // >= 0: a low hit at this distance whose top the coarse rays did not find - refine rays (stage 3) look right behind it
    int ref_coarse;
    float ref_cd, ref_cn[2];
    char ref_why[160];          // refine run on a LOW coarse cover (step fit): the coarse result to fall back on
    float fit_gap, fit_step_h;  // the frame was moved this far behind a low step's face onto the taller cover behind it (0 = no step), the step's top
    float pend_top, pend_fit, pend_step;
    int pend_kind;              // the candidate's surface (top, step fit) for the contact-gap log
    float plan_top, plan_fit, plan_step;
    int plan_kind;
    float rt[CD_NJOB];          // replica ray results (fraction of CD_RAYLEN / of the vertical span, -1 = miss)
    int pend_pair;
    float pend_ap[7], pend_sa[3], pend_me[3], pend_p0;
    int pend_src;               // candidate waiting for the attacker-spot ray
    int plan_ok;
    uint64_t plan_frame;
    int plan_slot, plan_pair, plan_src;
    float plan_V[3], plan_ap[7], plan_me[3], plan_p0;   // the last good plan
    float Vyaw;
    int Vyaw_ok;                // the scanned victim's facing (world yaw, deg)
    int last_pair;              // 1 + the pair of our last takedown (0 = none yet) (never twice in a row if another is eligible)
    uint32_t rng;
    char note[200];             // the last reason a scan found nothing: logged in one line at the down hit
    int eng_state;              // 0 no engine hit seen yet, 1 the engine probe has answered on a puppet
    int eng_calls, eng_hits;
    // victim side
    int vic_on;
    uint64_t vic_t0;
    const char *vic_name;
    int cam_fin;                // the animated-camera window (fin_cam_start) is ours
    int cam_xc;                 // the takedown's authored camera (ctxcam_tracks.h, xcam) is ours
    uint64_t cam_frame, cam_check, cam_other;
    float cam_end;
    int cam_role;               // when it opened / the fallback's inputs (xcam)
    int ended;
    uint32_t ended_uid;
    int solid_done;             // the solidity probe (stage 4) already passed for the candidate cd_replica_cands is being re-entered with
    float sol_dist, sol_face, sol_n[2];
    int sol_kind;
    char sol_why[160];
} g_cd;

static void CdCamEnd(const char *why)
{
    if (g_cd.cam_fin) {
        g_cd.cam_fin = 0;
        FinCamEnd(why);
    }
    if (g_cd.cam_xc) {
        g_cd.cam_xc = 0;
        if (strcmp(why, "contextual down hand-off")
            && strcmp(why, "psync over"))
            XcamStop(why);
    }                           // the hand-off / the end: the camera eases out on its own (xcam_start's duration)
}

static void CdBlendOutVictim(void)
{                               // downed_set: the lying pose takes over from the takedown's victim half
    if (g_ps.role == 2 && g_ps.kind == PS_CTXDOWN) {
        uint64_t r = PsyncStop(SID_PLAYER, g_ps.vic_sid, 0.6f);        // (a smoother blend into the downed pose; was 0.3 s)
        Xlog("ctxdown: downed - the victim half %s blended out over 0.6 s into the lying pose (stop-animating -> %llx)", PsyncName(g_ps.vic_sid), (unsigned long long) r);
    } else
        Xlog("ctxdown: downed - the victim half was already blended out by the hand-off");
}

static void CdDrain(void)
{
    for (int i = 0; i < CD_NJOB; i++)
        if (g_cd.job[i]) {
            RayT(g_cd.job[i]);
            g_cd.job[i] = 0;
        }
}

// ---------------------------------------------------------------------------------------------------- the target
// the standing hostile nearest in front of us within 2 m (the one a punch could be hitting)
static int CdTarget(const float *me, float *V)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    float fx = 2.0f * (me[4] * me[6] + me[7] * me[5]), fz =
        1.0f - 2.0f * (me[4] * me[4] + me[5] * me[5]);
    int best = -1;
    float bd = CD_TARGET_REACH;
    for (uint32_t s = 0; s < T2R_MAX_PEERS; s++) {
        uint8_t *peer = ((fn_peer) T2rPeer) (s);
        uint64_t h = peer ? *(uint64_t *) (peer + 0x60) : 0;
        if (!h || !peer[4] || peer[5] || peer[0x4d] || s == PsyncSelf()
            || IsTeammate(s) || g_peerDowned[s] || g_puppetDeadHere[s])
            continue;
        float b[16];
        const float *l = PuppetLocator(h, b);
        if (!l || !PsLocOk(l))
            continue;
        float dx = l[0] - me[0], dz = l[2] - me[2], dy = l[1] - me[1], d =
            __builtin_sqrtf(dx * dx + dz * dz);
        if (d > bd || d < 0.2f || dy > 1.2f || dy < -1.2f)
            continue;
        if ((dx * fx + dz * fz) / d < CD_TARGET_CONE)
            continue;           // within ~66 deg of where we face (was 60)
        best = (int) s;
        bd = d;
        V[0] = l[0];
        V[1] = l[1];
        V[2] = l[2];
    }
    return best;
}

// melee-direction-check(attacker, defender, front, ...) of melee-script-funcs-impl.bin: the attacker is in the defender's front half plane
// (melee-direction-check-locator: is-in-front? 180 deg for the 'front' bit). vl = the victim's locator.
static int CdFrontOk(const float *me, const float *vl)
{
    float fx = 2.0f * (vl[4] * vl[6] + vl[7] * vl[5]), fz =
        1.0f - 2.0f * (vl[4] * vl[4] + vl[5] * vl[5]);
    return fx * (me[0] - vl[0]) + fz * (me[2] - vl[2]) > 0.0f;
}

// ---------------------------------------------------------------------------------------------------- the engine's probe
// IsAgainstWallForMelee (debug string in the exe: "bool IsAgainstWallForMelee(const MeleeProbeResult *, float, float, CustomStartFuncData *, int,
// MeleeProbeResult::EWallDir, bool, bool *, bool *, bool)", melee-util.cpp). The DC native against-wall-relative? (0x140e69e70) is a thin wrapper:
// sector = EWallDir of the direction (0x140e8d580), then this with min 0 and refine 1; the engine's own player-target scan (0x1406e1470) makes
// the same call on [target npc + 0x3108]->vfunc(0x1d0)() with distance 1.5. The refined bound frame lands in cache + 0x70 (index 0).
typedef uint8_t(*cd_fn_wall) (void *probes, float dist, float dmin,
                              void *cache, int idx, int sector,
                              uint8_t low, uint8_t * o_hi, uint8_t * o_lo,
                              uint8_t refine);
typedef int (*cd_fn_sector)(const void *dir, const void *a, const void *b,
                            float off);
typedef void *(*cd_fn_asnpc)(void *);
typedef const float *(*cd_fn_bfloc)(const void *bf, float *buf);
static int CdEngineOk(void)
{                               // the exe build is the one the addresses were read from (finisher-camera hook does the same check)
    static int ok = -1;
    if (ok >= 0)
        return ok;
    uint8_t *b = (uint8_t *) GetModuleHandleW(NULL);
    static const uint8_t e_wall[16] =
        { 0x48, 0x8b, 0xc4, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
0x41, 0x56, 0x41, 0x57, 0x48 };
    static const uint8_t e_sect[16] =
        { 0x4c, 0x8b, 0xdc, 0x53, 0x48, 0x81, 0xec, 0x90, 0x00, 0x00, 0x00,
0xc4, 0xc1, 0x78, 0x29, 0x73 };
    static const uint8_t e_npc[7] =
        { 0x33, 0xc0, 0x48, 0x85, 0xc9, 0x74, 0x25 };
    static const uint8_t e_bf[10] =
        { 0x48, 0x89, 0x5c, 0x24, 0x18, 0x57, 0x48, 0x83, 0xec, 0x50 };
    ok = !memcmp(b + 0xe91720, e_wall, 16)
        && !memcmp(b + 0xe8d580, e_sect, 16)
        && !memcmp(b + 0xda5c20, e_npc, 7)
        && !memcmp(b + 0x133db30, e_bf, 10);
    if (!ok)
        HOOK_FAIL
            ("ctxdown: exe build differs at the melee probe functions (0x140e91720 / 0x140e8d580 / 0x140da5c20) - engine probe off, rays only");
    return ok;
}

// the puppet's MeleeProbeResult, the way the engine's target scan finds it; NULL = not reachable
static uint8_t *CdProbesOf(uint8_t *pr, const char **why)
{
    uint8_t *b = (uint8_t *) GetModuleHandleW(NULL);
    *why = "ok";
    if (!pr || IsBadReadPtr(pr, 0x3110)) {
        *why = "puppet object unreadable";
        return NULL;
    }
    uint8_t *npc = (uint8_t *) ((cd_fn_asnpc) (b + 0xda5c20)) (pr);     // class cast: the object itself if it is an NPC
    if (!npc) {
        *why = "the puppet is not an NPC class object (0x140da5c20)";
        return NULL;
    }
    uint8_t *comp = *(uint8_t **) (npc + 0x3108);
    if (!comp || IsBadReadPtr(comp, 8)) {
        *why = "no component at npc+0x3108";
        return NULL;
    }
    uintptr_t *vt = *(uintptr_t **) comp;
    if (!vt || IsBadReadPtr(vt + 0x1d0 / 8, 8)
        || !IsCode((void *) vt[0x1d0 / 8])) {
        *why = "component vfunc 0x1d0 is not code";
        return NULL;
    }
    uint8_t *probes = ((uint8_t * (*)(void *)) vt[0x1d0 / 8]) (comp);
    if (!probes || IsBadReadPtr(probes, 0x940)) {
        *why = "vfunc 0x1d0 returned no probes";
        return NULL;
    }
    if (!*(uint64_t *) (probes + 0x140)) {
        *why = "the probes have no owner handle yet (never updated)";
        return NULL;
    }
    return probes;
}

// one probe call. out: the bf0 world locator (8 floats); returns 1 = the engine says the victim stands against that wall / cover
static int CdEngineProbe(uint8_t *probes, const float *dirv, float dmax,
                           int high, float *loc_out)
{
    uint8_t *b = (uint8_t *) GetModuleHandleW(NULL);
    __attribute__((aligned(16))) float dir[4] =
        { dirv[0], dirv[1], dirv[2], 0.0f };
    int sector =
        ((cd_fn_sector) (b + 0xe8d580)) (dir, probes + 0x10, probes + 0xe0,
                                         0.0f);
    if (sector < 0 || sector > 7)
        return 0;
    __attribute__((aligned(64))) uint8_t cache[0x180];
    memset(cache, 0, sizeof cache);
    uint8_t o = 0;
    uint8_t ok =
        ((cd_fn_wall) (b + 0xe91720)) (probes, dmax, 0.0f, cache, 0,
                                       sector, high ? 0 : 1,
                                       high ? &o : NULL, high ? NULL : &o,
                                       1);
    g_cd.eng_calls++;
    if (!(ok && o))
        return 0;
    const PsyncBf *bf = (const PsyncBf *) (cache + 0x70);
    if (bf->magic[0] != 0x7fccccccu || bf->magic[1] != 0x7fccccccu)
        return 0;
    __attribute__((aligned(16))) float tmp[16];
    const float *l = ((cd_fn_bfloc) (b + 0x133db30)) (bf, tmp);
    if (!l || !PsLocOk(l))
        return 0;
    memcpy(loc_out, l, 8 * sizeof(float));
    g_cd.eng_hits++;
    return 1;
}

static void CdProbeDump(int slot, uint8_t *probes, uint8_t *pr,
                          const float *dirv)
{                               // raw state of the victim's probes, once a second
    static uint64_t last;
    static int n;
    if (g_frame - last < 60 || n >= 80)
        return;
    last = g_frame;
    n++;
    uint8_t *b = (uint8_t *) GetModuleHandleW(NULL);
    __attribute__((aligned(16))) float dir[4] =
        { dirv[0], dirv[1], dirv[2], 0.0f };
    int sector =
        ((cd_fn_sector) (b + 0xe8d580)) (dir, probes + 0x10, probes + 0xe0,
                                         0.0f);
    float *d = (float *) (probes + 0x6f0);
    Xlog("ctxdown: probes of slot %d (npc %p, probes %p, owner handle %llx): sector %d, flag bytes +931..+933 %02x %02x %02x, wall distance per sector %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f", slot, (void *) pr, (void *) probes, (unsigned long long) *(uint64_t *) (probes + 0x140), sector, probes[0x931], probes[0x932], probes[0x933], d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
}

// ---------------------------------------------------------------------------------------------------- geometry of a candidate
// ap7 = the move's apReference. Authored spots = ap o C (C: each half's align at phase 0, read from the clips by psync). mv / da = how far the
// victim / we have to be carried to those spots (xz). 0 = the clips' channels are unreadable.
// p0 = a time into the first segment (s, clip time; the takedowns use 0: every push from its first frame). vyaw_out (optional) = the
// victim's authored facing (deg, world yaw) at that time.
static int CdOffsetAt(uint64_t ent, const char *clip, float p0, float *c)
{
    if (p0 <= 0.0005f)
        return PsyncOffset(ent, FNV64(clip), c);
    float L = AnimLen(SID_PLAYER, clip);
    if (L <= 0.0f)
        return 0;
    static const float id7[7] = { 0, 0, 0, 0, 0, 0, 1 };
    PsyncBf bf;
    PsyncBfMake(&bf, id7);
    return PsyncExpected(ent, FNV64(clip), &bf, p0 / L, c);
}

static int CdSpotsAt(int pair, uint64_t h, const float *ap7,
                       const float *V, const float *me, float p0,
                       float *mv, float *da, float *sa_out,
                       float *vyaw_out)
{
    float cv[8], ca[8], a8[8], Sv[8], sa[8];
    if (!CdOffsetAt(h, CD_VIC0(pair), p0, cv)
        || !CdOffsetAt(SID_PLAYER, CD_ATT0(pair), p0, ca))
        return 0;
    PsAp8(ap7, a8);
    PsCompose(a8, cv, Sv);
    PsCompose(a8, ca, sa);
    *mv =
        __builtin_sqrtf((Sv[0] - V[0]) * (Sv[0] - V[0]) +
                        (Sv[2] - V[2]) * (Sv[2] - V[2]));
    *da =
        __builtin_sqrtf((sa[0] - me[0]) * (sa[0] - me[0]) +
                        (sa[2] - me[2]) * (sa[2] - me[2]));
    sa_out[0] = sa[0];
    sa_out[1] = sa[1];
    sa_out[2] = sa[2];
    if (vyaw_out)
        *vyaw_out = Uc4Yaw(Sv);
    return 1;
}

static int CdSpots(int pair, uint64_t h, const float *ap7, const float *V,
                    const float *me, float *mv, float *da, float *sa_out)
{
    return CdSpotsAt(pair, h, ap7, V, me, 0.0f, mv, da, sa_out, NULL);
}

// The push animation played without moving the characters, and the start into the push did not feel right: SP keeps ONE world ap for the whole chain and plays the chosen push FROM ITS FIRST FRAME (E=28: enter / combo / intro / finisher all on
// the wall frame within 1 cm, the attacker's 0.83 m start error removed in 0.20 s, quadratic). The distance is matched by WHICH push plays: every move with
// a push has variants by the victim's authored start distance ("move~far" rows); cd_pick keeps, per move, the variant whose authored start spots are nearest
// the bodies (SP's contextual-swing ranking "wall dist / ideal dist"), and psync's pull removes the rest. The chain never starts into a clip any more.
static float CdVariantScore(float mv, float da)
{
    return mv + 0.5f * da;
}

static float CdWrap180(float a)
{
    while (a > 180.0f)
        a -= 360.0f;
    while (a < -180.0f)
        a += 360.0f;
    return a;
}

static float CdApYaw(const float *ap7)
{
    return 2.0f * __builtin_atan2f(ap7[4], ap7[6]) * 57.29578f;
}

// candidates -> pick -> the attacker-spot ray
static void CdNote(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_cd.note, sizeof g_cd.note, fmt, ap);
    va_end(ap);
}

static uint64_t g_cdPlanSteadyFrame; // last frame a plan streak stood at 4+ (a pair running mid-combo resets the streak, the wall did not move)
static int g_cdPlanStreak;    // consecutive plan-ready scans (a scan that finds no surface resets it): melee_duel_pin.c needs a steady wall, not a tree that flickers
static void CdScanFail(void)
{
    g_cdPlanStreak = 0;
    if (g_cd.plan_slot != g_cd.slot)
        g_cd.plan_ok = 0;
    g_cdRot = g_cdHeadon > 0 ? 0 : (g_cdRot + 1) % 5;
}                               // (cone search: the next scan casts along the next direction)

static struct {
    int pair;
    float ap[7], dw, fit, top;
} g_cd_cand[CD_N];              // fit = the frame is on the cover behind a step (m behind the step's face), top = the surface top used
static int g_cdNcand;
static void CdCandAdd(int pair, const float *ap7, float dw)
{
    if (g_cdNcand >= (int) CD_N)
        return;
    g_cd_cand[g_cdNcand].pair = pair;
    memcpy(g_cd_cand[g_cdNcand].ap, ap7, 28);
    g_cd_cand[g_cdNcand].dw = dw;
    g_cd_cand[g_cdNcand].fit = 0.0f;
    g_cd_cand[g_cdNcand].top = -1.0f;
    g_cdNcand++;
}

static int CdTopContact(int p);
struct cd_alt {
    int pair;
    float ap[7], sa[3], p0, top, fit;
};
static struct cd_alt g_cdAlt[CD_N];
static int g_cd_nalt, g_cd_alt_i, g_cd_alt_src;
static float g_cd_alt_step, g_cd_spot_a[3], g_cd_spot_b[3];
static int CdKickAlt(uint64_t h, int first);
static uint32_t CdRand(void)
{
    if (!g_cd.rng)
        g_cd.rng = (uint32_t) GetTickCount64() * 2654435761u + 0x9e3779b9u;
    g_cd.rng ^= g_cd.rng << 13;
    g_cd.rng ^= g_cd.rng >> 17;
    g_cd.rng ^= g_cd.rng << 5;
    return g_cd.rng;
}

// the plan the attacker-spot check approved (t = the ray's fraction, -1 = no ray was needed)
// ps35 (seven scans in a row read ONE wall behind slot 1 - 1.34, 1.24, 0.99, 0.91, 0.81, 0.62, 0.35 m, normal 2-10 deg, 4 of 5
// chest rays - while the victim was driven toward it, but one scan in between ([16603] 'chest height only') reset the streak: 'PIN not started: surface not
// steady (plan streak 2) - a tree / foliage' at [16595], no wall push in the whole fight). Two plans in a row (<= 20 frames apart) whose surface frames lie on
// one plane (normals within CD_AGREE_DEG, the second ap within CD_AGREE_M of the first one's plane) are a wall, not a trunk: melee_duel_pin.c mpn_steady takes
// that for CD_AGREE_LIFE frames. t2r-pin-steady-ps34.txt = the 4-scan streak only.
#define CD_AGREE_DEG 10.0f      // [TUNING ps35] (the run's own spread of one wall's normal: 2-10 deg)
#define CD_AGREE_M 0.15f        // [TUNING ps35]
#define CD_AGREE_LIFE 30        // [TUNING ps35] frames
static uint64_t g_cdPlanAgreeFrame;
static void CdPlanCommit(float t)
{
    if (g_cd.plan_ok && g_cd.plan_slot == g_cd.slot
        && g_frame - g_cd.plan_frame <= 20
        && g_cd.plan_kind == g_cd.pend_kind && (g_cd.pend_kind == 1
                                                || g_cd.pend_kind == 2)) {
        float y0 = CdApYaw(g_cd.plan_ap), y1 =
            CdApYaw(g_cd.pend_ap), dyw = CdWrap180(y1 - y0), yr =
            y0 * 0.0174533f;
        float off =
            (g_cd.pend_ap[0] - g_cd.plan_ap[0]) * __builtin_sinf(yr) +
            (g_cd.pend_ap[2] - g_cd.plan_ap[2]) * __builtin_cosf(yr);
        if (__builtin_fabsf(dyw) <= CD_AGREE_DEG
            && __builtin_fabsf(off) <= CD_AGREE_M) {
            if (!g_cdPlanAgreeFrame
                || g_frame - g_cdPlanAgreeFrame > CD_AGREE_LIFE) {
                static int nla;
                if (LOGCAP(nla, 60))
                    Xlog("ctxdown: two plans in a row on one plane (normals %.0f deg apart, %.2f m off the first plane, %d frames apart) - a steady surface for the pin push (ps35)", __builtin_fabsf(dyw), __builtin_fabsf(off), (int) (g_frame - g_cd.plan_frame));
            }
            g_cdPlanAgreeFrame = g_frame;
        }
    }
    g_cdPlanStreak++;
    if (g_cdPlanStreak >= 4)
        g_cdPlanSteadyFrame = g_frame;
    g_cd.plan_ok = 1;
    g_cd.plan_frame = g_frame;
    g_cd.plan_slot = g_cd.slot;
    g_cd.plan_pair = g_cd.pend_pair;
    g_cd.plan_src = g_cd.pend_src;
    g_cd.plan_p0 = g_cd.pend_p0;
    memcpy(g_cd.plan_V, g_cd.V, sizeof g_cd.plan_V);
    memcpy(g_cd.plan_ap, g_cd.pend_ap, sizeof g_cd.plan_ap);
    memcpy(g_cd.plan_me, g_cd.pend_me, sizeof g_cd.plan_me);
    g_cd.plan_kind = g_cd.pend_kind;
    g_cd.plan_top = g_cd.pend_top;
    g_cd.plan_fit = g_cd.pend_fit;
    g_cd.plan_step = g_cd.pend_step;
    g_cd_plan_ang = g_cd_cand_ang;
    static int nl;
    if (LOGCAP(nl, 40))
        Xlog("ctxdown: plan ready slot %d %s (spot ray %.2f, src %s, start %.2f s in)", g_cd.slot, CD_PAIRS[g_cd.plan_pair].name, t, g_cd.plan_src ? "rays" : "engine", g_cd.plan_p0);
}

// returns 1 = a candidate was chosen (its attacker-spot ray kicked: stage 2, or the plan committed at once)
// RANDOM among the eligible pairs (weights in CD_PAIRS), never the pair of our last takedown if another is eligible: the nearest-carry rule
// always chose the same pair on the same spot
static int CdPick(uint64_t h, const float *V, const float *me, int src)
{
    static uint64_t lastlog;
    int verbose = g_frame - lastlog >= 60;
    if (verbose)
        lastlog = g_frame;
    int el[CD_N], nel = 0;
    float emv[CD_N], eda[CD_N], esa[CD_N][3], ep0[CD_N];
    int n_ang = 0, n_turn = 0, n_carry = 0;
    int any_facing = GetFileAttributesA("t2r-cd-sp-facing.txt") == INVALID_FILE_ATTRIBUTES;     // any victim facing (MP victims move freely)
    for (int i = 0; i < g_cdNcand; i++) {
        float mv, da, sa[3], vyaw = 0.0f;
        int p = g_cd_cand[i].pair;
        const float *ap = g_cd_cand[i].ap;
        float p0 = 0.0f;        // (every push from its first frame, see cd_variant_score)
        if (!CdSpotsAt(p, h, ap, V, me, p0, &mv, &da, sa, &vyaw)) {
            static int nl;
            if (LOGCAP(nl, 6))
                Xlog("ctxdown: fallback - %s: the clips' align channels are unreadable (no authored spots)", CD_PAIRS[p].name);
            continue;
        }
        // SP's start lambda (finishers.bin, every wall / low-cover finisher): (< (- 180 (angle-in-between-locators bf0 (rotate-locator-lookat attacker bf0))) 45):
        // the attacker stands within 45 deg of the surface frame's facing (out of the wall), seen from the frame
        float ay = CdApYaw(ap) * 0.0174533f, fx =
            __builtin_sinf(ay), fz = __builtin_cosf(ay);
        float vx = me[0] - ap[0], vz = me[2] - ap[2], vl =
            __builtin_sqrtf(vx * vx + vz * vz);
        float ang =
            vl >
            0.05f ?
            __builtin_acosf(fmaxf
                            (-1.0f,
                             fminf(1.0f,
                                   (vx * fx +
                                    vz * fz) / vl))) * 57.29578f : 0.0f;
        // the victim's needed turn onto its authored facing: SP only starts with the attacker in the victim's front half (melee-direction-check front),
        // so the victim half never has to swing round more than 90 deg (A [63884]: a 168 deg turn in the 0.2 s pull = a spin / teleport)
        float turn =
            g_cd.
            Vyaw_ok ? __builtin_fabsf(CdWrap180(vyaw - g_cd.Vyaw)) : 0.0f;
        if (verbose)
            Xlog("ctxdown: %s %s: wall %.2f m (SP window %.2f .. %.2f), bf0 %.2f %.2f %.2f yaw %.0f, attacker %.0f deg off its facing (SP max 45, ours %.0f); start %.2f s into %s; " "authored spots: victim %.2f m (max %.2f) turn %.0f deg (max 90), attacker %.2f m (max %.2f)", src ? "rays" : "engine", CD_PAIRS[p].name, g_cd_cand[i].dw, CdDmin(p), CD_PAIRS[p].dmax, ap[0], ap[1], ap[2], CdApYaw(ap), ang, (double) CD_ATT_ANGLE_MAX, p0, CD_ATT0(p), mv, CdVicMax(p), turn, da, CD_ATT_MOVE_MAX);
        if (ang > CD_ATT_ANGLE_MAX) {
            n_ang++;
            continue;
        }
        if (!any_facing && turn > 90.0f) {
            n_turn++;
            continue;
        }
        if (mv > CdVicMax(p) || da > CD_ATT_MOVE_MAX) {
            n_carry++;
            continue;
        }
        el[nel] = i;
        emv[nel] = mv;
        eda[nel] = da;
        memcpy(esa[nel], sa, sizeof sa);
        ep0[nel] = p0;
        nel++;
    }
    if (!nel) {
        CdNote
            ("slot %d: %d pair(s) fit the surface; refused: %d attacker > 45 deg off the frame, %d victim turn > 90 deg, %d over the carry limits (victim %.2f m open / %.2f m push, attacker %.1f m)",
             g_cd.slot, g_cdNcand, n_ang, n_turn, n_carry,
             CD_VIC_MOVE_MAX, CD_VIC_MOVE_MAX_PUSH, CD_ATT_MOVE_MAX);
        CdScanFail();
        return 0;
    }
    // per move, only its push variant whose authored start fits the bodies best (cd_variant_score)
    int keep[CD_N], nmv = 0;
    for (int k = 0; k < nel; k++) {
        keep[k] = 1;
        for (int k2 = 0; k2 < nel; k2++) {
            if (k2 == k
                || !CdSameMove(g_cd_cand[el[k]].pair,
                                 g_cd_cand[el[k2]].pair))
                continue;
            float s1 = CdVariantScore(emv[k], eda[k]), s2 =
                CdVariantScore(emv[k2], eda[k2]);
            if (s2 < s1 || (s2 == s1 && k2 < k)) {
                keep[k] = 0;
                break;
            }
        }
        nmv += keep[k];
    }
    // RANDOM among the eligible moves (the pool is only the listed takedowns), never the move of our last takedown while another is eligible
    // the wall head smash is the signature fist wall takedown - weight 3 against 1 for every other eligible move (it is still not repeated
    // back to back while another move fits)
    int best = 0, n_ok = 0, pick_n = 0;
#define CD_LASTMOVE(k) (g_cd.last_pair > 0 && CdSameMove(g_cd_cand[el[k]].pair, g_cd.last_pair - 1))
#define CD_WEIGHT(k) (CdIsMove("wall-combo-head-smash", g_cd_cand[el[k]].pair) ? 3 : 1)
    for (int k = 0; k < nel; k++)
        if (keep[k] && !(nmv > 1 && CD_LASTMOVE(k)))
            n_ok += CD_WEIGHT(k);
    int roll = n_ok > 0 ? (int) (CdRand() % (uint32_t) n_ok) : 0;
    for (int k = 0; k < nel; k++) {
        if (!keep[k] || (nmv > 1 && CD_LASTMOVE(k)))
            continue;
        best = k;
        pick_n += CD_WEIGHT(k);
        if (pick_n > roll)
            break;
    }
#undef CD_WEIGHT
#undef CD_LASTMOVE
    int p = g_cd_cand[el[best]].pair;
    // the other eligible variants, nearest attacker carry first, are the fallbacks when the spot path of the chosen one is blocked
    g_cd_nalt = 0;
    {
        int used[CD_N] = { 0 }, cur = best;
        for (;;) {
            g_cdAlt[g_cd_nalt].pair = g_cd_cand[el[cur]].pair;
            memcpy(g_cdAlt[g_cd_nalt].ap, g_cd_cand[el[cur]].ap, 28);
            memcpy(g_cdAlt[g_cd_nalt].sa, esa[cur], 12);
            g_cdAlt[g_cd_nalt].p0 = ep0[cur];
            g_cdAlt[g_cd_nalt].top = src ? g_cd_cand[el[cur]].top : -1.0f;
            g_cdAlt[g_cd_nalt].fit = src ? g_cd_cand[el[cur]].fit : 0.0f;
            g_cd_nalt++;
            used[cur] = 1;
            int nx = -1;
            for (int k = 0; k < nel; k++)
                if (!used[k] && (nx < 0 || eda[k] < eda[nx]))
                    nx = k;
            if (nx < 0 || g_cd_nalt >= (int) CD_N)
                break;
            cur = nx;
        }
    }
    g_cd_alt_i = 0;
    g_cd_alt_src = src;
    g_cd_alt_step = src ? g_cd.fit_step_h : -1.0f;
    memcpy(g_cd.pend_me, me, sizeof g_cd.pend_me);
    if (verbose)
        Xlog("ctxdown: pick %s of %d eligible move(s) (%d pair variants; random; last was %s; push from its first frame, victim carried %.2f m, attacker %.2f m, src %s)", CD_PAIRS[p].name, nmv, nel, g_cd.last_pair > 0 ? CD_PAIRS[g_cd.last_pair - 1].name : "none", emv[best], eda[best], src ? "rays" : "engine");
    // the attacker's authored spot must be reachable from where WE stand: a chest-height ray from us to that spot (A/B: a ray from the
    // victim's chest hit right next to the victim - a hood, the wall - and rejected good cover plans)
    return CdKickAlt(h, 1);
}

// arms alternative g_cd_alt_i (its spot ray, or the commit); 0 = none left
static int CdKickAlt(uint64_t h, int first)
{
    (void) first;
    if (g_cd_alt_i >= g_cd_nalt) {
        CdScanFail();
        return 0;
    }
    const struct cd_alt *A = &g_cdAlt[g_cd_alt_i];
    const float *me = g_cd.pend_me;
    g_cd.pend_pair = A->pair;
    memcpy(g_cd.pend_ap, A->ap, 28);
    memcpy(g_cd.pend_sa, A->sa, sizeof g_cd.pend_sa);
    g_cd.pend_src = g_cd_alt_src;
    g_cd.pend_p0 = A->p0;
    g_cd.pend_kind = CD_PAIRS[A->pair].kind;
    g_cd.pend_top = A->top;
    g_cd.pend_fit = A->fit;
    g_cd.pend_step = g_cd_alt_step;
    float dxs = A->sa[0] - me[0], dzs = A->sa[2] - me[2];
    if (dxs * dxs + dzs * dzs < 0.2f * 0.2f) {
        CdPlanCommit(-1.0f);
        return 1;
    }
    // the victim's own body is ignored (its capsule is in the static mask near t = 0) and the ray stops 0.15 m short of the spot (the cover's face / the
    // victim's side is where the spot lies)
    float len = __builtin_sqrtf(dxs * dxs + dzs * dzs), k =
        len > 0.3f ? (len - 0.15f) / len : 1.0f;
    float a[3] = { me[0], me[1] + 1.1f, me[2] }, bb[3] =
        { me[0] + dxs * k, me[1] + 1.1f, me[2] + dzs * k };
    memcpy(g_cd_spot_a, a, 12);
    memcpy(g_cd_spot_b, bb, 12);
    g_cd.job[0] = WorldRayKickM(a, bb, h, CD_MASK);
    g_cd.kick_frame = g_frame;
    g_cd.stage = g_cd.job[0] ? 2 : 0;
    if (!g_cd.job[0])
        CdScanFail();
    return g_cd.stage == 2;
}

// ---------------------------------------------------------------------------------------------------- the ray replica of the wall frame
// Static-world rays (mask 0x207, results read next frame) from the victim's feet position along u (attacker -> victim), CD_RAYLEN m long:
//   0..2 chest-level at 1.15 / 1.4 / 1.7 m up (above any low cover: a hood / counter / low wall tops out near 1.05 m), 3 / 4 the same
//   kind at 1.3 m up, +-0.35 m to the sides (they give the surface's real normal), 5 knee height (0.45 m), 6..8 vertical rays (1.25 m -> 0.25 m
//   up) 0.35 / 0.7 / 1.05 m along u: the top of a low surface.
// WALL = at least TWO of the five chest-level rays hit within CD_RAYLEN (A/B: the single chest ray at 1.2 m missed on a full-height wall
// and the scan called it COVER - one ray through a gap / foliage is not a verdict any more). COVER = none of them hit, the knee ray hits, and the
// first vertical ray BEHIND the knee hit finds a top 0.55 .. 1.05 m above the feet (the old fixed 0.75 m ray went past a thin wall and read the
// terrain behind it). One chest-level hit alone = ambiguous (a pole, a branch): no surface.
// The frame: on the surface at the victim's xz projection, facing out of it, at the victim's feet height. The facing is the surface normal when
// both side rays hit (the attacker no longer has to stand square to the wall - the frame yaw varied with the approach angle in the A/B logs
// -91 / 162 / -105 on one car), else -u. That convention is the engine's own facing test (re/tlou2-env-melee.md section 5); the authored victim
// spots in the A/B logs agree with it (0.75 m out of the ap for the cover face smash: victim carry = |0.75 - wall distance| in every sample).
static const float CD_RAY_H[5] =
    { 1.15f, 1.4f, 1.7f, 1.3f, 1.3f }, CD_RAY_LAT[5] =
    { 0.0f, 0.0f, 0.0f, 0.35f, -0.35f };
// Ledge executions were inconsistent - a wall played where a ledge should: the logs had 784 scans "knee ray 1.30 m, no top found
// behind it" (the 3 top rays stopped at 1.05 m) and railings read as WALL (their top rail hits the 1.15 m ray, the side rays hit posts). The
// top rays now reach 2.1 m out and start at 1.45 m (tops up to 1.25 m: railings); WALL needs a hit of the 1.4 / 1.7 m centre rays (a real
// full-height surface), else a top found behind the nearest hit makes it COVER / LEDGE.
#define CD_NTOP 6
static const float CD_TOP_D[CD_NTOP] =
    { 0.35f, 0.7f, 1.05f, 1.4f, 1.75f, 2.1f };
#define CD_TOP_Y0 1.75f
#define CD_TOP_SPAN 2.35f       // down to 0.6 m below the feet (a drop behind a ledge reads as no hit)
// SP's own detection (re/tlou2-env-melee.md 4): the engine's player-target scan asks IsAgainstWallForMelee with 1.5 m (exe 0x1406e15a0) - a victim farther than that
// from its surface is not "against" anything for SP's contextual melee; the finishers' own wall-dist-max (up to 2.5) only widens the SECOND test. SP's real probe (nav-mesh
// boundary edges + collision casts, MeleeProbe) cannot run in MP: the maps have no nav mesh / cover database (spawn-system.md) and the puppets are no NPC class objects.
// So the replica applies SP's rules: solid knee-height face, plane, <= 1.5 m. t2r-cd-no-sp-face.txt = the older looser verdicts.
#define CD_SP_DETECT 1.5f
static int g_cdSpFaceOff = -1;
static int CdSpFaceOff(void)
{
    if (g_cdSpFaceOff < 0) {
        FILE *f = ExeSideOpen("t2r-cd-no-sp-face.txt", "r");
        g_cdSpFaceOff = f != NULL;
        if (f)
            fclose(f);
        if (g_cdSpFaceOff)
            Xlog("ctxdown: SP-like face rules OFF (t2r-cd-no-sp-face.txt)");
    }
    return g_cdSpFaceOff;
}

// out: kind (1 wall, 2 cover, 0 none), perpendicular distance, outward normal (xz), why (note text)
// Players can stand ~30 degrees angled to a wall instead of perpendicular, and SP turns both onto the surface: the surface normal
// from two hits along u at lateral offsets la / lb (s = (-u1, u0)); 1 = n set (facing the attacker, within 70 deg of -u), 0 = keep -u.
static int CdFaceN(float da, float la, float db, float lb, float u0,
                     float u1, float n_out[2])
{
    if (da < 0.0f || db < 0.0f || __builtin_fabsf(la - lb) < 0.2f)
        return 0;
    float dd = da - db, dl = la - lb;   // tangent = u * dd + s * dl
    if (__builtin_fabsf(dd) > 1.5f * __builtin_fabsf(dl))
        return 0;
    float tx = u0 * dd + (-u1) * dl, tz = u1 * dd + u0 * dl, tl =
        __builtin_sqrtf(tx * tx + tz * tz);
    if (tl < 0.1f)
        return 0;
    float nx = tz / tl, nz = -tx / tl;
    if (nx * (-u0) + nz * (-u1) < 0.0f) {
        nx = -nx;
        nz = -nz;
    }
    if (nx * (-u0) + nz * (-u1) < 0.34f)
        return 0;
    n_out[0] = nx;
    n_out[1] = nz;
    return 1;
}

// the face normal from the coarse scan's knee rays (centre g_cd.rt[5], +-0.35 m g_cd.rt[12] / [13]) for the refine verdicts (wall with a lip, step fit)
static void CdKneeN(float n_out[2])
{
    const float *rt = g_cd.rt;
    float u0 = g_cd.u[0], u1 = g_cd.u[1];
    float kc = rt[5] >= 0.0f
        && rt[5] < 1.0f ? rt[5] * CD_RAYLEN : -1.0f, kl = rt[12] >= 0.0f
        && rt[12] < 1.0f ? rt[12] * CD_RAYLEN : -1.0f, kr = rt[13] >= 0.0f
        && rt[13] < 1.0f ? rt[13] * CD_RAYLEN : -1.0f;
    n_out[0] = -u0;
    n_out[1] = -u1;
    if (!CdFaceN(kl, 0.35f, kr, -0.35f, u0, u1, n_out)
        && !CdFaceN(kl, 0.35f, kc, 0.0f, u0, u1, n_out))
        CdFaceN(kc, 0.0f, kr, -0.35f, u0, u1, n_out);
}

// a knee-height face from the centre knee hit dk and the side knee rays (+-0.35 m, fractions of CD_RAYLEN): one side within 0.45 m of the centre, or (a victim standing 50+ deg to a flat half wall read 0.70 m centre, 1.15 / 0.25 m sides: a perfectly straight face, rejected
// by the 0.45 m test) all three on one straight line no steeper than cd_face_n's limit. Foliage / clutter scatters and fails both.
static int CdKneeFaceOk(float dk, float s0, float s1)
{
    float a = s0 >= 0.0f
        && s0 < 1.0f ? s0 * CD_RAYLEN : -1.0f, b = s1 >= 0.0f
        && s1 < 1.0f ? s1 * CD_RAYLEN : -1.0f;
    if ((a >= 0.0f && __builtin_fabsf(a - dk) <= 0.45f)
        || (b >= 0.0f && __builtin_fabsf(b - dk) <= 0.45f))
        return 1;
    return a >= 0.0f && b >= 0.0f
        && __builtin_fabsf(a + b - 2.0f * dk) <= 0.15f
        && __builtin_fabsf(a - b) <= 1.5f * 0.7f;
}

static int CdClassify(float *dist, float n_out[2], char *why, size_t wn)
{
    const float *rt = g_cd.rt;
    float hit_d[5];
    int nh = 0;
    for (int i = 0; i < 5; i++) {
        hit_d[i] = (rt[i] >= 0.0f
                    && rt[i] < 1.0f) ? rt[i] * CD_RAYLEN : -1.0f;
        if (hit_d[i] >= 0.0f)
            nh++;
    }
    float u0 = g_cd.u[0], u1 = g_cd.u[1];
    n_out[0] = -u0;
    n_out[1] = -u1;
    *dist = 0;
    int tall = hit_d[2] >= 0.0f;        // a wall only when it rises above chest height (the 1.7 m ray); a car roof / counter / railing is a ledge
    // Pinning failed at a half wall (attacker side log: "wall 1.94 m behind the victim - beyond 1.5 m" with all 5 chest rays at 2.2 m): the chest
    // rays went OVER the half wall the victim stood at and hit a taller wall behind it. A knee ray hit clearly nearer than every chest hit = that low face is the surface.
    {
        float tk0 = rt[5], dk0 = (tk0 >= 0.0f
                                  && tk0 < 1.0f) ? tk0 * CD_RAYLEN : -1.0f,
            cmin = 99.0f;
        for (int i = 0; i < 5; i++)
            if (hit_d[i] >= 0.0f && hit_d[i] < cmin)
                cmin = hit_d[i];
        if (tall && dk0 >= 0.0f && dk0 + 0.3f < cmin) {
            static uint64_t nll;
            if (g_frame - nll > 30) {
                nll = g_frame;
                Xlog("ctxdown: low face %.2f m in front of a taller wall %.2f m - the low face is the surface (half wall / cover)", dk0, cmin);
            }
            tall = 0;
        }
    }
    if (nh >= 2 && tall && CdPlaneCheck()) {
        // WALL = one plane: the centre rays (parallel, 1.15 / 1.4 / 1.7 m up) read the same distance on a vertical surface whatever its angle, and
        // at least one side ray (+-0.35 m) lands within 0.45 m of it (a 50 deg slanted wall). Foliage / clumps / a pole scatter.
        float lo = 99.0f, hi = -1.0f, dcen = -1.0f;
        int nc = 0;
        for (int i = 0; i < 3; i++)
            if (hit_d[i] >= 0.0f) {
                nc++;
                if (hit_d[i] < lo)
                    lo = hit_d[i];
                if (hit_d[i] > hi)
                    hi = hit_d[i];
            }
        if (nc)
            dcen = 0.5f * (lo + hi);
        int sides = 0, sides_ok = 0;
        for (int i = 3; i < 5; i++)
            if (hit_d[i] >= 0.0f) {
                sides++;
                if (dcen >= 0.0f
                    && __builtin_fabsf(hit_d[i] - dcen) <= 0.45f)
                    sides_ok++;
            }
        if (nc < 2 || hi - lo > 0.15f || (dcen >= 0.0f && !sides_ok)) {
            static uint64_t nlw;
            if (g_frame - nlw > 30) {
                nlw = g_frame;
                Xlog("ctxdown: wall ignored - the rays do not form a surface (foliage / clutter?): centre %.2f %.2f %.2f, sides %.2f %.2f m", hit_d[0], hit_d[1], hit_d[2], hit_d[3], hit_d[4]);
            }
            snprintf(why, wn,
                     "no surface: scattered hits, not a plane (foliage?): centre %.2f %.2f %.2f, sides %.2f %.2f",
                     hit_d[0], hit_d[1], hit_d[2], hit_d[3], hit_d[4]);
            return 0;
        }
    }
    // ps44 T1 (rec43a wall-throat-punch u0104aabe: a crate at the wall's foot; A.log [20041] 'WALL 0.70 m behind the victim ... 5 of 5 chest rays hit (0.72 0.72 0.72
    // 0.43 0.56 m)', then 'travel clamp ... victim half (puppet) - low obstacle (knee height only) 0.24 m ahead'): the wall verdict never looked at knee height unless
    // the low face stood 0.3 m in front of EVERY chest hit. A wall pair's victim spot is authored ~0.26 m off the face ('victim root ... authored 0.11 0.24 (0.26 m
    // from the ap)'): a knee-height face (centre, or 0.35 m to a side) more than CD_WALL_LOWPROP_M nearer than the chest ray above it is a low prop standing where
    // the victim half is authored - no wall takedown from this scan (the cone search goes on). The knee distances are logged either way (the scan of [20041] printed
    // none: whether THIS rule would have refused that takedown is not known). t2r-cd-wall-lowprop-old.txt = old.
#define CD_WALL_LOWPROP_M 0.15f // [TUNING ps44] m a knee-height face may stand in front of the wall face above it (0.15 m hold limit)
    if (nh >= 2 && tall) {
        static int old44 = -1;
        static uint64_t chk44;
        if (old44 < 0 || GetTickCount64() - chk44 > 2000) {
            chk44 = GetTickCount64();
            old44 =
                GetFileAttributesA("t2r-cd-wall-lowprop-old.txt") !=
                INVALID_FILE_ATTRIBUTES;
        }
        float kd44[3] = { rt[5], rt[12], rt[13] }, cd44[3] =
            { hit_d[0] >= 0.0f ? hit_d[0] : hit_d[1], hit_d[3], hit_d[4] },
            worst44 = 0.0f;
        int wi44 = -1;
        for (int i = 0; i < 3; i++) {
            kd44[i] = kd44[i] >= 0.0f
                && kd44[i] < 1.0f ? kd44[i] * CD_RAYLEN : -1.0f;
            if (kd44[i] >= 0.0f && cd44[i] >= 0.0f
                && cd44[i] - kd44[i] > worst44) {
                worst44 = cd44[i] - kd44[i];
                wi44 = i;
            }
        }
        static int nl44;
        if (worst44 > 0.05f && LOGCAP(nl44, 40)) {
            Xlog("ctxdown: wall knee check - knee rays %.2f / %.2f / %.2f m (centre, +0.35, -0.35) under chest rays %.2f / %.2f / %.2f m: a low face stands %.2f m in front of the wall%s (ps44 T1, limit %.2f)", kd44[0], kd44[1], kd44[2], cd44[0], cd44[1], cd44[2], worst44, worst44 > CD_WALL_LOWPROP_M ? (old44 ? " - old: the takedown stays" : " - a low prop on the victim's authored spot: no wall takedown") : "", (double) CD_WALL_LOWPROP_M);
        }
        if (!old44 && worst44 > CD_WALL_LOWPROP_M) {
            snprintf(why, wn,
                     "no surface: a low prop %.2f m in front of the wall (knee ray %d %.2f m, chest %.2f m) stands on the victim's authored spot (ps44)",
                     worst44, wi44, wi44 >= 0 ? kd44[wi44] : -1.0f,
                     wi44 >= 0 ? cd44[wi44] : -1.0f);
            return 0;
        }
    }
    if (nh >= 2 && tall) {
        float dc =
            hit_d[0] >= 0.0f ? hit_d[0] : hit_d[1] >=
            0.0f ? hit_d[1] : hit_d[2] >= 0.0f ? hit_d[2] : -1.0f;
        if (dc < 0.0f)
            dc = (hit_d[3] >= 0.0f
                  && hit_d[4] >=
                  0.0f) ? 0.5f * (hit_d[3] + hit_d[4]) : hit_d[3] >=
                0.0f ? hit_d[3] : hit_d[4];
        // the normal of the face: both side rays (0.7 m apart), else the centre and the one side ray that hit
        if (!CdFaceN(hit_d[3], 0.35f, hit_d[4], -0.35f, u0, u1, n_out)
            && dc >= 0.0f)
            if (!CdFaceN(hit_d[3], 0.35f, dc, 0.0f, u0, u1, n_out))
                CdFaceN(dc, 0.0f, hit_d[4], -0.35f, u0, u1, n_out);
        float perp = dc * (u0 * -n_out[0] + u1 * -n_out[1]);
        *dist = perp > 0.0f ? perp : dc;
        snprintf(why, wn,
                 "wall: %d of 5 chest rays hit (%.2f %.2f %.2f %.2f %.2f m)",
                 nh, hit_d[0], hit_d[1], hit_d[2], hit_d[3], hit_d[4]);
        return 1;
    }
    float tk = rt[5], dk = (tk >= 0.0f
                            && tk < 1.0f) ? tk * CD_RAYLEN : -1.0f;
    float dn = dk;              // the nearest low hit: the knee ray or a low chest ray (a railing's top rail)
    for (int i = 0; i < 5; i++)
        if (hit_d[i] >= 0.0f && (dn < 0.0f || hit_d[i] < dn))
            dn = hit_d[i];
    // (low-cover pushing over people who just have vines or grass behind them, or a huge space between them and actual cover; A/B logs: "ledge:
    // nearest 1.95 m (knee -1.00, 1 chest hits), beyond it the floor" = ONE chest-height ray hit (a vine / branch / thin prop, no knee ray hit at all) read as a ledge
    // up to 2 m out). SP's probe only ever reports a solid face (nav-mesh boundary edge with a collision hit at crouch height). A ledge / cover therefore needs the
    // KNEE ray (0.45 m up) to hit the face, and that face to be a plane (a knee ray 0.35 m to a side lands within 0.3 m of it).
    if (dn >= 0.0f && CdPlaneCheck() && !CdSpFaceOff()) {
        if (dk < 0.0f) {
            static uint64_t nlk;
            if (g_frame - nlk > 30) {
                nlk = g_frame;
                Xlog("ctxdown: ledge / cover ignored - nothing at knee height (%.2f m hit at chest height only: vine / branch / thin prop?)", dn);
            }
            snprintf(why, wn,
                     "no surface: hit %.2f m at chest height only, no knee-height face (vine / branch / thin prop)",
                     dn);
            return 0;
        }
        float ks0[2] = { rt[12], rt[13] };
        int kok0 = 0;
        // (duel pins did not work on cars - a car's rounded side put the side knee ray 0.35 m nearer than the centre one (1.21 vs 1.56 m),
        //  over the old 0.3 m. 0.45 m still rejects foliage / clutter, whose side rays miss or land metres apart.)
        kok0 = CdKneeFaceOk(dk, ks0[0], ks0[1]);
        if (!kok0) {
            static uint64_t nlk;
            if (g_frame - nlk > 30) {
                nlk = g_frame;
                Xlog("ctxdown: ledge / cover ignored - the knee hit %.2f m is not a face (side knee rays %.2f %.2f m: foliage / clutter?)", dk, ks0[0] >= 0.0f ? ks0[0] * CD_RAYLEN : -1.0f, ks0[1] >= 0.0f ? ks0[1] * CD_RAYLEN : -1.0f);
            }
            snprintf(why, wn,
                     "no surface: knee hit %.2f m has no flat face (side knee rays %.2f %.2f)",
                     dk, ks0[0] * CD_RAYLEN, ks0[1] * CD_RAYLEN);
            return 0;
        }
        dn = dk;                // the face is the knee-level one (a nearer chest hit is a branch in front of it)
    }
    if (dn >= 0.0f) {
        for (int j = 0; j < CD_NTOP; j++) {     // the first vertical ray beyond the hit: the surface top
            if (CD_TOP_D[j] <= dn + 0.04f)
                continue;
            float tt = rt[6 + j];
            float top = tt >= 0.0f
                && tt < 1.0f ? CD_TOP_Y0 - tt * CD_TOP_SPAN : -9.0f;
            // A car top of 1.15-1.25 m put the victim INTO the hood (TLOU2's cover finishers are authored on ~0.9 m
            // surfaces): cover / ledge tops 0.55-1.1 m only; higher = no takedown (a normal hit)
            if (top > CD_COVER_MAX) {
                snprintf(why, wn,
                         "no surface: nearest hit %.2f m but its top is %.2f m up (cover / ledge tops are 0.55-1.35)",
                         dn, top);
                g_cd.ref_dn = dn;
                return 0;
            }                   // (the refine rays check the face: a car side under its roof line)
            if (top >= 0.55f && CdPlaneCheck()) {     // COVER = a low FACE: a knee ray at +-0.35 m lands within 0.3 m of the nearest hit (a clump / bush does not)
                float ks[2] = { rt[12], rt[13] };
                int kok = 0;
                kok = CdKneeFaceOk(dn, ks[0], ks[1]);
                if (!kok) {
                    static uint64_t nlc;
                    if (g_frame - nlc > 30) {
                        nlc = g_frame;
                        Xlog("ctxdown: cover ignored - no face: nearest %.2f m, knee sides %.2f %.2f m (foliage / clutter?)", dn, ks[0] >= 0.0f ? ks[0] * CD_RAYLEN : -1.0f, ks[1] >= 0.0f ? ks[1] * CD_RAYLEN : -1.0f);
                    }
                    snprintf(why, wn,
                             "no surface: nearest %.2f m has no flat face (knee sides %.2f %.2f)",
                             dn, ks[0] * CD_RAYLEN, ks[1] * CD_RAYLEN);
                    return 0;
                }
            }
            if (top >= 0.55f && dk >= 0.0f) {   // the cover face normal from the knee rays (+-0.35 m), else the centre knee ray and one side
                float kl = rt[12] >= 0.0f
                    && rt[12] < 1.0f ? rt[12] * CD_RAYLEN : -1.0f, kr =
                    rt[13] >= 0.0f
                    && rt[13] < 1.0f ? rt[13] * CD_RAYLEN : -1.0f;
                if (!CdFaceN(kl, 0.35f, kr, -0.35f, u0, u1, n_out)
                    && !CdFaceN(kl, 0.35f, dk, 0.0f, u0, u1, n_out))
                    CdFaceN(dk, 0.0f, kr, -0.35f, u0, u1, n_out);
                float c = u0 * -n_out[0] + u1 * -n_out[1];
                if (c > 0.3f)
                    dn *= c;    // distance along the normal (the frame sits on the face)
            }
            if (top >= 0.55f) {
                *dist = dn;
                snprintf(why, wn,
                         "cover: nearest %.2f m (knee %.2f), top %.2f m up (vertical ray %d, %d chest hits)",
                         dn, dk, top, j, nh);
                g_cd.surf_h = top;
                return 2;
            }
            // the ray landed past a THIN low surface (a railing / parapet / low wall thinner than the ray spacing): floor beyond it (top below
            // 0.55 m) or a drop (no hit down to 0.6 m below the feet) - a ledge; its height is not measured (0.95 m, the authored covers')
            if (!nh && top >= -5.0f) {
                snprintf(why, wn,
                         "no surface: knee hit %.2f m with floor behind it (a step / crate / stairs, not a ledge)",
                         dn);
                g_cd.ref_dn = dn;
                return 0;
            }
            *dist = dn;
            g_cd.surf_h = 0.95f;
            snprintf(why, wn,
                     "ledge: nearest %.2f m (knee %.2f, %d chest hits), beyond it %s (vertical ray %d)",
                     dn, dk, nh, top < -5.0f ? "a drop" : "the floor", j);
            return 2;
        }
        snprintf(why, wn,
                 "no surface: nearest hit %.2f m, no top found behind it",
                 dn);
        g_cd.ref_dn = dn;
        return 0;
    }
    snprintf(why, wn,
             "no surface: chest rays %d hit (%.2f %.2f %.2f %.2f %.2f), knee %.2f",
             nh, hit_d[0], hit_d[1], hit_d[2], hit_d[3], hit_d[4], dk);
    return 0;
}

// Most takedowns became low cover ones: every ray starts at / near the victim, inside its own capsule, and the static mask 0x207 still
// returns the puppet's capsule at t ~0 (A/B: 325 of 338 scans "ledge / cover: nearest 0.00-0.09 m ... beyond it the floor" on open ground -
// the chest rays hit the victim itself, the floor behind made it a "ledge"). The rays ignore the victim's puppet now (kick-raycast's ignore object,
// as paired.c's wall guard does for the same reason).
static void CdKickRays(const float *V, const float *u, uint64_t vic_h)
{
    float s[2] = { -u[1], u[0] };
    for (int k = 0; k < CD_NJOB; k++) {
        float a[3], b[3];
        if (k < 6 || k >= 12) { // 12 / 13: knee height +-0.35 m to the sides (the plane check of a COVER verdict)
            float h = k < 5 ? CD_RAY_H[k] : 0.45f, lat =
                k < 5 ? CD_RAY_LAT[k] : k == 12 ? 0.35f : k ==
                13 ? -0.35f : 0.0f;
            a[0] = V[0] + s[0] * lat;
            a[1] = V[1] + h;
            a[2] = V[2] + s[1] * lat;
            b[0] = a[0] + u[0] * CD_RAYLEN;
            b[1] = a[1];
            b[2] = a[2] + u[1] * CD_RAYLEN;
        } else {
            float d = CD_TOP_D[k - 6];
            a[0] = V[0] + u[0] * d;
            a[1] = V[1] + CD_TOP_Y0;
            a[2] = V[2] + u[1] * d;
            b[0] = a[0];
            b[1] = V[1] + CD_TOP_Y0 - CD_TOP_SPAN;
            b[2] = a[2];
        }
        g_cd.job[k] = WorldRayKickM(a, b, vic_h, CD_MASK);
    }
    int ok = 1;
    for (int k = 0; k < CD_NJOB; k++)
        if (!g_cd.job[k])
            ok = 0;
    g_cd.stage = ok ? 1 : 0;
    if (!g_cd.stage)
        CdDrain();
}

// (B [84385]: a victim 1.46 m from a car's side, "knee hit 1.46 m with floor behind it" - no takedown): the coarse top rays
// stand 0.35 m apart along u from the VICTIM, so the first one behind a far hit lands 0.3 m inside the car, through its window glass (not in the
// static mask) onto the seat / floor, and a car / counter / any low cover with a hollow or a lower level behind its front face read as a step.
// The cover test is now made AT the front face: four vertical rays 0.06 / 0.15 / 0.28 / 0.45 m behind the low hit. COVER = a top 0.55 .. 1.1 m
// anywhere in those 0.45 m (a door sill, a hood behind a bumper, a counter, a low wall), whatever is behind the far side; a step / crate / stair
// stays out (its top is below 0.55 m there: walkable). Runs for every low hit the coarse rays could not settle, over the whole 2.0 m ray range. SP's own low-cover probe (MeleeProbe, C++) is not readable from
// the DC data; its finishers / openers only take its high / low flags (re/contextual-downs.md).
// (no push despite a ~0.6 m gap between the cover and the animation; A [139645] cover-face-smash on a pickup truck's
// front): the knee ray (0.45 m up) hit the BUMPER, whose top is 0.51-0.56 m ("tops behind its face 0.55 0.54 0.53 0.51"), and the frame went on the
// bumper's face. TLOU2's low-cover finishers are authored on ~0.9-1.0 m surfaces (the decoded halves: the victim's head meets the surface 1.0-1.2 m
// up, at the frame's face), so the face smash slammed the head into the air in front of the grille / hood ~0.6 m behind the bumper, and a 0.65 m
// "wall distance" picked the open face smash (no push) instead of a push to the real cover. The step fit: when the cover found is LOW (top under
// CD_FIT_TOP) the refine rays run on 1.05 m behind its face, and if the surface rises to a cover top (CD_FIT_TOP .. 1.1 m) there, the frame goes on
// THAT face (half way between the last low and the first tall sample) - the distance grows by the step's depth, so the push variants carry the
// victim over it to the real cover (the good takedown before it, A [137012]: "tops behind the face 0.55 0.52 0.49 1.02" - the same truck).
#define CD_NREF 8
static const float CD_REF_D[CD_NREF] =
    { 0.06f, 0.15f, 0.28f, 0.45f, 0.60f, 0.75f, 0.90f, 1.05f };
#define CD_NREF_FACE 4          // the cover test itself: a top within the first 0.45 m behind the face
#define CD_WALL_TOP 1.3f        // a surface this high within 0.6 m behind a lip: the lip is a sill on a WALL
#define CD_FIT_TOP 0.75f        // a cover top lower than this is a step in front of the cover the finishers are authored on
static void CdReplicaCands(uint64_t h, const float *me, int style,
                             int kind, float dist, const float *n,
                             const char *why);
static int CdStyleOk(int cur, int pair_style);
static int CdKickRefine(uint64_t vic_h)
{
    for (int k = 0; k < CD_NREF; k++) {
        float d = g_cd.ref_dn + CD_REF_D[k], a[3], b[3];
        a[0] = g_cd.V[0] + g_cd.u[0] * d;
        a[1] = g_cd.V[1] + CD_TOP_Y0;
        a[2] = g_cd.V[2] + g_cd.u[1] * d;
        b[0] = a[0];
        b[1] = g_cd.V[1] + CD_TOP_Y0 - CD_TOP_SPAN;
        b[2] = a[2];
        g_cd.job[k] = WorldRayKickM(a, b, vic_h, CD_MASK);
        if (!g_cd.job[k]) {
            CdDrain();
            return 0;
        }
    }
    g_cd.stage = 3;
    return 1;
}

static void CdRefineFinish(uint64_t h, const float *me, int style)
{
    float top[CD_NREF];
    for (int k = 0; k < CD_NREF; k++) {
        float t = RayT(g_cd.job[k]);
        g_cd.job[k] = 0;
        top[k] = t >= 0.0f
            && t < 1.0f ? CD_TOP_Y0 - t * CD_TOP_SPAN : -9.0f;
    }
    g_cd.stage = 0;
    char why[200];
    float best = -9.0f;
    int kb = -1;
    int coarse = g_cd.ref_coarse;
    g_cd.ref_coarse = 0;
    // the first top in the window across the obstacle's first 0.45 m (a bumper / wheel-arch gap or a short hood reads lower / floor at one sample, the
    // body or the hood at the next); what lies beyond its far side does not matter. Steps / stairs never reach 0.55 m within 0.45 m (0.18 / 0.28 m treads: 0.36 m).
    // (B [31736]: "low hit 0.78 m, top 0.91 m up (tops behind the face 0.91 1.50 1.49 1.49 ...)" = a wall with a sill, played as low cover): a lip whose
    // surface rises to a wall height (>= 1.3 m, three samples in a row) within 0.6 m behind the face is a WALL, framed on the high part (SP's probe flags a high
    // wall, not a low cover, there). The 1.5 m detection cap is checked on the wall distance by cd_replica_cands.
    if (!CdSpFaceOff()) {
        for (int k = 0; k + 2 < CD_NREF && k <= 4; k++) {
            if (top[k] < CD_WALL_TOP)
                continue;
            if (top[k + 1] >= CD_WALL_TOP && top[k + 2] >= CD_WALL_TOP) {
                float wd =
                    g_cd.ref_dn +
                    (k ? 0.5f * (CD_REF_D[k - 1] + CD_REF_D[k]) : 0.03f);
                float wn2[2];
                CdKneeN(wn2);
                if (coarse) {
                    wn2[0] = g_cd.ref_cn[0];
                    wn2[1] = g_cd.ref_cn[1];
                }
                g_cd.surf_h = 0.0f;
                g_cd.fit_gap = 0.0f;
                g_cd.fit_step_h = -1.0f;
                snprintf(why, sizeof why,
                         "WALL with a lip: low hit %.2f m, the surface rises to %.2f m %.2f m behind it -> frame on the high part %.2f m (tops %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f)",
                         g_cd.ref_dn, top[k], wd - g_cd.ref_dn, wd, top[0],
                         top[1], top[2], top[3], top[4], top[5], top[6],
                         top[7]);
                CdReplicaCands(h, me, style, 1, wd, wn2, why);
                return;
            }
        }
    }
    for (int k = 0; k < CD_NREF_FACE && best < 0.0f; k++)
        if (top[k] >= 0.55f && top[k] <= CD_COVER_MAX) {
            best = top[k];
            kb = k;
        }
    if (best < 0.0f && coarse) {        // the step check found no top where the coarse rays did (ray noise): the coarse cover stands
        g_cd.fit_gap = 0.0f;
        g_cd.fit_step_h = -1.0f;
        CdReplicaCands(h, me, style, 2, g_cd.ref_cd, g_cd.ref_cn,
                         g_cd.ref_why);
        return;
    }
    float dist = g_cd.ref_dn;
    g_cd.fit_gap = 0.0f;
    g_cd.fit_step_h = -1.0f;
    if (best >= 0.0f && best < CD_FIT_TOP) {    // a low step: is the cover the finishers are authored on right behind it?
        for (int k = kb + 1; k < CD_NREF; k++) {
            if (top[k] > CD_COVER_MAX)
                break;          // a wall / cab behind the step: no cover top there
            if (top[k] >= CD_FIT_TOP) {
                float fd = 0.5f * (CD_REF_D[k - 1] + CD_REF_D[k]);
                // (A [33758]: low face 1.41 m, step top 0.69, cover 0.52 m behind -> frame 1.93 m -> refused by the 1.5 m cap): the fit must not push a valid
                // low cover past SP's detection distance - the nearest qualifying face stays the frame
                if (!CdSpFaceOff() && g_cd.ref_dn <= CD_SP_DETECT
                    && g_cd.ref_dn + fd > CD_SP_DETECT)
                    break;
                g_cd.fit_step_h = best;
                g_cd.fit_gap = fd;
                dist = g_cd.ref_dn + fd;
                best = top[k];
                break;
            }
        }
    }
    if (best < 0.0f) {
        static uint64_t nlf;
        snprintf(why, sizeof why,
                 "no surface: low hit %.2f m, tops behind its face %.2f %.2f %.2f %.2f m (cover = a top 0.55 .. 1.35 m within 0.45 m of the face)",
                 g_cd.ref_dn, top[0], top[1], top[2], top[3]);
        CdNote("slot %d: %s", g_cd.slot, why);
        if (g_frame - nlf > 60) {
            nlf = g_frame;
            Xlog("ctxdown: scan slot %d - %s (victim at %.2f %.2f %.2f, rays along %.2f %.2f)", g_cd.slot, why, g_cd.V[0], g_cd.V[1], g_cd.V[2], g_cd.u[0], g_cd.u[1]);
        }
        CdScanFail();
        return;
    }
    g_cd.surf_h = best;
    float n[2];
    CdKneeN(n);
    if (coarse) {
        n[0] = g_cd.ref_cn[0];
        n[1] = g_cd.ref_cn[1];
    }
    if (g_cd.fit_gap > 0.0f)
        snprintf(why, sizeof why,
                 "STEP FIT: low face %.2f m (step top %.2f), cover top %.2f m %.2f m behind it -> frame on the cover face %.2f m (tops %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f)",
                 g_cd.ref_dn, g_cd.fit_step_h, best, g_cd.fit_gap, dist,
                 top[0], top[1], top[2], top[3], top[4], top[5], top[6],
                 top[7]);
    else
        snprintf(why, sizeof why,
                 "cover at the face: low hit %.2f m, top %.2f m up (tops behind the face %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f)",
                 g_cd.ref_dn, best, top[0], top[1], top[2], top[3], top[4],
                 top[5], top[6], top[7]);
    CdReplicaCands(h, me, style, 2, dist, n, why);
}

static void CdReplicaFinish(uint64_t h, const float *me, int style)
{
    for (int k = 0; k < CD_NJOB; k++) {
        g_cd.rt[k] = RayT(g_cd.job[k]);
        g_cd.job[k] = 0;
    }
    g_cd.stage = 0;
    float dist, n[2];
    char why[160];
    g_cd.surf_h = 0;
    g_cd.ref_dn = -1.0f;
    g_cd.ref_coarse = 0;
    g_cd.fit_gap = 0.0f;
    g_cd.fit_step_h = -1.0f;
    int kind = CdClassify(&dist, n, why, sizeof why);
    if (kind == 2 && g_cd.surf_h > 0.0f && (g_cd.surf_h < CD_FIT_TOP || !CdSpFaceOff())) {   // a LOW cover (bumper / curb / step): look for the real cover behind it; every cover (a sill may sit on a wall)
        g_cd.ref_dn = dist;
        g_cd.ref_coarse = 1;
        g_cd.ref_cd = dist;
        g_cd.ref_cn[0] = n[0];
        g_cd.ref_cn[1] = n[1];
        snprintf(g_cd.ref_why, sizeof g_cd.ref_why, "%s", why);
        if (CdKickRefine(h))
            return;
        g_cd.ref_coarse = 0;
    }
    if (!kind) {
        if (g_cd.ref_dn >= 0.0f && CdKickRefine(h))
            return;             // a low hit with no top found by the coarse rays: look right behind its face
        static uint64_t nlf;
        CdNote("slot %d: %s", g_cd.slot, why);
        if (g_frame - nlf > 60) {
            nlf = g_frame;
            Xlog("ctxdown: scan slot %d - %s", g_cd.slot, why);
        }
        CdScanFail();
        return;
    }
    CdReplicaCands(h, me, style, kind, dist, n, why);
}

// Grass still got counted as low cover: the static-world ray API (kick-raycast / wait-raycast-t) answers only a fraction, no
// material / layer, so vegetation with collision cannot be named - it is told by its SHAPE. Every cover / ledge verdict (rays or step fit) must pass
// a solidity probe before it is used: 9 horizontal rays at the face (0.25 / 0.50 / 0.75 m up x 0.30 m to the left / centre / right) must hit within
// 0.35 m of the verdict's distance, the centre column's heights within 0.15 m of each other (a wall face is vertical; blades / bushes scatter), at
// least 7 of 9 hit and no ray 0.6 m closer or farther than the rest (spread <= 0.45 m); and three vertical rays 0.10 m behind the face (left / centre /
// right) must land on one flat top within 0.15 m (two of three suffice for a thin ledge, that has no top to land on). Logged as
// "ctxdown: cover rejected (foliage ...)". t2r-cd-no-solid.txt next to the exe = off.
#define CD_NSOL 12
static int CdSolidOff(void)
{
    static int v = -1;
    static uint64_t fr;
    if (v < 0 || g_frame - fr > 600) {
        fr = g_frame;
        FILE *f = ExeSideOpen("t2r-cd-no-solid.txt", "r");
        v = f != NULL;
        if (f)
            fclose(f);
    }
    return v;
}
static const float CD_SOL_H[3] = { 0.25f, 0.50f, 0.75f }, CD_SOL_LAT[3] =
    { -0.30f, 0.0f, 0.30f };
static int CdKickSolid(uint64_t vic_h, float dist)
{
    float s[2] = { -g_cd.u[1], g_cd.u[0] };
    for (int k = 0; k < CD_NSOL; k++) {
        float a[3], b[3];
        if (k < 9) {
            float h = CD_SOL_H[k / 3], lat = CD_SOL_LAT[k % 3];
            a[0] = g_cd.V[0] + s[0] * lat;
            a[1] = g_cd.V[1] + h;
            a[2] = g_cd.V[2] + s[1] * lat;
            b[0] = a[0] + g_cd.u[0] * CD_RAYLEN;
            b[1] = a[1];
            b[2] = a[2] + g_cd.u[1] * CD_RAYLEN;
        } else {
            float lat = CD_SOL_LAT[k - 9], d = dist + 0.10f;
            a[0] = g_cd.V[0] + g_cd.u[0] * d + s[0] * lat;
            a[1] = g_cd.V[1] + CD_TOP_Y0;
            a[2] = g_cd.V[2] + g_cd.u[1] * d + s[1] * lat;
            b[0] = a[0];
            b[1] = g_cd.V[1] + CD_TOP_Y0 - CD_TOP_SPAN;
            b[2] = a[2];
        }
        g_cd.job[k] = WorldRayKickM(a, b, vic_h, CD_MASK);
        if (!g_cd.job[k]) {
            CdDrain();
            return 0;
        }
    }
    g_cd.stage = 4;
    return 1;
}

static void CdSolidFinish(uint64_t h, const float *me, int style)
{
    float t[CD_NSOL];
    for (int k = 0; k < CD_NSOL; k++) {
        t[k] = RayT(g_cd.job[k]);
        g_cd.job[k] = 0;
    }
    g_cd.stage = 0;
    float dist = g_cd.sol_face, lo = 99.0f, hi = -99.0f, clo = 99.0f, chi =
        -99.0f;
    int nh = 0, near_ok = 1, nuse = 0;
    for (int k = 0; k < 9; k++) {
        // (real low cover rejected - the 0.75 m face rays pass OVER a cover whose top is lower and hit what stands behind it:
        // only the rays below the cover's top (surf_h - 0.1) judge its face)
        if (g_cd.surf_h > 0.0f && CD_SOL_H[k / 3] > g_cd.surf_h - 0.10f)
            continue;
        nuse++;
        if (!(t[k] >= 0.0f && t[k] < 1.0f))
            continue;
        float d = t[k] * CD_RAYLEN;
        nh++;
        if (d < lo)
            lo = d;
        if (d > hi)
            hi = d;
        if (__builtin_fabsf(d - dist) > 0.35f)
            near_ok = 0;
        if (k % 3 == 1) {
            if (d < clo)
                clo = d;
            if (d > chi)
                chi = d;
        }
    }
    float tops[3];
    int nt = 0;
    float tlo = 99.0f, thi = -99.0f;
    for (int k = 0; k < 3; k++) {
        float tt = t[9 + k];
        tops[k] = tt >= 0.0f
            && tt < 1.0f ? CD_TOP_Y0 - tt * CD_TOP_SPAN : -9.0f;
        if (tops[k] > -5.0f) {
            nt++;
            if (tops[k] < tlo)
                tlo = tops[k];
            if (tops[k] > thi)
                thi = tops[k];
        }
    }
    const char *bad = NULL;
    if (nuse < 3)
        nuse = 3;
    if (nh * 10 < nuse * 7)
        bad = "face rays mostly miss (a gappy surface)";
    else if (!near_ok)
        bad = "face rays land far from the verdict's distance";
    else if (hi - lo > 0.45f)
        bad = "face rays scattered in depth";
    // (real covers rejected - sloped / rounded faces (car sides, sandbags) and uneven tops failed the old 0.15 m vertical / flat-top
    // tests; grass is caught by the gap / depth-scatter tests, so these two only reject a clearly broken surface now)
    else if (chi - clo > 0.35f)
        bad = "face not vertical (centre column scattered)";
    else if (g_cd.surf_h >= 0.55f && nt < 1)
        bad = "no top behind the face";
    char why[200];
    if (bad) {
        snprintf(why, sizeof why,
                 "cover rejected (foliage / clutter: %s - face %d of 9 rays %.2f..%.2f m, centre %.2f..%.2f, tops %.2f %.2f %.2f)",
                 bad, nh, nh ? lo : -1.0f, nh ? hi : -1.0f,
                 nh ? clo : -1.0f, nh ? chi : -1.0f, tops[0], tops[1],
                 tops[2]);
        static uint64_t nlf;
        CdNote("slot %d: %s", g_cd.slot, why);
        if (g_frame - nlf > 30) {
            nlf = g_frame;
            Xlog("ctxdown: %s (verdict %s %.2f m, victim at %.2f %.2f %.2f)", why, g_cd.sol_kind == 1 ? "wall" : "cover", dist, g_cd.V[0], g_cd.V[1], g_cd.V[2]);
        }
        CdScanFail();
        return;
    }
    g_cd.solid_done = 1;
    CdReplicaCands(h, me, style, g_cd.sol_kind, g_cd.sol_dist,
                     g_cd.sol_n, g_cd.sol_why);
    g_cd.solid_done = 0;
}

static void CdReplicaCands(uint64_t h, const float *me, int style,
                             int kind, float dist, const float *n,
                             const char *why)
{
    if (kind == 2 && !g_cd.solid_done && !CdSolidOff()) {     // solidity probe first (re-entered through cd_solid_finish)
        g_cd.sol_dist = dist;
        g_cd.sol_face = g_cd.fit_gap > 0.0f ? dist - g_cd.fit_gap : dist;
        g_cd.sol_kind = kind;
        g_cd.sol_n[0] = n[0];
        g_cd.sol_n[1] = n[1];
        snprintf(g_cd.sol_why, sizeof g_cd.sol_why, "%s", why);
        if (CdKickSolid(h, g_cd.sol_face))
            return;
    }
    g_cd.kind = kind;
    if (kind != 2) {
        g_cd.fit_gap = 0.0f;
        g_cd.fit_step_h = -1.0f;
    }
    if (!CdSpFaceOff() && dist > CD_SP_DETECT) {
        Xlog("ctxdown: rays slot %d: %s %.2f m behind the victim - beyond SP's 1.5 m player-target detection distance: no takedown (%s) [SP MeleeProbe on puppet: %s]", g_cd.slot, kind == 1 ? "wall" : "cover", dist, why, g_cdSpWhy);
        CdNote("slot %d: %s %.2f m is farther than SP's 1.5 m detection",
                g_cd.slot, kind == 1 ? "wall" : "cover", dist);
        CdScanFail();
        return;
    }
    {                           // cone search: the surface normal's angle to the reversed attack direction (every scan: logged in the plan lines); a turned cast must find a real face normal
        float ca = n[0] * -g_cdUatt[0] + n[1] * -g_cdUatt[1];
        g_cd_cand_ang =
            __builtin_acosf(fmaxf(-1.0f, fminf(1.0f, ca))) * 57.29578f;
        if (g_cdRot != 0 && g_cdHeadon <= 0) {
            float cu = n[0] * -g_cd.u[0] + n[1] * -g_cd.u[1];
            if (g_cd_cand_ang > CD_CONE_DEG || cu > 0.99999f) {
                CdNote
                    ("slot %d: cone search (cast %+.0f deg): %s %.2f m, %s",
                     g_cd.slot, (double) CD_CONE_ROT[g_cdRot],
                     kind == 1 ? "wall" : "cover", dist,
                     cu >
                     0.99999f ?
                     "no face normal from the side rays (the frame would be the cast direction, not the wall's)"
                     :
                     "normal outside the 45 deg cone of the attack direction");
                CdScanFail();
                return;
            }
            static int nlr;
            if (LOGCAP(nlr, 60))
                Xlog("ctxdown: cone search slot %d: %s %.2f m found by the cast %+.0f deg off the attack direction; its normal is %.0f deg off the attack direction (cone %.0f)", g_cd.slot, kind == 1 ? "WALL" : "COVER", dist, (double) CD_CONE_ROT[g_cdRot], (double) g_cd_cand_ang, (double) CD_CONE_DEG);
        }
    }
    float yaw =
        __builtin_atan2f(n[0],
                         n[1]) + (g_cdYawOver ==
                                  -999 ? 0.0f : (float) g_cdYawOver *
                                  0.0174533f);
    float ap7[7] =
        { g_cd.V[0] - n[0] * dist, g_cd.V[1], g_cd.V[2] - n[1] * dist, 0,
 __builtin_sinf(yaw * 0.5f), 0, __builtin_cosf(yaw * 0.5f) };
    // the step fit (cd_refine_finish) is for the finishers whose contact is ON the cover top (head smashed onto it / bent back over it: cd_top_contact);
    // the lift-n-slam's contact is the floor in front of the face, and it played on the step's face in the good takedown (A [137096]),
    // so it keeps the step's face. A top-contact finisher on a LOW cover with no taller one behind (a curb / bench under CD_FIT_TOP) is refused: its
    // contact would be in the air above it (SP's low-cover probe is crouch-cover height).
    float dlow = kind == 2
        && g_cd.fit_gap > 0.0f ? dist - g_cd.fit_gap : dist;
    float ap7l[7] =
        { g_cd.V[0] - n[0] * dlow, g_cd.V[1], g_cd.V[2] - n[1] * dlow, 0,
ap7[4], 0, ap7[6] };
    float top_hi = g_cd.surf_h, top_lo = kind == 2
        && g_cd.fit_gap > 0.0f ? g_cd.fit_step_h : g_cd.surf_h;
    int low_only = kind == 2 && g_cd.fit_gap <= 0.0f && g_cd.surf_h > 0.0f
        && g_cd.surf_h < CD_FIT_TOP;
    int n_low = 0;
    g_cdNcand = 0;
    for (size_t i = 0; i < CD_N; i++) {
        int tc = kind == 2 && CdTopContact((int) i);
        float dw = tc ? dist : dlow;
        if (CD_PAIRS[i].kind == kind && CdStyleOk(style, CD_PAIRS[i].style) && dw <= CD_PAIRS[i].dmax && dw >= CdDmin((int) i) &&    // SP's window, no slack
            CdPairLoaded((int) i)) {
            if (tc && low_only) {
                n_low++;
                continue;
            }
            CdCandAdd((int) i, tc ? ap7 : ap7l, dw);
            g_cd_cand[g_cdNcand - 1].fit = tc
                && kind == 2 ? g_cd.fit_gap : 0.0f;
            g_cd_cand[g_cdNcand - 1].top =
                kind == 2 ? (tc ? top_hi : top_lo) : -1.0f;
        }
    }
    Xlog("ctxdown: rays slot %d: %s %.2f m behind the victim%s, normal %.0f deg off -u, style %d - %s -> %d pair(s)%s [SP MeleeProbe on puppet: %s]", g_cd.slot, kind == 1 ? "WALL" : "COVER", dist, dlow != dist ? " (top-contact finishers; the others on the step's face)" : "", __builtin_acosf(n[0] * -g_cd.u[0] + n[1] * -g_cd.u[1]) * 57.29578f, style, why, g_cdNcand, n_low ? " (top-contact finishers refused: the cover top is under 0.75 m, their contact would be in the air)" : "", g_cdSpWhy);
    if (!g_cdNcand) {
        CdNote
            ("slot %d: %s %.2f m but no pair of style %d allows that distance / is loaded",
             g_cd.slot, kind == 1 ? "wall" : "cover", dist, style);
        CdScanFail();
        return;
    }
    CdPick(h, g_cd.V, me, 1);
}

// ---------------------------------------------------------------------------------------------------- scan (attacker)
// the attacker's pair set: -1 fists, 3 = a female life's knife, >= 100 = a held melee weapon: 100 + the bit set of the pair styles its SP move sets allow
// (bit 0 = style 0 1h blunt, 1 = style 1 1h blade, 2 = style 2 2h blunt, 4 = machete-only blade moves, 5 = hammer / crowbar-class blunt moves; cd_weapon_mask),
// -2 = unknown / an UPGRADED melee (those keep their own one-shot).
// SP's own rule (the finisher start lambdas' melee-weapon-bit-check masks of player-1h-blade / -1h-blunt / -2h-blunt-finishers.bin, decoded against the weapon lists
// of melee-script-funcs-impl.bin ENTRY 0x433E8): 0x100000 = every 1h blade (hatchet*, machete*, scythe-scar, tomahawk*), 0x2400000 = machete* only, 0x400 = every 1h blunt
// (club-1h-*, crowbar, hammer-*, pipe-wrench, short-pipe*), 0xc4000 = crowbar + hammer-modern + clubs / hammer-scar / pipe-wrench (not the short pipe) = the hammer neck jab,
// 0x8010000000 = the 2h blunt (baseball-bat*, club-2h-scar, lead-pipe, two-by-four, sledgehammer / pickaxe). The axes have no wall / cover finisher (2h blade: ledge only).
#define CD_WSET_BASE 100
static int CdWeaponMask(uint64_t w)
{
    static const struct {
        const char *n;
        int mask;
    } T[] = {
        { "machete", 0x02 | 0x10 }, { "machete-scar", 0x02 | 0x10 },
            { "tomahawk", 0x02 }, { "hatchet-modern", 0x02 }, { "hatchet",
                                                               0x02 },
        { "crowbar", 0x01 | 0x20 }, { "hammer-modern", 0x01 | 0x20 },
            { "hammer-scar", 0x01 | 0x20 }, { "club-1h-police",
                                             0x01 | 0x20 },
            { "club-1h-scar", 0x01 | 0x20 }, { "pipe-wrench",
                                              0x01 | 0x20 },
        { "short-pipe", 0x01 },
        { "lead-pipe", 0x04 }, { "two-by-four", 0x04 }, { "baseball-bat",
                                                         0x04 },
        { "fireman-axe", 0x04 },        // v5-dmg: the axe is out of the game; held anyway = a 2h blunt (was 0x40 2h blade)
    };
    static uint64_t h[sizeof T / sizeof T[0]];
    if (!h[0])
        for (size_t i = 0; i < sizeof T / sizeof T[0]; i++)
            h[i] = FNV64(T[i].n);
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++)
        if (h[i] == w)
            return T[i].mask;
    return g_meleeStyle == STYLE_1H_BLADE ? 0x02 : g_meleeStyle == STYLE_2H_BLUNT ? 0x04 : 0x01;      // an unlisted melee item: its last swing's move set
}

static int CdStyleOk(int cur, int pair_style)
{
    if (cur >= CD_WSET_BASE)
        return pair_style >= 0 && pair_style < 8
            && (((cur - CD_WSET_BASE) >> pair_style) & 1);
    return cur == pair_style;
}

// A pin that switched to unarmed (with a 2h weapon) must end in an unarmed pin finisher too, not use the melee weapon: a duel pin that runs on the fist / knife set (2h weapon put away, or a gun
// in the hand with a melee item owned) sets this style for its finisher pick on that frame (melee_duel_pin.c mpn_fin_plan): -1 fists, 3 knife.
static int g_cd_style_force, g_cd_style_force_on;
static uint64_t g_cdStyleForceFrame;
static int CdCurStyle(void)
{
    if (g_cd_style_force_on && g_cdStyleForceFrame == g_frame)
        return g_cd_style_force;
    uint64_t none = 0, hasw = DcCall(SID_HAS_MELEE_WEAPON, 0, &none);
    if (hasw == ~0ULL)
        return -2;
    if (hasw & 0xff) {
        if (was_up_g)
            return -2;          // an upgraded melee keeps its one-shot (upg13_start / cd_upg_start), never a contextual takedown
        static int off = -1;
        static uint64_t chk;
        if (off < 0 || g_frame - chk > 120) {
            chk = g_frame ? g_frame : 1;
            off =
                GetFileAttributesA("t2r-cd-no-weapon-takedowns.txt") !=
                INVALID_FILE_ATTRIBUTES;
        }
        if (off)
            return -2;
        return CD_WSET_BASE + CdWeaponMask(CurrentWeapon());
    }
    return g_femKnifeNow ? 3 : -1;
}

// ps44 T10 (every open finisher row refused by walls -> do wall/halfwall finishers, as SP): melee_duel.c asks for a scan of the lethal
// blow's victim at that moment (cd_force_scan44) - cd_scan_tick then scans every frame (not every 6th) and takes that victim even when cd_target picks another,
// until the plan for it is ready or the window ends. The scan itself (engine MeleeProbe / ray replica, wall + low cover, spot path ray) is unchanged.
static int g_cdForce44 = -1;
static uint64_t g_cdForce44Until;
static void CdForceScan44(int slot, int frames)
{
    g_cdForce44 = slot;
    g_cdForce44Until = g_frame + (uint64_t) frames;
}

static int CdForce44On(void)
{
    return g_cdForce44 >= 0 && g_frame < g_cdForce44Until;
}

static void CdScanTick(void)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    CdFilesPoll();
    if (!CdEnabled() || MatchSubstate() != 6 || !g_meleeDown || g_downed || g_ps.role || (g_gx.until && g_t60 < g_gx.until) || LocalHp() <= 0.0f || (g_stun_until && g_t60 < g_stun_until) || Mv3StandMe("ctxdown: takedown scan")) {     // (melee v3: no takedown plans while a bubble owns our melee)
        if (g_cd.stage)
            CdDrain();
        g_cd.stage = 0;
        g_cd.plan_ok = 0;
        return;
    }
    float mb[16];
    const float *me = PlayerLocator(mb);
    if (!me || !PsLocOk(me)) {
        CdDrain();
        g_cd.stage = 0;
        return;
    }
    uint8_t *peer0 = g_cd.slot >= 0
        && g_cd.slot <
        T2R_MAX_PEERS ? ((fn_peer) T2rPeer) ((uint32_t) g_cd.slot) : NULL;
    uint64_t h0 = peer0 ? *(uint64_t *) (peer0 + 0x60) : 0;
    int style = CdCurStyle();
    if (g_cd.stage == 1) {
        CdReplicaFinish(h0, me, style);
        return;
    }                           // the replica's surface rays kicked last frame
    if (g_cd.stage == 3) {
        CdRefineFinish(h0, me, style);
        return;
    }
    if (g_cd.stage == 4) {
        CdSolidFinish(h0, me, style);
        return;
    }                           // the foliage probe behind a cover / ledge verdict       // the refine rays behind a low hit's face
    if (g_cd.stage == 2) {      // the attacker-spot ray kicked last frame
        float t = RayT(g_cd.job[0]);
        g_cd.job[0] = 0;
        g_cd.stage = 0;
        if (t >= 0.0f && t < 0.95f) {
            float *A = g_cd_spot_a, *B = g_cd_spot_b;
            Xlog("ctxdown: spot path of %s blocked at %.2f of %.2f m (static world, victim ignored) at %.2f %.2f %.2f (us %.2f %.2f, spot %.2f %.2f) - %s", CD_PAIRS[g_cd.pend_pair].name, t, __builtin_sqrtf((B[0] - A[0]) * (B[0] - A[0]) + (B[2] - A[2]) * (B[2] - A[2])), A[0] + (B[0] - A[0]) * t, A[1], A[2] + (B[2] - A[2]) * t, A[0], A[2], B[0], B[2], g_cd_alt_i + 1 < g_cd_nalt ? "trying the next variant" : "no variant left");
            CdNote
                ("slot %d: %s - the way to the authored attacker spot is blocked (ray %.2f)",
                 g_cd.slot, CD_PAIRS[g_cd.pend_pair].name, t);
            g_cd_alt_i++;
            if (CdKickAlt(h0, 0))
                return;
            CdScanFail();
            return;
        }
        CdPlanCommit(t);
        return;
    }
    if (g_frame % 6 && !CdForce44On())
        return;                 // (ps44 T10: a forced scan runs every frame)
    // (the plan is not expired here any more: a stale plan is still judged by cd_try against where the bodies are NOW - a 5-punch combo
    // took 424 frames and the 150-frame life had killed the plan; scans that find nothing for a moment (hit reactions) no longer clear it either)
    float V[3];
    int slot = CdTarget(me, V);
    if (CdForce44On() && slot != g_cdForce44 && g_cdForce44 < T2R_MAX_PEERS && !g_peerDowned[g_cdForce44] && !g_puppetDeadHere[g_cdForce44]) { // ps44 T10: the lethal blow's victim
        uint8_t *pf = ((fn_peer) T2rPeer) ((uint32_t) g_cdForce44);
        uint64_t hf = pf ? *(uint64_t *) (pf + 0x60) : 0;
        float fb[16];
        const float *fl = hf ? PuppetLocator(hf, fb) : NULL;
        if (fl && PsLocOk(fl)) {
            static int nf;
            if (LOGCAP(nf, 40))
                Xlog("ctxdown: ps44 forced scan - slot %d (the lethal blow's victim) instead of cd_target's %d", g_cdForce44, slot);
            slot = g_cdForce44;
            V[0] = fl[0];
            V[1] = fl[1];
            V[2] = fl[2];
        }
    }
    if (slot < 0) {
        CdNote
            ("no standing hostile within 2.5 m / 66 deg of where we face");
        return;
    }
    float dx = V[0] - me[0], dz = V[2] - me[2], d =
        __builtin_sqrtf(dx * dx + dz * dz);
    if (d < 0.2f)
        return;
    uint8_t *peer = ((fn_peer) T2rPeer) ((uint32_t) slot);
    uint64_t h = peer ? *(uint64_t *) (peer + 0x60) : 0;
    typedef uint8_t *(*fn_proc)(uint64_t);
    uint8_t *pr = h ? ((fn_proc) T2R_SYM(kFnEntityFromHandle)) (h) : NULL;
    float vb[16];
    const float *vl = h ? PuppetLocator(h, vb) : NULL;
    if (!pr || !vl || !PsLocOk(vl))
        return;
    g_cd.slot = slot;
    memcpy(g_cd.V, V, sizeof V);
    g_cd.u[0] = dx / d;
    g_cd.u[1] = dz / d;
    g_cdUatt[0] = g_cd.u[0];
    g_cdUatt[1] = g_cd.u[1];
    if (g_cdRot != 0 && g_cdHeadon <= 0) {    // cone search: this scan casts along the attack direction turned by CD_CONE_ROT[g_cd_rot]
        float ra = CD_CONE_ROT[g_cdRot] * 0.0174533f, cs =
            __builtin_cosf(ra), sn = __builtin_sinf(ra);
        g_cd.u[0] = g_cdUatt[0] * cs - g_cdUatt[1] * sn;
        g_cd.u[1] = g_cdUatt[0] * sn + g_cdUatt[1] * cs;
    }
    float cfw = 0, csd = 0;     // melee-direction-check front: we are in their front half plane
    // (A/B: puppet_locator's rotation said "faces away" on every scan of a test where the victim faced us; finisher_dir reads the
    // process locator the shiv / execution direction rules use)
    // (A/B: a wall plan died because the victim turned during the punch combo): "not from behind" - the front and the sides pass (cfw > -|side|);
    // only an attacker that is really in the victim's back half (behind by more than sideways) is refused. In MP the victim's facing flickers with the
    // hit reactions, TLOU2 itself only checks it at the move start.
    // A victim turned backwards to the attacker did not play at all: a victim facing away is taken too - the pair's
    // blend-in turns it to its authored facing. Only an unreadable direction refuses now; t2r-cd-front-only.txt = the old front-half rule.
    // Match SP's behaviour exactly (A [63884]: a victim facing the wall was spun 168 deg in 0.2 s = a teleport): SP's
    // finisher lambdas start only with (melee-direction-check attacker defender (melee-bit-dir front) ...) = is-in-front?(defender, attacker, 180 deg):
    // the attacker in the victim's front HALF plane. NOT in MP (victims move freely) - any victim facing works as long as the attacker faces the victim and there is cover behind it; t2r-cd-sp-facing.txt = SP's front rule + the 90 deg turn limit.
    static int any_facing = -1;
    static uint64_t fo_chk;
    if (any_facing < 0 || g_frame >= fo_chk) {
        any_facing =
            GetFileAttributesA("t2r-cd-sp-facing.txt") ==
            INVALID_FILE_ATTRIBUTES;
        fo_chk = g_frame + 120;
    }
    {
        float lb[16];
        const float *pl = PsyncLoc(h, lb);
        g_cd.Vyaw_ok = pl && PsLocOk(pl);
        if (g_cd.Vyaw_ok)
            g_cd.Vyaw = Uc4Yaw(pl);
    }
    if (FinisherDir((int32_t) slot, &cfw, &csd) < 0
        || (!any_facing && !(cfw > 0.0f))) {
        static uint64_t nlf;
        CdNote("slot %d: we are behind them (front %.2f side %.2f)", slot,
                cfw, csd);
        if (g_frame - nlf > 120) {
            nlf = g_frame;
            Xlog("ctxdown: slot %d faces away from us (front %.2f side %.2f; puppet_locator test %d) - no takedown", slot, cfw, csd, CdFrontOk(me, vl));
        }
        CdScanFail();
        return;
    }
    int use_eng = !g_cdRayFile && CdEngineOk();
    if (use_eng) {
        const char *why;
        uint8_t *probes = CdProbesOf(pr, &why);
        float dirv[3] = { V[0] - me[0], V[1] - me[1], V[2] - me[2] };
        if (!probes) {
            snprintf(g_cdSpWhy, sizeof g_cdSpWhy, "unavailable (%s)",
                     why);
            static uint64_t nlf;
            if (g_frame - nlf > 180) {
                nlf = g_frame;
                Xlog("ctxdown: no engine probe on the puppet of slot %d (%s) - ray replica this scan", slot, why);
            }
        } else {
            CdProbeDump(slot, probes, pr, dirv);
            g_cdNcand = 0;
            for (size_t i = 0; i < CD_N; i++) {
                if (CD_PAIRS[i].kind == 0 || CD_PAIRS[i].kind >= 3
                    || !CdStyleOk(style, CD_PAIRS[i].style)
                    || !CdPairLoaded((int) i))
                    continue;   // (kind 0: the upgraded one-shots, no surface)
                float loc[8];
                if (!CdEngineProbe
                    (probes, dirv, CD_PAIRS[i].dmax, CD_PAIRS[i].kind == 1,
                     loc))
                    continue;
                float ap7[7] =
                    { loc[0], loc[1], loc[2], loc[4], loc[5], loc[6],
             loc[7] };
                float qn =
                    __builtin_sqrtf(ap7[3] * ap7[3] + ap7[4] * ap7[4] +
                                    ap7[5] * ap7[5] + ap7[6] * ap7[6]);
                for (int k = 3; k < 7; k++)
                    ap7[k] /= qn;
                float dw =
                    __builtin_sqrtf((loc[0] - V[0]) * (loc[0] - V[0]) +
                                    (loc[2] - V[2]) * (loc[2] - V[2]));
                if (dw < CdDmin((int) i))
                    continue;   // the contextual opener's wall-min-range (combo-only fist moves)
                CdCandAdd((int) i, ap7, dw);
            }
            if (g_cd.eng_hits && !g_cd.eng_state) {
                g_cd.eng_state = 1;
                Xlog("ctxdown: the engine's probe answers on the puppets (%d hits in %d calls)", g_cd.eng_hits, g_cd.eng_calls);
            }
            if (!g_cdNcand) {
                static uint64_t nlf;
                CdNote
                    ("slot %d: the engine probe finds no wall / cover for any pair of style %d",
                     slot, style);
                if (g_frame - nlf > 120) {
                    nlf = g_frame;
                    Xlog("ctxdown: scan slot %d - the engine finds no wall / low cover behind them for any pair (%d calls, %d hits so far)", slot, g_cd.eng_calls, g_cd.eng_hits);
                }
                CdScanFail();
                return;
            }
            CdPick(h, V, me, 0);
            return;
        }
    }
    if (!use_eng)
        snprintf(g_cdSpWhy, sizeof g_cdSpWhy, "not tried (%s)",
                 g_cdRayFile ? "t2r-cd-ray-probe.txt" :
                 "exe build differs");
    CdKickRays(V, g_cd.u, h); // replica (engine probe off / unavailable)
}

// ---------------------------------------------------------------------------------------------------- start (attacker)
// fallback for a victim that refused (or a move that could not start after the kill move was already declined): the plain down hit
static void CdPlainDown(int slot, const char *why)
{
    if (slot < 0 || slot >= T2R_MAX_PEERS || g_peerDowned[slot])
        return;
    typedef void (*fn_hit3)(uint32_t, uint64_t, float);
    ((fn_hit3) T2R_SYM(kFnApplyHit)) ((uint32_t) slot,
                                      FNV64("abby-unarmed"), 150.0f);
    g_kdFallbackFrame = g_t60;
    g_kdFallbackSlot = slot;
    Xlog("ctxdown: fallback - %s: the plain down hit goes to slot %d", why,
         slot);
}

// the camera of the pair: TLOU2's own authored animated camera (apReference-camera1 / 2 channels of the clip we play), through the finisher-camera
// hook (re/finisher-camera.md); the execution camera only if that hook is not there
static uint64_t CdCamDelayFrames(void)
{                               // the chain's segments before the finisher: the clip camera can only start with the finisher's clip
    int p = CdPairOfMove(g_ps.move);
    if (p < 0)
        return 0;
    float f = CdChainTotal(p) - CdFinLen(p) - (float) ((PsyncMs() - g_ps.t0 + g_ps.chain_shift) / 1000.0); // (minus the move time already run: a start into the push)
    return f > 0.0f ? (uint64_t) (f * 60.0f) : 0;
}

// TLOU2's own camera of the takedown, decoded offline from the clips' apReference-camera1 / 2 channels (tools/ctxcam_gen.py -> ctxcam_tracks.h) and played by
// the execution camera (xcam: ap space, the chain's clock, the clip's lens / roll): the attacker gets the camera the reference video shows, the victim the other
// side's authored camera (same lens / motion / timing). The finisher-camera hook (route through the game's PlayerCameraAnimationDirector) did not start for the
// throat punch / victim halves in the A/B ("no animated camera 30 frames in") and cannot show the push-hit segments' cameras, so it is only the fallback.
static void CdCamKicks(int pair, int role);
static int CdMfIntro(int p);
static int CdCamTracks(uint64_t other_h, float end_s, int role)
{
#ifdef XCAM_HAVE_CTX
    int p = CdPairOfMove(g_ps.move);
    if (p < 0 || !other_h)
        return 0;
    char nm[64];
    snprintf(nm, sizeof nm, "ctxdown:%s", CD_PAIRS[p].name);
    // a victim joins a few ms late (its START skip): the track runs on the move's clock
    float off =
        g_ps.role ? (float) ((PsyncMs() - g_ps.t0 + g_ps.chain_shift) /
                             1000.0) : 0.0f;
    if (!(off > 0.0f && off < end_s - 0.5f))
        off = 0.0f;
    // ps32 (SP zooms in smoothly on a contextual takedown, which MP did not use;
    // A 14250 'exec cam: ctxdown:mf-ellie-2h-blunt-right-ko-finisher-2 role 1 ... blend in 450 / out 1000 ms', 14306 'exec cam: +60 fov 79.43', 13081
    // 'xcam: cam-swing - the blend-in of ctxdown:mf-fist-throat-punch-kill orbited 124 deg round our head'): on the ATTACKER's screen the duel rig keeps the
    // camera through the finisher / contextual takedown and pushes in on SP's curves (melee_spcam.c s32_fin_begin: only when the rig is up on this victim);
    // no authored takedown camera is started then. The blow kicks below are still collected - the rig plays them. t2r-ps32-cam-fin-old.txt = the old camera.
    // ps34 (as the victim of a finisher the view quickly cut to the enemy's point of view; B 23454 'xcam victim:
    // ctxdown:wall-throat-punch role 2: 2013 AUTHORED camera (... apReference-camera1 of the defender clip ...)'): on the VICTIM's screen the pin camera's
    // timed shot keeps (a finisher out of a pin) or takes (a side shot of the victim) the view through the move and eases into the downed camera
    // (melee_spcam.c s34_vic_take). t2r-ps34-cam-vicfin-old.txt / t2r-ps34-cam-pinvic-old.txt = the authored victim camera as before.
    int rig32 = role == 1 && S32FinBegin(other_h, end_s - off, nm);
    int vic34 = role == 2 && S34VicTake(other_h, end_s - off, nm);
    if (!rig32 && !vic34) {
        if (!XcamStart
            (other_h, end_s - off + 0.5f, 0, -1, end_s - off, role, nm))
            return 0;
        if (!g_xcamTrk) {
            XcamStop("no authored track");
            if (role == 1
                && S32FinForce(other_h, end_s - off, nm,
                                 "the move has no authored camera track"))
                rig32 = 1;
            else
                return 0;
        }                       // (ps35: the duel rig, when it is up)
    }
    g_xcamTrkOff = off;
    if (off > 0.0f)
        Xlog("ctxdown: camera track clock starts %.0f ms in (the move's time)", off * 1000.0f);
    CdCamKicks(p, role);
    // v5 (minimal - v5-cam owns the transitions): the authored track (ctxcam_tracks.h) is the -sync pair's camera; with the SP intro in front of it
    // the track clock runs intro-length behind the move clock (key 0 holds through the windup while xcam blends in), the kicks move with it
    if (CdMfIntro(p)) {
        float il = CdSegLen(p, 0);
        g_xcamTrkOff = off - il;
        for (int i = 0; i < g_xcamKicks.n; i++)
            g_xcamKicks.t[i] -= il;
        Xlog("ctxdown: camera track of %s starts after its SP intro (%.0f ms): track clock = move time - intro", CD_PAIRS[p].name, il * 1000.0f);
    }
    if (vic34) {
        Xlog("ctxdown: camera for us (victim) of %s: the victim's own shot (ps34) - no authored takedown camera", CD_PAIRS[p].name);
        return 1;
    }
    if (rig32) {
        S32FinKicks(g_xcamTrkOff);
        Xlog("ctxdown: camera for us (attacker) of %s: the duel rig keeps the camera and pushes in (ps32; t2r-ps32-cam-fin-old.txt = the authored takedown camera)", CD_PAIRS[p].name);
        return 1;
    }
    g_cd.cam_xc = 1;
    Xlog("ctxdown: camera for us (%s): TLOU2's own authored camera of %s (%d candidates, camera %d first, %d keys)", role == 1 ? "attacker" : "victim", CD_PAIRS[p].name, g_xcam_ncand, XcamCamIx(g_xcamTrk), g_xcamTrk->n);
    return 1;
#else
    (void) other_h;
    (void) end_s;
    (void) role;
    return 0;
#endif
}

static int CdMfRow(int i);
static void CdCam(const char *clip, uint64_t other_h, float end_s,
                   int role)
{
    {
        int fp = CdPairOfMove(g_ps.move);
        // (A/B frames: the victim's authored-track camera whipped ~180 deg through its own body and then framed its back, the attacker out of view - SP has no
        // player-victim camera for these). The duel finisher's victim keeps the duel framing (melee_spcam.c: look-at on the attacker) until it is down.
        // v5-lethal: also outside a duel (the contact finisher, cd_contact_fin): the game camera stays then
        // (the victim now shows the attacker's authored shot - xcam_track_pick; t2r-fin-victim-duel-cam.txt = this old rule)
        if (role == 2 && fp >= 0 && CdMfRow(fp)
            && GetFileAttributesA("t2r-fin-victim-duel-cam.txt") !=
            INVALID_FILE_ATTRIBUTES) {
            Xlog("ctxdown: camera for us (victim) of %s: the %s stays (no takedown camera)", CD_PAIRS[fp].name, MduEngaged()? "duel framing" : "game camera");
            return;
        }
    }
    if (CdCamTracks(other_h, end_s, role))
        return;
    if (g_finCam && g_fincamTramp) {
        FinCamStart(clip);
        if (g_fincamOn) {
            g_fincam_until = g_frame + 4800;    // the 240-frame safety cap of the standing finisher is too short for a 2-3 s pair at high fps; cd_tick / the abort paths end it
            g_cd.cam_fin = 1;
            g_cd.cam_frame = g_frame;
            g_cd.cam_check = g_frame + CdCamDelayFrames() + 36;
            g_cd.cam_other = other_h;
            g_cd.cam_end = end_s;
            g_cd.cam_role = role;
            Xlog("ctxdown: camera for us (%s): the clip's own animated camera (%s)", role == 1 ? "attacker" : "victim", clip);
        } else
            Xlog("ctxdown: the finisher-camera window did not open (no camera director on the player) - the game camera stays");
    } else if (other_h
               && XcamStart(other_h, end_s + 0.5f, 0, -1, end_s, role,
                             "ctxdown")) {
        Xlog("ctxdown: camera for us (%s): finisher-camera hook off / not installed - the execution pair-framing camera", role == 1 ? "attacker" : "victim");
    } else
        Xlog("ctxdown: no camera started (finisher-camera hook and exec cam both off) - the game camera stays");
}

// every refusal at the down hit is ONE line (A/B: "no surface plan ... age 424" did not say why the scans had found nothing): the reason
// here, the plan's state, and the last thing the scans reported (g_cd.note)
static int g_cdSwingPath;     // (tentative: defined again below with cd_wear_begin)
static int CdRefuse(int32_t dslot, const char *fmt, ...)
{
    char r[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r, sizeof r, fmt, ap);
    va_end(ap);
    static int nl;
    // v5-lethal: the swing-start try (cd_swing_start) is not the knockdown - its contact decides (park_hook: cd_try, then the open-ground SP finisher, then the plain down)
    if (LOGCAP(nl, 120))
        Xlog("ctxdown: DOWN HIT slot %d -> %s: %s | plan %d (slot %d, %s, age %llu frames) | last scan: %s", dslot, g_cdSwingPath ? "no surface takedown at the swing start (the contact decides)" : "no surface takedown", r, g_cd.plan_ok, g_cd.plan_slot, g_cd.plan_ok && g_cd.plan_pair >= 0 && g_cd.plan_pair < (int) CD_N ? CD_PAIRS[g_cd.plan_pair].name : "-", (unsigned long long) (g_frame - g_cd.plan_frame), g_cd.note[0] ? g_cd.note : "(no scan note)");
    return 0;
}

// ---- empty hands for the FIST takedowns (build 4321dd8: even in the THROAT PUNCH contextual takedown the RIGHT HAND isn't animated
// (doesn't punch), same as the shiv kill). A log 33423 (male, wall-throat-punch): the fists weapon object abby-unarmed (6ac05fba9c921900, drawn by
// the punch combo) stayed in the hand through the whole move ("wpose: weapon 6ac05fba9c921900" at 33450 / 33480 / 33510), and its held-weapon arm
// layer held the right arm over the abby-fist-hwall-* clip - the same thing 4b736f0 found for the female shiv (B 136240: fists drawn for the move).
// The abby-fist takedown clips carry their own put-away (PlayerHolsterWeaponAnimAction + an item-controller Holster request every frame:
// "stow fix: PlayerHolsterWeaponAnimAction not begun" / "Holster item state request SKIPPED (attacker half)" from 0xbd91cf, 33423-33517) = SP
// plays them with EMPTY hands; our holster gates (built for the 2013 gun executions, where the gun must stay in the hand) refused it, and the
// fists were only put away after the release (A prev 126643 head smash: fists held to 126780, "wpose: no weapon" at 126810). Now a fist pair puts
// the held weapon away instantly at the START (our own call passes the gates; the downed state's instant holster empties the fists the same way,
// A 16595 -> 16620 "wpose: no weapon"), the engine's melee unholster stays off and anything that shows up in the hand is put away again until
// the attacker is released. Knife pairs (style 3) keep the knife: the ellie-knife clips are authored with it.
// the same on other screens: slot s's puppet is a half of a running fist pair (attacker or victim: the SP fist clips are authored empty-handed)
static uint32_t CdPairUid(void)
{
    return g_ps.uid;
}

// v5-armed (the 2x4's appearance was inconsistent; dlog4 25394-25653 "fist pair puppet: slot 0 item cae096b9f46d66d0 hidden for the paired move" on the
// attacker's screen while the victim's own screen kept the 2x4 in the hand - clips 15-02-45 +1.6 s / 15-04-02 +1.0 s: the enemy's 2x4 vanishes at the first punch,
// and the disarm's weapon was gone from the START instead of SP's take frame): the VICTIM's melee item is never hidden here - its own client keeps it in the hand
// (SP: the defender holds its melee weapon through the blows), a disarm hides it at the take frame (cd_dis_tick, every screen); a gun / bow / throwable of the
// victim is still emptied (its own client holsters it too: cd_vic_gun_tick). After a disarm's take frame the ATTACKER's new item (the taken weapon, given to it at
// the take frame) is not hidden either. t2r-pup-hide-vic-melee.txt = the old rule (every item of both halves hidden).
static uint64_t CdDisPeerWeapon(int slot);
static int CdDisAfterTake(int s);
// Escaping a pin with a melee weapon in the hand played the unarmed escape with the weapon still held; it should be hidden,
// and for females the knife should be out for the duration of the anim. The pin ESCAPE rows (pe-*, melee_duel_pin.c mpn_escape -> dc_launch, style 9) went through
// neither fist-pair rule (cd_hands_begin / cd_pup_hands_empty: style -1 only), so the escaper's held item stayed in the hand on both screens.
static int CdEscRow(int p)
{
    return p >= 0 && !strncmp(CD_PAIRS[p].name, "pe-", 3);
}

static int CdPupHandsEmpty(uint32_t s)
{
    if (s >= T2R_MAX_PEERS || !PsyncActive() || g_ps.kind != PS_CTXDOWN
        || g_ps.ended || !g_ps_pup[s].h)
        return 0;
    if ((int) s != g_ps.att && (int) s != g_ps.vic)
        return 0;
    int p = CdPairOfMove(g_ps.move);
    if (CdEscRow(p)) {        // the escaper's puppet (its half = att): whatever it holds is hidden (the fist pair path: visibility byte, shown again after) - a female's knife stays out
        if ((int) s != g_ps.att)
            return 0;
        uint64_t w = CdDisPeerWeapon((int) s);
        return !(w && w != ~0ULL
                 && (FemUnarmedId(w) || FemUnarmedId(BaseWeapon(w))));
    }
    if (!(p >= 0 && CD_PAIRS[p].style == -1))
        return 0;
    static int old = -1;
    static uint64_t chk;
    if (old < 0 || GetTickCount64() - chk > 3000) {
        chk = GetTickCount64();
        old =
            GetFileAttributesA("t2r-pup-hide-vic-melee.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    if (old)
        return 1;
    // (where there is no pair for the weapons there is a both-unarmed clash - the weapons stay in the hands): a parry / clash
    // pair (kind 7) never hides a melee item of either half.
    if (CD_PAIRS[p].kind == 7) {
        uint64_t w = CdDisPeerWeapon((int) s);
        if (w && w != ~0ULL
            && (MeleeClassOf(BaseWeapon(w), NULL) >= 0
                || MeleeClassOf(w, NULL) >= 0))
            return 0;
    }
    if ((int) s == g_ps.vic) {
        uint64_t w = CdDisPeerWeapon((int) s);
        if (w && w != ~0ULL
            && (MeleeClassOf(BaseWeapon(w), NULL) >= 0
                || MeleeClassOf(w, NULL) >= 0)) {
            static uint32_t lu;
            if (lu != g_ps.uid) {
                lu = g_ps.uid;
                Xlog("ctxdown: %s u%08x - the victim slot %u's melee item %llx stays in its puppet's hand (its own screen keeps it; a disarm takes it at SP's take frame)", CD_PAIRS[p].name, g_ps.uid, s, (unsigned long long) w);
            }
            return 0;
        }
    }
    if ((int) s == g_ps.att && CdDisAfterTake((int) s))
        return 0;
    return 1;
}

static uint32_t g_cdHandsUid;
static uint64_t g_cdHandsTried;
// the escaper's own body: a male's held item is hidden for the whole pair (draw-control visibility byte +0x48, the disarm / wpnbreak way: no graph change inside the
// pair) and shown again at the end; a female has the knife (her fists) OUT in the hand: any other item is put away and the knife drawn, the melee weapon she held is drawn
// back at the end; her puppet on the other screens gets the knife prop (MSG_KNIFE, re-sent while the pair runs) over whatever it holds. t2r-esc-keep-weapon.txt = the old behaviour.
static struct {
    uint32_t uid;
    uint8_t *hw, *hdc;
    uint32_t hid;
    uint8_t vis;
    uint64_t prev;
    int fem;
    uint64_t knife_next;
} g_cdEsc;
static void CdEscKnifeSend(void)
{
    int w = 1;
    FemKnifeId(&w);
    if (g_sendTramp && !g_femKnifeOff) {
        uint8_t m[1] = { (uint8_t) w };
        g_sendTramp(MSG_KNIFE, m, 1, 0xff);
    }
}

static void CdEscBegin(int pair)
{
    memset(&g_cdEsc, 0, sizeof g_cdEsc);
    if (GetFileAttributesA("t2r-esc-keep-weapon.txt") !=
        INVALID_FILE_ATTRIBUTES)
        return;
    g_cdEsc.uid = g_ps.uid;
    g_cdEsc.fem = OwnFemale() && g_femKnifeCur && !g_femKnifeOff;
    uint64_t held = XrHeld();
    if (g_cdEsc.fem) {
        if (held != g_femKnifeCur) {
            uint64_t z[2] = { 0, 0 }, r = 0;
            if (held && held != ~0ULL && held != FNV64("abby-unarmed")
                && held != FNV64("ellie-unarmed")
                && MeleeClassOf(BaseWeapon(held), NULL) >= 0)
                g_cdEsc.prev = held;
            if (held && held != ~0ULL)
                r = (g_holsterInstFrame =
                     g_frame ? g_frame : 1,
                     DcCall(FNV64("holster-player-weapon-instantly"), 2,
                             z));
            uint64_t d = XrDraw(g_femKnifeCur), now = XrHeld();
            Xlog("ctxdown: pin escape %s u%08x - female life: held %llx put away (%llx), the knife %llx drawn (%llx, held now %llx)", CD_PAIRS[pair].name, g_ps.uid, (unsigned long long) held, (unsigned long long) r, (unsigned long long) g_femKnifeCur, (unsigned long long) d, (unsigned long long) now);
        } else
            Xlog("ctxdown: pin escape %s u%08x - female life: the knife %llx is in the hand already", CD_PAIRS[pair].name, g_ps.uid, (unsigned long long) held);
        CdEscKnifeSend();
        g_cdEsc.knife_next = GetTickCount64() + 500;
    } else
        Xlog("ctxdown: pin escape %s u%08x - the held item %llx is hidden for the pair (shown again at its end)", CD_PAIRS[pair].name, g_ps.uid, (unsigned long long) held);
}

static void CdEscEnd(const char *why)
{
    if (g_cdEsc.hw && g_cdEsc.hdc && !IsBadReadPtr(g_cdEsc.hw + 0x94, 4)
        && *(uint32_t *) (g_cdEsc.hw + 0x94) == g_cdEsc.hid
        && !IsBadWritePtr(g_cdEsc.hdc + 0x48, 1)) {
        if (g_cdEsc.hdc[0x48] == 0)
            g_cdEsc.hdc[0x48] = g_cdEsc.vis ? g_cdEsc.vis : 0xff;
        Xlog("ctxdown: pin escape u%08x - the held item shown again (%s)",
             g_cdEsc.uid, why);
    }
    if (g_cdEsc.prev) {
        uint64_t cw = XrHeld(), p = g_cdEsc.prev, has =
            DcCall(FNV64("has-weapon?"), 1, &p);
        if ((cw == 0 || cw == ~0ULL || cw == g_femKnifeCur
             || cw == FNV64("abby-unarmed")) && has != ~0ULL
            && (has & 0xff) && !g_downed) {
            uint64_t r = XrDraw(p);
            Xlog("ctxdown: pin escape u%08x - the melee weapon %llx drawn back (%llx, held %llx)", g_cdEsc.uid, (unsigned long long) p, (unsigned long long) r, (unsigned long long) XrHeld());
        }
    }
    memset(&g_cdEsc, 0, sizeof g_cdEsc);
}

static void CdEscTick(void)
{
    if (!g_cdEsc.uid)
        return;
    if (!
        (g_ps.role == 1 && g_ps.kind == PS_CTXDOWN && !g_ps.ended
         && g_ps.uid == g_cdEsc.uid)) {
        CdEscEnd("the pair is over");
        return;
    }
    if (g_cdEsc.fem) {
        if (GetTickCount64() >= g_cdEsc.knife_next) {
            g_cdEsc.knife_next = GetTickCount64() + 500;
            CdEscKnifeSend();
        }
        return;
    }
    uint8_t *me = BpMe(), *w = me
        && !IsBadReadPtr(me + 0x2f60, 8) ? PlayerWeapon(me) : NULL;
    if (!w || IsBadReadPtr(w + 0x94, 4) || IsBadReadPtr(w + 0x1b0, 8))
        return;
    uint8_t *dc = *(uint8_t **) (w + 0x1b0);
    if (!dc || IsBadWritePtr(dc + 0x48, 1))
        return;
    if (g_cdEsc.hw != w || g_cdEsc.hid != *(uint32_t *) (w + 0x94)) {
        uint32_t uid = g_cdEsc.uid;
        int fem = g_cdEsc.fem;
        if (g_cdEsc.hw)
            CdEscEnd("another item in the hand");
        g_cdEsc.uid = uid;
        g_cdEsc.fem = fem;
        g_cdEsc.hw = w;
        g_cdEsc.hdc = dc;
        g_cdEsc.hid = *(uint32_t *) (w + 0x94);
        g_cdEsc.vis = dc[0x48];
        Xlog("ctxdown: pin escape u%08x - held item %llx hidden (visibility %02x -> 00)", g_cdEsc.uid, (unsigned long long) PstSidOf(w), dc[0x48]);
    }
    dc[0x48] = 0;
}

static void CdHandsBegin(int pair)
{
    g_cdHandsOn = 0;
    g_cdHandsUid = 0;
    g_cdHandsTried = 0;
    if (CdEscRow(pair) && g_ps.role == 1) {
        CdEscBegin(pair);
        return;
    }
    if (CD_PAIRS[pair].style != -1 || g_ps.role != 1
        || GetFileAttributesA("t2r-cd-keep-fists.txt") !=
        INVALID_FILE_ATTRIBUTES)
        return;
    uint64_t none0 = 0, z[2] = { 0, 0 }, r = 0;
    uint64_t held =
        DcCall(FNV64("get-player-weapon-id"), 0, &none0), now = held;
    if (CD_PAIRS[pair].kind == 7 && held && held != ~0ULL && (MeleeClassOf(BaseWeapon(held), NULL) >= 0 || MeleeClassOf(held, NULL) >= 0)) {       // (night88: a clash keeps the weapon)
        Xlog("ctxdown: clash pair %s - our melee item %llx stays in the hand (no SP pair for the two weapons: the unarmed clash plays with the weapons held)", CD_PAIRS[pair].name, (unsigned long long) held);
        return;
    }
    g_cdHandsOn = 1;
    g_cdHandsUid = g_ps.uid;
    if (held && held != ~0ULL) {
        r = (g_holsterInstFrame =
             g_frame ? g_frame : 1,
             DcCall(FNV64("holster-player-weapon-instantly"), 2, z));
        now = DcCall(FNV64("get-player-weapon-id"), 0, &none0);
    }
    DcCall(FNV64("player-disable-unholstering"), 0, &none0);
    Xlog("ctxdown: fist takedown %s - hands emptied as in SP (held %llx -> holster-player-weapon-instantly %llx -> held %llx), no unholster until the release " "(t2r-cd-keep-fists.txt = the fists stay)", CD_PAIRS[pair].name, (unsigned long long) held, (unsigned long long) r, (unsigned long long) now);
}

// the finisher's authored contact: the victim's head where it meets the surface, in the frame (ap) space: z out of the surface face, y up from the
// frame (the victim's feet). Decoded halves (tools/local/ctxcam/pose/*.npz), the frame within +-0.2 s of CD_HITS [1] where the head is nearest the
// surface. Every low-cover finisher meets a ~0.9-1.0 m top (head 1.0-1.2 m up at the face); z < 0 = the head goes over the cover.
static const struct {
    const char *move;
    float z, y;
} CD_CONTACT[] = {
    { "cover-face-smash", 0.04f, 1.02f }, { "cover-lift-n-slam", 0.02f,
                                           1.18f },
        { "cover-combo-head-smash", -0.09f, 0.99f },
    { "wall-combo-head-smash", 0.11f, 1.47f }, { "wall-throat-punch",
                                                0.09f, 1.49f },
    { "knife-cover-ice-pick", -0.40f, 1.26f }, { "knife-cover-chest-stab",
                                                -0.36f, 1.13f },
    { "knife-wall-chest-stab-long", 0.09f, 1.19f },
        { "knife-wall-left-chest-stab", 0.15f, 1.35f },
        { "knife-wall-kneed-gut-slice", 0.50f, 1.35f },
};

#define CD_HEAD_R 0.11f
// ps46 STATURE (the machete throat slash wall finisher put the machete in the middle of the face: Abby is smaller than our MP bodies).
// The attacker halves are authored on ellie-skel / abby-skel against a base-female victim; our MP bodies are base-male / base-female, which have the SAME
// bind heights (headb 1.646, pelvis 1.030: re/female-characters.md 3, t2skel.py), so every victim half fits (rec45a A [21166]-[21256]: the victim puppet's
// headb / pelvis over its root read 1.43 / 0.87, 1.52 / 0.93, 1.50 / 0.89 m at 0 / 250 / 500 ms = the decoded npc-f clip's 1.439 / 0.877, 1.520 / 0.943,
// 1.497 / 0.896). The attacker body is the clip's pose scaled about its root by the stature ratio: rec45a B [23107]-[23152] attacker puppet (male) headb /
// pelvis 1.59 / 0.99, 1.57 / 0.95, 1.52 / 0.93, 1.54 / 0.94 m vs Ellie's decoded 1.458 / 0.905, 1.422 / 0.857, 1.391 / 0.853, 1.406 / 0.856 = x1.09-1.10
// (bind ratio MP / Ellie 1.646 / 1.511 = 1.089, MP / Abby 1.646 / 1.570 = 1.048). So the weapon hand sits (ratio - 1) x its height higher than authored,
// the same for every look pair (male / female attacker x male / female victim). dy = that at the contact, from the decoded halves (S/ps46st/dec.py,
// tools/ctxcam_poses.py FK; grip = r_hand_prop_attachment, fist = r_wrist); the relative error is removed by raising the victim puppet by dy (attacker's and
// third screens) or, on the victim's own screen (its own body is the engine's), lowering the attacker puppet by dy. Own bodies are never moved.
// Ramp (smoothstep, s of the LAST segment's clip): in from the chain's start to in1, full to out0, out by out1 - before the victim's fall, so the body lands
// on the real floor. t2r-cd-stature-ps46-old.txt (re-read every 2 s) = off.
static const struct {
    const char *move;
    float dy, in1, out0, out1;
} CD_STATURE[] = {
    // machete throat slash: blade held at the throat main 0..1.57 s (grip 0.18-0.20 m from the victim's neck, grip y 1.36-1.47, mean 1.448), the slash
    // 1.57-1.83 s; victim pelvis still 0.78 m at 2.67 s, falls from 2.87 s. 0.0893 x 1.448 = 0.129 m (range 0.121-0.132): blade at y ~1.58 = the
    // victim's headb 1.46 m (the face) instead of the neck 1.35-1.41 m.
    { "wpn-machete-wall-neck-slice", 0.129f, 0.00f, 1.83f, 2.67f },
    // Abby throat punch: right fist 0.14 m from the neck at 633 ms, y 1.437 (victim neck 1.392, headb 1.491); 0.0484 x 1.437 = 0.070 m (the chin);
    // head slam 1.03 s, victim pelvis 0.89 at 1.30 s and falling after
    { "wall-throat-punch", 0.070f, 0.40f, 1.07f, 1.30f },
    // 1h blade neck chop: blade in the neck main 0..1.37 s (grip 0.34-0.36 m from the neck while the victim slides down the wall, grip y 0.97-1.31, mean 1.138);
    // 0.0893 x 1.138 = 0.102 m (0.084-0.117); victim slumps on (pelvis 0.42 m at 2.0 s, 0.25 at 2.4 s)
    { "wpn-blade-wall-neck-chop", 0.102f, 0.00f, 1.40f, 2.00f },
    // hammer / crowbar neck jab: head of the weapon at the neck main 0..0.90 s (grip 0.24-0.30 m from it, grip y 0.99-1.32, mean 1.235); 0.0893 x 1.235 =
    // 0.110 m (0.088-0.118); the victim drops from 0.9 s (pelvis 0.63 at 1.2 s, on the floor 1.4 s)
    { "wpn-hammer-wall-neck-jab", 0.110f, 0.00f, 0.87f, 1.20f },
};

// ps46b: with the intro skipped (psync t0 = now - skip0: the duel path, D2) a row with in1 0.00 got its FULL weight on the pair's
// first frame (w = pre / (pre + 0)) -> a one-frame 10-13 cm pop up + 3-8 cm sideways at the start of most finishers, added after the start-pull lerp
// (nothing eased it). Both stature weights are multiplied by a real-time smoothstep from the pair's first placement (this uid's first call here: the
// first drive frame; a chain segment switch does not restart it), CD_STATURE_RAMP_S long, on every screen. t2r-cd-stature-ramp-ps46-old.txt = no ramp.
// Proof line "ps46b stature ramp".
#define CD_STATURE_RAMP_S 0.20f /* [TUNING ps46] s */
static float CdStatureRamp(int k, double now, float w_row)
{
    static uint32_t u;
    static double t0;
    static int old = -1;
    static uint64_t chk;
    static uint32_t lu[2];
    uint64_t t64 = GetTickCount64();
    if (old < 0 || t64 - chk > 2000) {
        chk = t64;
        old =
            GetFileAttributesA("t2r-cd-stature-ramp-ps46-old.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    if (u != g_ps.uid) {
        u = g_ps.uid;
        t0 = now;
    }
    if (old)
        return 1.0f;
    float r = (float) ((now - t0) / (CD_STATURE_RAMP_S * 1000.0));
    r = r < 0.0f ? 0.0f : r > 1.0f ? 1.0f : r;
    r = r * r * (3.0f - 2.0f * r);
    if (lu[k & 1] != g_ps.uid) {
        lu[k & 1] = g_ps.uid;
        Xlog("ctxdown: ps46b stature ramp u%08x %s puppet: row weight %.2f at the pair's first placement (%.0f ms into the move) - eased in over %.2f s real time from here (t2r-cd-stature-ramp-ps46-old.txt = no ramp)", g_ps.uid, k ? "victim" : "attacker", (double) w_row, now - g_ps.t0, (double) CD_STATURE_RAMP_S);
    }
    return r;
}

static float CdStatureDy(int k, double now)
{
    static int old = -1;
    static uint64_t chk;
    uint64_t t64 = GetTickCount64();
    if (old < 0 || t64 - chk > 2000) {
        chk = t64;
        int o =
            GetFileAttributesA("t2r-cd-stature-ps46-old.txt") !=
            INVALID_FILE_ATTRIBUTES;
        if (o != old && old >= 0)
            Xlog("ctxdown: ps46 stature offsets %s (t2r-cd-stature-ps46-old.txt)", o ? "OFF" : "on");
        old = o;
    }
    if (old || g_ps.nseg < 1)
        return 0.0f;
    int p = CdPairOfMove(g_ps.move);
    if (p < 0 || CD_PAIRS[p].nseg != g_ps.nseg)
        return 0.0f;
    int r = -1;
    for (size_t i = 0; i < sizeof CD_STATURE / sizeof CD_STATURE[0]; i++)
        if (CdIsMove(CD_STATURE[i].move, p)) {
            r = (int) i;
            break;
        }
    if (r < 0)
        return 0.0f;
    int vic_own = g_ps.ent_vic == SID_PLAYER;
    if (k == 1 ? vic_own : !vic_own)
        return 0.0f;            // the victim puppet is raised; only where the victim is our own body does the attacker puppet go down
    int last = CD_PAIRS[p].nseg - 1;
    float pre = 0.0f;
    for (int i = 0; i < last; i++)
        pre += CdSegLen(p, i);
    float tin = (float) ((now - g_ps.t0) / 1000.0);     // s inside the current segment
    float tm =
        g_ps.cur_seg >= last ? tin * (CD_PAIRS[p].seg[last].spd >
                                      1.001f ? CD_PAIRS[p].seg[last].
                                      spd : 1.0f) : -1e3f;
    float tc =
        g_ps.cur_seg >=
        last ? pre +
        tm : (float) ((now - g_ps.t0 + g_ps.chain_shift) / 1000.0);
    const float in1 = CD_STATURE[r].in1, out0 = CD_STATURE[r].out0, out1 =
        CD_STATURE[r].out1;
    float w;
    if (g_ps.cur_seg < last || tm < in1) {
        float d = pre + in1;
        w = d > 0.01f ? tc / d : 1.0f;
    } else if (tm < out0)
        w = 1.0f;
    else
        w = 1.0f - (tm - out0) / (out1 - out0);
    w = w < 0.0f ? 0.0f : w > 1.0f ? 1.0f : w;
    w = w * w * (3.0f - 2.0f * w);
    w *= CdStatureRamp(k, now, w);
    float dy = (k ? 1.0f : -1.0f) * CD_STATURE[r].dy * w;
    static uint32_t lu[2];
    static int lfull[2];
    if (lu[k] != g_ps.uid) {
        lu[k] = g_ps.uid;
        lfull[k] = 0;
        Xlog("ctxdown: ps46 stature u%08x %s: the %s puppet %s by up to %.3f m (the MP attacker body = the clip's %s skeleton x%.3f; ramp in to %.2f s, out %.2f-%.2f s of the main clip; " "t2r-cd-stature-ps46-old.txt = off)", g_ps.uid, CD_PAIRS[p].name, k ? "victim" : "attacker", k ? "RAISED" : "LOWERED", (double) CD_STATURE[r].dy, !strncmp(CD_PAIRS[p].seg[last].att, "abby-", 5) ? "Abby" : "Ellie", !strncmp(CD_PAIRS[p].seg[last].att, "abby-", 5) ? 1.646 / 1.570 : 1.646 / 1.511, (double) in1, (double) out0, (double) out1);
    }
    if (w >= 0.999f && !lfull[k]) {
        lfull[k] = 1;
        Xlog("ctxdown: ps46 stature u%08x %s: %s puppet at the full %+.3f m (main clip %.2f s)", g_ps.uid, CD_PAIRS[p].name, k ? "victim" : "attacker", (double) dy, (double) tm);
    }
    return dy;
}

// ps46 FINISHER STATURE (mf-switchblade-quick-left-ice-pick: the knife stabbed into the air behind the victim's head
// instead of the neck as in SP). The TLOU2 one-shot / duel finisher rows (CD_PAIRS kind 0: "mf-*", the upgraded one-shots) are psync PS_CTXDOWN pairs too
// (A [112604] "PLAY role 1 kind 6") but none was in CD_STATURE, so their attacker half played at the MP body's x1.089 (Ellie rig) / x1.048 (Abby rig) with
// the victim at the clip's height. Scaled about its root, the attacker's weapon hand lands (s - 1) x its height HIGHER and (s - 1) x its reach from the
// attacker's root FURTHER along the pair line (past the victim's neck = behind the head). Same fix as CD_STATURE: the victim puppet raised by dy and pushed
// away from the attacker by dz along the pair line (attacker's and third screens); on the victim's own screen the attacker puppet lowered by dy and moved
// back by dz. Own bodies never move. Measured rows: the MAIN clip pair decoded (S/ps46st/dec.py, dec2.py; ana.py / ana2.py / ana3.py), the contact = the
// frames where the striking hand / weapon is on the victim's head or neck; dy = (s-1) x the hand's mean height there, dz = (s-1) x the hand's mean reach
// from the attacker root along the line attacker root -> victim neck; ang = that line's mean direction off the ap's -z (deg, + = towards ap -x) - a weapon
// tip moves with its hand, so the hand's error is the weapon's. Ramp times are s of the main clip (in to in1 from the chain's start; out out0 -> out1, ending
// before the victim falls). s = 1.089 Ellie rows, 1.048 Abby rows. Played in the test logs: every kind 0 row of those logs is here.
static const struct {
    const char *move;
    float dy, dz, ang, in1, out0, out1;
} CD_FIN_STATURE[] = {
    // quick left ice pick: blade at the neck main 0..0.37 s (grip 0.15-0.17 m from the victim's neck, grip y 1.40-1.51, mean 1.456; reach 0.67-0.74, mean
    // 0.698-0.709, line +4..+9 deg); 0.0893 x 1.456 = 0.130 m (0.125-0.135), dz 0.062 (0.060-0.066); the knife pulls out 0.37-0.50 s, the victim is shoved
    // off (pelvis 0.81 m at 1.0 s, 0.51 at 1.2 s, on the floor 1.4 s)
    { "mf-switchblade-quick-left-ice-pick", 0.130f, 0.062f, 6.0f, 0.00f,
     0.40f, 1.00f },
    // knife neck throw-down (npc-m victim): knife swings in 0.20-0.30 s, blade at the neck 0.33-1.07 s (grip 0.16-0.24 m from the neck, y 1.20-1.37, mean
    // 1.291; reach 0.62-0.75, mean 0.659; line +6..+28 deg): dy 0.115 (0.108-0.122), dz 0.059 (0.053-0.067); thrown down 1.1-1.33 s (pelvis 0.71 at 1.13 s)
    { "mf-switchblade-neck-2-finisher", 0.115f, 0.059f, 18.0f, 0.30f,
     1.07f, 1.25f },
    // machete neck chop: blade in the neck main 0..0.40 s (grip 0.30-0.37 m from the neck = the blade, y 1.08-1.18, mean 1.137; reach mean 0.854, line +2..+7):
    // dy 0.102 (0.096-0.105), dz 0.076 (0.074-0.078); the victim staggers off upright (pelvis 0.89-0.99 to 1.4 s), falls from 1.5 s
    { "mf-machete-slash-2-finisher", 0.102f, 0.076f, 5.0f, 0.00f, 0.45f,
     1.00f },
    // 1h blunt head stop: weapon held on the head main 0..0.40 s (grip 0.35-0.52 m from the head, y 1.23-1.44, mean 1.353; reach mean 0.604, line -33..-3):
    // dy 0.121 (0.110-0.128), dz 0.054 (0.046-0.068); the victim falls from 0.5 s (pelvis 0.72 at 0.60 s)
    { "mf-1h-blunt-left-head-stop-finisher", 0.121f, 0.054f, -21.0f, 0.00f,
     0.40f, 0.55f },
    // 1h blunt swing-through: the blow at main f0-f1 (grip 0.34 m from the head, y 1.47-1.50; victim head speed peak f1; reach 0.49-0.56, line -5..-2):
    // dy 0.133 (0.132-0.134), dz 0.047 (0.044-0.050); upright to 0.6 s (pelvis 0.92), falls 0.7-1.1 s
    { "mf-1h-blunt-left-swing-through-finisher", 0.133f, 0.047f, -3.0f,
     0.00f, 0.05f, 0.50f },
    // 2h blunt right KO 1: bat on the head at main f0 (hand 0.89 m from the head = the bat, y 1.16-1.21; head speed peak 100 ms; reach 0.36-0.46, line +2..+4):
    // dy 0.106 (0.103-0.108), dz 0.036 (0.032-0.041); pelvis 0.89 to 0.2 s, 0.77 at 0.3 s, down 0.6 s - the ramp is out by 0.27 s
    { "mf-ellie-2h-blunt-right-ko-finisher-1", 0.106f, 0.036f, 3.0f, 0.00f,
     0.07f, 0.27f },
    // 2h blunt left KO 2: bat on the head at main f0 (hand 0.86 m from the head, y 1.08-1.21; head speed peak 7.9 m/s f1; reach 0.56-0.61, line -9..0):
    // dy 0.102 (0.097-0.108), dz 0.052 (0.050-0.055); the head drops from f1, pelvis 0.90 to 0.27 s - out by 0.20 s
    { "mf-ellie-2h-blunt-left-ko-finisher-2", 0.102f, 0.052f, -4.0f, 0.00f,
     0.03f, 0.20f },
    // Abby fist left hook: left fist on the head at main f0 (l_wrist 0.18 m from the head, y 1.29-1.37; reach 0.55-0.62, line -13..-11): dy 0.066
    // (0.059-0.066), dz 0.030 (0.023-0.030); victim upright (pelvis 0.86-0.90) to 0.8 s, falls 0.8-1.2 s
    { "mf-fist-left-hook-kill", 0.066f, 0.030f, -12.0f, 0.00f, 0.10f,
     0.60f },
    // Abby fist nose break: left hand holds the head main 0..1.13 s (l_wrist 0.11-0.18 m from the neck, y mean 1.290 -> 0.062), right hand on the face
    // 0.40-1.13 s (r_wrist 0.12-0.17 m from the head, y mean 1.488 -> 0.072): dy 0.067 (0.058-0.074); reach 0.29-0.39 -> dz 0.014-0.019, under 2 cm: not
    // applied; thrown down from 1.2 s (pelvis 0.62 at 1.33 s)
    { "mf-fist-nose-break-kill", 0.067f, 0.000f, 0.0f, 0.00f, 1.13f,
     1.30f },
};

static int CdFinStatureOld(void)
{
    static int old = -1;
    static uint64_t chk;
    uint64_t t64 = GetTickCount64();
    if (old < 0 || t64 - chk > 2000) {
        chk = t64;
        int o =
            GetFileAttributesA("t2r-fin-stature-ps46-old.txt") !=
            INVALID_FILE_ATTRIBUTES;
        if (o != old && old >= 0)
            Xlog("ctxdown: ps46 finisher stature offsets %s (t2r-fin-stature-ps46-old.txt)", o ? "OFF" : "on");
        old = o;
    }
    return old;
}

// 1 = the GUESSED generic dy for unmeasured kind 0 rows is on (t2r-fin-stature-generic-ps46-on.txt, re-read every 2 s). Off by default (a guessed
//  ~13 cm lift floats a lying / falling victim): only CD_FIN_STATURE's measured rows apply
static int CdFinStatureGenericOn(void)
{
    static int on = -1;
    static uint64_t chk;
    uint64_t t64 = GetTickCount64();
    if (on < 0 || t64 - chk > 2000) {
        chk = t64;
        int o =
            GetFileAttributesA("t2r-fin-stature-generic-ps46-on.txt") !=
            INVALID_FILE_ATTRIBUTES;
        if (o != on && on >= 0)
            Xlog("ctxdown: ps46 finisher stature generic (unmeasured rows) %s (t2r-fin-stature-generic-ps46-on.txt)", o ? "ON" : "off");
        on = o;
    }
    return on;
}

// adds the stature offset to a PUPPET half's placement t (world xyz): CD_STATURE rows (cd_stature_dy, vertical only) and the kind 0 finishers
static void CdStatureApply(int k, double now, float *t)
{
    int p = g_ps.nseg >= 1 ? CdPairOfMove(g_ps.move) : -1;
    if (p < 0 || CD_PAIRS[p].kind != 0) {
        t[1] += CdStatureDy(k, now);
        return;
    }
    if (CdFinStatureOld() || CD_PAIRS[p].nseg != g_ps.nseg)
        return;
    int vic_own = g_ps.ent_vic == SID_PLAYER;
    if (k == 1 ? vic_own : !vic_own)
        return;                 // the victim puppet moves; only where the victim is our own body does the attacker puppet
    int last = CD_PAIRS[p].nseg - 1, abby =
        !strncmp(CD_PAIRS[p].seg[last].att, "abby-", 5);
    float s = abby ? 1.646f / 1.570f : 1.646f / 1.511f;
    int r = -1;
    for (size_t i = 0;
         i < sizeof CD_FIN_STATURE / sizeof CD_FIN_STATURE[0]; i++)
        if (CdIsMove(CD_FIN_STATURE[i].move, p)) {
            r = (int) i;
            break;
        }
    float DY, DZ, ANG = 0.0f, in1, out0, out1;
    if (r >= 0) {
        DY = CD_FIN_STATURE[r].dy;
        DZ = CD_FIN_STATURE[r].dz;
        ANG = CD_FIN_STATURE[r].ang;
        in1 = CD_FIN_STATURE[r].in1;
        out0 = CD_FIN_STATURE[r].out0;
        out1 = CD_FIN_STATURE[r].out1;
    } else {
        if (!CdFinStatureGenericOn())
            return;             // unmeasured row: nothing unless opted in
        // [TUNING ps46] unmeasured row (opt-in only): the generic scale-based dy at an assumed contact height (1.40 m = the npc-f neck 1.33-1.41; 1.10 m for a gut / chest
        // / leg row), no along-the-line dz (not derived for it); ramp full to the row's first contact + 0.35 s, out by 0.8 x its end time (the ice pick's
        // measured 0.40 / 1.00 s against its 0.033 / 1.26 s row times)
        const char *n = CD_PAIRS[p].name;
        float h = strstr(n, "gut") || strstr(n, "chest")
            || strstr(n, "leg-") ? 1.10f : 1.40f;
        DY = (s - 1.0f) * h;
        DZ = 0.0f;
        in1 = 0.0f;
        out0 = CD_PAIRS[p].dmax + 0.35f;
        out1 = CD_PAIRS[p].end_s * 0.8f;
        if (out1 < out0 + 0.30f)
            out1 = out0 + 0.30f;
    }
    float pre = 0.0f;
    for (int i = 0; i < last; i++)
        pre += CdSegLen(p, i);
    float tin = (float) ((now - g_ps.t0) / 1000.0);
    float tm =
        g_ps.cur_seg >= last ? tin * (CD_PAIRS[p].seg[last].spd >
                                      1.001f ? CD_PAIRS[p].seg[last].
                                      spd : 1.0f) : -1e3f;
    float tc =
        g_ps.cur_seg >=
        last ? pre +
        tm : (float) ((now - g_ps.t0 + g_ps.chain_shift) / 1000.0);
    float w;
    if (g_ps.cur_seg < last || tm < in1) {
        float d = pre + in1;
        w = d > 0.01f ? tc / d : 1.0f;
    } else if (tm < out0)
        w = 1.0f;
    else
        w = 1.0f - (tm - out0) / (out1 - out0);
    w = w < 0.0f ? 0.0f : w > 1.0f ? 1.0f : w;
    w = w * w * (3.0f - 2.0f * w);
    w *= CdStatureRamp(k, now, w);
    const float *q = g_ps.ap + 3;       // ap forward = q * (0,0,1): ap-space +z, toward the attacker (authored attacker spot z ~+0.7)
    float fx = 2.0f * (q[0] * q[2] + q[3] * q[1]), fz =
        1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]), fn =
        __builtin_sqrtf(fx * fx + fz * fz);
    if (fn > 0.5f) {
        fx /= fn;
        fz /= fn;
    } else {
        fx = fz = 0.0f;
    }
    // the line attacker -> victim in ap space = (-sin ang, -cos ang); ap +x maps to world (fz, -fx) (yaw-only ap), ap +z to (fx, fz)
    float ar = ANG * 0.0174533f, ux = -__builtin_sinf(ar), uz =
        -__builtin_cosf(ar);
    float lx = ux * fz + uz * fx, lz = -ux * fx + uz * fz;
    float dy = (k ? 1.0f : -1.0f) * DY * w, dz = (k ? 1.0f : -1.0f) * DZ * w;   // victim: up + away from the attacker; attacker: down + back from the victim
    t[0] += lx * dz;
    t[1] += dy;
    t[2] += lz * dz;
    static uint32_t lu[2];
    static int lfull[2];
    const char *scr =
        g_ps.role == 1 ? "attacker's" : g_ps.role ==
        2 ? "victim's" : "third player's";
    if (lu[k] != g_ps.uid) {
        lu[k] = g_ps.uid;
        lfull[k] = 0;
        Xlog("ctxdown: ps46 fin stature u%08x %s (%s row): on the %s screen the %s puppet %s by up to %.3f m and %s by %.3f m along the pair line (%+.0f deg off the ap -z) " "(MP attacker body = the clip's %s skeleton x%.3f; ramp in to %.2f s, out %.2f-%.2f s of the main clip; t2r-fin-stature-ps46-old.txt = off)", g_ps.uid, CD_PAIRS[p].name, r >= 0 ? "measured" : "generic [TUNING ps46]", scr, k ? "victim" : "attacker", k ? "RAISED" : "LOWERED", (double) DY, k ? "pushed away from the attacker" : "moved back from the victim", (double) DZ, (double) ANG, abby ? "Abby" : "Ellie", (double) s, (double) in1, (double) out0, (double) out1);
    }
    if (w >= 0.999f && !lfull[k]) {
        lfull[k] = 1;
        Xlog("ctxdown: ps46 fin stature u%08x %s: %s puppet at the full dy %+.3f dz %+.3f m (main clip %.2f s, %s screen)", g_ps.uid, CD_PAIRS[p].name, k ? "victim" : "attacker", (double) dy, (double) dz, (double) tm, scr);
    }
}

// 1 = the finisher's contact is ON the cover top (head onto it / bent back over it, z <= 0.1 at 1.0-1.3 m up): it needs the ~0.9 m cover the
// finishers are authored on (the step fit); the lift-n-slam ends on the floor in front of the face
static int CdTopContact(int p)
{
    if (CD_PAIRS[p].kind != 2)
        return 0;
    for (size_t i = 0; i < sizeof CD_CONTACT / sizeof CD_CONTACT[0]; i++)
        if (CdIsMove(CD_CONTACT[i].move, p))
            return !CdIsMove("cover-lift-n-slam", p);
    return 0;
}

// one line at START: the gap between the measured surface and the finisher's contact, and what carries the victim across it (the push or none)
static void CdContactLog(int pair, uint64_t h, const float *V,
                           const float *ap)
{
    int c = -1;
    for (size_t i = 0; i < sizeof CD_CONTACT / sizeof CD_CONTACT[0]; i++)
        if (CdIsMove(CD_CONTACT[i].move, pair))
            c = (int) i;
    float stand =
        __builtin_sqrtf((V[0] - ap[0]) * (V[0] - ap[0]) +
                        (V[2] - ap[2]) * (V[2] - ap[2]));
    float c0[8], cf[8];
    int last = CD_PAIRS[pair].nseg - 1;
    float first =
        CdOffsetAt(h, CD_VIC0(pair), 0.0f,
                     c0) ? __builtin_sqrtf(c0[0] * c0[0] +
                                           c0[2] * c0[2]) : -1.0f;
    float fin =
        CdOffsetAt(h, CD_PAIRS[pair].seg[last].vic, 0.0f,
                     cf) ? __builtin_sqrtf(cf[0] * cf[0] +
                                           cf[2] * cf[2]) : -1.0f;
    float cz = c >= 0 ? CD_CONTACT[c].z : 0.0f, cy =
        c >= 0 ? CD_CONTACT[c].y : 0.0f;
    char surf[160], vert[96], carry[256];
    if (g_cd.plan_fit > 0.0f)
        snprintf(surf, sizeof surf,
                 "the cover face %.2f m behind a %.2f m step's face (step fit)",
                 g_cd.plan_fit, g_cd.plan_step);
    else if (CD_PAIRS[pair].kind == 2 && !CdTopContact(pair))
        snprintf(surf, sizeof surf,
                 "the measured (first) face - floor contact, no step fit");
    else
        snprintf(surf, sizeof surf, "the measured face");
    vert[0] = 0;
    if (CD_PAIRS[pair].kind == 2 && c >= 0 && g_cd.plan_top > 0.0f)
        snprintf(vert, sizeof vert,
                 ", vertical %.2f m (authored top ~%.2f, measured %.2f)",
                 cy - CD_HEAD_R - g_cd.plan_top, cy - CD_HEAD_R,
                 g_cd.plan_top);
    if (CdHasPush(pair))
        snprintf(carry, sizeof carry,
                 "PUSH %s (victim authored %.2f m out at its first frame) carries it %.2f m to the finisher's start %.2f m out, the finisher %.2f m on to the contact",
                 CD_VIC0(pair), first, stand - fin, fin, fin - cz);
    else
        snprintf(carry, sizeof carry,
                 "no push (open finisher: its own clip%s carries the victim %.2f m from its start %.2f m out to the contact)",
                 CD_PAIRS[pair].nseg > 1 ? "s (intro + main)" : "",
                 (first >= 0.0f ? first : stand) - cz, first);
    Xlog("ctxdown: contact gap %s: frame on %s; the victim stands %.2f m out; the contact (victim head) is authored %.2f m out, %.2f m up -> gap at the contact %.2f m " "horizontal%s; %s%s", CD_PAIRS[pair].name, surf, stand, cz, cy, cz > CD_HEAD_R ? cz - CD_HEAD_R : 0.0f, vert, carry, c < 0 ? " (no contact data for this move)" : "");
}

// A weapon takedown that REPLACES the downing swing at its start (cd_swing_start: the swing is cancelled, no engine contact) costs the weapon the one durability
// point that swing's contact would have (durability cost like a normal hit); one that starts from the connected hit (the park hook path) was already
// worn by the engine's own contact. The point is taken when the attacker is released: setting the health to 0 breaks the weapon, which must not happen mid-clip.
static int g_cdSwingPath;     // cd_swing_start is calling cd_try (no contact happened)
static int g_cdFinSkipIntro; // v5-lethal: cd_upg_go passes an mf- finisher's SP intro over (cd_contact_fin: the blow already landed)
static int g_cdFinSyncSpot;  // v5-sfx: cd_upg_go turned this pair on the -sync attacker half's frame-0 spot (reset per call)
static int g_cdFinWindup;     // v5-finwind (melee_duel.c): the intro passed over already played on us as the swing's windup
static int g_cdFinGateKeep;  // ps41 (melee_duel.c mfg_keep): pair + 1 = this finisher's intro started on a path gate YES for these spots - a gate NO now does not refuse it
static int g_cdFinProbe;      // v5-finwind (melee_duel.c mdu_fw_probe): cd_upg_go runs up to its path gate only (3 = clear, nothing started)
static uint64_t g_cd_wear_w, g_cd_wear_at;
static int64_t g_cdWearTo;
static void CdWearBegin(uint64_t w, float end_s)
{
    if (!w || !g_cdSwingPath)
        return;
    if (SkSteady() >= 2) {
        Xlog("skills: STEADY STANCE 2: weapon takedown %llx costs no durability", (unsigned long long) w);
        return;
    }
    uint64_t none = 0, h = DcCall(SID_GET_WEAPON_HEALTH2, 0, &none);
    if (h == ~0ULL || (int64_t) h <= 0)
        return;
    g_cd_wear_w = w;
    g_cdWearTo = (int64_t) h - 1;
    g_cd_wear_at = g_t60 + S60(end_s) + 45;
    Xlog("ctxdown: weapon takedown %llx - durability %lld -> %lld when the attacker is released (the replaced swing's contact)", (unsigned long long) w, (long long) h, (long long) g_cdWearTo);
}

static void CdWearTick(void)
{
    if (!g_cd_wear_at || g_t60 < g_cd_wear_at)
        return;
    g_cd_wear_at = 0;
    uint64_t none = 0, h = DcCall(SID_GET_WEAPON_HEALTH2, 0, &none);
    if (CurrentWeapon() != g_cd_wear_w || h == ~0ULL
        || (int64_t) h <= g_cdWearTo)
        return;                 // another weapon / already worn
    uint64_t nh = (uint64_t) g_cdWearTo;
    DcCall(SID_SET_WEAPON_HEALTH2, 1, &nh);
    Xlog("ctxdown: weapon takedown over - weapon health %lld -> %lld",
         (long long) h, (long long) g_cdWearTo);
}

// attacker, from the Melee Down branch of start_melee_hook / the confirmed down hit: 1 = the contextual down started (the caller sends no hit),
// 0 = nothing happened, the normal knockdown follows
static int MpnDownTakeover(int slot); // melee_duel_pin.c
static int CdTry(int32_t dslot)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    if (!CdEnabled())
        return 0;
    if (dslot >= 0 && dslot < T2R_MAX_PEERS
        && Mv3Stand("ctxdown: takedown start", (uint32_t) dslot))
        return CdRefuse(dslot,
                         "melee v3: a melee bubble owns this fight");
    if (g_ps.role && dslot >= 0 && dslot < T2R_MAX_PEERS && !g_downed
        && MpnDownTakeover(dslot))
        return 1;               // our pin pair on this victim: its takedown
    if (dslot < 0 || dslot >= T2R_MAX_PEERS || g_ps.role || g_downed
        || g_peerDowned[dslot])
        return CdRefuse(dslot,
                         "not eligible now (slot ok %d, in a paired move %d, we are downed %d, victim downed %d)",
                         dslot >= 0
                         && dslot < T2R_MAX_PEERS, g_ps.role != 0,
                         g_downed, dslot >= 0 && dslot < T2R_MAX_PEERS
                         && g_peerDowned[dslot]);
    if (!g_cd.plan_ok || g_cd.plan_slot != dslot)
        return CdRefuse(dslot, "no plan for this victim");
    uint64_t age = g_frame - g_cd.plan_frame;
    int style = CdCurStyle();
    if (style == -2)
        return CdRefuse(dslot, "the held-weapon check failed");
    int pair = g_cd.plan_pair;
    if (!CdStyleOk(style, CD_PAIRS[pair].style))
        return CdRefuse(dslot,
                         "the plan's pair %s is for style %d, we hold style %d now",
                         CD_PAIRS[pair].name, CD_PAIRS[pair].style, style);
    uint8_t *peer = ((fn_peer) T2rPeer) ((uint32_t) dslot);
    uint64_t h = peer ? *(uint64_t *) (peer + 0x60) : 0;
    float vb[16], mb[16];
    const float *vl = h ? PuppetLocator(h, vb) : NULL, *me =
        PlayerLocator(mb);
    if (!vl || !me || !PsLocOk(vl) || !PsLocOk(me))
        return CdRefuse(dslot, "no locators (victim %d, us %d)",
                         vl != NULL, me != NULL);
    float V[3] = { vl[0], vl[1], vl[2] }, mv, da, sa[3];
    if (age > CD_PLAN_LIFE) {   // a stale plan (a long combo): good while both bodies still stand about where the plan was made
        float vd =
            __builtin_sqrtf((V[0] - g_cd.plan_V[0]) * (V[0] -
                                                       g_cd.plan_V[0]) +
                            (V[2] - g_cd.plan_V[2]) * (V[2] -
                                                       g_cd.plan_V[2]));
        float ad =
            __builtin_sqrtf((me[0] - g_cd.plan_me[0]) * (me[0] -
                                                         g_cd.plan_me[0]) +
                            (me[2] - g_cd.plan_me[2]) * (me[2] -
                                                         g_cd.plan_me[2]));
        if (age > CD_PLAN_STALE || vd > 0.9f || ad > 1.5f)
            return CdRefuse(dslot,
                             "the plan is stale and the bodies moved (age %llu > %d or victim moved %.2f m > 0.9 or we moved %.2f m > 1.5)",
                             (unsigned long long) age, CD_PLAN_STALE, vd,
                             ad);
        Xlog("ctxdown: stale plan used (age %llu frames, victim moved %.2f m, we %.2f m)", (unsigned long long) age, vd, ad);
    }
    // where the push / intro starts: re-chosen for where the bodies stand NOW (the plan may be a combo old)
    float p0 = 0.0f, vyaw = 0.0f;
    if (!CdSpotsAt
        (pair, h, g_cd.plan_ap, V, me, p0, &mv, &da, sa, &vyaw))
        return CdRefuse(dslot,
                         "the clips' align channels are unreadable (no authored spots)");
    {                           // the bodies moved since the plan (a combo): the planned move's push variant that fits them best NOW
        int bp = pair;
        float bs = CdVariantScore(mv, da), bmv = mv, bda = da, bsa[3] =
            { sa[0], sa[1], sa[2] }, bvy = vyaw;
        for (int q = 0; q < (int) CD_N; q++) {
            float qmv, qda, qsa[3], qvy;
            if (q == pair || !CdSameMove(q, pair) || !CdPairLoaded(q)
                || !CdSpotsAt(q, h, g_cd.plan_ap, V, me, 0.0f, &qmv,
                                &qda, qsa, &qvy))
                continue;
            if (CdVariantScore(qmv, qda) < bs) {
                bs = CdVariantScore(qmv, qda);
                bp = q;
                bmv = qmv;
                bda = qda;
                memcpy(bsa, qsa, sizeof bsa);
                bvy = qvy;
            }
        }
        if (bp != pair) {
            Xlog("ctxdown: push variant %s -> %s (fits the bodies better now: victim %.2f m, attacker %.2f m)", CD_PAIRS[pair].name, CD_PAIRS[bp].name, bmv, bda);
            pair = bp;
        }
        mv = bmv;
        da = bda;
        memcpy(sa, bsa, sizeof bsa);
        vyaw = bvy;
    }
    if (mv > CdVicMax(pair) + 0.4f)
        return CdRefuse(dslot,
                         "the victim moved: its authored spot for %s is now %.2f m away (max %.2f)",
                         CD_PAIRS[pair].name, mv, CdVicMax(pair) + 0.4f);
    {                           // ps34 (A [15877]-[15892]: the plan put the surface 0.47 m behind slot 1; 15 frames later its puppet - carried on by its dodge replica, no
        // collision - stood 0.17 m on the FAR side of that surface ('victim root ... authored 0.04 0.67 ... placed -0.17 -0.17 ... err 0.86 m', 'victim carried
        // 0.97 m') and the takedown started anyway: 3 s of both bodies held 0.36-0.55 m off their clips against the wall). A victim that stands behind the
        // surface its half starts in front of cannot be taken through it: no takedown now (the caller's fallback - finisher / plain down blow - takes the blow).
        // t2r-cd-far-side-old.txt = the takedown starts anyway.
        float cv0[8];
        if (GetFileAttributesA("t2r-cd-far-side-old.txt") ==
            INVALID_FILE_ATTRIBUTES
            && CdOffsetAt(h, CD_VIC0(pair), p0, cv0)) {
            float yr = CdApYaw(g_cd.plan_ap) * 0.0174533f, zn =
                (V[0] - g_cd.plan_ap[0]) * __builtin_sinf(yr) + (V[2] -
                                                                 g_cd.
                                                                 plan_ap
                                                                 [2]) *
                __builtin_cosf(yr);
            if (__builtin_fabsf(cv0[2]) > 0.10f && zn * cv0[2] < 0.0f
                && __builtin_fabsf(zn) > 0.05f)
                return CdRefuse(dslot,
                                 "the victim stands %.2f m on the far side of the surface %s is anchored on (its half starts %.2f m in front of it) - no takedown through the surface (ps34)",
                                 __builtin_fabsf(zn), CD_PAIRS[pair].name,
                                 __builtin_fabsf(cv0[2]));
        }
    }
    {                           // SP's front rule at the move start (the victim may have turned during the combo): never a > 90 deg spin of the victim half
        float lb[16];
        const float *pl = PsyncLoc(h, lb);
        float turn = pl
            && PsLocOk(pl) ?
            __builtin_fabsf(CdWrap180(vyaw - Uc4Yaw(pl))) : 0.0f;
        if (turn > 90.0f
            && GetFileAttributesA("t2r-cd-sp-facing.txt") !=
            INVALID_FILE_ATTRIBUTES)
            return CdRefuse(dslot,
                             "the victim faces %.0f deg off its authored facing for %s (SP: attacker in the victim's front half, max 90)",
                             turn, CD_PAIRS[pair].name);
    }
    float end_s = CdChainTotal(pair); // REAL seconds of the whole chain: the segments before the finisher + its hand-off (+ 0.10 s so the slide down the surface settles)
    const float *ap = g_cd.plan_ap;
    Xlog("ctxdown: START slot %d %s: %d segments (first %s on us, %s on them), ap (bf0, src %s) %.2f %.2f %.2f yaw %.0f, hand-off at %.2f s real; starts %.2f s into the first segment " "(the push plays from its first frame); victim carried %.2f m, we %.2f m", dslot, CD_PAIRS[pair].name, CD_PAIRS[pair].nseg, CD_ATT0(pair), CD_VIC0(pair), g_cd.plan_src ? "rays" : "engine", ap[0], ap[1], ap[2], CdApYaw(ap), end_s, p0, mv, da);
    g_psStartSkipMs = p0 * 1000.0f;  // psync_start: the move clock starts p0 in (sent as the move time: every client plays from there)
    for (int i = 0; i < CD_PAIRS[pair].nseg; i++)
        Xlog("ctxdown:   segment %d: %s / %s x%.2f, %.2f s real%s", i + 1,
             CD_PAIRS[pair].seg[i].att, CD_PAIRS[pair].seg[i].vic,
             CD_PAIRS[pair].seg[i].spd,
             i + 1 < CD_PAIRS[pair].nseg ? CdSegLen(pair,
                                                      i) :
             CdFinLen(pair),
             i + 1 < CD_PAIRS[pair].nseg ? "" : " (to the hand-off)");
    CdContactLog(pair, h, V, ap);
    // v5-dis2 (A log 44017-44020: own-held 2bb2c011 -> 0 by the finisher windup's put-away 3 frames before this START, the 1h-blade wall hack then played with an
    // empty hand on both screens): cd_cur_style reads player-has-non-basic-melee-weapon? (OWNED, not held), so a weapon takedown could start with the item on
    // the back / holster. SP's weapon takedowns always have it in the hand: an owned melee item that is not in the hand is drawn here (xr_begin: instant draw +
    // hand check), and it must fit the pair's weapon set. Every weapon takedown is recorded like cd_upg_go's one-shots (xr_begin), so the release restores the
    // item if the engine's auto-holster takes it when the gates open. t2r-cd-no-weapon-draw.txt = old behaviour (no draw, no record).
    int cd_xr = 0;
    if (style >= CD_WSET_BASE
        && GetFileAttributesA("t2r-cd-no-weapon-draw.txt") ==
        INVALID_FILE_ATTRIBUTES) {
        uint64_t cw = CurrentWeapon();
        if (cw == ~0ULL)
            cw = 0;
        if (!HfixMeleeHeld()) {
            uint64_t mw = XrMeleeWeapon(cw);
            int ism = mw && (MeleeClassOf(mw, NULL) >= 0
                             || MeleeClassOf(BaseWeapon(mw),
                                               NULL) >= 0);
            if (!ism)
                return CdRefuse(dslot,
                                 "weapon takedown %s: we own a melee item but none is in the hand or known (held %llx, last %llx)",
                                 CD_PAIRS[pair].name,
                                 (unsigned long long) cw,
                                 (unsigned long long) g_lastMelee);
            if (!CdStyleOk
                (CD_WSET_BASE + CdWeaponMask(mw), CD_PAIRS[pair].style))
                return CdRefuse(dslot,
                                 "weapon takedown %s: the owned melee item %llx (not in the hand, held %llx) is not of the pair's weapon set",
                                 CD_PAIRS[pair].name,
                                 (unsigned long long) mw,
                                 (unsigned long long) cw);
            Xlog("ctxdown: weapon takedown %s - the owned melee item %llx is not in the hand (held %llx): drawn for the move", CD_PAIRS[pair].name, (unsigned long long) mw, (unsigned long long) cw);
            if (!XrBegin
                (mw, mw,
                 "contextual weapon takedown (drawn at its start)"))
                return CdRefuse(dslot,
                                 "weapon takedown %s: the draw of %llx did not take",
                                 CD_PAIRS[pair].name,
                                 (unsigned long long) mw);
            cd_xr = 1;
        }
    }
    uint64_t wpn = style >= CD_WSET_BASE ? CurrentWeapon() : 0;        // a weapon takedown: the victim's downer credit shows the weapon
    if (wpn == ~0ULL)
        wpn = 0;
    if (!PsyncStart
        (PS_CTXDOWN, (uint32_t) dslot, CD_ATT0(pair), CD_VIC0(pair), ap,
         end_s, 0, 0xff, wpn, FNV64(CD_PAIRS[pair].name)))
        return CdRefuse(dslot,
                         "psync refused the pair (see the psync lines)");
    g_cd.ended = 0;
    g_cd.plan_ok = 0;
    g_cd.last_pair = pair + 1;
    if (wpn && !cd_xr
        && GetFileAttributesA("t2r-cd-no-weapon-draw.txt") ==
        INVALID_FILE_ATTRIBUTES)
        XrBegin(wpn, wpn, "contextual weapon takedown");       // (v5-dis2: restored at the release)
    CdWearBegin(wpn, end_s);  // a weapon takedown costs the weapon one durability point, like the swing it replaces
    CdHandsBegin(pair);       // fists: empty hands for the move (the held fists' arm layer froze the punching arm)
    // the execution's attacker side (gx_melee_begin role 1): winmm's pose/locator windows, finpos (collision off + log), faces
    g_visVicAnim = CD_VIC0(pair);
    g_vis_vic_puppet = h;
    g_vis_vic_frame = g_t60;
    g_mpose_att_frame = g_vis_vic_frame;
    g_mposeAttSlot = (int32_t) dslot;
    g_finposVictim = (int32_t) dslot;
    g_finposSlot = -1;
    g_finPlaceFrame = 0;
    g_execAttFrame = 0;
    FinposBegin(1, (int) dslot, 0, S60(end_s) + 60,
                 "contextual down, attacker");
    XfaceStart(SID_PLAYER, h, end_s + 0.6f);
    CdCam(CD_ATT0(pair), h, end_s, 1);
    g_cdUpgSlot = -1;
    g_cdUpgW = 0;
    return 1;
}

// ---------------------------------------------------------------------------------------------------- the upgraded 1h melee one-shot
// t2r-ext.c upg13_start (the connected one-shot swing of an upgraded crowbar-class / blade weapon): TLOU2's own finisher pair (kind 0 rows of
// CD_PAIRS: style 0 = Ellie's 1h blunt / hammer finishers, 1 = the machete gut chop + kick) through psync PS_CTXDOWN on every client, the victim DOWNED at
// the hand-off (cd_victim_begin: 1 hp, no fall, its half blends into the lying pose). The pair is anchored on the victim (its authored frame-0 spot = where
// it stands) and turned so the authored attacker spot points at us - psync's pull turns the victim to its authored facing (any angle), so the row is picked
// by our side of the victim's facing first (finisher_dir: in front -> the front rows, behind -> the "-back-" rows; none loaded for that side -> any row of
// the style), round robin among the side's loaded rows. The crowbar eye jab only with t2r-upg-crowbar-stab.txt next to the exe.
// 0 = not started (the caller sends the plain down hit, the ONETAP13_DMG marker).
static int CdUpgStabOn(void)
{
    static int on = -1;
    static uint64_t chk;
    if (on < 0 || g_frame - chk >= 120) {
        chk = g_frame ? g_frame : 1;
        FILE *f = ExeSideOpen("t2r-upg-crowbar-stab.txt", "r");
        int v = f != NULL;
        if (f)
            fclose(f);
        if (v != on)
            Xlog("melee upgrade: crowbar class one-shot = %s",
                 v ? "the crowbar eye jab (t2r-upg-crowbar-stab.txt)" :
                 "TLOU2's hammer finishers");
        on = v;
    }
    return on;
}

#define CD_FIN_TURN_MAX 40.0f   // 40 deg of freedom for finishers / takedowns from the sides (our facing and the victim's)
// ps46 lethal turn (every downing blow of a duel plays a finishing pair or the wall / cover takedown - never a plain engine down). The gap: a
// side-on lethal blow (~65-115 deg off the victim's facing) met every row with a victim turn over CD_FIN_TURN_MAX (+ the ps25 split's 25 deg) and was refused,
// and cd_fin_pick offered only the "back" rows from behind (the female life / switchblade set has no plain back row at all). SP turns its NPC into the synced
// pair; here, for the LETHAL finisher rows only (mf-, forced by the duel: non-lethal hits never come here and keep the ps44 T9 back reaction):
//  - cd_fin_pick / cd_fin_all: side-on = the front AND the back rows; no row on our side = the other side's rows (the nearest that exist)
//  - cd_upg_go: the pair is turned about the victim as far as our own start guard allows (the ps25 split, partial), the rest of the victim's turn is ACCEPTED
//    up to CD_T46_TURN_MAX and eased by psync's SP pull (paired.c: victim at 180 deg/s, quadratic ease-out, never under PS_SP_PULL_S 0.20 s, cap 1.0 s;
//    the victim's own client turns its body at the glide's rate, ps39 B4 / ps44 T7) - no 1-2 frame flip.
// t2r-fin-turn-ps46-old.txt = off (the 40 / 65 deg refusal and the side filters as before), re-read every 2 s. Proof line "ctxdown: ps46 lethal turn ...".
#define CD_T46_SIDE_LO 63.0f    /* [TUNING ps46] deg of our bearing off the victim's facing where side-on starts (= CD_FIN_TURN_MAX + the ps25 split 25 - 2) */
#define CD_T46_TURN_MAX 180.0f  /* [TUNING ps46] deg the lethal finisher's victim may be turned into its row (180 = any facing) */
// ps46b (pre-mortem of 5911abc5): with the intro skipped the blow lands ~0.2 s in, while a 180 deg turn eases over up to 1 s - the blow landed mid-spin. The
// eased turn is accepted only when it ends before the blow (blow time >= turn time) or is at most CD_T46_TURN_SAFE; otherwise the row is refused like
// before ps46 (the 40 deg refusal -> the caller's next row / D3 / the old path). t2r-fin-turn180-ps46-on.txt = the full CD_T46_TURN_MAX again
// (re-read every 2 s). Proof line "ps46b lethal turn".
#define CD_T46_TURN_SAFE 90.0f  /* [TUNING ps46] deg a lethal finisher's victim may be turned even when the turn outlasts the blow */
static int CdT46Turn180On(void)
{
    static int v = -1;
    static uint64_t at;
    uint64_t n = GetTickCount64();
    if (v < 0 || n - at > 2000) {
        at = n;
        int o =
            GetFileAttributesA("t2r-fin-turn180-ps46-on.txt") !=
            INVALID_FILE_ATTRIBUTES;
        if (o != v)
            Xlog("ctxdown: ps46b lethal turn limit %s",
                 o ?
                 "180 deg (t2r-fin-turn180-ps46-on.txt: any turn, even one that ends after the blow)"
                 : "the turn must end before the blow or be <= 90 deg");
        v = o;
    }
    return v;
}

static int CdT46On(void)
{
    static int v = -1;
    static uint64_t at;
    uint64_t n = GetTickCount64();
    if (v < 0 || n - at > 2000) {
        at = n;
        int o =
            GetFileAttributesA("t2r-fin-turn-ps46-old.txt") !=
            INVALID_FILE_ATTRIBUTES;
        if (o != v)
            Xlog("ctxdown: ps46 lethal turn %s",
                 o ?
                 "OFF (t2r-fin-turn-ps46-old.txt: the 40 deg victim-turn refusal, our side's rows only)"
                 :
                 "on (a lethal finisher turns the victim into the nearest row, eased)");
        v = o;
    }
    return !v;
}

static int CdT46SideOn(int dir, float fw, float sd)
{
    if (dir < 0 || (fw == 0.0f && sd == 0.0f))
        return 0;
    float a = __builtin_fabsf(__builtin_atan2f(sd, fw)) * 57.29578f;
    return a >= CD_T46_SIDE_LO && a <= 180.0f - CD_T46_SIDE_LO;
}

static int CdUpgGo(uint32_t slot, uint64_t w, int style, int forced);
static int CdUpgStart(uint32_t slot, uint64_t w, int style)
{
    return CdUpgGo(slot, w, style, -1);
}

// melee duel finishers (melee_duel.c lethal blow, tools/mv3/gen_duel_fin.py rows "mf-<SP move>"): the pool of SP's own player finishers for the weapon we hold now
// (cd_cur_style) and the life we live (male = Abby's pool, female = Ellie's: the rows' char bits), the side of the victim we stand on (finisher_dir: the "back" rows
// from behind), round robin among the loaded ones. -1 = none (the lethal blow stays the engine's contact + Melee Down). *why gets the reason.
static const struct {
    const char *name;
    uint8_t back, chr;
    uint64_t dmask;
} MDF_META[] = { MDF_META_DATA { NULL, 0, 0, 0 } };

static int CdMfRow(int i)
{
    return !strncmp(CD_PAIRS[i].name, "mf-", 3);
}

// v5 (the windup): an mf- row with SP's intro is a 2-segment chain, segment 0 = the attacker-only intro (the windup), segment 1 = the -sync pair
static int CdMfIntro(int p)
{
    return p >= 0 && CdMfRow(p) && CD_PAIRS[p].nseg > 1;
}

static int CdMfIntroMove(uint64_t move)
{
    return CdMfIntro(CdPairOfMove(move));
}

// ps37 (A [19752], hatchet finisher mf-1h-blade-left-finisher: 'PLAY ... skip 467 ms (phase att 0.950 vic 0.189)' and 'takedown ... attacker 0.05 m /
// 10 deg, victim 0.54 m / 30 deg off their spots at the start phase (467 ms in)' - in the frames the attacker stands almost on top of the victim and the blow
// goes past its head): a finisher whose intro is passed over starts BOTH halves at the -sync switch, and the victim's clip is one clip over intro + -sync, so
// its half starts an intro's length in. The pair was turned and placed with the victim clip's FRAME-0 spot though, 0.54 m / 30 deg from where that half
// starts. The victim's reference is its spot at the switch: s into the victim clip where the -sync part starts (0 = its frame 0: another clip per segment /
// no intro). t2r-fin-vic-frame0-ps36.txt = the frame-0 spot.
static float CdFinVicT(int p)
{
    static int old = -1;
    static uint64_t at;
    uint64_t n = GetTickCount64();
    if (old < 0 || n - at > 3000) {
        at = n;
        old =
            GetFileAttributesA("t2r-fin-vic-frame0-ps36.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    // ps39 B: WRONG, taken back (rec38a A [16734] 'contact distance 2.13 m = the -sync attacker start', [16761] 'attacker 0.80 m / 8 deg ... WARNING'; [15234]
    // 0.44 m; [12876] 0.17 m; [18338] 0.42 m): the victim's clip is the -sync move's defender clip - its frame 0 IS the switch ('victim half ... deferred to 120 ms
    // before the hand-over (phase 0 there)'), so the ps37 reference an intro's length in put the attacker 0.2-0.9 m too far out. The '0.54 m / 30 deg' of rec35a was
    // the start audit reading the deferred victim half at the skip time (paired.c, fixed there). The frame-0 spot again unless t2r-fin-vic-switch-ps37.txt.
    if (GetFileAttributesA("t2r-fin-vic-switch-ps37.txt") ==
        INVALID_FILE_ATTRIBUTES)
        return 0.0f;
    if (old || !CdMfIntro(p)
        || strcmp(CD_PAIRS[p].seg[0].vic,
                  CD_PAIRS[p].seg[CD_PAIRS[p].nseg - 1].vic))
        return 0.0f;
    return CdSegLen(p, 0);
}

// the clip-event tables (ctx_sfx / ctx_fx / ctx_vox events, tools/ctx*_gen.py) index the chain's segments; the intro rows' segment 0 has the attacker's intro events
static int CdTabSeg(int p, int sg, int role)
{
    return CdMfIntro(p) && sg == 0 && role == 1 ? 99 : sg;
}                               // (v5: the tables are generated per chain segment;

                                                // an mf- row's segment 0 victim clip is the deferred -sync victim half - its events are segment 1's, never fired twice)
// real ms of the first segment of a chain (0 = a single-pair move)
static float CdSeg0Ms(uint64_t move)
{
    int p = CdPairOfMove(move);
    return p >= 0
        && CD_PAIRS[p].nseg > 1 ? CdSegLen(p, 0) * 1000.0f : 0.0f;
}

// SP's pick (re/melee-dc/melee-dc.md "How SP picks a move", decoded DC + the NR recorder): the lethal swing is replaced on its press frame by a finisher of the
// SWING'S OWN list (its frame-[0,0] auto link, mduel_fin.h MDF_SWING_DATA): a right-hand swing (fists light-1 / 3, sprint, dodge attacks) lists the haymaker + the
// kick only, a left-hand one (left-1, light-2, 4-punch) the throat punch / left hook / nose break + the kick; 1h / 2h / knife lists follow their swing's side the
// same way (2h attack-1 / 3 -> right KOs, attack-2 / 4 -> left KOs). Then the rows' own checks: the direction at the intro's end (front / back rows), the attacker's
// weapon and character bits. SP recordings: light-2 -> throat-punch-kill (session2 F 80601) and -> kick-face-kill (session3 F 172181), sprint -> right-haymaker-kill
// (F 82280), 2h attack-2 -> left-ko-finisher-1 (F 71611): different rows of one list for one swing, so the pick inside the list is not fixed - a random valid
// entry here (alt > 0: the alt-th next one, for a refused path). Old pool (every mf- row of the style, round robin): t2r-duel-fin-any.txt.
static const struct {
    const char *swing, *list;
} MDF_SWING[] = { MDF_SWING_DATA { NULL, NULL } };

static int CdFinListHas(const char *list, const char *move)
{
    size_t l = strlen(move);
    for (const char *p = list; p && *p;) {
        const char *e = strchr(p, ' ');
        size_t n = e ? (size_t) (e - p) : strlen(p);
        if (n == l && !strncmp(p, move, l))
            return 1;
        p = e ? e + 1 : NULL;
    }
    return 0;
}

// ---- v5 DISARM finishers (finisher blows against players that have a melee weapon (except the machete) are the disarms when you have no melee weapon yourself). SP (re/melee-dc/melee-dc.md "Disarms", melee-moves.json): player-fists-finishers-disarms (attacker mask 0x20) and
// player-switchblade-finishers-disarms (0x4), macro melee-player-finisher-with-intro like every mf- row, picked by the defender's weapon class
// (defender_weapons_mask); the lethal swings' own auto lists name them next to the plain finishers (mduel_fin.h MDF_SWING_DATA). SP hands the defender's
// weapon to the player (script give-player-weapon-instance, frame 29 of the arm-break sync; set-virtual-weapon-to-enemy-weapon 0-800 ms). Here: the rows are
// mf- rows with MDF_META dmask != 0, played on a victim whose HELD weapon has that class while we are unarmed (fists / a female life's knife).
// t2r-duel-no-disarm.txt = never pick them (the plain finishers only).
// v5-fb1 (the disarm should work on a hatchet): SP's own front rows of the other classes are not resident in the MP
// paks (mduel_fin.h coverage: fist-1h-blade-disarm-iron-rhino / punch-n-hack, 1h-blunt arm-break / left-kick-knee / back-arm-break = SP-only), so a class
// without a resident row of its own on that side borrows the closest resident row of the same grip (the weapon prop is the victim's real item, cd_dis_tick):
//   fists  1h blade (hatchet / tomahawk) FRONT -> the 1h-blunt front rows (left-back-hand, shoulder-check: the same one-hand grip and arm grab)
//          v5-hatch: once the data build has made SP's own iron-rhino / punch-n-hack resident
//          (cd_unstream_ok), those two rows are the fists' 1h-blade FRONT disarms and this borrow is OFF; it stays only on old data
//   fists  1h blunt BACK                       -> the 1h-blade back-hack (the only resident 1h back disarm)
//   knife  1h blade FRONT / BACK               -> the 1h-blunt left-overhead / back-ice-pick
//   knife  2h blade (axe) FRONT                -> the 2h-blunt double-wrist (two-hand grip)
//   knife  2h blunt / 2h blade BACK            -> the 1h-blunt back-ice-pick (no resident 2h back row for the knife)
// Every non-machete melee item of MELEE_TAB then has a disarm row on both sides for both attacker sets. t2r-duel-dis-sp-only.txt = SP's own masks only.
static uint64_t CdMfDmaskMp(const char *row)
{
    static const struct {
        const char *n;
        uint64_t add;
    } X[] = {
        { "mf-fist-1h-blunt-disarm-left-back-hand-finisher",
         0x1900000ULL },
{ "mf-fist-1h-blunt-disarm-shoulder-check-finisher", 0x1900000ULL },
        { "mf-fist-1h-blade-disarm-back-hack-finisher", 0x400ULL },
        { "mf-switchblade-1h-blunt-disarm-left-overhead-finisher",
         0x1900000ULL },
        { "mf-switchblade-1h-blunt-disarm-back-ice-pick-finisher",
         0x1900000ULL | 0x10000000ULL | 0x400000000ULL },
        { "mf-switchblade-2h-blunt-disarm-double-wrist-finisher",
         0x400000000ULL },
    };
    static int off = -1;
    static uint64_t chk;
    if (off < 0 || GetTickCount64() - chk > 2000) {
        chk = GetTickCount64();
        off =
            GetFileAttributesA("t2r-duel-dis-sp-only.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    if (off)
        return 0;
    for (size_t k = 0; k < sizeof X / sizeof X[0]; k++)
        if (!strcmp(X[k].n, row)) {
            uint64_t add = X[k].add;
            if (!strncmp(row, "mf-fist-1h-blunt-disarm-", 24)
                && CdUnstreamOk())
                add &= ~0x1900000ULL;   // the real hatchet rows are loaded: no borrow
            return add;
        }
    return 0;
}

static uint64_t CdMfDmask(int i)
{
    for (size_t k = 0; MDF_META[k].name; k++)
        if (!strcmp(MDF_META[k].name, CD_PAIRS[i].name))
            return MDF_META[k].dmask ? MDF_META[k].
                dmask | CdMfDmaskMp(CD_PAIRS[i].name) : 0;
    return 0;
}

// SP's defender weapon bits of a held melee item (melee-dc.md bit table): 0 = none / not a melee / the machete (rule: no disarm of the machete)
static uint64_t CdDisClass(uint64_t w, const char **nm)
{
    const char *n = NULL;
    if (nm)
        *nm = NULL;
    if (!w || w == ~0ULL)
        return 0;
    if (MeleeClassOf(w, &n) < 0 || !n) {
        uint64_t bw = BaseWeapon(w);
        n = NULL;
        if (bw == w || MeleeClassOf(bw, &n) < 0 || !n)
            return 0;
    }
    if (nm)
        *nm = n;
    static const struct {
        const char *n;
        uint64_t m;
    } T[] = {
        { "crowbar", 0x400 }, { "short-pipe", 0x400 }, { "pipe-wrench",
                                                        0x400 },
            { "hammer-modern", 0x400 }, { "hammer-scar", 0x400 },
            { "club-1h-police", 0x400 }, { "club-1h-scar", 0x400 },
        { "tomahawk", 0x100000 | 0x1800000 }, { "hatchet-modern", 0x100000 | 0x1800000 }, { "hatchet", 0x100000 | 0x1800000 },  // (0x1800000 "hatchet variants": the fists' back-hack row)
        { "lead-pipe", 0x10000000 }, { "two-by-four", 0x10000000 },
            { "baseball-bat", 0x10000000 },
        { "fireman-axe", 0x10000000 },  // v5-dmg: the axe is out of the game; held anyway = the 2h blunt disarm class (was 0x400000000 2h blade)
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++)
        if (!strcmp(T[i].n, n))
            return T[i].m;
    return 0;
}

static int CdDisOff(void)
{
    static int off = -1;
    static uint64_t chk;
    if (off < 0 || GetTickCount64() - chk > 2000) {
        chk = GetTickCount64();
        off =
            GetFileAttributesA("t2r-duel-no-disarm.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    return off;
}

static uint64_t CdDisPeerWeapon(int slot)
{                               // the weapon in a peer's hand (its state record, as wood_slot reads it)
    typedef uint8_t *(*fn_peer)(uint32_t);
    if (slot < 0 || slot >= T2R_MAX_PEERS)
        return 0;
    uint8_t *pl = ((fn_peer) T2rPeer) ((uint32_t) slot);
    uint64_t w = 0;
    if (pl && !IsBadReadPtr(pl, 0x99))
        memcpy(&w, pl + 0x91, 8);
    return w;
}

// A finisher swing sometimes started from the opposite side than the finisher blow swings from (not in SP): the hand/side a finisher row's
// blow comes from (-1 left, +1 right, 0 neutral: kicks, handle, back kills ...), read from its name, and the side of a swing's SP list (its rows all left =
// a left-hand swing, all right = a right-hand one, mixed / neutral = 0: SP lists both sides there, nothing to keep).
static int CdFinRowSide(const char *n)
{
    int l = strstr(n, "left") != NULL, r = strstr(n, "right") != NULL;
    return l == r ? 0 : l ? -1 : 1;
}

static int CdFinListSide(const char *list)
{
    int l = 0, r = 0;
    for (const char *p = list; p && *p;) {
        const char *e = strchr(p, ' ');
        size_t n = e ? (size_t) (e - p) : strlen(p);
        char t[96];
        if (n >= sizeof t)
            n = sizeof t - 1;
        memcpy(t, p, n);
        t[n] = 0;
        int s = CdFinRowSide(t);
        if (s < 0)
            l++;
        else if (s > 0)
            r++;
        p = e ? e + 1 : NULL;
    }
    return l && !r ? -1 : r && !l ? 1 : 0;
}

static int CdFinPick(uint32_t slot, const char *swing, int alt,
                       char *why, size_t wl)
{
    static unsigned rr[2], seed;
    int style = CdCurStyle();
    if (style == -2) {
        snprintf(why, wl, "an upgraded melee keeps its own one-shot");
        return -1;
    }
    float fw = 0.0f, sd = 0.0f;
    int dir = FinisherDir((int32_t) slot, &fw, &sd), back = dir >= 0
        && fw < 0.0f, fem = OwnFemale();
    const char *vn = NULL;
    uint64_t vw = CdDisPeerWeapon((int) slot), vm =
        CdDisClass(vw, &vn);
    // v5-dis4 (A/B A 45378: B carried its crowbar on the backpack with the pistol in the hand -> the plain fist kill, no disarm, no
    // drop): no melee weapon in the victim's hand but one carried (its MSG_MOWN, melee_drop.c) = that weapon's disarm rows; the victim's own client draws it
    // into the hand at the START (cd_dis_tick). Not over a held machete. t2r-dis-held-only.txt = the held weapon only (old rule).
    if ((style == -1 || style == 3) && !vm
        && !(vn && strstr(vn, "machete")) && !CdDisOff()
        && !MownDisHeldOnly()) {
        const char *cn = NULL;
        uint64_t cw = MownPeer((int) slot), cm = CdDisClass(cw, &cn);
        if (cm) {
            Xlog("ctxdown: finisher pick - the victim (slot %u) holds %llx but carries %s (its MSG_MOWN): its disarm rows, its own client draws it at the START", slot, (unsigned long long) vw, cn ? cn : "?");
            vw = cw;
            vm = cm;
            vn = cn;
        }
    }
    int dis = (style == -1 || style == 3) && vm
        && !CdDisOff(), got_dis = 0;
    if ((style == -1 || style == 3) && !vm && vn)
        Xlog("ctxdown: finisher pick - the victim holds %s: no disarm (%s)", vn, strstr(vn, "machete") ? "the machete: the user's rule" : "no SP disarm class");
    const char *list = NULL;
    static int any = -1;
    static uint64_t any_chk;
    if (any < 0 || GetTickCount64() - any_chk > 2000) {
        any_chk = GetTickCount64();
        any =
            GetFileAttributesA("t2r-duel-fin-any.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    if (!any && swing)
        for (size_t k = 0; MDF_SWING[k].swing; k++)
            if (!strcmp(MDF_SWING[k].swing, swing)) {
                list = MDF_SWING[k].list;
                break;
            }
    // v5-var (the fights felt unvaried): SP's swing lists hold only 2 open-ground fist kills for a right-hand swing (haymaker / kick),
    // and the duel opens with the right jab nearly always - every kill was one of two. Now the pool is EVERY loaded finisher of our style that fits the
    // situation (front / back rows, character bits, the disarm rule); the swing's own SP list rows weigh double, then spacing fit, never the same kill
    // twice in a row (cd_fin_wpick below). t2r-duel-fin-sp-list.txt = the old pick (the swing's SP list only, the pool only when nothing of it fits).
    static int sp_only = -1;
    static uint64_t sp_chk;
    if (sp_only < 0 || GetTickCount64() - sp_chk > 2000) {
        sp_chk = GetTickCount64();
        sp_only =
            GetFileAttributesA("t2r-duel-fin-sp-list.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    int cand[CD_N], nc = 0, nm = 0, nl = 0;
    char miss[160];
    int mo = 0;
    miss[0] = 0;
    int t46 = CdT46On(), side46 = t46 && CdT46SideOn(dir, fw, sd), flip46 = 0;     // ps46: side-on = both sides' rows; none on ours = the other side's
    for (int fl46 = 0, bk46 = back; fl46 < (t46 ? 2 : 1) && !nc;
         fl46++, bk46 = !back) {
        for (int dpass = dis ? 1 : 0; dpass >= 0 && !nc; dpass--)       // dpass 1: the SP disarms of the victim's weapon class, 0: the plain finishers
            for (int pass = list && sp_only ? 0 : 1; pass < 2 && !nc; pass++) { // pass 0: the swing's SP list only (t2r-duel-fin-sp-list.txt), pass 1: the full pool
                nm = nl = 0;
                mo = 0;
                miss[0] = 0;
                for (size_t i = 0; i < CD_N; i++) {
                    if (CD_PAIRS[i].kind != 0 || !CdMfRow((int) i)
                        || !CdStyleOk(style, CD_PAIRS[i].style))
                        continue;
                    // (A fell through the map after a finisher): the SP slope-down kills are authored for a 30 deg downhill - the
                    // victim half ends ~0.8 m under flat ground and the body drops through the floor at the hand-off. Out of the pool (t2r-fin-slope-ok.txt = in).
                    if (strstr(CD_PAIRS[i].name, "-slope-")
                        && GetFileAttributesA("t2r-fin-slope-ok.txt") ==
                        INVALID_FILE_ATTRIBUTES)
                        continue;
                    {
                        uint64_t dmk = CdMfDmask((int) i);
                        if (dpass ? !(dmk & vm) : dmk != 0)
                            continue;
                    }
                    if (!pass
                        && !CdFinListHas(list, CD_PAIRS[i].name + 3))
                        continue;
                    int back_r = 0, chr = 3;
                    for (size_t k = 0; MDF_META[k].name; k++)
                        if (!strcmp(MDF_META[k].name, CD_PAIRS[i].name)) {
                            back_r = MDF_META[k].back;
                            chr = MDF_META[k].chr;
                            break;
                        }
                    if (!(chr & (fem ? 2 : 1))
                        || (back_r != bk46 && !side46))
                        continue;
                    nm++;
                    if (!CdPairLoaded((int) i)) {
                        if (mo < (int) sizeof miss - 48)
                            mo +=
                                snprintf(miss + mo, SNLEFT(miss, mo),
                                         "%s%s", mo ? ", " : "",
                                         CD_PAIRS[i].name + 3);
                        continue;
                    }
                    nl++;
                    cand[nc++] = (int) i;
                }
                if (!nc && !pass)
                    Xlog("ctxdown: finisher pick - nothing of %s's SP %slist fits (style %d, %s life, %s the victim; not loaded: %s) - the old pool", swing, dpass ? "DISARM " : "", style, fem ? "female" : "male", back ? "behind" : "in front of", mo ? miss : "-");
                if (nc && dpass)
                    got_dis = 1;
                if (!nc && dpass && pass)
                    Xlog("ctxdown: finisher pick - no loaded SP disarm of %s (class %llx) for style %d, %s the victim - a plain finisher", vn ? vn : "?", (unsigned long long) vm, style, back ? "behind" : "in front of");
            }
        if (nc && fl46)
            flip46 = 1;
    }
    if (side46 || flip46)
        Xlog("ctxdown: ps46 lethal turn pick on slot %u: %s (fwd %.2f side %.2f m, %.0f deg off the victim's facing; style %d, %s life) - %d row(s) offered, cd_upg_go turns the victim into the row", slot, flip46 ? (back ? "BEHIND the victim, no back row of this set: the FRONT rows" : "in front, no front row of this set: the BACK rows") : "SIDE-ON: the front and the back rows", (double) fw, (double) sd, (double) (__builtin_fabsf(__builtin_atan2f(sd, fw)) * 57.29578f), style, fem ? "female" : "male", nc);
    if (!nc) {
        snprintf(why, wl,
                 "no loaded finisher for style %d, %s life, %s the victim (%d rows, not loaded: %s)",
                 style, fem ? "female" : "male",
                 back ? "behind" : "in front of", nm, mo ? miss : "-");
        return -1;
    }
    unsigned ix;
    // v5-geo2 (A i10 [12965]: the haymaker's intro starts 2.40 m from the victim, we stood 1.25 m off - the pull dragged us 1.15 m back onto it): SP chooses its
    // kill by distance, so a new lethal swing picks among the finishers whose intro start spot (attacker -> victim, authored frame 0) is within 0.30 m of the
    // distance we stand at; none in reach = the closest one (its intro's start step covers the rest, paired.c psync_fin_step).
    float fd = __builtin_sqrtf(fw * fw + sd * sd), gap[CD_N];
    int fit[CD_N], nf = 0, best = -1;
    char sp[200];
    int so = 0;
    sp[0] = 0;
    {
        uint64_t vh = PuppetHandle(slot);
        for (int i = 0; i < nc; i++) {
            float cv[8], ca[8];
            int p = cand[i];
            gap[i] = 9.0f;
            if (vh && CdOffsetAt(vh, CD_VIC0(p), 0.0f, cv)
                && CdOffsetAt(SID_PLAYER, CD_ATT0(p), 0.0f, ca))
                gap[i] =
                    __builtin_sqrtf((ca[0] - cv[0]) * (ca[0] - cv[0]) +
                                    (ca[2] - cv[2]) * (ca[2] - cv[2])) -
                    fd;
            if (__builtin_fabsf(gap[i]) <= 0.30f)
                fit[nf++] = i;
            if (best < 0
                || __builtin_fabsf(gap[i]) < __builtin_fabsf(gap[best]))
                best = i;
            if (so < (int) sizeof sp - 48)
                so +=
                    snprintf(sp + so, SNLEFT(sp, so), "%s%s %+.2f",
                             i ? ", " : "", CD_PAIRS[p].name + 3,
                             gap[i] < 8.0f ? gap[i] : 0.0f);
        }
    }
    static int last_pair[2] = { -1, -1 };
    if (alt <= 0 && !sp_only) {
        // v5-var: weighted pick over the whole pool - weight = (2 in the swing's SP list, else 1) x spacing fit 1 / (0.25 + |gap|) (a kill whose intro starts at
        // our distance ~5x one 1 m off), 0 for the kill we played last on this side (when another one is loaded). The log lists the pool with its weights.
        // the blow must come from the hand the swing ended on - a pool row of the OPPOSITE side to the swing's SP list is out (weight 0), unless nothing
        // else is left (then the side filter drops). A neutral row (kick, handle, back kill) and the list's own rows stay.
        int sws = list && swing ? CdFinListSide(list) : 0, sidefix = 0;
        float wt[CD_N], tot = 0.0f;
        char pl[420];
        int po = 0;
        pl[0] = 0;
        for (int pass2 = 0; pass2 < 2; pass2++) {
            tot = 0.0f;
            po = 0;
            pl[0] = 0;
            sidefix = pass2 == 0 && sws != 0;
            for (int i = 0; i < nc; i++) {
                int inl = list
                    && CdFinListHas(list, CD_PAIRS[cand[i]].name + 3);
                float g = gap[i] < 8.0f ? __builtin_fabsf(gap[i]) : 1.5f;
                wt[i] = (inl ? 2.0f : 1.0f) / (0.25f + g);
                if (nc > 1 && cand[i] == last_pair[back])
                    wt[i] = 0.0f;
                if (sidefix && !inl
                    && CdFinRowSide(CD_PAIRS[cand[i]].name + 3) == -sws)
                    wt[i] = 0.0f;
                tot += wt[i];
                if (po < (int) sizeof pl - 64)
                    po +=
                        snprintf(pl + po, SNLEFT(pl, po), "%s%s%s %.2f",
                                 i ? ", " : "", CD_PAIRS[cand[i]].name + 3,
                                 inl ? "*" : "", (double) wt[i]);
            }
            if (tot > 0.0f || !sidefix)
                break;
        }
        if (sws)
            Xlog("ctxdown: finisher side - %s's SP list is %s-hand: opposite-side pool rows %s", swing, sws < 0 ? "LEFT" : "RIGHT", sidefix ? "excluded" : "had to be allowed (nothing else fits)");
        seed = seed * 1103515245u + 12345u + (unsigned) GetTickCount64();
        float r = tot * (float) ((seed >> 8) & 0xffff) / 65536.0f;
        ix = 0;
        for (int i = 0; i < nc; i++) {
            if (wt[i] <= 0.0f)
                continue;
            ix = (unsigned) i;
            if (r < wt[i])
                break;
            r -= wt[i];
        }
        rr[back] = ix;
        Xlog("ctxdown: finisher POOL at %.2f m, %s the victim (%d loaded; * = in %s's SP list; weight = list x spacing fit, 0 = the last kill on this side): %s -> %s (gap %+.2f m)", fd, back ? "BEHIND" : "in front of", nc, swing ? swing : "?", pl, CD_PAIRS[cand[ix]].name + 3, gap[ix] < 8.0f ? (double) gap[ix] : 0.0);
    } else if (alt <= 0 && nf < nc && best >= 0) {
        seed = seed * 1103515245u + 12345u + (unsigned) GetTickCount64();
        ix = nf ? (unsigned) fit[(seed >> 8) %
                                 (unsigned) nf] : (seed >> 8) %
            (unsigned) nc;
        rr[back] = ix;          // none fits = a random one, not always the closest (the duel stands ~1.2 m: the kick always won)
        Xlog("ctxdown: finisher pick by spacing at %.2f m (intro start - our distance: %s): %s %s", fd, sp, CD_PAIRS[cand[ix]].name + 3, nf ? "fits (<= 0.30 m)" : "picked at random - none within 0.30 m (its intro holds our distance)");
    } else if (alt <= 0) {
        seed = seed * 1103515245u + 12345u + (unsigned) GetTickCount64();
        ix = (seed >> 8) % (unsigned) nc;
        rr[back] = ix;
    }                           // a new lethal swing: a random entry
    else
        ix = (rr[back] + (unsigned) alt) % (unsigned) nc;       // the alt-th next one
    last_pair[back] = cand[ix];
    int pair = cand[ix];
    char names[200];
    int no = 0;
    names[0] = 0;
    for (int i = 0; i < nc && no < (int) sizeof names - 48; i++)
        no +=
            snprintf(names + no, SNLEFT(names, no), "%s%s", i ? " / " : "",
                     CD_PAIRS[cand[i]].name + 3);
    float idur = CdMfIntro(pair) ? CdSegLen(pair, 0) : 0.0f;
    snprintf(why, wl,
             "%s%s%s%s (from %s: %s, %d valid of %d [%s]; style %d, %s life, %s the victim: fwd %.2f side %.2f m, dist %.2f m; windup = SP intro %s %.0f ms%s)",
             got_dis ? "DISARM of the victim's " : "", got_dis
             && vn ? vn : "", got_dis ? ": " : "", CD_PAIRS[pair].name + 3,
             swing ? swing : "?", list ? (nc
                                          && CdFinListHas(list,
                                                             CD_PAIRS
                                                             [pair].name +
                                                             3) ?
                                          "its SP list" : sp_only ?
                                          "NOT in its SP list, old pool" :
                                          "the pool (not in its SP list)")
             : any ? "old pool (t2r-duel-fin-any.txt)" :
             "no SP list, old pool", nl, nm, names, style,
             fem ? "female" : "male", back ? "BEHIND" : "in front of", fw,
             sd, __builtin_sqrtf(fw * fw + sd * sd),
             CdMfIntro(pair) ? CD_PAIRS[pair].seg[0].att : "-",
             idur * 1000.0f, alt > 0 ? ", an alternative" : "");
    return pair;
}

// (a finisher did not play, B [41936]-[41940] Bookstore): the windup's pick was refused by the path gate (the victim's 2.12 m path met
// clutter 0.6 m behind it) and the contact's alternative loop stepped the pick's round robin onto the same row - nothing else was tried, the plain down
// blow took the slot. SP never gives up on a finisher while another one fits the room. This lists EVERY loaded finisher row of the weapon we hold that suits
// the victim (front / back rows, our character, the disarm class, no slope rows - cd_fin_pick's filters), both hands / mirror rows included, sorted by
// spacing fit (|intro start distance - our distance|). The caller gates each one against the actual space (melee_duel.c mdu_fin_chain).
static int CdFinAll(uint32_t slot, int *out, float *gap_o, int maxn)
{
    int style = CdCurStyle();
    if (style == -2)
        return 0;
    float fw = 0.0f, sd = 0.0f;
    int dir = FinisherDir((int32_t) slot, &fw, &sd), back = dir >= 0
        && fw < 0.0f, fem = OwnFemale();
    uint64_t vw = CdDisPeerWeapon((int) slot), vm = (style == -1
                                                        || style == 3)
        && !CdDisOff()? CdDisClass(vw, NULL) : 0;
    float fd = __builtin_sqrtf(fw * fw + sd * sd);
    uint64_t vh = PuppetHandle(slot);
    int n = 0, t46 = CdT46On(), side46 = t46 && CdT46SideOn(dir, fw, sd);  // ps46: as cd_fin_pick (side-on = both sides, none on ours = the other side)
    for (int fl46 = 0, bk46 = back; fl46 < (t46 ? 2 : 1) && !n;
         fl46++, bk46 = !back)
        for (size_t i = 0; i < CD_N && n < maxn; i++) {
            if (CD_PAIRS[i].kind != 0 || !CdMfRow((int) i)
                || !CdStyleOk(style, CD_PAIRS[i].style))
                continue;
            if (strstr(CD_PAIRS[i].name, "-slope-")
                && GetFileAttributesA("t2r-fin-slope-ok.txt") ==
                INVALID_FILE_ATTRIBUTES)
                continue;
            {
                uint64_t dmk = CdMfDmask((int) i);
                if (dmk && !(dmk & vm))
                    continue;
            }
            int back_r = 0, chr = 3;
            for (size_t k = 0; MDF_META[k].name; k++)
                if (!strcmp(MDF_META[k].name, CD_PAIRS[i].name)) {
                    back_r = MDF_META[k].back;
                    chr = MDF_META[k].chr;
                    break;
                }
            if (!(chr & (fem ? 2 : 1)) || (back_r != bk46 && !side46))
                continue;
            if (!CdPairLoaded((int) i))
                continue;
            float cv[8], ca[8], g = 9.0f;
            if (vh && CdOffsetAt(vh, CD_VIC0((int) i), 0.0f, cv)
                && CdOffsetAt(SID_PLAYER, CD_ATT0((int) i), 0.0f, ca))
                g = __builtin_fabsf(__builtin_sqrtf
                                    ((ca[0] - cv[0]) * (ca[0] - cv[0]) +
                                     (ca[2] - cv[2]) * (ca[2] - cv[2])) -
                                    fd);
            int j = n++;
            while (j > 0 && gap_o[j - 1] > g) {
                out[j] = out[j - 1];
                gap_o[j] = gap_o[j - 1];
                j--;
            }
            out[j] = (int) i;
            gap_o[j] = g;
        }
    return n;
}

// The disarm's weapon, on every screen (role 1 attacker / 2 victim / 3 bystander), on the move clock: at SP's take frame (the -sync pair's blow: the
// damage-character tick, = give-player-weapon-instance on the arm-break) the victim's held item is hidden in its hand (draw-control visibility byte, the
// wpnbreak / pup_hide_held way - no graph change inside the pair); at the hand-off the inventories change hands: the victim's own client removes it
// (remove-player-weapon, and forgets it as a kept upgraded melee: it never comes back at the respawn), the attacker's own client gives it to itself as a
// plain pickup (give-player-weapon + an instant draw while the hands are empty: SP ends the move with the weapon in the player's hand), every screen moves
// the death drop (g_peer_melee) from the victim to the attacker - no second copy, no ghost pickup. A move cut before the hand-off shows the item again.
static struct {
    uint32_t uid;
    int pair, role, vic, att, hid, done, given;
    uint64_t w;
    float take_s;
    uint8_t *hw, *hdc;
    uint8_t vis;
} g_cdDis;
static int CdDisLateGive(void)
{
    static int v = -1;
    static uint64_t at;
    if (v < 0 || GetTickCount64() - at > 3000) {
        at = GetTickCount64();
        v = GetFileAttributesA("t2r-dis-give-at-handoff.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    return v;
}

// v5-armed (the victim's weapon stays visible in the victim's hand until the SP disarm's take frame, then is in the attacker's hand): the
// attacker's own client takes the weapon AT the take frame (was: the hand-off, 0.6-0.8 s later - nobody held it in between). The fist takedown's hands hold
// (cd_hands_*: no unholster, anything drawn is put away) ends here so the draw stays; the victim's client still removes it from its inventory at the hand-off,
// and the attacker's puppet on the other screens shows it as soon as winmm's weapon sync gives it (cd_pup_hands_empty no longer hides it).
// t2r-dis-give-at-handoff.txt = the old hand-off give.
static int CdDisKeeps(uint64_t held)
{
    return g_cdDis.uid && g_cdDis.uid == g_ps.uid && g_cdDis.given
        && g_cdDis.role == 1
        && BaseWeapon(held) == BaseWeapon(g_cdDis.w ? g_cdDis.w : 1);
}

static void CdDisGive(const char *when)
{
    uint64_t none = 0, hm = DcCall(SID_HAS_MELEE_WEAPON, 0, &none), w =
        BaseWeapon(g_cdDis.w);
    if (!w)
        w = g_cdDis.w;
    if (hm != ~0ULL && (hm & 0xff)) {
        Xlog("ctxdown: DISARM (attacker, %s): we hold a melee weapon already - %llx not taken (no second one)", when, (unsigned long long) w);
        g_cdDis.given = 1;
        return;
    }
    if (!g_give_orig) {
        Xlog("ctxdown: DISARM (attacker, %s): no give hook - %llx not taken", when, (unsigned long long) w);
        return;
    }
    // (the fist takedown's empty-hands hold stays on: it keeps hfix_active() off, so the sling fix does not skip the draw's hand attach, and its holster gates keep
    //  the engine's own put-away off; cd_hands_tick lets the taken weapon stay - cd_dis_keeps. The 20-frame redraw window covers the attach for safety.)
    g_xrRedrawUntil = g_t60 + 20;
    uint64_t ret[64] = { 0 }, give[4] = { w, 0, 0, 1 };
    g_give_orig(ret, 4, give, 0);
    typedef union {
        float f;
        uint32_t u;
    } FuT;
    FuT z = { 0.0f };
    uint64_t a[2] = { w, z.u }, rd =
        DcCall(FNV64("player-instant-draw-weapon-by-name"), 2, a);
    uint64_t has = DcCall(FNV64("has-weapon?"), 1, &w), hp =
        DcCall(SID_GET_WEAPON_HEALTH, 0, &none), cw = CurrentWeapon();
    g_cdHandsTried = cw;
    g_cdDis.given = 1;
    {                           // v5-dis4: the taken weapon keeps the victim's durability (its MSG_MOWN) instead of a fresh pickup's
        int vd = MownPeerDur(g_cdDis.vic);
        int64_t hv = vd;
        if (vd > 0 && !MownOff() && cw != ~0ULL
            && BaseWeapon(cw) == BaseWeapon(w)) {
            uint64_t r =
                DcCall(SID_SET_WEAPON_HEALTH, 1, (uint64_t *) & hv), hn =
                DcCall(SID_GET_WEAPON_HEALTH, 0, &none);
            Xlog("ctxdown: DISARM (attacker, %s): durability %lld -> %d (the victim's, its MSG_MOWN) -> set %llx, now %lld", when, (long long) hp, vd, (unsigned long long) r, (long long) hn);
            PdurWant(w, vd, "a disarm: the victim's durability");      // v5-mown2: full-condition's pickup edge right after kept setting the class max (A 44435 5 -> A 44440 10)
        }
    }
    Xlog("ctxdown: DISARM (attacker, %s): the victim's %llx is ours (SP give-player-weapon-instance) - give %llx, has-weapon? %llx, instant draw %llx, held %llx, durability %lld (a fresh pickup's)", when, (unsigned long long) w, (unsigned long long) ret[0], (unsigned long long) has, (unsigned long long) rd, (unsigned long long) cw, (long long) hp);
}

// v5-armed: the disarm's weapon goes to the attacker at SP's take frame (cd_dis_give), so its puppet's new item stays visible from there (cd_pup_hands_empty)
static int CdDisAfterTake(int s)
{
    return g_cdDis.uid && g_cdDis.uid == g_ps.uid && g_cdDis.att == s
        && g_cdDis.hid != 0 && CdDisClass(g_cdDis.w, NULL) != 0;
}

static uint8_t *CdDisHeldProc(int role, uint64_t ent_vic)
{                               // the victim's held weapon process on this screen
    if (role == 2) {
        uint8_t *me = BpMe();
        return me
            && !IsBadReadPtr(me + 0x2f60, 8) ? PlayerWeapon(me) : NULL;
    }
    uint8_t *pn = ent_vic ? PstNpc(ent_vic) : NULL;
    return pn ? PstCurrent(pn) : NULL;
}

// Victims did not see the weapon properly in disarms: victim log A 81505 take frame -> 81564 puppet released -> 81570 "upgsync: slot 1 gun
// 8d6843d0484b5aa2 process" = the attacker's puppet on the screens that watch got the taken weapon only AFTER the pair (the attacker's own client has it in the hand from
// the take frame: me log 68783), so the second half of the disarm showed the attacker's hand empty. At the take frame the puppet's winmm give block is re-armed with the
// weapon (peer+0x91 = the taken item, last given peer+0x9e0 cleared: the same idiom as the g_pup_gone re-give), so winmm's own destroy + npc-give-weapon + hand attach runs now.
static void CdDisPupGive(void)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    int s = g_cdDis.att;
    if (s < 0 || s >= T2R_MAX_PEERS || g_cdDis.role == 1)
        return;
    uint8_t *peer = ((fn_peer) T2rPeer) ((uint32_t) s);
    uint64_t h = peer ? *(uint64_t *) (peer + 0x60) : 0;
    if (!h || IsBadWritePtr(peer, 0x9f0))
        return;
    uint8_t *pn = PstNpc(h), *cur = pn ? PstCurrent(pn) : NULL;
    uint64_t rec = *(uint64_t *) (peer + 0x91), last =
        *(uint64_t *) (peer + 0x9e0), sid = cur ? PstSidOf(cur) : 0, w =
        g_cdDis.w;
    int has = cur && w && BaseWeapon(sid) == BaseWeapon(w);
    Xlog("ctxdown: disarm u%08x - attacker puppet slot %d at the take frame: net record %llx, last given %llx, in its hand %llx, taken weapon %llx -> %s", g_cdDis.uid, s, (unsigned long long) rec, (unsigned long long) last, (unsigned long long) sid, (unsigned long long) w, has ? "already in its hand" : "give block re-armed with it");
    if (has || !w)
        return;
    *(uint64_t *) (peer + 0x91) = w;
    *(uint64_t *) (peer + 0x9e0) = 0;
}

static int CdDisDropStyle(uint64_t m)
{
    return m & 0x400 ? 0 : m & 0x100000 ? 1 : m & 0x10000000 ? 2 : -1;
}                               // g_peer_melee / MELEE_DROP_POOL style

static void CdDisUnhide(const char *why)
{
    if (g_cdDis.hid && g_cdDis.hdc
        && !IsBadWritePtr(g_cdDis.hdc + 0x48, 1)
        && CdDisHeldProc(g_cdDis.role, g_ps.ent_vic) == g_cdDis.hw) {
        g_cdDis.hdc[0x48] = g_cdDis.vis;
        Xlog("ctxdown: disarm u%08x - the victim's held item shown again (%s)", g_cdDis.uid, why);
    }
    g_cdDis.hid = 0;
    g_cdDis.hw = g_cdDis.hdc = NULL;
}

// v5-disfix (B 15472 / A: the carried crowbar was drawn into the hand only at the pair START = mid-move): the victim's client
// draws the weapon it CARRIES when the attacker's finisher windup (the disarm row's SP intro) reaches it - SP's defender holds its weapon through the whole disarm. The
// engine only has the instant draw (player-instant-draw-weapon-by-name; no SP draw clip is reachable from a net game), so the item appears at the windup's first frame
// instead of at the take frame. cd_dis_tick then finds it in the hand. t2r-no-free-fin-windup.txt = off (drawn at the START as before).
static void CdDisEarlyDraw(int fin)
{
    if (fin < 0 || fin >= (int) CD_N || !CdMfRow(fin)
        || !CdMfDmask(fin) || MownDisHeldOnly())
        return;
    if (GetFileAttributesA("t2r-no-free-fin-windup.txt") !=
        INVALID_FILE_ATTRIBUTES)
        return;
    if (g_ps.role || g_downed || g_downDying)
        return;
    uint64_t cw = CurrentWeapon();
    if (cw == ~0ULL)
        cw = 0;
    if (CdDisClass(cw, NULL))
        return;                 // a melee weapon is in the hand already
    uint64_t mw = XrMeleeWeapon(cw);
    if (!CdDisClass(mw, NULL) && g_lastMelee) {
        uint64_t hw = DcCall(FNV64("has-weapon?"), 1, &g_lastMelee);
        if (hw != ~0ULL && (hw & 0xff))
            mw = g_lastMelee;
    }
    if (!(CdDisClass(mw, NULL) & CdMfDmask(fin)))
        return;
    // v5-disdraw (the bat teleported during the disarm): the instant draw popped the stowed bat from the back into the
    // hand on the windup's first frame (A 16167, pistol out). Now the engine's ANIMATED draw (player-draw-weapon-by-name, the weapon wheel's draw) runs
    // through the 500 ms intro; cd_dis_tick's instant draw at the pair start stays the fallback. t2r-dis-instant-draw.txt = the instant draw here.
    int anim =
        GetFileAttributesA("t2r-dis-instant-draw.txt") ==
        INVALID_FILE_ATTRIBUTES;
    uint64_t r;
    if (anim) {
        union {
            float f;
            uint32_t u;
        } z = { 0.0f };
        uint64_t a[2] = { mw, z.u };
        r = DcCall(FNV64("player-draw-weapon-by-name"), 2, a);
    } else
        r = XrDraw(mw);
    Xlog("ctxdown: DISARM windup %s (victim): our carried %llx is not in the hand (held %llx) - drawn at the windup's start, not at the take frame (%s draw %llx, held now %llx)", CD_PAIRS[fin].name, (unsigned long long) mw, (unsigned long long) cw, anim ? "ANIMATED" : "instant", (unsigned long long) r, (unsigned long long) CurrentWeapon());
}

static void CdDisTick(double t)
{
    if (g_cdDis.uid && (!g_ps.role || g_ps.uid != g_cdDis.uid)) {     // the move is over / replaced
        if (!g_cdDis.done) {
            CdDisUnhide
                ("the move ended before the hand-off: no transfer");
            Xlog("ctxdown: disarm u%08x cut before the hand-off - the weapon stays with the victim", g_cdDis.uid);
        }
        memset(&g_cdDis, 0, sizeof g_cdDis);
    }
    if (!g_ps.role || g_ps.kind != PS_CTXDOWN)
        return;
    if (g_cdDis.uid != g_ps.uid) {
        int p = CdPairOfMove(g_ps.move);
        if (p < 0 || !CdMfRow(p) || !CdMfDmask(p))
            return;
        memset(&g_cdDis, 0, sizeof g_cdDis);
        g_cdDis.uid = g_ps.uid;
        g_cdDis.pair = p;
        g_cdDis.role = g_ps.role;
        g_cdDis.vic = g_ps.vic;
        g_cdDis.att = g_ps.att;
        g_cdDis.w =
            g_ps.role ==
            2 ? CurrentWeapon() : CdDisPeerWeapon(g_ps.vic);
        // v5-dis4: the attacker picked this row for the weapon the victim CARRIES (cd_fin_pick, MSG_MOWN) - not in its hand: the victim's client draws it now
        // (SP's defender holds its weapon through the disarm), every other screen takes the carried one as the disarm's weapon
        if (!CdDisClass(g_cdDis.w, NULL) && !MownDisHeldOnly()) {
            if (g_ps.role == 2) {
                uint64_t cw = g_cdDis.w == ~0ULL ? 0 : g_cdDis.w, mw =
                    XrMeleeWeapon(cw);
                if (!CdDisClass(mw, NULL) && g_lastMelee) {
                    uint64_t hw =
                        DcCall(FNV64("has-weapon?"), 1, &g_lastMelee);
                    if (hw != ~0ULL && (hw & 0xff))
                        mw = g_lastMelee;
                }
                if (CdDisClass(mw, NULL) & CdMfDmask(p)) {
                    uint64_t r = XrDraw(mw);
                    Xlog("ctxdown: DISARM %s u%08x (victim): our carried %llx is not in the hand (held %llx) - drawn for it (instant draw %llx, held now %llx)", CD_PAIRS[p].name, g_ps.uid, (unsigned long long) mw, (unsigned long long) cw, (unsigned long long) r, (unsigned long long) CurrentWeapon());
                    g_cdDis.w = mw;
                }
            } else {
                uint64_t c = MownPeer(g_ps.vic);
                if (CdDisClass(c, NULL))
                    g_cdDis.w = c;
            }
        }
        g_cdDis.take_s =
            (CdMfIntro(p) ? CdSegLen(p, 0) : 0.0f) + CD_PAIRS[p].dmax;
        // v5-distake (on a disarm the axe was not transferred until the second half of the animation, while the first half had a slash):
        // the iron-rhino / punch-n-hack attacker clips swing the taken axe at frame 36-37 (their weapon-effect events 23/36/59, 21/37/60) - dmax (1.59 / 2.16 s)
        // is the kill blow, not the take. The take is frame 35 (1.15 s). t2r-dis-late-take.txt = dmax.
        // double-wrist (the 2x4 was not in the attacker's hands during the disarm, the knife in its place): ellie-disarm-kill-2hblunt-double-wrist--ellie
        // swings the taken 2x4 at frame 42 (whoosh) and hits at 47-48 (melee-effect) - before dmax 1.67 s (frame 50); its take is frame 37 (1.23 s).
        if (GetFileAttributesA("t2r-dis-late-take.txt") ==
            INVALID_FILE_ATTRIBUTES) {
            float tk = strstr(CD_PAIRS[p].name, "iron-rhino")
                || strstr(CD_PAIRS[p].name,
                          "punch-n-hack") ? 1.15f : strstr(CD_PAIRS[p].
                                                           name,
                                                           "double-wrist")
                ? 1.20f : -1.0f;
            if (tk > 0.0f)
                g_cdDis.take_s =
                    (CdMfIntro(p) ? CdSegLen(p, 0) : 0.0f) + tk;
        }
        const char *n = NULL;
        uint64_t m = CdDisClass(g_cdDis.w, &n);
        Xlog("ctxdown: DISARM %s u%08x (role %d): the victim (slot %d) holds %s (%llx, SP class %llx, the row wants %llx) - take at %.2f s, transfer at the hand-off %.2f s", CD_PAIRS[p].name, g_ps.uid, g_ps.role, g_ps.vic, n ? n : "NO MELEE", (unsigned long long) g_cdDis.w, (unsigned long long) m, (unsigned long long) CdMfDmask(p), g_cdDis.take_s, (double) g_ps.kill_s);
        if (!m)
            g_cdDis.done = 1;  // nothing to take (already swapped): the move plays, no transfer
    }
    if (g_cdDis.done)
        return;
    if (!g_cdDis.hid && t >= g_cdDis.take_s) {
        uint8_t *w = CdDisHeldProc(g_cdDis.role, g_ps.ent_vic);
        // v5-dishide (buggy axe behavior on disarm, on the disarmed player's view): on the victim's own screen the held lookup found no item
        // (A 22494 / 30940: the axe drawn instantly for the disarm is not the controller's held proc yet) - the axe stayed in our hand to the hand-off 1.5 s
        // later while the attacker swung its copy. Our own inventory's instance of the taken weapon is hidden instead.
        if (!w && g_cdDis.role == 2) {
            uint8_t *me =
                ((uint8_t * (*)(void)) T2R_SYM(kFnLocalPlayer)) ();
            if (me)
                w = HdebtProc(me, g_cdDis.w);
        }
        g_cdDis.hid = -1;
        if (w && !IsBadReadPtr(w + 0x840, 8) && !IsBadReadPtr(w + 0x1b0, 8)
            && BaseWeapon(PstSidOf(w)) == BaseWeapon(g_cdDis.w)) {
            uint8_t *dc = *(uint8_t **) (w + 0x1b0);
            if (dc && !IsBadWritePtr(dc + 0x48, 1)) {
                g_cdDis.hw = w;
                g_cdDis.hdc = dc;
                g_cdDis.vis = dc[0x48];
                dc[0x48] = 0;
                g_cdDis.hid = 1;
            }
        }
        Xlog("ctxdown: disarm u%08x at %.2f s (SP's take frame): the victim's held item %s on this screen (role %d)", g_cdDis.uid, t, g_cdDis.hid == 1 ? "hidden" : "not found in its hand (already gone)", g_cdDis.role);
        if (g_cdDis.role != 1)
            CdDisPupGive();
        if (g_cdDis.role == 1 && !CdDisLateGive())
            CdDisGive("SP's take frame");     // v5-armed: SP's give-player-weapon-instance is AT the take frame - the rest of the clip is authored with it in the hand
    }
    if (t < (double) g_ps.kill_s - 0.03)
        return;
    g_cdDis.done = 1;          // the hand-off: the inventories change hands
    uint64_t m = CdDisClass(g_cdDis.w, NULL), none = 0;
    int ds = CdDisDropStyle(m);
    if (g_cdDis.vic >= 0 && g_cdDis.vic < T2R_MAX_PEERS)
        g_peerMelee[g_cdDis.vic] = -1;
    int vic_dur = MownPeerDur(g_cdDis.vic);  // v5-mown2: (read before the forget) the late give below keeps it too
    MownForget(g_cdDis.vic, g_cdDis.w);      // v5-dis4: the victim's carried weapon is the attacker's now (no death drop of it)
    if (g_cdDis.att >= 0 && g_cdDis.att < T2R_MAX_PEERS && ds >= 0)
        g_peerMelee[g_cdDis.att] = (int8_t) ds;
    if (g_cdDis.role == 2) {
        uint64_t w = g_cdDis.w, has =
            DcCall(FNV64("has-weapon?"), 1, &w), r = 0;
        if (has != ~0ULL && (has & 0xff))
            r = DcCall(FNV64("remove-player-weapon"), 1, &w);
        if (g_keepMelee && BaseWeapon(g_keepMelee) == BaseWeapon(w)) {
            Xlog("ctxdown: disarm - our kept upgraded melee %llx is gone with it (no respawn copy)", (unsigned long long) g_keepMelee);
            g_keepMelee = 0;
        }
        if (g_lastMelee && BaseWeapon(g_lastMelee) == BaseWeapon(w))
            g_lastMelee = 0;
        uint64_t has2 = DcCall(FNV64("has-weapon?"), 1, &w);
        Xlog("ctxdown: DISARMED (victim): %llx taken from us - has-weapon? %llx -> remove-player-weapon %llx -> has-weapon? %llx", (unsigned long long) w, (unsigned long long) has, (unsigned long long) r, (unsigned long long) has2);
        if (has2 != ~0ULL && (has2 & 0xff))
            CdDisUnhide
                ("remove-player-weapon refused: the victim keeps it");
        else
            g_cdDis.hid = 0;   // (gone: nothing to show again)
    } else if (g_cdDis.role == 1) {
        uint64_t hm = DcCall(SID_HAS_MELEE_WEAPON, 0, &none), w =
            BaseWeapon(g_cdDis.w);
        if (!w)
            w = g_cdDis.w;
        if (g_cdDis.given) {   // v5-armed: given at the take frame - only a draw that did not show in the hand is tried again here
            uint64_t cw = CurrentWeapon();
            if (cw != w && BaseWeapon(cw) != w) {
                typedef union {
                    float f;
                    uint32_t u;
                } FuT;
                FuT z = { 0.0f };
                uint64_t a[2] = { w, z.u }, rd =
                    DcCall(FNV64("player-instant-draw-weapon-by-name"), 2,
                            a);
                Xlog("ctxdown: DISARM (attacker) hand-off: %llx (given at the take frame) was not in the hand (held %llx) - instant draw again %llx, held %llx", (unsigned long long) w, (unsigned long long) cw, (unsigned long long) rd, (unsigned long long) CurrentWeapon());
            } else
                Xlog("ctxdown: DISARM (attacker) hand-off: %llx in the hand since the take frame", (unsigned long long) w);
        } else if (hm != ~0ULL && (hm & 0xff))
            Xlog("ctxdown: DISARM (attacker): we hold a melee weapon already - %llx not taken (no second one)", (unsigned long long) w);
        else if (!g_give_orig)
            Xlog("ctxdown: DISARM (attacker): no give hook - %llx not taken", (unsigned long long) w);
        else {
            uint64_t ret[64] = { 0 }, give[4] = { w, 0, 0, 1 };
            if (vic_dur > 0 && !MownOff())
                PdurWant(w, vic_dur, "a disarm (late give): the victim's durability"); // v5-mown2: the pickup edge sets it
            g_give_orig(ret, 4, give, 0);
            typedef union {
                float f;
                uint32_t u;
            } FuT;
            FuT z = { 0.0f };
            uint64_t a[2] = { w, z.u }, rd =
                DcCall(FNV64("player-instant-draw-weapon-by-name"), 2, a);
            uint64_t has = DcCall(FNV64("has-weapon?"), 1, &w), hp =
                DcCall(SID_GET_WEAPON_HEALTH, 0, &none);
            Xlog("ctxdown: DISARM (attacker): the victim's %llx is ours (SP give-player-weapon-instance) - give %llx, has-weapon? %llx, instant draw %llx, durability %lld (a fresh pickup's)", (unsigned long long) w, (unsigned long long) ret[0], (unsigned long long) has, (unsigned long long) rd, (long long) hp);
        }
        g_cdDis.hid = 0;       // (the victim's client removes it: its puppet's hand empties by itself)
    } else {
        Xlog("ctxdown: disarm u%08x hand-off (bystander): slot %d's %llx goes to slot %d (death drop moved)", g_cdDis.uid, g_cdDis.vic, (unsigned long long) g_cdDis.w, g_cdDis.att);
        g_cdDis.hid = 0;
    }
}

// ps49 (melee_duel.c forced finisher pass): the path gate's wall-only BLOCKED is accepted while g_cd49_force is set
#define CD49_ROOM_M 0.35f       /* [TUNING ps49] m of room the victim's path keeps before the wall */
static int g_cd49Force;
static int Cd49Off(void)
{
    static int v = -1;
    static uint64_t at;
    uint64_t n = GetTickCount64();
    if (v < 0 || n - at > 2000) {
        at = n;
        int o =
            GetFileAttributesA("t2r-fin-force-ps49-old.txt") !=
            INVALID_FILE_ATTRIBUTES;
        if (o != v)
            Xlog("ctxdown: ps49 forced finisher %s",
                 o ?
                 "OFF (t2r-fin-force-ps49-old.txt): a room that blocks every row = the plain down blow"
                 :
                 "ON - when every row is refused by the room, a row blocked by a wall only plays with the victim held at the wall");
        v = o;
    }
    return v;
}

static int MdgSlotImmune(uint32_t sl);        // melee_dodge.c
static int CdUpgGo(uint32_t slot, uint64_t w, int style, int forced)
{
    g_cdFinSyncSpot = 0;
    // (B's dodge was accepted on A ('immune for 380 ms') yet the lethal contact started the cover-face-smash surface takedown
    // 20 ms BEFORE the duel's dodge ruling said DODGED - B was grabbed out of its dodge and killed): no takedown / finisher starts on a victim in dodge i-frames.
    if (!g_cdFinProbe && slot < T2R_MAX_PEERS && MdgSlotImmune(slot)
        && GetFileAttributesA("t2r-takedown-on-dodger.txt") ==
        INVALID_FILE_ATTRIBUTES) {
        static int nl;
        if (LOGCAP(nl, 30))
            Xlog("ctxdown: slot %u is in dodge i-frames - no takedown / finisher start (the dodge ruling decides the blow)", slot);
        return 0;
    }
    typedef uint8_t *(*fn_peer)(uint32_t);
    int pair = forced;
    if (pair < 0) {
        static unsigned rr[2];
        int stab = style == 0 && CdUpgStabOn();
        float fw = 0.0f, sd = 0.0f;
        int dir = FinisherDir((int32_t) slot, &fw, &sd), back = dir >= 0
            && fw < 0.0f;
        int cand[CD_N], nc = 0, nside = 0, any = 0;
        char miss[200];
        int mo = 0;
        miss[0] = 0;
        for (int pass = 0; pass < 2 && !nc; pass++)
            for (size_t i = 0; i < CD_N; i++) {
                if (CD_PAIRS[i].kind != 0 || CD_PAIRS[i].style != style
                    || CdMfRow((int) i))
                    continue;   // (the duel's finisher rows "mf-" are cd_fin_pick's)
                if (style == 0
                    && CdIsMove("upg-crowbar-eye-jab", (int) i) != stab)
                    continue;   // the eye jab only on the opt-out file (and then only it)
                if (!pass && !stab
                    && (strstr(CD_PAIRS[i].name, "-back-") != NULL) !=
                    back)
                    continue;   // pass 0: our side's rows, pass 1: any row of the style
                if (!pass)
                    nside++;
                any = pass;
                if (!CdPairLoaded((int) i)) {
                    if (mo < (int) sizeof miss - 48)
                        mo +=
                            snprintf(miss + mo, SNLEFT(miss, mo), "%s%s",
                                     mo ? ", " : "", CD_PAIRS[i].name);
                    continue;
                }
                cand[nc++] = (int) i;
            }
        if (!nc) {
            Xlog("melee upgrade: no loaded TLOU2 one-shot pair for style %d (clips not loaded here: %s) - plain down hit", style, mo ? miss : "no rows");
            return 0;
        }
        pair = cand[rr[back] % (unsigned) nc];
        rr[back]++;
        Xlog("melee upgrade: %s %s picked for slot %u (we are %s the victim: fwd %.2f side %.2f m, dir %d; %d %s row(s) loaded of %d%s%s, round robin %u)", style ? "blade one-shot" : stab ? "crowbar eye jab (t2r-upg-crowbar-stab.txt)" : "hammer finisher", CD_PAIRS[pair].name, slot, back ? "BEHIND" : "in front of", fw, sd, dir, nc, any ? "any-side (none loaded for ours)" : back ? "back" : "front", any ? nc : nside, mo ? "; not loaded: " : "", mo ? miss : "", rr[back] - 1);
    }
    // (no punch -> finisher, just finisher): a duel finisher (forced) replaces the duel's own running -hit pair the way the combo's next blow
    // does - psync_start's own "a new move" abort (reason 0, the victim chains it), not an explicit psync_abort (that path released the victim: teleport + death)
    if (slot >= T2R_MAX_PEERS
        || (g_ps.role
            && !(forced >= 0 && MduInPair() && g_ps.vic == (int) slot)))
        return 0;
    uint8_t *peer = ((fn_peer) T2rPeer) (slot);
    uint64_t h = peer ? *(uint64_t *) (peer + 0x60) : 0;
    float vb[16], mb[16];
    const float *vl = h ? PuppetLocator(h, vb) : NULL, *me =
        PlayerLocator(mb);
    if (!vl || !me || !PsLocOk(vl) || !PsLocOk(me)) {
        Xlog("melee upgrade: %s on slot %u not started (no locators)",
             CD_PAIRS[pair].name, slot);
        return 0;
    }
    float dx = me[0] - vl[0], dz = me[2] - vl[2], d =
        __builtin_sqrtf(dx * dx + dz * dz);
    if (d < 0.05f || d > 4.0f) {
        Xlog("melee upgrade: %s on slot %u not started (distance %.2f m)",
             CD_PAIRS[pair].name, slot, d);
        return 0;
    }
    float cv[8], ca[8];
    // v5-finwind: the intro passed over = the -sync pair starts from the intro's END spot: the pair is turned so THAT spot (not the intro's frame 0) lies toward us
    float att_t = g_cdFinWindup && g_cdFinSkipIntro && forced >= 0
        && CdMfIntro(pair) ? CdSegLen(pair, 0) * 0.999f : 0.0f;
    // v5-sfx (A [27214]-[27215]: the hammer finisher's attacker SNAPPED 0.85 m + 66 deg in one frame at the hand-over - "start 1.69 m / 66 deg
    // from the attacker's authored spot", 't=1 att ap(-0.65 0.58 y 132) exp(-0.02 1.13 y -162) err 0.84'; B [47098] the hatchet's 1.93 m / 75 deg, contact distance 0.30 m):
    // the intro is the attacker's own move - its align channel is not in the -sync pair's apReference frame - so its end spot read in that frame (0.87 m here, 0.30 m
    // for machete-slash-3) is not where the -sync attacker half starts (1.13 m at 19 deg). The pair is now turned onto the -sync attacker half's frame-0 spot, the
    // one psync places (and the duel's windup warp now closes to that distance, melee_duel.c mdu_fw_begin). t2r-fin-intro-end-spot.txt = the old intro-end spot.
    const char *att_ref = CD_ATT0(pair);
    if (att_t > 0.0f && CD_PAIRS[pair].nseg > 1
        && GetFileAttributesA("t2r-fin-intro-end-spot.txt") ==
        INVALID_FILE_ATTRIBUTES) {
        att_ref = CD_PAIRS[pair].seg[CD_PAIRS[pair].nseg - 1].att;
        att_t = 0.0f;
        g_cdFinSyncSpot = 1;
    }
    float vic_t = g_cdFinSyncSpot ? CdFinVicT(pair) : 0.0f;       // ps37: the intro is passed over - the victim's spot at the -sync switch
    if (vic_t > 0.0f) {
        static int nlv;
        if (LOGCAP(nlv, 60))
            Xlog("melee upgrade: %s - the pair is placed with the victim half's spot at the -sync switch (%.0f ms into %s), not its frame 0 (ps37)", CD_PAIRS[pair].name, (double) (vic_t * 1000.0f), CD_VIC0(pair));
    }
    if (!CdOffsetAt(h, CD_VIC0(pair), vic_t, cv)
        || !CdOffsetAt(SID_PLAYER, att_ref, att_t, ca)) {
        Xlog("melee upgrade: %s - the clips' align channels are unreadable (no authored spots) - plain down hit", CD_PAIRS[pair].name);
        return 0;
    }
    // the yaw that turns the authored victim -> attacker vector onto the victim -> us direction, checked by composing it (sign-safe), then the
    // translation that puts the victim's authored frame-0 spot where it stands
    float want = __builtin_atan2f(dx, dz), yaw =
        0.0f, a8[8], Sv[8], sa[8], res = 0.0f;
    float got0 = __builtin_atan2f(ca[0] - cv[0], ca[2] - cv[2]);        // the authored victim -> attacker direction in ap space
    float best = 1e9f, cand[2] = { want - got0, got0 - want };  // (either rotation sense: the one whose composed vector lands on us)
    for (int k = 0; k < 2; k++) {
        float ap0[7] =
            { 0, 0, 0, 0, __builtin_sinf(cand[k] * 0.5f), 0,
     __builtin_cosf(cand[k] * 0.5f) };
        PsAp8(ap0, a8);
        PsCompose(a8, cv, Sv);
        PsCompose(a8, ca, sa);
        float r =
            CdWrap180((want -
                        __builtin_atan2f(sa[0] - Sv[0],
                                         sa[2] - Sv[2])) * 57.29578f);
        if (__builtin_fabsf(r) < best) {
            best = __builtin_fabsf(r);
            yaw = cand[k];
            res = r;
        }
    }
    {
        float ap0[7] =
            { 0, 0, 0, 0, __builtin_sinf(yaw * 0.5f), 0,
     __builtin_cosf(yaw * 0.5f) };
        PsAp8(ap0, a8);
        PsCompose(a8, cv, Sv);
        PsCompose(a8, ca, sa);
    }
    if (best > 5.0f) {
        Xlog("melee upgrade: %s - the pair could not be turned onto us (residual %.1f deg) - plain down hit", CD_PAIRS[pair].name, res);
        return 0;
    }
    float ap[7] =
        { vl[0] - Sv[0], vl[1] - Sv[1], vl[2] - Sv[2], 0,
  __builtin_sinf(yaw * 0.5f), 0, __builtin_cosf(yaw * 0.5f) };
    // v5-sfx (A [27215] / B [47098]): a duel finisher never starts with our body more than 0.5 m / 30 deg off the spot where
    // its attacker half begins - psync would snap us there in one frame (the 0.85 m + 66 deg / 1.93 m + 75 deg pops). Off = this pick is refused (the contact
    // tries the swing's other finishers, then the plain down blow). t2r-fin-no-spot-guard.txt = off.
    if (forced >= 0 && !g_cdFinProbe && CdMfRow(pair)
        && GetFileAttributesA("t2r-fin-no-spot-guard.txt") ==
        INVALID_FILE_ATTRIBUTES) {
        float wx = sa[0] + ap[0], wz = sa[2] + ap[2], pe =
            __builtin_sqrtf((me[0] - wx) * (me[0] - wx) +
                            (me[2] - wz) * (me[2] - wz));
        float ye = CdWrap180(Uc4Yaw(sa) - Uc4Yaw(me));
        // v5-guard75 (B 16649 fist-2h-blunt-disarm-back-grab-n-bash refused at 0.63 m): psync's SP
        // pull (quadratic ease-out over 0.20 s) closes up to 0.75 m without a pop. t2r-fin-guard-50.txt = 0.5 m.
        // Pins / contextual takedowns activate from the sides: the turn tolerance is 40 deg (was 30).
        float gmax =
            GetFileAttributesA("t2r-fin-guard-50.txt") ==
            INVALID_FILE_ATTRIBUTES ? 0.75f : 0.5f;
        if (pe > gmax || __builtin_fabsf(ye) > CD_FIN_TURN_MAX) {
            Xlog("melee upgrade: %s on slot %u REFUSED - we stand %.2f m / %.0f deg off its attacker start (max 0.5 m / 40 deg): no one-frame snap - another finisher or the plain down blow", CD_PAIRS[pair].name, slot, (double) pe, (double) ye);
            return 0;
        }
        Xlog("melee upgrade: %s on slot %u - we stand %.2f m / %.0f deg off its attacker start (within 0.5 m / 40 deg)", CD_PAIRS[pair].name, slot, (double) pe, (double) ye);
    }
    float vturn = 0.0f;
    {
        float lb[16];
        const float *pl = PsyncLoc(h, lb);
        if (pl && PsLocOk(pl))
            vturn = CdWrap180(Uc4Yaw(Sv) - Uc4Yaw(pl));
    }
    // ps25 (A [44315], the lethal 7th blow on open ground: 'melee upgrade: mf-fist-2h-blunt-disarm-back-grab-n-bash-finisher ... we stand 0.00 m / 11 deg off
    // its attacker start' then 'finisher refused: victim 0.00 m / 53 deg off (max 0.5 m / 40 deg)'; the two fallbacks the same way - flip-kick 'victim 54 deg off', choke
    // 'victim 41 deg off' - and the contact finisher again 'victim 50 deg off'; every path gate read 'clear'). Not the path, not the locators, not psync: the victim
    // stood side-on (the pick at [44293]: 'BEHIND the victim: fwd -0.03'), and the pair is always turned so that WE stand exactly on our authored spot - the whole
    // facing error lands on the victim, and a side-on victim is 41-54 deg off every back row. The error is now SHARED: when only the victim's turn fails, the pair is
    // turned about the victim by what it is over (+ 2 deg), up to CD_FIN_SPLIT_MAX, as long as our own start then stays inside the same guard (0.75 m / 40 deg;
    // SP's finishers start with the attacker up to 80 deg / 2.67 m off, the defender up to 38 deg: re/sp-melee-spec-1110.md 8). Both senses are composed and the one
    // that brings the victim inside is kept (as for the pair turn above). The gate probe ahead of the contact gets the same turn. t2r-fin-turn-split-ps24.txt = off.
#define CD_FIN_SPLIT_MAX 25.0f  /* [TUNING ps25] deg the pair may be turned off our exact spot to bring the victim's turn inside CD_FIN_TURN_MAX */
    if (forced >= 0 && CdMfRow(pair)
        && __builtin_fabsf(vturn) > CD_FIN_TURN_MAX
        && __builtin_fabsf(vturn) <=
        CD_FIN_TURN_MAX + CD_FIN_SPLIT_MAX - 2.0f
        && GetFileAttributesA("t2r-fin-no-spot-guard.txt") ==
        INVALID_FILE_ATTRIBUTES
        && GetFileAttributesA("t2r-fin-turn-split-ps24.txt") ==
        INVALID_FILE_ATTRIBUTES) {
        float need =
            __builtin_fabsf(vturn) - CD_FIN_TURN_MAX + 2.0f, lb2[16], gm2 =
            GetFileAttributesA("t2r-fin-guard-50.txt") ==
            INVALID_FILE_ATTRIBUTES ? 0.75f : 0.5f;
        const float *pl2 = PsyncLoc(h, lb2);
        int done = 0;
        for (int k = 0; k < 2 && !done && pl2 && PsLocOk(pl2); k++) {
            float y2 =
                yaw + (k ? -need : need) * 0.01745329f, b8[8], sv2[8],
                sa2[8];
            float apk[7] =
                { 0, 0, 0, 0, __builtin_sinf(y2 * 0.5f), 0,
         __builtin_cosf(y2 * 0.5f) };
            PsAp8(apk, b8);
            PsCompose(b8, cv, sv2);
            PsCompose(b8, ca, sa2);
            float vt2 = CdWrap180(Uc4Yaw(sv2) - Uc4Yaw(pl2));
            if (__builtin_fabsf(vt2) > CD_FIN_TURN_MAX)
                continue;       // (the other rotation sense)
            float ax2 = vl[0] - sv2[0], az2 = vl[2] - sv2[2], wx2 =
                sa2[0] + ax2, wz2 = sa2[2] + az2;
            float pe2 =
                __builtin_sqrtf((me[0] - wx2) * (me[0] - wx2) +
                                (me[2] - wz2) * (me[2] - wz2)), ye2 =
                CdWrap180(Uc4Yaw(sa2) - Uc4Yaw(me));
            if (!g_cdFinProbe
                && (pe2 > gm2 || __builtin_fabsf(ye2) > CD_FIN_TURN_MAX)) {
                Xlog("melee upgrade: %s on slot %u - sharing the victim's %.0f deg turn would put us %.2f m / %.0f deg off our start (max %.2f m / %.0f deg): not shared", CD_PAIRS[pair].name, slot, (double) __builtin_fabsf(vturn), (double) pe2, (double) ye2, (double) gm2, (double) CD_FIN_TURN_MAX);
                continue;
            }
            Xlog("melee upgrade: %s on slot %u - TURN SHARED (ps25): the pair is turned %.0f deg about the victim, the victim now turns %.0f deg (was %.0f, max %.0f), we start %.2f m / %.0f deg off our authored spot%s", CD_PAIRS[pair].name, slot, (double) need, (double) vt2, (double) vturn, (double) CD_FIN_TURN_MAX, (double) pe2, (double) ye2, g_cdFinProbe ? " (gate probe)" : "");
            yaw = y2;
            vturn = vt2;
            ap[0] = ax2;
            ap[1] = vl[1] - sv2[1];
            ap[2] = az2;
            ap[4] = __builtin_sinf(y2 * 0.5f);
            ap[6] = __builtin_cosf(y2 * 0.5f);
            done = 1;
        }
    }
    int t46ok = 0;
    if (forced >= 0 && CdMfRow(pair)
        && __builtin_fabsf(vturn) > CD_FIN_TURN_MAX && CdT46On()
        && GetFileAttributesA("t2r-fin-no-spot-guard.txt") ==
        INVALID_FILE_ATTRIBUTES) {
        // ps46 lethal turn: first take what our own start guard allows off the victim's turn (the ps25 split, partial: up to CD_FIN_SPLIT_MAX - 2 deg), then the rest
        // is the victim's eased turn into the row (psync's SP pull, see cd_t46_on)
        float v0 = vturn, lb3[16], gm3 =
            GetFileAttributesA("t2r-fin-guard-50.txt") ==
            INVALID_FILE_ATTRIBUTES ? 0.75f : 0.5f, shared = 0.0f;
        const float *pl3 = PsyncLoc(h, lb3);
        float need3 = __builtin_fabsf(vturn) - CD_FIN_TURN_MAX + 2.0f;
        if (need3 > CD_FIN_SPLIT_MAX - 2.0f)
            need3 = CD_FIN_SPLIT_MAX - 2.0f;
        for (int k = 0;
             k < 2 && pl3 && PsLocOk(pl3)
             && GetFileAttributesA("t2r-fin-turn-split-ps24.txt") ==
             INVALID_FILE_ATTRIBUTES; k++) {
            float y3 =
                yaw + (k ? -need3 : need3) * 0.01745329f, b8[8], sv3[8],
                sa3[8];
            float apk[7] =
                { 0, 0, 0, 0, __builtin_sinf(y3 * 0.5f), 0,
         __builtin_cosf(y3 * 0.5f) };
            PsAp8(apk, b8);
            PsCompose(b8, cv, sv3);
            PsCompose(b8, ca, sa3);
            float vt3 = CdWrap180(Uc4Yaw(sv3) - Uc4Yaw(pl3));
            if (__builtin_fabsf(vt3) >= __builtin_fabsf(vturn))
                continue;       // (the other rotation sense)
            float ax3 = vl[0] - sv3[0], az3 = vl[2] - sv3[2], wx3 =
                sa3[0] + ax3, wz3 = sa3[2] + az3;
            float pe3 =
                __builtin_sqrtf((me[0] - wx3) * (me[0] - wx3) +
                                (me[2] - wz3) * (me[2] - wz3)), ye3 =
                CdWrap180(Uc4Yaw(sa3) - Uc4Yaw(me));
            if (!g_cdFinProbe
                && (pe3 > gm3 || __builtin_fabsf(ye3) > CD_FIN_TURN_MAX))
                continue;
            yaw = y3;
            vturn = vt3;
            shared = need3;
            ap[0] = ax3;
            ap[1] = vl[1] - sv3[1];
            ap[2] = az3;
            ap[4] = __builtin_sinf(y3 * 0.5f);
            ap[6] = __builtin_cosf(y3 * 0.5f);
            break;
        }
        if (__builtin_fabsf(vturn) <= CD_T46_TURN_MAX) {
            int intro_plays = CdMfIntro(pair) && !g_cdFinSkipIntro
                && GetFileAttributesA("t2r-duel-fin-no-intro.txt") ==
                INVALID_FILE_ATTRIBUTES;
            float ts = __builtin_fabsf(vturn) / 180.0f, blow =
                (intro_plays ? CdSegLen(pair, 0) : 0.0f) +
                CD_PAIRS[pair].dmax;
            if (ts < PS_SP_PULL_S)
                ts = PS_SP_PULL_S;
            if (ts > 1.0f)
                ts = 1.0f;
            if (!CdT46Turn180On()
                && __builtin_fabsf(vturn) > CD_T46_TURN_SAFE
                && blow < ts) {
                static int nlr46;
                if (LOGCAP(nlr46, 200))
                    Xlog("ctxdown: ps46b lethal turn %s on slot %u REFUSED: the victim would turn %+.0f deg (> %.0f) over ~%.2f s but the blow lands at +%.2f s - mid-spin; " "the row falls back to the old refusal (t2r-fin-turn180-ps46-on.txt = allow)%s", CD_PAIRS[pair].name, slot, (double) vturn, (double) CD_T46_TURN_SAFE, (double) ts, (double) blow, g_cdFinProbe ? " (gate probe)" : "");
            } else {
                t46ok = 1;
                static int nl46;
                if (LOGCAP(nl46, 200))
                    Xlog("ctxdown: ps46 lethal turn %s on slot %u: the victim stands %.0f deg off the row's facing (max %.0f): the pair is turned %.0f deg about it (our start guard), the victim turns %+.0f deg into the row, " "eased by psync's SP pull over ~%.2f s (180 deg/s, quadratic ease-out, >= %.2f s); the blow lands at +%.2f s%s%s", CD_PAIRS[pair].name, slot, (double) __builtin_fabsf(v0), (double) CD_FIN_TURN_MAX, (double) shared, (double) vturn, (double) ts, (double) PS_SP_PULL_S, (double) blow, blow < ts ? (__builtin_fabsf(vturn) > CD_T46_TURN_SAFE ? " - BEFORE the turn ends (t2r-fin-turn180-ps46-on.txt; unverified look)" : " - before the turn ends (<= 90 deg: ps46b allows it)") : "", g_cdFinProbe ? " (gate probe)" : "");
            }
        }
    }
    // A 44821: the victim was 101 deg off its authored spot at the start (it had dodged away) - the SP pull dragged it ~2 m and turned it over 1 s, the blow landed
    // mid-pull. Same tolerance as our own guard above (30 deg; the victim is placed where it stands, so only its facing can be off). t2r-fin-no-spot-guard.txt = off.
    if (forced >= 0 && !g_cdFinProbe && !t46ok && CdMfRow(pair)
        && GetFileAttributesA("t2r-fin-no-spot-guard.txt") ==
        INVALID_FILE_ATTRIBUTES
        && __builtin_fabsf(vturn) > CD_FIN_TURN_MAX) {
        Xlog("melee upgrade: finisher refused: victim 0.00 m / %.0f deg off (max 0.5 m / 40 deg) - %s on slot %u not started, the swing plays as a normal blow / miss", (double) __builtin_fabsf(vturn), CD_PAIRS[pair].name, slot);
        return 0;
    }
    {                           // the first segment's END spots (the chain continues from there): wall / floor, like the 2013 pipe finisher (pg_gate in t2r-ext.c)
        g_pgLie = 1;           // v5-fincol: the victim goes down at the hand-off - its farthest root spot + the lying body past it are gated (A [25351]-[25644])
        int g =
            PgGate(slot, CD_ATT0(pair), CD_VIC0(pair), ap, vl, me, 0,
                    "TLOU2 one-shot");
        g_pgLie = 0;
        if (g == 2)
            return 2;
        // v5-fb2 (both players clipped into a wall and fell out of bounds; dlog3 116299-116318, log 90013-90199, Hospital
        // 255 13.7 -98): the duel finisher's gate was asked every frame while both bodies drifted; fist-nose-break-kill was BLOCKED twice
        // ('victim ... wall t 0.28 / 0.37, a wall in the way | attacker ... floor 12.73 vs 13.73, no floor under the end spot'), every other
        // finisher was blocked too, then ONE poll 170 ms later read 'clear' (the victim's path had swung 25 deg past the wall's end) and the
        // pair started: the victim was carried into the wall (the stuck guard: THROUGH GEOMETRY), the attacker was handed back at the
        // drop's edge and fell 5 m ('killed by YOURSELF (fall)'). A pair blocked on this slot within PS_GATE_STICKY_MS stays blocked:
        // one lucky ray set never overrides a wall / drop the gate saw a moment ago. t2r-no-gate-sticky.txt = off.
        {
            static uint64_t blk_ms[CD_N][T2R_MAX_PEERS];
            static int off = -1;
            static uint64_t oat;
            uint64_t nowm = GetTickCount64();
            if (off < 0 || nowm - oat > 2000) {
                oat = nowm;
                off =
                    GetFileAttributesA("t2r-no-gate-sticky.txt") !=
                    INVALID_FILE_ATTRIBUTES;
            }
            if (pair >= 0 && pair < (int) CD_N && slot < T2R_MAX_PEERS) {
                if (!g)
                    blk_ms[pair][slot] = nowm;
                else if (!off && blk_ms[pair][slot]
                         && nowm - blk_ms[pair][slot] < 700) {
                    Xlog("melee upgrade: %s on slot %u - the path gate reads clear now but was BLOCKED %llu ms ago - kept blocked (sticky gate, v5-fb2)", CD_PAIRS[pair].name, slot, (unsigned long long) (nowm - blk_ms[pair][slot]));
                    g = 0;
                }
            }
        }
        if (!g && g_cdFinGateKeep == pair + 1) {     // ps41 gate first: the intro that already plays was started on this gate's YES for the same victim spot / bearing
            Xlog("melee upgrade: %s on slot %u - the path gate reads BLOCKED now, but it answered clear for these spots before the finisher's intro started: the started intro is not refused (ps41; t2r-fin-gate-first-off.txt = old)", CD_PAIRS[pair].name, slot);
            g = 1;
        }
        if (!g && g_cd49Force && g_pgLastBad && !(g_pgLastBad & ~3)
            && (!(g_pgLastBad & 2) || g_pgLastVwall >= CD49_ROOM_M)
            && CdMfRow(pair)) {
            Xlog("melee upgrade: %s on slot %u - ps49 FORCED: the path meets a wall only (bad %#x, the victim's wall %.2f m along its path, floors fine) - it plays, the travel clamp holds the bodies at the wall", CD_PAIRS[pair].name, slot, (unsigned) g_pgLastBad, (double) g_pgLastVwall);
            g = 1;
        }
        if (!g) {
            Xlog("melee upgrade: %s on slot %u - the path is blocked (path gate) - plain down hit", CD_PAIRS[pair].name, slot);
            return 0;
        }
    }
    if (g_cdFinProbe)
        return 3;               // (v5-finwind: the gate's verdict asked ahead, cached for the real start)
    float end_s = CdChainTotal(pair);
    {
        uint64_t me0 = SID_PLAYER, r =
            DcCall(0x394888944e949d5fULL /* cancel-melee-action */ , 1,
                    &me0);
        Xlog("melee upgrade: our swing on slot %u cancelled for %s (-> %llx)", slot, CD_PAIRS[pair].name, (unsigned long long) r);
    }
    g_psFinPull = forced >= 0;        // the duel finisher: the attacker's run-in pull (paired.c), not SP's 4.15 m/s snap
    // v5 (kick / hard punch finishers played right away, skipping the first half of the anim, the windup): the row's
    // SP intro (segment 0) plays from its frame 0 on this press frame; the -sync pair follows at the intro's end. t2r-duel-fin-no-intro.txt = the old start
    // straight into the -sync pair (the move clock starts at the hand-over: the intro is passed over, both screens).
    int no_intro = CdMfIntro(pair) && (g_cdFinSkipIntro
                                         ||
                                         GetFileAttributesA
                                         ("t2r-duel-fin-no-intro.txt") !=
                                         INVALID_FILE_ATTRIBUTES);
    g_psStartSkipMs = no_intro ? CdSegLen(pair, 0) * 1000.0f : 0.0f;
    int pst =
        PsyncStart(PS_CTXDOWN, slot, CD_ATT0(pair), CD_VIC0(pair), ap,
                    end_s, 0, 0xff, w, FNV64(CD_PAIRS[pair].name));
    g_psFinPull = 0;
    if (pst && forced >= 0 && CdMfIntro(pair) && !no_intro) {
        float ad0 =
            __builtin_sqrtf((ca[0] - cv[0]) * (ca[0] - cv[0]) +
                            (ca[2] - cv[2]) * (ca[2] - cv[2]));
        if (ad0 > d + 0.30f) {
            g_psFinStepUid = g_ps.uid;
            Xlog("ctxdown: finisher %s - its intro starts %.2f m from the victim, we stand %.2f m off: the full intro plays, our body holds its distance until the intro's own run-in closes in (no %.2f m drag back)", CD_PAIRS[pair].name, ad0, d, ad0 - d);
        }
    }
    if (pst && CdMfRow(pair))
        Xlog("ctxdown: finisher %s on us (attacker): %s - start phase %.3f, windup %.0f ms to the hand-over, then %s from phase 0 (victim half deferred to it); SP damage tick at +%.0f ms, hand-off at %.2f s%s", CD_PAIRS[pair].name, CdMfIntro(pair) ? CD_PAIRS[pair].seg[0].att : "(no SP intro: the -sync pair at once)", g_ps.att_dur > 0.0f ? fminf(0.95f, g_ps.skip_ms / 1000.0f / g_ps.att_dur) : 0.0f, CdMfIntro(pair) && !no_intro ? CdSegLen(pair, 0) * 1000.0f : 0.0f, CD_PAIRS[pair].seg[CD_PAIRS[pair].nseg - 1].att, (CdMfIntro(pair) ? CdSegLen(pair, 0) * 1000.0f : 0.0f) + CD_PAIRS[pair].dmax * 1000.0f, end_s, no_intro ? (g_cdFinSkipIntro ? (att_t > 0.0f || g_cdFinSyncSpot ? (g_cdFinSyncSpot ? " - the SP intro already played as the dodgeable windup: the -sync pair from the contact, turned on its own attacker start spot (v5-sfx)" : " - the SP intro already played as the dodgeable windup (melee_duel.c v5-finwind): the -sync pair from the contact, turned on the intro's end spot") : " - INTRO PASSED OVER (the swing's contact already landed: the -sync pair at once)") : " - INTRO PASSED OVER (t2r-duel-fin-no-intro.txt)") : "");
    if (!pst) {
        Xlog("melee upgrade: %s on slot %u not started (psync refused, see above)", CD_PAIRS[pair].name, slot);
        return 0;
    }
    g_cd.ended = 0;
    g_cd.plan_ok = 0;
    g_cdUpgSlot = (int32_t) slot;
    g_cdUpgW = w;
    // (tomahawk gut chop + kick): the move's start script runs player-begin-holster (the holster fix swaps the weapon anim state to
    // s_weapon-holster^no-weapon), the release at 1.47 s opens the gates and the engine's auto-holster took the weapon (own-held -> 0) with its prop
    // still on the hand - nothing restored it (only upg13_start's 2013 path had xr_begin). Recorded now, xr_tick restores at the release (g_cd_att_free).
    if (w)
        XrBegin(w, w, "upgraded TLOU2 one-shot");      // (a duel finisher with empty hands: no weapon to restore)
    g_visVicAnim = CD_VIC0(pair);
    g_vis_vic_puppet = h;
    g_vis_vic_frame = g_t60;
    g_mpose_att_frame = g_vis_vic_frame;
    g_mposeAttSlot = (int32_t) slot;
    g_finposVictim = (int32_t) slot;
    g_finposSlot = -1;
    g_finPlaceFrame = 0;
    g_execAttFrame = 0;
    FinposBegin(1, (int) slot, 0, S60(end_s) + 60,
                 "upgraded melee one-shot, attacker");
    XfaceStart(SID_PLAYER, h, end_s + 0.6f);
    CdCam(CD_ATT0(pair), h, end_s, 1);
    Xlog("melee upgrade: TLOU2 one-shot %s on slot %u (weapon %llx, %.2f m): %s on us, %s on them, pair turned %.0f deg (authored attacker spot -> us, residual %.1f deg), " "victim turns %.0f deg onto its authored facing; victim DOWN at %.2f s (hand-off), never killed", CD_PAIRS[pair].name, slot, (unsigned long long) w, d, CD_ATT0(pair), CD_VIC0(pair), yaw * 57.29578f, res, vturn, end_s);
    return 1;
}

// ---------------------------------------------------------------------------------------------------- victim
// from psync_begin_remote (START of kind PS_CTXDOWN, we are the victim): 0 = refused (ABORT goes back, the attacker plays the plain down)
static int DcVictimBegin(uint32_t slot, float end_s, uint64_t skipf, int pr); // dcounter.c
static int MsqVictimBegin(uint32_t slot, float end_s, uint64_t skipf, int pr);        // melee_seq.c: the synced pair of a melee exchange (kind 6 rows)
static int PulVictimBegin(uint32_t slot, float end_s, uint64_t skipf, int pr);        // melee_pullout.c: the prone pull-out pairs (kind 8)
static int MsqAborted(int role, uint32_t from);
static int DcAborted(int role, uint32_t from);
static void DcTick(void);
static uint64_t g_cdVicAbortT60;     // frame of the last ABORT that reached us as the victim of a contextual pair (a pending down it armed may be superseded)
static void CdVictimCancel(const char *why);
static int CdVictimBegin(uint32_t slot, float end_s, uint64_t skipf)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    // (A 92166): the pin's held pair u003e5204 was accepted (g_down_at armed), the attacker cut it 4 ms later ("pin finisher") and sent the takedown
    // u003e5205 at once - refused by this very guard (g_down_at of the cut pair), the takedown never played. A START right after an ABORT replaces its pending down.
    if (g_downAt && !g_downed && g_cd.vic_on && g_cdVicAbortT60
        && g_t60 - g_cdVicAbortT60 < 90)
        CdVictimCancel
            ("superseded: the attacker cut the pair and sent its next START");
    {
        int dpr = CdPairOfMove(g_ps.move);
        if (dpr >= 0 && CD_PAIRS[dpr].kind == 6)
            return MsqVictimBegin(slot, end_s, skipf, dpr);
    }                           // the melee sequence pair: its own rules
    {
        int dpr = CdPairOfMove(g_ps.move);
        if (dpr >= 0 && CD_PAIRS[dpr].kind == 8)
            return PulVictimBegin(slot, end_s, skipf, dpr);
    }                           // the prone pull-out (melee_pullout.c)
    {
        int dpr = CdPairOfMove(g_ps.move);
        if (dpr >= 0 && CD_PAIRS[dpr].kind == 7)
            return MduVictimBegin(slot, end_s, skipf, dpr);
    }                           // the melee duel pair (melee_duel.c)
    {
        int dpr = CdPairOfMove(g_ps.move);
        if (dpr >= 0 && CD_PAIRS[dpr].kind >= 3)
            return DcVictimBegin(slot, end_s, skipf, dpr);
    }                           // the dodge counter: no down, its own rules
    int upg = CdPairOfMove(g_ps.move) >= 0 && CD_PAIRS[CdPairOfMove(g_ps.move)].kind == 0;        // the upgraded one-shot (t2r-no-contextual-downs.txt does not cover it)
    if (!(CdEnabled() || upg) || slot > T2R_MAX_PEERS - 1 || g_downed
        || g_downDying || g_execUntil || g_uc4DyingUntil || g_downAt
        || LocalHp() <= 0.0f || MatchSubstate() != 6
        || !__builtin_isfinite(end_s) || end_s < 0.5f || end_s > 14.0f) {
        Xlog("ctxdown: on us by slot %u refused (enabled %d, downed %d, dying %d, exec %d, down_at %d, uc4 dying %d, hp %.1f, substate %d, end_s %.2f) - the attacker plays the plain down", slot, CdEnabled(), g_downed, g_downDying, g_execUntil != 0, g_downAt != 0, g_uc4DyingUntil != 0, LocalHp(), MatchSubstate(), end_s);
        return 0;
    }
    uint8_t *peer = ((fn_peer) T2rPeer) (slot);
    uint64_t ah = peer ? *(uint64_t *) (peer + 0x60) : 0;
    MeleeLockStart(1);
    uint64_t a[2] = { SID_PLAYER, 1 };
    DcCall(FNV64("character-set-invincibility-cannot-die-threshold"), 2, a);   // survive the move at 1 hp, as a finisher on a standing us
    g_down_slot = (uint8_t) slot;
    uint64_t left = S60(end_s) > skipf + 3 ? S60(end_s) - skipf : 3;
    g_downAt = g_t60 + left - 2;       // two frames before the shared cut: the down blends the clip out first
    g_finGuardUntil = g_t60 + left + S60(1.0f);
    g_downHit.w = g_ps.weapon ? g_ps.weapon : FNV64("abby-unarmed");
    g_downHit.me = g_downHit.att = 0;
    g_downHit.ammo = 0;
    g_downHit.flag = 0;
    g_fin_vic_w = g_ps.weapon && g_ps.weapon != ~0ULL
        && MeleeBladeKills(g_ps.weapon) ? g_ps.weapon : 0;
    g_fin_vic_t60 = g_t60;      // A blade START's weapon, kept for the kill decision at the hand-off
    g_cdVicDown = 1;
    {
        int mfp = CdPairOfMove(g_ps.move);
        g_skMfVic = mfp >= 0 && CdMfRow(mfp);
    }                           // Heavy Hands II: an SP finisher pair (mf- row) ends in this down
    g_cd.vic_on = 1;
    g_cd.vic_t0 = g_t60;
    // the execution's victim side (gx_victim_begin): our UC4 own-movement loops off for the move, finpos (collision off + log), faces
    Uc4OvOffExec();
    if (g_uc4_me.cur)
        Uc4Stop(&g_uc4_me);
    if (g_uc4_pup[slot].cur)
        Uc4Stop(&g_uc4_pup[slot]);
    FinposBegin(2, (int) slot, 0, left + 60, "contextual down on us");
    if (ah)
        XfaceStart(ah, SID_PLAYER, end_s + 0.6f);
    Xlog("ctxdown: on us by slot %u%s: survive at 1 hp, controls locked, down in %.2f s (hand-off, no fall) - skip %llu ticks", slot, upg ? " (its UPGRADED melee one-shot: a down, never a kill)" : "", (float) left / 60.0f, (unsigned long long) skipf);
    CdCam(PsyncName(g_ps.vic_sid), ah, end_s, 2);
    return 1;
}

static void CdVictimCancel(const char *why)
{
    CdCamEnd(why);
    if (!g_cd.vic_on)
        return;
    Xlog("ctxdown: victim side cancelled (%s)", why);
    g_cd.vic_on = 0;
    g_cdVicDown = 0;
    g_skMfVic = 0;
    g_meleeLockFrom = 0;
    if (!g_downed) {
        g_downAt = 0;
        g_finGuardUntil = 0;
        uint64_t a[2] = { SID_PLAYER, 0 };
        DcCall(FNV64("character-set-invincibility-cannot-die-threshold"),
                2, a);
    }
}

// the other half went away (ABORT): the attacker whose victim refused plays the plain down; the victim keeps its pending down
static void CdAborted(int role, uint32_t from)
{
    if (role == 2)
        g_cdVicAbortT60 = g_t60;
    if (MduAborted(role, from))
        return;                 // a melee duel pair (melee_duel.c): the blow already landed, no plain down hit
    if (MsqAborted(role, from))
        return;                 // a melee sequence pair: the blow already landed, no plain down hit
    if (DcAborted(role, from))
        return;                 // a dodge counter: its own end (no plain down hit)
    // (psync_abort already cleared g_ps: only `from` is known - for the attacker the only other player of the move is the victim)
    CdCamEnd("aborted");
    if (role == 1) {
        Xlog("ctxdown: aborted by slot %u", from);
        if (g_psAbortReason == PS_AB_DODGED)
            Xlog("ctxdown: the victim dodged - no plain down hit");     // melee_dodge.c
        else if (g_cdUpgSlot == (int32_t) from && g_cdUpgW)
            OnetapSendMarker(from, g_cdUpgW);       // the upgraded one-shot: its plain down hit (the victim's Melee Down)
        else
            CdPlainDown((int) from, "the victim refused / aborted");
    }
    g_cdUpgSlot = -1;
    g_cdUpgW = 0;
}

// ---------------------------------------------------------------------------------------------------- sounds
// TLOU2's sfx EVENTS are silent in MP (re: mp-sound-events-silent), so the takedown's sounds go through the same two paths the executions use, on
// EVERY client (attacker, victim, third parties - cd_tick arms it from the psync move that runs), timed on the shared move clock:
//   - flesh / bone / wall / body-fall impacts and the grab: ex_play_label(...) = the ripped waves through our PCM mixer, positional on the body hit
//     (the victim's puppet / our own body), the same labels the 2013 executions' timelines use (fists: punch + finisher thump, impact-head = the
//     head into the wall / ledge, impact-body = the body down; knife: the switchblade stab / gore; blades: machete hits; blunt: pipe hits);
//   - the attacker's effort grunt and the victim's pain cries: gx_play(GXS_EFFORT / GXS_PAIN, body) = the vox `say` path (a female speaker gets her own
//     female take, fem_effort_sid), on the body that makes the sound.
// The times are fractions of the pair's hand-off time end_s (the move is the grab, the blows, the slam into the surface, the slide down), except where
// CD_HITS has the measured contact times of the clips (victim head speed peaks / wrist-to-head contacts of the decoded halves).
enum { CE_GRAB, CE_GRUNT, CE_HIT, CE_HEAVY, CE_THUD, CE_FALL, CE_WHOOSH,
        CE_PAIN, CE_CRY, CE_KICK };
// measured contact times in ms: [0] = the first blow, [1] = the heavy one (the head into the surface / the final stab); 0 = use the fractions
static const struct {
    const char *name;
    uint16_t hit[4];
} CD_HITS[] = {
    CD_HITS_DATA { NULL, { 0, 0, 0} }
};

_Static_assert(CE_GRAB == EPC_GRAB && CE_GRUNT == EPC_GRUNT
               && CE_HIT == EPC_HIT && CE_HEAVY == EPC_HEAVY
               && CE_THUD == EPC_THUD && CE_FALL == EPC_FALL
               && CE_WHOOSH == EPC_WHOOSH && CE_PAIN == EPC_PAIN
               && CE_CRY == EPC_CRY
               && CE_KICK == EPC_KICK, "explus.c EPC_* order");
static struct {
    uint32_t uid;
    int pair;
    size_t i, n;
    struct {
        uint16_t ms;
        uint8_t act;
    } ev[16];
    size_t ni, nn;
    struct {
        uint16_t ms;
        uint8_t role;
        uint64_t name, joint;
    } nat[24];
    size_t fi, fn;
    struct {
        uint16_t ms;
        uint8_t role, done, tries;
        int row;
    } fx[64];
    int native;
    float att_free_ms;
    int att_freed;
    int att_rel;                // the attacker half really went back to locomotion (psync_att_out, not t2r-cd-att-hold.txt): our move is over for the stuck guard
    uint64_t t60_0;
    int fx_ours, fx_engine, fx_fail;
} g_cdSfx;
// The exact same stab / slash and foley sounds as the references: the clips' own TLOU2 sound events (tools/ctxsfx_gen.py:
// event type 0xe46e6156, name + joint; their sounds are in wpn-melee-switchblade / wpn-melee-unarmed / gbl-fight, all in the MP level adds)
// fire through play-sound-at-joint on the half's body at their clip frame on the chain clock; our hand-mixed hit / heavy / whoosh / impact
// cues are then off for the move (grunts / pain stay). t2r-cd-pcm-cues.txt next to the exe = the old PCM cues instead.
#include "ctx_sfx_events.h"
static const struct {
    const char *pair;
    int seg, role, ms;
    uint64_t name, joint;
} CTX_SFX[] = { CTX_SFX_DATA { NULL, 0, 0, 0, 0, 0 } };

// (the hammer wall uppercut: the finisher audio and hit times were wrong): the weapon takedowns ("wpn-" rows) have no CD_HITS entry, so their blows sat at 35 % / 70 % of the finisher - the uppercut's ONE blow (33 ms into
// its main clip) got two generic pipe PCM hits 450 / 930 ms late. SP plays these with the melee system: the attacker clip's weapon-effect whoosh, the victim
// clip's melee-effect (hit / finisher at a joint), wall-impact-slam and bodyfall-torso. ctx_hit_events.h (tools/ctxhit_gen.py) has their clip frames;
// cd_sfx_arm / cd_cam_kicks put the cues and kicks there, and a blunt row plays the held weapon's SP whoosh / hit / finisher events (mws_native).
// t2r-wpn-hit-old.txt = the old fractions and pipe waves.
#include "ctx_hit_events.h"
static const struct {
    const char *pair;
    int seg, role, ms, kind, tier, zone;
} CTX_HIT[] = { CTX_HIT_DATA { NULL, 0, 0, 0, 0, 0, 0 } };

static struct {
    int on, blunt, zh, zf;
} g_cdw;
static int CdwOld(void)
{
    static int v = -1;
    static uint64_t at;
    uint64_t n = GetTickCount64();
    if (v < 0 || n - at > 2000) {
        at = n;
        v = GetFileAttributesA("t2r-wpn-hit-old.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    return v;
}

// The knife takedowns' blood stream must match TLOU2 SP exactly:
// the clips' own blood part-effect events (type 0xc0d9313c7f55b23e, tools/ctxfx_gen.py: EVERY param of the event verbatim - group, joint,
// offset, the per-effect knobs, blood-mask UVs, world splatters) fire through the engine's own event path on the half's body at their clip
// frame on the chain clock (clipfx.c clipfx_fire). t2r-cd-pcm-cues.txt (the old cues) turns them off as well; t2r-clipfx-off.txt = the old
// spawn-particles-at-joint spawns of the joint-bound ones (default knobs).
#include "ctx_fx_events.h"
static const struct {
    const char *pair;
    int seg, role, ms;
    float frame;
    uint64_t name;
    const char *pn, *jn;
    int n;
    const uint64_t(*pp)[2];
} CTX_FX[] = {
    CTX_FX_DATA { NULL, 0, 0, 0, 0.0f, 0, NULL, NULL, 0, NULL }
};

static CfxeEntry g_ctxFxEnt[CTX_FX_N];       // persistent event entries (FireEffect keeps the pointer in its trackers)
static CfxeEntry g_ctxFxSelf[CTX_FX_N];      // the same without `actor player`, for halves on a puppet (clipfx.c cfxe_self_variant)
// knife melee HITS (SP: the victim's -hit clip cuts the body at its frame 1 - bm-knife-wound-cloth, dcl-dynamic-blood-melee; the SP sandbox's
// Ellie melee fight): fired on the cut body when a female knife move lands (knife_hit_native). On the attacker's screen the move is ours
// (g_fem_last_move, exact); on the victim's screen (our own body) the attacker's move is not known: the light combo's slashes in turn.
static const struct {
    uint64_t move;
    const char *mn, *clip;
    int ms;
    float frame;
    uint64_t name;
    const char *pn, *jn;
    int n;
    const uint64_t(*pp)[2];
} KNIFE_HIT_FX[] = {
    KNIFE_HIT_FX_DATA { 0, NULL, NULL, 0, 0.0f, 0, NULL, NULL, 0, NULL }
};

static CfxeEntry g_knifeHitEnt[KNIFE_HIT_FX_N];
static int g_khfxBlunt;        // melee_hit_fx: a blunt class hit - the cut rows (bm-knife-wound-cloth wound mask, ribbon / spray) are skipped
static void KnifeHitFx(uint64_t ent, uint64_t move)
{
    static unsigned rr;
    static int nl;
    if (ClipfxOff() || !CfxeCheck())
        return;
    uint8_t *obj = CfxeObjOf(ent);
    if (!obj)
        return;
    if (!move) {
        static const char *const RR[3] =
            { "switchblade-light-attack-1", "switchblade-light-attack-2",
"switchblade-light-attack-3" };
        move = FNV64(RR[rr++ % 3]);
    }
    int own = obj == BpMe(), n = 0;
    for (size_t k = 0; k < KNIFE_HIT_FX_N; k++) {
        if (KNIFE_HIT_FX[k].move != move)
            continue;
        if (g_khfxBlunt && KNIFE_HIT_FX[k].pn
            && (strstr(KNIFE_HIT_FX[k].pn, "wound")
                || strstr(KNIFE_HIT_FX[k].pn, "ribbon")
                || strstr(KNIFE_HIT_FX[k].pn, "spray"))) {
            if (LOGCAP(nl, 40))
                Xlog("knife hit fx: %s %s skipped - a blunt hit gets no cut / spray (SP blunt -hit clips 1-2: dust only)", KNIFE_HIT_FX[k].mn, KNIFE_HIT_FX[k].pn);
            n++;
            continue;
        }
        if (!g_knifeHitEnt[k].type) {
            g_knifeHitEnt[k].frame = KNIFE_HIT_FX[k].frame;
            g_knifeHitEnt[k].n = (uint32_t) KNIFE_HIT_FX[k].n;
            g_knifeHitEnt[k].pairs = KNIFE_HIT_FX[k].pp;
            g_knifeHitEnt[k].type = CFXE_TYPE;
        }
        uint64_t r =
            ClipfxFire(obj, FNV64(KNIFE_HIT_FX[k].clip),
                        &g_knifeHitEnt[k], own);
        n++;
        if (LOGCAP(nl, 40))
            Xlog("knife hit fx: %s %s at %s on %s (%s) -> engine event path %llx", KNIFE_HIT_FX[k].mn, KNIFE_HIT_FX[k].pn, KNIFE_HIT_FX[k].jn, own ? "our own body" : "the cut puppet", KNIFE_HIT_FX[k].clip, (unsigned long long) r);
    }
    if (!n && nl++ < 40)
        Xlog("knife hit fx: move %016llx has no SP -hit blood events (SP shows none for it)", (unsigned long long) move);
}

// Blade / blunt MELEE HITS (SP blood on every blade / blunt hit): the weapon's class picks a rotation of SP victim -hit clips that carry blood events
// (ctxfx_gen.py MELEE_HITS: machete slash 1-3 npc-m, 1h blade sprint / dodge-back / knee-hack / wall slashes / handle bash, 1h blunt wall strikes + short-pipe sprint,
// 2h blunt open-combo 3-4 / dodge-back / sprint / wall strikes). Fired on the body that was hit: on the attacker's screen the cut puppet (hitsend / directhit hooks), on
// the victim's screen our own body (recv hit hook) - the same call as the knife's, so every hit shows on both screens. t2r-no-melee-blood.txt = off.
static int FxOffExtra(void)
{
    static int off = -1;
    static uint64_t at;
    if (off < 0 || g_frame - at >= 120) {
        at = g_frame ? g_frame : 1;
        off =
            GetFileAttributesA("t2r-no-melee-gore.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    return off;
}

static void MeleeWeaponHitSfx(uint64_t ent, uint64_t weapon,
                                 int victim);
static void MeleeHitFx(uint64_t ent, uint64_t weapon)
{
    static unsigned rr[4];
    static uint64_t last;
    static int nl, c_off = -1;
    static uint64_t k_off;
    const char *nm = NULL;
    if (!ent
        || (MeleeClassOf(weapon, &nm) < 0
            && MeleeClassOf(BaseWeapon(weapon), &nm) < 0) || !nm
        || g_t60 - last < 6)
        return;                 // (v5-sfx: an upgraded id too)
    if (CfxeFile("t2r-no-melee-blood.txt", &c_off, &k_off))
        return;
    last = g_t60;
    int cls = !strcmp(nm, "machete") || !strcmp(nm, "machete-scar") ? 0 :
        !strcmp(nm, "tomahawk") || !strcmp(nm, "hatchet-modern")
        || !strcmp(nm, "hatchet")
        || !strcmp(nm, "fireman-axe") ? 1 : !strcmp(nm, "two-by-four")
        || !strcmp(nm, "baseball-bat") ? 3 : 2;
    static const char *const PFX[4] =
        { "mhit-machete-", "mhit-blade-", "mhit-blunt1-", "mhit-blunt2-" };
    static const int N[4] = { 3, 6, 4, 6 };
    char key[40];
    snprintf(key, sizeof key, "%s%u", PFX[cls],
             rr[cls]++ % (unsigned) N[cls] + 1);
    if (LOGCAP(nl, 40))
        Xlog("melee hit fx: %s (%s) on %s -> %s", nm, PFX[cls],
             ent == SID_PLAYER ? "our own body" : "the hit puppet", key);
    MeleeWeaponHitSfx(ent, weapon, ent == SID_PLAYER);
    // Blood effects did not fit blunt weapons (slash blood spawned on hits): the 2h / 1h blunt rotation stamped
    // the bat combo-3 / dodge-back clips' bm-knife-wound-cloth CUT mask and blood sprays on every blunt hit (combo-1/2 in SP carry only dust-melee-eff), and a duel
    // pair already plays its own -hit clip's SP events through the engine. Blunt hits keep the impact gore (gore-minimal, teeth burst, drops), no cut / spray.
    // t2r-blunt-cut-blood.txt = the old rows.
    g_khfx_blunt = cls >= 2
        && GetFileAttributesA("t2r-blunt-cut-blood.txt") ==
        INVALID_FILE_ATTRIBUTES;
    KnifeHitFx(ent, FNV64(key));
    g_khfxBlunt = 0;
    // The machete had no special effects: SP's machete / 1h blade -hit clips carry only a blood MASK (bm-knife-wound-cloth, no joint),
    // which is barely visible, while the bat's clips also fire gore particles. Blades get the same gore burst + blood drops on the chest, 1h blunt a drops burst.
    if (cls <= 1 && !FxOffExtra()) {
        ArmorFxSpawn(FNV64("melee-blood-gore-minimal-eff"), ent,
                       "spined");
        ArmorFxSpawn(FNV64("blood-drops-burst-eff"), ent, "spined");
    } else if (cls == 2 && !FxOffExtra())
        ArmorFxSpawn(FNV64("blood-drops-burst-eff"), ent, "spined");
}

// The bat had no bat hit sound effects, only the fists: a melee weapon's contact on a player played winmm's PlayLocalHitFeedback list
// (sfx-unarmed-hit-*), the fists' punch, whatever was swung. Now (melee_hit_sfx_swap silences that list while a melee weapon is held) each weapon class plays
// TLOU2's own waves through the PCM mixer on the hit body, on the attacker's screen (puppet we hit) and the victim's (our body; blades there already get the
// slash cue from hitfx): wood layers for the bat / 2x4 / wooden clubs, the pipe set for pipes / crowbar / hammers, the blade cues for machete / axes.
// t2r-no-melee-weapon-sfx.txt = off (and winmm's fist list back).
static int MwsOff(void)
{
    static int off = -1;
    static uint64_t at;
    if (off < 0 || g_frame - at >= 120) {
        at = g_frame ? g_frame : 1;
        off =
            GetFileAttributesA("t2r-no-melee-weapon-sfx.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    return off;
}

static int MeleeWeaponSfxWanted(uint64_t w)
{
    const char *n = NULL;
    return w && w != ~0ULL && !MwsOff()
        && (MeleeClassOf(BaseWeapon(w), &n) >= 0
            || MeleeClassOf(w, &n) >= 0) && !KnifeWeaponId(w);
}

// v5-sfx: every duel / plain / pin / counter
// blow of a melee weapon above played the PCM mixer's waves - the 2013-style lead-pipe torso set (EXS_HIT_PIPE_TORSO) for the hammer, crowbar, wrench and short pipe, the bat's wood layers for the clubs, the machete cue for the axes.
// SP plays the weapon's OWN event at the victim on the impact frame (re/melee-duel-sp-spec.md audio table: the victim -hit clip's melee-effect, +32..+45 ms):
// sfx-<weapon>-<tier>-<zone> from the weapon's bank (sp-common soundbank4/wpn-melee-<weapon>.bnk, every one of them in the MP level adds,
// dc_data_patches SP_MELEE_BANKS): tier hit / breaker (the combo's last blow, SP round2 hammer F25588) / finisher (the kill), zone gut at the pelvis (SP's light
// swings: sfx-hammer-hit-gut, sfx-club-1h-hit-gut, sfx-club-2h-hit-gut, sfx-hatchet-hit-gut), face-skin at the head (dodge / counter attacks: sfx-club-1h-hit-face-skin),
// back at the spine (from behind: sfx-club-1h-hit-back, sfx-machete-hit-back). Every name below is in its bank's event list (review-1029/sfx/bnk/names.txt).
// The PCM waves stay the fallback when the engine refuses the event (~0). t2r-mws-pcm.txt = the old PCM waves only.
enum { MWS_HIT, MWS_BREAKER, MWS_FINISHER };
enum { MWS_GUT, MWS_FACE, MWS_BACK };
static int g_mwsTier = MWS_HIT, g_mws_zone = MWS_GUT;  // the caller's hint for the next weapon hit sound (reset after each)
static uint64_t g_mwsNativeT60;       // the frame an SP weapon event last played on OUR body (melee_hitfx.c: no blade slash cue on top)
static const char *MwsPrefix(const char *nm)
{
    static const struct {
        const char *item, *p;
    } T[] = {
        { "hammer-modern", "hammer" }, { "hammer-scar", "hammer" },
            { "pipe-wrench", "pipe-wrench" }, { "crowbar",
                                               "crowbar-one-hand" },
            { "short-pipe", "lead-pipe" },
        { "lead-pipe", "lead-pipe" }, { "club-1h-police",
                                       "club-1h-police" },
            { "club-1h-scar", "club-1h" }, { "club-2h-scar", "club-2h" },
            { "sledgehammer", "club-2h" },
        { "baseball-bat", "baseball-bat" }, { "two-by-four", "2x4" },
            { "tomahawk", "hatchet" }, { "hatchet-modern", "hatchet" },
            { "hatchet", "hatchet" },
        { "fireman-axe", "two-hand-axe" }, { "machete", "machete" },
            { "machete-scar", "machete" }, { "pickaxe", "pickaxe" }
    };
    for (size_t i = 0; nm && i < sizeof T / sizeof *T; i++)
        if (!strcmp(T[i].item, nm))
            return T[i].p;
    return NULL;
}

static int MwsPcmOnly(void)
{
    static int v = -1;
    static uint64_t at;
    if (v < 0 || g_frame - at >= 120) {
        at = g_frame ? g_frame : 1;
        v = GetFileAttributesA("t2r-mws-pcm.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    return v;
}

// the SP event name of a weapon blow; joint out; NULL = no SP event for it
static const char *MwsEvent(const char *nm, int tier, int zone, char *buf,
                             size_t bn, const char **joint)
{
    const char *p = MwsPrefix(nm);
    if (!p)
        return NULL;
    const char *z =
        zone == MWS_FACE ? "face-skin" : zone == MWS_BACK ? "back" : "gut";
    *joint =
        zone == MWS_FACE ? "heada" : zone ==
        MWS_BACK ? "spinec" : "pelvis";
    if (tier == MWS_BREAKER && !strcmp(p, "two-hand-axe"))
        tier = MWS_HIT;         // (the axe bank has no breaker set)
    if (tier == MWS_BREAKER && !strcmp(p, "pickaxe") && zone == MWS_FACE)
        z = "gut";              // (pickaxe: breaker-gut / -back only)
    if (tier == MWS_FINISHER) { // SP's own kill sounds per weapon (re/sp-sandbox-runs)
        if (!strcmp(p, "machete")) {
            z = "head-hair";
            *joint = "heada";
        }                       // (sfx-machete-finisher-head-hair, 12x in the SP runs)
        else if (!strcmp(p, "hatchet")) {
            z = "slice-gut";
            *joint = "pelvis";
        }                       // (sfx-hatchet-finisher-slice-gut)
        else if (!strcmp(p, "two-hand-axe")) {
            z = "gut";
            *joint = "pelvis";
        }                       // (sfx-two-hand-axe-finisher-gut)
        else {
            z = "face-skin";
            *joint = "heada";
        }                       // (sfx-hammer- / -baseball-bat- / -club-2h-finisher-face-skin)
    }
    snprintf(buf, bn, "sfx-%s-%s-%s", p,
             tier == MWS_FINISHER ? "finisher" : tier ==
             MWS_BREAKER ? "breaker" : "hit", z);
    return buf;
}

// 1 = the SP event played on ent
static int MwsNative(uint64_t ent, const char *nm, int tier, int zone,
                      const char *why)
{
    static int nl;
    if (!ent || ent == ~0ULL || MwsPcmOnly())
        return 0;
    char ev[64];
    const char *j = "pelvis";
    if (!MwsEvent(nm, tier, zone, ev, sizeof ev, &j))
        return 0;
    uint64_t a[5] = { Fnv64Rt(ev), ent, Fnv64Rt(j), 0, 0 };
    uint64_t r = DcCall(FNV64("play-sound-at-joint"), 5, a);
    if (LOGCAP(nl, 80))
        Xlog("melee weapon sfx: SP %s at %s on %s (%s, %s) -> %llx%s", ev,
             j, ent == SID_PLAYER ? "our body" : "the hit body", nm,
             why ? why : "hit", (unsigned long long) r,
             r == ~0ULL ? " (refused - PCM fallback)" : "");
    if (r == ~0ULL)
        return 0;
    if (ent == SID_PLAYER)
        g_mwsNativeT60 = g_t60 ? g_t60 : 1;
    return 1;
}

static void MeleeWeaponHitSfx(uint64_t ent, uint64_t weapon, int victim)
{
    static uint64_t last;
    static int nl;
    const char *nm = NULL;
    int tier = g_mwsTier, zone = g_mws_zone;
    g_mwsTier = MWS_HIT;
    g_mws_zone = MWS_GUT;
    if (!ent || MwsOff() || g_t60 - last < 6)
        return;
    if (!
        (MeleeClassOf(BaseWeapon(weapon), &nm) >= 0
         || MeleeClassOf(weapon, &nm) >= 0) || !nm
        || KnifeWeaponId(weapon))
        return;
    last = g_t60;
    if (MwsNative
        (ent, nm, tier, zone,
         tier == MWS_FINISHER ? "kill blow" : tier ==
         MWS_BREAKER ? "combo breaker" : "blow"))
        return;                 // (both screens: the blade victim too)
    int blade = !strcmp(nm, "machete") || !strcmp(nm, "machete-scar")
        || !strcmp(nm, "tomahawk") || !strcmp(nm, "hatchet-modern")
        || !strcmp(nm, "hatchet") || !strcmp(nm, "fireman-axe");
    int wood = !strcmp(nm, "baseball-bat") || !strcmp(nm, "two-by-four")
        || !strcmp(nm, "club-2h-scar") || !strcmp(nm, "club-1h-scar")
        || !strcmp(nm, "club-1h-police");
    int ok = 0;
    if (blade) {
        if (victim)
            return;
        ok = BladePcmOn()
            && BladeCuePlay(BC_M_TORSO, 0.9f, ent, NULL, "weapon hit");
    } else if (wood) {
        unsigned w = 0;
        ok = WoodCue(!strcmp(nm, "baseball-bat") ? T2M_WOOD_TORSO :
                      T2M_WOOD_HIT, 1.1f, ent, NULL, &w, !strcmp(nm,
                                                                 "baseball-bat")
                      ? 2 : 1);
    } else
        ok = ExT2mLabelWave(EXS_HIT_PIPE_TORSO, 1.0f, ent, "weapon hit",
                               nm);
    if (LOGCAP(nl, 60))
        Xlog("melee weapon sfx: %s on %s (%s) -> %d", nm,
             victim ? "our body" : "the hit puppet",
             blade ? "blade cue" : wood ? "wood layers" : "pipe set", ok);
}

#include "ctx_vox.h"
static int CdSfxLabel(int style, int act)
{
    int fist = style == -1, knife = style == 3, blade = style == 1
        || style == 4;
    switch (act) {
    case CE_GRAB:
        return EXS_GRAB_TORSO;
    case CE_HIT:
        return fist ? EXS_HIT_FIST_FACE : knife ? EXS_SHIV_STAB : blade ?
            EXS_HIT_MACHETE_TORSO_FINISHER : EXS_HIT_PIPE_TORSO;
    case CE_HEAVY:
        return fist ? EXS_HIT_FIST_FACE_FINISHER : knife ? EXS_SHIV_GORE :
            blade ? EXS_HIT_MACHETE_HEAD_FINISHER :
            EXS_HIT_PIPE_HEAD_FINISHER;
    case CE_THUD:
        return EXS_IMPACT_HEAD;
    case CE_FALL:
        return EXS_IMPACT_BODY;
    case CE_KICK:
        return EXS_GUT_KICK;
    case CE_WHOOSH:
        return fist ? -1 : knife ? EXS_MACHETE_WHOOSH : blade ?
            EXS_MACHETE_WHOOSH : EXS_PIPE_WHOOSH;
    }
    return -1;
}

// the blow of a duel finisher: the weapon's own SP finisher sound event on the victim's head (banks wpn-melee-* / gbl-fight are in the MP level adds)
// v5-sfx: a hammer finisher
// played the FIST finisher: g_ps.weapon was 0 - the duel read dc_weapon() at the windup's contact while the SP intro ran on us (it answered abby-unarmed; also the
// hatchet case). The weapon now comes from the pair's style + the duel swing's own weapon (melee_duel.c mdu_blow_weapon), and on the victim's
// screen from the attacker's net record when the START carried none. The event = the weapon's own SP kill sound (mws_event: hammer / club / bat / pipe ->
// sfx-<w>-finisher-face-skin, machete -head-hair, hatchet -slice-gut, axe -gut; the old table gave the wrench / crowbar / short pipe the hammer's and the 2x4 the
// bat's), plus SP's weapon kill layers: the swung weapon's whoosh is cd_mf_whoosh. 1 = the SP weapon event played (the PCM heavy cue then stays off).
// v5-disfix (a disarm sounded like a punch hit instead of the heavy crowbar SP disarm sound, probably all disarms; the log read 'mf-fist-1h-blunt-disarm-left-back-hand-finisher
// - 1 native TLOU2 sound event(s) of the clips armed, our hit / impact cues off'): a disarm row is a FIST style row (the attacker starts empty-handed), so the whole finisher sound
// set took the fist branch - cd_mf_weapon answered the fists, cd_mf_impact played sfx-unarmed-finisher-face-skin and the PCM fist finisher punch played on top, and the clips' one
// event (967 ms) was all that was left of SP's disarm audio (SP plays the rest from the melee move data: re/melee-duel-sp-spec.md 'Disarms seen in the clip'). A disarm row now
// plays SP's sequence on the chain clock: the grab effect (sfx-grab-torso-leather + sfx-grab-arms-leather at +30 ms of the -sync pair) on the victim, the row's first blow where
// SP's trace has it (arm-break: sfx-unarmed-disarm-elbow-break +430; shoulder-check: sfx-unarmed-hit-check-torso-cotton +32; left-elbow: sfx-unarmed-finisher-elbow-face-skin +32), the
// TAKEN weapon's own grab (sfx-weapon-<class>-grab, ~250 ms before the take frame), its whoosh, and its SP kill blow (sfx-<weapon>-finisher-face-skin via cd_mf_impact; the weapon
// is cd_dis's, the victim's weapon being taken) instead of the fist punch. t2r-no-disarm-sp-sfx.txt = the old set.
static int CdDisSfxOff(void)
{
    static int v = -1;
    static uint64_t at;
    uint64_t n = GetTickCount64();
    if (v < 0 || n - at > 2000) {
        at = n;
        int o =
            GetFileAttributesA("t2r-no-disarm-sp-sfx.txt") !=
            INVALID_FILE_ATTRIBUTES;
        if (o != v)
            Xlog("ctxdown: disarm SP sounds %s",
                 o ?
                 "OFF (t2r-no-disarm-sp-sfx.txt): the fist finisher set" :
                 "on: grab / first blow / taken weapon's grab + whoosh + kill blow (SP's melee move sounds)");
        v = o;
    }
    return v;
}

static int CdMfDisRow(int pair)
{
    return pair >= 0 && CdMfRow(pair)
        && strstr(CD_PAIRS[pair].name, "disarm") && CdMfDmask(pair)
        && !CdDisSfxOff();
}
enum { CE_DGRAB = 32, CE_DBRK, CE_DWGR };       // v5-disfix: acts of a disarm row's own sound list (handled by cd_dis_sfx_act, not explus.c)
static const char *MwsPrefix(const char *nm);
static void CdDisSnd(const char *ev, uint64_t ent, const char *joint,
                       const char *why)
{
    static int nl;
    if (!ent || ent == ~0ULL)
        return;
    uint64_t a[5] = { Fnv64Rt(ev), ent, Fnv64Rt(joint), 0, 0 };
    uint64_t r = DcCall(FNV64("play-sound-at-joint"), 5, a);
    if (LOGCAP(nl, 80))
        Xlog("ctxdown: disarm sfx %s at %s on the %s (%s) -> %llx%s", ev,
             joint, ent == SID_PLAYER ? "our body" : "the other body", why,
             (unsigned long long) r, r == ~0ULL ? " (refused)" : "");
}

static uint64_t CdMfWeapon(void);
static void CdDisSfxAct(int act, uint64_t att, uint64_t vic)
{
    const char *nm = CD_PAIRS[g_cdSfx.pair].name;
    if (act == CE_DGRAB) {
        CdDisSnd("sfx-grab-torso-leather", vic, "l_shoulder",
                   "SP grab-effect");
        CdDisSnd("sfx-grab-arms-leather", vic, "l_shoulder",
                   "SP grab-effect");
    } else if (act == CE_DBRK) {
        if (strstr(nm, "arm-break"))
            CdDisSnd("sfx-unarmed-disarm-elbow-break", vic, "spinec",
                       "first blow");
        else if (strstr(nm, "shoulder-check"))
            CdDisSnd("sfx-unarmed-hit-check-torso-cotton", vic, "spinec",
                       "first blow");
        else if (strstr(nm, "left-elbow"))
            CdDisSnd("sfx-unarmed-finisher-elbow-face-skin", vic,
                       "heada", "first blow");
        // v5-dissfx2 (the iron-rhino disarm was missing its sounds): the other disarm rows had no first blow at all - their clips carry no sound
        // events (SP plays the melee system's hit sounds): the unarmed face / gut hit of SP's fist sets by the row's first contact
        else if (strstr(nm, "iron-rhino") || strstr(nm, "punch-n-hack")
                 || strstr(nm, "back-hand") || strstr(nm, "grab-n-bash"))
            CdDisSnd("sfx-unarmed-hit-face-skin", vic, "heada",
                       "first blow");
        else if (strstr(nm, "kick") || strstr(nm, "grab-n-ram"))
            CdDisSnd("sfx-unarmed-hit-gut", vic, "spinea", "first blow");
    } else if (act == CE_DWGR) {
        uint64_t w = CdMfWeapon();
        const char *wn = NULL;
        const char *p;
        if (w && w != ~0ULL
            && (MeleeClassOf(BaseWeapon(w), &wn) >= 0
                || MeleeClassOf(w, &wn) >= 0) && wn
            && (p = MwsPrefix(wn))) {
            char ev[64];
            snprintf(ev, sizeof ev, "sfx-weapon-%s-grab",
                     !strcmp(p, "2x4") ? "baseball-bat" : p);
            CdDisSnd(ev, vic, "r_wrist", "the taken weapon's grab");
        }
    }
    (void) att;
}

static uint64_t CdMfWeapon(void)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    uint64_t w = g_ps.weapon;
    int st = g_cdSfx.uid == g_ps.uid ? CD_PAIRS[g_cdSfx.pair].style : 0;
    if (g_cdSfx.uid == g_ps.uid && CdMfDisRow(g_cdSfx.pair)
        && g_cdDis.uid == g_ps.uid && g_cdDis.w && g_cdDis.w != ~0ULL
        && CdDisClass(g_cdDis.w, NULL))
        return g_cdDis.w;      // v5-disfix: a disarm's blows are the TAKEN weapon's
    if (st == -1)
        return w;               // (a fist row: the fists)
    if ((!w || w == ~0ULL || w == FNV64("abby-unarmed")) && g_ps.role == 2
        && g_ps.att >= 0 && g_ps.att < T2R_MAX_PEERS) {
        uint8_t *pa = ((fn_peer) T2rPeer) ((uint32_t) g_ps.att);
        uint64_t pw = pa
            && !IsBadReadPtr(pa + 0x91, 8) ? *(uint64_t *) (pa + 0x91) : 0;
        if (pw && pw != ~0ULL
            && MeleeClassOf(BaseWeapon(pw), NULL) >= 0)
            w = pw;
    }
    if ((!w || w == ~0ULL || w == FNV64("abby-unarmed")) && g_ps.role == 1) {
        uint64_t cw = CurrentWeapon(), x = cw && cw != ~0ULL
            && MeleeClassOf(BaseWeapon(cw),
                              NULL) >= 0 ? cw : XrMeleeWeapon(cw);
        if (x && MeleeClassOf(BaseWeapon(x), NULL) >= 0)
            w = x;
    }
    return w;
}

static int CdMfImpact(uint64_t vic)
{
    static int nl;
    if (!vic)
        return 0;
    uint64_t w = CdMfWeapon();
    const char *nm = NULL;
    if (w && !KnifeWeaponId(w)
        && (MeleeClassOf(BaseWeapon(w), &nm) >= 0
            || MeleeClassOf(w, &nm) >= 0) && nm) {
        int ok =
            MwsNative(vic, nm, MWS_FINISHER, MWS_FACE,
                       "duel finisher blow");
        if (LOGCAP(nl, 40))
            Xlog("ctxdown: duel finisher blow - SP weapon kill sound (weapon %s, START weapon %llx) -> %d", nm, (unsigned long long) g_ps.weapon, ok);
        if (ok) {               // SP's killing blunt blow: the victim's "bleeh" (vox-eff-gen-death-melee-blunt-large) when the clips carry no voice of their own
            if (!CdVoxHas()
                && MeleeClassOf(BaseWeapon(w), NULL) != MC_1HBLADE
                && MeleeClassOf(BaseWeapon(w), NULL) != MC_MACHETE)
                DmSay(vic, "vox-eff-gen-death-melee-blunt-large",
                       VXE_DEATH);
            return 1;
        }
        return 0;
    }
    if (w && w != FNV64("abby-unarmed"))
        return 0;               // (a knife / an unknown item: the clips' own events + our heavy cue)
    uint64_t a[5] =
        { FNV64("sfx-unarmed-finisher-face-skin"), vic, FNV64("headb"), 0,
0 };
    uint64_t r = DcCall(FNV64("play-sound-at-joint"), 5, a);
    if (LOGCAP(nl, 40))
        Xlog("ctxdown: duel finisher blow sfx-unarmed-finisher-face-skin on the victim's head (fists) -> %llx", (unsigned long long) r);
    return 0;
}

// the swung weapon's own SP whoosh (sfx-weapon-<w>-whoosh, melee_dodge.c's table) + SP's swing foley (sfx-gear-swish-cotton) at the attacker's wrist before a
// weapon finisher's blow: SP's audio table (re/melee-duel-sp-spec.md: -170..-44 ms, "weapons add sfx-weapon-{club-1h, machete, club-2h, hammer}-whoosh"). The
// clips' own events never carried it, and the native-clip rule muted our CE_WHOOSH on every finisher with a clip event (mf-1h-blunt-*: the palm grab at 300 ms).
static void CdMfWhoosh(uint64_t att)
{
    static int nl;
    uint64_t w = CdMfWeapon();
    const char *nm = NULL, *p;
    if (!att || !w || w == FNV64("abby-unarmed") || KnifeWeaponId(w)
        || !(MeleeClassOf(BaseWeapon(w), &nm) >= 0
             || MeleeClassOf(w, &nm) >= 0) || !(p = MwsPrefix(nm)))
        return;
    char ev[64];
    snprintf(ev, sizeof ev, "sfx-weapon-%s-whoosh", p); // (= melee_dodge.c's whoosh table: sfx-weapon-hammer-whoosh, -crowbar-one-hand-, -club-1h-, ...)
    uint64_t a[5] = { Fnv64Rt(ev), att, FNV64("r_wrist"), 0, 0 }, b[5] =
        { FNV64("sfx-gear-swish-cotton"), att, FNV64("r_wrist"), 0, 0 };
    uint64_t r = DcCall(FNV64("play-sound-at-joint"), 5, a), r2 =
        DcCall(FNV64("play-sound-at-joint"), 5, b);
    if (LOGCAP(nl, 40))
        Xlog("ctxdown: duel finisher swing - SP %s + sfx-gear-swish-cotton at the attacker's wrist -> %llx / %llx", ev, (unsigned long long) r, (unsigned long long) r2);
}

// v5-knife (a pipe execution: the body hit sounded like a bat hit and then the head hit like a proper pipe hit;
// A [19502] / B [12672] "execution sfx: pipe-execute-left: hit-pipe-torso -> TLOU2 wave 92" = lead-pipe-hit-rattle + hammer-hit-gut-break-rib, a thin 3-8 kHz rattle
// over the 2013 recording's dull torso thud): the 2013 weapon executions (pipe-execute / -left, machete-decapitate / -execute-right, the pipe-mod finishers) play
// every blow as the HELD weapon's own SP event at the victim, the same events its duel blows use (mws_event; review-1029/sfx/bnk/names.txt): torso blow
// sfx-<w>-hit-gut (pelvis), a head blow that does not kill sfx-<w>-hit-face-skin, the kill blow the weapon's finisher (mws_event: -finisher-face-skin, machete
// -head-hair, hatchet -slice-gut, axe -gut; a blade's torso kill -finisher-gut, the machete's head chop -finisher-head-chop), a blade pulled out
// sfx-weapon-<w>-extract. The whooshes, body falls and foley stay. Fallback: the PCM waves below (an event refused, an unknown weapon). Two blows inside 150 ms
// (the pipe-mod rows fire a pipe and a machete finisher 30 ms apart) are one SP event. t2r-exec-wpn-pcm.txt = the old waves.
static int ExwOff(void)
{
    static int v = -1;
    static uint64_t at;
    if (v < 0 || g_frame - at >= 120) {
        at = g_frame ? g_frame : 1;
        v = GetFileAttributesA("t2r-exec-wpn-pcm.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    return v;
}

static const char *ExwName(uint64_t w)
{
    const char *nm = NULL;
    if (!w || w == ~0ULL || KnifeWeaponId(w)
        || w == FNV64("abby-unarmed"))
        return NULL;
    if (!
        (MeleeClassOf(BaseWeapon(w), &nm) >= 0
         || MeleeClassOf(w, &nm) >= 0) || !nm || !MwsPrefix(nm))
        return NULL;
    return nm;
}

static int ExwWants(uint64_t w)
{
    return !ExwOff() && !MwsOff() && !MwsPcmOnly()
        && ExwName(w) != NULL;
}

static int ExwContact(int label, const char *pair)
{
    static int nl;
    static uint64_t last_ms;
    if (label != EXS_HIT_PIPE_TORSO && label != EXS_HIT_PIPE_HEAD_FINISHER
        && label != EXS_HIT_MACHETE_TORSO_FINISHER
        && label != EXS_HIT_MACHETE_HEAD_FINISHER
        && label != EXS_HIT_MACHETE_TORSO_EXTRACT)
        return 0;
    if (!ExwWants(g_exWpn) || !g_ex.vic_ent)
        return 0;
    const char *nm = ExwName(g_exWpn), *p = MwsPrefix(nm), *j =
        "pelvis";
    uint64_t now = GetTickCount64();
    if (label != EXS_HIT_MACHETE_TORSO_EXTRACT && last_ms
        && now - last_ms < 150)
        return 1;               // (the 2nd label of one blow)
    double ems = PsyncMs() - g_ex.t0;
    int kill = 1;               // the kill blow = the timeline's last head / torso blow (pipe-execute: 1500 ms hits, 2633 ms kills; pipe-execute-left: the 2500 ms overhead kills)
    for (size_t i = 0; i < sizeof EXSFX_MELEE / sizeof EXSFX_MELEE[0]; i++)
        if (EXSFX_MELEE[i].pair == g_ex.pair
            && (double) EXSFX_MELEE[i].ms > ems + 150.0
            && (EXSFX_MELEE[i].label == EXS_HIT_PIPE_HEAD_FINISHER
                || EXSFX_MELEE[i].label == EXS_HIT_PIPE_TORSO
                || EXSFX_MELEE[i].label == EXS_HIT_MACHETE_HEAD_FINISHER
                || EXSFX_MELEE[i].label ==
                EXS_HIT_MACHETE_TORSO_FINISHER)) {
            kill = 0;
            break;
        }
    int blade = !strcmp(p, "machete") || !strcmp(p, "hatchet")
        || !strcmp(p, "two-hand-axe");
    char ev[64];
    const char *e = NULL;
    switch (label) {
    case EXS_HIT_PIPE_TORSO:
        snprintf(ev, sizeof ev, "sfx-%s-hit-gut", p);
        e = ev;
        break;
    case EXS_HIT_PIPE_HEAD_FINISHER:
    case EXS_HIT_MACHETE_HEAD_FINISHER:
        if (!kill) {
            snprintf(ev, sizeof ev, "sfx-%s-hit-face-skin", p);
            e = ev;
            j = "heada";
        } else if (label == EXS_HIT_MACHETE_HEAD_FINISHER
                   && !strcmp(p, "machete")) {
            snprintf(ev, sizeof ev, "sfx-machete-finisher-head-chop");
            e = ev;
            j = "heada";
        } else
            e = MwsEvent(nm, MWS_FINISHER, MWS_FACE, ev, sizeof ev, &j);
        break;
    case EXS_HIT_MACHETE_TORSO_FINISHER:
        snprintf(ev, sizeof ev, "sfx-%s-finisher-gut", p);
        e = ev;
        break;
    case EXS_HIT_MACHETE_TORSO_EXTRACT:
        if (!blade && strcmp(p, "pickaxe"))
            return 0;
        snprintf(ev, sizeof ev, "sfx-weapon-%s-extract", p);
        e = ev;
        break;
    }
    if (!e)
        return 0;
    uint64_t a[5] = { Fnv64Rt(e), g_ex.vic_ent, Fnv64Rt(j), 0, 0 };
    uint64_t r = DcCall(FNV64("play-sound-at-joint"), 5, a);
    int ok = r && r != ~0ULL;
    if (LOGCAP(nl, 120))
        Xlog("execution sfx: %s +%.0f ms: %s -> SP %s at %s on the victim (%s, %s) -> %llx%s", pair ? pair : "?", ems, EXSFX_NAME[label], e, j, nm, kill ? "kill blow" : "blow", (unsigned long long) r, ok ? "" : " (refused - the PCM wave plays)");
    if (!ok)
        return 0;
    if (label != EXS_HIT_MACHETE_TORSO_EXTRACT)
        last_ms = now;
    if (label == EXS_HIT_MACHETE_HEAD_FINISHER)
        ThroatBloodStart(g_ex.vic_ent);       // (the neck blood stream the blade cue path starts)
    if (g_ex.vic_ent == SID_PLAYER)
        g_mwsNativeT60 = g_t60 ? g_t60 : 1;
    return 1;
}

// the kill blow of a disarm row. The row's dmax (gen_duel_fin.py) is not a contact time for every row (the knife's 1h-blunt left-overhead has dmax 33 ms = the take tick),
// and with neither a CD_HITS entry nor dmax cd_sfx_arm fell back to 35 % of the move (1121 ms chain = 654 ms into a 1.87 s clip: 150 ms after the weapon change, ~750 ms
// BEFORE the wrench hit the head; the camera's second kick shared the guess). SP puts the kill blow on the victim's death voice event (re/melee-duel-sp-spec.md, 'Disarms seen
// in the clip': +1502 kill blow with vox-...-death-melee-blunt-large-gore, +1004, +1101) and the clips carry it (ctx_vox_events.h, kind 2 on the victim): the LAST one inside the
// chain is the blow (left-overhead: 1400 ms of the main clip, the head blood at 1433). Returns the chain-clock ms, -1 = none. t2r-dis-sfx-old.txt = the old timing.
static int CdDisBlowOld(void)
{
    static int v = -1;
    static uint64_t at;
    uint64_t n = GetTickCount64();
    if (v < 0 || n - at > 2000) {
        at = n;
        v = GetFileAttributesA("t2r-dis-sfx-old.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    return v;
}

static float CdDisBlowMs(int pair, const float *st, int ns)
{
    float best = -1.0f;
    if (CdDisBlowOld())
        return -1.0f;
    for (size_t k = 0; CTX_VOX[k].pair; k++) {
        if (CTX_VOX[k].kind != 2 || CTX_VOX[k].role != 1
            || strcmp(CTX_VOX[k].pair, CD_PAIRS[pair].name))
            continue;
        int sg = CdTabSeg(pair, CTX_VOX[k].seg, CTX_VOX[k].role);
        if (sg >= ns)
            continue;
        float lim =
            sg + 1 < ns ? CdSegLen(pair,
                                     sg) * 1000.0f : CD_PAIRS[pair].end_s *
            1000.0f + 300.0f;
        if ((float) CTX_VOX[k].ms > lim)
            continue;
        float m = st[sg] * 1000.0f + (float) CTX_VOX[k].ms;
        if (m > best)
            best = m;
    }
    return best;
}

static float DcHitMsNow(void);       // dcounter.c: the running counter's contact (clip ms), -1 = none of ours
static float MdpAttFreeMs(int pair); // melee_duel_parry.c (ps28)
static void CdSfxArm(int pair)
{
    memset(&g_cdSfx, 0, sizeof g_cdSfx);
    g_cdSfx.uid = g_ps.uid;
    g_cdSfx.pair = pair;
    // the chain's clock (absolute real ms): segment starts, the quick hit's segment, the finisher
    int ns = CD_PAIRS[pair].nseg;
    float st[CD_MAXSEG + 1];
    st[0] = 0.0f;
    for (int i = 0; i < ns; i++)
        st[i + 1] =
            st[i] + (i + 1 < ns ? CdSegLen(pair, i) : CdFinLen(pair));
    float L0 = CdSegLen(pair, 0) * 1000.0f;
    float sf = st[ns - 1] * 1000.0f, fe = (CD_PAIRS[pair].end_s) * 1000.0f;     // the finisher starts at sf, its hand-off is fe clip-ms later (x1)
    int push = CdHasPush(pair);
    float hit_at = push ? 0.20f * L0 : -1000.0f;        // the push's blow lands in its first frames (the -hit pair starts at the contact); OPEN moves have no push
    uint16_t f1 = 0, f2 = 0, frel = 0, fgrab = 0;       // the finisher's own contacts (measured) or fractions of it
    for (size_t k = 0; CD_HITS[k].name; k++)
        if (CdIsMove(CD_HITS[k].name, pair)) {
            f1 = CD_HITS[k].hit[0];
            f2 = CD_HITS[k].hit[1];
            frel = CD_HITS[k].hit[2];
            fgrab = CD_HITS[k].hit[3];
        }
    float h1 = sf + (f1 ? f1 : 0.35f * fe), h2 =
        sf + (f2 ? f2 : 0.70f * fe);
    int fist = CD_PAIRS[pair].style == -1;
    struct {
        uint16_t ms;
        uint8_t act;
    } t[16];
    int n = 0;
#define EV(m, a) do { float m_ = (m); if (n < 16) { t[n].ms = (uint16_t)(m_ < 0.0f ? 0.0f : m_ > 60000.0f ? 60000.0f : m_); t[n].act = (a); n++; } } while (0)
    memset(&g_cdw, 0, sizeof g_cdw);
    if (!strncmp(CD_PAIRS[pair].name, "wpn-", 4) && !CdwOld()) {
        // the clip frames of the weapon takedown's own swing / blows / wall slam / body fall (CTX_HIT, see above)
        float bl[4];
        int bz[4], nb = 0, slam = 0, fall = 0;
        for (size_t k = 0; CTX_HIT[k].pair; k++) {
            if (strcmp(CTX_HIT[k].pair, CD_PAIRS[pair].name)
                || CdTabSeg(pair, CTX_HIT[k].seg, CTX_HIT[k].role) >= ns)
                continue;
            int sg = CdTabSeg(pair, CTX_HIT[k].seg, CTX_HIT[k].role);
            float lim =
                sg + 1 < ns ? CdSegLen(pair,
                                         sg) *
                1000.0f : CD_PAIRS[pair].end_s * 1000.0f + 300.0f;
            if ((float) CTX_HIT[k].ms > lim)
                continue;
            float m = st[sg] * 1000.0f + (float) CTX_HIT[k].ms;
            if (CTX_HIT[k].kind == 0)
                EV(m - 40.0f, CE_WHOOSH);       // (the swing's sound leads the event frame a touch, like the duel's -44 ms)
            else if (CTX_HIT[k].kind == 1 && nb < 4) {
                bl[nb] = m;
                bz[nb] = CTX_HIT[k].zone;
                nb++;
            } else if (CTX_HIT[k].kind == 2 && !slam++)
                EV(m, CE_THUD);
            else if (CTX_HIT[k].kind == 3 && !fall++)
                EV(m, CE_FALL);
        }
        if (nb) {
            g_cdw.on = 1;
            g_cdw.blunt = strstr(CD_PAIRS[pair].name, "-blunt-")
                || strstr(CD_PAIRS[pair].name, "-hammer-")
                || strstr(CD_PAIRS[pair].name, "-2h-");
            g_cdw.zh = bz[0];
            g_cdw.zf = bz[nb - 1];
            EV(bl[nb - 1] - 180.0f, CE_GRUNT);
            for (int b = 0; b < nb; b++) {
                EV(bl[b], b == nb - 1 ? CE_HEAVY : CE_HIT);
                EV(bl[b] + 40.0f, CE_PAIN);
            }
            EV(bl[nb - 1] + 220.0f, CE_CRY);
            h1 = bl[0];
            h2 = bl[nb - 1];
            Xlog("ctxdown: %s - the clips' own blow frames: %d blow(s) %.0f%s ms, wall slam %s, body fall %s on the chain clock (%s)", CD_PAIRS[pair].name, nb, bl[0], nb > 1 ? " .. last" : "", slam ? "yes" : "no", fall ? "yes" : "no", g_cdw.blunt ? "the held weapon's SP whoosh / hit / finisher" : "the blade cues");
        } else
            n = 0;
    }
    if (g_cdw.on) {
    } else if (CD_PAIRS[pair].kind >= 3) {
        // dodge counter (the haymaker lacked a punch sound): its blow goes out as our own hit message (no engine hit sound), so the cue plays here on
        // both screens at the authored contact: effort grunt on the wind-up, the heavy punch / stab, the victim's pain
        float hm = DcHitMsNow();
        if (hm > 0.0f) {
            EV(hm - 180.0f, CE_GRUNT);
            if (!fist)
                EV(hm - 120.0f, CE_WHOOSH);
            EV(hm, CD_PAIRS[pair].nseg > 1 ? CE_HEAVY : CE_HIT);
            EV(hm + 40.0f, CE_PAIN);
        } else if (CD_PAIRS[pair].kind == 6) {
            // melee sequence pair (melee_seq.c): its clips begin AT the contact and carry no sound events of their own (ctxsfx_gen.py finds none); the blow's own
            // impact already played when it landed - in a live test the pair ran silent. The attacker's effort
            // and the victim's pain voice now ride the pair's contact.
            EV(0.0f, CE_GRUNT);
            EV(40.0f, CE_PAIN);
        }
    } else if (CdMfRow(pair)) {
        // melee DUEL finisher (final blows were silent and lacked punch): the clips carry only a few sound events of their own (the blow itself is
        // the melee system's hit event in SP), so the blow gets the weapon's SP finisher event + our heavy cue (cd_mf_impact, the cue below is not muted by the
        // clips' native events), the attacker's effort before it, the victim's pain / cry after it; the body fall is the clips' own event or our cue. Camera kick: cd_cam_kicks.
        // (the finishing blow looked weird and instant, washed out - dmax is the row's max start skip (0.067 s), not its contact: the heavy cue
        //  fired 67 ms in, ~290 ms before the fist landed. The blow is the clip's first measured contact (CD_HITS), else 35 % of the move)
        float b = h1;
        if (!f1 && CdMfDmask(pair) && CD_PAIRS[pair].dmax > 0.2f)
            b = sf + CD_PAIRS[pair].dmax * 1000.0f;     // (v5 disarm rows: no measured contact - the clips' own impact event / SP damage tick, gen_duel_fin.py)
        if (CdMfDisRow(pair)) {
            float kb = CdDisBlowMs(pair, st, ns);
            if (kb > sf)
                b = kb;
        }                       // (the victim's death voice event = SP's kill blow)
        EV(b - 180.0f, CE_GRUNT);
        if (!fist)
            EV(b - 110.0f, CE_WHOOSH);
        if (CdMfDisRow(pair)) {      // v5-disfix: SP's disarm sound sequence on the -sync pair's clock (see cd_mf_weapon); b = the take / kill frame of this row
            EV(sf + 30.0f, CE_DGRAB);
            EV(sf +
               (strstr(CD_PAIRS[pair].name, "arm-break") ? 430.0f : 32.0f),
               CE_DBRK);
            EV(b - 250.0f, CE_DWGR);
            EV(b - 80.0f, CE_WHOOSH);
        }
        EV(b, CE_HEAVY);
        EV(b + 40.0f, CE_PAIN);
        EV(b + 220.0f, CE_CRY);
        EV(sf + (fe - 60.0f), CE_FALL);
    } else if (CD_PAIRS[pair].kind == 0) {
        // the upgraded one-shot (kind 0): the swing already landed (no whoosh / grab), the clip starts with the weapon in the body. h1 = the blow (BLADE_CD
        // hit cue: crowbar K_EYE, machete M_GUT), h2 = the crowbar's yank out (K_EXTRACT) / the machete's kick (EXS_GUT_KICK) with the blade pulled
        // out 0.2 s later (M_EXTRACT); the body fall at the victim's pelvis on the floor. The TLOU2 clip sounds (sfx-crowbar-one-hand-finisher-* /
        // sfx-machete-impact-kick, banks not in the MP level) are not generated for these rows (tools/ctxsfx_gen.py): these PCM cues play instead.
        int kick = CdIsMove("upg-blade-gut-chop-kick", pair);
        if (!strncmp(CD_PAIRS[pair].name, "upg-blunt-", 10)) {
            // the hammer finishers (1h blunt): the clip starts AT the blow - one heavy blunt head hit (cd_sfx_label style 0: the pipe head
            // finisher crunch) with the attacker's effort and the victim's pain, h2 = the head hitting the floor (the back
            // bonk: the knees), the body fall at the hand-off. No stab / extract cues (BLADE_CD has no upg-blunt-* row).
            EV(h1 - 20.0f, CE_GRUNT);
            EV(h1, CE_HEAVY);
            EV(h1 + 40.0f, CE_PAIN);
            EV(h2, CE_THUD);
        } else {
            EV(h1, CE_HIT);
            EV(h1 + 40.0f, CE_PAIN);
            EV(h2 - 20.0f, CE_GRUNT);
            if (kick) {
                EV(h2, CE_KICK);
                EV(h2 + 200.0f, CE_HEAVY);
            } else
                EV(h2, CE_HEAVY);
            EV(h2 + 60.0f, CE_CRY);
        }
        EV(sf + (fe - 60.0f), CE_FALL);
    } else if (push) {
        EV(0.0f, CE_GRAB);
        EV(0.02f * L0, CE_GRUNT);
        if (CD_PAIRS[pair].kind == 1)
            EV(0.45f * L0, CE_THUD);    // the push (walls): the victim's back / head meets the surface
        if (!fist)
            EV(hit_at - 120.0f, CE_WHOOSH);
        EV(hit_at, CE_HIT);
        EV(hit_at + 40.0f, CE_PAIN);
    } else {                    // OPEN: the run-in (intro), the grab as the finisher starts
        EV(0.0f, CE_GRUNT);
        EV(sf + 60.0f, CE_GRAB);
    }
    if (CD_PAIRS[pair].kind != 0 && CD_PAIRS[pair].kind < 3 && !g_cdw.on) {
        if (!fist)
            EV(h1 - 120.0f, CE_WHOOSH);
        if (fgrab)
            EV(h1, CE_GRAB);    // the hand closes on the head (wall combo head smash): no blow there
        else if (h1 > hit_at + 250.0f) {
            EV(h1, CE_HIT);
            EV(h1 + 40.0f, CE_PAIN);
        }
        if (CD_PAIRS[pair].style == 3)
            EV(h2 - 60.0f, CE_HIT);     // knife: a stab right before the final one
        EV(h2 - 20.0f, CE_GRUNT);
        EV(h2, CE_HEAVY);
        EV(h2, CE_THUD);
        EV(h2 + 60.0f, CE_CRY);
        EV(sf + 0.95f * fe, CE_FALL);
    }
#undef EV
    for (int a = 1; a < n; a++) {       // by time (insertion sort, 16 items)
        int b = a;
        while (b > 0 && t[b - 1].ms > t[b].ms) {
            __typeof__(t[0]) x = t[b];
            t[b] = t[b - 1];
            t[b - 1] = x;
            b--;
        }
    }
    memcpy(g_cdSfx.ev, t, sizeof t);
    g_cdSfx.n = (size_t) n;
    g_cdSfx.att_free_ms = frel ? sf + frel : h2 + 450.0f;
    g_cdSfx.att_freed = 0;
    g_cdSfx.att_rel = 0;
    // v5-finend (the disarm ended a bit too early): the 0.45 s release after the final blow (a takedown's attacker escapes gunfire) cut a
    // duel finisher's attacker clip ~0.7 s before SP's end (iron-rhino: released 2.07 s, its clip runs on). A finisher ends the life - its attacker plays SP's
    // clip to the end (psync's own blend-out). t2r-fin-early-release.txt = the old release.
    if (CdMfRow(pair)
        && GetFileAttributesA("t2r-fin-early-release.txt") ==
        INVALID_FILE_ATTRIBUTES)
        g_cdSfx.att_free_ms = 0.0f;    // SP's release (knife, CD_HITS [2]) / 0.45 s after the final blow (fists)
    // ps28 (A [117323]-[117375]: the parry pair's hand-off stopped both halves at once - 'psync: ... ABORT (contextual down hand-off)' at 922 ms - and the
    // engine drops a stopped clip's root motion in one frame: the shoved swinger stood still mid-stumble). A parry / clash pair releases its swinger half at the
    // parrier's authored exit through psync_att_out below (0.25 s fade with the clip's root kept by the blend-out drive); the hand-off follows MDP_OUT_TAIL_MS later.
    // The same move time on every client (from the START's kill_s). t2r-parry-ps27.txt = no release (0).
    {
        float pf = MdpAttFreeMs(pair);
        if (pf > 0.0f)
            g_cdSfx.att_free_ms = pf;
    }
    {
        static int pcm_only = -1;
        static uint64_t chk;
        if (pcm_only < 0 || GetTickCount64() - chk > 2000) {
            chk = GetTickCount64();
            pcm_only =
                GetFileAttributesA("t2r-cd-pcm-cues.txt") !=
                INVALID_FILE_ATTRIBUTES;
        }
        for (size_t k = 0;
             !pcm_only && CTX_SFX[k].pair
             && g_cdSfx.nn < sizeof g_cdSfx.nat / sizeof g_cdSfx.nat[0];
             k++) {
            if (strcmp(CTX_SFX[k].pair, CD_PAIRS[pair].name)
                || CdTabSeg(pair, CTX_SFX[k].seg, CTX_SFX[k].role) >= ns)
                continue;
            int sg = CdTabSeg(pair, CTX_SFX[k].seg, CTX_SFX[k].role);
            float lim =
                sg + 1 < ns ? CdSegLen(pair,
                                         sg) *
                1000.0f : CD_PAIRS[pair].end_s * 1000.0f + 300.0f;
            if ((float) CTX_SFX[k].ms > lim)
                continue;       // past the cut / the hand-off: never plays in the chain
            float m = st[sg] * 1000.0f + (float) CTX_SFX[k].ms;
            size_t j = g_cdSfx.nn++;
            g_cdSfx.nat[j].ms = (uint16_t) (m > 60000.0f ? 60000.0f : m);
            g_cdSfx.nat[j].role = (uint8_t) CTX_SFX[k].role;
            g_cdSfx.nat[j].name = CTX_SFX[k].name;
            g_cdSfx.nat[j].joint = CTX_SFX[k].joint;
        }
        for (size_t a = 1; a < g_cdSfx.nn; a++)
            for (size_t b = a;
                 b > 0 && g_cdSfx.nat[b - 1].ms > g_cdSfx.nat[b].ms;
                 b--) {
                __typeof__(g_cdSfx.nat[0]) x = g_cdSfx.nat[b];
                g_cdSfx.nat[b] = g_cdSfx.nat[b - 1];
                g_cdSfx.nat[b - 1] = x;
            }
        for (size_t k = 0;
             !pcm_only && CTX_FX[k].pair
             && g_cdSfx.fn < sizeof g_cdSfx.fx / sizeof g_cdSfx.fx[0];
             k++) {
            if (strcmp(CTX_FX[k].pair, CD_PAIRS[pair].name)
                || CdTabSeg(pair, CTX_FX[k].seg, CTX_FX[k].role) >= ns)
                continue;
            int sg = CdTabSeg(pair, CTX_FX[k].seg, CTX_FX[k].role);
            float lim =
                sg + 1 < ns ? CdSegLen(pair,
                                         sg) *
                1000.0f : CD_PAIRS[pair].end_s * 1000.0f + 300.0f;
            if ((float) CTX_FX[k].ms > lim)
                continue;       // past the cut / the hand-off (the victim's death spurts: throat_blood_start)
            float m = st[sg] * 1000.0f + (float) CTX_FX[k].ms;
            size_t j = g_cdSfx.fn++;
            g_cdSfx.fx[j].ms = (uint16_t) (m > 60000.0f ? 60000.0f : m);
            g_cdSfx.fx[j].role = (uint8_t) CTX_FX[k].role;
            g_cdSfx.fx[j].row = (int) k;
            g_cdSfx.fx[j].done = 0;
            if (k < CTX_FX_N && !g_ctxFxEnt[k].type) {
                g_ctxFxEnt[k].frame = CTX_FX[k].frame;
                g_ctxFxEnt[k].n = (uint32_t) CTX_FX[k].n;
                g_ctxFxEnt[k].pairs = CTX_FX[k].pp;
                g_ctxFxEnt[k].type = CFXE_TYPE;
            }
        }
        g_cdSfx.t60_0 = g_t60 ? g_t60 : 1;
        g_cdSfx.fx_ours = g_cdSfx.fx_engine = g_cdSfx.fx_fail = 0;
        g_cfxeWatch = 1;
        for (size_t a = 1; a < g_cdSfx.fn; a++)
            for (size_t b = a;
                 b > 0 && g_cdSfx.fx[b - 1].ms > g_cdSfx.fx[b].ms; b--) {
                __typeof__(g_cdSfx.fx[0]) x = g_cdSfx.fx[b];
                g_cdSfx.fx[b] = g_cdSfx.fx[b - 1];
                g_cdSfx.fx[b - 1] = x;
            }
        g_cdSfx.native = g_cdSfx.nn > 0;
        Xlog("ctxdown: %s - %d native TLOU2 sound event(s) of the clips armed%s", CD_PAIRS[pair].name, (int) g_cdSfx.nn, pcm_only ? " (off: t2r-cd-pcm-cues.txt)" : g_cdSfx.native ? ", our hit / impact cues off" : "");
    }
    CdVoxArm(pair, st, ns);   // the clips' own voice events (ctx_vox.h): the generic grunt / pain cues are off for this move
    Xlog("ctxdown: sounds armed for %s: %d events on the chain clock (quick hit %.0f ms, finisher contacts %.0f / %.0f ms, finisher from %.0f ms)", CD_PAIRS[pair].name, n, (double) hit_at, (double) h1, (double) h2, (double) sf);
}

// TLOU2 SP's camera kick at every impact of the move (t2r-ext.c XCAM_KICK: SP's camera-shake-melee on top of the authored camera, measured in the
// SP sandbox). The impacts = the clips' own impact sound events (ctx_sfx_events.h, the same events SP's shake follows: the head slam, the throat
// strike, the neck break, the stab / tear into the body, the body into the wall), on the chain clock as the sounds; whooshes, throws, grabs, the
// blade pulled out are not impacts. A move whose clips carry none of them kicks at its measured contacts (CD_HITS / the push's blow).
// Both screens (SP has no player victim; the victim's camera 2 is the same move from the other side). t2r-cd-no-camfx.txt next to the exe = off.
static void CdCamKicks(int pair, int role)
{
    XcamKicksClear();
    {
        FILE *f = ExeSideOpen("t2r-cd-no-camfx.txt", "r");
        if (f) {
            fclose(f);
            Xlog("ctxdown: camera hit kicks off (t2r-cd-no-camfx.txt)");
            return;
        }
    }
    static const char *const IMPACT[] =
        { "sfx-unarmed-finisher-headslam-skin",
"sfx-unarmed-finisher-throat-face-skin", "sfx-unarmed-finisher-neckbreak-combat",
        "sfx-wall-impact-sweetener", "sfx-switchblade-finisher-gut",
            "sfx-switchblade-finisher-gut-cotton",
            "sfx-switchblade-finisher-gut-tear-small",
        "sfx-switchblade-finisher-gut-tear-large"
    };
    static uint64_t sid[sizeof IMPACT / sizeof IMPACT[0]];
    if (!sid[0])
        for (size_t i = 0; i < sizeof IMPACT / sizeof IMPACT[0]; i++)
            sid[i] = FNV64(IMPACT[i]);
    const float amp = 1.0f;     // SP: one amplitude for fist / knife / blunt hits (1.3-1.6 deg roll peaks); see XCAM_KICK
    int ns = CD_PAIRS[pair].nseg;
    float st[CD_MAXSEG + 1];
    st[0] = 0.0f;
    for (int i = 0; i < ns; i++)
        st[i + 1] =
            st[i] + (i + 1 < ns ? CdSegLen(pair, i) : CdFinLen(pair));
    int nsfx = 0, nseg0 = 0;
    for (size_t k = 0; CTX_SFX[k].pair; k++) {
        if (strcmp(CTX_SFX[k].pair, CD_PAIRS[pair].name)
            || CdTabSeg(pair, CTX_SFX[k].seg, CTX_SFX[k].role) >= ns)
            continue;
        int imp = 0;
        for (size_t i = 0; i < sizeof sid / sizeof sid[0]; i++)
            imp |= CTX_SFX[k].name == sid[i];
        if (!imp)
            continue;
        int sg = CdTabSeg(pair, CTX_SFX[k].seg, CTX_SFX[k].role);
        float lim =
            sg + 1 < ns ? CdSegLen(pair,
                                     sg) * 1000.0f : CD_PAIRS[pair].end_s *
            1000.0f + 300.0f;
        if ((float) CTX_SFX[k].ms > lim)
            continue;           // past the cut / the hand-off: never plays in the chain
        XcamKickAdd(st[sg] + (float) CTX_SFX[k].ms / 1000.0f, amp);
        nsfx++;
        nseg0 += sg == 0;
    }
    // SP E=28: the combo hit of the push gets the kick too (roll 2.2 / pitch 3.3 deg on the melee camera); the cover push pairs (-combo-N-hit) carry no
    // impact event of their own: the kick at the push's blow, the time its hit sound plays (cd_sfx_arm)
    if (nsfx && !nseg0 && CdHasPush(pair))
        XcamKickAdd(0.20f * CdSegLen(pair, 0), amp);
    if (CdMfRow(pair)) {
        float hb = 0.35f * CD_PAIRS[pair].end_s;
        for (size_t k = 0; CD_HITS[k].name; k++)
            if (CdIsMove(CD_HITS[k].name, pair) && CD_HITS[k].hit[0])
                hb = CD_HITS[k].hit[0] / 1000.0f;
        float kb =
            CdMfDisRow(pair) ? CdDisBlowMs(pair, st, ns) : -1.0f;
        XcamKickAdd(kb >
                      st[ns - 1] * 1000.0f ? kb / 1000.0f : st[ns - 1] +
                      hb, 1.3f);
    }                           // (was + dmax = 67 ms, before the fist landed)   // a duel finisher: the camera shake at the blow (SP camera-shake-melee 0.5 s / scale 1 per move), a notch above the plain hits
    if (!g_xcamKicks.n && !strncmp(CD_PAIRS[pair].name, "wpn-", 4) && !CdwOld())      // a weapon takedown: its clips' own blow frames (CTX_HIT)
        for (size_t k = 0; CTX_HIT[k].pair; k++) {
            if (CTX_HIT[k].kind != 1
                || strcmp(CTX_HIT[k].pair, CD_PAIRS[pair].name)
                || CdTabSeg(pair, CTX_HIT[k].seg, CTX_HIT[k].role) >= ns)
                continue;
            int sg = CdTabSeg(pair, CTX_HIT[k].seg, CTX_HIT[k].role);
            float lim =
                sg + 1 < ns ? CdSegLen(pair,
                                         sg) *
                1000.0f : CD_PAIRS[pair].end_s * 1000.0f + 300.0f;
            if ((float) CTX_HIT[k].ms <= lim) {
                XcamKickAdd(st[sg] + (float) CTX_HIT[k].ms / 1000.0f,
                              amp);
                nsfx++;
            }
        }
    if (!g_xcamKicks.n) {      // no impact events in the clips: the measured contacts
        float sf = st[ns - 1], fe = CD_PAIRS[pair].end_s;
        uint16_t f1 = 0, f2 = 0;
        for (size_t k = 0; CD_HITS[k].name; k++)
            if (CdIsMove(CD_HITS[k].name, pair)) {
                f1 = CD_HITS[k].hit[0];
                f2 = CD_HITS[k].hit[1];
            }
        if (CdHasPush(pair))
            XcamKickAdd(0.20f * CdSegLen(pair, 0), amp);
        XcamKickAdd(sf + (f1 ? f1 / 1000.0f : 0.35f * fe), amp);
        XcamKickAdd(sf + (f2 ? f2 / 1000.0f : 0.70f * fe), amp);
    }
    char b[160];
    int o = 0;
    for (int i = 0; i < g_xcamKicks.n && o < (int) sizeof b - 12; i++)
        o += snprintf(b + o, SNLEFT(b, o), "%s%.0f", i ? " / " : "",
                      g_xcamKicks.t[i] * 1000.0f);
    Xlog("ctxdown: camera hit kicks (%s) for %s: %d at %s ms on the chain clock (%s), SP's camera-shake-melee shape (0.35 s, 1.4 deg peak)", role == 1 ? "attacker" : "victim", CD_PAIRS[pair].name, g_xcamKicks.n, g_xcamKicks.n ? b : "-", nsfx ? "the clips' impact sound events" : "the measured contacts, no impact events in the clips");
}

// the clips' own blood part-effect events (ctx_fx_events.h) on the chain clock: through the engine's event path (clipfx.c), once per event,
// never on top of the engine's own dispatch of the same clip event (verdict per owner kind, see clipfx.c)
static const char *CdFxName(uint64_t sid)
{
    for (size_t k = 0; CTX_FX[k].pair; k++)
        if (CTX_FX[k].name == sid)
            return CTX_FX[k].pn;
    return NULL;
}

static void CdFxTick(double t_ms)
{
    static int nlf, nlv;
    int off = ClipfxOff() || !CfxeCheck();
    uint8_t *me = BpMe();
    if (g_cfxe_fe_orig)
        CfxeLogEngine(CdFxName, (uint64_t) CfxeObjOf(g_ps.ent_att),
                        (uint64_t) CfxeObjOf(g_ps.ent_vic));
    for (size_t i = g_cdSfx.fi; i < g_cdSfx.fn; i++) {
        __typeof__(g_cdSfx.fx[0]) * f = &g_cdSfx.fx[i];
        if (f->done)
            continue;
        if ((double) f->ms > t_ms)
            break;              // (sorted by time)
        int k = f->row;
        if (k < 0 || k >= (int) CTX_FX_N) {
            f->done = 1;
            continue;
        }
        if (t_ms - f->ms > 400.0 + CFXE_LAG_MS) {
            f->done = 1;
            continue;
        }                       // a late start: what has passed stays passed
        uint64_t ent = f->role ? g_ps.ent_vic : g_ps.ent_att;
        uint8_t *obj = CfxeObjOf(ent);
        if (!obj) {
            f->done = 1;
            continue;
        }
        int kind = obj != me;
        const char *pn = CTX_FX[k].pn, *jn = CTX_FX[k].jn;
        if (CTX_FX[k].name == 0x4b1e84fb0b3138b4ULL && kind) {
            f->done = 1;
            continue;
        }                       // screen-blood-splatter: only on OUR screen (we are the attacker)
        if (!f->role && !strncmp(pn, "bm-", 3) && ClipfxNoAttMasks()) {
            f->done = 1;
            continue;
        }
        if (off) {              // the old path: joint-bound particles only, default knobs
            f->done = 1;
            if (!*jn || !strncmp(pn, "bm-", 3)
                || !strncmp(pn, "screen-", 7))
                continue;
            uint64_t r = ArmorFxSpawn(CTX_FX[k].name, ent, jn);
            if ((!r || r == ~0ULL)
                && !strcmp(jn, "r_hand_prop_attachment"))
                r = ArmorFxSpawn(CTX_FX[k].name, ent, "r_hand_prop");
            if (LOGCAP(nlf, 80))
                Xlog("ctxdown: clip particle %s at %s on the %s (%u ms, spawn-particles-at-joint) -> %llx", pn, jn, f->role ? "victim" : "attacker", f->ms, (unsigned long long) r);
            continue;
        }
        int *v = &g_cfxeVerdict[kind];
        if (*v == 1) {
            f->done = 1;
            g_cdSfx.fx_engine++;
            continue;
        }                       // the engine dispatches the clip events of this kind itself
        if (*v < 0) {
            if (t_ms < f->ms + CFXE_LAG_MS)
                continue;       // wait: does the engine fire it on its own?
            int had =
                CfxeEngineHad(CTX_FX[k].name, obj, g_cdSfx.t60_0), any =
                CfxeEngineAny(obj, g_cdSfx.t60_0);
            *v = had || any;
            Xlog("clipfx: verdict for %s: the engine %s (first takedown event %s on the %s, %u ms; same effect seen %d, any clip part-effect on that body %d)", CfxeKind(kind), *v ? "DISPATCHES the clip part-effects itself - ours stay off for them" : "is SILENT - the clip events fire through clipfx", pn, f->role ? "victim" : "attacker", f->ms, had, any);
            if (*v) {
                f->done = 1;
                g_cdSfx.fx_engine++;
                continue;
            }
        } else if (CfxeEngineHad(CTX_FX[k].name, obj, g_cdSfx.t60_0)) {
            *v = 1;
            f->done = 1;
            g_cdSfx.fx_engine++;
            if (LOGCAP(nlv, 8))
                Xlog("clipfx: the engine fired %s on %s itself after all - verdict flipped, ours off for that kind", pn, CfxeKind(kind));
            continue;
        }
        // GPU-compute streams (pulse / splurt / stream): a full compute-entry table drops them - end one dressing effect first (t2r-ext.c cfx_*)
        if (CfxStream(CTX_FX[k].name)
            || CTX_FX[k].name ==
            0xb9d9c3db6fb35cf7ULL /* blood-ribbon-stream-eff */ ) {
            uint8_t *m = CfxRtmgr();
            if (m && CfxUsed(m) >= CFX_N && f->tries < 15) {
                if (!f->tries)
                    CfxLog(m,
                            "clip stream event waits for a compute entry");
                f->tries++;
                if (f->tries == 1 && !CfxMakeRoom(m))
                    f->tries = 15;
                continue;       // next frame (GameFrameUpdate hands the entry back)
            }
        }
        f->done = 1;
        const CdSeg *sg =
            &CD_PAIRS[g_cdSfx.pair].
            seg[CdTabSeg(g_cdSfx.pair, CTX_FX[k].seg, CTX_FX[k].role)];
        const CfxeEntry *ev = &g_ctxFxEnt[k];
        if (kind && CfxeHasActorPlayer(ev))
            ev = CfxeSelfVariant(ev, &g_ctxFxSelf[k]);
        uint64_t r =
            ClipfxFire(obj, FNV64(f->role ? sg->vic : sg->att), ev,
                        !kind);
        if (r)
            g_cdSfx.fx_ours++;
        else
            g_cdSfx.fx_fail++;
        if (LOGCAP(nlf, 120))
            Xlog("ctxdown: clip fx %s at %s on the %s (%u ms, frame %.0f, %d params) -> engine event path %llx", pn, *jn ? jn : "(world / origin)", f->role ? "victim" : "attacker", f->ms, (double) CTX_FX[k].frame, CTX_FX[k].n, (unsigned long long) r);
    }
    while (g_cdSfx.fi < g_cdSfx.fn && g_cdSfx.fx[g_cdSfx.fi].done)
        g_cdSfx.fi++;
}

static void CdFxSummary(const char *why)
{
    if (!g_cdSfx.fn || g_cdSfx.pair < 0 || g_cdSfx.pair >= (int) CD_N)
        return;
    Xlog("ctxdown: %s - clip blood events: %d of %d fired through the engine event path, %d left to the engine's own dispatch, %d failed (%s)", CD_PAIRS[g_cdSfx.pair].name, g_cdSfx.fx_ours, (int) g_cdSfx.fn, g_cdSfx.fx_engine, g_cdSfx.fx_fail, why);
    g_cdSfx.fn = 0;
}

// ps47 (the finisher punch played late; B [18103]-[18136], 60 fps): the haymaker's punch wave
// played on the chain clock's contact (1079 ms = 546 ms after the -sync START) while the victim's head snapped back ~180 ms earlier on B's screen. For a FIST
// finisher the next punch event is now fired on the frame the attacker's fist is closest to the victim (mct_reach_dist: fist -> head / chest), when that frame
// comes inside 350 ms before the scheduled time: the hand gets within 0.30 m and then moves away again (0.03 m). Otherwise the table time stands.
// t2r-fin-punch-sync-ps47-old.txt = old (table time only).
static float MctReachDist(uint64_t att, uint64_t vic, uint64_t wpn, const char **jn); // melee_duel_contact.c
static struct {
    uint32_t uid;
    size_t i;
    float dmin;
    double tmin;
} g_cd47ps;
static void Cd47PunchSync(double t_ms, uint64_t att, uint64_t vic)
{
    static int off = -1;
    static uint64_t at;
    uint64_t n = GetTickCount64();
    if (off < 0 || n - at > 2000) {
        at = n;
        off =
            GetFileAttributesA("t2r-fin-punch-sync-ps47-old.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    if (off || !att || !vic || g_cdSfx.pair < 0
        || g_cdSfx.pair >= (int) CD_N || !CdMfRow(g_cdSfx.pair))
        return;
    if (g_ps.weapon && g_ps.weapon != FNV64("abby-unarmed"))
        return;                 // fists only
    if (strstr(CD_PAIRS[g_cdSfx.pair].name, "grab"))
        return;                 // (a grab finisher: the grabbing hand sits on the head before the punch)
    size_t i = g_cdSfx.i;
    while (i < g_cdSfx.n && g_cdSfx.ev[i].act != CE_HIT
           && g_cdSfx.ev[i].act != CE_HEAVY)
        i++;
    if (i >= g_cdSfx.n)
        return;
    double due = (double) g_cdSfx.ev[i].ms;
    if (due - t_ms > 350.0 || due <= t_ms)
        return;
    if (g_cd47ps.uid != g_cdSfx.uid || g_cd47ps.i != i) {
        g_cd47ps.uid = g_cdSfx.uid;
        g_cd47ps.i = i;
        g_cd47ps.dmin = -1.0f;
        g_cd47ps.tmin = 0.0;
    }
    const char *jn = "?";
    float d = MctReachDist(att, vic, 0, &jn);
    if (d < 0.0f)
        return;
    if (g_cd47ps.dmin < 0.0f || d < g_cd47ps.dmin) {
        g_cd47ps.dmin = d;
        g_cd47ps.tmin = t_ms;
        return;
    }
    if (g_cd47ps.dmin <= 0.30f && d > g_cd47ps.dmin + 0.03f) {
        static int nl;
        if (LOGCAP(nl, 120))
            Xlog("ctxdown: ps47 %s punch event %u at the visible contact: fist %.2f m from the victim's %s at %.0f ms on the chain clock - fired %.0f ms before its table time %.0f", CD_PAIRS[g_cdSfx.pair].name, (unsigned) i, (double) g_cd47ps.dmin, jn, g_cd47ps.tmin, due - t_ms, due);
        g_cdSfx.ev[i].ms = (uint16_t) (t_ms > 0.0 ? t_ms : 0.0);
        g_cd47ps.i = (size_t) -1;
    }
}

static void CdSfxTick(double t_ms)
{
    BladePendingTick();       // (blade_sfx.h: the knife / blade coming back out after the final blow)
    CdFxTick(t_ms);
    if (!ExSfxOn())
        return;
    uint64_t att = g_ps.ent_att, vic = g_ps.ent_vic;
    g_ex.att_ent = att;
    g_ex.vic_ent = vic;
    g_ex.have_pos = 0;          // (no execution runs at the same time: g_ex.kind stays 0)
    CdVoxTick(t_ms, g_cdSfx.pair, att, vic);
    for (;
         g_cdSfx.ni < g_cdSfx.nn
         && (double) g_cdSfx.nat[g_cdSfx.ni].ms <= t_ms; g_cdSfx.ni++) {
        static int nl;
        if (t_ms - g_cdSfx.nat[g_cdSfx.ni].ms > 400.0)
            continue;
        uint64_t ent = g_cdSfx.nat[g_cdSfx.ni].role ? vic : att;
        if (!ent)
            continue;
        uint64_t a[5] =
            { g_cdSfx.nat[g_cdSfx.ni].name, ent,
    g_cdSfx.nat[g_cdSfx.ni].joint, 0, 0 };
        uint64_t r = DcCall(FNV64("play-sound-at-joint"), 5, a);
        if (LOGCAP(nl, 80))
            Xlog("ctxdown: native sound %016llx at joint %016llx on the %s (%u ms) -> %llx", (unsigned long long) a[0], (unsigned long long) a[2], g_cdSfx.nat[g_cdSfx.ni].role ? "victim" : "attacker", g_cdSfx.nat[g_cdSfx.ni].ms, (unsigned long long) r);
    }
    if (!g_cdSfx.n || g_cdSfx.i >= g_cdSfx.n)
        return;
    Cd47PunchSync(t_ms, att, vic);    // ps47: a fist finisher's punch sounds on the visible contact frame
    for (;
         g_cdSfx.i < g_cdSfx.n
         && (double) g_cdSfx.ev[g_cdSfx.i].ms <= t_ms; g_cdSfx.i++) {
        int act = g_cdSfx.ev[g_cdSfx.i].act;
        if (t_ms - g_cdSfx.ev[g_cdSfx.i].ms > 400.0)
            continue;           // a late start: skip what has passed
        if (act >= CE_DGRAB) {
            CdDisSfxAct(act, att, vic);
            continue;
        }                       // v5-disfix: a disarm row's own SP sounds
        EpCtxAct(act, CD_PAIRS[g_cdSfx.pair].style, att, vic);       // explus.c: the layers of the act (steps, cloth, gear, falls, thumps)
        if (act == CE_GRUNT) {
            if (!CdVoxHas())
                GxPlay(GXS_EFFORT, att, 0);
            continue;
        }                       // (a pair with clip voice events: those play instead, ctx_vox.h)
        if (act == CE_PAIN) {
            if (!CdVoxHas())
                GxPlay(GXS_PAIN, vic, 0);
            continue;
        }
        if (act == CE_CRY) {
            if (!CdVoxHas())
                GxPlay(GXS_PAIN, vic, 1);
            continue;
        }
        if (g_cdSfx.native && act == CE_HEAVY
            && (CD_PAIRS[g_cdSfx.pair].style == 3
                || CD_PAIRS[g_cdSfx.pair].style == 1)) {
            // the clips carry no sound for the blade coming back out and no blood: the pair's extract cue
            // (blade_sfx BLADE_CD post, BLADE_POST_MS after the final blow) and the blood stream on the victim
            const char *pn = CD_PAIRS[g_cdSfx.pair].name;
            for (size_t i = 0; i < sizeof BLADE_CD / sizeof BLADE_CD[0];
                 i++)
                if (CdIsMove(BLADE_CD[i].pair, g_cdSfx.pair)
                    && BLADE_CD[i].post < BC_N)
                    for (int k = 0; k < 4; k++)
                        if (!g_blade_pend[k].due) {
                            g_blade_pend[k].due =
                                (double) GetTickCount64() + BLADE_POST_MS;
                            g_blade_pend[k].cue = BLADE_CD[i].post;
                            g_blade_pend[k].ent = vic;
                            snprintf(g_blade_pend[k].tag,
                                     sizeof g_blade_pend[k].tag,
                                     "%s extract", pn);
                            break;
                        }
            // (SP sandbox: SP's own blood of these finishers is the clips' part-effect events, fired by cd_fx_tick with SP's params -
            // the extra neck stream of throat_blood is not in SP; it stays only on the old path, t2r-clipfx-off.txt)
            // no blood stream came out of the stab wound - SP's stream (blood-ribbon-stream-eff) sits in the combo-mid
            // segment MP's chain skips, so the victim's stream stays on every path (the clips' own events fire on top)
            int oldfx = ClipfxOff() || !CfxeCheck();
            ThroatBloodStart(vic);
            // v5-dishit (the knife disarm's last hit felt very weak at the end, like a bump): a knife disarm's kill blow is the TAKEN weapon's
            // (double-wrist: the 2x4) - its SP kill sound plays too, not only the blade extract cue.
            if (CdMfRow(g_cdSfx.pair)
                && (CD_PAIRS[g_cdSfx.pair].style == 1
                    || (CdMfDisRow(g_cdSfx.pair)
                        && !CdDisSfxOff())))
                CdMfImpact(vic);      // v5-sfx: SP's blade kill sound (melee-effect) on top of the clips' events
            Xlog("ctxdown: %s final blow - blade extract cue armed, blood stream on the victim%s", pn, oldfx ? "" : " (+ the clips' own events)");
            continue;
        }
        // (fist takedowns had no punches - the fist clips carry only 2-3 impact events, SP's punches come from the melee
        // hit system: the fists keep our Abby punch cues for the blows; knife / blade moves play only the clips' own)
        // v5-sfx (melee weapon finisher sfx was lackluster compared to the executions): a weapon duel finisher (mf- row, not the fists / knife)
        // gets SP's layers on the chain clock like the executions' execsp.c layer - the swing's own weapon whoosh + swing foley before the blow (CE_WHOOSH, which the
        // clips' native palm-grab event muted before), the weapon's SP kill event at the head (cd_mf_impact; the 2013-style PCM pipe crunch then stays off), the
        // victim's blunt "bleeh", and the body fall at the hand-off (CE_FALL, also muted by the native rule before). t2r-mf-old-sfx.txt = the old set.
        int mfw = CdMfRow(g_cdSfx.pair) && ((CD_PAIRS[g_cdSfx.pair].style != -1 && CD_PAIRS[g_cdSfx.pair].style != 3) || CdMfDisRow(g_cdSfx.pair)) && GetFileAttributesA("t2r-mf-old-sfx.txt") == INVALID_FILE_ATTRIBUTES;     // (v5-disfix: a disarm row is a fist row that plays the taken weapon's set)
        if (mfw && act == CE_WHOOSH) {
            CdMfWhoosh(att);
            continue;
        }
        if (g_cdw.on && g_cdw.blunt && g_cdSfx.uid == g_ps.uid) {      // a blunt weapon takedown: the held weapon's SP events at the clips' own frames (see CTX_HIT)
            if (act == CE_WHOOSH) {
                CdMfWhoosh(att);
                continue;
            }
            if (act == CE_HIT || act == CE_HEAVY) {
                uint64_t w = CdMfWeapon();
                const char *wn = NULL;
                if (w && w != ~0ULL && !KnifeWeaponId(w)
                    && (MeleeClassOf(BaseWeapon(w), &wn) >= 0
                        || MeleeClassOf(w, &wn) >= 0) && wn
                    && MwsNative(vic, wn,
                                  act == CE_HEAVY ? MWS_FINISHER : MWS_HIT,
                                  act == CE_HEAVY ? g_cdw.zf : g_cdw.zh,
                                  "weapon takedown blow"))
                    continue;
            }
        }
        // A counter with the crowbar did not feel like a crowbar (A 54396-54402 dc-1hblunt-back with the crowbar): a blunt weapon's dodge counter
        // (kind 3-5, not fists / knife / blade) fell through to the generic pipe waves - pipe-whoosh + hit-pipe-torso PCM on top of the counter's own normal
        // hit, which already plays the held weapon's SP hit (sfx-crowbar-one-hand-hit-face-skin). Now its whoosh is the weapon's SP whoosh and the pipe hit
        // wave is gone. t2r-dc-pipe-sfx.txt = the old pipe waves.
        if (CD_PAIRS[g_cdSfx.pair].kind >= 3
            && CD_PAIRS[g_cdSfx.pair].kind <= 5
            && CD_PAIRS[g_cdSfx.pair].style != -1
            && CD_PAIRS[g_cdSfx.pair].style != 1
            && CD_PAIRS[g_cdSfx.pair].style != 3
            && !strstr(CD_PAIRS[g_cdSfx.pair].name, "-palm")
            && GetFileAttributesA("t2r-dc-pipe-sfx.txt") ==
            INVALID_FILE_ATTRIBUTES) {
            if (act == CE_WHOOSH) {
                CdMfWhoosh(att);
                continue;
            }
            if (act == CE_HIT || act == CE_HEAVY) {
                static int nd;
                if (LOGCAP(nd, 30))
                    Xlog("ctxdown: %s blow - no pipe wave, the counter's own hit plays the weapon's SP sound", CD_PAIRS[g_cdSfx.pair].name);
                continue;
            }
        }
        if (act == CE_HEAVY && CdMfRow(g_cdSfx.pair)
            && CdMfImpact(vic) && mfw)
            continue;
        if (mfw && act == CE_HEAVY && CdMfDisRow(g_cdSfx.pair) && g_cdDis.uid == g_ps.uid && g_cdDis.w && CdDisClass(g_cdDis.w, NULL)) {       // v5-disfix: the SP event was refused - the weapon's heavy PCM cue, never the fist punch
            ExPlayLabel(EXS_HIT_PIPE_HEAD_FINISHER, 1.0f,
                          CD_PAIRS[g_cdSfx.pair].name, 1);
            continue;
        }
        if (g_cdSfx.native
            && (act == CE_THUD || act == CE_FALL || act == CE_WHOOSH)
            && !(mfw && act == CE_FALL))
            continue;
        int palm = strstr(CD_PAIRS[g_cdSfx.pair].name, "-palm") != NULL;       // knife palm bash counter: a punch, not a stab
        if (g_cdSfx.native && !palm && CD_PAIRS[g_cdSfx.pair].style != -1
            && (act == CE_HIT || act == CE_HEAVY) && !(act == CE_HEAVY
                                                       &&
                                                       CdMfRow(g_cdSfx.
                                                                 pair)))
            continue;           // the clips' own sounds play (a duel finisher's blow keeps our heavy cue)
        if (CD_PAIRS[g_cdSfx.pair].kind >= 3
            && (act == CE_HIT || act == CE_HEAVY) && (palm
                                                      || CD_PAIRS[g_cdSfx.
                                                                  pair].
                                                      style == -1)
            && vic) {
            // counterattack sfx made punchier: TLOU2's own unarmed finisher layers on the victim's head under our PCM punch
            // (palm: + SP's own contact sound of the palm bash, sfx-unarmed-hit-face-skin - SP capture)
            static const char *const L[3] =
                { "sfx-unarmed-finisher-face-chin",
"sfx-unarmed-breaker-face-skin", "sfx-unarmed-hit-face-skin" };
            for (int q = 0; q < (palm ? 3 : 2); q++) {
                uint64_t a[5] = { FNV64(L[q]), vic, FNV64("headb"), 0, 0 };
                uint64_t r = DcCall(FNV64("play-sound-at-joint"), 5, a);
                static int nq;
                if (LOGCAP(nq, 40))
                    Xlog("ctxdown: counter punch layer %s -> %llx", L[q],
                         (unsigned long long) r);
            }
        }
        if (palm) {
            int lb = CdSfxLabel(-1, act);
            if (lb >= 0)
                ExPlayLabel(lb, 1.0f, CD_PAIRS[g_cdSfx.pair].name, 1);
            continue;
        }
        {
            int bk = act == CE_WHOOSH ? 0 : act == CE_HIT ? 1 : act == CE_HEAVY ? 2 : -1;       // knife / 1h-blade pairs: TLOU2's layered blade cues (blade_sfx.h BLADE_CD)
            if (bk >= 0) {
                char bn[64];
                size_t bl = CdBaseLen(CD_PAIRS[g_cdSfx.pair].name);
                if (bl > 63)
                    bl = 63;
                memcpy(bn, CD_PAIRS[g_cdSfx.pair].name, bl);
                bn[bl] = 0;
                if (BladeCdAct
                    (bn, CD_PAIRS[g_cdSfx.pair].style, bk, vic, att))
                    continue;
            }
        }
        int lb = CdSfxLabel(CD_PAIRS[g_cdSfx.pair].style, act);
        if (lb >= 0)
            ExPlayLabel(lb, 1.0f, CD_PAIRS[g_cdSfx.pair].name, 1);
    }
}

// every frame of our fist takedown half (cd_hands_begin): the engine's melee unholster stays off and a weapon that shows up in the hand is put away
// again (once per weapon id), until the attacker is released after the final blow (or the move ends / aborts)
static void CdHandsTick(void)
{
    if (!g_cdHandsOn)
        return;
    if (g_ps.role != 1 || g_ps.kind != PS_CTXDOWN
        || g_ps.uid != g_cdHandsUid || (g_cdSfx.uid == g_ps.uid
                                          && g_cdSfx.att_freed)) {
        g_cdHandsOn = 0;
        Xlog("ctxdown: fist takedown hands hold over (%s) - the next punch draws the fists again", g_ps.role != 1 || g_ps.uid != g_cdHandsUid ? "the move ended" : "attacker released");
        return;
    }
    uint64_t none0 = 0, z[2] = { 0, 0 };
    DcCall(FNV64("player-disable-unholstering"), 0, &none0);
    uint64_t held = DcCall(FNV64("get-player-weapon-id"), 0, &none0);
    if (held && held != ~0ULL && CdDisKeeps(held))
        return;                 // v5-armed: the disarm's taken weapon, drawn at SP's take frame
    if (held && held != ~0ULL && held != g_cdHandsTried) {
        g_cdHandsTried = held;        // one put-away per weapon id per move (an object that will not holster is not retried every frame)
        uint64_t r = (g_holsterInstFrame =
                      g_frame ? g_frame : 1,
                      DcCall(FNV64("holster-player-weapon-instantly"), 2,
                              z)), now =
            DcCall(FNV64("get-player-weapon-id"), 0, &none0);
        static int n;
        if (LOGCAP(n, 12))
            Xlog("ctxdown: %llx showed up in the hand mid takedown - put away again (holster %llx, held now %llx)", (unsigned long long) held, (unsigned long long) r, (unsigned long long) now);
    }
}

static int CdOpenFinisher(uint64_t move)
{
    int p = CdPairOfMove(move);
    return p >= 0 && !strncmp(CD_PAIRS[p].name, "mf-", 3);
}

// an open-ground duel pair (md- hit, mf- finisher, mp- clash, dc- dodge counter): no surface, each screen places it on its own bodies (paired.c psync_local_anchor)
// dcounter.c pairs (dc-*): the dodger starts them DC p0 into the clips (0.2-0.3 s) - the countered side must start at the same phase
static int CdCounterMove(uint64_t move)
{
    int p = CdPairOfMove(move);
    return p >= 0 && !strncmp(CD_PAIRS[p].name, "dc-", 3);
}

static int CdLocalMove(uint64_t move)
{
    int p = CdPairOfMove(move);
    const char *n = p >= 0 ? CD_PAIRS[p].name : "";
    if (strstr(n, "wall") || strstr(n, "cover"))
        return 0;               // pin pairs (mp-*-wall- / -low-cover-) stay on the surface frame
    // v5-finap (the two screens showed a finisher out of sync): the victim's local fit moved a finisher's ap
    // 1.64 m (A 28694); the victim fell where A saw the pair, the attacker stood where B saw it, and the attacker's release 0.45 s after the last blow
    // handed its puppet back to the net 1+ m away mid-clip (B swung at the air on A's screen). A finisher ends the life: both screens play it on the
    // attacker's sent ap (the victim's body is pulled onto its spot by SP's 0.35 s pull). t2r-fin-local-fit.txt = the local fit again.
    if (!strncmp(n, "mf-", 3)
        && GetFileAttributesA("t2r-fin-local-fit.txt") ==
        INVALID_FILE_ATTRIBUTES)
        return 0;
    return !strncmp(n, "md-", 3) || !strncmp(n, "mf-", 3)
        || !strncmp(n, "mp-", 3) || !strncmp(n, "dc-", 3);
}

static int PulFailMove(uint64_t move)
{
    int p = CdPairOfMove(move);
    return p >= 0 && !strncmp(CD_PAIRS[p].name, "pu-fail", 7);
}

static int PulDragMove(uint64_t move)
{
    int p = CdPairOfMove(move);
    return p >= 0 && CD_PAIRS[p].kind == 8;
}

static const char *PulPairName(uint64_t move)
{
    int p = CdPairOfMove(move);
    return p >= 0 && CD_PAIRS[p].kind == 8 ? CD_PAIRS[p].name : NULL;
}                               // paired.c psync_baked_root

static int CdWallMove(uint64_t move)
{
    int p = CdPairOfMove(move);
    return p >= 0 && strstr(CD_PAIRS[p].name, "wall") != NULL;
}

static int MpnHoldAtt(void);  // melee_duel_pin.c: we pin someone - our attacker half stays on until the next blow
static void CdlTick(void);
// ps40 D1 (the blade finisher's corpse lies at 117.800, stands at 117.834, hangs in the air to ~131 s; again at 174.03).
// The ps39 hold never ran: A [15476] 'slot 0 puppet released (contextual down hand-off, alive)', [15480] 'slot 0 puppet dead here 0 -> 1 (net dead 0, downed 0, hp 0.0)',
// 'fling: ... held at ... for 45 frames', 'deathreact: slot 0 died (standing)'; B [17550] 'finisher by a blade (machete): the victim dies at the kill frame (hp 32.0 ...)',
// [17551] 'our victim body taken over at t=1797 ms (Death state before the kill)', [17552] 'hand-off at 2.28 s (role 2 ...)'. Causes: 1) the fate wait asked for <= 1.5 hp, a
// blade kills from any hp (32 here) and its death lands 4 frames after the hand-off; 2) psync_dead(SID_PLAYER) is always 0, so our own death was never seen by the hold;
// 3) the Death enter cleared our lying half one frame before the hand-off. Now: a row that kills (an mf- finisher row with a START weapon whose blade kills - known on
// every screen from the START) never lets its victim half go as alive: the other screens wait for the death (CD_KILL_WAIT_T), then the dead puppet goes into psync's
// corpse hold (the half frozen on its lying end pose at the clip's place, net channels suspended until the respawn: no base pose, no standing death reaction, no
// fling pin - both skip a paired body); our own body keeps its half from before the kill and goes limp from the clip pose at the hand-off (vdf_limp).
// t2r-cd-kill-row-ps39.txt = old.
#define CD_KILL_WAIT_T 45       // [TUNING ps40] ticks (0.75 s: the death arrived 4 frames after the hand-off in rec39a, both cases; a slow link needs more)
#define CD_OWN_LIMP_T 8         // [TUNING ps40] ticks at most between our own limp call and the hand-off (ends on the ragdoll bit)
static int CdKillRowOff40(void)
{
    static int off = -1;
    static uint64_t chk;
    uint64_t n = GetTickCount64();
    if (off < 0 || n - chk > 2000) {
        chk = n;
        off =
            GetFileAttributesA("t2r-cd-kill-row-ps39.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    return off;
}

static int CdRowKills40(void)
{
    if (!g_ps.role || g_ps.kind != PS_CTXDOWN || !g_ps.weapon
        || CdKillRowOff40())
        return 0;
    int p = CdPairOfMove(g_ps.move);
    return p >= 0 && CdMfRow(p) && MeleeBladeKills(g_ps.weapon);
}

static void CdBladeKillNote(void)
{                               // t2r-ext.c: our own blade finisher death, called right before the kill
    if (g_ps.role != 2 || g_ps.kind != PS_CTXDOWN
        || g_ps.ent_vic != SID_PLAYER || CdKillRowOff40())
        return;
    g_vdfScreamUid = g_ps.uid;
    PsyncKeepVictim
        ("blade finisher kill inside its pair - the lying half stays through the Death enter, the engine's scream is kept (ps40)");
}

#define CD_FATE_WAIT_T 15       // [TUNING ps39] ticks (0.25 s: the death arrived 4 frames late at 9 ms one-way in rec38a)
#define CD_DEAD_HOLD_T 36       // [TUNING ps39] ticks (0.6 s: the standing phase before the ragdoll measured 0.3-0.6 s in rec38a)
// v5-armed (the victim's gun stayed in its hand: the victim of the wall takedown
// abby-fist-kill-hwall-combo-head-smash, kept the rifle in its hands through the whole half on its own screen - log 50609 "stow fix: PlayerHolsterWeaponAnimAction
// not begun (no put-away arm layer, victim half)": our holster gates refuse the NPC clip's own put-away on the victim, while every other screen hid the gun
// (cd_pup_hands_empty)). The victim halves are authored empty-handed: the victim of a paired melee move puts a GUN / bow / throwable away with our own instant
// holster at its START (passes the gates; a long gun goes onto its sling like any instant holster), as the execution victims do (vic_unarm_tick). A melee item stays
// in the hand (SP: the defender keeps its melee weapon; a disarm takes it at its take frame). The gun comes back (instant draw) once no pair has run on us for
// 0.35 s while we stand with empty hands (a duel blow chain keeps it away until the chain is over; a down / death never draws it). t2r-vic-keep-gun.txt = off.
static struct {
    uint32_t uid;
    uint64_t gun, end_t60;
} g_cdVgun;
static int MduEngagedNow(void);       // melee_duel.c
static void CdVicGunTick(void)
{
    static int off = -1;
    static uint64_t chk;
    if (off < 0 || GetTickCount64() - chk > 3000) {
        chk = GetTickCount64();
        off =
            GetFileAttributesA("t2r-vic-keep-gun.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    if (g_ps.role == 2 && g_ps.kind == PS_CTXDOWN && !g_ps.ended) {
        g_cdVgun.end_t60 = 0;
        if (off || g_ps.uid == g_cdVgun.uid)
            return;
        g_cdVgun.uid = g_ps.uid;
        uint64_t held = CurrentWeapon();
        if (!held || held == ~0ULL
            || !(BaseWeaponIsGun(held) || WeaponIsThrowable(held)))
            return;
        uint64_t z[2] = { 0, 0 }, r = (g_holsterInstFrame =
                                       g_frame ? g_frame : 1,
                                       DcCall(FNV64
                                               ("holster-player-weapon-instantly"),
                                               2, z)), now =
            CurrentWeapon();
        if (!g_cdVgun.gun)
            g_cdVgun.gun = held;
        int p = CdPairOfMove(g_ps.move);
        Xlog("ctxdown: victim half %s u%08x on us - our gun %llx put away for it (the victim halves are authored empty-handed; holster-player-weapon-instantly %llx, held %llx); drawn back after the last pair", p >= 0 ? CD_PAIRS[p].name : "?", g_ps.uid, (unsigned long long) held, (unsigned long long) r, (unsigned long long) now);
        return;
    }
    if (!g_cdVgun.gun)
        return;
    if (g_downed || g_downDying || g_downAt || g_execUntil
        || LocalHp() <= 0.0f || g_inSpawn) {
        Xlog("ctxdown: our gun %llx not drawn back (down / dead / respawn)", (unsigned long long) g_cdVgun.gun);
        g_cdVgun.gun = 0;
        return;
    }
    if (g_ps.role)
        return;                 // another paired move of ours (attacker side): later
    if (!g_cdVgun.end_t60) {
        g_cdVgun.end_t60 = g_t60 ? g_t60 : 1;
        return;
    }
    if (g_t60 - g_cdVgun.end_t60 < S60(0.35f))
        return;
    if (MduEngagedNow() && g_t60 - g_cdVgun.end_t60 < S60(1.2f))
        return;                 // a duel still engaged: the next blow's pair may follow - no pop in / out between blows
    uint64_t cw = CurrentWeapon(), gun = g_cdVgun.gun;
    g_cdVgun.gun = 0;
    g_cdVgun.end_t60 = 0;
    if (cw && cw != ~0ULL && cw != FNV64("abby-unarmed")
        && cw != FNV64("ellie-unarmed") && !(g_femKnifeCur
                                             && cw == g_femKnifeCur)) {
        Xlog("ctxdown: our gun %llx not drawn back - the hand holds %llx now (the player's own pick)", (unsigned long long) gun, (unsigned long long) cw);
        return;
    }
    uint64_t has = DcCall(FNV64("has-weapon?"), 1, &gun);
    if (has == ~0ULL || !(has & 0xff)) {
        Xlog("ctxdown: our gun %llx not drawn back - not in the inventory any more", (unsigned long long) gun);
        return;
    }
    uint64_t r =
        DcCall(FNV64("player-instant-draw-weapon-by-name"), 1, &gun);
    Xlog("ctxdown: our gun %llx drawn back after the paired move(s) on us (instant draw %llx, held %llx)", (unsigned long long) gun, (unsigned long long) r, (unsigned long long) CurrentWeapon());
}

// ---- ps31 D. FINISHER VOICES, armed once per finisher (mf- row) on every screen from cd_tick, on the move's own clock.
//  victim : the shocked gasp at the finisher's start (exec_beg.c eb_fin_gasp: the victim's own voice set, `say` with subtitles) - here for the screens that saw no
//           intro (the default PUNCH windup passes the SP intro over; bystanders); where an intro played the gasp was said at its first frame (once per finisher).
//  attacker (our own screen only): SP says `player-melee-killed-enemy` a median 600 ms after the -sync pair starts (57 of 59 requested, 5 audible: the engine drops it
//           while the attacker's effort voice is busy). MP already HAS the attacker's line - the team voice kind VX_DOWN ("we downed an enemy"), but only 1.1 s after
//           the victim's MSG_DOWNED = ~3.3 s after the -sync start (A [20947] FINISHER -> [21146] line). Now the same line is asked P31V_KILL_MS after the
//           -sync start; like SP it is dropped when our voice is busy (is-speaking?), and then the old after-the-down line still comes. When it plays, VX_DOWN's
//           5 s cooldown drops the later one. t2r-fin-voices-ps30.txt = off (the gasp too).
#define P31V_KILL_MS 600        // SP: median 600 ms after the -sync start
static void EbFinGasp(uint64_t ent, uint32_t slot, uint32_t seed, const char *why);   // exec_beg.c
static struct {
    uint32_t uid;
    double kill_ms;
    int said;
} g_p31v;
static int P31vOff(void)
{
    static int v = -1;
    static uint64_t at;
    uint64_t n = GetTickCount64();
    if (v < 0 || n - at > 2000) {
        at = n;
        v = GetFileAttributesA("t2r-fin-voices-ps30.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    return v;
}

static void P31vArm(int pr, double tms)
{
    g_p31v.uid = g_ps.uid;
    g_p31v.said = 1;
    g_p31v.kill_ms = 0.0;
    if (P31vOff())
        return;
    int ns = CD_PAIRS[pr].nseg;
    double sync0 = 0.0;
    for (int i = 0; i + 1 < ns; i++)
        sync0 += (double) CdSegLen(pr, i) * 1000.0;
    if (tms < sync0 + 250.0)
        EbFinGasp(g_ps.ent_vic, (uint32_t) g_ps.vic, g_ps.uid, "the finisher pair's start");  // (not for a move joined late)
    if (g_ps.role == 1) {
        g_p31v.kill_ms = sync0 + P31V_KILL_MS;
        g_p31v.said = 0;
    }
    static int nl;
    if (LOGCAP(nl, 80))
        Xlog("fin voices: %s armed at %.0f ms of the move (role %d): -sync pair from %.0f ms%s (ps31)", CD_PAIRS[pr].name + 3, tms, g_ps.role, sync0, g_ps.role == 1 ? ", our kill line asked 600 ms after it" : "");
}

static void P31vTick(double tms)
{
    if (g_p31v.said || g_p31v.uid != g_ps.uid || tms < g_p31v.kill_ms)
        return;
    g_p31v.said = 1;
    static int nl;
    if (tms - g_p31v.kill_ms > 400.0 || g_downed || g_downDying)
        return;                 // a stalled frame / we went down: the moment has passed
    uint64_t a1 = SID_PLAYER, sp = DcCall(FNV64("is-speaking?"), 1, &a1);
    if (sp != ~0ULL && (sp & 0xff)) {
        if (LOGCAP(nl, 80))
            Xlog("fin voices: attacker kill line at %.0f ms dropped - our voice is busy (the clip's own effort); the enemy-down line after the hand-off stays (ps31; SP: 5 of 57 audible)", tms);
        return;
    }
    if (LOGCAP(nl, 80))
        Xlog("fin voices: attacker kill line asked at %.0f ms = %d ms after the -sync start (team voice kind VX_DOWN, own voice / faction / sex; ps31)", tms, P31V_KILL_MS + (int) (tms - g_p31v.kill_ms));
    VxEvent(VX_DOWN,
             "finisher kill line 600 ms after the -sync start (ps31)");
}

static void CdTick(void)
{
    CdEscTick();
    CdVicGunTick();
    DcTick();
    CdlTick();                 // v5-lethal: a held lethal contact's open-ground finisher (gate answers)
    CdScanTick();
    CdWearTick();
    if (g_cdDis.uid
        && !(g_ps.role && g_ps.kind == PS_CTXDOWN
             && g_ps.uid == g_cdDis.uid))
        CdDisTick(0.0);       // (a disarm cut / over: its clean-up)
    if (g_ps.role && g_ps.kind == PS_CTXDOWN) { // the shared cut: every client at the same move time
        double t = (PsyncMs() - g_ps.t0 + g_ps.chain_shift) / 1000.0;  // ABSOLUTE move time: a chain moves t0 forward at every segment switch
        if (g_cdSfx.uid != g_ps.uid) { // arm the takedown's sounds once per move, on every client
            int pr = CdPairOfMove(g_ps.move);
            if (pr >= 0)
                CdSfxArm(pr);
            else {
                memset(&g_cdSfx, 0, sizeof g_cdSfx);
                g_cdSfx.uid = g_ps.uid;
            }
            if (pr >= 0 && CdMfRow(pr))
                P31vArm(pr, t * 1000.0);       // ps31 D: the victim's gasp + our kill line
            // a late start (the victim's START skip): the cues of the part never played are dropped, not fired at once
            double t0ms = t * 1000.0 - 30.0;
            while (g_cdSfx.i < g_cdSfx.n
                   && (double) g_cdSfx.ev[g_cdSfx.i].ms < t0ms)
                g_cdSfx.i++;
            while (g_cdSfx.ni < g_cdSfx.nn
                   && (double) g_cdSfx.nat[g_cdSfx.ni].ms < t0ms)
                g_cdSfx.ni++;
            for (size_t k = 0; k < g_cdSfx.fn; k++)
                if ((double) g_cdSfx.fx[k].ms < t0ms)
                    g_cdSfx.fx[k].done = 1;
        }
        CdSfxTick(t * 1000.0);
        P31vTick(t * 1000.0);  // ps31 D: our kill line 600 ms after the -sync start
        // The attacker must not stay locked until the victim is downed (it needs time to escape if shot): the
        // attacker half blends back to locomotion 0.45 s after the final blow on every screen (psync_att_out), and our takedown camera ends;
        // the victim half plays on to the hand-off. t2r-cd-att-hold.txt = the old hold to the hand-off.
        if (!g_cdSfx.att_freed && g_cdSfx.att_free_ms > 0.0f && t * 1000.0 >= g_cdSfx.att_free_ms && t < g_ps.kill_s - 0.2 && g_ps.cur_seg >= g_ps.nseg - 1) {       // (last segment only: a segment switch re-animates both halves)
            static int hold = -1;
            static uint64_t hchk;
            if (hold < 0 || GetTickCount64() - hchk > 2000) {
                hchk = GetTickCount64();
                hold =
                    GetFileAttributesA("t2r-cd-att-hold.txt") !=
                    INVALID_FILE_ATTRIBUTES;
            }
            g_cdSfx.att_freed = 1;
            // A pinned player was teleported out of bounds (the first hit punched it far away): a pin blow pressed after
            // this release started its swing on a body whose top animation instance no longer carried the pair's apReference - the engine's
            // melee swing takes its ap from that instance, so the low-cover / wall swing aligned to the world origin and dragged us 300 m
            // (313 -> 194 -> 0, 404 -> 139 in 3 frames). Blows pressed before the release were clean. While we pin, our half is not released early:
            // the next blow's swing always starts on top of the pair (its ap), the hand-off still ends it.
            int pinhold = !hold && g_ps.role == 1 && MpnHoldAtt();
            if (pinhold)
                Xlog("ctxdown: attacker half kept to the hand-off (we pin: the next blow's swing takes its ap from it)");
            if (!hold && !pinhold) {
                PsyncAttOut(t * 1000.0);
                g_cdSfx.att_rel = 1;
                g_cdAttFree = 1;      // feature_stuck_guard: the attacker's guarded move ends HERE (control is back), not at the victim's hand-off
                if (g_ps.role == 1) {   // where the clip leaves us (root motion kept, as in SP): the stuck guard's end spot is checked against this
                    float lb2[16], ex[8];
                    const float *pl = PlayerLocator(lb2);
                    PsyncBf bf2;
                    PsyncBfMake(&bf2, g_ps.ap);
                    float ph = PsyncPhase(SID_PLAYER, g_ps.att_sid);
                    if (pl && PsLocOk(pl) && ph >= 0.0f
                        && PsyncExpected(SID_PLAYER, g_ps.att_sid, &bf2,
                                          ph, ex)) {
                        g_cdRelOk = 1;
                        memcpy(g_cdRelClip, ex, sizeof g_cdRelClip);
                        Xlog("ctxdown: release - we stand at %.2f %.2f %.2f, the clip's attacker spot (phase %.3f) %.2f %.2f %.2f: %.2f m apart (the body stays where the clip left it)", pl[0], pl[1], pl[2], ph, ex[0], ex[1], ex[2], __builtin_sqrtf((pl[0] - ex[0]) * (pl[0] - ex[0]) + (pl[2] - ex[2]) * (pl[2] - ex[2])));
                    }
                }
                if (g_ps.role == 1)
                    CdCamEnd("attacker free after the final blow");
                Xlog("ctxdown: attacker released at %.2f s (%s), the victim half plays on to %.2f s", t, CD_PAIRS[g_cdSfx.pair].style == 3 ? "TLOU2 SP's knife release: last blade event + 0.45 s" : "final blow + 0.45 s", (double) g_ps.kill_s);
            }
        }
        CdDisTick(t);         // (v5 disarm finishers: the weapon changes hands at the hand-off, before the abort below)
        // ps39 B1 (rec38a, 3 of 3 blade finisher kills, both screens: the lying corpse stands up for 0.3-0.6 s, then ragdolls. B [17517] 'finisher by a blade
        // (machete): the victim dies at the kill frame', [17520] 'psync: ... ABORT (contextual down hand-off)'; A [15401] 'fling: slot 0 puppet died here', [15402]
        // 'visdiag: slot 0 [ pose ]: pelvis 0.11 -> 0.87 m'): the kill comes 0.10 s before the hand-off, and the hand-off blended the victim half out of a body the
        // engine had no death pose for yet - it showed its base (standing) pose until the ragdoll took it. A victim that is dead at the hand-off keeps its half
        // (psync_keep_victim: put back whenever the Death state drops the layer) for CD_DEAD_HOLD_T ticks, then the hand-off runs as before.
        // t2r-cd-dead-hold-ps38.txt = old.
        static uint32_t dh_uid;
        static uint64_t dh_until;
        int dh_wait = 0;
        int own_dead40 = g_ps.role == 2 && g_ps.ent_vic == SID_PLAYER && PsyncOwnDeadNow() && !CdKillRowOff40();  // ps40 D1: psync_dead() never answers for our own body
        int kills40 = CdRowKills40();
        if (own_dead40 && t >= g_ps.kill_s && g_cd.ended_uid != g_ps.uid) {
            static uint32_t ol_uid;
            static uint64_t ol_until;
            uint8_t *me40 = VdfPlayer();
            if (ol_uid != g_ps.uid) {
                ol_uid = g_ps.uid;
                ol_until = g_t60 + CD_OWN_LIMP_T;
                PsyncKeepVictim
                    ("dead inside the finisher / takedown - our lying pose is kept until the ragdoll owns the body (ps40)");
                // ps41 (rec40a B [18702] 'own death controller starts an action (after the half): FadeToState 482732f9a2bb5fbb' inside this limp, sheet rev40b
                // z_121.06_B_standup: lying at +0.066 s, upright with the arms up at +0.100 s): the death controller answers its ragdoll call with its
                // post-ragdoll state, and the powered ragdoll follows that state's standing death pose. An execution's limp withholds that action (paired.c
                // vdh_start: g_vdf_noanim + g_vdf_exec_limp - set there by the downed death); a standing finisher victim never had g_vdf_noanim. Set for
                // this death too, before the limp. t2r-cd-own-limp-noanim-ps40.txt = old.
                if (GetFileAttributesA("t2r-cd-own-limp-noanim-ps40.txt")
                    == INVALID_FILE_ATTRIBUTES) {
                    g_vdf_noanim = 1;
                    g_vdfNoanimFrame = g_t60;
                    g_vdf_ragged = 1;
                    Xlog("ctxdown: our death inside the pair - the death controller's post-ragdoll action is withheld like an execution's (no standing death pose under the limp; ps41)");
                }
                VdfLimp("hand-off of a move we died in (ps40)");
                Xlog("ctxdown: hand-off at %.2f s - our own body is dead (Death state, hp %.1f): limp from the clip's lying pose now, the half is held until the ragdoll owns the body (max %d ticks; ragdoll bit %d; ps40)", t, (double) LocalHp(), CD_OWN_LIMP_T, me40 ? (me40[0x9fd] & 1) : -1);
            }
            dh_wait = g_t60 < ol_until && !(me40 && (me40[0x9fd] & 1));
        } else if (kills40 && g_ps.role != 2 && t >= g_ps.kill_s
                   && g_cd.ended_uid != g_ps.uid && g_ps.ent_vic
                   && g_ps.vic >= 0 && g_ps.vic < T2R_MAX_PEERS
                   && !g_peerDowned[g_ps.vic]) {
            static uint32_t kw_uid;
            static uint64_t kw_until;
            if (kw_uid != g_ps.uid) {
                kw_uid = g_ps.uid;
                kw_until = g_t60 + CD_KILL_WAIT_T;
                Xlog("ctxdown: hand-off at %.2f s - this row kills (blade %llx from the START): the victim half (slot %d) is not let go alive, dead here %d (waits up to %d ticks for the death; role %d; ps40)", t, (unsigned long long) g_ps.weapon, g_ps.vic, PsyncDead(g_ps.ent_vic), CD_KILL_WAIT_T, g_ps.role);
            }
            if (PsyncDead(g_ps.ent_vic)) {
                g_ps.flags |= PS_F_LETHAL;      // (local: the corpse hold is the lethal moves' own end of a victim half)
                g_psCorpseForce = 1;
                PsyncPupRelease(g_ps.vic,
                                  "killing finisher row: the dead victim keeps its lying end pose (ps40)",
                                  0);
                g_psCorpseForce = 0;
                g_psCorpseKeep40 = 1;
                Xlog("ctxdown: hand-off at %.2f s - the victim (slot %d) died %d ticks after the hand-off time: corpse hold on the clip's lying pose, no release as alive (ps40)", t, g_ps.vic, (int) (CD_KILL_WAIT_T - (int64_t) (kw_until - g_t60)));
            } else {
                dh_wait = g_t60 < kw_until;
                if (!dh_wait)
                    Xlog("ctxdown: hand-off at %.2f s - the death of slot %d did not arrive in %d ticks: its half is let go alive (ps40)", t, g_ps.vic, CD_KILL_WAIT_T);
            }
        } else
            if (t >= g_ps.kill_s && g_cd.ended_uid != g_ps.uid
                && g_ps.ent_vic && PsyncDead(g_ps.ent_vic)
                && GetFileAttributesA("t2r-cd-dead-hold-ps38.txt") ==
                INVALID_FILE_ATTRIBUTES) {
            if (dh_uid != g_ps.uid) {
                dh_uid = g_ps.uid;
                dh_until = g_t60 + CD_DEAD_HOLD_T;
                PsyncKeepVictim
                    ("killed inside the finisher / takedown - its lying pose is held until the ragdoll owns the body (ps39)");
                Xlog("ctxdown: hand-off at %.2f s held %d ticks - the victim (slot %d) is dead: its half keeps the body's pose until the ragdoll owns it (role %d; ps39)", t, CD_DEAD_HOLD_T, g_ps.vic, g_ps.role);
            }
            dh_wait = g_t60 < dh_until;
        }
        // ps39 B7 (rec38a B [16640] 'hand-off at 1.43 s (role 1 ...)', [16644] 'trade: slot 1 died', [16645] 'fling: slot 1 puppet ... died here' - the pin-escape shove
        // killed the pinner; on ITS screen the death came 2 frames before the hand-off (A [14516] / [14518], held by the rule above), on ours the news came 4 frames
        // after we had let its puppet go: it stood up on the net, then died): on a screen that is not the victim's, a victim at <= 1.5 hp (cut to 1 hp by the pair's
        // blow: it goes down or dies at the hand-off) is not let go until its down / death has arrived, at most CD_FATE_WAIT_T ticks. t2r-cd-fate-wait-ps38.txt = old.
        if (!dh_wait && t >= g_ps.kill_s && g_cd.ended_uid != g_ps.uid
            && g_ps.role != 2 && g_ps.ent_vic && g_ps.vic >= 0
            && g_ps.vic < T2R_MAX_PEERS && !PsyncDead(g_ps.ent_vic)
            && !g_peerDowned[g_ps.vic]
            && GetFileAttributesA("t2r-cd-fate-wait-ps38.txt") ==
            INVALID_FILE_ATTRIBUTES) {
            typedef uint8_t *(*fn_peer39)(uint32_t);
            static uint32_t fw_uid;
            static uint64_t fw_until;
            uint8_t *pe39 = ((fn_peer39) T2rPeer) ((uint32_t) g_ps.vic);
            float nh = pe39
                && !IsBadReadPtr(pe39 + 0xa5,
                                 4) ? *(float *) (pe39 + 0xa5) : 100.0f;
            if (nh <= 1.5f) {
                if (fw_uid != g_ps.uid) {
                    fw_uid = g_ps.uid;
                    fw_until = g_t60 + CD_FATE_WAIT_T;
                    Xlog("ctxdown: hand-off at %.2f s waits up to %d ticks - the victim (slot %d) is at %.1f hp: its down / death has not arrived yet (role %d; ps39)", t, CD_FATE_WAIT_T, g_ps.vic, (double) nh, g_ps.role);
                }
                dh_wait = g_t60 < fw_until;
            }
        }
        if (t >= g_ps.kill_s && g_cd.ended_uid != g_ps.uid && !dh_wait) {       // (per move: a client that was the attacker once is victim / third party of the next)
            g_cd.ended = 1;
            g_cd.ended_uid = g_ps.uid;
            g_cdUpgSlot = -1;
            g_cdUpgW = 0;
            Xlog("ctxdown: hand-off at %.2f s (role %d, slot %d -> %d): both halves blend out%s", t, g_ps.role, g_ps.att, g_ps.vic, g_ps.role == 2 ? ", the victim goes down now" : g_ps.role == 1 ? ", we are free" : " (third party)");
            // (spamming punch did nothing for 3 s: [184601] a duel parry-sync pair on us took the victim melee lock, the duel ended at [184621]
            // before the pair did (its 'pair over' unlock never ran), the pair's hand-off at [184670] downed nobody (no pending down), and the 240-frame victim lock ran on to [184849] = 3 s of
            // dead punches). A hand-off that goes to no down releases the victim lock itself.
            if (g_ps.role == 2 && g_meleeLockVictim && !g_cdVicDown
                && !g_downAt) {
                g_meleeLockFrom = 0;
                Xlog("melee input unlocked at the hand-off (no down follows)");
            }
            CdCamEnd("contextual down hand-off");
            CdFxSummary("hand-off");
            PsyncAbort("contextual down hand-off", 0);
            g_psCorpseKeep40 = 0;
            g_cdAttFree = 0;
        }
        CdHandsTick();
    } else {
        g_cdAttFree = 0;
        if (g_cdHandsOn)
            CdHandsTick();
        if (g_cdSfx.fn)
            CdFxSummary("psync over");
        g_cfxeWatch = 0;
        if (g_cd.cam_fin)
            CdCamEnd("psync over");
    }
    // the clip's own camera did not come up (attacker log: "finisher cam: no animated camera 30 frames in"): the exec pair-framing camera
    // (cinematic, smooth in / out) takes over from there so the attacker never keeps the plain gameplay camera for a takedown
    if (g_cd.cam_fin && g_cd.cam_check && g_frame == g_cd.cam_check) {
        uint8_t *d = FincamDirector();
        if (!d || !*(uint32_t *) (d + 0x60)) {
            CdCamEnd("no animated camera 36 frames in");
            float left =
                g_cd.cam_end + 0.5f - (float) (g_frame -
                                               g_cd.cam_frame) / 60.0f;
            if (left > 0.3f && g_cd.cam_other
                && XcamStart(g_cd.cam_other, left, 0, -1, g_cd.cam_end,
                              g_cd.cam_role, "ctxdown"))
                Xlog("ctxdown: the clip camera never started - the execution pair-framing camera for the remaining %.2f s", left);
            else
                Xlog("ctxdown: the clip camera never started and the fallback camera did not either - the game camera stays");
        }
        g_cd.cam_check = 0;
    }
    if (g_cd.vic_on) {
        if (g_downed) {
            Xlog("ctxdown: hand-off to downed - downed %d, no fall %d, %llu ticks after the start", g_downed, g_downNoFall, (unsigned long long) (g_t60 - g_cd.vic_t0));
            g_cd.vic_on = 0;
            g_cdVicDown = 0;
        } else if (!g_downAt && g_t60 - g_cd.vic_t0 > 6) {
            Xlog("ctxdown: victim side dropped - the pending down is gone (death / reset / execution) without a down");
            g_cd.vic_on = 0;
            g_cdVicDown = 0;
            g_skMfVic = 0;
        } else if (g_t60 - g_cd.vic_t0 > 600) {
            Xlog("ctxdown: victim side timed out without a down (10 s)");
            g_cd.vic_on = 0;
            g_cdVicDown = 0;
            g_skMfVic = 0;
        }
    }
}

// the would-be downing swing (feature_early_finisher, at the swing's START): with a ready plan on that victim the swing is cancelled and the
// takedown chain plays INSTEAD of the 5th punch; the contact path (park_hook -> cd_try) stays as the fallback
static int CdSwingStart(void)
{
    if (!CdEnabled() || !g_cd.plan_ok || g_cd.plan_slot < 0
        || g_cd.plan_slot >= T2R_MAX_PEERS || g_ps.role)
        return 0;
    int32_t slot = g_cd.plan_slot;
    if (g_peerDowned[slot] || g_puppetDeadHere[slot]
        || IsTeammate((uint32_t) slot))
        return 0;
    if (g_meleeDown && was_up_g && g_upgradeArmed > 0)
        return 0;               // an upgraded melee's one-shot plays its own 2013 finisher on contact (onetap_contact -> upg13_start): no takedown instead
    float hp = 0;
    int stunned = 0;
    uint64_t none_w = 0, has_w = DcCall(SID_HAS_MELEE_WEAPON, 0, &none_w);
    float dmg = has_w != ~0ULL && (has_w & 0xff) ? MeleeDmgNow() : FIST_DAMAGE;       // (a held weapon's swing: its own damage decides "this swing downs them")
    if (!MeleeWouldKill(slot, dmg, &hp, &stunned))
        return 0;
    g_cdSwingPath = 1;
    int started = CdTry(slot);
    g_cdSwingPath = 0;
    if (!started)
        return 0;
    g_kdFallbackFrame = g_t60;
    g_kdFallbackSlot = slot;  // a late kill-move answer / a contact of the cancelled swing is dropped
    uint64_t me = SID_PLAYER;
    uint64_t c =
        DcCall(0x394888944e949d5fULL /* cancel-melee-action */ , 1, &me);
    Xlog("ctxdown: the downing swing on slot %d (hp %.0f) is REPLACED by the takedown (swing cancelled -> %llx)", slot, hp, (unsigned long long) c);
    return 1;
}

// ---------------------------------------------------------------------------------------------------- v5-lethal: the lethal CONTACT outside a duel
// Reported case (female, switchblade): the last hit did not activate a finisher, just a normal hit with no sound. VERIFIED main log [53269]-[53582]:
// the duel had ended idle ([53269]); the lethal swing started 2.96 m from the victim ([53553]), past the duel's 2.4 m engage reach, so no duel swing armed an
// SP finisher; the engine's lunge + lock-on glide carried it to a contact ([53568]) and park_hook knew only ctxdown's wall / cover plan - a cover plan made
// 293-308 frames earlier 1.5-3.6 m away (the scans stop beyond 2.5 m and a 'no surface' scan of the same victim keeps the old plan) - cd_try refused it as
// stale and the would-kill blow went out as the plain knockdown hit. The open-ground SP finishers (cd_fin_pick / the mf- rows) were reachable only from the
// duel's own swing. Now (park_hook, after cd_try): the open-ground SP finisher on the spot - the full-path gate of cd_upg_go (pg_gate, the lying body
// included), the SP intro passed over (the swing's contact IS the blow), the hit held while the gate answers (<= CDL_TRIES frames, then the next loaded
// finisher, up to 4); every refusal ends in kd_plain_hit, which now plays the contact's sound. t2r-no-contact-finisher.txt next to the exe = the old plain down.
static int MduOwnsLethal(int s);      // melee_duel.c: our running duel swing on s is lethal with its SP finisher armed
static int MduRowOf(uint64_t move);   // melee_duel.c
static int MduLethalSound(int s);     // melee_duel.c: the duel's lethal blow (no finisher) plays its own sound
static uint64_t DcWeapon(void);        // dcounter.c
static uint64_t MhrPunchAt(uint64_t ent, int str, const char *who);   // melee_hitreact.c
static int g_mhrPunchThud;    // (tentative: melee_hitreact.c)
#define CDL_TRIES 12
static struct {
    int on, slot, pair, first, tries, alt;
    uint64_t w, frame;
    float dmg;
    const char *sw;
} g_cdl;
static int CdlOff(void)
{
    static int v = -1;
    static uint64_t at;
    uint64_t n = GetTickCount64();
    if (v < 0 || n - at > 2000) {
        at = n;
        int o =
            GetFileAttributesA("t2r-no-contact-finisher.txt") !=
            INVALID_FILE_ATTRIBUTES;
        if (o != v)
            Xlog("ctxdown: contact finisher %s",
                 o ?
                 "OFF (t2r-no-contact-finisher.txt): a lethal contact outside a duel is the plain knockdown"
                 :
                 "on: a lethal contact that no surface takedown takes plays an open-ground SP finisher");
        v = o;
    }
    return v;
}

// the plain knockdown hit of a connected would-kill blow: our own type 3 hit, lethal 0 (their recv_hit_hook cuts it to 1 hp and downs them), WITH the contact's
// sound + blood on the puppet. VERIFIED ([53568], no 'knife hit sfx' / 'melee hit fx' line after it, the whoosh at [53556] was the only cue): winmm parks only
// the hits it predicts lethal (park_hook), so the downing blow never ran hitsend_hook / directhit_hook, where every other melee contact plays its sound
// (knife_hit_native for a female's knife, melee_hit_fx for a weapon, the fist punch) - the knockdown was the one silent hit.
static void KdPlainHit(uint32_t ks, uint64_t weapon, float dmg,
                         const char *why)
{
    if (ks >= T2R_MAX_PEERS)
        return;
    uint8_t m[0x16];
    uint64_t ammo = 0;
    memcpy(m, &weapon, 8);
    memcpy(m + 8, &ammo, 8);
    memcpy(m + 16, &dmg, 4);
    m[20] = 0;
    m[21] = 0;
    if (g_sendTramp)
        HrSendHit(m, sizeof m, (uint8_t) ks);
    uint64_t ke = PuppetHandle(ks);
    const char *snd =
        MduLethalSound((int) ks) ?
        "the duel's own lethal blow sound (mdu_contact)" :
        "none (no puppet / a pair owns the bodies)";
    if (ke && !PsyncOwn() && !MduLethalSound((int) ks)) {
        MfkAttackerHit(ks, weapon, dmg, 1);   // the hit kick / hit-stop of a heavy blow
        if (g_femEllieOn || KnifeWeaponId(weapon)) {
            KnifeHitNative(ke, 1);
            snd = "the knife's finisher hit (knife_hit_native)";
        } else if (MeleeClassOf(weapon, NULL) >= 0
                   || MeleeClassOf(BaseWeapon(weapon), NULL) >= 0) {
            g_mwsTier = MWS_FINISHER;
            g_mws_zone = MWS_FACE;
            MeleeHitFx(ke, weapon);
            g_mwsTier = MWS_HIT;
            g_mws_zone = MWS_GUT;
            snd = "the weapon's SP kill sfx + blood (melee_hit_fx)";
        } else {
            g_mhrPunchThud = 1;
            MhrPunchAt(ke, 2, "knockdown blow");
            g_mhrPunchThud = 0;
            snd = "the heavy punch (mhr_punch_at)";
        }
    }
    Xlog("knockdown: our swing CONNECTED with slot %u (would-kill %.0f, weapon %llx) - sent as a plain hit, lethal 0 (no 0x11 ask, no 250 marker); contact sound: %s (%s)", ks, dmg, (unsigned long long) weapon, snd, why);
}

// one try of the held contact finisher: 1 = started or the path gate is still answering (the hit stays held), 0 = given up (g_cdl cleared)
static int CdlStep(void)
{
    uint64_t dw = DcWeapon(), mel = dw == FNV64("abby-unarmed") ? 0 : dw;
    char why[240];
    for (;;) {
        g_cdFinSkipIntro = 1;
        int r = CdUpgGo((uint32_t) g_cdl.slot, mel, 0, g_cdl.pair);
        g_cdFinSkipIntro = 0;
        if (r == 1) {
            g_kdFallbackFrame = g_t60;
            g_kdFallbackSlot = g_cdl.slot;    // a late kill-move answer / second contact for this victim is dropped
            Xlog("ctxdown: contact finisher %s STARTED on slot %d %llu frame(s) after the contact (gate tries %d, alternative %d) - the victim is DOWN at its hand-off, no hit sent", CD_PAIRS[g_cdl.pair].name + 3, g_cdl.slot, (unsigned long long) (g_frame - g_cdl.frame), g_cdl.tries, g_cdl.alt);
            g_cdl.on = 0;
            return 1;
        }
        if (r == 2 && ++g_cdl.tries <= CDL_TRIES)
            return 1;
        Xlog("ctxdown: contact finisher %s on slot %d not started (%s)",
             CD_PAIRS[g_cdl.pair].name + 3, g_cdl.slot,
             r ==
             2 ? "the path gate never answered" :
             "cd_upg_go refused: the reason is the 'melee upgrade' line just above - path gate / spot guard / victim turn / locators / psync");
        if (g_cdl.alt >= 4)
            break;
        g_cdl.alt++;
        int nx =
            CdFinPick((uint32_t) g_cdl.slot, g_cdl.sw, g_cdl.alt, why,
                        sizeof why);
        if (nx < 0 || nx == g_cdl.first)
            break;
        Xlog("ctxdown: contact finisher - the next one: %s", why);
        g_cdl.pair = nx;
        g_cdl.tries = 0;
    }
    g_cdl.on = 0;
    return 0;
}

// park_hook, a would-kill CONTACT of ours that cd_try did not take: 1 = handled here (finisher started / held for its gate / the duel owns it), the caller sends nothing;
// 0 = the caller sends the plain down hit (kd_plain_hit)
static int CdContactFin(uint32_t ks, uint64_t weapon, float dmg)
{
    if (ks >= T2R_MAX_PEERS)
        return 0;
    if (MduOwnsLethal((int) ks)) {
        Xlog("ctxdown: contact finisher - slot %u: the duel's lethal swing owns this blow (its SP finisher is armed) - the engine contact dropped", ks);
        return 1;
    }
    if (g_cdl.on)
        return g_cdl.slot == (int) ks;  // (a second contact of the held swing: dropped)
    if (CdlOff() || !CdEnabled())
        return 0;
    if (g_ps.role || g_downed || g_downDying) {
        Xlog("ctxdown: contact finisher - slot %u: not now (paired move %d, we are down %d)", ks, g_ps.role, g_downed);
        return 0;
    }
    typedef uint64_t(*fn_move) (uint64_t);
    int row = MduRowOf(((fn_move) T2R_SYM(kFnCurrentMove)) (SID_PLAYER));
    const char *sw = row >= 0 ? MDU_MOVES[row].move : NULL;
    char why[240];
    int pair = CdFinPick(ks, sw, 0, why, sizeof why);
    if (pair < 0) {
        Xlog("ctxdown: contact finisher - slot %u: no open-ground SP finisher (%s) - the plain down hit", ks, why);
        return 0;
    }
    Xlog("ctxdown: contact finisher - slot %u: lethal contact (%.0f) that no surface takedown took -> open-ground SP finisher %s", ks, dmg, why);
    memset(&g_cdl, 0, sizeof g_cdl);
    g_cdl.on = 1;
    g_cdl.slot = (int) ks;
    g_cdl.pair = g_cdl.first = pair;
    g_cdl.w = weapon;
    g_cdl.dmg = dmg;
    g_cdl.frame = g_frame;
    g_cdl.sw = sw;
    return CdlStep();
}

// cd_tick: the held contact's next gate try; given up = the plain down hit now (a few frames late at most)
static void CdlTick(void)
{
    if (!g_cdl.on || g_cdl.frame == g_frame)
        return;
    uint32_t s = (uint32_t) g_cdl.slot;
    uint64_t w = g_cdl.w;
    float dmg = g_cdl.dmg;
    if (g_peerDowned[s] || g_puppetDeadHere[s] || g_downed
        || g_downDying || MatchSubstate() != 6) {
        Xlog("ctxdown: contact finisher on slot %u dropped (victim down %d / dead %d, we down %d, substate %d, %llu frames) - no hit", s, g_peerDowned[s], g_puppetDeadHere[s], g_downed, MatchSubstate(), (unsigned long long) (g_frame - g_cdl.frame));
        g_cdl.on = 0;
        return;
    }
    if (g_frame - g_cdl.frame > 60) {
        g_cdl.on = 0;
        KdPlainHit(s, w, dmg, "the contact finisher was held 60 frames");
        return;
    }                           // (safety cap: cdl_step gives up long before)
    if (CdlStep())
        return;
    KdPlainHit(s, w, dmg, "the contact finisher was refused");
}
