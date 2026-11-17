// motion.c - the single Motion funnel (melee rework phase 1, step 1). BEHAVIOUR-PRESERVING.
//
// Every melee / pair write of a body's pose goes through mo_write() (set-object-locator, our set_obj_loc) or mo_teleport() (teleport-player,
// our teleport_player_to); every melee set-animation-speed through mo_rate_dc(); the render camera chain reports through mo_cam_*().
// With the defaults each wrapper forwards its arguments EXACTLY to the old call (same pointer, same values) and only records:
//   - per body, per frame: every write (source id, step vs the previous committed pose, step vs the engine's own pose, yaw step);
//   - frames in which more than one write hit the same body ("motion: <body> frame F: N writers [src a 0.12 m, src b 0.40 m]");
//   - per source: writes, max step, steps > 0.15 m, frames with > 1 writer, teleport sizes, pair table of who collides with whom;
//   - the same for animation speed writes on one (entity, layer, clip) in one frame ("motion: rate ...").
// A global speed cap exists but is OFF: t2r-motion-cap.txt next to the exe holds m/s (e.g. "6"); never applied to MO_F_HARD writes.
// Output: dev builds log every multi-writer frame; release builds only the per-match summary table (leaving a match: "motion: SUMMARY").
// Reading it: re/melee-motion-funnel.md (tools/ext) - or the header comment of mo_summary() below.
// Included early (t2r-ext.c, right after set_obj_loc's declaration) so every melee file can call it. Game thread only.
#ifndef T2R_MOTION_C
#define T2R_MOTION_C

#define MO_F_HARD 0x01u         // hard hand-off (death / respawn / intro class): never speed capped
#define MO_F_NOENG 0x02u        // hot path: skip the engine-pose lookup (step vs engine = -1)
#define MO_NSRC 160
#define MO_NBODY 16
#define MO_WMAX 8
#define MO_NPAIR 96
#define MO_NRATE 48

static const void *g_solOwnRa, *g_tpRa;      // (defined with their frames next to set_obj_loc)
typedef struct {
    const char *name;
    uint32_t writes, tps, big, multi, capped, esc, tp_big;
    float maxstep, maxeng, maxyaw, tpmax, tpsum;
    uint32_t rsets, rover;      // animation-speed writes / frames this source set a speed on a layer another source also set
} MoSrcT;
static MoSrcT g_moSrc[MO_NSRC];
static int g_moNsrc;
static int MoSrcReg(const char *name)
{
    for (int i = 0; i < g_moNsrc; i++)
        if (g_moSrc[i].name == name || !strcmp(g_moSrc[i].name, name))
            return i;
    if (g_moNsrc >= MO_NSRC)
        return MO_NSRC - 1;
    g_moSrc[g_moNsrc].name = name;
    return g_moNsrc++;
}

// MO_SRC("paired.tc_frame") = the source id of one call site (registered once per site)
#define MO_SRC(name) ({ static int mo_i_ = -1; if (mo_i_ < 0) mo_i_ = MoSrcReg(name); mo_i_; })

// MO_HERE() = the source id "<file>:<function>" of the call site: for helpers (dtl_speed, mstag_speed, psync_half_speed) that many callers share
static char g_moPool[MO_NSRC][64];
static int MoSrcHere(const char *file, const char *fn)
{
    const char *b = file;
    for (const char *c = file; *c; c++)
        if (*c == '/' || *c == 92)
            b = c + 1;
    char nm[64];
    size_t n = 0;
    while (b[n] && b[n] != '.' && n < 30) {
        nm[n] = b[n];
        n++;
    }
    snprintf(nm + n, sizeof nm - n, ":%s", fn);
    for (int i = 0; i < g_moNsrc; i++)
        if (!strcmp(g_moSrc[i].name, nm))
            return i;
    if (g_moNsrc >= MO_NSRC)
        return MO_NSRC - 1;
    memcpy(g_moPool[g_moNsrc], nm, sizeof nm);
    g_moSrc[g_moNsrc].name = g_moPool[g_moNsrc];
    return g_moNsrc++;
}

