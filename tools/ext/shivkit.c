// 2013 shiv executions: TLOU2 SP's own knife-kill clip events (Ellie's knife kills), fired as "kits" at the 2013 contact times. Included after clipfx.c.
// Goal: a throat cut with blood, choking, grunts and the slice sound taken straight from SP (Ellie's throat slash sound, all of it mixed). tools/shivkit_gen.py -> shiv_kit.h holds every sound / part-effect / voice event of
// one beat of a donor clip (STAB, PULL, DEEP, THROAT, DEATH), timed from the beat's contact frame. Each 2013 shiv move gets a plan of kits at its own
// stab / gore times (exsfx2013.h rows, the ground shiv's blade frames). The events play exactly as in the contextual takedowns: play-sound-at-joint
// (bank wpn-melee-switchblade, an MP level add), clipfx_fire (part-effects with every knob), say (the speaker's voice; a female look: her take).
// While a plan runs, the 2013 hand-mixed shiv PCM cues (EXS_SHIV_STAB / _GORE, the ground shiv's knife cues) stay off. t2r-no-shiv-kit.txt = old cues.
#include "shiv_kit.h"
typedef struct {
    int kit, role, ms, kind;
    uint64_t sid;
    const char *name;
    uint64_t joint;
    const char *jn;
    float frame;
    int n;
    const uint64_t(*pp)[2];
    const char *clip;
    int vk;
} SkRow;
static const SkRow SK_ROWS[] = { SHIV_KIT_DATA };

// Shiv executions sounded like slashes, not like Ellie's stab / pull-out thunks, and characters barely vocalized: the generated kit voices one stab sound per move
// and the pull-out only at the death. Every stab / deep stab of every shiv move now also gets (shiv audio:) TLOU2's own ripped Ellie knife cues through the PCM mixer
// (BC_K_STAB / BC_K_STAB_DEEP flesh thunk on the victim - audible whatever banks the MP level has; kind 3), the blade coming back out (BC_K_EXTRACT, before the next
// stab / after the last), the attacker's exertion on the stab and the victim's pain between stabs (`say`, each speaker's own voice set, a female look her own take).
// The rows follow the generated ones (index SHIV_KIT_N + i); kind 3 = PCM cue vk, kind 2 = voice (vk 0 effort / 1 pain). t2r-no-shiv-pcm.txt = the PCM thunks off.
enum { XR_STAB, XR_DEEP, XR_PULL, XR_EFFORT, XR_PAIN_A, XR_PAIN_B, XR_N };
static const SkRow SK_XR[XR_N] = {
    { SK_STAB, 1, 0, 3, 0, "pcm-stab", 0, "", 0, 0, NULL, NULL,
     BC_K_STAB },
    { SK_DEEP, 1, 0, 3, 0, "pcm-stab-deep", 0, "", 0, 0, NULL, NULL,
     BC_K_STAB_DEEP },
    { SK_PULL, 1, 0, 3, 0, "pcm-pull-out", 0, "", 0, 0, NULL, NULL,
     BC_K_EXTRACT },
    { SK_STAB, 0, 0, 2, 0, "vox-eff-gen-fight-give-med", 0, "", 0, 0, NULL,
     NULL, 0 },
    { SK_STAB, 1, 0, 2, 0, "vox-eff-gen-fight-take-med-shrp", 0, "", 0, 0, NULL, NULL, 1 },     // (the -gut takes are indexed but have no streams in speech1/english.psarc: silent)
    { SK_STAB, 1, 0, 2, 0, "vox-eff-gen-fight-take-sml-shrp", 0, "", 0, 0,
     NULL, NULL, 1 },
};

static const SkRow *SkRowp(int r)
{
    return r <
        (int) SHIV_KIT_N ? &SK_ROWS[r] : &SK_XR[r - (int) SHIV_KIT_N];
}

static int ShivkitPcmOff(void)
{
    static int c = -1;
    static uint64_t k;
    return CfxeFile("t2r-no-shiv-pcm.txt", &c, &k);
}

