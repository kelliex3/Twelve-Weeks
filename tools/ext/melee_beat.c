// melee_beat.c - the melee BEAT QUEUE: a duel arbitrated by its OWNER, client to client (design re/melee-beat-queue.md).
// Included after melee_duel_parry.c in t2r-ext.c (forward declarations next to the melee_duel.c ones). Log prefix "beat:". Opt-out: t2r-melee-beat-off.txt = the old path.
//
// WHY: contact was decided by the attacker's client and vetoed by the victim's screen, every pair START arrived one-way late (skip ahead), each screen saw its own fight.
// MODEL: the two duelists of a duel (melee_duel.c ENGAGE) have ONE owner (the duelist whose ENGAGE formed the duel, simultaneous engage: the lower slot). The other
// duelist unicasts its stamped intents to the owner; the owner resolves every exchange and broadcasts RESULT to everyone. Every client then plays the resolved outcome at
// PLAY_AT through the EXISTING paths (mdu_contact -> the -hit pair, the miss, the parry / clash pair of melee_duel_parry.c). The press itself still plays its local
// anticipation at once (the swing's windup, the dodge clip); only the contact decision waits for the RESULT (mdu_tick holds the contact: bq_hold).
//
// DUEL IDENTITY: the record g_bq (pair + owner + beat clock T0 + capability) is created at the first engage of two players and lives until one of them dies,
// leaves the match, or the match ends. The old duel (g_mdu) comes and goes inside it (idle timeouts, third-party breaks, downs): re-engaging the same two players never makes
// a new record or owner. Idle beats cost nothing: "beat: duel kept (idle N s)" every 10 s while only the record exists.
//
// CLOCK: no clock is synchronised across machines. T0 = the owner's ENGAGE send time; the other side sets T0 = arrival - one-way(owner), re-derived at every ENGAGE of the
// owner and smoothed by every RESULT (which carries the owner's duel ms at its send). Stamps / PLAY_AT are "duel ms" (local ms - T0). A RESULT's PLAY_AT is converted by the
// owner's own clock difference (PLAY_AT - owner's duel ms at send - one-way), so a T0 drift does not move it.
//
// WIRE (MSG_MEX 0x9C, buf[0] = 5 = the duel family, buf[1] = op, buf[2..3] = duel uid; older clients route op >= 11 into pul_recv, which ignores unknown ops; the transport is
// NOT reliable: INTENT is resent once, RESULT is sent twice, receivers dedupe):
//   ENGAGE (op 1) payload byte buf[6] / SWING (op 2) buf[11] = flags: bit 0 "I run the beat queue", bit 1 "I am the owner of the record"
//   op 15 INTENT  duelist -> owner, len 14: [4] kind (1 attack, 2 dodge, 3 withdraw an attack) [5..8] stamp i32 duel ms [9] row [10] step [11..12] tce u16 (contact - stamp) [13] dir
//   op 16 RESULT  owner -> all, len 27: [4] rid [5] outcome [6] att slot [7] other slot [8] att row [9] other row [10] perfect [11..14] PLAY_AT i32 duel ms
//                 [15..18] att stamp [19..22] other's attack stamp (0x7fffffff = none) [23..26] the owner's duel ms at send  (receivers accept len >= 23; fields past 27 are free)
//   op 17 CAP     len 5: [4] bit 0 = "I saw your capability" (non-owner -> owner: turns beat mode on), bit 1 = "I am the owner" (owner -> other: adopt uid, turn beat mode on)
// BYSTANDERS: they run no beat code on the pair; they see what they always saw (the swing replicas, the pair START of psync, the dodge / miss clips). RESULT is only logged.
#define BQ_BEAT_MS 700          // beat length (labels results; SP blow ~600-850 ms)
#define BQ_LOCK_MS 120          // a press in the last this of a beat is labelled for the next one (queue)
#define BQ_DODGE_WIN 450        // a dodge press at most this long before the contact dodges it
#define BQ_PERFECT 250          // ... within this = perfect (flag only: the counter arming stays dcounter.c's own late-press rule)
#define BQ_PARRY_WIN 300        // the other duelist's press at most this long before the contact (and >= MDP_MIN_LEAD_MS) = PARRY
#define BQ_CLASH_MS 120         // both attacks stamped within this = CLASH
#define BQ_FB_MS 5000           // after a swing's RESULT never came, the old rules run for this long (or until the next ENGAGE)
#define BQ_NONE 0x7fffffff
#define BQ_QN 8
#define BQ_CN 16
#define BQO_INTENT 15
#define BQO_RESULT 16
#define BQO_CAP 17
enum { BQK_ATTACK = 1, BQK_DODGE = 2, BQK_WITHDRAW = 3 };
enum { BQR_HIT = 1, BQR_DODGED, BQR_PARRY, BQR_CLASH, BQR_TRADE };
// At 30 ms ping duel hit reactions were a tad delayed after the 1st punch (log lines 39924-40260): every blow resolved at
// contact + grace 60 ms (one-way 9 ms: the 60 ms floor, not the network) and played PLAY_AT 60 ms later = the reaction ~120 ms after the visible contact.
// The fixed 40 ms margins and the 60 / 80 ms floors are cut to 15 / 25 ms: at one-way 9 ms ~45 ms. Network-sized waits (2 x one-way) stay. TUNING, A/B check.)
#define BQ_MARGIN 15
#define BQ_MIN_WAIT 25
static const char *const BQR_NAME[] =
    { "idle", "HIT", "DODGED", "PARRY", "CLASH", "TRADE" };
enum { BQC_NONE = 0, BQC_HIT = 1, BQC_DODGED = 2, BQC_BEATEN = 3, BQC_PAIR = 5 };       // the ruling on OUR swing (bq_take_ruling: 1 / 2 / 3 for mdu_contact)
static struct {
    int valid, partner, owner_self, active;
    uint16_t uid;
    uint64_t t0, born, last_act, idle_log, fb_until;
    struct {
        int8_t slot, kind, row, step;
        int32_t stamp;
        int16_t tce, dir;
    } q[BQ_QN];
    int nq;
    struct {
        int8_t slot, kind;
        int32_t stamp;
        uint64_t at;
    } cons[BQ_CN];
    int ci;                     // consumed / withdrawn intents: a resent copy is not queued again
    struct {
        uint8_t b[14];
        uint64_t at;
        int used;
    } rs[2];                    // the one resend of our last attack / dodge INTENT
    struct {
        uint8_t b[37];
        int n;
        uint64_t at;
        int used;
    } r2;                       // the owner's second RESULT copy
    struct {
        int valid, att, vic;
        float x, z, yaw;
        uint64_t until;
        int logged;
    } al;                       // smooth:align - the owner's agreed align point of the last HIT / TRADE result
    int16_t rid_seen[8];
    int rid_i;                  // RESULT dedupe (non-owner)
    // our own pending swing
    int sw_pending, cr;
    uint64_t sw_t0, go_at, expect_by, parry_at, shift_until;
    int32_t sw_stamp;
    int shift_slot;
    int parry_kind, parry_bset, parry_sw, parry_rel;
    // victim side: a HIT result against us (vetoes off); attacker side: the dodger is not "immune" for this blow
    int vf_slot, nf_slot;
    uint64_t vf_until, nf_until;
    // smooth:windup - our swing's windup clip, slowed to x0.8 while its contact waits for the RESULT, then retimed so the contact frame lands at PLAY_AT
    struct {
        int on;
        uint64_t clip, last;
        float base, rel, prog, tce;
        int n;
    } wu;
    uint8_t rid;
    int dead_n, nlog;
} g_bq;
#define BQLOG(...) do { if (g_bq.nlog < 4000) { g_bq.nlog++; Xlog(__VA_ARGS__); } } while (0)
static void BqWuEnd(uint64_t now, const char *why);   // smooth:windup (below)
static int BqOff(void)
{
    static int v = -1;
    static uint64_t at;
    uint64_t n = GetTickCount64();
    if (v < 0 || n - at > 2000) {
        at = n;
        FILE *f = ExeSideOpen("t2r-melee-beat-on.txt", "r");
        int o = f == NULL;
        if (f)
            fclose(f);          /* OFF by default, t2r-melee-beat-on.txt = on */
        if (o != v)
            Xlog("beat: %s",
                 o ?
                 "OFF (default; t2r-melee-beat-on.txt = on) - the old duel path everywhere"
                 : "on (host-less duel arbitration by the duel owner)");
        v = o;
    }
    return v;
}

