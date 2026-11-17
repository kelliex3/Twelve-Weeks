// melee_stagger.c - a melee blow that lands on a standing player INTERRUPTS him (melee should be useful instead of a last resort).
// Included after ctxdown.c in t2r-ext.c, before feel.c (feel.c's hrx_victim_hit feeds mhstun_note). Notes: re/melee-gap-1025.md.
//
// Before: a non-lethal melee blow only played the additive visual flinch (feel.c) and extended an in-progress swing (mstagger_*); the victim could keep
// aiming and shooting through the whole combo, so a punch was never better than shooting back (SP and 2013 both stop the victim: 2013 Factions rules/melee.md
// "a blow that doesn't down plays a normal hit reaction", the PvP hit-reaction list is 0.6-0.9 s per entry, vulnerability HitReaction).
// Now (on the VICTIM's screen, where the hit is applied - the streamed body shows it to everybody): for MHS_FIST_MS / MHS_WPN_MS after the blow every joypad
// command except camera + move is disabled (the revive lock's call, re-issued every frame): no aim, fire, melee, reload, throw, sprint-jump. Blows that land
// during the stagger extend it, up to MHS_MAX_MS from the first blow; after the stagger a MHS_IMMUNE_MS window gives no new stagger (2013's grazing
// protection idea: no infinite stun-lock, a 4-5 punch chain is still all one stagger, then the victim gets a real window to answer).
// Never on: downed / dying / dead, stun-bomb stun, execution / paired move / contextual down, finisher guard, revive, backpack, intro, crossbow bolt.
// A lethal blow (Melee Down) never reaches this code (the recv_hit hook returns earlier). Opt-out: t2r-no-melee-hitstun.txt next to the exe (re-read every 2 s).
// Log prefix "meleestun:".
#define MHS_FIST_MS 550
#define MHS_WPN_MS 700
#define MHS_MAX_MS 1400
#define MHS_HARD_MS 280         // camera + move only for this long after each blow, then aim / fire come back (melee stays locked)
#define MHS_IMMUNE_MS 900
#define MHS_REACH 4.5f          // the blow came from a body this close (a thrown brick / bottle is not a melee blow)
static struct {
    uint64_t start, until, immune_until, last_blow;
    uint32_t slot;
    int on, blows, react;
} g_mhs;
static int MhsStaggerOn(void)
{
    return g_mhs.on;
}                               // t2r-ext.c hfix_hold_tick: a melee weapon stays in the hand through the stagger (v5-fb1)

static int MhrVictimStart(uint32_t slot, uint64_t w, float dmg, uint64_t * ms);       // melee_hitreact.c: the SP hit-reaction clip (replaces the flinch; the lock shrinks to its window)
static void MhrVictimNone(uint32_t slot);
static void MhrVictimNone2(uint32_t slot, int landed);
static int MhrRunning(void);
static int MhrCoversBlow(void);
static uint64_t g_mhrPainW;   // the weapon of the blow being judged (melee_hitreact.c pain grunt: blade / blunt variant)
static int HrxBusyOwn(void);  // feel.c: the states a hit reaction must never touch
static int MhsOff(void)
{
    static int off = -1;
    static uint64_t at;
    if (off < 0 || g_frame - at >= 120) {
        at = g_frame ? g_frame : 1;
        FILE *f = ExeSideOpen("t2r-no-melee-hitstun.txt", "r");
        int o = f != NULL;
        if (f)
            fclose(f);
        if (o != off)
            Xlog("meleestun: %s",
                 o ? "OFF (t2r-no-melee-hitstun.txt)" :
                 "on (a melee blow that lands stops the victim aiming / shooting / meleeing for ~0.55-0.7 s)");
        off = o;
    }
    return off;
}

static int MhsIsBlow(uint64_t w)
{
    if (!w || BaseWeaponIsGun(BaseWeapon(w)))
        return 0;
    if (w == 0xa1130a6d7634cb3aULL /* explosion */ 
        || w == 0x2a3164cb4a1c95d9ULL /* molotov */ 
        || w == 0xcf576d288d014eacULL /* pipe bomb */ )
        return 0;
    return 1;
}

