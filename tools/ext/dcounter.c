// dcounter.c - the DODGE COUNTER: after a successful dodge, a melee press inside a short window plays TLOU2's own connected counter instead of an empty swing
// (instead of a regular 'empty' swing it plays a connected, SP one; the counter must NOT decide the fight, since it would almost guarantee the initial attacker loses if they
// stay in melee). Included after melee_dodge.c in t2r-ext.c. Log prefix "dcounter:".
// Opt-out: t2r-no-dodge-counter.txt next to the exe (re-read every 2 s).
//
// It is a psync pair of kind PS_CTXDOWN whose rows (ctxdown.c CD_PAIRS kind 3 / 4 / 5, names "dc-*") carry the hit moment and the hand-off: the dodger plays the SP counter
// attack half (abby-fist-dodge-back-attack--abby, ellie-1hblade-dodge-back-attack--ellie ...), the over-extended attacker the matching "-hit--npc-f" reaction half, around ONE
// apReference on every client (paired.c). Nothing else in the pair is new: start / ACK / abort / blend-out / path gate are the contextual takedowns'.
//  * Entered only from a skill moment: a dodge that really dropped the attacker's blow (melee_dodge.c mdg_hit_dodged / mdg_pair_dodged -> dc_arm) and a melee press by the
//    dodger from the end of the dodge lock (MD_MELEE_OK_MS) for DC_WINDOW_MS. One try per dodge. 4 s cooldown for the dodger (no counter chains).
//  * Who decides: the dodger's client starts it (as it decided the dodge); the over-extended attacker's client accepts or refuses (it owns its body): refused while it is
//    down / dying / in a paired move, or within DC_VIC_IMM_MS of the end of a previous counter on it. A refusal = the plain melee hit of the press.
//  * Damage: ONE normal melee blow of the dodger's weapon (the same hit message and the victim-side 2013 rescale as a swing, fists 20 / blunt 26 / machete 40), sent by the
//    dodger at the authored hit moment of the attack half (the weapon-trail event of the clip). A would-kill blow is the Melee Down (never a kill). The payoff is the cinematic
//    moment + tempo, not damage: both halves end at the same instant (psync hand-off blend), no stagger on top, the victim's miss lock is cleared, its dodge cooldown untouched.
//  * Third parties: both bodies stay hittable; ANY hit from a third party (or the world) breaks the pair at once (psync_abort with ABORT to the partner): both blend out.
//  * Environment: the engine's own wall / low-cover probe (the contextual takedowns') picks TLOU2's contextual combo-hit pair of the same weapon set (kind 4 / 5 rows); open ground
//    uses the dodge-counter pair (kind 3); the path gate (rays at both bodies' end spots) only chooses between them. Plain melee hit only if nothing fits (logged).
#define DC_WINDOW_MS 600        // melee press window, counted from the end of the dodge lock (or the dodged blow, whichever is later)
#define DC_COOLDOWN_MS 2200     // the dodger can't counter again before this (no chains); from the counter's START, ~1 s after its hand-off (was 4000;
                                  // 3 dodges did not allow a counter attack: every one of them fell in the 4 s after a counter)
#define DC_VIC_IMM_MS 3000      // the countered attacker can't be pulled into another scripted exchange before this
#define DC_MAX_DIST 3.8f
#define DC_MIN_DIST 0.25f
#define DC_ATT_MOVE_MAX_ENV 2.2f        // wall / low-cover rows: the victim is pinned to the surface, so no step-in - the old limit
#define DC_VIC_STEP 0.6f        // max step-in of the countered player toward the dodger (the space between the two is the dodge's own path: open)
#define DC_ATT_MOVE_MAX 2.8f    // (2.2 -> 2.8 with the victim step-in below: a gap no longer falls back to the plain hit)
// DC_ATT_MOVE_MAX: our body to the authored attacker spot: the root-motion dodge (melee_dodge_rm.h) carries the dodger up to 1.8 m away, so the counter steps back in (ctxdown keeps CD_ATT_MOVE_MAX 1.4)
#define DC_COLLAPSE_S 1.50f     // the haymaker main clip's victim has SETTLED on the floor by here (s after the contact). measured on a 60 fps capture:
                                 // 1.05 handed off mid-fall (decoded: head 1.00 m at 1.0 s, 0.53 at 1.2, 0.15-0.18 from 1.4 s) - the lying pose then flopped the body down
#define DC_DMG DC_COUNTER_DMG   // 20 (was 40). Old line: #define DC_DMG 40.0f                  // the counter deals 40 (was 2 x the old fist 20; kept at 40 with the v5-dmg table's fist 15). > 25 so the victim's per-class rescale (<= 25 only) leaves it alone; a would-kill blow is the Melee Down
#define DC_PIN_ESC_DMG 20.25f   // the pin-escape counter: 20 (v5-dmg keeps it; the old fist value; ps55 keeps it at 20 while the dodge counter is 40 again)
uint64_t g_dcPinEscMs;       // melee_duel_pin.c: our pin escape press (GetTickCount64) - the counter that follows it is a pin escape
// a pin escape's counter deals one fist hit (20)
static float DcDmg(void)
{
    return g_dcPinEscMs
        && GetTickCount64() - g_dcPinEscMs <
        4000 ? DC_PIN_ESC_DMG : DC_DMG;
}

#define DC_PLAIN_DMG FIST_DAMAGE        // the plain fallback hit (refused / nothing fits): the value of an ordinary swing, rescaled by the victim per weapon class
#define DC_LEAD_MIN 0.30f       // clip seconds between the START and the hit: the telegraph the escape window sits in (open rows start <= hit - this into the clip)
#define DC_VIC_RECOVER_MS 650   // the countered player's stagger after a landed counter (dc_finish)
#define DC_ESC_WIN_MS 0         // a counter that cancelled itself (the opponent escaped, then downed us 0.8 s later) - a counter earned
                                  // by a dodge always lands - the escape is off (t2r-dodge-counter-escape-ms.txt "130" = the earlier escape). Was 130; before that 240.
//        // the countered attacker's escape-dodge window = 50 % of MD_WIN_MS (480): shorter than a normal dodge window and than the 380 ms MD_REACT_MS, so it rewards reading the lunge
                                  // (it is also 92 % of the 260 ms second-dodge window: the countered attacker is not penalised twice). t2r-dodge-counter-escape-ms.txt = another value in ms.
#define DC_ESC_LATE_MS 60       // the window runs this long past the authored hit (latency forgiveness: the blow itself reaches the victim one-way later)
#ifndef PS_AB_ESCAPED
#define PS_AB_ESCAPED 8         // psync ABORT reason: the countered attacker escape-dodged
#endif
static void MexNotePairEnd(int slot, int ms);        // melee_exchange.c
static int MexOptout(const char *name, int *v, uint64_t * at);
#define DC_PEND_FRAMES 20       // the path gate's rays are read next frame; give up (plain hit) after this many frames
typedef struct {
    int row;
    float ap[7], p0;
} DcCand;
static struct {
    uint32_t arm_slot;
    uint64_t arm_until;
    int arm_dir;
    uint64_t cool_until, vic_imm_until;
    int pend, pend_slot, pend_dir;
    uint64_t pend_f;
    uint64_t pend_w, draw_w;    // draw_w (v5-fb3): the owned melee weapon drawn into the hand for this press
    int on_att, on_vic;
    uint32_t uid;
    int row, slot;
    float hit_s, end_s;
    int hit_sent;
    uint64_t t0_ms, hit_exp_ms, w;
    int break_req;
    char break_why[64];
    int exp_set, down_ext;      // down_ext: the hand-off was moved to the collapse (the blow downed the victim)
    int escaped;
    uint64_t esc_ms;
    int esc_slot;               // we escape-dodged a counter (survives dc_finish: a late blow of it is dropped)
    unsigned rr;
    int n_try, n_started, n_plain, n_refused, n_broken, n_escaped;
} g_dcm;

// The victim ("-hit") half starts at CONTACT, not with the attacker half: SP plays it as the synced hit of the attack (SP trace: abby-fist-dodge-back-attack--abby at F=29402, its -hit--abby half new at F=29430 = +0.47 s). Started together
// (the two did not form a connected hit; dc-fist-back) the victim staggered 0.35 m
// away before the blow and the bodies were 2.0 m apart at the 0.33 s trail event (1.5 m at the closest). With the victim half 0.47 s late the
// roots are 0.7-0.9 m apart from contact on (the same log, recomputed). Other rows: the trail event + the same 0.14 s.
#define DC_CONTACT_LATE 0.14f
static float DcLagRow(int row)
{
    if (row < 0 || CD_PAIRS[row].kind != 3 || CD_PAIRS[row].nseg > 1)
        return 0.0f;            // (a chain's victim waits through segment 0 instead: dc_seg0_hold)
    if (!strcmp(CD_PAIRS[row].name, "dc-fist-back"))
        return 0.47f;
    return CD_PAIRS[row].dmax + DC_CONTACT_LATE;
}

// a dodge counter pair (open-ground rows, kind 3)
static int DcIsCounter(uint64_t move)
{
    for (size_t i = 0; i < CD_N; i++)
        if (FNV64(CD_PAIRS[i].name) == move)
            return CD_PAIRS[i].kind == 3;
    return 0;
}

// chain rows (the haymaker): the victim holds the main clip's first frame through the attacker-only intro
static int DcSeg0Hold(uint64_t move)
{
    for (size_t i = 0; i < CD_N; i++)
        if (FNV64(CD_PAIRS[i].name) == move)
            return CD_PAIRS[i].kind == 3 && CD_PAIRS[i].nseg > 1;
    return 0;
}

// the haymaker only: its main clip is TLOU2's kill (the victim collapses by ~1.0 s), so a downing / would-kill blow plays on to that collapse.
// Other chains (the knife palm bash) stagger and stay up.
static int DcCollapses(uint64_t move)
{
    return DcSeg0Hold(move) && move == FNV64("dc-fist-haymaker");
}

static int DcDownSupine(void)
{
    return g_ps.role == 2 && g_ps.kind == PS_CTXDOWN
        && DcCollapses(g_ps.move)
        && GetFileAttributesA("t2r-dc-down-morph.txt") ==
        INVALID_FILE_ATTRIBUTES;
}

static float DcVicLagOf(uint64_t move)
{
    static int off = -1;
    static uint64_t at;
    uint64_t now = GetTickCount64();
    if (off < 0 || now - at >= 2000) {
        at = now ? now : 1;
        FILE *f = ExeSideOpen("t2r-dc-no-contact-lag.txt", "r");
        off = f != NULL;
        if (f)
            fclose(f);
    }
    if (off)
        return 0.0f;
    for (size_t i = 0; i < CD_N; i++)
        if (FNV64(CD_PAIRS[i].name) == move)
            return DcLagRow((int) i);
    return 0.0f;
}

