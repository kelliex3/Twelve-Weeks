// melee_feel.c - TLOU2 single-player melee camera feel in MP (camera shakes and reactions for both melee attacker and melee victim).
// Included after feel.c in t2r-ext.c. Notes: re/melee-feel-1025.md.
//
// What SP does (DC data, re/contextual-downs.md): every melee hit script calls start-rumble(melee-*-rumble-settings) + camera-shake-melee ->
// camera-shake(camera-shake-melee-hit-b) = an ADDITIVE camera animation. Measured on the SP gameplay camera (tools/spcam/*.py): the same 0.35 s kick
// after every melee hit of any weapon (XCAM_KICK: view down 1.36 deg, roll +0.76 -> -1.43 deg, yaw <= 0.37 deg); the player being HIT gets ~1.7x.
// The SP shake clips live in an unextracted pak and the script funcs (camera-shake-melee, start-rumble) are DC lambdas, not natives, so they cannot be
// called from here; the measured template is replayed instead, on the render camera only (gsh_filter: the unmodified game camera, no aim / shot /
// game-state change, never during a ctxdown / execution / intro camera which own the view).
//   attacker  - a blow that lands (hitsend / direct hit hooks): template x (fist 0.8, weapon 1.1, downing / one-shot blow 1.6), the roll side
//               alternates per blow (a combo swings left, right).
//   victim    - a blow that hurts us and leaves us standing (feel.c hrx_victim_hit): template x 1.7 of the same class, roll + a yaw push AWAY from the
//               attacker's side, head snapping back (view up) for a blow from the front, down from behind. The victim's body already plays the engine's
//               directional upper-body hit reaction (feel.c MSG_HITREACT, everyone sees it) and the stagger (melee_stagger.c); nothing extra is sent.
//   whiff     - a free swing with no contact 0.3 s in: a light follow-through (x0.3) once per swing.
// Not done: hit-stop (per-object time dilation would desync MP), controller rumble (start-rumble is a script lambda; opt-in probe below).
// Skipped in: downed / dying, paired moves, executions, ctxdown cameras, finisher guard, stun.
// Opt-out t2r-no-melee-shake.txt (victim overlay / ring / slash: t2r-no-melee-hitfx.txt, melee_hitfx.c); scale t2r-melee-shake-scale.txt (0.1 .. 3, default 1); t2r-melee-rumble-probe.txt = try native start-rumble (dev, 3 tries).
// Log prefix "meleefeel:".
// (measured from the SP log FXN lines + the per-frame camera of melee-prev / melee-cur):
// SP's hit shake IS one additive camera animation, started by the native camera-animation-enable on the contact frame:
//   landed blow      (melee-camera-shake,     camera-shake-melee-hit-b, 0,   S,   0.1, 1, 1.0)   S = 1.5 on every fist blow (95 calls: sprint attack, light 1-4,
//                                                                                               stun combo, dodge attack), 1.0 on blade / blunt blows (45 calls)
//   finisher blow    (script-user-func-shake, camera-shake-melee-hit-b, 0.1, 0.5, ...)           147 calls (finishers, wall / cover attacks, parry pairs)
//   the PLAYER hit   (script-user-func-shake, camera-shake-melee-hit-b, 0.1, 1.75, ...)          10 calls (ellie-1h-hr-*, npc-*-hit, struggle enter)
// Measured camera response (residual off a quadratic through frames -14..-2 and +30..+44, medians; n = 76 at 1.5, 27 at 1.0, 103 at 0.5, 7 at 1.75): the
// SAME curve times S, rotation only - pitch and roll in the camera frame, no fov change (0.00), no yaw of its own (the yaw residual is the rig's orbit):
//   roll   +0.38 S at +1 frame, +1.10 S at +4, through 0 at +6.3, -1.74 S at +10, -0.80 S at +14, 0 at +20.5, a +0.08 S tail to +26   (deg; 76 of 76 the same side)
//   pitch  +0.23 S at +3..+4 (up), through 0 at +5, -1.31 S at +10 (DOWN), -0.40 S at +14, 0 by +20
//   lens   a single dip of the lens against the root, -19 .. -36 mm x S at +6 frames, back by +10 (no oscillation)
// = at S 1.5: view down 1.96 deg + roll 2.66 deg + lens 5 cm low, 0.17 s after the contact: 4-6 % of the frame height (the 40-70 px at 1080p seen in the clip).
// MP before ('meleefeel: attacker kick - fist ... x0.8 of SP's 1.36 deg template'): the earlier template (peak 2 frames earlier, roll 1.43) at x0.8
// for fists (SP 1.5: MP showed 55 % of the pitch, 43 % of the roll), x1.1 weapons (SP 1.0), x1.6 'heavy' (SP: 1.5 fists / 1.0 weapons, 0.5 inside a finisher);
// the roll side alternated per blow and a +-0.4 deg x amp yaw push was added (SP: neither); the victim's view snapped UP x0.7 for a blow from the front with
// a yaw push away and a class factor 0.6-1.6 (SP: the same down-kick at 1.75 for every weapon); and a 13 / 17 Hz positional sine (52-87 mm, tau 80 ms) ran in
// spm_pos (SP: one smooth dip). With PS32_SHAKE: the curve below, SP's scales, one side, no yaw, the dip (melee_spcam.c spm_pos).
// t2r-ps32-cam-shake-old.txt = all of the old. t2r-ps32-cam-shake-native.txt (opt-in probe, NOT verified in MP: the clip may not be loaded) = call the
// engine's own camera-animation-enable with SP's arguments instead of replaying the curve.
static const float MFK32[][2] = {       // {pitch, roll} deg per unit scale, 60 Hz rows, row 0 = the contact frame
    { 0.00f, 0.00f }, { 0.02f, 0.38f }, { 0.20f, 0.76f }, { 0.23f, 0.93f },
        { 0.23f, 1.10f }, { -0.06f, 0.65f }, { -0.39f, 0.20f }, { -0.77f,
                                                                 -0.50f },
        { -1.13f, -1.21f },
    { -1.22f, -1.47f }, { -1.31f, -1.74f }, { -1.11f, -1.55f }, { -0.89f,
                                                                 -1.33f },
        { -0.66f, -1.06f }, { -0.40f, -0.80f }, { -0.28f, -0.64f },
        { -0.17f, -0.48f },
    { -0.11f, -0.36f }, { -0.05f, -0.24f }, { -0.03f, -0.14f }, { -0.02f,
                                                                 -0.04f },
        { 0.00f, 0.02f }, { 0.00f, 0.08f }, { 0.00f, 0.08f }, { 0.00f,
                                                               0.08f },
        { 0.00f, 0.06f },
    { 0.00f, 0.04f }, { 0.00f, 0.00f }
};

