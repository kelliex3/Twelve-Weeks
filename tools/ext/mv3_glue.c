// mv3_glue.c - melee v3 engine glue (spec re/melee-v3.md, contract mv3.h). Owns:
//  - the switch: t2r-melee-v3.txt next to the exe = v3 on (opt-in for the first cut). Off = melee v2 exactly as before.
//  - our per-frame input for the sim (mv3_glue_input): engine commands only (pad parity), F on the keyboard like melee_pace.c, stick for the dodge direction
//  - bubble decisions: propose with a standing enemy within 2.5 m when either of us presses melee / swings, end on downs / > 4 m for 1 s / a raised gun / match over
//  - damage: the sim's hit on OUR fighter, once its frame is confirmed (every input known: no rollback can take it back), goes through the real receive path
//    (recv_hook_body type 3, as if the attacker's winmm had sent it) - downs, kills, awards, the kill feed and hp sync stay engine-true and credit the attacker
//  - presentation dispatch (mv3_fx_*, mv3_anim_apply) and the stand-down gate of every older melee layer (mv3_stand*, called at their entry points)
// Only the VICTIM's machine applies a hit (both sims agree on it); the attacker's engine contact between bubble members is dropped (hitsend / directhit / park gates).

#define MV3G_PEND_N   24        // predicted hits on us waiting for confirmation (a rollback window holds at most a few)
#define MV3G_DONE_N   64        // applied hits (rollback dedupe)
#define MV3G_PROPOSE_R  2.5f    // m: a press / swing with a standing enemy this close starts a bubble
#define MV3G_KEEP_R     4.0f    // m: the bubble holds while the nearest standing enemy member is this close ...
#define MV3G_FAR_MS     1000    // ... and ends after this long beyond it
#define MV3G_GUN_MS     500     // a gun raised this long ends it
#define MV3G_SWING_MS   450     // an enemy's native swing announce (MSG_DODGE_SWING) this recent counts as "is swinging"

static struct {
    Mv3In in;
    uint64_t in_frame;
    uint8_t prev_btn;           // this game frame's input (read once), last frame's buttons (press edges)
    int active;
    uint64_t start_ms;
    uint32_t start_wf;          // bubble seen active last frame
    uint32_t mem[MV3_MAX_F];
    int nmem;                   // its member slots last frame (anim release on leave)
    uint64_t far_since, gun_since, prop_ms[T2R_MAX_PEERS];
    struct {
        uint32_t frame;
        uint8_t a;
        int16_t move, x;
    } pend[MV3G_PEND_N];
    int npend;
    struct {
        uint32_t frame;
        uint8_t a;
        int16_t move;
    } done[MV3G_DONE_N];
    int ndone;
    int hits_in, dropped, down_logged;
    uint32_t down_wf;
    uint64_t out_since, applied_frame;
    int16_t hp_sent;            // sim OUT with the engine body standing (since), mv3_anim_apply done this frame, our hp last reported
    const char *stood[48];
    int nstood;
} g_mv3g;

// ---- switch (cached like mv2_on: exe_side_open is cheap, re-polled every 2 s)
static int Mv3On(void)
{
    static int on = -1;
    static uint64_t at;
    uint64_t now = GetTickCount64();
    if (on < 0 || now - at >= 2000) {
        at = now ? now : 1;
        FILE *f = ExeSideOpen("t2r-melee-v3.txt", "r");
        int o = f != NULL;
        if (f)
            fclose(f);          // OFF again by default (v3 has no lock-on / sequences yet); t2r-melee-v3.txt = on
        if (o != on)
            Xlog("melee v3: %s",
                 o ?
                 "ON (t2r-melee-v3.txt: melee bubbles - one deterministic sim per fight, inputs on the wire, rollback; old melee layers stand down for bubble members)"
                 : "off (melee v2; t2r-melee-v3.txt = on)");
        on = o;
    }
    return on;
}

static uint32_t Mv3Self(void)
{
    return *(uint32_t *) T2R_SYM(kMySlot);
}

// the slot (us or a puppet) is a fighter of the bubble running on this machine
static int Mv3Owns(uint32_t slot)
{
    if (slot >= T2R_MAX_PEERS || !Mv3On() || !Mv3NetActive()
        || Mv3NetLocalIdx() < 0)
        return 0;
    return Mv3NetIdxOfSlot(slot) >= 0;
}