// the blow lands at the contact (the victim half's start) when the pair has one, else at the trail event
static float DcHitRow(int row)
{
    float l = DcVicLagOf(FNV64(CD_PAIRS[row].name));
    return l > 0.0f ? l : CD_PAIRS[row].dmax;
}

// Melee camera on both screens (both perspectives have the melee camera activated alongside the connected punch, easing
// out towards the end so it is back to the normal camera when both players have input). The engine's own melee camera follows a melee ACTION (none runs during the
// pair: our swing is cancelled, the clips play on a psync layer) and has no request native, so the execution camera's procedural T1 melee framing (xcam role 1:
// over our shoulder at the other body, 1.19 m right / 0.63 m back, chest + 0.3) runs on each client from its own body, eased in over 0.45 s (SP measured) and out
// over 0.4 s that ENDS at the hand-off. Off: t2r-no-dc-cam.txt.
static int g_dcCam;
static void DcCam(uint64_t other_h, float left_s)
{
    g_dcCam = 0;
    // the execution-camera framing cut the view for a counter too; SP plays its dodge-counter on the gameplay camera.
    // Off by default (melee_spcam.c frames it); t2r-dc-cam.txt = the old framing.
    FILE *f = ExeSideOpen("t2r-dc-cam.txt", "r");
    if (!f)
        return;
    fclose(f);
    if (!other_h || left_s < 0.6f)
        return;
    if (!XcamStart(other_h, left_s + 0.5f, 0, -1, left_s, 1, "ctxdown")) {
        Xlog("dcounter: no melee camera (exec camera off / no render hook)");
        return;
    }
    g_xcamBinF = 0.45f * 60.0f;
    g_xcam_bout_f = 0.4f * 60.0f;
    g_xcam_dur = S60(left_s);
    g_dcCam = 1;
    Xlog("dcounter: melee camera on us for %.2f s (in 450 ms, out 400 ms ending at the hand-off)", left_s);
}

// the hand-off moved later (the collapse): the melee camera's hand-back moves with it, so it still ends AT the hand-off (otherwise the camera left
// 0.45 s before the down while the body was still falling)
static void DcCamExtend(float by_s)
{
    if (!g_dcCam || !(by_s > 0.0f))
        return;
    g_xcam_dur += S60(by_s);
    g_xcam_kill += by_s;
    Xlog("dcounter: melee camera extended by %.2f s to the collapse hand-off", (double) by_s);
}

static void DcCamAbort(const char *why)
{
    if (g_dcCam) {
        g_dcCam = 0;
        XcamStop(why);
    }
}

// v5-cam6 (the PINNER's screen of a wall escape showed the wall with both bodies out of frame): the escaper plays its SP player half's
// own animated camera (melee_duel_pin.c mpn_escape); the pinner - the NPC side in SP, no authored camera - gets the open-side two-shot of the pair framing camera
// (xcam role 2, g_xcam_pesc: t2r-ext.c xcam_victim_frame), blended in from the frozen pin camera (pincam5 hand-over), out over 0.6 s ending 0.3 s after the
// hand-off (the countered pinner's 650 ms stagger keeps the move locked). Opt-out t2r-no-pesc-pinner-cam.txt.
static void DcPescCam(uint64_t other_h, float left_s, const char *pair)
{
    FILE *f = ExeSideOpen("t2r-no-pesc-pinner-cam.txt", "r");
    if (f) {
        fclose(f);
        Xlog("dcounter: pin escape %s on us (the pinner): no escape camera (t2r-no-pesc-pinner-cam.txt)", pair);
        return;
    }
    if (!other_h || left_s < 0.6f)
        return;
    char nm[64];
    snprintf(nm, sizeof nm, "ctxdown:%s", pair);        // ("ctxdown" in the name: no execution music on a role-2 camera)
    if (!XcamStart(other_h, left_s + 0.5f, 0, -1, left_s, 2, nm)) {
        Xlog("dcounter: pin escape %s: no escape camera for the pinner (exec camera off / no render hook)", pair);
        return;
    }
    g_xcamPesc = 1;
    g_xcamBinF = 0.45f * 60.0f;
    g_xcam_bout_f = 0.6f * 60.0f;
    g_xcam_dur = S60(left_s + 0.3f);
    g_dcCam = 1;
    Xlog("dcounter: pin escape %s on us (the pinner): open-side escape camera for %.2f s (in 450 ms from the pin camera, out 600 ms ending 0.3 s after the hand-off)", pair, (double) left_s);
}

// The haymaker / knife palm bash are
// opt-in now (t2r-dc-haymaker.txt); the counter is SP's own dodge-attack pair of the weapon set, back and forward variants in turn
// ps55 (weighty counters are back; the lighter ones made the opponent wait to get hit and
// played concussion for the victim): the heavy counters are the DEFAULT again - fists = dc-fist-haymaker (the victim holds the main clip's
// first frame through the intro, dc_seg0_hold; mhfx_counter on the victim), knife = its palm bash. Inside a duel melee_duel.c mdu_dc_route hands these two
// sets to the dc pair (the other weapon sets keep SP's dodge attack as a duel hit). t2r-dc-no-haymaker.txt = SP's varied dodge attacks.
static int DcOffHeavy(void)
{
    static int v = -1, lg = -1;
    static uint64_t at;
    uint64_t n = GetTickCount64();
    if (v < 0 || n - at > 2000) {
        at = n;
        FILE *f = ExeSideOpen("t2r-dc-no-haymaker.txt", "r");
        v = f != NULL;
        if (f)
            fclose(f);
    }
    if (lg != v) {
        lg = v;
        Xlog("dcounter: ps55 heavy counters %s",
             v ? "OFF (t2r-dc-no-haymaker.txt): SP's varied dodge attacks"
             :
             "ON - fists: the haymaker (victim held through the intro, heavy concussion), knife: the palm bash; in a duel too");
    }
    return v;
}

static int DcLongSlide(void)
{
    return GetFileAttributesA("t2r-dc-long-slide.txt") !=
        INVALID_FILE_ATTRIBUTES;
}

static int DcHeavyRow(int r)
{
    return CD_PAIRS[r].nseg > 1
        || strstr(CD_PAIRS[r].name, "-palm") != NULL;
}

static int DcOff(void)
{
    static int off = -1;
    static uint64_t at;
    uint64_t now = GetTickCount64();
    if (off < 0 || now - at >= 2000) {
        at = now ? now : 1;
        FILE *f = ExeSideOpen("t2r-no-dodge-counter.txt", "r");
        int o = f != NULL;
        if (f)
            fclose(f);
        if (o != off)
            Xlog("dcounter: %s",
                 o ? "OFF (t2r-no-dodge-counter.txt)" :
                 "on (a melee press within 0.6 s after a dodge that dropped a blow plays the connected SP counter; 2.2 s cooldown)");
        off = o;
    }
    return off;
}

static int MduDcRoute(uint32_t slot, int dir, uint64_t h);    // melee_duel.c
static int DcArmedAt(uint64_t now)
{
    return !DcOff() && g_dcm.arm_until && now <= g_dcm.arm_until;
}

// melee_dodge.c: our dodge really dropped a blow / refused a pair from `slot`
static void DcArm(uint32_t slot)
{
    if (DcOff() || slot >= T2R_MAX_PEERS)
        return;
    if (Mv3Stand("dcounter: counter arm after a dodge", slot))
        return;                 // melee v3: the sim's dodge-counter (MV3_S_COUNTER) inside a bubble
    if (g_ps.role && g_ps.kind == PS_CTXDOWN) {
        int pr = CdPairOfMove(g_ps.move);
        if (pr >= 0 && CD_PAIRS[pr].kind >= 3)
            return;
    }                           // we dodged a counter's START: no counter-counter
    if (GetTickCount64() < g_dcm.cool_until)
        return;
    // the escape dodge out of a counter dropped the
    // countering player's trailing swing and armed a counter for the escapee - a counter-counter (a player who dodged and countered got countered back). An escape only saves you; no counter from it.
    if (g_dcm.escaped && (int) slot == g_dcm.esc_slot
        && GetTickCount64() - g_dcm.esc_ms < 1500) {
        Xlog("dcounter: no counter on slot %u - that dodge was the escape out of its counter", slot);
        return;
    }
    uint64_t now = GetTickCount64(), base =
        g_mdg.melee_ok_at > now ? g_mdg.melee_ok_at : now;
    g_dcm.arm_slot = slot;
    g_dcm.arm_dir = g_mdg.combat_dir;
    g_dcm.arm_until = base + (uint64_t) SkCounterWindowMs(); // Counter Fighter: 600 -> 850 ms
    if (Skl(SK_RIPOSTE))
        Xlog("skills: COUNTER FIGHTER %d: counter window %d ms (base %d), cooldown %d ms", Skl(SK_RIPOSTE), SkCounterWindowMs(), DC_WINDOW_MS, SkCounterCoolMs());
    Xlog("dcounter: armed on slot %u (dodge dir %d): a melee press between +%llu and +%llu ms counters", slot, g_dcm.arm_dir, (unsigned long long) (g_mdg.melee_ok_at > now ? g_mdg.melee_ok_at - now : 0), (unsigned long long) (g_dcm.arm_until - now));
}

// v5-fb3: a melee ITEM (not a gun / throwable / the fists). The held id with a gun out is the gun: log 74907 / 75034: the pin escape's blow went out as
// the pistol (226cdcf298320a8c) while the pipe wrench sat on the back
static int DcIsMelee(uint64_t w)
{
    return w && w != ~0ULL && w != FNV64("abby-unarmed")
        && !BaseWeaponIsGun(BaseWeapon(w)) && !WeaponIsThrowable(w);
}

static uint64_t DcWeapon(void)
{
    uint64_t none = 0, hw = DcCall(SID_HAS_MELEE_WEAPON, 0, &none);
    if (hw != ~0ULL && (hw & 0xff)) {
        uint64_t w = CurrentWeapon();
        if (DcIsMelee(w))
            return w;
    }
    return FNV64("abby-unarmed");
}