static int BqCapFlags(void)
{
    return BqOff()? 0 : 1 | (g_bq.valid && g_bq.owner_self ? 2 : 0);
}

static int BqClampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static int BqOw(int slot)
{
    return BqClampi((int) (PsyncOneway(slot) + 0.5f), 0, 250);
}

static int32_t BqDt(uint64_t now)
{
    return (int32_t) ((int64_t) now - (int64_t) g_bq.t0);
}

static int BqFb(uint64_t now)
{
    return now < g_bq.fb_until;
}

static int BqReady(void)
{
    return g_bq.valid && g_bq.active && !BqFb(GetTickCount64())
        && g_mdu.on && g_mdu.slot >= 0 && g_mdu.slot == g_bq.partner
        && !BqOff();
}

static void BqSend(int slot, const uint8_t *b, int n)
{
    if (g_sendTramp)
        g_sendTramp(MSG_MEX, (uint8_t *) b, (uint64_t) n,
                     (uint64_t) slot);
}

static void BqCapSend(int slot, int bits)
{
    uint8_t b[5] =
        { 5, BQO_CAP, (uint8_t) g_bq.uid, (uint8_t) (g_bq.uid >> 8),
 (uint8_t) bits };
    BqSend(slot, b, 5);
}

static void BqRecEnd(const char *why)
{
    if (!g_bq.valid)
        return;
    BQLOG
        ("beat: duel ended (%s) with slot %d, owner %s, record age %llu s",
         why, g_bq.partner, g_bq.owner_self ? "us" : "them",
         (unsigned long long) ((GetTickCount64() - g_bq.born) / 1000));
    BqWuEnd(GetTickCount64(), "duel record ended");
    int nl = g_bq.nlog;
    memset(&g_bq, 0, sizeof g_bq);
    g_bq.nlog = nl;
    g_bq.vf_slot = g_bq.nf_slot = g_bq.shift_slot = -1;
}

// the first engage of two players creates the record; every later one keeps it (owner, capability). owner_self = our swing formed this engage.
static void BqNoteEngage(int s, int owner_self, uint16_t uid,
                           uint64_t now)
{
    if (BqOff() || s < 0 || s >= T2R_MAX_PEERS)
        return;
    if (g_bq.valid) {
        g_bq.fb_until = 0;      // the next ENGAGE clears a fallback
        if (g_bq.partner != s) {
            BQLOG
                ("beat: engage with slot %d while the beat duel with slot %d stands - this fight runs the old path",
                 s, g_bq.partner);
            return;
        }
        BQLOG("beat: duel kept (re-engage after %llu ms idle, owner %s)",
              (unsigned long long) (now - g_bq.last_act),
              g_bq.owner_self ? "us" : "them");
        g_bq.last_act = now;
        if (owner_self && g_bq.owner_self) {
            g_bq.uid = uid;
            g_bq.t0 = now;
            g_bq.nq = 0;        /* The owner kept the first engage's uid while the other side adopted the re-engage's ENGAGE uid - every later RESULT / INTENT was dropped as a stranger */
            g_bq.sw_pending = 0;
            g_bq.cr = BQC_NONE;
            BqCapSend(s, 2);
        }                       // T0 re-derived: the owner's ENGAGE time (the other side re-derives at its arrival)
        return;
    }
    memset(&g_bq, 0, sizeof g_bq);
    g_bq.vf_slot = g_bq.nf_slot = g_bq.shift_slot = -1;
    g_bq.valid = 1;
    g_bq.partner = s;
    g_bq.owner_self = owner_self;
    g_bq.uid = uid;
    g_bq.born = g_bq.last_act = g_bq.idle_log = now;
    g_bq.t0 = owner_self ? now : now - (uint64_t) BqOw(s);
    BQLOG
        ("beat: duel record u%04x with slot %d, owner %s, T0 %s (cap exchange pending)",
         uid, s, owner_self ? "us" : "them",
         owner_self ? "= our ENGAGE" : "= their ENGAGE arrival - one-way");
    if (owner_self)
        BqCapSend(s, 2);      // the owner announces itself outright: a lost ENGAGE still activates the other side
}

// melee_duel.c mdu_recv: ENGAGE (op 1) / SWING (op 2) from the partner carry its flags
static void BqRecvEngage(uint32_t slot, int flags, uint16_t uid,
                           uint64_t now, int op)
{
    if (!g_bq.valid || g_bq.partner != (int) slot || !(flags & 1)
        || BqOff())
        return;
    int claim = (flags & 2) != 0;
    if (op == 1)
        g_bq.fb_until = 0;
    if (g_bq.owner_self) {
        if (claim && g_bq.rid == 0 && g_bq.nq == 0 && (int) PsyncSelf() > (int) slot) {        // two owners (simultaneous engage): the lower slot owns, no time limit while nothing was resolved
            g_bq.owner_self = 0;
            g_bq.uid = uid;
            g_bq.t0 = now - (uint64_t) BqOw((int) slot);
            g_bq.active = 1;
            g_bq.sw_pending = 0;
            g_bq.cr = BQC_NONE;
            BqCapSend((int) slot, 1);
            BQLOG
                ("beat: two owners (simultaneous engage) with slot %u - the lower slot owns: owner is THEM now, beat mode on",
                 slot);
        } else if (!g_bq.active) {
            g_bq.active = 1;
            BQLOG
                ("beat: slot %u runs the beat queue (its flags) - beat mode on, we own",
                 slot);
        }
        return;
    }
    if (claim && op == 1) {
        g_bq.uid = uid;
        g_bq.t0 = now - (uint64_t) BqOw((int) slot);
    }                           // the owner's ENGAGE: T0 re-derived
    if (claim && !g_bq.active) {
        g_bq.active = 1;
        BqCapSend((int) slot, 1);
        BQLOG
            ("beat: owner slot %u runs the beat queue - beat mode on, CAP sent",
             slot);
    }
}

