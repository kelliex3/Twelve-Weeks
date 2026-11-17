// melee_duel_contact.c - DUEL CONTACT: every duel blow visibly connects, and the victim's reaction starts on the frame the fist / blade touches, on BOTH screens
// (v5: the duel rules unchanged, the picture like SP melee). Design + evidence: re/melee-duel-contact.md. Included after melee_duel_tl.c.
// Log prefix "mct:". Opt-OUT: t2r-duel-no-contact.txt next to the exe (re-read every 2 s); it only acts while the duel is on (t2r-melee-duel.txt).
//
// Why (live clips at ~160 ms RTT, remote player = victim, their screen; A/B at ~200 ms RTT, B = victim):
//  - the replica swing on the attacker puppet (melee_duel_tl.c dtl_pred_swing) reached its contact frame at the SENDER's contact time; the pair START is
//    sent at that contact and arrives one-way latency later (B log: "CONFIRMED by its pair (97..130 ms vs the predicted contact)" at one-way 100-105 ms).
//    For that one-way the fist went through its follow-through with no reaction, then the reaction started with the fist already pulled back.
//  - the puppet stood where the net showed it (the sender's warp seen late): "local fit (victim) ... bodies 1.49 / 1.89 m apart (authored 0.89 / 0.82)"
//    - the pair then pulled both bodies together over >= 0.20 s while the head-snap already played: the reaction with the fist 0.5-1 m short.
//  - on the attacker's screen the victim half stood FROZEN on its first frame for the hit-confirm wait (<= 140 ms) while our half ran on after a 50 ms
//    hit-stop: the fist was withdrawing before the head moved.
//  - pin blows (mp-*-wall / -low-cover, surface frame) on the victim started 100 ms in (phase skip): the impact at +35 ms was never shown.
//
// What it does (mechanics - damage, dodge windows, verdicts, hand-off times - untouched):
//  VICTIM SCREEN  REPLICA TIMING  the swing replica plays from phase 0 at the rate that puts its contact frame on the pair START's arrival (SWING
//                                 arrival + its tce = the sender's own windup speed); closed loop on the read phase, so a queued / late start still
//                                 lands on it. At the contact frame it hit-stops (x MCT_HOLD_RATE) until the START, at most MCT_WAIT_MS; no START =
//                                 a miss here: it plays its follow-through at 1.0.
//                 WARP            the attacker puppet is carried over the windup onto the pair's authored contact spot: on the line to our body at the
//                                 pair's authored root distance, facing us, eased in (smoothstep from the SWING to the contact), <= MCT_STEP per frame,
//                                 on top of whatever placed the puppet this call (dtl's displayed locator / psync's hand-back glide / the net). The START
//                                 then finds the bodies at the authored distance: the local fit moves nothing, there is no pull during the reaction.
//                                 Frozen while we dodge / swing ourselves; no START -> it fades out over MCT_FADE_MS.
//                 OUR HALF        our victim half starts on that frame with a short blend (the residual offset at 3 m/s / 360 deg/s, >= 0.08 s)
//                                 instead of the >= 0.20 s pull: the head moves with the impact (SP: no blend at the switch).
//                 PINS            a pin blow START on us starts at frame 0 (its replica reaches contact at the START's arrival, no catch-up).
//  ATTACKER SCREEN HIT-CONFIRM    the wait for a dodge refusal (paired.c vlag) plays BOTH halves from frame 0 at x(impact / wait): the fist travels its last
//                                 frames onto the face while the victim half is still before its impact; both reach the impact (+35 ms clip) when the wait
//                                 ends. A refusal inside the wait still stops the pair before any reaction shows. No frozen frame on either half.
//  METRICS        per blow, each screen: 'mct: ... CONTACT' = hand joint -> victim head / chest at the attacker half's impact frame, roots vs authored,
//                 victim impact vs attacker impact (ms), frames a half did not advance in its first 400 ms; victim screen also 'mct: ... START ... vs its
//                 replica's contact'; 30 s summary.
#define MCT_IMPACT_MS 35.0f     // the -hit pair's impact sits ~35 ms into both halves (mduel_moves.h dmax 0.03; SP spec: 32-45 ms)
#define MCT_WAIT_MS 90          // the replica hit-stops at its contact this long for the START (network jitter), then it is a miss here
#define MCT_HOLD_RATE 0.12f     // the replica's rate while it waits on its contact frame
#define MCT_FADE_MS 300.0f
#define MCT_STEP 0.10f          // m per frame of warp change (6 m/s at 60 fps)
#define MCT_YSTEP 9.0f          // deg per frame
#define MCT_MAX_PULL 1.30f      // m: never carried further than this
#define MCT_HS_MIN 0.15f
#define MCT_VIC_BL_MIN 0.08f
#define MCT_REACH_ROOT_M 0.40f  // [guess] bodies this much farther apart than the authored contact = a blow that can't have connected
#define MCT_FAR_M 0.30f         // a hand farther than this from the victim at the impact frame = no visible contact
#define MCT_SUM_MS 30000
// round 2 (A/B, 200 ms RTT: timing fixed, 5 / 8 blows still 0.3-0.6 m short on the victim screen)
#define MCT_PUP_XFADE 0.05f     // s: the attacker half's crossfade on the puppet when its replica stands on the contact frame (dtl's 0.18 s hid the -hit's fist)
#define MCT_LATE_MS 40.0f       // a START this much before the replica's contact frame keeps dtl's crossfade (the poses differ)
#define MCT_DYAW_CAP 30.0f      // deg: the warp turns the puppet at most this far off its net yaw (TUNING)
#define MCT_YAW_MAX 75.0f       // deg: the pair's authored attacker yaw off the line to the victim is used up to this
#define MCT_OWN_XFADE 0.05f     // s (round 3): our own attacker half's blend-in on the attacker screen (was the move's 0.20-0.30 s pull)
#define MCT_WIN_MS 150.0f       // ms of the attacker -hit clip searched for the closest hand -> victim (the contact frame)
#define MCTLOG(...) do { if (LOGCAP(g_mct.nlog, 2500)) Xlog(__VA_ARGS__); } while (0)
static struct {
    int on, cfg_logged, nlog, no_yaw, no_xfade, no_own;
    uint64_t cfg_at;
    // victim screen: the partner's swing on its way to us
    struct {
        int on, state, row, reached, released, nset, held, taken;
        uint32_t slot;
        uint64_t h, sid, t_arm, T, reached_at;
        float hd_reach, rd_reach, D, dur, tc_s, rate, o[2], oy, o_arm[2],
            oy_arm, os[2], dys, pull_max;
        double last_t, fade_t0;
        float o_f[2], oy_f, da;
        uint64_t rj[2];
        int ray_st;             // path rays puppet -> us (0 pending, 1 clear, 2 blocked)
        int plain, plain_at, plain_done;
        uint64_t plain_t0;
        float plain_min;
        const char *plain_jn;   // ps25: the blow landed plain (MDUO_PLAIN) - no START comes; the -hit clip's contact measurement
    } w;
    // the running pair's contact measurement (both screens)
    struct {
        uint32_t uid;
        int role, done, ok_imp, still[2], run[2], runmax[2];
        double t0, ai, vi;
        float ct[2], dur[2], dmin, roots, auth;
        const char *jn;
        float d35, cmin, rmin;
        const char *j35;        // round 2: dmin = the closest hand -> victim over the first MCT_WIN_MS of the clip (at clip ms cmin, roots rmin); d35 = at +35 ms
        int rep_ok, rep_held;
        float rep_delta, warp;
    } m;
    struct {
        uint32_t uid;
        int held;
        float delta, warp;
    } pend;                     // mct_note_pair: the replica part of the START just played (copied into m for that uid)
    uint32_t n_blows, n_far, n_rep, n_nostart, still_sum;
    double dist_sum, react_sum, rep_sum;
    uint64_t sum_at;
} g_mct;
static int MctOn(void)
{
    if (!MduOn()) {
        g_mct.on = 0;
        return 0;
    }
    uint64_t now = GetTickCount64();
    if (!g_mct.cfg_at || now - g_mct.cfg_at >= 2000) {
        g_mct.cfg_at = now ? now : 1;
        FILE *f = ExeSideOpen("t2r-duel-no-contact.txt", "r");
        int o = f == NULL;
        if (f)
            fclose(f);
        if (o != g_mct.on || !g_mct.cfg_logged) {
            Xlog("mct: %s",
                 o ?
                 "ON - duel contact: replica swing on its contact frame when the START arrives, attacker puppet carried onto the contact spot, hit-confirm wait as one slowed pair, contact metrics (t2r-duel-no-contact.txt = off)"
                 : "OFF (t2r-duel-no-contact.txt)");
            g_mct.cfg_logged = 1;
        }
        g_mct.on = o;
        int ny = 0, nx = 0;
        if ((f = ExeSideOpen("t2r-duel-contact-no-yaw.txt", "r"))) {
            fclose(f);
            ny = 1;
        }
        if ((f = ExeSideOpen("t2r-duel-contact-no-xfade.txt", "r"))) {
            fclose(f);
            nx = 1;
        }
        {
            int no = 0;
            if ((f =
                 ExeSideOpen("t2r-duel-contact-no-own-xfade.txt",
                               "r"))) {
                fclose(f);
                no = 1;
            }
            if (no != g_mct.no_own || g_mct.cfg_logged == 1)
                Xlog("mct: round 3 - our own attacker half blends in over %s", no ? "the move's pull (t2r-duel-contact-no-own-xfade.txt)" : "0.05 s on the attacker screen (t2r-duel-contact-no-own-xfade.txt = the move's pull)");
            g_mct.no_own = no;
        }
        if (ny != g_mct.no_yaw || nx != g_mct.no_xfade
            || g_mct.cfg_logged == 1) {
            Xlog("mct: round 2 - warp yaw %s; attacker half crossfade on the puppet %s", ny ? "faces us (t2r-duel-contact-no-yaw.txt)" : "= the pair's authored attacker yaw off the line to us (t2r-duel-contact-no-yaw.txt = face us)", nx ? "dtl's 0.18 s (t2r-duel-contact-no-xfade.txt)" : "0.05 s when its replica stands on the contact frame (t2r-duel-contact-no-xfade.txt = dtl's 0.18 s)");
            g_mct.cfg_logged = 2;
        }
        g_mct.no_yaw = ny;
        g_mct.no_xfade = nx;
    }
    return g_mct.on;
}