// v5-fb3 (counters with melee weapons played the wrong animations, the weapon was not in the hands): log 77025 dc-1hblunt-back
// played with the FISTS (blow weapon 6ac05fba9c921900, another client's log 92398 the same) while the pipe wrench was on the back (stowed 71462 by the downed
// kick clip): cd_cur_style() read player-has-non-basic-melee-weapon? = 1 (owned) and cd_weapon_mask(the held fists) fell back to the 1h-blunt mask, so the
// 1h blunt counter swung an empty fist (capture: bare fist overhead, wrench on the backpack). SP never counters with a melee weapon in
// the bag: the press draws it. Now an owned melee weapon that is not in the hand (fists / a gun / nothing held) is drawn instantly at the counter press
// and the counter set is that weapon's class; t2r-dc-no-draw.txt = old behaviour.
static uint64_t DcDrawMelee(void)
{
    g_dcm.draw_w = 0;
    static int off = -1;
    static uint64_t at;
    if (off < 0 || GetTickCount64() - at > 2000) {
        at = GetTickCount64();
        off =
            GetFileAttributesA("t2r-dc-no-draw.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    if (off)
        return 0;
    uint64_t none = 0, hw = DcCall(SID_HAS_MELEE_WEAPON, 0, &none);
    if (hw == ~0ULL || !(hw & 0xff))
        return 0;
    uint64_t cw = CurrentWeapon();
    if (cw == ~0ULL)
        cw = 0;
    if (DcIsMelee(cw))
        return 0;               // already in the hand
    uint64_t w = XrMeleeWeapon(cw);
    if (!DcIsMelee(w)) {
        Xlog("dcounter: we own a melee weapon but none is known (held %llx, last %llx) - the fist counter", (unsigned long long) cw, (unsigned long long) g_lastMelee);
        return 0;
    }
    uint64_t r = XrDraw(w);
    Xlog("dcounter: melee weapon %llx drawn into the hand for the counter (held %llx: %s) -> %llx", (unsigned long long) w, (unsigned long long) cw, !cw || cw == FNV64("abby-unarmed") ? "fists / nothing" : "a gun / throwable", (unsigned long long) r);
    g_dcm.draw_w = w;
    return w;
}

// the counter's weapon set: the drawn weapon's class while its draw lands (current_weapon may still say the fists / gun for a frame or two)
static int DcStyle(void)
{
    int s = CdCurStyle();
    if (s >= CD_WSET_BASE && g_dcm.draw_w)
        s = CD_WSET_BASE + CdWeaponMask(g_dcm.draw_w);
    return s;
}

static void DcPlainHit(int slot, const char *why)
{
    if (slot < 0 || slot >= T2R_MAX_PEERS || g_peerDowned[slot]
        || g_puppetDeadHere[slot])
        return;
    typedef void (*fn_hit3)(uint32_t, uint64_t, float);
    uint64_t w = g_dcm.w ? g_dcm.w : DcWeapon();
    ((fn_hit3) T2R_SYM(kFnApplyHit)) ((uint32_t) slot, w, DC_PLAIN_DMG);
    g_kdFallbackFrame = g_t60;
    g_kdFallbackSlot = slot;
    g_dcm.n_plain++;
    Xlog("dcounter: plain melee hit to slot %d instead (%s)", slot, why);
}

// the clip's authored start spots composed with an ap that anchors the VICTIM half where the victim stands and turns the pair so the authored attacker spot points at us
// (ctxdown.c cd_upg_start's solution), at clip time p0. 1 = ok; *da = how far our body is from the attacker half's authored spot at p0
static float g_dcSa[3];        // dc_anchor: the authored attacker spot (world) of its last answer
static int DcAnchor(int row, uint64_t h, const float *vl, const float *me,
                     float p0, float *ap, float *da)
{
    float cv[8], ca[8], a8[8], Sv[8], sa[8];
    if (!CdOffsetAt(h, CD_VIC0(row), p0, cv)
        || !CdOffsetAt(SID_PLAYER, CD_ATT0(row), p0, ca))
        return 0;
    float dx = me[0] - vl[0], dz = me[2] - vl[2];
    if (dx * dx + dz * dz < 0.0025f)
        return 0;
    float want = __builtin_atan2f(dx, dz), got0 =
        __builtin_atan2f(ca[0] - cv[0], ca[2] - cv[2]), yaw = 0.0f, best =
        1e9f, cand[2] = { want - got0, got0 - want };
    for (int k = 0; k < 2; k++) {
        float ap0[7] =
            { 0, 0, 0, 0, __builtin_sinf(cand[k] * 0.5f), 0,
     __builtin_cosf(cand[k] * 0.5f) };
        PsAp8(ap0, a8);
        PsCompose(a8, cv, Sv);
        PsCompose(a8, ca, sa);
        float r =
            __builtin_fabsf(CdWrap180
                            ((want -
                              __builtin_atan2f(sa[0] - Sv[0],
                                               sa[2] -
                                               Sv[2])) * 57.29578f));
        if (r < best) {
            best = r;
            yaw = cand[k];
        }
    }
    if (best > 5.0f)
        return 0;
    float ap0[7] =
        { 0, 0, 0, 0, __builtin_sinf(yaw * 0.5f), 0,
 __builtin_cosf(yaw * 0.5f) };
    PsAp8(ap0, a8);
    PsCompose(a8, cv, Sv);
    PsCompose(a8, ca, sa);
    ap[0] = vl[0] - Sv[0];
    ap[1] = vl[1] - Sv[1];
    ap[2] = vl[2] - Sv[2];
    ap[3] = 0;
    ap[4] = __builtin_sinf(yaw * 0.5f);
    ap[5] = 0;
    ap[6] = __builtin_cosf(yaw * 0.5f);
    PsAp8(ap, a8);
    PsCompose(a8, ca, sa);
    *da =
        __builtin_sqrtf((sa[0] - me[0]) * (sa[0] - me[0]) +
                        (sa[2] - me[2]) * (sa[2] - me[2]));
    g_dcSa[0] = sa[0];
    g_dcSa[1] = sa[1];
    g_dcSa[2] = sa[2];
    return 1;
}

// the rows of one environment kind for the held weapon set; fw = the dodge was forward (a "-fw" row where one exists)
static int DcRows(int style, int kind, int fw, int *out)
{
    for (int pass = 0; pass < 2; pass++) {
        int n = 0;
        for (size_t i = 0; i < CD_N && n < 8; i++) {
            if (CD_PAIRS[i].kind != kind
                || !CdStyleOk(style, CD_PAIRS[i].style)
                || !CdPairLoaded((int) i))
                continue;
            if (kind == 3
                && (strstr(CD_PAIRS[i].name, "-fw") != NULL) != (pass ==
                                                                 0 ? fw :
                                                                 0))
                continue;
            out[n++] = (int) i;
        }
        if (n || !fw)
            return n;
    }
    return 0;
}

// the engine's wall / low-cover probe behind the victim (the contextual takedowns' own): the surface frame as an ap, 1 = found
static int DcProbe(uint64_t h, const float *V, const float *me, int high,
                    float *ap7, float *dw)
{
    if (!CdEngineOk())
        return 0;
    uint8_t *pr =
        h ? ((uint8_t * (*)(uint64_t)) T2R_SYM(kFnEntityFromHandle)) (h) :
        NULL;
    const char *why = "";
    uint8_t *probes = pr ? CdProbesOf(pr, &why) : NULL;
    if (!probes)
        return 0;
    float dirv[3] = { V[0] - me[0], V[1] - me[1], V[2] - me[2] }, loc[8];
    if (!CdEngineProbe(probes, dirv, 1.6f, high, loc))
        return 0;
    ap7[0] = loc[0];
    ap7[1] = loc[1];
    ap7[2] = loc[2];
    ap7[3] = loc[4];
    ap7[4] = loc[5];
    ap7[5] = loc[6];
    ap7[6] = loc[7];
    float qn =
        __builtin_sqrtf(ap7[3] * ap7[3] + ap7[4] * ap7[4] +
                        ap7[5] * ap7[5] + ap7[6] * ap7[6]);
    if (!(qn > 0.5f))
        return 0;
    for (int k = 3; k < 7; k++)
        ap7[k] /= qn;
    *dw =
        __builtin_sqrtf((loc[0] - V[0]) * (loc[0] - V[0]) +
                        (loc[2] - V[2]) * (loc[2] - V[2]));
    return 1;
}

// the candidates, best first: the contextual (high wall, then low cover) pair if the engine's probe finds a surface behind the victim and the bodies fit its authored spots,
// then the open-ground counter. Returns the count; `why` collects why a variant was left out.
static int DcPlan(int slot, int dir, int style, DcCand *out, char *why,
                   size_t wn)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    uint8_t *peer = ((fn_peer) T2rPeer) ((uint32_t) slot);
    uint64_t h = peer ? *(uint64_t *) (peer + 0x60) : 0;
    float vb[16], mb[16];
    const float *vl = h ? PuppetLocator(h, vb) : NULL, *me =
        PlayerLocator(mb);
    why[0] = 0;
    if (!vl || !me || !PsLocOk(vl) || !PsLocOk(me)) {
        snprintf(why, wn, "no locators");
        return 0;
    }
    float V[3] = { vl[0], vl[1], vl[2] };
    int n = 0, rows[8];
    for (int kind = 4; kind <= 5; kind++) {
        if (!DcRows(style, kind, 0, rows))
            continue;
        float ap7[7], dw;
        if (!DcProbe(h, V, me, kind == 4, ap7, &dw))
            continue;
        int row =
            rows[g_dcm.rr % (unsigned) DcRows(style, kind, 0, rows)];
        float mv, da, sa[3], vyaw;
        if (!CdSpotsAt(row, h, ap7, V, me, 0.0f, &mv, &da, sa, &vyaw)) {
            snprintf(why + strlen(why), wn - strlen(why),
                     " [%s: no authored spots]", CD_PAIRS[row].name);
            continue;
        }
        float ay = CdApYaw(ap7) * 0.0174533f, fx =
            __builtin_sinf(ay), fz = __builtin_cosf(ay), vx =
            me[0] - ap7[0], vz = me[2] - ap7[2], vlen =
            __builtin_sqrtf(vx * vx + vz * vz);
        float ang =
            vlen >
            0.05f ?
            __builtin_acosf(fmaxf
                            (-1.0f,
                             fminf(1.0f,
                                   (vx * fx +
                                    vz * fz) / vlen))) * 57.29578f : 0.0f;
        if (mv > CD_VIC_MOVE_MAX_PUSH || da > DC_ATT_MOVE_MAX_ENV
            || ang > CD_ATT_ANGLE_MAX) {
            snprintf(why + strlen(why), wn - strlen(why),
                     " [%s: victim moves %.2f, we %.2f, %.0f deg off the frame - out of range]",
                     CD_PAIRS[row].name, mv, da, ang);
            continue;
        }
        out[n].row = row;
        memcpy(out[n].ap, ap7, sizeof ap7);
        out[n].p0 = 0.0f;
        n++;
        Xlog("dcounter: %s surface behind slot %d at %.2f m (probe): %s fits (victim %.2f m, we %.2f m, %.0f deg)", kind == 4 ? "high wall" : "low cover", slot, dw, CD_PAIRS[row].name, mv, da, ang);
        break;
    }
    int nr = 0;
    // v5-fb3 (a wrench counter played a stab animation): log 78425 dc-1hblunt-fw (abby-1hblunt-dodge-fw-attack, SP's forward-dodge
    // jab) after a BACK dodge (dir 3), the pair sliding us 2.13 m in (capture: the wrench thrust into the belly). SP plays a weapon's
    // forward variant only out of a forward dodge; a weapon set now follows the dodge's direction (forward -> the "-fw" row, else the back rows; the
    // other kind when the set has none). The fists keep both in one list (SP's varied anims).
    int wset_dir = style >= CD_WSET_BASE;
    {
        int a[8], na = wset_dir && dir == 0 ? 0 : DcRows(style, 3, 0, a), b[8], nb = wset_dir && dir != 0 && na ? 0 : DcRows(style, 3, 1, b); // back + forward variants, one list (SP variety)
        for (int i = 0; i < na + nb && nr < 8; i++) {
            int r = i < na ? a[i] : b[i - na], dup = 0;
            if (DcHeavyRow(r) && DcOffHeavy())
                continue;
            for (int j = 0; j < nr; j++)
                dup |= rows[j] == r;
            if (!dup)
                rows[nr++] = r;
        }
        (void) dir;
    }
    int heavy = -1;
    if (n < 2) {
        int all[8], na = DcRows(style, 3, 0, all);
        for (int i = 0; i < na; i++)
            if (CD_PAIRS[all[i]].nseg > 1
                || strstr(CD_PAIRS[all[i]].name, "-palm")) {
                heavy = all[i];
                break;
            }
    }
    if (heavy >= 0 && DcOffHeavy())
        heavy = -1;
    if ((nr || heavy >= 0) && n < 2) {
        int row = heavy >= 0 ? heavy : rows[g_dcm.rr % (unsigned) nr];
        // v5-axectr (the axe counter used the same anim as the blunt one): a hatchet / tomahawk (1h blade, not the machete set) takes
        // the 1h blade set's HEAVY back-dodge attack (ellie-1hblade-dodge-bk-heavy-atk: the overhead chop) over the plain back slash. t2r-dc-axe-slash.txt = the slash.
        if (heavy < 0 && style >= CD_WSET_BASE
            && ((style - CD_WSET_BASE) & 0x12) == 0x02 && !DcOffHeavy()
            && GetFileAttributesA("t2r-dc-axe-slash.txt") ==
            INVALID_FILE_ATTRIBUTES)
            for (int i = 0; i < nr; i++)
                if (!strcmp(CD_PAIRS[rows[i]].name, "dc-1hblade-heavy")) {
                    row = rows[i];
                    break;
                }
        float best = 1e9f, bap[7], bp0 = 0.0f, pmax =
            CD_PAIRS[row].dmax - DC_LEAD_MIN;
        if (pmax < 0.0f || CD_PAIRS[row].nseg > 1)
            pmax = 0.0f;        // (a chain's held victim stands on its first frame: no start skip)
        float bsa[3] = { 0, 0, 0 };
        for (float p0 = 0.0f; p0 <= pmax + 0.001f; p0 += 0.06f) {
            float ap[7], da;
            if (!DcAnchor(row, h, vl, me, p0, ap, &da))
                continue;
            if (da < best) {
                best = da;
                memcpy(bap, ap, sizeof bap);
                bp0 = p0;
                memcpy(bsa, g_dcSa, sizeof bsa);
            }
        }
        // The connected punch always sets the characters into place. Both bodies are pulled onto their authored spots (psync's SP pull);
        // a big gap is no longer closed by our body alone: the whole pair moves toward us by 40 % of it (max DC_VIC_STEP m), so the countered player
        // steps into the blow too and our slide is shorter - and a gap up to DC_ATT_MOVE_MAX still plays the pair instead of the plain hit.
        if (best < 1e8f && best > 0.5f) {
            float gx = me[0] - bsa[0], gz = me[2] - bsa[2], gl = __builtin_sqrtf(gx * gx + gz * gz), st = DcLongSlide()? fminf(DC_VIC_STEP, 0.4f * gl) : fminf(0.9f, 0.5f * gl);      // (0.8 -> 0.9 - a wrench opener from 2.0 m left a 1.88 m gap after a perfect back dodge, 0.08 m over: plain hit, no counter)
            if (gl > 0.01f) {
                bap[0] += gx / gl * st;
                bap[2] += gz / gl * st;
                Xlog("dcounter: gap %.2f m to the authored attacker spot - the pair moves %.2f m toward us (victim steps in, we slide %.2f m)", best, st, best - st);
                best -= st;
            }
        }
        // v5-dcslide (weird big travel after a counter; log 22283: gap 1.95 m, we slid 1.35 m): the pair splits the gap evenly (victim up to 0.8 m) and our
        // slide is capped at 1.0 m - a longer gap is the plain swing. t2r-dc-long-slide.txt = the earlier rule (40 %, our slide up to DC_ATT_MOVE_MAX).
        if (best <= (DcLongSlide()? DC_ATT_MOVE_MAX : 1.0f)) {
            out[n].row = row;
            memcpy(out[n].ap, bap, sizeof bap);
            out[n].p0 = bp0;
            n++;
        } else
            snprintf(why + strlen(why), wn - strlen(why),
                     " [%s: our body %.2f m from the authored attacker spot (max %.1f) or no anchor]",
                     CD_PAIRS[row].name, best, DC_ATT_MOVE_MAX);
    } else if (!nr)
        snprintf(why + strlen(why), wn - strlen(why),
                 " [no open-ground counter loaded for style %d]", style);
    return n;
}

static void DcLaunch(int slot, const DcCand *c, int dir)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    int row = c->row;
    uint8_t *peer = ((fn_peer) T2rPeer) ((uint32_t) slot);
    uint64_t h = peer ? *(uint64_t *) (peer + 0x60) : 0;
    float end_s = CD_PAIRS[row].end_s;
    uint64_t w = g_dcm.pend_w ? g_dcm.pend_w : DcWeapon();
    uint64_t mel = w == FNV64("abby-unarmed") ? 0 : w;
    {
        uint64_t me0 = SID_PLAYER, r =
            DcCall(0x394888944e949d5fULL /* cancel-melee-action */ , 1,
                    &me0);
        Xlog("dcounter: our swing cancelled for %s (-> %llx)",
             CD_PAIRS[row].name, (unsigned long long) r);
    }
    g_psStartSkipMs = c->p0 * 1000.0f;
    if (!PsyncStart
        (PS_CTXDOWN, (uint32_t) slot, CD_ATT0(row), CD_VIC0(row), c->ap,
         end_s, 0, 0xff, mel, FNV64(CD_PAIRS[row].name))) {
        g_psStartSkipMs = 0.0f;
        DcPlainHit(slot, "psync refused the pair (see the psync lines)");
        return;
    }
    g_cd.ended = 0;
    g_cd.plan_ok = 0;
    g_dcm.on_att = 1;
    g_dcm.down_ext = 0;
    g_dcm.on_vic = 0;
    g_dcm.uid = g_ps.uid;
    g_dcm.row = row;
    g_dcm.slot = slot;
    g_dcm.hit_s = DcHitRow(row);
    g_dcm.end_s = end_s;
    g_dcm.hit_sent = 0;
    g_dcm.t0_ms = GetTickCount64();
    g_dcm.w = w;
    g_dcm.break_req = 0;
    g_dcm.cool_until = g_dcm.t0_ms + (uint64_t) (Skl(SK_RIPOSTE) >= 2 ? SkCounterCoolMs() : DC_COOLDOWN_MS); // Counter Fighter II: 2.2 -> 1.5 s g_dcm.arm_until = 0; g_dcm.n_started++; g_dcm.rr++;
    CdHandsBegin(row);
    MduNoteDc(1);
    g_visVicAnim = CD_VIC0(row);
    g_vis_vic_puppet = h;
    g_vis_vic_frame = g_t60;
    g_mpose_att_frame = g_vis_vic_frame;
    g_mposeAttSlot = (int32_t) slot;
    g_finposVictim = (int32_t) slot;
    g_finposSlot = -1;
    g_finPlaceFrame = 0;
    g_execAttFrame = 0;
    FinposBegin(1, slot, 0, S60(end_s) + 60, "dodge counter, attacker");
    XfaceStart(SID_PLAYER, h, end_s + 0.6f);
    DcCam(h, end_s - c->p0);
    Xlog("dcounter: START %s on slot %d (%s, dodge dir %d): %s on us, %s on them, starts %.2f s into the clips, hit at %.2f s, hand-off at %.2f s, damage = one normal blow (%llx), in the hand %llx", CD_PAIRS[row].name, slot, CD_PAIRS[row].kind == 3 ? "open ground" : CD_PAIRS[row].kind == 4 ? "high wall" : "low cover", dir, CD_ATT0(row), CD_VIC0(row), c->p0, g_dcm.hit_s, end_s, (unsigned long long) w, (unsigned long long) CurrentWeapon());
}