static int BqPeerHere(int s)
{
    uint8_t *pe = MdgPeer((uint32_t) s);
    return pe && pe[4];
}

// ---- the owner's queue
static int BqBeatOf(int32_t st)
{
    return st % BQ_BEAT_MS >
        BQ_BEAT_MS - BQ_LOCK_MS ? st / BQ_BEAT_MS + 1 : st / BQ_BEAT_MS;
}

static int BqWasConsumed(int slot, int kind, int32_t stamp)
{
    for (int i = 0; i < BQ_CN; i++)
        if (g_bq.cons[i].at && g_bq.cons[i].slot == slot
            && g_bq.cons[i].kind == kind && g_bq.cons[i].stamp == stamp
            && GetTickCount64() - g_bq.cons[i].at < 8000)
            return 1;
    return 0;
}

static void BqConsume(int slot, int kind, int32_t stamp)
{
    g_bq.cons[g_bq.ci % BQ_CN].slot = (int8_t) slot;
    g_bq.cons[g_bq.ci % BQ_CN].kind = (int8_t) kind;
    g_bq.cons[g_bq.ci % BQ_CN].stamp = stamp;
    g_bq.cons[g_bq.ci % BQ_CN].at = GetTickCount64();
    g_bq.ci++;
}

static void BqQDel(int i)
{
    for (int k = i; k + 1 < g_bq.nq; k++)
        g_bq.q[k] = g_bq.q[k + 1];
    g_bq.nq--;
}

static void BqQAdd(int slot, int kind, int32_t stamp, int row, int step,
                     int tce, int dir)
{
    if (kind == BQK_WITHDRAW) { // withdraw the attack of that slot stamped within 40 ms of this stamp
        for (int i = g_bq.nq - 1; i >= 0; i--)
            if (g_bq.q[i].slot == slot && g_bq.q[i].kind == BQK_ATTACK
                && g_bq.q[i].stamp - stamp <= 40
                && stamp - g_bq.q[i].stamp <= 40) {
                BqConsume(slot, BQK_ATTACK, g_bq.q[i].stamp);
                BqQDel(i);
                BQLOG
                    ("beat: attack of slot %d stamped %d withdrawn (its swing ended)",
                     slot, stamp);
            }
        BqConsume(slot, BQK_ATTACK, stamp);    // (a resent copy arriving later is dropped too)
        return;
    }
    if (BqWasConsumed(slot, kind, stamp))
        return;
    int beat = BqBeatOf(stamp);
    for (int i = 0; i < g_bq.nq; i++) { // one buffered intent per player and kind per beat: a newer press replaces the older
        if (g_bq.q[i].slot == slot && g_bq.q[i].kind == kind
            && BqBeatOf(g_bq.q[i].stamp) == beat) {
            g_bq.q[i].stamp = stamp;
            g_bq.q[i].row = (int8_t) row;
            g_bq.q[i].step = (int8_t) step;
            g_bq.q[i].tce = (int16_t) tce;
            g_bq.q[i].dir = (int16_t) dir;
            return;
        }
    }
    if (g_bq.nq >= BQ_QN) {
        memmove(g_bq.q, g_bq.q + 1, sizeof g_bq.q[0] * (BQ_QN - 1));
        g_bq.nq = BQ_QN - 1;
    }
    g_bq.q[g_bq.nq].slot = (int8_t) slot;
    g_bq.q[g_bq.nq].kind = (int8_t) kind;
    g_bq.q[g_bq.nq].stamp = stamp;
    g_bq.q[g_bq.nq].row = (int8_t) row;
    g_bq.q[g_bq.nq].step = (int8_t) step;
    g_bq.q[g_bq.nq].tce = (int16_t) tce;
    g_bq.q[g_bq.nq].dir = (int16_t) dir;
    g_bq.nq++;
}

static void BqIntent(int kind, int32_t stamp, int row, int step, int tce,
                      int dir, uint64_t now)
{
    g_bq.last_act = now;
    if (g_bq.owner_self) {
        BqQAdd((int) PsyncSelf(), kind, stamp, row, step, tce, dir);
        return;
    }
    uint8_t b[14] =
        { 5, BQO_INTENT, (uint8_t) g_bq.uid, (uint8_t) (g_bq.uid >> 8),
(uint8_t) kind };
    memcpy(b + 5, &stamp, 4);
    b[9] = (uint8_t) row;
    b[10] = (uint8_t) step;
    b[11] = (uint8_t) tce;
    b[12] = (uint8_t) (tce >> 8);
    b[13] = (uint8_t) dir;
    BqSend(g_bq.partner, b, 14);
    if (kind == BQK_ATTACK || kind == BQK_DODGE) {
        int k = kind == BQK_ATTACK ? 0 : 1;
        memcpy(g_bq.rs[k].b, b, 14);
        g_bq.rs[k].at =
            now + (uint64_t) BqClampi(2 * BqOw(g_bq.partner), 120, 500);
        g_bq.rs[k].used = 1;
    } else
        g_bq.rs[0].used = 0;
}

// ---- smooth:windup: the windup clip does not run into its contact frame and stand there for the RESULT. In the clip's second half (until the RESULT is here) it plays at
// x0.8; when the RESULT says when the contact plays (go_at: HIT / DODGED, parry_at: pair), the speed is set so the contact frame lands exactly then (x0.6 .. x1.25). Progress
// is tracked in "unstretched ms" (prog, the swing's own tce = contact), speed set with mstag_speed = set-animation-speed on the clip over the player's layers (the ping
// windup of melee_duel.c uses the same call; its base speed g_mdu_sw_sp is kept as the base). t2r-smooth-windup-off.txt = the clip runs at its own pace.
#define BQ_WU_SLOW 0.8f
#define BQ_WU_FAST 1.25f
#define BQ_WU_MIN 0.6f
static void BqWuSpeed(float rel, uint64_t now, const char *why)
{
    if (!g_bq.wu.on)
        return;
    mstag_speed(g_bq.wu.clip, g_bq.wu.base * rel);
    if (g_bq.wu.n++ < 12)
        BQLOG("smooth: windup speed x%.2f (clip %.0f of %.0f ms done, %s)",
              (double) (g_bq.wu.base * rel), (double) g_bq.wu.prog,
              (double) g_bq.wu.tce, why);
    g_bq.wu.rel = rel;
    (void) now;
}

static void BqWuEnd(uint64_t now, const char *why)
{
    if (!g_bq.wu.on)
        return;
    mstag_speed(g_bq.wu.clip, 1.0f);
    BQLOG
        ("smooth: windup stretch ends (%s): clip at %.0f of %.0f ms, speed back to x1.00",
         why, (double) g_bq.wu.prog, (double) g_bq.wu.tce);
    g_bq.wu.on = 0;
    (void) now;
}

