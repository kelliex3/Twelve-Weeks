// ---- down camera: SP death animations where the player is shot while running put the camera at a point ahead of / beside the
// character, following it as it falls. When the player is DOWNED while running (or shot down while stationary) the camera goes into that style until they
// get input control of being downed, then shifts back into the regular position in a smooth, cinematic way. Measurements / clip list: tools/dcam_gen.py.
//
// Data (tools/dcam_gen.py -> dcam_tracks.h): TLOU2 SP's own player death clips (ellie-death-run-fw-front-r-headshot / -shldr, ellie-death-stand-frnt-l-shldr /
// -back-headshot / -side-l-headshot / -side-r-headshot of anim-ellie-player-movement-combat.pak) carry 4-6 cameras each (apReference-camera1..N + dof + extra)
// in the clip's ALIGN (object) frame: the camera rides with the character (world = align(t) o camera(t), checked: the camera looks at the character within
// 5-17 deg at the start). Position / lens are used here: world = OUR body's root now + R(yaw0) * (left, up, forward), yaw0 = the travel direction (running) or the
// facing (stationary) at the hit - the camera is ahead of the character, low, and follows our own body's real motion (the go-down slide / fall). The aim is
// the live body (spined joint, springs), not the clip's rotation: our fall is not the SP clip, the authored rotation would drift off our body.
// Lens: hfov of a 24 mm film-height lens (tools/ctxcam_gen.py): 24 mm = 82 deg, 28 mm = 74, 31 mm = 70.
// Flow: downed_set(1) -> dcam_on_down (st 1: world rays from the chest to every candidate camera's start / 1 s pose over one tick, the clear one wins) ->
// st 2 HOLD (hard cut, the render filter of the execution camera: g_xcam_*, role 3) until the downed state gives control (the very condition the
// downed tick uses to leave the fall: the stationary fall's lying moment / the knock-down clip's cut) -> st 3 BLEND (smoothstep over 1 s: the filter blends position
// as an orbit around our head, orientation nlerp and the FOV with the same weight toward the LIVE game camera, so the end is the game's downed camera and
// stick input already moves it) -> off. t2r-no-down-cam.txt = off. Log prefix "downcam:".
#include "dcam_tracks.h"
#define DCAM_RUN_MS 3.0f        // m/s (the real planar velocity at the hit, mom_use_peak): above = the running variant (Ellie's walk 1.5-2, jog 2.5-3, run 3.5+)
#define DCAM_CONTROL_BLEND_S 1.0f
#define DCAM_ABORT_BLEND_S 0.5f
#define DCAM_MIN_HOLD_S 0.6f    // a camera shown for less than this reads as a glitch, not a shot
#define DCAM_NCAND 6
static struct {
    int st;                     // 0 off, 1 choosing (rays out), 2 hold, 3 blend out
    int run, variant, ncand, tick;
    const dcam_track *cand[DCAM_NCAND], *trk;
    uint64_t job[DCAM_NCAND][2];
    float root0[3], fwd[2], left[2], spd, fov0, fov_t;
    double ms0, msb, bl_s;
    float lk[3], lkv[3], pos[3], posv[3], arm, arm_tgt, ray_len;
    int init, ray_state;
    uint64_t ray;
    uint64_t downf;
    int ab;                     // the blend is an abort blend (not the control hand-over)
    float O[3], T[3], Tv[3];
    int oinit;
    int aslot;
    float afov;                 // who downed us (-1 unknown): framed too; the lens widening that keeps them in      // torso-follow framing: the shot side (world, fixed at the cut), the smoothed torso
} g_dc;
static void XvBody(uint64_t h, const float *loc, float *pel, float *chest,
                    float *head);
// the head of the player in slot (its headb joint, else its root + 1.6 m); 0 = unknown
static int DcamSlotHead(int slot, float *out)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    typedef void *(*fn_proc)(uint64_t);
    typedef const float *(*fn_loc)(void *, float *);
    if (slot < 0 || slot > T2R_MAX_PEERS - 1)
        return 0;
    uint8_t *peer = ((fn_peer) T2rPeer) ((uint32_t) slot);
    uint64_t h = peer ? *(uint64_t *) (peer + 0x60) : 0;
    if (!h)
        return 0;
    if (GxJointPos(h, "headb", out) && XvFin3(out))
        return 1;
    uint8_t *pr = ((fn_proc) T2R_SYM(kFnEntityFromHandle)) (h);
    float b[16];
    const float *l =
        pr
        ? ((fn_loc) ((uint8_t *) GetModuleHandleW(NULL) + 0x133db30)) (pr +
                                                                       0x100,
                                                                       b) :
        NULL;
    if (!l || !XvFin3(l))
        return 0;
    out[0] = l[0];
    out[1] = l[1] + 1.6f;
    out[2] = l[2];
    return 1;
}

static void DcamKeyAt(const dcam_track *k, float t, float *p, float *fov)
{
    const dcam_key *a = k->k;
    if (t <= a[0].t) {
        memcpy(p, a[0].p, 12);
        *fov = a[0].fov;
        return;
    }
    if (t >= a[k->n - 1].t) {
        memcpy(p, a[k->n - 1].p, 12);
        *fov = a[k->n - 1].fov;
        return;
    }
    int i = 0;
    while (i < k->n - 2 && a[i + 1].t <= t)
        i++;
    float u = (t - a[i].t) / (a[i + 1].t - a[i].t);
    for (int j = 0; j < 3; j++)
        p[j] = a[i].p[j] + (a[i + 1].p[j] - a[i].p[j]) * u;
    *fov = a[i].fov + (a[i + 1].fov - a[i].fov) * u;
}

static void DcamWorld(const dcam_track *k, float t, const float *root,
                       float *C, float *fov)
{
    float p[3];
    DcamKeyAt(k, t, p, fov);
    C[0] = root[0] + g_dc.left[0] * p[0] + g_dc.fwd[0] * p[2];
    C[1] = root[1] + p[1];
    C[2] = root[2] + g_dc.left[1] * p[0] + g_dc.fwd[1] * p[2];
}