#define MFK32_N ((int)(sizeof MFK32 / sizeof MFK32[0]))
#define MFK32_FIST 1.5f
#define MFK32_WEAPON 1.0f
#define MFK32_TAKEN 1.75f
#define MFK32_FINISHER 0.5f
#define MFK_N 4
static struct {
    volatile int on;
    double t0;
    float amp, roll, pitch, yaw;
} g_mfk[MFK_N];
static volatile int g_mfkNext;
static int MfkFlag(int which)
{                               // 0 off, 1 rumble probe
    static int v[2] = { -1, -1 };
    static uint64_t at;
    uint64_t now = GetTickCount64();
    if (v[0] < 0 || now - at >= 2000) {
        at = now ? now : 1;
        static const char *fn[2] =
            { "t2r-no-melee-shake.txt", "t2r-melee-rumble-probe.txt" };
        for (int i = 0; i < 2; i++) {
            FILE *f = ExeSideOpen(fn[i], "r");
            int o = f != NULL;
            if (f)
                fclose(f);
            if (o != v[i])
                Xlog("meleefeel: %s %s",
                     i ? "rumble probe" : "melee camera shake",
                     i ? (o ? "ON" : "off") : (o ?
                                               "OFF (t2r-no-melee-shake.txt)"
                                               :
                                               "on (attacker kick, victim jolt, light whiff follow-through)"));
            v[i] = o;
        }
    }
    return v[which];
}

static float MfkScale(void)
{
    static float s = 1.0f;
    static uint64_t at;
    uint64_t now = GetTickCount64();
    if (!at || now - at >= 2000) {
        at = now ? now : 1;
        FILE *f = ExeSideOpen("t2r-melee-shake-scale.txt", "r");
        float v;
        if (f) {
            if (fscanf(f, "%f", &v) == 1 && v >= 0.1f && v <= 3.0f
                && v != s) {
                s = v;
                Xlog("meleefeel: scale %.2f", (double) v);
            }
            fclose(f);
        } else if (s != 1.0f) {
            s = 1.0f;
            Xlog("meleefeel: scale back to 1.00");
        }
    }
    return s;
}