static void BqWuBegin(int tce, uint64_t now)
{
    BqWuEnd(now, "new swing");
    if (SmoothOff(3) || tce < 120)
        return;
    uint64_t clip = MstagClipOf(g_mduSwMove);
    if (!clip) {
        int r = g_mdu.sw_row;
        if (r >= 0 && r < (int) MDU_MOVES_N && MDU_MOVES[r].swing
            && *MDU_MOVES[r].swing)
            clip = FNV64(MDU_MOVES[r].swing);
    }
    if (!clip)
        return;
    g_bq.wu.on = 1;
    g_bq.wu.clip = clip;
    g_bq.wu.last = now;
    g_bq.wu.base = g_mduSwClip == clip ? g_mduSwSp : 1.0f;
    g_bq.wu.rel = 1.0f;
    g_bq.wu.prog = 0.0f;
    g_bq.wu.tce = (float) tce;
    g_bq.wu.n = 0;
}

static void BqWuTick(uint64_t now)
{
    if (!g_bq.wu.on)
        return;
    if (!g_bq.sw_pending || g_bq.sw_t0 != g_mdu.sw_t0 || !g_mdu.sw_on) {
        BqWuEnd(now, "swing over");
        return;
    }
    g_bq.wu.prog += (float) (now - g_bq.wu.last) * g_bq.wu.rel;
    g_bq.wu.last = now;
    float want = 1.0f;
    const char *why = "clip's first half";
    if (g_bq.cr == BQC_NONE) {
        if (g_bq.wu.prog >= 0.5f * g_bq.wu.tce) {
            want = BQ_WU_SLOW;
            why = "contact waits for the RESULT";
        }
    } else if (g_bq.cr == BQC_HIT || g_bq.cr == BQC_DODGED
               || g_bq.cr == BQC_PAIR) {
        uint64_t tgt = g_bq.cr == BQC_PAIR ? g_bq.parry_at : g_bq.go_at;
        float R = g_bq.wu.tce - g_bq.wu.prog;
        int64_t W = (int64_t) tgt - (int64_t) now;
        why = "RESULT: contact retimed to PLAY_AT";
        if (R > 5.0f && W > 5) {
            want = R / (float) W;
            if (want > BQ_WU_FAST)
                want = BQ_WU_FAST;
            if (want < BQ_WU_MIN)
                want = BQ_WU_MIN;
        } else
            why = "RESULT: contact frame reached / PLAY_AT due";
    }
    if (want - g_bq.wu.rel > 0.04f || g_bq.wu.rel - want > 0.04f)
        BqWuSpeed(want, now, why);
}

// melee_duel.c mdu_swing_note: our plain duel swing started (contact in tce ms): stamp + send the intent. The windup plays now; the contact waits for the RESULT.
static void BqPressAttack(int row, int step, int tce, uint64_t now)
{
    if (!BqReady() || tce < 0 || tce > 3000)
        return;
    int ow = BqOw(g_bq.partner), grace =
        BqClampi(2 * ow + BQ_MARGIN, BQ_MIN_WAIT,
                  420) /* = the owner's resolve grace (view shift) */ ;
    g_bq.sw_pending = 1;
    g_bq.sw_t0 = g_mdu.sw_t0;
    g_bq.sw_stamp = BqDt(now);
    g_bq.cr = BQC_NONE;
    g_bq.go_at = 0;
    g_bq.parry_at = 0;
    g_bq.expect_by = now + (uint64_t) tce + (uint64_t) BqClampi(grace + ow + 150, 400, 900);   // the contact is held at most this long for the RESULT
    BqWuBegin(tce, now);
    BQLOG
        ("beat: press ATTACK row %d step %d stamped %d ms (beat %d), contact +%d ms, %s",
         row, step, g_bq.sw_stamp, g_bq.sw_stamp / BQ_BEAT_MS, tce,
         g_bq.owner_self ? "queued (we own)" : "sent to the owner");
    BqIntent(BQK_ATTACK, g_bq.sw_stamp, row, step, tce, 0, now);
}

// melee_dodge.c mdg_start: our dodge press (the dodge clip already plays)
static void BqPressDodge(uint32_t slot, uint64_t now)
{
    if (!BqReady() || (int) slot != g_bq.partner)
        return;
    int32_t st = BqDt(now);
    BQLOG("beat: press DODGE stamped %d ms (beat %d), %s", st,
          st / BQ_BEAT_MS,
          g_bq.owner_self ? "queued (we own)" : "sent to the owner");
    BqIntent(BQK_DODGE, st, 0, 0, 0, 0, now);
}

// ---- smooth:align - the pair's align point is AGREED: the owner picks it from the two bodies at resolve time and every screen aligns to it, instead of each
// screen fitting the pair onto its own (up to a metre different) view. dc_anchor (the pair placement) takes the victim's spot + the bearing victim -> attacker; the point
// is exactly that: victim x / z (the victim stays where it stands), the yaw of the line to the attacker (the attacker is placed on that line at the clip's distance by
// the pair's own authored spot). Wire: RESULT bytes [27..30] x f32, [31..34] z f32, [35..36] yaw u16 (2 pi / 65536); a 27-byte RESULT = no align point.
// t2r-smooth-align-off.txt = every screen fits on its own view (the old way). Arming is a plain assignment: the second RESULT copy / a resend is idempotent.
static int BqLocXz(int slot, float *x, float *z)
{
    float b[16];
    uint64_t ent =
        slot == (int) PsyncSelf()? SID_PLAYER : PsyncPuppet(slot);
    const float *l =
        slot ==
        (int) PsyncSelf()? PlayerLocator(b) : (ent ? PsyncLoc(ent, b) :
                                                 NULL);
    if (!l || !PsLocOk(l))
        return 0;
    *x = l[0];
    *z = l[2];
    return 1;
}

static int BqAlignPick(int a, int o, float *x, float *z, float *yaw)
{
    if (SmoothOff(2))
        return 0;
    float ax, az, vx, vz;
    if (!BqLocXz(a, &ax, &az) || !BqLocXz(o, &vx, &vz))
        return 0;
    float dx = ax - vx, dz = az - vz;
    if (dx * dx + dz * dz < 0.09f || dx * dx + dz * dz > 25.0f)
        return 0;
    float y = __builtin_atan2f(dx, dz);
    if (y < 0.0f)
        y += 6.2831853f;
    *x = vx;
    *z = vz;
    *yaw = y;
    return 1;
}

static void BqAlignArm(int a, int o, float x, float z, float yaw,
                         uint64_t until)
{
    g_bq.al.valid = 1;
    g_bq.al.att = a;
    g_bq.al.vic = o;
    g_bq.al.x = x;
    g_bq.al.z = z;
    g_bq.al.yaw = yaw;
    g_bq.al.until = until;
    g_bq.al.logged = 0;
    BQLOG
        ("smooth: align point agreed for slot %d -> slot %d: x %.2f z %.2f, bearing %.0f deg",
         a, o, (double) x, (double) z, (double) (yaw * 57.29578f));
}

