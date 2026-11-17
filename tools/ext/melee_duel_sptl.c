// melee_duel_sptl.c - DUEL SP TIMELINE (sptl): every half of a duel exchange plays at SP's time on BOTH screens, and every exchange is logged
// against SP's measured timeline (re/melee-sp-timeline.md, from the SP sandbox recorder runs). Every half is played as clips, mocked up the way SP plays them and when; hits connect just like SP.
// Included after melee_duel_contact.c in t2r-ext.c. Log prefix "sptl:". Opt-OUT: t2r-duel-no-sptl.txt next to the exe (re-read every 2 s); it only acts
// while the duel is on (t2r-melee-duel.txt). Duel rules / damage / dodge windows / verdicts / hand-off times are untouched: only WHEN a half or a cue shows.
//
// What SP does (re/melee-sp-timeline.md, VERIFIED rows): press = swing MSTART (0-10 ms); the swing clip blends in over ~125 ms; at tc (fists 257-461 ms) the
// attacker's move switches to <swing>-hit and the victim's reaction starts ON THE SAME FRAME; the -hit clip is at full weight on its first frame (fade
// 1.000 within 3-5 ms, no blend); the impact (melee-effect + impact sfx) is 32-45 ms after the switch; pain vox +3..+5; effort vox -190..-400 / swish
// -43..-170 before the switch, on the swing clip's own frames. Finishers: the intro plays on the attacker alone, the -sync pair starts at its end, the main
// clip at full weight at once (7/7 finishers).
// What we did (A/B v5-int at ~200 ms RTT, lpslog4): see re/melee-sp-timeline.md part 2. Fixed here:
//  1 ENGINE -HIT  (attacker screen) the engine's own contact (dropped) switched our swing to its -hit clip at SP time while the duel's contact (windup slowed
//                 by the one-way latency) came ~110 ms later and started the -hit half again from frame 0: the impact played twice, the fist jumped. Now the
//                 engine's early -hit move is cancelled on its first frame and the swing clip continues from its own clip time at the windup speed
//                 (t2r-sptl-no-swing-restore.txt = off).
//  2 -HIT CUT     (attacker screen) our -hit half started with the pair's 0.20 s pull as animate_ blend: at the impact frame the fist pose was a mix of the
//                 swing and the -hit. SP: full weight on the switch frame. Now blend 0 (the swing at its switch frame = the -hit's first frame, authored),
//                 unless the engine's -hit could not be undone (then the old blend).
//  3 IMPACT SOUND (attacker screen) the victim's pain / impact / second pain fired at contact +4 / +35 / +40 while the hit-confirm wait (paired.c vlag, mct's
//                 slowed pair) shows the impact frame only at +vlag: the punch sounded ~105 ms before it landed. Now the three cues move with the visible
//                 impact frame; a dodge refusal inside the wait drops them (no pain vox on a dodge).
//  4 SWING CUES   (victim screen) the attacker's effort vox / swish were timed on the SENDER's clock (t0 = arrival - one-way): at 160-210 ms one-way they
//                 fired 30-160 ms late or were dropped, all at the replica's first frame. Now on the replica's own clock (SWING arrival, scaled by
//                 tce / tc = the replica's rate); on the attacker's own screen scaled the same way (the windup clip runs at tc / tce).
//  5 PUPPET -HIT  (victim screen) the attacker's -hit half on the puppet crossfaded in over 0.18 s from the replica swing; when the replica reached its contact
//                 frame for this START (mct, within 45 ms) it is the authored switch pose: 0.04 s (SP: no blend).
//  6 FINISHER     (both screens) the intro -> -sync switch blended the attacker over 0.35 s: SP's main clip is at full weight at once; with the intro played to
//                 >= 0.9 of its length the switch is a cut now (the authored hand-over).
//  LOG            one line per exchange per screen:
//                 "sptl: <move> attacker <clip> @+N ms, victim <clip> @+N ms (SP: +N / +N), contact @+N (SP +N) | <screen> ..." (tools/sptl_diff.py)
#define SPTL_IMPACT_MS 35.0f    // SP: the impact sits 32-45 ms (median 36) after the -hit switch, the same in our -hit clips (mduel_moves.h dmax 0.03)
#define SPTL_SUM_MS 30000
#define SPTLOG(...) do { if (LOGCAP(g_sptl.nlog, 3000)) Xlog(__VA_ARGS__); } while (0)
// (dtl_speed: motion.c macro -> dtl_speed_s, melee_duel_tl.c)
static struct {
    int on, cfg_logged, nlog, no_restore;
    uint64_t cfg_at;
    // our duel swing (attacker screen)
    int sw_row, sw_n;
    double sw_t0;
    float sw_sp;
    int eng_hit, restored, unrestored, rs_n, rs_press, rs_lastp;
    uint64_t rs_sid, rs_stop_at;
    double eng_hit_at;
    // the partner's duel swing as it reached us (victim screen)
    double rsw_t[T2R_MAX_PEERS];
    int rsw_row[T2R_MAX_PEERS], rsw_tce[T2R_MAX_PEERS];
    // the running exchange's measurement (one pair at a time, like psync)
    struct {
        uint32_t uid;
        int role, fam, row, cd, done, seg_seen;
        double anchor, t_pair, t_vic, t_imp;
        float imp_ms, bl_att, bl_vic, vlag, hs;
        const char *anchor_what;
        int sp_a, sp_v, sp_c, stretch;
    } m;
    float last_att_bl, last_pup_bl;
    uint32_t last_att_bl_uid, last_pup_bl_uid;
    uint32_t n, n_c;
    double sum_a, sum_v, sum_c;
    uint64_t sum_at;
} g_sptl = {.sw_row = -1 };
enum { SPF_HIT, SPF_FIN, SPF_DC, SPF_PIN, SPF_OTHER };
static int SptlOn(void)
{
    if (!MduOn()) {
        g_sptl.on = 0;
        return 0;
    }
    uint64_t now = GetTickCount64();
    if (!g_sptl.cfg_at || now - g_sptl.cfg_at >= 2000) {
        g_sptl.cfg_at = now ? now : 1;
        FILE *f = ExeSideOpen("t2r-duel-no-sptl.txt", "r");
        int o = f == NULL;
        if (f)
            fclose(f);
        g_sptl.no_restore =
            GetFileAttributesA("t2r-sptl-no-swing-restore.txt") !=
            INVALID_FILE_ATTRIBUTES;
        if (o != g_sptl.on || !g_sptl.cfg_logged) {
            Xlog("sptl: %s",
                 o ?
                 "ON - duel halves on SP's timeline: the engine's early -hit undone (one -hit from frame 0), -hit halves at full weight on the switch frame, impact / pain cues on the visible impact frame, swing cues on the replica's clock, finisher main clip cut in; one 'sptl:' line per exchange vs SP (t2r-duel-no-sptl.txt = off)"
                 : "OFF (t2r-duel-no-sptl.txt)");
            g_sptl.cfg_logged = 1;
        }
        g_sptl.on = o;
    }
    return g_sptl.on;
}

