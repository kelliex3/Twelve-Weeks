// melee_autodraw.c (v5-mdraw: meleeing an enemy that has a melee weapon but does not have it out puts them in a
// 'selected melee weapon' anim, for immersion).
//
// SP's NPCs ready the melee weapon they carry the moment a fight turns to melee. In MP a player who carried a bat on its back but held a gun (or
// nothing) fought the whole duel with the pistol in its hand / bare fists - the weapon only came out through a disarm windup (cd_dis_early_draw: instant
// draw, only for a disarm finisher) or the player's own weapon pick. Now the victim's OWN client draws the melee weapon it carries when a melee attack
// reaches it:
//  - TRIGGER 1: a duel partner's ENGAGE / SWING arrives (melee_duel.c mdu_recv: slot X swings at us, ~tc_ms before its contact),
//  - TRIGGER 2: a melee blow (a melee-table weapon, a knife or fists, dmg < 150) reaches us (t2r-ext.c recv_hit_hook, right after mdu_note_hit) - also
//    when the duel is off or the swing message never came.
//  Both only set a request (mad_note); mad_tick (mdrop_tick's neighbour, every frame) draws when the gates are open, up to 1.5 s after the request.
// WHAT: the carried melee weapon (xr_melee_weapon / g_last_melee, has-weapon? checked; the machete counts, the female knife does not - its own code owns
//    it) is drawn with the engine's ANIMATED draw, the script native player-draw-weapon-by-name(weapon, 0.0 s) - the call the weapon wheel's pick and
//    "equip-player-weapon" make: holster state, then the draw state, the hand attach at the draw's grab frame (the female knife code documents the
//    Holster -> Draw states of this call, t2r-ext.c fem knife phase 0). Not the instant draw (player-instant-draw-weapon-by-name: no clip). The hand is
//    verified every frame (xr_hand_ok); if it is not in the hand 0.8 s after the call (the engine refused / swallowed the animated request) the INSTANT draw
//    takes over (then the forced draw past CannotSwitchWeapon), and every success arms the 0.5 s attach-point check (xr_verify_tick, hand-stuck guard).
// GATES (all must hold at the draw; the soft ones wait inside the 1.5 s, the hard ones drop the request):
//    hard: not in a match (substate 6) / dead / downed / dying / executed / respawning / opt-out; a melee weapon (or the female knife) already in the hand = nothing to do
//    soft: not aiming or firing a gun (player-is-aiming? / player-is-firing?), no weapon switch under way (current_weapon != get-player-weapon-id, or the
//          engine's CannotSwitchWeapon gate exe+be0190 raised), no paired move of ours (psync_active), no execution weapon keeper (g_xr_on / g_unarm_exec),
//          no holster debt (g_hdebt_item: an owed Phase1 must settle first), no backpack stow phase
// ONCE PER ENGAGEMENT: the request is ignored while the previous one is done and fewer than MAD_QUIET_MS passed since the last trigger (a fight is one
//    engagement however many blows it has; a player who puts the weapon away on purpose mid-fight is not overruled). Never on every hit.
// HOLSTER-DEBT SAFETY: nothing here holsters; the draw goes through the engine's own switch, and nothing is drawn while a debt is owed or a pair runs.
//    The gun the player held is not remembered / drawn back (the victim half keeps its own g_cd_vgun logic for guns it put away itself).
// Opt-out: t2r-no-melee-autodraw.txt (re-read every 2 s). Log prefix "melee autodraw:".
#define MAD_QUIET_MS 4000       // no trigger for this long = the engagement is over, the next trigger may draw again
#define MAD_WAIT_MS 1500        // a request that finds the gates closed for this long is dropped
#define MAD_VERIFY_T60 48       // frames (0.8 s) the animated draw gets to put the weapon in the hand before the instant draw takes over
static struct {
    int pend, phase, done, slot;        // pend: a request waits for open gates; phase 1: the animated draw was called, the hand is checked; done: this engagement drew / gave up
    uint64_t note_ms, last_ms, t60, w;  // request time, last trigger time, frame of the draw call, the weapon being drawn
    char why[48];
    const char *block;          // the gate that held the request last tick (logged when it gives up)
} g_mad;
static int MadOff(void)
{
    static int v = -1;
    static uint64_t at;
    return MownFile("t2r-no-melee-autodraw.txt", &v, &at);
}

