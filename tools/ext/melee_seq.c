// melee_seq.c - the MELEE SEQUENCE: every blow of a melee exchange that LANDS plays TLOU2's own synced-hit pair (attacker clip + victim "-hit" clip around one apReference, played by psync)
// instead of the free flinch (melee is more scripted and fair for both sides: the other person is put in a melee
// sequence if they get pushed and meleed, and the sequence is responsive, SP-like, and fair). Design, pair table, beat timing, net ops: re/melee-seq-1027.md.
// Included after melee_exchange.c in t2r-ext.c (it drives g_mex). Log prefix "mseq:". Opt-out: t2r-no-melee-seq.txt next to the exe (re-read every 2 s) = the exchange as before.
// Tuning: t2r-mseq-tune.txt "<fist hand-off ms> <weapon hand-off ms>" (default 650 / 750, 400..1500).
//
// It is NOT a second state machine for the fight: the exchange (melee_exchange.c) still owns the beats (ENGAGE -> ATTACK with the defender's dodge window -> HIT / DODGED -> COMBO window ->
// blow 2, 3 -> END, DODGED -> dcounter.c). The sequence only gives a HIT its authored look:
//  * TRIGGER  the first landed blow of an exchange (mex_after_hit: the defender's verdict was HIT and the contact went out through the normal hit path - damage, hit sound, marker are
//             all today's, at contact, no hold, no hit-stop). One frame later (so the hit message is on the wire first) the attacker's client asks for the pair of the held weapon set and
//             the blow number. Blow 2 and 3 do the same; blow 3 is the last (the exchange ends when its pair does).
//  * THE PAIR "<set>-combo-<N>-hit": TLOU2's connected hit of the SP combo. Its clips BEGIN AT THE CONTACT (re/scripted-melee-1026.md A3/A4: victim vox + blood events at 0.03 s), so it
//             starts exactly where the engine's swing touched: the attacker's follow-through and the victim's recoil are authored, both bodies are placed around one apReference (psync),
//             the attacker's own swing is cancelled at that moment (the pair's attacker half continues it). It adds NO damage (the blow already landed) and is cut at a short hand-off
//             (fists 0.65 s, weapons 0.75 s) with psync's blend-out on both bodies at the same instant.
//  * VICTIM AGENCY  there is none INSIDE a pair (both bodies are scripted, like SP's synced hit); the choice comes BEFORE each follow-up blow: the attacker's next press (buffered during the
//             pair, released at the hand-off - the blow's swing starts then) opens the exchange's normal dodge window (melee_dodge.c, evade glyph prompt, ~0.5 s from the swing start). A
//             press there is the DODGED verdict -> dcounter.c's dodge counter (the sequence flips into the existing counter pair). No press = the next blow lands = the next pair.
//  * THIRD PARTIES  any hit or shot from a third player on either body is mex_note_hit's "broken for both": mex_reset -> msq_break aborts the pair (ABORT to everyone) -> both blend out and
//             both get control back at once.
//  * FAIR     the pair adds no damage and no extra lock beyond its 0.65-0.75 s; a blow that would down the victim (hp - dmg <= 1) is not scripted at all - the normal Melee Down takes it;
//             after the sequence (END with a pair played) neither side can be pulled into another exchange with that partner for MSQ_COOL_MS (2.5 s); the victim refuses a pair it
//             is not owed (not in an exchange with that attacker, cooldown, down / dying, a pair already running, bad values).
// Net: no new message id. The pair is psync's START (kind PS_CTXDOWN, rows "ms-*" of ctxdown.c CD_PAIRS, kind 6); the victim accepts it through cd_victim_begin -> msq_victim_begin. The
// exchange's MSG_MEX ops are unchanged (START of blow n+1 after the hand-off, END, ...); a third-party break is mex_reset's END (op 4) + psync's ABORT.
#define MSQ_COOL_MS 2500
#define MSQ_FIT_MAX 1.20f       // m: our body to the pair's authored attacker spot after the pair stepped toward us (psync slides us the rest; 0 pulled it in at the start)
#define MSQ_VIC_STEP 0.45f      // max step-in of the hit player toward the attacker (the gap between the bodies at contact is the swing's own reach)
#define MSQ_REQ_FRAMES 14       // the path gate answers in 1-3 frames; the request is dropped (the exchange goes on without a pair) after this many
#define MSQ_BUF_GUARD_MS 40     // a buffered combo press is released this long after the hand-off (the partner's pair ends on the same move time, one-way later)
#define MSQ_BUF_MAX_MS 700      // a combo press made while the pair plays is replayed at the hand-off (pressed 499 ms before an early
                                  // contextual-down hand-off, dropped at 250 ms -> beat lapsed, initiative flipped, exchange ended on 2 eaten presses). Was 250 (log 60052).
