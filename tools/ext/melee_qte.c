// melee_qte.c - the melee exchange as a two-sided, turn-based QTE (spec re/melee-qte-1027.md, "Implementation 10-27"; goals: a turn-based QTE for both sides where the loser is penalized by getting punched, dodging still available on a cooldown, a combo prompt that does something, melee cameras so sequences feel different from regular gunplay, and no punches that silently do not play).
// Included after melee_seq.c in t2r-ext.c (drives g_mex / g_msq). Log prefix "mqte:". Opt-out for ALL of it: t2r-no-melee-qte.txt next to the exe (re-read every 2 s) = the earlier
// behaviour (600 ms combo window, trade on a stray blow, engine camera, pace lock). Camera only off: t2r-no-mqte-cam.txt. Windows: t2r-mseq-tune.txt
// "<fist hand-off ms> <weapon hand-off ms> <attacker window ms> <defender window ms>" (defaults ... 300 / 350; 100..1500).
//
// BEAT.  Every beat has an attacker (initiative) and a defender; both read the same shared clock H = the moment the beat OPENS (the end of the synced pair of the blow before, the end of
//   the dodge counter, or the lapse of the beat before; the pair's hand-off is on psync's move clock on both machines).
//   attacker  COMBO = melee button inside [H, H + AW] (AW 300 ms; a press buffered during the pair is released at H + 40 ms and counts as +40).  The prompt "Combo" (the melee glyph) shows
//             350 ms before H and the closing bar (the dodge cooldown bar's widget, t2rDodgeBar) runs H .. H + AW.
//   defender  DODGE (its own cooldown ready: the existing window / i-frames / prompt of melee_dodge.c, opens with the swing) or STRIKE = melee button inside [H, H + DW] (DW 350 ms)
//             BEFORE the attacker's START reaches it.  Prompt "Strike", the same bar.
//   RESOLVE   by the defender (it owns its body, as mex already does):
//             * attacker pressed, defender did nothing / pressed later (or too late)   -> defender LOSES: the blow lands, SP synced pair (melee_seq.c), initiative stays
//             * defender DODGE in its window                                            -> the dodge counter (dcounter.c), initiative flips when it is over
//             * defender STRIKE earlier than the attacker's COMBO (d < a; tie -> attacker) -> attacker LOSES: the defender answers the START with a FORCED START (flag bit 1): the old
//               attacker cancels its swing and becomes the defender of a beat without a dodge window (the blow lands = SP pair), the initiative flips
//             * attacker did not press inside AW                                          -> the defender's free beat: roles swap with no damage (MQ_FREE_MS window)
//             * 2 beats in a row with no input, 3 landed blows in a row (mex), a down, a teammate / third-party hit (mex), distance > 2.5 m  -> END, both released, 2.5 s no re-engage
//   The first beat (ENGAGE) has no attacker window (the press IS the engage, a = 0): a defender STRIKE cannot beat it, so only the dodge is offered there.
// NET.  MSG_MEX START grows from 10 to 13 bytes: [10] flags (bit 0 = sender runs the QTE, bit 1 = FORCED: the strike answer), [11..12] a = the attacker's press offset from H in ms.
//   ACK grows from 5 to 6: [5] bit 0 = the answerer runs the QTE.  Old builds read len >= 10 / 5 and ignore the rest; they never send the flags, so the QTE is simply off for that pair
//   (the exchange runs exactly as before).  Nothing else on the wire changes: the verdict, the pair and the counter are the existing messages.
#define MQ_FREE_MS 600          // the window of a free beat (roles swapped after a lapse / a counter)
#define MQ_END_DIST 2.5f        // m
#define MQ_IDLE_END 2           // beats in a row with no input
#define MQ_SYNTH_GRACE_MS 120   // the beat's own contact if the engine found none this long after the planned contact
enum { MQF_QTE = 1, MQF_FORCED = 2 };
static struct {
    int off, cam_off, cfg_logged, nlog, on, idle, strike, cam_on,
        audit_done, tx_flags, tx_a, inject, free_beat, synthing;
    uint64_t cfg_at, h_ms, strike_ms, flip_until, cam_retry, synth_ms,
        bar_t0, bar_end;
    int synth_slot, n_beats, n_strike_win, n_strike_lost, n_lapse, n_flip,
        n_synth;
} g_mqt = {.synth_slot = -1 };