// the camera must be the unmodified game camera and we in a state where a kick is fine
static const char *MfkBusy(void)
{
    if (MatchSubstate() != 6)
        return "not in a match";
    if (g_downed || g_downDying || g_uc4DyingUntil || LocalHp() <= 0.0f
        || g_downAt)
        return "downed / dying";
    if ((PsyncOwn() && !MduInPair()) || GxRunning()
        || GxVictimActive() || g_unarmExec)
        return "paired move / execution";       // (melee_duel.c: the duel's own -hit pairs keep the SP framing + hit kicks on both screens - root cause of "the melee camera never enables": every pair set this busy, spm_tick zeroed the framing)
    // (the first thrown punch had no hit reaction - A [16760] "attacker blow on slot 1 - no kick", B [12694] "victim
    // blow from slot 0 - no jolt: execution / ctxdown camera"; the later blows at A [16804] / [16863] kicked): the escaped pull-out's camera was still in its 1.5 s
    // ease-out (the sequence had ended 0.6 s / 68 frames earlier) and g_xcam_on alone vetoed the hit kick / jolt AND the SP duel framing (spm_tick's hard
    // gate calls this, so its own g_spm_xout 'framing under the blend-out' exception never took). A camera only on its way OUT (not the down camera) no
    // longer blocks the blow feel. t2r-mfk-xout-busy.txt = old.
    int xout_ok = g_xcamOn && g_xcam_out && g_xcam_role != 3
        && GetFileAttributesA("t2r-mfk-xout-busy.txt") ==
        INVALID_FILE_ATTRIBUTES;
    if ((g_execUntil && g_t60 < g_execUntil) || FinisherGuardOn()
        || (g_xcamOn && !g_mqCam && !xout_ok) || g_stun_until)
        return "execution / ctxdown camera / finisher / stun";
    return NULL;
}

// v5-spcam (SP run 1031 wall pin: the lens tightens 75 -> 66 -> 64 -> 62 on each strike; meleefov.c pin fov): count of real blows, 350 ms apart (an attacker blow and
// the victim jolt of the same hit on one screen count once)
static volatile int g_mfkBlows;
static uint64_t g_mfkBlowMs;
// ps32 opt-in probe (t2r-ps32-cam-shake-native.txt next to the exe, re-read every 2 s): SP's own call. 1 = the native took it (our curve is not added).
static int Mfk32Native(float scale)
{
    static int on = -1, nl;
    static uint64_t at;
    uint64_t now = GetTickCount64();
    if (on < 0 || now - at >= 2000) {
        at = now;
        int o =
            GetFileAttributesA("t2r-ps32-cam-shake-native.txt") !=
            INVALID_FILE_ATTRIBUTES;
        if (o != on)
            Xlog("meleefeel: ps32 native shake probe %s",
                 o ?
                 "ON (t2r-ps32-cam-shake-native.txt): camera-animation-enable(melee-camera-shake, camera-shake-melee-hit-b, 0, scale, 0.1, 1, 1.0)"
                 : "off (the measured curve is replayed)");
        on = o;
    }
    if (!on)
        return 0;
    float z = 0.0f, bo = 0.1f, one = 1.0f;
    uint64_t a[7] =
        { FNV64("melee-camera-shake"), FNV64("camera-shake-melee-hit-b"),
0, 0, 0, 1, 0 };
    memcpy(&a[2], &z, 4);
    memcpy(&a[3], &scale, 4);
    memcpy(&a[4], &bo, 4);
    memcpy(&a[6], &one, 4);
    uint64_t r = DcCall(FNV64("camera-animation-enable"), 7, a);
    if (LOGCAP(nl, 40))
        Xlog("meleefeel: ps32 native shake camera-animation-enable(scale %.2f) -> %llx%s", (double) scale, (unsigned long long) r, r == ~0ULL ? " (not callable here - the measured curve plays)" : "");
    return r != ~0ULL;
}