static struct {
    int on, role, slot, row, blow, final_, req, req_blow, req_final, off,
        cfg_logged, nlog, hand[2], n_pairs, n_refused, n_broken, qaw, qdw;
    uint32_t uid;
    uint16_t used_uid;
    uint64_t t0_ms, end_ms, req_at, req_f, buf_at, over_at, cfg_at,
        cool_until[T2R_MAX_PEERS];
    float end_s, last_dmg;
} g_msq = {.hand = { 650, 750 },.qaw = 300,.qdw = 350 };

#define MQLOG(...) do { if (LOGCAP(g_msq.nlog, 600)) Xlog(__VA_ARGS__); } while (0)
static void MsqCfg(uint64_t now)
{
    if (g_msq.cfg_at && now - g_msq.cfg_at < 2000)
        return;
    g_msq.cfg_at = now ? now : 1;
    FILE *f = ExeSideOpen("t2r-no-melee-seq.txt", "r");
    int o = f != NULL;
    if (f)
        fclose(f);
    if (o != g_msq.off || !g_msq.cfg_logged) {
        g_msq.cfg_logged = 1;
        Xlog("mseq: %s",
             o ?
             "OFF (t2r-no-melee-seq.txt) - landed blows play the plain hit reaction"
             :
             "on (a landed blow of a melee exchange plays TLOU2's synced hit pair; the victim's dodge window comes before each follow-up blow; 2.5 s anti-chain cooldown)");
    }
    g_msq.off = o;
    f = ExeSideOpen("t2r-mseq-tune.txt", "r");
    if (f) {                    // "<fist hand-off> <weapon hand-off> [<QTE attacker window> <QTE defender window>]" (melee_qte.c; windows 100..1500 ms, defaults 300 / 350)
        int v[4] = { 0, 0, 0, 0 }, n =
            fscanf(f, "%d %d %d %d", &v[0], &v[1], &v[2], &v[3]);
        if (n >= 2 && v[0] >= 400 && v[0] <= 1500 && v[1] >= 400
            && v[1] <= 1500) {
            g_msq.hand[0] = v[0];
            g_msq.hand[1] = v[1];
        }
        if (n >= 3 && v[2] >= 100 && v[2] <= 1500)
            g_msq.qaw = v[2];
        if (n >= 4 && v[3] >= 100 && v[3] <= 1500)
            g_msq.qdw = v[3];
        fclose(f);
    }
}

static void MexNotePairEnd(int slot, int ms);
static void MqPairEnd(uint64_t now);  // melee_qte.c
static int MsqOn(void)
{
    return g_msq.on;
}

static int MsqLeftMs(void)
{
    return g_msq.on ? (g_msq.end_ms >
                       GetTickCount64()? (int) (g_msq.end_ms -
                                                GetTickCount64()) : 0) :
        -1;
}

// ctxdown.c / paired.c: this move id is a sequence pair (kind 6 row)
static int MsqMove(uint64_t move)
{
    for (size_t i = 0; i < CD_N; i++)
        if (FNV64(CD_PAIRS[i].name) == move)
            return CD_PAIRS[i].kind == 6;
    return 0;
}

static int MsqRowBlow(int row)
{
    size_t l = strlen(CD_PAIRS[row].name);
    return l && CD_PAIRS[row].name[l - 1] >= '1'
        && CD_PAIRS[row].name[l - 1] <=
        '3' ? CD_PAIRS[row].name[l - 1] - '0' : 0;
}

static int MsqRow(int style, int blow)
{
    for (size_t i = 0; i < CD_N; i++)
        if (CD_PAIRS[i].kind == 6 && MsqRowBlow((int) i) == blow
            && CdStyleOk(style, CD_PAIRS[i].style)
            && CdPairLoaded((int) i))
            return (int) i;
    return -1;
}

