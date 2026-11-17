// pairshift.c - "pws": the paired-move anti-wall SHIFT (instead of stopping the player(s) from being animated or even put in an
// animation in the first place, both shift against the wall so nothing of them clips, and the bodies move THROUGHOUT the animation, not only at the start).
// Included in t2r-ext.c right after pg_gate_base (the path gate); paired.c (psync_start / tick / wire / tc_frame) uses the profile built here.
//
// WHAT IT DOES. pg_gate() is now this wrapper around the old gate (pg_gate_base). Before a pair is begun (every caller that already asks the gate: duel hits,
// parry, pin escape, gun / melee / TLOU2 executions, shiv, upgraded finishers, one-shots, interrogation) it
//  1. reads the authored root path of BOTH halves from the clips (psync_pair_end at PWS_NK = 6 key times over the move) -> up to PWS_NP unique path points;
//  2. kicks a ring of 8 chest-height static-world rays (mask 0x1b07, grass passes) round each point (reach PWS_REACH) - read next frame like every ray here;
//  3. the hit points are world-fixed obstacle points; a DP over a 0.10 m grid of shifts (disc of PWS_MAX, or a line along the wall for wall pairs) picks, for
//     each key time, the shift S_k that keeps both halves' path points >= PWS_R_PLAN from every hit, minimising |S_k| + 2 |S_k - S_k-1| (small, smooth);
//  4. verifies: rings again round the SHIFTED points (new hits are added to the obstacle set, up to 3 rounds), the old gate runs on the shifted ap too;
//  5. publishes the profile; psync_start() takes it: ap := ap + S_0 (the START shift, goes on the wire as the ap itself = old clients get it too) and the dynamic
//     part D(t) = S(t) - S_0 (Catmull-Rom through the keys, t = move time / tend) rides in a trailer after the 84-byte START (see paired.c): every client
//     adds D(t) to both halves' placement each frame (puppets: psync_place; our own engine-driven body: tc_frame's set-object-locator, same mechanism as the
//     travel clamp). Deterministic on both screens because only keys + move time are used; no per-frame ray decides anything.
// A pair is refused only when no shift up to PWS_MAX clears it AND the old gate also says blocked; an unsolvable ring with a clear old gate plays unshifted (as
// before, the old hold / clamp stay as the net for it). A pair that carries a verified profile (or verified clear) gets no 'pair wall stop' hold.
// t2r-no-pair-shift.txt next to the exe = the old behaviour exactly. Logs: 'pair shift:'.
// All numbers below marked (guess) are our own tuning, not 2013 / SP data: R_PLAN 0.40 = the capsule beyond-step 0.40 of the old pair wall stop, R_VER 0.32 = TC_R 0.28 + a
// 4 cm margin so planning and checking do not chase each other, MAX 1.0 m, REACH 0.95 m, 8 rays, 6 keys, 10 points.
#define PWS_NK 6
#define PWS_NP 10
#define PWS_RAYS 8
#define PWS_NH 256
#define PWS_R_PLAN 0.40f        // (guess)
#define PWS_R_VER 0.32f         // (guess)
#define PWS_MAX 1.0f            // (guess) the sane limit of the shift
#define PWS_REACH 0.95f         // (guess)
#define PWS_STEP 0.10f
static float PsyncDur(uint64_t ent, uint64_t sid);     // paired.c
static int PwsOff(void)
{
    static int off = -1;
    static uint64_t chk;
    if (off < 0 || GetTickCount64() - chk > 2000) {
        FILE *f = ExeSideOpen("t2r-no-pair-shift.txt", "r");
        off = f != NULL;
        if (f)
            fclose(f);
        chk = GetTickCount64();
    }
    return off;
}

typedef struct {
    int n;
    float p[PWS_NP][3];
    int idx[PWS_NK][2];
} PwsPts;
typedef struct {
    int on, state, round, wall;
    uint32_t slot;
    uint64_t key, pkey, kick_f, done_f;
    int verdict, verified;
    float ap0[7], pv[2], ml[2], tend, fwd[2], tan[2];
    char tag[24];
    PwsPts K, V;
    uint64_t j[PWS_NP][PWS_RAYS];
    int jn;                     // K = path points at ap0; V = the points being verified (shifted)
    float H[PWS_NH][2];
    int nh;
    float S[PWS_NK][2];
} PwsEnt;
static PwsEnt g_pws[8];
typedef struct {
    int on;
    uint32_t slot;
    uint64_t key, pkey;
    float ap0[7];
    uint64_t f;
    float S[PWS_NK][2], tend;
    int dyn, verified;
} PwsFin;
static PwsFin g_pwsf[8];
static void PwsDrain(PwsEnt *e)
{
    for (int i = 0; i < PWS_NP; i++)
        for (int d = 0; d < PWS_RAYS; d++)
            if (e->j[i][d]) {
                RayT(e->j[i][d]);
                e->j[i][d] = 0;
            }
}