// a melee attack reached us: why = log text, slot = the attacker (-1 unknown)
static void MadNote(int slot, const char *why)
{
    if (MadOff() || MatchSubstate() != 6)
        return;
    uint64_t now = GetTickCount64();
    if (g_mad.last_ms && now - g_mad.last_ms > MAD_QUIET_MS)
        g_mad.done = 0;         // quiet long enough: a new engagement
    g_mad.last_ms = now ? now : 1;
    if (g_mad.done || g_mad.pend || g_mad.phase)
        return;
    g_mad.pend = 1;
    g_mad.slot = slot;
    g_mad.note_ms = now;
    g_mad.block = NULL;
    snprintf(g_mad.why, sizeof g_mad.why, "%s", why ? why : "?");
}

// recv_hit_hook: only melee blows (a melee-table weapon, a knife, fists) - not bullets, fire, explosions, executions
static void MadNoteHit(uint32_t slot, uint64_t w, float dmg)
{
    if (!(dmg > 0.0f) || dmg >= 150.0f || !w)
        return;
    if (!
        (MeleeClassOf(w, NULL) >= 0
         || MeleeClassOf(BaseWeapon(w), NULL) >= 0 || KnifeWeaponId(w)
         || w == FNV64("abby-unarmed") || w == FNV64("ellie-unarmed")))
        return;
    MadNote(slot < T2R_MAX_PEERS ? (int) slot : -1,
             "a melee blow reached us");
}

// Source (log lines 'ANIMATED draw ... controller holds 226c...', 'held 0, current 0', 'SWING abby-1h-blunt-light-1' - the first swing came with the hand
// empty while the club's draw was still pending behind the victim pairs; the watchers saw a pistol / an empty hand swing club moves): no melee press starts a swing while our own melee draw is
// under way and the weapon is not in the hand yet (<= 2.5 s after the draw call; the fallback instant draw runs the frame the pairs end). melee_duel.c mdu_press_latched.
// t2r-swing-wait-draw-old.txt = off. Log "melee autodraw: a press waits for the draw".
static int MadDrawing(void)
{
    static int v = -1;
    static uint64_t at;
    static uint64_t lastlog;
    if (!g_mad.phase || !g_mad.w || MadOff()
        || MownFile("t2r-swing-wait-draw-old.txt", &v, &at)
        || g_t60 - g_mad.t60 > 150 || XrHandOk(g_mad.w))
        return 0;
    if (g_t60 - lastlog > 300) {
        lastlog = g_t60;
        Xlog("melee autodraw: a melee press waits for the draw of %llx (held %llx) - no swing with the hand empty / a gun in it", (unsigned long long) g_mad.w, (unsigned long long) XrHeld());
    }
    return 1;
}

static void MadReset(void)
{
    g_mad.pend = g_mad.phase = 0;
    g_mad.w = 0;
}

