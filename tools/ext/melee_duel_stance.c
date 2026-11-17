// melee_duel_stance.c - the DUEL FIGHTING STANCE between blows (v5-feel3). Included after melee_duel_sptl.c. Log prefix "mst:".
// Opt-out t2r-duel-no-stance.txt next to the exe (re-read every 2 s). Presentation only: no position, timing, damage or window changes.
//
// SP (re/melee-duel-sp-spec.md section 1 "Melee stance between moves"; review sheets sp_s3 / sp_s4): while engaged (cmel 1, no move, enemy within 3 m)
// both fighters stay crouched with the fists up and face each other within ~10 deg: locomotion switches to the melee-mode set (abby-mm-melee-idle-fist,
// *-mm-tense-idle*, *-mm-tense-run-strafe-*). Ours (A15 a15_s3 f528-575): after the -hit hand-off both stand upright, arms down, 1-1.5 s - the MP
// player's own locomotion idle; nothing drove the melee-mode set (VERIFIED: no stance code in melee_duel*.c / paired.c, the MP locomotion has no duel mode).
//
// What it does, fists only (our weapon abby-unarmed; the partner's hand weapon unarmed / none):
//  US       while the duel is engaged and our body is free (no swing / pair / dodge / hit taken in the last 600 ms, standing: < MST_SPEED m/s and the
//           left stick centred) for MST_SETTLE_MS, the first resident clip of MST_CLIPS plays on our body (animate_ on the move layer, MST_BLEND_IN),
//           looped by a crossfade restart at phase MST_LOOP_PH, and the body turns to face the partner at <= MST_TURN_DPS (SP: within ~10 deg).
//           Any move of ours (mdu_swing_note: on its start frame) cuts it over MST_BLEND_MOVE; walking / the stick over MST_BLEND_WALK.
//  PUPPET   the same clip on the partner's puppet while it stands (< MST_SPEED m/s on its displayed locator) and nothing else plays on it: no duel pair,
//           no dtl clip running or queued (swing replica / reaction / dodge), no forced reaction / dodge clip. dtl's next clip crossfades over it.
// Not done: SP's strafe / circle set while moving (the MP locomotion graph owns moving bodies) - the stance is the standing case only.
#define MST_SPEED 0.35f         // m/s: above = walking, the stance yields to locomotion
#define MST_SETTLE_MS 120       // free and standing this long before the stance starts
#define MST_BLEND_IN 0.30f
#define MST_BLEND_MOVE 0.08f    // our own move starts: out fast (the swing / dodge shows from its first frames)
#define MST_BLEND_WALK 0.20f
#define MST_LOOP_PH 0.80f
#define MST_TURN_DPS 220.0f
#define MST_HIT_HOLD_MS 600
#define MSTLOG(...) do { if (LOGCAP(g_mst.nlog, 600)) Xlog(__VA_ARGS__); } while (0)
static const char *const MST_CLIPS[] =
    { "abby-mm-melee-idle-fist", "ellie-mm-melee-idle",
"ellie-mm-tense-idle-new", "abby-mm-tense-idle", "ellie-mm-tense-crouch-idle^melee-idle" };
#define MST_NCLIPS (sizeof MST_CLIPS / sizeof MST_CLIPS[0])
static struct {
    int on, cfg_logged, nlog, picked;
    uint64_t cfg_at, sid;
    const char *name;
    float len;
    struct {
        int on;
        uint64_t ent, sid, t0, free_since, last_t;
        float p[3];
        int p_ok;
        float spd;
        uint32_t starts, loops;
    } b[2];                     // 0 = us, 1 = the partner puppet
    uint64_t hit_until;
    float hp_last;
    uint32_t slot;
} g_mst = {.picked = -1 };