static int PwsAdd(PwsPts *q, const float *p)
{
    for (int i = 0; i < q->n; i++) {
        float dx = q->p[i][0] - p[0], dz = q->p[i][2] - p[2];
        if (dx * dx + dz * dz < 0.25f * 0.25f
            && __builtin_fabsf(q->p[i][1] - p[1]) < 0.6f)
            return i;
    }
    if (q->n < PWS_NP) {
        memcpy(q->p[q->n], p, 12);
        return q->n++;
    }
    int b = 0;
    float bd = 1e9f;            // full: the nearest existing point stands in
    for (int i = 0; i < q->n; i++) {
        float dx = q->p[i][0] - p[0], dz = q->p[i][2] - p[2], d =
            dx * dx + dz * dz;
        if (d < bd) {
            bd = d;
            b = i;
        }
    }
    return b;
}

// the shifted copy of K for verification (point i of V = a (key, half) point of K plus that key's S)
static void PwsShifted(const PwsEnt *e, PwsPts *v)
{
    memset(v, 0, sizeof *v);
    for (int k = 0; k < PWS_NK; k++)
        for (int h = 0; h < 2; h++) {
            v->idx[k][h] = -1;
            if (e->K.idx[k][h] < 0)
                continue;
            float p[3];
            memcpy(p, e->K.p[e->K.idx[k][h]], 12);
            p[0] += e->S[k][0];
            p[2] += e->S[k][1];
            v->idx[k][h] = PwsAdd(v, p);
        }
}

static void PwsKick(PwsEnt *e, const PwsPts *q)
{
    PwsDrain(e);
    for (int i = 0; i < q->n; i++)
        for (int d = 0; d < PWS_RAYS; d++) {
            float ang = d * (6.2831853f / PWS_RAYS), ux =
                __builtin_cosf(ang), uz = __builtin_sinf(ang);
            float a[3] =
                { q->p[i][0], q->p[i][1] + 1.0f, q->p[i][2] }, b[3] =
                { a[0] + ux * PWS_REACH, a[1], a[2] + uz * PWS_REACH };
            e->j[i][d] = WorldRayKickM(a, b, 0, 0x1b07);
        }
    e->jn = q->n;
    e->kick_f = g_frame ? g_frame : 1;
}

// reads the ring of the last kick: appends the obstacle points; returns the number of rays that gave an answer
static int PwsRead(PwsEnt *e, const PwsPts *q)
{
    int got = 0, added = 0;
    for (int i = 0; i < q->n; i++)
        for (int d = 0; d < PWS_RAYS; d++) {
            uint64_t jj = e->j[i][d];
            e->j[i][d] = 0;
            if (!jj)
                continue;
            g_rayErr = 0;
            float t = RayT(jj);
            if (g_rayErr)
                continue;
            got++;
            if (!(t > 0.02f && t < 0.999f))
                continue;       // a miss, or a ray that started inside something (no direction): nothing to push from
            float ang = d * (6.2831853f / PWS_RAYS), hx =
                q->p[i][0] + __builtin_cosf(ang) * t * PWS_REACH, hz =
                q->p[i][2] + __builtin_sinf(ang) * t * PWS_REACH;
            if ((hx - e->pv[0]) * (hx - e->pv[0]) +
                (hz - e->pv[1]) * (hz - e->pv[1]) < 0.7f * 0.7f)
                continue;       // the victim's body (pg_gate's own filter)
            if ((hx - e->ml[0]) * (hx - e->ml[0]) +
                (hz - e->ml[1]) * (hz - e->ml[1]) < 0.45f * 0.45f)
                continue;       // our body
            if (e->wall
                && (hx - e->ap0[0]) * e->fwd[0] + (hz -
                                                   e->ap0[2]) * e->fwd[1] <
                0.10f)
                continue;       // a wall pair: its surface and what is behind it
            if (e->nh < PWS_NH) {
                e->H[e->nh][0] = hx;
                e->H[e->nh][1] = hz;
                e->nh++;
                added++;
            }
        }
    return got;
}