static CfxeEntry g_skEnt[SHIV_KIT_N], g_sk_self[SHIV_KIT_N];  // persistent event entries (FireEffect keeps the pointer in its trackers)
// the beats of each 2013 shiv move: {move, kit, contact ms from the move start}. Stabs = STAB, the neck slash = THROAT (the moves that cut the neck:
// bloodbuild.c bb_shiv_neck), the chest / belly finisher = DEEP; DEATH (gurgle, cough, pool, the blade out at +333) follows the final blow.
static const struct {
    const char *move;
    int kit;
    uint16_t ms;
} SK_PLAN[] = {
    { "fist-insta-shiv-stand-front", SK_STAB, 300 },
        { "fist-insta-shiv-stand-front", SK_STAB, 530 },
        { "fist-insta-shiv-stand-front", SK_STAB, 820 },
    { "fist-insta-shiv-stand-front", SK_STAB, 1100 },
        { "fist-insta-shiv-stand-front", SK_DEEP, 1250 },
        { "fist-insta-shiv-stand-front", SK_DEATH, 1250 },
    { "fist-insta-shiv-stand-back", SK_STAB, 230 },
        { "fist-insta-shiv-stand-back", SK_STAB, 630 },
        { "fist-insta-shiv-stand-back", SK_STAB, 1000 },
    { "fist-insta-shiv-stand-back", SK_THROAT, 1100 },
        { "fist-insta-shiv-stand-back", SK_STAB, 1330 },
        { "fist-insta-shiv-stand-back", SK_DEATH, 1330 },
    { "shiv-stealth-back", SK_STAB, 300 }, { "shiv-stealth-back",
                                            SK_THROAT, 360 },
        { "shiv-stealth-back", SK_DEATH, 360 },
    { "shiv-stealth-front", SK_STAB, 540 }, { "shiv-stealth-front",
                                             SK_DEEP, 600 },
        { "shiv-stealth-front", SK_DEATH, 600 },
    { "shiv-ground-execution-left", SK_STAB, 433 },
        { "shiv-ground-execution-left", SK_STAB, 1233 },
        { "shiv-ground-execution-left", SK_STAB, 1633 },
    { "shiv-ground-execution-left", SK_DEEP, 1900 },
        { "shiv-ground-execution-left", SK_STAB, 2200 },
        { "shiv-ground-execution-left", SK_STAB, 2700 },
    { "shiv-ground-execution-left", SK_DEEP, 3133 },
        { "shiv-ground-execution-left", SK_DEATH, 3133 },
    { "shiv-ground-execution-right", SK_THROAT, 2200 },
        { "shiv-ground-execution-right", SK_DEATH, 2200 },
    // The 2013 SHIV INTERROGATIONS (MX_INT_SHIV): the blow is the victim half's head blood burst at the kill frame (back f77 = 2.55 s, front-dup f68 = 2.25 s); a stab just before it
    { "shiv-interrogate-enter-back", SK_STAB, 2450 },
        { "shiv-interrogate-enter-back", SK_THROAT, 2550 },
        { "shiv-interrogate-enter-back", SK_DEATH, 2550 },
    { "shiv-interrogate-enter-front-dup", SK_STAB, 2150 },
        { "shiv-interrogate-enter-front-dup", SK_DEEP, 2250 },
        { "shiv-interrogate-enter-front-dup", SK_DEATH, 2250 },
};