// melee_duel.c mdu_pair_place (the attacker's pair placement): 1 = vl_o / me_o hold the agreed spots (copies of vl / me with x / z replaced)
static int BqAlignFit(int vic_slot, const float *vl, const float *me,
                        float *vl_o, float *me_o)
{
    if (!g_bq.valid || !g_bq.al.valid || !vl || !me)
        return 0;
    if (GetTickCount64() > g_bq.al.until) {
        g_bq.al.valid = 0;
        return 0;
    }
    if (g_bq.al.att != (int) PsyncSelf() || g_bq.al.vic != vic_slot
        || SmoothOff(2))
        return 0;
    float dx = me[0] - vl[0], dz = me[2] - vl[2], d =
        __builtin_sqrtf(dx * dx + dz * dz);
    d = d < 0.7f ? 0.7f : d > 1.6f ? 1.6f : d;
    memcpy(vl_o, vl, 8 * sizeof(float));
    memcpy(me_o, me, 8 * sizeof(float));
    vl_o[0] = g_bq.al.x;
    vl_o[2] = g_bq.al.z;
    me_o[0] = g_bq.al.x + d * __builtin_sinf(g_bq.al.yaw);
    me_o[2] = g_bq.al.z + d * __builtin_cosf(g_bq.al.yaw);
    if (!g_bq.al.logged) {
        g_bq.al.logged = 1;
        BQLOG
            ("smooth: align - the pair is placed on the agreed point: victim spot moved %.2f m from our view of it, attacker bearing %+.0f deg from ours",
             (double) __builtin_sqrtf((vl[0] - vl_o[0]) *
                                      (vl[0] - vl_o[0]) + (vl[2] -
                                                           vl_o[2]) *
                                      (vl[2] - vl_o[2])),
             (double) (CdWrap180
                       ((g_bq.al.yaw -
                         __builtin_atan2f(dx, dz)) * 57.29578f)));
    }
    return 1;
}

// paired.c psync_play: the victim keeps the START's (agreed) ap instead of refitting it on its own view
static int BqAlignVic(int att_slot)
{
    if (!g_bq.valid || !g_bq.al.valid || SmoothOff(2))
        return 0;
    if (GetTickCount64() > g_bq.al.until) {
        g_bq.al.valid = 0;
        return 0;
    }
    if (g_bq.al.vic != (int) PsyncSelf() || g_bq.al.att != att_slot)
        return 0;
    g_bq.al.valid = 0;
    Xlog("smooth: align - the pair on us keeps the agreed ap (no refit on our own view of the two bodies)");
    return 1;
}

// ---- resolution (the owner)
static void BqApply(int outcome, int a, int o, int row_a, int row_o,
                     int perfect, int in_ms, int32_t st_a, int32_t st_o,
                     uint64_t now);
static void BqResolve(uint64_t now)
{
    int64_t ndt = BqDt(now);
    for (int i = g_bq.nq - 1; i >= 0; i--)
        if (ndt - (int64_t) g_bq.q[i].stamp > 4000)
            BqQDel(i);
    int e = -1;
    for (int i = 0; i < g_bq.nq; i++)
        if (g_bq.q[i].kind == BQK_ATTACK
            && (e < 0 || g_bq.q[i].stamp < g_bq.q[e].stamp
                || (g_bq.q[i].stamp == g_bq.q[e].stamp
                    && g_bq.q[i].slot < g_bq.q[e].slot)))
            e = i;
    if (e < 0)
        return;
    int self = (int) PsyncSelf(), ow = BqOw(g_bq.partner), grace =
        BqClampi(2 * ow + BQ_MARGIN, BQ_MIN_WAIT, 420);
    // With 100 ms one-way: the other duelist SEES the swing one-way late, so a dodge pressed 150 ms after the swing on its screen was stamped
    // after the contact and every dodge became a HIT. The answers are judged against the contact as the answering side saw it (contact + one-way);
    // the grace waits for an answer stamped that late (one more one-way to arrive). t2r-beat-no-view-shift.txt = judge against the bare contact.
    int vshift =
        GetFileAttributesA("t2r-beat-no-view-shift.txt") ==
        INVALID_FILE_ATTRIBUTES ? BqClampi(ow, 0, 200) : 0;
    int a = g_bq.q[e].slot, o = a == self ? g_bq.partner : self;
    int32_t sa = g_bq.q[e].stamp;
    int64_t contact64 = (int64_t) sa + g_bq.q[e].tce;
    if (ndt < contact64 + grace)
        return;                 // every intent that can still affect this blow arrives within one-way + margin of its contact
    int32_t contact = (int32_t) contact64, cv = contact + vshift;       // cv: the contact on the answering side's screen
    int ra = g_bq.q[e].row, ro = -1, atk_o = -1, dod = -1;
    int32_t so = BQ_NONE;
    for (int i = 0; i < g_bq.nq; i++) {
        if (g_bq.q[i].slot != o)
            continue;
        if (g_bq.q[i].kind == BQK_ATTACK && g_bq.q[i].stamp <= cv
            && (atk_o < 0 || g_bq.q[i].stamp < g_bq.q[atk_o].stamp))
            atk_o = i;
        if (g_bq.q[i].kind == BQK_DODGE && g_bq.q[i].stamp <= cv
            && cv - g_bq.q[i].stamp <= BQ_DODGE_WIN && (dod < 0
                                                        || g_bq.q[i].
                                                        stamp >
                                                        g_bq.q[dod].stamp))
            dod = i;
    }
    if (atk_o >= 0) {
        so = g_bq.q[atk_o].stamp;
        ro = g_bq.q[atk_o].row;
    }
    int outcome = BQR_HIT, perfect = 0;
    const char *why = "attack, no answer";
    if (dod >= 0) {
        outcome = BQR_DODGED;
        perfect = cv - g_bq.q[dod].stamp <= BQ_PERFECT;
        why = "dodge press inside the dodge window before the contact";
    } else if (atk_o >= 0) {
        int32_t d = so - sa;
        if (d <= BQ_CLASH_MS) {
            outcome = BQR_CLASH;
            why = "both attacks stamped within the clash window";
        } else if (so <= cv - MDP_MIN_LEAD_MS && so >= cv - BQ_PARRY_WIN) {
            outcome = BQR_PARRY;
            why =
                "the other press inside the parry window before the contact";
        } else {
            outcome = BQR_TRADE;
            why =
                "the other press came inside the blow but outside the parry window: the earlier press wins";
        }
    }
    int send = BqClampi(ow + BQ_MARGIN, BQ_MIN_WAIT, 250), mx = 0;     /* The RESULT travels one-way to the other duelist (was 2x one-way): ~120 ms less contact delay */
    for (int s = 0; s < T2R_MAX_PEERS; s++)
        if (s != self && BqPeerHere(s)) {
            int w = BqOw(s) + BQ_MARGIN;
            if (w > mx)
                mx = w;
        }
    if (mx > send)
        send = BqClampi(mx, BQ_MIN_WAIT, 250);
    int32_t pa = (int32_t) ndt + send;
    BQLOG
        ("beat: owner resolve beat %d: attack slot %d row %d stamp %d (contact %d); inputs: other attack %s (%d), dodge %s (%d) -> %s (%s)%s; PLAY_AT in %d ms (grace %d, one-way %d, view shift %d)",
         sa / BQ_BEAT_MS, a, ra, sa, contact, atk_o >= 0 ? "yes" : "no",
         atk_o >= 0 ? so : 0, dod >= 0 ? "yes" : "no",
         dod >= 0 ? g_bq.q[dod].stamp : 0, BQR_NAME[outcome], why,
         perfect ? ", PERFECT dodge" : "", send, grace, ow, vshift);
    BqConsume(a, BQK_ATTACK, sa);
    if (atk_o >= 0 && outcome != BQR_HIT && outcome != BQR_DODGED)
        BqConsume(o, BQK_ATTACK, so);
    if (dod >= 0)
        BqConsume(o, BQK_DODGE, g_bq.q[dod].stamp);
    int del[3] = { e, atk_o, dod };     // consumed (delete high index first)
    if (outcome == BQR_HIT || outcome == BQR_DODGED)
        del[1] = -1;
    for (int pass = 0; pass < 3; pass++) {
        int mi = -1;
        for (int k = 0; k < 3; k++)
            if (del[k] >= 0 && (mi < 0 || del[k] > del[mi]))
                mi = k;
        if (mi < 0)
            break;
        BqQDel(del[mi]);
        del[mi] = -1;
    }
    int32_t nd = (int32_t) ndt;
    float alx = 0.0f, alz = 0.0f, aly = 0.0f;
    int has_al = (outcome == BQR_HIT || outcome == BQR_TRADE)
        && BqAlignPick(a, o, &alx, &alz, &aly);
    uint8_t b[37] =
        { 5, BQO_RESULT, (uint8_t) g_bq.uid, (uint8_t) (g_bq.uid >> 8),
++g_bq.rid, (uint8_t) outcome, (uint8_t) a, (uint8_t) o, (uint8_t) ra,
(uint8_t) (ro < 0 ? 255 : ro), (uint8_t) perfect };
    memcpy(b + 11, &pa, 4);
    memcpy(b + 15, &sa, 4);
    memcpy(b + 19, &so, 4);
    memcpy(b + 23, &nd, 4);
    int blen = 27;
    if (has_al) {
        uint16_t yq =
            (uint16_t) (int) (aly * (65536.0f / 6.2831853f) + 0.5f);
        memcpy(b + 27, &alx, 4);
        memcpy(b + 31, &alz, 4);
        memcpy(b + 35, &yq, 2);
        blen = 37;
        BqAlignArm(a, o, alx, alz, aly, now + (uint64_t) send + 600);
    }
    BqSend(0xff, b, blen);
    memcpy(g_bq.r2.b, b, blen);
    g_bq.r2.n = blen;
    g_bq.r2.at = now + 60;
    g_bq.r2.used = 1;           // second copy (lossy transport; receivers dedupe by rid)
    BqApply(outcome, a, o, ra, ro, perfect, send, sa, so, now);
}