#define MO_HERE() ({ static int mo_h_ = -1; if (mo_h_ < 0) mo_h_ = MoSrcHere(__FILE__, __func__); mo_h_; })
// shared set-animation-speed helpers: every caller is its own source (the macro adds __FILE__:__func__)
static void DtlSpeedS(uint64_t ent, uint64_t sid, float v, int src);  // melee_duel_tl.c
#define DTL_SPEED(e, s, v) DtlSpeedS((e), (s), (v), MO_HERE())
static void MstagSpeedS(uint64_t clip, float sp, int src);    // t2r-ext.c (melee stagger / swing clip speed)
#define mstag_speed(c, s) MstagSpeedS((c), (s), MO_HERE())

typedef struct {
    int src;
    float step, eng, yaw;
    int tp;
} MoWT;
typedef struct {
    uint64_t h;
    int valid;
    float pos[3], yaw;
    uint64_t pframe;            // the last committed pose written through the funnel
    LARGE_INTEGER pt;
    uint64_t frame;
    int nw;
    MoWT w[MO_WMAX];
} MoBodyT;
static MoBodyT g_moBody[MO_NBODY];
static struct {
    int a, b;
    uint32_t n;
    float maxstep;
} g_mo_pair[MO_NPAIR];
static int g_moNpair;
static uint32_t g_mo_multi_frames, g_mo_multi_own, g_mo_writes,
    g_mo_rate_over;
static uint64_t g_moFrame0;
static float g_moCap;          // m/s, 0 = off (t2r-motion-cap.txt)
static int g_moEsc;            // set by set_obj_loc when it turned a >1.5 m step into teleport-player
static int g_moLastSolSrc = -1, g_mo_last_tp_src = -1;       // melee_duel_tl.c SNAP lines name them
static double g_moQpfInv;
#ifdef T2R_RELEASE
#define MO_VERBOSE (g_debugMode)
#else
#define MO_VERBOSE 1
#endif

static const char *MoBodyName(uint64_t h, char *b, size_t n)
{
    if (h == SID_PLAYER)
        return "own";
    snprintf(b, n, "ent%06llx", (unsigned long long) (h & 0xffffff));
    return b;
}

static double MoSecs(LARGE_INTEGER t)
{
    if (g_moQpfInv == 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_moQpfInv = 1.0 / (double) f.QuadPart;
    }
    return (double) t.QuadPart * g_moQpfInv;
}

static float MoYawQ(const float *q)
{                               // q = xyzw; world yaw (deg) of the +Z axis
    float fx = 2.0f * (q[0] * q[2] + q[3] * q[1]), fz =
        1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]);
    return __builtin_atan2f(fx, fz) * 57.2957795f;
}

static float MoDyaw(float a, float b)
{
    float d = __builtin_remainderf(a - b, 360.0f);
    return d < 0 ? -d : d;
}

static MoBodyT *MoBody(uint64_t h)
{
    MoBodyT *oldest = &g_moBody[0];
    for (int i = 0; i < MO_NBODY; i++) {
        if (g_moBody[i].h == h)
            return &g_moBody[i];
        if (!g_moBody[i].h) {
            oldest = &g_moBody[i];
            break;
        }
        if (g_moBody[i].pframe < oldest->pframe)
            oldest = &g_moBody[i];
    }
    memset(oldest, 0, sizeof *oldest);
    oldest->h = h;
    return oldest;
}

static void MoPairAdd(int a, int b, float step)
{
    if (a > b) {
        int t = a;
        a = b;
        b = t;
    }
    for (int i = 0; i < g_moNpair; i++)
        if (g_mo_pair[i].a == a && g_mo_pair[i].b == b) {
            g_mo_pair[i].n++;
            if (step > g_mo_pair[i].maxstep)
                g_mo_pair[i].maxstep = step;
            return;
        }
    if (g_moNpair >= MO_NPAIR)
        return;
    g_mo_pair[g_moNpair].a = a;
    g_mo_pair[g_moNpair].b = b;
    g_mo_pair[g_moNpair].n = 1;
    g_mo_pair[g_moNpair].maxstep = step;
    g_moNpair++;
}