// the hand-off (clip seconds from the pair's start = the contact): fists / knife-less hands shorter than weapons
static float MsqEndS(int row)
{
    return (CD_PAIRS[row].style ==
            -1 ? g_msq.hand[0] : g_msq.hand[1]) / 1000.0f;
}

// ---- the tracked state
static void MsqFinish(const char *why)
{
    if (!g_msq.on)
        return;
    uint64_t now = GetTickCount64();
    MQLOG("mseq: u%04x blow %d pair %s over (%s) after %llu ms, role %s",
          g_msq.used_uid, g_msq.blow, CD_PAIRS[g_msq.row].name, why,
          (unsigned long long) (now - g_msq.t0_ms),
          g_msq.role == 1 ? "attacker" : "hit player");
    if (g_msq.role == 2 && g_meleeLockVictim)
        g_meleeLockFrom = 0;
    g_msq.on = 0;
    g_msq.over_at = now;
    MqPairEnd(now);           // melee_qte.c: the next beat opens now (both roles: the pair's hand-off is on the shared move clock)
    {
        static int nr = -1;
        static uint64_t nra;
        if (!MexOptout("t2r-no-mex-recover.txt", &nr, &nra))
            MexNotePairEnd(g_msq.slot, 500);
    }                           // the victim's recovery, no new ENGAGE for 500 ms
}

// mex_reset: the exchange ends / is broken: the pair goes with it
static void MsqBreak(const char *why, int normal_end)
{
    g_msq.req = 0;
    g_msq.buf_at = 0;
    if (!g_msq.on)
        return;
    if (normal_end && MsqLeftMs() < 400) {
        MQLOG
            ("mseq: u%04x exchange ended (%s) with %d ms of the pair left - it plays out",
             g_msq.used_uid, why, MsqLeftMs());
        return;
    }
    int mine = g_ps.role && g_ps.kind == PS_CTXDOWN
        && g_ps.uid == g_msq.uid;
    MQLOG
        ("mseq: u%04x BROKEN (%s) at %llu ms into the pair - both halves blend out now%s",
         g_msq.used_uid, why,
         (unsigned long long) (GetTickCount64() - g_msq.t0_ms),
         mine ? "" : " (psync was already gone)");
    g_msq.n_broken++;
    if (mine)
        PsyncAbort("melee sequence broken", 1);
    MsqFinish("broken");
}

// mex_reset, after the cooldown line: a sequence that played a pair makes the exchange's cooldown the long one (no immediate re-lock on the same partner)
static void MsqEnded(int slot, uint16_t uid, uint64_t now)
{
    if (slot < 0 || slot >= T2R_MAX_PEERS || g_msq.used_uid != uid || !uid)
        return;
    g_msq.cool_until[slot] = now + MSQ_COOL_MS;
    if (g_mex.pair_cool[slot] < g_msq.cool_until[slot])
        g_mex.pair_cool[slot] = g_msq.cool_until[slot];
    MQLOG
        ("mseq: u%04x END - slot %d cannot be locked into another exchange for %d ms (no chaining)",
         uid, slot, MSQ_COOL_MS);
}

static int MsqCooling(uint32_t slot, uint64_t now)
{
    return slot < T2R_MAX_PEERS && now < g_msq.cool_until[slot];
}

// mex_contact: the damage of the contact that is about to be judged (the downing-blow test)
static void MsqNoteContact(float dmg)
{
    g_msq.last_dmg = dmg;
}

