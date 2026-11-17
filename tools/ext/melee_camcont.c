// melee_camcont.c - v5 CAMERA CONTINUITY: no single-frame camera cut or whip during a duel (the duel's rules stay unchanged, but it
// must LOOK like SP melee; SP's melee camera never cuts during a fight, it eases between framings). Live clips at ~160 ms RTT:
// 63fdb756 f5-8 / f28-33 a ~90 deg swing in 2-3 frames at engage, 56868857 cuts at f65/66, f77/78, f110, f122, f249-353 and a 3-frame unrelated view at
// f374-377, a4371fab f187-192 / 851ea9ca f66-72 a blurred turn away then a cut at pin start, 851ea9ca f182-184 a cut after the pin escape.
// Included after melee_duel_tl.c. Log prefix "camcont:". Opt-out t2r-v5-no-cam.txt next to the exe (re-read every 2 s): MEASURE ONLY (every jump is
// still logged, nothing is absorbed), so the same build gives the before / after numbers.
//
// Why one guard at the end instead of fixing each source: every camera writer of ours (the duel framing pull-in / re-centre / look-at yaw + pitch
// (melee_spcam.c spm_*), the pinned victim's camera (spc_apply), the takedown / finisher / escape / execution camera (xcam_filter, its hand-back ehb,
// xcam_hard_stop), the down camera (gdn), the hand-over spm -> xcam (g_spm_ho)) switches its weight or its target on some frame, and the engine's own
// camera switches too (melee camera force-melee-camera-target / camera-cancel-aim-override / melee-camera-disable, collision pull-in, mode changes).
// Each of those was fixed one by one before and new cuts kept showing at the seams BETWEEN them. Here the final render camera of
// icam_filter (after every filter of ours, before the 3D-audio listener capture) goes through one continuity filter:
//   OUR offset     = output o inverse(engine camera) (position offset + world-frame rotation). A step of it in one frame above CCG_OUR_DEG (or
//                  CCG_OUR_RATE x dt) / CCG_OUR_M (CCG_OUR_MPS x dt) is a source switch, not motion: the step is moved into a RESIDUAL, so the
//                  displayed camera stays where it was this frame; the residual then springs back to zero (critically damped, omega CCG_OMEGA,
//                  its speed capped at CCG_RES_RATE / CCG_RES_MPS): the switch is shown as a ~0.4-0.6 s ease instead of a cut. Continuous filter motion
//                  under the limits passes untouched (no added lag to the framing / look-at / hit shake).
//   ENGINE camera  inside a duel (mode 2 = engaged, pinned, a duel pair / takedown camera, or < CCG_TAIL_MS after any of them): an engine rotation step
//                  above CCG_ENG_DEG, or an engine position step not explained by our root's motion + the engine's own orbit above CCG_ENG_M, is
//                  absorbed the same way. Outside a duel (mode 1) only glitch-size engine steps (CCG_GLITCH_DEG / CCG_GLITCH_M) are absorbed and the
//                  residual is HELD for CCG_HOLD_FRAMES first: an excursion of the engine camera that comes back within that time (56868857 f375-377)
//                  is never shown; one that stays eases in after the hold.
//   COLLISION      a position step that brings the camera closer to our head by more than 0.10 m (and < CCG_INWARD_MAX) is a collision pull-in: it
//                  passes as is (the lens never goes into a wall), logged as "inward ... passed". Combined with a rotation step > 20 deg it is a
//                  deliberate cut and the rotation passes too.
//   NEVER          the debug free camera, the intro camera (own branch), a respawn / teleport (our root moved > 2 m), outside a match; outside a duel the
//                  2013 AUTHORED execution shots keep their own cuts (xcam_authored).
// Also: in a duel an execution / takedown camera that would CUT in (2013 victim blend-in 0) blends in over CCG_XCAM_IN s (ccg_xcam_blend_in, xcam_start).
// Metrics (pass / fail from the logs): every step above the limits -> one "camcont:" line with what jumped (our filter / engine, deg / m in
// one frame, the limit), what was done (absorbed / held / passed) and the CAUSE (which camera source changed on that frame: xcam on/off + weight, framing
// weight, look-at weight, pin camera weight, down camera, hand-back, hand-over); "camcont: DISPLAYED" when the final output itself still stepped over
// the limit; 'EXCURSION' when an engine jump came back within 8 frames; a 30 s summary with the displayed max deg / m per frame. Pass target in a duel:
// displayed max <= ~6 deg and <= 0.15 m per 60 fps frame, DISPLAYED lines 0 (inward collision passes excepted).
#define CCG_OUR_DEG 8.0f        // deg in one frame (at least; scales with dt above 60 fps frames; the summed gameplay shakes stay under it)
#define CCG_OUR_RATE 480.0f     // deg/s
#define CCG_OUR_M 0.15f         // m in one frame (the contact shake peaks at ~0.12 m / frame)
#define CCG_OUR_MPS 4.0f
#define CCG_ENG_DEG 10.0f       // duel: engine rotation step
#define CCG_ENG_RATE 600.0f
#define CCG_ENG_M 0.25f         // duel: engine position step beyond root motion + orbit
#define CCG_ENG_MPS 4.0f
#define CCG_GLITCH_DEG 35.0f    // outside a duel: only glitch-size engine steps
#define CCG_GLITCH_M 1.2f
#define CCG_HOLD_FRAMES 5       // glitch hold before the ease (an excursion that returns inside it is never seen)
#define CCG_INWARD_MAX 1.2f     // a collision pull-in step larger than this is not a collision
#define CCG_OMEGA 9.0f          // rad/s: residual spring (critically damped, ~0.5 s to settle)
#define CCG_RES_RATE 4.36f      // rad/s = 250 deg/s: residual rotation speed cap
#define CCG_RES_MPS 3.0f        // m/s: residual position speed cap
#define CCG_TAIL_MS 2500        // the duel context lasts this long after the last duel / pin / duel camera frame
#define CCG_XCAM_IN 0.35f       // s: a cutting takedown / execution camera blends in over this inside a duel
#define CCG_SUM_MS 30000
#define CCG_EVN 64
enum { CCG_E_OUR_ROT =
        1, CCG_E_OUR_POS, CCG_E_ENG_ROT, CCG_E_ENG_POS, CCG_E_IN_PASS,
        CCG_E_CUT_PASS, CCG_E_EXCUR, CCG_E_DISP, CCG_E_RESET,
        CCG_E_IN_EASE, CCG_E_WALL };
static const char *const CCG_EN[] =
    { "-", "OUR FILTER rotation", "OUR FILTER position",
"ENGINE CAMERA rotation", "ENGINE CAMERA position",
    "inward step (collision pull-in)", "cut (inward position + rotation)",
        "EXCURSION", "DISPLAYED", "reset",
    "inward step (pull-in, the head-to-lens ray clear)",
        "eased lens INTO A WALL"
};

// the camera sources on a frame (render thread snapshot) - what changed between the last frame and this one is the cause of a jump
typedef struct {
    uint8_t path, xon, auth, ho, gdn, ehb, spok, out;
    float xw, sw, yw, pw;
} CcgSrc;
static struct {
    volatile int mode;          // main thread: 0 = off (pass, reset), 1 = in a match, 2 = duel context
    volatile int absorb;        // 0 = t2r-v5-no-cam.txt: measure only
    volatile float root[3];
    volatile int root_ok;
    // events (render thread writes, main thread drains)
    struct {
        uint64_t frame;
        int kind, done;
        float deg, m, lim_deg, lim_m, res_deg, res_m;
        CcgSrc a, b;
        int mode, frames;
        float rb, ra, sd, ld;
        int clr;
    } ev[CCG_EVN];
    volatile LONG head;
    LONG tail;
    // 30 s metrics (render thread accumulates, main thread logs + clears; tearing is harmless)
    volatile float disp_deg, disp_m, raw_deg, raw_m, res_deg, res_m;
    volatile uint32_t n_frames, n_disp, n_raw, n_our_r, n_our_p, n_eng_r,
        n_eng_p, n_in, n_cut, n_exc, n_hold, n_in_ease, n_wall;
    int nlog, logged_cfg;
    uint64_t sum_at, last_duel;
    volatile float fin_m, fin_deg;
    volatile uint64_t fin_frame;
    volatile int fin_passed;    // v5-cam4: the final camera's max step relative to our body in a duel
} g_ccg;
static volatile float g_ccgTp[3];
static volatile LONG g_ccgTpSeq;
static volatile LONG g_ccgTpN;        // cam-tp: the own-body teleport the camera must follow smoothly (ccg_note_tp, main thread -> ccg_filter, render thread)
static int g_ccgTpOff;        // t2r-cam-tp-old.txt next to the exe (re-read every 2 s): the old behaviour (the camera follows an own-body teleport in one frame)
static float g_ccg_sd, g_ccg_ld;
static int g_ccgClr;           // v5-cam4: the inward step being logged - shown / live lens distance from our root, the ray's verdict
static void CcgEv(int kind, int done, float deg, float m, float ld,
                   float lm, float rd, float rm, const CcgSrc *a,
                   const CcgSrc *b, int frames)
{
    LONG h = g_ccg.head;
    int i = (int) (h & (CCG_EVN - 1));
    g_ccg.ev[i].frame = g_frame;
    g_ccg.ev[i].kind = kind;
    g_ccg.ev[i].done = done;
    g_ccg.ev[i].deg = deg;
    g_ccg.ev[i].m = m;
    g_ccg.ev[i].lim_deg = ld;
    g_ccg.ev[i].lim_m = lm;
    g_ccg.ev[i].res_deg = rd;
    g_ccg.ev[i].res_m = rm;
    g_ccg.ev[i].a = *a;
    g_ccg.ev[i].b = *b;
    g_ccg.ev[i].mode = g_ccg.mode;
    g_ccg.ev[i].frames = frames;
    g_ccg.ev[i].rb = g_spm.ok ? g_spm.bmax : -1.0f;
    g_ccg.ev[i].ra = g_spmRal;
    g_ccg.ev[i].sd = g_ccg_sd;
    g_ccg.ev[i].ld = g_ccg_ld;
    g_ccg.ev[i].clr = g_ccgClr;
    MemoryBarrier();
    g_ccg.head = h + 1;
}

// quaternion helpers (x y z w)
static void CcgQmul(const float *a, const float *b, float *o)
{
    float r[4] =
        { a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
   a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
        a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]
    };
    memcpy(o, r, sizeof r);
}

static void CcgQconj(const float *a, float *o)
{
    o[0] = -a[0];
    o[1] = -a[1];
    o[2] = -a[2];
    o[3] = a[3];
}

static int CcgQnorm(float *q)
{
    float l =
        __builtin_sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] +
                        q[3] * q[3]);
    if (!(l > 0.5f) || !(l < 2.0f))
        return 0;
    for (int i = 0; i < 4; i++)
        q[i] /= l;
    return 1;
}

static float CcgQang(const float *q)
{                               // rotation angle (rad, 0..pi) of a unit quaternion
    float v = __builtin_sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]), w =
        __builtin_fabsf(q[3]);
    return 2.0f * __builtin_atan2f(v, w);
}

static void CcgQ2rv(const float *q0, float *rv)
{
    float q[4] = { q0[0], q0[1], q0[2], q0[3] };
    if (q[3] < 0.0f)
        for (int i = 0; i < 4; i++)
            q[i] = -q[i];
    float v = __builtin_sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
    if (v < 1e-7f) {
        rv[0] = rv[1] = rv[2] = 0.0f;
        return;
    }
    float a = 2.0f * __builtin_atan2f(v, q[3]) / v;
    rv[0] = q[0] * a;
    rv[1] = q[1] * a;
    rv[2] = q[2] * a;
}

static void CcgRv2q(const float *rv, float *q)
{
    float a =
        __builtin_sqrtf(rv[0] * rv[0] + rv[1] * rv[1] + rv[2] * rv[2]);
    if (a < 1e-7f) {
        q[0] = rv[0] * 0.5f;
        q[1] = rv[1] * 0.5f;
        q[2] = rv[2] * 0.5f;
        q[3] = 1.0f;
        CcgQnorm(q);
        return;
    }
    float s = __builtin_sinf(a * 0.5f) / a;
    q[0] = rv[0] * s;
    q[1] = rv[1] * s;
    q[2] = rv[2] * s;
    q[3] = __builtin_cosf(a * 0.5f);
}