// the frame of a body is over: count the writers, log a frame with more than one write
static void MoFlush(MoBodyT *r)
{
    if (r->nw > 1) {
        g_mo_multi_frames++;
        if (r->h == SID_PLAYER)
            g_mo_multi_own++;
        int seen[MO_WMAX], ns = 0;
        for (int i = 0; i < r->nw; i++) {
            int dup = 0;
            for (int j = 0; j < ns; j++)
                if (seen[j] == r->w[i].src)
                    dup = 1;
            if (!dup) {
                seen[ns++] = r->w[i].src;
                g_moSrc[r->w[i].src].multi++;
            }
        }
        for (int i = 0; i < ns; i++)
            for (int j = i + 1; j < ns; j++) {
                float st = 0;
                for (int k = 0; k < r->nw; k++)
                    if (r->w[k].src == seen[i] || r->w[k].src == seen[j])
                        st = fmaxf(st,
                                   r->w[k].step >
                                   0 ? r->w[k].step : r->w[k].eng);
                MoPairAdd(seen[i], seen[j], st);
            }
        if (MO_VERBOSE) {
            char bn[24], line[700];
            int n =
                snprintf(line, sizeof line,
                         "motion: %s frame %llu: %d writers [",
                         MoBodyName(r->h, bn, sizeof bn),
                         (unsigned long long) r->frame, r->nw);
            for (int i = 0; i < r->nw && n < (int) sizeof line - 80; i++) {
                const MoWT *w = &r->w[i];
                n += snprintf(line + n, sizeof line - n, "%s%s%s %.2f m",
                              i ? ", " : "", g_moSrc[w->src].name,
                              w->tp ? " TP" : "",
                              w->step >= 0 ? w->step : 0.0f);
                if (w->eng >= 0)
                    n += snprintf(line + n, sizeof line - n,
                                  " (eng %.2f, yaw %.0f)", w->eng, w->yaw);
            }
            snprintf(line + n, sizeof line - n, "]");
            Xlog("%s", line);
        }
    }
    r->nw = 0;
}

// records one write; pos may be changed by the (default OFF) speed cap: returns 1 then
static int MoNote(uint64_t body, float *pos, const float *quat, int src,
                   unsigned fl, int tele)
{
    MoBodyT *r = MoBody(body);
    uint64_t fr = g_frame ? g_frame : 1;
    if (r->nw && r->frame != fr)
        MoFlush(r);
    if (!r->nw)
        r->frame = fr;
    if (!g_moFrame0)
        g_moFrame0 = fr;
    MoSrcT *s = &g_moSrc[src];
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    int changed = 0;
    float step = -1.0f, eng = -1.0f, yaw = 0.0f, dyaw = 0.0f;
    float yq = quat ? MoYawQ(quat) : 0.0f;
    int prev_ok = r->valid && fr - r->pframe <= 8;
    if (pos) {
        if (prev_ok) {
            float dx = pos[0] - r->pos[0], dy = pos[1] - r->pos[1], dz =
                pos[2] - r->pos[2];
            step = __builtin_sqrtf(dx * dx + dy * dy + dz * dz);
            // the (default OFF) speed cap: this write may move the body at most cap m/s from the last committed pose
            if (g_moCap > 0.0f && !(fl & MO_F_HARD)) {
                double dt = MoSecs(now) - MoSecs(r->pt);
                if (dt < 1.0 / 240.0)
                    dt = 1.0 / 240.0;
                float mx = g_moCap * (float) dt;
                if (dt < 0.5 && step > mx && step > 1e-4f) {
                    float k = mx / step;
                    pos[0] = r->pos[0] + dx * k;
                    pos[1] = r->pos[1] + dy * k;
                    pos[2] = r->pos[2] + dz * k;
                    changed = 1;
                    s->capped++;
                    static int ncap;
                    if (LOGCAP(ncap, 40))
                        Xlog("motion: cap %.1f m/s held %s %s from %.2f m to %.2f m", g_moCap, body == SID_PLAYER ? "own" : "puppet", s->name, step, mx);
                    step = mx;
                }
            }
        }
        if (!(fl & MO_F_NOENG)) {
            float lb[16];
            const float *ep =
                body ==
                SID_PLAYER ? PlayerLocator(lb) : PuppetLocator(body, lb);
            if (ep && __builtin_isfinite(ep[0])
                && __builtin_isfinite(ep[1]) && __builtin_isfinite(ep[2])
                && __builtin_fabsf(ep[0]) < 1e6f) {
                float dx = pos[0] - ep[0], dy = pos[1] - ep[1], dz =
                    pos[2] - ep[2];
                eng = __builtin_sqrtf(dx * dx + dy * dy + dz * dz);
                if (quat)
                    yaw = MoDyaw(yq, MoYawQ(ep + 4));
            }
        }
        if (prev_ok && quat)
            dyaw = MoDyaw(yq, r->yaw);
        r->valid = 1;
        r->pos[0] = pos[0];
        r->pos[1] = pos[1];
        r->pos[2] = pos[2];
        if (quat)
            r->yaw = yq;
        r->pframe = fr;
        r->pt = now;
    }
    s->writes++;
    g_mo_writes++;
    if (step > s->maxstep)
        s->maxstep = step;
    if (eng > s->maxeng)
        s->maxeng = eng;
    if (yaw > s->maxyaw)
        s->maxyaw = yaw;
    if (dyaw > s->maxyaw)
        s->maxyaw = dyaw;
    if (step > 0.15f)
        s->big++;
    if (tele) {
        float z = eng >= 0 ? eng : step;
        if (z < 0)
            z = 0;
        s->tps++;
        s->tpsum += z;
        if (z > s->tpmax)
            s->tpmax = z;
        if (z > 0.5f)
            s->tp_big++;
    }
    if (r->nw < MO_WMAX) {
        MoWT *w = &r->w[r->nw++];
        w->src = src;
        w->step = step;
        w->eng = eng;
        w->yaw = yaw > dyaw ? yaw : dyaw;
        w->tp = tele;
    }
    return changed;
}