// the gate every older layer asks at its entry: 1 = stand down (the slot is a bubble member). The first stand-down of each layer is logged.
static int Mv3Stand(const char *layer, uint32_t slot)
{
    if (!Mv3Owns(slot))
        return 0;
    for (int i = 0; i < g_mv3g.nstood; i++)
        if (g_mv3g.stood[i] == layer)
            return 1;
    if (g_mv3g.nstood < (int) (sizeof g_mv3g.stood / sizeof *g_mv3g.stood))
        g_mv3g.stood[g_mv3g.nstood++] = layer;
    Xlog("mv3: '%s' stands down for slot %u (melee bubble member; first time this session)", layer, slot);
    return 1;
}

static int Mv3StandMe(const char *layer)
{
    return Mv3Stand(layer, Mv3Self());
}

// a melee blow (not a gun / explosion / thrown item) between two bubble members: the sim decides it, the engine's contact is not damage
static int MdgIsThrown(uint64_t w);
static int Mv3StandBlow(const char *layer, uint32_t att, uint64_t w)
{
    if (att >= T2R_MAX_PEERS || !MhsIsBlow(w) || MdgIsThrown(w)
        || !Mv3Owns(Mv3Self()))
        return 0;
    return Mv3Stand(layer, att);
}

// ---- weapon set of a held item (SP's melee sets; the dodge-counter rows use the same split)
static uint8_t Mv3gWsetOf(uint64_t w)
{
    if (!w || w == ~0ULL || w == FNV64("abby-unarmed"))
        return MV3_WS_FIST;
    if (KnifeWeaponId(w))
        return MV3_WS_KNIFE;
    if (BaseWeaponIsGun(BaseWeapon(w)))
        return MV3_WS_FIST;     // a gun in hand: the bash is a fist-set swing
    int two = 0, c = MhfxClass(w, &two, NULL);
    if (c == MHFX_NONE)
        return MeleeClassOf(w, NULL) >= 0 ? MV3_WS_1HBLUNT : MV3_WS_FIST;
    return two ? MV3_WS_2H : c ==
        MHFX_BLADE ? MV3_WS_1HBLADE : MV3_WS_1HBLUNT;
}

// the attacker's weapon id for the hit we apply: what its puppet holds (winmm's held id, peer+0x91) when that matches the sim's set, else a stand-in of the set
static uint64_t Mv3gAttWeapon(uint32_t att, uint8_t ws)
{
    if (ws == MV3_WS_FIST)
        return FNV64("abby-unarmed");
    uint8_t *pe = MdgPeer(att);
    uint64_t w = 0;
    if (pe && !IsBadReadPtr(pe + 0x91, 8))
        memcpy(&w, pe + 0x91, 8);
    if (w && w != ~0ULL && Mv3gWsetOf(w) == ws)
        return w;
    switch (ws) {
    case MV3_WS_KNIFE:
        return FNV64("knife-survival-mel-player");
    case MV3_WS_1HBLADE:
        return FNV64("machete");
    case MV3_WS_2H:
        return FNV64("baseball-bat");
    default:
        return FNV64("lead-pipe");
    }
}

// the real button while padinject hides that command from the engine (padinj_block keeps ONE mask: only when it is this command's)
static int Mv3gBlockedRaw(const char *name)
{
    struct padinj_w *w = PadinjWatch(name);
    return w && w->mask && g_padi.blk_mask == w->mask
        && PadinjBlockedRaw();
}

static int Mv3gGunAim(void)
{
    uint64_t w = CurrentWeapon();
    return w && w != ~0ULL && BaseWeaponIsGun(BaseWeapon(w))
        && PadCmdOn("aim");
}

