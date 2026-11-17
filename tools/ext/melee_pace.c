// melee_pace.c - a slower, weightier cadence between the local player's melee swings (melee spamming made dodging useless, so there is a slower, more impactful
// interval between melee attacks). Included after melee_dodge.c in t2r-ext.c. Log prefix "mpace:".
//
// Before: the engine chained swings as fast as the combo window allowed (fist-light-attack-1..n, 0.4-0.5 s between blows, re/melee-gap-1025.md; the only
// pacing was the 0.75 s downed-kick lock, dm_locked, and the 0.9 s lock a DODGED attacker gets, melee_dodge.c atk_lock_until), so a defender's dodge window
// (480 ms, 2.5 s cooldown) was gone before the next swing and a whiff cost nothing.
// Now (every client drives its own player, so it holds for everyone): from the START of a free swing (the same detection the stagger / whiff code use,
// mstagger_tick's g_ms_atk + move) the engine's melee is held off with player-disable-melee every frame (the downed lock's call) until
// MP_FIST_MS / MP_WPN_MS after that start; a swing with no hit event MP_WHIFF_CHECK_MS after its start (g_melee_contact_at) extends it to MP_WHIFF_MS
// ("each swing commits": a miss leaves the real opening); a swing the victim DODGED (mpace_note_dodged from mdg_attacker_missed) extends it past the
// attacker's dodge recovery by MP_DODGE_EXTRA_MS. The swing animation is never touched (nothing stopped, nothing slowed): only the NEXT start is deferred.
// Buffer: a melee press during the lock is remembered (one; spam coalesces). When the lock ends the press is replayed to the engine's own
// joypad-command-active?(melee-attack) read (a 50 ms false first so a held button still makes an edge, then true for 100 ms; ends the moment a swing
// starts) - no SendInput, the real press is just delivered late. Never while downed / dying / in a paired move / execution / finisher / stun / backpack,
// and a combo that is part of a paired move (psync, gx, ctxdown, early finisher) is not paced: those have their own placement.
// Tune: t2r-melee-pace.txt "<fist ms> <weapon ms> <whiff ms> <dodged extra ms>" (default 850 950 1000 300). Off: t2r-no-melee-pace.txt. Replay off (presses in
// the lock are then dropped): t2r-no-melee-pace-replay.txt. All re-read every 2 s.
#define MP_FIST_MS 850          // halfway between our old 700 / 850 and SP's connected cadence ~1035 (re/sp-melee-measure-1026.md)
#define MP_WPN_MS 950
#define MP_WHIFF_MS 1000
#define MP_DODGE_EXTRA_MS 300
#define MP_WHIFF_CHECK_MS 450   // WHIFF_DELAY_MS is 300 (no hit yet); the blow's impact frame is ~0.3-0.4 s in, so a hit is known by then
#define MP_MASK_MS 50
#define MP_FIRE_MS 100
static void MexWhyFree(uint64_t now); // melee_exchange.c: why a free swing did not become an exchange (log)
static struct {
    int cfg[4], off, replay_off, on, buf, hooked, last_atk, swings,
        fire_reads, whiff_done, lastp, path, pad_reads0;
    uint64_t start, until, t60_start, last_move, cfg_at, buf_at, fire_at,
        last_start, vlock_until;
} g_mp = {.cfg =
        { MP_FIST_MS, MP_WPN_MS, MP_WHIFF_MS, MP_DODGE_EXTRA_MS } };