static float CcgLen3(const float *v)
{
    return __builtin_sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

// critically damped spring of a 3-vector toward 0, its speed capped (substeps <= 1/120 s)
static void CcgSpring(float *x, float *v, float dt, float vmax)
{
    int n = (int) (dt * 120.0f) + 1;
    if (n > 32)
        n = 32;
    float h = dt / (float) n;
    for (int s = 0; s < n; s++) {
        for (int i = 0; i < 3; i++)
            v[i] +=
                (-2.0f * CCG_OMEGA * v[i] -
                 CCG_OMEGA * CCG_OMEGA * x[i]) * h;
        float vl = CcgLen3(v);
        if (vl > vmax)
            for (int i = 0; i < 3; i++)
                v[i] *= vmax / vl;
        for (int i = 0; i < 3; i++)
            x[i] += v[i] * h;
    }
}

static void CcgSnap(CcgSrc *s, const float *loc, const void *r)
{
    s->path =
        r ==
        (const void *) loc ? 0 : g_xcamOn ? 1 : g_fcam_on ? 2 :
        g_mcamWatch ? 3 : 4;
    s->xon = (uint8_t) (g_xcamOn != 0);
    s->auth = (uint8_t) (g_xcamAuthored != 0);
    s->ho = (uint8_t) (g_spmHo != 0);
    s->gdn = (uint8_t) (g_gdn.on != 0);
    s->ehb = (uint8_t) (g_ehbOn != 0);
    s->spok = (uint8_t) (g_spm.ok != 0);
    s->out = (uint8_t) (g_xcam_out != 0);
    s->xw = g_xcam_w;
    s->sw = g_spm.w;
    s->yw = g_spm.yw;
    s->pw = g_pcam.w;
}

// v5-cam3 (review of v5-int11: camcont 'inward step (collision pull-in) 0.4-0.87 m in 1 frame - passed as is', cam2 max 0.97 m a frame vs SP's 0.10):
// every inward step used to pass as a collision pull-in, also when nothing was behind us - the engine camera's / our framing's own steps toward our head
// (B 13238-13299: the lens followed our body's 0.53 m ping-pong in, out, in ...). An inward step is a cut unless the lens would otherwise be IN a wall.
// The duel framing's head-to-lens ray (melee_spcam.c spm_tick: g_spm.bmax = how far out from our head the lens may stand, SPM_CAM_MARGIN off the first
// surface) says whether the shown spot is still clear: clear = the step is absorbed and eased in (residual spring, <= CCG_RES_MPS); not clear (or no
// ray answer: outside the duel framing) = it passes as before, and an eased lens that a wall reaches is clamped onto the live camera at once.
static int CcgLensClear(const float *w)
{                               // 1 = the world spot w is inside the clear length of the head-to-lens ray
    // (v5-cam4 r3, A/B v5-int17 B 9797: 'framing weight 0.00 -> 0.37' - the old w > 0.5 gate passed every pull-in while the framing faded in; the ray
    //  runs whenever the framing has a partner, whatever its weight)
    float dx = w[0] - g_spm.px, dz = w[2] - g_spm.pz;
    g_ccg_sd = __builtin_sqrtf(dx * dx + dz * dz);
    g_ccgClr = -1;
    if (!g_spm.ok || !(g_spm.bmax > 0.0f))
        return 0;
    g_ccgClr = g_ccg_sd <= g_spm.bmax + 0.5f * SPM_CAM_MARGIN;
    return g_ccgClr;
}

// render thread, icam_filter (non-intro branch), for the main view (k 0, m + 0x130) and its second copy (k 1, m + 0x550): r = the camera after all our
// filters, loc = the engine's. Returns r or a continuity-corrected copy.
// Under the 2013 execution cameras the image tilted 20-45 deg, so the shown execution camera's
// roll (its right axis out of the horizontal, about the view axis) is clamped to +-S41_ROLL_MAX. Render thread; r = the camera the chain shows.
#define S41_ROLL_MAX 0.1745f    // rad = 10 deg
static void *S41Roll(int k, void *r)
{
    static float buf[2][32] __attribute__((aligned(16)));
    if (!g_ps41x || k < 0 || k > 1 || !r)
        return r;
    const float *U = (const float *) r;
    float q[4] = { U[4], U[5], U[6], U[7] };
    if (!CcgQnorm(q))
        return r;
    static const float Z[3] = { 0, 0, 1 }, X[3] = { 1, 0, 0 };
    float f[3], rg[3];
    QRot(q, Z, f);
    QRot(q, X, rg);
    float hl = __builtin_sqrtf(f[0] * f[0] + f[2] * f[2]);
    if (!(hl > 0.43f))
        return r;               // steeper than ~65 deg: no horizon to hold
    float r0[3] = { f[2] / hl, 0.0f, -f[0] / hl };
    if (r0[0] * rg[0] + r0[2] * rg[2] < 0.0f) {
        r0[0] = -r0[0];
        r0[2] = -r0[2];
    }
    float cx[3] =
        { r0[1] * rg[2] - r0[2] * rg[1], r0[2] * rg[0] - r0[0] * rg[2],
  r0[0] * rg[1] - r0[1] * rg[0] };
    float ph =
        __builtin_atan2f(cx[0] * f[0] + cx[1] * f[1] + cx[2] * f[2],
                         r0[0] * rg[0] + r0[1] * rg[1] + r0[2] * rg[2]);
    if (!(ph == ph) || __builtin_fabsf(ph) <= S41_ROLL_MAX)
        return r;
    float ex = ph - (ph > 0.0f ? S41_ROLL_MAX : -S41_ROLL_MAX), best[4] =
        { q[0], q[1], q[2], q[3] }, bd = 1e9f;
    for (int s = -1; s <= 1; s += 2) {  // (both senses tried, the one that lands on the limit kept: no convention to get wrong)
        float h = 0.5f * ex * (float) s, sn = __builtin_sinf(h), qr[4] =
            { f[0] * sn, f[1] * sn, f[2] * sn, __builtin_cosf(h) }, o[4],
            g2[3];
        CcgQmul(qr, q, o);
        if (!CcgQnorm(o))
            continue;
        QRot(o, X, g2);
        float c2[3] =
            { r0[1] * g2[2] - r0[2] * g2[1], r0[2] * g2[0] - r0[0] * g2[2],
      r0[0] * g2[1] - r0[1] * g2[0] };
        float p2 =
            __builtin_atan2f(c2[0] * f[0] + c2[1] * f[1] + c2[2] * f[2],
                             r0[0] * g2[0] + r0[1] * g2[1] +
                             r0[2] * g2[2]);
        float d = __builtin_fabsf(__builtin_fabsf(p2) - S41_ROLL_MAX);
        if (d < bd) {
            bd = d;
            memcpy(best, o, sizeof best);
        }
    }
    if (bd > 0.05f)
        return r;
    memcpy(buf[k], r, sizeof buf[k]);
    for (int i = 0; i < 4; i++)
        buf[k][4 + i] = best[i];
    if (k == 0) {
        InterlockedIncrement(&g_s41r.n);
        float dg = __builtin_fabsf(ph) * 57.29578f;
        if (dg > g_s41r.mx)
            g_s41r.mx = dg;
    }
    return buf[k];
}

static void *CcgFilter(uint8_t *dst, const float *loc, void *r)
{
    static float buf[2][32] __attribute__((aligned(16)));
    // per view: last frame's engine camera (ep / eq), our offset (tp / tq), raw output (up / uq), displayed output (dp / dq), root; the residual (rp / rr) + its
    // velocity (vp / vr); the glitch hold; the excursion watch (the engine pose before a glitch-size jump)
    static struct {
        int ok, hold, exc, exc_n;
        double t;
        float ep[3], eq[4], tp[3], tq[4], up[3], uq[4], root[3], rp[3],
            vp[3], rr[3], vr[3], dp[3], dq[4], exp_[3], exq[4], exroot[3],
            tpd[3], rin[3];
        int tpn;
        LONG tpseq;
        CcgSrc src;
    } S[2];
    uint8_t *m = IcamManager();
    int k = !m ? -1 : dst == m + 0x130 ? 0 : dst == m + 0x550 ? 1 : -1;
    if (k < 0 || !r || IsBadReadPtr(loc, 0x80) || IsBadReadPtr(r, 0x80))
        return r;
    int mode = g_ccg.mode;
    // The victim's finisher shot turned 12-20 deg a frame for 0.3 s (its own timed blend). This filter took that as a source switch (log: 'camcont: ... OUR FILTER rotation
    // jump 9.2 / 12.8 / 17.3 / 20.5 / 20.2 / 21.0 / 69.0 deg ... absorbed', 'residual 151.5 deg / 0.95 m', and a frame check warning that both heads stood BEHIND the lens;
    // also 'cam2: duel framing -> pin camera ... blended over 450 ms', 'lensguard: the lens is pulled in ... clear length 0.22 m from the head', 'downlens: lens pulled in
    // front of a wall ... max 1.07 m'), kept the old view and paid the 150 deg back over the next second, cam2 blended the same switch a second time, and the two lens guards
    // pulled the shot's lens toward a HEAD point that stays 1.45 m above the root of a body lying on the floor. The victim's shot (melee_spcam.c s34_apply) is one timed
    // owner with its own rays: all four stages pass it through and restart from the game camera when it has eased back onto it.
    if (!mode || g_fcam_on || (g_s34Tb && g_pcam.w > 0.001f)
        || S40ExecCam() || S45sSwing()) {
        S[k].ok = 0;
        return S40ExecCam()? S41Roll(k, r) : r;
    }                           // (ps45 S1: the rig's timed side swing - melee_spcam.c PS45_SIDE)      // (ps40 E5: nor a 2013 execution camera - melee_spcam.c spm_tick's line)
    // ps39 C7 (melee_spcam.c spm25_rig): the rig took its spot on one cut (g_s37_gen bumped on the render thread, before this stage) - not a jump to
    // absorb and pay back over 0.5 s through our own body: this stage restarts on it, like cam2_filter does
    {
        static LONG g39[2];
        if (g_ps37 && g39[k] != g_s37Gen) {
            g39[k] = g_s37Gen;
            S[k].ok = 0;
        }
    }
    const float *U = (const float *) r;
    float Ep[3] = { loc[0], loc[1], loc[2] }, Eq[4] =
        { loc[4], loc[5], loc[6], loc[7] }, Up[3] =
        { U[0], U[1], U[2] }, Uq[4] = { U[4], U[5], U[6], U[7] };
    if (!CcgQnorm(Eq) || !CcgQnorm(Uq))
        return r;
    for (int i = 0; i < 3; i++)
        if (!(__builtin_fabsf(Ep[i]) < 1e6f)
            || !(__builtin_fabsf(Up[i]) < 1e6f))
            return r;
    float root[3] = { g_ccg.root[0], g_ccg.root[1] + 1.5f, g_ccg.root[2] };
    int rok = g_ccg.root_ok;    // pivot: our head height
    // ps23 (melee_spcam.c spm_slew): the reference of the two FINAL-camera tests below (the inward limit, the 'relative to our body' metric). With the duel
    // framing on, the lens stands off the framing's eased pivot, not off the raw root - measured against the raw root, a one-frame step of our body TOWARD the
    // lens read as a pull-in of that size and the inward limit pushed the lens back out after the root (the step back on screen).
    float rin[3] = { root[0], root[1], root[2] };
    if (g_ps23Piv && g_spm.ok) {
        float qx = g_spm.px, qz = g_spm.pz;
        if (qx == qx && qz == qz
            && (qx - root[0]) * (qx - root[0]) + (qz - root[2]) * (qz -
                                                                   root[2])
            < 1.0f) {
            rin[0] = qx;
            rin[2] = qz;
        }
    }
    // our offset this frame
    float dp[3] =
        { Up[0] - Ep[0], Up[1] - Ep[1], Up[2] - Ep[2] }, eqc[4], dq[4];
    CcgQconj(Eq, eqc);
    CcgQmul(Uq, eqc, dq);
    CcgQnorm(dq);
    double now = GshNow();
    float dt = S[k].ok ? (float) (now - S[k].t) : 0.0f;
    CcgSrc cs;
    CcgSnap(&cs, loc, r);
    int reset = !S[k].ok || dt > 0.25f || dt < 0.0f;
    if (!reset && rok) {
        float rd[3] =
            { root[0] - S[k].root[0], root[1] - S[k].root[1],
      root[2] - S[k].root[2] };
        if (CcgLen3(rd) > 2.0f) {
            reset = 1;
            if (k == 0)
                CcgEv(CCG_E_RESET, 0, 0, CcgLen3(rd), 0, 2.0f, 0, 0,
                       &S[k].src, &cs, 0);
        }
    }
    if (reset) {
        memset(&S[k], 0, sizeof S[k]);
        S[k].ok = 1;
        S[k].t = now;
        S[k].tpseq = g_ccgTpSeq;
        memcpy(S[k].ep, Ep, 12);
        memcpy(S[k].eq, Eq, 16);
        memcpy(S[k].tp, dp, 12);
        memcpy(S[k].tq, dq, 16);
        memcpy(S[k].root, root, 12);
        S[k].src = cs;
        memcpy(S[k].dp, Up, 12);
        memcpy(S[k].dq, Uq, 16);
        memcpy(S[k].up, Up, 12);
        memcpy(S[k].uq, Uq, 16);
        memcpy(S[k].rin, rin, 12);
        return r;
    }
    if (dt < 0.0005f)
        goto apply;             // another copy of the same frame's camera: the same correction
    S[k].t = now;
    {
        int own41 = mode == 2 && S41RigOwner(), absorb = g_ccg.absorb
            && !own41, duel = mode == 2, authored = g_xcamOn
            && g_xcamAuthored && !duel;
        float f60 = dt * 60.0f;
        if (f60 < 1.0f)
            f60 = 1.0f;
        float limOd = fmaxf(CCG_OUR_DEG, CCG_OUR_RATE * dt), limOm =
            fmaxf(CCG_OUR_M, CCG_OUR_MPS * dt);
        float limEd =
            duel ? fmaxf(CCG_ENG_DEG,
                         CCG_ENG_RATE * dt) : CCG_GLITCH_DEG * f60, limEm =
            duel ? fmaxf(CCG_ENG_M, CCG_ENG_MPS * dt) : CCG_GLITCH_M * f60;
        // ---- attribution: the step of OUR offset (output o inverse(engine)) this frame
        if (dq[0] * S[k].tq[0] + dq[1] * S[k].tq[1] + dq[2] * S[k].tq[2] +
            dq[3] * S[k].tq[3] < 0.0f)
            for (int i = 0; i < 4; i++)
                dq[i] = -dq[i];
        float tqc[4], so[4];
        CcgQconj(S[k].tq, tqc);
        CcgQmul(dq, tqc, so);
        float aO = CcgQang(so) * 57.2957795f;
        // ---- the engine camera's step: rotation, and position beyond our root's motion + its own orbit about our head
        float epc[4], se[4];
        CcgQconj(S[k].eq, epc);
        CcgQmul(Eq, epc, se);
        CcgQnorm(se);
        float aE = CcgQang(se) * 57.2957795f;
        float off0[3] =
            { S[k].ep[0] - S[k].root[0], S[k].ep[1] - S[k].root[1],
    S[k].ep[2] - S[k].root[2] }, offr[3];
        QRot(se, off0, offr);
        float ue[3], de[3], rdl[3];
        for (int i = 0; i < 3; i++) {
            ue[i] = Ep[i] - (root[i] + offr[i]);
            de[i] = Ep[i] - S[k].ep[i];
            rdl[i] = rok ? root[i] - S[k].root[i] : 0.0f;
        }
        float mE = rok ? CcgLen3(ue) : 0.0f;
        int inward_e; {
            float c[3], e[3];
            for (int i = 0; i < 3; i++) {
                c[i] = S[k].ep[i] - S[k].root[i];
                e[i] = Ep[i] - root[i];
            } inward_e = rok && CcgLen3(e) < CcgLen3(c) - 0.10f;
        }
        int in_exc = S[k].exc > 0;      // inside an engine excursion's window: its way back is absorbed whatever its direction
        // the engine's motion is ALLOWED motion (walking, orbit, mouse, a collision pull-in) unless it is a jump: then only our root's motion is
        int eng_r_ok = aE <= limEd, eng_p_ok = mE <= limEm || (inward_e
                                                               && mE <
                                                               CCG_INWARD_MAX
                                                               && !in_exc);
        int eng_in_ease = 0;
        if (duel && absorb && eng_r_ok && eng_p_ok && mE > limEm && inward_e) { // v5-cam3: an engine pull-in with the shown spot still clear is eased
            float sh[3];
            for (int i = 0; i < 3; i++)
                sh[i] = S[k].dp[i] + (rok ? root[i] - S[k].root[i] : 0.0f);
            if (CcgLensClear(sh)) {
                eng_p_ok = 0;
                eng_in_ease = 1;
            }
        }
        if (!eng_r_ok)
            eng_p_ok = 0;       // an engine cut: its position goes with its rotation
        int eng_jump = !eng_r_ok || !eng_p_ok, glitch = eng_jump && !duel;
        // (v5-cam4 r3: an engine pull-in that passes here is judged by the final inward limit below, which logs it with the ray's answer)
        if (eng_in_ease && k == 0) {
            g_ccg.n_in_ease++;
            CcgEv(CCG_E_IN_EASE, 1, aE, mE, limEd, limEm, 0, 0, &S[k].src,
                   &cs, 0);
        }
        float sa[4] = { 0, 0, 0, 1 }, ma[3];
        if (eng_r_ok)
            memcpy(sa, se, 16);
        for (int i = 0; i < 3; i++)
            ma[i] = eng_p_ok ? de[i] : rdl[i];
        // ---- the raw output's step beyond the allowed motion
        float uq[4] = { Uq[0], Uq[1], Uq[2], Uq[3] };
        if (uq[0] * S[k].uq[0] + uq[1] * S[k].uq[1] + uq[2] * S[k].uq[2] +
            uq[3] * S[k].uq[3] < 0.0f)
            for (int i = 0; i < 4; i++)
                uq[i] = -uq[i];
        // allowed = following the engine camera's allowed motion OR holding still (an override of ours - exec / takedown / pin camera, the full look-at - does
        // not follow the engine): the smaller excess counts, and its reference is the one the absorbed residual keeps
        float upc[4], su[4], sac[4], sx[4], mX[3], mX2[3];
        CcgQconj(S[k].uq, upc);
        CcgQmul(uq, upc, su);
        CcgQconj(sa, sac);
        CcgQmul(su, sac, sx);
        float aX = CcgQang(sx) * 57.2957795f, aS =
            CcgQang(su) * 57.2957795f;
        if (aS < aX) {
            aX = aS;
            sa[0] = sa[1] = sa[2] = 0.0f;
            sa[3] = 1.0f;
            CcgQconj(sa, sac);
        }
        for (int i = 0; i < 3; i++) {
            mX[i] = (Up[i] - S[k].up[i]) - ma[i];
            mX2[i] = (Up[i] - S[k].up[i]) - rdl[i];
        }
        float mXl = CcgLen3(mX);
        if (CcgLen3(mX2) < mXl) {
            mXl = CcgLen3(mX2);
            memcpy(mX, mX2, 12);
            memcpy(ma, rdl, 12);
        }
        // ps23 (melee_spcam.c spm_slew): the framing's lens no longer rides a one-frame step of our root. Position had no 'holding still' reference (rotation
        // has one above): a lens that stayed in the world while the root / the engine camera stepped 0.3-0.9 m read as OUR jump of that size and was
        // 'absorbed' = pushed after the root in that same frame. The output's own world step is the third reference; a step under the limit is no jump.
        if (g_ps23Piv) {
            float mX3[3] =
                { Up[0] - S[k].up[0], Up[1] - S[k].up[1],
         Up[2] - S[k].up[2] };
            if (CcgLen3(mX3) < mXl) {
                mXl = CcgLen3(mX3);
                memcpy(mX, mX3, 12);
                memset(ma, 0, 12);
            }
        }
        int absR = aX > limOd, absP = mXl > limOm;
        if (absR || absP) {
            g_ccg.n_raw++;
            if (k == 0) {
                if (aX > g_ccg.raw_deg)
                    g_ccg.raw_deg = aX;
                if (mXl > g_ccg.raw_m)
                    g_ccg.raw_m = mXl;
            }
        }
        int passed = 0, in_ease = eng_in_ease;
        if (authored) {
            absR = absP = 0;
        }                       // outside a duel the 2013 authored execution shots keep their own cuts
        else if (absP && !eng_jump && !in_exc && mXl < CCG_INWARD_MAX) {
            // a step of ours toward our head = a collision pull-in (the pin camera's / hand-back's ray, the framing's 1.1 m clearance): passes as is
            float a[3], b[3];
            for (int i = 0; i < 3; i++) {
                a[i] = S[k].up[i] + ma[i] - root[i];
                b[i] = Up[i] - root[i];
            }
            float al = CcgLen3(a), along =
                al >
                0.05f ? -(mX[0] * a[0] + mX[1] * a[1] +
                          mX[2] * a[2]) / al : 0.0f;
            float shw[3];
            for (int i = 0; i < 3; i++)
                shw[i] = S[k].dp[i] + ma[i];
            int clear = duel && absorb && aX <= 20.0f && CcgLensClear(shw);   // v5-cam3: the shown spot is clear - ease the pull-in (absorbed below)
            if (clear && rok && CcgLen3(b) < al - 0.10f
                && along > 0.7f * mXl) {
                in_ease = 1;
                if (k == 0) {
                    g_ccg.n_in_ease++;
                    CcgEv(CCG_E_IN_EASE, 1, aX, mXl, limOd, limOm, 0, 0,
                           &S[k].src, &cs, 0);
                }
            } else if (rok && CcgLen3(b) < al - 0.10f && along > 0.7f * mXl) { // (mostly along the line to our head: a lateral blend is not a pull-in)
                passed = 1;
                absP = 0;
                if (aX > 20.0f) {
                    absR = 0;
                    if (k == 0) {
                        g_ccg.n_cut++;
                        CcgEv(CCG_E_CUT_PASS, 0, aX, mXl, limOd, limOm, 0,
                               0, &S[k].src, &cs, 0);
                    }
                }
                // (v5-cam4 r3: judged + logged by the final inward limit below)
            }
        }
        // excursion: the engine camera came back to where it was before a glitch-size jump
        if (S[k].exc > 0) {
            S[k].exc_n++;
            float bq[4], sb[4];
            CcgQconj(S[k].exq, bq);
            CcgQmul(Eq, bq, sb);
            CcgQnorm(sb);
            float back[3];
            for (int i = 0; i < 3; i++)
                back[i] =
                    Ep[i] - S[k].exp_[i] - (root[i] - S[k].exroot[i]);
            if (CcgQang(sb) * 57.2957795f < 15.0f
                && CcgLen3(back) < 0.5f) {
                if (k == 0) {
                    g_ccg.n_exc++;
                    CcgEv(CCG_E_EXCUR, absorb, aE, mE, 0, 0, 0, 0,
                           &S[k].src, &cs, S[k].exc_n);
                }
                S[k].exc = 0;
            } else if (--S[k].exc <= 0)
                S[k].exc = 0;
        }
        if (glitch && !S[k].exc) {
            memcpy(S[k].exp_, S[k].ep, 12);
            memcpy(S[k].exq, S[k].eq, 16);
            memcpy(S[k].exroot, S[k].root, 12);
            S[k].exc = 8;
            S[k].exc_n = 0;
        }
        if (absR || absP) {
            if (k == 0 && !in_ease) {
                int kind =
                    eng_jump ? (!eng_r_ok ? CCG_E_ENG_ROT : CCG_E_ENG_POS)
                    : (absR ? CCG_E_OUR_ROT : CCG_E_OUR_POS);
                CcgEv(kind, absorb | (glitch ? 2 : 0), eng_jump ? aE : aX,
                       eng_jump ? mE : mXl, eng_jump ? limEd : limOd,
                       eng_jump ? limEm : limOm,
                       CcgLen3(S[k].rr) * 57.2957795f, CcgLen3(S[k].rp),
                       &S[k].src, &cs, 0);
                if (eng_jump) {
                    g_ccg.n_eng_r += absR;
                    g_ccg.n_eng_p += absP;
                } else {
                    g_ccg.n_our_r += absR;
                    g_ccg.n_our_p += absP;
                }
                (void) aO;
            }
            if (absorb) {
                // displayed now = R' * U must equal (allowed motion) applied to last frame's displayed = sa * R * U_prev (world frame); position likewise
                if (absR) {
                    float R[4], t1[4], t2[4], ucq[4], Rn[4];
                    CcgRv2q(S[k].rr, R);
                    CcgQmul(sa, R, t1);
                    CcgQmul(t1, S[k].uq, t2);
                    CcgQconj(uq, ucq);
                    CcgQmul(t2, ucq, Rn);
                    CcgQnorm(Rn);
                    CcgQ2rv(Rn, S[k].rr);
                }
                if (absP)
                    for (int i = 0; i < 3; i++)
                        S[k].rp[i] -= mX[i];
                if (glitch) {
                    S[k].hold = CCG_HOLD_FRAMES;
                    if (k == 0)
                        g_ccg.n_hold++;
                }
                if (CcgLen3(S[k].rp) > 4.0f) {
                    memset(S[k].rp, 0, 12);
                    memset(S[k].vp, 0, 12);
                }               // nothing to ease from that far
            }
        }
        // cam-tp (the late-dodge camera cut = the release sync's teleport-player onto the dodge's end spot): the engine camera
        // follows a teleported body at once, so the shown view jumped by the body's step in one frame. The step (ccg_note_tp, the choke point teleport_player_to)
        // is moved into the residual as the engine camera takes it (its measured step along the teleport, up to 8 frames), so the shown camera stays and then
        // eases onto the body over ~0.5 s (the residual spring, <= CCG_RES_MPS): a smooth follow, never a cut. Duel context only; t2r-cam-tp-old.txt = off.
        if (duel && absorb && !g_ccgTpOff) {
            LONG sq = g_ccgTpSeq;
            if (sq != S[k].tpseq) {
                S[k].tpseq = sq;
                for (int i = 0; i < 3; i++)
                    S[k].tpd[i] += g_ccgTp[i];
                S[k].tpn = 8;
            }
            if (S[k].tpn > 0) {
                S[k].tpn--;
                float dl2 =
                    S[k].tpd[0] * S[k].tpd[0] + S[k].tpd[1] * S[k].tpd[1] +
                    S[k].tpd[2] * S[k].tpd[2];
                if (dl2 > 1e-6f) {
                    // (ps23: measured on the raw OUTPUT's step - with the framing pivot eased (melee_spcam.c spm_slew) the lens takes only part of the engine
                    //  camera's step, and holding back the engine's whole step would move the shown lens BACKWARD by the difference; the game camera = the same value)
                    float os[3] =
                        { g_ps23Piv ? Up[0] - S[k].up[0] : de[0],
                  g_ps23Piv ? Up[1] - S[k].up[1] : de[1],
                  g_ps23Piv ? Up[2] - S[k].up[2] : de[2] };
                    float f =
                        (os[0] * S[k].tpd[0] + os[1] * S[k].tpd[1] +
                         os[2] * S[k].tpd[2]) / dl2;
                    f = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
                    if (f > 0.02f) {
                        for (int i = 0; i < 3; i++) {
                            float a = S[k].tpd[i] * f;
                            S[k].rp[i] -= a;
                            S[k].tpd[i] -= a;
                        } if (k == 0)
                            g_ccgTpN++;
                    }
                } else
                    S[k].tpn = 0;
                if (S[k].tpn == 0)
                    memset(S[k].tpd, 0, 12);
            } else
                memset(S[k].tpd, 0, 12);
        }
        // the residual eases back (held first after a glitch-size engine jump)
        if (S[k].hold > 0) {
            S[k].hold--;
            memset(S[k].vp, 0, 12);
            memset(S[k].vr, 0, 12);
        } else {
            CcgSpring(S[k].rr, S[k].vr, dt, CCG_RES_RATE);
            CcgSpring(S[k].rp, S[k].vp, dt, CCG_RES_MPS);
            if (CcgLen3(S[k].rr) < 0.0005f && CcgLen3(S[k].vr) < 0.01f) {
                memset(S[k].rr, 0, 12);
                memset(S[k].vr, 0, 12);
            }
            if (CcgLen3(S[k].rp) < 0.0005f && CcgLen3(S[k].vp) < 0.01f) {
                memset(S[k].rp, 0, 12);
                memset(S[k].vp, 0, 12);
            }
        }
        if (own41 && k == 0)
            InterlockedIncrement(&g_s41.n_ccg); // ps41 step 0: the rig owns the lens - nothing new absorbed, a running pay-back ends
        if (!g_ccg.absorb) {
            memset(S[k].rr, 0, 12);
            memset(S[k].vr, 0, 12);
            memset(S[k].rp, 0, 12);
            memset(S[k].vp, 0, 12);
        }
        // v5-cam3: an eased position residual that holds the lens farther out than the live camera is clamped onto it the moment the head-to-lens ray
        // says that spot is no longer clear (a wall came in): the lens never stands in a wall
        if (duel && rok
            && (S[k].rp[0] != 0.0f || S[k].rp[1] != 0.0f
                || S[k].rp[2] != 0.0f) && g_spm.ok && g_spm.bmax > 0.0f) {
            float shw[3], a[3], b[3];
            for (int i = 0; i < 3; i++) {
                shw[i] = Up[i] + S[k].rp[i];
                a[i] = shw[i] - root[i];
                b[i] = Up[i] - root[i];
            }
            if (CcgLen3(a) > CcgLen3(b) + 0.02f && !CcgLensClear(shw)) {
                if (k == 0) {
                    g_ccg.n_wall++;
                    CcgEv(CCG_E_WALL, 0, 0.0f, CcgLen3(S[k].rp), 0, 0, 0,
                           0, &S[k].src, &cs, 0);
                }
                memset(S[k].rp, 0, 12);
                memset(S[k].vp, 0, 12);
            }
        }
        // v5-cam4 r3 (A/B v5-int17 B: FINAL 0.28 / 0.41 / 0.43 / 0.97 m pull-ins in one frame, 'inward step (collision pull-in) ... passed as is' on paths the
        // per-branch ease never saw: B 26494 the engine collapsed 1.06 m and its collapse counted as ALLOWED motion, so absorbing our framing's hold-out
        // still left the shown lens following the engine in; 19771 a pin camera switch cam2 does not blend when it is inward; 9797 the framing fading in
        // (weight 0.37, the old w > 0.5 gate); 19021 / 21242 the engine's step in the duel framing). One rule on the FINAL camera instead of per source:
        // in a duel the shown lens comes in toward our head by at most 0.10 m per 60 fps frame (SP's max), unless the head-to-lens ray says the spot it
        // would stay on is no longer clear (inside a surface) - then it cuts, and the line logs the ray's answer.
        if (duel && rok && absorb) {
            float a[3], b[3], pw[3];
            for (int i = 0; i < 3; i++) {
                a[i] = Up[i] + S[k].rp[i] - rin[i];
                b[i] = S[k].dp[i] - S[k].rin[i];
                pw[i] = rin[i] + b[i];
            }                   // (ps23: rin = the framing's eased pivot, else our root)
            float la = CcgLen3(a), lb = CcgLen3(b), lim = 6.0f * dt;
            if (lim < 0.10f)
                lim = 0.10f;
            if (lb > 0.2f && la > 0.05f && la < lb - lim) {
                float st_m = lb - la;
                g_ccg_ld = la;
                if (CcgLensClear(pw)) {
                    float f = (lb - lim) / la;
                    for (int i = 0; i < 3; i++)
                        S[k].rp[i] += a[i] * (f - 1.0f);
                    if (k == 0) {
                        g_ccg.n_in_ease++;
                        if (st_m > 0.15f)
                            CcgEv(CCG_E_IN_EASE, 1, 0.0f, st_m, 0.0f, lim,
                                   0, 0, &S[k].src, &cs, 0);
                    }
                } else if (k == 0 && st_m > 0.15f) {
                    g_ccg.n_in++;
                    CcgEv(CCG_E_IN_PASS, 0, 0.0f, st_m, 0.0f, lim, 0, 0,
                           &S[k].src, &cs, 0);
                }
            }
        }
        // the displayed result vs last frame's, beyond the allowed motion: the metric
        float Rq[4], oq[4], op[3];
        CcgRv2q(S[k].rr, Rq);
        CcgQmul(Rq, uq, oq);
        CcgQnorm(oq);
        for (int i = 0; i < 3; i++)
            op[i] = Up[i] + S[k].rp[i];
        if (k == 0) {
            float q2[4] =
                { oq[0], oq[1], oq[2], oq[3] }, pc[4], s1[4], s2[4], mv[3];
            if (q2[0] * S[k].dq[0] + q2[1] * S[k].dq[1] +
                q2[2] * S[k].dq[2] + q2[3] * S[k].dq[3] < 0.0f)
                for (int i = 0; i < 4; i++)
                    q2[i] = -q2[i];
            CcgQconj(S[k].dq, pc);
            CcgQmul(q2, pc, s1);
            CcgQmul(s1, sac, s2);
            for (int i = 0; i < 3; i++)
                mv[i] = (op[i] - S[k].dp[i]) - ma[i];
            float ad_ex = CcgQang(s2) * 57.2957795f, dm_ex = CcgLen3(mv);
            g_ccg.n_frames++;
            if (ad_ex > g_ccg.disp_deg)
                g_ccg.disp_deg = ad_ex;
            if (!passed && dm_ex > g_ccg.disp_m)
                g_ccg.disp_m = dm_ex;
            // v5-cam4 (A/B v5-int13: cam2's 'shown in a duel max 1.19 m per frame' with no camcont line explaining it - cam2 measures BEFORE this
            // filter, and the step metric above leaves out the engine's allowed motion and passed pull-ins): the FINAL camera's own step per frame
            // relative to our body (offset from our root, rotation), every kind of step included, with its frame - what the screen really shows
            if (duel && rok) {
                float a[3], b[3];
                for (int i = 0; i < 3; i++) {
                    a[i] = op[i] - rin[i];
                    b[i] = S[k].dp[i] - S[k].rin[i];
                }               // (ps23: rin, as above)
                float dv[3] =
                    { a[0] - b[0], a[1] - b[1], a[2] - b[2] }, fm =
                    CcgLen3(dv) / f60, q3[4] =
                    { oq[0], oq[1], oq[2], oq[3] }, qc[4], qd[4];
                CcgQconj(S[k].dq, qc);
                CcgQmul(q3, qc, qd);
                float fd = CcgQang(qd) * 57.2957795f / f60;
                if (fd > 90.0f)
                    fd = 180.0f - fd;   // (q / -q)
                if (fm + 0.1f * fd > g_ccg.fin_m + 0.1f * g_ccg.fin_deg) {
                    g_ccg.fin_m = fm;
                    g_ccg.fin_deg = fd;
                    g_ccg.fin_frame = g_frame;
                    g_ccg.fin_passed = passed || (eng_p_ok && mE > limEm);
                }
            }
            float rd = CcgLen3(S[k].rr) * 57.2957795f, rm =
                CcgLen3(S[k].rp);
            if (rd > g_ccg.res_deg)
                g_ccg.res_deg = rd;
            if (rm > g_ccg.res_m)
                g_ccg.res_m = rm;
            if (ad_ex > limOd || (!passed && dm_ex > limOm)) {
                g_ccg.n_disp++;
                CcgEv(CCG_E_DISP, 0, ad_ex, dm_ex, limOd, limOm, rd, rm,
                       &S[k].src, &cs, 0);
            }
        }
        memcpy(S[k].ep, Ep, 12);
        memcpy(S[k].eq, Eq, 16);
        memcpy(S[k].tp, dp, 12);
        memcpy(S[k].tq, dq, 16);
        if (rok)
            memcpy(S[k].root, root, 12);
        memcpy(S[k].up, Up, 12);
        memcpy(S[k].uq, uq, 16);
        memcpy(S[k].dp, op, 12);
        memcpy(S[k].dq, oq, 16);
        S[k].src = cs;
        memcpy(S[k].rin, rin, 12);
    }
  apply:
    if (S[k].rr[0] == 0.0f && S[k].rr[1] == 0.0f && S[k].rr[2] == 0.0f
        && S[k].rp[0] == 0.0f && S[k].rp[1] == 0.0f && S[k].rp[2] == 0.0f)
        return r;
    {
        float Rq[4], oq[4];
        CcgRv2q(S[k].rr, Rq);
        CcgQmul(Rq, Uq, oq);
        if (!CcgQnorm(oq))
            return r;
        memcpy(buf[k], U, sizeof buf[k]);
        for (int i = 0; i < 3; i++) {
            float v = Up[i] + S[k].rp[i];
            if (!(__builtin_fabsf(v) < 1e6f))
                return r;
            buf[k][i] = v;
        }
        for (int i = 0; i < 4; i++)
            buf[k][4 + i] = oq[i];
        return buf[k];
    }
}

// ---- main thread
static int CcgOff(void)
{
    static int v = -1;
    static uint64_t at;
    uint64_t now = GetTickCount64();
    if (v < 0 || now - at >= 2000) {
        at = now;
        FILE *f = ExeSideOpen("t2r-v5-no-cam.txt", "r");
        int o = f != NULL;
        if (f)
            fclose(f);
        if (o != v)
            Xlog("camcont: %s",
                 o ?
                 "MEASURE ONLY (t2r-v5-no-cam.txt): camera jumps are logged, none absorbed"
                 :
                 "ON - v5 camera continuity: our filters' switches and the engine camera's jumps in a duel ease over ~0.5 s instead of cutting (collision pull-ins pass); 'camcont:' lines measure every jump");
        v = o;
    }
    return v;
}

// main thread, teleport_player_to (t2r-ext.c, before the event is sent: our locator still holds the old spot): an own-body teleport in a duel context is a
// step the camera must ease over, not show (ccg_filter 'cam-tp'). Steps < 3 cm are in-place resets; > 2 m are respawns / real relocations (ccg resets there).
static void CcgNoteTp(const float *pos)
{
    static uint64_t at;
    static float last[3];
    static uint64_t last_fr;
    uint64_t now = GetTickCount64();
    if (!at || now - at >= 2000) {
        at = now;
        FILE *f = ExeSideOpen("t2r-cam-tp-old.txt", "r");
        g_ccgTpOff = f != NULL;
        if (f)
            fclose(f);
    }
    if (g_ccgTpOff || g_ccg.mode != 2 || !pos)
        return;
    float pb[16];
    const float *pl = PlayerLocator(pb);
    if (!pl)
        return;
    float from[3] = { pl[0], pl[1], pl[2] };
    if (g_frame && g_frame == last_fr)
        memcpy(from, last, 12); // (two teleports in one frame: the engine has not moved the body yet - chain them)
    float d[3] = { pos[0] - from[0], pos[1] - from[1], pos[2] - from[2] };
    last[0] = pos[0];
    last[1] = pos[1];
    last[2] = pos[2];
    last_fr = g_frame;
    float l = __builtin_sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(l > 0.03f) || !(l < 2.0f))
        return;
    for (int i = 0; i < 3; i++)
        g_ccgTp[i] = d[i];
    MemoryBarrier();
    InterlockedIncrement(&g_ccgTpSeq);
    static int nl;
    if (LOGCAP(nl, 80))
        Xlog("camcont: own-body teleport %.2f m (frame %llu, from t2r-ext.dll+%llx) - the camera follows it eased over ~0.5 s, no cut", (double) l, (unsigned long long) g_frame, (unsigned long long) ((uintptr_t) g_tpRa & 0xfffff));
}