// the worst clearance of key k's points (shifted by s) against all hits
static float PwsClear(const PwsEnt *e, int k, const float *s)
{
    float m = 9.0f;
    for (int h = 0; h < 2; h++) {
        if (e->K.idx[k][h] < 0)
            continue;
        const float *p = e->K.p[e->K.idx[k][h]];
        for (int i = 0; i < e->nh; i++) {
            float dx = p[0] + s[0] - e->H[i][0], dz =
                p[2] + s[1] - e->H[i][1], d =
                __builtin_sqrtf(dx * dx + dz * dz);
            if (d < m)
                m = d;
        }
    }
    return m;
}

// the DP: S_k per key, 1 = solved (S all zero when nothing is in reach)
static int PwsSolve(PwsEnt *e)
{
    enum { G = 10, NC = (2 * G + 1) * (2 * G + 1) };
    static float cs[NC][2];
    static float cost[PWS_NK][NC];
    static short from[PWS_NK][NC];
    static unsigned char ok[PWS_NK][NC];
    int nc = 0;
    if (e->wall) {
        for (int i = -G; i <= G; i++) {
            cs[nc][0] = e->tan[0] * i * PWS_STEP;
            cs[nc][1] = e->tan[1] * i * PWS_STEP;
            nc++;
    }} else
        for (int i = -G; i <= G; i++)
            for (int j = -G; j <= G; j++) {
                float x = i * PWS_STEP, z = j * PWS_STEP;
                if (x * x + z * z <= PWS_MAX * PWS_MAX + 1e-4f) {
                    cs[nc][0] = x;
                    cs[nc][1] = z;
                    nc++;
                }
            }
    for (int k = 0; k < PWS_NK; k++)
        for (int c = 0; c < nc; c++) {
            ok[k][c] = PwsClear(e, k, cs[c]) >= PWS_R_PLAN;
            float mag =
                __builtin_sqrtf(cs[c][0] * cs[c][0] + cs[c][1] * cs[c][1]);
            cost[k][c] = 1e9f;
            from[k][c] = -1;
            if (!ok[k][c])
                continue;
            if (k == 0) {
                cost[k][c] = mag;
                continue;
            }
            for (int c2 = 0; c2 < nc; c2++) {
                if (cost[k - 1][c2] >= 1e8f)
                    continue;
                float dx = cs[c][0] - cs[c2][0], dz =
                    cs[c][1] - cs[c2][1], v =
                    cost[k - 1][c2] + 2.0f * __builtin_sqrtf(dx * dx +
                                                             dz * dz) +
                    mag;
                if (v < cost[k][c]) {
                    cost[k][c] = v;
                    from[k][c] = (short) c2;
                }
            }
        }
    int best = -1;
    for (int c = 0; c < nc; c++)
        if (cost[PWS_NK - 1][c] < 1e8f
            && (best < 0 || cost[PWS_NK - 1][c] < cost[PWS_NK - 1][best]))
            best = c;
    if (best < 0)
        return 0;
    for (int k = PWS_NK - 1; k >= 0; k--) {
        e->S[k][0] = cs[best][0];
        e->S[k][1] = cs[best][1];
        best = from[k][best];
        if (best < 0 && k > 0)
            return 0;
    }
    return 1;
}

static int PwsViolates(const PwsEnt *e)
{
    for (int k = 0; k < PWS_NK; k++)
        if (PwsClear(e, k, e->S[k]) < PWS_R_VER)
            return k + 1;
    return 0;
}

static void PwsPublish(PwsEnt *e, int verified, int dyn)
{
    PwsFin *f = NULL;
    for (int i = 0; i < 8; i++)
        if (!g_pwsf[i].on
            || (g_pwsf[i].slot == e->slot && g_pwsf[i].key == e->key)) {
            f = &g_pwsf[i];
            if (!g_pwsf[i].on)
                break;
        }
    if (!f) {
        f = &g_pwsf[0];
        for (int i = 1; i < 8; i++)
            if (g_pwsf[i].f < f->f)
                f = &g_pwsf[i];
    }
    f->on = 1;
    f->slot = e->slot;
    f->key = e->key;
    f->pkey = e->pkey;
    memcpy(f->ap0, e->ap0, sizeof f->ap0);
    f->f = g_frame ? g_frame : 1;
    memcpy(f->S, e->S, sizeof f->S);
    f->tend = e->tend;
    f->dyn = dyn;
    f->verified = verified;
}