// ---- set-object-locator: forwards bf to set_obj_loc unchanged (same pointer) unless the cap is on and trims the position
static void *MoPrep(uint64_t body, void *bfp, int src, unsigned flags,
                     PsyncBf *cp)
{
    const PsyncBf *bf = (const PsyncBf *) bfp;
    int ok = bf && bf->magic[0] == 0x7fccccccu
        && bf->magic[1] == 0x7fccccccu && !bf->binding;
    if (ok) {
        float p[3] = { bf->local[0], bf->local[1], bf->local[2] };
        if (__builtin_isfinite(p[0]) && __builtin_isfinite(p[1])
            && __builtin_isfinite(p[2]) && __builtin_fabsf(p[0]) < 1e6f) {
            if (MoNote(body, p, bf->local + 4, src, flags, 0)) {
                *cp = *bf;
                cp->local[0] = p[0];
                cp->local[1] = p[1];
                cp->local[2] = p[2];
                return cp;
            }
        } else
            MoNote(body, NULL, NULL, src, flags, 0);
    } else
        MoNote(body, NULL, NULL, src, flags, 0);
    return bfp;
}

static __attribute__((noinline))
void mo_write(uint64_t body, void *bfp, int src, unsigned flags)
{
    const void *ra = __builtin_return_address(0);
    PsyncBf cp;
    void *use = MoPrep(body, bfp, src, flags, &cp);
    g_moEsc = 0;
    SetObjLoc(body, use);
    if (body == SID_PLAYER) {
        g_solOwnRa = ra;
        g_moLastSolSrc = src;
    }                           // (the SNAP lines name the CALLER, not this wrapper)
    if (g_moEsc) {
        g_moSrc[src].esc++;
        g_moSrc[src].tps++;
        g_moEsc = 0;
        g_tpRa = ra;
        g_mo_last_tp_src = src;
    }
}

// the direct dc_call(set-object-locator) of psync_drive (no set_obj_loc checks, as before): a = {body, &bf}; returns the call's result
static uint64_t MoWriteDc(uint64_t *a, int src, unsigned flags)
{
    PsyncBf cp;
    void *use = MoPrep(a[0], (void *) (uintptr_t) a[1], src, flags, &cp);
    if (use != (void *) (uintptr_t) a[1]) {
        uint64_t b2[2] = { a[0], (uint64_t) (uintptr_t) use };
        return DcCall(0xad3960fee28b6f8eULL /* set-object-locator */ , 2,
                       b2);
    }
    return DcCall(0xad3960fee28b6f8eULL /* set-object-locator */ , 2, a);
}