static int MqPaceFree(void);  // melee_qte.c: a QTE exchange runs (the beat clock replaces the pace lock)
static int MexEngageBuffered(void);   // melee_exchange.c: the buffered press becomes an exchange ENGAGE when an enemy stands in front
static void MpaceCfg(void)
{
    uint64_t now = GetTickCount64();
    if (g_mp.cfg_at && now - g_mp.cfg_at < 2000)
        return;
    g_mp.cfg_at = now ? now : 1;
    FILE *f = ExeSideOpen("t2r-no-melee-pace.txt", "r");
    int o = f != NULL;
    if (f)
        fclose(f);
    if (o != g_mp.off || !g_mp.swings) {
        static int first = 1;
        if (o != g_mp.off || first)
            Xlog("mpace: %s",
                 o ? "OFF (t2r-no-melee-pace.txt)" :
                 "on (a free swing blocks the next swing start: fists 0.7 s, weapons 0.85 s, whiff 1.0 s; a press inside it is replayed when it ends)");
        first = 0;
    }
    g_mp.off = o;
    f = ExeSideOpen("t2r-no-melee-pace-replay.txt", "r");
    g_mp.replay_off = f != NULL;
    if (f)
        fclose(f);
    f = ExeSideOpen("t2r-melee-pace.txt", "r");
    if (f) {
        int v[4] =
            { MP_FIST_MS, MP_WPN_MS, MP_WHIFF_MS, MP_DODGE_EXTRA_MS };
        int n = fscanf(f, "%d %d %d %d", &v[0], &v[1], &v[2], &v[3]);
        fclose(f);
        for (int i = 0; i < 4; i++)
            if (v[i] < 0 || v[i] > 3000)
                v[i] = g_mp.cfg[i];
        if (n >= 1 && memcmp(v, g_mp.cfg, sizeof v)) {
            memcpy(g_mp.cfg, v, sizeof v);
            Xlog("mpace: t2r-melee-pace.txt -> fists %d ms, weapons %d ms, whiff %d ms, dodged +%d ms", v[0], v[1], v[2], v[3]);
        }
    }
}

// ---- the replay: wrap the generic handler + typed pointer of joypad-command-active? (the record the world-prompt hook and the dev dodge test also wrap; the generic
// handler returns the pointer to its ret block, so the original runs first and only the low byte of the answer is changed - an earlier crash at exe+161359c was a void thunk)
static fn_dccall g_mpGen;
static uint64_t(*g_mp_typed) (uint64_t, uint64_t, uint64_t, uint64_t,
                              uint64_t, uint64_t, uint64_t, uint64_t,
                              uint64_t);
static volatile uint64_t g_mp_mask_t, g_mp_fire_t;      // GetTickCount64 deadlines: false until mask_t, true until fire_t (0 = none)
static int MpWant(uint64_t sid, int *val)
{                               // 1 = override the answer with *val
    static uint64_t melee;
    if (!melee)
        melee = FNV64("melee-attack");
    if (sid != melee || !g_mp_fire_t)
        return 0;
    uint64_t now = GetTickCount64();
    if (now < g_mp_mask_t) {
        *val = 0;
        return 1;
    }
    if (now < g_mp_fire_t) {
        *val = 1;
        g_mp.fire_reads++;
        return 1;
    }
    return 0;
}

typedef uint64_t *(*mp_gen_t)(uint64_t * ret, int argc, uint64_t * argv,
                              uint64_t sec);
static uint64_t *MpThunkGen(uint64_t *ret, int argc, uint64_t *argv,
                              uint64_t sec)
{
    uint64_t *r = ((mp_gen_t) g_mpGen) (ret, argc, argv, sec);
    int v;
    if (ret && argc >= 1 && argv && !IsBadReadPtr(argv, 8)
        && MpWant(argv[0], &v)) {
        ret[0] = (ret[0] & ~0xffULL) | (uint64_t) v;
        if (!r)
            r = ret;
    }
    return r;
}

static uint64_t MpThunkTyped(uint64_t a, uint64_t b, uint64_t c,
                               uint64_t d, uint64_t e, uint64_t f,
                               uint64_t g, uint64_t h, uint64_t j)
{
    uint64_t r = g_mp_typed(a, b, c, d, e, f, g, h, j);
    int v;
    return MpWant(a, &v) ? (r & ~0xffULL) | (uint64_t) v : r;
}