static void MadTick(void)
{
    if (!g_mad.pend && !g_mad.phase)
        return;
    typedef union {
        float f;
        uint32_t u;
    } FuT;
    uint64_t now = GetTickCount64(), none = 0;
    if (MadOff() || MatchSubstate() != 6) {
        MadReset();
        return;
    }
    if (g_downed || g_downDying || g_downAt || g_execUntil || LocalHp() <= 0.0f || g_inSpawn) {    // hard: never over a down / death / respawn
        Xlog("melee autodraw: dropped (%s) - down / dead / respawning",
             g_mad.why);
        g_mad.done = 1;
        MadReset();
        return;
    }
    uint64_t cw = CurrentWeapon();
    if (cw == ~0ULL)
        cw = 0;
    if (g_mad.phase) {          // the animated draw was called: wait for the hand, then the instant fallback
        uint64_t w = g_mad.w;
        if (XrHandOk(w)) {
            Xlog("melee autodraw: %llx is in the hand %llu frames after the animated draw (held %llx) - done", (unsigned long long) w, (unsigned long long) (g_t60 - g_mad.t60), (unsigned long long) XrHeld());
            if (!XrHandCheckOff()) {
                g_xr_vfy_w = w;
                g_xr_vfy_at = g_t60 + 30;
                g_xrVfyTry = 0;
            }
            g_mad.done = 1;
            MadReset();
            return;
        }
        if (g_t60 - g_mad.t60 < MAD_VERIFY_T60)
            return;
        // v5-madwait (the victim's bat came out only at the 3rd blow - late): the draw was dropped when a -hit pair ran on us at
        // the check; it now waits up to 3 s for the pair to end, then the instant fallback below draws. t2r-mad-no-wait.txt = old.
        if ((PsyncOwn()) && !g_xrOn && !g_unarmExec
            && g_t60 - g_mad.t60 < 180
            && GetFileAttributesA("t2r-mad-no-wait.txt") ==
            INVALID_FILE_ATTRIBUTES)
            return;
        if (PsyncOwn() || g_xrOn || g_unarmExec) {
            Xlog("melee autodraw: %llx not in the hand %llu frames after the animated draw (held %llx) and a paired move / execution runs - left to the engine, no fallback", (unsigned long long) w, (unsigned long long) (g_t60 - g_mad.t60), (unsigned long long) XrHeld());
            g_mad.done = 1;
            MadReset();
            return;
        }
        uint64_t held0 = XrHeld(), r = XrDraw(w);
        int forced = 0, busy = -1;
        if (!XrHandOk(w)) {
            forced = (int) XrDrawForced(w, &busy);
        }
        int ok = XrHandOk(w);
        Xlog("melee autodraw: animated draw of %llx did not reach the hand (held %llx) - FALLBACK instant draw %llx%s -> %s (held now %llx)", (unsigned long long) w, (unsigned long long) held0, (unsigned long long) r, forced ? " + forced draw" : "", ok ? "in the hand" : "STILL NOT IN THE HAND", (unsigned long long) XrHeld());
        if (ok && !XrHandCheckOff()) {
            g_xr_vfy_w = w;
            g_xr_vfy_at = g_t60 + 30;
            g_xrVfyTry = 0;
        }
        g_mad.done = 1;
        MadReset();
        return;
    }
    // ---- a request is waiting for open gates
    if (cw && !BaseWeaponIsGun(BaseWeapon(cw)) && !WeaponIsThrowable(cw) && cw != FNV64("abby-unarmed") && cw != FNV64("ellie-unarmed")) {        // a melee weapon / the knife is out already
        Xlog("melee autodraw: skipped (%s) - %llx is in the hand already",
             g_mad.why, (unsigned long long) cw);
        g_mad.done = 1;
        MadReset();
        return;
    }
    uint64_t mw = XrMeleeWeapon(cw);
    if (!(MeleeClassOf(mw, NULL) >= 0) && g_lastMelee) {
        uint64_t hw = DcCall(FNV64("has-weapon?"), 1, &g_lastMelee);
        if (hw != ~0ULL && (hw & 0xff))
            mw = g_lastMelee;
    }
    if (mw) {
        uint64_t hw = DcCall(FNV64("has-weapon?"), 1, &mw);
        if (hw != ~0ULL && !(hw & 0xff))
            mw = 0;
    }                           // (stale id: not in the inventory)
    if (!mw || MeleeClassOf(mw, NULL) < 0 || KnifeWeaponId(mw)) {
        static int nl;
        if (LOGCAP(nl, 40))
            Xlog("melee autodraw: skipped (%s) - we carry no melee weapon to draw (held %llx, last melee %llx)", g_mad.why, (unsigned long long) cw, (unsigned long long) g_lastMelee);
        g_mad.done = 1;
        MadReset();
        return;
    }
    const char *blk = NULL;
    uint64_t ar = DcCall(FNV64("player-is-aiming?"), 0, &none), fr =
        DcCall(FNV64("player-is-firing?"), 0, &none), gw = XrHeld();
    uint8_t *me = ((uint8_t * (*)(void)) T2R_SYM(kFnLocalPlayer)) ();
    uint8_t *ctrl = me
        && !IsBadReadPtr(me + 0x2F60,
                         8) ? *(uint8_t **) (me + 0x2F60) : NULL;
    if ((ar != ~0ULL && (ar & 0xff)) || (fr != ~0ULL && (fr & 0xff)))
        blk = "aiming / firing";
    else if (cw && gw && gw != ~0ULL && cw != gw)
        blk = "a weapon switch is under way";
    else if (XrCannotSwitch(ctrl) == 1)
        blk = "the engine's CannotSwitchWeapon gate";
    else if (PsyncOwn())
        blk = "a paired move";
    else if (g_xrOn || g_unarmExec)
        blk = "an execution weapon keeper";
    else if (g_hdebtItem)
        blk = "a holster debt is owed";
    else if (BpOwnPhaseNow())
        blk = "the backpack stow phase";
    // v5-mad2 (a melee attack did make the opponent draw their melee weapon, but far too late: it waits for the animation to end, which
    // never happens in a series of hits): in a series every blow is a paired move on us, the gate never opened. As the VICTIM of a paired move the
    // carried weapon now goes into the hand at once (instant draw - the hit reaction is full-body, the next blow's reaction is the armed one; the
    // engine's animated draw cannot run under the pair). Aiming / firing / a switch / an execution keeper still hold it. t2r-mad-wait-pair.txt = wait.
    if (blk && g_ps.role == 2 && !strcmp(blk, "a paired move")
        && GetFileAttributesA("t2r-mad-wait-pair.txt") ==
        INVALID_FILE_ATTRIBUTES) {
        uint64_t r = XrDraw(mw);
        Xlog("melee autodraw: %s (slot %d) - we are the victim of a paired move: INSTANT draw of %llx under it -> %llx, held now %llx, %d ms after the trigger", g_mad.why, g_mad.slot, (unsigned long long) mw, (unsigned long long) r, (unsigned long long) XrHeld(), (int) (now - g_mad.note_ms));
        g_mad.done = 1;
        MadReset();
        return;
    }
    if (blk) {
        g_mad.block = blk;
        if (now - g_mad.note_ms > MAD_WAIT_MS) {
            Xlog("melee autodraw: gave up (%s) - held back for %d ms by: %s", g_mad.why, (int) (now - g_mad.note_ms), blk);
            g_mad.done = 1;
            MadReset();
        }
        return;
    }
    // v5-mad3 (getting hit did not draw into the melee pose with the melee weapon in hand on the first frame; the character started pulling
    // out the melee weapon from the backpack after the 2nd/3rd hit; log 30621: the animated draw from the pistol had not landed 48 frames later, the next blow's pair blocked it): the
    // carried weapon goes into the hand at once on the trigger. t2r-mad-animated.txt = the engine's animated draw.
    // v5-mad4 (hits connected much worse): drawn at once, the victim is ARMED from the first blow on, and every following duel blow
    // on an armed victim is a plain contact (SP: armed defender = standing hit reaction, no synced pair) - the combo lost its pairs. The animated draw is the
    // default again; t2r-mad-instant.txt = the instant draw.
    if (GetFileAttributesA("t2r-mad-instant.txt") !=
        INVALID_FILE_ATTRIBUTES) {
        uint64_t r = XrDraw(mw);
        Xlog("melee autodraw: %s (slot %d) - we carry %llx (held %llx) - INSTANT draw -> %llx, held now %llx, %d ms after the trigger", g_mad.why, g_mad.slot, (unsigned long long) mw, (unsigned long long) cw, (unsigned long long) r, (unsigned long long) XrHeld(), (int) (now - g_mad.note_ms));
        if (!XrHandCheckOff()) {
            g_xr_vfy_w = mw;
            g_xr_vfy_at = g_t60 + 30;
            g_xrVfyTry = 0;
        }
        g_mad.done = 1;
        MadReset();
        return;
    }
    FuT z = { 0.0f };
    uint64_t a[2] = { mw, z.u };
    uint64_t r = DcCall(FNV64("player-draw-weapon-by-name"), 2, a);
    g_mad.phase = 1;
    g_mad.pend = 0;
    g_mad.t60 = g_t60;
    g_mad.w = mw;
    Xlog("melee autodraw: %s (slot %d) - we carry %llx (held %llx, ranged-in-hand %d) - ANIMATED draw player-draw-weapon-by-name -> %llx, controller holds %llx now, %d ms after the trigger", g_mad.why, g_mad.slot, (unsigned long long) mw, (unsigned long long) cw, cw && BaseWeaponIsGun(BaseWeapon(cw)), (unsigned long long) r, (unsigned long long) XrHeld(), (int) (now - g_mad.note_ms));
}