static int MstOn(void)
{
    uint64_t now = GetTickCount64();
    if (!g_mst.cfg_at || now - g_mst.cfg_at >= 2000) {
        g_mst.cfg_at = now ? now : 1;
        int o = GetFileAttributesA("t2r-duel-stance.txt") != INVALID_FILE_ATTRIBUTES;   // the stance held players in place (keyboard moves never released it) - opt-in until fixed
        if (o != g_mst.on || !g_mst.cfg_logged)
            Xlog("mst: %s",
                 o ?
                 "ON - duel fighting stance between blows (SP melee-mode idle on both bodies while standing; t2r-duel-no-stance.txt = off)"
                 : "OFF (t2r-duel-no-stance.txt)");
        g_mst.on = o;
        g_mst.cfg_logged = 1;
    }
    return g_mst.on && MduOn();
}

// the first resident clip of the list (asked once per load: the paks of this map)
static int MstPick(void)
{
    if (g_mst.picked >= 0)
        return g_mst.sid != 0;
    char got[400];
    size_t o = 0;
    got[0] = 0;
    for (size_t i = 0; i < MST_NCLIPS; i++) {
        uint64_t sid = FNV64(MST_CLIPS[i]);
        float l = MdgClipLen(sid);
        o += (size_t) snprintf(got + o,
                               o < sizeof got ? sizeof got - o : 0,
                               "%s%s %.2f s", i ? ", " : "", MST_CLIPS[i],
                               (double) l);
        if (o >= sizeof got)
            o = sizeof got - 1;
        if (!g_mst.sid && l > 0.3f) {
            g_mst.sid = sid;
            g_mst.name = MST_CLIPS[i];
            g_mst.len = l;
        }
    }
    g_mst.picked = 1;
    Xlog("mst: stance clip %s (resident lengths: %s)",
         g_mst.name ? g_mst.name : "NONE resident - no stance", got);
    return g_mst.sid != 0;
}

static void MstStop(int k, float bl, const char *why)
{
    if (!g_mst.b[k].on)
        return;
    MhrStop(g_mst.b[k].ent, g_mst.b[k].sid, bl);
    MSTLOG
        ("mst: stance off on %s after %llu ms (%u loops), blend-out %.2f s - %s",
         k ? "the partner puppet" : "us",
         (unsigned long long) (GetTickCount64() - g_mst.b[k].t0),
         g_mst.b[k].loops, (double) bl, why);
    g_mst.b[k].on = 0;
    g_mst.b[k].free_since = 0;
}

// melee_duel.c mdu_swing_note: a melee move of ours starts this frame - the stance must not cover its first frames
static void MstNoteMove(int started)
{
    if (!started)
        return;
    if (g_mst.b[0].on)
        MstStop(0, MST_BLEND_MOVE, "our move starts");
    g_mst.b[0].free_since = 0;
}

// a puppet entry replaced by another clip on the layer (dtl / psync / reaction): no stop-animating of ours afterwards, the new clip took the layer
static void MstDrop(int k, const char *why)
{
    if (!g_mst.b[k].on)
        return;
    MSTLOG("mst: stance on %s handed to %s after %llu ms",
           k ? "the partner puppet" : "us", why,
           (unsigned long long) (GetTickCount64() - g_mst.b[k].t0));
    g_mst.b[k].on = 0;
    g_mst.b[k].free_since = 0;
}

static float MstSpeed(int k, const float *l, uint64_t now)
{
    typeof(&g_mst.b[0]) b = &g_mst.b[k];
    if (b->p_ok && now > b->last_t && now - b->last_t < 200) {
        float dt = (float) (now - b->last_t) / 1000.0f, dx =
            l[0] - b->p[0], dz = l[2] - b->p[2], v =
            __builtin_sqrtf(dx * dx + dz * dz) / dt;
        b->spd += (v - b->spd) * 0.35f;
    } else
        b->spd = 0.0f;
    b->p[0] = l[0];
    b->p[1] = l[1];
    b->p[2] = l[2];
    b->p_ok = 1;
    b->last_t = now;
    return b->spd;
}

