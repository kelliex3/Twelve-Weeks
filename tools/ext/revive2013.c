// revive2013.c - the 2013 Factions revive as a psync paired move (kind PS_REVIVE), included after paired.c (re/revive.md).
//
// What 2013 does (TLOU1 PS3 data, bake_revive.py): the reviver kneels down next to the downed player (multi-heal-ally-player1, the pair whose
// align / apReference channels give the authored distance + facing of the two bodies), works on them while the button is held (the give
// loop) and stands up with them at the end (player1^idle); the revived gets onto its knees (co-op-stunned-enter), waits kneeling
// (co-op-stunned-loop, the revive actor's own clip) and stands up (co-op-stunned-revive).
// Here: ENTER -> LOOP -> FINISH is one psync chain (paired.c psync_rv_chain), both halves on every client around one apReference
// (the revived's body, yawed towards the reviver). The reviver's client starts it when the hold passes its first frames and ends it:
//   - the hold is released / the reviver is downed or gone / the revived is gone or dies:  psync_abort(notify) = both halves blend out
//     (the revived's downed loops, the reviver's locomotion take over again, nothing is applied),
//   - the revive completes (progress >= 1): MSG_REVIVE on every client switches the chain to FINISH (the 2013 stand-up of both) and the
//     revived client's own revive logic (revived_recv: hp, stand, award) runs as before; the move ends with its longer half.
// All game logic (who can revive, the 5 s timer and its skills, multi-reviver rates, MSG_REVIVE, awards, hp after revive, downed state) is
// unchanged - this replaces only the old gesture hover (frozen set-animation-speed 0 pickup arm + revive_face teleports + hard-landing get-up).
// A second reviver of the same body, a refused / unloaded move, or t2r-old-revive.txt next to the exe = the old gesture hand-out.

static int GxJointPos(uint64_t h, const char *joint, float *out);
static int g_rvOff = -1;
static int RvOff(void)
{
    if (g_rvOff < 0) {
        FILE *f = ExeSideOpen("t2r-old-revive.txt", "r");
        g_rvOff = f != NULL;
        if (f)
            fclose(f);
        Xlog("revive 2013: psync pair %s",
             g_rvOff ?
             "OFF (t2r-old-revive.txt): the old gesture hand-out + hard-landing get-up"
             : "on (ENTER -> LOOP -> FINISH, bake_revive.py clips)");
    }
    return g_rvOff;
}

static struct {
    int vic;                    // our own body is the revived of the running move (set by rv_victim_begin)
    int unarm;                  // the reviver put its weapon away for the move (g_unarm_exec, drawn again at the end)
    uint64_t t0;                // GetTickCount64 at the start (the "nobody attached" guard)
    uint32_t uid;
} g_rvp;
static int RvMoveActive(void)
{
    return g_ps.role && g_ps.kind == PS_REVIVE;
}

static int RvMoveMine(void)
{
    return g_ps.role == 1 && g_ps.kind == PS_REVIVE;
}                               // we are the reviver of the running move

static int RvVictimMove(void)
{
    return g_ps.role == 2 && g_ps.kind == PS_REVIVE;
}                               // our own body is the revived of the running move

static int RvPupSlot(uint32_t sl)
{                               // that slot's puppet is the revived of the running move (a half plays on it here)
    return sl < T2R_MAX_PEERS && g_ps.role && g_ps.kind == PS_REVIVE
        && g_ps.vic == (int) sl && PsyncBusySlot(sl);
}

static int RvReviverSlot(uint32_t sl)
{
    return PsyncReviverPup((int) sl) && PsyncBusySlot(sl);
}                               // (paired.c: the reviver half of the running move is a hittable standing body)