static uint64_t MpnPinnedAt(void);    // melee_duel_pin.c
// one planning pass for the pending press. 1 = done (started, plain hit sent or nothing fits), 0 = ask again next frame (the path gate's rays)
static int DcResolve(void)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    int slot = g_dcm.pend_slot, dir = g_dcm.pend_dir;
    if (slot < 0 || slot >= T2R_MAX_PEERS || g_peerDowned[slot]
        || g_puppetDeadHere[slot] || g_downed || g_ps.role
        || LocalHp() <= 0.0f) {
        Xlog("dcounter: pending counter dropped (state changed)");
        return 1;
    }
    // v5-dcwait (the baseball bat counter showed a 1h anim): the press with the pistol out drew the bat instantly (log 13413) and the 2h
    // pair started the next frame (13414) while the engine still ran the pistol's arm state - the bat swung in the right hand, the left arm out wide (SP's clip
    // keeps the wrists 0.12-0.20 m apart, offline pose). The pair now waits until the drawn weapon is the engine's held weapon (12 frames max, then it goes
    // anyway). t2r-dc-no-draw-wait.txt = start at once.
    static uint64_t dw_ok_f;    // the press frame whose wait is over (the path gate may still hold the pair 1-3 frames)
    if (g_dcm.draw_w && dw_ok_f != g_dcm.pend_f
        && GetFileAttributesA("t2r-dc-no-draw-wait.txt") ==
        INVALID_FILE_ATTRIBUTES) {
        uint64_t none = 0, cw = CurrentWeapon(), gid =
            DcCall(FNV64("get-player-weapon-id"), 0, &none);
        if (g_frame - g_dcm.pend_f < 12) {
            if (cw != g_dcm.draw_w || gid != g_dcm.draw_w
                || g_frame - g_dcm.pend_f < 4)
                return 0;       // (log 13414: the controller already said the bat one frame after the instant draw, the arm state not yet - 4 frames minimum)
            Xlog("dcounter: drawn %llx is the held weapon %llu frames after the press - the pair starts", (unsigned long long) g_dcm.draw_w, (unsigned long long) (g_frame - g_dcm.pend_f));
        } else
            Xlog("dcounter: drawn %llx still not the held weapon after 12 frames (held %llx / %llx) - the pair starts anyway", (unsigned long long) g_dcm.draw_w, (unsigned long long) cw, (unsigned long long) gid);
        dw_ok_f = g_dcm.pend_f;
    }
    // If a counter would kill, a finisher plays instead of the counter: a counter blow that downs the victim (its net hp - DC_DMG <= 1) starts the
    // open-ground SP finisher of our weapon (ctxdown.c cd_contact_fin: the same pick and path gate as a lethal contact) instead of the counter pair; only when
    // no finisher fits does the counter below play (the haymaker collapse). t2r-dc-lethal-counter.txt = the old way (the counter pair always).
    if (GetFileAttributesA("t2r-dc-lethal-counter.txt") ==
        INVALID_FILE_ATTRIBUTES) {
        uint8_t *vp = MdgPeer((uint32_t) slot);
        uint64_t hk = PuppetHandle((uint32_t) slot);
        float nh = vp
            && !IsBadReadPtr(vp + 0xa5,
                             4) ? *(float *) (vp + 0xa5) : -1.0f, vhp =
            nh > 0.0f ? nh : (hk ? PuppetHp((uint32_t) slot, hk) : -1.0f);
        if (vhp > 0.0f && vhp - DcDmg() <= 1.0f) {
            if (CdContactFin((uint32_t) slot, g_dcm.pend_w, DcDmg())) {
                Xlog("dcounter: the counter would down slot %d (hp %.0f) - the SP finisher instead of the counter", slot, (double) vhp);
                return 1;
            }
            Xlog("dcounter: the counter would down slot %d (hp %.0f) but no SP finisher fits - the counter plays", slot, (double) vhp);
        }
    }
    int style = DcStyle(); {
        int r3[8], n3 = style == 3 ? DcRows(3, 3, 0, r3) : 0, ok3 = 0;
        for (int i = 0; i < n3; i++)
            ok3 |= !DcHeavyRow(r3[i]) || !DcOffHeavy();
        if (style == 3 && !ok3)
            style = -1;
    }                           // knife: its palm-bash counter, else the fist rows
    // (the full haymaker fall anim played and then morphed): the knife palm bash has no fall - a counter whose blow
    // downs the victim (its hp - DC_DMG <= 1) is the haymaker with its collapse for every weapon. t2r-dc-knife-always.txt = the palm bash anyway.
    if (style == 3 && !DcOffHeavy()
        && GetFileAttributesA("t2r-dc-knife-always.txt") ==
        INVALID_FILE_ATTRIBUTES) {
        uint64_t hk = PuppetHandle((uint32_t) slot);
        float vhp = hk ? PuppetHp((uint32_t) slot, hk) : -1.0f;
        if (vhp > 0.0f && vhp - DcDmg() <= 1.0f) {
            Xlog("dcounter: the blow downs slot %d (hp %.0f) - the haymaker with its collapse instead of the knife palm bash", slot, (double) vhp);
            style = -1;
        }
    }
    // right after a pin we still stand against the wall / van; the open-ground counter slid the pair into it (the path gate's straight
    // rays were clear, the clip's arc was not). Within 3 s of a pin pair on us: the plain hit (the pin's own escape pair goes through dc_launch, not here)
    {
        uint64_t pa = MpnPinnedAt(), nw = GetTickCount64();
        if (pa && nw - pa < 3000) {
            Xlog("dcounter: we were pinned %llu ms ago (still at the wall) - no open-ground counter, the plain hit", (unsigned long long) (nw - pa));
            DcPlainHit(slot, "just out of a pin");
            return 1;
        }
    }
    DcCand cand[3];
    char why[400];
    int n = DcPlan(slot, dir, style, cand, why, sizeof why);
    uint8_t *peer = ((fn_peer) T2rPeer) ((uint32_t) slot);
    uint64_t h = peer ? *(uint64_t *) (peer + 0x60) : 0;
    float vb[16], mb[16];
    const float *vl = h ? PuppetLocator(h, vb) : NULL, *me =
        PlayerLocator(mb);
    int wait = 0;
    for (int i = 0; i < n && vl && me; i++) {
        // the pair is handed off at end_s (both back to free play), so only the travel up to there must be free (test: a wall 1.0 m behind the countered
        // player blocked the clip's full 2.3 m stagger and the counter fell back to the plain hit); one phase for both halves: the victim's at the hand-off
        {
            float vd = PsyncDur(h, FNV64(CD_VIC0(cand[i].row))), lag =
                DcLagRow(cand[i].row);
            g_pgEndPh =
                vd >
                0.2f ? (CD_PAIRS[cand[i].row].end_s - lag) / vd : 0.0f;
        }
        int g =
            PgGate((uint32_t) slot, CD_ATT0(cand[i].row),
                    CD_VIC0(cand[i].row), cand[i].ap, vl, me, 0,
                    "dodge counter");
        g_pgEndPh = 0.0f;
        if (g == 2) {
            wait = 1;
            break;
        }
        if (g == 0) {
            Xlog("dcounter: %s - the path is blocked (path gate)%s",
                 CD_PAIRS[cand[i].row].name,
                 i + 1 < n ? ", next variant" : "");
            continue;
        }
        DcLaunch(slot, &cand[i], dir);
        return 1;
    }
    if (wait && g_frame - g_dcm.pend_f < DC_PEND_FRAMES)
        return 0;
    Xlog("dcounter: nothing fits (%d candidates%s)%s", n,
         wait ? ", path gate unanswered" : "", why);
    DcPlainHit(slot, "no counter fits");
    return 1;
}