static int CcgDuelNow(void)
{
    return MduEngaged() || MpnPinnedNow() || MduInPair() || g_mqCam || g_pcam.w > 0.001f || g_spmHo || g_s32.on; // (ps32: a finisher on the duel rig is duel context to its end - the duel itself ends 2 frames into it, rec21 A 14252)
}

// v5-cam7 (log 44010 'DUEL: u5785 ENGAGE ... (their swing)' then 'END ... after 0 ms' and the takedown 16 frames later ran
// 'outside a duel': its authored victim shot cut in, camcont / cam2 stood aside): an engage that ended within the same tick never reached
// ccg_tick's duel test. A recent ENGAGE counts as the duel context by itself.
static int CcgRecentEngage(uint64_t now)
{
    return g_mdu.eng_ms && now >= g_mdu.eng_ms
        && now - g_mdu.eng_ms < CCG_TAIL_MS;
}

static int CcgDuelCtx(void)
{                               // xcam_start (t2r-ext.c): a camera starting now belongs to a duel
    uint64_t now = GetTickCount64();
    return CcgDuelNow() || CcgRecentEngage(now) || (g_ccg.last_duel
                                                        && now -
                                                        g_ccg.last_duel <
                                                        CCG_TAIL_MS);
}

// xcam_start: inside a duel a takedown / execution camera never cuts in (2013's victim blend-in 0, v5 works like SP: SP blends its takedown cameras in)
static void CcgXcamBlendIn(float *bin_f, const char *name)
{
    float in = g_cam2On ? 0.45f : CCG_XCAM_IN; // v5-cam2: SP's synced-move camera blends in over ~0.45-0.47 s (re/melee-v5-cam.md)
    if (CcgOff() || !CcgDuelCtx() || !bin_f || *bin_f >= in * 60.0f)
        return;
    static int nl;
    if (LOGCAP(nl, 60))
        Xlog("camcont: %s blends in over %.0f ms instead of %.0f ms (a duel camera never cuts in)", name ? name : "?", in * 1000.0f, *bin_f * (1000.0f / 60.0f));
    *bin_f = in * 60.0f;
}

// v5-cam7 (log 39223 'xcam victim: ctxdown:pe-fist-cover-abby role 2 blends: in 450 ms, out 300 ms' and cam2 39385-39394
// 5.5-7.7 deg / 0.26-0.32 m per frame on its way out = above SP's 5.6 deg / 0.10 m max; the 0.3 s out cap is for gunplay control outside a fight):
// inside a duel a takedown / execution camera releases over at least SP's ~0.6 s (cam2_T out of a synced camera 0.6-0.75 s). The camera only - the
// move's control hand-back is not on this clock. Returns the frames added (the caller moves the camera's end by them).
#define CCG_XCAM_OUT 1.0f       // SP: the hand-back after a finisher eases over 1-1.5 s (was 0.6)
static float CcgXcamBlendOut(float *bout_f, const char *name)
{
    if (CcgOff() || !g_cam2On || !CcgDuelCtx() || !bout_f
        || *bout_f >= CCG_XCAM_OUT * 60.0f)
        return 0.0f;
    float add = CCG_XCAM_OUT * 60.0f - *bout_f;
    static int nl;
    if (LOGCAP(nl, 60))
        Xlog("camcont: %s releases over %.0f ms instead of %.0f ms (a duel camera never whips out)", name ? name : "?", CCG_XCAM_OUT * 1000.0f, *bout_f * (1000.0f / 60.0f));
    *bout_f = CCG_XCAM_OUT * 60.0f;
    return add;
}

static void CcgDesc(char *o, size_t n, const CcgSrc *a, const CcgSrc *b)
{
    static const char *const P[] =
        { "game camera", "exec/takedown camera", "free camera",
"menu camera", "our framing" };
    size_t l = 0;
    o[0] = 0;
#define CCGA(...) do { if (l < n) { int w_ = snprintf(o + l, n - l, __VA_ARGS__); if (w_ > 0) l += (size_t)w_; } } while (0)
    if (a->path != b->path)
        CCGA("path %s -> %s; ", P[a->path < 5 ? a->path : 0],
             P[b->path < 5 ? b->path : 0]);
    if (a->xon != b->xon)
        CCGA("exec/takedown camera %s; ", b->xon ? "STARTED" : "STOPPED");
    if (__builtin_fabsf(a->xw - b->xw) > 0.05f)
        CCGA("exec/takedown weight %.2f -> %.2f; ", (double) a->xw,
             (double) b->xw);
    if (a->auth != b->auth)
        CCGA("authored shot %d -> %d; ", a->auth, b->auth);
    if (a->out != b->out)
        CCGA("exec blend-out %s; ", b->out ? "begins" : "ends");
    if (a->ehb != b->ehb)
        CCGA("exec hand-back %s; ", b->ehb ? "begins" : "ends");
    if (a->ho != b->ho)
        CCGA("framing hand-over to the takedown camera %s; ",
             b->ho ? "begins" : "ends");
    if (a->spok != b->spok)
        CCGA("duel framing %s; ", b->spok ? "on" : "OFF (hard)");
    if (__builtin_fabsf(a->sw - b->sw) > 0.05f)
        CCGA("framing weight %.2f -> %.2f; ", (double) a->sw,
             (double) b->sw);
    if (__builtin_fabsf(a->yw - b->yw) > 0.05f)
        CCGA("look-at weight %.2f -> %.2f; ", (double) a->yw,
             (double) b->yw);
    if (__builtin_fabsf(a->pw - b->pw) > 0.05f)
        CCGA("pin camera weight %.2f -> %.2f; ", (double) a->pw,
             (double) b->pw);
    if (a->gdn != b->gdn)
        CCGA("down camera %s; ", b->gdn ? "on" : "off");
    if (!l)
        CCGA("no source switched on this frame (a moving target: look-at / framing / pin camera target, or the engine camera itself); ");
    CCGA("now: path %s, exec w %.2f, framing w %.2f, look-at %.2f, pin cam %.2f", P[b->path < 5 ? b->path : 0], (double) b->xw, (double) b->sw, (double) b->yw, (double) b->pw);
#undef CCGA
}