// the segments, the same table on every client: [0] ENTER, [1] LOOP, [2] FINISH; the carrier names come from the build (revive_carriers.h)
static int RvChainOf(uint64_t *att, uint64_t *vic, float *spd,
                       float *cut, int max)
{
    static const int ORDER[3] = { 2, 0, 1 };    // REVIVE_CARRIER rows: revive-enter, revive-loops, revive-finish
    if (max < 3)
        return 0;
    for (int i = 0; i < 3; i++) {
        const char *a = REVIVE_CARRIER[ORDER[i]][0], *v =
            REVIVE_CARRIER[ORDER[i]][1];
        if (!a || !*a || !v || !*v)
            return 0;
        att[i] = FNV64(a);
        vic[i] = FNV64(v);
        spd[i] = 1.0f;
        cut[i] = 0.0f;
    }
    return 3;
}

// the reviver's weapon: away for the move (as the unarmed executions: the kneeling clip's hands are empty hands), drawn again after it
static void RvUnarmBegin(void)
{
    uint64_t cur = CurrentWeapon();
    if (g_unarmExec || !cur || cur == ~0ULL
        || cur == FNV64("abby-unarmed"))
        return;
    uint64_t z[2] = { 0, 0 };
    g_unarmExec = 1;
    g_unarm_weapon = cur;
    g_unarm_until = g_t60 + S60(40.0f);
    uint64_t r = (g_holsterInstFrame =
                  g_frame ? g_frame : 1,
                  DcCall(FNV64("holster-player-weapon-instantly"), 2, z));
    FemUnarmEmptyHands(cur, "revive 2013");
    g_rvp.unarm = 1;
    Xlog("revive 2013: weapon %llx put away for the move (holster-player-weapon-instantly -> %llx), back at its end", (unsigned long long) cur, (unsigned long long) r);
}

static void RvUnarmEnd(void)
{
    if (!g_rvp.unarm)
        return;
    g_rvp.unarm = 0;
    if (g_unarmExec && g_unarm_until > g_t60 + 1)
        g_unarm_until = g_t60 + 1;      // gunexec_tick draws it again next frame
}