#define MQLG(...) do { if (LOGCAP(g_mqt.nlog, 900)) Xlog(__VA_ARGS__); } while (0)
static void MqCfg(uint64_t now)
{
    if (g_mqt.cfg_at && now - g_mqt.cfg_at < 2000)
        return;
    g_mqt.cfg_at = now ? now : 1;
    FILE *f = ExeSideOpen("t2r-no-melee-qte.txt", "r");
    int o = f != NULL;
    if (f)
        fclose(f);
    // The melee exchange did not feel like SP: the execution camera orbited in front of us and cut twice in 1.5 s at the exchange
    // start. SP never cuts / orbits for a fist fight: the gameplay camera only re-frames (melee_spcam.c, which now runs through the exchange too). Off by
    // default; t2r-mqte-cam.txt = the old execution-camera framing.
    f = ExeSideOpen("t2r-mqte-cam.txt", "r");
    g_mqt.cam_off = f == NULL;
    if (f)
        fclose(f);
    if (o != g_mqt.off || !g_mqt.cfg_logged) {
        g_mqt.cfg_logged = 1;
        Xlog("mqte: %s",
             o ?
             "OFF (t2r-no-melee-qte.txt) - the exchange as before (600 ms combo window, trade on a stray blow, engine camera)"
             :
             "on (turn-based QTE: attacker COMBO / defender DODGE or STRIKE on the shared clock, loser takes the SP pair, initiative flips, melee camera, no pace lock inside an exchange)");
    }
    g_mqt.off = o;
}

static int MqActive(void)
{
    return g_mqt.on && g_mex.role;
}

static int MqPaceFree(void)
{
    return MqActive();
}

static int MqTxFlags(void)
{
    return g_mqt.tx_flags;
}

static int MqTxA(void)
{
    return g_mqt.tx_a;
}

static int MqCap(void)
{
    return g_mqt.on ? 1 : 0;
}

static int MqWiden(int slot)
{
    float ow = slot >= 0 ? PsyncOneway(slot) : 0.0f;
    int rtt = (int) (2.0f * ow);
    return rtt > 250 ? (rtt - 250) / 2 : 0;
}                               // spec: above 250 ms RTT widen by half the extra RTT

static int MqAw(int slot)
{
    return g_msq.qaw + MqWiden(slot);
}

static int MqDw(int slot)
{
    return g_msq.qdw + MqWiden(slot);
}

// ---- the bar (melee_dodge.c mdg_bar_draw asks): fraction of the running window left, alpha
static int MqteBar(float *frac, float *ga)
{
    if (!g_mqt.bar_end || !g_mqt.on)
        return 0;
    uint64_t now = GetTickCount64();
    if (now >= g_mqt.bar_end || now < g_mqt.bar_t0
        || g_mqt.bar_end <= g_mqt.bar_t0)
        return 0;
    uint64_t left = g_mqt.bar_end - now, len =
        g_mqt.bar_end - g_mqt.bar_t0, in = now - g_mqt.bar_t0;
    *frac = (float) left / (float) len;
    float a = left < 90 ? (float) left / 90.0f : 1.0f;
    if (in < 60) {
        float i = (float) in / 60.0f;
        if (i < a)
            a = i;
    }
    *ga = a;
    return 1;
}

// ---- the pair table audit (step 4): every weapon set x blow 1..3 must have a loaded synced pair, for both roles (either side can be the attacker)
static void MqAudit(void)
{
    if (g_mqt.audit_done)
        return;
    int ok[5][3], all = 1;
    for (int s = 0; s < 5; s++)
        for (int b = 0; b < 3; b++) {
            ok[s][b] = MsqRow(s - 1, b + 1) >= 0;
            if (!ok[s][b])
                all = 0;
        }
    g_mqt.audit_done = 1;
    Xlog("mqte: pair coverage (ms-* rows, loaded) fists %d%d%d | 1h blunt %d%d%d | 1h blade %d%d%d | 2h %d%d%d | knife %d%d%d - %s (the SP -hit pair starts at the contact frame; SP: swing 268-301 ms, contact +67..420 ms, -hit 730-770 ms, victim npc-*-hr starts on the same frame: re/sp-melee-measure-1026.md, sea-1026-human-melee-timeline.txt F=165207..165232)", ok[0][0], ok[0][1], ok[0][2], ok[1][0], ok[1][1], ok[1][2], ok[2][0], ok[2][1], ok[2][2], ok[3][0], ok[3][1], ok[3][2], ok[4][0], ok[4][1], ok[4][2], all ? "all covered" : "GAPS: a missing row = that blow plays no pair");
}