// psync_start: the profile of the gate that cleared this pair (same slot, same clips, ap within 0.8 m, <= 3 s old). *ap7 is moved by S_0.
typedef struct {
    int ok, dyn;
    float S[PWS_NK][2], tend;
} PwsTakeT;
static uint64_t PwsKey(const char *att, const char *vic)
{
    return FNV64(att) * 31u + FNV64(vic);
}

static void PwsTake(uint32_t slot, const char *att, const char *vic,
                     float *ap7, PwsTakeT *out)
{
    memset(out, 0, sizeof *out);
    if (PwsOff() || !att || !vic)
        return;
    uint64_t key0 = PwsKey(att, vic);
    PwsFin *b = NULL;
    float bd = 0.8f * 0.8f;
    for (int i = 0; i < 8; i++) {
        PwsFin *f = &g_pwsf[i];
        if (!f->on || f->slot != slot || f->pkey != key0
            || f->f + 180 < g_frame)
            continue;
        float dx = f->ap0[0] - ap7[0], dz = f->ap0[2] - ap7[2], d =
            dx * dx + dz * dz;
        if (d < bd) {
            bd = d;
            b = f;
        }
    }
    if (!b || !b->verified)
        return;
    out->ok = 1;
    out->dyn = b->dyn;
    out->tend = b->tend;
    memcpy(out->S, b->S, sizeof out->S);
    ap7[0] += b->S[0][0];
    ap7[2] += b->S[0][1];
    float mx = 0.0f;
    for (int k = 0; k < PWS_NK; k++) {
        float m =
            __builtin_sqrtf(b->S[k][0] * b->S[k][0] +
                            b->S[k][1] * b->S[k][1]);
        if (m > mx)
            mx = m;
    }
    Xlog("pair shift: slot %u %s / %s taken by the pair start - ap moved %.2f m at the start (%.2f %.2f), largest shift over the move %.2f m, %s", slot, att, vic, __builtin_sqrtf(b->S[0][0] * b->S[0][0] + b->S[0][1] * b->S[0][1]), b->S[0][0], b->S[0][1], mx, b->dyn ? "sliding with the move time (sent in the START)" : "start shift only");
}

// ---- the gate
static void PwsFree(PwsEnt *e)
{
    PwsDrain(e);
    memset(e, 0, sizeof *e);
}

static int PwsBuild(PwsEnt *e, const char *att, const char *vic,
                     const float *ap, int gun)
{
    float pe = g_pgEndPh > 0.0f
        && g_pgEndPh < 1.0f ? g_pgEndPh : 1.0f;
    float da = PsyncDur(SID_PLAYER, FNV64(att)), dv =
        PsyncDur(SID_PLAYER, FNV64(vic));
    if (!(da > 0.2f))
        da = dv;
    if (!(dv > 0.2f))
        dv = da;
    if (!(da > 0.2f))
        return 0;
    float dm = da > dv ? da : dv;
    e->tend = pe * dm;
    memset(&e->K, 0, sizeof e->K);
    int any = 0;
    for (int k = 0; k < PWS_NK; k++) {
        float t = e->tend * (float) k / (float) (PWS_NK - 1);
        for (int h = 0; h < 2; h++) {
            e->K.idx[k][h] = -1;
            float ph = t / (h ? dv : da);
            if (ph > 1.0f)
                ph = 1.0f;
            float out[2][8];
            int got =
                PsyncPairEnd(ap, FNV64(att), FNV64(vic), ph, gun, out);
            if (got & (1 << h)) {
                e->K.idx[k][h] = PwsAdd(&e->K, out[h]);
                any = 1;
            }
        }
    }
    return any;
}

static int PwsApplyFinal(PwsEnt *e, int verdict, int verified, int dyn,
                           const char *why)
{
    e->state = 6;
    e->done_f = g_frame ? g_frame : 1;
    e->verdict = verdict;
    if (verdict == 1)
        PwsPublish(e, verified, dyn);
    Xlog("pair shift: %s slot %u %s - %s -> %s", e->tag, e->slot,
         e->wall ? "(wall pair)" : "", why, verdict ? "clear" : "BLOCKED");
    return verdict;
}