// The "Kontra" prompt used to show over an attacker the press could not counter: the prompt is shown only while the
// press would actually start a counter - the same refusals dc_swing_start applies (busy / cooldown / attacker gone, down or in a paired move / out of reach / off level / not facing).
// t2r-dc-prompt-old.txt = shown for the whole window. Returns NULL when a counter can start, else the reason.
static const char *DcPromptBlock(uint32_t slot)
{
    if (GetFileAttributesA("t2r-dc-prompt-old.txt") !=
        INVALID_FILE_ATTRIBUTES)
        return NULL;
    uint64_t now = GetTickCount64();
    float vb[16], mb[16];
    const float *vl = NULL, *me = PlayerLocator(mb);
    uint8_t *peer = slot < T2R_MAX_PEERS ? MdgPeer(slot) : NULL;
    uint64_t h = peer ? *(uint64_t *) (peer + 0x60) : 0;
    if (h)
        vl = PuppetLocator(h, vb);
    if (MatchSubstate() != 6 || g_ps.role || g_downed || g_downDying
        || g_stun_until || LocalHp() <= 0.0f || MdgBusy())
        return "we are busy / downed / in a paired move / in the dodge";
    if (now < g_dcm.cool_until)
        return "cooldown";
    if (slot >= T2R_MAX_PEERS || IsTeammate(slot) || g_peerDowned[slot]
        || g_puppetDeadHere[slot] || !h)
        return "the attacker is gone / down";
    if (PsyncBusySlot(slot) || GxBusySlot(slot))
        return "the attacker is in another paired move";
    if (!vl || !me || !PsLocOk(vl) || !PsLocOk(me))
        return "no locators";
    if (CdCurStyle() == -2)
        return "held weapon unknown / upgraded";
    float dx = vl[0] - me[0], dz = vl[2] - me[2], d =
        __builtin_sqrtf(dx * dx + dz * dz);
    float fx = 2.0f * (me[4] * me[6] + me[7] * me[5]), fz =
        1.0f - 2.0f * (me[4] * me[4] + me[5] * me[5]), fl =
        __builtin_sqrtf(fx * fx + fz * fz);
    if (d > DC_MAX_DIST || d < DC_MIN_DIST)
        return "out of reach";
    if (__builtin_fabsf(vl[1] - me[1]) > 1.3f)
        return "not on our level";
    if (!MduEngaged()
        && (!(fl > 1e-3f) || (fx * dx + fz * dz) / (fl * d) < 0.0f))
        return "we do not face the attacker";
    return NULL;
}