// ---- conventions (one for every mv3 module; mv3_sim.c header, mv3_anim.c mv3a_yaw_q):
//   position mm: px = engine x, pz = engine z (ground plane), py = engine y (up).
//   yaw 1/100 deg = uc4_yaw(locator) x 100 = atan2(forward x, forward z): 0 faces +z, +9000 faces +x. Quaternion back: (0, sin y/2, 0, cos y/2).
//   handedness: +x is the character's LEFT when it faces +z (uc4_yaw), so yaw + 90 is the body's LEFT and yaw - 90 its RIGHT. The sim uses the same:
//   dir 1 = left (yaw + 90), dir 2 = right, root_side + = right (mv3_moves.h), so clip side, root motion and stick agree with no conversion here.
//   stick: body frame of the sim like a pad, sy+ = forward, sx+ = the body's RIGHT (toward yaw - 90); -127..127.
// screen stick (x+ right, y+ away from the camera) -> the sim's body frame. Screen right is the camera's yaw - 90 (the +x = left handedness above).
// No render camera this frame: camera = body (TLOU2's melee camera sits behind the body).
static void Mv3gStickBody(float x, float y, int16_t body_yaw, int8_t *sx,
                            int8_t *sy)
{
    float m = __builtin_sqrtf(x * x + y * y);
    if (m < 0.3f) {
        *sx = 0;
        *sy = 0;
        return;
    }                           // (melee_dodge.c's dead zone: neutral = the sim's default dodge)
    if (m > 1.0f)
        m = 1.0f;
    float by = (float) body_yaw * 0.01f, cy = by, cp[3], cd[3];
    if (CamPosDir(cp, cd) && cd[0] * cd[0] + cd[2] * cd[2] > 1e-4f)
        cy = __builtin_atan2f(cd[0], cd[2]) * 57.29578f;
    float world = cy + __builtin_atan2f(-x, y) * 57.29578f;     // stick direction as a yaw (atan2(x, z) convention)
    float r = (world - by) * 0.0174533f;        // ... relative to the body (+ = toward its left)
    *sx = (int8_t) __builtin_lroundf(-__builtin_sinf(r) * m * 127.0f);  // sx+ = right
    *sy = (int8_t) __builtin_lroundf(__builtin_cosf(r) * m * 127.0f);
}

// ---- input (read once per game frame; the net layer may ask more than once)
static void Mv3gReadInput(void)
{
    if (g_mv3g.in_frame == g_frame)
        return;
    g_mv3g.in_frame = g_frame;
    g_mv3g.prev_btn = g_mv3g.in.btn;
    Mv3In in;
    memset(&in, 0, sizeof in);
    int focus = GameFocused();
    int atk = PadCmdOn("melee-attack") || Mv3gBlockedRaw("melee-attack") || (focus && (GetAsyncKeyState(0x46) & 0x8000));   // never melee-attack-shiv (E contextual)
    if (atk && InteractDown())
        atk = 0;
    int dodge = PadCmdOn("evade") || PadCmdOn("dodge") || PadCmdOn("evade-start") || Mv3gBlockedRaw("evade") || PadinjDodgeHeld();    // (ps53: the device bits too)
    in.btn = (uint8_t) ((atk ? MV3_B_ATK : 0) | (dodge ? MV3_B_DODGE : 0));
    // dodge direction: the move stick (melee_dodge.c mdg_stick_dir without its XInput fallback: engine pad first, WASD), screen frame x+ right / y+ away
    float x = 0.0f, y = 0.0f;
    if (focus) {
        x = (float) (((GetAsyncKeyState('D') & 0x8000) ? 1 : 0) -
                     ((GetAsyncKeyState('A') & 0x8000) ? 1 : 0));
        y = (float) (((GetAsyncKeyState('W') & 0x8000) ? 1 : 0) -
                     ((GetAsyncKeyState('S') & 0x8000) ? 1 : 0));
    }
    float ex, ey;
    if (x == 0.0f && y == 0.0f && focus && PadinjLeftStick(&ex, &ey)) {
        x = ex;
        y = ey;
    }
    if (x > 1.0f) {
        x = 1.0f;
    }
    if (x < -1.0f) {
        x = -1.0f;
    }
    if (y > 1.0f) {
        y = 1.0f;
    }
    if (y < -1.0f) {
        y = -1.0f;
    }
    in.wset = Mv3gWsetOf(DcWeapon());
    float mb[16];
    const float *ml = Uc4MyLoc(mb);
    if (ml && __builtin_isfinite(ml[0]) && __builtin_isfinite(ml[1])
        && __builtin_isfinite(ml[2])) {
        in.px = (int32_t) __builtin_lroundf(ml[0] * 1000.0f);
        in.pz = (int32_t) __builtin_lroundf(ml[2] * 1000.0f);
        in.py = (int32_t) __builtin_lroundf(ml[1] * 1000.0f);
        float fx = 2.0f * (ml[4] * ml[6] + ml[7] * ml[5]), fz =
            1.0f - 2.0f * (ml[4] * ml[4] + ml[5] * ml[5]);
        float yaw = (fx * fx + fz * fz > 1e-6f) ? __builtin_atan2f(fx, fz) * 5729.578f : 0.0f;  // = uc4_yaw(ml) x 100
        if (yaw > 18000.0f) {
            yaw = 18000.0f;
        }
        if (yaw < -18000.0f) {
            yaw = -18000.0f;
        }
        in.yaw = (int16_t) __builtin_lroundf(yaw);
    } else {
        in.px = g_mv3g.in.px;
        in.pz = g_mv3g.in.pz;
        in.py = g_mv3g.in.py;
        in.yaw = g_mv3g.in.yaw;
    }                           // no locator this frame: hold the last pose
    Mv3gStickBody(x, y, in.yaw, &in.sx, &in.sy);
    int can = MatchSubstate() == 6 && !g_downed && !g_downDying
        && !PsyncOwn() && !g_execUntil && !GxRunning() && !g_unarmExec
        && LocalHp() > 0.0f && !(g_stun_until && g_t60 < g_stun_until)
        && !FinisherGuardOn() && !Mv3gGunAim()
        && !Mv3AnimBodyBusy(MV3A_US);
    in.flags =
        (uint16_t) ((can ? 1 : 0) |
                    (BpOwnSpeed() >= OWN_SPRINT_MPS ? 2 : 0));
    g_mv3g.in = in;
}