static float MctSmooth(float u)
{
    if (u <= 0.0f)
        return 0.0f;
    if (u >= 1.0f)
        return 1.0f;
    return u * u * (3.0f - 2.0f * u);
}

static int MctDuelPair(void)
{
    return g_ps.kind == PS_CTXDOWN && g_ps.nseg <= 1
        && MduMove(g_ps.move);
}

// the closest hand joint of `att` to the head / chest of `vic` (m), -1 = no joints
static float MctHandDist(uint64_t att, uint64_t vic, const char **jn)
{
    static const char *const H[2] =
        { "l_hand_prop_attachment", "r_hand_prop_attachment" },
        *const V[2] = { "headb", "spined" };
    float hp[2][3], vp[2][3], best = -1.0f;
    int okh[2], okv[2];
    for (int i = 0; i < 2; i++) {
        okh[i] = GxJointPos(att, H[i], hp[i]);
        okv[i] = GxJointPos(vic, V[i], vp[i]);
    }
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++) {
            if (!okh[i] || !okv[j])
                continue;
            float dx = hp[i][0] - vp[j][0], dy = hp[i][1] - vp[j][1], dz =
                hp[i][2] - vp[j][2], d =
                __builtin_sqrtf(dx * dx + dy * dy + dz * dz);
            if (best < 0.0f || d < best) {
                best = d;
                if (jn)
                    *jn =
                        j ? (i ? "chest (right hand)" :
                             "chest (left hand)") : (i ?
                                                     "head (right hand)" :
                                                     "head (left hand)");
            }
        }
    return best;
}

// ('NO VISIBLE CONTACT' for weapon blows with the blade on the body - the metric measured the HAND; 6 of 6): with a melee weapon held the
// nearer of the hand and the weapon's segment counts. The segment is an ESTIMATE, the one ctxdown.c:116 uses for the weapon takedowns: from the right wrist
// MCT_WPN_LEN_M along the forearm (elbow -> wrist); the label says so. Fists: the hand, as before.
#define MCT_WPN_LEN_M 0.70f     // [TUNING ps25] not measured per weapon
static float MctReachDist(uint64_t att, uint64_t vic, uint64_t wpn,
                            const char **jn)
{
    float best = MctHandDist(att, vic, jn);
    if (!wpn || wpn == ~0ULL || wpn == FNV64("abby-unarmed")
        || (MeleeClassOf(wpn, NULL) < 0
            && MeleeClassOf(BaseWeapon(wpn), NULL) < 0))
        return best;
    static const char *const V[2] = { "headb", "spined" };
    float eb[3], wr[3];
    if (!GxJointPos(att, "r_elbow", eb)
        || !GxJointPos(att, "r_wrist", wr))
        return best;
    float ax = wr[0] - eb[0], ay = wr[1] - eb[1], az = wr[2] - eb[2], al =
        __builtin_sqrtf(ax * ax + ay * ay + az * az);
    if (!(al > 0.05f))
        return best;
    ax /= al;
    ay /= al;
    az /= al;
    for (int j = 0; j < 2; j++) {
        float vp[3];
        if (!GxJointPos(vic, V[j], vp))
            continue;
        float px = vp[0] - wr[0], py = vp[1] - wr[1], pz =
            vp[2] - wr[2], t = px * ax + py * ay + pz * az;
        t = t < 0.0f ? 0.0f : t > MCT_WPN_LEN_M ? MCT_WPN_LEN_M : t;
        float dx = px - ax * t, dy = py - ay * t, dz = pz - az * t, d =
            __builtin_sqrtf(dx * dx + dy * dy + dz * dz);
        if (best < 0.0f || d < best) {
            best = d;
            if (jn)
                *jn =
                    j ? "chest (weapon segment, est.)" :
                    "head (weapon segment, est.)";
        }
    }
    return best;
}