// reviver: the hold passed its first frames - start the move around the revived's body. 1 = the move plays (no gesture needed)
static int RvPsStart(uint32_t vic)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    {                           // Reviving while prone showed the crouch anim for a moment: the prone flag is read FIRST. It used to be read after the early-outs below, so a
        // refused pair (old-revive file, another psync running ...) left g_rv_prone at 0 and the per-frame player-force-crouch of the hand-out crouched a prone reviver.
        uint64_t none = 0, pr =
            DcCall(FNV64("player-is-prone?"), 0, &none);
        g_rvProne = pr != ~0ULL && (pr & 0xff);
    }
    if (RvOff() || PsyncActive() || vic >= T2R_MAX_PEERS || g_downed
        || g_execUntil || g_downDying)
        return 0;
    {                           // A prone reviver stays prone. The crouched 2013 pair (full-body kneel clip + stance snap) is not played; the old arm-only hand-out gesture
        // (the SP pickup reach, a partial right-arm clip over whatever the body does, no forced crouch) plays on the reviver's own body and, through the
        // same MSG_UC4_REACH (byte 3 = 0), on its puppet on every other screen. Timing / ring / MSG_REVIVE are the unchanged revive logic.
        if (g_rvProne) {
            Xlog("revive 2013: prone reviver - prone variant (stays prone, arm-only reach gesture %s, no crouch pair, no stance change; slot %u)", RG_GESTURE[0], vic);
            return 0;
        }
    }
    for (int i = 0; i < 3; i++)
        for (int h = 0; h < 2; h++) {
            const char *c = REVIVE_CARRIER[i][h];
            if (!c || !*c || AnimLen(SID_PLAYER, c) <= 0.0f) {
                static int nl;
                if (LOGCAP(nl, 4))
                    Xlog("revive 2013: clip %s (pair %d half %d) is not loaded - the old gesture hand-out", c ? c : "(none)", i, h);
                return 0;
            }
        }
    uint8_t *peer = ((fn_peer) T2rPeer) (vic);
    uint64_t h = peer ? *(uint64_t *) (peer + 0x60) : 0;
    float pb[16], mb[16];
    const float *pl = h ? PuppetLocator(h, pb) : NULL, *ml =
        PlayerLocator(mb);
    if (!pl || !ml || !PsLocOk(pl) || !PsLocOk(ml))
        return 0;
    float vx = pl[0], vz = pl[2];
    const float *np = peer ? (const float *) (peer + 0x68) : NULL;      // their net position: where they really are (the lying puppet's locator can lag)
    if (np && !IsBadReadPtr(np, 12) && np[0] == np[0] && np[2] == np[2]
        && __builtin_fabsf(np[0]) < 1e5f && __builtin_fabsf(np[2]) < 1e5f
        && (np[0] - vx) * (np[0] - vx) + (np[2] - vz) * (np[2] - vz) <
        1.5f * 1.5f) {
        vx = np[0];
        vz = np[2];
    }
    float dx = ml[0] - vx, dz = ml[2] - vz;
    // (a reviver slingshot across the map): the pair puts BOTH bodies on this anchor - the revived's own client teleports its body
    // to it. The reviver holds E only within 2.12 m of the downed player, so an anchor more than 3 m from the reviver is a stale puppet locator (a body that lags / was
    // just respawned or slid), never where the player really is: the pair is refused and the old arm-only gesture plays instead (no teleport of anyone).
    if (dx * dx + dz * dz > 3.0f * 3.0f) {
        Xlog("revive 2013: anchor of slot %u is %.1f m from the reviver (the puppet's locator is stale, net position %s) - no pair, the old gesture hand-out", vic, __builtin_sqrtf(dx * dx + dz * dz), np ? "read" : "unread");
        return 0;
    }
    if (dx * dx + dz * dz < 0.2f * 0.2f) {      // standing on them: the way we face
        const float *q = ml + 4;
        dx = 2.0f * (q[0] * q[2] + q[3] * q[1]);
        dz = 1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]);
    }
    float yaw = __builtin_atan2f(dx, dz);       // the ap's +z = from the revived towards us: the revived faces its reviver, the reviver kneels in front of it
    float ap[7] =
        { vx, pl[1], vz, 0.0f, __builtin_sinf(yaw * 0.5f), 0.0f,
  __builtin_cosf(yaw * 0.5f) };
    if (!PsyncStart
        (PS_REVIVE, vic, REVIVE_CARRIER[2][0], REVIVE_CARRIER[2][1], ap,
         -1.0f, 0, 0xff, 0, 0)) {
        Xlog("revive 2013: psync refused the move on slot %u (see above) - the old gesture hand-out", vic);
        return 0;
    }
    RvPsStarted();
    RvUnarmBegin();
    Xlog("revive 2013: ENTER -> LOOP -> FINISH started on slot %u (ap %.2f %.2f %.2f, reviver %.2f m away, yaw %.0f)", vic, vx, pl[1], vz, __builtin_sqrtf(dx * dx + dz * dz), yaw * 57.29578f);
    return 1;
}

// the hold ended (released / out of range / we went down ...): both halves blend out, nothing is applied
static void RvPsCancel(const char *why)
{
    if (RvMoveMine() && !g_ps.rv_fin)
        PsyncAbort(why, 1);
}

// the revive completed on our side: the chain goes to its FINISH segment (the others get the same from MSG_REVIVE). 1 = a move of ours handles the stand-up
static int RvPsComplete(uint32_t who)
{
    if (!RvMoveMine() || g_ps.vic != (int) who)
        return 0;
    if (!g_ps.rv_fin) {
        g_ps.rv_fin = 1;
        g_ps.rv_fin_ms = PsyncMs();
        Xlog("revive 2013: revive of slot %u complete - FINISH (both stand up)", who);
    }
    return 1;
}

// MSG_REVIVE on this client (every role): the revive of `target` is booked - its move goes to FINISH
static void RvPsFinishRemote(uint32_t target)
{
    if (!RvMoveActive() || g_ps.vic != (int) target || g_ps.rv_fin)
        return;
    g_ps.rv_fin = 1;
    g_ps.rv_fin_ms = PsyncMs();
    Xlog("revive 2013: MSG_REVIVE for slot %u - FINISH (role %d)", target,
         g_ps.role);
}