static void MpaceHook(void)
{
    if (g_mp.hooked || g_mp.replay_off)
        return;
    g_mp.hooked = 1;
    uint8_t *rec =
        (uint8_t *) ((fn_lookup)
                     T2R_SYM(kFnNativeLookup)) (FNV64
                                                ("joypad-command-active?"));
    if (!rec) {
        Xlog("mpace: no joypad-command-active? record - presses inside the lock are dropped (no replay)");
        g_mp.replay_off = 1;
        return;
    }
    fn_dccall gen = *(fn_dccall *) (rec + 8);
    void *typed = *(void **) (rec + 0x10);
    DWORD old;
    int n = 0;
    if (gen && IsCode((void *) gen) && !IsBadWritePtr(rec + 8, 8)) {
        g_mpGen = gen;
        *(mp_gen_t volatile *) (rec + 8) = MpThunkGen;
        n |= 1;
    }
    if (typed && IsCode(typed)
        && VirtualProtect(rec + 0x10, 8, PAGE_READWRITE, &old)) {
        g_mp_typed =
            (uint64_t(*)
             (uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
              uint64_t, uint64_t, uint64_t)) typed;
        *(void *volatile *) (rec + 0x10) = (void *) MpThunkTyped;
        n |= 2;
    }
    Xlog("mpace: replay hook on joypad-command-active?(melee-attack) (generic %s, typed %s)", n & 1 ? "yes" : "no", n & 2 ? "yes" : "no");
}

// the attacker's blow was dodged (mdg_attacker_missed): the lock reaches past its dodge recovery so the dodger gets a real opening
static void MpaceNoteDodged(void)
{
    if (g_mp.off || !g_mp.on)
        return;
    uint64_t now = GetTickCount64(), u =
        now + MD_ATK_RECOVER_MS + (uint64_t) g_mp.cfg[3];
    if (u > g_mp.until) {
        g_mp.until = u;
        Xlog("mpace: swing dodged - next swing start held %d ms from now (dodge recovery %d + %d)", (int) (u - now), MD_ATK_RECOVER_MS, g_mp.cfg[3]);
    }
}

// the victim of our blow is playing its hit reaction (melee_hitreact.c, each landed blow gives the victim a beat): the next swing START waits until the reaction's
// impact part is over (an absolute GetTickCount64 deadline, 0 = released). Set at the confirmed blow (provisional), corrected by the victim's MSG_HITREACT2 (its real cut, or a release).
// Only the swing start is held (same player-disable-melee lock and press buffer as the cadence lock); a swing already running is never touched.
static void MpaceNoteVictim(uint64_t until_abs)
{
    if (g_mp.off)
        return;
    g_mp.vlock_until = until_abs;
}

// replay bookkeeping: a swing starting ends the press at once; no swing within the window = the engine did not take it.
// Two paths (re/pad-inject-1026.md): 1 = the engine-level virtual pad (padinject.c: the bit goes into the Pad before Joypad::Update, a real press edge), 2 = the old
// wrapper on the native joypad-command-active?. The pad path goes first; when it is not taken the wrapper gets one try, and the line says which path took the press.
static const char *MpacePathName(int p)
{
    return p == 1 ? "pad" : "wrapper";
}

static uint64_t MpaceSwingStart(void)
{
    return g_mp.start;
}

static int MpaceLocked(void)
{
    uint64_t now = GetTickCount64();
    return (g_mp.on
            && now < (g_mp.until >
                      g_mp.vlock_until ? g_mp.until : g_mp.vlock_until) +
            300) || g_mp.buf || g_mp.fire_at || (g_mp.vlock_until
                                                 && now <
                                                 g_mp.vlock_until);
}

static void MpaceWrapperPress(uint64_t now)
{
    g_mp_mask_t = now + MP_MASK_MS;
    g_mp_fire_t = now + MP_MASK_MS + MP_FIRE_MS;
    g_mp.fire_at = now;
    g_mp.fire_reads = 0;
    g_mp.path = 2;
}

static void MpaceReplayCheck(uint64_t now)
{
    if (!g_mp.fire_at)
        return;
    unsigned reads =
        g_mp.path ==
        1 ? PadinjReadsTrue("melee-attack") : (unsigned) g_mp.fire_reads;
    if (g_mp.last_start >= g_mp.fire_at) {
        Xlog("mpace: replayed press taken (path %s) - swing started %llu ms after the lock ended (%u engine reads saw the press)", MpacePathName(g_mp.path), (unsigned long long) (g_mp.last_start - g_mp.fire_at), reads);
        g_mp_fire_t = g_mp_mask_t = 0;
        g_mp.fire_at = 0;
        g_mp.path = 0;
    } else if (now > g_mp.fire_at + MP_MASK_MS + MP_FIRE_MS + 150) {
        Xlog("mpace: replayed press NOT taken by the engine (path %s, %u reads saw it, %u pad frames applied, no swing started)", MpacePathName(g_mp.path), reads, PadinjApplied());
        g_mp_fire_t = g_mp_mask_t = 0;
        g_mp.fire_at = 0;
        if (g_mp.path == 1 && !g_mp.replay_off) {
            MpaceWrapperPress(now);
            Xlog("mpace: retrying the press through the native wrapper");
        } else
            g_mp.path = 0;
    }
}