// mex_after_hit: a blow of the exchange landed. final = blow 3 (the exchange ends with its pair). 1 = a pair was asked for (the exchange stays alive for it)
static int MsqTryStart(uint64_t now, int final_)
{
    MsqCfg(now);
    if (g_msq.off || g_mex.role != 1 || g_ps.role || g_msq.on) {
        MQLOG("mseq: u%04x blow %d landed - no pair (%s)", g_mex.uid,
              g_mex.blow,
              g_msq.off ? "switched off" : g_msq.
              on ? "a pair is running" : g_mex.role !=
              1 ? "not the attacker" : "another paired move runs");
        return 0;
    }
    // (every landed sequence blow is audible exactly once): the attacker's punch wave plays at the contact (mhr_atk_hit). Safety net: a fist blow that reached here with no punch
    // in the last 600 ms (victim-immune flag raced, wave pool failed) gets one now; the normal case plays nothing here (no double). The pair itself arms 0 sound events.
    if (CdCurStyle() == -1 && now - g_mhrAtkPunchMs > 600
        && !IsTeammate((uint32_t) g_mex.slot)
        && PuppetHandle((uint32_t) g_mex.slot)) {
        MhrPunchAt(PuppetHandle((uint32_t) g_mex.slot), 1,
                     "sequence safety net (no punch at the contact)");
        g_mhrAtkPunchMs = now;
        MQLOG
            ("mseq: u%04x blow %d had no punch at its contact - played one now",
             g_mex.uid, g_mex.blow);
    }
    g_msq.req = 1;
    g_msq.req_blow = g_mex.blow;
    g_msq.req_final = final_;
    g_msq.req_at = now;
    g_msq.req_f = g_frame;
    g_msq.buf_at = 0;
    MQLOG
        ("mseq: u%04x blow %d LANDED on slot %d (dmg %.0f) - asking for its synced pair next frame%s",
         g_mex.uid, g_mex.blow, g_mex.slot, (double) g_msq.last_dmg,
         final_ ? " (last blow: the exchange ends with it)" : "");
    return 1;
}

// ---- the attacker: plan + path gate + launch (dcounter.c's placement, the pair starts AT the contact so there is no start skip)
static int MsqLaunch(int slot, int row, const float *ap, float gap)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    uint8_t *peer = ((fn_peer) T2rPeer) ((uint32_t) slot);
    uint64_t h = peer ? *(uint64_t *) (peer + 0x60) : 0;
    float end_s = MsqEndS(row);
    uint64_t w = DcWeapon(), mel = w == FNV64("abby-unarmed") ? 0 : w;
    {
        uint64_t me0 = SID_PLAYER, r =
            DcCall(0x394888944e949d5fULL /* cancel-melee-action */ , 1,
                    &me0);
        MQLOG("mseq: u%04x our swing cancelled for %s (-> %llx)",
              g_mex.uid, CD_PAIRS[row].name, (unsigned long long) r);
    }
    g_psStartSkipMs = 0.0f;
    if (!PsyncStart
        (PS_CTXDOWN, (uint32_t) slot, CD_ATT0(row), CD_VIC0(row), ap,
         end_s, 0, 0xff, mel, FNV64(CD_PAIRS[row].name))) {
        MQLOG
            ("mseq: u%04x psync refused %s - the exchange goes on without a pair",
             g_mex.uid, CD_PAIRS[row].name);
        return 0;
    }
    g_cd.ended = 0;
    g_cd.plan_ok = 0;
    g_msq.on = 1;
    g_msq.role = 1;
    g_msq.slot = slot;
    g_msq.row = row;
    g_msq.blow = g_mex.blow;
    g_msq.uid = g_ps.uid;
    g_msq.used_uid = g_mex.uid;
    g_msq.end_s = end_s;
    g_msq.t0_ms = GetTickCount64();
    g_msq.end_ms = g_msq.t0_ms + (uint64_t) (end_s * 1000.0f);
    g_msq.n_pairs++;
    g_msq.buf_at = 0;
    g_msq.final_ = g_msq.req_final;
    CdHandsBegin(row);
    g_visVicAnim = CD_VIC0(row);
    g_vis_vic_puppet = h;
    g_vis_vic_frame = g_t60;
    g_mpose_att_frame = g_vis_vic_frame;
    g_mposeAttSlot = (int32_t) slot;
    g_finposVictim = (int32_t) slot;
    g_finposSlot = -1;
    g_finPlaceFrame = 0;
    g_execAttFrame = 0;
    FinposBegin(1, slot, 0, S60(end_s) + 60, "melee sequence, attacker");
    XfaceStart(SID_PLAYER, h, end_s + 0.6f);
    g_mex.combo_until = g_msq.end_ms + MX_COMBO_MS;
    MQLOG
        ("mseq: u%04x START blow %d %s on slot %d: %s on us, %s on them, from the contact, hand-off at %.2f s, our body %.2f m from the authored spot; then the combo window (%d ms)%s",
         g_mex.uid, g_msq.blow, CD_PAIRS[row].name, slot, CD_ATT0(row),
         CD_VIC0(row), (double) end_s, (double) gap, MX_COMBO_MS,
         g_msq.final_ ? "; last blow: END at the hand-off" : "");
    return 1;
}