// The loop clip (3.6 s, one give and back) after the 1.47 s enter ends 67 ms after a 5.0 s revive's end. The reviver's own screen finishes at ~4.99 s, but a
// screen that only WATCHES (role 2 = the revived, role 3 = a bystander) gets MSG_REVIVE 100-300 ms later than its move clock says, so the loop had run out and was
// restarted ("loop restart at 5067 ms") just before the FINISH (logs: every five-second revive seen on a puppet): the reviver's give played a second time
// for a moment (the reviver played their animation twice). When the revive is due within RV_TAIL_MS by the reviver's own announced
// start + time (MSG_UC4_REACH, g_rvt) the watching screen goes on to the FINISH segment now (both stand up, as the booked revive does a moment later) instead of
// restarting the loop; a revive that really runs on (long hold) still restarts it. The reviver's own screen (role 1) knows the exact moment and is unchanged.
#define RV_TAIL_MS 800.0
static int RvLoopTail(void)
{
    if (g_ps.kind != PS_REVIVE || (g_ps.role != 2 && g_ps.role != 3))
        return 0;
    if (g_ps.att < 0 || g_ps.att >= T2R_MAX_PEERS || g_ps.vic < 0
        || g_ps.vic >= T2R_MAX_PEERS)
        return 0;
    uint64_t ms = g_rvt[g_ps.vic][g_ps.att].ms;
    float secs = g_rvt[g_ps.vic][g_ps.att].secs;
    if (!ms || secs < 0.2f)
        return 0;
    double left =
        (double) ms + (double) secs * 1000.0 - (double) GetTickCount64();
    if (left > RV_TAIL_MS)
        return 0;
    Xlog("revive 2013: loop ran out %.0f ms before the revive's due time (slot %d on slot %d, %.1f s) - FINISH instead of a second give", left, g_ps.att, g_ps.vic, secs);
    return 1;
}

static void RvPsStarted(void)
{
    uint32_t me = PsyncSelf();
    g_rvp.uid = g_ps.uid;
    g_rvp.t0 = GetTickCount64();
    if (g_ps.vic >= 0 && g_ps.vic < T2R_MAX_PEERS && g_ps.vic != (int) me) {    // the revived is a puppet here: no downed loops / get-up clip over its half
        int s = g_ps.vic;
        if (g_uc4_pup[s].cur)
            Uc4Stop(&g_uc4_pup[s]);
        g_uc4_pup[s].ent = 0;   // (the loops start from scratch if the move is cancelled)
        g_pupPreDone[s] = 1;  // "the get-up already played" for the down's end that follows a completed revive
    }
    Xlog("revive 2013: move u%08x plays here (role %d, reviver slot %d, revived slot %d)", g_ps.uid, g_ps.role, g_ps.att, g_ps.vic);
}

// the revived's own client (psync_begin_remote): accept the move? Same rules as an execution's victim: lying downed, alive, nothing else running
static int RvVictimBegin(uint32_t slot, uint64_t skipf)
{
    if (RvOff() || slot >= T2R_MAX_PEERS || !g_downed || g_downDying
        || g_execUntil || g_uc4DyingUntil || g_downAt
        || LocalHp() <= 0.0f || MatchSubstate() != 6
        || (g_specialExecUntil && g_t60 < g_specialExecUntil)) {
        Xlog("revive 2013: the move of slot %u on us is refused (off %d, downed %d, dying %d, exec %d, hp %.1f) - it plays the old gesture", slot, RvOff(), g_downed, g_downDying, g_execUntil != 0, LocalHp());
        return 0;
    }
    g_rvp.vic = 1;
    Uc4OvOffExec();          // our own UC4 lying overlay / loops off while the half plays (the execution's victim side, gx_victim_begin)
    if (g_uc4_me.cur)
        Uc4Stop(&g_uc4_me);
    if (g_uc4_pup[slot].cur)
        Uc4Stop(&g_uc4_pup[slot]);
    Xlog("revive 2013: slot %u revives us - our body plays the revived half (skip %llu ticks); bleed-out paused by its revive message", slot, (unsigned long long) skipf);
    return 1;
}

