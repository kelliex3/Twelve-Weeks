// melee_duel_tl.c - the DUEL TIMELINE: one presentation arbiter for both duel bodies (from ~160 ms RTT live clips: puppet
// snaps of 0.5-1 m in one frame, clips cut into each other, both fighters reacting at once, duel camera whips, swings whiffing from 1.5-3 m; the fix is a confirmation system that
// plays and queues animations instead of cutting them into each other, with prediction + smoothing so they play fully and not late).
// Design + every rerouted call site: re/melee-duel-timeline.md. Included after melee_duel_pin.c (before melee_duel_parry.c). Log prefix "dtl:". Opt-OUT: t2r-duel-no-timeline.txt
// next to the exe (re-read every 2 s); it only ever acts while the duel itself is on (t2r-melee-duel.txt).
//
// What it owns (the partner = the duel partner's puppet; us = our own body):
//  POSITION   dtl_locator (locator_hook, after psync's own skip / hand-back): the partner puppet's net locator is never applied as is. A displayed locator
//             follows it with velocity extrapolation (the net's own motion, outliers > DTL_VMAX rejected) plus a critically damped spring toward the net
//             position (omega DTL_OMEGA), the correction speed capped at DTL_DRIFT m/s; while one of our clips plays on the puppet the spring is frozen
//             (only the net motion is carried). > DTL_TELEPORT m = respawn / teleport, taken as is. Yaw: follows at <= DTL_YAW_RATE deg/s.
//             Starts from where the body stands (no first-frame jump) and keeps running for DTL_TAIL_MS after the duel ends until it is on the net.
//  CLIPS      dtl_pup_route: every clip that wants the partner puppet (reaction MSG_HITREACT2, dodge MSG_DODGE press, the predicted swing replica) goes
//             through a per-body queue: never a hard cut, a lower / equal priority entry waits for the running clip's blend-out point, a higher one starts
//             early with a DTL_XFADE crossfade, stale entries are dropped (never played late out of order). Priority: swing 1 < reaction 2 < dodge 3 <
//             pair 4 (psync owns the puppet) < down 5.
//  FULL PLAY  a remote clip starts at phase 0 (no windup trimmed); its lateness is absorbed by rate: x(1 + late / half-clip), <= DTL_RATE_MAX, then 1.0.
//  PREDICT    the duel's SWING message leaves the attacker on its move-start frame (= its press frame, SP: press and MSTART on the same frame) with the
//             SP row: the receiving screen starts that swing clip on the puppet at once (dtl_pred_swing); the pair START (contact) or the miss confirms it.
//  CONFIRM    trades: a duel pair START on us while our own duel pair on the same partner runs is decided by the shared contact times (earlier wins,
//             +-DTL_TIE_MS = lower slot wins): the loser aborts its own pair and plays the winner's (dtl_trade_yield, paired.c psync_begin_remote).
//             No speculative reaction on the partner (mhr_pred_arm veto); the victim's dodge window is shortened by what the attacker's hit-confirm hold
//             cannot cover (dtl_window_trim), so a dodge verdict is in before any reaction shows.
//  CAMERA     melee_spcam.c spm_yaw / spm_pitch: the duel look-at's world heading / pitch follow their target at bounded, eased rates (g_dtl_cam_on).
//  METRICS    per frame: 'dtl: SNAP' (a body moved > DTL_SNAP_M in one frame beyond its clip root motion / velocity); 'dtl: CUT' (a clip replaced
//             another without a crossfade); 'dtl: TRIM' (a clip lost its start or its body before its blend-out); a 30 s summary line.
#define DTL_XFADE 0.20f         // s: crossfade of every queued / early clip start
#define DTL_RATE_MAX 1.30f      // catch-up playback rate cap
#define DTL_DODGE_RATE_MAX 1.15f        // the dodge replica's catch-up cap (a dodge is never visibly sped up)
#define DTL_TIE_MS 40.0         // trade: contacts closer than this are a tie (lower slot wins)
#define DTL_TRADE_WIN_MS 250.0  // a blow of theirs this much after ours is no trade (our pair is in its tail): ours yields
#define DTL_SNAP_M 0.15f        // m in one frame beyond what clip / velocity explain
#define DTL_DODGE_CAP 1.45f     // m: a dodging partner's puppet travel from its press spot (SP 1.17 m at the move end, 1.45 m side dodges: MDS_SP_END_SIDE_M)
#define DTL_DRIFT 1.5f          // m/s: max net-correction speed of the displayed puppet
#define DTL_OMEGA 9.0f          // rad/s: spring of the displayed puppet onto the net position (critically damped, ~0.45 s settle)
#define DTL_VMAX 8.0f           // m/s: a net step faster than this is a correction, not motion
#define DTL_TELEPORT 4.0f       // m: larger = respawn / teleport, taken as is
#define DTL_YAW_RATE 540.0f     // deg/s: displayed puppet yaw follow cap
#define DTL_TAIL_MS 1500        // after the duel the filter runs until on the net (or this long)
#define DTL_SUM_MS 30000
enum { DTL_S_SWING = 1, DTL_S_REACT = 2, DTL_S_DODGE = 3, DTL_S_PAIR =
        4, DTL_S_DOWN = 5 };
static const char *const DTL_SN[] =
    { "-", "swing replica", "reaction", "dodge", "pair", "down" };
#define DTLOG(...) do { if (LOGCAP(g_dtl.nlog, 2500)) Xlog(__VA_ARGS__); } while (0)
static struct {
    int on, cfg_logged, nlog;
    uint64_t cfg_at;
    // 30 s metrics
    uint32_t snaps[2], cuts, trims, queued, dropped, xfades, catchups,
        predicted, conf_same, corrected, late_n, trades_won, trades_lost,
        reach_saves, win_trims;
    double late_sum;
    uint64_t sum_at, tail_any;
    int had_duel;
    // snap detector: 0 = us, 1 = the partner
    struct {
        float p[3], v[3], pl[3];
        int ok, pl_ok;
        uint64_t h;
        double t;
        uint32_t pl_uid;
    } sd[2];                    // pl_uid: the pair whose placement pl is (v5-body)
    // displayed-locator filter per slot
    struct {
        uint64_t h;
        float d[3], v[3], np[3], nv[3], yaw;
        int ok, np_ok, hb;
        double t;
        uint64_t tail_until, tail_hard;
        uint32_t n, jumps;
        float maxe;
    } lf[T2R_MAX_PEERS];        // hb: restarted by a psync release (v5-body)
    // the clip running on the partner puppet (ours) + the queue
    struct {
        int src;
        uint64_t ent, sid, t0, cancel_at, end;
        uint32_t slot;
    } run;
    struct {
        int used, src;
        uint32_t slot;
        uint64_t sid, at, stale_at;
        uint32_t cut;
        float age0;
    } q[6];
    // catch-up rate restores
    struct {
        uint64_t ent, sid, at;
        float base;
    } rr[8];
    // the predicted swing replica
    struct {
        int on, row;
        uint32_t slot;
        uint64_t ent, sid, t0, contact, swing_t0;
    } pr;
} g_dtl;
// ps25 (melee_duel_contact.c, included after this file): the contact carry is merged into dtl_locator's own write - ONE position writer for the duel partner's puppet
static int MctCarryAt(uint64_t h, const float *l, float *o8);
static int MctMergeOld(void);
static uint64_t g_mct_carried_f, g_mct_carried_h;
static int g_dtlSnapLatch;    // c1657: t2r-dtl-snap-latch.txt = a velocity-explained SNAP keeps the predicted step as the velocity (old, self-repeating)
static int DtlOn(void)
{
    if (!MduOn()) {
        g_dtl.on = 0;
        g_dtlCamOn = 0;
        return 0;
    }
    uint64_t now = GetTickCount64();
    if (!g_dtl.cfg_at || now - g_dtl.cfg_at >= 2000) {
        g_dtl.cfg_at = now ? now : 1;
        FILE *f = ExeSideOpen("t2r-duel-no-timeline.txt", "r");
        int o = f == NULL;
        if (f)
            fclose(f);
        if (o != g_dtl.on || !g_dtl.cfg_logged) {
            Xlog("dtl: %s",
                 o ?
                 "ON - duel timeline: one owner per duel body, queued crossfaded clips, full playback with catch-up, predicted swings, bounded puppet drift, rate-limited duel camera (t2r-duel-no-timeline.txt = off)"
                 : "OFF (t2r-duel-no-timeline.txt)");
            g_dtl.cfg_logged = 1;
        }
        g_dtl.on = o;
        f = ExeSideOpen("t2r-dtl-snap-latch.txt", "r");
        g_dtlSnapLatch = f != NULL;
        if (f)
            fclose(f);
    }
    g_dtlCamOn = g_dtl.on;
    return g_dtl.on;
}

static int DtlPartner(uint32_t slot)
{
    return DtlOn() && g_mdu.slot >= 0 && (uint32_t) g_mdu.slot == slot;
}

static void DtlCut(const char *what, uint32_t slot, uint64_t sid,
                    const char *why)
{
    g_dtl.cuts++;
    DTLOG("dtl: CUT %s on slot %u (clip %llx) without a crossfade - %s",
          what, slot, (unsigned long long) sid, why);
}

static void DtlTrim(const char *what, int ms, const char *why)
{
    g_dtl.trims++;
    DTLOG("dtl: TRIM %s lost %d ms (%s)", what, ms, why);
}

// ---- catch-up: the clip runs from phase 0 at x rate until the lateness is gone (spread over `window_s` of it), then at base
static void DtlSpeedS(uint64_t ent, uint64_t sid, float v, int src)
{
    for (int l = 0; l < 4; l++) {
        uint64_t a[4] = { ent, 0, (uint64_t) l, sid };
        memcpy(&a[1], &v, 4);
        MoRateDc(a, src);
    }
}

static void DtlCatchup(uint64_t ent, uint64_t sid, float late_ms,
                        float window_s, float base, const char *what)
{
    g_dtl.late_n++;
    g_dtl.late_sum += late_ms;
    if (late_ms < 8.0f || window_s < 0.05f) {
        if (base != 1.0f)
            DTL_SPEED(ent, sid, base);
        return;
    }
    float rmax = DTL_RATE_MAX;
    // (a dodge is not sped up or played late into the animation): the partner's DODGE replica absorbs its lateness (one-way latency) at x1.15 at most
    // (paired.c smoothing 4's rule), from phase 0 - never a skip; what is left late at the end of the window stays late (the clip plays fully). t2r-dodge-old-catchup.txt = x1.30 as the other clips
    if (!strcmp(what, "dodge")) {
        static int old = -1;
        static uint64_t at;
        uint64_t n = GetTickCount64();
        if (old < 0 || n - at > 2000) {
            at = n;
            old =
                GetFileAttributesA("t2r-dodge-old-catchup.txt") !=
                INVALID_FILE_ATTRIBUTES;
        }
        if (!old)
            rmax = DTL_DODGE_RATE_MAX;
    }
    float r = 1.0f + late_ms / (window_s * 1000.0f);
    if (r > rmax)
        r = rmax;
    float T = late_ms / (r - 1.0f), resid = late_ms - (r - 1.0f) * window_s * 1000.0f;  // real ms at the fast rate; what is left late at the end
    if (T > window_s * 1000.0f)
        T = window_s * 1000.0f;
    DTL_SPEED(ent, sid, base * r);
    for (int i = 0; i < 8; i++)
        if (!g_dtl.rr[i].at
            || (g_dtl.rr[i].ent == ent && g_dtl.rr[i].sid == sid)) {
            g_dtl.rr[i].ent = ent;
            g_dtl.rr[i].sid = sid;
            g_dtl.rr[i].base = base;
            g_dtl.rr[i].at = GetTickCount64() + (uint64_t) T;
            break;
        }
    g_dtl.catchups++;
    DTLOG
        ("dtl: catch-up %s clip %llx: %.0f ms late -> x%.2f (cap x%.2f) for %.0f ms from phase 0 (no trim)%s",
         what, (unsigned long long) sid, (double) late_ms,
         (double) (base * r), (double) rmax, (double) T,
         resid >
         5.0f ?
         " - still late at the end of the window (the clip plays fully anyway)"
         : "");
}

static void DtlRrTick(uint64_t now)
{
    for (int i = 0; i < 8; i++)
        if (g_dtl.rr[i].at && now >= g_dtl.rr[i].at) {
            DTL_SPEED(g_dtl.rr[i].ent, g_dtl.rr[i].sid, g_dtl.rr[i].base);
            g_dtl.rr[i].at = 0;
        }
}