static void Cam2Tick(void);
static void ClgTick(uint64_t now, int duel);   // cam-lens (end of this file)
static void CcgTick(void)
{
    uint64_t now = GetTickCount64();
    Cam2Tick();                // v5-cam2 (below): opt-out, victim engage, cam2 lines
    int off = CcgOff();
    int in_match = MatchSubstate() == 6 && !g_fcam_on && !g_icam_on;
    if (in_match && CcgDuelNow())
        g_ccg.last_duel = now;
    else if (in_match && CcgRecentEngage(now)
             && g_mdu.eng_ms > g_ccg.last_duel)
        g_ccg.last_duel = g_mdu.eng_ms; // v5-cam7: a 0 ms engage (ccg_recent_engage)
    int duel = in_match && g_ccg.last_duel
        && now - g_ccg.last_duel < CCG_TAIL_MS;
    {
        float pb[16];
        const float *pl = in_match ? PlayerLocator(pb) : NULL;
        if (pl && __builtin_isfinite(pl[0]) && __builtin_isfinite(pl[1])
            && __builtin_isfinite(pl[2])) {
            g_ccg.root[0] = pl[0];
            g_ccg.root[1] = pl[1];
            g_ccg.root[2] = pl[2];
            g_ccg.root_ok = 1;
        } else
            g_ccg.root_ok = 0;
    }
    g_ccg.absorb = !off;
    g_ccg.mode = !in_match ? 0 : duel ? 2 : 1;
    ClgTick(now, in_match && duel);
    if (!g_ccg.logged_cfg) {
        g_ccg.logged_cfg = 1;
        Xlog("camcont: limits - ours %.0f deg / %.2f m per frame (%.0f deg/s, %.1f m/s), engine in a duel %.0f deg / %.2f m (%.0f deg/s), outside a duel glitches only (%.0f deg / %.1f m, held %d frames), residual spring omega %.0f capped %.0f deg/s / %.1f m/s, duel tail %d ms, duel exec cameras blend in >= %.0f ms", (double) CCG_OUR_DEG, (double) CCG_OUR_M, (double) CCG_OUR_RATE, (double) CCG_OUR_MPS, (double) CCG_ENG_DEG, (double) CCG_ENG_M, (double) CCG_ENG_RATE, (double) CCG_GLITCH_DEG, (double) CCG_GLITCH_M, CCG_HOLD_FRAMES, (double) CCG_OMEGA, (double) (CCG_RES_RATE * 57.2957795f), (double) CCG_RES_MPS, CCG_TAIL_MS, (double) (CCG_XCAM_IN * 1000.0f));
    }
    // drain the render thread's events
    LONG h = g_ccg.head;
    if (h - g_ccg.tail > CCG_EVN)
        g_ccg.tail = h - CCG_EVN;
    for (; g_ccg.tail < h; g_ccg.tail++) {
        int i = (int) (g_ccg.tail & (CCG_EVN - 1));
        if (!LOGCAP(g_ccg.nlog, 600))
            continue;
        char d[512];
        CcgDesc(d, sizeof d, &g_ccg.ev[i].a, &g_ccg.ev[i].b);
        int kd = g_ccg.ev[i].kind, done = g_ccg.ev[i].done;
        const char *act = kd == CCG_E_IN_PASS
            || kd ==
            CCG_E_CUT_PASS ?
            "passed as is (the lens never goes into a wall)" : kd ==
            CCG_E_IN_EASE ?
            "EASED (absorbed, the shown spot stays inside the clear head-to-lens ray)"
            : kd ==
            CCG_E_WALL ?
            "clamped onto the live camera at once (hard clamp: the ray hit a wall)"
            : kd == CCG_E_DISP ? "SHOWN on screen" : kd ==
            CCG_E_EXCUR ? (done ? "never shown (held, absorbed both ways)"
                           : "shown (measure only)") : kd ==
            CCG_E_RESET ? "filter reset (respawn / teleport)" : !(done & 1)
            ? "NOT absorbed (measure only)" : (done & 2) ?
            "absorbed, held 5 frames then eased" :
            "absorbed, eased over ~0.5 s";
        if (kd == CCG_E_EXCUR)
            Xlog("camcont: frame %llu EXCURSION - the engine camera jumped away and came back after %d frame(s) (last step %.1f deg / %.2f m): %s | %s | %s", (unsigned long long) g_ccg.ev[i].frame, g_ccg.ev[i].frames, (double) g_ccg.ev[i].deg, (double) g_ccg.ev[i].m, act, g_ccg.ev[i].mode == 2 ? "duel" : "outside a duel", d);
        else if (kd == CCG_E_RESET)
            Xlog("camcont: frame %llu %s - our root moved %.2f m in one frame", (unsigned long long) g_ccg.ev[i].frame, act, (double) g_ccg.ev[i].m);
        else if (kd == CCG_E_IN_PASS || kd == CCG_E_IN_EASE)
            Xlog("camcont: frame %llu %s jump %.1f deg / %.2f m in 1 frame (limit %.1f deg / %.2f m) - %s | ray: shown lens %.2f m from us -> live %.2f m, allowed %.2f m, newest ray answer %s%.2f m, verdict %s | %s | cause: %s", (unsigned long long) g_ccg.ev[i].frame, CCG_EN[kd], (double) g_ccg.ev[i].deg, (double) g_ccg.ev[i].m, (double) g_ccg.ev[i].lim_deg, (double) g_ccg.ev[i].lim_m, act, (double) g_ccg.ev[i].sd, (double) g_ccg.ev[i].ld, (double) g_ccg.ev[i].rb, g_ccg.ev[i].ra >= 9.0f ? "clear " : "", (double) (g_ccg.ev[i].ra >= 9.0f ? 0.0f : g_ccg.ev[i].ra), g_ccg.ev[i].clr < 0 ? "NO RAY (no framing partner)" : g_ccg.ev[i].clr ? "clear" : "INSIDE a surface", g_ccg.ev[i].mode == 2 ? "duel" : "outside a duel", d);
        else
            Xlog("camcont: frame %llu %s jump %.1f deg / %.2f m in 1 frame (limit %.1f deg / %.2f m) - %s; residual %.1f deg / %.2f m | %s | cause: %s", (unsigned long long) g_ccg.ev[i].frame, CCG_EN[kd], (double) g_ccg.ev[i].deg, (double) g_ccg.ev[i].m, (double) g_ccg.ev[i].lim_deg, (double) g_ccg.ev[i].lim_m, act, (double) g_ccg.ev[i].res_deg, (double) g_ccg.ev[i].res_m, g_ccg.ev[i].mode == 2 ? "duel" : "outside a duel", d);
    }
    if (!g_ccg.sum_at)
        g_ccg.sum_at = now;
    if (now - g_ccg.sum_at >= CCG_SUM_MS) {
        g_ccg.sum_at = now;
        if (g_ccg.n_raw || g_ccg.n_disp || g_ccg.n_in || g_ccg.n_exc)
            Xlog("camcont: 30 s summary (%s) - DISPLAYED max %.1f deg / %.2f m per frame beyond the engine's own motion, %u frame(s) over the limit; raw steps over the limits %u (ours max %.1f deg / %.2f m); absorbed: ours rot %u / pos %u, engine rot %u / pos %u (%u held); inward passed %u, cuts passed %u, excursions %u; residual max %.1f deg / %.2f m; %u frames", off ? "measure only" : "on", (double) g_ccg.disp_deg, (double) g_ccg.disp_m, g_ccg.n_disp, g_ccg.n_raw, (double) g_ccg.raw_deg, (double) g_ccg.raw_m, g_ccg.n_our_r, g_ccg.n_our_p, g_ccg.n_eng_r, g_ccg.n_eng_p, g_ccg.n_hold, g_ccg.n_in, g_ccg.n_cut, g_ccg.n_exc, (double) g_ccg.res_deg, (double) g_ccg.res_m, g_ccg.n_frames);
        g_ccg.disp_deg = g_ccg.disp_m = g_ccg.raw_deg = g_ccg.raw_m =
            g_ccg.res_deg = g_ccg.res_m = 0.0f;
        if (g_ccg.fin_frame)
            Xlog("camcont: 30 s summary - FINAL camera in a duel max %.1f deg / %.2f m per frame relative to our body (the framing's eased pivot while it is on, ps23) at frame %llu%s (SP: 5.6 deg / 0.10 m)", (double) g_ccg.fin_deg, (double) g_ccg.fin_m, (unsigned long long) g_ccg.fin_frame, g_ccg.fin_passed ? " (a pull-in passed as is)" : "");
        g_ccg.fin_m = g_ccg.fin_deg = 0.0f;
        g_ccg.fin_frame = 0;
        g_ccg.fin_passed = 0;
        if (g_ccg.n_in_ease || g_ccg.n_wall)
            Xlog("camcont: 30 s summary - inward steps eased %u (head-to-lens ray clear), eased lens clamped at a wall %u", g_ccg.n_in_ease, g_ccg.n_wall);
        g_ccg.n_in_ease = g_ccg.n_wall = 0;
        g_ccg.n_frames = g_ccg.n_disp = g_ccg.n_raw = g_ccg.n_our_r =
            g_ccg.n_our_p = g_ccg.n_eng_r = g_ccg.n_eng_p = g_ccg.n_in =
            g_ccg.n_cut = g_ccg.n_exc = g_ccg.n_hold = 0;
    }
}

// ==== v5-cam2: ONE duel camera (one continuous camera with blends in between, as SP does) ====
// SP, measured: t2r-sp-melee*.log (melee_rec, 4537 player frames of SP melee incl. synced stabs, dodges, combos) -
// the render camera NEVER steps more than 5.6 deg / 0.10 m in one 60 fps frame (p99 1.6 deg / 0.04 m); the melee camera switch on (is-current-camera-
// melee 0 -> 1) blends fov 70 -> 75 in ~265 ms and the offset over ~0.5 s, off (1 -> 0) fov 75 -> 70 in ~450 ms / offset t90 ~0.6-0.75 s, a synced
// stab's camera blends fov 75 -> 62 over ~465 ms from the melee camera, back out the same; the reference clip (60 fps)
// has no cut (max ffmpeg scene score 0.13). So SP is one camera whose framing parameters blend between states.
// Ours: several writers (engine camera + melee lock, duel framing spm, look-at, pin camera pincam5, takedown / finisher / escape / execution xcam, its
// hand-back ehb, the down camera gdn). cam2_filter sits after all of them in icam_filter (before the camcont guard) and is the duel camera's single owner
// of TRANSITIONS: it classifies the frame's camera STATE (game / framing / pin / takedown / hand-back / down); when the state changes and the output
// would step (> CAM2_SW_DEG / CAM2_SW_M from the last shown camera), the last shown camera (relative to our head, so it moves with us) blends into
// the new state's live camera over the SP time of that transition (cam2_T, smootherstep); in a duel a jump of the engine camera inside one state
// (melee camera / lock switch) blends the same over CAM2_ENG_S. Collision: the lens never stays farther out than the live camera (pull-in passes).
// The guard (ccg_filter) stays behind it as the safety net: "camcont:" absorbed lines in a duel should now be 0.
// Also (main thread, cam2_tick): the victim engage - on the partner's first SWING / ENGAGE message (or its first pair if no swing came first) the
// look-at comes in at once with the fast spring + orbit (melee_spcam.c g_cam2_boost_until) and "cam2: victim framed slot N after X ms, Y ms before its
// contact" measures it. Opt-out t2r-duel-cam-v5.txt next to the exe (re-read every 2 s) = the v5-cam camera (no cam2 blends, old look-at / gates).
#define CAM2_SW_DEG 3.0f        // a state switch blends when the output would step more than this ...
#define CAM2_SW_M 0.08f         // ... or this (one frame's continuous motion stays under both)
#define CAM2_ENG_DEG 10.0f      // in a duel, an engine camera jump inside one state (per 60 fps frame; = camcont's CCG_ENG_DEG, a mouse flick stays under it)
#define CAM2_ENG_M 0.25f
#define CAM2_ENG_S 0.45f        // its blend (SP melee camera switch ~0.27-0.5 s)
#define CAM2_EVN 32
enum { CAM2_G, CAM2_F, CAM2_P, CAM2_X, CAM2_H, CAM2_D, CAM2_NS };
static const char *const CAM2_SN[] =
    { "game", "duel framing", "pin camera",
"takedown/finisher/exec camera", "hand-back", "down camera" };
// blend time (s) of a switch from state a to state b (SP numbers above; rows = from)
static float Cam2T(int a, int b)
{
    if (b == CAM2_X)
        return 0.45f;           // into a synced / takedown camera: SP ~0.45-0.47 s (fov 75 -> 62)
    if (a == CAM2_X || a == CAM2_H)
        return 0.70f;           // out of it: SP release ~0.6-0.75 s
    if (b == CAM2_D)
        return 0.45f;
    if (a == CAM2_D)
        return 0.60f;
    if (b == CAM2_P)
        return 0.45f;
    if (a == CAM2_P)
        return 0.60f;
    if (a == CAM2_G && b == CAM2_F)
        return 0.30f;           // melee camera on: fov in ~265 ms
    if (b == CAM2_G)
        return 0.60f;           // melee camera off: ~450-750 ms
    return 0.45f;
}

static struct {
    struct {
        uint64_t frame;
        int a, b, eng;
        float T, deg, m;
        float ey, orb, ya, c, pt, le, ls, em;
        int gated, spl, sgn;
    } ev[CAM2_EVN];
    // v5-cam4: the 30 s max shown step's frame and what every stage did on it
    struct {
        uint64_t frame;
        int st, bl, jin, gated, spl, sgn;
        float T, deg, m, wm, lsd, lsm, rm, yc, pc, ey, orb, ya, c, pt, le,
            ls, em;
    } mx;
    volatile LONG head;
    LONG tail;
    volatile uint32_t n_blend[CAM2_NS][CAM2_NS], n_eng, n_in, n_frames,
        n_rej;
    volatile float max_deg, max_m;
    volatile float err;
    volatile int err_ok;        // render thread: how far the partner is off the view centre (rad, final camera)
    volatile uint32_t fm_n;
    volatile float fm_h, fm_d, fm_p, fm_yp, fm_ye;      // v5-feel2: the shown duel framing (render thread sums, the 30 s summary logs)
    volatile int aim;           // main thread: the aim button is held (gunplay: switches blend fast, no engine-jump blends)
    int nlog;
    uint64_t sum_at;
} g_cam2;
static int Cam2State(void)
{
    if (g_xcamOn)
        return g_ehbOn ? CAM2_H : CAM2_X;
    if (g_gdn.on)
        return CAM2_D;
    if (g_pcam.w > 0.02f)
        return CAM2_P;
    if (g_spm.ok && (g_spm.w > 0.02f || g_spm.yw > 0.02f))
        return CAM2_F;
    return CAM2_G;
}

static float Cam2Qdiff(const float *a, const float *b)
{                               // angle (rad) between two unit quaternions
    float d =
        __builtin_fabsf(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] +
                        a[3] * b[3]);
    d = d > 1.0f ? 1.0f : d;
    return 2.0f * __builtin_acosf(d);
}

static void Cam2Slerp(const float *a, const float *b, float s, float *o)
{
    float ac[4], d[4], rv[3];
    CcgQconj(a, ac);
    CcgQmul(b, ac, d);
    CcgQnorm(d);
    CcgQ2rv(d, rv);
    for (int i = 0; i < 3; i++)
        rv[i] *= s;
    CcgRv2q(rv, d);
    CcgQmul(d, a, o);
    CcgQnorm(o);
}