static void RvVictimCancel(const char *why)
{
    if (!g_rvp.vic)
        return;
    g_rvp.vic = 0;
    Xlog("revive 2013: our side of the move cancelled (%s)", why);
}

// the other player aborted it (psync_abort already cleared g_ps). The reviver whose revived refused (it is busy / dying / has no clips): the old gesture hand-out
static void RvAborted(int role, uint32_t from)
{
    if (role != 1)
        return;
    Xlog("revive 2013: aborted by slot %u", from);
    if (g_uc4ReachOn && !RvOff() && from < T2R_MAX_PEERS
        && g_peerDowned[from] == 1) {
        ReachOwn(1);
        float rs = REVIVE_SECONDS * 10.0f;
        uint8_t b[4] =
            { (uint8_t) from, 1, (uint8_t) (rs > 255 ? 255 : rs), 0 };
        if (g_sendTramp)
            g_sendTramp(MSG_UC4_REACH, b, 4, 0xff);
        Xlog("revive 2013: the old gesture hand-out for slot %u instead",
             from);
    }
}

// psync_finish: the move is over here. cancelled = it was aborted (blended out), else it ran to its end
static void RvPsEnded(const char *why, int cancelled)
{
    uint32_t me = PsyncSelf();
    if (g_ps.vic >= 0 && g_ps.vic < T2R_MAX_PEERS && g_ps.vic != (int) me
        && cancelled)
        g_pupPreDone[g_ps.vic] = 0;   // the revived is still down: its loops resume
    if (g_ps.role == 2 && g_rvp.vic) {
        g_rvp.vic = 0;
        if (cancelled && g_downed) {
            g_uc4_me.cur = NULL;
            g_uc4_me.busy_until = 0;
        }                       // back to the lying loops
    }
    if (g_ps.role == 1)
        RvUnarmEnd();
    Xlog("revive 2013: move u%08x over (%s, %s) - role %d, chain segment %d of %d%s", g_ps.uid, why, cancelled ? "cancelled" : "ran to its end", g_ps.role, g_ps.cur_seg + 1, g_ps.nseg, g_ps.rv_fin ? ", completed" : "");
}

// every frame (feature_downed): the revived's lock, the end conditions and the safety nets for lost messages
// raw input of the local player (the revive disables the game's own commands, so they cannot be asked): movement keys, mouse buttons, sprint /
// jump / crouch keys, or a gamepad stick past its dead zone / any face or shoulder button / trigger
static int RvRawInput(void)
{
    if (!GameFocused())
        return 0;
    static const int K[] =
        { 'W', 'A', 'S', 'D', VK_LBUTTON, VK_RBUTTON, VK_SPACE, VK_LSHIFT,
VK_LCONTROL, 'C' };
    for (size_t i = 0; i < sizeof K / sizeof K[0]; i++)
        if (GetAsyncKeyState(K[i]) & 0x8000)
            return 1;
    typedef DWORD(WINAPI * fn_xs) (DWORD, void *);
    static fn_xs xs;
    static int tried;
    if (!tried) {
        tried = 1;
        HMODULE m = LoadLibraryA("xinput1_4.dll");
        if (m)
            xs = (fn_xs) GetProcAddress(m, "XInputGetState");
    }
    if (xs) {
        struct {
            DWORD pk;
            WORD b;
            BYTE lt, rt;
            SHORT lx, ly, rx, ry;
        } st;
        for (DWORD u = 0; u < 4; u++)
            if (xs(u, &st) == 0
                && ((st.b & 0xf300) || st.lt > 60 || st.rt > 60
                    || st.lx > 12000 || st.lx < -12000 || st.ly > 12000
                    || st.ly < -12000))
                return 1;
    }
    return 0;
}