// 0 = no pair (the exchange goes on), 1 = launched, 2 = ask again next frame
static int MsqAttempt(void)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    int slot = g_mex.slot;
    const char *no = NULL;
    uint8_t *peer = slot >= 0
        && slot <
        T2R_MAX_PEERS ? ((fn_peer) T2rPeer) ((uint32_t) slot) : NULL;
    uint64_t h = peer ? *(uint64_t *) (peer + 0x60) : 0;
    float vb[16], mb[16];
    const float *vl = h ? PuppetLocator(h, vb) : NULL, *me =
        PlayerLocator(mb);
    int style = CdCurStyle(), row = -1;
    if (MatchSubstate() != 6 || g_mex.role != 1
        || g_mex.beat != MXB_COMBO)
        no = "the exchange moved on";
    else if (g_ps.role || g_downed || g_downDying || g_downAt
             || LocalHp() <= 0.0f)
        no = "we are busy / down / in another paired move";
    else if (!h || g_peerDowned[slot] || g_puppetDeadHere[slot]
             || IsTeammate((uint32_t) slot))
        no = "the target is gone / down";
    else if (PsyncBusySlot((uint32_t) slot)
             || GxBusySlot((uint32_t) slot) || g_dcm.on_att
             || g_dcm.on_vic)
        no = "the target is in another paired move";
    else if (!vl || !me || !PsLocOk(vl) || !PsLocOk(me))
        no = "no locators";
    else if (style == -2)
        no = "held weapon unknown / upgraded (keeps its one-shot)";
    if (!no) {
        float vhp = PuppetHp((uint32_t) slot, h);
        if (vhp > 0.0f && g_msq.last_dmg > 0.0f
            && vhp - g_msq.last_dmg <= 1.0f)
            no = "the blow downs the target (hp - dmg <= 1): the normal Melee Down takes it";
    }
    if (!no) {
        row = MsqRow(style, g_msq.req_blow);
        if (row < 0)
            no = "no pair loaded for this weapon set / blow (style/blow)";
    }
    if (no) {
        MQLOG("mseq: u%04x blow %d - no pair: %s (style %d)", g_mex.uid,
              g_msq.req_blow, no, style);
        return 0;
    }
    // placement: the victim half where the victim stands, the pair turned so the authored attacker spot points at us (dcounter.c's solution), contact = clip start
    float ap[7], da = 0.0f;
    if (!DcAnchor(row, h, vl, me, 0.0f, ap, &da)) {
        MQLOG
            ("mseq: u%04x blow %d - %s: no anchor (bodies on top of each other / unreadable clips)",
             g_mex.uid, g_msq.req_blow, CD_PAIRS[row].name);
        return 0;
    }
    if (da > 0.5f) {            // a gap: the pair moves toward us by 40 % of it (the hit player steps into the blow, our slide shrinks)
        float gx = me[0] - g_dcSa[0], gz = me[2] - g_dcSa[2], gl =
            __builtin_sqrtf(gx * gx + gz * gz), st =
            fminf(MSQ_VIC_STEP, 0.4f * gl);
        if (gl > 0.01f) {
            ap[0] += gx / gl * st;
            ap[2] += gz / gl * st;
            da -= st;
            MQLOG
                ("mseq: u%04x gap to the authored attacker spot - the pair moves %.2f m toward us (slide left %.2f m)",
                 g_mex.uid, (double) st, (double) da);
        }
    }
    if (da > MSQ_FIT_MAX) {
        MQLOG
            ("mseq: u%04x blow %d - %s: our body is %.2f m from the authored attacker spot (max %.2f) - no pair",
             g_mex.uid, g_msq.req_blow, CD_PAIRS[row].name, (double) da,
             (double) MSQ_FIT_MAX);
        return 0;
    }
    {
        float vd = PsyncDur(h, FNV64(CD_VIC0(row)));
        g_pgEndPh = vd > 0.2f ? MsqEndS(row) / vd : 0.0f;
    }
    int g =
        PgGate((uint32_t) slot, CD_ATT0(row), CD_VIC0(row), ap, vl, me, 0,
                "melee sequence");
    g_pgEndPh = 0.0f;
    if (g == 2)
        return 2;
    if (g == 0) {
        MQLOG
            ("mseq: u%04x blow %d - %s: the path is blocked (path gate) - no pair",
             g_mex.uid, g_msq.req_blow, CD_PAIRS[row].name);
        return 0;
    }
    return MsqLaunch(slot, row, ap, da) ? 1 : 0;
}