// v5-pickstow (sometimes a picked up melee weapon went into neither the hand nor the backpack; it appeared when meleeing, and went to the backpack on switching
// weapons or just waiting; log 14290-14342: tomahawk picked up while unarmed, carried stowed but never drawn): a melee item that
// joins the inventory without a draw has the never-drawn draw-control visibility byte (+0x1b0 -> +0x48 = 0x00, see spawn stow) and no holster point
// until the next weapon switch. Every carried, not held melee item with that byte gets it set (as its first draw does) and goes to its holster
// (PutWeaponInHolster, SP's HolsterAllWeapons args). t2r-no-pickup-stow.txt = off.
static void PickstowTick(void)
{
    typedef uint8_t *(*fn_me)(void);
    typedef void (*fn_putw)(void *, void *, int, float);
    typedef uint64_t *(*fn_apsid)(void *, void *);
    static int off = -1;
    static uint64_t off_at;
    if (MatchSubstate() != 6 || g_frame % 15 || g_downed
        || LocalHp() <= 0.0f || PsyncActive())
        return;
    if (off < 0 || GetTickCount64() - off_at > 2000) {
        off_at = GetTickCount64();
        off =
            GetFileAttributesA("t2r-no-pickup-stow.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    if (off)
        return;
    uint64_t none = 0, anym =
        DcCall(0x79763b864843c645ULL
                /* player-has-non-basic-melee-weapon? */ , 0, &none);
    if (anym == ~0ULL || !(anym & 0xff))
        return;
    uint8_t *me = ((fn_me) T2R_SYM(kFnLocalPlayer)) ();
    if (!me || IsBadReadPtr(me + 0x2F60, 8))
        return;
    uint8_t *ctrl = *(uint8_t **) (me + 0x2F60);
    if (!ctrl)
        return;
    uint64_t held = 0;
    HdebtHeld(me, &held);
    for (size_t i = 0; i < sizeof MELEE_TAB / sizeof *MELEE_TAB; i++) {
        uint64_t id = FNV64(MELEE_TAB[i].n);
        if (id == held)
            continue;
        uint64_t has = DcCall(FNV64("has-weapon?"), 1, &id);
        if (has == ~0ULL || !(has & 0xff))
            continue;
        uint8_t *w = HdebtProc(me, id);
        if (!w || w == g_cdDis.hw || IsBadReadPtr(w + 0x1b0, 8))
            continue;
        uint8_t *dc = *(uint8_t **) (w + 0x1b0);
        if (!dc || IsBadWritePtr(dc + 0x48, 4) || dc[0x48] != 0x00
            || dc[0x49] != 0xff || dc[0x4a] != 0xff)
            continue;
        dc[0x48] = 0xff;
        uint8_t *exe = (uint8_t *) GetModuleHandleW(NULL);
        uint64_t o0[8] = { 0 }, o1[8] = { 0 };
        uint64_t *r0 = ((fn_apsid) (exe + 0x1cd4870)) (w, o0), ap0 =
            r0 ? *r0 : 0;
        // taking a melee weapon puts it in the hand, as before - a pickup (not the spawn loadout) goes into the hand.
        // t2r-pickup-to-bag.txt = holstered instead.
        if (g_frame - g_spawnedFrame > 180
            && GetFileAttributesA("t2r-pickup-to-bag.txt") ==
            INVALID_FILE_ATTRIBUTES) {
            uint64_t r = XrDraw(id);
            Xlog("pickup stow: %s (%016llx) picked up, never drawn, invisible - shown + instant draw -> %llx, held now %016llx (was %016llx)", MELEE_TAB[i].n, (unsigned long long) id, (unsigned long long) r, (unsigned long long) XrHeld(), (unsigned long long) held);
            return;
        }
        ((fn_putw) (exe + 0xbdd9d0)) (ctrl, w, -1, 0.0f);
        uint64_t *r1 = ((fn_apsid) (exe + 0x1cd4870)) (w, o1), ap1 =
            r1 ? *r1 : 0;
        Xlog("pickup stow: %s (%016llx) carried, never drawn, invisible - shown + PutWeaponInHolster: %s -> %s (held %016llx)", MELEE_TAB[i].n, (unsigned long long) id, ap0 ? BpApName(ap0) : "none", ap1 ? BpApName(ap1) : "none", (unsigned long long) held);
    }
}