// v5-pinpress: mpace only lifts a melee-attack block it armed itself. It used to clear the one global block on EVERY frame of a paired move / stun, so the
// duel's combo-window block (set once at the contact) and the pin's block were dropped under them (the engine got the presses the duel meant to hold back),
// and the raw bit the duel / pin read their pad presses from was zeroed each frame before they ran (pin follow-ups were dead on a controller).
static int g_mpOwnBlk;
static void MpaceUnblock(void)
{
    if (g_mpOwnBlk) {
        g_mpOwnBlk = 0;
        PadinjBlock("melee-attack", 0);
    }
}

static const char *MduPressLatched(void);     // melee_duel.c (v5-sfx press latch)
static void MpaceTick(void)
{
    MpaceCfg();
    uint64_t now = GetTickCount64();
    if (!g_mp.off && !g_mp.replay_off && MatchSubstate() == 6)
        PadinjInstall();       // the pad path (padinject.c): hooks in once, in a match
    PadinjTick();
    {                           // v5-sfx press latch (melee_duel.c mdu_press_latched): a finisher windup / our finisher, counter or disarm half owns the presses - no buffer, no replay
        //  (A [45235] "lock over after 953 ms - replaying the buffered press" restarted the windup with a new pick)
        const char *lw = MduPressLatched();
        if (lw) {
            if (g_mp.buf)
                Xlog("mpace: buffered press dropped - %s owns the melee presses (press latch)", lw);
            g_mp.buf = 0;
            g_mp.fire_at = 0;
            g_mp.last_atk = g_msAtk;
            g_mp.last_move = g_msCurmove;
            return;
        }
    }
    MpaceReplayCheck(now);
    if (Mv3StandMe("mpace: cadence lock / press buffer / replay")) {  // melee v3: the bubble's sim owns every press (its own chain / buffer windows); the pad block is mv3_anim's
        g_mp.on = 0;
        g_mp.buf = 0;
        g_mp.until = 0;
        g_mp.vlock_until = 0;
        g_mp.fire_at = 0;
        g_mp_fire_t = g_mp_mask_t = 0;
        g_mp.last_atk = g_msAtk;
        g_mp.last_move = g_msCurmove;
        return;
    }
    // Punches sometimes did not play: inside a QTE exchange the beat clock (melee_qte.c) owns every melee press - the cadence lock, its player-disable-melee and its stale-press
    // buffer would swallow a COMBO / STRIKE press silently ("lock over after 860 ms - buffered press dropped (pressed 1985 ms ago)"). Pace is off while an exchange runs; a press that is no beat input
    // is logged by melee_qte.c. Right after the exchange the lock starts clean.
    if (!g_mp.off && MqPaceFree()) {
        if (g_mp.on || g_mp.buf)
            Xlog("mpace: lock dropped - a melee QTE exchange owns the presses now");
        g_mp.on = 0;
        g_mp.buf = 0;
        g_mp.until = 0;
        g_mp.vlock_until = 0;
        g_mp.fire_at = 0;
        g_mp_fire_t = g_mp_mask_t = 0;
        g_mp.last_atk = g_msAtk;
        g_mp.last_move = g_msCurmove;
        return;
    }
    // (live de92fedb: a ~29 fps client's swings came 500-650 ms apart): the brief states (stun, downed kick, early finisher, a finisher on us) used to wipe the lock
    // and the swing history, so the first free frame after them started a swing with no pace at all. Those now only pause it (the deadline is absolute and keeps running);
    // the states that really end a fight (downed, dying, paired move, execution, leaving the match) still reset it.
    int hard = MatchSubstate() != 6 || g_downed || g_downDying
        || PsyncOwn() || g_execUntil || g_xcamOn || GxRunning()
        || g_unarmExec || LocalHp() <= 0.0f;
    int busy = hard || g_kdEarlySlot >= 0 || g_stun_until || FinisherGuardOn() || GxVictimActive() || g_dmKickFrame || (MduEngaged() && GetFileAttributesA("t2r-mpace-in-duel.txt") == INVALID_FILE_ATTRIBUTES);       // an engaged duel paces itself (MDU_NEXT gates); this lock on top held every duel blow 1 s as a "whiff" (the duel contact is no engine hit) - t2r-mpace-in-duel.txt = old
    if (!g_mp.off && busy && !hard) {
        MpaceUnblock();
        static uint64_t npz;
        if (g_mp.on && now - npz > 5000) {
            npz = now;
            Xlog("mpace: lock paused (stun / kick / early finisher) - its deadline keeps running, %lld ms left", (long long) (g_mp.until > now ? g_mp.until - now : 0));
        }
        return;
    }
    if (g_mp.off || busy) {
        if (g_mp.on)
            Xlog("mpace: lock dropped (%s)",
                 g_mp.
                 off ? "off" :
                 "busy: downed / paired move / execution / stun / match over");
        g_mp.vlock_until = 0;
        MpaceUnblock();
        g_mp.on = 0;
        g_mp.buf = 0;
        g_mp_fire_t = g_mp_mask_t = 0;
        g_mp.last_atk = 0;
        g_mp.last_move = 0;
        g_mp.fire_at = 0;
        g_mp.path = 0;
        return;
    }
    // swing start: the same edge mstagger_tick uses (a new swing, or the next move of a combo)
    int atk = g_msAtk;
    if (atk
        && (!g_mp.last_atk
            || (g_msCurmove && g_msCurmove != g_mp.last_move))) {
        uint64_t w = CurrentWeapon();
        int wpn = w && w != ~0ULL && MeleeClassOf(w, NULL) >= 0;
        int ms = g_mp.cfg[wpn ? 1 : 0];
        uint64_t prev = g_mp.last_start;
        g_mp.start = now;
        g_mp.last_start = now;
        g_mp.t60_start = g_t60 ? g_t60 : 1;
        g_mp.whiff_done = 0;
        g_mp.swings++;
        g_mp.until = now + (uint64_t) ms;
        g_mp.on = ms > 0;
        g_mp_fire_t = g_mp_mask_t = 0;
        uint64_t clip = MstagClipOf(g_msCurmove), da[2] =
            { SID_PLAYER, clip }, dr =
            clip ? DcCall(FNV64("get-animation-duration"), 2, da) : ~0ULL;
        float dur = 0;
        uint32_t lo = (uint32_t) dr;
        memcpy(&dur, &lo, 4);
        static int nl;
        if (LOGCAP(nl, 120))
            Xlog("mpace: swing #%d start (%s, move %llx, clip %llx %.2f s), %llu ms since the previous swing start, next swing start held %d ms", g_mp.swings, wpn ? "weapon" : "fists", (unsigned long long) g_msCurmove, (unsigned long long) clip, dur > 0.0f && dur < 10.0f ? dur : 0.0f, (unsigned long long) (prev ? now - prev : 0), ms);
        MexWhyFree(now);
        MpaceHook();
    }
    g_mp.last_atk = atk;
    g_mp.last_move = g_msCurmove;
    if (!g_mp.on) {
        g_mp.lastp = PadCmdOn("melee-attack") || (GameFocused() && (GetAsyncKeyState(0x46) & 0x8000));       // track the button outside the lock too: the press that STARTS a swing is still down on the lock's first frame and was read as a new press, buffering a phantom 2nd swing (A/B: "0 ms into the lock buffered", replayed 1.5 s later into a dodger)
        if (g_mp.vlock_until && now < g_mp.vlock_until) {
            uint64_t none = 0;
            DcCall(0x0fa21391e0e4e478ULL /* player-disable-melee */ , 0,
                    &none);
        }                       // victim-reaction lock with no free swing lock under it
        else
            g_mp.vlock_until = 0;
        return;
    }
    uint64_t eff = g_mp.until > g_mp.vlock_until ? g_mp.until : g_mp.vlock_until;       // the swing cadence lock, or the victim's reaction if that ends later
    // a swing with no hit by now commits to its miss
    if (!g_mp.whiff_done && now - g_mp.start >= MP_WHIFF_CHECK_MS) {
        g_mp.whiff_done = 1;
        if (!(g_meleeContactAt >= g_mp.t60_start)) {
            uint64_t u = g_mp.start + (uint64_t) g_mp.cfg[2];
            if (u > g_mp.until) {
                g_mp.until = u;
                static int nw;
                if (LOGCAP(nw, 60))
                    Xlog("mpace: swing #%d whiffed (no hit %d ms in) - next swing start held to +%d ms", g_mp.swings, MP_WHIFF_CHECK_MS, g_mp.cfg[2]);
            }
        }
    }
    // a melee press inside the lock is remembered (the engine command, or F on the keyboard; never the E-contextual melee-attack-shiv, never with interact down)
    // the engine chained combo punches through the lock from the pad bits - hide melee-attack from the engine while the lock runs (our buffer reads the raw bit)
    int blk = !g_mp.replay_off
        && GetFileAttributesA("t2r-no-melee-block.txt") ==
        INVALID_FILE_ATTRIBUTES && now < eff
        && PadinjBlock("melee-attack", eff);
    if (blk)
        g_mpOwnBlk = 1;
    int pr = PadCmdOn("melee-attack") || (blk && PadinjBlockedRaw())
        || (GameFocused() && (GetAsyncKeyState(0x46) & 0x8000));
    if (pr && InteractDown())
        pr = 0;
    if (pr && !g_mp.lastp && now < eff && now - g_mp.start >= 60 && !g_mp.replay_off && !g_mp.buf) {    // < 60 ms in = the press that started this swing (seen in the same tick as the start)
        g_mp.buf = 1;
        g_mp.buf_at = now;
        static int nb;
        if (LOGCAP(nb, 80))
            Xlog("mpace: melee press %llu ms into the lock buffered (fires in %lld ms)", (unsigned long long) (now - g_mp.start), (long long) (eff - now));
    }
    g_mp.lastp = pr;
    if (now < eff) {
        uint64_t none = 0;
        DcCall(0x0fa21391e0e4e478ULL /* player-disable-melee */ , 0,
                &none);
        return;
    }
    // lock over
    MpaceUnblock();
    uint64_t held = now - g_mp.start;
    if (g_mp.buf && Mv2On() && g_mhs.last_blow > g_mp.buf_at) {
        g_mp.buf = 0;
        Xlog("mpace: buffered press dropped - we were hit after it (melee v2)");
    }
    if (g_mp.buf && now - g_mp.buf_at > (Mv2On()? 1500u : 400u)
        && GetFileAttributesA("t2r-melee-buf-old.txt") ==
        INVALID_FILE_ATTRIBUTES) {
        // A press 734 ms old was replayed as a fresh swing into an enemy who had just dodged - he countered and
        // downed us. A stale press is dropped: only a press in the last 450 ms of the lock is carried over. (log 60052: a stale buffered press fired): 250 ms; later 400 ms (live logs: 1-11 presses per player dropped silently).
        g_mp.buf = 0;
        Xlog("mpace: lock over after %llu ms - buffered press dropped (pressed %llu ms ago, too old to replay)", (unsigned long long) (now - g_mp.start), (unsigned long long) (now - g_mp.buf_at));
    }
    if (g_mp.buf && MexEngageBuffered()) {
        g_mp.buf = 0;
        Xlog("mpace: lock over after %llu ms - the buffered press (pressed %llu ms ago) starts a melee exchange", (unsigned long long) held, (unsigned long long) (now - g_mp.buf_at));
    } else if (g_mp.buf) {
        g_mp.buf = 0;
        if (PadinjPress("melee-attack", MP_FIRE_MS + MP_MASK_MS)) {
            g_mp.fire_at = now;
            g_mp.fire_reads = 0;
            g_mp.path = 1;
            g_mp_mask_t = g_mp_fire_t = 0;
        } else
            MpaceWrapperPress(now);
        Xlog("mpace: lock over after %llu ms - replaying the buffered press (pressed %llu ms ago) through the %s path", (unsigned long long) held, (unsigned long long) (now - g_mp.buf_at), MpacePathName(g_mp.path));
    } else {
        static int nf;
        if (LOGCAP(nf, 60))
            Xlog("mpace: lock over after %llu ms - melee free",
                 (unsigned long long) held);
    }
    g_mp.on = 0;
    g_mp.until = 0;
    g_mp.vlock_until = 0;
}
