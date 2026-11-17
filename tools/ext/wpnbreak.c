// ---------------------------------------------------------------- melee weapon break (looks and sounds like single player)
// SP trace (re/sp-sandbox-runs/sea-1026-weapon-break.log, Ellie, hatchet, machete-slash-3 finisher, last durability point):
//   contact      SND sfx-hatchet-hit-break on the weapon object (sfx-<weapon>-hit-break exists for 16 weapons), MDUR 1 -> 0
//   +0.41 s      gesture ellie-1hblunt-drop-broken-weapon-gesture (+ ellie-1hblunt-drop-broken-weapon) on the player (2h: ellie-2hblunt-...)
//   +0.34 s more EVT drop-broken-weapon (player_event of the clip): the weapon leaves the hand, then the next weapon comes out
// MP before this file: the weapon just ran out (hp 0), nothing played. Now, on our screen: the break sound at the right hand at the break, the
// gesture 0.41 s later (after a paired move / execution that is running - never over it), at the clip's drop frame the held broken item is
// hidden (draw-control visibility byte; NO physics throw: no engine native for a prop drop of the held weapon is known - visual only, logged).
// Other screens: MSG_WPNBREAK (0x9B) -> the same sound (positional) + the gesture on the puppet. The gesture needs its clips loaded (checked by
// get-animation-duration, logged; none loaded = sound only). t2r-no-break-sfx.txt next to the exe = off. Log prefix "wpnbreak:".
#define MSG_WPNBREAK 0x9B       // peer msg (ours): [u8 flags: bit0 break sound, bit1 gesture, bit2 two-handed][u64 weapon sid]
#define WB_GESTURE_DELAY_MS 410 // SP: break sound F182971 -> gesture F183008
#define WB_DROP_MS 340          // SP: gesture start -> player_event drop-broken-weapon
#define WB_DEFER_MAX_MS 4000
#define WB_DROP_FB_MS 1500      // a peer's break sound with no gesture message: its item is hidden this long after (SP: sound -> drop event 0.75-0.83 s)
// Break sfx, then the broken hatchet's next blow 450 ms later - the 410 ms wait before the gesture left the weapon in the hand through a second swing: the gesture
// starts at the break (the sound is the contact; the drop frame follows at +340 ms); a paired move that runs still defers it. t2r-wbreak-delay-old.txt = SP's 410 ms
static int WbGdelay(void)
{
    static int v = -1;
    static uint64_t at;
    uint64_t n = GetTickCount64();
    if (v < 0 || n - at > 2000) {
        at = n;
        v = GetFileAttributesA("t2r-wbreak-delay-old.txt") ==
            INVALID_FILE_ATTRIBUTES ? 0 : WB_GESTURE_DELAY_MS;
    }
    return v;
}

static int WbOff(void)
{
    static int off = -1;
    static uint64_t at;
    return FeelFlag("t2r-no-break-sfx.txt", &at, &off, 180);
}

// weapon item -> its sfx-<name>-hit-break event
static const char *WbSfx(uint64_t w)
{
    static const struct {
        const char *item, *ev;
    } T[] = {
        { "crowbar", "sfx-crowbar-one-hand-hit-break" }, { "lead-pipe",
                                                          "sfx-lead-pipe-hit-break"
                                                          },
            { "short-pipe", "sfx-lead-pipe-hit-break" },
        { "pipe-wrench", "sfx-pipe-wrench-hit-break" }, { "hammer-modern",
                                                         "sfx-hammer-hit-break"
                                                         },
            { "hammer-scar", "sfx-hammer-hit-break" },
        { "club-1h-police", "sfx-club-1h-police-hit-break" },
            { "club-1h-scar", "sfx-club-1h-hit-break" }, { "two-by-four",
                                                          "sfx-2x4-hit-break"
                                                          },
        { "baseball-bat", "sfx-baseball-bat-hit-break" }, { "tomahawk",
                                                           "sfx-hatchet-hit-break"
                                                           },
            { "hatchet-modern", "sfx-hatchet-hit-break" }, { "hatchet",
                                                            "sfx-hatchet-hit-break"
                                                            },
        { "fireman-axe", "sfx-two-hand-axe-hit-break" }, { "machete",
                                                          "sfx-machete-hit-break"
                                                          },
            { "machete-scar", "sfx-machete-hit-break" },
    };
    for (size_t i = 0; i < sizeof T / sizeof *T; i++)
        if (Fnv64Rt(T[i].item) == w)
            return T[i].ev;
    return "sfx-hatchet-hit-break";
}