// ---- victim screen: arm on the partner's SWING message (melee_duel.c mdu_recv, after dtl_pred_swing)
static int MpnHintCd(int row1);       // melee_duel_pin.c
static void MctFade(const char *why);
static void MctArmSwing(uint32_t slot, int row, int tce, float ow,
                          int pin_row1)
{
    if (!MctOn() || g_mdu.slot != (int) slot || row < 0
        || row >= (int) MDU_MOVES_N)
        return;
    const MduRowT *m = &MDU_MOVES[row];
    if (!m->swing || !*m->swing || m->tc_ms <= 0)
        return;
    uint64_t h = PuppetHandle(slot), now = GetTickCount64();
    if (!h)
        return;
    MduInit();
    int cd = g_mdu.mv_cd[row];
    // v5-contact3: the sender expects this contact to become a PIN push (SWING byte 8) - warp to the push pair's spacing, not the duel pair's
    int pcd = pin_row1 ? MpnHintCd(pin_row1) : -1;
    if (pcd >= 0 && CdPairLoaded(pcd)) {
        MCTLOG
            ("mct: slot %u's swing %s - the sender expects a pin push %s at its contact: the warp targets that pair",
             slot, m->move, CD_PAIRS[pcd].name);
        cd = pcd;
    }
    float D = 0.0f, cv[8], ca[8];
    if (cd >= 0 && CdOffsetAt(SID_PLAYER, CD_VIC0(cd), 0.0f, cv)
        && CdOffsetAt(h, CD_ATT0(cd), 0.0f, ca))
        D = __builtin_sqrtf((ca[0] - cv[0]) * (ca[0] - cv[0]) +
                            (ca[2] - cv[2]) * (ca[2] - cv[2]));
    // round 2 (B u0071acec, punch-2: "start 0.02 m / 28 deg from the attacker's authored spot" - the warp turned the puppet straight at us, the pair has
    // the attacker 28 deg off that line; the pull turned it over 0.20 s, so at the +35 ms impact it still stood ~20 deg off: the hook passed the head)
    float da = 0.0f;
    if (D > 0.2f && D < 3.5f) {
        float th = __builtin_atan2f(ca[0] - cv[0], ca[2] - cv[2]) * 57.29578f;  // victim -> attacker line (ap space)
        da = Uc4Wrap(Uc4Yaw(ca) - th - 180.0f);       // the attacker's yaw off "facing the victim"
        if (da > MCT_YAW_MAX || da < -MCT_YAW_MAX)
            da = 0.0f;
    }
    if (!(D > 0.2f && D < 3.5f))
        D = m->dist > 0.2f ? m->dist : 0.95f;
    // (B's knife lunge carried B's puppet THROUGH a crate on A's screen at 111.8-112.45 s while B's own
    // screen said 'the partner is 2.23 m away - out of the swing's reach, no placement'; the machete at 186.9 s pulled A through a train wall): the carry
    // has the attacker's reach check (MDU_FAR_M, same as the attacker screen's warp) and two path rays (knee / chest) puppet -> us; a blocked path = no carry.
    {
        float b_[16], mb_[16];
        const float *pl = PsyncLoc(h, b_), *me = PlayerLocator(mb_);
        if (pl && me && PsLocOk(pl)) {
            float ddx = pl[0] - me[0], ddz = pl[2] - me[2], d0 =
                __builtin_sqrtf(ddx * ddx + ddz * ddz);
            if (d0 > MDU_FAR_M + 0.2f) {
                MCTLOG
                    ("mct: slot %u's swing %s - its puppet is %.2f m from us (reach %.2f m): out of reach, not carried",
                     slot, m->move, (double) d0,
                     (double) (MDU_FAR_M + 0.2f));
                if (g_mct.w.on && g_mct.w.h == h)
                    MctFade("out of reach");
                else
                    g_mct.w.on = 0;
                return;
            }
        }
    }
    typeof(&g_mct.w) w = &g_mct.w;
    int cont = w->on && w->h == h && w->state == 1;     // a new blow while the last one's warp is on: it continues from there (no snap)
    if (!cont && w->on && w->h == h && w->state == 2)
        cont = 1;
    float o0[2] = { cont ? w->o[0] : 0.0f, cont ? w->o[1] : 0.0f }, oy0 =
        cont ? w->oy : 0.0f;
    memset(w, 0, sizeof *w);
    w->on = 1;
    w->state = 1;
    w->row = row;
    w->slot = slot;
    w->h = h;
    w->sid = FNV64(m->swing);
    w->t_arm = now;
    w->T = now + (uint64_t) (tce > 0 ? tce : m->tc_ms);
    w->D = D;
    w->dur = MdgClipLen(w->sid);
    w->tc_s = (float) m->tc_ms * MduClipK(m) / 1000.0f;
    w->rate = -1.0f;            // (ps43 N1: the replica reaches the clip ms the -hit half continues from, melee_duel.c mdu_clip_k)
    w->o[0] = w->o_arm[0] = o0[0];
    w->o[1] = w->o_arm[1] = o0[1];
    w->oy = w->oy_arm = oy0;
    w->da = g_mct.no_yaw ? 0.0f : da;
    {
        float b_[16], mb_[16];
        const float *pl = PsyncLoc(h, b_), *me = PlayerLocator(mb_);
        w->ray_st = 1;
        if (pl && me && PsLocOk(pl)) {
            float ddx = me[0] - pl[0], ddz = me[2] - pl[2], dd =
                __builtin_sqrtf(ddx * ddx + ddz * ddz);
            if (dd > 1.5f) {    // (closer: nothing that blocks a body fits between the two capsules)
                // (32 / 39 BLOCKED over open floor): the chest ray met the swinging arm / weapon and the knee ray a stride.
                // One ray at shin height (0.3 m: under the arms, still stops on crates / walls / panels), 0.6 m off its body to 0.7 m short of ours.
                float k = (dd - 0.7f) / dd, k0 = 0.6f / dd;
                w->rj[1] = 0;
                for (int i = 0; i < 1; i++) {
                    float hy = 0.3f, a[3] =
                        { pl[0] + ddx * k0, pl[1] + hy, pl[2] + ddz * k0 },
                        b[3] =
                        { pl[0] + ddx * k, me[1] + hy, pl[2] + ddz * k };
                    w->rj[i] = WorldRayKickM(a, b, h, 0x1b07);
                }
                w->ray_st = (w->rj[0] || w->rj[1]) ? 0 : 1;
            }
        }
    }
    MCTLOG
        ("mct: slot %u's swing %s - its puppet is carried onto the contact spot (%.2f m from us, yaw %+.0f deg off facing us, pair %s) over %d ms; the replica reaches its contact frame on the START's arrival (one-way %.0f ms; dtl's absolute contact would be %.0f ms earlier)",
         slot, m->move, (double) D, (double) w->da,
         cd >= 0 ? CD_PAIRS[cd].name : "?", (int) (w->T - now),
         (double) ow, (double) ow);
}

static void MctFade(const char *why)
{
    typeof(&g_mct.w) w = &g_mct.w;
    if (!w->on || w->state == 2)
        return;
    w->state = 2;
    w->fade_t0 = PsyncMs();
    w->o_f[0] = w->o[0];
    w->o_f[1] = w->o[1];
    w->oy_f = w->oy;
    MCTLOG
        ("mct: slot %u warp fades out over %.0f ms from %.2f m / %.0f deg (%s)",
         w->slot, (double) MCT_FADE_MS,
         (double) __builtin_sqrtf(w->o[0] * w->o[0] + w->o[1] * w->o[1]),
         (double) w->oy, why);
}

// ps44 T4 (melee_duel_parry.c mdp_loser_cut44): the winner's screen stops the loser's swing replica - its contact carry fades out with it (1 = it was carrying)
static int MctCancelCarry44(uint32_t slot, const char *why)
{
    typeof(&g_mct.w) w = &g_mct.w;
    if (!w->on || w->slot != slot || w->state != 1)
        return 0;
    MctFade(why);
    return 1;
}