// The moment the MP attacker clip really pulls the blade out (ms from the move start), measured from the baked 2013 clip: the weapon-hand prop's distance to the
// victim's neck / spine / pelvis (re/factions2013/t1anim decoded poses in the pair's align space, 30 fps) stays ~0.5 m (blade in) from the stab until it jumps to > 1.2 m
// within 4 frames. shiv-stealth-front (Joel's atk-shiv-fw-finisher, right hand): stab in at 0.50-0.57 s, held, out at 1.57-1.60 (clear 1.70 s). The SP death kit puts the
// extract (f61 blood-ribbon stream on the blade, f62 extract-large, the victim's pulse / cough / spray) 333 ms after ITS anchor = the kill (600 ms): 967 ms, 0.63 s before the
// blade actually leaves the body. Re-measured for the others the same way (blade-in = hand prop within ~0.25 m of the victim's neck / head / spine, out = the last frame
// before the distance rises past +0.3 m): shiv-stealth-back in 0.40-1.45 s, out 1.40-1.50 (extract was at 727 ms); shiv-ground-execution-right: the throat cut swipe ends
// 2.20-2.30 s (blade away by 2.4; extract was 2567 ms; a later hand contact 2.8-3.3 s is the head, not the blade); fist-insta-shiv-stand-front: deep stab 1.40, out 1.55-1.70
// (extract 1617). fist-insta-shiv-stand-back (min 0.23 m, no clean in / out) and the ground left (many stabs) stay with the SP timing. For a move listed here every DEATH row from +333 on (the whole pull-out part) is moved so +333 lands on the listed time.
static const struct {
    const char *move;
    uint16_t ms;
} SK_PULL_AT[] =
    { { "shiv-stealth-front", 1580 }, { "shiv-stealth-back", 1400 },
    { "shiv-ground-execution-right", 2300 },
    { "fist-insta-shiv-stand-front", 1550 } };
#define SK_MAX_EV 160
static struct {
    double t0;
    int pair, n, i, fired, failed;
    uint64_t vox_ms[2];
    uint8_t vox_tier[2];        // per speaker (0 attacker, 1 victim): the line playing now and its tier (see sk_vox_tier)
    int neck;                   // the move cuts the throat (a THROAT beat): the victim's blood goes to the neck (sk_neck_entry)
    struct {
        uint16_t ms;
        uint8_t role, done, tries;
        int16_t row;
    } ev[SK_MAX_EV];
} g_sk = { -1.0, -1 };

// The blood appeared on the mouth instead of the throat in the shiv cut throat animations: the SP donor rows put the victim's blood
// masks / bursts at the joints of the SP clip - the npc's own hand props (her hand clutching the throat in ellie-knife-kill-rt-ft-neck-tear) and
// headb (the gut-to-neck death's mouth bleed). On a 2013 throat cut the victim's hands are elsewhere (up by the face), so the masks painted the
// mouth. In a throat move every victim blood fx at a hand prop or headb is re-aimed at the neck joint; the blood cough (a mouth particle that
// leaves no mask) and the floor pool keep theirs. The remapped event is a persistent copy (FireEffect keeps the pointer).
#define SK_J_NECK 0xe58d5bbaa07431f2ULL
#define SK_NECK_PP 48
static CfxeEntry g_skNeck[SHIV_KIT_N], g_sk_neck_self[SHIV_KIT_N];    // (+ the actor-less copy for a puppet victim, like g_sk_self)
static uint64_t g_skNeckPp[SHIV_KIT_N][SK_NECK_PP][2];
// SP's own throat wound mask (the victim row of ellie-1hblade-hwall-kill-left-neck-chop--npc-f, ctx_fx_events.h CTX_FXP_499): bm-knife-wound-cloth stamped at
// "neck" - the effect id, the joint and every param carry the neck joint's id (top 32 bits of each value). The mouth mask (bm-ellie-melee-mouth) is its own
// effect id whose art IS the mouth region: re-aiming its joint cannot move it, so on a throat move it is swapped for this neck wound.
static const uint64_t SK_NECK_WOUND[][2] =
    { { 0xc4bcadba8e631b86ULL, 0xbc7b6e36b713281fULL },
    { 0x52e7fc8056bc65d7ULL, 0xe58d5bbaa07431f2ULL },
    { 0xdec598a6c4eb90bcULL, 0xe58d5bba3f800000ULL },
    { 0xcb7b3b3576905b9cULL, 0xe58d5bbabdb851ecULL },
    { 0xcb7b3c3576905d4fULL, 0xe58d5bbabcf5c28fULL },
    { 0xcb7b3d3576905f02ULL, 0xe58d5bba3d8f5c29ULL },
    { 0x401ab8cd06158638ULL, 0xe58d5bba43340000ULL },
    { 0x02751f65f27faaa0ULL, 0xe58d5bba00000000ULL },
    { 0x52b4569b59de042eULL, 0xe58d5bba3f800000ULL },
    { 0xea11573f1af59eb5ULL, 0xe58d5bba3ecccccdULL },
    { 0xed7a438e35fb0264ULL, 0xe58d5bba3f000000ULL },
    { 0x8c30080da895c1f4ULL, 0xe58d5bba3eb851ecULL },
    { 0x8c300b0da895c70dULL, 0xe58d5bba3f3ae148ULL },
    { 0x13b672f7c938286fULL, 0xe58d5bba3f800000ULL },
    { 0x273bd346991fed73ULL, 0xe58d5bba42ea0000ULL },
    { 0x273bd246991febc0ULL, 0xe58d5bba43020000ULL },
    { 0x273bd546991ff0d9ULL, 0xe58d5bba42e80000ULL } };
