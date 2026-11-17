// melee_duel_pin.c - the PIN sequence of the MP melee DUEL. Included after melee_duel.c in t2r-ext.c. Log prefix "DUEL: PIN".
// A duel blow that lands with a wall / low cover behind the victim (ctxdown.c's takedown plan: the engine's wall probe) pushes the victim INTO the surface (SP's
// "<set>-wall-attack-enter-*-hit" / low-cover pair) and starts a PIN: the attacker's next melee presses play SP's wall combo (<set>-wall-attack-N, its -hit pair), like SP
// where a pin takes 4-5 punches before the final blow. Tables: mduel_pins.h (tools/mv3/gen_duel_pins.py). Opt-out: t2r-no-duel-pin.txt (the duel itself is opt-in).
//  QTE      before every pin blow after the push the victim gets a DODGE prompt (the Dodge widget, evade glyph) for MPN_WIN_MS. The attacker's swing windup is slowed so its
//           contact falls MPN_WIN_MS after the press. The press is judged on the VICTIM's screen against the blow's start as the victim sees it (the BLOW message arrival, so the
//           one-way latency is subtracted exactly as for the duel dodge ruling): valid from 0 to MPN_WIN_MS + 30 ms; a press up to 300 ms before the prompt burns that QTE (anti-mash).
//  ESCAPE   a valid press cuts the pin pair at once (ABORT), tells the attacker (op ESC: it cancels its swing, no pair, no damage), the victim drops the blow's damage and plays
//           the dodge COUNTER (dcounter.c: SP's dodge-back-attack pair, victim = dodger, pinner = countered, 2 x fist damage, always lands) - the "guaranteed counter hit".
//           The escape plays SP's own NPC-pins-the-player struggle-success pair (mpn_escape: player half on the victim with its animated camera, NPC half on the pinner, the shove = the
//           counter blow at the pair's hit moment; clips from the SP-common paks the parry branch loads, MPN_ESC); the dodge counter is the fallback when that pair is not loaded.
//  MISSED   a legal hit: the table's damage of the pinner's class (v5-dmg: fists 15, 1h blunt 20, 1h blade 25, 2h / machete 34, upgraded = 1 hit less; the push is
//           the duel blow that started the pin, so push + pin blows = the class's hits), the -hit pair of the blow (attacker half + victim half on one wall frame). A blow that would down the
//           victim goes to mpn_finisher_hook (branch `finish`; default = the duel's lethal path: engine contact + Melee Down -> ctxdown's wall takedown from the same plan).
//           After the last pair of the set the attacker shoves off: the duel's 4th-hit gap (MDU_GAP4_MS) and the duel continues.
//  CHARACTER SP authored fists for Abby only, the knife for Ellie only, every 1h / 2h melee weapon for Ellie only: male fists = Abby's pins, female (her fists are the knife) =
//           Ellie's knife pins, a held melee weapon = Ellie's weapon pins for both (mpn_ellie_set).
#include "mduel_pins.h"
// (ESCAPE only worked for a short time after it appeared, and pressing it after that did nothing although it was still on screen;
// log 20288-20482 + a clip at 30 fps): the press window was open+160 .. open+450 ms (PRESS logged at 171 ms, window closed 27
// frames after the open) = 290 ms, but "ESCAPE!" stayed on screen ~600 ms (clip frames 9..26, id 1; ~430 ms past the close for id 3): the engine's
// InteractableIcon keeps its last label up ~300-430 ms after the requests stop, then fades. Now the prompt switches to "Too late" the frame the window
// closes (requested MPN_LATE_MS, so the engine's linger shows "Too late", never a dead "ESCAPE!"), and the window is longer: WIN 420 -> 480, RING 260 -> 330
// = PRESS 150 .. 510 ms (360 ms pressable, WAIT 150 ms as before); the windup slows to the new contact (333 ms SP swing: x0.79 -> x0.69).
// (hit reactions in pins were delayed: 'PIN blow 2/99 QTE
// OPEN ... contact in 480 ms (SP 267 ms, windup slowed x0.56)' - the slow-down was a set-animation-speed, which the engine's own swing ignores (ps20 readback):
// the punch played in 267 ms and the reaction came 213 ms after it landed). The window is the light swings' pace (380 ms; a longer clip keeps its own time) and
// the wind-up is DRIVEN: the swing clip's phase is written each frame along the duel's wind-up profile (wup_ct), so the fist arrives on the contact frame.
// t2r-pin-win-ps27.txt = 480 ms and the speed call only.
#define MPN_WIN_NEW_MS 290      /* 290 ms (was 380 in the first version) */
static int MpnWinOld(void)
{
    static int v = -1;
    static uint64_t at;
    uint64_t n = GetTickCount64();
    if (v < 0 || n - at > 3000) {
        at = n;
        v = GetFileAttributesA("t2r-pin-win-ps27.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    return v;
}

#define MPN_WIN_MS (MpnWinOld() ? 480 : MPN_WIN_NEW_MS)
static struct {
    uint64_t clip, t0;
    float tc, x, len;
} g_mpnWup;
#define MPN_LATE_MS 150
#define MPN_GRACE_MS 30
#define MPN_ESC_COOL_MS 8000    // an escaped victim can't be pinned again by that pinner for 8 s
#define MPN_NEXT_MS 330         // SP: the next press is accepted 330 ms after the previous -hit switch
#define MPN_PRESS_WAIT_MS 1500  // no press for this long after a blow: the attacker lets go
#define MPN_AFTER_MS 1200       // v5-pinpress 2 (keyboard, knife: 7 pins "PIN END (no press) after 1 blow", each next press ~0.8 s after the END): the 1500 ms
                                  // ran from the push's START and the push pair itself hands off at 1.50 s - the press window closed the moment the push finished
                                  // playing, so a pinner who presses when he sees the push land never chained (an earlier build hid it: a stale press fired blow 2 by itself).
                                  // The window now lasts to the pair's hand-off + MPN_AFTER_MS, and a press after a normal hand-off still starts the next blow.
#define MPN_BURN_MS 300
#define MPN_LOCK_MS 250         // v5-pinfix3: anti-mash = a QTE press counts only when the previous E press (in a window or between) is >= MPN_LOCK_MS old
// (pin blows at 290 ms: the 330 ms prompt covered the whole blow, so a press on its first frame counted = mashing):
// the prompt shows for the last MPN_RING_NEW_MS before the contact (290 ms blow: a press in its first ~90 ms fails that QTE, ~230 ms pressable with the grace).
#define MPN_RING_NEW_MS 200     // [TUNING ps28]
#define MPN_RING_MS (MpnWinOld() ? 330 : MPN_RING_NEW_MS)
#define MPN_RING_OLD_UNUSED 330 // (was 260) Escaping a pin was just mashing E rather than a quick time event: the Escape prompt only shows for the LAST
                                  // MPN_RING_MS before the blow's contact (+ grace); a press before the prompt fails that QTE, so mashing always loses
#define MPN_RING_AT() MpnRingAtF()   // (Iron Grip: the pressable part of the window x1.5, never before the QTE opened)
#define MPN_MAXB 5
#define MPN_VIC_MAX 2.0f        // the victim's carry to the push variant's authored start (the far variants start 1.7 m out)
#define MPN_ATT_MAX 1.0f        // (the pin left the victim about 2 m away): 1.25 m carry = a visible pull; Ellie's 0.65 m push still passes
#define MDPO_BLOW 8             // attacker -> victim: {id, win_ms lo, hi}
#define MDPO_ESC 9              // victim -> attacker: {id, rel_ms lo, hi}
#define MDPO_END 10
// v5-dmg the pin's blows follow the melee table (was a flat 20 x weapon factor capped 1.7): every pin blow = one blow of the pinner's class
// (the weapon put away for the pin still counts: its class, with its upgrade), the push being one of them - fists = push + 6 blows (7 hits), 1h blunt push + 4,
// 1h blade push + 3, 2h / machete push + 2. The blows cycle the wall steps (mpn_row_step) until the lethal one plays its takedown.
static struct {
    int on, nlog, slot, surf, set, k, maxb, state, row_cd[MPN_ROWS_N], row_init, id, esc_id, vic_cd;    // state: 0 off, 1 armed (waiting for a press), 2 swing running
    float ap[7];
    uint64_t t_press, due, last_hit, win_end, buf_at, swing_id, slow_clip,
        off_until, esc_cool[T2R_MAX_PEERS];
    // victim side
    uint64_t vq_open, vq_end, vq_last_press, vq_prev_press, vq_drop_until,
        ctr_at;
    int vq_id, vq_burn, vq_ok, vq_slot;
    int vq_shown;               // v5-pinfix3 5: the prompt state shown for vq_id (-1 none, 0 WAIT, 1 PRESS, 2 FAILED) - one log line + one sound per change
    uint16_t vq_rel;
    float vq_ap[7];
    int vq_ap_ok, vq_row;
    uint64_t vq_ap_at;          // the wall frame + the pin row the victim last saw in a pin pair (the escape pair is placed on it)
    uint64_t vq_late_until, vq_closed_at;
    int vq_late_id;             // v5-qte4: "Too late" after a window closed without a valid press (the engine's prompt linger shows it)
    uint64_t wpn;
    int wbroke;                 // v5-wpnpin: the 1h weapon this pin runs with (0 = fists / knife / holstered) and the break flag: it broke mid-pin, the next press re-pins unarmed
    int vq_att;
    uint64_t vq_end_at, vq_esc_at, esc_wait;    // the pin camera (melee_spcam.c spc_tick5): the pinner's slot, its END / our escape press
} g_mpn = {.slot = -1,.esc_id = -1 };

static uint64_t MpnRingAtF(void)
{
    uint64_t span =
        (uint64_t) SkIronWinMs(MPN_RING_MS) + MPN_GRACE_MS, r =
        g_mpn.vq_end > span ? g_mpn.vq_end - span : 0;
    return r < g_mpn.vq_open ? g_mpn.vq_open : r;
}

static int g_mpn_np_seen, g_mpn_np_used, g_mpn_f_frames,
    g_mpn_unfoc_frames, g_mpn_cmd_frames;
static uint64_t g_mpnPairEnd;
static uint64_t MpnWinEnd(uint64_t now);      // v5-pinpress: presses seen / used in the running pin (the no-press END line)
static int MpnOldBurn(void);
// v5-pinfix3 5 (anti-mashing is a rule with visible and audible feedback that you messed up, and the QTE shows early when you have to press): the default QTE now shows from the blow's START - WAIT (the Escape prompt with its ring filling toward the press moment),
// then PRESS at the window start (the ring gives way to the plain button + "Escape!", the TLOU2 UI select sound sfx-select); a press during WAIT FAILS that blow's QTE
// (the prompt turns to "Too early" with an empty ring for the rest of the blow, the UI error sound sfx-menu-error, a TOO EARLY warning notification). Every blow starts
// clean (no burn carried between blows). One press in PRESS escapes. t2r-pin-no-mash-fail.txt = the lenient rule (prompt only from the window, early presses no effect,
// 250 ms mash gap); t2r-pin-old-burn.txt = the old hidden-burn rule.
static int MpnLenient(void)
{
    static int v = -1;
    static uint64_t at;
    uint64_t now = GetTickCount64();
    if (v < 0 || now - at >= 2000) {
        at = now ? now : 1;
        v = GetFileAttributesA("t2r-pin-no-mash-fail.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    return v;
}

// v5-qte4 (the cue was not heard): play-sound(sfx-select / sfx-menu-error) returned handles mid-match (log "sfx-select played",
// "sfx-menu-error -> 466c") but was not heard - a handle proves nothing (gbl-menu is a menu bank). Now TLOU2's own menu waves (the launcher's ripped
// confirm.wav / error.wav, pin_qte_sfx.pcm) go through the ext PCM mixer, the route every mid-match ext sound already plays on (duel punch waves,
// heartbeat, execution sfx in the same log), at the game's sfx volume. t2r-pin-engine-sfx.txt = the old play-sound events.
#include "pin_qte_sfx.h"
__asm__
    (".section .rdata,\"dr\"\n.balign 4\n.globl pqs_blob\npqs_blob:\n.incbin \"pin_qte_sfx.pcm\"\n.text\n");
extern const int16_t pqs_blob[];
// The press-moment cue ("ESCAPE!") = SP hud-momentum-timer-complete-01, FAILED = TLOU2 MP ui-mp-round-timer-tick-zero
// (the menu confirm / error waves before). t2r-pin-menu-cue.txt = the old menu confirm as the press cue
static int MpnUiSound(int fail)
{
    if (GetFileAttributesA("t2r-pin-engine-sfx.txt") !=
        INVALID_FILE_ATTRIBUTES) {
        uint64_t a[2] =
            { FNV64(fail ? "sfx-menu-error" : "sfx-select"), 0 };
        uint64_t r = DcCall(FNV64("play-sound"), 2, a);
        return r && r != ~0ULL ? 2 : 0;
    }
    if (!fail
        && GetFileAttributesA("t2r-pin-menu-cue.txt") ==
        INVALID_FILE_ATTRIBUTES)
        return PcmPlayRate(pqs_blob + PQS_WIN_OFF, PQS_WIN_N, 0.8f,
                             PQS_RATE) ? 1 : 0;
    return PcmPlayRate(pqs_blob + (fail ? PQS_FAIL_OFF : PQS_PRESS_OFF),
                         fail ? PQS_FAIL_N : PQS_PRESS_N,
                         fail ? 1.0f : 0.45f, PQS_RATE) ? 1 : 0;
}

static const char *MpnSndWord(int r)
{
    return r == 1 ? "PCM mixer playing" : r ==
        2 ? "engine play-sound handle (unverified)" :
        "NOT played (mixer refused)";
}

#define PLOG(...) do { if (LOGCAP(g_mpn.nlog, 1200)) Xlog(__VA_ARGS__); } while (0)
static int MpnOff(void)
{
    static int v = -1;
    static uint64_t at;
    uint64_t now = GetTickCount64();
    if (v < 0 || now - at >= 2000) {
        at = now ? now : 1;
        v = GetFileAttributesA("t2r-no-duel-pin.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    return v;
}

static void MpnInit(void)
{
    if (g_mpn.row_init)
        return;
    g_mpn.row_init = 1;
    for (size_t i = 0; i < MPN_ROWS_N; i++)
        g_mpn.row_cd[i] = CdPairOfMove(Fnv64Rt(MPN_ROWS[i].pair));
}

// pins by the pair's name (cd_victim_begin: a kind 7 pair named mp-*)
// a wall / low-cover pin row only: the duel's parry pairs are mp- too (mp-1h-blunt-left-2h-parry-sync, mp-fisth-parry-sync ...) and ran the pin camera +
// pinned-victim rules on a parry (the camera went 1.6 m behind the attacker's shoulder for a parry)
static int MpnPairIsPin(int pr)
{
    if (pr < 0)
        return 0;
    const char *n = CD_PAIRS[pr].name;
    return !strncmp(n, "mp", 2) && !strstr(n, "parry")
        && (strstr(n, "wall-attack") || strstr(n, "low-cover"));
}

static int MpnActive(void)
{
    return g_mpn.on && g_mpn.slot >= 0 && g_mdu.slot == g_mpn.slot;
}

// ---- which pin set (male = Abby, female = Ellie, weapon = Ellie)
static int MpnEllieSet(int set)
{
    return set != MDU_FIST;
}

// ---- v5-pinfix3 2: a pin never runs with a held 1h / 2h melee weapon. The weapon sets' pins are Ellie-skeleton clips (CD_WPN_PAIR, ellie-1hblunt-hwall-*) on
// any body, 2h has no low-cover rows, 1h-blade only 1 cover / 2 wall steps, and the weapon's engine contacts (40 a blow) landed mid-pin (bat pin). The pin
// takes the character's own unarmed set instead (male = Abby's fists, female = Ellie's knife, the female knife rule of the duel), the weapon is put away instantly at
// PIN START (holster-player-weapon-instantly, as ctxdown's fist takedowns / the pin escape's female gun put-away do) and drawn back once the pin and its takedown are
// over (xr_finish: the unarmed-move restore with the forced draw + hand check). t2r-pin-keep-weapon.txt = the old weapon pins.
static int MpnKeepWeapon(void)
{
    static int v = -1;
    static uint64_t at;
    uint64_t now = GetTickCount64();
    if (v < 0 || now - at >= 2000) {
        at = now ? now : 1;
        v = GetFileAttributesA("t2r-pin-keep-weapon.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    return v;
}

// v5-wpnpin (SP-style weapon wall pins in the duel pin; re/sp-melee-run-1031.md "Weapon wall pin"): SP pins WITH the 1h weapon
// (1h-blunt-wall-attack-enter-right, ellie-1hblunt-hwall-combo-*), every strike wears it, the break on a strike (sfx-pipe-wrench-hit-break, ellie-1hblunt-drop-broken-weapon
// gesture ~180 ms later) ends THAT pin and the player re-pins with fists (fist-wall-attack-enter-right from 1.68 m, then the fist chain). Default now: a held 1h blunt / 1h
// blade pins with the weapon (rows: blunt wall enter x3 + 3 strikes, cover enter x3 + 3 strikes; blade wall enter x3 + 2 strikes, cover enter x3 + 1 strike; all Ellie-skeleton
// clips, the male body plays them too as before), every blow wears it (mpn_deliver -> mdw_blow), a break ends the weapon pin and the next press re-pins unarmed (mpn_repin).
// 2h / machete (no SP low-cover rows) keep the instant holster. t2r-pin-holster-weapon.txt = the instant-holster rule for every weapon; t2r-pin-keep-weapon.txt = every weapon incl. 2h.
static int MpnHolsterFile(void)
{
    static int v = -1;
    static uint64_t at;
    uint64_t now = GetTickCount64();
    if (v < 0 || now - at >= 2000) {
        at = now ? now : 1;
        v = GetFileAttributesA("t2r-pin-holster-weapon.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    return v;
}

static int MpnUnarmedSet(int set)
{
    if (set == MDU_FIST || set == MDU_KNIFE || MpnKeepWeapon())
        return set;
    if ((set == MDU_1HBLUNT || set == MDU_1HBLADE) && !MpnHolsterFile())
        return set;             // v5-wpnpin: the 1h weapon pins itself
    return OwnFemale()? MDU_KNIFE : MDU_FIST;
}

static struct {
    uint64_t w, end_ms, at;
    int pend, tries;
} g_mpnStow;                   // the melee weapon put away for the running pin; pend = draw it back after the pin
static uint64_t MpnWeapon(void)
{                               // the pin's weapon: the fists while ours is put away / broken
    if (g_mpnStow.pend && g_mpnStow.w)
        return FNV64("abby-unarmed");
    if (g_mpn.on && g_mpn.wbroke)
        return FNV64("abby-unarmed");
    uint64_t w = DcWeapon();
    if (g_mpn.on && g_mpn.wpn && w == FNV64("abby-unarmed"))
        return g_mpn.wpn;       // v5-wpnpin: dc_weapon reads the fists mid-pair (dlog u6b97) - the pin's own weapon
    return w;
}

static void MpnStowWeapon(int set, int uset)
{
    if (uset == set)
        return;
    uint64_t none0 = 0, z[2] = { 0, 0 }, held =
        DcCall(FNV64("get-player-weapon-id"), 0, &none0), cw =
        CurrentWeapon();
    uint64_t w = held && held != ~0ULL
        && MeleeClassOf(held, NULL) >= 0 ? held : cw && cw != ~0ULL
        && MeleeClassOf(cw, NULL) >= 0 ? cw : 0;
    uint64_t r = (g_holsterInstFrame =
                  g_frame ? g_frame : 1,
                  DcCall(FNV64("holster-player-weapon-instantly"), 2, z)),
        after = DcCall(FNV64("get-player-weapon-id"), 0, &none0);
    if (w) {
        g_mpnStow.w = w;
        g_mpnStow.pend = 1;
        g_mpnStow.tries = 0;
        g_mpnStow.end_ms = 0;
        g_mpnStow.at = GetTickCount64();
    }
    DULOG
        ("DUEL: PIN with a melee weapon (%llx, held %llx, set %d): put away instantly (holster %llx, held now %llx) - the pin runs on %s, the weapon is drawn back after it",
         (unsigned long long) w, (unsigned long long) held, set,
         (unsigned long long) r, (unsigned long long) after,
         uset ==
         MDU_KNIFE ? "Ellie's knife set (female life)" :
         "Abby's fist set (male life)");
}

// every frame (mpn_tick): the stowed weapon stays away while the pin runs, and comes back once the pin and our pair half (its takedown) are over
static void XrFinish(const char *why);
static void MpnStowTick(uint64_t now)
{
    if (!g_mpnStow.pend || !g_mpnStow.w)
        return;
    uint64_t none0 = 0;
    if (g_mpn.on) {
        uint64_t held = DcCall(FNV64("get-player-weapon-id"), 0, &none0);
        if (held == g_mpnStow.w && g_mpnStow.tries < 3) {
            uint64_t z[2] = { 0, 0 }, r = (g_holsterInstFrame =
                                           g_frame ? g_frame : 1,
                                           DcCall(FNV64
                                                   ("holster-player-weapon-instantly"),
                                                   2, z));
            g_mpnStow.tries++;
            DULOG
                ("DUEL: PIN: the stowed weapon %llx showed up in the hand mid-pin - put away again (holster %llx, try %d)",
                 (unsigned long long) held, (unsigned long long) r,
                 g_mpnStow.tries);
        }
        return;
    }
    if (!g_mpnStow.end_ms)
        g_mpnStow.end_ms = now;
    if (g_downed || g_downDying || LocalHp() <= 0.0f
        || MatchSubstate() != 6) {
        DULOG("DUEL: PIN stowed weapon %llx not drawn back: downed / dead",
              (unsigned long long) g_mpnStow.w);
        g_mpnStow.pend = 0;
        return;
    }
    int busy = g_ps.role == 1 || g_xrOn || g_unarmExec || g_execUntil
        || GxRunning();
    if (busy && now - g_mpnStow.end_ms < 8000)
        return;                 // the pin's surface takedown (our pair half) plays first
    uint64_t w = g_mpnStow.w;
    g_mpnStow.pend = 0;
    if (busy) {
        DULOG
            ("DUEL: PIN stowed weapon %llx: still busy 8 s after the pin (psync role %d, xr %d, unarmed %d) - left for the engine's own pick",
             (unsigned long long) w, g_ps.role, g_xrOn, g_unarmExec);
        return;
    }
    g_unarmExec = 1;
    g_unarm_weapon = w;
    g_unarm_until = g_t60;      // the unarmed-move restore (forced draw, verified 0.5 s later)
    XrFinish("duel pin over: the melee weapon put away for it");
    DULOG
        ("DUEL: PIN over %llu ms ago: stowed weapon %llx drawn back (held now %llx)",
         (unsigned long long) (now - g_mpnStow.end_ms),
         (unsigned long long) w,
         (unsigned long long) DcCall(FNV64("get-player-weapon-id"), 0,
                                      &none0));
}

static int MpnRows(int set, int surf, int kind, int step, int *out,
                    int max)
{
    MpnInit();
    int n = 0;
    for (size_t i = 0; i < MPN_ROWS_N && n < max; i++)
        if (MPN_ROWS[i].set == set && MPN_ROWS[i].surf == surf
            && MPN_ROWS[i].kind == kind && (kind == MPN_ENTER
                                            || MPN_ROWS[i].step == step)
            && g_mpn.row_cd[i] >= 0 && CdPairLoaded(g_mpn.row_cd[i]))
            out[n++] = (int) i;
    return n;
}

static int MpnMaxStep(int set, int surf)
{
    int m = 0;
    for (int s = 1; s <= 4; s++) {
        int r[1];
        if (MpnRows(set, surf, MPN_BLOW, s, r, 1))
            m = s;
        else
            break;
    }
    return m;
}

static float MpnDmg(int k)
{
    (void) k;
    // night58 decision: the damage follows the set that plays - a fist / knife pin (2h weapon put away, gun in the hand, empty hands, a female's knife) is
    // unarmed through and through: FIST_DAMAGE (the female knife = her fists, f0af15c7). A 1h weapon pinning with its own set keeps its table value.
    if ((g_mpn.set == MDU_FIST || g_mpn.set == MDU_KNIFE)
        && GetFileAttributesA("t2r-pin-fin-weapon-style.txt") ==
        INVALID_FILE_ATTRIBUTES) {
        static uint32_t lk;
        if (lk != (uint32_t) g_mpn.k) {
            lk = (uint32_t) g_mpn.k;
            DULOG
                ("DUEL: PIN damage: the pin runs on the %s set - unarmed blow %.0f (not the stowed / owned weapon's)",
                 g_mpn.set == MDU_KNIFE ? "knife" : "fist",
                 (double) FIST_DAMAGE);
        }
        return FIST_DAMAGE;
    }
    uint64_t w = g_mpnStow.pend && g_mpnStow.w ? g_mpnStow.w : MpnWeapon(); // the class of the pinner's melee (its own 1h set)
    return MeleeDmgW(w);
}

// ---- HOOK: the final blow. The default is the duel's lethal path: the engine's contact + the normal Melee Down take it and ctxdown's wall takedown (g_cd plan) plays.
// Branch `finish` replaces the body (the pin keeps ap / surface / set in g_mpn; slot = the victim).
// the takedown plan the pin started from (the scan drops plans while a pair runs): its wall / low-cover takedown is the pin's finisher
static struct {
    int ok, pair, src, kind, step, slot;
    uint64_t end_ms;
    float p0, top, fit, V[3], ap[7], me[3];
} g_mpnFin;
static void MpnFinSave(void)
{
    g_mpnFin.ok = g_cd.plan_ok;
    g_mpnFin.pair = g_cd.plan_pair;
    g_mpnFin.src = g_cd.plan_src;
    g_mpnFin.kind = g_cd.plan_kind;
    g_mpnFin.step = g_cd.plan_step;
    g_mpnFin.p0 = g_cd.plan_p0;
    g_mpnFin.top = g_cd.plan_top;
    g_mpnFin.fit = g_cd.plan_fit;
    memcpy(g_mpnFin.V, g_cd.plan_V, sizeof g_mpnFin.V);
    memcpy(g_mpnFin.ap, g_cd.plan_ap, sizeof g_mpnFin.ap);
    memcpy(g_mpnFin.me, g_cd.plan_me, sizeof g_mpnFin.me);
}

// the saved plan back into ctxdown's plan (bodies as they stand now; cd_try re-checks them)
static void MpnFinPlan(int slot, uint64_t vh)
{
    float vb[16], mb[16];
    const float *vl = vh ? PuppetLocator(vh, vb) : NULL, *me =
        PlayerLocator(mb);
    g_cd.plan_ok = 1;
    g_cd.plan_frame = g_frame;
    g_cd.plan_slot = slot;
    g_cd.plan_pair = g_mpnFin.pair;
    g_cd.plan_src = g_mpnFin.src;
    g_cd.plan_kind = g_mpnFin.kind;
    g_cd.plan_step = g_mpnFin.step;
    g_cd.plan_p0 = g_mpnFin.p0;
    g_cd.plan_top = g_mpnFin.top;
    g_cd.plan_fit = g_mpnFin.fit;
    memcpy(g_cd.plan_ap, g_mpnFin.ap, sizeof g_mpnFin.ap);
    if (vl)
        memcpy(g_cd.plan_V, vl, sizeof g_cd.plan_V);
    else
        memcpy(g_cd.plan_V, g_mpnFin.V, sizeof g_cd.plan_V);
    if (me)
        memcpy(g_cd.plan_me, me, sizeof g_cd.plan_me);
    else
        memcpy(g_cd.plan_me, g_mpnFin.me, sizeof g_cd.plan_me);
    // v5-pinfix3 2: the plan was found with the melee weapon in hand (a weapon takedown); the pin put it away, cd_try would refuse the weapon pair ("the plan's pair
    // is for style N"). The same surface frame takes the best-fitting loaded takedown of our style now (fists / knife) of the same kind.
    // night58: a pin on the fist / knife set finishes with that set's takedown (never the put-away / not-held weapon's): the style is forced for this frame's
    // pick and cd_try's re-check. A 1h weapon pinning with its own set keeps the weapon style. t2r-pin-fin-weapon-style.txt = the old pick.
    if ((g_mpn.set == MDU_FIST || g_mpn.set == MDU_KNIFE)
        && GetFileAttributesA("t2r-pin-fin-weapon-style.txt") ==
        INVALID_FILE_ATTRIBUTES) {
        int fs = g_mpn.set == MDU_KNIFE ? 3 : -1, was =
            (g_cd_style_force_on = 0, CdCurStyle());
        g_cd_style_force = fs;
        g_cd_style_force_on = 1;
        g_cdStyleForceFrame = g_frame;
        if (was != fs)
            DULOG
                ("DUEL: PIN finisher style: the pin ran on the %s set - its takedown comes from that set (style %d), not the weapon style %d",
                 fs == 3 ? "knife" : "fist", fs, was);
    }
    {
        int cur = CdCurStyle();
        if (cur != -2 && !CdStyleOk(cur, CD_PAIRS[g_cd.plan_pair].style)
            && vl && me) {
            float V[3] = { vl[0], vl[1], vl[2] }, best = 1e9f;
            int bi = -1;
            float dw =
                __builtin_sqrtf((vl[0] - g_cd.plan_ap[0]) * (vl[0] -
                                                             g_cd.
                                                             plan_ap[0]) +
                                (vl[2] - g_cd.plan_ap[2]) * (vl[2] -
                                                             g_cd.
                                                             plan_ap[2]));
            for (size_t i = 0; i < CD_N; i++) {
                if (CD_PAIRS[i].kind != g_cd.plan_kind
                    || !CdStyleOk(cur, CD_PAIRS[i].style)
                    || !CdPairLoaded((int) i)
                    || dw > CD_PAIRS[i].dmax + 0.3f)
                    continue;
                float mv, da, sa[3], vy;
                if (!CdSpotsAt
                    ((int) i, vh, g_cd.plan_ap, V, me, 0.0f, &mv, &da, sa,
                     &vy) || mv > CdVicMax((int) i)
                    || da > CD_ATT_MOVE_MAX)
                    continue;
                float sc = CdVariantScore(mv, da);
                if (sc < best) {
                    best = sc;
                    bi = (int) i;
                }
            }
            DULOG
                ("DUEL: PIN takedown plan %s is for style %d, we hold style %d now (the weapon is put away): %s%s",
                 CD_PAIRS[g_cd.plan_pair].name,
                 CD_PAIRS[g_cd.plan_pair].style, cur,
                 bi >=
                 0 ? "the same surface takes " :
                 "no takedown of our style fits - cd_try refuses, plain down",
                 bi >= 0 ? CD_PAIRS[bi].name : "");
            if (bi >= 0) {
                g_cd.plan_pair = bi;
                g_cd.plan_p0 = 0.0f;
            }
        }
    }
    // (both screens): the takedown's authored start had the attacker 1.25 m back from where the pin left it - a 1.25 m slide in 0.35 s.
    // From a pin the takedown starts where its attacker is nearest to us now (the first 0.6 s of the clip, 50 ms steps; the earliest within 0.15 m
    // of the best wins).
    if (vl && me) {
        float V[3] = { vl[0], vl[1], vl[2] }, best = 1e9f, bp =
            g_cd.plan_p0, d0 = -1.0f;
        float dist[13];
        for (int i = 0; i <= 12; i++) {
            float mv, da, sa[3], vy;
            dist[i] =
                CdSpotsAt(g_cd.plan_pair, vh, g_cd.plan_ap, V, me,
                            0.05f * i, &mv, &da, sa, &vy) ? da : 1e9f;
            if (dist[i] < best)
                best = dist[i];
            if (!i)
                d0 = dist[i];
        }
        for (int i = 0; i <= 12; i++)
            if (dist[i] <= best + 0.15f) {
                bp = 0.05f * i;
                break;
            }
        if (best < 1e8f && bp > g_cd.plan_p0) {
            DULOG
                ("DUEL: PIN takedown %s starts %.2f s in (our spot %.2f m off there, %.2f m at its start)",
                 CD_PAIRS[g_cd.plan_pair].name, (double) bp,
                 (double) dist[(int) (bp / 0.05f + 0.5f)], (double) d0);
            g_cd.plan_p0 = bp;
        }
    }
}

// a pin ran to its last blow, the next free punch downed the victim plainly - the takedown scan drops its plans while a pair runs, so the
// lethal swing found none. For 3 s after a pin on this slot ends, the pin's saved plan stands in (melee_duel.c's surface-finisher fallback).
static int MpnFinRestore(int slot)
{
    if (!g_mpnFin.ok || g_mpnFin.slot != slot || !g_mpnFin.end_ms
        || GetTickCount64() - g_mpnFin.end_ms > 3000)
        return 0;
    MpnFinPlan(slot, PuppetHandle((uint32_t) slot));
    return 1;
}

static int CdTry(int32_t dslot);
static void MpnDeliver(int s, int k, const char *why);
// the swing clip of a blow row: the move record's clip when the engine has one, else the row's own SP clip name (the low-cover / wall pin
// moves have no character-melee-info, mstag_clip_of gave 0, the victim got "no clip for the move here" and saw no punch)
static uint64_t MpnSwingClip(const MpnRowT *r, uint64_t mv)
{
    uint64_t c = mv ? MstagClipOf(mv) : 0;
    return c ? c : r->swing && r->swing[0] ? Fnv64Rt(r->swing) : 0;
}

static void MpnFinisherHook(int slot, uint64_t vh, const char *why)
{
    // Finishers from pins did not play out on either side: the hook only logged - the pin ended, the next press was an open-ground duel
    // swing and every mf- finisher was refused by the path gate (the wall the victim is pinned on). SP: the lethal pinned blow is the surface's own takedown.
    // The pin's held pair is let go and ctxdown starts the takedown of the plan the pin started from (bodies re-checked by cd_try); if it refuses, the
    // blow lands as a plain down hit.
    int ok = 0;
    if (g_mpnFin.ok) {
        if (g_ps.role == 1)
            PsyncAbort("pin finisher", 1);
        MpnFinPlan(slot, vh);
        ok = CdTry(slot);
        if (!ok)
            g_cd.plan_ok = 0;
    }
    if (!ok)
        MpnDeliver(slot, g_mpn.k,
                    "pin finisher refused - plain down hit");
    DULOG("DUEL: PIN final blow on slot %d (%s): %s", slot, why,
          ok ? "the surface takedown started (ctxdown)" : g_mpnFin.
          ok ? "takedown refused (see ctxdown) - plain down hit" :
          "no takedown plan saved - plain down hit");
    if (ok)
        return;

    g_mhrPunchThud = 1;
    MhrPunchAt(vh, 2, "duel pin lethal blow");
    g_mhrPunchThud = 0;
}

// ctxdown.c cd_try: a down hit on the pinned victim while our pin pair still plays (at 80 ms, knife pin: the engine's contact downed the
// victim between two pin blows - "not eligible now (in a paired move)" - plain knockdown, no finisher). The pin gives way to its surface takedown.
// 1 = the takedown started (or the plain down was delivered by the hook), 0 = not our pin.
static void MpnEnd(const char *why, int notify);
static int MpnDownTakeover(int slot)
{
    static int in;
    if (in || !g_mpn.on || g_mpn.slot != slot || g_ps.role != 1)
        return 0;
    in = 1;
    MpnFinisherHook(slot, PuppetHandle((uint32_t) slot),
                      "a down hit landed during the pin");
    MpnEnd("lethal blow", 1);
    in = 0;
    return 1;
}

static uint64_t g_mpnEndMs;
static int g_mpnEndSlot = -1;
static int MpnContactWindow(uint32_t slot)
{
    return (g_mpn.on && g_mpn.slot == (int) slot)
        || (g_mpnEndSlot == (int) slot
            && GetTickCount64() - g_mpnEndMs < 900);
}

static void MpnEnd(const char *why, int notify)
{
    if (!g_mpn.on)
        return;
    g_mpnEndMs = GetTickCount64();
    g_mpnEndSlot = strcmp(why, "lethal blow") ? g_mpn.slot : -1;      // the lethal end's engine contact must land (it carries the Melee Down)
    DULOG("DUEL: PIN END (%s) after %d blow(s) on slot %d", why, g_mpn.k,
          g_mpn.slot);
    g_mpnFin.end_ms = GetTickCount64();
    if (notify && g_mpn.slot >= 0)
        MduSend(MDPO_END, NULL, 0);
    if (g_mpn.slow_clip) {
        mstag_speed(g_mpn.slow_clip, 1.0f);
        g_mpn.slow_clip = 0;
        g_mpnWup.clip = 0;
    }
    PadinjBlock("melee-attack", 0);
    g_mpn.on = 0;
    g_mpn.state = 0;
    g_mpn.slot = -1;
    g_mpn.buf_at = 0;
    g_mpn.wpn = 0;
    g_mpn.wbroke = 0;
}

// the damage of blow k (0 based) through the normal hit path
static void MpnDeliver(int s, int k, const char *why)
{
    uint8_t *peer = MdgPeer((uint32_t) s);
    if (!peer)
        return;
    float dmg = MpnDmg(k);
    {
        int mc = MeleeClassOf(MpnWeapon(), NULL);
        BbMeleeNote(s, mc == MC_1HBLADE || mc == MC_MACHETE);
    }                           // bloodbuild: head masks for a blunt pin blow
    g_mdu.synthing = 1;
    g_mdmgAuthored = 1;        // (v5-dmg: the table's value under the fists' id - not the raw fist 20 the conversion would cut to 15)
    DirecthitHook(peer, MpnWeapon(), 0, dmg, 0);
    g_mdu.synthing = 0;
    g_mdmgAuthored = 0;
    {
        uint64_t pw = MpnWeapon();
        if (pw != FNV64("abby-unarmed") && MeleeDurW(pw)) {   // v5-pinfix3 3: a weapon pin blow wears it
            int h0 = WbHold();
            MdwBlow("pin blow", 400);
            // v5-wpnpin: SP's wrench dur 1 -> 0 on strike-1-hit: break sfx now, drop gesture after this -hit pair (wpnbreak.c), THAT pin is over (mpn_blow_contact sees wbroke)
            if (!h0 && WbHold() && g_mpn.on && g_mpn.wpn) {
                g_mpn.wbroke = 1;
                DULOG
                    ("DUEL: PIN WEAPON BROKE on blow %d (%llx): the break sfx plays now, the drop-broken gesture after this pair (wpnbreak.c); this weapon pin is over, the next press re-pins unarmed",
                     k + 1, (unsigned long long) pw);
            }
        }
    }
    EvHitNote((uint32_t) s);
    DULOG
        ("DUEL: PIN damage %.0f (blow %d, the melee table's value for the pinner's class) to slot %d (%s)",
         (double) dmg, k + 1, s, why);
}

// start the pair `row` of the pin on slot s (ap = the pin's wall frame). 1 = started
static int MpnPlay(int row, int s, uint64_t h, float *ap, const char *tag)
{
    int cd = g_mpn.row_cd[row];
    uint64_t w = MpnWeapon(), mel = w == FNV64("abby-unarmed") ? 0 : w;
    float vl_dur = PsyncDur(h, Fnv64Rt(CD_VIC0(cd))), end_s =
        vl_dur > 0.3f ? vl_dur - 0.15f : 1.2f;
    if (end_s > 1.5f)
        end_s = 1.5f;
    if (end_s < 0.7f)
        end_s = 0.7f;
    if (g_ps.role == 2) {
        DULOG
            ("DUEL: PIN %s refused: we are the victim half of a running pair (an escape) - our press must not cut it",
             tag);
        return 0;
    }
    {
        uint64_t me0 = SID_PLAYER;
        DcCall(0x394888944e949d5fULL /* cancel-melee-action */ , 1, &me0);
    }
    g_psStartSkipMs = 0.0f;
    if (!PsyncStart
        (PS_CTXDOWN, (uint32_t) s, CD_ATT0(cd), CD_VIC0(cd), ap, end_s, 0,
         0xff, mel, Fnv64Rt(CD_PAIRS[cd].name)))
        return 0;
    uint64_t now = GetTickCount64();
    g_mpnPairEnd = now + (uint64_t) (end_s * 1000.0f);        // the pair's hand-off (the press window runs past it: mpn_win_end)
    g_cd.ended = 0;
    g_cd.plan_ok = 0;
    g_mdu.pair_on = 1;
    g_mdu.pair_role = 1;
    g_mdu.ps_uid = g_ps.uid;
    g_mdu.pair_row = cd;
    g_mdu.pair_ms = now;
    g_mdu.hit_ms = now;
    g_mdu.act_ms = now;
    g_mdu.n_blows++;
    CdHandsBegin(cd);
    g_visVicAnim = CD_VIC0(cd);
    g_vis_vic_puppet = h;
    g_vis_vic_frame = g_t60;
    g_mpose_att_frame = g_vis_vic_frame;
    g_mposeAttSlot = (int32_t) s;
    FinposBegin(1, s, 0, S60(end_s) + 60, "melee duel pin, attacker");
    MduContactCues(now, h, w, MPN_ROWS[row].set, 1);
    DULOG
        ("DUEL: PIN %s: pair %s = %s on us + %s on slot %d, hand-off %.2f s",
         tag, CD_PAIRS[cd].name, CD_ATT0(cd), CD_VIC0(cd), s,
         (double) end_s);
    return 1;
}

// v5-contact3 (every hit has to connect): the push variant the contact will pick, predicted at OUR swing start, sent in the SWING message
// (melee_duel.c byte 8 = MPN row + 1). The victim screen warps the puppet to that push pair's authored spacing instead of the duel pair's (A/B:
// B warped to 1.02 m, the push pair needs 0.50 m - at the impact the roots were still 0.75 m apart). Only when every mpn_contact check passes now;
// our position is taken where the duel warp puts us at the contact (the line to the victim at sw_D). -1 = no pin expected / opt-out
// t2r-duel-contact-no-pinhint.txt. mpn_contact prefers the hinted row when it still fits the bodies, so both screens play the same push.
static int g_mpnHintRow = -1, g_mpn_hint_slot = -1;
static int MpnHintOff(void)
{
    static int v = -1;
    static uint64_t at;
    uint64_t now = GetTickCount64();
    if (v < 0 || now - at >= 2000) {
        at = now ? now : 1;
        v = GetFileAttributesA("t2r-duel-contact-no-pinhint.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    return v;
}

static int MpnSteady(void)
{
    return g_cdPlanStreak >= 4 || (g_cdPlanSteadyFrame
                                     && g_frame - g_cdPlanSteadyFrame <=
                                     90) || (g_cdPlanAgreeFrame
                                             && g_frame -
                                             g_cdPlanAgreeFrame <=
                                             CD_AGREE_LIFE
                                             &&
                                             GetFileAttributesA
                                             ("t2r-pin-steady-ps34.txt") ==
                                             INVALID_FILE_ATTRIBUTES);
}                               // (or two plans in a row on one plane, ctxdown.c cd_plan_commit)   // the streak resets under a running combo pair

static int MpnPredict(int set, int s, uint64_t h, float sw_D)
{
    g_mpnHintRow = -1;
    g_mpn_hint_slot = -1;
    if (!g_mdu.on || MpnOff() || MpnHintOff() || g_mpn.on || !h
        || !g_cd.plan_ok || g_cd.plan_slot != s)
        return -1;
    set = MpnUnarmedSet(set); // v5-pinfix3 2: a held melee weapon pins with the unarmed set
    int surf =
        g_cd.plan_kind == 1 ? MPN_WALL : g_cd.plan_kind ==
        2 ? MPN_COVER : -1;
    if (surf < 0 || g_frame - g_cd.plan_frame > 90
        || GetTickCount64() < g_mpn.esc_cool[s] || !MpnSteady())
        return -1;
    float vb[16], mb[16];
    const float *vl = PsyncLoc(h, vb), *me0 = PlayerLocator(mb);
    if (!vl || !me0 || !PsLocOk(vl) || !PsLocOk(me0))
        return -1;
    float dx = me0[0] - vl[0], dz = me0[2] - vl[2], dl =
        __builtin_sqrtf(dx * dx + dz * dz);
    if (dl < 0.05f)
        return -1;
    float me[16];
    memcpy(me, me0, sizeof(float) * 8);
    me[0] = vl[0] + dx / dl * sw_D;
    me[2] = vl[2] + dz / dl * sw_D;     // where mdu_warp puts us at the contact
    float vd =
        __builtin_sqrtf((vl[0] - g_cd.plan_V[0]) * (vl[0] -
                                                    g_cd.plan_V[0]) +
                        (vl[2] - g_cd.plan_V[2]) * (vl[2] -
                                                    g_cd.plan_V[2]));
    float ad =
        __builtin_sqrtf((me[0] - g_cd.plan_me[0]) * (me[0] -
                                                     g_cd.plan_me[0]) +
                        (me[2] - g_cd.plan_me[2]) * (me[2] -
                                                     g_cd.plan_me[2]));
    if (vd > 0.6f || ad > 1.0f)
        return -1;
    int en[8], n = MpnRows(set, surf, MPN_ENTER, 0, en, 8), ms =
        MpnMaxStep(set, surf);
    if (!n || !ms)
        return -1;
    float V[3] = { vl[0], vl[1], vl[2] }, best = 1e9f;
    int bi = -1;
    for (int i = 0; i < n; i++) {
        float mv, da, sa[3], vy;
        if (!CdSpotsAt
            (g_mpn.row_cd[en[i]], h, g_cd.plan_ap, V, me, 0.0f, &mv, &da,
             sa, &vy))
            continue;
        float sc = CdVariantScore(mv, da);
        if (mv <= MPN_VIC_MAX && da <= MPN_ATT_MAX && sc < best) {
            best = sc;
            bi = en[i];
        }
    }
    if (bi >= 0) {
        g_mpnHintRow = bi;
        g_mpn_hint_slot = s;
        PLOG("DUEL: PIN expected at this swing's contact on slot %d: push %s (hint in the SWING for the victim screen's warp)", s, MPN_ROWS[bi].name);
    }
    return bi;
}

// the victim screen: the hinted push row's -hit pair (CD_PAIRS index), -1 = none
static int MpnHintCd(int row1)
{
    MpnInit();
    int r = row1 - 1;
    return r >= 0 && r < (int) MPN_ROWS_N
        && MPN_ROWS[r].kind == MPN_ENTER ? g_mpn.row_cd[r] : -1;
}

// ---- HOOK (mdu_contact): the duel blow has contact. 1 = the pin took it (push pair + damage + pin state)
// cone search: the angle (deg) between the plan's wall frame facing (out of the wall) and the line victim -> attacker at the contact = how far off head-on the
// blow came; the plan's own angle is the surface normal vs the attack direction at the scan (ctxdown.c g_cd_plan_ang). Logged in the pin plan lines.
static float MpnPlanOff(const float *vl, const float *me)
{
    float ay = CdApYaw(g_cd.plan_ap) * 0.0174533f, fx =
        __builtin_sinf(ay), fz = __builtin_cosf(ay), vx =
        me[0] - vl[0], vz = me[2] - vl[2], l =
        __builtin_sqrtf(vx * vx + vz * vz);
    return l >
        0.05f ?
        __builtin_acosf(fmaxf(-1.0f, fminf(1.0f, (vx * fx + vz * fz) / l)))
        * 57.29578f : 0.0f;
}

static int MpnContact(uint64_t now, int set, int s, uint64_t h,
                       const float *vl, const float *me)
{
    if (!g_mdu.on || MpnOff() || g_mpn.on || !vl || !me || !g_cd.plan_ok
        || g_cd.plan_slot != s) {
        static int nl;
        if (LOGCAP(nl, 400))
            DULOG
                ("pin plan: blow %d slot %d NO PIN: %s (plan_ok %d plan_slot %d, scan: %s)",
                 g_mdu.step, s,
                 !g_mdu.on ? "no duel" : MpnOff()? "pins off" : g_mpn.
                 on ? "a pin already runs" : !vl
                 || !me ? "no locators" : !g_cd.
                 plan_ok ?
                 "no wall / car / cover plan behind the victim (surface ray found nothing, or beyond SP's 1.5 m)"
                 : "plan is for another slot", g_cd.plan_ok,
                 g_cd.plan_slot, g_cd.note);
        return 0;
    }
    int set0 = set;
    set = MpnUnarmedSet(set); // v5-pinfix3 2: a held melee weapon pins with the unarmed set (put away at the push)
    int surf =
        g_cd.plan_kind == 1 ? MPN_WALL : g_cd.plan_kind ==
        2 ? MPN_COVER : -1;
    if (surf < 0 || g_frame - g_cd.plan_frame > 90) {
        static int nl;
        if (LOGCAP(nl, 400))
            DULOG
                ("pin plan: blow %d slot %d NO PIN: plan kind %d (1 wall, 2 cover; else a takedown-only surface) age %d frames (max 90)",
                 g_mdu.step, s, g_cd.plan_kind,
                 (int) (g_frame - g_cd.plan_frame));
        return 0;
    }
    // a simultaneous trade - their blow's hit reaction recoiled us and the pin started 1.25 m off, then our body was held 2.3 m away
    if (MhrRunning()) {
        static int nl;
        if (LOGCAP(nl, 30))
            DULOG
                ("DUEL: PIN not started: our own hit reaction is running (simultaneous trade) - plain blow");
        return 0;
    }
    if (now < g_mpn.esc_cool[s]) {
        static int nl;
        if (LOGCAP(nl, 30))
            DULOG
                ("DUEL: PIN not started: slot %d escaped a pin %llu ms ago (8 s no re-pin)",
                 s,
                 (unsigned long long) (now + MPN_ESC_COOL_MS -
                                       g_mpn.esc_cool[s]));
        return 0;
    }
    if (!MpnSteady()) {
        static int nl;
        if (LOGCAP(nl, 30))
            DULOG
                ("DUEL: PIN not started: surface not steady (plan streak %d) - a tree / foliage, not a wall",
                 g_cdPlanStreak);
        return 0;
    }                           // a tree trunk passed the ray replica now and then - B got stuck in the tree
    float vd =
        __builtin_sqrtf((vl[0] - g_cd.plan_V[0]) * (vl[0] -
                                                    g_cd.plan_V[0]) +
                        (vl[2] - g_cd.plan_V[2]) * (vl[2] -
                                                    g_cd.plan_V[2]));
    float ad =
        __builtin_sqrtf((me[0] - g_cd.plan_me[0]) * (me[0] -
                                                     g_cd.plan_me[0]) +
                        (me[2] - g_cd.plan_me[2]) * (me[2] -
                                                     g_cd.plan_me[2]));
    if (vd > 0.6f || ad > 1.0f) {
        static int nl;
        if (LOGCAP(nl, 400))
            DULOG
                ("pin plan: blow %d slot %d NO PIN: bodies moved since the plan - victim %.2f m (max 0.6), us %.2f m (max 1.0) [wall frame %.0f deg off the blow line, plan angle %.0f]",
                 g_mdu.step, s, (double) vd, (double) ad,
                 (double) MpnPlanOff(vl, me), (double) g_cd_plan_ang);
        return 0;
    }
    {
        static int nla;
        if (LOGCAP(nla, 400))
            DULOG
                ("pin plan: blow %d slot %d: wall frame %.0f deg off the blow line (victim -> us), surface normal %.0f deg off the attack direction at the scan (cone %.0f deg%s)",
                 g_mdu.step, s, (double) MpnPlanOff(vl, me),
                 (double) g_cd_plan_ang, (double) CD_CONE_DEG,
                 g_cdHeadon > 0 ? ", OFF: t2r-pin-headon.txt" : "");
    }
    int en[8], n = MpnRows(set, surf, MPN_ENTER, 0, en, 8), ms =
        MpnMaxStep(set, surf);
    if (!n || !ms) {
        PLOG("DUEL: PIN no %s pin set for style %d (push rows %d, blow rows %d)", surf == MPN_WALL ? "wall" : "cover", set, n, ms);
        return 0;
    }
    float V[3] = { vl[0], vl[1], vl[2] }, best = 1e9f, bmv = 0, bda =
        0, hmv = 0, hda = 0;
    int bi = -1, hb = -1;
    for (int i = 0; i < n; i++) {
        float mv, da, sa[3], vy;
        if (!CdSpotsAt
            (g_mpn.row_cd[en[i]], h, g_cd.plan_ap, V, me, 0.0f, &mv, &da,
             sa, &vy))
            continue;
        float sc = CdVariantScore(mv, da);
        if (mv <= MPN_VIC_MAX && da <= MPN_ATT_MAX && sc < best) {
            best = sc;
            bi = en[i];
            bmv = mv;
            bda = da;
        }
        if (en[i] == g_mpnHintRow && g_mpn_hint_slot == s
            && mv <= MPN_VIC_MAX && da <= MPN_ATT_MAX) {
            hb = en[i];
            hmv = mv;
            hda = da;
        }                       // (v5-contact3: the row the victim screen warped to)
    }
    if (hb >= 0 && hb != bi) {
        PLOG("DUEL: PIN push %s (sent in the SWING hint) instead of the best fit %s - the victim screen warped to it", MPN_ROWS[hb].name, bi >= 0 ? MPN_ROWS[bi].name : "-");
        bi = hb;
        bmv = hmv;
        bda = hda;
    }
    g_mpnHintRow = -1;
    g_mpn_hint_slot = -1;
    if (bi < 0) {
        PLOG("DUEL: PIN no push variant fits the bodies (%d tried, victim carry max %.1f m, ours %.1f m)", n, (double) MPN_VIC_MAX, (double) MPN_ATT_MAX);
        return 0;
    }
    float ap[7];
    memcpy(ap, g_cd.plan_ap, sizeof ap);
    MpnFinSave();
    g_mpnFin.slot = s;
    g_mpnFin.end_ms = 0;
    MpnStowWeapon(set0, set); // (a refused push: mpn_stow_tick draws it back next frame - no pin is on)
    if (!MpnPlay(bi, s, h, ap, "PUSH"))
        return 0;
    memcpy(g_mpn.ap, ap, sizeof ap);
    g_mpn.on = 1;
    g_mpn.slot = s;
    g_mpn.surf = surf;
    g_mpn.set = set;
    g_mpn.k = 1;
    g_mpn.maxb = 99;
    g_mpn.state = 1;
    g_mpn.last_hit = now;
    g_mpn.wpn = 0;
    g_mpn.wbroke = 0;
    if (set == MDU_1HBLUNT || set == MDU_1HBLADE) {     // v5-wpnpin: the weapon this pin runs with (the swing's own: dc_weapon reads the fists mid-pair)
        uint64_t bw = MduBlowWeapon();
        if (!bw || bw == FNV64("abby-unarmed"))
            bw = g_mdw.sw_w && now - g_mdw.sw_ms < 3000 ? g_mdw.sw_w : 0;
        g_mpn.wpn = bw;
        DULOG
            ("DUEL: PIN WEAPON PIN with %llx (set %d, durability %lld): SP's 1h wall / cover combo, every blow wears it, a break ends this pin (opt-out t2r-pin-holster-weapon.txt)",
             (unsigned long long) bw, set, (long long) MdwHp());
    }
    g_mpn.win_end = MpnWinEnd(now);
    g_mpn.buf_at = 0;
    g_mpn.esc_id = -1;
    g_mpn_np_seen = 0;
    g_mpn_np_used = 0;
    g_mpn_f_frames = g_mpn_unfoc_frames = g_mpn_cmd_frames = 0;
    // (the push deals no damage - the push is the grab; the blows do the damage)
    g_mdu.next_move = 0;
    g_mdu.win_end = 0;
    g_mdu.lock_until = 0;
    DULOG
        ("DUEL: PIN START on slot %d: %s, %s set (%s, %s), push %s (victim carried %.2f m, we %.2f m), up to %d blows; each next press opens a %d ms dodge QTE for the victim",
         s, surf == MPN_WALL ? "WALL" : "LOW COVER",
         MpnEllieSet(set) ? "Ellie's" : "Abby's",
         OwnFemale()? "female life" : "male life",
         set == MDU_FIST ? "fists" : set == MDU_KNIFE ? "knife" : "weapon",
         MPN_ROWS[bi].name, (double) bmv, (double) bda, g_mpn.maxb,
         MPN_WIN_MS);
    return 1;
}

static int MpnHoldAtt(void)
{
    return g_mpn.on;
}

// v5-pinpress 2: the next press is awaited until the running pin pair's hand-off + MPN_AFTER_MS (at least MPN_PRESS_WAIT_MS after the blow)
static uint64_t MpnWinEnd(uint64_t now)
{
    uint64_t a = now + MPN_PRESS_WAIT_MS, b =
        g_mpnPairEnd + MPN_AFTER_MS;
    return b > a ? b : a;
}

// t2r-ext.c adelta_hook: the pin blow swing window (its start .. 300 ms past its contact), when the per-frame step guard applies
static uint64_t g_mpn_swing_t0, g_mpn_swing_t1;
static int MpnSwingGuardOn(void)
{
    static int off = -1;
    static uint64_t at;
    uint64_t now = GetTickCount64();
    if (off < 0 || now - at >= 2000) {
        at = now;
        off =
            GetFileAttributesA("t2r-pin-no-step-guard.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    return !off && g_mpn.on && g_mpn_swing_t0 && now >= g_mpn_swing_t0 && now < g_mpn_swing_t1; // (a pin that ended - escape, END - is no pin swing any more)
}

// A pin lasts up to death if needed (it used to break after 4 wall hits and force another): the blows
// cycle through the set's combo steps (1..last, 1..last, ...) until the victim escapes, leaves the wall or the lethal blow plays its takedown
static int MpnRowStep(int k)
{
    int ms = MpnMaxStep(g_mpn.set, g_mpn.surf);
    if (ms > MPN_MAXB - 1)
        ms = MPN_MAXB - 1;
    return ms < 1 ? 0 : (k - 1) % ms + 1;
}

// v5-wpnpin: SP T=394004 - after the wrench broke the player pins AGAIN with fists (fist-wall-attack-enter-right, victim 1.68 m away, the attacker closes 0.89 m): the press after
// the break plays the unarmed push on the still-pinned victim from the same wall frame and the pin goes on as a fist / knife pin (k restarts, the weapon is gone).
// 1 = started. 0 = refused (the caller ends the pin).
static int MpnRepin(uint64_t now)
{
    int s = g_mpn.slot, uset = OwnFemale()? MDU_KNIFE : MDU_FIST;
    uint64_t h = PuppetHandle((uint32_t) s);
    float vb[16], mb[16];
    const float *vl = h ? PuppetLocator(h, vb) : NULL, *me =
        PlayerLocator(mb);
    if (!h || !vl || !me || !MduPartnerOk(s) || !PsLocOk(vl)
        || !PsLocOk(me)) {
        DULOG("DUEL: PIN RE-PIN refused: partner / locators gone");
        return 0;
    }
    if (g_ps.role && now + 100 < g_mpnPairEnd) {
        DULOG
            ("DUEL: PIN RE-PIN refused: the break's -hit pair is still running (%lld ms to its hand-off)",
             (long long) (g_mpnPairEnd - now));
        return 0;
    }
    int en[8], n = MpnRows(uset, g_mpn.surf, MPN_ENTER, 0, en, 8), ms =
        MpnMaxStep(uset, g_mpn.surf);
    if (!n || !ms) {
        DULOG("DUEL: PIN RE-PIN refused: no unarmed %s rows for style %d",
              g_mpn.surf == MPN_WALL ? "wall" : "cover", uset);
        return 0;
    }
    float V[3] = { vl[0], vl[1], vl[2] }, best = 1e9f, bmv = 0, bda = 0;
    int bi = -1;
    for (int i = 0; i < n; i++) {
        float mv, da, sa[3], vy;
        if (!CdSpotsAt
            (g_mpn.row_cd[en[i]], h, g_mpn.ap, V, me, 0.0f, &mv, &da, sa,
             &vy))
            continue;
        float sc = CdVariantScore(mv, da);
        if (mv <= MPN_VIC_MAX && da <= MPN_ATT_MAX && sc < best) {
            best = sc;
            bi = en[i];
            bmv = mv;
            bda = da;
        }
    }
    if (bi < 0) {
        DULOG
            ("DUEL: PIN RE-PIN refused: no unarmed push fits the bodies (%d tried; victim carry max %.1f m, ours %.1f m)",
             n, (double) MPN_VIC_MAX, (double) MPN_ATT_MAX);
        return 0;
    }
    float ap[7];
    memcpy(ap, g_mpn.ap, sizeof ap);
    g_mpn.wbroke = 0;
    g_mpn.wpn = 0;              // (mpn_weapon: dc_weapon = the fists once the item left the hand)
    if (!MpnPlay(bi, s, h, ap, "RE-PIN")) {
        g_mpn.wbroke = 1;
        return 0;
    }
    g_mpn.set = uset;
    g_mpn.k = 1;
    g_mpn.maxb = 99;
    g_mpn.state = 1;
    g_mpn.last_hit = now;
    g_mpn.win_end = MpnWinEnd(now);
    g_mpn.buf_at = 0;
    g_mpn.esc_id = -1;
    g_mdu.next_move = 0;
    g_mdu.win_end = 0;
    g_mdu.lock_until = 0;
    DULOG
        ("DUEL: PIN RE-PIN on slot %d after the weapon broke: %s set (%s), push %s (victim carried %.2f m, we %.2f m), the unarmed chain goes on",
         s, uset == MDU_KNIFE ? "knife" : "fists",
         OwnFemale()? "female life" : "male life", MPN_ROWS[bi].name,
         (double) bmv, (double) bda);
    return 1;
}

// ---- the attacker: a press starts the next blow
static void MpnBlowStart(uint64_t now)
{
    int s = g_mpn.slot, step = MpnRowStep(g_mpn.k);   // pin blow k (k = 1 is the first after the push) -> combo step (cycles)
    int bl[2];
    if (step < 1
        || !MpnRows(g_mpn.set, g_mpn.surf, MPN_BLOW, step, bl, 2)) {
        DULOG
            ("DUEL: PIN last blow done (%d): the attacker shoves off, the duel continues (%d ms gap)",
             g_mpn.k, MDU_GAP4_MS);
        g_mdu.lock_until = now + MDU_GAP4_MS;
        PadinjBlock("melee-attack", g_mdu.lock_until);
        MpnEnd("max blows", 1);
        return;
    }
    uint64_t h = PuppetHandle((uint32_t) s);
    uint8_t *vp = MdgPeer((uint32_t) s);
    float nh = vp && !IsBadReadPtr(vp + 0xa5, 4) ? *(float *) (vp + 0xa5) : -1.0f;      // the victim's NET hp (the puppet's own stays 100 here)
    float vhp = nh > 0.0f ? nh : h ? PuppetHp((uint32_t) s, h) : -1.0f;
    if (vhp > 0.0f && vhp - MpnDmg(g_mpn.k) <= 1.0f) {
        MpnFinisherHook(s, h, "the blow would down");
        MpnEnd("lethal blow", 1);
        return;
    }
    const MpnRowT *r = &MPN_ROWS[bl[0]];
    g_mpn.id = (g_mpn.id + 1) & 0xff;
    g_mpn.t_press = now;
    g_mpn.state = 2;
    g_mpn.buf_at = 0;
    if (!g_mpnStow.pend)
        MdwSwing(now);         // v5-pinfix3 3: the durability before this blow (weapon pins only)
    int tc = r->tc_ms;
    if (tc < MPN_WIN_MS)
        tc = MPN_WIN_MS;
    g_mpn.due = now + (uint64_t) (MpnWinOld()? MPN_WIN_MS : tc);
    g_mpn.swing_id = 0;         /* ps28: a clip longer than the window lands at its own contact */
    // v5-pinesc (A/B v5-int22, B 36477-36496): the victim's dodge ABORTed the pin pair, the next press started switchblade-wall-attack-3 with no pair on our body and the
    // engine's wall-swing align slid us 174 m in 16 frames to the map origin (out of the map, dead). 777 of 778 logged pin blows started inside the pair; the one
    // outside it is this. The wall swing is only valid as the pair's continuation: a cut pair ends the pin (the duel goes on). t2r-pin-blow-no-pair.txt = old rule.
    // v5-pinpress 2: a pair that ran out to its own hand-off (not cut: a cut one ends before g_mpn_pair_end) is no cut - the press after it starts the blow from
    // where both bodies stand (the swing's apReference is solved on our body, the next pair re-places both on the wall frame), if the victim is still at the wall.
    int held = g_mdu.pair_on && g_ps.role == 1
        && g_ps.ent_att == SID_PLAYER && g_ps.uid == g_mdu.ps_uid;
    if (!held && !PS_OURS && g_mpnPairEnd && now + 100 >= g_mpnPairEnd
        && GetFileAttributesA("t2r-pin-no-after-handoff.txt") ==
        INVALID_FILE_ATTRIBUTES) {
        float hb[16], hm[16];
        const float *hv = h ? PuppetLocator(h, hb) : NULL, *hme =
            PlayerLocator(hm);
        float d = hv
            && hme ? __builtin_sqrtf((hv[0] - hme[0]) * (hv[0] - hme[0]) +
                                     (hv[2] - hme[2]) * (hv[2] -
                                                         hme[2])) : 99.0f;
        float dw =
            hv ? __builtin_sqrtf((hv[0] - g_mpn.ap[0]) *
                                 (hv[0] - g_mpn.ap[0]) + (hv[2] -
                                                          g_mpn.ap[2]) *
                                 (hv[2] - g_mpn.ap[2])) : 99.0f;
        if (hv && hme && PsLocOk(hv) && PsLocOk(hme) && d <= 2.0f
            && dw <= 1.3f && MduPartnerOk(s)) {
            held = 1;
            DULOG
                ("DUEL: PIN blow %d after the pair's hand-off (%lld ms past it): bodies %.2f m apart, the victim %.2f m from the wall frame - the blow starts from here",
                 g_mpn.k + 1, (long long) (now - g_mpnPairEnd),
                 (double) d, (double) dw);
        } else {
            DULOG
                ("DUEL: PIN blow %d refused after the pair's hand-off: bodies %.2f m apart (max 2.0), the victim %.2f m from the wall frame (max 1.3)%s - the pin is over",
                 g_mpn.k + 1, (double) d, (double) dw,
                 MduPartnerOk(s) ? "" : ", partner gone");
            g_mpn.state = 1;
            MpnEnd("the victim left the wall after the hand-off", 1);
            return;
        }
    }
    if (!held
        && GetFileAttributesA("t2r-pin-blow-no-pair.txt") ==
        INVALID_FILE_ATTRIBUTES) {
        DULOG
            ("DUEL: PIN blow %d refused: the pin pair no longer holds us (cut by the victim: pair_on %d, psync role %d, %lld ms before its hand-off) - the wall swing would align off the map; the pin is over",
             g_mpn.k + 1, g_mdu.pair_on, g_ps.role,
             (long long) (g_mpnPairEnd >
                          now ? g_mpnPairEnd - now : 0));
        g_mpn.state = 1;
        MpnEnd("the pin pair was cut", 1);
        return;
    }
    PsyncOwnAttFree("pin blow swing");       // (blow 5 pressed during blow 4's release drive slid us to the map origin)
    uint64_t mv = Fnv64Rt(r->move), ret[2] = { 0, 0 }, args[8] =
        { SID_PLAYER, h, mv, 0, 0, 0, 0, 0 };
    // v5-pinpop (every first blow after the push): with the victim as the swing's target the engine ran its own synced hit ~330 ms in (move switch
    // 6d82.. -> 705b..) and placed us at its contact spot - a 0.8 m one-frame jump into the victim, 6 frames before our -hit pair starts anyway. The pin's
    // -hit pair carries the blow, so the swing has no target: no engine hit switch, the pair pulls us in from where the swing left us (0.45 s).
    // t2r-pin-swing-target.txt = old.
    if (GetFileAttributesA("t2r-pin-swing-target.txt") ==
        INVALID_FILE_ATTRIBUTES)
        args[1] = 0;
    if (g_maHave)
        for (int i = 3; i < 8; i++)
            args[i] = g_maCache[i];
    // v5-pinesc: start-melee-action's a3 / a7 are BoundFrame pointers (exe 0x140e79ec0: copied with the BoundFrame copy 0x14133da80 into the move's start params,
    // a7 defaults to a3). We passed 0 (g_ma_cache is never filled: player swings do not go through the native) and the wall / cover combo swings, whose clips
    // align on an apReference SP takes from the wall, got the params' default BoundFrame = the WORLD ORIGIN: the align carried our body there (A/B v5-int22 B
    // 36480: to -0.37 0.01 1.47; v5-int23 A 44954: 307.8 17.5 -146.9 scaled x0.62 toward 0 0 0 in 4 frames). The ap is solved on our own body (the swing's align
    // at phase 0 lands where we stand), so the swing moves us only by its own root motion. t2r-pin-swing-no-ap.txt = the old 0 args.
    static PsyncBf sw_bf;
    int sw_ap = 0;
    if (GetFileAttributesA("t2r-pin-swing-no-ap.txt") ==
        INVALID_FILE_ATTRIBUTES) {
        float lb[16], ap7[7];
        const float *ml = PlayerLocator(lb);
        uint64_t sclip = MpnSwingClip(r, mv);
        if (ml && PsLocOk(ml)) {
            if (sclip && PsyncSolveAp(SID_PLAYER, sclip, ml, ap7))
                sw_ap = 1;
            else {
                float y = Uc4Yaw(ml) * 0.0174533f;
                ap7[0] = ml[0];
                ap7[1] = ml[1];
                ap7[2] = ml[2];
                ap7[3] = 0;
                ap7[4] = __builtin_sinf(y * 0.5f);
                ap7[5] = 0;
                ap7[6] = __builtin_cosf(y * 0.5f);
                sw_ap = 2;
            }
            PsyncBfMake(&sw_bf, ap7);
            args[3] = (uint64_t) (uintptr_t) & sw_bf;
            args[7] = args[3];
            float dw =
                __builtin_sqrtf((ap7[0] - g_mpn.ap[0]) * (ap7[0] -
                                                          g_mpn.ap[0]) +
                                (ap7[2] - g_mpn.ap[2]) * (ap7[2] -
                                                          g_mpn.ap[2]));
            DULOG
                ("DUEL: PIN blow swing %s: apReference %.2f %.2f %.2f %s (%.2f m from the pin's wall ap)",
                 r->move, (double) ap7[0], (double) ap7[1],
                 (double) ap7[2],
                 sw_ap ==
                 1 ? "solved on our body from the swing clip's align" :
                 "= our body (clip align unreadable)", (double) dw);
        } else
            DULOG
                ("DUEL: PIN blow swing %s: our locator unreadable - no apReference passed",
                 r->move);
    }
    g_mpn_swing_t0 = now;
    g_mpn_swing_t1 = now + (uint64_t) MPN_WIN_MS + 300;
    if (g_startMeleeOrig && h && r->move[0])
        g_startMeleeOrig(ret, 8, args, 0);
    g_mpn.swing_id = ret[0] ? mv : 0;
    g_mpn.slow_clip = 0;
    if (ret[0]) {
        uint64_t clip = MpnSwingClip(r, mv);
        if (clip && r->tc_ms > 0) {
            float sp = (float) r->tc_ms / (float) MPN_WIN_MS;
            mstag_speed(clip, sp);
            g_mpn.slow_clip = clip;
            g_mpnWup.clip = 0;
            if (!MpnWinOld() && r->tc_ms < MPN_WIN_MS - 15) {
                g_mpnWup.clip = clip;
                g_mpnWup.t0 = now;
                g_mpnWup.tc = (float) r->tc_ms;
                g_mpnWup.x = (float) (MPN_WIN_MS - r->tc_ms);
                g_mpnWup.len = MdgClipLen(clip);
            }
        }
    }
    PadinjBlock("melee-attack", g_mpn.due + 200);
    g_mdu.hit_ms = now;
    g_mdu.act_ms = now;         // the engine's own contact of this swing is dropped (mdu_contact_drop: 600 ms after hit_ms)
    // [3] = the swing row, [4..11] = its clip: the victim plays it on our puppet (a victim with another move set could not look the move up - "no clip for the move here")
    int winv = MpnWinOld()? MPN_WIN_MS : tc;  /* ps28: the victim's window ends at THIS blow's contact (a 400 ms clip lands at 400, not at the 290 ms window) */
    uint8_t p[12] =
        { (uint8_t) g_mpn.id, (uint8_t) winv, (uint8_t) (winv >> 8),
(uint8_t) (ret[0] ? bl[0] : 0xff) };
    {
        uint64_t sc = ret[0] ? MpnSwingClip(r, mv) : 0;
        memcpy(p + 4, &sc, 8);
    }
    MduSend(MDPO_BLOW, p, 12);
    DULOG
        ("DUEL: PIN blow %d/%d QTE OPEN (id %d): %s swing %s (%s), contact in %d ms (SP %d ms, windup slowed x%.2f), victim prompt Dodge",
         g_mpn.k + 1, g_mpn.maxb, g_mpn.id, r->name, r->move,
         ret[0] ? "started" : "refused - no visible windup", MPN_WIN_MS,
         r->tc_ms, r->tc_ms > 0 ? (double) r->tc_ms / MPN_WIN_MS : 1.0);
}

static void MpnBlowContact(uint64_t now)
{
    int s = g_mpn.slot, step = MpnRowStep(g_mpn.k), bl[2];
    g_mpn.state = 1;
    if (g_mpn.slow_clip) {
        mstag_speed(g_mpn.slow_clip, 1.0f);
        g_mpn.slow_clip = 0;
        g_mpnWup.clip = 0;
    }
    {
        uint64_t me0 = SID_PLAYER;
        DcCall(0x394888944e949d5fULL, 1, &me0);
    }
    if (g_mpn.esc_id == g_mpn.id) {
        DULOG
            ("DUEL: PIN blow %d (id %d) ESCAPED before the contact (the victim's valid QTE press reached us): no pair, no damage - its counter follows",
             step + 1, g_mpn.id);
        MpnEnd("escaped", 0);
        return;
    }                           // (the pin stayed on, a mashed press started another blow outside the pair and its align dragged us off the map)
    uint64_t h = PuppetHandle((uint32_t) s);
    float vb[16], mb[16];
    const float *vl = h ? PuppetLocator(h, vb) : NULL, *me =
        PlayerLocator(mb);
    if (!MduPartnerOk(s) || !vl || !me) {
        MpnEnd("the partner is gone", 1);
        return;
    }
    float d =
        __builtin_sqrtf((vl[0] - me[0]) * (vl[0] - me[0]) +
                        (vl[2] - me[2]) * (vl[2] - me[2]));
    if (d > 2.4f) {
        DULOG
            ("DUEL: PIN blow %d: the partner is %.2f m away - the pin is over",
             step + 1, (double) d);
        MpnEnd("the victim left the wall", 1);
        return;
    }
    if (!MpnRows(g_mpn.set, g_mpn.surf, MPN_BLOW, step, bl, 2)) {
        MpnEnd("no blow row", 1);
        return;
    }
    float V[3] = { vl[0], vl[1], vl[2] }, mv, da, sa[3], vy;
    if (!CdSpotsAt
        (g_mpn.row_cd[bl[0]], h, g_mpn.ap, V, me, 0.0f, &mv, &da, sa, &vy)
        || mv > 1.2f) {
        DULOG
            ("DUEL: PIN blow %d: the victim is %.2f m off the wall frame - the pin is over",
             step + 1, (double) mv);
        MpnEnd("the victim left the wall", 1);
        return;
    }
    float ap[7];
    memcpy(ap, g_mpn.ap, sizeof ap);
    char tag[32];
    snprintf(tag, sizeof tag, "BLOW %d/%d HIT", step + 1, g_mpn.maxb);
    if (!MpnPlay(bl[0], s, h, ap, tag)) {
        MpnDeliver(s, step, "psync refused");
    } else
        MpnDeliver(s, step, "QTE missed");
    g_mdu.hit_ms = now;
    g_mpn.last_hit = now;
    g_mpn.win_end = MpnWinEnd(now);
    g_mpn.k++;
    if (g_mpn.wbroke) {
        DULOG
            ("DUEL: PIN blow %d broke the weapon: the weapon pin is over - the next press (within reach of the staggered victim) re-pins with %s",
             step + 1, OwnFemale()? "the knife" : "fists");
        return;
    }
    DULOG
        ("DUEL: PIN blow %d landed (QTE missed / not pressed in time), %s",
         step + 1, g_mpn.k >= g_mpn.maxb
         || g_mpn.k > MpnMaxStep(g_mpn.set,
                                   g_mpn.
                                   surf) ? "next press = shove off" :
         "next press = next blow");
}

// v5-pinpress (on a CONTROLLER the follow-through after the first hit failed in a wall and a low cover pin; every pin of that log ended "PIN END (no press)"
// with no press line between): the pad press was read only as padinj_blocked_raw(), i.e. padinject's raw_blk - and melee_pace.c's mpace_tick, which runs before
// mpn_tick, calls padinj_block("melee-attack", 0) on every frame of a paired move (a pin IS one), zeroing raw_blk before we read it. Keyboard F worked through
// GetAsyncKeyState. The one "press buffered (0 ms after the blow)" each pin had was a stale raw_blk from the duel's own block at the push frame (before the pair
// started), not a new press - so blow 2 always came and blow 3 never did on a pad. Now: padinj_mel_edge (the device's melee bit kept by the Update hook, block
// or not, + keyboard F + bot), read EVERY frame so a press made before the pin never counts later; every press seen is logged with what the pin did with it.
static PadinjEdgeT g_mpnEdge;
static int MpnPressEdge(uint64_t now, const char **src)
{
    // (v5-pinpress 2, keyboard: + the engine's own melee-attack command - any key binding; our block hides only the read joypad's bit and keyboard / mouse
    //  input never showed on it. F and the command are one source: a held F keeps it on, so the command's own edge adds no second press)
    int foc = GameFocused(), fk =
        (GetAsyncKeyState(0x46) & 0x8000) != 0, cmd =
        PadCmdOn("melee-attack");
    if (g_mpn.on) {
        if (fk)
            g_mpn_f_frames++;
        if (!foc)
            g_mpn_unfoc_frames++;
        if (cmd)
            g_mpn_cmd_frames++;
    }
    int kb = (foc && fk) || cmd, bot = g_botMeleeMs && now - g_botMeleeMs < 80;     // bot.c presses (dev A/B tests)
    int e = PadinjMelEdge(&g_mpnEdge, now, kb, bot, src);
    if (!g_mpn.on || !e)
        return 0;
    if (e == 2) {
        PLOG("DUEL: PIN press (%s) at blow %d ignored: the same press through a second source (< 90 ms)", *src, g_mpn.k);
        return 0;
    }
    g_mpn_np_seen++;
    if (InteractDown()) {
        PLOG("DUEL: PIN press (%s) at blow %d REFUSED: interact is held (the E-contextual melee-attack-shiv, not a pin blow)", *src, g_mpn.k);
        return 0;
    }
    return 1;
}

// ---- the victim: the QTE
static int DtlPartner(uint32_t slot);
static void DtlCatchup(uint64_t ent, uint64_t sid, float late_ms, float window_s, float base, const char *what);       // melee_duel_tl.c
#define MPN_OUT_DEFER_T 12      // [TUNING ps39] ticks (the replica's 0.18 s blend-in + 1 frame)
#define PS45_PIN_SW_BLEND 0.30f // [TUNING ps45] the pin swing replica's crossfade off a held pair half (old 0.18 s: 34-42 deg hip / shoulder turn in 4 frames)
static struct {
    uint32_t uid;
    int slot;
    uint64_t at;
} g_mpnOut39;
static void MpnOut39Tick(void)
{
    if (!g_mpnOut39.at || g_t60 < g_mpnOut39.at)
        return;
    g_mpnOut39.at = 0;
    if (g_ps.role == 2 && g_ps.uid == g_mpnOut39.uid
        && g_ps.att == g_mpnOut39.slot)
        PsyncAttOut(PsyncMs() - g_ps.t0);
}

static void MpnVicSwing(uint32_t slot, const MpnRowT *r, int win,
                          uint64_t sent_clip)
{
    uint64_t ent = PuppetHandle(slot), clip =
        sent_clip ? sent_clip : MpnSwingClip(r,
                                               r->move[0] ? Fnv64Rt(r->
                                                                     move)
                                               : 0);
    if (!ent || !clip || g_peerDowned[slot] || g_puppetDeadHere[slot]) {
        DULOG("DUEL: PIN swing %s on slot %u's puppet: %s", r->move, slot,
              !clip ? "no clip for the move here" : "no puppet");
        return;
    }
    // ps39 B6 (rec38a B [12513]: 'attacker puppet slot 1 blend-out 0.25 s ... net replay back under the fading clip' on the very frame 'PIN swing fist-wall-attack-1
    // on slot 1's puppet' starts with its 0.18 s blend-in; frames B 38.97-39.17: the pinner stands in relaxed idle for 5 frames, then pops into the blow): the pair
    // half was let go before the next pin clip had any weight, so the net pose showed between them. The half is let go MPN_OUT_DEFER_T ticks later, when the
    // swing replica has its full weight over it. t2r-pin-att-out-ps38.txt = at once.
    // ps45 item 2 (rec44a A [16241] 'PIN swing fist-low-cover-attack-1 on slot 1's puppet' over the last pair's attacker half, [16247] 'BODY TURN 4f slot 1 ... drawn hips
    // -34 deg, shoulders -42 deg within 4 frames' inside the replica's 0.18 s crossfade, [16252] 'attacker half's layer lost' (the crossfade over: the engine dropped the
    // half) one frame BEFORE the ps39 release [16253]; the review's A pops 128.366 / 130.30 s (bent over the victim -> upright windup in one step) fall inside that crossfade,
    // not at the release - the release found the replica already owning the pose). The replica that replaces a held pair half (bent over the victim, the windup's
    // frame 0 stands upright) crossfades over PS45_PIN_SW_BLEND, the half kept under it for that blend + 1 tick. t2r-pin-swing-blend-ps45-old.txt = old 0.18 s / 12 ticks.
    int over_half45 = 0;
    if (g_ps.role == 2 && g_ps.att == (int) slot && g_ps.ent_att == ent) {
        static int off45 = -1;
        static uint64_t at45;
        if (off45 < 0 || GetTickCount64() - at45 > 2000) {
            at45 = GetTickCount64();
            int o =
                GetFileAttributesA("t2r-pin-swing-blend-ps45-old.txt") !=
                INVALID_FILE_ATTRIBUTES;
            if (o != off45)
                DULOG
                    ("DUEL: PIN swing replica over the last pair's half (ps45): %s",
                     o ?
                     "OFF (t2r-pin-swing-blend-ps45-old.txt): 0.18 s crossfade, half kept 12 ticks"
                     : "on - longer crossfade off the held half");
            off45 = o;
        }
        over_half45 = !off45;
        int defer =
            over_half45 ? (int) S60(PS45_PIN_SW_BLEND) +
            1 : MPN_OUT_DEFER_T;
        if (GetFileAttributesA("t2r-pin-att-out-ps38.txt") !=
            INVALID_FILE_ATTRIBUTES)
            PsyncAttOut(PsyncMs() - g_ps.t0);        // the last blow's pair half lets go of the puppet
        else {
            g_mpnOut39.uid = g_ps.uid;
            g_mpnOut39.slot = (int) slot;
            g_mpnOut39.at = g_t60 + (uint64_t) defer;
            PLOG("DUEL: PIN swing on slot %u's puppet: the last pair's attacker half is kept under it for %d ticks (no net pose between the two clips; ps39%s)", slot, defer, over_half45 ? " / ps45 longer crossfade" : "");
        }
    }
    float age =
        g_psRtt[slot] > 0.0f ? g_psRtt[slot] * 0.5f : 40.0f, dur =
        MdgClipLen(clip);
    if (!(dur > 0.2f)) {
        DULOG("DUEL: PIN swing %s: clip %llx not loaded here", r->move,
              (unsigned long long) clip);
        return;
    }
    float sp = r->tc_ms > 0
        && win > 0 ? (float) r->tc_ms / (float) win : 1.0f, ph =
        age * sp / 1000.0f / dur;
    if (ph > 0.5f)
        ph = 0.5f;
    int full = DtlPartner(slot);       // duel timeline: the windup is never trimmed - phase 0, the lateness caught up by rate over the windup
    if (full)
        ph = 0.0f;
    float bl45 = full ? 0.18f : 0.12f;
    if (over_half45 && DtlPartner(slot))
        bl45 = PS45_PIN_SW_BLEND;       // ps45 item 2
    uint64_t res = MhrAnimate(ent, clip, ph, bl45);
    if (over_half45)
        DULOG
            ("DUEL: PIN swing replica (ps45): crossfade %.2f s off the held pair half (old 0.18 s)",
             (double) bl45);
    for (int l = 0; l < 4; l++) {
        uint64_t a[4] = { ent, 0, (uint64_t) l, clip };
        memcpy(&a[1], &sp, 4);
        MoRateDc(a, MO_SRC("pin.swing_replica_rate"));
    }
    // (melee_duel_contact.c: no catch-up - at the pinner's own speed from phase 0 the replica reaches its contact frame one-way latency after the
    //  pinner's, exactly when the blow's START arrives here and the victim half starts: the fist meets the reaction, see re/melee-duel-contact.md)
    if (full)
        DtlCatchup(ent, clip, MctOn()? 0.0f : age,
                    win > 0 ? (float) win / 2000.0f : dur * 0.25f, sp,
                    "pin swing replica");
    g_mhr.pd[slot].ent = ent;
    g_mhr.pd[slot].sid = clip;
    g_mhr.pd[slot].end = GetTickCount64() + (uint64_t) win + 250;       // the contact pair takes the puppet over before this
    DULOG
        ("DUEL: PIN swing %s on slot %u's puppet (clip %llx at phase %.2f, speed x%.2f) -> %llx",
         r->move, slot, (unsigned long long) clip, (double) ph,
         (double) sp, (unsigned long long) res);
}

static int MpnQteOpen(void)
{
    return g_mpn.vq_open && GetTickCount64() <= g_mpn.vq_end;
}

static int MpnVicPin(void)
{
    return MpnQteOpen();
}

static void MpnRecv(uint32_t slot, const uint8_t *buf, uint64_t len)
{
    if (slot >= T2R_MAX_PEERS || !buf || len < 4 || !MduOn() || MpnOff())
        return;
    int op = buf[1];
    uint64_t now = GetTickCount64();
    if (g_mdu.slot != (int) slot)
        return;
    if (op == MDPO_BLOW && len >= 7) {
        int win = buf[5] | (buf[6] << 8);
        BbMeleeNote(T2R_US, 0);
        g_mpn.vq_open = now;
        g_mpn.vq_end = now + (uint64_t) win + MPN_GRACE_MS;
        g_mpn.vq_id = buf[4];
        g_mpn.vq_ok = 0;
        g_mpn.vq_slot = (int) slot;
        // (ONE late press for blow 3 burnt blow 4's QTE - only a mash burns now: 2 presses < 500 ms apart, the last < 300 ms before the prompt)
        // v5-pinfix3 (breaking out of a pin was only available after the first hit, the prompt disappeared for the rest of the pin; log
        //  35529 / dlog 29175-31424: QTE ids 4..7 opened BURNT by the presses between the blows, ids 5 / 9 burnt by one press at 94 / 16 ms - a burnt QTE hid its
        //  prompt, so the victim never saw when to press, mashed, and burnt every next QTE): no QTE starts burnt and no press burns a QTE any more. The prompt shows
        //  for every blow; the anti-mash is per press (mpn_qte_press: a press counts only >= MPN_LOCK_MS after the previous one). t2r-pin-old-burn.txt = old rule.
        g_mpn.vq_burn = MpnOldBurn() && g_mpn.vq_last_press
            && now - g_mpn.vq_last_press < MPN_BURN_MS
            && g_mpn.vq_prev_press
            && g_mpn.vq_last_press - g_mpn.vq_prev_press < 500;
        g_mdu.act_ms = now;
        g_mdu.hold_until = now + MDU_HOLD_MS;
        // (the victim side of a pin did not see the attacker use the proper anims: the pinner's puppet threw a
        // plain upright punch / kept the last blow's pair half while the pinner's screen showed the wall / low-cover swing). The pinner's swing row comes
        // with the blow: the previous pair's attacker half on the puppet is released and the same swing clip plays on it, slowed to the same contact time.
        g_mpn.vq_shown = -1;
        g_mpn.vq_late_until = 0;
        if (len >= 8 && buf[7] < MPN_ROWS_N) {
            uint64_t sc = 0;
            if (len >= 16)
                memcpy(&sc, buf + 8, 8);
            MpnVicSwing(slot, &MPN_ROWS[buf[7]], win, sc);
        }
        DULOG
            ("DUEL: PIN QTE OPEN on us (id %d) from slot %u: dodge within %d ms of the blow's start (+%d ms tie grace)%s",
             buf[4], slot, win, MPN_GRACE_MS,
             g_mpn.
             vq_burn ?
             " - BURNT: a press came < 300 ms before the prompt (mash)" :
             "");
    } else if (op == MDPO_ESC && len >= 7) {
        if (g_mpn.on && g_mpn.slot == (int) slot) {
            g_mpn.esc_id = buf[4];
            g_mpn.esc_cool[slot] = GetTickCount64() + MPN_ESC_COOL_MS;
            DULOG
                ("DUEL: PIN slot %u escaped blow id %d (its press %d ms after the blow's start on its screen, latency subtracted): our swing is cancelled, its counter follows",
                 slot, buf[4], buf[5] | (buf[6] << 8));
            if (g_mpn.state == 2 && g_mpn.id == buf[4]) {
                g_mpn.due = GetTickCount64();
            }                   // contact now: mpn_blow_contact sees the escape
            if (g_mdu.pair_on && g_ps.role == 1
                && g_ps.uid == g_mdu.ps_uid) {
                if (Pe45Off())
                    PsyncAbort("pin escaped", 1);
                else
                    Pe45Arm(1);
            }                   // ps45 item 1: held until the escape START (paired.c)
            g_mdu.pair_on = 0;
            g_mdu.hold_until = GetTickCount64() + MDU_HOLD_MS;
            if (g_mpn.state != 2)
                MpnEnd("escaped", 0);
        }
    } else if (op == MDPO_END && g_mpn.on && g_mpn.slot == (int) slot)
        MpnEnd("the partner ended it", 0);
    else if (op == MDPO_END) {
        g_mpn.vq_open = 0;
        g_mpn.vq_end_at = now;
    }
}

// melee_dodge.c, the press edge when no escape / dodge window is open: an early press burns the next QTE
static int MpnOldBurn(void)
{
    static int v = -1;
    static uint64_t at;
    uint64_t now = GetTickCount64();
    if (v < 0 || now - at >= 2000) {
        at = now ? now : 1;
        v = GetFileAttributesA("t2r-pin-old-burn.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    return v;
}

static void MpnEarlyPress(uint64_t now)
{
    if (g_mdu.on && g_mdu.slot >= 0) {
        g_mpn.vq_prev_press = g_mpn.vq_last_press;
        g_mpn.vq_last_press = now;
    }
}

static uint64_t MpnPinnedAt(void)
{
    return g_mpn.vq_ap_at;
}                               // dcounter.c: when a pin pair last had us on a wall

// melee_dodge.c, the press edge inside our QTE window
static void MpnQteSuccess(uint64_t now, int rel);
static void MpnQtePress(uint64_t now)
{
    if (!MpnQteOpen() || g_mpn.vq_ok)
        return;
    int rel = (int) (now - g_mpn.vq_open);
    if (!MpnOldBurn()) {      // v5-pinfix3: every press is judged alone - before the prompt = no effect, < MPN_LOCK_MS after the previous press = mash (ignored)
        uint64_t last = g_mpn.vq_last_press;
        g_mpn.vq_prev_press = last;
        g_mpn.vq_last_press = now;
        if (now < MPN_RING_AT() && !MpnLenient()) {    // v5-pinfix3 5: a press in WAIT fails this blow's QTE - shown + heard (mpn_prompt FAILED state)
            if (!g_mpn.vq_burn) {
                g_mpn.vq_burn = 1;
                int sr = MpnUiSound(1);       // (no top-of-screen notification: the prompt with its sound is enough)
                DULOG
                    ("DUEL: PIN QTE FAILED (id %d): E pressed %d ms into the blow, %d ms before the press moment - the prompt turns to \"Too early\" for this blow, FAILED sound (menu error wave): %s (no notification); the blow lands",
                     g_mpn.vq_id, rel, (int) (MPN_RING_AT() - now),
                     MpnSndWord(sr));
            } else
                DULOG
                    ("DUEL: PIN QTE press at %d ms ignored: this blow's QTE already failed (pressed too early)",
                     rel);
            return;
        }
        if (g_mpn.vq_burn && !MpnLenient()) {
            DULOG
                ("DUEL: PIN QTE press at %d ms ignored: this blow's QTE already failed (pressed too early) - the next blow starts clean",
                 rel);
            return;
        }
        if (now < MPN_RING_AT()) {
            DULOG
                ("DUEL: PIN QTE press at %d ms - before the prompt (it shows at %d ms): no effect, a press when it shows (>= %d ms after this one) escapes",
                 rel, (int) (MPN_RING_AT() - g_mpn.vq_open), MPN_LOCK_MS);
            return;
        }
        if (MpnLenient() && last && now - last < MPN_LOCK_MS) {
            DULOG
                ("DUEL: PIN QTE press at %d ms ignored: mash (the previous press %d ms ago, < %d ms) - one more press escapes",
                 rel, (int) (now - last), MPN_LOCK_MS);
            return;
        }
    }
    if (g_mpn.vq_burn) {
        DULOG
            ("DUEL: PIN QTE press at %d ms ignored: this QTE was burnt by an earlier press",
             rel);
        return;
    }
    if (now < MPN_RING_AT()) {
        g_mpn.vq_burn = 1;
        DULOG
            ("DUEL: PIN QTE press at %d ms - TOO EARLY (the prompt shows at %d ms): this QTE is failed, the blow lands",
             rel, (int) (MPN_RING_AT() - g_mpn.vq_open));
        return;
    }
    MpnQteSuccess(now, rel);
}

// the escape itself (a valid press, or Iron Grip II's free escape the moment the press window opens)
static void MpnQteSuccess(uint64_t now, int rel)
{
    g_mpn.vq_ok = 1;
    g_mpn.vq_rel = (uint16_t) rel;
    g_mpn.vq_open = 0;
    g_mpn.vq_esc_at = now;
    g_dcPinEscMs = GetTickCount64();
    g_mpn.vq_drop_until = now + 900;
    int s = g_mpn.vq_slot;
    uint8_t p[3] =
        { (uint8_t) g_mpn.vq_id, (uint8_t) rel, (uint8_t) (rel >> 8) };
    MduSend(MDPO_ESC, p, 3);
    if (g_ps.role == 2 && g_mdu.pair_on) {
        if (Pe45Off())
            PsyncAbort("pin escape", 1);
        else
            Pe45Arm(2);
        if (g_meleeLockVictim)
            g_meleeLockFrom = 0;
        g_mdu.pair_on = 0;
    }                           // ps45 item 1: held until our escape START (paired.c)
    g_mpn.ctr_at = now + 40;
    g_mdu.hold_until = now + MDU_HOLD_MS;
    g_mdu.act_ms = now;
    MdgPromptWithdraw("pin escape");
    DULOG
        ("DUEL: PIN ESCAPE: Dodge pressed %d ms into the blow (judged on our screen, window %d ms): the pin pair is cut, slot %d told, the blow's damage dropped, the escape pair (SP's struggle-success, else the dodge COUNTER) follows",
         rel, MPN_WIN_MS, s);
}

// victim: the pin pair START of the blow we just escaped (race) is refused
static int MpnVicGate(uint32_t slot, int pr)
{
    if (!MpnPairIsPin(pr))
        return 0;
    if (g_mpn.vq_ok && GetTickCount64() < g_mpn.vq_drop_until
        && (int) slot == g_mpn.vq_slot) {
        DULOG("DUEL: PIN pair %s on us refused: we escaped this blow",
              CD_PAIRS[pr].name);
        return 1;
    }
    return 0;
}

// recv_hit_hook: the blow we escaped is dropped
static int MpnNoteHit(uint32_t slot, uint64_t w, float dmg)
{
    (void) w;
    if (!g_mpn.vq_ok || GetTickCount64() >= g_mpn.vq_drop_until
        || (int) slot != g_mpn.vq_slot || !(dmg > 0.0f))
        return 0;
    DULOG
        ("DUEL: PIN blow damage %.0f from slot %u dropped: we escaped it (QTE press at %d ms)",
         (double) dmg, slot, g_mpn.vq_rel);
    return 1;
}

// ---- the SP escape pair (NPCs can also pin the player to walls and low cover in SP, so those escapes are reused): the victim that escaped plays SP's struggle-success pair
// of the same surface (player half on us, NPC half on the pinner; mduel_pins.h MPN_ESC, dcounter.c dc_launch: it is a dodge-counter row, the shove lands at the row's hit moment as
// the guaranteed counter blow) with the player half's own animated camera (pinesc_cam_tracks.h). None of the 8 pairs is in the MP melee paks: they are in the SP-common paks of the
// parry branch (anim-melee-npc-t2 / abby-npc-t2 / ellie-npc-t2); a pair not loaded here = the old dodge counter (mpn_counter).
static void MpnVicTrack(uint64_t now)
{                               // every frame: the pin pair that has us remembers the wall frame
    if (g_ps.role != 2 || g_ps.kind != PS_CTXDOWN)
        return;
    if (Pe45Held(2))
        return;                 // ps45: the escaped pin pair held for the escape START is no new pin frame (mpn_cam_role: vq_esc_at >= vq_ap_at)
    int pr = CdPairOfMove(g_ps.move);
    if (!MpnPairIsPin(pr) || !PsyncAp(g_mpn.vq_ap))
        return;
    g_mpn.vq_ap_ok = 1;
    g_mpn.vq_row = pr;
    g_mpn.vq_ap_at = now;
    g_mpn.vq_att = g_ps.att;
}

static int MpnPinnedNow(void)
{
    return g_mpn.vq_ap_ok && GetTickCount64() - g_mpn.vq_ap_at < 700;
}

// melee_spcam.c, the v5 pin camera: 2 = we pin (from PIN START to PIN END), 1 = we are pinned, 0 = no pin; *slot = the other body.
// The victim's side holds between two blows (a pin pair hands off after 0.7-1.5 s and the next press may come 1.5 s + 330 ms later - mpn_pinned_now's
// 700 ms tail let the camera go and come back between blows) until the pinner's END, our escape press, the duel's end or 2.2 s without a pin pair.
static int PulCamRole(int *slot);     // melee_pullout.c: a pull-out frames both bodies with the pin camera (the attacker camera stared at the wall during the drag)
static int MpnCamRole(int *slot)
{
    uint64_t now = GetTickCount64();
    {
        int r = PulCamRole(slot);
        if (r)
            return r;
    }
    if (MpnActive() && !MpnOff()) {
        *slot = g_mpn.slot;
        return 2;
    }
    if (!g_mpn.vq_ap_ok || g_mpn.vq_att < 0
        || g_mpn.vq_att >= T2R_MAX_PEERS)
        return 0;
    uint64_t age = now - g_mpn.vq_ap_at;
    if (g_mpn.vq_esc_at >= g_mpn.vq_ap_at)
        return 0;               // we escaped: the escape pair's camera takes over (spc_tick5's hand-over)
    if (!
        (age < 700
         || (age < 2200 && g_mdu.on && g_mdu.slot == g_mpn.vq_att
             && g_mpn.vq_end_at < g_mpn.vq_ap_at)))
        return 0;
    *slot = g_mpn.vq_att;
    return 1;
}

static int MpnVicHeld(void)
{
    int s = -1;
    return MpnPinnedNow() || MpnCamRole(&s) == 1;
}                               // melee_dodge.c: pinned = Escape only

static int MpnEscape(uint64_t now)
{
    int s = g_mpn.vq_slot;
    if (!g_mpn.vq_ap_ok || now - g_mpn.vq_ap_at > 30000) {
        DULOG
            ("DUEL: PIN ESCAPE pair: no pin pair has had us on this wall (no frame) - the dodge counter instead");
        return 0;
    }
    MpnInit();
    int ri = -1;
    for (size_t i = 0; i < MPN_ROWS_N; i++)
        if (g_mpn.row_cd[i] == g_mpn.vq_row) {
            ri = (int) i;
            break;
        }
    if (ri < 0)
        return 0;
    int surf = MPN_ROWS[ri].surf, set = MPN_ROWS[ri].set, wpn =
        (set == MDU_FIST
         || set == MDU_KNIFE) ? 0 : 1, want = OwnFemale()? 1 : 0, cd = -1;
    for (int pass = 0; pass < 2 && cd < 0; pass++)      // our own character's player half first (male = Abby, female = Ellie), then the other
        for (size_t i = 0; i < MPN_ESC_N; i++) {
            if (MPN_ESC[i].wpn != wpn || MPN_ESC[i].surf != surf
                || MPN_ESC[i].ch != (pass ? 1 - want : want))
                continue;
            int c = CdPairOfMove(Fnv64Rt(MPN_ESC[i].pair));
            if (c >= 0 && CdPairLoaded(c)) {
                cd = c;
                break;
            }
        }
    if (cd < 0) {
        DULOG
            ("DUEL: PIN ESCAPE pair: no SP struggle-success pair loaded for %s / %s (SP-common paks off?) - the dodge counter instead",
             surf == MPN_WALL ? "wall" : "cover",
             wpn ? "weapon" : "fists");
        return 0;
    }
    uint8_t *peer = MdgPeer((uint32_t) s);
    uint64_t h = peer ? *(uint64_t *) (peer + 0x60) : 0;
    float vb[16], mb[16];
    const float *vl = h ? PuppetLocator(h, vb) : NULL, *me =
        PlayerLocator(mb);
    if (!h || !vl || !me || !PsLocOk(vl) || !PsLocOk(me)
        || g_peerDowned[s] || g_puppetDeadHere[s] || g_downed
        || LocalHp() <= 0.0f) {
        DULOG
            ("DUEL: PIN ESCAPE pair %s: the pinner / we are gone or down - the dodge counter instead",
             CD_PAIRS[cd].name);
        return 0;
    }
    // v5-pinesc (A/B v5-int22, A 41035): the escape pair started with the pinner already 176 m away (its body had left the map) and psync's SP pull dragged its
    // puppet across the map onto the pair's spot. The pair is the pin's own continuation: the pinner must still stand at the pin spacing in front of us.
    {
        float dx = vl[0] - me[0], dz = vl[2] - me[2], d =
            __builtin_sqrtf(dx * dx + dz * dz), dy =
            __builtin_fabsf(vl[1] - me[1]);
        if (d > 2.4f || dy > 1.0f) {
            DULOG
                ("DUEL: PIN ESCAPE pair %s refused: the pinner is %.2f m away (%.2f m up / down) - not at the wall with us, the escape alone",
                 CD_PAIRS[cd].name, (double) d, (double) dy);
            return 1;
        }
    }
    DcCand c;
    memset(&c, 0, sizeof c);
    c.row = cd;
    memcpy(c.ap, g_mpn.vq_ap, sizeof c.ap);
    c.p0 = 0.0f;
    // the path gate on both halves up to the hand-off (wall / floor / drop at the spots the clips carry the bodies to), as dc_try does for every dodge counter:
    // a pair whose pinner half would end inside or behind geometry is refused BEFORE it plays (2 = rays in flight: mpn_counter asks again next frame)
    {
        float vd = PsyncDur(h, FNV64(CD_VIC0(cd))), lag = DcLagRow(cd);
        g_pgEndPh = vd > 0.2f ? (CD_PAIRS[cd].end_s - lag) / vd : 0.0f;
        int g = GetFileAttributesA("t2r-pin-esc-no-gate.txt") != INVALID_FILE_ATTRIBUTES ? 1 : PgGate((uint32_t) s, CD_ATT0(cd), CD_VIC0(cd), c.ap, vl, me, 0, "pin escape");  // (opt-out file = v5-int: no gate)
        g_pgEndPh = 0.0f;
        if (g == 2)
            return 2;
        if (!g) {
            DULOG
                ("DUEL: PIN ESCAPE pair %s refused: the path gate found a wall / drop on its spots - the dodge counter instead",
                 CD_PAIRS[cd].name);
            return 0;
        }
    }
    g_dcm.pend_w = 0;
    g_dcm.cool_until = 0;
    g_dcm.rr++;
    DcLaunch(s, &c, 0);
    if (!g_dcm.on_att || g_dcm.row != cd) {
        DULOG
            ("DUEL: PIN ESCAPE pair %s refused by psync (the plain counter hit followed)",
             CD_PAIRS[cd].name);
        return 1;
    }
    float end_s = CD_PAIRS[cd].end_s;
    char nm[64];
    snprintf(nm, sizeof nm, "ctxdown:%s", CD_PAIRS[cd].name);
    int cam = 0;
    if (XcamStart(h, end_s + 0.5f, 0, -1, end_s, 1, nm)) {
        if (g_xcamTrk) {
            cam = 1;
            g_dcCam = 1;
        } else
            XcamStop("no authored track");
    }
    DULOG
        ("DUEL: PIN ESCAPE pair %s (%s %s, %s): %s on us + %s on slot %d, shove lands at %.2f s, hand-off %.2f s, camera: %s",
         CD_PAIRS[cd].name, surf == MPN_WALL ? "wall" : "low cover",
         wpn ? "weapon" : "fists",
         want == (strstr(CD_PAIRS[cd].name, "ellie") !=
                  NULL) ? "our own character's half" :
         "the other character's half", CD_ATT0(cd), CD_VIC0(cd), s,
         (double) g_dcm.hit_s, (double) end_s,
         cam ? "the player half's own animated camera" :
         "none (the game camera stays)");
    return 1;
}

static void MpnCounter(uint64_t now)
{
    int s = g_mpn.vq_slot;
    int held45 = Pe45Held(2);  // ps45 item 1: our escaped pin pair still runs (held for this START) - it is no busy state
    if (!held45 && (PS_OURS || MdgBusy())) {
        if (now > g_mpn.ctr_at + 1500)
            g_mpn.ctr_at = 0;
        return;
    }
    g_mpn.ctr_at = 0;
    if (held45 && DcOff())
        Pe45End("the dodge counter is off here");
    if (DcOff()) {
        DULOG
            ("DUEL: PIN escape: the dodge counter is switched off here - the escape alone");
        return;
    }
    if (OwnFemale()) {         // female characters holster their weapon (not animated) at the start of a pin escape; before, they stabbed with the sidearm in hand and the knife
        uint64_t none0 = 0, z[2] = { 0, 0 }, held =
            DcCall(FNV64("get-player-weapon-id"), 0, &none0);
        if (held && held != ~0ULL && BaseWeaponIsGun(BaseWeapon(held))) {
            uint64_t r = (g_holsterInstFrame =
                          g_frame ? g_frame : 1,
                          DcCall(FNV64("holster-player-weapon-instantly"),
                                  2, z)), after =
                DcCall(FNV64("get-player-weapon-id"), 0, &none0);
            DULOG
                ("DUEL: PIN escape: female life - gun %llx put away instantly before the escape (-> %llx, held now %llx): the knife clip plays with an empty gun hand",
                 (unsigned long long) held, (unsigned long long) r,
                 (unsigned long long) after);
        }
    }
    {
        int er = MpnEscape(now);
        if (er == 2) {
            if (!g_mpn.esc_wait)
                g_mpn.esc_wait = now;
            if (now - g_mpn.esc_wait < 250) {
                g_mpn.ctr_at = now;
                return;
            }
            DULOG
                ("DUEL: PIN ESCAPE pair: path gate unanswered after 250 ms - the dodge counter instead");
        }
        g_mpn.esc_wait = 0;
        if (Pe45Held(2))
            Pe45End(er == 1 ? "the escape pair did not start (refused)" : "no escape pair - the dodge counter instead");       // (a started pair took it in psync_start)
        if (er == 1)
            return;
    }
    g_dcm.arm_slot = (uint32_t) s;
    g_dcm.arm_dir = 0;
    g_dcm.arm_until = now + 800;
    g_dcm.cool_until = 0;
    g_dcm.escaped = 0;
    int r = DcSwingStart();
    DULOG("DUEL: PIN escape counter vs slot %d: %s", s,
          r ?
          "dodge-counter pair started (guaranteed hit, 2 x fist damage)" :
          "no counter fits - the escape alone");
}

// the pin escape is the INTERACT button (E / triangle / Y) with an "Escape" prompt, not the dodge. interact_down() feeds the engine's own
// rising edge (interact_pressed: keyboard E + every pad); bot.c injects g_bot_e_ms (dev).
static uint64_t g_botEMs;
static int MpnEEdge(void)
{
    static uint64_t cf;
    static int cr;
    if (cf && cf == g_frame)
        return cr;              // (melee_pullout.c reads the same edge in the same frame: one engine read per frame)
    cf = g_frame;
    // pull-outs could not be won: 7 pull-out windows with the native ESCAPE! prompt up, not one press seen -
    // the native interactable prompt takes the engine's interact command edge. The rising edge of the whole interact answer (keyboard E read
    // directly, the engine's interact-hold, the pad latch) counts as a press too.
    static int dprev;
    int d = InteractDown();
    int e = InteractPressed();
    if (d && !dprev && !e) {
        e = 1;
        static int nl;
        if (nl++ < 20)
            Xlog("interact edge: press taken from the held state (E key / interact-hold) - the interact command edge was not seen");
    }
    dprev = d;
    static uint64_t lb;
    if (g_botEMs && g_botEMs != lb) {
        lb = g_botEMs;
        e = 1;
    }
    cr = e;
    return e;
}

// the Escape prompt over the attacker (the world prompt widget with the interact key's glyph, like Execute / Revive)
static void MpnPrompt(void)
{
    if (!MpnQteOpen() && g_mpn.vq_late_until && GetTickCount64() < g_mpn.vq_late_until && !g_mpn.vq_ok) {     // v5-qte4: the window just closed - "Too late", never a dead "ESCAPE!"
        uint64_t lh = PuppetHandle((uint32_t) g_mpn.vq_slot);
        if (lh) {
            if (PaOld())
                WorldPrompt(lh, "Too late", 0.0f);
            else
                WorldPromptGrey(lh, "Too late", 0);
        }
        return;
    }
    if (!MpnOldBurn() && !MpnLenient()) {    // v5-pinfix3 5: WAIT (ring filling) -> PRESS (pop + sound) / FAILED (too early) for every blow
        uint64_t now = GetTickCount64();
        if (!MpnQteOpen() || g_mpn.vq_ok)
            return;
        uint64_t h = PuppetHandle((uint32_t) g_mpn.vq_slot), ra =
            MPN_RING_AT();
        if (!h)
            return;
        int st = g_mpn.vq_burn ? 2 : now < ra ? 0 : 1;
        float fill =
            ra >
            g_mpn.vq_open ? (float) (now - g_mpn.vq_open) / (float) (ra -
                                                                     g_mpn.
                                                                     vq_open)
            : 1.0f;
        if (fill > 0.99f)
            fill = 0.99f;
        if (fill < 0.0f)
            fill = 0.0f;
        // v5-fb1 (the prompt look did not visibly change, still just regular E - Escape; a clip at 30 fps, 3 blows): the label
        // DID change in place every blow - WAIT "E Escape" ~5 frames, a 1-frame glyph-only blip, then "E Escape!" ~0.5 s - but the two labels differ by one "!"
        // and the hold ring (request+0x139 on interact-default) never draws, so it read as one plain prompt. Now the words carry the state:
        // WAIT "Wait..." -> PRESS "ESCAPE!" -> FAILED "Too early" (+ sfx-menu-error at the press; no top-of-screen notification).
        const char *lbl =
            st == 0 ? "Wait..." : st == 1 ? "ESCAPE!" : "Too early";
        int shown = 1;
        if (PaOld())
            shown =
                WorldPrompt(h, lbl,
                             st == 0 ? fill : st == 2 ? 0.0f : -1.0f);
        else if (st == 0)
            WorldPromptGreyP(h, "", 1, fill);        // the key + a ring contracting onto it, no "Wait..." text
        else if (st == 2)
            WorldPromptGrey(h, lbl, 0);
        else
            shown = WorldPrompt(h, lbl, -1.0f);
        if (st != g_mpn.vq_shown) {
            int sr = 0;
            if (st == 1)
                sr = MpnUiSound(0);   // the press moment: the menu confirm wave (the FAILED sound plays at the press)
            DULOG
                ("DUEL: PIN QTE prompt on us (id %d) -> %s at %d ms (%s)%s%s, pressable until %d ms",
                 g_mpn.vq_id,
                 st == 0 ? "WAIT (\"Wait...\")" : st ==
                 1 ? "PRESS (\"ESCAPE!\")" : "FAILED (\"Too early\")",
                 (int) (now - g_mpn.vq_open),
                 shown ? "world prompt SHOWN" :
                 "world prompt refused - text fallback",
                 st == 1 ? ", PRESS sound: " : "",
                 st == 1 ? MpnSndWord(sr) : "",
                 (int) (g_mpn.vq_end - g_mpn.vq_open));
            g_mpn.vq_shown = st;
        }
        if (!shown) {
            static uint64_t pf;
            static int pst = -1;
            if (pst != st || g_t60 - pf >= 120) {
                pf = g_t60;
                pst = st;
                T2R_PROMPT_MSG(st == 0 ? "GET READY: E TO ESCAPE" : st ==
                               1 ? "PRESS E NOW TO ESCAPE" : "TOO EARLY",
                               0.5f, 0, 0);
            }
        }
        return;
    }
    if (!MpnQteOpen() || g_mpn.vq_burn || g_mpn.vq_ok
        || GetTickCount64() < MPN_RING_AT())
        return;
    uint64_t h = PuppetHandle((uint32_t) g_mpn.vq_slot);
    if (!h)
        return;
    int shown = WorldPrompt(h, "Escape", -1.0f);
    {
        static int lid = -1;
        if (lid != g_mpn.vq_id) {
            lid = g_mpn.vq_id;
            DULOG("DUEL: PIN QTE prompt on us (id %d): world prompt %s",
                  g_mpn.vq_id,
                  shown ? "SHOWN" : "refused - text fallback");
        }
    }
    if (!shown) {
        static uint64_t pf;
        if (g_t60 - pf >= 120) {
            pf = g_t60;
            T2R_PROMPT_MSG("PRESS E TO ESCAPE", 0.6f, 0, 0);
        }
    }
}

// ---- every frame (t2r-ext.c, after mdu_tick)
static void MpnTick(void)
{
    MpnOut39Tick();           // ps39 B6
    uint64_t now = GetTickCount64();
    if (g_mpnWup.clip) {       // ps28: the pin blow's wind-up profile (see MPN_WIN_MS)
        float el = (float) (now - g_mpnWup.t0);
        if (!g_mpn.on || !g_mpn.slow_clip
            || el >= g_mpnWup.tc + g_mpnWup.x || !(g_mpnWup.len > 0.1f))
            g_mpnWup.clip = 0;
        else {
            float ph =
                WupCt(el, g_mpnWup.tc,
                       g_mpnWup.x) / (g_mpnWup.len * 1000.0f);
            if (ph > 0.98f)
                ph = 0.98f;
            for (int ly = 0; ly < 4; ly++) {
                uint64_t b_[4] =
                    { SID_PLAYER, 0, (uint64_t) ly, g_mpnWup.clip };
                memcpy(&b_[1], &ph, 4);
                DcCall(FNV64("set-animation-phase"), 4, b_);
            }
            static int nl;
            static uint64_t lf;
            if (g_frame - lf >= 6 && LOGCAP(nl, 300)) {
                lf = g_frame;
                float rb = PsyncPhase(SID_PLAYER, g_mpnWup.clip);
                DULOG
                    ("DUEL: PIN wind-up (ps28) +%.0f ms: clip wants %.0f ms, plays %.0f ms (contact at %.0f ms, SP %.0f)",
                     (double) el, (double) WupCt(el, g_mpnWup.tc,
                                                  g_mpnWup.x),
                     (double) (rb >=
                               0.0f ? rb * g_mpnWup.len *
                               1000.0f : -1.0f),
                     (double) (g_mpnWup.tc + g_mpnWup.x),
                     (double) g_mpnWup.tc);
            }
        }
    }
    // a press 20 frames into the after-hand-off pin blow chained the ENGINE's own next melee move (97770219, start-melee-action
    // never saw it, so no apReference): its align used the default BoundFrame = the world origin and carried us 301 m off the map (dead). While our pin swing runs
    // the engine starts no melee of its own (player-disable-melee each frame, as melee_pace.c's lock; the running swing is not touched). t2r-pin-no-melee-lock.txt = old.
    // a WEAPON pin keeps the lock for the whole pin (bat pin: the weapon's engine contacts, 40 a blow, landed mid-pin between our swings)
    if (g_mpn.on
        &&
        ((g_mpn.state == 2 && g_mpn_swing_t0 && now >= g_mpn_swing_t0
          && now < g_mpn_swing_t1) || (g_mpn.wpn && !g_mpn.wbroke))) {
        static int off = -1;
        static uint64_t at;
        if (off < 0 || now - at >= 2000) {
            at = now;
            off =
                GetFileAttributesA("t2r-pin-no-melee-lock.txt") !=
                INVALID_FILE_ATTRIBUTES;
        }
        if (!off) {
            uint64_t none = 0;
            DcCall(0x0fa21391e0e4e478ULL /* player-disable-melee */ , 0,
                    &none);
        }
    }
    MpnVicTrack(now);
    MpnStowTick(now);
    if (g_mpn.ctr_at && now >= g_mpn.ctr_at)
        MpnCounter(now);
    if (MpnQteOpen() && !g_mpn.vq_ok && now >= MPN_RING_AT()
        && SkIronFree("pin")) {
        DULOG
            ("DUEL: PIN QTE passed for us (Iron Grip II) as the press window opens");
        MpnQteSuccess(now, (int) (now - g_mpn.vq_open));
    }
    if (g_mdu.on || MpnQteOpen()) {
        if (MpnEEdge()) {
            if (MpnQteOpen())
                MpnQtePress(now);
            else if (!g_mpn.on) {
                MpnEarlyPress(now);   // (the pinner's own E is not a victim press)
                if (g_mpn.vq_closed_at && now - g_mpn.vq_closed_at < 1000) {
                    static int nl;
                    if (LOGCAP(nl, 60))
                        DULOG
                            ("DUEL: PIN QTE press %d ms AFTER the window of id %d closed (prompt %s) - no effect",
                             (int) (now - g_mpn.vq_closed_at),
                             g_mpn.vq_late_id,
                             now <
                             g_mpn.
                             vq_late_until ? "showing \"Too late\"" :
                             "gone / engine linger");
                }
            }
        }
    }
    if (g_mpn.vq_open && now > g_mpn.vq_end) {
        int burnt = g_mpn.vq_burn;
        uint64_t op = g_mpn.vq_open;
        g_mpn.vq_open = 0;
        if (!g_mpn.vq_ok) {
            MdgPromptWithdraw("pin window closed");
            g_mpn.vq_closed_at = now;
            g_mpn.vq_late_id = g_mpn.vq_id;
            g_mpn.vq_late_until = (!burnt && !MpnOldBurn())? now + MPN_LATE_MS : 0;   // ("Too early" already says it: it stays as the engine's linger)
            DULOG
                ("DUEL: PIN QTE window closed on us without a valid press (id %d, %d ms after the open) - the blow lands; prompt -> %s",
                 g_mpn.vq_id, (int) (now - op),
                 g_mpn.
                 vq_late_until ? "\"Too late\" this frame" : burnt ?
                 "\"Too early\" (kept)" : "off");
        }
    }
    MpnPrompt();
    const char *esrc = "-";
    int e = MpnPressEdge(now, &esrc); // (every frame: the press baseline follows the buttons outside a pin too)
    if (!g_mpn.on)
        return;
    if (!MpnActive() || !g_mdu.on) {
        MpnEnd("the duel is over", 0);
        return;
    }
    if (g_mpn.state == 1) {
        g_mdu.hold_until = now + 700;
        g_mdu.act_ms = now;
        PadinjBlock("melee-attack", g_mpn.win_end + 100);
    }
    if (g_mpn.state == 1) {
        if (g_mpn.wbroke) {     // v5-wpnpin: the weapon broke - the next press (after the break's pair) re-pins unarmed, never a weapon blow
            if (e) {
                g_mpn_np_used++;
                DULOG
                    ("DUEL: PIN press (%s) %d ms after the weapon broke: re-pin unarmed",
                     esrc, (int) (now - g_mpn.last_hit));
                if (MpnRepin(now))
                    return;
                MpnEnd("weapon broke, no re-pin", 1);
                return;
            }
        } else if (e && now < g_mpn.last_hit + MPN_NEXT_MS) {
            if (!g_mpn.buf_at) {
                g_mpn.buf_at = now;
                g_mpn_np_used++;
                DULOG
                    ("DUEL: PIN press (%s) buffered (%d ms after the blow, fires at +%d ms)",
                     esrc, (int) (now - g_mpn.last_hit), MPN_NEXT_MS);
            } else
                PLOG("DUEL: PIN press (%s) %d ms after the blow REFUSED: one press is already buffered", esrc, (int) (now - g_mpn.last_hit));
        } else if (e
                   || (g_mpn.buf_at
                       && now >= g_mpn.last_hit + MPN_NEXT_MS)) {
            if (e) {
                g_mpn_np_used++;
                DULOG
                    ("DUEL: PIN press (%s) %d ms after the blow: next blow",
                     esrc, (int) (now - g_mpn.last_hit));
            }
            MpnBlowStart(now);
            return;
        }
        if (now >= g_mpn.win_end) {
            DULOG
                ("DUEL: PIN no press in the window after blow %d (%lld ms since it, to the pair's hand-off + %d ms): presses seen in this pin %d (used %d) | frames in the pin: F down %d, window unfocused %d, engine melee-attack on %d | pad melee bit %016llx on joypad %p held %d, edges %u, last edge %lld ms ago",
                 g_mpn.k, (long long) (now - g_mpn.last_hit), MPN_AFTER_MS,
                 g_mpn_np_seen, g_mpn_np_used, g_mpn_f_frames,
                 g_mpn_unfoc_frames, g_mpn_cmd_frames,
                 (unsigned long long) (g_padi.mel_w ? g_padi.mel_w->
                                       mask : 0), g_padi.jp,
                 PadinjMelHeld(), PadinjMelPresses(),
                 g_padi.raw_mel_ms ? (long long) (now -
                                                  g_padi.
                                                  raw_mel_ms) : -1LL);
            MpnEnd("no press", 1);
            return;
        }
    } else if (g_mpn.state == 2) {
        if (e)
            PLOG("DUEL: PIN press (%s) REFUSED: blow %d's swing is still running (contact in %lld ms) - press again after it lands", esrc, g_mpn.k + 1, (long long) (g_mpn.due > now ? g_mpn.due - now : 0));
        if (now >= g_mpn.due)
            MpnBlowContact(now);
    }
}