// read every ray job still in flight (a kicked job must be read once)
static void DcamDrain(void)
{
    for (int i = 0; i < DCAM_NCAND; i++)
        for (int k = 0; k < 2; k++)
            if (g_dc.job[i][k]) {
                RayT(g_dc.job[i][k]);
                g_dc.job[i][k] = 0;
            }
    if (g_dc.ray) {
        RayT(g_dc.ray);
        g_dc.ray = 0;
    }
    g_dc.ray_state = 0;
}

static void DcamReleaseCamera(void)
{
    g_xcamShake[0] = g_xcamShake[1] = g_xcamShake[2] = 0.0f;
    g_xcamCrane = 0;
    g_xcamOn = 0;
    g_xcam_w = 0.0f;
    g_xcam_out = 0;
    g_xcamAvOk = 0;
    g_xcamAuthored = 0;
    g_ehbOn = 0;               // (the exec attacker hand-back is role 1 only: never on for the down camera)
    if (!g_fcam_on)
        g_mcamWatch = 0;
}

// the camera ends at once (no blend): the game's camera shows. why = the log reason
static void DcamHard(const char *why)
{
    if (g_dc.st >= 2 && g_xcamOn && g_xcam_role == 3)
        DcamReleaseCamera();
    if (g_dc.st)
        Xlog("downcam: done (%s) after %.0f ms", why,
             g_dc.ms0 > 0.0 ? PsyncMs() - g_dc.ms0 : 0.0);
    DcamDrain();
    g_dc.st = 0;
}

// the execution / takedown camera (xcam_start) takes the camera filter over: no release, it re-initialises everything
static void DcamYield(void)
{
    if (!g_dc.st)
        return;
    g_xcamCrane = 0;
    Xlog("downcam: done (yields to the execution / takedown camera) after %.0f ms", g_dc.ms0 > 0.0 ? PsyncMs() - g_dc.ms0 : 0.0);
    DcamDrain();
    g_dc.st = 0;
}

static void DcamBeginBlend(float secs, const char *why, int abort)
{
    if (g_dc.st != 2)
        return;
    g_dc.st = 3;
    g_dc.msb = PsyncMs();
    g_dc.bl_s = secs;
    g_dc.ab = abort;
    g_xcam_out = 1;             // the way back to the game camera is an orbit around our head (the filter's blend-out)
    Xlog("downcam: %s -> blend over %.1f s (held %.0f ms)", why, secs,
         g_dc.msb - g_dc.ms0);
}

static void DcamOnUp(void)
{
    if (g_dc.st == 1) {
        DcamDrain();
        g_dc.st = 0;
        Xlog("downcam: cancelled before the cut (up / revived)");
    } else if (g_dc.st == 2)
        DcamBeginBlend(DCAM_ABORT_BLEND_S, "up / revived", 1);
}