// ---- teleport-player (our body only): forwards pos / rot unchanged unless the cap trims pos
static __attribute__((noinline))
void mo_teleport(const float *pos, const float *rot, int src,
                 unsigned flags)
{
    const void *ra = __builtin_return_address(0);
    float p[3];
    const float *use = pos;
    if (pos && rot && __builtin_isfinite(pos[0])
        && __builtin_isfinite(pos[1]) && __builtin_isfinite(pos[2])
        && __builtin_fabsf(pos[0]) < 1e6f) {
        p[0] = pos[0];
        p[1] = pos[1];
        p[2] = pos[2];
        if (MoNote(SID_PLAYER, p, rot, src, flags, 1))
            use = p;            // (p is 3 floats: teleport_player_to copies 12 bytes)
    } else
        MoNote(SID_PLAYER, NULL, NULL, src, flags, 1);
    TeleportPlayerTo(use, rot);
    g_tpRa = ra;
    g_mo_last_tp_src = src;
}

// ---- animation speed: forwards b = {entity, f32 rate bits, layer, clip} to set-animation-speed unchanged
static struct {
    uint64_t ent, clip;
    int layer;
    uint64_t frame;
    int n;
    int src[4];
    float rate[4];
} g_mo_rate[MO_NRATE];
static void MoRateFlush(int i)
{
    typeof(g_mo_rate[0]) * e = &g_mo_rate[i];
    if (e->n > 1) {
        int seen[4], ns = 0;
        for (int a = 0; a < e->n; a++) {
            int d = 0;
            for (int j = 0; j < ns; j++)
                if (seen[j] == e->src[a])
                    d = 1;
            if (!d)
                seen[ns++] = e->src[a];
        }
        if (ns > 1) {
            g_mo_rate_over++;
            for (int a = 0; a < ns; a++)
                g_moSrc[seen[a]].rover++;
            for (int a = 0; a < ns; a++)
                for (int c = a + 1; c < ns; c++)
                    MoPairAdd(seen[a] + 1000, seen[c] + 1000, 0);     // (+1000: rate pairs, printed separately)
            if (MO_VERBOSE) {
                char line[500];
                int n =
                    snprintf(line, sizeof line,
                             "motion: rate ent %llx layer %d clip %llx frame %llu: %d setters [",
                             (unsigned long long) e->ent, e->layer,
                             (unsigned long long) e->clip,
                             (unsigned long long) e->frame, e->n);
                for (int a = 0; a < e->n && n < (int) sizeof line - 60;
                     a++)
                    n += snprintf(line + n, sizeof line - n, "%s%s x%.2f",
                                  a ? ", " : "", g_moSrc[e->src[a]].name,
                                  e->rate[a]);
                snprintf(line + n, sizeof line - n, "]");
                Xlog("%s", line);
            }
        }
    }
    e->n = 0;
}

static uint64_t MoRateDc(uint64_t *b, int src)
{
    uint64_t fr = g_frame ? g_frame : 1;
    float rate;
    memcpy(&rate, &b[1], 4);
    int slot = -1, old = 0;
    for (int i = 0; i < MO_NRATE; i++) {
        if (g_mo_rate[i].ent == b[0] && g_mo_rate[i].layer == (int) b[2]
            && g_mo_rate[i].clip == b[3]) {
            slot = i;
            break;
        }
        if (g_mo_rate[i].frame < g_mo_rate[old].frame)
            old = i;
    }
    if (slot < 0) {
        slot = old;
        if (g_mo_rate[slot].n)
            MoRateFlush(slot);
        memset(&g_mo_rate[slot], 0, sizeof g_mo_rate[slot]);
        g_mo_rate[slot].ent = b[0];
        g_mo_rate[slot].layer = (int) b[2];
        g_mo_rate[slot].clip = b[3];
    }
    if (g_mo_rate[slot].n && g_mo_rate[slot].frame != fr)
        MoRateFlush(slot);
    g_mo_rate[slot].frame = fr;
    if (g_mo_rate[slot].n < 4) {
        int n = g_mo_rate[slot].n++;
        g_mo_rate[slot].src[n] = src;
        g_mo_rate[slot].rate[n] = rate;
    }
    g_moSrc[src].rsets++;
    return DcCall(FNV64("set-animation-speed"), 4, b);
}