// ---- life of the QTE inside one exchange
static void MqBegin(int role, uint64_t now, int peer_flags)
{                               // peer_flags < 0: we start it (the answerer's ACK tells whether it runs the QTE)
    MqCfg(now);
    g_mqt.on = !g_mqt.off && (peer_flags < 0 || (peer_flags & MQF_QTE));
    g_mqt.idle = 0;
    g_mqt.strike = 0;
    g_mqt.inject = 0;
    g_mqt.h_ms = now;
    g_mqt.flip_until = 0;
    g_mqt.bar_end = 0;
    g_mqt.free_beat = 0;
    g_mqt.tx_flags = g_mqt.on ? MQF_QTE : 0;
    g_mqt.tx_a = 0;
    if (!g_mqt.on && !g_mqt.off)
        MQLG("mqte: exchange u%04x - the partner does not run the QTE (old build / opt-out): the exchange runs as before", g_mex.uid);
    if (g_mqt.on) {
        MqAudit();
        MQLG("mqte: exchange u%04x begins (we are the %s): windows attacker %d ms / defender %d ms (RTT widening %d ms)", g_mex.uid, role == 1 ? "attacker" : "defender", MqAw(g_mex.slot), MqDw(g_mex.slot), MqWiden(g_mex.slot));
    }
}

static void MqCamStop(const char *why)
{
    if (!g_mqt.cam_on)
        return;
    g_mqt.cam_on = 0;
    if (g_xcamOn && !strcmp(g_xcamName, "ctxdown-mqte")) {
        XcamStop(why);
        MQLG("mqte: melee camera off (%s) - SP hand-back", why);
    }
}

// the ACK of our START: does the answerer run the QTE? (an old build answers 5 bytes)
static void MqPeerAck(int cap)
{
    if (!g_mqt.on || cap)
        return;
    g_mqt.on = 0;
    g_mqt.tx_flags = 0;
    MQLG("mqte: u%04x the partner answered without the QTE flag (old build / opt-out) - this exchange runs as before", g_mex.uid);
}

// mex_reset: the exchange is over
static void MqEnd(const char *why)
{
    (void) why;
    if (g_mqt.on)
        MQLG("mqte: exchange u%04x over (%s): %d beats, %d strike wins, %d strike losses, %d lapses, %d flips, %d beat contacts of our own", g_mex.uid, why, g_mqt.n_beats, g_mqt.n_strike_win, g_mqt.n_strike_lost, g_mqt.n_lapse, g_mqt.n_flip, g_mqt.n_synth);
    MqCamStop(why);
    g_mqt.on = 0;
    g_mqt.bar_end = 0;
    g_mqt.strike = 0;
    g_mqt.inject = 0;
    g_mqt.flip_until = 0;
    g_mqt.free_beat = 0;
    g_mqt.n_beats = g_mqt.n_strike_win = g_mqt.n_strike_lost =
        g_mqt.n_lapse = g_mqt.n_flip = g_mqt.n_synth = 0;
}

// melee_seq.c msq_finish: a synced pair is over = the next beat opens now
static void MqPairEnd(uint64_t now)
{
    g_mqt.h_ms = now;
    g_mqt.strike = 0;
    g_mqt.idle = 0;
    g_mqt.free_beat = 0;
}

// melee_exchange.c mex_after_hit: a blow landed (no input lapse)
static void MqAfterHit(uint64_t now)
{
    g_mqt.h_ms = now;
    g_mqt.strike = 0;
    g_mqt.idle = 0;
    g_mqt.free_beat = 0;
    g_mqt.n_beats++;
}