// feature_early_finisher: the dodger's melee press (a new swing, not a dodge move). 1 = taken over (the swing is cancelled, the counter / its plain hit follows)
static int DcSwingStart(void)
{
    if (DcOff() || !g_dcm.arm_until)
        return 0;
    uint64_t now = GetTickCount64();
    if (now > g_dcm.arm_until) {
        g_dcm.arm_until = 0;
        return 0;
    }
    uint32_t slot = g_dcm.arm_slot;
    g_dcm.arm_until = 0;        // one try per dodge
    g_dcm.n_try++;
    const char *no = NULL;
    float vb[16], mb[16];
    const float *vl = NULL, *me = PlayerLocator(mb);
    uint8_t *peer = slot < T2R_MAX_PEERS ? MdgPeer(slot) : NULL;
    uint64_t h = peer ? *(uint64_t *) (peer + 0x60) : 0;
    if (h)
        vl = PuppetLocator(h, vb);
    if (MatchSubstate() != 6 || g_ps.role || g_downed || g_downDying
        || g_stun_until || LocalHp() <= 0.0f || MdgBusy())
        no = "we are busy / downed / in a paired move";
    else if (now < g_dcm.cool_until)
        no = "cooldown (no counter chains)";
    else if (slot >= T2R_MAX_PEERS || IsTeammate(slot)
             || g_peerDowned[slot] || g_puppetDeadHere[slot] || !h)
        no = "the dodged attacker is gone / down / a teammate";
    else if (PsyncBusySlot(slot) || GxBusySlot(slot))
        no = "the attacker is in another paired move";
    else if (!vl || !me || !PsLocOk(vl) || !PsLocOk(me))
        no = "no locators";
    else if (CdCurStyle() == -2)
        no = "held weapon unknown / upgraded (keeps its one-shot)";
    if (!no) {
        float dx = vl[0] - me[0], dz = vl[2] - me[2], d =
            __builtin_sqrtf(dx * dx + dz * dz);
        float fx = 2.0f * (me[4] * me[6] + me[7] * me[5]), fz =
            1.0f - 2.0f * (me[4] * me[4] + me[5] * me[5]), fl =
            __builtin_sqrtf(fx * fx + fz * fz);
        if (d > DC_MAX_DIST || d < DC_MIN_DIST)
            no = "out of reach";
        else if (__builtin_fabsf(vl[1] - me[1]) > 1.3f)
            no = "not on our level";
        else if (!MduEngaged()
                 && (!(fl > 1e-3f)
                     || (fx * dx + fz * dz) / (fl * d) < 0.0f))
            no = "we do not face the attacker (> 90 deg)";      // (a duel: the dodge turned us, the pair aligns both bodies anyway)   // (was > 70 deg; the pair aligns both bodies anyway)
    }
    if (no) {
        Xlog("dcounter: no counter for the press after the dodge: %s - the swing plays as usual", no);
        return 0;
    }
    // SP-exact: inside a duel the press after the dodge is SP's dodge attack as an ordinary connected duel hit
    // (melee_duel.c mdu_dc_route: swing, then the attacker -hit + the victim half on one frame at SP's switch, combo on at step 2) instead of the dc- pair
    if (slot < T2R_MAX_PEERS) {
        g_md56CtrMs[slot] = now;
        g_mdg.imm_until[slot] = 0;
        g_mdg49_cover[slot] = 0;
    }                           // ps56 (melee_dodge.c): the countered player's i-frames end, its dodge presses no longer make it immune
    if (MduDcRoute(slot, g_dcm.arm_dir, h))
        return 1;
    // the press is taken over now: the swing is cancelled this frame; the pair starts as soon as the path gate has its rays (1-3 frames), else the plain hit
    {
        uint64_t me0 = SID_PLAYER;
        DcCall(0x394888944e949d5fULL /* cancel-melee-action */ , 1, &me0);
    }
    g_kdFallbackFrame = g_t60;
    g_kdFallbackSlot = (int32_t) slot;
    DcDrawMelee();
    g_dcm.pend = 1;
    g_dcm.pend_slot = (int) slot;
    g_dcm.pend_dir = g_dcm.arm_dir;
    g_dcm.pend_f = g_frame;
    g_dcm.pend_w = g_dcm.draw_w ? g_dcm.draw_w : DcWeapon();
    if (DcResolve()) {
        g_dcm.pend = 0;
        g_dcm.pend_w = 0;
        g_dcm.draw_w = 0;
    }
    return 1;
}

// ---- the end of a counter (either role)
static void DcClearMissState(void)
{                               // the countered attacker returns to neutral with the dodger: no miss lock / stumble left over from the dodge
    g_mdg.st_on = 0;
    g_md_carry.on = 0;
    g_mdg.atk_lock_until = 0;
    g_mdg.miss_until = 0;
    g_mdg.tr_until = 0;
    g_mdg.slow_until = 0;
}

static float DcHitMsNow(void)
{
    return (g_dcm.on_att || g_dcm.on_vic) ? g_dcm.hit_s * 1000.0f : -1.0f;
}

static void DcFinish(const char *why)
{
    if (!g_dcm.on_att && !g_dcm.on_vic)
        return;
    if (g_dcm.on_vic && g_dcm.down_ext && (g_downed || !g_downAt))
        g_cdVicDown = 0;      // (dc_vic_collapse_down's no-fall flag ends with the down it was set for)
    uint64_t now = GetTickCount64();
    Xlog("dcounter: %s over (%s) after %llu ms: role %s, blow %s%s",
         CD_PAIRS[g_dcm.row].name, why,
         (unsigned long long) (now - g_dcm.t0_ms),
         g_dcm.on_att ? "dodger" : "countered",
         g_dcm.hit_sent ? "sent" : g_dcm.
         on_vic ? "(the dodger's)" : "NOT sent",
         g_dcm.break_req ? ", broken by a third party" : "");
    if (g_dcm.on_vic) {
        g_dcm.vic_imm_until = now + DC_VIC_IMM_MS;
        if (g_meleeLockVictim)
            g_meleeLockFrom = 0;
        DcClearMissState();
        // the opponent got out of the counter animation quicker than the player who threw it: the countered player was free
        // at the hand-off with every command. Now the haymaker staggers: DC_VIC_RECOVER_MS of melee_stagger.c's lock (camera + move only) after a landed,
        // non-downing counter, so the dodger's reward is a real opening. t2r-dc-no-recover.txt = old.
        if (!strcmp(why, "psync over") && !g_downed && !g_downAt
            && LocalHp() > 0.0f
            && GetFileAttributesA("t2r-dc-no-recover.txt") ==
            INVALID_FILE_ATTRIBUTES) {
            g_mhs.on = 1;
            g_mhs.react = 0;
            g_mhs.start = now;
            g_mhs.until = now + DC_VIC_RECOVER_MS;
            g_mhs.blows = 0;
            g_mhs.slot = (uint32_t) g_dcm.slot;
            g_mhs.last_blow = now;
            Xlog("dcounter: countered - staggered %d ms after the hand-off (camera + move only)", DC_VIC_RECOVER_MS);
        }
    }
    {
        static int nr = -1;
        static uint64_t nra;
        if (!MexOptout("t2r-no-mex-recover.txt", &nr, &nra))
            MexNotePairEnd(g_dcm.slot, DC_VIC_RECOVER_MS);
    }                           // (log 59840): no ENGAGE on the countered player while it staggers
    g_dcm.on_att = g_dcm.on_vic = 0;
    g_dcm.break_req = 0;
    g_dcCam = 0;               // a normal end: the camera's own fade ends at the hand-off
}

// recv_hit_hook: every damaging hit on us. 1 = drop it (a late blow of a counter we already escaped). The partner's counter blow at its moment is the exchange itself;
// anything else breaks the pair.
static int DcNoteHit(uint64_t w, float dmg)
{
    uint64_t now = GetTickCount64();
    if (g_dcm.escaped && g_dcm.esc_ms && now - g_dcm.esc_ms < 700
        && dmg > 0.0f && dmg < 100000.0f
        && (int) g_lastHitSlot == g_dcm.esc_slot && MhsIsBlow(w)) {
        Xlog("dcounter: the counter blow from slot %d (weapon %llx, dmg %.0f) arrived %llu ms after our escape - dropped", g_dcm.esc_slot, (unsigned long long) w, dmg, (unsigned long long) (now - g_dcm.esc_ms));
        return 1;
    }
    if (!(g_dcm.on_att || g_dcm.on_vic) || !(dmg > 0.0f)
        || dmg >= 100000.0f)
        return 0;
    int partner = g_dcm.slot;
    if (g_dcm.on_vic && !g_dcm.hit_sent && g_dcm.hit_exp_ms
        && now + 300 >= g_dcm.hit_exp_ms && now <= g_dcm.hit_exp_ms + 500
        && (int) g_lastHitSlot == partner && MhsIsBlow(w)) {
        g_dcm.hit_sent = 1;     // the counter's own blow landing on us
        g_mws_zone = MWS_FACE;
        MeleeHitFx(SID_PLAYER, w);
        g_mws_zone = MWS_GUT;   // (v5-sfx: SP's dodge-counter blow = -hit-face-skin)
        MhfxCounter((uint32_t) partner, w);
        Xlog("dcounter: the counter blow from slot %d lands on us (weapon %llx, dmg %.0f), %lld ms from the expected moment", partner, (unsigned long long) w, dmg, (long long) (now - g_dcm.hit_exp_ms));
        return 0;
    }
    // an opponent kept attacking through counters ([72232] / [75125] its knife blow, dmg 20, landed 40 ms into our counter):
    // the countered player's swing was already in flight on his screen when our START reached him. While we counter him, his melee blows on us are
    // dropped - the counter owns both bodies. t2r-dc-partner-hits.txt = old (taken).
    if ((int) g_lastHitSlot == partner && g_dcm.on_att && MhsIsBlow(w)
        && GetFileAttributesA("t2r-dc-partner-hits.txt") ==
        INVALID_FILE_ATTRIBUTES) {
        Xlog("dcounter: blow from slot %d (weapon %llx, dmg %.0f) during our counter on it - dropped", partner, (unsigned long long) w, dmg);
        return 1;
    }
    if ((int) g_lastHitSlot == partner && g_dcm.on_att)
        return 0;
    // Counter attacks were cut at 90% of the swing (A [59662]/[59687]): the counter's blow reached us twice - the dodger's hit
    // message at the authored moment (taken above as the counter blow) and, 0.45 s later, its engine's own contact of the counter clip as a plain melee hit.
    // The second one broke the pair as "a third party" 0.33 s before the hand-off. A partner blow on the countered side after the counter blow landed is that
    // same blow: dropped, never a break. t2r-dc-partner-rehit-old.txt = old.
    if ((int) g_lastHitSlot == partner && g_dcm.on_vic && MhsIsBlow(w)
        && GetFileAttributesA("t2r-dc-partner-rehit-old.txt") ==
        INVALID_FILE_ATTRIBUTES) {
        Xlog("dcounter: a second blow from slot %d (weapon %llx, dmg %.0f) during its counter on us - its engine contact of the same counter blow: dropped, the pair plays to its hand-off", partner, (unsigned long long) w, dmg);
        return 1;
    }
    g_dcm.break_req = 1;
    snprintf(g_dcm.break_why, sizeof g_dcm.break_why,
             "hit %.0f (weapon %llx) from slot %u", dmg,
             (unsigned long long) w, g_lastHitSlot);
    return 0;
}