static void Mv3GlueInput(Mv3In *out)
{
    Mv3gReadInput();
    if (out)
        *out = g_mv3g.in;
}

// ---- bubble decisions
static int Mv3gEnemyDist(uint32_t s, const float *ml, float *d)
{
    uint64_t h = PuppetHandle(s);
    float pb[16];
    const float *pl = h ? PuppetLocator(h, pb) : NULL;
    if (!ml || !pl || !__builtin_isfinite(pl[0])
        || __builtin_fabsf(pl[1] - ml[1]) > 1.5f)
        return 0;               // (another floor is no melee)
    float dx = pl[0] - ml[0], dz = pl[2] - ml[2];
    *d = __builtin_sqrtf(dx * dx + dz * dz);
    return 1;
}

static void Mv3gBubble(uint64_t now)
{
    uint32_t self = Mv3Self();
    float mb[16];
    const float *ml = Uc4MyLoc(mb);
    if (ml && !__builtin_isfinite(ml[0]))
        ml = NULL;
    int gun = Mv3gGunAim();
    if (!gun)
        g_mv3g.gun_since = 0;
    else if (!g_mv3g.gun_since)
        g_mv3g.gun_since = now;
    if (Mv3NetActive()) {
        const char *why = NULL;
        if (MatchSubstate() != 6)
            why = "the match is not running";
        else if (g_downed || g_downDying || LocalHp() <= 0.0f)
            why = "we are down / dead";
        else if (PsyncOwn() || g_execUntil || GxRunning())
            why = "we are in a paired move / execution";
        else if (g_mv3g.gun_since && now - g_mv3g.gun_since >= MV3G_GUN_MS)
            why = "our gun was raised for 0.5 s";
        else {
            const Mv3World *w = Mv3NetWorld();
            int up = 0;
            float nd = 1e9f;
            for (int i = 0; w && i < w->n && i < MV3_MAX_F; i++) {
                uint32_t s = w->f[i].slot;
                if (s == 0xff || s == self || s >= T2R_MAX_PEERS
                    || IsTeammate(s) || !MexPeerOk(s))
                    continue;   // (mex_peer_ok: alive, not downed, not dead here)
                up++;
                float d;
                if (Mv3gEnemyDist(s, ml, &d) && d < nd)
                    nd = d;
            }
            // the sim's OUT is final (mv3_sim.c: hp <= 0 -> DOWN + OUT). A fighter OUT in the sim whose engine body still stands = the sim's hp ran apart from the
            // engine's (hp is the owner's report, mv3_net_set_hp): the fight restarts as a fresh bubble with fresh hp instead of a frozen fighter
            int out_up = 0;
            for (int i = 0; w && i < w->n && i < MV3_MAX_F; i++) {
                uint32_t s = w->f[i].slot;
                if (s == 0xff || w->f[i].state != MV3_S_OUT)
                    continue;
                if (s ==
                    self ? !(g_downed || g_downDying
                             || LocalHp() <= 0.0f) : (s < T2R_MAX_PEERS
                                                       && MexPeerOk(s)))
                    out_up = 1;
            }
            if (!out_up)
                g_mv3g.out_since = 0;
            else if (!g_mv3g.out_since)
                g_mv3g.out_since = now;
            if (!up)
                why =
                    "no standing enemy left in the bubble (downed / dead / gone)";
            else if (g_mv3g.out_since && now - g_mv3g.out_since >= 500)
                why =
                    "a fighter is OUT in the sim but stands in the engine for 0.5 s (sim hp out of sync) - a fresh bubble re-reads hp";
            else if (nd > MV3G_KEEP_R) {
                if (!g_mv3g.far_since)
                    g_mv3g.far_since = now;
                else if (now - g_mv3g.far_since >= MV3G_FAR_MS)
                    why = "more than 4 m from every enemy for 1 s";
            } else
                g_mv3g.far_since = 0;
        }
        if (why) {
            Xlog("mv3: bubble end requested - %s", why);
            Mv3NetEnd(why);
            g_mv3g.far_since = 0;
            g_mv3g.out_since = 0;
            return;
        }
    } else
        g_mv3g.far_since = 0;
    // propose: a standing enemy within 2.5 m and either of us presses melee / swings
    if (!ml || !(g_mv3g.in.flags & 1))
        return;
    int edge = (g_mv3g.in.btn & MV3_B_ATK)
        && !(g_mv3g.prev_btn & MV3_B_ATK);
    const Mv3World *w = Mv3NetActive()? Mv3NetWorld() : NULL;
    for (uint32_t s = 0; s < T2R_MAX_PEERS; s++) {
        if (s == self || !MexPeerOk(s) || PsyncBusySlot(s)
            || GxBusySlot(s) || Mv3AnimBodyBusy(s))
            continue;           // (mv3_anim_body_busy: psync / downed / dead / held by another move)
        if (Mv3NetActive() && Mv3NetIdxOfSlot(s) >= 0)
            continue;           // already a member
        float d;
        if (!Mv3gEnemyDist(s, ml, &d) || d > MV3G_PROPOSE_R)
            continue;
        int swing = g_mdg.sw[s].ms && now - g_mdg.sw[s].ms <= MV3G_SWING_MS;    // its native swing announce (outside a bubble the engine still swings)
        for (int i = 0; w && i < w->n && i < MV3_MAX_F && !swing; i++)
            if (w->f[i].state == MV3_S_SWING && w->f[i].target != 0xff
                && w->f[i].target < w->n && w->f[w->f[i].target].slot == s)
                swing = 1;
        if (!edge && !swing)
            continue;
        if (g_mv3g.prop_ms[s] && now - g_mv3g.prop_ms[s] < 250)
            continue;           // (the net layer answers within a round trip: no per-frame spam)
        g_mv3g.prop_ms[s] = now;
        int r = Mv3NetPropose(s);
        static int np;
        if (LOGCAP(np, 120))
            Xlog("mv3: propose bubble with slot %u (%.2f m, %s) -> %s", s,
                 d, edge ? "our melee press" : "it swings",
                 r ? "started / joined" : "not (yet)");
    }
}