// the attacker's COMBO press was accepted for the next blow: tx fields for the START
static void MqNotePress(uint64_t now)
{
    uint64_t a = now > g_mqt.h_ms ? now - g_mqt.h_ms : 0;
    if (a > 1500)
        a = 1500;
    g_mqt.tx_a = (int) a;
    g_mqt.tx_flags = g_mqt.on ? MQF_QTE : 0;
    g_mqt.idle = 0;
    g_mqt.strike = 0;
    g_mqt.free_beat = 0;
    MQLG("mqte: u%04x COMBO pressed %llu ms after the beat opened (window %d ms) -> blow %d START", g_mex.uid, (unsigned long long) a, MqAw(g_mex.slot), g_mex.blow);
}

// roles swap in place (no message: both machines see the same lapse / counter end). New attacker: COMBO beat, free window; new defender: waits for its START.
static void MqFlip(uint64_t now, const char *why)
{
    int was = g_mex.role;
    g_mex.role = was == 1 ? 2 : 1;
    g_mex.beat = MXB_COMBO;
    g_mex.blow = 0;
    g_mex.verdict = 0;
    g_mex.contact = 0;
    g_mex.miss_done = 0;
    g_mex.hit_got = 0;
    g_mex.burnt = 0;
    g_mex.ctr_armed = 0;
    g_mex.ctr_seen = 0;
    g_mex.fired = 0;
    g_mex.swing_seen = 0;
    g_mex.res_pending = g_mex.start_pending = 0;
    g_mex.hold.on = 0;
    g_mex.hard_dl = now + 9000;
    g_mex.lastp = 1;
    g_mex.acked = 1;
    g_mex.lat = MexLat((uint32_t) g_mex.slot);
    g_mex.vis_ms = MexVis((uint32_t) g_mex.slot);
    g_mex.combo_until = now + MQ_FREE_MS + (uint64_t) g_mex.lat * 2 + 300;
    g_mqt.h_ms = now;
    g_mqt.flip_until = now + 900;
    g_mqt.free_beat = 1;
    g_mqt.n_flip++;
    g_mqt.bar_end = 0;
    g_mqt.tx_a = 0;
    g_mqt.tx_flags = MQF_QTE;
    if (was == 2 && g_mqt.strike && now - g_mqt.strike_ms < 700)
        g_mqt.inject = 1;       // we pressed STRIKE and the attacker never answered: that press starts our beat at once
    g_mqt.strike = 0;
    MQLG("mqte: u%04x INITIATIVE FLIPS (%s): we are the %s now, free window %d ms%s", g_mex.uid, why, g_mex.role == 1 ? "attacker" : "defender", MQ_FREE_MS, g_mqt.inject ? " (our early STRIKE press starts the beat)" : "");
    PadinjBlock("melee-attack", 0);
}

// a lapse (nobody pressed inside the window): 2 in a row end the exchange, else the initiative flips. 1 = ended or flipped (the caller stops for this frame)
static int MqLapse(uint64_t now, const char *why)
{
    g_mqt.n_lapse++;
    g_mqt.idle++;
    MQLG("mqte: u%04x beat lapsed (%s) - %d beat%s with no input in a row",
         g_mex.uid, why, g_mqt.idle, g_mqt.idle == 1 ? "" : "s");
    if (g_mqt.idle >= MQ_IDLE_END) {
        MexReset("2 beats with no input", 1, 1);
        return 1;
    }
    MqFlip(now, why);
    return 1;                   // (the roles changed: the caller's frame is over)
}

// the attacker's COMBO beat, every frame (pedge = this frame's press edge already filtered by melee_seq's buffer). Returns 1 when the exchange ended
static int MqAtkBeat(uint64_t now, int slot, int *press)
{
    if (g_mqt.inject) {
        g_mqt.inject = 0;
        if (!*press) {
            *press = 1;
            MQLG("mqte: u%04x the early STRIKE press becomes our COMBO",
                 g_mex.uid);
        }
    }
    if (*press || MsqOn() || g_msq.req)
        return 0;               // a press is taken below; the window opens when the pair is over
    uint64_t end =
        g_mqt.h_ms +
        (g_mqt.free_beat ? (uint64_t) MQ_FREE_MS : (uint64_t) MqAw(slot));
    if (now > end)
        return MqLapse(now,
                        g_mqt.
                        free_beat ? "our free beat was not used" :
                        "the attacker did not press COMBO inside its window");
    return 0;
}