// one frame of the camera: writes g_xcam_pos / g_xcam_look; 0 = keep the previous frame
static int DcamFrame(const float *me, float dt, float t)
{
    float vp[3], vc[3], vh[3];
    XvBody(SID_PLAYER, me, vp, vc, vh);
    float C[3], fov;
    DcamWorld(g_dc.trk, t, me, C, &fov);
    // the first version was not SP style (janky, very linear). Wanted: more close up, focused on the character, moving synced with the torso,
    // slight camera shake / zoom in (SP camera work); on input it zooms back out to the regular FOV and smooths to the regular position. The SP camera only
    // gives the SIDE of the shot (its start direction from the chest, world-fixed at the cut); the lens rides on the live torso (chest joint, spring), 1.45 m
    // pushing in to 1.0 m over 1.6 s (ease-out), lens 58 -> 44 deg (a slow zoom in), elevation -8..+18 deg; a decaying impact kick at the cut + a handheld
    // sway (g_xcam_shake: the filter's right / up / back rotations). The blend-out (filter orbit + FOV to the game's) is the hand-back.
    if (!g_dc.oinit) {
        float o[3] = { C[0] - vc[0], C[1] - vc[1], C[2] - vc[2] }, oh =
            __builtin_sqrtf(o[0] * o[0] + o[2] * o[2]);
        if (oh < 0.1f) {
            o[0] = g_dc.fwd[0];
            o[2] = g_dc.fwd[1];
            oh = 1.0f;
        }
        float el = __builtin_atan2f(o[1], oh);
        {                       // always frame the attacker as well, even behind cover, so you know WHO got you: the lens goes on our far side from the
            // shooter (looking past our body at them), 25 deg off that line toward the SP camera's side so the body does not hide them
            float A[3];
            if (DcamSlotHead(g_dc.aslot, A)) {
                float ax = vc[0] - A[0], az = vc[2] - A[2], al =
                    __builtin_sqrtf(ax * ax + az * az);
                if (al > 0.5f) {
                    ax /= al;
                    az /= al;
                    float sd =
                        (ax * o[2] - az * o[0]) >= 0.0f ? 1.0f : -1.0f, r =
                        0.436f * sd, c = __builtin_cosf(r), sn =
                        __builtin_sinf(r);
                    o[0] = (ax * c + az * sn) * oh;
                    o[2] = (-ax * sn + az * c) * oh;
                    Xlog("downcam: framing the shooter (slot %d, %.1f m away) - lens on our far side, 25 deg off the line", g_dc.aslot, al);
                }
            }
        }
        el = el < 0.21f ? 0.21f : el > 0.42f ? 0.42f : el;      // 12 .. 24 deg above the body: over the grass, the lying body readable (was -8 .. +18)
        g_dc.O[0] = o[0] / oh * __builtin_cosf(el);
        g_dc.O[1] = __builtin_sinf(el);
        g_dc.O[2] = o[2] / oh * __builtin_cosf(el);
        memcpy(g_dc.T, vc, 12);
        memset(g_dc.Tv, 0, 12);
        g_dc.oinit = 1;
        Xlog("downcam: torso-follow framing - side %.2f %.2f %.2f (elev %.0f deg) from %s camera %d", g_dc.O[0], g_dc.O[1], g_dc.O[2], el * 57.2957795f, g_dc.trk->clip, g_dc.trk->cam);
    }
    {                           // the anchor: the body's middle in the plane (a lying body stays whole in frame), at the HEAD's height minus 0.15 m (the camera
        // follows the downed with a delay and doesn't follow the height of the head"): stiff spring, no visible lag
        float mid[3] =
            { (vc[0] + vp[0]) * 0.5f, vh[1] - 0.15f,
     (vc[2] + vp[2]) * 0.5f };
        XcamVspring(g_dc.T, g_dc.Tv, mid, 30.0f, 0.05f, dt);
    }
    float e = t / 1.6f;
    e = e > 1.0f ? 1.0f : e;
    e = 1.0f - (1.0f - e) * (1.0f - e) * (1.0f - e);    // ease-out cubic
    // the second version was still too zoomed in (at 1.3 m the torso filled the frame, then the lens sat in the grass on the
    // backpack of the lying body): body scale - the whole falling / lying body (~1.8 m) in frame with ground around it, a gentle push-in only
    float D = 2.9f - 0.4f * e;
    fov = 66.0f - 6.0f * e;
    g_dc.fov_t = fov;
    for (int i = 0; i < 3; i++)
        C[i] = g_dc.T[i] + g_dc.O[i] * D;
    {
        float kick = 2.2f * __builtin_expf(-t * 4.0f);  // the hit: a short jolt that dies out in ~0.6 s
        float hs = 0.35f;       // handheld sway, deg
        g_xcamShake[0] =
            kick * __builtin_sinf(t * 31.0f) +
            hs * (__builtin_sinf(t * 1.7f) * 0.6f +
                  __builtin_sinf(t * 2.9f + 1.3f) * 0.4f);
        g_xcamShake[1] =
            kick * 0.6f * __builtin_sinf(t * 27.0f + 0.7f) +
            hs * (__builtin_sinf(t * 1.3f + 0.4f) * 0.6f +
                  __builtin_sinf(t * 2.3f + 2.1f) * 0.4f);
        g_xcamShake[2] =
            kick * 0.4f * __builtin_sinf(t * 23.0f + 1.9f) +
            hs * 0.5f * __builtin_sinf(t * 1.1f + 0.9f);
    }
    float S[3] = { vc[0], vc[1], vc[2] };
    // collision: from the chest out to the lens, pulled in along the line when blocked (shot mask 0x1b07: it does not stop on tall grass like the static
    // 0x207, which reads ~0.45 m over the ground); the lens is a low one, the arm only shortens (never lifts)
    if (g_dc.ray_state == 0) {
        float v[3] = { C[0] - S[0], C[1] - S[1], C[2] - S[2] };
        g_dc.ray_len =
            __builtin_sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        g_dc.ray =
            g_dc.ray_len > 0.1f ? WorldRayKickM(S, C, 0, 0x1b07) : 0;
        g_dc.ray_state = 1;
    } else {
        float tr = g_dc.ray ? RayT(g_dc.ray) : -1.0f, Ln =
            g_dc.ray_len, fr = 1.0f;
        if (tr >= 0.0f && tr < 0.98f && Ln > 0.1f)
            fr = (tr * Ln - 0.25f) / Ln;
        g_dc.arm_tgt = fr < 0.2f ? 0.2f : fr;
        g_dc.ray_state = 0;
    }
    if (!g_dc.init)
        g_dc.arm = 1.0f;
    g_dc.arm +=
        (g_dc.arm_tgt - g_dc.arm) * XcamK(g_dc.arm_tgt <
                                           g_dc.arm ? 0.4f : 0.08f, dt);
    float Cd[3];
    for (int i = 0; i < 3; i++)
        Cd[i] = S[i] + (C[i] - S[i]) * g_dc.arm;
    {                           // never inside our own body (capsules root -> pelvis -> chest -> head + 0.12 m), pushed out along the aim -> lens line; never under the floor
        struct {
            float a[3], b[3], r;
        } cap[4];
        float root[3] = { me[0], me[1] + 0.15f, me[2] }, top[3] =
            { vh[0], vh[1] + 0.12f, vh[2] };
        memcpy(cap[0].a, vh, 12);
        memcpy(cap[0].b, top, 12);
        cap[0].r = 0.17f + 0.12f;
        memcpy(cap[1].a, vc, 12);
        memcpy(cap[1].b, vh, 12);
        cap[1].r = 0.25f + 0.12f;
        memcpy(cap[2].a, vp, 12);
        memcpy(cap[2].b, vc, 12);
        cap[2].r = 0.25f + 0.12f;
        memcpy(cap[3].a, root, 12);
        memcpy(cap[3].b, vp, 12);
        cap[3].r = 0.28f + 0.12f;
        float dir[3] =
            { Cd[0] - vc[0], Cd[1] - vc[1], Cd[2] - vc[2] }, dl =
            __builtin_sqrtf(dir[0] * dir[0] + dir[1] * dir[1] +
                            dir[2] * dir[2]);
        if (dl > 1e-3f) {
            for (int i = 0; i < 3; i++)
                dir[i] /= dl;
            float pushed = 0.0f;
            for (int it = 0; it < 24; it++) {
                int inside = 0;
                for (int i = 0; i < 4; i++)
                    if (XcamSegDist(Cd, cap[i].a, cap[i].b) < cap[i].r) {
                        inside = 1;
                        break;
                    }
                if (!inside)
                    break;
                for (int i = 0; i < 3; i++)
                    Cd[i] += dir[i] * 0.05f;
                pushed += 0.05f;
            }
            static uint64_t nl;
            if (pushed > 0.0f && g_t60 - nl > 60) {
                nl = g_t60;
                Xlog("downcam: lens was inside our body - pushed out %.2f m", pushed);
            }
        }
        float fy = me[1] - 0.05f + 0.12f;
        if (Cd[1] < fy)
            Cd[1] = fy;
    }
    // the aim: between our head and chest (the live body), springs; the first frame snaps
    float tg[3] = { (vc[0] * 2.0f + vh[0] + vp[0]) * 0.25f, (vc[1] + vh[1] * 3.0f) * 0.25f, (vc[2] * 2.0f + vh[2] + vp[2]) * 0.25f };   // aim height: mostly the head
    {                           // the shooter in frame too: the aim leans 35 % of the way toward their head, the lens widens (<= 80 deg) until they sit inside 80 % of the half height
        float A[3], need = 0.0f;
        if (DcamSlotHead(g_dc.aslot, A)) {
            float b[3] =
                { tg[0] - Cd[0], tg[1] - Cd[1], tg[2] - Cd[2] }, a[3] =
                { A[0] - Cd[0], A[1] - Cd[1], A[2] - Cd[2] };
            float bl =
                __builtin_sqrtf(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]),
                al =
                __builtin_sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
            if (bl > 0.2f && al > 0.5f) {
                for (int i = 0; i < 3; i++) {
                    b[i] /= bl;
                    a[i] /= al;
                }
                float d = b[0] * a[0] + b[1] * a[1] + b[2] * a[2], th =
                    __builtin_acosf(d > 1.0f ? 1.0f : d <
                                    -1.0f ? -1.0f : d);
                if (th < 2.0f) {
                    float m[3], ml = 0.0f;
                    for (int i = 0; i < 3; i++) {
                        m[i] = b[i] * 0.65f + a[i] * 0.35f;
                        ml += m[i] * m[i];
                    }
                    ml = __builtin_sqrtf(ml);
                    if (ml > 1e-3f)
                        for (int i = 0; i < 3; i++)
                            tg[i] = Cd[i] + m[i] / ml * bl;
                    float off = 0.65f * th / 0.8f;      // the shooter's angle off the aim / the 80 % margin = the half height needed
                    off = off > 1.3f ? 1.3f : off;
                    need = 2.0f * __builtin_atanf(__builtin_tanf(off) / 0.5625f) * 57.2957795f; // as a horizontal lens (16:9)
                }
            }
        }
        need = need > 80.0f ? 80.0f : need;
        g_dc.afov += (need - g_dc.afov) * XcamK(0.3f, dt);
        if (g_dc.afov > g_dc.fov_t)
            g_dc.fov_t = g_dc.afov;
    }
    {
        static double lms;
        double nms = PsyncMs();
        if (nms - lms > 250.0) {
            lms = nms;
            Xlog("downcam: body t %.2f s | root %.2f %.2f %.2f pelvis %.2f %.2f %.2f chest %.2f %.2f %.2f head %.2f %.2f %.2f | lens %.2f %.2f %.2f", t, me[0], me[1], me[2], vp[0], vp[1], vp[2], vc[0], vc[1], vc[2], vh[0], vh[1], vh[2], Cd[0], Cd[1], Cd[2]);
        }
    }
    if (!g_dc.init) {
        memcpy(g_dc.lk, tg, 12);
        memset(g_dc.lkv, 0, 12);
        memcpy(g_dc.pos, Cd, 12);
        memset(g_dc.posv, 0, 12);
        g_dc.init = 1;
    } else {
        XcamVspring(g_dc.lk, g_dc.lkv, tg, 28.0f, 0.1f, dt);   // stiff: the lens and aim stay on the body (were 11 / 13: a visible delay)
        XcamVspring(g_dc.pos, g_dc.posv, Cd, 24.0f, 0.1f, dt);
    }
    if (!XvFin3(g_dc.pos) || !XvFin3(g_dc.lk)) {
        g_dc.init = 0;
        return 0;
    }
    float f[3] =
        { g_dc.lk[0] - g_dc.pos[0], g_dc.lk[1] - g_dc.pos[1],
   g_dc.lk[2] - g_dc.pos[2] }, fl =