static volatile int g_cam2Jshown;      // v5-feel opt-out: engine jumps measured against the shown camera (old)
static void *Cam2Filter(uint8_t *dst, const float *loc, void *r)
{
    static float buf[2][32] __attribute__((aligned(16)));
    static struct {
        int ok, st, bl, lok, fok, wok;
        double t;
        float T, bt, Fp[3], Fq[4], Dp[3], Dq[4], Up[3], Uq[4], root[3],
            Lp[3], Lq[4], Lw[3], yc, pc, ly, lpa, lhl;
        LONG gen37;
        double grace, bad0;
    } S[2];
    uint8_t *m = IcamManager();
    int k = !m ? -1 : dst == m + 0x130 ? 0 : dst == m + 0x550 ? 1 : -1;
    if (k < 0 || !r || IsBadReadPtr(r, 0x80))
        return r;
    int mode = g_ccg.mode;
    if (!g_cam2On || !mode || g_fcam_on || !g_ccg.root_ok
        || (g_s34Tb && g_pcam.w > 0.001f) || S40ExecCam()
        || S45sSwing()) {
        S[k].ok = 0;
        if (k == 0)
            g_cam2.err_ok = 0;
        return r;
    }                           // (ps35: the victim's timed shot - ccg_filter's note)
    // An own-body teleport broke the camera: the log showed 'camcont: filter reset - our root moved 52.88 m', then 'cam2: duel camera FLIP REJECTED in duel framing - ... its lens
    // swung 117.6 deg round our head in one frame ... engine lens moved 56.38 m', then 13 s of 'frame check - our head x +9.99 y +9.99 ... rig latched, look-at 1.00': the ENGINE
    // camera arrived at the teleported body 8 frames after the body; its arrival read as the lens swinging round our head, the flip guard took the 117.6 deg back
    // and held that correction 'for the rest of the duel' - the lens stood on the far side of our head, the look-at aimed from there, our own head behind
    // the lens. Now: (1) an own-body teleport (melee_spcam.c spm_tick, g_s37_gen) restarts this stage and no flip is rejected for 1 s after it, nor on a
    // frame where the live lens itself moved > 2 m; (2) a held correction under which our own head is behind / more than ~70 deg off the view for 0.3 s
    // is dropped and the view re-acquires the live camera over 0.6 s. t2r-ps37-cam-reset-old.txt = neither.
    if (g_ps37 && S[k].gen37 != g_s37Gen) {
        S[k].gen37 = g_s37Gen;
        S[k].ok = 0;
        S[k].grace = GshNow() + 1.0;
    }
    const float *U = (const float *) r;
    float Up[3] = { U[0], U[1], U[2] }, Uq[4] = { U[4], U[5], U[6], U[7] };
    if (!CcgQnorm(Uq))
        return r;
    for (int i = 0; i < 3; i++)
        if (!(__builtin_fabsf(Up[i]) < 1e6f))
            return r;
    float root[3] = { g_ccg.root[0], g_ccg.root[1] + 1.5f, g_ccg.root[2] };
    // ps23 (melee_spcam.c spm_slew): with the duel framing on, the offsets of this stage (a running blend, a held flip correction) are kept relative to the
    // framing's eased pivot - relative to the raw root they carried every one-frame step of our body onto the screen
    if (g_ps23Piv && g_spm.ok) {
        float qx = g_spm.px, qz = g_spm.pz;
        if (qx == qx && qz == qz
            && (qx - root[0]) * (qx - root[0]) + (qz - root[2]) * (qz -
                                                                   root[2])
            < 1.0f) {
            root[0] = qx;
            root[2] = qz;
        }
    }
    double now = GshNow();
    float dt = S[k].ok ? (float) (now - S[k].t) : 0.0f;
    int st = Cam2State();
    float Ur[3] = { Up[0] - root[0], Up[1] - root[1], Up[2] - root[2] };
    int reset = !S[k].ok || dt > 0.25f || dt < 0.0f;
    if (!reset) {
        float rd[3] =
            { root[0] - S[k].root[0], root[1] - S[k].root[1],
      root[2] - S[k].root[2] };
        if (CcgLen3(rd) > 2.0f)
            reset = 1;
    }                           // respawn / teleport
    // outside a duel the 2013 authored execution shots keep their own cuts
    if (reset || (g_xcamAuthored && mode != 2)) {
        S[k].ok = 1;
        S[k].t = now;
        S[k].st = st;
        S[k].bl = 0;
        memcpy(S[k].Dp, Ur, sizeof Ur);
        memcpy(S[k].Dq, Uq, sizeof Uq);
        memcpy(S[k].root, root, sizeof root);
        memcpy(S[k].Lp, Ur, sizeof Ur);
        memcpy(S[k].Lq, Uq, sizeof Uq);
        S[k].lok = 1;
        S[k].yc = S[k].pc = 0.0f;
        S[k].fok = 0;
        S[k].wok = 0;
        S[k].bad0 = 0.0;
        return r;
    }
    // v5-cam3 (review of v5-int11: B 60 fps f1038 -> 1039 a hard cut to the opposite side, then the lens inside / behind our own head to ~f1052; B log
    // 'engine camera jump in duel framing (178.7 deg / 0.02 m in one frame) - blended over 450 ms', 27 of 204 jumps >= 90 deg; SP changes the shoulder
    // side 0 times in 1979 melee-camera frames and never turns more than 5.6 deg a frame): a duel camera never flips. A live yaw step > 90 deg in one
    // frame (the view turned round at the same lens) or a swing of the lens round our head > 90 deg in one frame (the opposite shoulder) is not motion
    // and not a switch to blend toward - it is REJECTED: the step is taken back into a world-yaw correction (yc for the view, pc for the lens round our
    // head) that stays for the rest of the duel, so the shown camera keeps its look direction and shoulder side; a flip back removes it again. When the
    // duel framing ends (takedown / pin / down camera, the duel over, aiming) a held correction is released with the state blend (0.6 s), never cut.
    {
        int own41 = mode == 2 && S41RigOwner();
        if (own41 && k == 0 && dt >= 0.0005f)
            InterlockedIncrement(&g_s41.n_cam2);        // ps41 step 0
        int keep = mode == 2 && (st == CAM2_F || st == CAM2_G)
            && !g_cam2.aim && !own41;
        float fx = 2.0f * (Uq[0] * Uq[2] + Uq[3] * Uq[1]), fz =
            1.0f - 2.0f * (Uq[0] * Uq[0] + Uq[1] * Uq[1]);
        float ya = __builtin_atan2f(fx, fz), hl =
            __builtin_sqrtf(Ur[0] * Ur[0] + Ur[2] * Ur[2]), pa =
            __builtin_atan2f(Ur[0], Ur[2]);
        if (dt >= 0.0005f) {
            if (keep && S[k].fok) {
                float sy =
                    __builtin_remainderf(ya - S[k].ly, 6.2831853f), sp =
                    hl > 0.6f
                    && S[k].lhl >
                    0.6f ? __builtin_remainderf(pa - S[k].lpa,
                                                6.2831853f) : 0.0f;
                int ry = __builtin_fabsf(sy) > 1.5707963f, rp =
                    __builtin_fabsf(sp) > 1.5707963f;
                if (g_ps37 && (ry || rp)) {     // ps37 (1): not after a teleport, not when the live lens itself jumped
                    float jw[3] =
                        { Up[0] - S[k].Lw[0], Up[1] - S[k].Lw[1],
                  Up[2] - S[k].Lw[2] };
                    if (now < S[k].grace
                        || (S[k].wok && CcgLen3(jw) > 2.0f))
                        ry = rp = 0;
                }
                // v5-cam4 (A/B v5-int13 B: two rejected flips of 177.2 + 176.9 deg left a 5.9 deg correction held for 11 s - 'flip correction released
                // (view 5.9 deg)', 7.3 deg at 13166 - and the shown framing sat ~6 deg off: 'us +17.6 / +20.1 deg off the view centre (SP +11.8)'): a flip is
                // taken back by exactly 180 deg, the rest of that frame's turn (the 1-3 deg of real motion) passes; two flips cancel to 0 exactly
                if (ry) {
                    S[k].yc =
                        __builtin_remainderf(S[k].yc -
                                             (sy >
                                              0.0f ? 3.14159265f :
                                              -3.14159265f), 6.2831853f);
                    if (__builtin_fabsf(S[k].yc) < 1e-3f)
                        S[k].yc = 0.0f;
                }
                if (rp)
                    S[k].pc =
                        __builtin_remainderf(S[k].pc - sp, 6.2831853f);
                if ((ry || rp) && k == 0) {
                    LONG h = g_cam2.head;
                    int i = (int) (h & (CAM2_EVN - 1));
                    g_cam2.ev[i].frame = g_frame;
                    g_cam2.ev[i].a = st;
                    g_cam2.ev[i].b = st;
                    g_cam2.ev[i].eng = 2;
                    g_cam2.ev[i].T = 0.0f;
                    g_cam2.ev[i].deg = __builtin_fabsf(sy) * 57.29578f;
                    g_cam2.ev[i].m = __builtin_fabsf(sp) * 57.29578f;
                    g_cam2.ev[i].ey = g_cst.eng_yaw;
                    g_cam2.ev[i].orb = g_cst.orb;
                    g_cam2.ev[i].ya = g_cst.ya;
                    g_cam2.ev[i].c = g_cst.c;
                    g_cam2.ev[i].pt = g_cst.pitch;
                    g_cam2.ev[i].le = g_cst.len_e;
                    g_cam2.ev[i].ls = g_cst.len_s;
                    g_cam2.ev[i].em = g_cst.eng_m;
                    g_cam2.ev[i].gated = g_cst.gated;
                    g_cam2.ev[i].spl = g_cst.spl;
                    g_cam2.ev[i].sgn = g_cst.sgn;
                    MemoryBarrier();
                    g_cam2.head = h + 1;
                    g_cam2.n_rej++;
                }
            } else if (!keep && (S[k].yc != 0.0f || S[k].pc != 0.0f)) {
                if (k == 0) {
                    LONG h = g_cam2.head;
                    int i = (int) (h & (CAM2_EVN - 1));
                    g_cam2.ev[i].frame = g_frame;
                    g_cam2.ev[i].a = S[k].st;
                    g_cam2.ev[i].b = st;
                    g_cam2.ev[i].eng = 3;
                    g_cam2.ev[i].T = 0.6f;
                    g_cam2.ev[i].deg =
                        __builtin_fabsf(S[k].yc) * 57.29578f;
                    g_cam2.ev[i].m = __builtin_fabsf(S[k].pc) * 57.29578f;
                    MemoryBarrier();
                    g_cam2.head = h + 1;
                }
                S[k].yc = S[k].pc = 0.0f;
                memcpy(S[k].Fp, S[k].Dp, sizeof S[k].Fp);
                memcpy(S[k].Fq, S[k].Dq, sizeof S[k].Fq);
                S[k].T = 0.6f;
                S[k].bt = 0.0f;
                S[k].bl = 1;
            }
            S[k].ly = ya;
            S[k].lpa = pa;
            S[k].lhl = hl;
            S[k].fok = 1;
        }
        if (S[k].yc != 0.0f) {
            float sn = __builtin_sinf(S[k].yc * 0.5f), cs =
                __builtin_cosf(S[k].yc * 0.5f), ry[4] =
                { 0.0f, sn, 0.0f, cs }, o[4];
            CcgQmul(ry, Uq, o);
            memcpy(Uq, o, sizeof o);
            CcgQnorm(Uq);
        }
        if (S[k].pc != 0.0f) {
            float sn = __builtin_sinf(S[k].pc), cs =
                __builtin_cosf(S[k].pc), x = Ur[0], z = Ur[2];
            Ur[0] = x * cs + z * sn;
            Ur[2] = -x * sn + z * cs;
        }
    }
    if (dt < 0.0005f) {
        if (!S[k].bl && S[k].yc == 0.0f && S[k].pc == 0.0f)
            return r;
        memcpy(buf[k], r, sizeof buf[k]);
        for (int i = 0; i < 3; i++)
            buf[k][i] = root[i] + S[k].Dp[i];
        for (int i = 0; i < 4; i++)
            buf[k][4 + i] = S[k].Dq[i];
        return buf[k];
    }
    S[k].t = now;
    memcpy(S[k].root, root, sizeof root);
    float n60 = dt * 60.0f;
    if (n60 < 1.0f)
        n60 = 1.0f;
    float dv[3] =
        { Ur[0] - S[k].Dp[0], Ur[1] - S[k].Dp[1], Ur[2] - S[k].Dp[2] },
        sm = CcgLen3(dv), sd = Cam2Qdiff(S[k].Dq, Uq) * 57.29578f;
    float dl = CcgLen3(Ur), dd = CcgLen3(S[k].Dp);
    int inward = dl < dd - 0.05f && sd < CAM2_SW_DEG;   // only the lens came in toward our head (collision pull-in): no blend
    // v5-feel: an engine jump is the LIVE camera's own step since the last frame. It used to be measured against the last SHOWN camera: while a blend
    // runs the shown camera lags the live one, so every frame read as a new jump and restarted the blend at s ~ 0 - the camera froze up to 74 deg /
    // 1.2 m behind the finisher camera for ~3 s (log frames 77876-78000, victim out of frame) and 10-12 deg behind the duel framing for 146 frames
    // (51579-51724). Opt-out t2r-cam2-shown-jump.txt (re-read every 2 s) = the old shown-camera test.
    float jsd = sd, jsm = sm;
    int jin = inward;
    if (!g_cam2Jshown && S[k].lok) {
        float lv[3] =
            { Ur[0] - S[k].Lp[0], Ur[1] - S[k].Lp[1], Ur[2] - S[k].Lp[2] };
        jsm = CcgLen3(lv);
        jsd = Cam2Qdiff(S[k].Lq, Uq) * 57.29578f;
        jin = dl < CcgLen3(S[k].Lp) - 0.05f && jsd < CAM2_SW_DEG;
        // ps23 (melee_spcam.c spm_slew): the offsets here are relative to our root, so a one-frame step of the ROOT under a lens that stayed where it was
        // (the framing pivot is eased now) read as an 'engine camera jump in duel framing' - and its blend starts from the old offset on the NEW root, i.e.
        // it would put the root's step back on screen. A jump is one the live lens made in the world as well; the smaller of the two steps counts.
        if (g_ps23Piv && S[k].wok) {
            float wv3[3] =
                { Up[0] - S[k].Lw[0], Up[1] - S[k].Lw[1],
         Up[2] - S[k].Lw[2] };
            float jw = CcgLen3(wv3);
            if (jw < jsm)
                jsm = jw;
        }
    }
    memcpy(S[k].Lp, Ur, sizeof Ur);
    memcpy(S[k].Lq, Uq, sizeof Uq);
    S[k].lok = 1;
    memcpy(S[k].Lw, Up, sizeof Up);
    S[k].wok = 1;
    int trig = 0, eng = 0;
    if (st != S[k].st) {
        if (!inward && (sd > CAM2_SW_DEG || sm > CAM2_SW_M))
            trig = 1;
    }
    // (pincam2: in the pin camera the live camera IS the pin camera, whose follower already moves at <= 4 deg / 0.08 m a frame; the engine camera under
    //  it is not shown. v5-int test: its old weight blend stepped 12-21 deg and this restarted a 450 ms blend ~20 times per pin - two blends fighting.)
    // (v5-cam7: after a pin-escape counter the camera froze, then swung toward the wall with both bodies leaving the
    //  frame. log 39385-39394: 'engine camera jump in takedown/finisher/exec camera (5.5-7.7 deg / 0.26-0.32 m)' on 10 frames in a row = the takedown
    //  camera's OWN blend-out / hand-back (not the engine), each frame restarting a 450 ms blend at s ~ 0 from the frozen shown camera. Our own
    //  blend-out is motion, not a jump: it is never re-blended here (its speed is bounded at xcam_start, ccg_xcam_blend_out).)
    else if (mode == 2 && !S41RigOwner() && st != CAM2_P
             && !((st == CAM2_X || st == CAM2_H) && g_xcam_out) && !jin
             && (jsd > CAM2_ENG_DEG * n60 || jsm > CAM2_ENG_M * n60)) {
        trig = 1;
        eng = 1;
        sd = jsd;
        sm = jsm;
    }
    if (eng && g_cam2.aim)
        trig = 0;               // aiming: the over-shoulder camera comes in as the engine moves it
    // ('engine camera jump in takedown/finisher/exec camera (10-23 deg / 0.18-0.34 m in one frame)' on 12-14 CONSECUTIVE frames at
    // every finisher start = the authored track moves that fast, each frame restarted the 450 ms blend at s 0 from the shown camera = a 0.2 s stutter): a
    // running jump blend already follows the live camera - a further jump in its first 60 % is taken by that blend, not a new one. t2r-cam2-reblend.txt = old.
    if (eng && trig && S[k].bl && S[k].T > 0.0f && S[k].bt < 0.6f * S[k].T
        && GetFileAttributesA("t2r-cam2-reblend.txt") ==
        INVALID_FILE_ATTRIBUTES)
        trig = 0;
    if (trig) {
        float T =
            eng ? CAM2_ENG_S : g_cam2.aim ? 0.15f : Cam2T(S[k].st, st);
        // v5-cam4 (A/B v5-int14 B 17586 'duel framing -> pin camera (would step 123.5 deg / 4.97 m) - blended over 450 ms' = 0.2-0.33 m / 4-6 deg per
        // frame for ~15 frames, A 20007 44 deg / 2.57 m the same): a fixed SP blend time is right for SP's short switches, not for a 5 m one. The blend
        // lasts at least long enough that its smootherstep peak (1.875 x step / T) stays at SP's per-frame max: 0.10 m (6 m/s) and 5.6 deg (336 deg/s),
        // capped at 1.2 s
        if (!g_cam2.aim) {
            float Tm = fmaxf(1.875f * sm / 6.0f, 1.875f * sd / 336.0f);
            Tm = Tm > 1.2f ? 1.2f : Tm;
            if (Tm > T)
                T = Tm;
        }
        memcpy(S[k].Fp, S[k].Dp, sizeof S[k].Fp);
        memcpy(S[k].Fq, S[k].Dq, sizeof S[k].Fq);
        S[k].T = T;
        S[k].bt = 0.0f;
        S[k].bl = 1;
        if (k == 0) {
            LONG h = g_cam2.head;
            int i = (int) (h & (CAM2_EVN - 1));
            g_cam2.ev[i].frame = g_frame;
            g_cam2.ev[i].a = S[k].st;
            g_cam2.ev[i].b = st;
            g_cam2.ev[i].eng = eng;
            g_cam2.ev[i].T = T;
            g_cam2.ev[i].deg = sd;
            g_cam2.ev[i].m = sm;
            MemoryBarrier();
            g_cam2.head = h + 1;
            if (eng)
                g_cam2.n_eng++;
            else
                g_cam2.n_blend[S[k].st][st]++;
        }
    }
    S[k].st = st;
    float Op[3], Oq[4];
    if (S[k].bl) {
        S[k].bt += dt;
        float x = S[k].T > 0.0f ? S[k].bt / S[k].T : 1.0f;
        if (x >= 1.0f) {
            S[k].bl = 0;
            memcpy(Op, Ur, sizeof Op);
            memcpy(Oq, Uq, sizeof Oq);
        } else {
            float s = x * x * x * (x * (x * 6.0f - 15.0f) + 10.0f);     // smootherstep: no velocity / acceleration step at either end
            for (int i = 0; i < 3; i++)
                Op[i] = S[k].Fp[i] + (Ur[i] - S[k].Fp[i]) * s;
            Cam2Slerp(S[k].Fq, Uq, s, Oq);
            float ol = CcgLen3(Op);
            float ow[3] =
                { root[0] + Op[0], root[1] + Op[1], root[2] + Op[2] };
            if (dl < ol - 0.02f && ol > 1e-3f && !CcgLensClear(ow)) {
                for (int i = 0; i < 3; i++)
                    Op[i] *= dl / ol;
                if (k == 0)
                    g_cam2.n_in++;
            }                   // collision: never farther out than the live camera (v5-cam3: unless the head-to-lens ray says the blended spot is clear)
        }
    } else {
        memcpy(Op, Ur, sizeof Op);
        memcpy(Oq, Uq, sizeof Oq);
    }
    if (k == 0 && mode == 2) {
        float ev[3] =
            { Op[0] - S[k].Dp[0], Op[1] - S[k].Dp[1], Op[2] - S[k].Dp[2] },
            em = CcgLen3(ev) / n60, ed =
            Cam2Qdiff(S[k].Dq, Oq) * 57.29578f / n60;
        // v5-cam4: the step is measured relative to our body (Op / Dp are offsets from our root); the world step and our root's own step are kept for the max
        static float pw[3], pr[3];
        static int pwok;
        float wv[3] =
            { root[0] + Op[0], root[1] + Op[1], root[2] + Op[2] }, wm =
            0.0f, rm = 0.0f;
        if (pwok) {
            float a[3] =
                { wv[0] - pw[0], wv[1] - pw[1], wv[2] - pw[2] }, b[3] =
                { root[0] - pr[0], root[1] - pr[1], root[2] - pr[2] };
            wm = CcgLen3(a) / n60;
            rm = CcgLen3(b) / n60;
        }
        memcpy(pw, wv, sizeof pw);
        memcpy(pr, root, sizeof pr);
        pwok = 1;
        if (ed > g_cam2.max_deg)
            g_cam2.max_deg = ed;
        if (em > g_cam2.max_m)
            g_cam2.max_m = em;
        if (em + ed * 0.1f > g_cam2.mx.m + g_cam2.mx.deg * 0.1f) {
            g_cam2.mx.frame = g_frame;
            g_cam2.mx.st = st;
            g_cam2.mx.bl = S[k].bl;
            g_cam2.mx.T = S[k].bl ? S[k].T : 0.0f;
            g_cam2.mx.jin = jin;
            g_cam2.mx.deg = ed;
            g_cam2.mx.m = em;
            g_cam2.mx.wm = wm;
            g_cam2.mx.lsd = jsd / n60;
            g_cam2.mx.lsm = jsm / n60;
            g_cam2.mx.rm = rm;
            g_cam2.mx.yc = S[k].yc;
            g_cam2.mx.pc = S[k].pc;
            g_cam2.mx.ey = g_cst.eng_yaw;
            g_cam2.mx.orb = g_cst.orb;
            g_cam2.mx.ya = g_cst.ya;
            g_cam2.mx.c = g_cst.c;
            g_cam2.mx.pt = g_cst.pitch;
            g_cam2.mx.le = g_cst.len_e;
            g_cam2.mx.ls = g_cst.len_s;
            g_cam2.mx.em = g_cst.eng_m;
            g_cam2.mx.gated = g_cst.gated;
            g_cam2.mx.spl = g_cst.spl;
            g_cam2.mx.sgn = g_cst.sgn;
        }
        g_cam2.n_frames++;
        // how far the partner is off the view centre (victim framing metric): the camera forward is +Z or -Z of its quaternion - the one pointing at us
        if (g_spm.ok && MduEngaged()) {
            float f[3];
            SpmFwd(Oq, f);
            float wx = root[0] + Op[0], wz = root[2] + Op[2];
            int flip =
                (g_spm.px - wx) * f[0] + (g_spm.pz - wz) * f[2] < 0.0f;
            if (flip) {
                f[0] = -f[0];
                f[2] = -f[2];
            }
            float e =
                __builtin_remainderf(__builtin_atan2f
                                     (g_spm.ex - wx,
                                      g_spm.ez - wz) -
                                     __builtin_atan2f(f[0], f[2]),
                                     6.2831853f);
            g_cam2.err = __builtin_fabsf(e);
            g_cam2.err_ok = e == e;
            // v5-feel2: the shown duel framing, measured the way melee_rec measures SP (tools/sp_melee_cam.py): lens height above our root, horizontal
            // distance behind us, pitch, and how far us / the partner sit off the view centre (+ = the side we are on)
            if (st == CAM2_F && !S[k].bl && g_spm.yw > 0.85f
                && g_spm.w > 0.85f) {
                float hx = g_spm.px - wx, hz = g_spm.pz - wz, hd =
                    __builtin_sqrtf(hx * hx + hz * hz), fy =
                    flip ? -f[1] : f[1];
                float fa = __builtin_atan2f(f[0], f[2]), yp =
                    __builtin_remainderf(__builtin_atan2f(hx, hz) - fa,
                                         6.2831853f), sg =
                    yp < 0.0f ? -1.0f : 1.0f;
                float h = root[1] + Op[1] - g_ccg.root[1], pi =
                    __builtin_asinf(fy > 1.0f ? 1.0f : fy <
                                    -1.0f ? -1.0f : fy);
                if (h == h && hd == hd && pi == pi && hd < 6.0f) {
                    g_cam2.fm_n++;
                    g_cam2.fm_h += h;
                    g_cam2.fm_d += hd;
                    g_cam2.fm_p += pi;
                    g_cam2.fm_yp += sg * yp;
                    g_cam2.fm_ye += sg * e;
                }
            }
        } else
            g_cam2.err_ok = 0;
    } else if (k == 0)
        g_cam2.err_ok = 0;
    if (g_ps37 && (S[k].yc != 0.0f || S[k].pc != 0.0f)) {       // ps37 (2): a kept direction that has our own head behind / far off the view is not kept
        float f3[3];
        SpmFwd(Oq, f3);
        float dl = CcgLen3(Op), dep = -(Op[0] * f3[0] + Op[1] * f3[1] + Op[2] * f3[2]);        // (our head-height pivot seen from the lens: -Op; the view axis is +Z)
        if (!(dl > 0.2f) || dep >= 0.35f * dl)
            S[k].bad0 = 0.0;
        else if (S[k].bad0 == 0.0)
            S[k].bad0 = now;
        else if (now - S[k].bad0 > 0.3) {
            if (k == 0) {
                LONG h = g_cam2.head;
                int i = (int) (h & (CAM2_EVN - 1));
                g_cam2.ev[i].frame = g_frame;
                g_cam2.ev[i].a = st;
                g_cam2.ev[i].b = st;
                g_cam2.ev[i].eng = 4;
                g_cam2.ev[i].T = 0.6f;
                g_cam2.ev[i].deg = __builtin_fabsf(S[k].yc) * 57.29578f;
                g_cam2.ev[i].m = __builtin_fabsf(S[k].pc) * 57.29578f;
                MemoryBarrier();
                g_cam2.head = h + 1;
            }
            S[k].yc = S[k].pc = 0.0f;
            S[k].fok = 0;
            S[k].bad0 = 0.0;
            memcpy(S[k].Fp, Op, sizeof S[k].Fp);
            memcpy(S[k].Fq, Oq, sizeof S[k].Fq);
            S[k].T = 0.6f;
            S[k].bt = 0.0f;
            S[k].bl = 1;
        }
    }
    memcpy(S[k].Dp, Op, sizeof Op);
    memcpy(S[k].Dq, Oq, sizeof Oq);
    if (!S[k].bl && S[k].yc == 0.0f && S[k].pc == 0.0f)
        return r;               // (v5-cam3: a held flip correction is shown too)
    memcpy(buf[k], r, sizeof buf[k]);
    for (int i = 0; i < 3; i++)
        buf[k][i] = root[i] + Op[i];
    for (int i = 0; i < 4; i++)
        buf[k][4 + i] = Oq[i];
    return buf[k];
}