// recv_hit_hook: the counter's would-kill blow on us - the down is scheduled for the collapse hand-off (takedown-style: survive at 1 hp, no fall, the half
// blends into the lying pose), not set at the blow
static int DcVicCollapseDown(void)
{
    if (!g_dcm.on_vic
        || !(g_ps.role == 2 && g_ps.kind == PS_CTXDOWN
             && g_ps.uid == g_dcm.uid) || !DcCollapses(g_ps.move))
        return 0;
    if (GetFileAttributesA("t2r-dc-no-collapse.txt") !=
        INVALID_FILE_ATTRIBUTES)
        return 0;
    double t = (PsyncMs() - g_ps.t0 + g_ps.chain_shift) / 1000.0;
    float k = g_dcm.hit_s + DC_COLLAPSE_S;
    if (k > g_ps.kill_s) {
        DcCamExtend(k - g_ps.kill_s);
        g_ps.kill_s = k;
    }
    g_dcm.down_ext = 1;
    uint64_t left =
        (double) g_ps.kill_s - t >
        0.1 ? S60((float) ((double) g_ps.kill_s - t)) : 6;
    uint64_t a[2] = { SID_PLAYER, 1 };
    DcCall(FNV64("character-set-invincibility-cannot-die-threshold"), 2,
            a);
    g_downAt = g_t60 + left - 2;
    g_finGuardUntil = g_t60 + left + S60(1.0f);
    g_cdVicDown = 1;
    Xlog("dcounter: the blow would kill us - 1 hp, the pair plays on to the haymaker's collapse, down in %.2f s at the hand-off (%.2f s, no fall)", (double) left / 60.0, (double) g_ps.kill_s);
    return 1;
}