__builtin_sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (fl < 0.25f)
        return 0;
    for (int i = 0; i < 3; i++)
        f[i] /= fl;
    {                           // pitch within -75 (down) .. +35 (up) deg: the render basis degenerates straight up / down
        float h = __builtin_sqrtf(f[0] * f[0] + f[2] * f[2]), p =
            __builtin_atan2f(f[1], h), lo = -75.0f * 0.0174532925f, hi =
            35.0f * 0.0174532925f;
        if (h < 1e-4f) {
            f[0] = g_dc.fwd[0];
            f[2] = g_dc.fwd[1];
            h = 1.0f;
        }
        p = p < lo ? lo : p > hi ? hi : p;
        float c = __builtin_cosf(p);
        f[0] = f[0] / h * c;
        f[2] = f[2] / h * c;
        f[1] = __builtin_sinf(p);
    }
    for (int i = 0; i < 3; i++) {
        g_xcamPos[i] = g_dc.pos[i];
        g_xcam_look[i] = f[i];
    }
    return 1;
}

static void DcamEngage(const float *me)
{
    uint8_t *m = IcamManager();
    uint64_t none = 0;
    uint64_t rd =
        DcCall(FNV64("camera-get-dir-through-reticle"), 0, &none);
    if (!m || !rd || rd == ~0ULL || IsBadReadPtr((void *) rd, 12)) {
        Xlog("downcam: not engaged (camera manager %p, reticle dir %llx)",
             (void *) m, (unsigned long long) rd);
        DcamDrain();
        g_dc.st = 0;
        return;
    }
    memcpy((void *) g_xcam_gdir, (void *) rd, 12);
    {                           // the game's FOV (camera-get-fov-degrees): our lens blends from / back to it
        uint64_t fv = DcCall(FNV64("camera-get-fov-degrees"), 0, &none);
        float f = 0;
        if (fv != ~0ULL)
            memcpy(&f, &fv, 4);
        g_dc.fov0 =
            MfGameFov((f > 20.0f
                         && f < 130.0f) ? f : XCAM_T2_DEFAULT_FOV);
    }
    g_dc.ms0 = PsyncMs();
    g_dc.init = 0;
    g_dc.oinit = 0;
    g_dc.ray_state = 0;
    g_dc.ray = 0;
    g_dc.arm = g_dc.arm_tgt = 1.0f;
    if (!DcamFrame(me, 1.0f / 60.0f, 0.0f)) {
        Xlog("downcam: not engaged (first frame invalid)");
        DcamDrain();
        g_dc.st = 0;
        return;
    }
    g_xcam_sign = 0;
    g_xcam_role = 3;
    g_xcamRoll = 0.0f;
    g_xcamShake[0] = g_xcamShake[1] = g_xcamShake[2] = 0.0f;
    g_xcamAuthored = 1;
    g_xcam_out = 0;
    g_ehbOn = 0;
    g_ehbOpOk = 0;
    g_ehb_rstate = 0;
    g_ehbRay = 0;
    g_xcamAvOk = 0;
    g_xcam_gfwd_ok = 0;
    g_xcamGposOk = 0;
    g_xcam_w = 1.0f;
    g_xcam_valid = 1;
    g_xcamShiv = 0;
    g_xcam_axis_fail = 0;
    snprintf(g_xcamName, sizeof g_xcamName, "downcam");
    g_xcamMgr = m;
    g_xcamOn = 1;
    g_mcamWatch = 1;
    g_dc.st = 2;
    Xlog("downcam: cut to %s camera %d (%s, lens %.0f deg at the start, %.2f s clip) - held until the downed state gives control", g_dc.trk->clip, g_dc.trk->cam, g_dc.run ? "ahead along the run direction" : "front / side", g_dc.fov_t, g_dc.trk->dur);
}