// amp in template units (1.0 = SP's 1.36 deg view-down peak), roll side +-1, pitch +1 = view down / -1 = up, yaw: peak degrees (+ = left)
static void MfkAdd(float amp, float roll, float pitch, float yaw)
{
    float sc = MfkScale();
    amp *= sc;
    if (!(amp > 0.0f))
        return;
    if (amp >= 0.5f) {
        uint64_t nb = GetTickCount64();
        g_spm_ev_ms = nb;
        if (nb - g_mfkBlowMs > 350) {
            g_mfkBlowMs = nb;
            g_mfkBlows++;
        }
    }                           // a real blow (not the whiff follow-through): opens the SP melee camera framing on both screens (melee_spcam.c)
    if (g_ps32[PS32_SHAKE] && Mfk32Native(amp))
        return;                 // ps32 opt-in probe: the engine's own shake clip played this blow
    SpmHit(amp);
    int i = g_mfkNext++ & (MFK_N - 1);
    g_mfk[i].on = 0;
    MemoryBarrier();
    g_mfk[i].t0 = GshNow();
    g_mfk[i].amp = amp;
    g_mfk[i].roll = roll;
    g_mfk[i].pitch = pitch;
    g_mfk[i].yaw = yaw * sc;
    MemoryBarrier();
    g_mfk[i].on = 1;
    g_gshAny = 1;
}

// render thread (gsh_filter): add the live kicks' rotation (gsh axes: rot0 pitch-down +, rot1 yaw-left +, rot2 roll) to rot; 1 = any live
static int MfkRot(double now, float *rot)
{
    int live = 0;
    for (int i = 0; i < MFK_N; i++) {
        if (!g_mfk[i].on)
            continue;
        float t = (float) (now - g_mfk[i].t0), x = t * 60.0f + 1.0f;
        if (t < 0.0f)
            continue;
        if (g_ps32[PS32_SHAKE]) {       // ps32: SP's camera-shake-melee-hit-b per unit scale (block comment at MFK32): pitch + roll, one side, no yaw
            float x2 = t * 60.0f;
            if (x2 > (float) (MFK32_N - 1)) {
                g_mfk[i].on = 0;
                continue;
            }
            live = 1;
            int k2 = (int) x2;
            if (k2 < 0)
                k2 = 0;
            if (k2 >= MFK32_N - 1)
                k2 = MFK32_N - 2;
            float u2 = x2 - (float) k2, pi2 =
                MFK32[k2][0] + (MFK32[k2 + 1][0] - MFK32[k2][0]) * u2,
                ro2 =
                MFK32[k2][1] + (MFK32[k2 + 1][1] - MFK32[k2][1]) * u2;
            rot[0] += -pi2 * g_mfk[i].amp;      // (gsh rot0: + = view down; the curve's pitch is - = down)
            if (g_mfk[i].roll != 0.0f)
                rot[2] += ro2 * g_mfk[i].amp;   // (the side the earlier template's roll x +1 had: -Maya z)
            continue;
        }
        if (x > (float) (XCAM_KICK_N - 1)) {
            g_mfk[i].on = 0;
            continue;
        }
        live = 1;
        int k = (int) x;
        if (k < 0)
            k = 0;
        if (k >= XCAM_KICK_N - 1)
            k = XCAM_KICK_N - 2;
        float u = x - (float) k, m[3];
        for (int j = 0; j < 3; j++)
            m[j] = XCAM_KICK[k][j] + (XCAM_KICK[k + 1][j] - XCAM_KICK[k][j]) * u;       // Maya axes: x right, y up, z back
        float prof = -m[0] / 1.36f;
        if (prof < 0.0f)
            prof = 0.0f;        // the down-kick's time shape
        rot[0] +=
            -m[0] * g_mfk[i].amp * g_mfk[i].pitch * (g_mfk[i].pitch >
                                                     0 ? 1.0f : 0.7f);
        rot[1] += m[1] * g_mfk[i].amp + g_mfk[i].yaw * prof;
        rot[2] += -m[2] * g_mfk[i].amp * g_mfk[i].roll;
    }
    return live;
}

