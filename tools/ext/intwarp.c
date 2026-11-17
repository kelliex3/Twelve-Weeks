// intwarp.c - the slowed hold of the fist Interrogation (slowed down to space out the dialogue a little).
// Included from t2r-ext.c before explus.c (gx_melee_begin / ex_begin / the exec camera and paired.c use it).
//
// The 2013 fist-interrogate-enter pair (MELEE2013 kill 4.6 s) holds and shakes the victim between the slam (2.1-2.2 s) and the lifted head for the kill swing
// (4.3 s). That held middle section [IW_A, IW_B] of the clips (clip ms) is played at IW_RATE on BOTH halves (every client: the same function of the shared
// move clock), so the kill lands IW_EXTRA ms later (4.6 s -> 7.98 s); the slam and the kill swing run at the clips' normal speed.
//   clip domain  = what psync / the exec+ rows / the shaking foley / the authored camera track measure: the move clock g_ps.t0 / g_ex.t0 (and the copies g_ivx.t0
//                  g_ifo.t0 g_es.t0) are moved forward by the slow-down's lost time every frame (iw_tick, paired.c), so t0-relative time IS clip time.
//   real domain  = ticks / real ms: the gx kill_at / until / lock, the hold ring, the exec camera's weights and duration, the music cue, the voices (intvox.c
//                  schedules in real ms = clip ms + g_ivx.shift).
// Only the fist interrogation (the shiv pair is 2.5 s and keeps its speed).
// Round 2 (too stiff and slow at the beginning; realistic, not plainly slowed down): the rate follows the INVERSE OF THE MOTION ENERGY of the clips. IW_E[] = the
// summed per-joint position + 0.2 x rotation change per 100 ms frame of fist-interrogate-enter-left (attacker + victim halves, decoded 2013 patch, 3-tap smoothed, frame i centred
// at 100 i + 50 ms; the right clip is its mirror). In the two sections [350,1950] and [2400,4250] (eased in / out over IW_RAMP) a frame with the highest energy (>= IW_EHI: the kick,
// the roll-over, the climb, the grab) plays at 1, the quietest ones at IW_MMOV (0.8) and, where the pair is almost still (< IW_ESTAT: the hold after the slam), at IW_MSTAT (0.6).
// The question needs more time than the moving part has to give, so the settled mount (clip 1350..1650, the attacker's energy is lowest there) is a HOLD: rate down to IW_HOLD_R for 300 ms, 120 ms smoothstep in / out; round 3 (the pull-up looked slow): sections end at 1650 and the PULL-UP + slam (1650..2400) run at exactly 1
// (a still pose slowed reads as a pause, a moving limb slowed reads as slow motion). Real time = the integral of 1 / r (a table, 1 clip ms per entry, built once - the same on every
// client). The slam impact (FALL_HEAD 2167 / thud 2267), the kill swing (4300) and the final smash (FALL_HEAD 4967 / 5000) run at rate 1.
#define IW_N 7000
static const double IW_SEC[2][3] = { { 350.0, 1650.0, 0.20 }, { 2400.0, 4250.0, 0.60 } };       // clip ms from, to, lowest rate (log)

#define IW_RAMP 250.0
#define IW_ELO 2.0
#define IW_EHI 3.6
#define IW_ESTAT 1.6
#define IW_MMOV 0.72
#define IW_MSTAT 0.60
#define IW_HOLD_C 1500.0
#define IW_HOLD_L 300.0
#define IW_HOLD_E 120.0
#define IW_HOLD_R 0.20
static const float IW_E[] =
    { 1.285f, 1.818f, 2.774f, 3.089f, 2.889f, 2.744f, 2.65f, 2.424f, 2.22f,
2.277f, 2.302f, 2.325f, 2.676f, 2.805f, 2.533f, 2.33f, 2.025f, 1.821f, 1.845f, 2.191f,
2.877f,
    2.754f, 1.86f, 1.445f, 1.382f, 1.255f, 1.317f, 1.418f, 1.336f, 1.203f,
        1.053f, 0.906f, 0.848f, 0.815f, 0.745f, 0.705f, 0.827f, 1.56f,
        2.375f, 2.332f, 1.873f, 1.565f,
    1.582f, 1.611f, 1.426f, 1.348f, 1.607f, 2.257f, 2.929f, 2.922f
};