// main thread (ccg_tick): opt-out, the victim engage boost + framing metric, the event / summary lines
#define CAM2_FRAMED_RAD 0.35f   // the partner within ~20 deg of the view centre with the look-at in = framed
static void Cam2Tick(void)
{
    uint64_t now = GetTickCount64();
    {
        static int v = -1;
        static uint64_t at;
        if (v < 0 || now - at >= 2000) {
            at = now;
            FILE *f = ExeSideOpen("t2r-duel-cam-v5.txt", "r");
            int o = f != NULL;
            if (f)
                fclose(f);
            if (o != v)
                Xlog("cam2: %s",
                     o ?
                     "OFF (t2r-duel-cam-v5.txt) - the v5-cam duel camera (camcont guard only, old look-at / pair gates)"
                     :
                     "ON - one duel camera: state switches (game / framing / pin / takedown / hand-back / down) blend at SP times (in 0.30, sync 0.45, out 0.60-0.70 s), engine jumps in a duel 0.45 s, the framing stays on through duel pairs, victim engage frames the attacker fast (spring + orbit)");
            v = o;
            g_cam2On = !o;
        }
    }
    {
        static int v = -1;
        static uint64_t at;     // v5-feel: engine-jump test on the live camera's own step (cam2_filter)
        if (v < 0 || now - at >= 2000) {
            at = now;
            FILE *f = ExeSideOpen("t2r-cam2-shown-jump.txt", "r");
            int o = f != NULL;
            if (f)
                fclose(f);
            if (o != v)
                Xlog("cam2: engine jump test %s",
                     o ?
                     "on the SHOWN camera (t2r-cam2-shown-jump.txt - old: a running blend re-triggers itself every frame)"
                     :
                     "on the live camera's own step since the last frame (a running blend is never restarted by its own lag)");
            v = o;
            g_cam2Jshown = o;
        }
    }
    {
        static int v = -1;
        static uint64_t at;     // cam-boost: t2r-cam-boost-old.txt = the old 600 deg/s caps of the victim-engage / dodge-follow springs
        if (v < 0 || now - at >= 2000) {
            at = now;
            FILE *f = ExeSideOpen("t2r-cam-boost-old.txt", "r");
            int o = f != NULL;
            if (f)
                fclose(f);
            if (o != v)
                Xlog("cam2: engage / dodge-follow boost springs %s",
                     o ?
                     "at the OLD 600 deg/s each (t2r-cam-boost-old.txt)" :
                     "capped at SP's pace: look-at 336 deg/s, orbit 168 deg/s (the view never whips)");
            v = o;
            g_cam2BoostOld = o;
        }
    }
    g_cam2.aim = g_ccg.mode && g_cam2On && PadCmdOn("aim");
    // the victim engage
    static uint64_t eng_seen, t0, contact;
    static int vslot = -1, how;
    static float err0;
    static int done;
    if (g_cam2On && MduEngaged() && g_mdu.eng_ms != eng_seen) {
        eng_seen = g_mdu.eng_ms;
        int s = g_mdu.slot, ours = g_mdu.sw_on && (int64_t) g_mdu.sw_t0 - (int64_t) g_mdu.eng_ms > -60 && (int64_t) g_mdu.sw_t0 - (int64_t) g_mdu.eng_ms < 60;  // (separate GetTickCount64 reads: +-1 tick)
        // (log 39576 'cam2: victim engage by slot 0 (their swing message) - the duel camera turns to it now (fast look-at + orbit)', 39582
        // 'victim framed slot 0 after 109 ms', then camrec 39579 'cyaw -0.8' -> 39642 'cyaw -107.4' as the attacker ran round a player who never pressed a
        // key; SP: hit outside the melee camera the camera turns 0.10 deg/frame, p90 0.59): no boost, no victim framing - the camera stays the player's
        // until a swing of ours (melee_spcam.c spm_tick, the rig latch). t2r-ps25-cam-victim-old.txt = the victim engage below.
        if (!ours && s >= 0 && s < T2R_MAX_PEERS && g_ps25[PS25_VICTIM]) {
            vslot = -1;
            static int nl25;
            if (LOGCAP(nl25, 200))
                Xlog("cam2: victim engage by slot %d - ps25: no fast look-at / orbit, the camera keeps the player's heading until a swing of ours", s);
        } else if (!ours && s >= 0 && s < T2R_MAX_PEERS) {
            vslot = s;
            t0 = g_mdu.eng_ms;
            done = 0;
            how = g_mdu.vsw_at[s] + 60 >= g_mdu.eng_ms ? 1 : 0;
            contact = how
                && g_mdu.vsw_end[s] >
                30 ? g_mdu.vsw_end[s] - 30 : g_mdu.eng_ms;
            err0 = g_cam2.err_ok ? g_cam2.err : -1.0f;
            g_cam2BoostUntil = now + 900;
            static int nl;
            if (LOGCAP(nl, 200))
                Xlog("cam2: victim engage by slot %d (%s) - the duel camera turns to it now (fast look-at + orbit), contact in ~%lld ms, rtt %d ms", s, how ? "their swing message" : "their pair on us - no swing message first", (long long) ((int64_t) contact - (int64_t) now), (int) (2.0f * PsyncOneway(s)));
        } else
            vslot = -1;
    }
    if (vslot >= 0 && !done) {
        int framed = g_cam2.err_ok && g_cam2.err < CAM2_FRAMED_RAD
            && g_spm.yw > 0.85f;
        static int nl2;
        if (framed) {
            done = 1;
            g_cam2BoostUntil = now + 150;
            if (LOGCAP(nl2, 300))
                Xlog("cam2: victim framed slot %d after %lld ms, %lld ms before its contact (start %.0f deg off-centre, orbit %.0f deg, %s)", vslot, (long long) (now - t0), (long long) ((int64_t) contact - (int64_t) now), err0 >= 0.0f ? (double) (err0 * 57.29578f) : -1.0, (double) (g_cam2Orb * 57.29578f), how ? "swing" : "pair first");
        } else if (now - t0 > 1500 || !MduEngaged()) {
            done = 1;
            if (LOGCAP(nl2, 300))
                Xlog("cam2: victim NOT framed slot %d %lld ms after engage (%s, still %.0f deg off-centre, look-at %.2f, contact was %lld ms after engage)", vslot, (long long) (now - t0), !MduEngaged()? "duel ended" : "timeout", g_cam2.err_ok ? (double) (g_cam2.err * 57.29578f) : -1.0, (double) g_spm.yw, (long long) ((int64_t) contact - (int64_t) t0));
        }
    }
    // events
    LONG h = g_cam2.head;
    if (h - g_cam2.tail > CAM2_EVN)
        g_cam2.tail = h - CAM2_EVN;
    for (; g_cam2.tail < h; g_cam2.tail++) {
        int i = (int) (g_cam2.tail & (CAM2_EVN - 1));
        if (!LOGCAP(g_cam2.nlog, 600))
            continue;
        if (g_cam2.ev[i].eng == 2)
            Xlog("cam2: frame %llu duel camera FLIP REJECTED in %s - the live camera turned %.1f deg / its lens swung %.1f deg round our head in one frame; the duel keeps its look direction and shoulder side (SP: 0 side changes)" " | stages: engine heading %.0f deg, engine lens moved %.2f m, lens to us %.2f m (engine) / %.2f m (framing %d), orbit %.1f deg, look-at turn %.1f deg (gate %d, al/dist %.2f, sign %d), pitch turn %.1f deg", (unsigned long long) g_cam2.ev[i].frame, CAM2_SN[g_cam2.ev[i].b], (double) g_cam2.ev[i].deg, (double) g_cam2.ev[i].m, (double) (g_cam2.ev[i].ey * 57.29578f), (double) g_cam2.ev[i].em, (double) g_cam2.ev[i].le, (double) g_cam2.ev[i].ls, g_cam2.ev[i].spl, (double) (g_cam2.ev[i].orb * 57.29578f), (double) (g_cam2.ev[i].ya * 57.29578f), g_cam2.ev[i].gated, (double) g_cam2.ev[i].c, g_cam2.ev[i].sgn, (double) (g_cam2.ev[i].pt * 57.29578f));
        else if (g_cam2.ev[i].eng == 4)
            Xlog("cam2: frame %llu duel camera kept direction DROPPED in %s - our own head was behind / far off the view for > 0.3 s under a held flip correction (view %.1f deg / lens %.1f deg round us): re-acquired from the live camera over %.0f ms (ps37)", (unsigned long long) g_cam2.ev[i].frame, CAM2_SN[g_cam2.ev[i].b], (double) g_cam2.ev[i].deg, (double) g_cam2.ev[i].m, (double) (g_cam2.ev[i].T * 1000.0f));
        else if (g_cam2.ev[i].eng == 3)
            Xlog("cam2: frame %llu duel flip correction released (%s -> %s: view %.1f deg / lens %.1f deg round us) - blended over %.0f ms", (unsigned long long) g_cam2.ev[i].frame, CAM2_SN[g_cam2.ev[i].a], CAM2_SN[g_cam2.ev[i].b], (double) g_cam2.ev[i].deg, (double) g_cam2.ev[i].m, (double) (g_cam2.ev[i].T * 1000.0f));
        else if (g_cam2.ev[i].eng)
            Xlog("cam2: frame %llu engine camera jump in %s (%.1f deg / %.2f m in one frame) - blended over %.0f ms", (unsigned long long) g_cam2.ev[i].frame, CAM2_SN[g_cam2.ev[i].b], (double) g_cam2.ev[i].deg, (double) g_cam2.ev[i].m, (double) (g_cam2.ev[i].T * 1000.0f));
        else
            Xlog("cam2: frame %llu %s -> %s (would step %.1f deg / %.2f m) - blended over %.0f ms", (unsigned long long) g_cam2.ev[i].frame, CAM2_SN[g_cam2.ev[i].a], CAM2_SN[g_cam2.ev[i].b], (double) g_cam2.ev[i].deg, (double) g_cam2.ev[i].m, (double) (g_cam2.ev[i].T * 1000.0f));
    }
    if (!g_cam2.sum_at)
        g_cam2.sum_at = now;
    if (now - g_cam2.sum_at >= 30000) {
        g_cam2.sum_at = now;
        uint32_t nb = 0;
        for (int a = 0; a < CAM2_NS; a++)
            for (int b = 0; b < CAM2_NS; b++)
                nb += g_cam2.n_blend[a][b];
        if (nb || g_cam2.n_eng)
            Xlog("cam2: 30 s summary - %u state blend(s) (framing in %u / out %u, into takedown %u, out of takedown %u, pin in %u / out %u, down %u), %u engine jump blend(s), %u collision clamp frame(s), shown in a duel max %.1f deg / %.2f m per frame (the stick's own turn included) over %u duel frames", nb, g_cam2.n_blend[CAM2_G][CAM2_F], g_cam2.n_blend[CAM2_F][CAM2_G], g_cam2.n_blend[CAM2_F][CAM2_X] + g_cam2.n_blend[CAM2_P][CAM2_X] + g_cam2.n_blend[CAM2_G][CAM2_X], g_cam2.n_blend[CAM2_X][CAM2_F] + g_cam2.n_blend[CAM2_X][CAM2_G] + g_cam2.n_blend[CAM2_H][CAM2_F] + g_cam2.n_blend[CAM2_H][CAM2_G] + g_cam2.n_blend[CAM2_X][CAM2_P], g_cam2.n_blend[CAM2_F][CAM2_P] + g_cam2.n_blend[CAM2_G][CAM2_P], g_cam2.n_blend[CAM2_P][CAM2_F] + g_cam2.n_blend[CAM2_P][CAM2_G], g_cam2.n_blend[CAM2_F][CAM2_D] + g_cam2.n_blend[CAM2_G][CAM2_D] + g_cam2.n_blend[CAM2_D][CAM2_G] + g_cam2.n_blend[CAM2_D][CAM2_F], g_cam2.n_eng, g_cam2.n_in, (double) g_cam2.max_deg, (double) g_cam2.max_m, g_cam2.n_frames);
        if (g_cam2.fm_n >= 30) {
            float n = (float) g_cam2.fm_n;
            Xlog("cam2: duel framing shown (%u settled frames) - lens %.2f m above our root (SP 1.61), %.2f m behind us (SP 1.22), pitch %+.1f deg (SP -16.5), us %+.1f deg / partner %+.1f deg off the view centre (SP +11.8 / -11.1)", g_cam2.fm_n, (double) (g_cam2.fm_h / n), (double) (g_cam2.fm_d / n), (double) (g_cam2.fm_p / n * 57.29578f), (double) (g_cam2.fm_yp / n * 57.29578f), (double) (g_cam2.fm_ye / n * 57.29578f));
        }
        if (g_cam2.mx.frame)
            Xlog("cam2: 30 s max shown step at frame %llu: %.1f deg / %.2f m relative to our body (world %.2f m) - %s, blend %s, live step %.1f deg / %.2f m%s, our root moved %.2f m, flip correction %.1f / %.1f deg" " | stages: engine heading %.0f deg, engine lens moved %.2f m, lens to us %.2f m (engine) / %.2f m (framing %d), orbit %.1f deg, look-at turn %.1f deg (gate %d, al/dist %.2f, sign %d), pitch turn %.1f deg", (unsigned long long) g_cam2.mx.frame, (double) g_cam2.mx.deg, (double) g_cam2.mx.m, (double) g_cam2.mx.wm, CAM2_SN[g_cam2.mx.st], g_cam2.mx.bl ? "running" : "none", (double) g_cam2.mx.lsd, (double) g_cam2.mx.lsm, g_cam2.mx.jin ? " (INWARD: passed unblended)" : "", (double) g_cam2.mx.rm, (double) (g_cam2.mx.yc * 57.29578f), (double) (g_cam2.mx.pc * 57.29578f), (double) (g_cam2.mx.ey * 57.29578f), (double) g_cam2.mx.em, (double) g_cam2.mx.le, (double) g_cam2.mx.ls, g_cam2.mx.spl, (double) (g_cam2.mx.orb * 57.29578f), (double) (g_cam2.mx.ya * 57.29578f), g_cam2.mx.gated, (double) g_cam2.mx.c, g_cam2.mx.sgn, (double) (g_cam2.mx.pt * 57.29578f));
        memset((void *) &g_cam2.mx, 0, sizeof g_cam2.mx);
        if (g_cam2.n_rej)
            Xlog("cam2: 30 s summary - %u duel camera flip(s) (> 90 deg in one frame) rejected", g_cam2.n_rej);
        g_cam2.n_rej = 0;
        g_cam2.fm_n = 0;
        g_cam2.fm_h = g_cam2.fm_d = g_cam2.fm_p = g_cam2.fm_yp =
            g_cam2.fm_ye = 0.0f;
        memset((void *) g_cam2.n_blend, 0, sizeof g_cam2.n_blend);
        g_cam2.n_eng = g_cam2.n_in = g_cam2.n_frames = 0;
        g_cam2.max_deg = g_cam2.max_m = 0.0f;
    }
}