// ([217379]: a duel fist blow "hit received ... dmg 250.0" + "+50 Execution" on a STANDING victim in the START frame of a duel pair): the Melee Down rule had
// "psync_role() != 2" (an execution / shiv on us must kill), so a lethal melee blow that reaches the victim of a duel / takedown HIT pair (kind PS_CTXDOWN) fell through to the plain
// hit and killed it. Such a blow is a DOWN like any other would-kill melee hit: 1 hp now, the pair plays on, the down at its hand-off (with the normal fall). A pair that already
// scheduled its own down (a finisher / takedown: g_down_at) is not touched. Opt-out t2r-pair-hit-down-old.txt. Log "pair hit down".
static int PairHitDownOn(void)
{
    static int off = -1;
    static uint64_t at;
    uint64_t n = GetTickCount64();
    if (off < 0 || n - at > 2000) {
        at = n;
        off =
            GetFileAttributesA("t2r-pair-hit-down-old.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    return !off;
}

static int PairHitDownOk(void)
{
    return g_ps.role == 2 && g_ps.kind == PS_CTXDOWN && !g_downAt
        && PairHitDownOn();
}

static void PairHitDownSched(float dmg, float hp, uint64_t w)
{
    double t = (PsyncMs() - g_ps.t0 + g_ps.chain_shift) / 1000.0;
    uint64_t left =
        (double) g_ps.kill_s - t >
        0.1 ? S60((float) ((double) g_ps.kill_s - t)) : 6;
    uint64_t a[2] = { SID_PLAYER, 1 };
    DcCall(FNV64("character-set-invincibility-cannot-die-threshold"), 2,
            a);
    g_downAt = g_t60 + left - 2;
    g_finGuardUntil = g_t60 + left + S60(1.0f);
    Xlog("pair hit down: lethal melee blow %.0f (weapon %llx, hp %.1f) on the victim of a pair (%s) - 1 hp, the pair plays on, down in %.2f s at its hand-off (not a death)", (double) dmg, (unsigned long long) w, (double) hp, PsyncName(g_ps.vic_sid), (double) left / 60.0);
}

static int DcOpenGround(void)
{
    return (g_dcm.on_att || g_dcm.on_vic) && CD_PAIRS[g_dcm.row].kind == 3;
}

// ---- the countered attacker's ESCAPE: a dodge press in the last DC_ESC_WIN_MS before the authored hit (clip time), not on dodge cooldown
static int DcEscMs(void)
{
    static int v = -1;
    static uint64_t at;
    uint64_t now = GetTickCount64();
    if (v < 0 || now - at >= 2000) {
        at = now ? now : 1;
        v = DC_ESC_WIN_MS;
        FILE *f = ExeSideOpen("t2r-dodge-counter-escape-ms.txt", "r");
        if (f) {
            int x = 0;
            if (fscanf(f, "%d", &x) == 1 && x >= 0 && x <= 1500)
                v = x;
            fclose(f);
        }
    }
    return v;
}

// the victim sometimes moved away from a counter that had started: 8 of 12 counters escaped in one match - a
// dodge spammer always hits the 240 ms window. A dodge press during the counter BEFORE the window burns the escape for that counter (a read, not a mash).
// t2r-dc-esc-spam-ok.txt = old (no burn).
static uint32_t g_dcEscBurnt; // the counter uid whose escape was burnt by an early press
static int DcEscBurnt(void)
{
    return g_dcm.on_vic && g_dcEscBurnt == g_dcm.uid
        && GetFileAttributesA("t2r-dc-esc-spam-ok.txt") ==
        INVALID_FILE_ATTRIBUTES;
}

static void DcEscEarlyPress(void)
{
    if (!g_dcm.on_vic || g_dcm.hit_sent || g_dcEscBurnt == g_dcm.uid)
        return;
    g_dcEscBurnt = g_dcm.uid;
    double t = (PsyncMs() - g_ps.t0 + g_ps.chain_shift) / 1000.0;
    Xlog("dcounter: dodge pressed at %.2f s of the counter, before the escape window (opens at %.2f s) - escape burnt for this counter", t, (double) g_dcm.hit_s - DcEscMs() / 1000.0);
}

static int DcEscOpen(uint64_t now)
{
    if (!g_dcm.on_vic || DcEscBurnt() || g_dcm.hit_sent
        || g_dcm.break_req || DcEscMs() <= 0
        || now < g_mdg.cooldown_until)
        return 0;
    if (!
        (g_ps.role == 2 && g_ps.kind == PS_CTXDOWN
         && g_ps.uid == g_dcm.uid) || g_downed || g_downDying
        || LocalHp() <= 0.0f)
        return 0;
    double t = (PsyncMs() - g_ps.t0 + g_ps.chain_shift) / 1000.0;
    return t >= (double) g_dcm.hit_s - DcEscMs() / 1000.0
        && t <= (double) g_dcm.hit_s + DC_ESC_LATE_MS / 1000.0;
}

static int DcEscSlot(void)
{
    return g_dcm.on_vic ? g_dcm.slot : -1;
}

// mdg_tick: the press edge inside the window. The pair ends for both (ABORT with PS_AB_ESCAPED), then the normal dodge starts (i-frames, press message: the dodger's contact with us
// is a miss, the 2.5 s dodge cooldown). No counter is armed for the escapee (no counter-counter); it is the dodger's 4 s cooldown and our 3 s immunity from here.
static void DcEscape(uint64_t now)
{
    int slot = g_dcm.slot;
    double t = (PsyncMs() - g_ps.t0 + g_ps.chain_shift) / 1000.0;
    Xlog("dcounter: ESCAPE - dodge pressed at %.2f s of the counter (hit due at %.2f s, window %d ms): the pair ends, the blow whiffs", t, g_dcm.hit_s, DcEscMs());
    g_dcm.escaped = 1;
    g_dcm.esc_ms = now;
    g_dcm.esc_slot = slot;
    g_dcm.n_escaped++;
    PsyncSend(PS_OP_ABORT, PS_AB_ESCAPED, 0xff);
    DcCamAbort("dodge counter aborted");
    PsyncAbort("escaped the dodge counter", 0);
    DcFinish("escaped");
    MdgStart((uint32_t) slot, now);
}

static int DcAborted(int role, uint32_t from)
{
    if (!(g_dcm.on_att || g_dcm.on_vic))
        return 0;
    Xlog("dcounter: aborted by slot %u (role %d, reason %d%s)", from, role,
         g_psAbortReason,
         g_psAbortReason ==
         PS_AB_ESCAPED ?
         ": it escape-dodged, the blow whiffs - no stagger, no lock, both back to neutral"
         : "");
    if (g_psAbortReason == PS_AB_ESCAPED)
        g_dcm.hit_sent = 1;
    if (role == 1 && g_dcm.on_att && !g_dcm.hit_sent
        && (g_psAbortReason == PS_AB_REFUSED
            || g_psAbortReason == PS_AB_NOACK
            || g_psAbortReason == PS_AB_NOPLAY)) {
        g_dcm.n_refused++;
        DcPlainHit((int) from,
                     "the countered attacker refused / never answered");
        g_dcm.hit_sent = 1;
    }
    DcFinish("aborted");
    return 1;
}

static int DcVictimBegin(uint32_t slot, float end_s, uint64_t skipf,
                           int pr)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    uint64_t now = GetTickCount64();
    const char *no = NULL;
    if (DcOff())
        no = "switched off";
    else if (slot > T2R_MAX_PEERS - 1 || MatchSubstate() != 6
             || !__builtin_isfinite(end_s) || end_s < 0.5f
             || end_s > 14.0f)
        no = "bad start";
    else if (g_downed || g_downDying || g_execUntil || g_uc4DyingUntil
             || g_downAt || LocalHp() <= 0.0f)
        no = "we are down / dying / in an execution";
    else if (now < g_dcm.vic_imm_until)
        no = "countered less than 3 s ago (no chain-locking)";
    else if (g_dcm.on_att || g_dcm.on_vic)
        no = "already in one";
    if (no) {
        Xlog("dcounter: on us by slot %u (%s) refused: %s - the dodger plays the plain hit", slot, CD_PAIRS[pr].name, no);
        g_dcm.n_refused++;
        return 0;
    }
    uint8_t *peer = ((fn_peer) T2rPeer) (slot);
    uint64_t ah = peer ? *(uint64_t *) (peer + 0x60) : 0;
    uint64_t left = S60(end_s) > skipf + 3 ? S60(end_s) - skipf : 3;
    MeleeLockStart(1);
    Uc4OvOffExec();
    if (g_uc4_me.cur)
        Uc4Stop(&g_uc4_me);
    if (g_uc4_pup[slot].cur)
        Uc4Stop(&g_uc4_pup[slot]);
    FinposBegin(2, (int) slot, 0, left + 60, "dodge counter on us");
    MduNoteDc(2);
    if (ah)
        XfaceStart(ah, SID_PLAYER, end_s + 0.6f);
    DcCam(ah, (float) left / 60.0f);
    if (!g_dcCam && ah && !strncmp(CD_PAIRS[pr].name, "pe-", 3))
        DcPescCam(ah, (float) left / 60.0f, CD_PAIRS[pr].name);       // v5-cam6: the pinner's escape camera
    DcClearMissState();      // the miss stumble / lock of the dodged swing ends here: both bodies leave the exchange together
    g_dcm.on_vic = 1;
    g_dcm.down_ext = 0;
    g_dcm.on_att = 0;
    g_dcm.uid = g_ps.uid;
    g_dcm.row = pr;
    g_dcm.slot = (int) slot;
    g_dcm.hit_s = DcHitRow(pr);
    g_dcm.end_s = end_s;
    g_dcm.hit_sent = 0;
    g_dcm.break_req = 0;
    g_dcm.t0_ms = now;
    g_dcm.hit_exp_ms = 0;
    g_dcm.exp_set = 0;          // set at the first tick from the shared move clock
    Xlog("dcounter: on us by slot %u: %s, the blow is due at %.2f s of the clip, escape window %d ms before it, hand-off at %.2f s, nothing else is changed (no down, no extra stagger, dodge cooldown untouched)", slot, CD_PAIRS[pr].name, g_dcm.hit_s, DcEscMs(), end_s);
    return 1;
}

static void DcTick(void)
{
    // a Counter / Kontra prompt after a successful dodge: over the dodged attacker for the counter's press window, with the melee button's glyph
    if (g_dcm.arm_until && !g_dcm.on_att && !g_dcm.on_vic
        && g_dcm.arm_slot < T2R_MAX_PEERS) {
        uint64_t now = GetTickCount64();
        if (now <= g_dcm.arm_until && MatchSubstate() == 6 && !g_downed) {
            uint64_t h = PuppetHandle(g_dcm.arm_slot);
            const char *pb = DcPromptBlock(g_dcm.arm_slot);
            if (pb) {
                static uint64_t lb;
                if (g_frame - lb > 600) {
                    lb = g_frame;
                    Xlog("dcounter: Counter prompt withheld - no counter could start now: %s", pb);
                }
                h = 0;
            }
            g_wpqGlyph = "gen:attack"; // ("gen:melee" is no command - a ? glyph; gen:attack = the melee button, device aware)
            int shown = h ? WorldPrompt(h, "Counter", -1.0f) : 0;
            g_wpqGlyph = NULL;
            static uint64_t lf;
            if (g_frame - lf > 600) {
                lf = g_frame;
                Xlog("dcounter: Counter prompt over slot %u (%u ms left) -> shown %d", g_dcm.arm_slot, (unsigned) (g_dcm.arm_until - now), shown);
            }
        }
    }
    if (g_dcm.pend) {
        if (MatchSubstate() != 6)
            g_dcm.pend = 0;
        else if (DcResolve())
            g_dcm.pend = 0;
        if (!g_dcm.pend) {
            g_dcm.pend_w = 0;
            g_dcm.draw_w = 0;
        }
    }
    if (!g_dcm.on_att && !g_dcm.on_vic)
        return;
    if (!(g_ps.role && g_ps.kind == PS_CTXDOWN && g_ps.uid == g_dcm.uid)) {
        DcFinish("psync over");
        return;
    }
    double t = (PsyncMs() - g_ps.t0 + g_ps.chain_shift) / 1000.0;      // clip seconds (absolute: a start into the clip counts from its first frame)
    if (g_dcm.on_vic && !g_dcm.exp_set) {
        g_dcm.exp_set = 1;
        double lead = (double) g_dcm.hit_s - t;
        if (lead < 0.0)
            lead = 0.0;
        g_dcm.hit_exp_ms = GetTickCount64() + (uint64_t) (lead * 1000.0);
        Xlog("dcounter: clip time %.2f s at our first frame, the blow is due in %.0f ms", t, lead * 1000.0);
        // a slight camera shake on the victim side of the counter: SP's melee hit kick (camera-shake-melee shape) on our
        // melee camera at the blow, x1.6 (a heavy counter blow; SP's plain hit = 1.0); a concussion effect joins later (SP capture pending)
        if (g_dcCam) {
            float tt =
                (float) (g_t60 - g_xcam_t0) / 60.0f + g_xcamTrkOff +
                (float) lead;
            XcamKickAdd(tt, 1.6f);
            Xlog("dcounter: camera kick x1.6 armed at %.2f s on the camera clock", tt);
        }
    }
    // SP capture of the palm bash (switchblade-stun-breaker-bash = ellie-knife-stun-handle-bash-2, 0.37 s to contact): the clip's voice-effect
    // event is Ellie's short effort grunt (vox-eff-gen-fight-give-lrg-short) 0.17 s in, under the switchblade whoosh; MP's pair plays no clip voice, so it is
    // said here on every screen for the attacker body - in the attacker's OWN voice (not Ellie / Abby vox): -lrg-short has
    // takes only for Ellie / Abby, so the closest effort every WLF / Seraphite voice has (give-med, mlt* / scr* banks; female looks via fem_effort_sid).
    // t2r-no-dodge-vox.txt = off (with the dodge grunts).
    {
        static uint32_t said_uid;
        static int nl;
        if (said_uid != g_dcm.uid && t >= 0.17 && g_dcm.row >= 0
            && strstr(CD_PAIRS[g_dcm.row].name, "-palm")) {
            said_uid = g_dcm.uid;
            uint64_t ae = g_dcm.on_att ? SID_PLAYER : (g_dcm.slot >= 0
                                                       && g_dcm.slot <
                                                       T2R_MAX_PEERS ?
                                                       PuppetHandle((uint32_t) g_dcm.slot) : 0);
            if (ae
                && GetFileAttributesA("t2r-no-dodge-vox.txt") ==
                INVALID_FILE_ATTRIBUTES) {
                DmSay(ae, "vox-eff-gen-fight-give-med", VXE_GIVE);
                if (LOGCAP(nl, 40))
                    Xlog("dcounter: palm bash effort grunt on the %s at %.2f s", g_dcm.on_att ? "our body" : "attacker puppet", t);
            }
        }
    }
    if (g_dcm.break_req) {
        Xlog("dcounter: BROKEN by a third party / the world (%s) at %.2f s - both halves blend out now", g_dcm.break_why, t);
        g_dcm.n_broken++;
        DcCamAbort("dodge counter aborted");
        PsyncAbort("dodge counter broken by a hit", 1);
        DcFinish("broken");
        return;
    }
    // a counter blow that downs the victim (Melee Down) left it standing in the
    // stagger until the hand-off and then morphed into downed (log 21073 down at the blow, 21107 hand-off). The haymaker's main clip is TLOU2's kill: its victim collapses by ~1.0 s (decoded
    // poses: pelvis 0.29 m at 1.0 s), so the pair plays on to that collapse and the down takes over from the floor. Both screens: the victim knows its own down,
    // the attacker / third party learn it from the down message.
    if (!g_dcm.down_ext && DcCollapses(g_ps.move)) {
        int vs = g_dcm.on_vic ? -1 : g_dcm.slot;
        int vdown = g_dcm.on_vic ? (g_downed || g_downAt) : (vs >= 0
                                                              && vs <
                                                              T2R_MAX_PEERS
                                                              &&
                                                              g_peerDowned
                                                              [vs]);
        if (vdown
            && GetFileAttributesA("t2r-dc-no-collapse.txt") ==
            INVALID_FILE_ATTRIBUTES) {
            float k = g_dcm.hit_s + DC_COLLAPSE_S;
            g_dcm.down_ext = 1;
            if (k > g_ps.kill_s) {
                Xlog("dcounter: the blow downed the victim - the pair plays on to the haymaker's collapse: hand-off %.2f -> %.2f s", (double) g_ps.kill_s, (double) k);
                DcCamExtend(k - g_ps.kill_s);
                g_ps.kill_s = k;
            }
        }
    }
    if (g_dcm.on_att && !g_dcm.hit_sent && t >= g_dcm.hit_s) {
        g_dcm.hit_sent = 1;
        int slot = g_dcm.slot;
        if (slot >= 0 && slot < T2R_MAX_PEERS && !g_peerDowned[slot]
            && !g_puppetDeadHere[slot]) {
            // our own type-3 hit, lethal 0 (a counter killed instead of downing a low-hp player): winmm's FUN_18002cd10 turns a hit its
            // hp prediction calls lethal into its 250 lethal marker (log 20474: "dmg 250", hp 20 -> dead standing). The knockdown path's plain hit instead: the
            // victim's recv_hit_hook cuts a would-kill blow to 1 hp and downs them (Melee Down). (> 25: not rescaled by the victim)
            uint8_t m[0x16];
            uint64_t ammo = 0;
            float dmg = DcDmg();
            memcpy(m, &g_dcm.w, 8);
            memcpy(m + 8, &ammo, 8);
            memcpy(m + 16, &dmg, 4);
            m[20] = 0;
            m[21] = 0;
            if (g_sendTramp)
                HrSendHit(m, sizeof m, (uint8_t) slot);
            uint64_t h = PuppetHandle((uint32_t) slot);
            // the victim's down is scheduled for ITS collapse hand-off, so on our screen the pair ended 0.4 s earlier (puppet
            // mid-fall, then its down loop popped in). A blow that leaves the victim at <= 1 hp downs it: our pair plays on to the same collapse.
            {
                float vhp = h ? PuppetHp((uint32_t) slot, h) : -1.0f;
                if (!g_dcm.down_ext && DcCollapses(g_ps.move)
                    && vhp > 0.0f && vhp - dmg <= 1.0f
                    && GetFileAttributesA("t2r-dc-no-collapse.txt") ==
                    INVALID_FILE_ATTRIBUTES) {
                    float k = g_dcm.hit_s + DC_COLLAPSE_S;
                    g_dcm.down_ext = 1;
                    if (k > g_ps.kill_s) {
                        Xlog("dcounter: our blow downs slot %d (hp %.0f) - the pair plays on to the haymaker's collapse: hand-off %.2f -> %.2f s", slot, (double) vhp, (double) g_ps.kill_s, (double) k);
                        DcCamExtend(k - g_ps.kill_s);
                        g_ps.kill_s = k;
                    }
                }
            }
            if (h) {
                g_mws_zone = MWS_FACE;
                MeleeHitFx(h, g_dcm.w);
                if (MeleeWeaponSfxWanted(g_dcm.w)) {
                    g_mws_zone = MWS_FACE;
                    MeleeWeaponHitSfx(h, g_dcm.w, 1);
                }
                g_mws_zone = MWS_GUT;
            }                   // (v5-sfx: SP -hit-face-skin; the 2nd call = an upgraded id melee_hit_fx does not know, throttled otherwise)
            Xlog("dcounter: the blow goes out at %.2f s (authored %.2f s): one normal melee hit (weapon %llx) to slot %d", t, g_dcm.hit_s, (unsigned long long) g_dcm.w, slot);
        }
    }
}