// ---- damage: predicted hits on us wait for confirmation, then go through the receive path once
static int Mv3gDone(uint32_t frame, uint8_t a, int16_t move)
{
    for (int i = 0; i < g_mv3g.ndone && i < MV3G_DONE_N; i++)
        if (g_mv3g.done[i].frame == frame && g_mv3g.done[i].a == a
            && g_mv3g.done[i].move == move)
            return 1;
    return 0;
}

static void Mv3gPendHit(const Mv3Ev *e)
{
    if (Mv3gDone(e->frame, e->a, e->move))
        return;
    for (int i = 0; i < g_mv3g.npend; i++)
        if (g_mv3g.pend[i].frame == e->frame && g_mv3g.pend[i].a == e->a
            && g_mv3g.pend[i].move == e->move) {
            g_mv3g.pend[i].x = e->x;
            return;
        }
    if (g_mv3g.npend >= MV3G_PEND_N) {
        static int nf;
        if (LOGCAP(nf, 20))
            Xlog("mv3: pending hit list full - hit of fighter %u at frame %u not queued", e->a, e->frame);
        return;
    }
    int k = g_mv3g.npend++;
    while (k > 0 && g_mv3g.pend[k - 1].frame > e->frame) {
        g_mv3g.pend[k] = g_mv3g.pend[k - 1];
        k--;
    }                           // kept in frame order
    g_mv3g.pend[k].frame = e->frame;
    g_mv3g.pend[k].a = e->a;
    g_mv3g.pend[k].move = e->move;
    g_mv3g.pend[k].x = e->x;
}