static struct {
    uint64_t lastw;
    int64_t lasthp;
    uint64_t seen, busy_t60;    // the held non-machete melee: item, durability when last read, frame; last frame a pair / swing ran
    int pend;
    uint64_t w, t0_ms, drop_ms;
    int two;                    // our break: gesture waiting (t0 = break time), drop frame (ms, 0 = gesture not started)
    struct {
        uint64_t ms;
        uint64_t ent;
        uint64_t w;
        int two;
        uint64_t clip, stop_ms;
        uint64_t fb_ms, keep_ms, keep_w;
    } pd[T2R_MAX_PEERS];        // fb_ms: no gesture message came - hide anyway; keep_ms: re-hide until then (v5-wbreak)   // puppets: gesture started at ms -> the drop at +WB_DROP_MS (clip: the full-body fallback, stopped at stop_ms)
    int zero, gtries;
    uint64_t zero_w;            // v5-pinfix3 4: the break was called at the HIT with 1 point held: health 0 at the drop frame
    int lend;
    uint64_t lend_t60, lend_w;  // v5-pinfix3 4: a swing started on the last point - 1 point lent so the engine's contact cannot remove the weapon first
    uint64_t rx_ms[T2R_MAX_PEERS];
    int rx_n[T2R_MAX_PEERS];
} g_wb;
// hide the broken item in a hand: the draw control's visibility byte (as pup_hide_held / the spawn fix use it). w = the weapon process.
static void WbNoteBroken(uint32_t k, uint8_t *w)
{
    if (k < T2R_MAX_PEERS && w && !IsBadReadPtr(w + 0x94, 4)) {
        g_pup_broken[k].w = w;
        g_pup_broken[k].id = *(uint32_t *) (w + 0x94);
    }
}

static int WbHide(uint8_t *w, uint64_t item)
{
    if (!w || IsBadReadPtr(w + 0x80, 0x18) || IsBadReadPtr(w + 0x840, 8)
        || IsBadReadPtr(w + 0x1b0, 8))
        return 0;
    uint64_t sid = BaseWeapon(PstSidOf(w));
    if (!MeleeDurW(sid) || (item && sid != BaseWeapon(item)))
        return 0;               // not the broken melee any more (already gone / swapped)
    uint8_t *dc = *(uint8_t **) (w + 0x1b0);
    if (!dc || IsBadWritePtr(dc + 0x48, 4))
        return 0;
    dc[0x48] = 0x00;
    return 1;
}

// gesture on a body (own SID_PLAYER or a puppet handle): the same call SP's script makes (gesture_(obj, gesture, default play params))
static int WbGesture(uint64_t ent, int two, const char *who)
{
    char g[80], c[80], b[80];
    snprintf(g, sizeof g, "ellie-%shblunt-drop-broken-weapon-gesture",
             two ? "2" : "1");
    snprintf(c, sizeof c, "ellie-%shblunt-drop-broken-weapon",
             two ? "2" : "1");
    snprintf(b, sizeof b, "ellie-%shblunt-drop-broken-weapon--base",
             two ? "2" : "1");
    float lg = MdgClipLen(Fnv64Rt(g)), lc =
        MdgClipLen(Fnv64Rt(c)), lb = MdgClipLen(Fnv64Rt(b));
    static int nl;
    if (!(lg > 0.2f || lc > 0.2f || lb > 0.2f)) {
        if (LOGCAP(nl, 6))
            Xlog("wpnbreak: %s: %s clips not loaded here (gesture %.2f clip %.2f base %.2f s) - sound only", who, two ? "2h" : "1h", lg, lc, lb);
        return 0;
    }
    uint64_t none = 0;
    uint64_t pp = DcCall(FNV64("alloc-gesture-play-params"), 0, &none);
    if (!pp || pp == ~0ULL || IsBadReadPtr((void *) pp, 0x90)) {
        if (LOGCAP(nl, 6))
            Xlog("wpnbreak: %s: alloc-gesture-play-params -> %llx - no gesture", who, (unsigned long long) pp);
        return 0;
    }
    ((uint8_t *) pp)[0x20] = 0;
    ((uint8_t *) pp)[0x21] = 0; // defaults only (rg_play: both set crashed gesture_ in MP)
    uint64_t a[3] = { ent, Fnv64Rt(g), pp };
    uint64_t r = DcCall(FNV64("gesture_"), 3, a);
    static int n2;
    if (LOGCAP(n2, 30))
        Xlog("wpnbreak: %s: gesture %s (clip lens %.2f / %.2f / %.2f s) -> %llx", who, g, lg, lc, lb, (unsigned long long) r);
    // v5-pinfix3 4 (the weapon breaking needs the appropriate animation; log 9317 / 9473: gesture_ on a PUPPET answers 0 every time - the other
    // screen saw no drop): a puppet plays SP's own drop-broken-weapon clip (the gesture's full-body twin, 0.83 s, loaded in MP) through animate_, stopped at its end
    if ((!r || r == ~0ULL) && ent != SID_PLAYER && lc > 0.2f) {
        uint64_t rr = MhrAnimate(ent, Fnv64Rt(c), 0.0f, 0.15f);
        static int n3;
        if (LOGCAP(n3, 30))
            Xlog("wpnbreak: %s: gesture refused - SP clip %s (%.2f s) played on the body instead -> %llx", who, c, lc, (unsigned long long) rr);
        return rr && rr != ~0ULL ? 3 : 0;
    }
    return r && r != ~0ULL ? 1 : 2;
}