// ---- SP reference numbers (re/melee-sp-timeline.md; ms from the press = the swing / pair start)
// dodge counters: SP's <set>-dodge-back-attack-1 switches to its -hit (attacker + victim halves) at tc (sp1027c a.log 171634 / 214640)
static int SptlSpDcTc(const char *name)
{
    if (!strcmp(name, "dc-fist-back"))
        return 433;             // VERIFIED fist-dodge-back-attack-1 -> -hit +433, impact +469
    if (!strcmp(name, "dc-1hblunt-back"))
        return 522;             // VERIFIED abby-1h-blunt-dodge-back-attack-1 -> -hit +522, impact +557
    return -1;                  // not recorded
}

static int SptlFamily(const char *n)
{
    if (!n)
        return SPF_OTHER;
    if (!strncmp(n, "md-", 3))
        return SPF_HIT;
    if (!strncmp(n, "mf-", 3))
        return SPF_FIN;
    if (!strncmp(n, "dc-", 3))
        return SPF_DC;
    if (!strncmp(n, "mp-", 3))
        return SPF_PIN;
    return SPF_OTHER;
}

static int SptlRowOfPair(int cd)
{
    MduInit();
    for (size_t i = 0; i < MDU_MOVES_N; i++)
        if (g_mdu.mv_cd[i] == cd)
            return (int) i;
    return -1;
}