// ---- the hit player: psync_begin_remote -> cd_victim_begin -> here. 1 = accepted
static int MsqVictimBegin(uint32_t slot, float end_s, uint64_t skipf,
                            int pr)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    uint64_t now = GetTickCount64();
    const char *no = NULL;
    MsqCfg(now);
    if (g_msq.off)
        no = "switched off";
    else if (slot > T2R_MAX_PEERS - 1 || MatchSubstate() != 6
             || !__builtin_isfinite(end_s) || end_s < 0.5f || end_s > 3.0f)
        no = "bad start";
    else if (g_mex.role != 2 || g_mex.slot != (int) slot
             || !(g_mex.beat == MXB_COMBO
                  || (g_mex.beat == MXB_ATK && g_mex.verdict != 2)))
        no = "we are not in an exchange with that attacker (or its blow was dodged)";
    else if (MsqCooling(slot, now))
        no = "less than 2.5 s since the last sequence with it (no chaining)";
    else if (g_downed || g_downDying || g_execUntil || g_uc4DyingUntil
             || g_downAt || LocalHp() <= 0.0f)
        no = "we are down / dying / in an execution (the blow downed us: the normal Melee Down)";
    else if (g_dcm.on_att || g_dcm.on_vic || g_msq.on)
        no = "already in a paired move";
    if (no) {
        MQLOG("mseq: pair %s on us by slot %u refused: %s",
              CD_PAIRS[pr].name, slot, no);
        g_msq.n_refused++;
        return 0;
    }
    uint8_t *peer = ((fn_peer) T2rPeer) (slot);
    uint64_t ah = peer ? *(uint64_t *) (peer + 0x60) : 0;
    uint64_t left = S60(end_s) > skipf + 3 ? S60(end_s) - skipf : 3;
    if (g_mhr.on)
        MhrEnd("the synced pair takes the body", 1);   // the flinch the hit message started: the pair's victim half replaces it
    g_mhs.on = 0;
    g_mhs.react = 0;
    MeleeLockStart(1);
    Uc4OvOffExec();
    if (g_uc4_me.cur)
        Uc4Stop(&g_uc4_me);
    if (g_uc4_pup[slot].cur)
        Uc4Stop(&g_uc4_pup[slot]);
    FinposBegin(2, (int) slot, 0, left + 60, "melee sequence on us");
    if (ah)
        XfaceStart(ah, SID_PLAYER, end_s + 0.6f);
    g_msq.on = 1;
    g_msq.role = 2;
    g_msq.slot = (int) slot;
    g_msq.row = pr;
    g_msq.blow = g_mex.blow;
    g_msq.uid = g_ps.uid;
    g_msq.used_uid = g_mex.uid;
    g_msq.end_s = end_s;
    g_msq.n_pairs++;
    g_msq.t0_ms = now;
    g_msq.end_ms = now + (uint64_t) (left * 1000ull / 60ull);
    g_msq.final_ = 0;
    g_msq.buf_at = 0;
    MQLOG
        ("mseq: u%04x pair %s (blow %d) on us by slot %u accepted: hand-off in %.2f s (skip %llu ticks); no damage in the pair, the next blow gets its dodge window",
         g_mex.uid, CD_PAIRS[pr].name, g_mex.blow, slot,
         (double) left / 60.0, (unsigned long long) skipf);
    return 1;
}

// cd_aborted: the other half went away / refused. 1 = ours (no plain down hit: the blow already landed)
static int MsqAborted(int role, uint32_t from)
{
    if (!g_msq.on)
        return 0;
    MQLOG
        ("mseq: u%04x pair aborted by slot %u (role %d, reason %d) - both back to neutral, the exchange goes on",
         g_msq.used_uid, from, role, g_psAbortReason);
    if (role == 1 && g_psAbortReason == PS_AB_REFUSED)
        g_msq.used_uid = 0;     // refused: no sequence happened, the normal 1.2 s cooldown applies
    MsqFinish("aborted");
    return 1;
}