// the break at the HIT (SP: sfx-<weapon>-hit-break on the contact frame, the broken weapon stays in the hand through the drop-broken-weapon gesture and leaves it at the
// clip's drop-broken-weapon event): the caller holds 1 durability point (the engine would take the item away at 0 at once); wb_tick sets 0 at the drop frame
static void WbBreak(uint64_t w, const char *why);
static void WbBreakHit(uint64_t w, const char *why)
{
    if (WbOff() || MatchSubstate() != 6 || g_wb.pend)
        return;
    WbBreak(w, why);
    g_wb.zero = 1;
    g_wb.zero_w = w;
    g_wb.gtries = 0;
}

static int WbPending(void)
{
    return g_wb.pend != 0;
}                               // melee_duel.c: presses wait while the gesture / drop is pending

static int WbHold(void)
{
    return g_wb.pend && g_wb.zero;
}                               // melee_duel.c: the break owns the last point now

static void WbZero(const char *why)
{                               // the broken weapon's last point goes (the engine takes the item out of the hand)
    if (!g_wb.zero)
        return;
    g_wb.zero = 0;
    uint64_t cw = CurrentWeapon(), none = 0, h =
        DcCall(SID_GET_WEAPON_HEALTH2, 0, &none);
    if (cw != g_wb.zero_w) {
        Xlog("wpnbreak: %s - %llx is not in the hand any more (held %llx) - health left", why, (unsigned long long) g_wb.zero_w, (unsigned long long) cw);
        return;
    }
    uint64_t z = 0;
    DcCall(SID_SET_WEAPON_HEALTH2, 1, &z);
    g_wb.lastw = 0;             // (no second break from the 0 read)
    Xlog("wpnbreak: %s - weapon %llx health %lld -> 0: the item leaves the hand", why, (unsigned long long) g_wb.zero_w, (long long) (h == ~0ULL ? -1 : (int64_t) h));
}

// v5-wpn2 (the broken half stayed in the hand and melee could not be prompted until it disappeared after spamming 8-10 times; log 73857: "health 1 -> 0: the
// item leaves the hand" and then no swing start for 8.5 s): player-set-weapon-health 0 does NOT take the item away - only the engine's own contact decrement does. A
// 0-point melee item stayed in the hand and the engine refused every melee press with it. SP's drop-broken-weapon event empties the hand and the item is gone: here
// the hand is emptied with our instant holster (the same call the fist takedowns / pin use, so the fists are up at once) and the item leaves the inventory
// (remove-player-weapon, as the disarm victim / the male-knife strip use it, on an item that is not in the hand any more); it is forgotten as the kept / last
// melee (no respawn copy, no execution draw of a removed item). Logged with has-weapon? before / after.
static void NaeBreakLatch(uint64_t w, uint64_t held_now);     // below (ps43 L2)
static void WbTakeOut(uint64_t w, const char *why)
{
    if (!w || w == ~0ULL)
        return;
    uint64_t none = 0, cw = CurrentWeapon(), r0 = ~0ULL;
    int held = cw != ~0ULL && cw && BaseWeapon(cw) == BaseWeapon(w);
    if (held) {
        uint64_t z[2] = { 0, 0 };
        r0 = DcCall(FNV64("holster-player-weapon-instantly"), 2, z);
    }
    uint64_t cw2 = CurrentWeapon(), has =
        DcCall(FNV64("has-weapon?"), 1, &w), r1 = ~0ULL;
    if (has != ~0ULL && (has & 0xff))
        r1 = DcCall(FNV64("remove-player-weapon"), 1, &w);
    uint64_t has2 = DcCall(FNV64("has-weapon?"), 1, &w), hm =
        DcCall(SID_HAS_MELEE_WEAPON, 0, &none);
    if (g_keepMelee && BaseWeapon(g_keepMelee) == BaseWeapon(w))
        g_keepMelee = 0;
    if (g_lastMelee && BaseWeapon(g_lastMelee) == BaseWeapon(w))
        g_lastMelee = 0;
    Xlog("wpnbreak: %s - broken %llx out of the hand (%s: holster-player-weapon-instantly %llx, held %llx -> %llx) and the inventory (has-weapon? %llx -> remove-player-weapon %llx -> has-weapon? %llx%s); any melee item left %llx - the fists are up", why, (unsigned long long) w, held ? "it was held" : "not in the hand", (unsigned long long) r0, (unsigned long long) cw, (unsigned long long) cw2, (unsigned long long) has, (unsigned long long) r1, (unsigned long long) has2, has2 != ~0ULL && (has2 & 0xff) ? " - REFUSED: the 0-point item stays in the inventory" : "", (unsigned long long) hm);
    NaeBreakLatch(w, cw2);    // ps43 L2: no engine auto-equip of the sidearm after the break
}