// ---- hooks: our swing (melee_duel.c mdu_swing_note, after the SWING message)
static void SptlNoteSwing(int row, float sp)
{
    if (!SptlOn())
        return;
    g_sptl.sw_row = row;
    g_sptl.sw_t0 = PsyncMs();
    g_sptl.sw_sp = sp > 0.05f ? sp : 1.0f;
    g_sptl.sw_n++;
    g_sptl.eng_hit = 0;
    g_sptl.restored = 0;
    g_sptl.unrestored = 0;
    g_sptl.rs_sid = 0;
    g_sptl.rs_n = 0;
    g_sptl.rs_press = 0;
    g_sptl.rs_stop_at = 0;
}

// the cue time scale of a swing: its windup runs at tc / tce, so the clip's own vox / swish frames are tce / tc later in real time
static float SptlCueK(int tc, int tce)
{
    if (!g_sptl.on || tc <= 0 || tce <= tc || tce > tc + 250)
        return 1.0f;
    return (float) tce / (float) tc;
}

// melee_duel.c mdu_recv: the partner's SWING reached us now (the replica starts here, its contact = now + tce)
static void SptlNoteRswing(uint32_t slot, int row, int tce)
{
    if (!SptlOn() || slot >= T2R_MAX_PEERS)
        return;
    g_sptl.rsw_t[slot] = PsyncMs();
    g_sptl.rsw_row[slot] = row;
    g_sptl.rsw_tce[slot] = tce;
}

// 1 ENGINE -HIT: the engine switched our duel swing to its own -hit move after its (dropped) contact, before the duel's contact
static void SptlEngineHit(uint64_t move)
{
    if (!SptlOn() || !g_mdu.sw_on || g_mdu.sw_row < 0)
        return;
    if (g_sptl.eng_hit++)
        return;
    const MduRowT *m = &MDU_MOVES[g_mdu.sw_row];
    double now = PsyncMs(), el = now - g_sptl.sw_t0;
    g_sptl.eng_hit_at = now;
    uint64_t nw = GetTickCount64();
    int to_contact = (int) (int64_t) (g_mdu.sw_tc - nw);
    // (double punches; A [51494] / [51566] / [51635]): this used to cancel the engine's -hit and put the swing clip back at its earlier clip time
    // (a visible rewind), then the duel's -hit played the punch again. SP has ONE switch: the swing -> its -hit on the frame the engine reaches it (measured 266-466 ms,
    // the same as the engine here), the victim half on that frame. Now the engine's -hit frame IS the duel's contact: sw_tc is pulled to this frame and mdu_tick (same
    // frame, after feature_early_finisher) starts the -hit pair from frame 0 at full weight - the same pose the engine just switched to. No cancel, no rewind.
    // (With the default SP pace the pull is 0-2 frames. With an opt-in stretch - t2r-duel-ping-windup.txt / t2r-knife-fist-pace.txt / t2r-duel-slow-pace.txt - the
    // engine still switches at SP time, so that blow's contact comes at SP time too: the stretch only slowed the clip.)
    if (to_contact > 0)
        g_mdu.sw_tc = nw;
    if (g_mduSwClip) {
        mstag_speed(g_mduSwClip, 1.0f);
        g_mduSwClip = 0;
    }
    SPTLOG
        ("sptl: engine -hit move %llx on our %s at +%.0f ms (SP switch +%d ms) - the duel contact %s this frame (%+d ms vs planned): one -hit from frame 0 with the victim half, no swing rewind",
         (unsigned long long) move, m->move, el, m->tc_ms,
         to_contact > 0 ? "pulled to" : "lands", -to_contact);
}