static void Mv3gApplyHit(uint32_t frame, uint8_t a, int16_t move,
                           int16_t x)
{
    int di = g_mv3g.ndone++ % MV3G_DONE_N;      // (ring: ndone counts every write, the lookup scans min(ndone, N))
    g_mv3g.done[di].frame = frame;
    g_mv3g.done[di].a = a;
    g_mv3g.done[di].move = move;
    int si = Mv3NetSlotOfIdx(a);
    uint32_t att = si < 0 ? 99u : (uint32_t) si, self = Mv3Self();
    float dmg = (float) x / 10.0f;
    const char *skip = NULL;
    if (att >= T2R_MAX_PEERS || att == self)
        skip = "the attacker fighter has no peer slot";
    else if (MatchSubstate() != 6)
        skip = "the match is not running";
    else if (g_downed || g_downDying || LocalHp() <= 0.0f)
        skip = "we are already down / dead";
    else if (!(dmg > 0.0f && dmg < 200.0f))
        skip = "damage out of range";
    if (skip) {
        static int ns;
        if (LOGCAP(ns, 40))
            Xlog("mv3: hit by fighter %u (slot %d) at frame %u, move %d, dmg %.1f NOT applied - %s", a, si, frame, move, dmg, skip);
        return;
    }
    const Mv3World *w = Mv3NetWorld();
    uint8_t ws = w && a < w->n ? w->f[a].wset : MV3_WS_FIST;
    uint64_t wid = Mv3gAttWeapon(att, ws);
    uint8_t hit[0x16] = { 0 };  // winmm's type-3 hit: [u64 weapon][u64 ammo][f32 dmg][u8 lethal][u8 head]
    memcpy(hit, &wid, 8);
    memcpy(hit + 0x10, &dmg, 4);
    float hp0 = LocalHp();
    g_mv3Applying = 1;
    RecvHookBody((uint64_t) att, 3, hit, sizeof hit); // the attacker's hit as its winmm would have sent it: friendly fire / trades / downs / credit all run
    g_mv3Applying = 0;
    g_mv3g.hits_in++;
    static int nl;
    if (LOGCAP(nl, 400))
        Xlog("mv3: hit applied - slot %u -> us, move %d (%s), weapon %llx, dmg %.1f, hp %.1f -> %.1f%s (frame %u, confirmed %u)", att, move, move >= 0 && move < MV3_MOVES_N && MV3_MOVES[move].name ? MV3_MOVES[move].name : "?", (unsigned long long) wid, dmg, hp0, LocalHp(), g_downed ? " DOWNED" : "", frame, Mv3NetConfirmed());
}

static void Mv3gApplyConfirmed(void)
{
    if (!g_mv3g.npend)
        return;
    uint32_t conf = Mv3NetConfirmed();
    int k = 0;
    for (int i = 0; i < g_mv3g.npend; i++) {
        if (g_mv3g.pend[i].frame <= conf) {
            if (!Mv3gDone
                (g_mv3g.pend[i].frame, g_mv3g.pend[i].a,
                 g_mv3g.pend[i].move))
                Mv3gApplyHit(g_mv3g.pend[i].frame, g_mv3g.pend[i].a,
                               g_mv3g.pend[i].move, g_mv3g.pend[i].x);
        } else
            g_mv3g.pend[k++] = g_mv3g.pend[i];
    }
    g_mv3g.npend = k;
}