// ---- applying a RESULT on this client (the owner's own result goes through here too; in_ms = PLAY_AT from now on OUR clock)
static void BqSetPair(int kind, int bset, int sw, int rel, uint64_t at)
{
    g_bq.parry_kind = kind;
    g_bq.parry_bset = bset;
    g_bq.parry_sw = sw;
    g_bq.parry_rel = rel;
    g_bq.parry_at = at ? at : 1;
    g_bq.cr = BQC_PAIR;
}

static void BqApply(int outcome, int a, int o, int row_a, int row_o,
                     int perfect, int in_ms, int32_t st_a, int32_t st_o,
                     uint64_t now)
{
    int me = (int) PsyncSelf();
    if (in_ms < 0)
        in_ms = 0;
    if (in_ms > 600)
        in_ms = 600;
    uint64_t pa = now + (uint64_t) in_ms;
    g_bq.last_act = now;
    int mine = me == a ? st_a : me == o ? st_o : BQ_NONE;
    int own = g_bq.sw_pending && g_bq.sw_t0 == g_mdu.sw_t0
        && mine != BQ_NONE && (mine - g_bq.sw_stamp <= 40
                               && g_bq.sw_stamp - mine <= 40);
    int other = me == a ? o : a;
    uint64_t go = in_ms > BqOw(other) ? pa - (uint64_t) BqOw(other) : now;    // the attacker's half starts one-way early: the partner's START arrives AT PLAY_AT
    BQLOG
        ("beat: RESULT %s on slot %d -> slot %d (rows %d / %d)%s received, PLAY_AT in %d ms; we are %s%s",
         BQR_NAME[outcome], a, o, row_a, row_o, perfect ? " PERFECT" : "",
         in_ms,
         me == a ? "the attacker" : me ==
         o ? "the other duelist" : "a bystander",
         own ? " with a pending swing" : "");
    if (own) {
        g_bq.rs[0].used = 0;
    }
    if (outcome == BQR_HIT || outcome == BQR_TRADE) {
        if (me == o) {
            g_bq.vf_slot = a;
            g_bq.vf_until = pa + 800;
        }
        if (me == a && own) {
            g_bq.cr = BQC_HIT;
            g_bq.go_at = go;
            g_bq.nf_slot = o;
            g_bq.nf_until = pa + 900;
            g_bq.shift_slot = o;
        } else if (me == o && own) {
            g_bq.cr = BQC_BEATEN;
            BQLOG
                ("beat: our swing lost the trade - it is cut at its contact, the pair on us comes at PLAY_AT");
        }
    } else if (outcome == BQR_DODGED) {
        if (me == a && own) {
            g_bq.cr = BQC_DODGED;
            g_bq.go_at = pa;
        }
        // The owner's DODGED never reached the dodge counter (dc_arm ran only on the old path's 'our dodge dropped its blow'). The dodger
        // arms it here with the same rules: a melee WEAPON blow, and a PERFECT dodge (or Counter Fighter II for a late one); punches are never countered.
        if (me == o && a >= 0 && a < T2R_MAX_PEERS) {
            uint8_t *wa = T2rPeer((uint32_t) a);
            uint64_t aw = wa
                && !IsBadReadPtr(wa, 0x99) ? *(uint64_t *) (wa + 0x91) : 0;
            if (!DcIsMelee(aw))
                BQLOG
                    ("beat: DODGED - no counter (slot %d's blow was unarmed: punches are dodged, never countered)",
                     a);
            else if (!perfect && !SkCounterLate())
                BQLOG("beat: DODGED - no counter (not a perfect dodge)");
            else {
                BQLOG("beat: DODGED %s - counter armed on slot %d",
                      perfect ? "PERFECT" : "late (Counter Fighter II)",
                      a);
                DcArm((uint32_t) a);
            }
            MdgWhiffWhoosh(PuppetHandle((uint32_t) a), aw, a);
        }
    } else if (outcome == BQR_PARRY) {
        if (me == a && own && row_a >= 0 && row_a < (int) MDU_MOVES_N
            && row_o >= 0 && row_o < (int) MDU_MOVES_N) {
            BqSetPair(MDPK_PARRY, MDU_MOVES[row_o].set, row_a,
                        st_o - st_a, go);
            g_bq.shift_slot = o;
        } else if (me == o && own)
            g_bq.cr = BQC_BEATEN;
    } else if (outcome == BQR_CLASH) {
        if (own && row_a >= 0 && row_o >= 0 && row_a < (int) MDU_MOVES_N
            && row_o < (int) MDU_MOVES_N) {
            int mr = me == a ? row_a : row_o, tr =
                me == a ? row_o : row_a, ws =
                MdpClashWeStart((uint32_t) other, mr, tr);
            if (ws == 1) {
                BqSetPair(MDPK_CLASH, MDU_MOVES[tr].set, mr,
                            me == a ? st_o - st_a : st_a - st_o, go);
                g_bq.shift_slot = other;
            } else if (ws == 0)
                g_bq.cr = BQC_BEATEN;
            else {
                g_bq.cr = BQC_HIT;
                g_bq.go_at = go;
                g_bq.nf_slot = other;
                g_bq.nf_until = pa + 900;
                g_bq.shift_slot = other;
                BQLOG
                    ("beat: CLASH with no SP pair for these weapons - both blows go on");
            }
        }
    }
}