// VICTIM SNAP: paired.c psync drive, the victim half's pull time. SP (re/melee-duel-sp-spec.md section 2, n=70): on the switch frame the victim's yaw snaps
// so it faces the attacker exactly (68/70, median 10 deg, up to 67), with no blend ("settle 0.0"), and its position does not jump (median 0.01 m). Our pull eased
// that yaw over 0.20 s (A [51444] "victim 0.00 m / 11 deg ... over 0.20 s": still ~8 deg off at the +35 ms impact). A duel -hit pair whose victim starts
// within 0.12 m of its spot (a pop below a frame of walking) now takes its spot on the first frame (the puppet's own dtl lunge cap still applies on the attacker screen). Farther = the pull
// (a latency-inflated offset; SP never has one). t2r-sptl-vic-pull.txt = the old pull.
static float SptlVicDriveS(float dbl)
{
    if (!SptlOn() || g_ps.kind != PS_CTXDOWN || g_ps.nseg > 1
        || !MduMove(g_ps.move) || !(g_psOffD[1] >= 0.0f)
        || g_psOffD[1] > 0.12f)
        return dbl;
    static int old = -1;
    static uint64_t at;
    uint64_t n = GetTickCount64();
    if (old < 0 || n - at > 2000) {
        at = n;
        old =
            GetFileAttributesA("t2r-sptl-vic-snap.txt") ==
            INVALID_FILE_ATTRIBUTES;
    }                           // (the 1-frame snap is opt-in; the pull + 0.08 s blend is the default)
    if (old)
        return dbl;
    static uint32_t lu;
    if (lu != g_ps.uid) {
        lu = g_ps.uid;
        SPTLOG
            ("sptl: u%08x victim half snaps onto its spot on the first frame (off %.2f m / %.0f deg; SP: yaw snap, no blend) - not the %.2f s pull",
             g_ps.uid, (double) g_psOffD[1], (double) g_ps_off_y[1],
             (double) dbl);
    }
    return 0.001f;
}

// 2 -HIT CUT: paired.c psync_play, our own attacker half's animate_ blend (-1 = the caller's)
static float SptlOwnAttBlend(void)
{
    if (!g_sptl.on || g_ps.role != 1 || g_ps.kind != PS_CTXDOWN
        || g_ps.nseg > 1 || !MduMove(g_ps.move)
        || g_ps.ent_att != SID_PLAYER)
        return -1.0f;
    if (g_sptl.unrestored && PsyncMs() - g_sptl.sw_t0 < 1500.0)
        return -1.0f;
    if (MduOwnOffSpot()) {
        Xlog("sptl: u%08x our attacker half starts %.2f m off its spot (parry / clash: no swing warp) - the SP pull, not the cut-in", g_ps.uid, (double) g_psOffD[0]);
        return -1.0f;
    }                           // the engine's -hit is on the body: a cut would jump back to its frame 0
    g_sptl.last_att_bl = 0.0f;
    g_sptl.last_att_bl_uid = g_ps.uid;
    return 0.0f;
}

// 5 PUPPET -HIT: paired.c ps_pup_att_blend (duel pair, victim screen): the crossfade when the replica is not on its contact frame
static float SptlPupAttBlend(float xfade)
{
    if (!SptlOn() || g_ps.role != 2)
        return xfade;
    float b = xfade;
    if (g_mct.on && g_mct.pend.uid == g_ps.uid
        && __builtin_fabsf(g_mct.pend.delta) <= 45.0f)
        b = 0.04f;
    g_sptl.last_pup_bl = b;
    g_sptl.last_pup_bl_uid = g_ps.uid;
    if (b != xfade)
        Xlog("sptl: u%08x attacker -hit half on slot %d's puppet blends in over %.2f s (the replica sat on its contact frame %+.0f ms from this START - the authored switch pose; SP: full weight at once), not %.2f s", g_ps.uid, g_ps.att, (double) b, (double) g_mct.pend.delta, (double) xfade);
    return b;
}

// 6 FINISHER: paired.c psync_seg_next, the attacker's intro -> -sync switch blend (-1 = the caller's 0.35 s)
static float SptlSegAttBlend(float intro_phase)
{
    if (!SptlOn() || g_ps.kind != PS_CTXDOWN || g_ps.cur_seg != 0
        || !CdMfIntroMove(g_ps.move))
        return -1.0f;
    if (!(intro_phase >= 0.9f || intro_phase < 0.0f))
        return -1.0f;           // (< 0: the intro layer already ended = it played fully)
    Xlog("sptl: u%08x finisher main clip on the attacker at full weight on the switch (intro at phase %.3f; SP: the -sync clip fade 1.000 on its first frame, 7/7)", g_ps.uid, (double) intro_phase);
    return 0.0f;
}