// ---- the exchange's combo press: buffered while the pair plays, released after the hand-off. pedge = the raw press edge of this frame. Returns the edge mex should act on
static int MsqComboPress(int pedge, uint64_t now)
{
    if (g_msq.req) {
        if (pedge) {
            g_msq.req = 0;
            MQLOG
                ("mseq: u%04x combo press before the pair started - no pair, the blow starts now",
                 g_mex.uid);
        }
        return pedge;
    }
    if (g_msq.on) {
        if (pedge && !g_msq.buf_at) {
            g_msq.buf_at = now;
            MQLOG
                ("mseq: u%04x combo press buffered (%d ms before the hand-off)",
                 g_mex.uid, MsqLeftMs());
        }
        return 0;
    }
    if (!pedge && g_msq.buf_at && g_msq.over_at
        && now >= g_msq.over_at + MSQ_BUF_GUARD_MS) {
        uint64_t b = g_msq.buf_at;
        g_msq.buf_at = 0;
        if (now - b <= MSQ_BUF_MAX_MS) {
            MQLOG
                ("mseq: u%04x buffered combo press released at the hand-off (pressed %llu ms ago)",
                 g_mex.uid, (unsigned long long) (now - b));
            return 1;
        }
    }
    if (pedge)
        g_msq.buf_at = 0;
    return pedge;
}

// every frame (first thing of mex_tick)
static void MsqTick(void)
{
    uint64_t now = GetTickCount64();
    MsqCfg(now);
    if (g_msq.req) {
        if (MatchSubstate() != 6 || g_mex.role != 1)
            g_msq.req = 0;
        else if (g_frame > g_msq.req_f) {       // (a frame after the hit message went out)
            int r = MsqAttempt();
            if (r == 2 && g_frame - g_msq.req_f < MSQ_REQ_FRAMES)
                return;
            if (r == 2)
                MQLOG
                    ("mseq: u%04x blow %d - the path gate never answered - no pair",
                     g_mex.uid, g_msq.req_blow);
            g_msq.req = 0;
            if (r != 1 && g_msq.req_final && g_mex.role == 1)
                MexReset("blow 3 landed (no pair)", 1, 1);
        }
    }
    if (!g_msq.on)
        return;
    if (MatchSubstate() != 6) {
        MsqFinish("left the match");
        return;
    }
    if (!(g_ps.role && g_ps.kind == PS_CTXDOWN && g_ps.uid == g_msq.uid)) {
        int fin = g_msq.role == 1 && g_msq.final_;
        MsqFinish("psync over");
        if (fin && g_mex.role == 1)
            MexReset("blow 3 landed, sequence over", 1, 1);
        return;
    }
    double t = (PsyncMs() - g_ps.t0 + g_ps.chain_shift) / 1000.0;      // clip seconds since the contact
    g_msq.end_ms =
        now +
        (uint64_t) ((g_ps.kill_s - t >
                     0.0 ? g_ps.kill_s - t : 0.0) * 1000.0);
    // the exchange must outlive its pair: the combo window (attacker) / the wait for the next blow (hit player) counts from the hand-off
    if (g_mex.role && g_mex.slot == g_msq.slot) {
        uint64_t need =
            g_msq.end_ms + (g_mex.role ==
                            1 ? (uint64_t) MX_COMBO_MS
                            : (uint64_t) (MX_COMBO_MS + g_mex.lat * 2 +
                                          300));
        if (g_mex.combo_until < need)
            g_mex.combo_until = need;
        if (g_mex.hard_dl < need + 3000)
            g_mex.hard_dl = need + 3000;
    }
    static uint32_t logged;
    if (logged != g_msq.uid && t >= 0.2) {
        logged = g_msq.uid;
        MQLOG("mseq: u%04x pair %s at %.2f s (role %s), hand-off in %d ms",
              g_msq.used_uid, CD_PAIRS[g_msq.row].name, t,
              g_msq.role == 1 ? "attacker" : "hit player", MsqLeftMs());
    }
}