// ---- revive sound: the First-Aid Training heal foley (skills.c hl_blob, crafting-health-kit-fast) plays 3D on the revived's body for as long as a
// revive of it runs, audible to everyone (both teams; 2013 range curve via pcm_play_at: full within 3 m, silent at 30 m). Every client derives it locally from
// the revive table g_rvt (MSG_UC4_REACH is broadcast, so all screens know who revives whom) - no extra message. One 1 s chunk per 950 ms, like the heal; each
// chunk is its own sample pointer per target (hl_blob + 2 * (1 + slot)), so the end cuts exactly those voices (40 ms fade) and never a FAT heal chunk.
// Ends at once when: the hold stops (table entry cleared), the reviver is downed / dead / gone, the victim is up / dead / executed / gone, the revive completes.
// t2r-no-revive-sound.txt next to the exe = off. Log prefix "revive sound:".
static uint64_t g_rvsndEh[T2R_MAX_PEERS];      // the engine sound handle of each slot's last heal one-shot (esfx_play), 0 = PCM chunk / none
static uint64_t g_rvsndEh2[T2R_MAX_PEERS];     // the layered second copy (louder; no gain input on play-sound-at-joint - two in phase = +6 dB)
static void RvsndCut(uint32_t t)
{
    if (g_rvsndEh[t]) {
        EsfxStop(g_rvsndEh[t]);
        g_rvsndEh[t] = 0;
    }
    if (g_rvsndEh2[t]) {
        EsfxStop(g_rvsndEh2[t]);
        g_rvsndEh2[t] = 0;
    }
    if (g_mixState != 1)
        return;
    const int16_t *src = hl_blob + 2 * (1 + t);
    EnterCriticalSection(&g_mixCs);
    for (int v = 0; v < MIX_VOICES; v++)
        if (g_mixv[v].on && g_mixv[v].src == src && !g_mixv[v].fadeN)
            g_mixv[v].fadeLeft = g_mixv[v].fadeN = 40 * MIX_RATE / 1000;
    LeaveCriticalSection(&g_mixCs);
}

static int RvsndValid(uint32_t t, uint64_t now)
{                               // a revive of slot t is really running (as far as this screen can tell)
    typedef uint8_t *(*fn_peer)(uint32_t);
    uint32_t me = SkMe();
    int any = 0;
    if (t == me) {
        if (!g_downed || g_downDying || g_uc4DyingUntil || g_execUntil
            || LocalHp() <= 0.0f)
            return 0;
    } else {
        uint8_t *pv = ((fn_peer) T2rPeer) (t);
        if (!pv || !pv[4] || pv[5] || g_peerDowned[t] != 1
            || *(uint64_t *) (pv + 0x60) == 0)
            return 0;
        if (PsyncBusySlot(t) && g_ps.kind != PS_REVIVE)
            return 0;           // an execution / finisher on it
    }
    if (g_ps.kind == PS_REVIVE && g_ps.role && g_ps.vic == (int) t
        && g_ps.rv_fin)
        return 0;               // booked: both stand up, the work is done
    if (RvProg(t) >= 1.0f)
        return 0;
    for (uint32_t r = 0; r < T2R_MAX_PEERS; r++) {
        if (!g_rvt[t][r].ms || r == t)
            continue;
        float secs = g_rvt[t][r].secs > 0.2f ? g_rvt[t][r].secs : 5.0f;
        if (now - g_rvt[t][r].ms > (uint64_t) ((secs + 1.5f) * 1000.0f))
            continue;           // overdue: a lost "stop"
        if (r == me) {
            if (!g_uc4ReachOn || g_downed || LocalHp() <= 0.0f)
                continue;
        } else {
            uint8_t *pr = ((fn_peer) T2rPeer) (r);
            if (!pr || !pr[4] || pr[5] || g_peerDowned[r])
                continue;
        }
        any = 1;
        break;
    }
    return any;
}