// melee_duel.c's ledger: the point lent for w (1) - its blows take the lend over (the ledger sets the true value)
static int WbLent(uint64_t w)
{
    return g_wb.lend && g_wb.lend_w == w;
}

static void WbLendTake(uint64_t w)
{
    if (WbLent(w)) {
        g_wb.lend = 0;
        Xlog("wpnbreak: lent point of %llx taken over by the duel's durability ledger", (unsigned long long) w);
    }
}

// t2r-ext.c onetap_swing_tick: a swing starts on the weapon's last point (no other lend, no duel - melee_duel.c's ledger owns its blows)
static void WbSwingStart(uint64_t w, int64_t h)
{
    if (g_wb.lend && g_wb.lend_w == w)
        return;
    if (WbOff() || h != 1 || !w || MeleeClassOf(w, NULL) == MC_MACHETE
        || g_otLent || g_wb.pend || (g_mdu.on && g_mdu.slot >= 0))
        return;
    uint64_t two = 2;
    DcCall(SID_SET_WEAPON_HEALTH2, 1, &two);
    g_wb.lend = 1;
    g_wb.lend_t60 = g_t60 ? g_t60 : 1;
    g_wb.lend_w = w;
    Xlog("wpnbreak: swing on the last durability point of %llx - 1 point lent (the break plays at the contact, the item leaves at the gesture's drop)", (unsigned long long) w);
}

static void WbSend(int flags, uint64_t w)
{
    if (!g_sendTramp)
        return;
    uint8_t b[9];
    b[0] = (uint8_t) flags;
    memcpy(b + 1, &w, 8);
    for (int i = 0; i < 2; i++)
        g_sendTramp(MSG_WPNBREAK, b, 9, 0xff); // twice: one lost datagram = a silent break
}

static void WbBreak(uint64_t w, const char *why)
{
    int two = 0;
    MhfxClass(w, &two, NULL);
    const char *ev = WbSfx(w);
    uint64_t h = EsfxPlay(ev, SID_PLAYER, "r_hand_prop");
    Xlog("wpnbreak: weapon %llx broke (%s) - %s at r_hand_prop -> %llx%s; gesture in %d ms (after any paired move)", (unsigned long long) w, why, ev, (unsigned long long) h, h ? "" : " (refused - bank not loaded?)", WbGdelay());
    WbSend(1 | (two ? 4 : 0), w);
    g_wb.pend = 1;
    g_wb.w = w;
    g_wb.two = two;
    g_wb.t0_ms = GetTickCount64();
    g_wb.drop_ms = 0;
}