// the defender's COMBO beat (waiting for the attacker's START), every frame. Returns 1 when the exchange ended / the roles swapped (the caller returns)
static int MqDefBeat(uint64_t now, int slot)
{
    int pedge = MexPressEdge(now);
    int pair = MsqOn() || g_msq.req;
    if (pedge) {
        if (pair) {
            static int n;
            if (LOGCAP(n, 60))
                Xlog("mqte: u%04x melee press ignored - our hit reaction pair is still playing (the beat opens at its hand-off)", g_mex.uid);
        } else if (!g_mqt.strike) {
            g_mqt.strike = 1;
            g_mqt.strike_ms = now;
            MQLG("mqte: u%04x STRIKE pressed %llu ms after the beat opened (window %d ms)", g_mex.uid, (unsigned long long) (now - g_mqt.h_ms), MqDw(slot));
        } else {
            static int n;
            if (LOGCAP(n, 60))
                Xlog("mqte: u%04x second STRIKE press ignored (one per beat)", g_mex.uid);
        }
    }
    if (pair)
        return 0;
    if (g_mex.blow >= MX_MAX_BLOW) {
        if (now > g_mqt.h_ms + 1200) {
            MexReset("blow 3 landed, nothing follows", 1, 1);
            return 1;
        }
        return 0;
    }
    uint64_t wait = (uint64_t) MqDw(slot), alt =
        (uint64_t) (MqAw(slot) + (int) PsyncOneway(slot) + 80);
    if (alt > wait)
        wait = alt;             // the attacker's last legal press must have had time to reach us
    if (g_mqt.free_beat)
        wait = MQ_FREE_MS + (uint64_t) ((int) PsyncOneway(slot) + 80);
    // our STRIKE press in the attacker's window counted as "no input" - the 2nd lapse ended the exchange and ate it.
    // A STRIKE press is input: the beat flips to us and that press starts our blow (mq_flip inject).
    if (now > g_mqt.h_ms + wait) {
        if (g_mqt.strike)
            g_mqt.idle = 0;
        MqLapse(now, "the attacker's window closed with no START");
        return 1;
    }
    return 0;
}

// mex_recv op 1 on the defender: the attacker's START (a = its press offset from the beat's opening) against our STRIKE press. 1 = we win (the caller returns, we answer with a forced START)
static int MqStrikeEval(uint64_t now, int slot, int a_ms, int forced)
{
    if (!g_mqt.on || forced || !g_mqt.strike
        || g_mqt.strike_ms < g_mqt.h_ms)
        return 0;
    int d = (int) (g_mqt.strike_ms - g_mqt.h_ms);
    if (d < a_ms) {
        g_mqt.n_strike_win++;
        MQLG("mqte: u%04x STRIKE WINS: our press %d ms after the beat opened, the attacker's COMBO %d ms - it loses this beat (tie goes to the attacker)", g_mex.uid, d, a_ms);
        g_mex.role = 1;
        g_mex.beat = MXB_ENG;
        g_mex.slot = slot;
        g_mex.blow = 1;
        g_mex.wcls = MexWcls();
        g_mex.tc_ms = MexTcOf(g_mex.wcls);
        g_mex.lat = MexLat((uint32_t) slot);
        g_mex.vis_ms = MexVis((uint32_t) slot);
        g_mex.t0 = now;
        g_mex.acked = 0;
        g_mex.fired = 0;
        g_mex.verdict = 0;
        g_mex.hit_got = 0;
        g_mex.contact = 0;
        g_mex.miss_done = 0;
        g_mex.burnt = 0;
        g_mex.swing_seen = 0;
        g_mex.start_pending = 1;
        g_mex.start_at = now + 70;
        g_mex.hold.on = 0;
        g_mex.hard_dl = now + 9000;
        g_mex.lastp = 1;
        g_mex.res_pending = 0;
        g_mqt.strike = 0;
        g_mqt.idle = 0;
        g_mqt.h_ms = now;
        g_mqt.flip_until = now + 900;
        g_mqt.free_beat = 0;
        g_mqt.tx_flags = MQF_QTE | MQF_FORCED;
        g_mqt.tx_a = 0;
        MexTxStart(now);
        MQLG("mqte: u%04x forced START sent: the old attacker cancels its swing and takes the hit (no dodge window for it)", g_mex.uid);
        return 1;
    }
    g_mqt.n_strike_lost++;
    MQLG("mqte: u%04x STRIKE LOST: our press %d ms, the attacker's COMBO %d ms was first - it lands (dodge window follows)", g_mex.uid, d, a_ms);
    g_mqt.strike = 0;
    return 0;
}