static int PgGate(uint32_t slot, const char *att, const char *vic,
                   const float *ap, const float *pv, const float *ml,
                   int gun, const char *tag)
{
    if (PwsOff() || !pv || !ml || !att || !vic || !ap)
        return PgGateBase(slot, att, vic, ap, pv, ml, gun, tag);
    uint64_t pk = PwsKey(att, vic);
    uint64_t key =
        pk + (uint64_t) (int64_t) g_pgKeyx * 1000003ULL +
        (uint64_t) g_pgReact * 7919ULL +
        (uint64_t) (int64_t) (g_pgEndPh * 1000.0f) * 104729ULL;
    PwsEnt *e = NULL, *fr = NULL;
    for (int i = 0; i < 8; i++) {
        PwsEnt *c = &g_pws[i];
        if (c->on
            && g_frame - (c->state == 6 ? c->done_f : c->kick_f) > 600)
            PwsFree(c);
        if (c->on && c->slot == slot && c->key == key) {
            float dx = c->ap0[0] - ap[0], dz = c->ap0[2] - ap[2];
            if (dx * dx + dz * dz < 0.6f * 0.6f)
                e = c;
        }
        if (!c->on && !fr)
            fr = c;
    }
    if (e && e->state == 6
        && (g_frame - e->done_f > 45
            || (pv[0] - e->pv[0]) * (pv[0] - e->pv[0]) + (pv[2] -
                                                          e->pv[1]) *
            (pv[2] - e->pv[1]) > 0.25f * 0.25f)) {
        PwsFree(e);
        fr = e;
        e = NULL;
    }
    if (e && e->state == 6)
        return e->verdict;
    int keyx = g_pgKeyx;
    if (!e) {
        if (!fr) {
            fr = &g_pws[0];
            for (int i = 1; i < 8; i++)
                if (g_pws[i].kick_f < fr->kick_f)
                    fr = &g_pws[i];
            PwsFree(fr);
        }
        e = fr;
        memset(e, 0, sizeof *e);
        e->on = 1;
        e->slot = slot;
        e->key = key;
        e->pkey = pk;
        memcpy(e->ap0, ap, 28);
        e->pv[0] = pv[0];
        e->pv[1] = pv[2];
        e->ml[0] = ml[0];
        e->ml[1] = ml[2];
        snprintf(e->tag, sizeof e->tag, "%s", tag ? tag : "?");
        e->wall = strstr(att, "wall") != NULL
            || strstr(vic, "wall") != NULL;
        {
            const float *q = ap + 3;
            float fx = 2.0f * (q[0] * q[2] + q[3] * q[1]), fz =
                1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]), fnn =
                __builtin_sqrtf(fx * fx + fz * fz);
            if (fnn < 0.3f)
                e->wall = 0;
            else {
                e->fwd[0] = fx / fnn;
                e->fwd[1] = fz / fnn;
                e->tan[0] = e->fwd[1];
                e->tan[1] = -e->fwd[0];
            }
        }
        if (!PwsBuild(e, att, vic, ap, gun)) {
            e->state = 9;
            Xlog("pair shift: %s slot %u %s / %s: clip paths unreadable - old gate only", e->tag, slot, att, vic);
        } else {
            PwsKick(e, &e->K);
            e->state = 1;
            if (!e->jn)
                e->state = 9;
        }
        int g0 = PgGateBase(slot, att, vic, ap, pv, ml, gun, tag);    // (the old gate kicks its own rays at once; its verdict is asked later)
        if (e->state == 9) {
            if (g0 != 2) {
                e->state = 6;
                e->done_f = g_frame ? g_frame : 1;
                e->verdict = g0;
            }
            return g0;
        }
        return 2;
    }
    if (e->state == 9) {
        int g = PgGateBase(slot, att, vic, ap, pv, ml, gun, tag);
        if (g != 2) {
            e->state = 6;
            e->done_f = g_frame ? g_frame : 1;
            e->verdict = g;
        }
        return g;
    }
    if (e->state == 5) {        // passthrough (nothing to shift / nothing solvable): the old gate decides on the original ap
        int g = PgGateBase(slot, att, vic, ap, pv, ml, gun, tag);
        if (g != 2) {
            e->state = 6;
            e->done_f = g_frame ? g_frame : 1;
            e->verdict = g;
            if (g == 1) {
                memset(e->S, 0, sizeof e->S);
                PwsPublish(e, e->verified, 0);
            }
        }
        return g;
    }
    if (g_frame <= e->kick_f)
        return 2;
    if (e->state == 1 || e->state == 3) {
        PwsPts *q = e->state == 1 ? &e->K : &e->V;
        int got = PwsRead(e, q);
        if (!got) {
            Xlog("pair shift: %s slot %u: no ray answered - old gate only",
                 e->tag, slot);
            e->state = 5;
            e->verified = 0;
            return 2;
        }
        if (e->state == 1) {
            memset(e->S, 0, sizeof e->S);
            if (!PwsViolates(e)) {
                Xlog("pair shift: %s slot %u %s / %s: %d path points, %d obstacle points in reach, nothing within %.2f m of the path - no shift", e->tag, slot, att, vic, e->K.n, e->nh, PWS_R_VER);
                e->state = 5;
                e->verified = 1;
                return 2;
            }
        } else {
            int bad = PwsViolates(e);
            if (!bad) {
                e->state = 4;
            } else
                Xlog("pair shift: %s slot %u: round %d - key %d still within %.2f m of geometry after the shift (%d obstacle points)", e->tag, slot, e->round, bad - 1, PWS_R_VER, e->nh);
        }
        if (e->state != 4) {
            if (e->round >= 3 || !PwsSolve(e)) {
                Xlog("pair shift: %s slot %u %s / %s: NO shift up to %.1f m clears the path (round %d, %d obstacle points) - the old gate decides, no shift", e->tag, slot, att, vic, PWS_MAX, e->round, e->nh);
                memset(e->S, 0, sizeof e->S);
                e->state = 5;
                e->verified = 0;
                return 2;
            }
            e->round++;
            PwsShifted(e, &e->V);
            PwsKick(e, &e->V);
            e->state = 3;
            float m = 0;
            for (int k = 0; k < PWS_NK; k++) {
                float d =
                    __builtin_sqrtf(e->S[k][0] * e->S[k][0] +
                                    e->S[k][1] * e->S[k][1]);
                if (d > m)
                    m = d;
            }
            Xlog("pair shift: %s slot %u %s / %s: round %d - shift keys (m) %.2f,%.2f  %.2f,%.2f  %.2f,%.2f  %.2f,%.2f  %.2f,%.2f  %.2f,%.2f (largest %.2f) over %.2f s, verifying", e->tag, slot, att, vic, e->round, e->S[0][0], e->S[0][1], e->S[1][0], e->S[1][1], e->S[2][0], e->S[2][1], e->S[3][0], e->S[3][1], e->S[4][0], e->S[4][1], e->S[5][0], e->S[5][1], m, e->tend);
            return 2;
        }
    }
    if (e->state == 4) {        // the rings round the shifted path are clear: the old gate on the shifted ap (floor / drop / lines) has the last word
        float ap1[7];
        memcpy(ap1, ap, sizeof ap1);
        ap1[0] += e->S[0][0];
        ap1[2] += e->S[0][1];
        g_pgKeyx = keyx + 31 * (e->round + 1) + 90000;
        int g = PgGateBase(slot, att, vic, ap1, pv, ml, gun, tag);
        g_pgKeyx = keyx;
        if (g == 2)
            return 2;
        if (g == 1) {
            int dyn = 0;
            for (int k = 1; k < PWS_NK; k++)
                if (__builtin_fabsf(e->S[k][0] - e->S[0][0]) +
                    __builtin_fabsf(e->S[k][1] - e->S[0][1]) > 0.02f)
                    dyn = 1;
            return PwsApplyFinal(e, 1, 1, dyn,
                                   "shifted path verified (rings clear, old gate clear)");
        }
        int g0 = PgGateBase(slot, att, vic, ap, pv, ml, gun, tag);
        if (g0 == 2)
            return 2;
        memset(e->S, 0, sizeof e->S);
        return PwsApplyFinal(e, g0, 0, 0,
                               g0 ?
                               "the old gate blocks the shifted ap but clears the original: played unshifted"
                               :
                               "the shift does not clear the old gate (floor / drop?) and the original is blocked");
    }
    return 2;
}