// ---- camera: who changes the shown view. Stages of the render filter chain report their own step (in -> out), natives are noted by name.
//   camwrite: frame F view step yaw +Y pitch +P deg | stages: <name> yaw/pitch, ... | engine step ... | natives: ...
#define MO_CSTAGES 12
static struct {
    const char *name;
    float dy, dp;
    int sub;
    float dm;
} g_mo_cs[MO_CSTAGES];          // dm = the stage's position step (m), camdiag
static int g_moNcs;
static const char *g_moCnat[2][8];
static int g_moNcnat[2], g_mo_cnat_ix;
static uint64_t g_mo_cnat_frame, g_mo_cnat_fr[2];
static int g_moCamOk;
static float g_mo_cam_yaw, g_mo_cam_pitch, g_mo_eng_dy, g_mo_eng_dp;
static float g_mo_cam_ly, g_mo_cam_lp;
static LARGE_INTEGER g_moCamT;
static uint32_t g_moCamEvents;
static float g_moCamMax;
static float MoPitchQ(const float *q)
{                               // q = xyzw; pitch (deg) of the +Z axis
    float fy = 2.0f * (q[1] * q[2] - q[3] * q[0]);
    return __builtin_asinf(fy > 1.0f ? 1.0f : fy <
                           -1.0f ? -1.0f : fy) * 57.2957795f;
}

static float MoWrap180(float a)
{
    return __builtin_remainderf(a, 360.0f);
}

static void MoCamNote(const char *what)
{                               // an engine-camera native (force target, fov, aim override ...) issued from a tick
    uint64_t fr = g_frame ? g_frame : 1;
    if (g_mo_cnat_frame != fr) {
        g_mo_cnat_ix ^= 1;
        g_moNcnat[g_mo_cnat_ix] = 0;
        g_mo_cnat_frame = fr;
        g_mo_cnat_fr[g_mo_cnat_ix] = fr;
    }
    int n = g_moNcnat[g_mo_cnat_ix];
    for (int i = 0; i < n; i++)
        if (g_moCnat[g_mo_cnat_ix][i] == what)
            return;
    if (n < 8) {
        g_moCnat[g_mo_cnat_ix][n] = what;
        g_moNcnat[g_mo_cnat_ix] = n + 1;
    }
}

static uint64_t MoCamSid(const char *name, uint64_t sid)
{
    MoCamNote(name);
    return sid;
}                               // dc_call(mo_cam_sid("native", fnv64("native")), ...): the engine-camera natives are listed in the camwrite line

static void MoCamBegin(const float *loc)
{                               // the engine camera this frame (view 0 only is judged)
    g_moNcs = 0;
    if (!loc || IsBadReadPtr(loc, 32))
        return;
    float y = MoYawQ(loc + 4), p = MoPitchQ(loc + 4);
    g_mo_eng_dy = g_moCamOk ? MoWrap180(y - g_mo_cam_ly) : 0.0f;
    g_mo_eng_dp = g_moCamOk ? p - g_mo_cam_lp : 0.0f;
    g_mo_cam_ly = y;
    g_mo_cam_lp = p;
}

static void MoCamStage(const char *name, const float *in,
                         const float *out, int sub)
{                               // in / out = 8-float camera poses
    if (!in || !out || in == out || g_moNcs >= MO_CSTAGES
        || IsBadReadPtr(in, 32) || IsBadReadPtr(out, 32))
        return;
    float dy = MoWrap180(MoYawQ(out + 4) - MoYawQ(in + 4)), dp =
        MoPitchQ(out + 4) - MoPitchQ(in + 4);
    g_mo_cs[g_moNcs].name = name;
    g_mo_cs[g_moNcs].dy = dy;
    g_mo_cs[g_moNcs].dp = dp;
    g_mo_cs[g_moNcs].sub = sub;
    {
        float ex = out[0] - in[0], ey = out[1] - in[1], ez =
            out[2] - in[2];
        float dm = __builtin_sqrtf(ex * ex + ey * ey + ez * ez);
        g_mo_cs[g_moNcs].dm = dm == dm ? dm : 0.0f;
    }
    g_moNcs++;
}