#define SK_NECK_HI 0xe58d5bbaULL
static const CfxeEntry *SkNeckEntry(int row, const CfxeEntry *ev)
{
    const SkRow *R = &SK_ROWS[row];
    if (!strcmp(R->name, "blood-cough")
        || !strcmp(R->name, "blood-splatter-pool-eff"))
        return ev;
    if (R->n <= 0 || R->n > SK_NECK_PP || !R->pp)
        return ev;
    if (!g_skNeck[row].type) {
        int hit = 0;
        if (!strcmp(R->name, "bm-ellie-melee-mouth") || (!strcmp(R->name, "bm-knife-wound-cloth") && R->n > 1 && !R->pp[1][1])) {       // (+ the throat beat's jointless wound row: zero offsets, no joint = no place on the neck)      // the throat slash still landed on the mouth: the death kit's mouth mask
            g_skNeck[row] = *ev;
            g_skNeck[row].pairs = SK_NECK_WOUND;
            g_skNeck[row].n =
                (uint32_t) (sizeof SK_NECK_WOUND /
                            sizeof SK_NECK_WOUND[0]);
            static int nlm;
            if (LOGCAP(nlm, 30))
                Xlog("shiv kit: %s (victim) swapped for SP's neck wound mask (bm-knife-wound-cloth at neck) on throat moves", R->name);
            return &g_skNeck[row];
        }
        for (int i = 0; i < R->n; i++) {
            uint64_t v = R->pp[i][1];
            uint64_t hi = v >> 32;
            // the donor's joint-bound values: pair 1 = the joint's full id, every other param = the joint's top 32 bits + a float. Both are re-aimed (the old code
            // moved only the full id and left the params tagged with the hand / head joint)
            if (v == 0x8b29524672c308d3ULL /* headb */ 
                || v == 0xa139f2b7015119a7ULL /* l_hand_prop */ 
                || v == 0x8bf9bd94ce265a0dULL /* r_hand_prop */ ) {
                v = SK_J_NECK;
                hit++;
            } else if (i > 0
                       && (hi == 0x8b295246ULL || hi == 0xa139f2b7ULL
                           || hi == 0x8bf9bd94ULL)) {
                v = (SK_NECK_HI << 32) | (v & 0xffffffffULL);
                hit++;
            }
            g_skNeckPp[row][i][0] = R->pp[i][0];
            g_skNeckPp[row][i][1] = v;
        }
        g_skNeck[row] = *ev;
        g_skNeck[row].pairs = (const uint64_t(*)[2]) g_skNeckPp[row];
        static int nl;
        if (LOGCAP(nl, 30))
            Xlog("shiv kit: %s (victim) re-aimed at the neck for throat moves (%d joint params)", R->name, hit);
    }
    return &g_skNeck[row];
}

static int ShivkitOff(void)
{
    static int c = -1;
    static uint64_t k;
    return CfxeFile("t2r-no-shiv-kit.txt", &c, &k);
}

// 1 = a kit plan runs for this execution (the caller keeps the hand-mixed shiv cues off)
static int ShivkitActive(void)
{
    return g_sk.pair >= 0 && g_sk.n > 0;
}

static void SkPush(int ms, int row)
{
    if (ms < 0 || g_sk.n >= SK_MAX_EV)
        return;
    int j = g_sk.n++;
    g_sk.ev[j].ms = (uint16_t) ms;
    g_sk.ev[j].role = (uint8_t) SkRowp(row)->role;
    g_sk.ev[j].row = (int16_t) row;
}