// 3 IMPACT SOUND: melee_duel.c mdu_contact, our pair just started: ms to add to the victim cues so they land on the visible impact frame
static int SptlCueShift(void)
{
    if (!SptlOn() || g_ps.role != 1 || g_ps.uid != g_mdu.ps_uid
        || !(g_ps.vlag > 0.0f))
        return 0;
    float w = g_ps.vlag * 1000.0f, imp = g_ps.hs_rate > 0.0f ? fminf(SPTL_IMPACT_MS / g_ps.hs_rate, w) : w + SPTL_IMPACT_MS;    // (slowed pair: the impact frame at 35 / rate; frozen victim: 35 ms after the wait)
    int sh = (int) (imp - SPTL_IMPACT_MS + 0.5f);
    if (sh < 0)
        sh = 0;
    if (sh > 300)
        sh = 300;
    SPTLOG
        ("sptl: u%08x victim pain / impact / second pain at contact +%d / +%d / +%d ms (the hit-confirm wait %.0f ms shows the impact frame at +%.0f; SP: +4 / +35 / +40 after the switch)",
         g_ps.uid, sh + 4, sh + 35, sh + 40, (double) w, (double) imp);
    return sh;
}

// mdu_miss_note: a dodge ruled after the contact - the victim cues still waiting on the impact frame are dropped
static void SptlDropVicCues(const char *why)
{
    if (!g_sptl.on)
        return;
    int n = 0;
    for (int i = 0; i < 12; i++)
        if (g_mdu.q[i].at
            && (g_mdu.q[i].act == MDUA_PAIN1
                || g_mdu.q[i].act == MDUA_IMPACT
                || g_mdu.q[i].act == MDUA_PAIN2)) {
            g_mdu.q[i].at = 0;
            n++;
        }
    if (n)
        SPTLOG
            ("sptl: %d victim cue(s) dropped (%s): no pain / impact sound on a dodged blow",
             n, why);
}

// ---- the per-exchange line (paired.c psync_play: a pair just started on this screen)
static void SptlNotePair(void)
{
    if (!SptlOn() || g_ps.kind != PS_CTXDOWN
        || (g_ps.role != 1 && g_ps.role != 2))
        return;
    int cd = CdPairOfMove(g_ps.move);
    if (cd < 0)
        return;
    int fam = SptlFamily(CD_PAIRS[cd].name);
    if (fam == SPF_OTHER)
        return;
    typeof(&g_sptl.m) m = &g_sptl.m;
    double now = PsyncMs();
    memset(m, 0, sizeof *m);
    m->uid = g_ps.uid;
    m->role = g_ps.role;
    m->fam = fam;
    m->cd = cd;
    m->t_pair = now;
    m->t_vic = -1.0;
    m->t_imp = -1.0;
    m->vlag = g_ps.vlag;
    m->hs = g_ps.hs_rate;
    m->row = fam == SPF_HIT ? SptlRowOfPair(cd) : -1;
    // v5-feel3: rows share a -hit pair (fist-light-attack-1 and -1-far both play md-fist-light-attack-1-hit): the first match named the far row and
    // compared light-1 with SP's +667 instead of +266 (A [16247]). The swing that really ran on this screen wins when it plays this pair.
    if (fam == SPF_HIT) {
        int sr = g_ps.role == 1 ? g_sptl.sw_row : g_ps.att >= 0
            && g_ps.att < T2R_MAX_PEERS ? g_sptl.rsw_row[g_ps.att] : -1;
        if (sr >= 0 && (size_t) sr < MDU_MOVES_N && g_mdu.mv_cd[sr] == cd)
            m->row = sr;
    }
    m->bl_att = g_ps.role == 1
        && g_sptl.last_att_bl_uid ==
        g_ps.uid ? g_sptl.last_att_bl : g_ps.role == 2
        && g_sptl.last_pup_bl_uid == g_ps.uid ? g_sptl.last_pup_bl : -1.0f;
    m->anchor = now;
    m->anchor_what =
        g_ps.role == 1 ? "the pair start" : "the START arrival";
    if (g_ps.role == 1
        && (fam == SPF_HIT || fam == SPF_FIN || fam == SPF_PIN)
        && g_sptl.sw_t0 > 0.0 && now - g_sptl.sw_t0 < 1500.0) {
        m->anchor = g_sptl.sw_t0;
        m->anchor_what = "our press (swing start)";
    }
    if (g_ps.role == 2 && g_ps.att >= 0 && g_ps.att < T2R_MAX_PEERS
        && fam == SPF_HIT && g_sptl.rsw_t[g_ps.att] > 0.0
        && now - g_sptl.rsw_t[g_ps.att] < 1500.0) {
        m->anchor = g_sptl.rsw_t[g_ps.att];
        m->anchor_what = "its SWING arrival (= its press here)";
    }
    if (fam == SPF_HIT && m->row >= 0)
        m->stretch = g_ps.role == 1 ? g_mduSwXtra : g_ps.att >= 0
            && g_ps.att <
            T2R_MAX_PEERS ? g_sptl.rsw_tce[g_ps.att] -
            MDU_MOVES[m->row].tc_ms : 0;
    // the impact moment inside the attacker's (last segment's) clip, and SP's times from the press
    m->sp_a = m->sp_v = m->sp_c = -1;
    if (fam == SPF_HIT) {
        m->imp_ms = SPTL_IMPACT_MS;
        if (m->row >= 0 && MDU_MOVES[m->row].tc_ms > 0) {
            m->sp_a = m->sp_v = MDU_MOVES[m->row].tc_ms;
            m->sp_c = m->sp_a + 35;
        }
    } else if (fam == SPF_FIN) {
        int il = (int) (CdSeg0Ms(g_ps.move) + 0.5f);
        m->imp_ms = CD_PAIRS[cd].dmax * 1000.0f;
        m->sp_a = 0;
        m->sp_v = il;
        m->sp_c = il + (int) (m->imp_ms + 0.5f);
    } else if (fam == SPF_DC) {
        m->imp_ms = DcHitRow(cd) * 1000.0f;
        int tc = SptlSpDcTc(CD_PAIRS[cd].name);
        if (tc > 0) {
            m->sp_a = 0;
            m->sp_v = tc;
            m->sp_c = tc + 36;
        }
    } else
        m->imp_ms = CD_PAIRS[cd].dmax * 1000.0f;
    // the victim half on this screen: single-segment pairs start it now (frozen through a dc lag / vlag without hit-stop rate), chains at the deferred start / switch
    if (g_ps.nseg <= 1)
        m->t_vic = now + (g_ps.vlag > 0.0f
                          && !(g_ps.hs_rate >
                               0.0f) ? g_ps.vlag * 1000.0 : 0.0);
    if (g_ps.role == 1 && fam == SPF_HIT && g_sptl.rs_press && m->row >= 0
        && MDU_MOVES[m->row].next) {
        g_mdu.buf_at = GetTickCount64();
        SPTLOG
            ("sptl: a press during the restored windup is buffered for the combo window");
    }
    g_sptl.rs_press = 0;
}