static void MoCamSub(const char *name, float dyaw_deg, float dpitch_deg)
{                               // a sub-writer inside a stage that knows its own step
    if (g_moNcs >= MO_CSTAGES
        || (__builtin_fabsf(dyaw_deg) < 0.05f
            && __builtin_fabsf(dpitch_deg) < 0.05f))
        return;
    g_mo_cs[g_moNcs].name = name;
    g_mo_cs[g_moNcs].dy = dyaw_deg;
    g_mo_cs[g_moNcs].dp = dpitch_deg;
    g_mo_cs[g_moNcs].sub = 1;
    g_mo_cs[g_moNcs].dm = 0.0f;
    g_moNcs++;
}

static void MoCamEnd(const float *shown, int view)
{                               // shown = the final camera of this frame; logs when its step > 10 deg
    if (view != 0 || !shown || IsBadReadPtr(shown, 32))
        return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float y = MoYawQ(shown + 4), p = MoPitchQ(shown + 4);
    if (!(__builtin_isfinite(y) && __builtin_isfinite(p)))
        return;
    double dt = g_moCamOk ? MoSecs(now) - MoSecs(g_moCamT) : 9.0;
    if (g_moCamOk && dt > 0.0 && dt < 0.25) {
        float dy = MoWrap180(y - g_mo_cam_yaw), dp = p - g_mo_cam_pitch;
        float big = fmaxf(__builtin_fabsf(dy), __builtin_fabsf(dp));
        if (big > g_moCamMax)
            g_moCamMax = big;
        if (big > 10.0f && ++g_moCamEvents <= 300) {  // capped (mouse flicks > 10 deg / frame are normal; the summary still counts them all below)
            char line[700];
            int n =
                snprintf(line, sizeof line,
                         "camwrite: frame %llu displayed step yaw %+.1f pitch %+.1f deg | engine %+.1f/%+.1f |",
                         (unsigned long long) g_frame, dy, dp, g_mo_eng_dy,
                         g_mo_eng_dp);
            for (int i = 0; i < g_moNcs && n < (int) sizeof line - 80;
                 i++)
                if (__builtin_fabsf(g_mo_cs[i].dy) >= 0.3f
                    || __builtin_fabsf(g_mo_cs[i].dp) >= 0.3f)
                    n += snprintf(line + n, sizeof line - n,
                                  " %s%s %+.1f/%+.1f,",
                                  g_mo_cs[i].sub ? "~" : "",
                                  g_mo_cs[i].name, g_mo_cs[i].dy,
                                  g_mo_cs[i].dp);
            n += snprintf(line + n, sizeof line - n, " natives:");
            for (int w = 0; w < 2; w++)
                for (int i = 0;
                     g_mo_cnat_fr[w] + 2 >= (g_frame ? g_frame : 1)
                     && i < g_moNcnat[w] && n < (int) sizeof line - 40;
                     i++)
                    n += snprintf(line + n, sizeof line - n, " %s",
                                  g_moCnat[w][i]);
            Xlog("%s", line);
        }
    }
    g_moCamOk = 1;
    g_mo_cam_yaw = y;
    g_mo_cam_pitch = p;
    g_moCamT = now;
}