// Observed: "wpnbreak: drop frame - broken 2bb2c011d62c43c8 out of the hand ... the fists are up", "dpad: in the hand 0 (none)"; then "htr~ #2914
// item-state-request(other) own sm=914f4265d8 state=exe+35c0350 | exe+bd84ef exe+bdb584 exe+bdb77c exe+d9f23c exe+c22d49", "own-held 0 -> 0c40526469ba8c07",
// "dpad: in the hand c40526469ba8c07 (pistol-beretta-supp)": no draw of ours, no input line - the player walks with the pistol to the end of the clip. Disassembly of the
// exe at that stack: exe+c22d44 (Player update) calls exe+d9ec60; its block exe+d9f014..d9f237 is the engine's own auto-equip: it looks up the script tunable
// *player-auto-holster-time* of anim-player (exe+1357810(0x706de0030a8ae6e6, 0xc2a30e7d296fcc26, 0) -> float *, seconds), and when the player is in its Move state,
// its idle clock is past that time (time > seconds * 300 * 2^32, exe+d9f06e..d9f082) and the item controller holds nothing (exe+bdc4a0 == 0), it picks a weapon
// from the inventory (last used, then the slots) and calls exe+bdb720 = draw it. With no melee item left that weapon is the sidearm. The same block is what puts a
// pistol in the hand of a fighter who punches empty-handed in a duel. Rule: while a duel is engaged, and after a melee weapon broke until the player draws
// something himself (or the life ends), the engine does not auto-equip - the fists stay. Stopped at that source: the tunable is raised to 1e6 s while the rule
// holds (below 2^63 / (300 * 2^32) = 7.1e6 s, so the engine's float -> int64 compare does not wrap) and put back after. Nothing else is touched: a draw the player
// asks for (weapon cross, aim) takes other callers of exe+bdb720. t2r-auto-equip-old.txt = off (the engine's auto-equip as before). Log prefix "auto-equip:".
#define NAE_BIG 1.0e6f
static struct {
    int latch, raised, nolook;
    uint64_t life, chk_t60, seen_f;
    float orig;
    int nseen;
} g_nae;
static float *NaeTunable(void)
{
    typedef float *(*fn_look)(uint64_t sym, uint64_t mod, float *def);
    uint8_t *exe = (uint8_t *) GetModuleHandleW(NULL);
    float *p =
        ((fn_look) (exe + 0x1357810)) (0x706de0030a8ae6e6ULL
                                       /* *player-auto-holster-time* */ ,
                                       0xc2a30e7d296fcc26ULL
                                       /* anim-player */ , NULL);
    if (!p || IsBadReadPtr(p, 4) || IsBadWritePtr(p, 4))
        return NULL;
    return p;
}

static void NaeBreakLatch(uint64_t w, uint64_t held_now)
{                               // wb_take_out: the broken item left the hand
    if (GetFileAttributesA("t2r-auto-equip-old.txt") !=
        INVALID_FILE_ATTRIBUTES)
        return;
    if (held_now && held_now != ~0ULL)
        return;                 // something else is in the hand already
    g_nae.latch = 1;
    g_nae.life = g_spawnedFrame;
    Xlog("auto-equip: melee weapon %llx broke with nothing else in the hand - the engine's auto-equip of a firearm is held off until the player draws something himself or this life ends (ps43)", (unsigned long long) w);
}

static void NaeNoteAutodraw(void)
{                               // sreq_hook: the engine's auto-equip block (return address exe+d9f23c) asked for its draw state
    g_nae.seen_f = g_frame;
    g_nae.nseen++;
    if (g_nae.nseen <= 40)
        Xlog("auto-equip: ENGINE auto-equip draw request (Player update exe+d9f23c) frame %llu - held %llx, duel engaged %d, break latch %d, tunable raised %d%s (ps43)", (unsigned long long) g_frame, (unsigned long long) CurrentWeapon(), MduEngagedNow(), g_nae.latch, g_nae.raised, g_nae.raised ? " - NOT EXPECTED while raised: the tunable is not what gates it" : "");
}

static void NaeTick(void)
{
    int off =
        GetFileAttributesA("t2r-auto-equip-old.txt") !=
        INVALID_FILE_ATTRIBUTES;
    int live = MatchSubstate() == 6 && !g_inSpawn && !g_downed
        && !g_downDying && LocalHp() > 0.0f;
    if (g_nae.latch) {
        uint64_t cw = CurrentWeapon();
        const char *why =
            off ? "t2r-auto-equip-old.txt" : g_nae.life !=
            g_spawnedFrame ? "a new life" : !live ?
            "down / dead / not in a match" : cw
            && cw !=
            ~0ULL ?
            "an item is in the hand (the player's own draw / pickup)" :
            NULL;
        if (why) {
            g_nae.latch = 0;
            Xlog("auto-equip: break latch ended - %s (held %llx) (ps43)",
                 why, (unsigned long long) cw);
        }
    }
    int duel = MduEngagedNow();
    int want = !off && live && (duel || g_nae.latch);
    if (want == g_nae.raised && !(want && g_t60 - g_nae.chk_t60 >= 60))
        return;                 // re-checked once a second while raised (a reloaded script module brings its own value back)
    g_nae.chk_t60 = g_t60;
    if (MatchSubstate() != 6) {
        g_nae.raised = 0;
        return;
    }                           // between matches the script module may be gone: nothing is looked up
    float *p = NaeTunable();
    if (!p) {
        if (!g_nae.nolook++)
            Xlog("auto-equip: tunable *player-auto-holster-time* (anim-player) not found / not writable - the engine's auto-equip is NOT held off (ps43)");
        g_nae.raised = 0;
        return;
    }
    static int nl;
    if (want) {
        if (*p != NAE_BIG) {
            if (*p > 0.0f && *p < 1.0e5f)
                g_nae.orig = *p;
            float was = *p;
            *p = NAE_BIG;
            if (LOGCAP(nl, 200))
                Xlog("auto-equip: engine auto-equip of a firearm held off - %s (tunable *player-auto-holster-time* %.2f s -> %.0f s at %p; held %llx) (ps43)", duel ? "a duel is engaged" : "melee weapon broke: fists until the player's own draw", (double) was, (double) NAE_BIG, (void *) p, (unsigned long long) CurrentWeapon());
        }
        g_nae.raised = 1;
    } else {
        if (*p == NAE_BIG && g_nae.orig > 0.0f) {
            *p = g_nae.orig;
            if (LOGCAP(nl, 200))
                Xlog("auto-equip: engine auto-equip allowed again (tunable back to %.2f s; %d engine draw requests seen so far, last frame %llu) (ps43)", (double) g_nae.orig, g_nae.nseen, (unsigned long long) g_nae.seen_f);
        }
        g_nae.raised = 0;
    }
}