// 2 = choke / gurgle / throat (the dying sounds: nothing smaller after them), 1 = grunts, efforts, pains, stab yelps
static int SkVoxTier(const char *n)
{
    return n && (strstr(n, "gurgle") || strstr(n, "throat")
                 || strstr(n, "choke") || strstr(n, "-interrupt")) ? 2 : 1;
}

static void ShivkitArm(int pair, double t0, const char *move)
{
    memset(&g_sk, 0, sizeof g_sk);
    g_sk.t0 = t0;
    g_sk.pair = pair;
    g_epVicDying = 0;
    if (ShivkitOff() || !move)
        return;
    int voiced[SK_KIT_N] = { 0 };
    int pull_at = 0;
    for (size_t q = 0; q < sizeof SK_PULL_AT / sizeof SK_PULL_AT[0]; q++)
        if (!strcmp(SK_PULL_AT[q].move, move))
            pull_at = SK_PULL_AT[q].ms;
    int first_hit = 100000, n_stab = 0, stab_ms = 0;    // the move's first blade contact; single-stab moves (stealth front / back)
    for (size_t p = 0; p < sizeof SK_PLAN / sizeof SK_PLAN[0]; p++)
        if (!strcmp(SK_PLAN[p].move, move) && SK_PLAN[p].kit != SK_DEATH) {
            if (SK_PLAN[p].ms < first_hit)
                first_hit = SK_PLAN[p].ms;
            if (SK_PLAN[p].kit == SK_STAB) {
                n_stab++;
                stab_ms = SK_PLAN[p].ms;
            }
        }
    for (size_t p = 0; p < sizeof SK_PLAN / sizeof SK_PLAN[0]; p++) {
        if (strcmp(SK_PLAN[p].move, move))
            continue;
        int kit = SK_PLAN[p].kit, vo = voiced[kit]++;
        if (kit == SK_THROAT)
            g_sk.neck = 1;
        for (size_t r = 0; r < SHIV_KIT_N && g_sk.n < SK_MAX_EV; r++) {
            if (SK_ROWS[r].kit != kit)
                continue;
            if (SK_ROWS[r].kind == 2 && vo)
                continue;       // a repeated beat (the 3rd / 4th stab): its voice once, not on every blow
            int m = (int) SK_PLAN[p].ms + SK_ROWS[r].ms;
            if (kit == SK_DEATH && SK_ROWS[r].ms >= 333
                && pull_at >= (int) SK_PLAN[p].ms + 60)
                m += pull_at - ((int) SK_PLAN[p].ms + 333);     // the pull-out part: when the blade really leaves
            // the victim's choke / gurgle never comes before the blow it belongs to (the SP lead-ins are timed for Ellie's long kills;
            // in a short 2013 move they put the choke ahead of the stab)
            if (SK_ROWS[r].kind == 2 && SK_ROWS[r].role == 1
                && SkVoxTier(SK_ROWS[r].name) == 2
                && m < (int) SK_PLAN[p].ms)
                m = (int) SK_PLAN[p].ms;
            if (m < 0)
                continue;       // a lead-in longer than the 2013 move: what would play before its start
            // The back1stabs sounded like just pulling out a knife, no stab: no blade-in-flesh sound before the first contact
            // (the SP throat kit's grind-push lead-in landed before the stab), and in a one-stab move the throat tear waits until the
            // stab has rung out (>= 160 ms after it) so the stab is heard first
            if (SK_ROWS[r].kind == 0 && m < first_hit)
                continue;
            if (SK_ROWS[r].kind == 0 && n_stab == 1
                && (kit == SK_THROAT || kit == SK_DEATH)
                && m < stab_ms + 160)
                m = stab_ms + 160;
            int j = g_sk.n++;
            g_sk.ev[j].ms = (uint16_t) m;
            g_sk.ev[j].role = (uint8_t) SK_ROWS[r].role;
            g_sk.ev[j].row = (int16_t) r;
            if (SK_ROWS[r].kind == 1 && !g_skEnt[r].type) {
                g_skEnt[r].frame = SK_ROWS[r].frame;
                g_skEnt[r].n = (uint32_t) SK_ROWS[r].n;
                g_skEnt[r].pairs = SK_ROWS[r].pp;
                g_skEnt[r].type = CFXE_TYPE;
            }
        }
    }
    if (g_sk.n) {               // the extra stab / pull-out / voice layers (see SK_XR), from the move's own beats
        int bm[16], bk[16], nb = 0, last = 0, firststab = 1, stabs =
            0, pulls = 0, pains = 0, efforts = 0;
        for (size_t p = 0;
             p < sizeof SK_PLAN / sizeof SK_PLAN[0] && nb < 16; p++)
            if (!strcmp(SK_PLAN[p].move, move)
                && SK_PLAN[p].kit != SK_DEATH) {
                bm[nb] = SK_PLAN[p].ms;
                bk[nb++] = SK_PLAN[p].kit;
                if (SK_PLAN[p].ms > last)
                    last = SK_PLAN[p].ms;
            }
        int pcm = !ShivkitPcmOff() && BladePcmOn();
        for (int b = 0; b < nb; b++) {
            int kit = bk[b], ms = bm[b], fin = ms >= last;
            if (kit != SK_STAB && kit != SK_DEEP)
                continue;
            int flam = 0;       // a push-in 60 ms after the stab (stealth front) is the same blow: one thunk
            for (int c = 0; c < nb; c++)
                if (bm[c] < ms && ms - bm[c] < 120
                    && (bk[c] == SK_STAB || bk[c] == SK_DEEP))
                    flam = 1;
            if (pcm && !flam) {
                SkPush(ms,
                        SHIV_KIT_N + (kit == SK_DEEP
                                      || n_stab == 1 ? XR_DEEP : XR_STAB));
                stabs++;
            }                   // one stab: the heavy thunk
            if (kit == SK_STAB) {
                SkPush(ms - 90, SHIV_KIT_N + XR_EFFORT);
                efforts++;
                if (!firststab && !fin) {
                    SkPush(ms + 60,
                            SHIV_KIT_N + (b & 1 ? XR_PAIN_A : XR_PAIN_B));
                    pains++;
                }
                firststab = 0;
            }
            if (pcm) {          // the blade comes out: before the next beat (>= 110 ms ahead of it), 340 ms after the last (the death kit's native extract sits at +367)
                int nxt = 100000;
                for (int c = 0; c < nb; c++)
                    if (bm[c] > ms && bm[c] < nxt)
                        nxt = bm[c];
                int dly = fin ? (pull_at > ms + 120 ? pull_at + 33 - ms : 340) : nxt - ms - 70 < 260 ? nxt - ms - 70 : 260;     // (last blow: the clip's real blade-out when measured)
                if (dly >= 110 || fin) {
                    SkPush(ms + dly, SHIV_KIT_N + XR_PULL);
                    pulls++;
                }
            }
        }
        Xlog("shiv audio: %s: %d beats -> %d PCM stab thunks, %d pull-outs, %d attacker efforts, %d victim pains on top of the SP kit (arm whooshes ducked)", move, nb, stabs, pulls, efforts, pains);
    }
    for (int a = 1; a < g_sk.n; a++)
        for (int b = a; b > 0 && g_sk.ev[b - 1].ms > g_sk.ev[b].ms; b--) {
            __typeof__(g_sk.ev[0]) x = g_sk.ev[b];
            g_sk.ev[b] = g_sk.ev[b - 1];
            g_sk.ev[b - 1] = x;
        }
    if (pull_at && g_sk.n)
        Xlog("shiv kit: %s - pull-out sound / blood trail / death tail (+333 on) moved to the clip's real blade-out at %d ms", move, pull_at);
    if (g_sk.n)
        Xlog("shiv kit: %s - %d SP knife-kill events armed (sound / blood / voice of Ellie's knife kills), the 2013 shiv cues off", move, g_sk.n);
}