static void RvsndTick(void)
{
    static uint64_t next[T2R_MAX_PEERS];
    static uint8_t on[T2R_MAX_PEERS];
    static uint64_t fat_at;
    static int off = -1, nl;
    uint64_t now = GetTickCount64();
    if (off < 0 || now - fat_at > 3000) {
        FILE *f = ExeSideOpen("t2r-no-revive-sound.txt", "r");
        int o = f != NULL;
        if (f)
            fclose(f);
        if (o != off && o)
            Xlog("revive sound: off (t2r-no-revive-sound.txt)");
        off = o;
        fat_at = now;
    }
    uint32_t me = SkMe();
    for (uint32_t t = 0; t < T2R_MAX_PEERS; t++) {
        int want = !off && RvsndValid(t, now);
        if (!want) {
            if (on[t]) {
                on[t] = 0;
                RvsndCut(t);
                if (LOGCAP(nl, 40))
                    Xlog("revive sound: slot %u - stopped", t);
            }
            continue;
        }
        if (!on[t]) {
            on[t] = 1;
            next[t] = 0;
        }
        if (now < next[t])
            continue;
        uint64_t ent = t == me ? SID_PLAYER : SkPeerBody(t);
        if (!ent)
            continue;
        // the engine's own event (sfx-craft-health-kit-fast = the same TLOU2 wave) on the body: native occlusion / reverb / mix; ~1.4 s one-shot, repeated
        // every 1.3 s; refused or opted out (t2r-no-engine-sfx.txt) = the PCM chunks as before
        if (!EsfxOff()) {
            // the game's real bandaging foley (gbl-hud, global hud level: sfx-bandage-ellie-*-2-wraps, 2.3 / 2.6 s), the two arms alternating
            static unsigned alt;
            int rt = (alt++ & 1) != 0;
            uint64_t eh =
                EsfxPlay(rt ? "sfx-bandage-ellie-rt-forearm-2-wraps" :
                          "sfx-bandage-ellie-lt-bicep-2-wraps", ent,
                          "spined");
            if (eh)
                g_rvsndEh2[t] =
                    EsfxPlay(rt ? "sfx-bandage-ellie-rt-forearm-2-wraps" :
                              "sfx-bandage-ellie-lt-bicep-2-wraps", ent,
                              "spined");
            if (eh) {
                next[t] = now + (rt ? 2550 : 2250);
                g_rvsndEh[t] = eh;
                if (nl < 40) {
                    nl++;
                    Xlog("revive sound: slot %u being revived - engine event on %s", t, t == me ? "our body" : "its puppet");
                }
                continue;
            }
        }
        next[t] = now + 950;
        int ok =
            PcmPlayAt(hl_blob + 2 * (1 + t),
                        HEAL_SFX_N - 2 * (1 + T2R_MAX_PEERS), 0.28f,
                        HEAL_SFX_RATE, ent);
        if (nl < 40) {
            nl++;
            Xlog("revive sound: slot %u being revived - chunk %s (3D on %s)", t, ok ? "playing" : "dropped / mixer full", t == me ? "our body" : "its puppet");
        }
    }
}