static double g_iwTab[IW_N + 1];       // real ms of the move at clip ms i
static int g_iwTabOk;
static double IwSs(double u)
{
    u = u < 0.0 ? 0.0 : u > 1.0 ? 1.0 : u;
    return u * u * (3.0 - 2.0 * u);
}

static double IwEnergy(double c)
{                               // motion energy at clip ms c
    int n = (int) (sizeof IW_E / sizeof *IW_E);
    double f = (c - 50.0) / 100.0;
    if (f <= 0.0)
        return IW_E[0];
    if (f >= n - 1)
        return IW_E[n - 1];
    int i = (int) f;
    return IW_E[i] + (IW_E[i + 1] - IW_E[i]) * (f - i);
}

static double IwRateClip(double c)
{                               // the playback rate at clip ms c
    double r = 1.0;
    for (int k = 0; k < 2; k++) {
        double a = IW_SEC[k][0], b = IW_SEC[k][1];
        if (c <= a || c >= b)
            continue;
        double w = IwSs((c - a) / IW_RAMP), w2 =
            IwSs((b - c) / IW_RAMP), e = IwEnergy(c);
        if (w2 < w)
            w = w2;
        double m = IW_MMOV - (IW_MMOV - IW_MSTAT) * IwSs((IW_ESTAT - e) / 0.6);        // the floor of this frame: 0.8 moving, 0.6 almost still
        double rr = m + (1.0 - m) * IwSs((e - IW_ELO) / (IW_EHI - IW_ELO));    // high energy -> 1, low energy -> the floor
        r = 1.0 - (1.0 - rr) * w;
    }
    double h0 = IW_HOLD_C - IW_HOLD_L * 0.5;
    if (c > h0 && c < h0 + IW_HOLD_L) {
        double w = IwSs((c - h0) / IW_HOLD_E), w2 =
            IwSs((h0 + IW_HOLD_L - c) / IW_HOLD_E);
        if (w2 < w)
            w = w2;
        double rh = 1.0 - (1.0 - IW_HOLD_R) * w;
        if (rh < r)
            r = rh;
    }
    return r;
}

static void IwBuild(void)
{
    if (g_iwTabOk)
        return;
    g_iwTab[0] = 0.0;
    for (int i = 1; i <= IW_N; i++)
        g_iwTab[i] = g_iwTab[i - 1] + 1.0 / IwRateClip(i - 0.5);
    g_iwTabOk = 1;
}

static double IwExtraTotal(void)
{
    IwBuild();
    return g_iwTab[IW_N] - (double) IW_N;
}                               // ms the whole warp adds (everything after the last section is shifted by it)

#define IW_EXTRA IwExtraTotal()
static struct {
    uint32_t uid;
    int on;
    double base;
    float rate;
} g_iw;                         // the run (psync uid), its unshifted t0, the rate set on both halves
static int IwName(const char *n)
{
    return n && !strncmp(n, "fist-interrogate", 16);
}

static double IwRealMs(double c)
{                               // clip ms -> real ms of the move
    IwBuild();
    if (c <= 0.0)
        return c;
    if (c >= IW_N)
        return g_iwTab[IW_N] + (c - IW_N);
    int i = (int) c;
    double f = c - i;
    return g_iwTab[i] + (g_iwTab[i + 1] - g_iwTab[i]) * f;
}

static double IwClipMs(double r)
{                               // real ms of the move -> clip ms
    IwBuild();
    if (r <= 0.0)
        return r;
    if (r >= g_iwTab[IW_N])
        return IW_N + (r - g_iwTab[IW_N]);
    int lo = 0, hi = IW_N;
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (g_iwTab[mid] <= r)
            lo = mid;
        else
            hi = mid;
    }
    return lo + (r - g_iwTab[lo]) / (g_iwTab[lo + 1] - g_iwTab[lo]);
}

static double IwExtraAt(double real_ms)
{
    return real_ms - IwClipMs(real_ms);
}                               // ms of move time lost to the slow-down by real ms `real_ms` of the move

static float IwRateReal(double r)
{
    double d = IwClipMs(r + 6.0) - IwClipMs(r);
    return d > 0.05 ? (float) (d / 6.0) : 1.0f;
}                               // playback rate at real ms r

static void IwCopiesShift(double d);  // execsp.c (after intvox / intfoley): moves g_ex.t0 and the timelines' copies of it