static void ShivkitFire(int j, uint64_t att, uint64_t vic)
{
    static int nl;
    const SkRow *R = SkRowp(g_sk.ev[j].row);
    uint64_t ent = g_sk.ev[j].role ? vic : att;
    if (!ent)
        return;
    uint64_t r = 0;
    int xtra = g_sk.ev[j].row >= (int) SHIV_KIT_N;
    if (R->kind == 3) {         // a ripped Ellie knife cue on the body (PCM mixer)
        r = (uint64_t) BladeCuePlay(R->vk,
                                      g_ex.gain > 0.0f ? g_ex.gain : 1.0f,
                                      ent, EXPOS, "shiv-audio");
    } else if (R->kind == 0) {
        uint64_t a[5] = { R->sid, ent, R->joint, 0, 0 };
        r = DcCall(FNV64("play-sound-at-joint"), 5, a);
    } else if (R->kind == 2) {
        int fk = R->vk;
        uint64_t now = GetTickCount64();
        // One voice per speaker, like a real body (no grunt and choke at the same time): every line - the kit's own and the
        // extra efforts / pains - keeps a 380 ms gap per speaker (shared with the exec+ voice rows); once the choke / gurgle / throat
        // line started, that speaker makes no more grunts or pains; a choke that lands on a grunt still playing cuts it (stop-vox).
        int sp = g_sk.ev[j].role ? 1 : 0, tier = SkVoxTier(R->name);
        uint64_t last = sp ? g_epVicVoxMs : g_attVoxMs;
        if (g_sk.vox_ms[sp] > last)
            last = g_sk.vox_ms[sp];
        if (g_sk.vox_tier[sp] == 2 && tier < 2)
            return;
        if (last && now - last < 900 && tier > g_sk.vox_tier[sp]) {
            uint64_t Sv[1] = { ent };
            VoxStop(Sv[0]);
        } else if (last && now - last < 380)
            return;
        g_sk.vox_ms[sp] = now;
        if (tier > g_sk.vox_tier[sp] || now - last >= 900)
            g_sk.vox_tier[sp] = (uint8_t) tier;
        if (sp && tier == 2)
            g_epVicDying = 1;
        uint64_t a[4] = { ent, xtra ? FNV64(R->name) : R->sid, 2, 0 };
        uint64_t fs = FemEffortSid(ent, fk);  // a female speaker: her own take of this kind
        if (fs)
            a[1] = fs;
        r = DcCall(FNV64("say"), 4, a);
        if (!g_sk.ev[j].role)
            g_attVoxMs = now;
        else
            g_epVicVoxMs = now;
        if (r == ~0ULL) {
            if (fk == 0)
                GxPlay(GXS_EFFORT, ent, 0);
            else
                GxPlay(GXS_PAIN, ent, fk >= 2);
        }                       // the speaker's bank has no such event
    } else {
        uint8_t *obj = CfxeObjOf(ent), *me = BpMe();
        if (!obj)
            return;
        int kind = obj != me;
        if (R->sid == 0x4b1e84fb0b3138b4ULL && kind)
            return;             // screen-blood-splatter: only on OUR screen (we are the attacker)
        if (!g_sk.ev[j].role && !strncmp(R->name, "bm-", 3)
            && ClipfxNoAttMasks())
            return;
        if (ClipfxOff())
            return;
        const CfxeEntry *ev = &g_skEnt[g_sk.ev[j].row];
        int nk = g_sk.neck && g_sk.ev[j].role && !xtra;
        if (nk) {
            const CfxeEntry *e2 = SkNeckEntry(g_sk.ev[j].row, ev);
            nk = e2 != ev;
            ev = e2;
        }
        if (kind && CfxeHasActorPlayer(ev))
            ev = CfxeSelfVariant(ev,
                                   nk ? &g_sk_neck_self[g_sk.ev[j].
                                                        row] :
                                   &g_sk_self[g_sk.ev[j].row]);
        r = ClipfxFire(obj, FNV64(R->clip), ev, !kind);
        if (MELEE2013[g_sk.pair].kind == MX_INT_SHIV && g_sk.ev[j].role) {      // Where the interrogation's blood lands - the victim's neck / head / the attacker's hand prop in world space
            float nk_[3] = { 0, 0, 0 }, hd_[3] = { 0, 0, 0 }, hp_[3] =
                { 0, 0, 0 };
            int a_ = XbxLoc(ent, "neck", nk_, NULL), b_ =
                XbxLoc(ent, "headb", hd_, NULL), c_ =
                XbxLoc(att, "r_hand_prop", hp_, NULL);
            static int nlp;
            if (LOGCAP(nlp, 60))
                Xlog("shiv kit: %s %s on the victim at %u ms%s: neck %s%.2f %.2f %.2f, head %s%.2f %.2f %.2f, attacker blade hand %s%.2f %.2f %.2f -> %llx (%s screen)", MELEE2013[g_sk.pair].name, R->name, g_sk.ev[j].ms, nk ? " [re-aimed at the neck]" : "", a_ ? "" : "?", nk_[0], nk_[1], nk_[2], b_ ? "" : "?", hd_[0], hd_[1], hd_[2], c_ ? "" : "?", hp_[0], hp_[1], hp_[2], (unsigned long long) r, kind ? "puppet-victim" : "our-own-victim");
        }
    }
    if (r && r != ~0ULL)
        g_sk.fired++;
    else
        g_sk.failed++;
    if (xtra) {
        static int nla;
        if (LOGCAP(nla, 160))
            Xlog("shiv audio: %s %s on the %s at %u ms (%s) -> %llx",
                 R->kind == 3 ? "PCM cue" : "voice", R->name,
                 g_sk.ev[j].role ? "victim" : "attacker", g_sk.ev[j].ms,
                 MELEE2013[g_sk.pair].name, (unsigned long long) r);
    } else if (LOGCAP(nl, 160))
        Xlog("shiv kit: %s %s on the %s (%u ms, %s f%.0f) -> %llx",
             R->kind == 0 ? "sound" : R->kind == 2 ? "voice" : "fx",
             R->name, g_sk.ev[j].role ? "victim" : "attacker",
             g_sk.ev[j].ms, R->clip, (double) R->frame,
             (unsigned long long) r);
}

