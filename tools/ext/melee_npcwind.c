// melee_npcwind.c - NPC WINDUP TELL.
// SP's human enemies announce a blunt-weapon blow with their own windup clip (the weapon is raised, ~0.6-0.9 s before the blow): the anim-melee-npc-t2 pak's
//   npc-f-<1hblunt|2hblunt>-tell^attack-<rt|lt>-foot-3m-<downward-swing|upward-swing|horiz-sweep>-a-enter   (0.60-0.97 s, get-animation-duration in MP)
// The pak is resident in MP (dc_data_patches.py PARRY_SP_COMMON_PAKS) and the clips play on the player rig (verified with a probe).
// Every earlier "windup" only slowed the player's own swing clip (it only played the same animation, longer, so the hit was delayed) or used SP
// MOVE names, which are not clip names (get-animation-duration 0).
// v1: the OPENER of a 1h-blunt / 2h chain against a player (start-melee-action of a step-1 light row) is held: the NPC -enter clip plays on our body at
// NPCW_RATE, the same clip on our puppet on every other screen (MSG_DODGE_SWING, kind bit 7), then the engine swing starts with the saved arguments. Combo
// follow-ups, fists, knives, blades (no NPC -enter clip in the pak), sprint / dodge attacks and finishers are untouched.
// Opt-out t2r-no-npc-windup.txt. NPCW_RATE is a TUNING value (not measured; tells should show faster than they do in SP).
// v2: FISTS and the KNIFE get their tell from the two boss fights -
//   fists: NPC Abby's punch openers of the Santa Barbara fight, ellie-vs-abby-punch-<right|left>-lt-ft-fwd-open-tell (anim-melee-ellie-abby-fight; bound to
//          abby-prisoner-skel, which dc_data_patches.PARRY_PRISONER_SKEL now lists in the MP level - without it the clips do not resolve and fists keep their start)
//   knife: NPC Ellie's slash opener of the theater fight, npc-ellie-knife-combo-slash-1-tell (anim-melee-abby-ellie-fight, 0.233 s in MP)
// The fist tell (0.533 / 0.600 s authored, 13 + 27 openers held 533 ms in a 70 s bot round, both screens) runs at NPCW_RATE like the blunt tells
// (395 / 444 ms), the knife tell (0.233 s) at its authored rate. Opt-outs t2r-no-npc-windup-fist.txt / t2r-no-npc-windup-knife.txt.
// NOT RUN: the knife tell - the bot's "weapon knife" (knife-survival-mel-player) swings the fist rows, no switchblade row started in the test rounds.
#define NPCW_RATE 1.35f
#define NPCW_RATE_BOSS 1.0f
enum { NPCW_1HB, NPCW_2HB, NPCW_FIST, NPCW_KNIFE, NPCW_NCLS };
#define NPCW_BLEND 0.12f
// v3 (windups always happen: quick enough that the attacker is not at a disadvantage for meleeing, but the victim should
// still be able to tell a windup; before, they activated only on every 2nd baseball hit and never for fists or knife):
//   - EVERY light / far blow gets its tell, not only the chain opener (step 1: the bat's follow-up had none = every 2nd hit)
//   - every tell is time-scaled to NPCW_TELL_MS (TUNING, not measured: short enough not to punish the attacker, long enough to read), rate capped at NPCW_RATE_MAX
//   - MALE bodies get fist tells: the Abby punch tells stay female-only (abby-prisoner-skel rig = A-pose on base-male), a male body plays NPC-f's open-struggle
//     punch tell (npc-fist-open-struggle-front-enter-tell-hr-atk-rt / -lt, base-female data like the blunt tells, which pose a male body correctly). The knife
//     swings the fist rows in MP, so it takes the fist tell too.
//   - 1h blades (machete / hatchet: no NPC -enter tell in the pak) take the 1h blunt tells
//   - the Abby punch tell A-poses a FEMALE body too on this build (no prisoner
//     retargets shipped), so v3 plays the npc-f open-struggle tell on every body; t2r-npc-windup-abby-fist.txt = the Abby tells on female bodies again.
// t2r-npc-windup-v2.txt = the v2 rules (openers only, own rates, no male fist / blade tells).
#define NPCW_CONTACT_MS ((float)Mw45ContactMs())   /* 330 ms (melee_dodge.c mw45_contact_ms; t2r-windup-330-ps45-old.txt = 380) */   /* (MP turns quicker; SP fists 267 / 300 / 334 / 384 / 434): was 460 */  // [TUNING] ms from the swing start to the contact for every light swing (SP: 267-467 by clip; the defender needs one reaction time + latency)
#define NPCW_TELL_MS 350.0f
#define NPCW_TELL_COMBO_MS 250.0f       // [TUNING] combo steps 2-4: shorter tell (the chain stays quick, still readable)
#define NPCW_RATE_MAX 2.4f
#define NPCW_TURN_DPS 540.0f    // [TUNING] deg/s: the tell's turn onto its target (a 180 deg turn in 0.33 s = the tell's length)
#define NPCW_STANDOFF 0.85f     // m: the tell's root travel stops this far from the target (the blows' own contact spots are 0.75-0.91 m)
static int NpcwV2(void)
{
    static int v = -1;
    static uint64_t at;
    uint64_t n = GetTickCount64();
    if (v < 0 || n - at > 2000) {
        at = n;
        v = GetFileAttributesA("t2r-npc-windup-v2.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    return v;
}

static int g_npcwTurn;         // the tell turns our body onto its target (npcw_tick)
static float g_npcwYawPrev;
static uint64_t g_npcwYawF;   // our facing (deg) on the last frame without a tell (npcw_tick)
static struct {
    int on, var, slot;
    uint64_t t0, end, sid, move, skip_until, last_end;
} g_npcw;
static const char *const NPCW_CLIP[NPCW_NCLS][4] = {
    { "npc-f-1hblunt-tell^attack-rt-foot-3m-downward-swing-a-enter",
     "npc-f-1hblunt-tell^attack-rt-foot-3m-horiz-sweep-a-enter",
     "npc-f-1hblunt-tell^attack-lt-foot-3m-upward-swing-a-enter" },
    { "npc-f-2hblunt-tell^attack-rt-foot-3m-downward-swing-a-enter",
     "npc-f-2hblunt-tell^attack-rt-foot-3m-horiz-sweep-a-enter",
     "npc-f-2hblunt-tell^attack-lt-foot-3m-downward-swing-a-enter" },
    { "ellie-vs-abby-punch-right-lt-ft-fwd-open-tell", "ellie-vs-abby-punch-left-lt-ft-fwd-open-tell--abby",    // var 0 = a right punch row, 1 = a left one (female)
     "npc-fist-open-struggle-front-enter-tell-hr-atk-rt--npc-f", "npc-fist-open-struggle-front-enter-tell-hr-atk-lt--npc-f" },  // var 2 / 3 = the same on a male body (v3)
    // No windup was visible in a test: the 0.233 s slash-1 tell fired twice on A and was not seen at all.
    // The opener now takes NPC Ellie's FAR slash tell (0.633 s, the same slash from range); t2r-npc-windup-knife-short.txt = the 0.233 s one.
    // var 2 of this class is not a tell - it is SP's far finisher transfer clip (melee_duel.c mdu_p31_run_begin sends class 3 var 2 with the run-in's
    // length; npcw_note never picks var 2). Older clients have NULL here and drop the message.
    { "npc-ellie-knife-combo-slash-1-far-tell",
     "npc-ellie-knife-combo-slash-1-tell", "player-kill-close-distance" },
};

// (the boss tells must READ as a wind-up - fists back at the authored 0.533 / 0.600 s, x1.35 = 395 ms is the old pace: t2r-npc-windup-fist-fast.txt)
static float NpcwRate(int cls)
{
    if (cls == NPCW_FIST)
        return GetFileAttributesA("t2r-npc-windup-fist-fast.txt") !=
            INVALID_FILE_ATTRIBUTES ? NPCW_RATE : NPCW_RATE_BOSS;
    return cls == NPCW_KNIFE ? NPCW_RATE_BOSS : NPCW_RATE;
}

static int NpcwClsOff(int cls)
{
    static int v[2] = { -1, -1 };
    static uint64_t at;
    uint64_t n = GetTickCount64();
    if (cls < NPCW_FIST)
        return 0;
    if (v[0] < 0 || n - at > 2000) {
        at = n;
        v[0] =
            GetFileAttributesA("t2r-no-npc-windup-fist.txt") !=
            INVALID_FILE_ATTRIBUTES;
        v[1] =
            GetFileAttributesA("t2r-no-npc-windup-knife.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    return v[cls - NPCW_FIST];
}

static int NpcwOff(void)
{
    static int v = -1;
    static uint64_t at;
    uint64_t n = GetTickCount64();
    if (v < 0 || n - at > 2000) {
        at = n;
        v = GetFileAttributesA("t2r-no-npc-windup.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    return v;
}

static void NpcwEnd(const char *why, int stop)
{
    if (!g_npcw.on)
        return;
    if (stop)
        MhrStop(SID_PLAYER, g_npcw.sid, 0.15f);
    PadinjBlock("melee-attack", 0);
    Xlog("npcwind: tell ended after %d ms (%s)",
         (int) (GetTickCount64() - g_npcw.t0), why);
    g_npcw.on = 0;
    g_npcw.last_end = GetTickCount64();
}

// t2r-ext.c feature_early_finisher, before mdu_swing_note: our melee move id changed. 1 = the swing was cancelled on its first frame and is held behind the
// tell (the engine starts swings natively, not through start-melee-action - the finisher windup of melee_duel.c takes the swing at the same point).
static int NpcwNote(int started, uint64_t move)
{
    if (!started || !move || g_npcw.on || NpcwOff())
        return 0;
    uint64_t now = GetTickCount64();
    if (now < g_npcw.skip_until)
        return 0;               // (the held swing itself, started by npcw_tick)
    int row = MduRowOf(move);
    if (row < 0)
        return 0;
    const MduRowT *m = &MDU_MOVES[row];
    int v3 = !NpcwV2();
    if (v3 ? (m->kind != MDUK_LIGHT && m->kind != MDUK_FAR)
        : (m->step != 1 || m->kind != MDUK_LIGHT))
        return 0;
    int cls = m->set == MDU_1HBLUNT || (v3
                                        && m->set ==
                                        MDU_1HBLADE) ? NPCW_1HB : m->set ==
        MDU_2H ? NPCW_2HB : m->set == MDU_FIST ? NPCW_FIST : m->set ==
        MDU_KNIFE ? NPCW_KNIFE : -1;
    if (cls < 0 || NpcwClsOff(cls))
        return 0;
    // (a fist round showed A-poses and teleporting): on a MALE body (base-male rig) the
    // fist tell is an A-pose for its whole length, on our screen and on our puppet - the Abby punch tells are abby-prisoner-skel data re-bound to base-female.
    // Only a female body plays the FIST tell (t2r-npc-windup-male.txt = also male, for a look at it); a male body keeps the plain fist start. The npc-f blunt
    // tells are genuine base-female clips and pose a male body correctly (militia-male-05 with the pipe) - not gated.
    int male_fist = cls == NPCW_FIST && !OwnFemale()
        && GetFileAttributesA("t2r-npc-windup-male.txt") ==
        INVALID_FILE_ATTRIBUTES;
    if (male_fist && !v3)
        return 0;
    // (15 of 23 combo blows 2-4 had no tell): the next combo press starts the step while our own -hit pair of the last blow is still
    // blending out (psync own), and that refused the tell. Our own duel pair in its blend-out does not block it (t2r-npc-windup-combo-off.txt = old).
    // gate trace (7 swings with no tell: "psync_own 1 role 1 tail 0"): an OPENER (step 1) pressed while our own last duel pair blends out was refused too - any step now.
    int own_pair_tail = PsyncOwn() && g_mdu.on && g_ps.role == 1
        && g_ps.uid == g_mdu.ps_uid
        && GetFileAttributesA("t2r-npc-windup-combo-off.txt") ==
        INVALID_FILE_ATTRIBUTES;
    // ("stale sw_lethal 1"): g_mdu.sw_lethal still held the LAST swing's verdict (it is set for the new swing only after this, in
    // mdu_swing_note) - a stale lethal flag skipped the tell of the next blow. Only a lethal swing still running blocks it.
    int lethal_now = g_mdu.sw_lethal && g_mdu.sw_on && !g_mdu.sw_done;
    // (A clubbed B three times while B still slid down the wall): a contextual takedown
    // releases its attacker 1.7 s before the victim half ends; every swing in that gap hit the helpless victim with no tell. The victim half is the takedown's
    // outcome (it ends downed): a swing at it is cancelled until it ends. t2r-takedown-free-hits.txt = the old free swings.
    if (PsyncOwn() && g_ps.role == 1 && g_ps.kind == PS_CTXDOWN
        && !MduMove(g_ps.move) && g_ps.vic >= 0
        && g_ps.vic < T2R_MAX_PEERS && !g_ps.out_done[1]
        && PsyncMs() - g_ps.t0 < g_ps.vic_dur * 1000.0
        && GetFileAttributesA("t2r-takedown-free-hits.txt") ==
        INVALID_FILE_ATTRIBUTES) {
        uint64_t me0 = SID_PLAYER;
        DcCall(0x394888944e949d5fULL /* cancel-melee-action */ , 1, &me0);
        static int nlf;
        if (LOGCAP(nlf, 30))
            Xlog("npcwind: %s cancelled - the takedown victim (slot %d) is still in its half (%.0f of %.0f ms): no free hits on it - FALLBACK: the ps39 press latch (melee_duel.c mdu_press_latched) should have kept this swing from starting", m->move, g_ps.vic, PsyncMs() - g_ps.t0, g_ps.vic_dur * 1000.0);
        return 1;
    }
    if (g_downed || g_downDying || (PsyncOwn() && !own_pair_tail) || MdgOwnClipOn() || g_mdfwu.on || lethal_now) { // our body is in a pair / a dodge / down; a lethal swing keeps its own flow
        // swings with no tell, 3 of them lethal - which gate (trace before the fix)
        static int nl;
        if (LOGCAP(nl, 60))
            Xlog("npcwind: %s step %d NO tell - gate: downed %d dying %d psync_own %d (role %d, tail %d) own dodge clip %d fin-windup %d stale sw_lethal %d (duel %d)", m->move, m->step, (int) g_downed, (int) g_downDying, PsyncOwn(), g_ps.role, own_pair_tail, MdgOwnClipOn(), (int) g_mdfwu.on, (int) g_mdu.sw_lethal, (int) g_mdu.on);
        return 0;
    }
    // the target: the duel partner, else the nearest enemy puppet inside 3.5 m (a swing at nobody keeps its SP time)
    float mb[16], pb[16];
    const float *me = PlayerLocator(mb);
    int slot = g_mdu.on ? g_mdu.slot : -1;
    float bd = 3.5f;
    if (slot < 0 && me)
        for (uint32_t s = 0; s < T2R_MAX_PEERS; s++) {
            uint64_t ph = PuppetHandle(s);
            if (!ph || IsTeammate(s) || g_peerDowned[s])
                continue;
            const float *pl = PuppetLocator(ph, pb);
            if (!pl)
                continue;
            float d =
                __builtin_sqrtf((pl[0] - me[0]) * (pl[0] - me[0]) +
                                (pl[2] - me[2]) * (pl[2] - me[2]));
            if (d < bd) {
                bd = d;
                slot = (int) s;
            }
        }
    uint64_t h = slot >= 0 ? PuppetHandle((uint32_t) slot) : 0;
    if (!h || !g_startMeleeOrig)
        return 0;
    // The swing's own wind-up (melee_duel.c wup_ct): the engine swing is not held - mdu_swing_note slows its own clip's raise, holds at
    // the top and strikes at x1 over the same extra time. t2r-npc-windup-tell.txt = the NPC tell clip before the swing (the old v3 path below).
    if (GetFileAttributesA("t2r-npc-windup-tell.txt") ==
        INVALID_FILE_ATTRIBUTES) {
        // (SP run: contact is fixed per clip, fists 267 / 300 / 334 / 384 / 434 ms, blunt 467, blade 334 / 267; MP +350 / +250 gave 616 / 500-690 ms with a
        // standing hold): every swing is brought to ONE contact time NPCW_CONTACT_MS instead (a swing already that long plays SP's own speed). t2r-wup-add-old.txt = +350 / +250.
        if (GetFileAttributesA("t2r-wup-add-old.txt") !=
            INVALID_FILE_ATTRIBUTES)
            g_npcwX =
                m->step >
                1 ? (int) NPCW_TELL_COMBO_MS : (int) NPCW_TELL_MS;
        else
            g_npcwX = m->tc_ms > 0
                && m->tc_ms <
                (int) NPCW_CONTACT_MS - 15 ? (int) NPCW_CONTACT_MS -
                m->tc_ms : 0;
        g_npcwXMove = move;
        g_npcwTgtSlot = slot;
        g_npcwTgtMs = now;
        static int nw;
        if (LOGCAP(nw, 200))
            Xlog("npcwind: %s step %d on slot %d (%.2f m) - the swing's own wind-up +%d ms (no tell clip, no hold)", m->move, m->step, slot, (double) bd, g_npcwX);
        return 0;
    }
    int var =
        cls == NPCW_FIST ? (strstr(m->move, "left") != NULL) + (male_fist
                                                                || (v3
                                                                    &&
                                                                    GetFileAttributesA
                                                                    ("t2r-npc-windup-abby-fist.txt")
                                                                    ==
                                                                    INVALID_FILE_ATTRIBUTES)
                                                                ? 2 : 0) :
        cls ==
        NPCW_KNIFE ? (GetFileAttributesA("t2r-npc-windup-knife-short.txt")
                      != INVALID_FILE_ATTRIBUTES) : (int) (now / 7) % 3;
    const char *clip = NPCW_CLIP[cls][var];
    float len = AnimLen(SID_PLAYER, clip), rate = NpcwRate(cls);
    if (v3 && len > 0.0f) {
        rate =
            len * 1000.0f / (m->step >
                             1 ? NPCW_TELL_COMBO_MS : NPCW_TELL_MS);
        if (rate > NPCW_RATE_MAX)
            rate = NPCW_RATE_MAX;
        if (rate < 1.0f)
            rate = 1.0f;
    }
    if (!(len >= 0.2f && len <= 1.5f)) {
        static int nl;
        if (LOGCAP(nl, 20))
            Xlog("npcwind: %s is not loaded here (%.2f s) - the swing keeps its own start", clip, (double) len);
        return 0;
    }
    g_npcwKeep = 1;
    g_mdfwuCancelT60 = g_t60 ? g_t60 : 1;     // (the cancel's begin-holster must not stow our melee item, mdu_fw_keep)
    uint64_t me0 = SID_PLAYER;
    DcCall(0x394888944e949d5fULL /* cancel-melee-action */ , 1, &me0);
    uint64_t sid = Fnv64Rt(clip), r =
        MhrAnimate(SID_PLAYER, sid, 0.0f, NPCW_BLEND);
    if (!r || r == ~0ULL) {     // refused: the swing starts again at once
        Xlog("npcwind: %s refused by animate_ (%llx) - the swing restarts at once", clip, (unsigned long long) r);
        g_npcw.move = move;
        g_npcw.slot = slot;
        g_npcw.t0 = now;
        g_npcw.end = now;
        g_npcw.sid = 0;
        g_npcw.on = 1;
        return 1;
    }
    DTL_SPEED(SID_PLAYER, sid, rate);
    int ms = (int) (len * 1000.0f / rate);
    g_npcw.on = 1;
    g_npcw.var = var;
    g_npcw.slot = slot;
    g_npcw.t0 = now;
    g_npcw.end = now + (uint64_t) ms;
    g_npcw.sid = sid;
    g_npcw.move = move;
    PadinjBlock("melee-attack", g_npcw.end);
    uint8_t b[4] =
        { (uint8_t) (0x80 | (cls << 2) | var), 0, (uint8_t) (ms & 0xff),
 (uint8_t) (ms >> 8) };
    if (g_sendTramp)
        g_sendTramp(MSG_DODGE_SWING, b, 4, 0xff);
    // (the body flipped and the camera went through it): the engine's swing start had already
    // turned the body to the STICK direction (a strafing / circling player: 55-168 deg) on the frame we cancelled it for the tell. The facing of the frame
    // before is put back here; npcw_tick turns it onto the target over the tell. t2r-npcw-yaw-old.txt = keep the engine's snap.
    if (g_frame - g_npcwYawF <= 2
        && GetFileAttributesA("t2r-npcw-yaw-old.txt") ==
        INVALID_FILE_ATTRIBUTES) {
        float yb[16];
        const float *yl = PlayerLocator(yb);
        if (yl) {
            float dy =
                __builtin_remainderf(Uc4Yaw(yl) - g_npcwYawPrev,
                                     360.0f);
            if (__builtin_fabsf(dy) > 20.0f) {
                float yr = g_npcwYawPrev / 57.29578f, l8[8] =
                    { yl[0], yl[1], yl[2], 1.0f, 0.0f,
__builtin_sinf(yr * 0.5f), 0.0f, __builtin_cosf(yr * 0.5f) };
                static PsyncBf bfy;
                PsyncBfFromLoc(&bfy, l8);
                uint64_t ay[2] = { SID_PLAYER, (uint64_t) & bfy };
                MoWriteDc(ay, MO_SRC("npcwind.yaw_restore"), 0);
                Xlog("npcwind: the engine's swing start turned us %+.0f deg in one frame (stick direction) - facing put back, the tell turns onto the target", (double) dy);
            }
        }
    }
    g_npcwTurn = 1;
    Xlog("npcwind: %s opener on slot %d (%.2f m) held %d ms behind the NPC tell %s (%.2f s at x%.2f); the swing starts after it", m->move, slot, (double) bd, ms, clip, (double) len, (double) rate);
    return 1;
}

// melee_dodge.c mdg_recv_swing: 1 = the message was a tell (the swing's own announce follows when it starts)
static int NpcwRecv(uint32_t slot, const uint8_t *buf, uint64_t len)
{
    if (len < 4 || !buf || !(buf[0] & 0x80))
        return 0;
    if (NpcwOff() || slot >= T2R_MAX_PEERS)
        return 1;
    int cls = (buf[0] >> 2) & 3, var = buf[0] & 3, ms =
        buf[2] | (buf[3] << 8);
    uint64_t h = PuppetHandle(slot);
    if (var > 3 || !h || ms < 150 || ms > 1500 || !NPCW_CLIP[cls][var])
        return 1;
    if (cls == NPCW_FIST && var < 2 && !SlotFemale(slot)
        && GetFileAttributesA("t2r-npc-windup-male.txt") ==
        INVALID_FILE_ATTRIBUTES)
        return 1;               // (an Abby tell on a male puppet: A-pose, see npcw_note)
    const char *clip = NPCW_CLIP[cls][var];
    float len_s = AnimLen(h, clip), ow = PsyncOneway((int) slot), rate =
        NpcwRate(cls);
    if (!(len_s >= 0.2f)) {
        static int nl;
        if (LOGCAP(nl, 20))
            Xlog("npcwind: slot %u's tell %s is not loaded here", slot,
                 clip);
        return 1;
    }
    if (!NpcwV2()) {
        rate = len_s * 1000.0f / (float) ms;
        if (rate > NPCW_RATE_MAX)
            rate = NPCW_RATE_MAX;
        if (rate < 1.0f)
            rate = 1.0f;
    }                           // v3: the sender's tell length decides
    int run_in = cls == NPCW_KNIFE && var == 2; // ps31: the far finisher run-in (SP plays player-kill-close-distance at x1.00 and cuts it at the intro)
    if (run_in)
        rate = 1.0f;
    float ph = ow > 0.0f
        && ow < 300.0f ? ow * rate / (len_s * 1000.0f) : 0.0f;
    uint64_t sid = Fnv64Rt(clip), r =
        MhrAnimate(h, sid, ph, NPCW_BLEND), now = GetTickCount64();
    if (!r || r == ~0ULL)
        return 1;
    DTL_SPEED(h, sid, rate);
    g_mhr.pd[slot].ent = h;
    g_mhr.pd[slot].sid = sid;
    g_mhr.pd[slot].end =
        now + (uint64_t) (ms > (int) ow ? ms - (int) ow : 0);
    {
        uint8_t *pe = MdgPeer(slot);
        if (pe && pe[0x4c] && !IsTeammate(slot))
            MtellCue(slot, h, 0, "npc windup tell");
    }
    static int nl;
    if (LOGCAP(nl, 200))
        Xlog("npcwind: slot %u's %s %s on its puppet (%d ms, phase %.2f for %.0f ms one-way)", slot, run_in ? "FAR FINISHER RUN-IN (ps31)" : "NPC tell", clip, ms, (double) ph, (double) ow);
    return 1;
}

// per frame (t2r_tick): the held swing starts at the tell's end
static void NpcwTick(void)
{
    WupTick();                 // melee_duel.c: the swing's own wind-up speed profile
    {
        float yb[16];
        const float *yl = g_npcw.on ? NULL : PlayerLocator(yb);
        if (yl) {
            g_npcwYawPrev = Uc4Yaw(yl);
            g_npcwYawF = g_frame;
        }
    }
    if (!g_npcw.on) {
        if (g_npcwKeep && GetTickCount64() - g_npcw.last_end > 250)
            g_npcwKeep = 0;
        return;
    }
    uint64_t now = GetTickCount64();
    int tail = PsyncOwn() && g_mdu.on && g_ps.role == 1 && g_ps.uid == g_mdu.ps_uid && GetFileAttributesA("t2r-npc-windup-combo-off.txt") == INVALID_FILE_ATTRIBUTES;  // (our own last duel pair blending out under a combo tell)
    if (g_downed || g_downDying || (PsyncOwn() && !tail)
        || MdgOwnClipOn()) {
        NpcwEnd("our body was taken (hit / dodge / down)", !PsyncOwn());
        return;
    }
    uint64_t h = PuppetHandle((uint32_t) g_npcw.slot);
    if (!h) {
        NpcwEnd("the target is gone", 1);
        return;
    }
    // the 1h / 2h blunt tells are the NPC "3m" lunge -enter clips, at x2.4 their root carried the
    // attacker 1.7 m - through the victim (bodies overlap 32.30-32.50), the swing then started 0.5 m past it, turned 176 deg in 100 ms and the lock-on camera
    // whipped onto our own face. The tell's travel stops at NPCW_STANDOFF from the target (the body never passes it). t2r-npcw-standoff-old.txt = off.
    if (now < g_npcw.end) {
        static int so = -1;
        static uint64_t sochk;
        if (so < 0 || now - sochk > 3000) {
            sochk = now;
            so = GetFileAttributesA("t2r-npcw-standoff-old.txt") !=
                INVALID_FILE_ATTRIBUTES;
        }
        float mb[16], pb[16];
        const float *me = PlayerLocator(mb), *pl =
            me ? PuppetLocator(h, pb) : NULL;
        if (me && pl && g_npcwTurn) {  // turn onto the target over the tell (NPCW_TURN_DPS cap per frame; a frame ~16 ms)
            float want =
                __builtin_atan2f(pl[0] - me[0], pl[2] - me[2]) * 57.29578f,
                cur = Uc4Yaw(me), dy =
                __builtin_remainderf(want - cur, 360.0f), mx =
                NPCW_TURN_DPS / 60.0f;
            if (__builtin_fabsf(dy) < 2.0f)
                g_npcwTurn = 0;
            else {
                float ny =
                    (cur +
                     (dy > mx ? mx : dy < -mx ? -mx : dy)) / 57.29578f,
                    l8[8] =
                    { me[0], me[1], me[2], 1.0f, 0.0f,
                 __builtin_sinf(ny * 0.5f), 0.0f,
                 __builtin_cosf(ny * 0.5f) };
                static PsyncBf bft;
                PsyncBfFromLoc(&bft, l8);
                uint64_t at[2] = { SID_PLAYER, (uint64_t) & bft };
                MoWriteDc(at, MO_SRC("npcwind.turn"), 0);
                me = PlayerLocator(mb);
            }
        }
        if (so)
            me = NULL;
        if (me && pl) {
            float dx = me[0] - pl[0], dz = me[2] - pl[2], d =
                __builtin_sqrtf(dx * dx + dz * dz);
            if (d < NPCW_STANDOFF && d > 0.05f) {
                static PsyncBf bf;
                float t8[8];
                memcpy(t8, me, 32);
                t8[0] = pl[0] + dx / d * NPCW_STANDOFF;
                t8[2] = pl[2] + dz / d * NPCW_STANDOFF;
                PsyncBfFromLoc(&bf, t8);
                uint64_t a[2] = { SID_PLAYER, (uint64_t) & bf };
                MoWriteDc(a, MO_SRC("npcwind.standoff"), 0);
                static int nl;
                if (LOGCAP(nl, 40))
                    Xlog("npcwind: tell travel stopped at %.2f m from slot %d (was %.2f m) - no walking through the target", (double) NPCW_STANDOFF, g_npcw.slot, (double) d);
            }
        }
        return;
    }
    PadinjBlock("melee-attack", 0);
    if (g_npcw.sid)
        MhrStop(SID_PLAYER, g_npcw.sid, 0.15f);        // the swing clip takes the body from the raised pose
    uint64_t ret[2] = { 0, 0 }, args[8] =
        { SID_PLAYER, h, g_npcw.move, 0, 0, 0, 0, 0 };
    if (g_maHave)
        for (int i = 3; i < 8; i++)
            args[i] = g_maCache[i];
    g_npcw.skip_until = now + 400;
    g_npcwTgtSlot = g_npcw.slot;
    g_npcwTgtMs = now;
    if (g_startMeleeOrig)
        g_startMeleeOrig(ret, 8, args, 0);
    Xlog("npcwind: tell over (%d ms) - the held swing starts -> %llx%s",
         (int) (now - g_npcw.t0), (unsigned long long) ret[0],
         ret[0] ? "" : " (refused: a pad press is injected)");
    if (!ret[0])
        PadinjPress("melee-attack", 120);
    g_npcw.on = 0;
    g_npcw.last_end = now;
}
