// melee_duel_parry.c - the melee DUEL's PARRY / CLASH beat (spec re/melee-duel-sp-spec.md beat 3, sections 4 + 10: the parry at clip 44.1, SP gate in melee-dc.md).
// Included after melee_duel.c (t2r-ext.c); melee_duel.c forward-declares the mdp_* hooks below (one call each: swing start, partner swing seen, op >= 6, combat tick,
// victim pair begin, contact hold). Everything is a no-op unless the duel is on (t2r-melee-duel.txt) and the SP parry paks are in the MP level (anim_len > 0).
//
// SP source: in the Abby-vs-Ellie final fight Abby parries Ellie's knife swings and shoves her off, Ellie parries Abby's punches with the knife, and once the
// knife is gone the same parries go on with fists - so the pairs work with NO melee weapon on either side. Table: mduel_parry.h (tools/mv3/gen_duel_parry.py):
// the boss block-pushoffs (parrier class 0: unarmed), the NPC 1h / 2h parries (class 1 / 2: spec 44.1, weapon meets weapon overhead), Ellie's knife parry of a punch (class 3).
//
// THE RULE
//  PARRY  the defender B presses melee (= B's own swing starts) while the attacker A's swing runs, and the SP gate holds, measured on A's clock:
//         frames-til-hit > 4 (133 ms, estimate-frames-til-hit), the swing's contact still ahead, B within reach and facing A within 90 deg, B's parry cooldown (SP
//         npc-contextual-parry-cooldown 4-6 s: 4 s) over. B's client sends CLAIM(kind, B's press time measured from A's swing start = latency-corrected by the one-way
//         estimate, B's weapon set). A (attacker-decides, like beat 1's contact) re-checks the same gate on its OWN swing timeline, then holds its contact, places and
//         path-gates the pair and starts it: A's swing is cancelled, the parry pair (swinger half on A, parrier half on B) replaces the -hit pair: no damage, the
//         blow is pushed off (the SP shove ~1 m at +0.75 s), hand-off at MDP_END_S, A's presses are blocked 300 ms longer than B's (B gets the initiative).
//         The pairs, the exit point, the approach and the initiative follow SP's pattern - see the ps28 block below (t2r-parry-ps27.txt = this text).
//  CLASH  both press within MDP_CLASH_MS (220 ms) of each other: the same pair, the LOWER SLOT is the initiator and plays the swinger half (no authority round trip:
//         mex_self() is the same on both screens, like melee_exchange's "the lower slot attacks"); both come out at the duel spacing, nobody gets the initiative.
//  A claim that fails the gate (late, cooldown, out of reach, not facing, pair clips not loaded, path blocked) costs nothing: the swing goes on as a normal duel blow.
// Logs: "DUEL: PARRY ...".
#include "mduel_parry.h"
#define MDP_MIN_LEAD_MS 133     // SP: estimate-frames-til-hit > 4 frames (30 fps) before the blow
#define MDP_MAX_LEAD_MS 300     // SP: the parry is accepted 133-300 ms before the contact - a 167 ms window
#define MDP_HONOR_MS 30         // the attacker must still have this long before its contact when the claim reaches it (mdp_recv "late: our contact is due")
// (the parry's usable window was ~0-40 ms in the earlier build): the old gate asked for 133 ms of lead AFTER the claim crossed (til - ow > 133),
// i.e. 133 + 2 x one-way ms ahead of the contact as the parrier saw it - past SP's 300 ms for any rtt > 70 ms, so the window was empty; and a press inside 340 ms of their swing start was
// a CLASH whenever the contact was > 20 ms ahead, which swallowed the rest. Now the window is judged on the contact AS THE PARRIER SAW IT (S = lead on the swinger's clock + one-way, the
// latency compensation the dodge ruling gives the dodger): S in (LB, LB + 167], LB = max(133, 30 + 2 x one-way) - SP's 133-300 ms at low latency, always 167 ms wide, the claim still
// reaching the attacker >= 30 ms before its contact. A press earlier than that is an ordinary swing (no early-press parry). The parry beats the clash inside that window; a press within
// 220 ms of their swing start stays a clash. t2r-parry-old-gate.txt = the old rule.
static int MdpOldGate(void)
{
    static int v = -1;
    static uint64_t at;
    uint64_t n = GetTickCount64();
    if (v < 0 || n - at > 2000) {
        at = n;
        v = GetFileAttributesA("t2r-parry-old-gate.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    return v;
}

static int MdpLb(int ow)
{
    int b = 30 + 2 * ow;
    return b > MDP_MIN_LEAD_MS ? b : MDP_MIN_LEAD_MS;
}

// Looser, tuning (not 2013 data; presses 99 / 36 ms before the contact were refused "too late"): the LATE edge of the parry window moves from
// mdp_lb (133 ms+, the claim reaching the attacker 30 ms before its contact) to the contact itself as the parrier's screen shows it, i.e. the claim reaches the attacker the
// moment its contact is due (seen > 2 x one-way). The EARLY edge stays lb + 167. t2r-melee-strict.txt = the old late edge (melee_dodge.c mw_strict).
static int MdpLo(int ow)
{
    return MwStrict()? MdpLb(ow) : (ow > 0 ? 2 * ow : 0);
}

// Clashes are made somewhat more prominent: test logs (~330 duel swings) had ONE
// clash (+165 ms) and one more near-simultaneous pair (+250 ms: the second swing started 53 ms before the first one's contact) that the 220 ms window missed.
//  - simultaneity window 220 -> MDP_CLASH_MS 340 ms; past the old 220 ms (MDP_CLASH_BASE_MS) the second swing must start "into the windup": the FIRST swing's contact
//    still MDP_CLASH_LEAD_MS ahead once the claim / START reaches the other side (til - one-way > 20 ms), so the pair always replaces both blows
//  - the higher slot holds its own contact while the lower slot's clash pair can still arrive (MDP_HOLD_MS + 2 x one-way, max MDP_EXP_MAX_MS): a swing that started
//    first can't land its blow on the clash that replaces it (the lower slot holds its own through mdp_hold already)
//  - clash cooldown 3 s -> 2 s (still no clash chains)
// The lower slot alone starts the pair (unchanged), so both screens agree.
#define MDP_CLASH_OLD_MS 340    // v5-dmg: both press within this of each other = a clash (was 220)
// (melee felt slower without improving fairness): the simultaneity window 340 -> 276 ms, so fewer near-simultaneous
// presses turn into a clash pair (the 330 ms wind-up made 340 ms cover a whole swing). t2r-clash-ps46-old.txt = 340 (re-read every 2 s).
#define MDP46W_CLASH_MS 276     // [TUNING ps46]
static int Mdp46wClashMs(void)
{
    static int v = -1, lg = -1;
    static uint64_t at;
    int o = Md45Flag("t2r-clash-ps46-old.txt", &v, &at), r =
        o ? MDP_CLASH_OLD_MS : MDP46W_CLASH_MS;
    if (lg != r) {
        lg = r;
        Xlog("duel clash rule: ps46 simultaneity window %d ms (%s)", r,
             o ? "t2r-clash-ps46-old.txt: v5-dmg's 340" :
             "[TUNING ps46], v5-dmg 340");
    }
    return r;
}

#define MDP_CLASH_MS Mdp46wClashMs()
#define MDP_CLASH_BASE_MS 220   // within this: a clash whatever the windups (the old rule)
#define MDP_CLASH_LEAD_MS 20    // past MDP_CLASH_BASE_MS: the first swing's contact must still be this far ahead once the message has crossed (til - one-way)
#define MDP_EXP_MAX_MS 320      // the higher slot's contact hold while a clash is expected
#define MDP_CLASH_COOL_MS 2000  // both mashing = 'parry parry parry' - a clash can't follow a clash with the same player for 2 s (v5-dmg: was 3 s)
#define MDP_COOL_MS 4000        // SP npc-contextual-parry-cooldown 4.0-6.0 s (low end): per parrier
#define MDP_END_S 1.10f         // hand-off: the shove is done at +0.75-0.83 s (spec 44.1 / 10), SP's counter tell came at +0.82-1.0 s
#define MDP_HOLD_MS 220         // A holds its own contact this long while the parry pair is placed / path-gated
#define MDP_INIT_MS 300         // the parrier's head start over the pushed attacker
#define MDPO_CLAIM 6            // MSG_MEX op 5 sub-op: [kind, B set, rel lo, rel hi, B swing row]
enum { MDPK_PARRY, MDPK_CLASH };
static const char *const MDPK_NAME[] = { "PARRY", "CLASH" };
static const char *const MDPK_TAG[] = { "", "CLASH " }; // ps28: the log read "DUEL: PARRY PARRY STARTED"

// (hand-played: B swung at A, A pressed melee, and B ducked and tried to haymaker but on contact the anim
// cut = 'PARRY pick ... vs a parrier of class 3: mp-fist-light-right-parry-enemy-ellie-hit' -> 'PARRY PARRY STARTED ... (933 ms pair ...), hand-off 0.88 s' -> 'psync: ...
// ABORT (contextual down hand-off)' at 922 ms of a 1.60 s move: the Abby-vs-Ellie boss knife parry, whose SP exit is frame 12 (0.40 s, into Ellie's riposte tell) and whose
// tail is her lunge into that riposte; our generic hand-off (the shorter half - 50 ms, cap MDP_END_S) cut it at the lunge. Decision: follow SP's
// pattern, a parry is a clinch and a shove with no built-in punch, played to its exit point; afterwards the parrier may swing first and the other player can still dodge.)
// SP (melee-only run, 12 parry-sync moves; DC npc-1h-parrys / npc-2h-parrys): parry-enter 250 ms -> parry-sync: clinch 0.36-0.52 m from +100 to +600 ms, the shove
// from +600 ms (1.1 m apart at +770-970 ms), the parrier leaves at its force-abort frame (833 ms 1h vs fist, 1000 ms the others) into a SEPARATE counter tell (450-700 ms)
// + swing (101 ms to the contact), the player may dodge out from the abort-evade frame (967-1167 ms).
//  - PAIRS  only the NPC clinch-and-shove families (mdp_row28): the boss rows (pcls 0 pushoffs, pcls 3 knife parry) are never picked. Fists on fists has no SP clip:
//           the regular parry of a punch (npc-f-1hblunt-parry-fist-*-cross) plays with the parrier's hands empty.
//  - EXIT   one hand-off for both = the parrier's authored exit + MDP_OUT_TAIL_MS; the swinger half is released AT the parrier's exit through psync_att_out
//           (ctxdown.c cd_tick, mdp_att_free_ms): its 0.25 s fade runs with the clip's root motion kept (psync's blend-out drive), so the stumble is not stopped dead.
//  - START  the swing's warp target eases onto the pair's authored start distance while the pair is placed; the other hand's row when only that one is within
//           MDP_START_OFF_M of where the bodies are (SP itself snaps up to 0.59 m / 26 deg on the pair's first frame).
//  - INITIATIVE  the parried swinger's melee is off (press latch: pad AND keyboard) to the hand-off + MDP_INIT28_MS; its dodge is free from the hand-off. The parrier's
//           swing started at once lands NPCW_CONTACT_MS 380 ms after the hand-off = before the swinger's press comes back: dodge it, or be hit. A clash gives none.
// t2r-parry-ps27.txt = everything as in ps27 (boss rows, 0.88 / 1.10 s hand-off, 300 ms pad-only lock).
static int MdpPs27(void)
{
    static int v = -1;
    static uint64_t at;
    uint64_t n = GetTickCount64();
    if (v < 0 || n - at > 2000) {
        at = n;
        v = GetFileAttributesA("t2r-parry-ps27.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    return v;
}

#define MDP_INIT28_MS (Mw45ContactMs() + 20)   /* ps45: = the light-swing contact + one frame (ps28: 400 = 380 + 20) */       // [TUNING ps28] the swinger's melee lock past the hand-off = NPCW_CONTACT_MS 380 + one frame (SP: the counter's contact comes 0.55-0.8 s after the lock)
#define MDP_OUT_TAIL_MS 370     // psync_att_out's fade PS_OUT 0.25 s + its 0.12 s root drive: the swinger's release to the pair's hand-off
#define MDP_PX_DEF_MS 1000      // a row with no authored parrier exit in the DC (none of the picked rows)
#define MDP_START_OFF_M 0.60f   // SP's largest first-frame snap of a parry-sync (0.59 m): a row whose authored start is further off than this is not this hand's first pick
#define MDP_APPR_STEP 0.06f     // [TUNING ps28] m a frame the swing's contact distance moves onto the pair's authored start distance (SP's own body step p90 0.07 m a frame)
#define MDP_APPR_OK_M 0.30f     // [TUNING ps28] the pair starts once our body is this close to its authored spot ...
#define MDP_APPR_MS 120         // [TUNING ps28] ... or this long after the claim, or one frame before the swing's contact - whichever is first
#define MDP_FU_MS 1500          // a swing of the parrier this long after the hand-off is logged as the parry's follow-up
#define MDP_FU_CONTACT_MS (Mw45ContactMs())   /* ps45 (was 380) */    // = melee_npcwind.c NPCW_CONTACT_MS (that file is included after this one): a light swing's start to its contact
static struct {
    int p_row, p_slot, my_row, pend, pend_kind, pend_bset, pend_rel,
        pend_sw, pend_pr, pend_mr, gate_tries, claim_kind, nlog;
    uint64_t p_t0, my_t0, pend_at, claim_at, exp_until,
        cool_until[T2R_MAX_PEERS], clash_cool[T2R_MAX_PEERS];
    struct {
        uint64_t at, ent;
        int8_t snd, aset;
    } q[6];
    // ps28
    float pend_auth, pend_D0;
    int pend_exact, appr_logged;        // the picked pair's authored start distance, the swing's own contact distance (put back if the pair is abandoned)
    uint32_t run_uid;
    int run_role, run_end_ms, run_out_ms, run_kind;
    uint64_t run_t0;            // the parry / clash pair in flight (for its exit line)
    int par_slot;
    uint64_t par_end;           // we were parried by par_slot, the pair's planned hand-off
    uint64_t fu_from, fu_until, fu_sw;
    int fu_slot;                // we parried fu_slot: the hand-off, the follow-up window's end, our follow-up swing's start
    // THEIR parry cooldown as the swinger tracks it. Until ps27 the swinger wrote it into cool_until[], the array our OWN parry press reads: being parried put our own
    // parry on the 4 s cooldown, so the parrier's follow-up could not be parried in turn (SP's cooldown is per parrier).
    uint64_t cool_them[T2R_MAX_PEERS];
} g_mdp = {.p_row = -1,.my_row = -1,.par_slot = -1,.fu_slot = -1 };

#define MDLOG(...) DULOG(__VA_ARGS__)
// the extra strikes (steps 5 / 6) do not follow the step parity - fists-stun-combo-1 is the LEFT backhand, fists-stun-combo-2 and the 1h blunt
// second strike (knee-bash-2) are right-hand overhead blows; the parry row must be the hand that swings (SP: left-hand swing -> the "-left-" parry rows).
static int MdpSide(const MduRowT *m)
{
    return strstr(m->move, "left")
        || !strcmp(m->move, "fists-stun-combo-1") ? 0 : !strcmp(m->move,
                                                                "fists-stun-combo-2")
        || !strcmp(m->move,
                   "1hblunt-stun-breaker-combo-2-breaker") ? 1 : (m->
                                                                  step & 1)
        ? 1 : 0;
}                               // left jab / even steps = the left hand

static int MdpPcls(int bset, int aset)
{
    return bset == MDU_FIST ? 0 : bset == MDU_KNIFE ? (aset ==
                                                       MDU_FIST ? 3 : 1) :
        bset == MDU_2H ? 2 : 1;
}

static int MdpRowOfPair(int pr)
{
    for (size_t i = 0; i < MDP_ROWS_N; i++)
        if (!strcmp(CD_PAIRS[pr].name, MDP_ROWS[i].pair))
            return (int) i;
    return -1;
}

static int MdpIsPair(int pr)
{
    return pr >= 0 && pr < (int) CD_N && CD_PAIRS[pr].kind == 7
        && MdpRowOfPair(pr) >= 0;
}

// the best loaded row: the swinger's set (or its nearest), the parrier's class (or the nearest), the side (or the other hand)
static int MdpPick(int aset, int side, int pcls, int *exact)
{
    static const int8_t AS[MDU_NSET][3] =
        { { MDU_FIST, -1, -1 }, { MDU_1HBLUNT, MDU_1HBLADE, MDU_KNIFE },
        { MDU_1HBLADE, MDU_KNIFE, MDU_1HBLUNT }, { MDU_1HBLUNT,
                                                  MDU_1HBLADE, MDU_KNIFE },
        { MDU_KNIFE, MDU_1HBLADE, MDU_1HBLUNT } };
    static const int8_t PC[4][4] =
        { { 0, 1, 2, 3 }, { 1, 2, 0, 3 }, { 2, 1, 0, 3 }, { 3, 1, 2, 0 } };
    for (int a = 0; a < 3; a++) {
        if (AS[aset][a] < 0)
            break;
        for (int c = 0; c < 4; c++)
            for (int sd = 0; sd < 2; sd++)
                for (size_t i = 0; i < MDP_ROWS_N; i++) {
                    const MdpRowT *r = &MDP_ROWS[i];
                    if (r->aset != AS[aset][a] || r->pcls != PC[pcls][c]
                        || r->side != (sd ? 1 - side : side))
                        continue;
                    int pr = CdPairOfMove(Fnv64Rt(r->pair));
                    if (pr < 0 || !CdPairLoaded(pr))
                        continue;
                    if (exact)
                        *exact = a == 0 && c == 0 && sd == 0;
                    return (int) i;
                }
    }
    return -1;
}

// (the same clash animation played even with a hammer and the right arm went behind the character; A [111480] 2h-blunt swing vs a hammer parrier and [113935] 2h-blunt swing vs FISTS both played
// mp-1h-blunt-right-1h-parry-sync: AS[] maps a 2h swinger onto the 1h-blunt swinger rows and PC[] maps an unarmed parrier onto the 1h-weapon parrier half,
// so the fist player's body played an NPC's 1h-blunt block with an empty hand - the right arm swings back holding nothing). SP has a parry pair only for
// the exact (swinger set, parrier class) combos of MDP_ROWS. Now a parry / clash plays only an EXACT row (either hand). A clash is started by the side
// whose swing is the SP swinger of such a row (the lower slot when both are; else the other one), and with no row for the two weapons there is no clash:
// both swings go on (each lands as its own blow). t2r-duel-parry-nearest.txt = the old nearest-row pick and lower-slot start.
static int MdpNearestOk(void)
{
    static int v = -1;
    static uint64_t at;
    uint64_t n = GetTickCount64();
    if (v < 0 || n - at > 2000) {
        at = n;
        v = GetFileAttributesA("t2r-duel-parry-nearest.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    return v;
}

static int MdpRowExact(int aset, int side, int pcls)
{
    for (int sd = 0; sd < 2; sd++)
        for (size_t i = 0; i < MDP_ROWS_N; i++) {
            const MdpRowT *r = &MDP_ROWS[i];
            if (r->aset != aset || r->pcls != pcls
                || r->side != (sd ? 1 - side : side))
                continue;
            int pr = CdPairOfMove(Fnv64Rt(r->pair));
            if (pr >= 0 && CdPairLoaded(pr))
                return (int) i;
        }
    return -1;
}

// ps28: the clinch-and-shove pair of every (swinger set, parrier set). Source families (SP DC npc-1h-parrys / npc-2h-parrys / npc-pistol-parries, all NPC halves base-female):
//   parrier class 1 = a 1h weapon (SP log: hammer, pipe wrench, machete, hatchet, 1h club all use npc-f-1hblunt-parry-*): vs fist, vs switchblade, vs 1h blunt
//   parrier class 2 = a 2h weapon (npc-f-2hblunt-parry-*): vs fist, vs switchblade; SP plays the switchblade pair for the player's 1h blade AND 1h blunt too
//   parrier class 4 = the pistol NPC (npc-f-pistol-parry-switchblade-*): vs switchblade only
// Substitutes (no SP clip; the parrier keeps what he holds, the swinger keeps his weapon):
//   unarmed parrier vs fists         -> class 1 vs fist (the weapon hand empty)            unarmed parrier vs any weapon -> class 4 vs switchblade (a hand on the weapon arm)
//   knife parrier                    -> class 1 (the knife is the 1h item)                 1h blade swinger vs class 1    -> the switchblade pair (as SP does vs class 2)
//   2h swinger                       -> the 1h blunt swinger's pair of the parrier's class
static int MdpRowFind(int aset, int side, int pcls)
{
    for (size_t i = 0; i < MDP_ROWS_N; i++) {
        const MdpRowT *r = &MDP_ROWS[i];
        if (r->aset != aset || r->pcls != pcls || r->side != side)
            continue;
        int pr = CdPairOfMove(Fnv64Rt(r->pair));
        if (pr >= 0 && CdPairLoaded(pr))
            return (int) i;
    }
    return -1;
}

static void MdpFam28(int aset, int pset, int *fa, int *fc, int *exact)
{
    int c =
        pset == MDU_2H ? 2 : pset == MDU_FIST ? (aset ==
                                                 MDU_FIST ? 1 : 4) : 1;
    int a = aset == MDU_FIST ? MDU_FIST : (aset == MDU_KNIFE
                                           || c == 4) ? MDU_KNIFE : aset ==
        MDU_1HBLADE ? (c == 2 ? MDU_1HBLADE : MDU_KNIFE) : MDU_1HBLUNT;
    *fa = a;
    *fc = c;
    *exact = a == aset && pset != MDU_FIST && pset != MDU_KNIFE;        // SP's own row for these two weapons
}

// the row for swinger row sw against a parrier holding set pset: the swing's hand, or the other hand's row when only that one starts within MDP_START_OFF_M of the
// bodies' distance d (d <= 0 / no handle: the hand's row). *auth = its authored start distance (0 = not read)
static int MdpRow28(int sw, int pset, float d, uint64_t h, int *exact,
                     float *auth)
{
    if (exact)
        *exact = 0;
    if (auth)
        *auth = 0.0f;
    if (sw < 0 || sw >= (int) MDU_MOVES_N || pset < 0 || pset >= MDU_NSET)
        return -1;
    const MduRowT *m = &MDU_MOVES[sw];
    int fa = 0, fc = 0, ex = 0, sd = MdpSide(m);
    MdpFam28(m->set, pset, &fa, &fc, &ex);
    int r0 = MdpRowFind(fa, sd, fc), r1 =
        MdpRowFind(fa, 1 - sd, fc), ri = r0 >= 0 ? r0 : r1;
    float a0 = 0.0f, a1 = 0.0f;
    if (h && d > 0.0f) {
        if (r0 >= 0)
            a0 = MduAuthDist(CdPairOfMove
                               (Fnv64Rt(MDP_ROWS[r0].pair)), h);
        if (r1 >= 0)
            a1 = MduAuthDist(CdPairOfMove
                               (Fnv64Rt(MDP_ROWS[r1].pair)), h);
        if (r0 >= 0 && r1 >= 0 && a0 > 0.0f && a1 > 0.0f
            && __builtin_fabsf(d - a0) > MDP_START_OFF_M
            && __builtin_fabsf(d - a1) <= MDP_START_OFF_M)
            ri = r1;
    }
    if (exact)
        *exact = ex && ri >= 0 && ri == r0;
    if (auth)
        *auth = ri < 0 ? 0.0f : ri == r0 ? a0 : a1;
    return ri;
}

// the SP row for swinger row sw (its set + hand) against a parrier holding set pset; -1 = SP has none
static int MdpRowFor(int sw, int pset)
{
    if (!MdpPs27())
        return MdpRow28(sw, pset, -1.0f, 0, NULL, NULL);       // ps28: every combination has a clinch pair (the hand's row or the other hand's)
    if (sw < 0 || sw >= (int) MDU_MOVES_N || pset < 0 || pset >= MDU_NSET)
        return -1;
    const MduRowT *m = &MDU_MOVES[sw];
    return MdpRowExact(m->set, MdpSide(m), MdpPcls(pset, m->set));
}

// who starts a clash of our swing row my_row with slot's swing row their_row: 1 we do, 0 they do, -1 no SP pair for these two weapons (no clash)
static int MdpClashWeStart(uint32_t slot, int my_row, int their_row)
{
    int lo = MexSelf() < slot;
    if (MdpNearestOk())
        return lo;
    if (my_row < 0 || my_row >= (int) MDU_MOVES_N || their_row < 0
        || their_row >= (int) MDU_MOVES_N)
        return -1;
    int mine = MdpRowFor(my_row, MDU_MOVES[their_row].set), theirs =
        MdpRowFor(their_row, MDU_MOVES[my_row].set);
    if (mine >= 0 && theirs >= 0)
        return lo;
    if (mine >= 0)
        return 1;
    if (theirs >= 0)
        return 0;
    // (where there is no pair for the weapons there is a both-unarmed clash: SP's fist-vs-fist pair, weapons kept in the hands;
    //  the lower slot starts it. t2r-duel-no-unarmed-clash.txt = no clash)
    if (GetFileAttributesA("t2r-duel-no-unarmed-clash.txt") ==
        INVALID_FILE_ATTRIBUTES && MdpRowExact(MDU_FIST, 1, 0) >= 0)
        return lo;
    return -1;
}

// ---- sounds (both screens): SP clash events when their bank is in the level, else the nearest hit sound
static void MdpCue(uint64_t at, uint64_t ent, int snd, int aset)
{
    for (int i = 0; i < 6; i++)
        if (!g_mdp.q[i].at) {
            g_mdp.q[i].at = at ? at : 1;
            g_mdp.q[i].ent = ent;
            g_mdp.q[i].snd = (int8_t) snd;
            g_mdp.q[i].aset = (int8_t) aset;
            return;
        }
}

static void MdpFire(int i)
{
    uint64_t ent = g_mdp.q[i].ent;
    int snd = g_mdp.q[i].snd, aset = g_mdp.q[i].aset;
    g_mdp.q[i].at = 0;
    if (!ent || ent == ~0ULL)
        return;
    const char *ev = snd == 0 ? "sfx-grab-face-skin" : (aset == MDU_1HBLUNT
                                                        || aset ==
                                                        MDU_2H) ?
        "sfx-weapon-club-1h-clash-hit" : "sfx-weapon-hatchet-clash-hit";
    if (EsfxPlay(ev, ent, "spined"))
        return;
    if (snd == 0) {
        g_mhrPunchThud = 1;
        MhrPunchAt(ent, 1, "duel parry");
        g_mhrPunchThud = 0;
    } else
        MeleeHitFx(ent, 0);
}

// ---- hooks
// v5-dmg: the clash rule in one place. since = the second press measured from the first swing's start (latency corrected), til = the first swing's contact left
// at that press, ow = one-way. A clash: |since| <= MDP_CLASH_BASE_MS, or |since| <= MDP_CLASH_MS with the first contact still MDP_CLASH_LEAD_MS ahead once crossed.
static int MdpClashOk(int since, int til, int ow)
{
    int a = since < 0 ? -since : since;
    if (a <= MDP_CLASH_BASE_MS)
        return 1;
    return a <= MDP_CLASH_MS && til - ow > MDP_CLASH_LEAD_MS;
}

// Clashes should not replace trades (25808 / 25822 both blows landed - a near-simultaneous pair 1.2 s after a clash, inside the
// clash cooldown): in the cooldown a near-simultaneous pair is no clash and no trade - the swing that STARTED first wins, the later one loses its blow (a tie
// inside 40 ms: the lower slot's). Both screens judge it from the same latency-corrected starts. t2r-duel-trades.txt = the old trade.
static uint64_t g_mdpBeatenUntil;
static int g_mdpBeatenSlot = -1;
// (both bodies raise the blade for the full wind-up; A.log [15110] 'CLASH cooldown: their swing started -7 ms
// before ours - FIRST WINS: ours loses its blow', yet [15129] 'finisher windup machete-slash-1-finisher ended at +390 ms of 341 (beaten ...)': the ruling was made at the
// SWING's arrival, the losing wind-up played on to its own contact time and was only voided there; on the winner's screen (B [17224] 'ours lands, theirs loses its blow')
// the loser's swing replica played on its puppet to its end). The ruling is the same on both screens (latency-corrected starts, tie -> lower slot); now it acts AT the
// ruling: the loser's own swing record ends (mdu_own_swing_cancel / mdu_swing_stop) and its engine swing is cancelled (cancel-melee-action: the engine's own blend back
// to locomotion - its length is the engine's, NOT measured to be 0.2 s), and the winner's screen stops the loser's swing replica on its puppet over MDP_LOSER_BLEND_S.
// The winner's blow lands within its own wind-up (their_tc_left) on a body that is out of its swing. t2r-duel-loser-cut-old.txt = old (the loser's wind-up plays out).
#define MDP_LOSER_BLEND_S 0.20f // [TUNING] s (an earlier value: 0.2 s)
static int MctCancelCarry44(uint32_t slot, const char *why);  // melee_duel_contact.c (included after this file)
static void MdpLoserCut44(uint32_t slot, int we_lost, int their_tc_left)
{
    static int old = -1;
    static uint64_t at;
    uint64_t n = GetTickCount64();
    if (old < 0 || n - at > 2000) {
        at = n;
        old =
            GetFileAttributesA("t2r-duel-loser-cut-old.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    if (old) {
        MDLOG
            ("DUEL: first-wins loser cut OFF (t2r-duel-loser-cut-old.txt) - the losing wind-up plays to its contact (ps44 T4)");
        return;
    }
    if (we_lost) {
        if (!g_mdu.on || !g_mdu.sw_on || g_mdu.sw_done
            || g_mdu.slot != (int) slot)
            return;
        int fin = g_mdfwu.on
            || g_mdu.sw_lethal, ms = (int) (n - g_mdu.sw_t0);
        uint64_t me0 = SID_PLAYER, cr =
            DcCall(0x394888944e949d5fULL /* cancel-melee-action */ , 1,
                    &me0);
        if (fin)
            MduOwnSwingCancel
                ("first wins: their swing started first - our wind-up ends at the ruling (ps44 T4)");
        else
            MduSwingStop
                ("first wins: their swing started first - our wind-up ends at the ruling (ps44 T4)");
        MDLOG
            ("DUEL: first-wins LOSER CUT: our %s%s ended at +%d ms at the ruling (cancel-melee-action -> %llx), slot %u's blow lands on us in ~%d ms (ps44 T4; t2r-duel-loser-cut-old.txt = old)",
             g_mdu.sw_row >= 0
             && g_mdu.sw_row <
             (int) MDU_MOVES_N ? MDU_MOVES[g_mdu.sw_row].move : "?",
             fin ? " (lethal / finisher wind-up)" : "", ms,
             (unsigned long long) cr, slot, their_tc_left);
        return;
    }
    g_mdpNosw44Slot = (int) slot;
    g_mdpNosw44Until = n + 50;        // (the SWING being handled: mdu_recv starts its replica right after this ruling - it is skipped there)
    // only THIS swing's replica (started at or after its start p_t0): a replica that started before it is the previous combo step's - left alone
    if (!DtlOn() || g_dtl.run.src != DTL_S_SWING || g_dtl.run.slot != slot
        || n >= g_dtl.run.end || !g_dtl.run.ent || !g_dtl.run.sid)
        return;
    if (g_dtl.run.t0 < g_mdp.p_t0) {
        MDLOG
            ("DUEL: first-wins LOSER CUT: slot %u's running replica %llx started %lld ms before this swing's start - the previous step's, left to play (ps44 T4)",
             slot, (unsigned long long) g_dtl.run.sid,
             (long long) (g_mdp.p_t0 - g_dtl.run.t0));
        return;
    }
    float bl = MDP_LOSER_BLEND_S;
    uint64_t a[5] = { g_dtl.run.ent, ANIM_LAYER, 0, g_dtl.run.sid, 0 };
    memcpy(&a[2], &bl, 4);
    uint64_t r = DcCall(FNV64("stop-animating"), 5, a);
    MDLOG
        ("DUEL: first-wins LOSER CUT: slot %u's swing replica %llx on its puppet stopped over %.2f s at the ruling -> %llx - our blow lands on a body out of its swing (ps44 T4)",
         slot, (unsigned long long) g_dtl.run.sid, (double) bl,
         (unsigned long long) r);
    g_dtl.run.end = n;
    g_dtl.run.cancel_at = n;
    int cc = MctCancelCarry44(slot, "first-wins loser cut (ps44 T4)");
    MDLOG
        ("DUEL: first-wins LOSER CUT: slot %u's contact carry %s (ps44 T4)",
         slot, cc ? "fades out at the ruling" : "was not running");
}

static void MdpFirstWins(uint32_t slot, int64_t they_minus_us,
                           uint64_t now, int their_tc_left)
{
    if (GetFileAttributesA("t2r-duel-trades.txt") !=
        INVALID_FILE_ATTRIBUTES)
        return;
    int we_win = they_minus_us > 40 || (they_minus_us >= -40
                                        && MexSelf() < slot);
    if (we_win) {
        MDLOG
            ("DUEL: CLASH cooldown: their swing started %lld ms after ours - FIRST WINS: ours lands, theirs loses its blow",
             (long long) they_minus_us);
        MdpLoserCut44(slot, 0, their_tc_left);
        return;
    }
    g_mdpBeatenSlot = (int) slot;
    g_mdpBeatenUntil =
        now + (uint64_t) (their_tc_left > 0 ? their_tc_left : 0) + 400;
    MDLOG
        ("DUEL: CLASH cooldown: their swing started %lld ms before ours - FIRST WINS: ours loses its blow (no trade)",
         (long long) -they_minus_us);
    MdpLoserCut44(slot, 1, their_tc_left);
}

static int MdpBeaten(int slot, uint64_t now)
{
    return slot == g_mdpBeatenSlot && now < g_mdpBeatenUntil;
}

// (two simultaneous swings of weapon sets with no SP clash pair both landed): a clash with no pair is a MUTUAL VOID - both sides void their own
// contact (mdu_contact), so no damage and no durability point either way; each swing's own tail plays on. t2r-duel-clash-all-old.txt = both blows go on.
static uint64_t g_mdpVoidUntil;
static int g_mdpVoidSlot = -1;
static int MdpClashVoid(int slot, uint64_t now)
{
    return slot == g_mdpVoidSlot && now < g_mdpVoidUntil;
}

// the higher slot: hold our contact while the lower slot's clash pair may still come
static void MdpExpect(int s, uint64_t now, const char *why)
{
    int ow = (int) PsyncOneway(s), ms = MDP_HOLD_MS + 2 * ow;
    if (ms > MDP_EXP_MAX_MS)
        ms = MDP_EXP_MAX_MS;
    g_mdp.exp_until = now + (uint64_t) ms;
    MDLOG
        ("DUEL: PARRY CLASH expected from the lower slot %d (%s) - our contact is held up to %d ms for its pair",
         s, why, ms);
}

// called from mdu_recv when the partner's SWING arrives: remember it; a swing of ours that started within the clash rule of theirs is a clash (lower slot starts it)
static void MdpNoteSwing(uint32_t slot, int row, int tc_ms, uint64_t now)
{
    if (row < 0 || row >= (int) MDU_MOVES_N || slot >= T2R_MAX_PEERS)
        return;
    g_mdp.p_row = row;
    g_mdp.p_slot = (int) slot;
    g_mdp.p_t0 = now - (uint64_t) PsyncOneway((int) slot);
    if (g_mdp.par_end && g_mdp.par_slot == (int) slot) {        // ps28: the parrier's first swing after it parried us
        if (now < g_mdp.par_end + MDP_FU_MS) {
            const char *db = MdgBusy();
            MDLOG
                ("DUEL: PARRY follow-up from slot %u (ps28): its swing %s reached us %+d ms from the parry pair's hand-off, contact on us in ~%d ms; our melee press is %s (%d ms left), our dodge is %s",
                 slot, MDU_MOVES[row].move,
                 (int) ((int64_t) now - (int64_t) g_mdp.par_end), tc_ms,
                 g_mduInitUntil >
                 now ? "still locked: dodge it" : "free again",
                 g_mduInitUntil >
                 now ? (int) (g_mduInitUntil - now) : 0,
                 db ? db : now <
                 g_mdg.cooldown_until ? "on its cooldown" : "available");
        }
        g_mdp.par_end = 0;
        g_mdp.par_slot = -1;
    }
    if (BqReady())
        return;                 // melee_beat.c: no local clash detection
    if (g_mdu.on && g_mdu.sw_on && g_mdp.my_t0 && g_mdu.slot == (int) slot
        && !g_mdp.pend && !g_mdu.pair_on) {
        int64_t dt = (int64_t) g_mdp.p_t0 - (int64_t) g_mdp.my_t0;
        int ow = (int) PsyncOneway((int) slot);
        // the FIRST swing's contact left now: ours (they started after us) or theirs (they started first: their contact = their start + tc on our clock)
        // ours first: we hold our own contact (the lower slot's pend / the higher's expect) - it only must not have passed (+ ow cancels the crossing term);
        // theirs first: our pair must reach them before their contact (their contact left now, minus the crossing in mdp_clash_ok)
        int til =
            dt >=
            0 ? (int) ((int64_t) g_mdu.sw_tc - (int64_t) now) +
            ow : (int) ((int64_t) g_mdp.p_t0 + tc_ms - (int64_t) now);
        int64_t sdt = dt;
        if (dt < 0)
            dt = -dt;
        if (dt <= MDP_CLASH_MS && now < g_mdp.clash_cool[slot])
            MdpFirstWins(slot, sdt, now,
                           (int) ((int64_t) g_mdp.p_t0 + tc_ms -
                                  (int64_t) now));
        if (dt <= MDP_CLASH_MS && MdpClashOk((int) dt, til, ow) && now >= g_mdp.clash_cool[slot]) {   // (spam: this path skipped the clash cooldown - a clash every 1.1 s)
            int ws = MdpClashWeStart(slot, g_mdp.my_row, row);
            if (ws < 0) {
                // (fists vs fists 130-320 ms apart = both void, both bodies swing through each other, nobody reacts = reads as a desync):
                // a clash with no SP pair is decided as the clash cooldown already decides it - FIRST WINS: the earlier swing lands with its normal -hit pair, the
                // later one loses its blow (no trade, no void). Both screens rule from the same two start times (tie: the lower slot). t2r-duel-clash-void.txt = the void.
                if (GetFileAttributesA("t2r-duel-clash-void.txt") ==
                    INVALID_FILE_ATTRIBUTES
                    && GetFileAttributesA("t2r-duel-clash-all-old.txt") ==
                    INVALID_FILE_ATTRIBUTES) {
                    g_mdp.clash_cool[slot] = now + 1200;
                    MDLOG
                        ("DUEL: CLASH with no SP pair (set %d vs set %d, their %s %d ms from ours): FIRST WINS",
                         g_mdp.my_row >=
                         0 ? MDU_MOVES[g_mdp.my_row].set : -1,
                         MDU_MOVES[row].set, MDU_MOVES[row].move,
                         (int) dt);
                    MdpFirstWins(slot, sdt, now,
                                   (int) ((int64_t) g_mdp.p_t0 + tc_ms -
                                          (int64_t) now));
                } else if (GetFileAttributesA("t2r-duel-clash-all-old.txt")
                           == INVALID_FILE_ATTRIBUTES) {
                    g_mdpVoidSlot = (int) slot;
                    g_mdpVoidUntil = now + 700;
                    g_mdp.clash_cool[slot] = now + 1200;
                    MDLOG
                        ("DUEL: CLASH VOID: their swing %s started %d ms from ours (set %d vs set %d: no SP clash / parry pair) - both contacts are void, no damage either way",
                         MDU_MOVES[row].move, (int) dt,
                         g_mdp.my_row >=
                         0 ? MDU_MOVES[g_mdp.my_row].set : -1,
                         MDU_MOVES[row].set);
                } else
                    MDLOG
                        ("DUEL: PARRY CLASH skipped: their swing %s started %d ms from ours, but SP has no clash / parry pair for set %d vs set %d - both blows go on",
                         MDU_MOVES[row].move, (int) dt,
                         g_mdp.my_row >=
                         0 ? MDU_MOVES[g_mdp.my_row].set : -1,
                         MDU_MOVES[row].set);
            } else if (ws) {
                MDLOG
                    ("DUEL: PARRY CLASH seen: their swing %s started %d ms from ours (latency corrected, one-way %d ms, first contact %d ms ahead) - our swing is the SP swinger (%s), we start it",
                     MDU_MOVES[row].move, (int) dt, ow, til - ow,
                     MexSelf() <
                     slot ? "lower slot" :
                     "their weapon has no row as the swinger");
                g_mdp.pend = 1;
                g_mdp.pend_kind = MDPK_CLASH;
                g_mdp.pend_bset = MDU_MOVES[row].set;
                g_mdp.pend_rel = (int) dt;
                g_mdp.pend_sw = g_mdp.my_row;
                g_mdp.pend_at = now;
                g_mdp.gate_tries = 0;
                g_mdp.pend_pr = -1;
            } else
                MdpExpect((int) slot, now, "their swing started inside the clash rule of ours");       // (ws == 0: their swing is the SP swinger, their pair comes)
        }
    }
}

// feature swing note (melee_duel.c mdu_swing_note): OUR melee press = our swing starts. If the partner's swing is running and the SP gate holds, claim the parry.
// THE TURN LOCK IS DECIDED IN THE ENGINE'S MOVE SEARCH (meleeenv.c me_cand_hook -> mdu_tl_veto below): the ps41 lock let the engine start our swing
// and cancelled it on its first frame (a one-frame twitch). The judge below (mdp_press2) is the old mdp_press; with dry = 1 it changes nothing, sends nothing,
// logs nothing and calls no engine function - its inputs (distance, facing, down state, the clash / parry rows of the two weapons) are cached by mdp_tl_inputs in
// the normal tick - and answers "this press would be NO claim and the partner's swing is registered here: their turn". The move search then refuses the candidate
// (the opener rule's way: nothing starts, nothing is cancelled). A press that IS a parry / clash claim is not refused: the engine starts the swing as before and
// mdp_press (dry = 0, from the tick's swing note) judges and sends the claim exactly as before - nothing is sent from inside the move search.
// t2r-turnlock-cancel-old.txt = the ps41 first-frame cancel; it also stays as the fallback (logged) for moves the search hook did not see.
static struct {
    uint64_t frame, key_t0;
    int key_row, key_s, pos, down;
    float d, dot;
    int8_t ws[MDU_MOVES_N], par[MDU_NSET];
} g_mdp_in = {.key_row = -1,.key_s = -1 };

static int MdpPress2(int r, uint64_t now, int dry);
static void MdpPress(int r, uint64_t now)
{
    MdpPress2(r, now, 0);
}

static int MdpPress2(int r, uint64_t now, int dry)
{
    if (!dry)
        g_mdp_tl = 0;
    if (r < 0 || r >= (int) MDU_MOVES_N)
        return 0;
    if (!dry) {
        g_mdp.my_row = r;
        g_mdp.my_t0 = now;
    }
    if (!dry && g_mdp.fu_until) {       // ps28: our first swing after our parry = the follow-up (an ordinary duel swing: wind-up, dodgeable, parryable)
        if (now < g_mdp.fu_until && g_mdu.slot == g_mdp.fu_slot) {
            MDLOG
                ("DUEL: PARRY follow-up (ps28): our swing %s starts %+d ms from the parry pair's hand-off on slot %d - an ordinary duel swing (contact in ~%d ms, its dodge window as always); its melee press comes back %d ms after the hand-off",
                 MDU_MOVES[r].move,
                 (int) ((int64_t) now - (int64_t) g_mdp.fu_from),
                 g_mdp.fu_slot, MDP_FU_CONTACT_MS, MDP_INIT28_MS);
            g_mdp.fu_sw = now;
        }
        g_mdp.fu_until = 0;
    }
    if (BqReady())
        return 0;               // melee_beat.c: the press is an ATTACK intent, the owner rules parry / clash
    if (!g_mdu.on || g_mdu.slot < 0 || g_mdp.p_slot != g_mdu.slot
        || g_mdp.p_row < 0 || g_mdp.pend || g_mdu.pair_on || (!dry
                                                              &&
                                                              MatchSubstate
                                                              () != 6))
        return 0;
    if (dry
        && (!g_mdp_in.frame || g_frame - g_mdp_in.frame > 2
            || g_mdp_in.key_s != g_mdu.slot
            || g_mdp_in.key_row != g_mdp.p_row
            || g_mdp_in.key_t0 != g_mdp.p_t0))
        return 0;               // (no fresh inputs: the fallback rules)
    int s = g_mdu.slot;
    const MduRowT *pm = &MDU_MOVES[g_mdp.p_row], *me = &MDU_MOVES[r];
    {                           // The dodge counter cannot be parried: the partner's swing inside its dodge-counter window on us (it dodged our
        // blow < MD56_DODGED_MS ago, melee_dodge.c) is its dodge counter - no parry / clash claim on it. t2r-counter-parry-ps50.txt = parryable again.
        static int cpar = -1;
        static uint64_t cpar_at;
        if (cpar < 0 || now - cpar_at > 3000) {
            cpar_at = now;
            cpar =
                GetFileAttributesA("t2r-counter-parry-ps50.txt") !=
                INVALID_FILE_ATTRIBUTES;
        }
        if (!cpar && Md56CounterFrom((uint32_t) s, now)) {
            static int ncp;
            if (!dry && LOGCAP(ncp, 60))
                MDLOG
                    ("DUEL: no parry - slot %d's swing is its dodge counter on us (it dodged our blow %llu ms ago): a counter cannot be parried",
                     s, (unsigned long long) (now - g_md56_dodged_ms[s]));
            return 0;
        }
    }
    int ptc = MduPartnerTc(s, g_mdp.p_row);   // (v5-finwind: a finisher windup's contact = its SP intro's end, not the row's)
    int since = (int) (int64_t) (now - g_mdp.p_t0), til = ptc - since;
    if (since > ptc + 100 || since < -MDP_CLASH_MS)
        return 0;               // no partner swing in progress
    if (!dry && since <= MDP_CLASH_MS && since <= ptc
        && now < g_mdp.clash_cool[s])
        MdpFirstWins((uint32_t) s, -(int64_t) since, now, ptc - since);       // (we pressed into their swing in the clash cooldown)
    float pb[16], mb[16];
    const float *pl = NULL, *ml = NULL;
    float d = 99.0f, dot = -1.0f;
    int have_pos, down;
    if (dry) {
        d = g_mdp_in.d;
        dot = g_mdp_in.dot;
        have_pos = g_mdp_in.pos;
        down = g_mdp_in.down;
    }                           // (the tick's cache: no engine call inside the move search)
    else {
        uint64_t h = PuppetHandle((uint32_t) s);
        pl = h ? PuppetLocator(h, pb) : NULL;
        ml = Uc4MyLoc(mb);
        if (pl && ml && __builtin_isfinite(pl[0])
            && __builtin_isfinite(ml[0])) {
            float dx = pl[0] - ml[0], dz = pl[2] - ml[2];
            d = __builtin_sqrtf(dx * dx + dz * dz);
            float fx = 2.0f * (ml[4] * ml[6] + ml[7] * ml[5]), fz =
                1.0f - 2.0f * (ml[4] * ml[4] + ml[5] * ml[5]), fl =
                __builtin_sqrtf(fx * fx + fz * fz);
            if (d > 0.05f && fl > 1e-3f)
                dot = (fx * dx + fz * dz) / (fl * d);
        }
        have_pos = pl && ml;
        down = g_downed || g_downDying || LocalHp() <= 0.0f;
    }
    int ow = (int) PsyncOneway(s), til1 = since >= 0 ? til : MduMyTc(r) + since;     // (the FIRST swing's contact left at the second press: theirs, or ours when we pressed first)
    int kind = MdpClashOk(since, til1, ow) ? MDPK_CLASH : MDPK_PARRY; // (v5-dmg: was since <= 220)
    int lb = MdpLb(ow), seen = til + ow, og = MdpOldGate();  // seen = the contact left as the parrier's screen showed it
    int hi50 = g_md50_narrow ? MdpLo(ow) + MD50_PARRY_W_MS : lb + (MDP_MAX_LEAD_MS - MDP_MIN_LEAD_MS); // ps50: the early edge under the dodger's priority (narrow)
    int late_clash = 0;
    if (!og && kind == MDPK_CLASH && since > MDP_CLASH_BASE_MS
        && seen > MdpLo(ow) && seen <= hi50) {
        kind = MDPK_PARRY;
        late_clash = 1;
    }                           // the parry window wins over the late clash zone
    const char *no = NULL;
    int ws = -1;
  judge:
    no = NULL;
    ws = -1;
    if (down)
        no = "we are down";
    else if (!have_pos)
        no = "no positions";
    else if (d > MDU_REACH + 0.3f)
        no = "out of reach";
    else if (dot < 0.0f)
        no = "not facing the attacker within 90 deg (SP melee-direction-check-locator-90s)";
    else if (kind == MDPK_PARRY && now < g_mdp.cool_until[s])
        no = "parry cooldown (SP 4 s)";
    else if (kind == MDPK_CLASH && now < g_mdp.clash_cool[s])
        no = "clash cooldown (3 s, no clash chains)";
    else if (kind == MDPK_PARRY && og && til - ow <= MDP_MIN_LEAD_MS)
        no = "too late: frames-til-hit <= 4 once the claim reaches the attacker";
    else if (kind == MDPK_PARRY && !og && seen <= MdpLo(ow))
        no = MwStrict()?
            "too late: the contact as we saw it was inside the latency-compensated gate (133 ms, claim must reach the attacker 30 ms before its contact)"
            :
            "too late: the contact as we saw it was already due (claim would reach the attacker after its contact; 11-08 looser late edge)";
    else if (kind == MDPK_PARRY && !og && seen > hi50)
        no = "too early: SP accepts the parry only 133-300 ms before the contact, this press is an ordinary swing";
    else if (kind == MDPK_PARRY && g_mdu.sw_on)
        no = "our own swing already runs (parry is a reaction)";
    if (!no && kind == MDPK_CLASH
        && (ws =
            dry ? g_mdp_in.ws[r] : MdpClashWeStart((uint32_t) s, r,
                                                      g_mdp.p_row)) < 0)
        no = "SP has no clash / parry pair for these two weapons (both blows go on)";
    if (!no && kind == MDPK_PARRY
        && (dry ? !g_mdp_in.par[me->set] : !MdpNearestOk()
            && MdpRowFor(g_mdp.p_row, me->set) < 0))
        no = "SP has no parry pair for their weapon against ours";
    // (clashes were not happening: fists vs fists, fists vs switchblade): both presses 229-243 ms apart passed the clash
    // rule, the late-clash-zone rule above turned them into a PARRY, and SP has no parry pair for those weapons - so neither a parry nor a clash, both blows went
    // on. A parry the late zone took from a clash that cannot run is the clash again. t2r-duel-late-clash-old.txt = old.
    if (no && late_clash && kind == MDPK_PARRY
        && GetFileAttributesA("t2r-duel-late-clash-old.txt") ==
        INVALID_FILE_ATTRIBUTES) {
        if (!dry)
            MDLOG
                ("DUEL: PARRY not possible (%s) - the press is inside the clash rule (%+d ms from their swing start): judged as the CLASH",
                 no, since);
        kind = MDPK_CLASH;
        late_clash = 0;
        goto judge;
    }
    // ps41 turn lock (melee_duel.c): their swing was registered here before this press (more than the one-way latency after its start, before its contact) and no
    // claim is possible - the caller cancels the swing this press started
    if (dry) {                  // the move search asks: 1 = no claim and their swing is registered here - their turn (the latch runs from now to their contact)
        if (!(no && since > ow && since <= ptc))
            return 0;
        g_mdp_tl_since = since;
        g_mdpTlNo = no;
        g_mdpTlUntil = now + (uint64_t) (ptc - since);
        return 1;
    }
    if (no && since > ow && since <= ptc) {
        g_mdp_tl = 1;
        g_mdp_tl_since = since;
        g_mdpTlNo = no;
        g_mdpTlUntil = now + (uint64_t) (ptc - since);
    }
    if (no) {
        MDLOG
            ("DUEL: PARRY no claim vs %s: %s (press %+d ms from their swing start, frames-til-hit %.1f, dist %.2f m, facing %.2f; contact as we saw it %d ms ahead, window %d..%d ms, one-way %d%s)",
             pm->move, no, since, (double) til * 0.03, (double) d,
             (double) dot, seen,
             og ? MDP_MIN_LEAD_MS + 2 * ow : MdpLo(ow),
             og ? MDP_MAX_LEAD_MS : lb + (MDP_MAX_LEAD_MS -
                                          MDP_MIN_LEAD_MS), ow,
             MwStrict()? ", strict" : ", loose");
        return 0;
    }
    uint8_t p[5] =
        { (uint8_t) kind, (uint8_t) me->set, (uint8_t) (since & 0xff),
 (uint8_t) ((since >> 8) & 0xff), (uint8_t) r };
    MduSend(MDPO_CLAIM, p, 5);
    g_mdp.claim_at = now;
    g_mdp.claim_kind = kind;
    MDLOG
        ("DUEL: PARRY %sclaim sent vs %s: our press %+d ms from their swing start (one-way %d ms already subtracted), frames-til-hit %.1f (gate > 4), dist %.2f m, facing %.2f; our set %d; contact as we saw it %d ms ahead (window %d..%d ms%s%s)",
         MDPK_TAG[kind], pm->move, since, ow, (double) til * 0.03,
         (double) d, (double) dot, me->set, seen,
         og ? MDP_MIN_LEAD_MS + 2 * ow : MdpLo(ow),
         og ? MDP_MAX_LEAD_MS : lb + (MDP_MAX_LEAD_MS - MDP_MIN_LEAD_MS),
         og ? ", OLD gate" : "", MwStrict()? ", strict" : ", loose");
    if (kind == MDPK_CLASH && ws == 1) {        // initiator (the SP swinger side): no round trip
        g_mdp.pend = 1;
        g_mdp.pend_kind = kind;
        g_mdp.pend_bset = pm->set;
        g_mdp.pend_rel = since;
        g_mdp.pend_sw = r;
        g_mdp.pend_at = now;
        g_mdp.gate_tries = 0;
        g_mdp.pend_pr = -1;
    } else if (kind == MDPK_CLASH)
        MdpExpect(s, now, "our clash claim");
    return 0;
}

// the tick (mdp_tick, every frame of an engaged duel): the dry judge's inputs while the partner's swing is in flight. The clash / parry row answers are read once
// per partner swing (they hash and look up the pair tables).
static void MdpTlInputs(uint64_t now)
{
    g_mdp_in.frame = 0;
    if (!g_mdu.on || g_mdu.slot < 0 || g_mdp.p_slot != g_mdu.slot
        || g_mdp.p_row < 0 || g_mdp.p_row >= (int) MDU_MOVES_N
        || !g_mdp.p_t0 || MatchSubstate() != 6 || !MduTurnlockOn()
        || MduTlCancelOld())
        return;
    int s = g_mdu.slot;
    if ((int64_t) (now - g_mdp.p_t0) >
        (int64_t) MduPartnerTc(s, g_mdp.p_row) + 100)
        return;
    uint64_t h = PuppetHandle((uint32_t) s);
    float pb[16], mb[16];
    const float *pl = h ? PuppetLocator(h, pb) : NULL, *ml =
        Uc4MyLoc(mb);
    g_mdp_in.d = 99.0f;
    g_mdp_in.dot = -1.0f;
    g_mdp_in.pos = pl && ml;
    if (pl && ml && __builtin_isfinite(pl[0]) && __builtin_isfinite(ml[0])) {
        float dx = pl[0] - ml[0], dz = pl[2] - ml[2];
        g_mdp_in.d = __builtin_sqrtf(dx * dx + dz * dz);
        float fx = 2.0f * (ml[4] * ml[6] + ml[7] * ml[5]), fz =
            1.0f - 2.0f * (ml[4] * ml[4] + ml[5] * ml[5]), fl =
            __builtin_sqrtf(fx * fx + fz * fz);
        if (g_mdp_in.d > 0.05f && fl > 1e-3f)
            g_mdp_in.dot = (fx * dx + fz * dz) / (fl * g_mdp_in.d);
    }
    g_mdp_in.down = g_downed || g_downDying || LocalHp() <= 0.0f;
    if (g_mdp_in.key_s != s || g_mdp_in.key_row != g_mdp.p_row
        || g_mdp_in.key_t0 != g_mdp.p_t0) {
        g_mdp_in.key_s = s;
        g_mdp_in.key_row = g_mdp.p_row;
        g_mdp_in.key_t0 = g_mdp.p_t0;
        for (int i = 0; i < (int) MDU_MOVES_N; i++)
            g_mdp_in.ws[i] =
                (int8_t) MdpClashWeStart((uint32_t) s, i, g_mdp.p_row);
        for (int ps = 0; ps < MDU_NSET; ps++)
            g_mdp_in.par[ps] = (int8_t) (MdpNearestOk()
                                         || MdpRowFor(g_mdp.p_row,
                                                        ps) >= 0);
    }
    g_mdp_in.frame = g_frame ? g_frame : 1;
}

// meleeenv.c me_cand_hook (inside the engine's move search): cheap pre-test - is the turn lock's question open for this candidate at all
static int MduTlCand(uint64_t aname)
{
    if (!aname || !g_mdu.on || g_mdu.slot < 0 || !g_mdp_in.frame
        || g_mdp.p_slot != g_mdu.slot || g_mdu.pair_on)
        return 0;
    return MduRowOf(aname) >= 0;
}

// meleeenv.c me_cand_hook, AFTER the engine's own check passed this duel row: 1 = answer "no" - the partner's swing is registered here and this press is no
// parry / clash claim: their turn, nothing starts. Not in the states where mdu_swing_note never reached mdp_press (run-in, pull-out, finisher windup, pin,
// dodge-counter window, MISS recovery, our own combo step / start).
static uint64_t g_mdpTlHookMs;
static int g_mdpTlHookN;
static int MduTlVeto(uint64_t aname)
{
    int r = MduRowOf(aname);
    uint64_t now = GetTickCount64();
    if (r < 0 || g_p31r.on || PulAttActive() || g_mdfwu.on
        || MpnHoldAtt() || DcArmedAt(now)
        || g_mdg.atk_lock_until > now)
        return 0;
    if (now - g_mduNextMs < 300 || now - g_p31.direct_ms < 300
        || (g_mdu.win_end > now && g_mdu.next_move))
        return 0;
    if (!MdpPress2(r, now, 1))
        return 0;
    static uint64_t lastf;
    if (lastf != g_frame) {     // (one line per press: the search may ask several rows in one frame; the latch keeps later presses out of the search)
        lastf = g_frame;
        g_mdpTlHookN++;
        MDLOG
            ("DUEL: press during slot %d's swing (+%d ms) - no swing of ours: their turn (parry not available: %s); dodge is open (%s refused in the move search - nothing started, nothing cancelled; melee presses off for %d ms; ps41b #%d; t2r-turnlock-cancel-old.txt = the first-frame cancel)",
             g_mdu.slot, g_mdp_tl_since, g_mdpTlNo ? g_mdpTlNo : "?",
             MDU_MOVES[r].move,
             g_mdpTlUntil > now ? (int) (g_mdpTlUntil - now) : 0,
             g_mdpTlHookN);
    }
    g_mdpTlHookMs = now;
    return 1;
}

// MSG_MEX op 5, op >= 6 (mdu_recv). 1 = handled
static int MdpRecv(uint32_t slot, int op, const uint8_t *buf,
                    uint64_t len)
{
    if (op != MDPO_CLAIM || len < 9 || slot >= T2R_MAX_PEERS || !MduOn())
        return 0;
    if (BqReady())
        return 1;               // melee_beat.c: claims are not used in beat mode
    uint64_t now = GetTickCount64();
    int kind = buf[4], bset = buf[5], rel =
        (int16_t) (buf[6] | (buf[7] << 8)), sw = -1;
    if (kind > MDPK_CLASH || bset >= MDU_NSET)
        return 1;
    const char *no = NULL;
    if (g_mdu.slot != (int) slot || MatchSubstate() != 6)
        no = "not in a duel with that slot";
    else if (g_downed || g_downDying || LocalHp() <= 0.0f)
        no = "we are down";
    else if (g_mdp.pend || g_mdu.pair_on || (PS_OURS && !(g_mdu.sw_on)))
        no = "another pair runs / is being placed";
    else if (kind == MDPK_CLASH
             && MdpClashWeStart(slot,
                                   g_mdu.sw_on ? g_mdu.sw_row : g_mdp.
                                   my_row, buf[8]) != 1)
        no = "clash: their swing is the SP swinger (or SP has no pair for these weapons) - their pair / no clash";
    else if (kind == MDPK_PARRY
             && now <
             (MdpPs27()? g_mdp.cool_until[slot] : g_mdp.cool_them[slot]))
        no = "parry cooldown (SP 4 s)";
    else if (kind == MDPK_CLASH && now < g_mdp.clash_cool[slot])
        no = "clash cooldown (3 s, no clash chains)";
    else if (kind == MDPK_PARRY && !g_mdu.sw_on)
        no = "our swing is not running (it already landed / ended)";
    else if (kind == MDPK_CLASH && !g_mdu.sw_on
             && !(g_mdp.my_t0 && now - g_mdp.my_t0 < 600))
        no = "no swing of ours near this clash";
    if (!no) {
        sw = g_mdu.sw_on ? g_mdu.sw_row : g_mdp.my_row;
        if (sw < 0)
            no = "no swing row";
        else if (kind == MDPK_PARRY) {
            int til = MduMyTc(sw) - rel, left =
                (int) (int64_t) (g_mdu.sw_tc - now);
            if (!MdpOldGate() && !MwStrict()) {
                if (left < 0)
                    no = "late: our contact already passed (11-08 looser late edge: a claim up to the contact itself)";
                else
                    MDLOG
                        ("DUEL: PARRY claim from slot %u accepted on our timeline (loose late edge): their press at +%d ms of our swing %s (tc %d ms), contact in %d ms (window ends at the contact)",
                         slot, rel, MDU_MOVES[sw].move,
                         MDU_MOVES[sw].tc_ms, left);
            }                   // (looser, tuning: the sender already judged the window)
            else if (til <=
                     MDP_MIN_LEAD_MS -
                     (MdpOldGate()? 0 : (int) PsyncOneway((int) slot)))
                no = "late: frames-til-hit <= 4 on our swing timeline"; // (the parrier saw the contact one-way later than our clock - the sender already judged its window, mdp_lb)
            else if (left < (MdpOldGate() || MwStrict()? 30 : 0))
                no = "late: our contact is due";
            else
                MDLOG
                    ("DUEL: PARRY claim from slot %u accepted on our timeline: their press at +%d ms of our swing %s (tc %d ms, one-way already subtracted by them), frames-til-hit %.1f > 4, contact in %d ms",
                     slot, rel, MDU_MOVES[sw].move, MDU_MOVES[sw].tc_ms,
                     (double) til * 0.03, left);
        }
    }
    if (no) {
        MDLOG
            ("DUEL: PARRY %sclaim from slot %u refused: %s (rel %+d ms, contact in %d ms, %s) - the blow goes on",
             MDPK_TAG[kind], slot, no, rel,
             g_mdu.sw_on ? (int) (int64_t) (g_mdu.sw_tc - now) : -9999,
             MwStrict()? "strict" : "loose");
        return 1;
    }
    g_mdp.pend = 1;
    g_mdp.pend_kind = kind;
    g_mdp.pend_bset = bset;
    g_mdp.pend_rel = rel;
    g_mdp.pend_sw = sw;
    g_mdp.pend_at = now;
    g_mdp.gate_tries = 0;
    g_mdp.pend_pr = -1;
    return 1;
}

// mdu_tick: hold our own contact while the pair is being placed
static int MdpHold(uint64_t now)
{
    if (g_mdp.pend && now < g_mdp.pend_at + MDP_HOLD_MS)
        return 1;
    return g_mdp.exp_until && now < g_mdp.exp_until && !g_mdu.pair_on;  // v5-dmg: the higher slot waits for the lower slot's clash pair
}

// victim side (mdu_victim_begin, a kind 7 pair of this table started on us): sounds from the pair's start, the cooldown
static void MdpVictimCues(uint64_t now, int skip_ms, int pr)
{
    int ri = MdpRowOfPair(pr);
    if (ri < 0)
        return;
    const MdpRowT *r = &MDP_ROWS[ri];
    uint64_t t0 = now - (uint64_t) skip_ms, ah =
        PuppetHandle((uint32_t) g_mdu.slot);
    int p28 = !MdpPs27();
    int snd = p28 && g_mdp.my_row >= 0 && g_mdp.my_row < (int) MDU_MOVES_N && MDU_MOVES[g_mdp.my_row].set == MDU_FIST ? 0 : r->snd;     // ps28: our empty hands meet the arm (no metal clash)
    int at = snd == 0 ? 20 : 70;
    MdpCue(t0 + (uint64_t) at, SID_PLAYER, snd, r->aset);
    MdpCue(t0 + (uint64_t) at, ah, snd, r->aset);
    int claimed = g_mdp.claim_at && now - g_mdp.claim_at < 1500;
    if (p28) {
        int end_ms = (int) (g_ps.kill_s * 1000.0f) - skip_ms;
        g_mdp.run_uid = g_ps.uid;
        g_mdp.run_role = 2;
        g_mdp.run_t0 = now;
        g_mdp.run_end_ms = end_ms;
        g_mdp.run_out_ms = end_ms - MDP_OUT_TAIL_MS;
        g_mdp.run_kind = claimed ? g_mdp.claim_kind : MDPK_CLASH;
        g_mdp.fu_until = 0;
        g_mdp.fu_sw = 0;
        if (claimed && g_mdp.claim_kind == MDPK_PARRY && g_mdu.slot >= 0) {
            g_mdp.fu_slot = g_mdu.slot;
            g_mdp.fu_from = now + (uint64_t) (end_ms > 0 ? end_ms : 0);
            g_mdp.fu_until = g_mdp.fu_from + MDP_FU_MS;
            MDLOG
                ("DUEL: PARRY ours (ps28): the clinch %s runs to its exit at %d ms (slot %d's half is released at %d ms, the hand-off frees us); then we have the initiative - its melee press stays locked %d ms past the hand-off, our swing started at once lands %d ms after it",
                 CD_PAIRS[pr].name, end_ms, g_mdu.slot,
                 end_ms - MDP_OUT_TAIL_MS, MDP_INIT28_MS,
                 MDP_FU_CONTACT_MS);
        }
    }
    g_mdp.exp_until = 0;        // (the expected clash came: no contact hold)
    if (claimed && g_mdp.claim_kind == MDPK_PARRY && g_mdu.slot >= 0)
        g_mdp.cool_until[g_mdu.slot] = now + MDP_COOL_MS;
    if (!(claimed && g_mdp.claim_kind == MDPK_PARRY) && g_mdu.slot >= 0)
        g_mdp.clash_cool[g_mdu.slot] = now + MDP_CLASH_COOL_MS; // (v5-dmg: also "their clash" - the lower slot's - so both sides keep the same cooldown)
    MDLOG
        ("DUEL: PARRY pair %s on us (parrier half %s) by slot %d: %s, phase offset %d ms, hand-off by the pair; sounds at +%d ms",
         CD_PAIRS[pr].name, CD_VIC0(pr), g_mdu.slot,
         claimed ? MDPK_NAME[g_mdp.claim_kind] : "their clash", skip_ms,
         at);
}

// ---- the initiator: place, path gate, start (every frame while pending)
static void MdpAbandon(const char *why)
{
    MDLOG("DUEL: PARRY %snot started: %s - the swing goes on",
          MDPK_TAG[g_mdp.pend_kind], why);
    if (g_mdp.pend_D0 > 0.0f && g_mdu.sw_on && !g_mdu.sw_done)
        g_mdu.sw_D = g_mdp.pend_D0;     // ps28: the swing lands at its own contact distance again
    g_mdp.pend_D0 = 0.0f;
    g_mdp.pend_auth = 0.0f;
    g_mdp.pend = 0;
}

static void MdpStartStep(uint64_t now)
{
    int s = g_mdu.slot;
    if (!MduOn() || s < 0 || !MduPartnerOk(s) || MatchSubstate() != 6) {
        MdpAbandon("the duel / partner is gone");
        return;
    }
    if (now > g_mdp.pend_at + 450) {
        MdpAbandon("timeout");
        return;
    }
    if (g_ps.role && g_ps.role != 3 && !(g_mdu.sw_on && !g_mdu.pair_on)) {
        MdpAbandon("another paired move runs");
        return;
    }
    const MduRowT *sm = &MDU_MOVES[g_mdp.pend_sw];
    int p28 = !MdpPs27() && !MdpNearestOk();
    uint64_t h = PuppetHandle((uint32_t) s);
    float vb[16], mb[16];
    const float *vl = h ? PuppetLocator(h, vb) : NULL, *me =
        PlayerLocator(mb);
    if (!vl || !me) {
        MdpAbandon("no positions");
        return;
    }
    if (g_mdp.pend_pr < 0 && p28) {
        // ps28: the clinch pair of the two weapons (mdp_row28), the hand by the bodies' distance; the swing's contact distance then eases onto the pair's start distance
        float dn =
            __builtin_sqrtf((vl[0] - me[0]) * (vl[0] - me[0]) +
                            (vl[2] - me[2]) * (vl[2] - me[2])), auth =
            0.0f;
        int exact = 0, fa = 0, fc = 0, fex = 0, ri =
            MdpRow28(g_mdp.pend_sw, g_mdp.pend_bset, dn, h, &exact,
                      &auth);
        if (ri < 0) {
            MdpAbandon
                ("no clinch pair is loaded for these two weapons (the SP parry paks are not in the level)");
            return;
        }
        MdpFam28(sm->set, g_mdp.pend_bset, &fa, &fc, &fex);
        g_mdp.pend_pr = CdPairOfMove(Fnv64Rt(MDP_ROWS[ri].pair));
        g_mdp.pend_mr = ri;
        g_mdp.pend_exact = exact;
        g_mdp.pend_auth = auth;
        g_mdp.appr_logged = 0;
        g_mdp.pend_D0 = g_mdu.sw_on && !g_mdu.sw_done ? g_mdu.sw_D : 0.0f;
        MDLOG
            ("DUEL: PARRY pick (ps28) for swing %s (set %d, %s hand) vs a parrier holding set %d: %s = SP family swinger set %d / parrier class %d (%s%s), clinch-and-shove only; authored start %.2f m, we are %.2f m apart; authored exits parrier %d / swinger %d ms",
             sm->move, sm->set, MdpSide(sm) ? "right" : "left",
             g_mdp.pend_bset, MDP_ROWS[ri].pair, fa, fc,
             exact ? "SP's own row for these weapons" : fex ?
             "SP's row, the other hand" : "substitute: closest class",
             MDP_ROWS[ri].side != MdpSide(sm) ? ", other hand's row" : "",
             (double) auth, (double) dn, MDP_ROWS[ri].px_ms,
             MDP_ROWS[ri].sx_ms);
    }
    if (g_mdp.pend_pr < 0) {
        int exact = 1, ri =
            MdpNearestOk()? MdpPick(sm->set, MdpSide(sm),
                                       MdpPcls(g_mdp.pend_bset, sm->set),
                                       &exact) : MdpRowFor(g_mdp.pend_sw,
                                                             g_mdp.
                                                             pend_bset);
        if (ri < 0 && !MdpNearestOk() && g_mdp.pend_kind == MDPK_CLASH
            && GetFileAttributesA("t2r-duel-no-unarmed-clash.txt") ==
            INVALID_FILE_ATTRIBUTES) {
            ri = MdpRowExact(MDU_FIST, MdpSide(sm), 0);
            exact = 0;
            if (ri >= 0)
                MDLOG
                    ("DUEL: PARRY CLASH: SP has no pair for set %d vs set %d - the unarmed clash %s plays, both weapons stay in the hands",
                     sm->set, g_mdp.pend_bset, MDP_ROWS[ri].pair);
        }
        if (ri < 0 && !MdpNearestOk()) {
            MdpAbandon
                ("SP has no parry / clash pair for our swing's set against their weapon");
            return;
        }
        if (ri < 0) {
            MdpAbandon
                ("no parry pair is loaded for this set (the SP parry paks are not in the level)");
            return;
        }
        g_mdp.pend_pr = CdPairOfMove(Fnv64Rt(MDP_ROWS[ri].pair));
        g_mdp.pend_mr = ri;
        MDLOG
            ("DUEL: PARRY pick for swing %s (set %d, %s hand) vs a parrier of class %d: %s (%s)",
             sm->move, sm->set, MdpSide(sm) ? "right" : "left",
             MdpPcls(g_mdp.pend_bset, sm->set), MDP_ROWS[ri].pair,
             exact ? "exact" : "nearest row");
    }
    int pr = g_mdp.pend_pr;
    float ap[7], da = 0.0f;
    if (!DcAnchor(pr, h, vl, me, 0.0f, ap, &da)) {
        MdpAbandon("no anchor");
        return;
    }
    const MdpRowT *row = &MDP_ROWS[g_mdp.pend_mr];
    float end_s = row->dur_ms / 1000.0f - 0.05f;
    if (end_s > MDP_END_S)
        end_s = MDP_END_S;
    if (end_s < 0.5f)
        end_s = 0.5f;
    if (p28) {
        // the exit point: the parrier's authored exit (its SP counter tell's frame), + the swinger half's release fade so the shove's stumble is not stopped dead
        end_s =
            (float) ((row->px_ms ? row->px_ms : MDP_PX_DEF_MS) +
                     MDP_OUT_TAIL_MS) / 1000.0f;
        if (end_s > row->dur_ms / 1000.0f - 0.03f)
            end_s = row->dur_ms / 1000.0f - 0.03f;
        if (end_s < 0.6f)
            end_s = 0.6f;
        // the approach: the swing's warp (mdu_warp, the body's one mover until the pair starts) lands on the pair's authored start distance
        if (g_mdp.pend_auth > 0.2f && g_mdu.sw_on && !g_mdu.sw_done) {
            float e = g_mdp.pend_auth - g_mdu.sw_D;
            g_mdu.sw_D +=
                e > MDP_APPR_STEP ? MDP_APPR_STEP : e <
                -MDP_APPR_STEP ? -MDP_APPR_STEP : e;
        }
    }
    {
        float vd = PsyncDur(h, Fnv64Rt(CD_VIC0(pr)));
        g_pgEndPh = vd > 0.2f ? end_s / vd : 0.0f;
    }
    int g =
        PgGate((uint32_t) s, CD_ATT0(pr), CD_VIC0(pr), ap, vl, me, 0,
                "melee duel parry");
    g_pgEndPh = 0.0f;
    if (g == 2) {
        if (++g_mdp.gate_tries > 24)
            MdpAbandon("path gate never answered");
        return;
    }
    if (g == 0) {
        MdpAbandon("path blocked");
        return;
    }
    if (p28 && da > MDP_APPR_OK_M && g_mdu.sw_on && !g_mdu.sw_done && now + 20 < g_mdu.sw_tc && now < g_mdp.pend_at + MDP_APPR_MS) {    // the approach still runs (never past the swing's contact)
        if (!g_mdp.appr_logged) {
            g_mdp.appr_logged = 1;
            MDLOG
                ("DUEL: PARRY approach (ps28): we are %.2f m off the pair's authored spot - the swing's warp closes onto its start distance %.2f m (was %.2f m) for up to %d ms / to one frame before the contact (%d ms away)",
                 (double) da, (double) g_mdp.pend_auth,
                 (double) g_mdp.pend_D0, MDP_APPR_MS,
                 (int) (int64_t) (g_mdu.sw_tc - now));
        }
        return;
    }
    uint64_t w = DcWeapon(), mel = w == FNV64("abby-unarmed") ? 0 : w;
    {
        uint64_t me0 = SID_PLAYER;
        DcCall(0x394888944e949d5fULL /* cancel-melee-action */ , 1, &me0);
    }
    g_psStartSkipMs = 0.0f;
    if (!PsyncStart
        (PS_CTXDOWN, (uint32_t) s, CD_ATT0(pr), CD_VIC0(pr), ap, end_s, 0,
         0xff, mel, Fnv64Rt(CD_PAIRS[pr].name))) {
        MdpAbandon("psync refused");
        return;
    }
    int kind = g_mdp.pend_kind;
    g_mdp.pend = 0;
    float appr_D0 = g_mdp.pend_D0;
    g_mdp.pend_D0 = 0.0f;
    g_cd.ended = 0;
    g_cd.plan_ok = 0;
    g_mdu.sw_on = 0;
    g_mdu.sw_done = 1;
    g_mdu.buf_at = 0;
    g_mdu.win_end = 0;
    g_mdu.next_move = 0;
    g_mdu.pair_on = 1;
    g_mdu.pair_role = 1;
    g_mdu.ps_uid = g_ps.uid;
    g_mdu.pair_row = pr;
    g_mdu.pair_ms = now;
    g_mdu.hit_ms = now;
    g_mdu.act_ms = now;
    g_mdu.n_blows++;
    CdHandsBegin(pr);
    g_visVicAnim = CD_VIC0(pr);
    g_vis_vic_puppet = h;
    g_vis_vic_frame = g_t60;
    g_mpose_att_frame = g_vis_vic_frame;
    g_mposeAttSlot = (int32_t) s;
    FinposBegin(1, s, 0, S60(end_s) + 60, "melee duel parry, swinger");
    // the parrier's initiative: our presses stay blocked MDP_INIT_MS past the hand-off (a clash gives none)
    int steady = kind == MDPK_PARRY && SkSteady() >= 1;        // Steady Stance I: the pushed attacker recovers at the hand-off (no head start for the parrier)
    int init_ms = kind == MDPK_PARRY
        && !steady ? (p28 ? MDP_INIT28_MS : MDP_INIT_MS) : 0, end_ms =
        (int) (end_s * 1000.0f);
    if (steady)
        Xlog("skills: STEADY STANCE %d: parried by slot %d - our melee button is free at the hand-off (no %d ms parrier head start)", SkSteady(), s, p28 ? MDP_INIT28_MS : MDP_INIT_MS);
    g_mdu.lock_until = now + (uint64_t) end_ms + (uint64_t) init_ms;
    PadinjBlock("melee-attack", g_mdu.lock_until);
    g_mduPlockUid = g_ps.uid;
    g_mduPlockEnd = now + (uint64_t) end_ms;
    if (kind == MDPK_PARRY) {
        if (!MdpPs27())
            g_mdp.cool_them[s] = now + MDP_COOL_MS;
        else
            g_mdp.cool_until[s] = now + MDP_COOL_MS;
    } else if (kind == MDPK_CLASH)
        g_mdp.clash_cool[s] = now + MDP_CLASH_COOL_MS;
    int snd = p28 && g_mdp.pend_bset == MDU_FIST ? 0 : row->snd;        // ps28: an unarmed parrier's hands meet the arm (no metal clash)
    int at = snd == 0 ? 20 : 70;
    MdpCue(now + (uint64_t) at, SID_PLAYER, snd, row->aset);
    MdpCue(now + (uint64_t) at, h, snd, row->aset);
    if (p28) {
        // the initiative as data: our melee is off (mdu_press_latched: pad and keyboard) to the hand-off + init_ms; the dodge is not locked (free from the hand-off)
        g_mduInitUntil = g_mdu.lock_until;
        g_mdp.run_uid = g_ps.uid;
        g_mdp.run_role = 1;
        g_mdp.run_t0 = now;
        g_mdp.run_end_ms = end_ms;
        g_mdp.run_out_ms = end_ms - MDP_OUT_TAIL_MS;
        g_mdp.run_kind = kind;
        g_mdp.par_end = kind == MDPK_PARRY ? now + (uint64_t) end_ms : 0;
        g_mdp.par_slot = kind == MDPK_PARRY ? s : -1;
        MDLOG
            ("DUEL: PARRY %sSTARTED (ps28) vs slot %d: clinch pair %s = %s on us + %s on them (%s), start %.2f m off our authored spot (approach: contact distance %.2f -> %.2f m); exit: our half is released at %d ms (0.25 s fade, the clip's root kept), hand-off %d ms (parrier's authored exit %d ms + %d); %s; no damage either way; decision: their press at %+d ms of the swing %s (tc %d), claim to start %d ms",
             MDPK_TAG[kind], s, CD_PAIRS[pr].name, CD_ATT0(pr),
             CD_VIC0(pr),
             g_mdp.pend_exact ? "SP's own row" : "substitute row",
             (double) da, (double) appr_D0, (double) g_mdu.sw_D,
             end_ms - MDP_OUT_TAIL_MS, end_ms, row->px_ms, MDP_OUT_TAIL_MS,
             kind ==
             MDPK_CLASH ?
             "a clash gives no initiative: our melee is back at the hand-off"
             : steady ? "Steady Stance: our melee is back at the hand-off"
             :
             "INITIATIVE to the parrier: our melee is off to the hand-off + 400 ms (dodge free from the hand-off)",
             g_mdp.pend_rel, sm->move, sm->tc_ms,
             (int) (now - g_mdp.pend_at));
        return;
    }
    MDLOG
        ("DUEL: PARRY %sSTARTED vs slot %d: pair %s = %s on us + %s on them (%d ms pair, %s, anchor residual %.2f m), hand-off %.2f s, our presses %s; no damage either way; decision: their press at %+d ms of the swing %s (tc %d), claim to start %d ms",
         MDPK_TAG[kind], s, CD_PAIRS[pr].name, CD_ATT0(pr), CD_VIC0(pr),
         row->dur_ms, row->mp ? "MP pak" : "SP pak added to the level",
         (double) da, (double) end_s,
         kind ==
         MDPK_PARRY ?
         "blocked +300 ms past the hand-off (the parrier's initiative)" :
         "free at the hand-off (a clash gives no initiative)",
         g_mdp.pend_rel, sm->move, sm->tc_ms, (int) (now - g_mdp.pend_at));
}

// ---- ps28 hooks
// ctxdown.c cd_sfx_arm (every client, once per move): when the swinger half of a parry / clash pair is released (psync_att_out) = the parrier's authored exit. 0 = not ours
static float MdpAttFreeMs(int pair)
{
    if (MdpPs27() || !MdpIsPair(pair) || !g_ps.role
        || g_ps.kind != PS_CTXDOWN)
        return 0.0f;
    float k = g_ps.kill_s * 1000.0f - (float) MDP_OUT_TAIL_MS;
    return k > 300.0f ? k : 0.0f;
}

// melee_duel.c mdu_miss_note: our swing was dodged - the follow-up's result
static void MdpFuMiss(const char *why)
{
    if (!g_mdp.fu_sw)
        return;
    MDLOG
        ("DUEL: PARRY follow-up (ps28): DODGED by slot %d %d ms after our swing's start (%s) - the back-and-forth goes on",
         g_mdp.fu_slot, (int) (GetTickCount64() - g_mdp.fu_sw),
         why ? why : "?");
    g_mdp.fu_sw = 0;
}

static int MduPartnerRowNow(uint32_t slot)
{                               // (melee_dodge.c pick): the duel row of slot's swing in flight (its SWING arrived < 1500 ms ago)
    uint64_t now = GetTickCount64();
    return g_mdp.p_slot == (int) slot && g_mdp.p_row >= 0 && g_mdp.p_t0
        && now - g_mdp.p_t0 < 1500 ? g_mdp.p_row : -1;
}

static void MdpTick(uint64_t now)
{
    MdpTlInputs(now);         // ps41b: the turn lock's judge inputs for the move search
    {
        static int logged;
        if (!logged) {
            logged = 1;
            Xlog("duel clash rule: both swings start within %d ms (latency corrected), or within %d ms with the first swing's contact still > %d ms ahead after the one-way; lower slot starts the pair, the higher holds its contact up to %d ms; cooldown %d ms", MDP_CLASH_BASE_MS, MDP_CLASH_MS, MDP_CLASH_LEAD_MS, MDP_EXP_MAX_MS, MDP_CLASH_COOL_MS);
        }
    }
    if (g_mdp.exp_until && now >= g_mdp.exp_until) {
        g_mdp.exp_until = 0;
        MDLOG
            ("DUEL: PARRY CLASH expected but no pair came - our contact goes on");
    }
    if (g_mdp.run_uid && !(g_ps.role && g_ps.uid == g_mdp.run_uid)) {   // ps28: the pair's exit, on both screens
        int el = (int) (now - g_mdp.run_t0);
        MDLOG
            ("DUEL: PARRY pair exit (ps28): the %s pair on us (%s half) is over after %d ms - planned hand-off %d ms, swinger half released at %d ms: %s",
             g_mdp.run_kind == MDPK_CLASH ? "clash" : "parry",
             g_mdp.run_role == 1 ? "swinger" : "parrier", el,
             g_mdp.run_end_ms, g_mdp.run_out_ms,
             el + 60 >=
             g_mdp.
             run_end_ms ? "played to its exit point" :
             "CUT before its exit point (an abort: see the psync ABORT line)");
        g_mdp.run_uid = 0;
    }
    if (g_mduInitUntil && now >= g_mduInitUntil) {
        g_mduInitUntil = 0;
        MDLOG
            ("DUEL: PARRY initiative lock over (ps28): our melee press is back (%d ms after the parry pair started on us)",
             g_mdp.run_t0 ? (int) (now - g_mdp.run_t0) : -1);
    }
    if (g_mdp.fu_sw) {
        if (g_mdu.pair_on && g_mdu.pair_role == 1
            && g_mdu.pair_ms >= g_mdp.fu_sw) {
            MDLOG
                ("DUEL: PARRY follow-up (ps28): %s - our pair %s on slot %d started %d ms after the swing's start",
                 MdpIsPair(g_mdu.
                             pair_row) ? "PARRIED / CLASHED in turn" :
                 "HIT",
                 g_mdu.pair_row >= 0 ? CD_PAIRS[g_mdu.pair_row].name : "?",
                 g_mdp.fu_slot, (int) (g_mdu.pair_ms - g_mdp.fu_sw));
            g_mdp.fu_sw = 0;
        } else if (now > g_mdp.fu_sw + 1200) {
            MDLOG
                ("DUEL: PARRY follow-up (ps28): no hit pair and no dodge within 1200 ms of the swing's start (a whiff / out of reach / parried in turn)");
            g_mdp.fu_sw = 0;
        }
    }
    for (int i = 0; i < 6; i++)
        if (g_mdp.q[i].at && now >= g_mdp.q[i].at)
            MdpFire(i);
    if (g_mdp.pend)
        MdpStartStep(now);
}