static void WbTick(void)
{
    NaeTick();
    if (MatchSubstate() != 6 || WbOff()) {
        g_wb.lastw = 0;
        g_wb.pend = 0;
        return;
    }
    uint64_t now = GetTickCount64();
    int busy = PsyncActive() || GxRunning() || (g_execUntil
                                                  && g_t60 < g_execUntil)
        || (g_otSwingAt && g_t60 - g_otSwingAt < 60);
    if (busy)
        g_wb.busy_t60 = g_t60;
    // puppets: the drop frame of their gesture
    for (uint32_t k = 0; k < T2R_MAX_PEERS; k++) {
        if (!g_wb.pd[k].ms || now - g_wb.pd[k].ms < WB_DROP_MS)
            continue;
        uint8_t *pn = PstNpc(g_wb.pd[k].ent), *pw =
            pn ? PstCurrent(pn) : NULL;
        static int nd;
        int ok = WbHide(pw, g_wb.pd[k].w);
        if (ok)
            WbNoteBroken(k, pw);
        if (LOGCAP(nd, 20))
            Xlog("wpnbreak: slot %u drop frame: held item %s (visual only, no thrown prop)", k, ok ? "hidden" : "already gone");
        g_wb.pd[k].ms = 0;
        g_wb.pd[k].keep_w = g_wb.pd[k].w;
        g_wb.pd[k].keep_ms = now + 4000;
        g_wb.pd[k].fb_ms = 0;
    }
    // v5-wbreak (on the victim's screen the attacker's broken weapon stayed in its hand): the gesture message may be lost / arrive while the puppet is in a paired move
    // (no drop scheduled then = no hide ever), and the puppet's weapon shows again when the engine re-reads it - so the break sound arms a fallback drop, and after the drop the
    // hide is re-asserted for 4 s (the hide is one byte write, nothing happens when the item is gone)
    for (uint32_t k = 0; k < T2R_MAX_PEERS; k++) {
        if (g_wb.pd[k].fb_ms && now >= g_wb.pd[k].fb_ms) {
            uint8_t *pn = PstNpc(PuppetHandle(k)), *pw =
                pn ? PstCurrent(pn) : NULL;
            int ok = WbHide(pw, g_wb.pd[k].keep_w);
            if (ok)
                WbNoteBroken(k, pw);
            Xlog("wpnbreak: slot %u: no drop-gesture message %d ms after the break sound - the broken item is hidden anyway (%s)", k, (int) (WB_DROP_FB_MS), ok ? "hidden" : "already gone");
            g_wb.pd[k].fb_ms = 0;
            g_wb.pd[k].keep_ms = now + 4000;
        }
        if (g_wb.pd[k].keep_ms) {
            if (now >= g_wb.pd[k].keep_ms)
                g_wb.pd[k].keep_ms = 0;
            else {
                uint8_t *pn = PstNpc(PuppetHandle(k)), *pw =
                    pn ? PstCurrent(pn) : NULL;
                WbHide(pw, g_wb.pd[k].keep_w);
            }
        }
    }
    for (uint32_t k = 0; k < T2R_MAX_PEERS; k++)
        if (g_wb.pd[k].clip && now >= g_wb.pd[k].stop_ms) {
            MhrStop(g_wb.pd[k].ent, g_wb.pd[k].clip, 0.2f);
            g_wb.pd[k].clip = 0;
        }
    if (g_wb.lend) {            // the lent swing: its contact = the break (SP: at the hit), no contact in 1.5 s = the point back
        uint64_t cw = CurrentWeapon(), none = 0;
        if (g_meleeContactAt >= g_wb.lend_t60 && cw == g_wb.lend_w
            && g_otRefundUntil && g_t60 < g_otRefundUntil) {
            uint64_t one = 1;
            DcCall(SID_SET_WEAPON_HEALTH2, 1, &one);
            g_wb.lend = 0;      // a hit on a downed player costs nothing (downed_hit_no_wear): no break
            Xlog("wpnbreak: lent point of %llx back (the contact was on a downed player - no wear)", (unsigned long long) g_wb.lend_w);
        } else if (g_meleeContactAt >= g_wb.lend_t60
                   && cw == g_wb.lend_w) {
            uint64_t one = 1, h =
                DcCall(SID_GET_WEAPON_HEALTH2, 0, &none);
            if (h == ~0ULL || (int64_t) h != 1)
                DcCall(SID_SET_WEAPON_HEALTH2, 1, &one);
            g_wb.lend = 0;
            WbBreakHit(g_wb.lend_w,
                         "the contact on the last durability point");
        } else if (cw != g_wb.lend_w || g_t60 - g_wb.lend_t60 > 90) {
            uint64_t h = DcCall(SID_GET_WEAPON_HEALTH2, 0, &none);
            if (cw == g_wb.lend_w && h != ~0ULL && (int64_t) h == 2) {
                uint64_t one = 1;
                DcCall(SID_SET_WEAPON_HEALTH2, 1, &one);
            }
            Xlog("wpnbreak: lent point of %llx back (%s)",
                 (unsigned long long) g_wb.lend_w,
                 cw !=
                 g_wb.
                 lend_w ? "the item left the hand" :
                 "the swing hit nothing");
            g_wb.lend = 0;
        }
    }
    if (g_wb.zero && g_wb.pend) {       // the broken weapon stays in the hand (1 point) until the drop frame
        uint64_t none = 0, h =
            CurrentWeapon() ==
            g_wb.zero_w ? DcCall(SID_GET_WEAPON_HEALTH2, 0,
                                  &none) : ~0ULL;
        if (h != ~0ULL && (int64_t) h == 0) {
            uint64_t one = 1;
            DcCall(SID_SET_WEAPON_HEALTH2, 1, &one);
        }
    }
    // our own break: detect
    if (!g_downed && !g_execUntil && !g_wb.zero) {
        uint64_t w = CurrentWeapon(), none = 0;
        int held = w != ~0ULL && MeleeDurW(w) && MeleeClassOf(w, NULL) != MC_MACHETE;       // (the machete never breaks)
        if (held) {
            uint64_t hh = DcCall(SID_GET_WEAPON_HEALTH2, 0, &none);
            if (w != g_wb.lastw) {
                g_wb.lastw = w;
                g_wb.lasthp = -1;
            }
            if (hh != ~0ULL && (int64_t) hh >= 0 && (int64_t) hh < 1000) {
                if ((int64_t) hh == 0 && g_wb.lasthp > 0 && !g_wb.pend)
                    WbBreak(w, "durability read 0 in hand");
                g_wb.lasthp = (int64_t) hh;
                g_wb.seen = g_t60;
            }
        } else if (g_wb.lastw) {
            // the item left the hand: a break if its last durability was the final point and a swing / paired move ran just now (a plain swap keeps >= 1 hp and no swing)
            // (the weapon broke without using up all the hits, with the break sound after the interrogation started; [24662]-[24663]): the
            // interrogation's own instant holster (melee execution 2013, unarmed move) took the 1-point lead pipe out of the hand 238 frames after its last
            // contact, inside the 180-frame "a pair ran" window = a break. A break leaves the hand at a CONTACT (the engine destroys the item on the blow that
            // takes its last point): the hit must be this recent, and an item our own unarmed-execution put away is no break.
            int contact_now = g_meleeContactAt
                && g_t60 >= g_meleeContactAt
                && g_t60 - g_meleeContactAt < 45;
            int our_holster = g_unarmExec && g_unarm_weapon == g_wb.lastw;
            if (g_wb.lasthp >= 0 && g_wb.lasthp <= 1
                && g_t60 - g_wb.seen < 30 && g_wb.busy_t60
                && g_t60 - g_wb.busy_t60 < 180 && LocalHp() > 0
                && !g_wb.pend) {
                if (contact_now && !our_holster)
                    WbBreak(g_wb.lastw,
                             "last durability point, item gone from the hand right after a swing");
                else {
                    static int nl;
                    if (LOGCAP(nl, 20))
                        Xlog("wpnbreak: %llx (1 point) left the hand %s - no break, the point stays", (unsigned long long) g_wb.lastw, our_holster ? "for our unarmed execution / interrogation (put away, back at its end)" : "with no contact of ours in the last 45 frames (a holster / swap)");
                }
            }
            if (g_t60 - g_wb.seen >= 30 || g_wb.lasthp == 0)
                g_wb.lastw = 0;
        }
    }
    // our gesture / drop
    if (g_wb.pend) {
        if (g_downed || g_execUntil || LocalHp() <= 0.0f
            || now - g_wb.t0_ms > WB_DEFER_MAX_MS) {
            int late = now - g_wb.t0_ms > WB_DEFER_MAX_MS;
            Xlog("wpnbreak: gesture dropped (%s)",
                 late ? "a paired move kept us busy too long" :
                 "downed / dead");
            g_wb.pend = 0;
            WbZero("no gesture");
            if (late && !g_downed && LocalHp() > 0.0f)
                WbTakeOut(g_wb.w, "no gesture");      // (v5-wpn2: a 0-point item left in the hand blocks melee)
        } else if (g_wb.pend == 1
                   && now - g_wb.t0_ms >= (uint64_t) WbGdelay()
                   && !PsyncActive() && !GxRunning()) {
            int gr = WbGesture(SID_PLAYER, g_wb.two, "own body");
            if (gr == 2 && g_wb.zero && g_wb.gtries++ < 6)
                return;         // (v5-pinfix3 4: log 41352 - gesture_ refused once right after the swing; asked again next frame)
            if (gr)
                WbSend(2 | (g_wb.two ? 4 : 0), g_wb.w);
            g_wb.drop_ms = now;
            g_wb.pend = 2;
        } else if (g_wb.pend == 2 && now - g_wb.drop_ms >= WB_DROP_MS) {
            WbZero("drop frame");
            uint8_t *me = BpMe(), *pw = me
                && !IsBadReadPtr(me + 0x2f60,
                                 8) ? PlayerWeapon(me) : NULL;
            int ok = WbHide(pw, g_wb.w);
            Xlog("wpnbreak: drop frame (SP event drop-broken-weapon): held broken item %s - no thrown prop (no engine drop native known)", ok ? "hidden" : "already gone");
            WbTakeOut(g_wb.w, "drop frame");
            g_wb.pend = 0;
        }
    }
}