// ======================================================================================================================================================
// cam-lens: THE final camera stage of a fight (after every other writer: engine, xcam, cam2, ccg) and its
// diagnostic. Evidence: me-match / bot run clips with the lens inside a pillar (c2_b 7.6-8.0 s), inside our own coat / arm / a crate with plank bars across
// the view (x7c 100.3-101.4, x1f 27.70, x3a 58.0), and a far jump at c4 10.0 s no log line explained - every writer of the chain has its own wall rule
// (spm ray, xcam body push, ehb collapse, engine collision) and none sees what the others finally output.
//  1. DIAGNOSTIC ("camdiag:" lines, capped 80 per match): the shown lens < 0.45 m from our head for > 5 frames, or its world step > 0.5 m in one frame -
//     names every stage that wrote that frame (position AND rotation step of each: motion.c g_mo_cs), the engine camera's own step, the natives issued,
//     the camera sources (xcam / hand-back / framing / look-at / pin / down) and the lens / head / pivot positions.
//  2. LENS GUARD ("lensguard:" lines): a fan of 5 rays (a sphere approximation: the centre line + 4 offsets of 0.14 m round it, radius CLG_R 0.20 m, the
//     shot mask 0x1b07 - the camera's own collision mask is not in our data; it stops on props / cars / bodies and passes tall grass) from our head to the
//     lens, kicked by the main thread each tick on the lens that ENTERS the guard, answered the next tick. The render thread pulls the lens in to
//     (clear length - radius), EASED: in with tau 60 ms, out at 1 m/s - no pop. Our own body is ignored by the ray (SID_PLAYER); the pair partner's body is
//     framing, not a wall: a ray hit within 0.55 m of its axis is discarded and a lens inside its 0.45 m radius is moved sideways over the shoulder
//     (<= 0.6 m, same easing) instead of being pulled through. Never closer than CLG_MIN to our head. Duel context only (ccg mode 2); 2013 AUTHORED special-
//     execution shots keep their own cameras; t2r-cam-lensguard-old.txt = off (the diagnostic stays).
#define CLG_R 0.20f
#define CLG_START 0.30f         // the rays start this far out from the head (beside our own capsule)
#define CLG_MIN 0.35f
#define CLG_TAU_IN 0.06f
#define CLG_OUT_MPS 1.0f
#define CLG_PART_HIT 0.55f
#define CLG_PART_R 0.45f
#define CLG_PART_OUT 0.52f
#define CLG_PART_MAX 0.60f
#define CLG_NR 5
#define CLG_DGN 8
// (camera inside a body: 'camdiag: frame 16445 LENS WITHIN 0.45 m OF OUR HEAD for 6 frames ... (lens-head 0.35 m)' with camrec 'dist
// 0.31-0.44' on B 16440-16458 and 'dist 0.34-0.35' for a whole second on B 16041-16095 (duel framing at full weight); A 20460-20469 'dist 0.47-0.58' through
// a 'ctxdown: hand-off'): this guard only ever pulled the lens IN (walls) or sideways off the partner's ROOT axis; nothing in the chain moved a lens out of
// our own body outside the downed state (dlg_filter is the downed guard and stands aside under an execution camera), and the partner test missed a torso
// leaning off its root (hit reactions, pair halves). Both bodies are in THIS stage now, for every owner it runs under (game camera, duel framing, pin,
// ctxdown takedown / finisher cameras, hand-back):
//   own body   capsules root -> pelvis -> chest -> head (dlg_filter's radii incl. 0.12 m lens margin, joints read by the main thread each tick): a lens inside
//              them is moved OUT along the head -> lens line (the camera's own arm, i.e. toward SP's melee distance), only as far as the ray fan says that
//              line is clear (the fan now reaches CLG_BODY_EXT past the lens and is also cast for a lens closer than 0.6 m) - never through a wall; no
//              clear spot within CLG_BODY_MAX = no move. Eased like the pull-in (in tau 60 ms, back at 1 m/s).
//   partner    the sideways shift measures against the nearest of its root / pelvis / chest / head (was: the root only).
// t2r-ps23-cam-bodyguard-old.txt next to the exe (re-read every 2 s) = the old guard. Log "lensguard: ... own body".
#define CLG_BODY_EXT 0.45f
#define CLG_BODY_MAX 0.45f
// ('lensguard: the lens was inside our own body - moved 0.06 m out', B 29294 '0.08 m' / 30306 '0.11 m', while the frames show the
// lens inside a 0.3-0.4 m body radius): (1) the line is written when the EASED push-out first passes 0.05 m (tau 60 ms), so it always read 0.06-0.11 m
// whatever the push became - the episode's largest value is logged at its end now; (2) the capsules were 0.29 / 0.37 / 0.37 / 0.40 m INCLUDING the 0.12 m
// lens margin, i.e. a 0.25-0.28 m body: a lens 0.31 m from the spine was moved 0.06 m and then stood on the surface of the coat / inside the backpack;
// (3) the push-out and the fan's reach past the lens were capped at 0.45 m. Under the game camera / duel framing (not under a takedown camera, whose
// authored close shots keep the ps23 radii) the body is 0.40 m + the 0.12 m margin (head 0.33 m [TUNING]) and the reach 0.70 m. t2r-ps25-cam-bodyguard-old.txt.
#define CLG25_BODY_EXT 0.70f    // [TUNING ps25]
#define CLG25_BODY_MAX 0.70f    // [TUNING ps25]
#define CLG25_BODY_R 0.52f      // 0.40 m body radius (coat + backpack) + 0.12 m lens margin
#define CLG25_HEAD_R 0.33f
static struct {
    volatile int bg;            // ps23: the body guard is on (main thread: clg_tick)
    float bod[2][4][3];
    volatile int bok[2];
    volatile uint64_t b_ms;     // ps23, main thread: [0] our / [1] the partner's root (+0.15 m), pelvis, chest, head; valid; their time
    volatile float bout, bout_max;
    volatile LONG n_bout;
    int ep_bo, nlog_bo;         // ps23: the own-body push-out in use (render thread), its frames; the episode + log cap (main thread)
    volatile int on, off;
    volatile float in[3];
    volatile LONG in_seq;       // render thread (view 0): the lens entering the guard
    volatile float clear;
    volatile uint64_t clear_ms;
    volatile int hit_i;
    volatile float hit_len;     // main thread: clear length from the head, its age, the ray that hit
    volatile float px, py, pz;
    volatile int pok;           // main thread: the pair partner's root (g_spm)
    volatile float red, shift, red_max;
    volatile LONG n_red, n_shift;       // render thread: the pull-in / partner shift in use, the episode's max and frames
    uint64_t job[CLG_NR];
    float cs[CLG_NR][3], ce[CLG_NR][3], dir[3];
    int njob;
    LONG kick_seq;
    int ep_red, ep_sh, nlog_red, nlog_sh, nlog_dg;
    char dg[CLG_DGN][780];
    volatile LONG dg_head;
    LONG dg_tail;
} g_clg;
static int ClgSpecialExec(void)
{
    return g_xcamOn && g_xcamAuthored
        && strncmp(g_xcamName, "ctxdown:", 8) != 0;
}

// render thread: the final stage. Returns r or a corrected copy (position only).
static void *ClgFilter(uint8_t *dst, const float *loc, void *r)
{
    static float buf[2][32] __attribute__((aligned(16)));
    static struct {
        int ok;
        double t;
        float red, sh, sdir[2], bo;
    } S[2];
    (void) loc;
    uint8_t *m = IcamManager();
    int k = !m ? -1 : dst == m + 0x130 ? 0 : dst == m + 0x550 ? 1 : -1;
    if (k < 0 || !r || IsBadReadPtr(r, 0x80))
        return r;
    const float *U = (const float *) r;
    int act = g_clg.on && !g_clg.off && g_ccg.root_ok && !g_fcam_on && !(g_s34Tb && g_pcam.w > 0.001f) && __builtin_isfinite(U[0]) && __builtin_isfinite(U[1]) && __builtin_isfinite(U[2]);    // (ps35: not under the victim's timed shot - ccg_filter's note)
    if (!act) {
        S[k].ok = 0;
        S[k].red = S[k].sh = S[k].bo = 0.0f;
        if (k == 0) {
            g_clg.red = 0.0f;
            g_clg.shift = 0.0f;
            g_clg.bout = 0.0f;
        }
        return r;
    }
    double now = GshNow();
    float dt = S[k].ok ? (float) (now - S[k].t) : 0.0f;
    if (dt < 0.0f || dt > 0.25f) {
        dt = 0.0f;
        S[k].red = S[k].sh = S[k].bo = 0.0f;
    }
    if (dt > 0.0005f || !S[k].ok)
        S[k].t = now;
    else
        dt = 0.0f;              // another copy of the same frame's camera: the same result
    S[k].ok = 1;
    float P[3] = { g_ccg.root[0], g_ccg.root[1] + 1.45f, g_ccg.root[2] };
    float d[3] = { U[0] - P[0], U[1] - P[1], U[2] - P[2] }, D =
        CcgLen3(d);
    if (k == 0) {
        g_clg.in[0] = U[0];
        g_clg.in[1] = U[1];
        g_clg.in[2] = U[2];
        InterlockedIncrement(&g_clg.in_seq);
    }
    if (!(D > 0.05f) || D > 20.0f)
        return r;
    int bg = g_clg.bg, cfresh = GetTickCount64() - g_clg.clear_ms < 250;
    float clear = cfresh ? g_clg.clear : 9.0f;
    float red_t = D - (clear > CLG_MIN ? clear : CLG_MIN);
    if (red_t < 0.0f)
        red_t = 0.0f;
    if (bg && D <= 0.6f)
        red_t = 0.0f;           // (ps23: the fan is cast for a close lens too now - it only bounds the push-out below, a close lens is never pulled further in)
    // ps43 Q1 (melee_spcam.c S43Q_*; rec42a A 16159 'lens guard pull-in 2.12 m' on the engage cut's frame): a pull-in built up on the OLD lens line (here
    // the respawn fly-in) is released at 1 m/s - after a cut (g_s37_gen: the rig's cut, a teleport) it held the new lens 0.35 m from our head. On a cut
    // the pull-in drops to what the newest fan answer asks for at once.
    {
        static LONG g43[2];
        if (g43[k] != g_s37Gen) {
            g43[k] = g_s37Gen;
            if (g_ps43q && S[k].red > red_t + 0.02f) {
                S[k].red = red_t;
                if (k == 0)
                    InterlockedIncrement(&g_s43e.clg);
            }
        }
    }
    if (red_t > S[k].red)
        S[k].red +=
            (red_t - S[k].red) * (1.0f - __builtin_expf(-dt / CLG_TAU_IN));
    else {
        S[k].red -= CLG_OUT_MPS * dt;
        if (S[k].red < red_t)
            S[k].red = red_t;
    }
    float maxred = D > CLG_MIN ? D - CLG_MIN : 0.0f;
    if (S[k].red > maxred)
        S[k].red = maxred;
    float f = (D - S[k].red) / D, L[3] =
        { P[0] + d[0] * f, P[1] + d[1] * f, P[2] + d[2] * f };
    // the pair partner: framing, not a wall - a lens inside it moves sideways over the shoulder
    float sh_t = 0.0f;
    if (g_clg.pok) {
        float wx = L[0] - g_clg.px, wz = L[2] - g_clg.pz, wl =
            __builtin_sqrtf(wx * wx + wz * wz);
        if (bg && g_clg.bok[1] && GetTickCount64() - g_clg.b_ms < 250)  // ps23: the nearest of the partner's root / pelvis / chest / head (a torso leaning off its root)
            for (int j = 1; j < 4; j++) {
                float jx = L[0] - g_clg.bod[1][j][0], jz =
                    L[2] - g_clg.bod[1][j][2], jl =
                    __builtin_sqrtf(jx * jx + jz * jz);
                if (jl == jl && jl < wl) {
                    wx = jx;
                    wz = jz;
                    wl = jl;
                }
            }
        if (wl < CLG_PART_R && L[1] > g_clg.py - 0.3f
            && L[1] < g_clg.py + 1.9f) {
            float need = CLG_PART_OUT - wl;
            sh_t = need > CLG_PART_MAX ? CLG_PART_MAX : need;
            if (wl > 0.05f) {
                S[k].sdir[0] = wx / wl;
                S[k].sdir[1] = wz / wl;
            } else {
                float hl = __builtin_sqrtf(d[0] * d[0] + d[2] * d[2]);
                if (hl > 0.05f) {
                    S[k].sdir[0] = -d[2] / hl;
                    S[k].sdir[1] = d[0] / hl;
                } else {
                    S[k].sdir[0] = 1.0f;
                    S[k].sdir[1] = 0.0f;
                }
            }
        }
    }
    if (sh_t > S[k].sh)
        S[k].sh +=
            (sh_t - S[k].sh) * (1.0f - __builtin_expf(-dt / CLG_TAU_IN));
    else {
        S[k].sh -= CLG_OUT_MPS * dt;
        if (S[k].sh < sh_t)
            S[k].sh = sh_t;
    }
    if (S[k].sh < 0.0f)
        S[k].sh = 0.0f;
    // ps23: our own body - a lens inside its capsules moves out along the head -> lens line, as far as the fan's answer says that line is clear
    float od[3] = { d[0] / D, d[1] / D, d[2] / D }, bo_t = 0.0f;
    if (bg && cfresh && g_clg.bok[0]
        && GetTickCount64() - g_clg.b_ms < 250) {
        int b25 = g_ps25[PS25_BODY] && !g_xcamOn;      // ps25: a 0.40 m body + margin under the game camera / duel framing (block comment at CLG25_*)
        float rh = b25 ? CLG25_HEAD_R : 0.29f, rc =
            b25 ? CLG25_BODY_R : 0.37f, rr =
            b25 ? CLG25_BODY_R : 0.40f, bmx =
            b25 ? CLG25_BODY_MAX : CLG_BODY_MAX;
        float room =
            (clear >=
             8.9f ? D +
             (g_ps25[PS25_BODY] ? CLG25_BODY_EXT : CLG_BODY_EXT) : clear) -
            (D - S[k].red);
        if (room > bmx)
            room = bmx;
        const float *b0 = g_clg.bod[0][0], *b1 = g_clg.bod[0][1], *b2 =
            g_clg.bod[0][2], *b3 = g_clg.bod[0][3];
        float top[3] = { b3[0], b3[1] + 0.12f, b3[2] };
        for (float t = 0.0f; t <= room + 1e-3f; t += 0.03f) {
            float Q[3] =
                { L[0] + od[0] * t, L[1] + od[1] * t, L[2] + od[2] * t };
            if (XcamSegDist(Q, b3, top) < rh
                || XcamSegDist(Q, b2, b3) < rc
                || XcamSegDist(Q, b1, b2) < rc
                || XcamSegDist(Q, b0, b1) < rr)
                continue;
            bo_t = t;
            break;
        }
    }
    if (bo_t > S[k].bo)
        S[k].bo +=
            (bo_t - S[k].bo) * (1.0f - __builtin_expf(-dt / CLG_TAU_IN));
    else {
        S[k].bo -= CLG_OUT_MPS * dt;
        if (S[k].bo < bo_t)
            S[k].bo = bo_t;
    }
    if (!(S[k].bo > 0.0f))
        S[k].bo = 0.0f;
    if (k == 0) {
        g_clg.bout = S[k].bo;
        if (S[k].bo > 0.02f)
            g_clg.n_bout++;
        if (S[k].bo > g_clg.bout_max)
            g_clg.bout_max = S[k].bo;
        g_clg.red = S[k].red;
        g_clg.shift = S[k].sh;
        if (S[k].red > 0.02f) {
            g_clg.n_red++;
            if (S[k].red > g_clg.red_max)
                g_clg.red_max = S[k].red;
        }
        if (S[k].sh > 0.02f)
            g_clg.n_shift++;
    }
    if (S[k].red < 0.002f && S[k].sh < 0.002f && S[k].bo < 0.002f)
        return r;
    for (int i = 0; i < 3; i++)
        L[i] += od[i] * S[k].bo;
    L[0] += S[k].sdir[0] * S[k].sh;
    L[2] += S[k].sdir[1] * S[k].sh;
    memcpy(buf[k], U, sizeof buf[k]);
    buf[k][0] = L[0];
    buf[k][1] = L[1];
    buf[k][2] = L[2];
    return buf[k];
}