static void SptlEmit(const char *why)
{
    typeof(&g_sptl.m) m = &g_sptl.m;
    if (!m->uid || m->done)
        return;
    m->done = 1;
    int cd = m->cd;
    const char *mv = m->fam == SPF_HIT
        && m->row >= 0 ? MDU_MOVES[m->row].move : CD_PAIRS[cd].name;
    const char *ca = m->fam == SPF_FIN
        && CD_PAIRS[cd].nseg > 1 ? CD_PAIRS[cd].seg[0].att : CD_ATT0(cd);
    const char *cv =
        CD_PAIRS[cd].nseg >
        1 ? CD_PAIRS[cd].seg[CD_PAIRS[cd].nseg - 1].vic : CD_VIC0(cd);
    int a = (int) (m->t_pair - m->anchor + 0.5), v =
        m->t_vic >= 0.0 ? (int) (m->t_vic - m->anchor + 0.5) : -1, c =
        m->t_imp >= 0.0 ? (int) (m->t_imp - m->anchor + 0.5) : -1;
    char sa[16], Sv[16], sc[16], pv[16], pc[16], spa[16];
    snprintf(spa, sizeof spa, m->sp_a >= 0 ? "+%d" : "+?", m->sp_a);
    snprintf(pv, sizeof pv, m->sp_v >= 0 ? "+%d" : "+?", m->sp_v);
    snprintf(pc, sizeof pc, m->sp_c >= 0 ? "+%d" : "+?", m->sp_c);
    snprintf(sa, sizeof sa, "+%d", a);
    snprintf(Sv, sizeof Sv, v >= 0 ? "+%d" : "+?", v);
    snprintf(sc, sizeof sc, c >= 0 ? "+%d" : "+?", c);
    char extra[340], abl[72];
    // ('att blend 0 (cut)' on halves that blended in over the 2-frame floor): the blend animate_ really got (paired.c
    // g_ps_att_bl_applied), next to what sptl asked for
    if (g_psAttBlUid == m->uid && g_ps_att_bl_applied >= 0.0f)
        snprintf(abl, sizeof abl, "%.3f s applied (%s)",
                 (double) g_ps_att_bl_applied,
                 m->bl_att >= 0.0f ? (m->bl_att ==
                                      0.0f ? "asked 0: the floor" :
                                      "asked short") :
                 "the pull / default");
    else
        snprintf(abl, sizeof abl, "%s",
                 m->bl_att >= 0.0f ? (m->bl_att ==
                                      0.0f ?
                                      "asked 0 (applied value not recorded)"
                                      : m->bl_att <
                                      0.1f ? "short" : "crossfade") :
                 "pull / default");
    snprintf(extra, sizeof extra,
             "%s screen (role %d), times from %s, windup stretch +%d ms (one-way rule), att blend %s, hit-confirm wait %.0f ms at x%.2f, engine -hit %s%s",
             m->role == 1 ? "attacker" : "victim", m->role, m->anchor_what,
             m->stretch, abl, (double) (m->vlag * 1000.0f), (double) m->hs,
             m->role != 1
             || !g_sptl.
             eng_hit ? "none" : "its frame = the duel contact (no rewind)",
             why ? why : "");
    SPTLOG
        ("sptl: %s attacker %s @%s ms, victim %s @%s ms (SP: %s / %s), contact @%s (SP %s) | %s",
         mv, ca, sa, cv, Sv, spa, pv, sc, pc, extra);
    g_sptl.n++;
    if (m->sp_a >= 0)
        g_sptl.sum_a += __builtin_fabs((double) (a - m->sp_a));
    if (m->sp_v >= 0 && v >= 0)
        g_sptl.sum_v += __builtin_fabs((double) (v - m->sp_v));
    if (m->sp_c >= 0 && c >= 0) {
        g_sptl.sum_c += __builtin_fabs((double) (c - m->sp_c));
        g_sptl.n_c++;
    }
}