// ---- the go-down camera (the SP styled camera downs are cut entirely: just a camera shake upon getting downed and a wobble down
// smoothly synced to the torso, kept slightly wobbly even when the character is already downed): the game camera stays; an impact shake + a slow
// decaying wobble (gsh_add), the render camera's height follows the chest's drop and a small sway stays on while downed (g_gdn, applied in
// gsh_filter). The SP death-camera framing below is kept only behind t2r-sp-down-cam.txt.
// c1657: the downed lens guard's main-thread half (render half: dlg_filter in t2r-ext.c). Body joints + the wall ray from our upper body toward the
// pre-guard lens, every frame while downed (also after finisher / takedown downs, which skip downcam). Opt-out: t2r-no-down-lens-guard.txt.
static void DlgTick(void)
{
    static uint64_t cfg_at;
    static int off = -1;
    static uint64_t nl;
    uint64_t now = GetTickCount64();
    if (off < 0 || now - cfg_at >= 2000) {
        cfg_at = now;
        int o = XcamSideFlag("t2r-no-down-lens-guard.txt");
        if (o != off)
            Xlog("downlens: %s",
                 o ? "OFF (t2r-no-down-lens-guard.txt)" :
                 "ON - while downed the lens stays out of walls (shot-mask ray, 0.25 m off) and out of our own body (t2r-no-down-lens-guard.txt = off)");
        off = o;
    }
    if (off || !g_downed || MatchSubstate() != 6 || g_downDying || g_uc4DyingUntil) {       // (a paired move: an authored camera fully on is skipped by dlg_filter itself)
        if (g_dlg.ray) {
            RayT(g_dlg.ray);
            g_dlg.ray = 0;
        }
        g_dlg.rs = 0;
        g_dlg.ok = 0;
        g_dlg.wall_d = -1.0f;
        return;
    }
    float a[16], vp[3], vc[3], vh[3];
    const float *me = PlayerLocator(a);
    if (!me || !XvFin3(me)) {
        g_dlg.ok = 0;
        return;
    }
    XvBody(SID_PLAYER, me, vp, vc, vh);
    float ex = vc[0] - me[0], ey = vc[1] - me[1], ez = vc[2] - me[2], hx =
        vh[0] - me[0], hy = vh[1] - me[1], hz = vh[2] - me[2];
    if (!XvFin3(vp) || !XvFin3(vc) || !XvFin3(vh)
        || ex * ex + ey * ey + ez * ez > 2.5f * 2.5f
        || hx * hx + hy * hy + hz * hz > 2.5f * 2.5f) {
        g_dlg.ok = 0;
        return;
    }
    float S[3] =
        { (vc[0] + vh[0]) * 0.5f, (vc[1] + vh[1]) * 0.5f + 0.10f,
   (vc[2] + vh[2]) * 0.5f };
    if (!g_dlg.ok && g_dlg.rs == 0)
        g_dlg.wall_d = -1.0f;
    if (g_dlg.rs == 1) {        // last frame's ray: the wall's distance along it
        float tr = RayT(g_dlg.ray);
        g_dlg.ray = 0;
        g_dlg.rs = 0;
        g_dlg.wall_d = tr >= 0.0f && tr < 0.98f ? tr * g_dlg.rlen : -1.0f;
    }
    if (g_dlg.rs == 0 && g_dlg.want_ok) {       // toward the lens the render thread would show without us, 0.30 m past it
        float L[3] =
            { g_dlg.want[0], g_dlg.want[1], g_dlg.want[2] }, d[3] =
            { L[0] - S[0], L[1] - S[1], L[2] - S[2] }, l =
            __builtin_sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (XvFin3(L) && l > 0.2f && l < 10.0f) {
            float E[3];
            for (int i = 0; i < 3; i++)
                E[i] = S[i] + d[i] / l * (l + 0.30f);
            g_dlg.ray = WorldRayKickM(S, E, 0, 0x1b07);
            if (g_dlg.ray) {
                g_dlg.rs = 1;
                g_dlg.rlen = l + 0.30f;
            }
        } else
            g_dlg.wall_d = -1.0f;
    }
    memcpy(g_dlg.S, S, 12);
    memcpy(g_dlg.me, me, 12);
    memcpy(g_dlg.vp, vp, 12);
    memcpy(g_dlg.vc, vc, 12);
    memcpy(g_dlg.vh, vh, 12);
    MemoryBarrier();
    g_dlg.t = GshNow();
    g_dlg.ok = 1;
    if ((g_dlg.n_wall || g_dlg.n_body || g_dlg.n_floor) && g_t60 - nl > 60) {
        nl = g_t60;
        Xlog("downlens: last ~1 s - lens pulled in front of a wall %u frame(s) (max %.2f m, wall %.2f m from the upper body), out of our body %u (max %.2f m), over the floor %u", g_dlg.n_wall, (double) g_dlg.pull_max, (double) g_dlg.wall_d, g_dlg.n_body, (double) g_dlg.push_max, g_dlg.n_floor);
        g_dlg.n_wall = g_dlg.n_body = g_dlg.n_floor = 0;
        g_dlg.push_max = g_dlg.pull_max = 0.0f;
    }
}