static void RvpTick(void)
{
    RvsndTick();
    if (!RvMoveActive()) {
        g_rvp.vic = 0;
        if (!g_ha.on)
            RvUnarmEnd();
        return;
    }                           // not under a First-Aid / gift hand-out gesture (ha_begin holsters via rv_unarm_begin, ha_stop ends it): this call drew the weapon back one frame into the gesture
    // (rv_unarm_end: a move cleared without psync_finish must not keep the weapon holstered)
    {                           // seam trace: the pelvis height over the locator of both halves for 10 frames after every segment switch (a base-pose frame reads ~0.9-1.0 m, the kneeling pose 0.4-0.5 m)
        static uint32_t seen_key;
        static int left;
        uint32_t key =
            g_ps.uid * 8u + (uint32_t) g_ps.cur_seg +
            (uint32_t) g_ps.rv_loops * 0x9e3779b1u;
        if (key != seen_key) {
            seen_key = key;
            left = 10;
        }
        if (left > 0) {
            left--;
            float pe[2] = { -99.0f, -99.0f };
            for (int k = 0; k < 2; k++) {
                uint64_t ent = k ? g_ps.ent_vic : g_ps.ent_att;
                float lb[16], pj[3];
                const float *l = ent ? PsyncLoc(ent, lb) : NULL;
                if (l && GxJointPos(ent, "pelvis", pj) && pj[1] == pj[1])
                    pe[k] = pj[1] - l[1];
            }
            Xlog("revive seam: u%08x segment %d frame %llu +%.0f ms pelvis over locator: reviver %.2f m, revived %.2f m (role %d)", g_ps.uid, g_ps.cur_seg + 1, (unsigned long long) g_frame, PsyncMs() - g_ps.t0, pe[0], pe[1], g_ps.role);
        }
    }
    uint64_t now = GetTickCount64();
    if (g_ps.role == 2 && g_ps.rv_fin && !g_downed
        && PsyncMs() - g_ps.rv_fin_ms > 200.0 && RvRawInput()) {
        // (after getting revived a player was forced to wait before being able to input anything -
        // people getting up are never blocked) once the revive is booked, any move / aim / fire / button input of the revived ends the stand-up
        // half on its own screen at once (the reviver's half plays on there); control is back the same frame.
        xlog("revive 2013: revived player gave input %.0f ms into the stand-up - stand-up cut, control back", PsyncMs() - g_ps.rv_fin_ms);
        g_standUntil = 0;
        PsyncFinish("revived: player input cut the stand-up", 1);
        return;
    }
    if (g_ps.role == 2) {       // our body is the revived: the camera only for the whole move (also after the revive booked: the stand-up plays)
        uint64_t a2[2] = { FNV64("camera"), FNV64("camera") }, none = 0;
        DcCall(FNV64("joypad-disable-all-commands-except"), 2, a2);
        DcCall(FNV64("disable-interact-one-frame"), 0, &none);
        if (LocalHp() <= 0.0f || g_downDying || g_uc4DyingUntil
            || g_execUntil) {
            PsyncAbort("the revived died / is executed", 1);
            return;
        }
    }
    if (g_ps.role == 1) {
        uint64_t none = 0;
        DcCall(FNV64("disable-interact-one-frame"), 0, &none);
    }                           // no cabinet / pickup under the reviver's move
    if (g_ps.role == 1 && !g_ps.rv_fin) {
        if (!g_uc4ReachOn) {
            PsyncAbort("the hold ended", 1);
            return;
        }
        if (g_downed || LocalHp() <= 0.0f) {
            PsyncAbort("the reviver went down", 1);
            return;
        }
    }
    if (g_ps.role == 2 && !g_ps.rv_fin && !g_downed) {
        PsyncAbort("we are not down any more", 1);
        return;
    }
    {                           // the other bodies (a dead / gone reviver, a revived that died or was booked up elsewhere): every screen ends the move, nothing is applied
        typedef uint8_t *(*fn_peer)(uint32_t);
        uint32_t me = PsyncSelf();
        int sl[2] = { g_ps.att, g_ps.vic };
        for (int k = 0; k < 2; k++) {
            if (sl[k] < 0 || sl[k] >= T2R_MAX_PEERS || sl[k] == (int) me)
                continue;
            uint8_t *pe = ((fn_peer) T2rPeer) ((uint32_t) sl[k]);
            if (!pe || !pe[4] || pe[5]) {
                PsyncAbort(k ? "the revived is gone / dead" :
                            "the reviver is gone / dead", 0);
                return;
            }
        }
        if (!g_ps.rv_fin && g_ps.vic != (int) me && g_ps.vic >= 0
            && g_ps.vic < T2R_MAX_PEERS && g_peerDowned[g_ps.vic] != 1
            && now - g_rvp.t0 > 500) {
            PsyncAbort("the revived is not down any more", 0);
            return;
        }
    }
    // nobody is attached to the revived any more (a lost ABORT / "stop" message): everyone gives up on a revive nobody does
    if (!g_ps.rv_fin && g_ps.cur_seg < 2 && now - g_rvp.t0 > 3000
        && g_ps.att >= 0 && g_ps.att < T2R_MAX_PEERS && g_ps.vic >= 0
        && g_ps.vic < T2R_MAX_PEERS && !g_rvt[g_ps.vic][g_ps.att].ms) {
        PsyncAbort("nobody revives any more", 0);
    }
}