// render thread, view 0, after the guard and its stage entry: the 'camdiag' trigger. Builds the line here (no xlog on this thread), the main thread logs it.
static void ClgDiag(uint8_t *dst, const float *loc, const float *r)
{
    static struct {
        int ok, nearc;
        double t;
        float p[3], ep[3], root[3];
    } D;
    uint8_t *m = IcamManager();
    if (!m || dst != m + 0x130 || !r || IsBadReadPtr(r, 0x80) || !loc
        || IsBadReadPtr(loc, 0x80))
        return;
    if (!g_ccg.mode || !g_ccg.root_ok || g_fcam_on) {
        D.ok = 0;
        D.nearc = 0;
        return;
    }
    float head[3] =
        { g_ccg.root[0], g_ccg.root[1] + 1.45f, g_ccg.root[2] };
    float rp[3] = { r[0], r[1], r[2] };
    for (int i = 0; i < 3; i++)
        if (!__builtin_isfinite(rp[i]) || __builtin_fabsf(rp[i]) > 1e6f)
            return;
    double now = GshNow();
    float dt = D.ok ? (float) (now - D.t) : 9.0f;
    float dh[3] =
        { rp[0] - head[0], rp[1] - head[1], rp[2] - head[2] }, dhl =
        CcgLen3(dh);
    float st = 0.0f, rm = 0.0f, es = 0.0f;
    if (D.ok && dt < 0.25f) {
        float s3[3] =
            { rp[0] - D.p[0], rp[1] - D.p[1], rp[2] - D.p[2] }, r3[3] =
            { g_ccg.root[0] - D.root[0], g_ccg.root[1] - D.root[1],
      g_ccg.root[2] - D.root[2] };
        float e3[3] =
            { loc[0] - D.ep[0], loc[1] - D.ep[1], loc[2] - D.ep[2] };
        st = CcgLen3(s3);
        rm = CcgLen3(r3);
        es = CcgLen3(e3);
        if (rm > 2.0f)
            st = 0.0f;          // a respawn / teleport (ccg resets there too)
    }
    int ev = 0;
    if (dhl < 0.45f) {
        if (++D.nearc == 6)
            ev |= 1;
    } else
        D.nearc = 0;
    if (st > 0.5f)
        ev |= 2;
    if (ev) {
        char *o = g_clg.dg[g_clg.dg_head & (CLG_DGN - 1)];
        size_t n = 0;
        const size_t cap = sizeof g_clg.dg[0];
#define CGD(...) do { if (n < cap) { int w_ = snprintf(o + n, cap - n, __VA_ARGS__); if (w_ > 0) n += (size_t)w_; } } while (0)
        CcgSrc cs;
        CcgSnap(&cs, loc, (const void *) r);
        float pv[3] = { head[0], head[1], head[2] };
        if (g_xcam_piv_ok) {
            pv[0] = g_xcamPiv[0];
            pv[1] = g_xcamPiv[1];
            pv[2] = g_xcamPiv[2];
        }
        CGD("camdiag: frame %llu %s%s - lens %.2f %.2f %.2f, head %.2f %.2f %.2f (lens-head %.2f m), pivot %.2f %.2f %.2f, shown step %.2f m (world; our root %.2f m, engine camera %.2f m) | stages:", (unsigned long long) g_frame, (ev & 1) ? "LENS WITHIN 0.45 m OF OUR HEAD for 6 frames" : "", (ev & 2) ? ((ev & 1) ? " + LENS STEP > 0.5 m" : "LENS STEP > 0.5 m IN ONE FRAME") : "", (double) rp[0], (double) rp[1], (double) rp[2], (double) head[0], (double) head[1], (double) head[2], (double) dhl, (double) pv[0], (double) pv[1], (double) pv[2], (double) st, (double) rm, (double) es);
        for (int i = 0; i < g_moNcs; i++)
            if (__builtin_fabsf(g_mo_cs[i].dy) >= 0.3f
                || __builtin_fabsf(g_mo_cs[i].dp) >= 0.3f
                || g_mo_cs[i].dm >= 0.02f)
                CGD(" %s%s rot %+.1f/%+.1f deg move %.2f m,",
                    g_mo_cs[i].sub ? "~" : "", g_mo_cs[i].name,
                    (double) g_mo_cs[i].dy, (double) g_mo_cs[i].dp,
                    (double) g_mo_cs[i].dm);
        CGD(" natives:");
        for (int w = 0; w < 2; w++)
            for (int i = 0;
                 g_mo_cnat_fr[w] + 2 >= (g_frame ? g_frame : 1)
                 && i < g_moNcnat[w]; i++)
                CGD(" %s", g_moCnat[w][i]);
        CGD(" | sources: path %s, exec/takedown camera %s (authored %d, w %.2f, hand-back %d, out %d), framing w %.2f / look-at %.2f, pin cam %.2f, down cam %d, hand-over %d | lens guard pull-in %.2f m, partner shift %.2f m (guard %s)", cs.path == 0 ? "game camera" : cs.path == 1 ? "exec/takedown camera" : cs.path == 2 ? "free camera" : cs.path == 3 ? "menu camera" : "our framing", cs.xon ? "ON" : "off", cs.auth, (double) cs.xw, cs.ehb, cs.out, (double) cs.sw, (double) cs.yw, (double) cs.pw, cs.gdn, cs.ho, (double) g_clg.red, (double) g_clg.shift, g_clg.on && !g_clg.off ? "on" : "off");
#undef CGD
        MemoryBarrier();
        g_clg.dg_head++;
    }
    memcpy(D.p, rp, 12);
    memcpy(D.ep, loc, 12);
    memcpy(D.root, (const void *) g_ccg.root, 12);
    D.t = now;
    D.ok = 1;
}

// main thread, every tick from ccg_tick
static void ClgTick(uint64_t now, int duel)
{
    static int v = -1;
    static uint64_t at;
    if (v < 0 || now - at >= 2000) {
        at = now;
        FILE *f = ExeSideOpen("t2r-cam-lensguard-old.txt", "r");
        int o = f != NULL;
        if (f)
            fclose(f);
        if (o != v)
            Xlog("lensguard: %s",
                 o ?
                 "OFF (t2r-cam-lensguard-old.txt) - the 'camdiag' diagnostic still runs"
                 :
                 "ON - the last camera stage of a fight pulls the lens in front of any surface between our head and it (ray fan, radius 0.20 m, eased in 60 ms / out 1 m/s); the pair partner is never a wall");
        v = o;
        g_clg.off = o;
        static int vb = -1;
        FILE *fb = ExeSideOpen("t2r-ps23-cam-bodyguard-old.txt", "r");
        int ob = fb != NULL;
        if (fb)
            fclose(fb);
        if (ob != vb)
            Xlog("lensguard: ps23 body guard %s",
                 ob ?
                 "OFF (t2r-ps23-cam-bodyguard-old.txt) - walls + the partner's root axis only"
                 :
                 "ON - a lens inside our own body moves out along the head -> lens line as far as the ray fan is clear (never through a wall); the partner shift measures against its root / pelvis / chest / head");
        vb = ob;
        g_clg.bg = !ob;
    }
    // drain the last tick's fan
    if (g_clg.njob) {
        float cl = 9.0f, hl = 0.0f;
        int hi = -1, got = 0;
        for (int i = 0; i < g_clg.njob; i++) {
            g_rayErr = 0;
            float t = RayT(g_clg.job[i]);
            int e = g_rayErr;
            g_clg.job[i] = 0;
            if (e || t == -2.0f)
                continue;
            got++;
            if (t >= 0.0f && t < 1.0f) {
                const float *S0 = g_clg.cs[i], *E0 = g_clg.ce[i];
                float H[3] =
                    { S0[0] + (E0[0] - S0[0]) * t,
               S0[1] + (E0[1] - S0[1]) * t, S0[2] + (E0[2] - S0[2]) * t };
                if (g_clg.pok) {
                    float ox = H[0] - g_clg.px, oz = H[2] - g_clg.pz;
                    if (ox * ox + oz * oz < CLG_PART_HIT * CLG_PART_HIT
                        && H[1] > g_clg.py - 0.3f
                        && H[1] < g_clg.py + 2.0f)
                        continue;
                }
                float along =
                    CLG_START + ((E0[0] - S0[0]) * g_clg.dir[0] +
                                 (E0[1] - S0[1]) * g_clg.dir[1] + (E0[2] -
                                                                   S0[2]) *
                                 g_clg.dir[2]) * t, a = along - CLG_R;
                if (a < cl) {
                    cl = a;
                    hi = i;
                    hl = along;
                }
            }
        }
        g_clg.njob = 0;
        if (got) {
            g_clg.clear = cl;
            g_clg.hit_i = hi;
            g_clg.hit_len = hl;
            MemoryBarrier();
            g_clg.clear_ms = now;
        }
    }
    int active = duel && !g_clg.off && g_ccg.root_ok && !g_fcam_on
        && !g_icam_on && !g_gdn.on && !ClgSpecialExec();
    g_clg.on = active;
    g_clg.pok = duel && g_spm.ok;
    g_clg.px = g_spm.ex;
    g_clg.py = g_spm.ey;
    g_clg.pz = g_spm.ez;
    {                           // ps23 body guard: both bodies' joints for the render thread (ours; the partner = the takedown camera's other body, else the framed duel partner)
        int ok0 = 0, ok1 = 0;
        if (active && g_clg.bg) {
            float ab[16], bb[16], p[3], c[3], h[3];
            const float *me = PlayerLocator(ab);
            if (me && XvFin3(me)) {
                XvBody(SID_PLAYER, me, p, c, h);
                if (XvFin3(p) && XvFin3(c) && XvFin3(h)) {
                    g_clg.bod[0][0][0] = me[0];
                    g_clg.bod[0][0][1] = me[1] + 0.15f;
                    g_clg.bod[0][0][2] = me[2];
                    memcpy(g_clg.bod[0][1], p, 12);
                    memcpy(g_clg.bod[0][2], c, 12);
                    memcpy(g_clg.bod[0][3], h, 12);
                    ok0 = 1;
                }
            }
            uint64_t ph = g_xcamOn
                && g_xcam_h ? g_xcam_h : g_spm.ok ? g_spmPh : 0;
            const float *pl = ph ? PuppetLocator(ph, bb) : NULL;
            if (pl && XvFin3(pl)) {
                XvBody(ph, pl, p, c, h);
                if (XvFin3(p) && XvFin3(c) && XvFin3(h)) {
                    g_clg.bod[1][0][0] = pl[0];
                    g_clg.bod[1][0][1] = pl[1] + 0.15f;
                    g_clg.bod[1][0][2] = pl[2];
                    memcpy(g_clg.bod[1][1], p, 12);
                    memcpy(g_clg.bod[1][2], c, 12);
                    memcpy(g_clg.bod[1][3], h, 12);
                    ok1 = 1;
                }
            }
        }
        MemoryBarrier();
        g_clg.bok[0] = ok0;
        g_clg.bok[1] = ok1;
        if (ok0 || ok1)
            g_clg.b_ms = now;
    }
    // kick the next fan on the lens that enters the guard
    if (active && g_clg.in_seq != g_clg.kick_seq) {
        g_clg.kick_seq = g_clg.in_seq;
        float P[3] =
            { g_ccg.root[0], g_ccg.root[1] + 1.45f, g_ccg.root[2] }, L[3] =
            { g_clg.in[0], g_clg.in[1], g_clg.in[2] };
        float d[3] = { L[0] - P[0], L[1] - P[1], L[2] - P[2] }, D =
            CcgLen3(d);
        float ext = g_clg.bg ? (g_ps25[PS25_BODY] ? CLG25_BODY_EXT : CLG_BODY_EXT) : 0.0f;      // ps23: the fan reaches past the lens (how far OUT the head -> lens line is clear) and is cast for a close lens too
        if (D > (g_clg.bg ? 0.15f : 0.6f) && D < 12.0f) {
            for (int i = 0; i < 3; i++)
                g_clg.dir[i] = d[i] / D;
            float u[3] = { g_clg.dir[2], 0.0f, -g_clg.dir[0] }, ul =
                __builtin_sqrtf(u[0] * u[0] + u[2] * u[2]);
            if (ul > 0.05f) {
                u[0] /= ul;
                u[2] /= ul;
            } else {
                u[0] = 1.0f;
                u[2] = 0.0f;
            }
            float vv[3] =
                { u[1] * g_clg.dir[2] - u[2] * g_clg.dir[1],
          u[2] * g_clg.dir[0] - u[0] * g_clg.dir[2],
          u[0] * g_clg.dir[1] - u[1] * g_clg.dir[0] };
            static const float so[CLG_NR][2] =
                { { 0, 0 }, { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
            for (int i = 0; i < CLG_NR; i++) {
                float ox = so[i][0] * 0.7f * CLG_R, oy =
                    so[i][1] * 0.7f * CLG_R;
                for (int c = 0; c < 3; c++) {
                    float off = u[c] * ox + vv[c] * oy;
                    g_clg.cs[i][c] = P[c] + g_clg.dir[c] * CLG_START + off;
                    g_clg.ce[i][c] =
                        L[c] + g_clg.dir[c] * (CLG_R + ext) + off;
                }
                g_clg.job[i] =
                    WorldRayKickM(g_clg.cs[i], g_clg.ce[i], 0, 0x1b07);
            }
            g_clg.njob = CLG_NR;
        }
    }
    // episodes of the pull-in / partner shift (the main thread logs what the render thread did)
    float red = g_clg.red;
    if (red > 0.05f && !g_clg.ep_red) {
        g_clg.ep_red = 1;
        g_clg.red_max = red;
        g_clg.n_red = 0;
        if (LOGCAP(g_clg.nlog_red, 80)) {
            float P[3] =
                { g_ccg.root[0], g_ccg.root[1] + 1.45f, g_ccg.root[2] };
            Xlog("lensguard: the lens is pulled in %.2f m - clear length %.2f m from the head (ray %d of the fan%s, %.2f m to the surface), lens entering the guard %.2f %.2f %.2f, head %.2f %.2f %.2f (%.2f m), %s", (double) red, (double) g_clg.clear, g_clg.hit_i, g_clg.hit_i == 0 ? " = centre" : "", (double) g_clg.hit_len, (double) g_clg.in[0], (double) g_clg.in[1], (double) g_clg.in[2], (double) P[0], (double) P[1], (double) P[2], (double) __builtin_sqrtf((g_clg.in[0] - P[0]) * (g_clg.in[0] - P[0]) + (g_clg.in[1] - P[1]) * (g_clg.in[1] - P[1]) + (g_clg.in[2] - P[2]) * (g_clg.in[2] - P[2])), g_xcamOn ? "while a takedown / finisher / execution camera runs" : "duel framing / game camera");
        }
    } else if (g_clg.ep_red && red < 0.01f) {
        g_clg.ep_red = 0;
        if (LOGCAP(g_clg.nlog_red, 80))
            Xlog("lensguard: pull-in released after %d frame(s), at most %.2f m", (int) g_clg.n_red, (double) g_clg.red_max);
        g_clg.red_max = 0.0f;
        g_clg.n_red = 0;
    }
    float bo = g_clg.bout;
    if (bo > 0.05f && !g_clg.ep_bo) {
        g_clg.ep_bo = 1;
        g_clg.n_bout = 0;
        g_clg.bout_max = bo;
        if (LOGCAP(g_clg.nlog_bo, 60))
            Xlog("lensguard: the lens is inside our own body - the push-out along the head -> lens line starts (%.2f m so far, eased; that line is clear to %.2f m from the head; body radius %.2f m incl. the lens margin, reach %.2f m), %s", (double) bo, (double) g_clg.clear, (double) (g_ps25[PS25_BODY] && !g_xcamOn ? CLG25_BODY_R : 0.37f), (double) (g_ps25[PS25_BODY] && !g_xcamOn ? CLG25_BODY_MAX : CLG_BODY_MAX), g_xcamOn ? "while a takedown / finisher / execution camera runs" : "duel framing / game camera");
    } else if (g_clg.ep_bo && bo < 0.01f) {
        g_clg.ep_bo = 0;
        if (LOGCAP(g_clg.nlog_bo, 60))
            Xlog("lensguard: own-body push-out released after %d frame(s), at most %.2f m", (int) g_clg.n_bout, (double) g_clg.bout_max);
        g_clg.n_bout = 0;
        g_clg.bout_max = 0.0f;
    }
    float sh = g_clg.shift;
    if (sh > 0.05f && !g_clg.ep_sh) {
        g_clg.ep_sh = 1;
        g_clg.n_shift = 0;
        if (LOGCAP(g_clg.nlog_sh, 40))
            Xlog("lensguard: the lens was inside the pair partner's body radius - moved %.2f m sideways over the shoulder (not pulled through)", (double) sh);
    } else if (g_clg.ep_sh && sh < 0.01f) {
        g_clg.ep_sh = 0;
        if (LOGCAP(g_clg.nlog_sh, 40))
            Xlog("lensguard: partner shift released after %d frame(s)",
                 (int) g_clg.n_shift);
        g_clg.n_shift = 0;
    }
    // the diagnostic lines the render thread built
    LONG h = g_clg.dg_head;
    if (h - g_clg.dg_tail > CLG_DGN)
        g_clg.dg_tail = h - CLG_DGN;
    for (; g_clg.dg_tail < h; g_clg.dg_tail++) {
        if (LOGCAP(g_clg.nlog_dg, 80))
            Xlog("%s", g_clg.dg[g_clg.dg_tail & (CLG_DGN - 1)]);
    }
}

// ps40 E6 (melee_spcam.c spm_tick): the duel partner's swing is registered on us (melee_duel.c vsw_at / vsw_end: its dodge window) and not over
static int S40Threat(void)
{
    if (!g_mdu.on || g_mdu.slot < 0 || g_mdu.slot >= T2R_MAX_PEERS)
        return 0;
    uint64_t now = GetTickCount64(), at = g_mdu.vsw_at[g_mdu.slot];
    return at && now >= at && now - at < 2000
        && now < g_mdu.vsw_end[g_mdu.slot] + 150;
}

// ps39 C1 (melee_spcam.c spc_tick5; this file is included after melee_duel_pin.c): the wall plane of the pin the pin camera runs for - a point on the
// surface (x, z) and its facing out of it. The frame is the pin plan's anchor (ctxdown.c: "on the surface at the victim's xz projection, facing out of
// it"): g_mpn.ap on the pinner's screen, the frame of the last pin pair seen on the victim's (g_mpn.vq_ap), the running pair's own frame for a wall
// takedown on us (role 3). 0 = none.
static int S39PinWall(int role, float *pt, float *n)
{
    float ap[7];
    if (role == 2) {
        if (!MpnActive())
            return 0;
        memcpy(ap, g_mpn.ap, sizeof ap);
    } else if (role == 1) {
        if (!g_mpn.vq_ap_ok)
            return 0;
        memcpy(ap, g_mpn.vq_ap, sizeof ap);
    } else if (!PsyncAp(ap))
        return 0;
    if (!XvFin3(ap) || !(ap[4] * ap[4] + ap[6] * ap[6] > 0.25f))
        return 0;
    float yr = CdApYaw(ap) * 0.0174533f;
    pt[0] = ap[0];
    pt[1] = ap[2];
    n[0] = __builtin_sinf(yr);
    n[1] = __builtin_cosf(yr);
    return n[0] == n[0] && n[1] == n[1];
}

// (melee_spcam.c s44m_*; here because melee_duel.c / melee_dodge.c are included after melee_spcam.c - read only): the victim's screen - the duel
// partner's LETHAL swing is a weapon finisher windup (its SWING named the finisher row: g_mdu_fw_ptc) whose dodge window on us has closed (g_mdu.vsw_end, the
// trim for the verdict's way back included) while no dodge clip of ours runs: the blow can no longer fail on our screen. 1 = *slot / *fin (CD_PAIRS row) /
// *to_c (ms until its contact shows here, estimated from the replica's end g_mdu_fw_rend = arrival + tce + 150). Fists (the punch rule) send no row.
static int S44mVicCommit(int *slot, int *fin, int *to_c)
{
    uint64_t now = GetTickCount64();
    if (!g_mdu.on || g_mdu.slot < 0 || g_mdu.slot >= T2R_MAX_PEERS)
        return 0;
    int s = g_mdu.slot;
    if (g_mduFwPtc[s] <= 0 || !g_mduFwRsid[s] || !g_mdu.vsw_end[s]
        || now < g_mdu.vsw_end[s] || now >= g_mdu_fw_rend[s]
        || MdgOwnClipOn())
        return 0;
    int f = -1;
    for (size_t i = 0; i < CD_N; i++)
        if (CdMfIntro((int) i)
            && FNV64(CD_PAIRS[i].seg[0].att) == g_mduFwRsid[s]) {
            f = (int) i;
            break;
        }
    if (f < 0)
        return 0;
    *slot = s;
    *fin = f;
    *to_c = (int) ((int64_t) g_mdu_fw_rend[s] - 150 - (int64_t) now);
    return 1;
}