// ---- the per-match summary (one xlog line per row, never rate limited). Columns:
//   writes       calls through the funnel from this source
//   max/eng      largest step (m) vs the previous committed pose / vs the engine's own pose at the time of the call
//   >0.15        writes whose step vs the previous committed pose was over 0.15 m
//   multi        frames in which this source's write shared the body with a write of ANOTHER source (the conflicts)
//   tp n/max/sum teleport-player calls (incl. set-object-locator steps > 1.5 m escalated by set_obj_loc), largest, sum of sizes (m)
//   yaw          largest yaw step (deg) vs previous committed / engine
//   cap          writes trimmed by the speed cap (0 with the cap off)
//   rate s/ov    set-animation-speed calls / frames another source set a speed on the same (entity, layer, clip)
static void MoSummary(const char *why)
{
    int was = g_xlogNolimit;
    g_xlogNolimit = 1;
    for (int i = 0; i < MO_NBODY; i++)
        if (g_moBody[i].nw)
            MoFlush(&g_moBody[i]);
    for (int i = 0; i < MO_NRATE; i++)
        if (g_mo_rate[i].n)
            MoRateFlush(i);
    Xlog("motion: SUMMARY (%s) frames %llu..%llu: %u writes, %u multi-writer frames (own body %u), %u rate overlaps, camera steps > 10 deg %u (max %.1f)", why, (unsigned long long) g_moFrame0, (unsigned long long) g_frame, g_mo_writes, g_mo_multi_frames, g_mo_multi_own, g_mo_rate_over, g_moCamEvents, g_moCamMax);
    Xlog("motion: src                          writes   max/eng m   >0.15  multi  tp n/max/sum         yaw  cap  rate s/ov");
    for (int i = 0; i < g_moNsrc; i++) {
        const MoSrcT *s = &g_moSrc[i];
        if (!s->writes && !s->rsets)
            continue;
        Xlog("motion: %-28s %6u  %5.2f/%5.2f  %5u  %5u  %3u/%4.2f/%5.1f  %5.0f  %3u  %u/%u", s->name, s->writes, s->maxstep, s->maxeng, s->big, s->multi, s->tps, s->tpmax, s->tpsum, s->maxyaw, s->capped, s->rsets, s->rover);
    }
    for (int i = 0; i < g_moNpair; i++) {      // collisions, biggest first (selection by repeated max: tiny table)
        int best = -1;
        for (int j = 0; j < g_moNpair; j++)
            if (g_mo_pair[j].n
                && (best < 0 || g_mo_pair[j].n > g_mo_pair[best].n))
                best = j;
        if (best < 0 || i >= 24)
            break;
        int a = g_mo_pair[best].a, b = g_mo_pair[best].b, rate = a >= 1000;
        if (rate) {
            a -= 1000;
            b -= 1000;
        }
        Xlog("motion: %s %s + %s: %u frames (max step %.2f m)",
             rate ? "rate-overlap" : "collision", g_moSrc[a].name,
             g_moSrc[b].name, g_mo_pair[best].n, g_mo_pair[best].maxstep);
        g_mo_pair[best].n = 0;  // (printed: the table is reset right after anyway)
    }
    g_xlogNolimit = was;
}

static void MoReset(void)
{
    for (int i = 0; i < g_moNsrc; i++) {
        const char *nm = g_moSrc[i].name;
        memset(&g_moSrc[i], 0, sizeof g_moSrc[i]);
        g_moSrc[i].name = nm;
    }
    memset(g_mo_pair, 0, sizeof g_mo_pair);
    g_moNpair = 0;
    g_mo_multi_frames = g_mo_multi_own = g_mo_writes = g_mo_rate_over = 0;
    g_moFrame0 = 0;
    g_moCamEvents = 0;
    g_moCamMax = 0;
}

// once per frame (t2r_tick): closes finished frames, re-reads the cap file, match start / end (summary on leaving a match)
static void MoTick(void)
{
    uint64_t fr = g_frame ? g_frame : 1;
    for (int i = 0; i < MO_NBODY; i++)
        if (g_moBody[i].nw && g_moBody[i].frame < fr)
            MoFlush(&g_moBody[i]);
    for (int i = 0; i < MO_NRATE; i++)
        if (g_mo_rate[i].n && g_mo_rate[i].frame < fr)
            MoRateFlush(i);
    static uint64_t next_ms;
    uint64_t t_ms = GetTickCount64();
    if (t_ms >= next_ms) {
        next_ms = t_ms + 2000;
        FILE *f = ExeSideOpen("t2r-motion-cap.txt", "r");
        float c = 0.0f;
        if (f) {
            if (fscanf(f, "%f", &c) != 1 || !(c > 0.0f && c < 1000.0f))
                c = 0.0f;
            fclose(f);
        }
        if (c != g_moCap) {
            Xlog("motion: speed cap %s",
                 c > 0.0f ? "ON (t2r-motion-cap.txt)" : "off");
            if (c > 0.0f)
                Xlog("motion: speed cap %.1f m/s", c);
            g_moCap = c;
        }
    }
    static int in_match;
    static uint64_t since, last_dump;
    int now_in = MatchSubstate() == 6;
    if (now_in && !in_match) {
        MoReset();
        since = t_ms;
        last_dump = t_ms;
    }
    if (!now_in && in_match && g_mo_writes > 0)
        MoSummary("left the match");
    in_match = now_in;
#ifndef T2R_RELEASE
    if (now_in && t_ms - last_dump > 180000 && g_mo_writes > 0) {
        last_dump = t_ms;
        MoSummary("dev, every 180 s");
    }
#endif
    (void) since;
}
#endif