// ---- callbacks from mv3_net.c
static void Mv3GlueEvents(const Mv3Evbuf *ev, int resim)
{
    if (!ev)
        return;
    int li = Mv3NetLocalIdx();
    for (int i = 0; i < ev->n && i < MV3_EV_MAX; i++) {
        const Mv3Ev *e = &ev->e[i];
        if (e->type == MV3_E_HIT && li >= 0 && e->b == (uint8_t) li)
            Mv3gPendHit(e);
        else if (e->type == MV3_E_DOWN
                 && (!g_mv3g.down_logged || e->frame > g_mv3g.down_wf)) {
            // the engine's own down follows the applied hit (recv_hit_hook: a would-kill blow downs); the end rules take the body out of the bubble
            g_mv3g.down_logged = 1;
            g_mv3g.down_wf = e->frame;
            int s = Mv3NetSlotOfIdx(e->a);
            Xlog("mv3: sim DOWN of fighter %u (slot %d%s) at frame %u%s - the engine's downed flow takes it (the bubble ends on the engine down)", e->a, s, li >= 0 && e->a == (uint8_t) li ? ", us" : "", e->frame, resim ? " (re-simulated)" : "");
        }
    }
    Mv3FxEvents(ev, resim);
}

static void Mv3GlueRolledBack(uint32_t to_frame)
{
    int k = 0;
    for (int i = 0; i < g_mv3g.npend; i++)
        if (g_mv3g.pend[i].frame <= to_frame)
            g_mv3g.pend[k++] = g_mv3g.pend[i];  // later ones come back from the re-simulation if they still happen
    if (k != g_mv3g.npend) {
        static int nr;
        if (LOGCAP(nr, 60))
            Xlog("mv3: rollback to frame %u - %d predicted hit(s) on us withdrawn (re-simulated)", to_frame, g_mv3g.npend - k);
    }
    g_mv3g.npend = k;
    if (g_mv3g.down_logged && g_mv3g.down_wf > to_frame)
        g_mv3g.down_logged = 0;
    Mv3FxRolledBack(to_frame);
}

static void Mv3GlueFrame(const Mv3World *w)
{
    if (!w)
        return;
    uint32_t now32 = (uint32_t) GetTickCount64(), f0 =
        Mv3NetFrameMs(w->frame), frac = now32 - f0;
    if ((int32_t) frac < 0)
        frac = 0;
    if (frac > 1000u / MV3_HZ)
        frac = 1000u / MV3_HZ;
    g_mv3g.applied_frame = g_frame;
    Mv3AnimApply(w, Mv3NetLocalIdx(), frac);
}