// ---- the partner puppet's clips
// ("jump in place": the partner's dodge clip was QUEUED behind the running pair, dropped stale 6 frames later, then the ABORT DODGED ended the pair - the dodger
// stood still): a dodge clip waits for the pair to end (the DODGED abort) and then plays from its elapsed time (dtl_play's catch-up), it is never dropped as stale inside 1.5 s.
// t2r-dtl-dodge-stale-old.txt = the old 300 ms.
static int DtlDodgeKeep(void)
{
    static int off = -1;
    static uint64_t at;
    uint64_t n = GetTickCount64();
    if (off < 0 || n - at > 2000) {
        at = n;
        off =
            GetFileAttributesA("t2r-dtl-dodge-stale-old.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    return !off;
}

static float DtlStaleMs(int src, uint32_t cut)
{
    return src ==
        DTL_S_REACT ? (cut ? (float) cut * 0.6f : 250.0f) : src ==
        DTL_S_DODGE ? (DtlDodgeKeep()? 1500.0f : 300.0f) : 220.0f;
}

// (the dodge clip on the victim's puppet arrived 219 ms after OUR confirmed blow, then the 2013 down came: "down with no hit"): once the victim has ACKed our pair
// (the blow is confirmed) a dodge clip of its puppet is a press made after the contact - dropped. t2r-dtl-dodge-after-hit-old.txt = old.
static int DtlDodgeAfterHit(uint32_t slot)
{
    static int off = -1;
    static uint64_t at;
    uint64_t n = GetTickCount64();
    if (off < 0 || n - at > 2000) {
        at = n;
        off =
            GetFileAttributesA("t2r-dtl-dodge-after-hit-old.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    return !off && g_mdu.on && g_mdu.slot == (int) slot && g_ps.role == 1
        && g_ps.uid == g_mdu.ps_uid && g_ps.vic == (int) slot && g_ps.acked
        && g_mdu.hit_ms && n >= g_mdu.hit_ms;
}

static uint64_t g_dtlRelMs[T2R_MAX_PEERS];    // dtl_hb_take: when psync last released the slot's puppet
static int DtlRunningPrio(uint32_t slot, uint64_t now)
{
    if (PsyncBusySlot(slot) && g_ps_pup[slot].on)
        return DTL_S_PAIR;      // (a half already handed back at its blend-out does not hold the puppet)
    if (g_peerDowned[slot] || g_puppetDeadHere[slot])
        return DTL_S_DOWN;
    if (g_dtl.run.src && g_dtl.run.slot == slot
        && now < g_dtl.run.cancel_at)
        return g_dtl.run.src;
    return 0;
}

// (a dodge replica 219 ms late, the down 21 frames later - the 2013 fall crossfaded over the still running dodge clip, a sliding lying pose): the puppet that goes
// down gets its running timeline clip (dodge / reaction / swing replica) cut with a short blend, the fall owns the body. t2r-down-cut-dodge-old.txt = off. Log "dtl: slot N went down".
static void DtlDownCut(uint32_t slot)
{
    static int off = -1;
    static uint64_t at;
    uint64_t now = GetTickCount64();
    if (off < 0 || now - at > 2000) {
        at = now;
        off =
            GetFileAttributesA("t2r-down-cut-dodge-old.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    if (off || !DtlOn() || !g_dtl.run.src || g_dtl.run.slot != slot
        || now >= g_dtl.run.end || !g_dtl.run.ent || !g_dtl.run.sid)
        return;
    float bl = 0.12f;
    uint64_t a[5] = { g_dtl.run.ent, ANIM_LAYER, 0, g_dtl.run.sid, 0 };
    memcpy(&a[2], &bl, 4);
    uint64_t r = DcCall(FNV64("stop-animating"), 5, a);
    DTLOG
        ("dtl: slot %u went down - the running %s clip %llx cut over %.2f s (the fall owns the body) -> %llx",
         slot, DTL_SN[g_dtl.run.src], (unsigned long long) g_dtl.run.sid,
         (double) bl, (unsigned long long) r);
    g_dtl.run.end = now;
    g_dtl.run.cancel_at = now;
}

// the attacker's contact ruled the victim's press TOO LATE, but the dodge replica started at the press kept the puppet ducking for 690 ms
// with the hit reaction queued behind it (34 damage on a dodging body). The ruling cuts the dodge (0.10 s) so the reaction plays at once. t2r-dodge-late-cut-old.txt = off.
static void DtlDodgeCut(uint32_t slot)
{
    uint64_t now = GetTickCount64();
    if (!DtlOn() || g_dtl.run.src != DTL_S_DODGE || g_dtl.run.slot != slot
        || now >= g_dtl.run.end || !g_dtl.run.ent || !g_dtl.run.sid)
        return;
    if (GetFileAttributesA("t2r-dodge-late-cut-old.txt") !=
        INVALID_FILE_ATTRIBUTES)
        return;
    float bl = 0.10f;
    uint64_t a[5] = { g_dtl.run.ent, ANIM_LAYER, 0, g_dtl.run.sid, 0 };
    memcpy(&a[2], &bl, 4);
    uint64_t r = DcCall(FNV64("stop-animating"), 5, a);
    DTLOG
        ("dtl: slot %u's dodge replica %llx cut (TOO LATE: the blow lands) -> %llx",
         slot, (unsigned long long) g_dtl.run.sid, (unsigned long long) r);
    g_dtl.run.end = now;
    g_dtl.run.cancel_at = now;
    if (slot < T2R_MAX_PEERS) {
        if (g_mdg.pd[slot].sid == a[3])
            g_mdg.pd[slot].end = 0;
        g_mdg_pp[slot].on = 0;
    }                           // (no second stop by the release tick; the dodge path stops)
}

// ps25 (melee_hitreact.c mhr_recv, flags 0x80): the victim left its reaction before the clip's exit - the timeline's record of that clip ends with it
static void DtlReactOver(uint32_t slot, uint64_t sid)
{
    if (g_dtl.run.src == DTL_S_REACT && g_dtl.run.slot == slot
        && g_dtl.run.sid == sid) {
        uint64_t now = GetTickCount64();
        g_dtl.run.end = now;
        g_dtl.run.cancel_at = now;
    }
}

// play one entry now on the partner puppet (age = how late it is)
static int DtlPlay(uint32_t slot, int src, uint64_t sid, uint32_t cut,
                    float age, int early)
{
    uint64_t ent = PuppetHandle(slot), now = GetTickCount64();
    if (!ent)
        return 0;
    float dur = MdgClipLen(sid);
    if (!(dur > 0.2f)) {
        DTLOG
            ("dtl: %s clip %llx on slot %u not loaded here - the streamed pose only",
             DTL_SN[src], (unsigned long long) sid, slot);
        return 0;
    }
    int running = g_dtl.run.src && g_dtl.run.slot == slot
        && now < g_dtl.run.end;
    float bl = src == DTL_S_DODGE ? (MdsFull()? 0.25f : 0.20f) : (running
                                                                   ||
                                                                   early) ?
        DTL_XFADE : src == DTL_S_REACT ? 0.10f : 0.15f;
    // a dodge started on the frame psync released the puppet (SENDER-CUT abort) blended in 0.20 s from a
    // base pose the net replay had not refilled yet (replay bypassed 39 frames, "jitter: no pose 39 frames") - 5 frames of bind / A pose. Within 250 ms of a
    // psync release the dodge clip cuts in (blend 0.03 s). t2r-dtl-rel-blend-old.txt = the old 0.20 s blend.
    if (src == DTL_S_DODGE && slot < T2R_MAX_PEERS && g_dtlRelMs[slot]
        && now - g_dtlRelMs[slot] < 250
        && GetFileAttributesA("t2r-dtl-rel-blend-old.txt") ==
        INVALID_FILE_ATTRIBUTES) {
        DTLOG
            ("dtl: dodge on slot %u %llu ms after its psync release - cut in (blend 0.03 s, no blend from the unfilled base pose)",
             slot, (unsigned long long) (now - g_dtlRelMs[slot]));
        bl = 0.03f;
    }
    // (hitting a passive player showed the wrong hit reactions; 'dtl: reaction clip ... on slot 0's puppet ...
    // blend-in 0.20 s (crossfade over the running clip)'; SP: the victim's reaction starts on the contact frame with NO blend - the one-frame pose change is the
    // impact, 102 of 102): a reaction on the puppet cuts in over 2 frames. t2r-dtl-react-blend-old.txt = 0.10 / 0.20 s.
    if (src == DTL_S_REACT
        && GetFileAttributesA("t2r-dtl-react-blend-old.txt") ==
        INVALID_FILE_ATTRIBUTES) {
        bl = MEL_HIT_BLEND;
        g_melFloorOnce = MEL_HIT_BLEND;
    }
    uint64_t r = MhrAnimate(ent, sid, 0.0f, bl);
    g_melFloorOnce = -1.0f;
    if (r == ~0ULL) {
        DTLOG("dtl: %s clip %llx on slot %u refused by animate_",
              DTL_SN[src], (unsigned long long) sid, slot);
        return 0;
    }
    if (running)
        g_dtl.xfades++;
    float win = src == DTL_S_REACT ? (float) cut / 2000.0f : dur * 0.5f;
    DtlCatchup(ent, sid, age, win, 1.0f, DTL_SN[src]);
    uint64_t end;
    uint64_t cut_end = 0;       // ps25: a reaction's exit point (where another clip may take the puppet) is still its cut; the clip itself stays to its exit
    if (src == DTL_S_REACT) {
        // ('reaction clip ... blend-out at +352 ms', every blow): the clip is no longer cut at the victim's control return - it stays
        // to the reaction's exit (melee_hitreact.c mhr_exit_ms_of; the victim's 0x80 notice ends it earlier, the next blow's reaction replaces it)
        uint64_t kx = MhrExitMsOf(sid, cut);
        cut_end =
            now + (cut > (uint32_t) age + 60 ? cut - (uint32_t) age : 60);
        end = now + (kx > (uint64_t) age + 60 ? kx - (uint64_t) age : 60);
        MhrWpPupBegin(slot, ent, end - now); // (ps25: the puppet's weapon-hold layers, as on the victim's own body)
        g_mhr.pd[slot].ent = ent;
        g_mhr.pd[slot].sid = sid;
        g_mhr.pd[slot].end = end;
    } else if (src == DTL_S_DODGE) {
        uint64_t len = (uint64_t) (dur * MD_CLIP_FRAC * 1000.0f);
        if (len > MD_CLIP_MAX_MS)
            len = MD_CLIP_MAX_MS;
        len = MdgSpHoldMs((int) slot, sid, len);     // melee_dodge.c: SP duel dodge = held to the clip's flat part, as on the dodger's own screen
        end =
            now + (len > (uint64_t) age + 60 ? len - (uint64_t) age : 60);
        g_mdgPupAnimMs[slot] = now;
        MdgPpArm(slot, ent, sid, age);        // the puppet travels the dodger's own scaled root path from here (melee_dodge.c)
        g_mdg.pd[slot].ent = ent;
        g_mdg.pd[slot].sid = sid;
        g_mdg.pd[slot].end = end;
        MdgDodgeSayAt(slot, ent, 150 - (int) age);   // SP's grunt time (melee_dodge.c MDS_*), minus how late the clip starts here
    } else {
        end =
            now + (uint64_t) (dur * 1000.0f) - (uint64_t) (age <
                                                           dur *
                                                           900.0f ? age :
                                                           0) - 150;
        if (src == DTL_S_SWING && g_dtl.pr.on && g_dtl.pr.sid == sid
            && end < g_dtl.pr.contact + 200)
            end = g_dtl.pr.contact + 200;       // (v5-finwind: a replica ending AT its contact - the finisher intro - holds until the pair START takes it)
        g_mhr.pd[slot].ent = ent;
        g_mhr.pd[slot].sid = sid;
        g_mhr.pd[slot].end = end;       // blended out by mhr_tick at its end unless the pair takes the puppet first
    }
    if (src == DTL_S_SWING)
        PupSwingHideArm(slot);       // a gun in the swinger's hand is hidden while its swings play (t2r-ext.c pup_swing_hide)
    g_dtl.run.src = src;
    g_dtl.run.slot = slot;
    g_dtl.run.ent = ent;
    g_dtl.run.sid = sid;
    g_dtl.run.t0 = now;
    g_dtl.run.end = end;
    {
        uint64_t ce = cut_end ? cut_end : end;
        g_dtl.run.cancel_at =
            src == DTL_S_SWING ? g_dtl.pr.contact : ce >
            now + 200 ? ce - 200 : now;
    }                           // a swing replica holds to its contact; others to their blend-out
    DTLOG
        ("dtl: %s clip %llx on slot %u's puppet from phase 0, %.0f ms late, blend-in %.2f s%s, blend-out at +%llu ms",
         DTL_SN[src], (unsigned long long) sid, slot, (double) age,
         (double) bl, running ? " (crossfade over the running clip)" : "",
         (unsigned long long) (end - now));
    return 1;
}

// the arbiter for a clip that wants the partner puppet. 1 = handled here (played / queued / dropped), 0 = not the duel partner: the caller's own path
static int DtlPupRoute(uint32_t slot, int src, uint64_t sid,
                         uint32_t cut, float age)
{
    if (!DtlPartner(slot) || !sid)
        return 0;
    uint64_t now = GetTickCount64();
    // ps40 D5 (rec39a A [16690] 'dtl: dodge on slot 0 15 ms after its psync release - cut in', 'catch-up dodge clip ...: 682 ms late', 'PATH armed ... 682 ms late', [16710]
    // 'travel 0.02 m at 700 ms'; again [19946] 525 ms late): the press message was NOT late - its dodge clip was QUEUED behind 'the running pair' because the dodger's
    // puppet still played the attacker half of its last blow's pair on our screen (the dodger left that follow-through on its own screen at the press, ps36: 'our
    // -hit follow-through is left for the dodge') and waited there for the pair's own blend-out. The dodge of a puppet that is the ATTACKER half of the duel pair
    // on us lets that half go now (its blend-out + hand-back glide, as the next swing's replica does in dtl_pred_swing), the dodge crossfades in at once.
    // t2r-dtl-dodge-att-out-ps39.txt = old.
    if (src == DTL_S_DODGE && g_ps.role == 2 && g_ps.att == (int) slot
        && g_ps.ent_att == PuppetHandle(slot) && g_ps_pup[slot].on
        && g_ps.kind == PS_CTXDOWN && MduMove(g_ps.move)
        && GetFileAttributesA("t2r-dtl-dodge-att-out-ps39.txt") ==
        INVALID_FILE_ATTRIBUTES) {
        double t40 = PsyncMs() - g_ps.t0;
        PsyncAttOut(t40);
        static int nl40;
        if (LOGCAP(nl40, 60))
            DTLOG
                ("dtl: dodge of slot %u out of its -hit follow-through - the attacker half on its puppet let go at t=%.0f ms of the pair (age %.0f ms; not queued behind the pair; ps40)",
                 slot, t40, (double) age);
    }
    int rp = DtlRunningPrio(slot, now);
    if (src == DTL_S_DODGE && DtlDodgeAfterHit(slot)) {
        g_dtl.dropped++;
        DTLOG
            ("dtl: dodge clip %llx on slot %u dropped - our blow on it was already confirmed (its ACK), the press came after the contact",
             (unsigned long long) sid, slot);
        return 1;
    }
    if (rp == DTL_S_PAIR && src == DTL_S_REACT) {
        g_dtl.dropped++;
        DTLOG
            ("dtl: %s clip %llx on slot %u dropped - the duel pair owns the puppet (it is the reaction)",
             DTL_SN[src], (unsigned long long) sid, slot);
        return 1;
    }
    if (rp == DTL_S_DOWN) {
        g_dtl.dropped++;
        DTLOG("dtl: %s on slot %u dropped - down / dead", DTL_SN[src],
              slot);
        return 1;
    }
    if (rp && src <= rp) {      // wait for the running clip's blend-out point (a newer entry of the same kind supersedes an older queued one)
        for (int i = 0; i < 6; i++)
            if (g_dtl.q[i].used && g_dtl.q[i].src <= src) {
                g_dtl.q[i].used = 0;
                g_dtl.dropped++;
                DTLOG("dtl: queued %s superseded by a newer %s - dropped",
                      DTL_SN[g_dtl.q[i].src], DTL_SN[src]);
            }
        for (int i = 0; i < 6; i++)
            if (!g_dtl.q[i].used) {
                g_dtl.q[i].used = 1;
                g_dtl.q[i].src = src;
                g_dtl.q[i].slot = slot;
                g_dtl.q[i].sid = sid;
                g_dtl.q[i].cut = cut;
                g_dtl.q[i].age0 = age;
                g_dtl.q[i].at = now;
                g_dtl.q[i].stale_at =
                    now + (uint64_t) (DtlStaleMs(src, cut) >
                                      age ? DtlStaleMs(src,
                                                         cut) - age : 0);
                g_dtl.queued++;
                DTLOG
                    ("dtl: %s clip %llx on slot %u QUEUED behind the running %s (its blend-out point in %lld ms)",
                     DTL_SN[src], (unsigned long long) sid, slot,
                     DTL_SN[rp],
                     rp ==
                     DTL_S_PAIR ? -1LL : (long long) (g_dtl.run.cancel_at -
                                                      now));
                return 1;
            }
        g_dtl.dropped++;
        return 1;
    }
    if (src == DTL_S_SWING && g_dtl.pr.on && g_dtl.pr.sid != sid)
        g_dtl.corrected++;
    DtlPlay(slot, src, sid, cut, age, rp != 0);
    return 1;
}

static void DtlQueueTick(uint64_t now)
{
    for (int i = 0; i < 6; i++) {
        if (!g_dtl.q[i].used)
            continue;
        if (!DtlPartner(g_dtl.q[i].slot)) {
            g_dtl.q[i].used = 0;
            continue;
        }
        float age = g_dtl.q[i].age0 + (float) (now - g_dtl.q[i].at);
        if (now >= g_dtl.q[i].stale_at) {
            g_dtl.q[i].used = 0;
            g_dtl.dropped++;
            DTLOG
                ("dtl: queued %s clip %llx on slot %u dropped stale (%.0f ms late, never played out of order)",
                 DTL_SN[g_dtl.q[i].src],
                 (unsigned long long) g_dtl.q[i].sid, g_dtl.q[i].slot,
                 (double) age);
            continue;
        }
        if (g_dtl.q[i].src == DTL_S_DODGE
            && DtlDodgeAfterHit(g_dtl.q[i].slot)) {
            g_dtl.q[i].used = 0;
            g_dtl.dropped++;
            DTLOG
                ("dtl: queued dodge clip on slot %u dropped - our blow was confirmed (ACK) before it played",
                 g_dtl.q[i].slot);
            continue;
        }
        int rp = DtlRunningPrio(g_dtl.q[i].slot, now);
        if (rp && g_dtl.q[i].src <= rp)
            continue;
        g_dtl.q[i].used = 0;
        DtlPlay(g_dtl.q[i].slot, g_dtl.q[i].src, g_dtl.q[i].sid,
                 g_dtl.q[i].cut, age, 0);
    }
}

// ---- prediction: the partner's duel SWING message (its move-start = press frame) starts the swing clip on its puppet now; tce = its contact on its clock (ms after its start)
static void DtlPredSwing(uint32_t slot, int row, int tce, float ow)
{
    if (!DtlPartner(slot) || row < 0 || row >= (int) MDU_MOVES_N)
        return;
    const MduRowT *m = &MDU_MOVES[row];
    if (!m->swing || !*m->swing || m->tc_ms <= 0)
        return;
    uint64_t sid = FNV64(m->swing), now = GetTickCount64();
    if (g_ps.role == 2 && g_ps.att == (int) slot
        && g_ps.ent_att == PuppetHandle(slot) && g_ps_pup[slot].on)
        PsyncAttOut(PsyncMs() - g_ps.t0);    // the last blow's attacker half on the puppet lets go (its blend-out, hand-back glide) - the next swing crossfades in (as melee_duel_pin.c does)
    g_dtl.pr.on = 1;
    g_dtl.pr.row = row;
    g_dtl.pr.slot = slot;
    g_dtl.pr.sid = sid;
    g_dtl.pr.t0 = now;
    g_dtl.pr.swing_t0 = now - (uint64_t) ow;
    g_dtl.pr.contact =
        now - (uint64_t) ow + (uint64_t) (tce > 0 ? tce : m->tc_ms);
    g_dtl.predicted++;
    // the sender's windup stretch (tce - tc = its one-way latency) already moves its contact to our screen's time: then the clip plays at 1.0 from phase 0.
    // Without it (or a larger lateness) the clip catches up (dtl_play -> dtl_catchup)
    float late = ow - (float) ((tce > m->tc_ms) ? (tce - m->tc_ms) : 0);
    if (late < 0.0f)
        late = 0.0f;
    // melee_duel_contact.c: the replica plays at the sender's own windup speed from phase 0 and reaches its contact frame when the pair START arrives
    // (now + tce: sent at the sender's contact, one-way later here) - the fist and the victim half meet on this screen; mct_tick drives its rate
    if (MctOn()) {
        late = 0.0f;
        g_dtl.pr.contact = now + (uint64_t) (tce > 0 ? tce : m->tc_ms);
    }
    if (!DtlPupRoute(slot, DTL_S_SWING, sid, 0, late)) {
        g_dtl.pr.on = 0;
        return;
    }
    DTLOG
        ("dtl: PREDICTED swing %s (%s) on slot %u's puppet from its SWING message: contact on our clock in %lld ms (sender windup +%d ms, one-way %.0f ms)",
         m->move, m->swing, slot, (long long) (g_dtl.pr.contact - now),
         tce - m->tc_ms, (double) ow);
}

// v5-finwind (melee_duel.c g_mdfwu): the partner's lethal swing is its finisher's SP intro - that clip on its puppet from the SWING message (same arbiter / priority as a
// swing replica). It ends at the contact (the -sync pair's start): paced (melee_duel_contact.c on) it runs at intro / tce from phase 0 and reaches its end when the
// pair START arrives (tce after this SWING here); else x1 with the catch-up for the lateness the sender's windup stretch did not cover
static void DtlPredFin(uint32_t slot, int row, int fin, int tce,
                         float ow)
{
    if (fin < 0 || fin >= (int) CD_N || !CdMfIntro(fin))
        return;
    const char *ic = CD_PAIRS[fin].seg[0].att;
    uint64_t sid = FNV64(ic), now = GetTickCount64(), ph =
        PuppetHandle(slot);
    float im = CdSegLen(fin, 0) * 1000.0f;
    if (!(im > 50.0f) || !ph || tce <= 0)
        return;
    if (!DtlPartner(slot)) {   // timeline off: the clip straight on the puppet
        uint64_t r = MhrAnimate(ph, sid, 0.0f, 0.15f);
        if (r != ~0ULL && (float) tce > im + 1.0f)
            DTL_SPEED(ph, sid, im / (float) tce);
        Xlog("dtl: (timeline off) finisher windup %s on slot %u's puppet from its SWING message -> %llx at x%.2f", ic, slot, (unsigned long long) r, (double) ((float) tce > im + 1.0f ? im / (float) tce : 1.0f));
        return;
    }
    if (g_ps.role == 2 && g_ps.att == (int) slot && g_ps.ent_att == ph
        && g_ps_pup[slot].on)
        PsyncAttOut(PsyncMs() - g_ps.t0);    // (the last blow's attacker half lets go, as dtl_pred_swing)
    int paced = MctOn();
    float late = ow - ((float) tce - im);
    if (late < 0.0f || paced)
        late = 0.0f;
    g_dtl.pr.on = 1;
    g_dtl.pr.row = row;
    g_dtl.pr.slot = slot;
    g_dtl.pr.sid = sid;
    g_dtl.pr.t0 = now;
    g_dtl.pr.swing_t0 = now - (uint64_t) ow;
    g_dtl.pr.contact =
        paced ? now + (uint64_t) tce : now - (uint64_t) ow +
        (uint64_t) tce;
    g_dtl.predicted++;
    if (!DtlPupRoute(slot, DTL_S_SWING, sid, 0, late)) {
        g_dtl.pr.on = 0;
        return;
    }
    int played = g_dtl.run.sid == sid && g_dtl.run.slot == slot
        && g_dtl.run.t0 == now;
    if (played && paced && (float) tce > im + 1.0f)
        DTL_SPEED(ph, sid, im / (float) tce);
    DTLOG
        ("dtl: PREDICTED finisher windup %s (SP intro %s, %.0f ms) on slot %u's puppet from its SWING message: %s, contact on our clock in %lld ms (sender windup +%.0f ms, one-way %.0f ms)%s",
         CD_PAIRS[fin].name + 3, ic, (double) im, slot,
         played ? "playing" : "queued behind the running clip",
         (long long) (g_dtl.pr.contact - now), (double) ((float) tce - im),
         (double) ow, played && paced
         && (float) tce > im + 1.0f ? " - paced to the START" : late >
         0.0f ? " - catch-up for the lateness" : "");
}

// paired.c psync_play: a duel pair takes the partner's puppet (the confirmation of the predicted swing / reaction)
static void DtlNotePair(uint32_t slot)
{
    if (!g_dtl.on)
        return;
    if (g_dtl.pr.on && g_dtl.pr.slot == slot) {
        g_dtl.conf_same++;
        g_dtl.pr.on = 0;
        DTLOG
            ("dtl: predicted swing on slot %u CONFIRMED by its pair (%lld ms vs the predicted contact) - crossfade into the pair",
             slot, (long long) (GetTickCount64() - g_dtl.pr.contact));
    }
    for (int i = 0; i < 6; i++)
        if (g_dtl.q[i].used && g_dtl.q[i].slot == slot
            && g_dtl.q[i].src < DTL_S_DODGE) {
            g_dtl.q[i].used = 0;
            g_dtl.dropped++;
        }
    if (g_dtl.run.slot == slot)
        g_dtl.run.src = 0;
}

// ---- confirmation: a trade (paired.c psync_begin_remote, before the busy refusal). 1 = we lose: the caller aborts our pair and plays theirs
static int DtlTradeYield(const PsyncWire *w, uint32_t from)
{
    if (!DtlOn() || g_ps.role != 1 || w->kind != PS_CTXDOWN
        || g_ps.kind != PS_CTXDOWN || !MduMove(w->move)
        || !MduMove(g_ps.move) || g_ps.vic != (int) from)
        return 0;
    double now = PsyncMs(), theirs =
        now - PsyncOneway((int) from) - (double) w->sent_ms, ours =
        g_ps.t0 - g_ps.clk_shift + g_ps.skip_ms, diff = theirs - ours;
    uint32_t self = PsyncSelf();
    if (diff > DTL_TRADE_WIN_MS) {      // not a trade: our blow landed long before - its pair is only in its tail here (their hand-off came earlier on their screen); the old busy refusal threw their blow away
        DTLOG
            ("dtl: slot %u's blow %+.0f ms after ours - not a trade, our pair is in its tail: it yields, theirs plays (was a BUSY refusal = a lost blow)",
             from, diff);
        return 1;
    }
    int yield = diff < -DTL_TIE_MS || (diff <= DTL_TIE_MS && from < self);
    if (yield)
        g_dtl.trades_lost++;
    else
        g_dtl.trades_won++;
    DTLOG
        ("dtl: TRADE with slot %u: its contact %+.0f ms vs ours (one-way %.0f) - %s",
         from, diff, (double) PsyncOneway((int) from),
         yield ? (diff <
                  -DTL_TIE_MS ?
                  "theirs was first: our pair yields, theirs plays (crossfade)"
                  : "a tie: the lower slot wins - theirs plays") : (diff >
                                                                    DTL_TIE_MS
                                                                    ?
                                                                    "ours was first: theirs is refused"
                                                                    :
                                                                    "a tie: the lower slot wins - ours stays"));
    return yield;
}

// melee_duel.c mdu_recv SWING: how much earlier the victim's dodge window closes so the verdict reaches the attacker inside its hit-confirm hold (paired.c vlag, cap 140 ms)
static int DtlWindowTrim(uint32_t slot)
{
    if (!DtlPartner(slot))
        return 0;
    int need = (int) (2.0f * PsyncOneway((int) slot)) + 34, t = need - g_dtlTrimHold;        // (a finisher's hold is 260 ms - the dodger keeps the whole window up to its contact)
    if (t <= 0)
        return 0;
    if (t > 120)
        t = 120;
    g_dtl.win_trims++;
    return t;
}

// melee_duel.c mdu_contact: extra reach for the partner's own motion over the latency (its puppet shows it one-way + the jitter buffer late)
static float DtlReachBonus(int s)
{
    if (!DtlOn() || s < 0 || s >= T2R_MAX_PEERS || !g_dtl.lf[s].np_ok)
        return 0.0f;
    float v =
        __builtin_sqrtf(g_dtl.lf[s].nv[0] * g_dtl.lf[s].nv[0] +
                        g_dtl.lf[s].nv[2] * g_dtl.lf[s].nv[2]), b =
        v * (PsyncOneway(s) + 50.0f) / 1000.0f;
    return b > 0.9f ? 0.9f : b;
}

static void DtlNoteReach(int s, float d, float lim, float bonus)
{
    g_dtl.reach_saves++;
    DTLOG
        ("dtl: reach - slot %d at %.2f m was past %.2f m, inside with its latency motion +%.2f m: contact kept",
         s, (double) d, (double) lim, (double) bonus);
}

// paired.c psync_abort: a duel pair cut before its hand-off (the next blow / a dodge / a break): logged as a TRIM when more than a blend of it was left
static void DtlNoteAbort(const char *why)
{
    if (!g_dtl.on || g_ps.kind != PS_CTXDOWN || !MduMove(g_ps.move)
        || !(g_ps.kill_s > 0.0f))
        return;
    double left = g_ps.kill_s * 1000.0 - (PsyncMs() - g_ps.t0);
    if (left > 350.0)
        DtlTrim(g_ps.role ==
                 2 ? "our victim half of a duel pair" : g_ps.role ==
                 1 ? "our attacker pair (both halves)" : "a duel pair",
                 (int) left, why);
}

// ---- BODY (v5-body; jitters and animations cutting into each other at ~160 ms RTT; remote POV clips 63fdb756 / a4371fab / 56868857):
// continuity of both duel bodies on our screen. Opt-OUT: t2r-duel-no-body.txt (re-read every 2 s; needs the timeline on). Design: re/melee-duel-timeline.md section 8.
//  HAND-BACK  the duel partner's puppet released by psync (its half's blend-out, the hand-off, an abort) is handed to dtl_locator's spring at once (dtl_hb_take)
//             instead of psync's 0.25-0.5 s smoothstep onto the RAW net locator: that glide passed every net step after its first frame straight through
//             (winmm's interpolation restarts with the replay reset +0x77e4 one frame after the release; netsmooth's playout clock resyncs after the
//             >300 ms the gate kept ns_locator from running) - the sideways snap right after a stumble (56868857 f121-123). The spring starts from the
//             body with its displayed velocity; the correction speed grows with the offset (DTL_DRIFT + DTL_DRIFT_K x offset, <= DTL_DRIFT_MAX).
//  PULL       a duel half's start pull on the PUPPET (psync_drive, SP's quadratic ease-out) peaks at 2 x offset / T: a 1.5 m latency-inflated offset over
//             the 0.36 s pull = 8 m/s in its first frames (63fdb756 f87-89: 1.5 m in 2 frames). dtl_pull_s lengthens it so the peak stays <= DTL_PULL_V /
//             DTL_PULL_W (<= DTL_PULL_MAX s). Our own body is the engine's (animate_ blend-in) and is not touched; nor are the authored spots.
//  SPACING    outside psync's pairs (dtl owns the puppet) the displayed puppet never stands closer than DTL_MIN_SEP to our body: it is pushed out at <= DTL_SEP_V
//             (no one-frame push), its velocity toward us dropped (63fdb756 f160-165: her body pressed into his - the net put her where his body is).
//  TAIL       the filter ends only when position AND yaw are on the net (a 90 deg offset ended in one frame before), or DTL_TAIL_MAX_MS after the duel.
//  METRICS    per 30 s 'dtl: body 30 s' line (max unexplained step m/frame, max yaw deg/frame, min body distance outside pairs, pose gaps); 'dtl: YAW SNAP' per
//             frame over DTL_YAW_SNAP deg; every SNAP / YAW SNAP on US names the writer of that frame (teleport-player / set-object-locator caller, psync, mover).
#define DTL_DRIFT_K 0.8f        // m/s of extra correction speed per m of offset
#define DTL_DRIFT_MAX 2.7f      // m/s: correction speed cap at large offsets (a jog; one-frame steps stay impossible)
#define DTL_MIN_SEP 0.55f       // m: two capsules (~0.3 m radius each) minus contact slack; SP's duel spacing is ~1.0-1.2 m
#define DTL_SEP_V 3.0f          // m/s: push-out speed
#define DTL_TAIL_MAX_MS 4000    // after the duel the filter may run this long to converge (position + yaw)
#define DTL_PULL_V 6.0f         // m/s: peak puppet speed of a duel half's start pull (a lunge)
#define DTL_PULL_W 540.0f       // deg/s: its peak turn rate
#define DTL_PULL_MAX 0.70f      // s: never longer than this
#define DTL_YAW_SNAP 45.0f      // deg in one frame beyond what clip / turn rate explain
static struct {
    int on, cfg_logged;
    uint64_t cfg_at;
    // 30 s metrics: [0] = us, [1] = the partner's puppet
    float step_max[2], yaw_max[2], yaw_unexpl_max[2], sep_min;
    uint32_t yaw_snaps[2], sep_frames, sep_pushes, hb_takes, pull_ext,
        sep_n;
    uint32_t gap_cur, gap_max, gap_n, own_cur, own_max;
    uint32_t posew_last;
    int gap_slot;               // pose gaps of the partner: no net pose and nobody animating it (real freeze) / psync-owned
    struct {
        float yaw, yv, plyaw;
        int ok;
    } yd[2];                    // yaw tracker per body (snap tick)
    struct {
        uint32_t uid;
        float T;
    } pull[2];                  // start pull length per half of the current uid
    int sep_on[T2R_MAX_PEERS];
} g_dtb;
static int DtlBodyOn(void)
{
    if (!g_dtl.on)
        return 0;
    uint64_t now = GetTickCount64();
    if (!g_dtb.cfg_at || now - g_dtb.cfg_at >= 2000) {
        g_dtb.cfg_at = now ? now : 1;
        FILE *f = ExeSideOpen("t2r-duel-no-body.txt", "r");
        int o = f == NULL;
        if (f)
            fclose(f);
        if (o != g_dtb.on || !g_dtb.cfg_logged) {
            Xlog("dtl: body %s",
                 o ?
                 "ON - duel partner hand-back through the bounded spring, start pull <= 6 m/s, min spacing 0.55 m outside pairs, per-30 s body metrics (t2r-duel-no-body.txt = off)"
                 : "OFF (t2r-duel-no-body.txt)");
            g_dtb.cfg_logged = 1;
        }
        g_dtb.on = o;
    }
    return g_dtb.on;
}

// paired.c psync_hb_arm: the duel partner's puppet was just released by psync. 1 = the spring owns its glide onto the net (psync's hand-back not armed)
static int DtlHbTake(int s, uint64_t h, const char *why)
{
    if (s < 0 || s >= T2R_MAX_PEERS || !h || !DtlBodyOn()
        || g_peerDowned[s] || PsyncDead(h))
        return 0;
    uint64_t now = GetTickCount64();
    if (g_mdu.slot != s && now >= g_dtl.lf[s].tail_until)
        return 0;               // not the duel partner (or its tail): psync's own glide
    g_dtl.lf[s].ok = 0;
    g_dtl.lf[s].hb = 1;         // restart from the body (dtl_locator's first frame), with its displayed velocity
    if (now + DTL_TAIL_MS > g_dtl.lf[s].tail_until)
        g_dtl.lf[s].tail_until = now + DTL_TAIL_MS;
    if (g_dtl.lf[s].tail_until > g_dtl.tail_any)
        g_dtl.tail_any = g_dtl.lf[s].tail_until;
    g_dtb.hb_takes++;
    g_dtlRelMs[s] = now;
    DTLOG
        ("dtl: slot %d released by psync (%s) - the duel spring glides it onto the net (no raw net hand-back)",
         s, why);
    return 1;
}

// paired.c psync_drive: the start pull length of half k on a puppet (dbl = psync's). Longer only for a duel half whose offset would move faster than a lunge
static float DtlPullS(int k, uint64_t ent, float dbl)
{
    if (k < 0 || k > 1 || !ent || ent == SID_PLAYER || !g_ps.drive_easeout
        || !g_ps.start_ok[k] || !g_ps.place_ok[k]
        || g_ps.kind != PS_CTXDOWN || !MduMove(g_ps.move)
        || !DtlBodyOn())
        return dbl;
    if (g_dtb.pull[k].uid == g_ps.uid && g_dtb.pull[k].T > 0.0f)
        return g_dtb.pull[k].T > dbl ? g_dtb.pull[k].T : dbl;
    const float *a = g_ps.start_loc[k], *b = g_ps.place[k];
    float d =
        __builtin_sqrtf((b[0] - a[0]) * (b[0] - a[0]) +
                        (b[2] - a[2]) * (b[2] - a[2])), ang =
        __builtin_fabsf(Uc4Wrap(Uc4Yaw(b) - Uc4Yaw(a)));
    float T = dbl, need = fmaxf(2.0f * d / DTL_PULL_V, 2.0f * ang / DTL_PULL_W);        // ease-out 1-(1-u)^2: peak rate = 2 x offset / T
    if (need > T)
        T = need > DTL_PULL_MAX ? (DTL_PULL_MAX >
                                   dbl ? DTL_PULL_MAX : dbl) : need;
    g_dtb.pull[k].uid = g_ps.uid;
    g_dtb.pull[k].T = T;
    if (T > dbl + 0.01f) {
        g_dtb.pull_ext++;
        DTLOG
            ("dtl: u%08x %s half's start pull on the puppet %.2f -> %.2f s (%.2f m / %.0f deg off its spot: peak %.1f m/s, %.0f deg/s instead of %.1f m/s)",
             g_ps.uid, k ? "victim" : "attacker", (double) dbl, (double) T,
             (double) d, (double) ang, (double) (2.0f * d / T),
             (double) (2.0f * ang / T), (double) (2.0f * d / dbl));
    }
    return T;
}

// dtl_locator: the displayed puppet keeps DTL_MIN_SEP from our body (outside psync's pairs - psync places the puppet then, this never runs)
static void DtlSep(int s, typeof(&g_dtl.lf[0]) f, float dt,
                    const float *net)
{
    float b[16];
    const float *me = PsyncLoc(SID_PLAYER, b);
    if (!me || !PsLocOk(me) || LocalHp() <= 0.0f || g_downed) {
        g_dtb.sep_on[s] = 0;
        return;
    }
    float dx = f->d[0] - me[0], dz = f->d[2] - me[2], dy =
        f->d[1] - me[1], dist = __builtin_sqrtf(dx * dx + dz * dz);
    if (dist >= DTL_MIN_SEP || __builtin_fabsf(dy) > 1.0f) {
        g_dtb.sep_on[s] = 0;
        return;
    }
    float ux, uz;
    if (dist > 0.02f) {
        ux = dx / dist;
        uz = dz / dist;
    } else {                    // on top of us: out toward where the net has it, else behind our facing
        float nx = net[0] - me[0], nz = net[2] - me[2], nl =
            __builtin_sqrtf(nx * nx + nz * nz);
        if (nl > 0.02f) {
            ux = nx / nl;
            uz = nz / nl;
        } else {
            float y = Uc4Yaw(me) * 0.01745329f;
            ux = __builtin_sinf(y);
            uz = __builtin_cosf(y);
        }
    }
    float push = DTL_MIN_SEP - dist, st = DTL_SEP_V * dt;
    if (st > push)
        st = push;
    f->d[0] += ux * st;
    f->d[2] += uz * st;
    float vr = f->v[0] * ux + f->v[2] * uz;
    if (vr < 0.0f) {
        f->v[0] -= ux * vr;
        f->v[2] -= uz * vr;
    }                           // no velocity into our body (inelastic)
    g_dtb.sep_frames++;
    if (!g_dtb.sep_on[s]) {
        g_dtb.sep_on[s] = 1;
        g_dtb.sep_pushes++;
        DTLOG
            ("dtl: slot %d puppet %.2f m from our body (net %.2f m) - pushed out to %.2f m at <= %.1f m/s",
             s, (double) dist,
             (double) __builtin_sqrtf((net[0] - me[0]) * (net[0] - me[0]) +
                                      (net[2] - me[2]) * (net[2] - me[2])),
             (double) DTL_MIN_SEP, (double) DTL_SEP_V);
    }
}

// ---- position: the partner puppet's displayed locator (locator_hook). 1 = we set it
static int DtlLocator(uint64_t h, void *loc)
{
    if (!h || !g_dtl.on)
        return 0;
    if (g_mdu.slot < 0 && GetTickCount64() >= g_dtl.tail_any)
        return 0;               // (cheap out: no duel, no tail running)
    int s = -1;
    for (int i = 0; i < T2R_MAX_PEERS; i++)
        if (PuppetHandle((uint32_t) i) == h) {
            s = i;
            break;
        }
    if (s < 0)
        return 0;
    uint64_t now = GetTickCount64();
    int duel = g_mdu.slot == s, body = DtlBodyOn();
    if (duel) {
        g_dtl.lf[s].tail_until = now + DTL_TAIL_MS;
        g_dtl.lf[s].tail_hard = now + DTL_TAIL_MAX_MS;
        uint64_t ta =
            body ? g_dtl.lf[s].tail_hard : g_dtl.lf[s].tail_until;
        if (ta > g_dtl.tail_any)
            g_dtl.tail_any = ta;
    }
    // (v5-body: the tail ends on convergence - position AND yaw - or at its hard limit; before, a body still 0.5 m / 90 deg off was handed to the raw net at 1.5 s)
    if (!duel && now >= g_dtl.lf[s].tail_until
        && !(body && g_dtl.lf[s].ok && now < g_dtl.lf[s].tail_hard)) {
        int was = g_dtl.lf[s].ok && body;
        g_dtl.lf[s].ok = 0;
        g_dtl.lf[s].np_ok = 0;
        g_dtl.lf[s].hb = 0;
        if (was) {              // (still off the net at the hard limit: psync's smoothstep glide from the body, never the raw net in one frame)
            DTLOG
                ("dtl: slot %d tail limit (%u frames, max offset %.2f m) - psync's hand-back glide finishes it",
                 s, g_dtl.lf[s].n, (double) g_dtl.lf[s].maxe);
            g_dtl.lf[s].tail_hard = 0;
            if (!g_peerDowned[s] && !PsyncDead(h)) {
                PsyncHbArm(s, h, "duel spring tail limit");
                return PsyncLocatorHandback(h, loc);
            }
        }
        return 0;
    }
    const PsyncBf *nb = (const PsyncBf *) loc;
    if (!nb || IsBadReadPtr(nb, sizeof *nb) || nb->magic[0] != 0x7fccccccu
        || nb->binding)
        return 0;
    float net[8];
    memcpy(net, nb->local, 32);
    net[3] = 1.0f;
    if (!PsLocOk(net) || PsyncDead(h) || g_peerDowned[s]) {
        int was = g_dtl.lf[s].ok;
        g_dtl.lf[s].ok = 0;
        g_dtl.lf[s].hb = 0;
        // v5-body: downed while the spring still glided it (a duel hand-off into a down: MSG_DOWNED lands after the release) - psync's own hand-back glide from
        // where the body stands now, not the raw net locator this frame (dtl_hb_take refuses a downed body, so psync_hb_arm arms its own glide)
        if (was && body && PsLocOk(net) && !PsyncDead(h)) {
            PsyncHbArm(s, h, "downed in the duel spring");
            return PsyncLocatorHandback(h, loc);
        }
        return 0;
    }
    double t = PsyncMs();
    typeof(&g_dtl.lf[0]) f = &g_dtl.lf[s];
    if (f->h != h) {
        f->ok = 0;
        f->np_ok = 0;
        f->h = h;
    }
    float dt = f->ok ? (float) ((t - f->t) / 1000.0) : 0.0f;
    if (f->ok && (dt <= 0.0005f)) {     // a second call in the same frame: the same output
        float o8[8] =
            { f->d[0], f->d[1], f->d[2], 1.0f, 0.0f,
      __builtin_sinf(f->yaw * 0.00872665f), 0.0f,
      __builtin_cosf(f->yaw * 0.00872665f) };
        {
            float c8[8];
            if (!MctMergeOld() && MctCarryAt(h, o8, c8)) {
                memcpy(o8, c8, sizeof c8);
                g_mct_carried_f = g_frame;
                g_mct_carried_h = h;
            }
        }                       // ps25: one writer
        static PsyncBf bf0;
        PsyncBfFromLoc(&bf0, o8);
        mo_write(h, &bf0, MO_SRC("duel.dtl_locator_same_frame"), 0);
        return 1;
    }
    if (dt > 0.1f)
        dt = 0.1f;
    f->t = t;
    // ('sdodge: puppet slot 1 travel 2.22 m at 1000 ms (SP 1.17 m)' = the 2 m fly): the dodger's streamed position is its character
    // controller, which follows the dodge clip's UNSCALED root (2.1 m authored) while its shown body is pinned to the scaled path (melee_dodge.c mdg_sync_tick). So on this screen the
    // puppet of a dodging partner is not carried further than DTL_DODGE_CAP from where it stood at the press; the dodger's own release sync then brings the net back inside it.
    // t2r-dodge-puppet-cap-old.txt = the old raw travel.
    {
        static uint64_t seen[T2R_MAX_PEERS];
        static float p0[T2R_MAX_PEERS][2];
        static int act[T2R_MAX_PEERS], old_ = -1;
        static uint64_t oc_;
        if (old_ < 0 || now - oc_ > 3000) {
            oc_ = now;
            old_ =
                GetFileAttributesA("t2r-dodge-puppet-cap-old.txt") !=
                INVALID_FILE_ATTRIBUTES;
        }
        uint64_t pa = g_mdgPupAnimMs[s];
        if (!old_ && pa && now - pa < 1300) {
            if (seen[s] != pa) {
                seen[s] = pa;
                act[s] = 1;
                p0[s][0] = net[0];
                p0[s][1] = net[2];
            }
            float ddx = net[0] - p0[s][0], ddz = net[2] - p0[s][1], dl =
                __builtin_sqrtf(ddx * ddx + ddz * ddz);
            if (act[s] && dl > DTL_DODGE_CAP) {
                static int nl_;
                if (LOGCAP(nl_, 30))
                    Xlog("dtl: slot %d dodge puppet travel %.2f m held to %.2f m (the net is the dodger's controller on the unscaled clip root; SP 1.17-1.45 m)", s, (double) dl, (double) DTL_DODGE_CAP);
                net[0] = p0[s][0] + ddx * DTL_DODGE_CAP / dl;
                net[2] = p0[s][1] + ddz * DTL_DODGE_CAP / dl;
            }
        } else
            act[s] = 0;
    }
    // net velocity (outlier steps rejected: those are corrections the spring absorbs, not motion)
    if (f->np_ok && dt > 0.0f) {
        float sx = net[0] - f->np[0], sy = net[1] - f->np[1], sz =
            net[2] - f->np[2], sp =
            __builtin_sqrtf(sx * sx + sz * sz) / dt;
        if (sp <= DTL_VMAX) {
            float k = 1.0f - __builtin_expf(-dt / 0.06f);
            f->nv[0] += (sx / dt - f->nv[0]) * k;
            f->nv[1] += (sy / dt - f->nv[1]) * k;
            f->nv[2] += (sz / dt - f->nv[2]) * k;
        } else
            f->jumps++;
    } else {
        f->nv[0] = f->nv[1] = f->nv[2] = 0.0f;
    }
    memcpy(f->np, net, 12);
    f->np_ok = 1;
    float ny = Uc4Yaw(net);
    if (!f->ok) {               // start from where the body stands now (no first-frame jump)
        float b[16];
        const float *l = PsyncLoc(h, b);
        if (!l || !PsLocOk(l))
            return 0;
        memcpy(f->d, l, 12);
        memcpy(f->v, f->nv, 12);
        f->yaw = Uc4Yaw(l);
        f->ok = 1;
        f->n = 0;
        f->maxe = 0.0f;
        // v5-body: a psync release keeps the body's own motion of its last frames (the clip's root motion as displayed) - no velocity step at the hand-back
        if (body && f->hb && g_dtl.sd[1].ok && g_dtl.sd[1].h == h) {
            float vl =
                __builtin_sqrtf(g_dtl.sd[1].v[0] * g_dtl.sd[1].v[0] +
                                g_dtl.sd[1].v[2] * g_dtl.sd[1].v[2]);
            if (vl < DTL_VMAX) {
                f->v[0] = g_dtl.sd[1].v[0];
                f->v[2] = g_dtl.sd[1].v[2];
            }
        }
        float e0 =
            __builtin_sqrtf((net[0] - l[0]) * (net[0] - l[0]) +
                            (net[2] - l[2]) * (net[2] - l[2]));
        DTLOG
            ("dtl: slot %d puppet position owned by the timeline (%s%s): starts %.2f m / %.0f deg off the net, spring %.1f rad/s, drift <= %.1f m/s",
             s, duel ? "duel" : "tail", f->hb ? ", psync release" : "",
             (double) e0, (double) __builtin_fabsf(Uc4Wrap(ny - f->yaw)),
             (double) DTL_OMEGA,
             (double) (body ? DTL_DRIFT_MAX : DTL_DRIFT));
        f->hb = 0;
        if (e0 > DTL_TELEPORT) {
            memcpy(f->d, net, 12);
            f->yaw = ny;
        }
        dt = 0.0f;
    }
    {                           // a dodging partner's puppet is placed on the dodge path (melee_dodge.c mdg_pp_pos), not on the spring; the spring continues from it after
        float ppx, ppz, pyaw;
        if (MdgPpPos(s, now, net, &ppx, &ppz, &pyaw)) {
            if (dt > 0.0f) {
                f->v[0] = (ppx - f->d[0]) / dt;
                f->v[2] = (ppz - f->d[2]) / dt;
                float vl =
                    __builtin_sqrtf(f->v[0] * f->v[0] + f->v[2] * f->v[2]);
                if (vl > DTL_VMAX) {
                    f->v[0] *= DTL_VMAX / vl;
                    f->v[2] *= DTL_VMAX / vl;
                }
            }
            f->d[0] = ppx;
            f->d[2] = ppz;
            if (dt > 0.0f) {
                f->d[1] +=
                    (net[1] - f->d[1]) * (1.0f -
                                          __builtin_expf(-dt / 0.08f));
                float dy = Uc4Wrap(pyaw - f->yaw), mx =
                    DTL_YAW_RATE * dt, st =
                    dy * (1.0f - __builtin_expf(-dt / 0.08f));
                if (st > mx)
                    st = mx;
                else if (st < -mx)
                    st = -mx;
                f->yaw = Uc4Wrap(f->yaw + st);
            }
            f->n++;
            float o8p[8] =
                { f->d[0], f->d[1], f->d[2], 1.0f, 0.0f,
         __builtin_sinf(f->yaw * 0.00872665f), 0.0f,
         __builtin_cosf(f->yaw * 0.00872665f) };
            if (PsLocOk(o8p)) {
                static PsyncBf bfp;
                PsyncBfFromLoc(&bfp, o8p);
                mo_write(h, &bfp, MO_SRC("dodge.puppet_path"), 0);
                return 1;
            }
        }
    }
    float e[3] =
        { f->d[0] - net[0], f->d[1] - net[1], f->d[2] - net[2] }, el =
        __builtin_sqrtf(e[0] * e[0] + e[2] * e[2]);
    if (el > DTL_TELEPORT) {
        DTLOG
            ("dtl: slot %d net position %.1f m away (respawn / teleport) - taken as is",
             s, (double) el);
        memcpy(f->d, net, 12);
        memcpy(f->v, f->nv, 12);
        f->yaw = ny;
        el = 0.0f;
        dt = 0.0f;
    }
    if (el > f->maxe)
        f->maxe = el;
    if (dt > 0.0f) {
        // critically damped spring on the error, in the frame moving with the net velocity: e'' = -w^2 e - 2 w e'
        int clip = g_dtl.run.src && g_dtl.run.slot == (uint32_t) s && now < g_dtl.run.end && el < 1.2f; // our clip plays: carry the net motion only, no correction mid-clip
        float w = DTL_OMEGA;
        for (int i = 0; i < 3; i++) {
            float rv = f->v[i] - f->nv[i];
            float a =
                clip ? -2.0f * w * rv : -w * w * e[i] - 2.0f * w * rv;
            rv += a * dt;
            f->v[i] = f->nv[i] + rv;
        }
        // the correction (velocity relative to the net) is capped: never more than DTL_DRIFT m/s horizontally, 4 m/s vertically
        float cx = f->v[0] - f->nv[0], cz = f->v[2] - f->nv[2], cl =
            __builtin_sqrtf(cx * cx + cz * cz);
        float cap = body ? fminf(DTL_DRIFT + DTL_DRIFT_K * el, DTL_DRIFT_MAX) : DTL_DRIFT;      // (v5-body: a 1.5 m release offset is not a 1 s slide at 1.5 m/s)
        if (cl > cap) {
            f->v[0] = f->nv[0] + cx * cap / cl;
            f->v[2] = f->nv[2] + cz * cap / cl;
        }
        float cy = f->v[1] - f->nv[1];
        if (cy > 4.0f)
            f->v[1] = f->nv[1] + 4.0f;
        else if (cy < -4.0f)
            f->v[1] = f->nv[1] - 4.0f;
        for (int i = 0; i < 3; i++)
            f->d[i] += f->v[i] * dt;
        float dy = Uc4Wrap(ny - f->yaw), mx = DTL_YAW_RATE * dt, st =
            dy * (1.0f - __builtin_expf(-dt / 0.05f));
        if (st > mx)
            st = mx;
        else if (st < -mx)
            st = -mx;
        f->yaw = Uc4Wrap(f->yaw + st);
        if (body)
            DtlSep(s, f, dt, net);
    }
    f->n++;
    if (!duel && el < 0.05f
        && (!body
            || (__builtin_fabsf(Uc4Wrap(ny - f->yaw)) < 3.0f
                && !g_dtb.sep_on[s]))) {
        DTLOG
            ("dtl: slot %d puppet back on the net position after the duel (%u frames, max offset %.2f m, %u net jumps absorbed)",
             s, f->n, (double) f->maxe, f->jumps);
        f->ok = 0;
        f->tail_until = 0;
        return 0;
    }
    float o8[8] =
        { f->d[0], f->d[1], f->d[2], 1.0f, 0.0f,
  __builtin_sinf(f->yaw * 0.00872665f), 0.0f,
  __builtin_cosf(f->yaw * 0.00872665f) };
    if (!PsLocOk(o8)) {
        f->ok = 0;
        return 0;
    }
    // ('motion: ent49d18d frame N: 2 writers [duel.dtl_locator ..., duel.mct_warp ...]' on every frame of every swing): the contact
    // carry (melee_duel_contact.c: the attacker's puppet onto the pair's contact spot over its wind-up) is applied to this pose before the ONE write; the filter's own
    // state (f->d / f->yaw) stays the uncarried pose, as before (mct read the written locator back). t2r-mct-merge-ps24.txt = the second write.
    {
        float c8[8];
        if (!MctMergeOld() && MctCarryAt(h, o8, c8)) {
            memcpy(o8, c8, sizeof c8);
            g_mct_carried_f = g_frame;
            g_mct_carried_h = h;
        }
    }
    static PsyncBf bf;
    PsyncBfFromLoc(&bf, o8);
    mo_write(h, &bf, MO_SRC("duel.dtl_locator"), 0);
    return 1;
}

// paired.c: psync took the puppet / handed it back - the filter restarts from the body when it is ours again (hand-back glide first)
static void DtlLocReset(int s)
{
    if (s >= 0 && s < T2R_MAX_PEERS)
        g_dtl.lf[s].ok = 0;
}

// ---- per frame (melee_duel.c mdu_tick, before its own work)
// v5-body: who moved OUR body this frame (named on a SNAP / YAW SNAP of us)
static uintptr_t DtlExtOff(const void *ra)
{
    static HMODULE m;
    if (!m)
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR) & DtlExtOff, &m);
    return m && ra ? (uintptr_t) ra - (uintptr_t) m : 0;
}

static const char *DtlOwnWriter(char *o, size_t n)
{
    int w = 0;
    o[0] = 0;
    if (g_tpFrame && g_frame - g_tpFrame <= 1 && w < (int) n)
        w += snprintf(o + w, n - w, " teleport-player from ext+%llx (%s);",
                      (unsigned long long) DtlExtOff(g_tpRa),
                      g_mo_last_tp_src >=
                      0 ? g_moSrc[g_mo_last_tp_src].name : "?");
    if (g_solOwnFrame && g_frame - g_solOwnFrame <= 1 && w < (int) n)
        w += snprintf(o + w, n - w,
                      " set-object-locator from ext+%llx (%s);",
                      (unsigned long long) DtlExtOff(g_solOwnRa),
                      g_moLastSolSrc >=
                      0 ? g_moSrc[g_moLastSolSrc].name : "?");
    if ((g_ps.role == 1 || g_ps.role == 2) && w < (int) n)
        w += snprintf(o + w, n - w,
                      " psync u%08x kind %d t=%.0f ms own drive %d out %d%d;",
                      g_ps.uid, g_ps.kind, PsyncMs() - g_ps.t0,
                      g_ps.own_drv, g_ps.out_done[0], g_ps.out_done[1]);
    if (g_md_fb.on && w < (int) n)
        w += snprintf(o + w, n - w, " dodge mover '%s';",
                      g_md_fb.tag ? g_md_fb.tag : "?");
    if (g_md_carry.on && w < (int) n)
        w += snprintf(o + w, n - w, " carry mover '%s';",
                      g_md_carry.tag ? g_md_carry.tag : "?");
    if (g_mpn.on && w < (int) n)
        w += snprintf(o + w, n - w, " pin state %d blow %d;", g_mpn.state,
                      g_mpn.k);
    if (!w)
        snprintf(o, n,
                 " no ext writer this frame (the engine: animate_ align / locomotion)");
    return o;
}

// v5-body: the partner's pose gaps - frames without a net pose (winmm's pose writer, g_posew_n) while nobody else animates it. psync-owned frames are
// counted apart: those are the 'jitter: slot N no pose for N frames' lines (their N = psync's 'frames N' of the same pair to the frame; a dead body's
// held pose is the long ones, e.g. 1579 frames of a corpse until the respawn) - the clip on the puppet poses it then, it is no freeze.
static void DtlGapTick(int s)
{
    if (s < 0 || s >= T2R_MAX_PEERS) {
        g_dtb.gap_slot = -1;
        g_dtb.gap_cur = g_dtb.own_cur = 0;
        return;
    }
    uint32_t n = g_posewN[s];
    if (g_dtb.gap_slot != s) {
        g_dtb.gap_slot = s;
        g_dtb.posew_last = n;
        g_dtb.gap_cur = g_dtb.own_cur = 0;
        return;
    }
    if (n != g_dtb.posew_last) {
        if (g_dtb.gap_cur >= 3) {
            g_dtb.gap_n++;
            DTLOG
                ("dtl: slot %d puppet had no net pose for %u frames with nothing else animating it (a real freeze)",
                 s, g_dtb.gap_cur);
        }
        g_dtb.posew_last = n;
        g_dtb.gap_cur = g_dtb.own_cur = 0;
        return;
    }
    if (g_ps_pup[s].on || g_ps_pup[s].corpse) {
        if (++g_dtb.own_cur > g_dtb.own_max)
            g_dtb.own_max = g_dtb.own_cur;
        return;
    }
    if (g_puppetDeadHere[s] || g_peerDowned[s])
        return;
    uint64_t now = GetTickCount64();
    if (g_dtl.run.src && g_dtl.run.slot == (uint32_t) s
        && now < g_dtl.run.end)
        return;                 // our queued clip poses it
    if (++g_dtb.gap_cur > g_dtb.gap_max)
        g_dtb.gap_max = g_dtb.gap_cur;
}

static void DtlSnapTick(double t)
{
    uint32_t self = PsyncSelf();
    int body = DtlBodyOn();
    float lp[2][3];
    int lp_ok[2] = { 0, 0 };
    for (int k = 0; k < 2; k++) {
        uint64_t h =
            k == 0 ? SID_PLAYER : (g_mdu.slot >=
                                   0 ? PuppetHandle((uint32_t) g_mdu.
                                                     slot) : 0);
        typeof(&g_dtl.sd[0]) d = &g_dtl.sd[k];
        if (!h || (k == 1 && g_mdu.slot < 0)) {
            d->ok = 0;
            continue;
        }
        float b[16];
        const float *l = PsyncLoc(h, b);
        if (!l || !PsLocOk(l)) {
            d->ok = 0;
            continue;
        }
        memcpy(lp[k], l, 12);
        lp_ok[k] = 1;
        // the clip's own root motion this frame: psync's placement of that body's half
        int half =
            !g_ps.role ? -1 : g_ps.ent_att == h ? 0 : g_ps.ent_vic ==
            h ? 1 : -1;
        float pl[3] = { 0 };
        int pl_ok = half >= 0 && g_ps.place_ok[half];
        if (pl_ok)
            memcpy(pl, g_ps.place[half], 12);
        float yaw = Uc4Yaw(l), plyaw =
            pl_ok ? Uc4Yaw(g_ps.place[half]) : 0.0f;
        typeof(&g_dtb.yd[0]) yd = &g_dtb.yd[k];
        if (d->ok && d->h == h) {
            float dt = (float) ((t - d->t) / 1000.0);
            if (dt <= 0.0f || dt > 0.1f)
                dt = 1.0f / 60.0f;
            float m[3] =
                { l[0] - d->p[0], l[1] - d->p[1], l[2] - d->p[2] }, ex[3];
            const char *src;
            // (v5-body, A/B A: 77 'SNAP slot 0 1.18 m ... clip root motion explains 1.05 m; us, attacker half' on combo step 2 - our body moved 0.16 m;
            //  last frame's pl was the PREVIOUS pair's placement (another ap), so its delta was no motion at all: a placement only explains within one pair)
            int same_pair = pl_ok && d->pl_ok && d->pl_uid == g_ps.uid;
            if (same_pair) {
                for (int i = 0; i < 3; i++)
                    ex[i] = pl[i] - d->pl[i];
                src = "clip root motion";
            } else {
                for (int i = 0; i < 3; i++)
                    ex[i] = d->v[i] * dt;
                src = "velocity";
            }
            float rx = m[0] - ex[0], rz = m[2] - ex[2], r =
                __builtin_sqrtf(rx * rx + rz * rz), ml =
                __builtin_sqrtf(m[0] * m[0] + m[2] * m[2]);
            char wb[200];
            wb[0] = 0;
            if (r > DTL_SNAP_M && ml < DTL_TELEPORT) {
                g_dtl.snaps[k]++;
                DTLOG
                    ("dtl: SNAP slot %u %.2f m in 1 frame (moved %.2f m, %s explains %.2f m; %s, psync role %d%s)%s%s",
                     k == 0 ? self : (uint32_t) g_mdu.slot, (double) r,
                     (double) ml, src,
                     (double) __builtin_sqrtf(ex[0] * ex[0] +
                                              ex[2] * ex[2]),
                     k == 0 ? "us" : "the partner's puppet", g_ps.role,
                     half >=
                     0 ? (half ? ", victim half" : ", attacker half") : "",
                     k == 0 && body ? " - writers:" : "", k == 0
                     && body ? DtlOwnWriter(wb, sizeof wb) : "");
            }
            if (ml < DTL_TELEPORT && r > g_dtb.step_max[k])
                g_dtb.step_max[k] = r;
            // yaw (v5-body): the frame's turn vs what the clip placement (psync) or last frame's turn rate explains
            if (yd->ok) {
                float dy = Uc4Wrap(yaw - yd->yaw), ey =
                    same_pair ? Uc4Wrap(plyaw - yd->plyaw) : yd->yv * dt,
                    ry = __builtin_fabsf(Uc4Wrap(dy - ey));
                if (__builtin_fabsf(dy) > g_dtb.yaw_max[k])
                    g_dtb.yaw_max[k] = __builtin_fabsf(dy);
                if (ry > g_dtb.yaw_unexpl_max[k])
                    g_dtb.yaw_unexpl_max[k] = ry;
                // ps39 B4: a turn that follows the pair's own placement was 'explained' and never printed (B [21426]: 110 deg at a pair start, no line). Any turn of a
                // body over 15 deg in one frame inside a pair is named.
                if (!(ry > DTL_YAW_SNAP) && body && same_pair
                    && __builtin_fabsf(dy) > 15.0f) {
                    static int nlt;
                    if (LOGCAP(nlt, 200))
                        DTLOG
                            ("dtl: FAST TURN slot %u turned %.0f deg in 1 frame inside a pair (its placement explains %.0f; %s, psync role %d; over 15 deg / frame; ps39)",
                             k == 0 ? self : (uint32_t) g_mdu.slot,
                             (double) dy, (double) ey,
                             k == 0 ? "us" : "the partner's puppet",
                             g_ps.role);
                }
                if (ry > DTL_YAW_SNAP && body) {
                    g_dtb.yaw_snaps[k]++;
                    DTLOG
                        ("dtl: YAW SNAP slot %u turned %.0f deg in 1 frame (%s explains %.0f; %s, psync role %d%s)%s%s",
                         k == 0 ? self : (uint32_t) g_mdu.slot,
                         (double) dy,
                         same_pair ? "clip placement" : "turn rate",
                         (double) ey,
                         k == 0 ? "us" : "the partner's puppet", g_ps.role,
                         half >=
                         0 ? (half ? ", victim half" : ", attacker half") :
                         "", k == 0 ? " - writers:" : "",
                         k ==
                         0 ? (wb[0] ? wb : DtlOwnWriter(wb, sizeof wb)) :
                         "");
                }
                float yv = dy / dt;
                yd->yv = __builtin_fabsf(yv) < 1440.0f ? yv : 0.0f;
            } else
                yd->yv = 0.0f;
            // (v5-body: after a SNAP the next frame's 'velocity' was the snap itself - 'moved 0.05 m, velocity explains 0.49 m' echoes in the A/B log: the
            //  unexplained part never feeds the velocity)
            // (c1657, A/B B 14422-15380 / 21000-22190 / 25300-25400: hundreds of 'SNAP slot 1 0.27 m (moved 0.00 m, velocity explains 0.27 m)'
            //  on a body standing still - a velocity-explained SNAP kept ex as the velocity, so the stale speed predicted itself every frame and never
            //  decayed. A snap the velocity could not explain resets the velocity: the next frame is judged from rest, not from the stale guess.
            //  t2r-dtl-snap-latch.txt = the old latch)
            int vsnap_reset = r > DTL_SNAP_M && !same_pair
                && !g_dtlSnapLatch;
            for (int i = 0; i < 3; i++) {
                float v =
                    vsnap_reset ? 0.0f : (r >
                                          DTL_SNAP_M ? ex[i] : m[i]) / dt;
                d->v[i] = ml < DTL_TELEPORT ? v : 0.0f;
            }
        } else {
            d->v[0] = d->v[1] = d->v[2] = 0.0f;
            yd->yv = 0.0f;
        }
        yd->yaw = yaw;
        yd->plyaw = plyaw;
        yd->ok = 1;
        memcpy(d->p, l, 12);
        memcpy(d->pl, pl, 12);
        d->pl_ok = pl_ok;
        d->pl_uid = g_ps.uid;
        d->t = t;
        d->h = h;
        d->ok = 1;
    }
    for (int k = 0; k < 2; k++)
        if (!g_dtl.sd[k].ok)
            g_dtb.yd[k].ok = 0;
    // ps40 D10 (rec39a: ~20 one-frame turns of our own body at a -hit pair's PLAY per duel on a still camera - A 7.33, 8.27, 8.97 ... sheets rev39a s007-s011 - and
    // 4 'YAW SNAP' lines, 1 'FAST TURN' in the whole run): the detector above reads the object's locator after our own writers ran, the turn that is drawn is
    // the engine's (its anim delta lands after our tick). This one reads the DRAWN skeleton: the hip line (l_upper_leg -> r_upper_leg) and the pelvis, frame to
    // frame, next to the root's own turn / step - every one-frame turn over 15 deg or pelvis step over 0.15 m of us or the duel partner is one line.
    // t2r-dtl-body-turn-ps39.txt = off.
    {
        static struct {
            uint64_t h, f;
            float hy, ry, pv[3], rt[3];
        } bt[2];
        static int boff = -1;
        static uint64_t bchk;
        uint64_t bn = GetTickCount64();
        if (boff < 0 || bn - bchk > 2000) {
            bchk = bn;
            boff =
                GetFileAttributesA("t2r-dtl-body-turn-ps39.txt") !=
                INVALID_FILE_ATTRIBUTES;
        }
        for (int k = 0; k < 2 && !boff; k++) {
            uint64_t h =
                k == 0 ? SID_PLAYER : (g_mdu.slot >=
                                       0 ? PuppetHandle((uint32_t) g_mdu.
                                                         slot) : 0);
            float jl[3], jr[3], jp[3], b[16];
            const float *l = h ? PsyncLoc(h, b) : NULL;
            if (!h || !l || !PsLocOk(l) || (!g_ps.role && g_mdu.slot < 0)
                || !GxJointPos(h, "l_upper_leg", jl)
                || !GxJointPos(h, "r_upper_leg", jr)
                || !GxJointPos(h, "pelvis", jp)) {
                bt[k].f = 0;
                continue;
            }
            float hy =
                __builtin_atan2f(jr[0] - jl[0], jr[2] - jl[2]) * 57.29578f,
                ry = Uc4Yaw(l);
            if (bt[k].f && g_frame - bt[k].f == 1 && bt[k].h == h) {
                float dh = Uc4Wrap(hy - bt[k].hy), dr =
                    Uc4Wrap(ry - bt[k].ry);
                float ps =
                    __builtin_sqrtf((jp[0] - bt[k].pv[0]) * (jp[0] -
                                                             bt[k].pv[0]) +
                                    (jp[2] - bt[k].pv[2]) * (jp[2] -
                                                             bt[k].pv[2]));
                float rs =
                    __builtin_sqrtf((l[0] - bt[k].rt[0]) * (l[0] -
                                                            bt[k].rt[0]) +
                                    (l[2] - bt[k].rt[2]) * (l[2] -
                                                            bt[k].rt[2]));
                if ((__builtin_fabsf(dh) > 15.0f || ps > 0.15f)
                    && ps < DTL_TELEPORT) {
                    int half =
                        !g_ps.role ? -1 : g_ps.ent_att ==
                        h ? 0 : g_ps.ent_vic == h ? 1 : -1;
                    char wb[200];
                    wb[0] = 0;
                    static int nbt;
                    if (LOGCAP(nbt, 600))
                        DTLOG
                            ("dtl: BODY TURN slot %u (%s) drawn hips %+.0f deg, pelvis %.2f m in 1 frame | root %+.0f deg, %.2f m (%s) | psync u%08x role %d%s t=%.0f ms, own drive %d | duel swing %d%s%s (ps40)",
                             k == 0 ? self : (uint32_t) g_mdu.slot,
                             k == 0 ? "us" : "the partner's puppet",
                             (double) dh, (double) ps, (double) dr,
                             (double) rs,
                             __builtin_fabsf(Uc4Wrap(dh - dr)) <= 10.0f
                             && ps - rs <
                             0.10f ? "the root did it" :
                             "the POSE moved on the root: a clip switch / the engine's align after our write",
                             g_ps.uid, g_ps.role,
                             half <
                             0 ? "" : half ? " victim half" :
                             " attacker half",
                             g_ps.role ? t - g_ps.t0 : 0.0, g_ps.own_drv,
                             g_mdu.sw_t0 != 0, k == 0 ? " - writers:" : "",
                             k == 0 ? DtlOwnWriter(wb, sizeof wb) : "");
                }
            }
            // ps41 (bis40 r1 B [18580]-[18600]: our own body's drawn turn into the knife recoil, ~100 deg of shoulder yaw inside 0.1 s, gave no line - the rule above
            // wants 15 deg of HIP turn in ONE frame; that turn ran through a 0.08 s blend and is larger in the shoulders): windowed - hips OR shoulders more than
            // 40 deg within 4 consecutive frames, with what the body was in. One line per 8 frames and body.
            {
                static struct {
                    uint64_t h, f, lf;
                    float hy[4], sy[4];
                    int n;
                } w4[2];
                float sl[3], sr[3];
                int sok = GxJointPos(h, "l_shoulder", sl)
                    && GxJointPos(h, "r_shoulder", sr);
                float sy =
                    sok ? __builtin_atan2f(sr[0] - sl[0],
                                           sr[2] - sl[2]) * 57.29578f : hy;
                if (w4[k].h != h || g_frame - w4[k].f != 1)
                    w4[k].n = 0;
                if (w4[k].n == 4) {
                    float dh4 = Uc4Wrap(hy - w4[k].hy[0]), ds4 =
                        Uc4Wrap(sy - w4[k].sy[0]);
                    if ((__builtin_fabsf(dh4) > 40.0f
                         || __builtin_fabsf(ds4) > 40.0f)
                        && g_frame - w4[k].lf >= 8) {
                        w4[k].lf = g_frame;
                        int half4 =
                            !g_ps.role ? -1 : g_ps.ent_att ==
                            h ? 0 : g_ps.ent_vic == h ? 1 : -1;
                        uint64_t nt = GetTickCount64();
                        static int nw4;
                        if (LOGCAP(nw4, 600))
                            DTLOG
                                ("dtl: BODY TURN 4f slot %u (%s) drawn hips %+.0f deg, shoulders %+.0f deg within 4 frames (root %+.0f deg this frame) | psync u%08x role %d%s, pair started %.0f ms ago, attacker entry blend %.3f s | %s%s%s (ps41)",
                                 k == 0 ? self : (uint32_t) g_mdu.slot,
                                 k == 0 ? "us" : "the partner's puppet",
                                 (double) dh4, (double) ds4,
                                 (double) Uc4Wrap(ry - bt[k].ry),
                                 g_ps.uid, g_ps.role,
                                 half4 <
                                 0 ? "" : half4 ? " victim half" :
                                 " attacker half",
                                 g_ps.role ? PsyncMs() -
                                 g_ps.play_ms : -1.0,
                                 g_psAttBlUid ==
                                 g_ps.
                                 uid ? (double) g_ps_att_bl_applied : -1.0,
                                 k == 0 && g_mduSwCutMs
                                 && nt - g_mduSwCutMs <
                                 300 ?
                                 "our swing was cut under 300 ms ago; " :
                                 "", k == 0
                                 && MdgOwnClipOn()?
                                 "our dodge clip runs; " : k == 1
                                 && g_dtl.run.src
                                 && g_dtl.run.slot ==
                                 (uint32_t) g_mdu.
                                 slot ?
                                 "a timeline clip (swing replica / dodge / reaction) runs on the puppet; "
                                 : "", g_mdu.sw_on && !g_mdu.sw_done
                                 && k == 0 ? "our swing runs" : "");
                    }
                    for (int i = 0; i < 3; i++) {
                        w4[k].hy[i] = w4[k].hy[i + 1];
                        w4[k].sy[i] = w4[k].sy[i + 1];
                    }
                    w4[k].n = 3;
                }
                w4[k].hy[w4[k].n] = hy;
                w4[k].sy[w4[k].n] = sy;
                w4[k].n++;
                w4[k].h = h;
                w4[k].f = g_frame;
            }
            bt[k].h = h;
            bt[k].f = g_frame;
            bt[k].hy = hy;
            bt[k].ry = ry;
            memcpy(bt[k].pv, jp, 12);
            memcpy(bt[k].rt, l, 12);
        }
    }
    if (!body)
        return;
    int s = g_mdu.slot;
    DtlGapTick(s);
    // min body distance outside pairs (psync not placing the partner): what the SPACING rule holds
    if (s >= 0 && lp_ok[0] && lp_ok[1] && !g_ps_pup[s].on
        && !g_peerDowned[s] && !g_puppetDeadHere[s] && LocalHp() > 0.0f
        && !g_downed) {
        float dx = lp[1][0] - lp[0][0], dz = lp[1][2] - lp[0][2], dd =
            __builtin_sqrtf(dx * dx + dz * dz);
        if (g_dtb.sep_n == 0 || dd < g_dtb.sep_min)
            g_dtb.sep_min = dd;
        g_dtb.sep_n++;
    }
}

// the 30 s body line (dtl_tick)
static void DtlBodySummary(int log)
{
    if (log && g_dtb.on)
        Xlog("dtl: body 30 s - max unexplained step us %.2f / partner %.2f m/frame, max yaw us %.0f / partner %.0f deg/frame (unexplained %.0f / %.0f, yaw snaps %u / %u), " "min body distance outside pairs %s%.2f m over %u frames (push-outs %u, %u frames), partner pose gaps: real freeze max %u frames (%u gaps >= 3), psync-owned max %u frames, " "releases to the spring %u, start pulls lengthened %u", (double) g_dtb.step_max[0], (double) g_dtb.step_max[1], (double) g_dtb.yaw_max[0], (double) g_dtb.yaw_max[1], (double) g_dtb.yaw_unexpl_max[0], (double) g_dtb.yaw_unexpl_max[1], g_dtb.yaw_snaps[0], g_dtb.yaw_snaps[1], g_dtb.sep_n ? "" : "(none) ", (double) (g_dtb.sep_n ? g_dtb.sep_min : 0.0f), g_dtb.sep_n, g_dtb.sep_pushes, g_dtb.sep_frames, g_dtb.gap_max, g_dtb.gap_n, g_dtb.own_max, g_dtb.hb_takes, g_dtb.pull_ext);
    g_dtb.step_max[0] = g_dtb.step_max[1] = g_dtb.yaw_max[0] =
        g_dtb.yaw_max[1] = g_dtb.yaw_unexpl_max[0] =
        g_dtb.yaw_unexpl_max[1] = 0.0f;
    g_dtb.yaw_snaps[0] = g_dtb.yaw_snaps[1] = 0;
    g_dtb.sep_n = 0;
    g_dtb.sep_min = 0.0f;
    g_dtb.sep_pushes = g_dtb.sep_frames = 0;
    g_dtb.gap_max = g_dtb.gap_n = g_dtb.own_max = 0;
    g_dtb.hb_takes = g_dtb.pull_ext = 0;
}

static void DtlTick(void)
{
    if (!DtlOn())
        return;
    uint64_t now = GetTickCount64();
    if (!g_dtl.sum_at)
        g_dtl.sum_at = now;
    if (g_mdu.slot >= 0)
        g_dtl.had_duel = 1;
    DtlRrTick(now);
    DtlQueueTick(now);
    if (g_dtl.run.src && now >= g_dtl.run.end + 400)
        g_dtl.run.src = 0;
    if (g_dtl.pr.on && now > g_dtl.pr.contact + 600) {
        g_dtl.pr.on = 0;
        g_dtl.conf_same++;
        DTLOG
            ("dtl: predicted swing on slot %u confirmed as a miss / plain blow (no pair) - it played to its end",
             g_dtl.pr.slot);
    }
    if (MatchSubstate() == 6)
        DtlSnapTick(PsyncMs());
    if (now - g_dtl.sum_at >= DTL_SUM_MS) {
        if (g_dtl.had_duel || g_dtl.snaps[0] || g_dtl.snaps[1]
            || g_dtl.cuts || g_dtl.trims)
            Xlog("dtl: 30 s summary - snaps %u (us %u, partner %u), cuts %u, trims %u, queued %u, dropped stale %u, crossfades %u, catch-ups %u, predicted %u / confirmed-same %u / corrected %u, " "avg start lateness %.0f ms over %u remote clips, trades won %u / lost %u, reach saves %u, dodge windows shortened %u, camera target jumps absorbed %ld", g_dtl.snaps[0] + g_dtl.snaps[1], g_dtl.snaps[0], g_dtl.snaps[1], g_dtl.cuts, g_dtl.trims, g_dtl.queued, g_dtl.dropped, g_dtl.xfades, g_dtl.catchups, g_dtl.predicted, g_dtl.conf_same, g_dtl.corrected, g_dtl.late_n ? g_dtl.late_sum / g_dtl.late_n : 0.0, g_dtl.late_n, g_dtl.trades_won, g_dtl.trades_lost, g_dtl.reach_saves, g_dtl.win_trims, (long) InterlockedExchange(&g_dtlCamJumps, 0));
        {
            long sc = InterlockedExchange(&g_spmCollapse, 0);
            if (sc)
                Xlog("spmelee cam: the engine lens collapsed within 0.3 m of us on %ld render frame(s) in 30 s - the framing kept its last direction (v5-cam4)", sc);
        }
        {
            long sf = InterlockedExchange(&g_spmSgnFlips, 0);
            if (sf)
                Xlog("spmelee cam: the view looked away from us on %ld test(s) in 30 s - the forward sign is a constant now (11-02), the look-at gated itself out", sf);
        }
        if (g_dtl.had_duel || g_dtb.step_max[0] > DTL_SNAP_M
            || g_dtb.step_max[1] > DTL_SNAP_M || g_dtb.yaw_snaps[0]
            || g_dtb.yaw_snaps[1])
            DtlBodySummary(1);
        else
            DtlBodySummary(0);
        g_dtl.snaps[0] = g_dtl.snaps[1] = 0;
        g_dtl.cuts = g_dtl.trims = g_dtl.queued = g_dtl.dropped =
            g_dtl.xfades = g_dtl.catchups = g_dtl.predicted =
            g_dtl.conf_same = g_dtl.corrected = 0;
        g_dtl.late_n = 0;
        g_dtl.late_sum = 0.0;
        g_dtl.trades_won = g_dtl.trades_lost = g_dtl.reach_saves =
            g_dtl.win_trims = 0;
        g_dtl.had_duel = 0;
        g_dtl.sum_at = now;
    }
}

// ps44 T5 (rec43a A [15325]-[15332], 114.05 s: a plain machete blow killed slot 0 standing; [15325] 'dtl: swing replica clip 74e499f169c823ef on slot 0's puppet
// ... PREDICTED finisher windup machete-slash-2-finisher (SP intro ellie-machete-kill-slash-2-neck-chop-intro--ellie)', [15326] 'slot 0's finisher windup replica on its
// puppet stopped' (= a 0.20 s blend-out), [15328] 'died here', then 'fling: slot 0 dead puppet moved 33.06 / 24.78 / 16.54 / 9.18 / 3.51 m off its death spot - put
// back' = a pull toward one far spot whose weight falls to 0 over ~5 frames; [15340] 'winmm's root write skipped 12 time(s) ... the engine's own callback moved the body
// up to 0.00 m'; the locator hook's dead-puppet skip never logged = no net locator for the dead puppet). The fading weight matches the 0.20 s blend-out of that intro
// replica - an INTRO of a paired move, authored around an apReference, played on a puppet with none set: while the puppet lived the net locator write placed it each
// frame; dead, nothing does, and the blending-out clip's align pulls it (INFERRED from the timing, NOT measured: the line below says what was running). A puppet that
// dies here has every replica the duel timeline started on it in the last 2 s stopped at once (no blend), so no clip align moves the corpse before the ragdoll.
// t2r-dead-replica-cut-old.txt = old.
static void DtlDeadCut44(uint32_t slot, uint64_t h)
{
    uint64_t now = GetTickCount64();
    static int old = -1;
    static uint64_t at;
    if (old < 0 || now - at > 2000) {
        at = now;
        old =
            GetFileAttributesA("t2r-dead-replica-cut-old.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    int had = g_dtl.run.src && g_dtl.run.slot == slot && g_dtl.run.ent == h
        && g_dtl.run.sid && now - g_dtl.run.t0 < 2000;
    // (only a windup still running: an intro whose windup ended - its START took the puppet / its end passed - is cleared, never stopped by a later death)
    if (slot < T2R_MAX_PEERS && g_mduFwRsid[slot]
        && now >= g_mdu_fw_rend[slot]) {
        g_mduFwRsid[slot] = 0;
        g_mdu_fw_rend[slot] = 0;
    }
    uint64_t fsid = slot < T2R_MAX_PEERS ? g_mduFwRsid[slot] : 0;
    Xlog("fling: slot %u died here - duel replica on its puppet: %s clip %llx started %lld ms ago (ends %+lld ms), finisher-intro replica %llx - %s (ps44 T5; t2r-dead-replica-cut-old.txt = old)", slot, had ? DTL_SN[g_dtl.run.src] : "none", had ? (unsigned long long) g_dtl.run.sid : 0ULL, had ? (long long) (now - g_dtl.run.t0) : -1LL, had ? (long long) ((int64_t) g_dtl.run.end - (int64_t) now) : 0LL, (unsigned long long) fsid, old ? "old: left to its blend-out" : had || fsid ? "stopped at once (no clip align on the corpse)" : "nothing to stop");
    if (old)
        return;
    float bl = 0.0f;
    if (had) {
        uint64_t a[5] = { h, ANIM_LAYER, 0, g_dtl.run.sid, 0 };
        memcpy(&a[2], &bl, 4);
        DcCall(FNV64("stop-animating"), 5, a);
        g_dtl.run.end = now;
        g_dtl.run.cancel_at = now;
    }
    if (fsid && (!had || fsid != g_dtl.run.sid)) {
        uint64_t a[5] = { h, ANIM_LAYER, 0, fsid, 0 };
        memcpy(&a[2], &bl, 4);
        DcCall(FNV64("stop-animating"), 5, a);
    }
    if (slot < T2R_MAX_PEERS) {
        g_mduFwRsid[slot] = 0;
        g_mdu_fw_rend[slot] = 0;
    }
}