// mex_recv op 1 on the attacker: a FORCED START (we lost the beat) or a START from a partner whose lapse came first (we yield). Prepares our body; the caller then runs the normal accept
static void MqYield(uint64_t now, int forced)
{
    if (forced) {
        uint64_t me0 = SID_PLAYER, r =
            DcCall(0x394888944e949d5fULL /* cancel-melee-action */ , 1,
                    &me0);
        MQLG("mqte: u%04x we LOST the beat (the partner's STRIKE was first): our swing cancelled (-> %llx), we take its blow", g_mex.uid, (unsigned long long) r);
    } else
        MQLG("mqte: u%04x the partner's START came inside our lapse margin - we yield the initiative", g_mex.uid);
    if (g_mex.hold.on)
        g_mex.hold.on = 0;
    PadinjBlock("melee-attack", 0);
    g_mqt.flip_until = now + 900;
    g_mqt.idle = 0;
    g_mqt.strike = 0;
}

// mex_recv op 1 acceptance in QTE: the cases the old state machine refused. 0 = not one of them, 1 = a forced start on our attacker, 2 = a yield / the next blow after a flip
static int MqStartOk(int slot, uint16_t uid, int blow, int flags)
{
    if (!g_mqt.on || !(flags & MQF_QTE) || !g_mex.role
        || g_mex.slot != slot || g_mex.uid != uid)
        return 0;
    if (g_mex.role == 1 && (flags & MQF_FORCED)
        && (g_mex.beat == MXB_ENG || g_mex.beat == MXB_ATK) && !MsqOn()
        && !g_dcm.on_att && !g_dcm.on_vic)
        return 1;
    if (g_mex.role == 1 && !(flags & MQF_FORCED)
        && (g_mex.beat == MXB_COMBO || g_mex.beat == MXB_CTR) && !MsqOn()
        && !g_msq.req)
        return 2;
    if (g_mex.role == 2 && g_mex.beat == MXB_COMBO
        && (blow != g_mex.blow || g_mex.blow == 0))
        return 2;               // (the same blow number = a repeat of a START we already took)
    return 0;
}

// a resent START of the attacker we just beat arrives after our forced START went out: not a refusal case (a refusal would end the exchange for it with a free swing)
static int MqIgnoreStart(int slot, uint16_t uid, int flags, uint64_t now)
{
    return g_mqt.on && g_mex.role == 1 && g_mex.slot == slot
        && g_mex.uid == uid && !(flags & MQF_FORCED)
        && now < g_mqt.flip_until && (g_mex.beat == MXB_ENG
                                      || g_mex.beat == MXB_ATK);
}

// ---- the beat's own contact (step 3): the engine finds no collider while psync holds the puppet ("melee: swing at standing slot 1 1.02 m in front - NO engine contact (... psync 1 ...)").
// Inside an exchange the contact is the beat's: the blow goes through the same hook an engine contact would (mex_contact judges it, the normal fx / send run).
static int MqSynthBlow(int slot, const char *why)
{
    uint8_t *peer = slot >= 0
        && slot < T2R_MAX_PEERS ? MdgPeer((uint32_t) slot) : NULL;
    if (!peer || !MexPeerOk((uint32_t) slot) || g_downed || g_downDying
        || MatchSubstate() != 6)
        return 0;
    uint64_t w = DcWeapon();
    float dmg = w == FNV64("abby-unarmed") ? FIST_DAMAGE : MeleeDmgNow();
    g_mqt.synth_ms = GetTickCount64();
    g_mqt.synth_slot = slot;
    g_mqt.n_synth++;
    MQLG("mqte: beat contact on slot %d (%s): weapon %llx dmg %.0f - the blow goes out as an engine contact would", slot, why, (unsigned long long) w, (double) dmg);
    g_mqt.synthing = 1;
    g_mdmgAuthored = 1;        // (v5-dmg: already the table's value)
    DirecthitHook(peer, w, 0, dmg, 0);
    g_mqt.synthing = 0;
    g_mdmgAuthored = 0;
    return 1;
}