// ---- bubble start / end / member changes (logs, anim release, engine melee suppression)
static void Mv3gTrack(uint64_t now, int act)
{
    uint32_t cur[MV3_MAX_F];
    int nc = 0;
    const Mv3World *w = act ? Mv3NetWorld() : NULL;
    for (int i = 0; w && i < w->n && i < MV3_MAX_F; i++)
        if (w->f[i].slot != 0xff)
            cur[nc++] = w->f[i].slot;
    if (act && !g_mv3g.active) {
        char b[96];
        int o = 0;
        b[0] = 0;
        for (int i = 0; i < nc && o < (int) sizeof b - 8; i++)
            o += snprintf(b + o, SNLEFT(b, o), "%s%u", i ? "," : "",
                          cur[i]);
        g_mv3g.start_ms = now;
        g_mv3g.start_wf = w ? w->frame : 0;
        g_mv3g.hits_in = 0;
        g_mv3g.dropped = 0;
        g_mv3g.down_logged = 0;
        Xlog("mv3: bubble START at world frame %u - fighters [%s], we are fighter %d, our hp %.0f, weapon set %u", g_mv3g.start_wf, b, Mv3NetLocalIdx(), LocalHp(), g_mv3g.in.wset);
    }
    for (int i = 0; i < g_mv3g.nmem; i++) {
        int still = 0;
        for (int j = 0; j < nc; j++)
            if (cur[j] == g_mv3g.mem[i])
                still = 1;
        if (!still && act) {
            Mv3AnimRelease(g_mv3g.mem[i]);
            Xlog("mv3: slot %u left the bubble", g_mv3g.mem[i]);
        }
    }
    if (!act && g_mv3g.active) {
        g_mv3g.dropped += g_mv3g.npend;
        Xlog("mv3: bubble END after %.1f s - %d hit(s) on us applied, %d predicted hit(s) never confirmed (dropped), our hp %.0f%s", (now - g_mv3g.start_ms) / 1000.0, g_mv3g.hits_in, g_mv3g.dropped, LocalHp(), g_downed ? ", downed" : "");
        g_mv3g.npend = 0;
        g_mv3g.out_since = 0;
        g_mv3g.hp_sent = 0;
        Mv3AnimReleaseAll("bubble over");    // every body back to the engine (+ engine melee back on)
        Mv3AnimSuppressEngine(0);
    }
    memcpy(g_mv3g.mem, cur, sizeof cur);
    g_mv3g.nmem = nc;
    g_mv3g.active = act;
}

// ---- hp: the sim mirrors the engine's hp x10 (mv3_fighter.hp). Each owner reports ITS OWN engine hp (the victim's engine is the truth: it applies the damage);
// mv3_net.c carries it so every sim sets it at the same frame (session start / resync / when it changes). -1 = unknown.
static int16_t Mv3GlueHp10(uint32_t slot)
{
    float hp = slot == Mv3Self()? LocalHp() : slot < T2R_MAX_PEERS
        && PuppetHandle(slot) ? PuppetHp(slot,
                                           PuppetHandle(slot)) : -1.0f;
    if (!(hp >= 0.0f && hp < 3000.0f))
        return -1;
    return (int16_t) __builtin_lroundf(hp * 10.0f);
}

static void Mv3gHpReport(int act)
{
    int li = Mv3NetLocalIdx();
    if (!act || li < 0)
        return;
    int16_t hp = Mv3GlueHp10(Mv3Self());
    if (hp < 0 || hp == g_mv3g.hp_sent)
        return;
    const Mv3World *w = Mv3NetWorld();
    static int nl;
    if (LOGCAP(nl, 200))
        Xlog("mv3: our hp %.1f reported to the bubble (sim had %.1f)",
             hp / 10.0, w && li < w->n ? w->f[li].hp / 10.0 : -1.0);
    g_mv3g.hp_sent = hp;
    Mv3NetSetHp(li, hp);
}

// ---- per game frame (main tick, next to the other melee ticks)
static void Mv3Tick(void)
{
    uint64_t now = GetTickCount64();
    Mv3AnimTestTick();       // mv3_anim.c: the live clip check (t2r-mv3-animtest.txt), independent of the switch
    if (!Mv3On()) {
        if (Mv3NetActive())
            Mv3NetEnd("melee v3 switched off");
        if (g_mv3g.active)
            Mv3gTrack(now, 0);
        return;
    }
    Mv3gReadInput();
    Mv3gBubble(now);
    Mv3NetTick(now);          // steps the sim to the shared frame (rollback inside): calls mv3_glue_input / _events / _rolled_back / _frame
    Mv3gApplyConfirmed();
    int act = Mv3NetActive();
    Mv3AnimSuppressEngine(act);      // (edge-detected in mv3_anim.c; its leases are renewed by mv3_anim_apply)
    if (act && g_mv3g.applied_frame != g_frame)
        Mv3GlueFrame(Mv3NetWorld());        // mv3_net.c did not call _frame this game frame: the bodies still follow the sim
    Mv3gHpReport(act);
    if (act) {                  // the engine's own evade (dive / dodge-roll) off inside a bubble - read above first, as melee_dodge.c does (frame-scoped native)
        uint64_t cmd = FNV64("evade");
        DcCall(FNV64("joypad-disable-commands"), 1, &cmd);
    }
    Mv3FxTick(now);
    Mv3gTrack(now, act);
}