// ('2 writers [duel.dtl_locator ..., duel.mct_warp ...]' on every swing frame): the carry no longer writes on top of dtl_locator.
// mct_carry_at turns the pose `l` that the puppet's owner is about to write into the carried pose `o8` (1 = carried); dtl_locator (melee_duel_tl.c) calls it before
// its one write. mct_locator_post is left for the calls without the timeline (psync's hand-back glide, the net locator): there it still adds its write.
// t2r-mct-merge-ps24.txt = dtl writes, then this writes again (old).
static uint64_t g_mct_carried_f, g_mct_carried_h;
static int MctMergeOld(void)
{
    static int v = -1;
    static uint64_t at;
    uint64_t n = GetTickCount64();
    if (v < 0 || n - at > 3000) {
        at = n;
        v = GetFileAttributesA("t2r-mct-merge-ps24.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    return v;
}

static int MctCarryAt(uint64_t h, const float *l, float *o8_out)
{
    typeof(&g_mct.w) w = &g_mct.w;
    if (!h || !w->on || h != w->h || !g_mct.on)
        return 0;
    float mb[16];
    const float *me = PlayerLocator(mb);
    if (!l || !me || !PsLocOk(l))
        return 0;
    double t = PsyncMs();
    uint64_t now = GetTickCount64();
    float P[3] = { l[0], l[1], l[2] }, yaw = Uc4Yaw(l);
    float dx = P[0] - me[0], dz = P[2] - me[2], d =
        __builtin_sqrtf(dx * dx + dz * dz);
    if (d > 4.0f || d < 0.05f) {
        w->on = 0;
        return 0;
    }
    // (still 19 / 23 BLOCKED over open floor - every cast from >= 1.5 m hit something; cause unknown): the path rays only rule with
    // t2r-mct-path-rays.txt; by default the reach check alone decides (the carry is capped at MCT_MAX_PULL).
    {
        static int on = -1;
        static uint64_t at;
        uint64_t n = GetTickCount64();
        if (on < 0 || n - at > 3000) {
            at = n;
            on = GetFileAttributesA("t2r-mct-path-rays.txt") !=
                INVALID_FILE_ATTRIBUTES;
        }
        if (!on)
            w->ray_st = 1;
    }
    if (w->state == 1 && w->ray_st == 0) {      // path rays still out: no carry yet (one or two frames)
        int bl = 0, pend = 0;
        for (int i = 0; i < 2; i++)
            if (w->rj[i]) {
                int r = RayBlocked(w->rj[i]);
                if (r < 0)
                    pend = 1;
                else if (r)
                    bl = 1;
            }
        if (bl) {
            w->ray_st = 2;
            MCTLOG
                ("mct: slot %u's carry path is BLOCKED (geometry between its puppet and us, %.2f m) - not carried",
                 w->slot, (double) d);
            if (w->o[0] != 0.0f || w->o[1] != 0.0f)
                MctFade("path blocked");
            else
                w->on = 0;
            return 0;
        }
        if (pend && GetTickCount64() - w->t_arm < 150)
            return 0;
        w->ray_st = 1;
    }
    if (t - w->last_t > 0.5) {  // (a second call in the same frame: the same offset)
        w->last_t = t;
        if (w->state == 1) {
            int freeze = g_mdu.sw_on || g_ps.role == 1 || MdgIframes(now) || (g_mdg.press_ms && now - g_mdg.press_ms < 700);   // we swing / dodge: the target spot stays where it was
            if (!freeze || !w->nset) {
                float gap = w->D - d;
                if (gap < -MCT_MAX_PULL)
                    gap = -MCT_MAX_PULL;
                if (gap > MCT_MAX_PULL)
                    gap = MCT_MAX_PULL;
                w->os[0] = dx / d * gap;
                w->os[1] = dz / d * gap;
                float px = P[0] + w->os[0], pz = P[2] + w->os[1];
                w->dys = Uc4Wrap(__builtin_atan2f(me[0] - px, me[2] - pz) * 57.29578f + w->da - yaw);  // (round 2: + the pair's authored yaw off the line)
                {               // ('warp fades out over 300 ms from 0.67 m / -139 deg (no pair START)' = a visible spin): the warp turns the puppet at most MCT_DYAW_CAP off its net yaw, so a fade-out never un-turns a half circle. t2r-mct-yaw-old.txt = the old unlimited turn.
                    static int old_ = -1;
                    static uint64_t oc_;
                    if (old_ < 0 || GetTickCount64() - oc_ > 3000) {
                        oc_ = GetTickCount64();
                        old_ =
                            GetFileAttributesA("t2r-mct-yaw-old.txt") !=
                            INVALID_FILE_ATTRIBUTES;
                    }
                    if (!old_
                        && (w->dys > MCT_DYAW_CAP
                            || w->dys < -MCT_DYAW_CAP)) {
                        static int nl_;
                        if (LOGCAP(nl_, 30))
                            Xlog("mct: slot %u warp turn %.0f deg limited to %.0f deg off the net yaw (no spin)", w->slot, (double) w->dys, (double) MCT_DYAW_CAP);
                        w->dys = w->dys > 0 ? MCT_DYAW_CAP : -MCT_DYAW_CAP;
                    }
                }
                w->nset = 1;
            }
            float s =
                w->T >
                w->t_arm ? MctSmooth((float) (int64_t) (now - w->t_arm) /
                                      (float) (w->T - w->t_arm)) : 1.0f;
            float on[2] =
                { w->o_arm[0] * (1.0f - s) + w->os[0] * s,
          w->o_arm[1] * (1.0f - s) + w->os[1] * s }, oyn =
w->oy_arm * (1.0f - s) + w->dys * s;
            float ex = on[0] - w->o[0], ez = on[1] - w->o[1], el =
                __builtin_sqrtf(ex * ex + ez * ez);
            if (el > MCT_STEP) {
                ex *= MCT_STEP / el;
                ez *= MCT_STEP / el;
            }
            w->o[0] += ex;
            w->o[1] += ez;
            float ey = Uc4Wrap(oyn - w->oy);
            if (ey > MCT_YSTEP)
                ey = MCT_YSTEP;
            else if (ey < -MCT_YSTEP)
                ey = -MCT_YSTEP;
            w->oy = Uc4Wrap(w->oy + ey);
            float ol =
                __builtin_sqrtf(w->o[0] * w->o[0] + w->o[1] * w->o[1]);
            if (ol > w->pull_max)
                w->pull_max = ol;
        } else {
            float s = MctSmooth((float) ((t - w->fade_t0) / MCT_FADE_MS));
            if (s >= 1.0f) {
                w->on = 0;
                return 0;
            }
            w->o[0] = w->o_f[0] * (1.0f - s);
            w->o[1] = w->o_f[1] * (1.0f - s);
            w->oy = w->oy_f * (1.0f - s);
        }
    }
    float y = (yaw + w->oy) * 0.00872665f;
    float o8[8] =
        { P[0] + w->o[0], P[1], P[2] + w->o[1], 1.0f, 0.0f,
  __builtin_sinf(y), 0.0f, __builtin_cosf(y) };
    if (!PsLocOk(o8))
        return 0;
    memcpy(o8_out, o8, sizeof o8);
    return 1;
}

// locator_hook, after whatever placed puppet h this call: add the warp (not when dtl_locator already merged it into its write this frame)
static void MctLocatorPost(uint64_t h)
{
    if (g_mct_carried_f == g_frame && g_mct_carried_h == h)
        return;
    float b[16], o8[8];
    const float *l = h ? PsyncLoc(h, b) : NULL;
    if (!MctCarryAt(h, l, o8))
        return;
    static PsyncBf bf;
    PsyncBfFromLoc(&bf, o8);
    mo_write(h, &bf, MO_SRC("duel.mct_warp"), 0);
}

// paired.c psync_play: a pair took the attacker puppet - on the victim screen that is the START of the blow the replica was swinging
static void MctNotePair(uint32_t slot)
{
    typeof(&g_mct.w) w = &g_mct.w;
    if (!g_mct.on || g_ps.role != 2 || !MctDuelPair())
        return;
    uint64_t now = GetTickCount64();
    if (w->on && w->slot == slot && w->state == 1) {
        float mb[16], ab[16];
        const float *me = PlayerLocator(mb), *pa =
            PsyncLoc(g_ps.ent_att, ab);
        float d = me
            && pa ? __builtin_sqrtf((pa[0] - me[0]) * (pa[0] - me[0]) +
                                    (pa[2] - me[2]) * (pa[2] -
                                                       me[2])) : -1.0f;
        g_mct.pend.uid = g_ps.uid;
        g_mct.pend.held = w->held;
        g_mct.pend.warp = w->pull_max;
        g_mct.pend.delta =
            w->reached ? (float) (int64_t) (now -
                                            w->
                                            reached_at) :
            -(float) (int64_t) (w->T - now);
        g_mct.n_rep++;
        g_mct.rep_sum += __builtin_fabsf(g_mct.pend.delta);
        MCTLOG
            ("mct: u%08x START of slot %u's %s arrived %+.0f ms vs its replica's contact frame (%s, %d frames hit-stopped there; planned %+lld ms) - bodies %.2f m apart (authored %.2f), warp carried the puppet up to %.2f m",
             g_ps.uid, slot, MDU_MOVES[w->row].move,
             (double) g_mct.pend.delta,
             w->reached ? "reached" : "not reached yet", w->held,
             (long long) ((int64_t) now - (int64_t) w->T), (double) d,
             (double) w->D, (double) w->pull_max);
        w->taken = 1;
        w->on = 0;              // psync owns the puppet from here (its start spot = where the warp left it)
    }
}

// paired.c psync_play (role 1, the duel hit-confirm wait armed): both halves play slowed from frame 0 to the impact frame instead of a frozen victim
static void MctHitstopArm(void)
{
    if (!MctOn() || g_ps.role != 1 || !MctDuelPair()
        || !(g_ps.vlag > 0.0f))
        return;
    float r = MCT_IMPACT_MS / (g_ps.vlag * 1000.0f);
    if (r < MCT_HS_MIN)
        r = MCT_HS_MIN;
    if (r > 0.9f)
        return;
    g_ps.hs_rate = r;
    g_ps.ahold = g_ps.vlag;
    Xlog("mct: u%08x hit-confirm wait %.0f ms: both halves from frame 0 at x%.2f (fist onto the face, the victim reaches its impact as the wait ends) - no frozen victim, no fist moving on", g_ps.uid, g_ps.vlag * 1000.0f, r);
}

// paired.c psync_play: the victim half's blend-in on a duel -hit pair (bl = what the caller had; < 0 = the move's pull)
static float MctVicBlend(float bl)
{
    if (bl >= 0.0f || !g_mct.on || !MctDuelPair()
        || g_psOffD[1] < 0.0f)
        return bl;
    float b = fmaxf(g_psOffD[1] / 3.0f, g_ps_off_y[1] / 360.0f);
    if (b < MCT_VIC_BL_MIN)
        b = MCT_VIC_BL_MIN;
    if (b >= g_psBlend)
        return bl;
    Xlog("mct: u%08x victim half %s on %s blends in over %.2f s (off its spot %.2f m / %.0f deg) instead of the %.2f s pull - the reaction moves with the impact", g_ps.uid, PsyncName(g_ps.vic_sid), g_ps.ent_vic == SID_PLAYER ? "US" : "the puppet", (double) b, (double) g_psOffD[1], (double) g_ps_off_y[1], (double) g_psBlend);
    return b;
}

// paired.c ps_pup_att_blend (round 2): the attacker half's crossfade on the PUPPET for a duel pair on the victim screen (bl = dtl's DTL_PUP_XFADE).
// A/B: the replica on its contact frame had the fist 0.74 m from our chest (the swing clip ends before the fist lands - the -hit clip's
// first ~35 ms carry it in, SP's melee-effect event at +33 ms); with the 0.18 s crossfade the -hit pose was ~20 % in at the impact, so B showed
// 0.42-0.85 m where A showed 0.20-0.42 m for the same pairs at the same root spacing. The replica plays on that puppet up to this frame (live graph,
// the pose dtl's crossfade guards against popping from is the -hit's own frame 0), so a 3-frame crossfade. A START well before the replica
// reached its contact keeps dtl's crossfade (the poses differ there).
static float MctPupBlend(float bl)
{
    if (!MctOn() || g_mct.no_xfade || g_ps.role != 2 || !MctDuelPair()
        || !(bl > MCT_PUP_XFADE))
        return bl;
    int rep = g_mct.pend.uid == g_ps.uid;
    if (rep && g_mct.pend.delta < -MCT_LATE_MS) {
        MCTLOG
            ("mct: u%08x attacker half keeps dtl's %.2f s crossfade - the START came %.0f ms before its replica's contact frame",
             g_ps.uid, (double) bl, (double) -g_mct.pend.delta);
        return bl;
    }
    MCTLOG
        ("mct: u%08x attacker half %s on slot %d's puppet crossfades in over %.2f s (not %.2f s): its replica stands on the contact frame (%s) - the -hit's fist lands at the impact",
         g_ps.uid, PsyncName(g_ps.att_sid), g_ps.att,
         (double) MCT_PUP_XFADE, (double) bl,
         rep ? "duel swing" : "pin blow");
    return MCT_PUP_XFADE;
}

// paired.c psync_play (round 3): OUR attacker half's blend-in on the attacker screen (bl < 0 = the move's pull, g_ps_blend).
// A/B: B (puppet attacker half, 0.05 s since round 2) closest 0.15 m at +33 ms for punch-1 u001230b1; A, the same blow at the same
// spacing (roots 0.99 vs 1.03, rel yaw 180 on both), closest 0.35 m at +67 ms - our half started with the move's pull as its blend-in ("SP's pull
// ... over 0.20 s"): our body faded the -hit in from the swing's contact pose (the fist short of the face), so only part of it showed at the impact. Right-hand blows (straights:
// most of the reach is in the -hit's first frames) missed by 0.15-0.2 m, left hooks (0.20-0.22 m) barely. SP switches swing -> -hit with no
// blend; our swing clip stands on its contact frame here (mdu_contact / the pin blow's contact starts the pair).
// (A/B clash: B's own body 'dtl: SNAP 1.14 m in 1 frame' at a PARRY / CLASH pair start, camera on the grass): the cut-in assumes the duel
// swing's warp already put our body on its spot (-hit pairs); a parry / clash pair starts up to ~1.2 m off it - that body gets the SP pull instead (-hit pairs: <= ~0.2 m anchor residual, kept)
static int MduOwnOffSpot(void)
{
    if (GetFileAttributesA("t2r-duel-own-cut.txt") !=
        INVALID_FILE_ATTRIBUTES)
        return 0;
    int p = CdPairOfMove(g_ps.move);
    if (p >= 0 && strstr(CD_PAIRS[p].name, "parry")
        && g_psOffD[0] > 0.05f)
        return 1;
    if (p >= 0
        && (strstr(CD_PAIRS[p].name, "wall-attack")
            || strstr(CD_PAIRS[p].name, "cover-attack"))
        && g_psOffD[0] > 0.05f
        && GetFileAttributesA("t2r-pin-pair-cut.txt") ==
        INVALID_FILE_ATTRIBUTES)
        return 1;               // (pin soak: the later pin blows' -hit pairs cut in 0.26 m)   // (soak 2: a clash 0.26 m off its spot still popped 0.33 m - parry / clash pairs never had a warp)
    return g_psOffD[0] > 0.35f;
}

static float MctOwnAttBlend(float bl)
{
    if (MduOwnOffSpot())
        return bl;
    if (!MctOn() || g_mct.no_own || g_ps.role != 1
        || g_ps.ent_att != SID_PLAYER || !MctDuelPair() || PsDc())
        return bl;              // (a dodge counter blends from our duck clip: kept)
    float was = bl >= 0.0f ? bl : g_psBlend;
    if (was <= MCT_OWN_XFADE)
        return bl;
    MCTLOG
        ("mct: u%08x our attacker half %s blends in over %.2f s (not %.2f s): the -hit's fist lands at the impact on our screen",
         g_ps.uid, PsyncName(g_ps.att_sid), (double) MCT_OWN_XFADE,
         (double) was);
    return MCT_OWN_XFADE;
}

// paired.c psync_begin_remote: a duel pin blow START on us (surface frame) starts at frame 0
static int MctPinFrom0(uint64_t move, float skip_ms)
{
    if (!MctOn() || CdLocalMove(move))
        return 0;
    int p = CdPairOfMove(move);
    MCTLOG
        ("mct: pin blow %s on us from frame 0 (was a %.0f ms skip): its replica reaches its contact on this arrival, the impact is shown",
         p >= 0 ? CD_PAIRS[p].name : "?", (double) skip_ms);
    return 1;
}

// (all 7 blows: 'mct: slot 0's replica on its contact frame ... hand 0.48-0.85 m from our head / chest, roots 1.09 / 0.82 / 0.65 / 0.99 /
// 1.03 / 0.82 / 0.65 m (authored 1.09 / 0.82 / 0.63 / 0.99 / 1.02 / 0.82 / 0.63) - NO VISIBLE CONTACT' then 'replica waited 5-7 frames on its contact - no START: it
// plays its follow-through (a miss on this screen)'): the roots WERE at the authored distance; the victim held a bat, so every blow was PLAIN (melee_duel.c
// mdu_contact 'no synced pair', MDUO_PLAIN) and no pair START ever came. The replica stood MCT_WAIT_MS on its contact frame for it, then played the SWING clip's
// own tail - the whiff - while the victim's reaction ran; the fist never came in, because the swing clip stops short of the body (the -hit clip's first ~35 ms
// carry it in, see mct_pup_blend). The plain notice arrives 0-1 frame before the replica's contact frame (B [39601] / [39602], [39650] / [39651]): on it the
// attacker's -hit clip (the pair's attacker half, the clip the attacker's own screen plays in place) starts on the puppet from the replica's pose over the pair's
// 2-frame blend - no wait, no whiff. It is kept MCT_PLAIN_HIT_MS, then blends out into the streamed pose (the attacker's own -hit move). The carry fades as for
// any blow without a START. t2r-mct-plain-hit-ps24.txt = the old wait + swing tail.
#define MCT_PLAIN_HIT_MS 450    // [TUNING ps25] how long the forced -hit clip stays on the puppet (SP: the next swing can start 333 ms after the contact)
static int MctPlainOld(void)
{
    static int v = -1;
    static uint64_t at;
    uint64_t n = GetTickCount64();
    if (v < 0 || n - at > 3000) {
        at = n;
        v = GetFileAttributesA("t2r-mct-plain-hit-ps24.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    return v;
}

static void MctNotePlain(uint32_t slot)
{
    typeof(&g_mct.w) w = &g_mct.w;
    if (!g_mct.on || !w->on || w->slot != slot || w->state != 1 || w->taken
        || w->plain || MctPlainOld())
        return;
    uint64_t now = GetTickCount64();
    w->plain = 1;               // (no START will come: the replica never waits on its contact frame)
    if (!w->reached) {          // the blow has landed on the attacker's clock: the reach test (mct_reach_hold / _tick) reads the bodies as they stand now
        const char *jn0 = "?";
        float mb[16], pb[16];
        const float *me = PlayerLocator(mb), *pl = PsyncLoc(w->h, pb);
        w->reached = 1;
        w->reached_at = now;
        w->hd_reach =
            MctReachDist(w->h, SID_PLAYER, MduVicMelee((int) slot),
                           &jn0);
        w->rd_reach = me
            && pl ? __builtin_sqrtf((pl[0] - me[0]) * (pl[0] - me[0]) +
                                    (pl[2] - me[2]) * (pl[2] -
                                                       me[2])) : -1.0f;
    }
    int cd = w->row >= 0
        && w->row < (int) MDU_MOVES_N ? g_mdu.mv_cd[w->row] : -1;
    uint64_t hs = cd >= 0 && cd < (int) CD_N ? Fnv64Rt(CD_ATT0(cd)) : 0;
    float hdur = hs ? MdgClipLen(hs) : 0.0f, ph =
        PsyncPhase(w->h, w->sid);
    if (!(hdur > 0.2f)) {
        MCTLOG
            ("mct: slot %u's plain blow - no -hit clip loaded here for row %d: the replica plays its own follow-through at once (no wait)",
             slot, w->row);
        return;
    }
    g_melFloorOnce = MEL_HIT_BLEND;   // (paired.c: a duel -hit half starts over 2 frames since ps22)
    uint64_t r = MhrAnimate(w->h, hs, 0.0f, MEL_HIT_BLEND);
    g_melFloorOnce = -1.0f;
    if (r == ~0ULL) {
        MCTLOG
            ("mct: slot %u's plain blow - animate_ refused the -hit clip %s: the replica plays its own follow-through at once (no wait)",
             slot, CD_ATT0(cd));
        return;
    }
    uint64_t keep =
        (uint64_t) (hdur * 1000.0f) >
        MCT_PLAIN_HIT_MS +
        150 ? MCT_PLAIN_HIT_MS : (uint64_t) (hdur * 1000.0f) - 150, end =
        now + keep;
    g_mhr.pd[slot].ent = w->h;
    g_mhr.pd[slot].sid = hs;
    g_mhr.pd[slot].end = end;   // (mhr_tick blends it out at its end)
    if (g_dtl.run.src == DTL_S_SWING && g_dtl.run.slot == slot
        && g_dtl.run.ent == w->h) {
        g_dtl.run.sid = hs;
        g_dtl.run.t0 = now;
        g_dtl.run.end = end;
        g_dtl.run.cancel_at = now;
    }
    w->released = 1;
    w->plain_t0 = now ? now : 1;
    w->plain_min = -1.0f;
    w->plain_jn = "?";
    MCTLOG
        ("mct: slot %u's blow %s landed plain (no pair START will come) - its -hit clip %s starts on the puppet from the replica's pose (swing clip at %.0f ms, its contact frame %.0f ms, %+lld ms vs the planned arrival) over %.0f ms: no wait on the contact frame, no swing tail; kept %llu ms -> %llx",
         slot, MDU_MOVES[w->row].move, CD_ATT0(cd),
         ph >= 0.0f ? (double) (ph * w->dur * 1000.0f) : -1.0,
         (double) (w->tc_s * 1000.0f),
         (long long) ((int64_t) now - (int64_t) w->T),
         (double) (MEL_HIT_BLEND * 1000.0f), (unsigned long long) keep,
         (unsigned long long) r);
}

// ---- per frame (melee_duel.c mdu_tick)
static void MctReplicaTick(uint64_t now)
{
    typeof(&g_mct.w) w = &g_mct.w;
    if (!w->on)
        return;
    if (g_mdu.slot != (int) w->slot || PuppetHandle(w->slot) != w->h) {
        MctFade("the duel / puppet changed");
        return;
    }
    if (w->state == 1 && !w->taken && now > w->T + MCT_WAIT_MS + 60) {
        if (!w->plain)
            g_mct.n_nostart++;
        MctFade(w->
                 plain ?
                 "a plain blow: its -hit follow-through played, no pair" :
                 "no pair START: a miss here");
    }
    if (w->plain_t0 && !w->plain_done) {        // ps25: the plain blow's -hit clip on the puppet - its closest reach to our head / chest over the impact window (the proof it connects here)
        const char *jn = "?";
        float hd =
            MctReachDist(w->h, SID_PLAYER, MduVicMelee((int) w->slot),
                           &jn);
        int pel = (int) (now - w->plain_t0);
        if (hd >= 0.0f && (w->plain_min < 0.0f || hd < w->plain_min)) {
            w->plain_min = hd;
            w->plain_at = pel;
            w->plain_jn = jn;
        }
        if (pel > (int) MCT_WIN_MS) {
            float mb[16], pb[16];
            const float *me = PlayerLocator(mb), *pl =
                PsyncLoc(w->h, pb);
            w->plain_done = 1;
            MCTLOG
                ("mct: slot %u's plain blow CONTACT on our screen: closest %.2f m to our %s at +%d ms of its -hit clip, roots %.2f m (authored %.2f)%s",
                 w->slot, (double) w->plain_min, w->plain_jn, w->plain_at,
                 me
                 && pl ? (double) __builtin_sqrtf((pl[0] - me[0]) *
                                                  (pl[0] - me[0]) +
                                                  (pl[2] -
                                                   me[2]) * (pl[2] -
                                                             me[2])) :
                 -1.0, (double) w->D, w->plain_min > MCT_FAR_M
                 || w->plain_min < 0.0f ? " - NO VISIBLE CONTACT" : "");
        }
    }
    if (w->released || !(w->dur > 0.2f))
        return;
    int rep = g_dtl.on && g_dtl.run.src == DTL_S_SWING
        && g_dtl.run.slot == w->slot && g_dtl.run.sid == w->sid
        && g_dtl.run.ent == w->h;
    if (!rep)
        return;
    float ph = PsyncPhase(w->h, w->sid);
    if (ph < 0.0f)
        return;
    float ct = ph * w->dur, rate;
    if (!w->reached && ct >= w->tc_s - 0.004f) {
        w->reached = 1;
        w->reached_at = now;
        const char *jn = "?";
        float hd =
            MctReachDist(w->h, SID_PLAYER, MduVicMelee((int) w->slot),
                           &jn), mb[16], pb[16];
        const float *me = PlayerLocator(mb), *pl = PsyncLoc(w->h, pb);        // (ps25: the weapon's segment when one is held)
        float d = me
            && pl ? __builtin_sqrtf((pl[0] - me[0]) * (pl[0] - me[0]) +
                                    (pl[2] - me[2]) * (pl[2] -
                                                       me[2])) : -1.0f;
        w->hd_reach = hd;
        w->rd_reach = d;        // (mct_reach_tick: the victim-decides reach test reads it)
        MCTLOG
            ("mct: slot %u's replica on its contact frame %+lld ms vs the planned START arrival: hand %.2f m from our %s, roots %.2f m (authored %.2f)%s",
             w->slot, (long long) ((int64_t) now - (int64_t) w->T),
             (double) hd, jn, (double) d, (double) w->D,
             // ps39 B3 (rec38a B [21653] 'hand 0.76 m ... roots 1.08 m (authored 1.08) - NO VISIBLE CONTACT', the pair's own line 10 frames later 'fist closest 0.12 m'):
             // the swing clip's last frame has the fist 0.6-0.8 m short by authoring (the -hit clip's first 35 ms carry it in) - with the roots on the authored
             // distance this line is no verdict; the pair's CONTACT line is.
             hd > MCT_FAR_M ? (d >= 0.0f
                               && __builtin_fabsf(d - w->D) <=
                               0.15f ?
                               " - the fist is not in yet at the swing's last frame (roots on the authored distance; the -hit pair's CONTACT line is the verdict)"
                               :
                               " - NO VISIBLE CONTACT (roots off the authored distance)")
             : "");
    }
    float wtce = (float) (int64_t) (w->T - w->t_arm), wtc =
        w->tc_s * 1000.0f;
    // the swing's own wind-up (melee_duel.c wup_ct): a SWING whose tce is >= 150 ms past the clip's contact follows the sender's profile (raise slower,
    // hold at the top, strike x1), closed loop on the read phase; else the old constant rate onto the START. t2r-npc-windup-tell.txt = the old rate.
    if (!w->reached && wtce - wtc >= 150.0f
        && GetFileAttributesA("t2r-npc-windup-tell.txt") ==
        INVALID_FILE_ATTRIBUTES) {
        float el = (float) (int64_t) (now - w->t_arm), want =
            WupCt(el + 50.0f, wtc, wtce - wtc) / 1000.0f;
        rate = (want - ct) / 0.05f;
        rate = rate < 0.02f ? 0.02f : rate > 1.4f ? 1.4f : rate;
    } else if (!w->reached) {
        float rem = (float) (int64_t) (w->T - now) / 1000.0f;
        rate = rem > 0.02f ? (w->tc_s - ct) / rem : 1.0f;
        rate = rate < 0.35f ? 0.35f : rate > 1.4f ? 1.4f : rate;
    }
    // (the swinger's puppet stood 5 frames on its contact pose on every dodged swing): while OUR dodge clip runs no START can come
    // for this swing (we are immune), so the replica does not wait - it swings through. t2r-mct-hold-on-dodge.txt = the old wait.
    else if (now < w->T + MCT_WAIT_MS && !w->taken && !w->plain
             && !(MdgOwnClipOn()
                  && GetFileAttributesA("t2r-mct-hold-on-dodge.txt") ==
                  INVALID_FILE_ATTRIBUTES)) {
        rate = MCT_HOLD_RATE;
        w->held++;
    } else {
        rate = 1.0f;
        w->released = 1;
        uint64_t end =
            now + (uint64_t) ((w->dur - ct) * 1000.0f >
                              200.0f ? (w->dur - ct) * 1000.0f -
                              150.0f : 50.0f);
        g_mhr.pd[w->slot].end = end;
        g_dtl.run.end = end;
        MCTLOG
            ("mct: slot %u's replica waited %d frames on its contact - %s: it plays its follow-through (a miss on this screen)",
             w->slot, w->held,
             w->
             plain ? "a plain blow whose -hit clip could not start" :
             "no START");
    }
    if (!w->released) {
        uint64_t keep = now + 250;
        if (g_mhr.pd[w->slot].ent == w->h
            && g_mhr.pd[w->slot].sid == w->sid
            && g_mhr.pd[w->slot].end < keep)
            g_mhr.pd[w->slot].end = keep;
        if (g_dtl.run.end < keep)
            g_dtl.run.end = keep;
    }
    if (w->nset < 4 || __builtin_fabsf(rate - w->rate) > 0.015f) {
        DTL_SPEED(w->h, w->sid, rate);
        w->rate = rate;
        w->nset++;
    }                           // (re-set on the first frames: a speed set on the layer's first frame does not stick)
}

static void MctMeasureTick(void)
{
    typeof(&g_mct.m) m = &g_mct.m;
    int run = (g_ps.role == 1 || g_ps.role == 2) && MctDuelPair()
        && g_ps.ent_att && g_ps.ent_vic;
    double t = PsyncMs();
    if (run && g_ps.uid != m->uid) {
        memset(m, 0, sizeof *m);
        if (g_mct.pend.uid == g_ps.uid) {
            m->rep_ok = 1;
            m->rep_held = g_mct.pend.held;
            m->rep_delta = g_mct.pend.delta;
            m->warp = g_mct.pend.warp;
        }
        m->uid = g_ps.uid;
        m->role = g_ps.role;
        m->t0 = t;
        m->ct[0] = m->ct[1] = -1.0f;
        m->dmin = -1.0f;
        m->dur[0] = PsyncDur(g_ps.ent_att, g_ps.att_sid);
        m->dur[1] = PsyncDur(g_ps.ent_vic, g_ps.vic_sid);
        float ca[8], cv[8];
        m->auth = PsyncOffset(g_ps.ent_att, g_ps.att_sid, ca)
            && PsyncOffset(g_ps.ent_vic, g_ps.vic_sid,
                            cv) ? __builtin_sqrtf((ca[0] -
                                                   cv[0]) * (ca[0] -
                                                             cv[0]) +
                                                  (ca[2] -
                                                   cv[2]) * (ca[2] -
                                                             cv[2])) :
            -1.0f;
    }
    if (!m->uid || m->done)
        return;
    if (!run || g_ps.uid != m->uid) {
        m->done = 1;
        MCTLOG
            ("mct: u%08x r%d pair ended %.0f ms in before both impact frames (attacker %s, victim %s)",
             m->uid, m->role, t - m->t0, m->ai > 0.0 ? "reached" : "not",
             m->vi > 0.0 ? "reached" : "not");
        return;
    }
    for (int k = 0; k < 2; k++) {
        uint64_t ent = k ? g_ps.ent_vic : g_ps.ent_att, sid =
            k ? g_ps.vic_sid : g_ps.att_sid;
        float ph = PsyncPhase(ent, sid);
        if (ph < 0.0f || !(m->dur[k] > 0.0f))
            continue;
        float ct = ph * m->dur[k] * 1000.0f;
        if (t - m->t0 < 400.0) {
            if (m->ct[k] >= 0.0f && ct - m->ct[k] < 1.0f
                && ct < m->dur[k] * 1000.0f - 20.0f) {
                m->still[k]++;
                if (++m->run[k] > m->runmax[k])
                    m->runmax[k] = m->run[k];
            } else
                m->run[k] = 0;
        }
        m->ct[k] = ct;
        if (k == 0 && ct <= MCT_WIN_MS) {       // round 2: every frame of the impact window - the closest hand -> victim is the contact frame, wherever it sits
            const char *jn = "?";
            float hd = MctReachDist(g_ps.ent_att, g_ps.ent_vic, g_ps.weapon, &jn);    // (ps25: the weapon's segment when one is held)
            float a[16], v[16];
            const float *pa = PsyncLoc(g_ps.ent_att, a), *pv =
                PsyncLoc(g_ps.ent_vic, v);
            float rd0 = pa
                && pv ? __builtin_sqrtf((pa[0] - pv[0]) * (pa[0] - pv[0]) +
                                        (pa[2] - pv[2]) * (pa[2] -
                                                           pv[2])) : -1.0f;
            if (hd >= 0.0f && (m->dmin < 0.0f || hd < m->dmin)) {
                m->dmin = hd;
                m->jn = jn;
                m->cmin = ct;
                m->rmin = rd0;
            }
            if (m->ai <= 0.0 && ct >= MCT_IMPACT_MS) {
                m->ai = t;
                m->d35 = hd;
                m->j35 = jn;
                m->roots = rd0;
            }
        } else if (k == 0 && m->ai <= 0.0 && ct >= MCT_IMPACT_MS) {
            m->ai = t;
            m->d35 = -1.0f;
            m->roots = -1.0f;
        }
        if (k == 1 && m->vi <= 0.0 && ct >= MCT_IMPACT_MS)
            m->vi = t;
    }
    if ((m->ai > 0.0 && m->vi > 0.0 && m->ct[0] > MCT_WIN_MS)
        || t - m->t0 > 700.0) {
        m->done = 1;
        float rd = m->ai > 0.0
            && m->vi > 0.0 ? (float) (m->vi - m->ai) : 9999.0f;
        g_mct.n_blows++;
        g_mct.still_sum += (uint32_t) (m->still[0] + m->still[1]);
        if (m->dmin >= 0.0f) {
            g_mct.dist_sum += m->dmin;
            if (m->dmin > MCT_FAR_M)
                g_mct.n_far++;
        }
        if (rd < 9000.0f)
            g_mct.react_sum += __builtin_fabsf(rd);
        char rep[200] = "";
        if (m->role == 2) {
            if (m->rep_ok)
                snprintf(rep, sizeof rep,
                         "; START %+.0f ms vs the replica's contact frame (%d frames hit-stopped there), warp %.2f m",
                         (double) m->rep_delta, m->rep_held,
                         (double) m->warp);
            else
                snprintf(rep, sizeof rep,
                         "; no replica / warp for this blow");
        } else if (g_ps.hs_rate > 0.0f)
            snprintf(rep, sizeof rep,
                     "; hit-confirm wait %.0f ms as one pair at x%.2f",
                     (double) (g_ps.vlag * 1000.0f),
                     (double) g_ps.hs_rate);
        else if (g_ps.vlag > 0.0f)
            snprintf(rep, sizeof rep,
                     "; hit-confirm wait %.0f ms FROZEN (opt-out / no hit-stop)",
                     (double) (g_ps.vlag * 1000.0f));
        MCTLOG
            ("mct: u%08x r%d %s CONTACT: %s closest %.2f m to the victim's %s at +%.0f ms clip (roots %.2f m there), %.2f m (%s) at the +%.0f ms impact frame, roots %.2f m (authored %.2f); victim impact %+.0f ms vs the attacker's; frames not advancing in the first 400 ms: attacker %d (longest %d), victim %d (longest %d)%s%s",
             m->uid, m->role, PsyncName(g_ps.vic_sid), g_ps.weapon
             && g_ps.weapon !=
             FNV64("abby-unarmed") ? "weapon hand" : "fist",
             (double) m->dmin, m->jn ? m->jn : "?", (double) m->cmin,
             (double) m->rmin, (double) m->d35, m->j35 ? m->j35 : "?",
             (double) MCT_IMPACT_MS, (double) m->roots, (double) m->auth,
             (double) rd, m->still[0], m->runmax[0], m->still[1],
             m->runmax[1], rep,
             m->dmin > MCT_FAR_M ? " - NO VISIBLE CONTACT" : "");
    }
}

// (a free dodge carried us 1.3 m back; slot 0's fist then 'landed' - hp 85 -> 70 - while its replica's hand stood 1.45 m
// from our head at its contact frame; the hit did not look like it connected at all). The VICTIM decides: a duel blow whose replica, on its contact frame on OUR screen,
// is farther than MCT_FAR_M (the existing 'no visible contact' reach) from our head and gets no pair START inside MCT_WAIT_MS is a MISS here - no damage, no reaction,
// and the attacker is told through the dodge verdict (MSG_DODGE_RES: it plays its miss). The damage may arrive before the replica reaches its contact: recv_hit_hook
// holds it (cap MCT_REACH_HOLD_MS [guess: the replica contact is <= ~70 ms ahead of a hit + MCT_WAIT_MS 90 + fade slack]) and replays it from mct_tick with the verdict.
// A pair START (taken) / a near hand / no replica = applied as before. t2r-duel-no-reach-miss.txt = off.
#define MCT_REACH_HOLD_MS 400   // [guess]
static struct {
    int on;
    uint32_t slot;
    uint64_t me, att, w, ammo, t0;
    float dmg;
    uint8_t flag;
} g_mrh;
static int g_mrhReplay;
static int MctReachOff(void)
{
    static int off = -1;
    static uint64_t at;
    uint64_t n = GetTickCount64();
    if (off < 0 || n - at >= 2000) {
        at = n ? n : 1;
        int o =
            GetFileAttributesA("t2r-duel-no-reach-miss.txt") !=
            INVALID_FILE_ATTRIBUTES;
        if (o != off)
            Xlog("mct: reach miss %s",
                 o ? "OFF (t2r-duel-no-reach-miss.txt)" :
                 "ON - a duel blow whose replica stands farther than the reach from our head on its contact frame (no pair START) is a miss on our screen: no damage, the attacker is told");
        off = o;
    }
    return off;
}

// recv_hit_hook, after the dodge ruling: 1 = the blow is held (mct_reach_tick applies or refuses it)
static int MctReachHold(uint64_t me, uint64_t att, uint64_t w,
                          uint64_t ammo, float dmg, uint8_t flag)
{
    if (g_mrhReplay || g_mrh.on || !(dmg > 0.0f) || dmg >= 200.0f
        || !MctOn() || MctReachOff() || !MhsIsBlow(w)
        || MdgIsThrown(w))
        return 0;
    uint32_t slot = g_lastHitSlot;
    typeof(&g_mct.w) rw = &g_mct.w;
    if (slot >= T2R_MAX_PEERS || g_mdu.slot != (int) slot || !rw->on
        || rw->slot != slot || rw->taken || BqHitForced(slot))
        return 0;
    if (rw->reached && rw->hd_reach <= MCT_FAR_M)
        return 0;               // the hand showed contact
    g_mrh.on = 1;
    g_mrh.slot = slot;
    g_mrh.me = me;
    g_mrh.att = att;
    g_mrh.w = w;
    g_mrh.ammo = ammo;
    g_mrh.dmg = dmg;
    g_mrh.flag = flag;
    g_mrh.t0 = GetTickCount64();
    return 1;
}

static void MctReachTick(uint64_t now)
{
    if (!g_mrh.on)
        return;
    typeof(&g_mct.w) rw = &g_mct.w;
    int live = MctOn() && g_mdu.slot == (int) g_mrh.slot && rw->on
        && rw->slot == g_mrh.slot && !rw->taken;
    // the hand alone is too strict - a blow with the bodies at their authored spacing (roots 0.75 / authored 0.79) showed the hand 0.54 m off
    // (the replica 118 ms late). Refuse only when the BODIES are also clearly apart: roots > authored contact + MCT_REACH_ROOT_M (44808: 1.39 vs 0.76).
    int outr = live && rw->reached && rw->hd_reach > MCT_FAR_M
        && rw->rd_reach > rw->D + MCT_REACH_ROOT_M;
    if (live && !(outr && rw->released)
        && now - g_mrh.t0 < MCT_REACH_HOLD_MS && !(rw->reached && !outr))
        return;                 // reached and not clearly apart = apply now   // still waiting for the contact frame / the START
    typeof(g_mrh) h = g_mrh;
    g_mrh.on = 0;
    if (outr && rw->released) {
        Xlog("DUEL: blow from slot %u refused - out of reach on our screen (hand %.2f m / roots %.2f m, contact %.2f m): damage %.0f dropped, the attacker is told it whiffed", h.slot, (double) rw->hd_reach, (double) rw->rd_reach, (double) rw->D, (double) h.dmg);
        MdgSendRes();         // MSG_DODGE_RES: the attacker plays its miss (mdg_attacker_missed), both screens agree
        g_mdg.sw[h.slot].landed_ms = now;
        return;
    }
    uint8_t *pe = MdgPeer(h.slot);
    uint64_t ph = pe ? *(uint64_t *) (pe + 0x60) : 0;
    uint8_t *pp =
        ph ? ((uint8_t * (*)(uint64_t)) T2R_SYM(kFnEntityFromHandle)) (ph)
        : NULL;
    uint64_t att = pp ? (uint64_t) (uintptr_t) pp : h.att;
    g_mrhReplay = 1;
    g_lastHitSlot = h.slot;
    RecvHitHook(h.me, att, h.w, h.ammo, h.dmg, h.flag);
    g_mrhReplay = 0;
}

static void MctTick(void)
{
    if (!MctOn()) {
        g_mct.w.on = 0;
        MctReachTick(GetTickCount64());
        return;
    }
    uint64_t now = GetTickCount64();
    if (!g_mct.sum_at)
        g_mct.sum_at = now;
    MctReplicaTick(now);
    MctReachTick(now);
    MctMeasureTick();
    if (now - g_mct.sum_at >= MCT_SUM_MS) {
        if (g_mct.n_blows || g_mct.n_rep || g_mct.n_nostart)
            Xlog("mct: 30 s summary - %u blows: hand -> victim closest in the impact window avg %.2f m, %u farther than %.2f m (no visible contact); |victim impact - attacker impact| avg %.0f ms; %u frames of a half not advancing; " "victim screen: %u STARTs avg |%.0f| ms off the replica's contact frame, %u replicas without a START", g_mct.n_blows, g_mct.n_blows ? g_mct.dist_sum / g_mct.n_blows : 0.0, g_mct.n_far, (double) MCT_FAR_M, g_mct.n_blows ? g_mct.react_sum / g_mct.n_blows : 0.0, g_mct.still_sum, g_mct.n_rep, g_mct.n_rep ? g_mct.rep_sum / g_mct.n_rep : 0.0, g_mct.n_nostart);
        g_mct.n_blows = g_mct.n_far = g_mct.n_rep = g_mct.n_nostart =
            g_mct.still_sum = 0;
        g_mct.dist_sum = g_mct.react_sum = g_mct.rep_sum = 0.0;
        g_mct.sum_at = now;
    }
}