static int MstUnarmed(uint64_t w)
{
    return !w || w == ~0ULL || w == FNV64("abby-unarmed")
        || w == FNV64("ellie-unarmed");
}

// why body k may not show the stance now (NULL = free)
static const char *MstBusy(int k, uint32_t s, uint64_t now, float spd)
{
    if (MatchSubstate() != 6)
        return "not in a match";
    if (k == 0) {
        if (DcWeapon() != FNV64("abby-unarmed"))
            return "not fists";
        if (g_downed || g_downDying || LocalHp() <= 0.0f)
            return "downed";
        if (g_ps.role)
            return "paired move";
        if (g_mdu.sw_on)
            return "swinging";
        uint64_t me = SID_PLAYER, r = DcCall(SID_IN_MELEE_ATTACK, 1, &me);
        if (r != ~0ULL && (r & 0xff))
            return "melee move";
        if (g_mdg.press_ms && now - g_mdg.press_ms < 1000)
            return "dodging";
        if (now < g_mst.hit_until)
            return "hit";
        float sx = 0.0f, sy = 0.0f;
        if (PadinjLeftStick(&sx, &sy) && sx * sx + sy * sy > 0.09f)
            return "moving (stick)";
        if (spd > MST_SPEED)
            return "moving";
        return NULL;
    }
    if (!MstUnarmed(CdDisPeerWeapon((int) s)))
        return "not fists";
    if (g_peerDowned[s] || g_puppetDeadHere[s])
        return "downed";
    if (PsyncBusySlot(s))
        return "paired move";
    if (g_dtl.run.src && g_dtl.run.slot == s && now < g_dtl.run.end)
        return "dtl clip";
    for (int i = 0; i < 6; i++)
        if (g_dtl.q[i].used && g_dtl.q[i].slot == s)
            return "dtl queue";
    if (g_mhr.pd[s].end > now || g_mdg.pd[s].end > now)
        return "reaction / dodge clip";
    if (g_mdu.vsw_at[s] && now - g_mdu.vsw_at[s] < 700)
        return "its swing";
    if (spd > MST_SPEED)
        return "moving";
    return NULL;
}

static void MstFace(const float *me, const float *them, uint64_t now,
                     float dt)
{
    float dx = them[0] - me[0], dz = them[2] - me[2];
    if (dx * dx + dz * dz < 0.04f)
        return;
    float want = __builtin_atan2f(dx, dz), cur =
        Uc4Yaw(me) / 57.29578f, dy =
        __builtin_remainderf(want - cur, 6.2831853f);
    if (__builtin_fabsf(dy) < 0.12f)
        return;                 // (~7 deg: SP's median facing error)
    float mx = MST_TURN_DPS / 57.29578f * dt;
    dy = dy > mx ? mx : dy < -mx ? -mx : dy;
    float yaw = cur + dy, l8[8] =
        { me[0], me[1], me[2], 1.0f, 0.0f, __builtin_sinf(yaw * 0.5f),
0.0f, __builtin_cosf(yaw * 0.5f) };
    static PsyncBf bf;
    PsyncBfFromLoc(&bf, l8);
    mo_write(SID_PLAYER, &bf, MO_SRC("duel.stance_face"), 0);
    (void) now;
}