// in a beat (attacker, ATK): the planned contact passed and the engine reported none
static void MqBeatContact(uint64_t now)
{
    if (g_mqt.off || g_mex.role != 1 || g_mex.beat != MXB_ATK
        || g_mex.contact || !g_mex.swing_seen || g_mex.verdict == 2
        || g_mex.hold.on)
        return;
    if (now < g_mex.t0 + (uint64_t) g_mex.tc_ms + MQ_SYNTH_GRACE_MS)
        return;
    {
        uint64_t ph = PuppetHandle((uint32_t) g_mex.slot);
        float mb[16], pb[16];
        const float *ml = Uc4MyLoc(mb), *pl =
            ph ? PuppetLocator(ph, pb) : NULL;
        if (!ml || !pl || !__builtin_isfinite(pl[0])
            || (pl[0] - ml[0]) * (pl[0] - ml[0]) + (pl[2] -
                                                    ml[2]) * (pl[2] -
                                                              ml[2]) >
            2.8f * 2.8f)
            return;
    }                           // the partner walked out of reach: a real miss    mq_synth_blow(g_mex.slot, "the engine reported no contact inside the exchange");
}

// outside an exchange: a swing at a standing enemy in reach whose collider psync holds (t2r-ext.c melee_nocontact_tick). 1 = the blow was delivered
static int MqSynthFree(int slot, float dist)
{
    if (g_mqt.off || g_mex.role || slot < 0 || slot >= T2R_MAX_PEERS
        || !g_msAtk || g_upgradeArmed > 0 || MdgBusy() || g_msq.on)
        return 0;
    if (!PsyncBusySlot((uint32_t) slot) || GxBusySlot((uint32_t) slot)
        || MdgSlotImmune((uint32_t) slot)
        || !MexPeerOk((uint32_t) slot))
        return 0;
    char why[96];
    snprintf(why, sizeof why,
             "swing %.2f m in front of a psync-held standing enemy, no engine contact",
             (double) dist);
    return MqSynthBlow(slot, why);
}

// hitsend_hook / directhit_hook: an engine contact on the slot we just delivered a beat contact to is a duplicate (the collider came back mid swing)
static int MqDupContact(uint32_t slot)
{
    return g_mqt.synth_slot == (int) slot && g_mqt.synth_ms
        && GetTickCount64() - g_mqt.synth_ms < 450 && !g_mex.releasing
        && !g_mqt.synthing;
}

// ---- the melee camera for both participants (the dodge counter's T1 framing, dcounter.c dc_cam: in 450 ms, out 400 ms), ENGAGE .. END, SP hand-back on END
static void MqCamTick(uint64_t now, int act)
{
    g_mqCam = g_xcamOn && !strcmp(g_xcamName, "ctxdown-mqte");
    if (!act || g_mqt.cam_off) {
        MqCamStop(act ? "camera switched off" : "exchange over");
        return;
    }
    int dc = g_dcm.on_att || g_dcm.on_vic || g_dcm.pend || g_dcCam;
    if (dc) {
        if (g_mqt.cam_on) {
            g_mqt.cam_on = 0;
            MQLG("mqte: melee camera hands over to the dodge counter's");
        }
        return;
    }
    if (g_mqt.cam_on) {
        if (!g_xcamOn || strcmp(g_xcamName, "ctxdown-mqte")) {
            g_mqt.cam_on = 0;
            g_mqt.cam_retry = now + 600;
            MQLG("mqte: melee camera was taken by another camera");
            return;
        }
        g_xcam_dur = (uint64_t) ((PsyncMs() - g_xcamMs0) * 0.06) + 180 + (uint64_t) g_xcam_bout_f;    // keeps it at full weight while the exchange runs
        return;
    }
    if (now < g_mqt.cam_retry || g_xcamOn || g_fcam_on || g_dc.st
        || g_xcam_w > 0.01f || g_downed || g_downDying
        || LocalHp() <= 0.0f || GxRunning() || g_execUntil)
        return;
    uint64_t h = PuppetHandle((uint32_t) g_mex.slot);
    if (!h || (g_mex.role == 1 && !g_mex.acked))
        return;                 // (the attacker waits for the answerer's ACK: a refusal = a free swing, no camera flick)
    if (!XcamStart(h, 8.0f, 0, -1, 30.0f, 1, "ctxdown-mqte")) {
        g_mqt.cam_retry = now + 3000;
        MQLG("mqte: no melee camera (exec camera off / no render hook) - retry in 3 s");
        return;
    }
    g_xcamBinF = 0.45f * 60.0f;
    g_xcam_bout_f = 0.4f * 60.0f;
    g_xcam_dur = S60(8.0f);
    g_mqt.cam_on = 1;
    g_mqCam = 1;
    MQLG("mqte: melee camera ON for the exchange (we are the %s; in 450 ms, out 400 ms; gun aim / ADS released, aim + fire commands disabled while it runs)", g_mex.role == 1 ? "attacker" : "defender");
}