static void GdnTick(void)
{
    DlgTick();
    if (MatchSubstate() == 6) {        // our root for the render thread's standing-offset history (gdn_stand_capture) and the offset hold
        float a0[16];
        const float *r0 = PlayerLocator(a0);
        if (r0 && XvFin3(r0)) {
            g_gdn.rx = r0[0];
            g_gdn.rz = r0[2];
            MemoryBarrier();
            g_gdn.rt = GshNow();
        }
    }
    if (!g_gdn.on)
        return;
    if (MatchSubstate() != 6 || PsyncActive() || g_downDying
        || g_uc4DyingUntil) {
        g_gdn.hold = 0;
        g_gdn.on = 0;
        Xlog("downcam: shake / sway stopped (match / paired move / dying)");
        return;
    }
    if (g_gdn.hold && !g_downed) {
        g_gdn.hold = 0;
        Xlog("downcam: up again - the downed sway fades out");
    }
    float a[16], vp[3], vc[3], vh[3];
    const float *me = PlayerLocator(a);
    if (!me || !XvFin3(me))
        return;
    XvBody(SID_PLAYER, me, vp, vc, vh);
    // Observed: the camera was right in the floor, then eased up on input: the chest read 0, 0, 0 (no joint yet) on the first frames, the dip
    // started from height 0 and sat at its -1.2 m clamp - only a chest within 2.5 m of our root counts
    float ex = vc[0] - me[0], ey = vc[1] - me[1], ez = vc[2] - me[2];
    if (!XvFin3(vc) || ex * ex + ey * ey + ez * ez > 2.5f * 2.5f)
        return;
    g_gdn.chest = vc[1];
    g_gdn.chest_ok = 1;
    static double lms;
    double nms = PsyncMs(), t = GshNow() - g_gdn.t0;
    if (t < 3.5 && nms - lms > 250.0) {
        lms = nms;
        Xlog("downcam: torso dip chest %.2f (start %.2f, smoothed %.2f) camera offset %.2f m; standing offset %s r %.2f f %.2f, game camera off it by r %.2f f %.2f (held, weight %.2f)", vc[1], g_gdn.chest0, g_gdn.sm, g_gdn.dy, g_gdn.so_ok > 0 ? "held" : g_gdn.so_ok < 0 ? "NONE (no standing sample)" : "pending", g_gdn.sor, g_gdn.sof, g_gdn.cr, g_gdn.cf, g_gdn.cw);
    }
}

static void GdnStart(void)
{
    if (g_downNoFall || g_cdVicDown || g_upgVicDown) {
        Xlog("downcam: no shake - the down comes out of a finisher / takedown (its own camera)");
        return;
    }
    if (MatchSubstate() != 6 || PsyncActive() || g_downDying
        || g_uc4DyingUntil)
        return;
    // the shake after getting downed was too fast and jerky: the 12 Hz 3 deg hit shake is now a 4 Hz 1.4 deg one, the go-down wobble
    // a little smaller and slower
    GshAdd(1.4f, 0.7f, 4.0f);  // the hit: a short shake
    GshAdd(1.8f, 2.8f, 0.35f); // the go-down: a slow wobble, decaying
    g_gdn.on = 0;
    MemoryBarrier();
    g_gdn.chest_ok = 0;
    g_gdn.init = 0;
    g_gdn.cinit = 0;
    g_gdn.dy = 0.0f;
    g_gdn.hw = 0.0f;
    g_gdn.hold = 1;
    g_gdn.so_ok = 0;
    g_gdn.cw = 1.0f;
    g_gdn.cr = g_gdn.cf = 0.0f;
    g_gdn.t0 = GshNow();
    MemoryBarrier();
    g_gdn.on = 1;
    g_gshAny = 1;
    Xlog("downcam: shake + torso dip + downed sway on the game camera");
}