static void WbRecv(uint32_t slot, const uint8_t *buf, uint64_t len)
{
    if (slot >= T2R_MAX_PEERS || slot == *(uint32_t *) T2R_SYM(kMySlot)
        || !buf || len < 9 || WbOff() || MatchSubstate() != 6)
        return;
    uint64_t w;
    memcpy(&w, buf + 1, 8);
    int fl = buf[0], two = (fl & 4) != 0;
    if (MeleeClassOf(w, NULL) < 0)
        return;                 // not one of our melee items
    uint64_t ent = PuppetHandle(slot), now = GetTickCount64();
    if (!ent || g_peerDowned[slot] || g_puppetDeadHere[slot])
        return;
    if (g_wb.rx_n[slot] == fl && now - g_wb.rx_ms[slot] < 150)
        return;                 // the duplicate datagram
    g_wb.rx_ms[slot] = now;
    g_wb.rx_n[slot] = fl;
    if (fl & 1) {
        const char *ev = WbSfx(w);
        uint64_t h = EsfxPlay(ev, ent, "r_hand_prop");
        static int nl;
        if (LOGCAP(nl, 30))
            Xlog("wpnbreak: slot %u's weapon broke - %s at its hand -> %llx", slot, ev, (unsigned long long) h);
        g_wb.pd[slot].fb_ms = now + WB_DROP_FB_MS;
        g_wb.pd[slot].keep_w = w;
    }
    if (fl & 2) {               // the gesture message came: the drop frame is scheduled from it below (busy or not), the fallback is not needed
        g_wb.pd[slot].fb_ms = 0;
        g_wb.pd[slot].keep_w = w;
        if (PsyncBusySlot(slot) || HrxBusyPeer(slot)) {
            g_wb.pd[slot].ms = now;
            g_wb.pd[slot].ent = ent;
            g_wb.pd[slot].w = w;
            g_wb.pd[slot].two = two;
        }                       // (no gesture over a paired move, but the item still leaves the hand)
    }
    if ((fl & 2) && !PsyncBusySlot(slot) && !HrxBusyPeer(slot)) {
        int gr = WbGesture(ent, two, "puppet");
        if (gr) {
            g_wb.pd[slot].ms = now;
            g_wb.pd[slot].ent = ent;
            g_wb.pd[slot].w = w;
            g_wb.pd[slot].two = two;
        }
        if (gr == 3) {
            char c[80];
            snprintf(c, sizeof c, "ellie-%shblunt-drop-broken-weapon",
                     two ? "2" : "1");
            g_wb.pd[slot].clip = Fnv64Rt(c);
            g_wb.pd[slot].stop_ms =
                now +
                (uint64_t) (MdgClipLen(g_wb.pd[slot].clip) * 1000.0f) -
                120;
        }
    }
}