// mdu_miss_note: our duel swing did not connect (dodged) - SP's miss = the swing clip plays on at x1, no -hit, no victim half
static void SptlNoteMiss(const char *why)
{
    if (!g_sptl.on || g_sptl.sw_row < 0
        || PsyncMs() - g_sptl.sw_t0 > 1500.0)
        return;
    const MduRowT *m = &MDU_MOVES[g_sptl.sw_row];
    SPTLOG
        ("sptl: %s MISS attacker %s @+0 ms x%.2f, no -hit, no victim half (SP: the swing at x1.00 to its end, no -hit; the dodger's own dodge clip) | attacker screen, %s, engine -hit %s",
         m->move, m->swing, (double) g_sptl.sw_sp, why,
         !g_sptl.
         eng_hit ? "none" : "its frame = the duel contact (no rewind)");
}

static void SptlTick(void)
{
    if (!SptlOn())
        return;
    uint64_t nowt = GetTickCount64();
    double now = PsyncMs();
    if (!g_sptl.sum_at)
        g_sptl.sum_at = nowt;
    // the restored swing: its speed on the first frames (a speed set on the creating frame may not stick), a press noted, stopped if no pair took the body
    if (g_sptl.restored && g_sptl.rs_sid) {
        if (g_sptl.rs_n < 6) {
            DTL_SPEED(SID_PLAYER, g_sptl.rs_sid, g_sptl.sw_sp);
            g_sptl.rs_n++;
        }
        int p = PadinjBlockedRaw();
        if (p && !g_sptl.rs_lastp && g_mdu.sw_on)
            g_sptl.rs_press = 1;
        g_sptl.rs_lastp = p;
        int taken = g_ps.role == 1 && g_ps.ent_att == SID_PLAYER
            && g_ps.play_ms > g_sptl.eng_hit_at;
        if (taken)
            g_sptl.rs_sid = 0;
        else if (!g_mdu.sw_on && g_sptl.rs_stop_at
                 && nowt >= g_sptl.rs_stop_at) {
            MhrStop(SID_PLAYER, g_sptl.rs_sid, 0.30f);
            SPTLOG
                ("sptl: restored swing %s stopped (blend-out 0.30 s): no duel pair took the body (a miss / plain blow)",
                 g_sptl.sw_row >=
                 0 ? MDU_MOVES[g_sptl.sw_row].swing : "?");
            g_sptl.rs_sid = 0;
        }
    }
    // the running exchange
    typeof(&g_sptl.m) m = &g_sptl.m;
    if (m->uid && !m->done) {
        int run = g_ps.role == m->role && g_ps.uid == m->uid
            && g_ps.kind == PS_CTXDOWN;
        if (!run)
            SptlEmit(", pair ended before its impact frame");
        else {
            if (m->t_vic < 0.0 && (g_ps.vic_early || g_ps.cur_seg >= 1))
                m->t_vic = now;
            int last = g_ps.nseg <= 1 || g_ps.cur_seg >= g_ps.nseg - 1;
            float ph =
                last ? PsyncPhase(g_ps.ent_att, g_ps.att_sid) : -1.0f,
                du =
                g_ps.att_dur * (g_ps.speed > 1.001f ? g_ps.speed : 1.0f);
            if (ph >= 0.0f && du > 0.0f && m->t_imp < 0.0
                && ph * du * 1000.0f >= m->imp_ms)
                m->t_imp = now;
            if (m->t_imp >= 0.0 && m->t_vic >= 0.0)
                SptlEmit(NULL);
            else if (now - m->t_pair >
                     (m->fam == SPF_FIN ? 3000.0 : 1500.0))
                SptlEmit(", no impact frame seen");
        }
    }
    if (nowt - g_sptl.sum_at >= SPTL_SUM_MS) {
        if (g_sptl.n)
            Xlog("sptl: 30 s summary - %u exchanges vs SP: |attacker half start - SP| avg %.0f ms, |victim half start - SP| avg %.0f ms, |contact - SP| avg %.0f ms over %u (tools/sptl_diff.py for the per-move table)", g_sptl.n, g_sptl.sum_a / g_sptl.n, g_sptl.sum_v / g_sptl.n, g_sptl.n_c ? g_sptl.sum_c / g_sptl.n_c : 0.0, g_sptl.n_c);
        g_sptl.n = g_sptl.n_c = 0;
        g_sptl.sum_a = g_sptl.sum_v = g_sptl.sum_c = 0.0;
        g_sptl.sum_at = nowt;
    }
}