// ---- every frame
static void BqTick(uint64_t now)
{
    if (!g_bq.valid)
        return;
    if (MatchSubstate() != 6) {
        BqRecEnd("left the match / match over");
        return;
    }
    if (!BqPeerHere(g_bq.partner)) {
        BqRecEnd("partner left");
        return;
    }
    if (g_puppetDeadHere[g_bq.partner]) {
        BqRecEnd("partner death");
        return;
    }
    if (LocalHp() <= 0.0f && !g_downed && !g_downDying) {
        if (++g_bq.dead_n >= 3) {
            BqRecEnd("our death");
            return;
        }
    } else
        g_bq.dead_n = 0;        // (a down is no death: three frames in a row)
    if (!g_mdu.on || g_mdu.slot < 0) {
        if (now - g_bq.idle_log >= 10000) {
            g_bq.idle_log = now;
            BQLOG
                ("beat: duel kept (idle %llu s, owner %s, partner slot %d)",
                 (unsigned long long) ((now - g_bq.last_act) / 1000),
                 g_bq.owner_self ? "us" : "them", g_bq.partner);
        }
    } else
        g_bq.idle_log = now;
    if (g_bq.vf_slot >= 0 && now >= g_bq.vf_until)
        g_bq.vf_slot = -1;
    if (g_bq.nf_slot >= 0 && now >= g_bq.nf_until)
        g_bq.nf_slot = -1;
    for (int k = 0; k < 2; k++)
        if (g_bq.rs[k].used && now >= g_bq.rs[k].at) {
            g_bq.rs[k].used = 0;
            BqSend(g_bq.partner, g_bq.rs[k].b, 14);
            BQLOG("beat: no RESULT yet - the %s INTENT is sent once more",
                  k ? "DODGE" : "ATTACK");
        }
    if (g_bq.r2.used && now >= g_bq.r2.at) {
        g_bq.r2.used = 0;
        BqSend(0xff, g_bq.r2.b, g_bq.r2.n);
    }
    if (g_bq.sw_pending && (!g_mdu.sw_on || g_mdu.sw_t0 != g_bq.sw_t0) && now > g_bq.sw_t0 + 60 && g_bq.cr == BQC_NONE) {       // our swing ended without a contact (cut / replaced): withdraw its intent
        BQLOG
            ("beat: our swing ended before its RESULT - intent WITHDRAWN");
        BqIntent(BQK_WITHDRAW, g_bq.sw_stamp, 0, 0, 0, 0, now);
        g_bq.sw_pending = 0;
        g_bq.rs[0].used = 0;
    }
    if (g_bq.parry_at && now >= g_bq.parry_at && g_mdu.on && g_mdu.slot == g_bq.partner && g_mdu.sw_on) {       // the SP parry / clash pair through melee_duel_parry.c's own start
        g_mdp.pend = 1;
        g_mdp.pend_kind = g_bq.parry_kind;
        g_mdp.pend_bset = g_bq.parry_bset;
        g_mdp.pend_rel = g_bq.parry_rel;
        g_mdp.pend_sw = g_bq.parry_sw;
        g_mdp.pend_at = now;
        g_mdp.gate_tries = 0;
        g_mdp.pend_pr = -1;
        g_bq.parry_at = 0;
        g_bq.shift_until = now + 600;
        g_bq.sw_pending = 0;
        g_bq.cr = BQC_NONE;
        BQLOG
            ("beat: PLAY_AT - %s pair start (our swing row %d, partner set %d) handed to melee_duel_parry.c",
             g_bq.parry_kind == MDPK_PARRY ? "PARRY" : "CLASH",
             g_bq.parry_sw, g_bq.parry_bset);
    } else if (g_bq.parry_at
               && (!g_mdu.on || g_mdu.slot != g_bq.partner
                   || !g_mdu.sw_on)) {
        g_bq.parry_at = 0;
        g_bq.cr = BQC_NONE;
        g_bq.sw_pending = 0;
        g_bq.shift_slot = -1;
        BQLOG("beat: parry / clash start dropped - our swing is gone");
    }
    BqWuTick(now);
    if (g_bq.owner_self && g_bq.active)
        BqResolve(now);        // (independent of the old duel: g_mdu may be off / cooling)
}

// melee_duel.c mdu_tick: 1 = keep holding our contact (the RESULT / PLAY_AT is not here yet)
static int BqHold(uint64_t now)
{
    if (!g_bq.sw_pending || g_bq.sw_t0 != g_mdu.sw_t0 || !g_bq.valid)
        return 0;
    if (g_bq.cr == BQC_NONE) {
        if (now > g_bq.expect_by) {     // per swing: THIS swing resolves with the old rules; the fallback ends at the next ENGAGE or after BQ_FB_MS
            BQLOG
                ("beat: no RESULT within %d ms of the contact (owner %s, slot %d) - this swing runs the old rules, beat mode off for %d ms",
                 (int) (g_bq.expect_by - g_bq.sw_t0),
                 g_bq.owner_self ? "us" : "them", g_bq.partner, BQ_FB_MS);
            g_bq.fb_until = now + BQ_FB_MS;
            g_bq.sw_pending = 0;
            g_bq.nq = 0;
            g_bq.rs[0].used = 0;
            return 0;
        }
        return 1;
    }
    if (g_bq.cr == BQC_PAIR)
        return 1;               // bq_tick hands the pair start to melee_duel_parry.c at PLAY_AT
    if (g_bq.cr == BQC_BEATEN)
        return 0;
    return now < g_bq.go_at;
}

// mdu_contact: the ruling on this swing (0 = none: the old rules), consumed
static int BqTakeRuling(void)
{
    if (!g_bq.sw_pending || g_bq.sw_t0 != g_mdu.sw_t0)
        return 0;
    int cr = g_bq.cr;
    BqWuEnd(GetTickCount64(), "the contact plays");
    g_bq.sw_pending = 0;
    g_bq.cr = BQC_NONE;
    g_bq.rs[0].used = 0;
    g_bq.vf_slot = -1;          // (we are the attacker now: no victim veto window of an older result)
    if (cr != BQC_HIT) {
        g_bq.shift_until = 0;
        g_bq.shift_slot = -1;
        g_bq.nf_slot = -1;
    }
    if (cr == BQC_HIT) {
        g_bq.shift_until = GetTickCount64() + 600;
        BQLOG
            ("beat: play HIT now (PLAY_AT): the -hit pair starts with the shared clock, damage by the normal hit path");
    } else if (cr == BQC_DODGED)
        BQLOG("beat: play DODGED now (PLAY_AT): our swing is a miss");
    else if (cr == BQC_BEATEN)
        BQLOG
            ("beat: our swing is consumed (parry / clash / lost trade): no blow");
    return cr == BQC_HIT ? 1 : cr == BQC_DODGED ? 2 : cr ==
        BQC_BEATEN ? 3 : 0;
}