static void MstBody(int k, uint32_t s, uint64_t ent, const float *l,
                     const float *other, uint64_t now)
{
    typeof(&g_mst.b[0]) b = &g_mst.b[k];
    float dt = b->last_t
        && now > b->last_t ? (float) (now - b->last_t) / 1000.0f : 0.016f;
    if (dt > 0.1f)
        dt = 0.1f;
    float spd = MstSpeed(k, l, now);
    const char *why = MstBusy(k, s, now, spd);
    if (b->on && b->ent != ent) {
        b->on = 0;
        b->free_since = 0;
    }                           // (a new puppet: respawn)
    if (why) {
        if (b->on) {
            int took = !strcmp(why, "paired move")
                || !strcmp(why, "dtl clip")
                || !strcmp(why, "reaction / dodge clip")
                || !strcmp(why, "melee move");
            if (took && k == 1)
                MstDrop(k, why);
            else
                MstStop(k, !strcmp(why, "moving")
                         || !strcmp(why,
                                    "moving (stick)") ? MST_BLEND_WALK :
                         MST_BLEND_MOVE, why);
        }
        b->free_since = 0;
        return;
    }
    if (!b->free_since)
        b->free_since = now ? now : 1;
    if (!b->on) {
        if (now - b->free_since < MST_SETTLE_MS)
            return;
        uint64_t r = MhrAnimate(ent, g_mst.sid, 0.0f, MST_BLEND_IN);
        if (r == ~0ULL) {
            static int nl;
            if (LOGCAP(nl, 10))
                Xlog("mst: %s refused by animate_ on %s", g_mst.name,
                     k ? "the puppet" : "us");
            b->free_since = now + 2000;
            return;
        }
        b->on = 1;
        b->ent = ent;
        b->sid = g_mst.sid;
        b->t0 = now;
        b->loops = 0;
        b->starts++;
        MSTLOG
            ("mst: stance ON on %s (%s, blend-in %.2f s) - free for %llu ms, %.2f m/s, %.2f m from %s",
             k ? "slot's puppet" : "us", g_mst.name, (double) MST_BLEND_IN,
             (unsigned long long) (now - b->free_since), (double) spd,
             (double) __builtin_sqrtf((l[0] - other[0]) *
                                      (l[0] - other[0]) + (l[2] -
                                                           other[2]) *
                                      (l[2] - other[2])),
             k ? "us" : "the partner");
        return;
    }
    float ph = PsyncPhase(ent, b->sid);
    if (ph < 0.0f && now - b->t0 > 300) {
        MstDrop(k, "another clip (the layer no longer has it)");
        return;
    }
    if (ph >= MST_LOOP_PH) {
        MhrAnimate(ent, b->sid, 0.0f, MST_BLEND_IN);
        b->loops++;
    }                           // (crossfade restart: no loop flag on animate_)
    if (k == 0)
        MstFace(l, other, now, dt);
}

static void MstTick(void)
{
    if (!MstOn()) {
        for (int k = 0; k < 2; k++)
            if (g_mst.b[k].on)
                MstStop(k, MST_BLEND_WALK, "switched off");
        return;
    }
    uint64_t now = GetTickCount64();
    float hp = LocalHp();
    if (g_mst.hp_last > 0.0f && hp < g_mst.hp_last - 0.5f) {
        g_mst.hit_until = now + MST_HIT_HOLD_MS;
        if (g_mst.b[0].on)
            MstStop(0, MST_BLEND_MOVE, "hit taken");
    }
    g_mst.hp_last = hp;
    if (g_mdu.slot < 0 || MatchSubstate() != 6) {
        for (int k = 0; k < 2; k++)
            if (g_mst.b[k].on)
                MstStop(k, 0.40f, "the duel ended");
        return;
    }
    uint32_t s = (uint32_t) g_mdu.slot;
    if (g_mst.b[1].on && g_mst.slot != s)
        MstStop(1, MST_BLEND_WALK, "a new partner");
    g_mst.slot = s;
    if (!MstPick())
        return;
    uint64_t h = PuppetHandle(s);
    float mb[16], pb[16];
    const float *me = PlayerLocator(mb), *pl =
        h ? PuppetLocator(h, pb) : NULL;
    if (!me || !pl || !__builtin_isfinite(me[0])
        || !__builtin_isfinite(pl[0]))
        return;
    MstBody(0, s, SID_PLAYER, me, pl, now);
    MstBody(1, s, h, pl, me, now);
}