// called from feel.c hrx_victim_hit with every hit that reached us and left us alive
static void MhstunNote(uint32_t slot, uint64_t w, float dmg,
                        float hp_before, float hp_after)
{
    if (MatchSubstate() != 6 || !(dmg > 0.0f) || dmg >= 150.0f
        || slot >= T2R_MAX_PEERS || !MhsIsBlow(w))
        return;
    g_mhrPainW = w;           // the pain grunt's blade / blunt variant (melee_hitreact.c)
    if (!(hp_after > 0.0f) || hp_before - hp_after < 0.5f) {
        MhrVictimNone2(slot, !(hp_after > 0.0f));
        return;
    }                           // dead / downed by it, or nothing taken (the attacker's reaction lock is released)
    if (MhsOff())
        return;
    uint64_t now = GetTickCount64();
    const char *skip = NULL;
    if (IsTeammate(slot))
        skip = "teammate";
    else if (HrxBusyOwn() || g_stun_until
             || (g_execUntil && g_t60 < g_execUntil) || g_downAt)
        skip =
            "busy (downed / stun / execution / paired move / finisher / revive / backpack)";
    int dplain = MduPlainStr(slot) >= 0;      // v5-armed: a plain duel blow (an armed victim's every combo blow, melee_duel.c) - each one restarts the SP reaction, as in SP
    if (skip);
    else if (!Mv2On() && !dplain && !g_mhs.on && now < g_mhs.immune_until)
        skip = "immune window after the last stagger";
    if (!skip) {                // the blow came from a body in reach (the attacker's puppet near us)
        float vb[16], ab[16];
        const float *vl = Uc4MyLoc(vb);
        uint64_t ah = PuppetHandle(slot);
        const float *al = ah ? PuppetLocator(ah, ab) : NULL;
        if (vl && al) {
            float dx = al[0] - vl[0], dz = al[2] - vl[2];
            if (dx * dx + dz * dz > MHS_REACH * MHS_REACH)
                skip = "attacker too far for a melee blow";
        }
    }
    int cls = MeleeClassOf(w, NULL);
    uint64_t ms = cls >= 0 ? MHS_WPN_MS : MHS_FIST_MS;
    static int nl;
    if (skip) {
        MhrVictimNone2(slot, strstr(skip, "immune") ? 3 : 1); // immune: no lock, but the pain grunt + flinch still show the hit
        if (LOGCAP(nl, 40))
            Xlog("meleestun: blow from slot %u (weapon %llx, dmg %.0f) - no stagger: %s", slot, (unsigned long long) w, dmg, skip);
        return;
    }
    int v2re = 0;               // melee v2: a blow after the reaction's impact part plays a new reaction (the lock is only extended, capped as before)
    if (g_mhs.on && ((Mv2On() && !MhrRunning()) || dplain)) {
        uint64_t rms = 0;
        v2re = 1;
        if (MhrVictimStart(slot, w, dmg, &rms)) {
            g_mhs.react = 1;
            ms = rms;
        } else
            MhrVictimNone2(slot, 2);
    }
    if (!v2re && g_mhs.on && g_mhs.react) {
        MhrVictimNone2(slot, 2);
        if (LOGCAP(nl, 40))
            Xlog("meleestun: blow from slot %u during the SP hit reaction - not restarted", slot);
        return;
    }
    if (!g_mhs.on) {
        uint64_t rms = 0;
        g_mhs.react = MhrVictimStart(slot, w, dmg, &rms);     // the SP clip plays: the command lock lasts exactly its impact part
        if (g_mhs.react)
            ms = rms;
        else
            MhrVictimNone2(slot, 2);
        g_mhs.start = now;
        g_mhs.until = now + ms;
        g_mhs.blows = 0;
        g_mhs.on = 1;
    } else {
        uint64_t u = now + ms;
        if (u > g_mhs.start + MHS_MAX_MS)
            u = g_mhs.start + MHS_MAX_MS;
        if (u > g_mhs.until)
            g_mhs.until = u;
    }
    g_mhs.slot = slot;
    g_mhs.blows++;
    if (LOGCAP(nl, 80))
        Xlog("meleestun: STAGGER by slot %u (weapon %llx, dmg %.0f, hp %.0f -> %.0f) blow #%d, %llu ms since the last blow, aim / fire / melee off until +%llu ms (cap %d ms from the first blow)", slot, (unsigned long long) w, dmg, hp_before, hp_after, g_mhs.blows, (unsigned long long) (g_mhs.last_blow ? now - g_mhs.last_blow : 0), (unsigned long long) (g_mhs.until - now), MHS_MAX_MS);
    g_mhs.last_blow = now;
}

static void MhstunTick(void)
{
    if (!g_mhs.on)
        return;
    uint64_t now = GetTickCount64();
    const char *end = NULL;
    if (MatchSubstate() != 6)
        end = "left the match state";
    else if (g_downed || g_downDying || g_uc4DyingUntil
             || LocalHp() <= 0.0f)
        end = "downed / dying";
    else if (g_stun_until || (g_execUntil && g_t60 < g_execUntil)
             || PsyncOwn() || GxVictimActive() || FinisherGuardOn())
        end = "a stun / execution / paired move took the body";
    else if (g_mhs.react && !MhrRunning() && now > g_mhs.start + 40)
        end = "SP reaction over";
    else if (now >= g_mhs.until || MhsOff())
        end = "time up";
    if (end) {
        g_mhs.on = 0;
        g_mhs.react = 0;
        g_mhs.immune_until = now + MHS_IMMUNE_MS;
        Xlog("meleestun: stagger over after %llu ms, %d blow(s) (%s)%s",
             (unsigned long long) (now - g_mhs.start), g_mhs.blows, end,
             strcmp(end, "time up")
             && strcmp(end,
                       "SP reaction over") ? "" :
             " - controls back, no new stagger for 0.9 s");
        return;
    }
    // Punching limits the victim but does not take away their gun: only the first MHS_HARD_MS after each
    // blow is camera + move only; after it the victim may aim and fire again (the concussion's gun shake makes the shot harder), while melee, sprint,
    // reload, throw and crafting stay locked to the end of the stagger. t2r-melee-stun-gunlock.txt = old (no gun for the whole stagger).
    static int gl = -1;
    static uint64_t glat;
    if (gl < 0 || now - glat > 2000) {
        glat = now;
        gl = GetFileAttributesA("t2r-melee-stun-gunlock.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    if (!gl && g_mhs.last_blow && now - g_mhs.last_blow >= MHS_HARD_MS) {
        static int nl;
        if (LOGCAP(nl, 30))
            Xlog("meleestun: gun back %llu ms after the blow (aim / fire allowed, melee still locked)", (unsigned long long) (now - g_mhs.last_blow));
        uint64_t ex2[7] =
            { FNV64("camera"), FNV64("move"), FNV64("aim"),
  FNV64("aim-hold"), FNV64("fire"), FNV64("fire-press"),
  FNV64("fire-weapon") };
        DcCall(FNV64("joypad-disable-all-commands-except"), 7, ex2);
        return;
    }
    uint64_t ex[2] = { FNV64("camera"), FNV64("move") };
    DcCall(FNV64("joypad-disable-all-commands-except"), 2, ex);
}