// paired.c psync_start: the pair starts at PLAY_AT on the victim (the shared-clock shift, once per armed result, only for that victim slot)
static int BqShiftNow(int vic_slot)
{
    if (!g_bq.valid || !g_bq.shift_until)
        return 0;
    if (GetTickCount64() > g_bq.shift_until) {
        g_bq.shift_until = 0;
        g_bq.shift_slot = -1;
        return 0;
    }
    if (vic_slot != g_bq.shift_slot)
        return 0;
    g_bq.shift_until = 0;
    g_bq.shift_slot = -1;
    return 1;
}

static int BqHitForced(uint32_t slot)
{
    return g_bq.valid && g_bq.vf_slot == (int) slot
        && GetTickCount64() < g_bq.vf_until;
}

static int BqNotImmune(uint32_t slot)
{
    return g_bq.valid && g_bq.nf_slot == (int) slot
        && GetTickCount64() < g_bq.nf_until;
}

// MSG_MEX ops 15..17 (t2r-ext.c dispatch)
static void BqRecv(uint32_t slot, const uint8_t *buf, uint64_t len)
{
    if (slot >= T2R_MAX_PEERS || !buf || len < 4 || BqOff())
        return;
    int op = buf[1];
    uint64_t now = GetTickCount64();
    uint16_t uid = (uint16_t) (buf[2] | (buf[3] << 8));
    if (op == BQO_RESULT && len >= 23) {
        int32_t pa, sa, so, od = BQ_NONE;
        memcpy(&pa, buf + 11, 4);
        memcpy(&sa, buf + 15, 4);
        memcpy(&so, buf + 19, 4);
        if (len >= 27)
            memcpy(&od, buf + 23, 4);
        int outcome = buf[5], a = buf[6], o = buf[7], me =
            (int) PsyncSelf();
        if (outcome < BQR_HIT || outcome > BQR_TRADE || a >= T2R_MAX_PEERS
            || o >= T2R_MAX_PEERS || a == o)
            return;             // (bounds: every slot a real slot)
        if (!
            (g_bq.valid && g_bq.partner == (int) slot && !g_bq.owner_self
             && g_bq.active) || (a != me && o != me)) {
            BQLOG
                ("beat: RESULT %s slot %d -> slot %d from slot %u (bystander / not our duel: the pair / miss clips come as before)",
                 BQR_NAME[outcome], a, o, slot);
            return;
        }
        if (uid != g_bq.uid) {
            BQLOG("beat: RESULT with duel uid u%04x ignored (ours u%04x)",
                  uid, g_bq.uid);
            return;
        }
        for (int i = 0; i < 8; i++)
            if (g_bq.rid_seen[i] == (int16_t) (buf[4] + 1))
                return;         // a duplicate copy
        g_bq.rid_seen[g_bq.rid_i++ & 7] = (int16_t) (buf[4] + 1);
        int in_ms;
        if (od != BQ_NONE) {    // PLAY_AT by the owner's own clock difference; T0 follows the owner (smoothed)
            in_ms =
                (int) ((int64_t) pa - (int64_t) od - BqOw((int) slot));
            int64_t Est =
                (int64_t) now - BqOw((int) slot) - (int64_t) od, d =
                Est - (int64_t) g_bq.t0;
            g_bq.t0 =
                (uint64_t) ((int64_t) g_bq.t0 +
                            BqClampi((int) (d / 4), -30, 30));
        } else
            in_ms = (int) ((int64_t) g_bq.t0 + pa - (int64_t) now);
        if (len >= 37 && (outcome == BQR_HIT || outcome == BQR_TRADE) && !SmoothOff(2)) {      // the owner's agreed align point (a 23 / 27-byte RESULT carries none)
            float ax, az;
            uint16_t yq;
            memcpy(&ax, buf + 27, 4);
            memcpy(&az, buf + 31, 4);
            memcpy(&yq, buf + 35, 2);
            if (__builtin_isfinite(ax) && __builtin_isfinite(az)
                && ax > -1e5f && ax < 1e5f && az > -1e5f && az < 1e5f)
                BqAlignArm(a, o, ax, az,
                             (float) yq * (6.2831853f / 65536.0f),
                             now + (uint64_t) (in_ms >
                                               0 ? in_ms : 0) + 600);
        }
        BqApply(outcome, a, o, buf[8], buf[9] == 255 ? -1 : buf[9],
                 buf[10], in_ms, sa, so, now);
    } else if (op == BQO_INTENT && len >= 14) {
        if (!(g_bq.valid && g_bq.owner_self && g_bq.partner == (int) slot))
            return;
        if (uid != g_bq.uid)
            return;
        int32_t st;
        memcpy(&st, buf + 5, 4);
        int tce = buf[11] | (buf[12] << 8), kind = buf[4];
        int64_t age = (int64_t) BqDt(now) - st;
        if (kind < BQK_ATTACK || kind > BQK_WITHDRAW || age <= -1000
            || age >= 2000 || tce < 0 || tce > 3000 || (kind == BQK_ATTACK
                                                        && buf[9] >=
                                                        MDU_MOVES_N)) {
            BQLOG
                ("beat: intent kind %d from slot %u rejected (stamp %d is %lld ms old, tce %d, row %d)",
                 kind, slot, st, (long long) age, tce, buf[9]);
            return;
        }
        if (!g_bq.active) {
            g_bq.active = 1;
            BQLOG("beat: first INTENT from slot %u - beat mode on", slot);
        }
        BQLOG
            ("beat: intent %s from slot %u stamp %d (arrived %lld ms later), row %d tce %d",
             kind == BQK_ATTACK ? "ATTACK" : kind ==
             BQK_DODGE ? "DODGE" : "WITHDRAW", slot, st, (long long) age,
             buf[9], tce);
        BqQAdd((int) slot, kind, st, buf[9], buf[10], tce, buf[13]);
        g_bq.last_act = now;
    } else if (op == BQO_CAP && len >= 5) {
        if (!g_bq.valid || g_bq.partner != (int) slot)
            return;
        if (g_bq.owner_self && (buf[4] & 1) && !g_bq.active) {
            g_bq.active = 1;
            BQLOG
                ("beat: CAP from slot %u - both duelists run the beat queue, beat mode on (we own)",
                 slot);
        } else if (!g_bq.owner_self && (buf[4] & 2)) {
            g_bq.uid = uid;
            if (!g_bq.active) {
                g_bq.t0 = now - (uint64_t) BqOw((int) slot);
                g_bq.active = 1;
                BqCapSend((int) slot, 1);
                BQLOG
                    ("beat: owner slot %u announced itself (CAP) - beat mode on, CAP sent",
                     slot);
            }
        }
    }
}