static void DcamOnDown(void)
{
    if (g_dc.st)
        DcamHard("a new down");
    if (XcamSideFlag("t2r-no-down-cam.txt")) {
        static int nl;
        if (!nl++)
            Xlog("downcam: off (t2r-no-down-cam.txt)");
        return;
    }
    if (!XcamSideFlag("t2r-sp-down-cam.txt")) {
        GdnStart();
        return;
    }
    // not for: a down that comes out of a finisher (no fall: its own camera plays), another camera running, no match, prone (tall-grass crawl / lying: no fall to frame)
    if (g_downNoFall || g_cdVicDown || g_upgVicDown) {
        Xlog("downcam: skipped - the down comes out of a finisher / takedown (its own camera)");
        return;
    }
    if (MatchSubstate() != 6 || !g_icam_tramp || g_xcamOn || g_icam_on
        || g_fcam_on || PsyncActive() || g_downDying
        || g_uc4DyingUntil) {
        Xlog("downcam: skipped (match %d, camera hook %d, other camera %d/%d/%d, paired move %d, dying %d)", MatchSubstate(), g_icam_tramp != NULL, g_xcamOn, g_icam_on, g_fcam_on, PsyncActive(), g_downDying);
        return;
    }
    uint64_t none = 0, pr = DcCall(FNV64("player-is-prone?"), 0, &none);
    if (pr != ~0ULL && (pr & 0xff)) {
        Xlog("downcam: skipped - we are prone");
        return;
    }
    float a[16];
    const float *me = PlayerLocator(a);
    if (!me || !XvFin3(me) || !(__builtin_fabsf(me[4]) <= 1.01f)) {
        Xlog("downcam: skipped - no body locator");
        return;
    }
    float lb[16];
    const float *loc = Uc4MyLoc(lb);
    int mc = loc ? MomDownClip(loc) : -1;     // fills g_mom_sp (the peak planar speed of the last 0.25 s: the real velocity at the hit) when it is a momentum down
    float sp = mc != -1 ? g_mom_sp : 0.0f;
    float fx = 2 * (me[4] * me[6] + me[7] * me[5]), fz =
        1 - 2 * (me[4] * me[4] + me[5] * me[5]), fl =
        __builtin_sqrtf(fx * fx + fz * fz);
    if (fl < 1e-3f) {
        fx = 0.0f;
        fz = 1.0f;
        fl = 1.0f;
    }
    fx /= fl;
    fz /= fl;
    g_dc.run = sp >= DCAM_RUN_MS;
    if (g_dc.run) {             // the camera frame = the travel direction (the clips are run-FORWARD deaths: the character faces the way it runs)
        float vl =
            __builtin_sqrtf(g_momV[0] * g_momV[0] +
                            g_momV[1] * g_momV[1]);
        if (vl > 0.5f) {
            fx = g_momV[0] / vl;
            fz = g_momV[1] / vl;
        }
    }
    g_dc.fwd[0] = fx;
    g_dc.fwd[1] = fz;
    g_dc.left[0] = fz;
    g_dc.left[1] = -fx;
    g_dc.aslot = (g_down_slot < T2R_MAX_PEERS
                  && (int) g_down_slot !=
                  (int) g_fl.me) ? (int) g_down_slot : -1;
    g_dc.afov = 0.0f;
    memcpy(g_dc.root0, me, 12);
    g_dc.spd = sp;
    int dir = g_dc.run ? 0 : (loc ? Uc4DirFromSlot(loc, g_down_slot) : -1);  // where the shot came from: 0 front, 1 behind, 2 left, 3 right (-1 unknown: front)
    g_dc.variant =
        g_dc.run ? 0 : dir == 1 ? 2 : dir == 2 ? 3 : dir == 3 ? 4 : 1;
    // the candidate cameras: one clip of the variant (alternating), its cameras that start AHEAD of the character first (the video: every cut puts the lens in front /
    // 3/4 front, low, looking back at the face), then the rest, at most DCAM_NCAND
    const char *clips[8];
    int nclips = 0;
    for (int i = 0; i < DCAM_N_TRACKS; i++) {
        if (DCAM_TRACKS[i].variant != g_dc.variant)
            continue;
        int seen = 0;
        for (int j = 0; j < nclips; j++)
            seen |= !strcmp(clips[j], DCAM_TRACKS[i].clip);
        if (!seen && nclips < 8)
            clips[nclips++] = DCAM_TRACKS[i].clip;
    }
    if (!nclips) {
        Xlog("downcam: skipped - no camera data for variant %d",
             g_dc.variant);
        return;
    }
    const char *clip =
        clips[(g_downCount + (int) (T2rRand() & 7)) % nclips];
    g_dc.ncand = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < DCAM_N_TRACKS && g_dc.ncand < DCAM_NCAND; i++) {
            const dcam_track *t = &DCAM_TRACKS[i];
            if (strcmp(t->clip, clip))
                continue;
            int front = t->k[0].p[2] > 0.3f;
            if ((pass == 0) == front)
                g_dc.cand[g_dc.ncand++] = t;
        }
    memset(g_dc.job, 0, sizeof g_dc.job);
    g_dc.tick = 0;
    g_dc.st = 1;
    g_dc.downf = g_downFrame;
    Xlog("downcam: start %s speed %.1f m/s (variant %d%s, shot from %s) clip %s, %d candidate cameras, frame forward %.2f %.2f at %.2f %.2f %.2f", g_dc.run ? "running" : "stationary", sp, g_dc.variant, g_dc.run ? " run" : "", dir == 1 ? "behind" : dir == 2 ? "the left" : dir == 3 ? "the right" : dir == 0 ? "the front" : "an unknown side", clip, g_dc.ncand, fx, fz, me[0], me[1], me[2]);
}