// ps40 D3 (paired.c ps_take_blend40): the other move a body is in when a pair's half takes it. k 0 attacker / 1 victim, own = our body.
static const char *PsTakeOther40(int k, int own)
{
    if (own && g_mduSwCutMs && GetTickCount64() - g_mduSwCutMs < 100) {
        g_psTakeBl40 = PS_TAKE_SWING_BL;
        return "inside its own swing (cut on this frame)";
    }
    if (!own && g_dtl.pr.on
        && (int) g_dtl.pr.slot == (k ? g_ps.vic : g_ps.att) && k == 1) {
        g_psTakeBl40 = PS_TAKE_SWING_BL;
        return
            "inside its own swing (its swing replica runs on the puppet)";
    }
    if (own
        && (MdgOwnClipOn()
            || (g_mdgLateRelMs
                && GetTickCount64() - g_mdgLateRelMs < 400))) {
        g_psTakeBl40 = PS_TAKE_SWING_BL;
        return
            "in its dodge clip (running, or released by a TOO LATE ruling under 400 ms ago)";
    }
    if (k == 1 && g_psEnd40Ms > 0.0 && PsyncMs() - g_psEnd40Ms < 120.0
        && g_psEnd40Vic == g_ps.ent_vic)
        return
            "the previous pair's victim half was on it until this frame";
    if (k == 1 && g_ps_off_y[1] > 30.0f)
        return "the victim stands more than 30 deg off the half's facing";
    if (k == 0 && own && g_sptl.on && g_sptl.eng_hit && g_mdu.sw_row >= 0
        && g_mdu.sw_row < (int) (sizeof MDU_MOVES / sizeof MDU_MOVES[0])
        && g_sptl.eng_hit_at - g_sptl.sw_t0 >
        (double) MDU_MOVES[g_mdu.sw_row].tc_ms + 50.0
        && PsyncMs() - g_sptl.eng_hit_at < 100.0)
        return "our swing ran more than 50 ms past its SP switch frame";
    return NULL;
}