// called by ex_tick every frame of a melee execution: pair = MELEE2013 row, t0 = the move's psync start, ems = ms since
static void ShivkitTick(int pair, double t0, double ems, uint64_t att,
                         uint64_t vic)
{
    if (t0 != g_sk.t0 || pair != g_sk.pair)
        ShivkitArm(pair, t0,
                    (size_t) pair < MX_N ? MELEE2013[pair].name : NULL);
    for (int j = g_sk.i; j < g_sk.n; j++) {
        if (g_sk.ev[j].done)
            continue;
        if ((double) g_sk.ev[j].ms > ems)
            break;              // (sorted by time)
        if (ems - g_sk.ev[j].ms > 400.0) {
            g_sk.ev[j].done = 1;
            continue;
        }                       // a late start: what has passed stays passed
        const SkRow *R = SkRowp(g_sk.ev[j].row);
        if (R->kind == 1
            && (CfxStream(R->sid)
                || R->sid ==
                0xb9d9c3db6fb35cf7ULL /* blood-ribbon-stream-eff */ )) {
            uint8_t *m = CfxRtmgr();   // GPU-compute streams: a full compute-entry table drops them (ctxdown cd_fx_tick)
            if (m && CfxUsed(m) >= CFX_N && g_sk.ev[j].tries < 15) {
                if (!g_sk.ev[j].tries++ && !CfxMakeRoom(m))
                    g_sk.ev[j].tries = 15;
                continue;
            }
        }
        g_sk.ev[j].done = 1;
        ShivkitFire(j, att, vic);
    }
    while (g_sk.i < g_sk.n && g_sk.ev[g_sk.i].done)
        g_sk.i++;
    if (g_sk.n && g_sk.i >= g_sk.n && g_sk.fired + g_sk.failed > 0) {
        Xlog("shiv kit: %s done - %d events played, %d refused",
             MELEE2013[pair].name, g_sk.fired, g_sk.failed);
        g_sk.fired = g_sk.failed = 0;
    }
}