// controller rumble as SP (re/sp-sandbox-runs/nr-1031 trace: start-rumble(<settings>, 0) - melee-standing-attack-rumble-settings on a landed
// blow (115x), melee-standing-finisher-strong-rumble-settings on heavy / finishing blows and the pull-out grab (113x)). The native is callable in MP
// (int110 probe: returns 0). Local pad only (it rumbles the player's own controller). t2r-no-rumble.txt = off.
static void MfkRumble(const char *settings)
{
    static int off = -1;
    static uint64_t at;
    uint64_t now = GetTickCount64();
    if (off < 0 || now - at > 3000) {
        at = now;
        off =
            GetFileAttributesA("t2r-no-rumble.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    if (off)
        return;
    uint64_t a[2] = { FNV64(settings), 0 };
    uint64_t r = DcCall(FNV64("start-rumble"), 2, a);
    static int nl;
    if (LOGCAP(nl, 30))
        Xlog("meleefeel: rumble %s -> %llx", settings,
             (unsigned long long) r);
}

static void MfkRumbleProbe(void)
{
    static int tried;
    if (tried >= 3 || !MfkFlag(1))
        return;
    tried++;
    uint64_t a[2] = { FNV64("melee-standing-finisher-medium-rumble-settings"), 0 };     // (SP's scripts call it with 2 args: settings, 0 - script-user-funcs-impl rumble-recorder)
    uint64_t r = DcCall(FNV64("start-rumble"), 2, a);
    Xlog("meleefeel: rumble probe start-rumble(melee-standing-finisher-medium-rumble-settings) -> %llx%s", (unsigned long long) r, r == ~0ULL ? " (not a callable native here)" : "");
}

// we landed a melee blow on a puppet (hitsend / direct hit paths; one kick per 140 ms)
static void MfkAttackerHit(uint32_t slot, uint64_t w, float dmg,
                             int heavy)
{
    static uint64_t last;
    static int flip, nl;
    if (!(dmg > 0.0f) || (!g_msAtk && !MduInPair()) || !MhsIsBlow(w)
        || MfkFlag(0))
        return;
    uint64_t now = GetTickCount64();
    if (now - last < 140)
        return;
    const char *b = MfkBusy();
    if (b) {
        if (LOGCAP(nl, 20))
            Xlog("meleefeel: attacker blow on slot %u - no kick: %s", slot,
                 b);
        return;
    }
    if (slot < T2R_MAX_PEERS && IsTeammate(slot))
        return;
    last = now;
    int wpn = MeleeClassOf(w, NULL) >= 0;
    float amp = heavy ? 1.6f : wpn ? 1.1f : 0.8f;
    flip ^= 1;
    if (g_ps32[PS32_SHAKE]) {
        amp = wpn ? MFK32_WEAPON : MFK32_FIST;
        flip = 1;
    }                           // ps32: SP's scale by weapon (fists 1.5, blade / blunt 1.0), always the same side
    MfkAdd(amp, flip ? 1.0f : -1.0f, 1.0f, (flip ? 0.4f : -0.4f) * amp);
    MfkRumble(heavy ? "melee-standing-finisher-strong-rumble-settings" :
               "melee-standing-attack-rumble-settings");
    MfkRumbleProbe();
    if (LOGCAP(nl, 60)) {
        if (g_ps32[PS32_SHAKE])
            Xlog("meleefeel: attacker kick - %s%s on slot %u: SP's camera-shake-melee-hit-b at scale %.2f (view down %.2f deg, roll +%.2f / -%.2f deg, peak 0.17 s after the contact; ps32)", wpn ? "weapon" : "fist", heavy ? " (downing / one-shot blow)" : "", slot, (double) amp * MfkScale(), (double) (1.31f * amp * MfkScale()), (double) (1.10f * amp * MfkScale()), (double) (1.74f * amp * MfkScale()));
        else
            Xlog("meleefeel: attacker kick - %s%s on slot %u, x%.1f of SP's 1.36 deg template, roll side %s", wpn ? "weapon" : "fist", heavy ? " (downing / one-shot blow)" : "", slot, (double) amp * MfkScale(), flip ? "+" : "-");
    }
}

// a swing with no contact (called from whiff_recover_tick when it starts the tail boost): light follow-through, once per swing
static void MfkWhiff(void)
{
    static int nl, flip;
    if (MfkFlag(0) || MfkBusy())
        return;
    flip ^= 1;
    MfkAdd(0.3f, flip ? 1.0f : -1.0f, 1.0f, 0.0f);
    if (LOGCAP(nl, 30))
        Xlog("meleefeel: whiff follow-through x%.2f",
             (double) (0.3f * MfkScale()));
}

// a melee blow hurt us and left us standing (feel.c hrx_victim_hit; same filters as the stagger)
static void MfkVictimHit(uint32_t slot, uint64_t w, float dmg,
                           float hp_before, float hp_after)
{
    static uint64_t last;
    static int nl;
    if (MatchSubstate() != 6 || !(dmg > 0.0f) || dmg >= 150.0f
        || slot >= T2R_MAX_PEERS || !MhsIsBlow(w))
        return;
    if (!(hp_after > 0.0f) || hp_before - hp_after < 0.5f || MfkFlag(0)
        || IsTeammate(slot))
        return;
    uint64_t now = GetTickCount64();
    if (now - last < 140)
        return;
    const char *b = HrxBusyOwn() && !MduInPair()? "busy" : MfkBusy();      // (the duel's -hit pair on us counted as "busy" - no SP jolt on duel blows)
    if (b) {
        if (LOGCAP(nl, 20))
            Xlog("meleefeel: victim blow from slot %u - no jolt: %s", slot,
                 b);
        return;
    }
    float vb[16], ab[16];
    const float *vl = Uc4MyLoc(vb);
    uint64_t ah = PuppetHandle(slot);
    const float *al = ah ? PuppetLocator(ah, ab) : NULL;
    if (!vl || !al)
        return;
    float dx = al[0] - vl[0], dz = al[2] - vl[2], dl =
        __builtin_sqrtf(dx * dx + dz * dz);
    if (dl > MHS_REACH)
        return;                 // a thrown brick, not a blow
    float fx = 2 * (vl[4] * vl[6] + vl[7] * vl[5]), fz =
        1 - 2 * (vl[4] * vl[4] + vl[5] * vl[5]);
    float side = dl > 0.05f ? (fz * dx - fx * dz) / dl : 0.0f, front = dl > 0.05f ? (fx * dx + fz * dz) / dl : 0.0f;    // side > 0: attacker on our left
    last = now;
    int wpn = MeleeClassOf(w, NULL) >= 0;
    int two = 0, hcls = MhfxClass(w, &two, NULL);      // melee_hitfx.c: 1 blunt, 2 blade, 0 fists / other
    float amp = (wpn ? 1.1f : 0.8f) * 1.7f * (hcls == 1 ? (two ? 1.6f : 1.35f) : hcls == 2 ? 0.6f : 1.0f);      // SP: the player being hit gets ~1.7x; blunt = the heavy-hit camera, blade = slight
    MhfxNote(slot, w, dmg);    // + the concussion / slash overlay and sound
    float sd = side > 0.0f ? 1.0f : -1.0f;
    float yaw = -sd * (0.5f + 0.5f * __builtin_fabsf(side)) * amp;      // pushed away from the attacker's side
    if (g_ps32[PS32_SHAKE]) {   // ps32: SP's hit-taken shake = the same clip at scale 1.75 for every weapon (7 player hits: fists, 1h blade, 2h blunt), view DOWN, no yaw push
        amp = MFK32_TAKEN;
        MfkAdd(amp, 1.0f, 1.0f, 0.0f);
        MfkRumble(hcls == 1
                   || dmg >=
                   30.0f ? "melee-standing-finisher-strong-rumble-settings"
                   : "melee-standing-attack-rumble-settings");
        if (LOGCAP(nl, 60))
            Xlog("meleefeel: victim jolt - %s blow from slot %u: SP's camera-shake-melee-hit-b at scale %.2f (view down %.2f deg, roll +%.2f / -%.2f deg; ps32)", wpn ? "weapon" : "fist", slot, (double) amp * MfkScale(), (double) (1.31f * amp * MfkScale()), (double) (1.10f * amp * MfkScale()), (double) (1.74f * amp * MfkScale()));
        return;
    }
    MfkAdd(amp, sd, front > 0.2f ? -1.0f : 1.0f, yaw);
    MfkRumble(hcls == 1
               || dmg >=
               30.0f ? "melee-standing-finisher-strong-rumble-settings" :
               "melee-standing-attack-rumble-settings");
    if (LOGCAP(nl, 60))
        Xlog("meleefeel: victim jolt - %s blow from slot %u (%s, attacker %s, side %.2f front %.2f), x%.1f of SP's template, yaw %.1f deg, view %s", wpn ? "weapon" : "fist", slot, front > 0.2f ? "front" : "behind / side", side > 0.0f ? "left" : "right", (double) side, (double) front, (double) amp * MfkScale(), (double) yaw * MfkScale(), front > 0.2f ? "snaps up" : "snaps down");
}