static void DcamTick(void)
{
    static double last;
    double now = PsyncMs();
    float dt = last > 0.0
        && now > last ? (float) ((now - last) * 0.001) : 1.0f / 60.0f;
    dt = dt < 0.002f ? 0.002f : dt > 0.05f ? 0.05f : dt;
    last = now;
    if (MatchSubstate() != 6) {
        DcamHard("match state changed");
        return;
    }
    if (g_dc.st >= 2) {
        if (!g_xcamOn || g_xcam_role != 3) {
            Xlog("downcam: done (the camera hook dropped it: axis check / another camera)");
            g_dc.st = 0;
            return;
        }
        if (IcamManager() != g_xcamMgr || g_icam_on || g_fcam_on) {
            DcamHard("camera manager changed / intro or free camera");
            return;
        }
    }
    if ((g_dc.st == 1 || g_dc.st == 2) && !g_downed) {
        if (g_dc.st == 1)
            DcamOnUp();
        else
            DcamBeginBlend(DCAM_ABORT_BLEND_S, "no longer downed", 1);
    }
    if (g_dc.st == 1
        && (g_downDying || g_uc4DyingUntil || LocalHp() <= 0.0f
            || PsyncActive())) {
        DcamDrain();
        g_dc.st = 0;
        Xlog("downcam: cancelled before the cut (dying / paired move)");
        return;
    }
    float a[16];
    const float *me = PlayerLocator(a);
    int fin = me && XvFin3(me) && __builtin_fabsf(me[4]) <= 1.01f;
    if (g_dc.st == 1) {
        if (!fin) {
            DcamDrain();
            g_dc.st = 0;
            Xlog("downcam: cancelled - no body locator");
            return;
        }
        float S[3] =
            { g_dc.root0[0], g_dc.root0[1] + 1.1f, g_dc.root0[2] };
        if (g_dc.tick == 0) {   // rays out: the chest to each candidate's first pose and its pose 1 s in
            for (int i = 0; i < g_dc.ncand; i++) {
                float C[3], fv;
                DcamWorld(g_dc.cand[i], 0.0f, g_dc.root0, C, &fv);
                g_dc.job[i][0] = WorldRayKickM(S, C, 0, 0x1b07);
                float t1 =
                    g_dc.cand[i]->dur < 1.0f ? g_dc.cand[i]->dur : 1.0f;
                DcamWorld(g_dc.cand[i], t1, g_dc.root0, C, &fv);
                g_dc.job[i][1] = WorldRayKickM(S, C, 0, 0x1b07);
            }
            g_dc.tick = 1;
            return;
        }
        int best = 0;
        float bv = -1.0f;
        char sc[96];
        int n = 0;
        sc[0] = 0;
        for (int i = 0; i < g_dc.ncand; i++) {
            float v = 0.0f;
            for (int k = 0; k < 2; k++) {
                float t = g_dc.job[i][k] ? RayT(g_dc.job[i][k]) : -2.0f;
                g_dc.job[i][k] = 0;
                v += (t < 0.0f || t >= 0.98f) ? 1.0f : t;       // clear = 1 (a failed kick counts clear); blocked = how far it got
            }
            if (v > bv + 1e-4f) {
                bv = v;
                best = i;
            }                   // ties: the earlier candidate (front-facing ones first)
            if (n < (int) sizeof sc - 12)
                n += snprintf(sc + n, SNLEFT(sc, n), "%s%d:%.2f",
                              i ? " " : "", g_dc.cand[i]->cam, v);
        }
        g_dc.trk = g_dc.cand[best];
        Xlog("downcam: camera %d of %s chosen by ray score (%s)",
             g_dc.trk->cam, g_dc.trk->clip, sc);
        DcamEngage(me);
        return;
    }
    if (g_dc.st < 2)
        return;
    // st 2 / 3: the camera of this frame
    float t = (float) ((now - g_dc.ms0) * 0.001);
    if (fin) {
        if (DcamFrame(me, dt, t))
            g_xcam_valid = 1;
    }
    float cutf = g_uc4Down && Uc4KnockOn()? g_uc4CutMe : g_downCut;
    uint64_t dn_ms = g_down_ms ? GetTickCount64() - g_down_ms : 0;
    if (g_dc.st == 2) {
        // the same condition the downed tick uses to leave the go-down (el >= cut: the stationary fall's lying moment, or the momentum / knock clip's cut), i.e. the lying
        // pose / forced prone / crawl take over = we have control of being downed
        int control = dn_ms >= (uint64_t) (cutf * 1000.0f) && !(g_fallMe
                                                                &&
                                                                !g_fallLieDone
                                                                [T2R_US]);
        if (control && now - g_dc.ms0 >= DCAM_MIN_HOLD_S * 1000.0)
            DcamBeginBlend(DCAM_CONTROL_BLEND_S, "control", 0);
        else if (PsyncActive() && PsyncRole() == 2) {
            DcamHard("a paired move plays on us");
            return;
        } else if (g_downDying || g_uc4DyingUntil || LocalHp() <= 0.0f)
            DcamBeginBlend(DCAM_ABORT_BLEND_S, "dying", 1);
        else if (now - g_dc.ms0 > 8000.0)
            DcamBeginBlend(DCAM_CONTROL_BLEND_S,
                             "cap (8 s, control never reported)", 0);
    }
    float w = 1.0f;
    if (g_dc.st == 3) {
        float u = (float) ((now - g_dc.msb) / (g_dc.bl_s * 1000.0));
        if (u >= 1.0f) {
            DcamHard("blend finished");
            return;
        }
        u = u < 0.0f ? 0.0f : u;
        w = 1.0f - u * u * (3.0f - 2.0f * u);   // ease in-out
    }
    g_xcam_w = w;
    g_xcamCrane = g_dc.st == 3;
    for (int i = 0; i < 3; i++)
        g_xcamShake[i] *= w;   // the sway / kick die with the hand-back
    if (g_dc.st == 3 && fin) {  // the orbit pivot: our head
        float vp[3], vc[3], vh[3];
        XvBody(SID_PLAYER, me, vp, vc, vh);
        g_xcamPiv[0] = vh[0];
        g_xcamPiv[1] = vh[1];
        g_xcamPiv[2] = vh[2];
        g_xcam_piv_ok = 1;
    }
    float fov = g_dc.fov0 + (MfScale(g_dc.fov_t) - g_dc.fov0) * w;     // relative to the player's FOV
    if (w > 0.001f) {
        uint64_t av = 0;
        memcpy(&av, &fov, 4);
        DcCall(MoCamSid("override-fov", FNV64("override-fov")), 1, &av);
    }
    static double logms;
    if (now - logms >= 500.0) {
        logms = now;
        Xlog("downcam: %s w %.2f t %.2f s cam %.2f %.2f %.2f look %.2f %.2f %.2f fov %.1f arm %.2f | down %llu ms cut %.2f s", g_dc.st == 2 ? "hold" : "blend", w, t, g_xcamPos[0], g_xcamPos[1], g_xcamPos[2], g_xcam_look[0], g_xcam_look[1], g_xcam_look[2], fov, g_dc.arm, (unsigned long long) dn_ms, cutf);
    }
}