// ---- per frame (first thing of mex_tick after msq_tick): camera, prompts, the bar, the distance end
static void MqTick(uint64_t now)
{
    MqCfg(now);
    int act = MqActive() && MatchSubstate() == 6;
    MqCamTick(now, act);
    g_mqt.bar_end = 0;
    if (!act)
        return;
    if (g_mex.role == 2 && g_mex.beat != MXB_COMBO)
        MexPressEdge(now);    // keeps the press-edge state fresh while the defender's own beats run (mq_def_beat reads it in the COMBO wait)
    int slot = g_mex.slot;
    if (slot < 0 || slot >= T2R_MAX_PEERS)
        return;
    uint64_t h = PuppetHandle((uint32_t) slot);
    // distance: > 2.5 m ends it for both (not while a pair places the bodies)
    if (!MsqOn() && !g_msq.req && !g_dcm.on_att && !g_dcm.on_vic && h) {
        float mb[16], pb[16];
        const float *ml = Uc4MyLoc(mb), *pl = PuppetLocator(h, pb);
        if (ml && pl && __builtin_isfinite(ml[0])
            && __builtin_isfinite(pl[0])) {
            float dx = pl[0] - ml[0], dz = pl[2] - ml[2];
            if (dx * dx + dz * dz > MQ_END_DIST * MQ_END_DIST) {
                MexReset("the bodies are more than 2.5 m apart", 1, 1);
                return;
            }
        }
    }
    if (g_mex.beat == MXB_COMBO && g_mex.blow < MX_MAX_BLOW) {  // (after blow 3 nothing follows: no prompt, no bar)
        int left = MsqOn()? MsqLeftMs() : (g_msq.req ? 600 : -1);    // ms until the beat opens (-1 = open)
        uint64_t open = left >= 0 ? now + (uint64_t) left : g_mqt.h_ms;
        uint64_t len =
            g_mex.role ==
            1 ? (uint64_t) MqAw(slot) : (uint64_t) MqDw(slot);
        if (g_mqt.free_beat && left < 0)
            len = MQ_FREE_MS;
        int showp = (left < 0 || left < 350) && !(g_mex.role == 2
                                                  && g_mqt.strike);
        if (showp && h) {
            g_wpqGlyph = "gen:attack";
            int ok =
                WorldPrompt(h, g_mex.role == 1 ? "Combo" : "Strike",
                             -1.0f);
            g_wpqGlyph = NULL;
            static uint64_t lg;
            if (now - lg > 1500) {
                lg = now;
                MQLG("mqte: u%04x %s prompt (shown %d), window opens in %d ms, runs %llu ms", g_mex.uid, g_mex.role == 1 ? "COMBO" : "STRIKE", ok, left > 0 ? left : 0, (unsigned long long) len);
            }
        }
        if (left < 0 && now >= open && now < open + len) {
            g_mqt.bar_t0 = open;
            g_mqt.bar_end = open + len;
        }
    } else if (g_mex.role == 2 && g_mex.beat == MXB_ATK && !g_mex.verdict
               && !g_mex.burnt && g_mex.win_hi > now
               && g_mex.t0 < g_mex.win_hi) {
        g_mqt.bar_t0 = g_mex.t0;
        g_mqt.bar_end = g_mex.win_hi;   // the dodge window's own closing bar
    }
}
