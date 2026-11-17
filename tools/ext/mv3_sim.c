// mv3_sim.c - melee v3 deterministic core. Spec re/melee-v3.md, contract mv3.h, tests tools/mv3/simtest.c.
// Pure: no engine calls, no mutable statics, integer state only. Floats are read only from the const move table and turned into integers
// (mm, 1/100 deg) with one IEEE multiply, so every x64 build gives the same numbers. Trig = integer tables / CORDIC, never libm.
// Everything the sim knows is inside mv3_world (memcpy = snapshot), so rollback = restore + re-step.
//
// Conventions (glue/anim must match; summary at the top of mv3.h):
//   - positions px, pz horizontal, py up (mm). yaw 1/100 deg, 0 = facing +z, +9000 = facing +x (the engine's atan2(x, z) = uc4_yaw x 100).
//   - handedness is the engine's: +x is the LEFT of a body facing +z (uc4_yaw, melee_hitreact.c's left = (fz, -fx)). So the direction yaw + 90
//     is the body's LEFT and yaw - 90 its RIGHT. dir 1 "left" / 2 "right" (dodges, reactions, mv3b_quad) and root_side (+ = the character's
//     RIGHT, as the clips' align channel decoded by gen_moves.py) all use that: a stick-left dodge picks a dir-1 (left) clip and its root
//     motion moves the body toward yaw + 90, i.e. to its left on screen, the same way the clip leans.
//   - the stick (sx, sy) is in the fighter's body frame like a pad: sy+ = forward, sx+ = the body's RIGHT (toward yaw - 90). The glue rotates
//     the camera-relative stick into it (mv3g_stick_body).
//
// Frame order inside one mv3_sim_step (each pass walks fighters in index order; decisions about OTHER fighters read a copy of the world
// taken after pass A, so the result never depends on index order except where documented as "lower index wins"):
//   A advance: mf++, moves that ran out -> IDLE, buffer ageing, counter window expiry, IDLE/OUT bodies take the owner's input pose.
//   B input: edges from prev_btn, buffer, combo chain queue, action starts (swing / dodge / counter).
//   D contact: every live swing / counter contact of this frame is collected first, then applied together (so two contacts in the same
//     frame trade; otherwise the first one interrupts the other swing before its contact). Dodged swings -> MISS, then hits -> REACT / DOWN.
//   C motion: windup turn toward the target, root motion of every non-IDLE fighter at its move frame (moves started this frame get slice 0).
//
// Timing rules (numbers below, all frames at 60 Hz):
//   - Buffered press: the last ATK / DODGE edge, kept MV3B_BUF_MAX frames (18 = 300 ms) and consumed on the first legal frame.
//     Not-yet-legal presses are never dropped inside that window; a dodge press with no swing coming at us is dropped at once (dodge does nothing without a threat, and it must not pre-arm).
//   - Move windows follow mv3_moves.h: the contact frames are [hit0, hit1) (hit1 exclusive; hit1 <= hit0 = the single frame hit0); a move's
//     state is over at cancel_f when 0 < cancel_f < dur_f (swings: chain1 / SP's whiff end, reactions: the control lock MHR_CUT_MS, dodges:
//     SP leaves the clip, counters: the hand-off), else at dur_f - the rest of the clip is presentation only (the anim blends it out).
//     Root motion is the clip's authored curve (mv3_move_rm_at) when the table has one, else root_fwd / root_side spread linearly.
//   - Combo: [chain0, chain1) of swing step 0/1 is where the next step may START (SP: contact + 331 ms for fists). An ATK press from
//     MV3B_CHAIN_EARLY frames before chain0 up to chain1 (or a still-buffered earlier one) commits it; it starts at max(chain0, hit window
//     over, MV3B_CADENCE). After chain1 the swing is over and a press is a fresh step-0 swing.
//   - Cadence floor MV3B_CADENCE = 20 frames (333 ms) between two swing starts of one fighter: mashing never out-paces SP's chain.
//   - Soft lock: the target is picked once at swing start (nearest enemy within reach + MV3B_LOCK_EXTRA and MV3B_LOCK_ARC) and not
//     re-picked - no target flipping mid-swing; a swing that started with no target picks the first enemy that is inside reach + arc
//     during its contact window (someone walking into a swing gets hit). The body turns toward the target during windup at most
//     MV3B_TURN per frame, then the facing is locked.
//   - Dodge: legal from IDLE or from our own swing past its dodge-cancel frame (MV3_MOVES_X.dodge_cancel_f, SP: contact + 100 ms), only while an enemy swing that targets us is in windup / active; the dodger snaps to
//     face the attacker (dodge clips are authored facing it) and the stick picks front/left/right/back relative to it (neutral = back).
//     A swing whose target is in i-frames during contact, or is dodging and out of reach at hit1, is DODGED: the attacker plays MISS.
//   - Counter: an ATK press while the dodge's counter window is open (MV3B_COUNTER_WIN from the DODGED frame, at least to the dodge's own
//     chain1 = landing + 600 ms), from DODGE or IDLE, with the attacker still in MISS and within the counter's reach (+ the counter clip's own
//     travel to its contact): COUNTER on us, COUNTERED (the row's pair) on it, contact at the counter's hit0. A forward dodge arms the "-fw"
//     row (dir 0), any other the back row (dir 3).
//   - Hits: one contact per swing. HIT x = dmg; sim hp -= dmg; hp <= 0 -> DOWN + OUT (glue hands it to the engine's downed flow).
//     Otherwise the victim always plays REACT (interrupting windup / recovery / dodge recovery / miss); reaction = direction of the
//     attacker around the victim + the swing's strength (damage class), rng among equal candidates.
#include "mv3.h"
#include <string.h>

#define MV3B_BUF_MAX     18     // buffered press life, frames (300 ms)
#define MV3B_CADENCE     20     // min frames between two swing starts of one fighter (333 ms = 3 swings/s at most)
#define MV3B_COUNTER_WIN 27     // frames after the DODGED frame that a counter press is accepted (450 ms), at least
#define MV3B_CHAIN_EARLY 16     // frames before chain0 that a press already commits the next combo step (SP held presses up to 256 ms early)
#define MV3B_DCANCEL     6      // swings: dodge-cancel this many frames after the contact window when the table has no dodge_cancel_f (100 ms)
// SP soft lock (re/sp-melee-1027/report.md: swings started at 2.6-3.8 m closed to ~1.0 m by impact, turn-180 starts): players could not hit each other when facing different directions - the pick reaches 4 m all around, the windup turns fully and lunges to contact range
#define MV3B_LOCK_EXTRA  2400   // mm past the swing's reach that the soft lock still picks a target at swing start (4 m with fists)
#define MV3B_LOCK_ARC    18000  // 1/100 deg half-angle of the soft lock pick: all around (front preferred, see mv3b_nearest's score)
#define MV3B_TURN        1500   // 1/100 deg per frame max turn toward the target during windup (900 deg/s: a 180 in 12 frames)
#define MV3B_LUNGE_TO    1000   // mm: the windup closes the gap to this (SP impact distance ~0.9-1.0 m)
#define MV3B_LUNGE_MAX   110    // mm per frame max lunge (6.6 m/s)
#define MV3B_MIN_SEP     450    // mm: root motion of a swing/counter never pushes the attacker closer than this to its target
#define MV3B_MAX_DY      1200   // mm height difference still allowed for contact / lock
#define MV3B_HP0         1000   // default sim hp (x10) until the glue mirrors the engine's
#define MV3B_REACH_DEF   1600   // mm, a swing row with no reach
#define MV3B_ARC_DEF     4500   // 1/100 deg, a swing row with no arc
#define MV3B_CTR_REACH   2200   // mm, a counter row with no reach
#define MV3B_STICK_DEAD  40     // stick magnitude below this = neutral (dodge back)
#define MV3B_BUF_PRESS   (MV3_B_ATK | MV3_B_DODGE)
#define MV3B_BUF_CHAIN   0x80   // fighter.buf bit: next combo step queued (not aged: it is a commitment, not a press)
#define MV3B_NONE        0xff
#define MV3B_CDIR(f)     ((f)->pad[0])  // sim-private (layout unchanged): the direction of the dodge that armed the counter window
#if defined(MV3_MOVES_COUNT) && defined(MV3_RM_N)
#define MV3B_REAL 1             // the generated table (mv3_moves.h): authored root-motion curves + MV3_MOVES_X
#endif

// ---- integer trig: sin in Q14 per whole degree, linear in between (error < 0.0002); atan2 by 16-step CORDIC in 1/10000 deg
static const int16_t mv3b_sin_t[91] = {
    0, 286, 572, 857, 1143, 1428, 1713, 1997, 2280, 2563, 2845, 3126, 3406,
        3686, 3964, 4240, 4516, 4790, 5063, 5334, 5604, 5872, 6138, 6402,
        6664, 6924, 7182, 7438,
    7692, 7943, 8192, 8438, 8682, 8923, 9162, 9397, 9630, 9860, 10087,
        10311, 10531, 10749, 10963, 11174, 11381, 11585, 11786, 11982,
        12176, 12365, 12551, 12733,
    12911, 13085, 13255, 13421, 13583, 13741, 13894, 14044, 14189, 14330,
        14466, 14598, 14726, 14849, 14968, 15082, 15191, 15296, 15396,
        15491, 15582, 15668,
    15749, 15826, 15897, 15964, 16026, 16083, 16135, 16182, 16225, 16262,
        16294, 16322, 16344, 16362, 16374, 16382, 16384
};
static const int32_t mv3b_atan_t[16] =
    { 450000, 265651, 140362, 71250, 35763, 17899, 8952, 4476, 2238, 1119,
560, 280, 140, 70, 35, 17 };

static int32_t Mv3bWrap(int32_t a)
{
    a %= 36000;
    if (a > 18000)
        a -= 36000;
    if (a < -18000)
        a += 36000;
    return a;
}

static int32_t Mv3bSin(int32_t a)
{
    a %= 36000;
    if (a < 0)
        a += 36000;
    int neg = 0;
    if (a >= 18000) {
        a -= 18000;
        neg = 1;
    }
    if (a > 9000)
        a = 18000 - a;
    int32_t i = a / 100, fr = a % 100;
    int32_t s =
        mv3b_sin_t[i] + (i <
                         90 ? (mv3b_sin_t[i + 1] -
                               mv3b_sin_t[i]) * fr / 100 : 0);
    return neg ? -s : s;
}

static int32_t Mv3bCos(int32_t a)
{
    return Mv3bSin(a + 9000);
}

static int32_t Mv3bAtan2(int64_t x, int64_t z) // yaw of the vector (x, z), 1/100 deg
{
    if (!x && !z)
        return 0;
    int64_t X = z, Y = x;
    int32_t ang = 0;
    if (X < 0) {
        X = -X;
        Y = -Y;
        ang = 1800000;
    }
    X *= 65536;
    Y *= 65536;
    for (int i = 0; i < 16; i++) {
        int64_t nx;
        if (Y > 0) {
            nx = X + (Y >> i);
            Y -= X >> i;
            ang += mv3b_atan_t[i];
        } else {
            nx = X - (Y >> i);
            Y += X >> i;
            ang -= mv3b_atan_t[i];
        }
        X = nx;
    }
    return Mv3bWrap((ang + (ang >= 0 ? 50 : -50)) / 100);
}

static int64_t Mv3bIsqrt(uint64_t v)
{
    uint64_t r = 0, b = (uint64_t) 1 << 62;
    while (b > v)
        b >>= 2;
    while (b) {
        if (v >= r + b) {
            v -= r + b;
            r = (r >> 1) + b;
        } else
            r >>= 1;
        b >>= 2;
    }
    return (int64_t) r;
}

// ---- move table access (only MV3_MOVES / MV3_MOVES_N and the kind / wset / dir / strength fields: works with mv3_moves.h or the stub)
static const Mv3Move *Mv3bMv(int m)
{
    return (m >= 0 && m < MV3_MOVES_N) ? &MV3_MOVES[m] : 0;
}

static int32_t Mv3bMm(float m)
{
    return (int32_t) (m * 1000.0f + (m >= 0.0f ? 0.5f : -0.5f));
}

static int32_t Mv3bReach(const Mv3Move *m, int32_t def)
{
    int32_t r = Mv3bMm(m->reach);
    return r > 0 ? r : def;
}

static int32_t Mv3bArc(const Mv3Move *m)
{
    int32_t a = (int32_t) (m->arc_deg * 100.0f + 0.5f);
    return a > 0 ? a : MV3B_ARC_DEF;
}

static int Mv3bHitLast(const Mv3Move *m)
{
    return m->hit1_f > m->hit0_f ? m->hit1_f - 1 : m->hit0_f;
}                               // last contact frame (hit1 exclusive)

static int Mv3bEnd(const Mv3Move *m)
{
    return m->cancel_f > 0
        && m->cancel_f < m->dur_f ? m->cancel_f : m->dur_f;
}                               // the state is over here

static int Mv3bDodgeCancel(int mi)    // swings: our own dodge may cut the swing from this frame
{
#ifdef MV3B_REAL
    if (MV3_MOVES_X[mi].dodge_cancel_f)
        return MV3_MOVES_X[mi].dodge_cancel_f;
#endif
    return Mv3bHitLast(&MV3_MOVES[mi]) + 1 + MV3B_DCANCEL;
}

// Distance balance: SP's dodges travel up to ~2.8 m (back) / 1.9 m (ducks), v2's capped them at 0.85 m. A dodge's whole
// travel is scaled down (curve shape kept) to MV3B_DODGE_MAX_MM - far enough to leave the swing's reach, short enough not to run away.
#define MV3B_DODGE_MAX_MM 1300
static void Mv3bRmRaw(int mi, int mf, int32_t * fwd, int32_t * right);
static void Mv3bRm(int mi, int mf, int32_t *fwd, int32_t *right)
{
    Mv3bRmRaw(mi, mf, fwd, right);
    if (MV3_MOVES[mi].kind != MV3_K_DODGE)
        return;
    int32_t tf, tr;
    Mv3bRmRaw(mi, MV3_MOVES[mi].dur_f, &tf, &tr);
    int64_t len =
        Mv3bIsqrt((uint64_t) ((int64_t) tf * tf + (int64_t) tr * tr));
    if (len <= MV3B_DODGE_MAX_MM)
        return;
    *fwd = (int32_t) ((int64_t) * fwd * MV3B_DODGE_MAX_MM / len);
    *right = (int32_t) ((int64_t) * right * MV3B_DODGE_MAX_MM / len);
}

static void Mv3bRmRaw(int mi, int mf, int32_t *fwd, int32_t *right)
{
    const Mv3Move *m = &MV3_MOVES[mi];
#ifdef MV3B_REAL
    for (int k = 0; k < MV3_RM_N; k++)
        if (MV3_MOVES_RM[mi][k][0] || MV3_MOVES_RM[mi][k][1]) {
            Mv3MoveRmAt(mi, mf, fwd, right);
            return;
        }
#endif
    int32_t d = m->dur_f ? m->dur_f : 1;
    if (mf < 0)
        mf = 0;
    if (mf > d)
        mf = d;
    *fwd = (int32_t) ((int64_t) Mv3bMm(m->root_fwd) * mf / d);
    *right = (int32_t) ((int64_t) Mv3bMm(m->root_side) * mf / d);
}

static int Mv3bFind(int kind, int wset, int dir)       // first row of this kind; wset / dir < 0 = any
{
    for (int i = 0; i < MV3_MOVES_N; i++) {
        const Mv3Move *m = &MV3_MOVES[i];
        if (m->kind == kind && (wset < 0 || m->wset == wset)
            && (dir < 0 || m->dir == dir))
            return i;
    }
    return -1;
}

static int Mv3bFindWs(int kind, int wset)     // own weapon set, else fists, else anything of the kind
{
    int m = Mv3bFind(kind, wset, -1);
    if (m < 0)
        m = Mv3bFind(kind, MV3_WS_FIST, -1);
    if (m < 0)
        m = Mv3bFind(kind, -1, -1);
    return m;
}

static int Mv3bSwingMove(int wset, int step)  // no fallback across sets for steps 2/3: a set without a step 3 simply ends its chain
{
    int m = Mv3bFind(MV3_K_SWING1 + step, wset, -1);
    if (m < 0 && step == 0)
        m = Mv3bFindWs(MV3_K_SWING1, wset);
    if (m < 0 && step > 0 && Mv3bFind(MV3_K_SWING1, wset, -1) < 0)
        m = Mv3bFind(MV3_K_SWING1 + step, MV3_WS_FIST, -1);
    return m;
}

static uint32_t Mv3bRng(Mv3World *w)
{
    uint32_t x = w->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    if (!x)
        x = 0x6d2b79f5u;
    return w->rng = x;
}

// dir penalty: reactions want the true side (adjacent before opposite); dodges fall back to back first (the safe SP default)
static int Mv3bPenReact(int want, int have)
{
    if (want == have)
        return 0;
    if ((want ^ have) == 3)
        return 2;
    return 1;
}

static int Mv3bPenDodge(int want, int have)
{
    if (want == have)
        return 0;
    if (have == 3)
        return 1;
    if ((want ^ have) == 3)
        return 3;
    return 2;
}

// best row of a kind by (dir, wset, strength); rng only when several rows tie, so the stream advances identically on every machine
static int Mv3bPick(Mv3World *w, int kind, int wset, int dir,
                     int strength, int dodge)
{
    int best = 1 << 30, cnt = 0, first = -1;
    for (int pass = 0; pass < 2; pass++) {
        int k = 0, pickn =
            pass ? (cnt >
                    1 ? (int) (Mv3bRng(w) % (uint32_t) cnt) : 0) : 0;
        for (int i = 0; i < MV3_MOVES_N; i++) {
            const Mv3Move *m = &MV3_MOVES[i];
            if (m->kind != kind)
                continue;
            int s =
                (dodge ? Mv3bPenDodge(dir, m->dir) :
                 Mv3bPenReact(dir,
                                m->dir)) * 1000 + (m->wset !=
                                                   wset ? 100 : 0)
                + (strength >=
                   0 ? (m->strength >
                        strength ? m->strength - strength : strength -
                        m->strength) : 0);
            if (!pass) {
                if (s < best) {
                    best = s;
                    cnt = 1;
                    first = i;
                } else if (s == best)
                    cnt++;
            } else if (s == best && k++ == pickn)
                return i;
        }
        if (cnt <= 1)
            return first;
    }
    return first;
}

// ---- geometry
static int Mv3bLive(const Mv3Fighter *f)
{
    return f->slot != MV3B_NONE && f->state != MV3_S_OUT;
}

static int Mv3bEnemy(const Mv3Fighter *a, const Mv3Fighter *b)
{
    return Mv3bLive(b) && a->team != b->team;
}

static int64_t Mv3bDist(const Mv3Fighter *a, const Mv3Fighter *b)
{
    int64_t dx = (int64_t) b->px - a->px, dz = (int64_t) b->pz - a->pz;
    return Mv3bIsqrt((uint64_t) (dx * dx + dz * dz));
}

static int32_t Mv3bYawTo(const Mv3Fighter *a, const Mv3Fighter *b)
{
    return Mv3bAtan2((int64_t) b->px - a->px, (int64_t) b->pz - a->pz);
}

static int32_t Mv3bRel(const Mv3Fighter *a, const Mv3Fighter *b)
{
    return Mv3bWrap(Mv3bYawTo(a, b) - a->yaw);
}                               // b around a's facing

static int Mv3bDyOk(const Mv3Fighter *a, const Mv3Fighter *b)
{
    int32_t d = a->py - b->py;
    return d <= MV3B_MAX_DY && d >= -MV3B_MAX_DY;
}

static int Mv3bQuad(int32_t rel)       // 0 front 1 left 2 right 3 back; rel > 0 = toward yaw + 90 = the body's LEFT (engine handedness)
{
    if (rel >= -4500 && rel <= 4500)
        return 0;
    if (rel > 4500 && rel < 13500)
        return 1;
    if (rel < -4500 && rel > -13500)
        return 2;
    return 3;
}

// nearest enemy of fighter i inside range + half-angle arc (ties: lower index)
static int Mv3bNearest(const Mv3World *w, int i, int32_t range,
                        int32_t arc)
{
    const Mv3Fighter *a = &w->f[i];
    int best = -1;
    int64_t bd = 0;
    for (int j = 0; j < w->n; j++) {
        const Mv3Fighter *b = &w->f[j];
        if (j == i || !Mv3bEnemy(a, b) || !Mv3bDyOk(a, b))
            continue;
        int64_t d = Mv3bDist(a, b);
        if (d > range)
            continue;
        int32_t r = Mv3bRel(a, b);
        if (r > arc || r < -arc)
            continue;
        if (r < 0)
            r = -r;
        d += (int64_t) r *15 / 100;     // score: 1.5 m per 100 deg off the facing (someone in front beats someone behind)
        if (best < 0 || d < bd) {
            best = j;
            bd = d;
        }
    }
    return best;
}

// an enemy swing aimed at fighter i still in windup / active (soonest contact first, ties lower index): the only time a dodge is legal
static int Mv3bThreat(const Mv3World *o, int i)
{
    int best = -1, bt = 0;
    for (int j = 0; j < o->n; j++) {
        const Mv3Fighter *b = &o->f[j];
        if (j == i || !Mv3bEnemy(&o->f[i], b) || b->state != MV3_S_SWING
            || b->hit_done || b->target != i)
            continue;
        const Mv3Move *m = Mv3bMv(b->move);
        if (!m || b->mf > Mv3bHitLast(m))
            continue;
        int t = (int) m->hit0_f - (int) b->mf;
        if (best < 0 || t < bt) {
            best = j;
            bt = t;
        }
    }
    return best;
}

static int Mv3bInvuln(const Mv3Fighter *f)
{
    if (f->state != MV3_S_DODGE)
        return 0;
    const Mv3Move *m = Mv3bMv(f->move);
    return m && f->mf >= m->iframe0_f && f->mf < m->iframe1_f;
}

// ---- events
static void Mv3bEv(Mv3Evbuf *ev, uint32_t fr, int type, int a, int b,
                    int move, int x)
{
    if (!ev || ev->n >= MV3_EV_MAX)
        return;
    Mv3Ev *e = &ev->e[ev->n++];
    e->frame = fr;
    e->type = (uint8_t) type;
    e->a = (uint8_t) a;
    e->b = (uint8_t) b;
    e->pad = 0;
    e->move = (int16_t) move;
    e->x = (int16_t) x;
}

// ---- state starts
static void Mv3bBegin(Mv3Fighter *f, int state, int move, int target)
{
    f->state = (uint8_t) state;
    f->move = (int16_t) move;
    f->mf = 0;
    f->target = (uint8_t) target;
    f->hit_done = 0;
    f->buf &= (uint8_t) ~ MV3B_BUF_CHAIN;
    f->react = state == MV3_S_REACT ? (int16_t) move : -1;
    if (state != MV3_S_DODGE)
        f->iframe_end = 0;
}

static void Mv3bToIdle(Mv3Fighter *f)
{
    Mv3bBegin(f, MV3_S_IDLE, -1, MV3B_NONE);
    f->chain = 0;
}

static int Mv3bStartSwing(Mv3World *w, const Mv3World *o, int i,
                            int step, Mv3Evbuf *ev)
{
    Mv3Fighter *f = &w->f[i];
    int m = Mv3bSwingMove(f->wset, step);
    if (m < 0)
        return 0;
    const Mv3Move *mm = &MV3_MOVES[m];
    int32_t arc = Mv3bArc(mm);
    int t =
        Mv3bNearest(o, i,
                     Mv3bReach(mm, MV3B_REACH_DEF) + MV3B_LOCK_EXTRA,
                     arc > MV3B_LOCK_ARC ? arc : MV3B_LOCK_ARC);
    Mv3bBegin(f, MV3_S_SWING, m, t < 0 ? MV3B_NONE : t);
    f->chain = (uint8_t) step;
    f->buf &= (uint8_t) ~ MV3B_BUF_PRESS;
    f->buf_age = 0;
    Mv3bEv(ev, w->frame, MV3_E_SWING, i, t < 0 ? MV3B_NONE : t, m, step);
    return 1;
}

static int Mv3bStartDodge(Mv3World *w, const Mv3World *o, int i,
                            int att, const Mv3In *in, Mv3Evbuf *ev)
{
    Mv3Fighter *f = &w->f[i];
    int32_t face = Mv3bYawTo(f, &o->f[att]);
    int dir = 3;
    int32_t mag2 = (int32_t) in->sx * in->sx + (int32_t) in->sy * in->sy;
    if (mag2 >= MV3B_STICK_DEAD * MV3B_STICK_DEAD)
        dir = Mv3bQuad(Mv3bWrap(f->yaw + Mv3bAtan2(-(int32_t) in->sx, in->sy) - face));      // stick world angle (sx+ = right = yaw - 90) vs the attacker
    int m = Mv3bPick(w, MV3_K_DODGE, f->wset, dir, -1, 1);
    if (m < 0)
        return 0;
    f->yaw = (int16_t) face;
    Mv3bBegin(f, MV3_S_DODGE, m, att);
    f->iframe_end =
        (uint16_t) ((w->frame + MV3_MOVES[m].iframe1_f) & 0xffff);
    f->chain = 0;
    MV3B_CDIR(f) = MV3_MOVES[m].dir;
    f->buf &= (uint8_t) ~ MV3B_BUF_PRESS;
    f->buf_age = 0;
    Mv3bEv(ev, w->frame, MV3_E_DODGE, i, att, m, dir);
    return 1;
}

// the dodge-counter row: own set + the dodge's direction, else own set, else fists (same dir, then any); rows whose contact lies past their
// state end are skipped (two-segment rows like dc-fist-haymaker carry only segment 0: their hit is in segment 1, out of the sim's reach)
static int Mv3bCounterRow(int wset, int dir)
{
    for (int pass = 0; pass < 4; pass++)
        for (int i = 0; i < MV3_MOVES_N; i++) {
            const Mv3Move *m = &MV3_MOVES[i];
            if (m->kind != MV3_K_COUNTER || m->hit0_f >= Mv3bEnd(m))
                continue;
            if (pass == 0 && (m->wset != wset || m->dir != dir))
                continue;
            if (pass == 1 && m->wset != wset)
                continue;
            if (pass == 2 && (m->wset != MV3_WS_FIST || m->dir != dir))
                continue;
            return i;
        }
    return -1;
}

static int Mv3bStartCounter(Mv3World *w, const Mv3World *o, int i,
                              Mv3Evbuf *ev)
{
    Mv3Fighter *f = &w->f[i];
    int j = f->dodged_by;
    if (j >= w->n)
        return 0;
    Mv3Fighter *v = &w->f[j];
    if (!Mv3bEnemy(f, &o->f[j]) || o->f[j].state != MV3_S_MISS
        || v->state != MV3_S_MISS)
        return 0;               // already countered this frame: lower index wins
    int m = Mv3bCounterRow(f->wset, MV3B_CDIR(f) == 0 ? 0 : 3);
    if (m < 0)
        return 0;
    int32_t cf, cr;
    Mv3bRm(m, MV3_MOVES[m].hit0_f, &cf, &cr);  // the counter clip closes this much of the gap itself before its contact
    if (Mv3bDist(f, v) >
        Mv3bReach(&MV3_MOVES[m], MV3B_CTR_REACH) + (cf > 0 ? cf : 0)
        || !Mv3bDyOk(f, v))
        return 0;
    int mc = -1;
    for (int k = 0; MV3_MOVES[m].pair && k < MV3_MOVES_N && mc < 0; k++)        // the row's own victim half
        if (MV3_MOVES[k].kind == MV3_K_COUNTERED && MV3_MOVES[k].name
            && !strcmp(MV3_MOVES[k].name, MV3_MOVES[m].pair))
            mc = k;
    if (mc < 0)
        mc = Mv3bFind(MV3_K_COUNTERED, MV3_MOVES[m].wset,
                       MV3_MOVES[m].dir);
    if (mc < 0)
        mc = Mv3bFindWs(MV3_K_COUNTERED, f->wset);
    if (mc < 0)
        mc = m;                 // the anim plays the row's pair clip on the victim
    int32_t y = Mv3bYawTo(f, v);
    f->yaw = (int16_t) y;
    v->yaw = (int16_t) Mv3bWrap(y + 18000);
    Mv3bBegin(f, MV3_S_COUNTER, m, j);
    Mv3bBegin(v, MV3_S_COUNTERED, mc, i);
    f->chain = v->chain = 0;
    f->dodged_by = MV3B_NONE;
    f->buf &= (uint8_t) ~ MV3B_BUF_PRESS;
    f->buf_age = 0;
    Mv3bEv(ev, w->frame, MV3_E_COUNTER, i, j, m, 0);
    return 1;
}

// ---- pass A: time
static void Mv3bAdvance(Mv3World *w, int i, const Mv3In *in)
{
    Mv3Fighter *f = &w->f[i];
    if (f->state != MV3_S_IDLE && f->state != MV3_S_OUT) {
        f->mf++;
        const Mv3Move *m = Mv3bMv(f->move);
        if (!m || f->mf >= Mv3bEnd(m))
            Mv3bToIdle(f);
    }
    if (f->buf & MV3B_BUF_PRESS) {
        if (++f->buf_age > MV3B_BUF_MAX) {
            f->buf &= (uint8_t) ~ MV3B_BUF_PRESS;
            f->buf_age = 0;
        }
    }
    if (f->dodged_by != MV3B_NONE
        && (int16_t) (uint16_t) (f->counter_end - (uint16_t) w->frame) < 0)
        f->dodged_by = MV3B_NONE;
    if (f->state == MV3_S_IDLE || f->state == MV3_S_OUT) {
        f->px = in->px;
        f->pz = in->pz;
        f->py = in->py;
        f->yaw = (int16_t) Mv3bWrap(in->yaw);
        if (f->state == MV3_S_IDLE)
            f->wset = in->wset < MV3_WS_N ? in->wset : MV3_WS_FIST;
    }
}

// ---- pass B: input
static void Mv3bInput(Mv3World *w, const Mv3World *o, int i,
                       const Mv3In *in, Mv3Evbuf *ev)
{
    Mv3Fighter *f = &w->f[i];
    if (f->state == MV3_S_OUT) {
        f->buf = 0;
        f->buf_age = 0;
        return;
    }
    uint8_t edge = (uint8_t) (in->btn & ~f->prev_btn);
    if (edge & (MV3_B_ATK | MV3_B_DODGE)) {
        uint8_t p = (edge & MV3_B_DODGE) ? MV3_B_DODGE : MV3_B_ATK;
        if (p == MV3_B_DODGE && Mv3bThreat(o, i) < 0)
            p = (edge & MV3_B_ATK) ? MV3_B_ATK : 0;     // nothing coming: the dodge does nothing
        if (p) {
            f->buf = (uint8_t) ((f->buf & MV3B_BUF_CHAIN) | p);
            f->buf_age = 0;
        }
    }
    const Mv3Move *m = Mv3bMv(f->move);
    // combo chain: a press (fresh or still buffered) inside the window commits the next step
    if (f->state == MV3_S_SWING && m && (f->buf & MV3_B_ATK)
        && !(f->buf & MV3B_BUF_CHAIN) && f->chain < 2
        && m->chain1_f > m->chain0_f
        && f->mf + MV3B_CHAIN_EARLY >= m->chain0_f && f->mf < m->chain1_f
        && Mv3bSwingMove(f->wset, f->chain + 1) >= 0) {
        f->buf = MV3B_BUF_CHAIN;
        f->buf_age = 0;
    }
    if (!(in->flags & 1))
        return;                 // engine says no melee now (reload, paired move ...): presses stay buffered
    if (f->state == MV3_S_SWING && m && (f->buf & MV3B_BUF_CHAIN)) {
        int cs = m->chain0_f, hl = Mv3bHitLast(m) + 1;
        if (cs < hl)
            cs = hl;
        if (cs < MV3B_CADENCE)
            cs = MV3B_CADENCE;
        if (cs > Mv3bEnd(m) - 1)
            cs = Mv3bEnd(m) - 1;
        if (f->mf >= cs) {
            Mv3bStartSwing(w, o, i, f->chain + 1, ev);
            return;
        }
    }
    if (f->buf & MV3_B_DODGE) {
        int legal = f->state == MV3_S_IDLE || (f->state == MV3_S_SWING && m
                                               && f->mf >=
                                               Mv3bDodgeCancel(f->move));
        int att = Mv3bThreat(o, i);
        if (att < 0) {
            f->buf &= (uint8_t) ~ MV3B_BUF_PRESS;
            f->buf_age = 0;
            return;
        }                       // the threat passed: drop, never pre-arm
        if (legal)
            Mv3bStartDodge(w, o, i, att, in, ev);
        return;
    }
    if (f->buf & MV3_B_ATK) {
        if (f->dodged_by != MV3B_NONE
            && (f->state == MV3_S_IDLE || f->state == MV3_S_DODGE)
            && Mv3bStartCounter(w, o, i, ev))
            return;
        int legal = f->state == MV3_S_IDLE
            || (f->state == MV3_S_SWING && m && !(f->buf & MV3B_BUF_CHAIN)
                && f->mf >= m->cancel_f && f->mf >= MV3B_CADENCE)
            || (f->state == MV3_S_DODGE && m && f->mf >= m->cancel_f);
        if (legal)
            Mv3bStartSwing(w, o, i, 0, ev);
    }
}

// ---- pass D: contacts
typedef struct {
    uint8_t a, b, kind;
    int16_t move, dmg;
} Mv3bContact;
enum { MV3B_C_HIT, MV3B_C_DODGED, MV3B_C_WHIFF };
static void Mv3bStartReact(Mv3World *w, int v, int a, int swing,
                             Mv3Evbuf *ev)
{
    Mv3Fighter *f = &w->f[v];
    const Mv3Move *sm = Mv3bMv(swing);
    int dir = Mv3bQuad(Mv3bRel(f, &w->f[a]));
    int m =
        Mv3bPick(w, MV3_K_REACT, sm ? sm->wset : w->f[a].wset, dir,
                  sm ? sm->strength : 0, 0);
    Mv3bBegin(f, MV3_S_REACT, m, a);
    f->chain = 0;
    f->dodged_by = MV3B_NONE;
    Mv3bEv(ev, w->frame, MV3_E_REACT, v, a, m, dir);
}

static void Mv3bContacts(Mv3World *w, Mv3Evbuf *ev)
{
    Mv3bContact c[MV3_MAX_F];
    int nc = 0;
    for (int i = 0; i < w->n; i++) {
        Mv3Fighter *f = &w->f[i];
        if (f->slot == MV3B_NONE || f->hit_done)
            continue;
        const Mv3Move *m = Mv3bMv(f->move);
        if (!m)
            continue;
        if (f->state == MV3_S_COUNTER) {
            if (f->mf < m->hit0_f)
                continue;
            int t = f->target;
            if (t < w->n && w->f[t].state == MV3_S_COUNTERED
                && w->f[t].target == i)
                c[nc++] = (Mv3bContact) {
                (uint8_t) i, (uint8_t) t, MV3B_C_HIT, f->move, m->dmg};
            else
            f->hit_done = 1;    // the pair broke (third-party hit): no contact
            continue;
        }
        int hl = Mv3bHitLast(m);
        if (f->state != MV3_S_SWING || f->mf < m->hit0_f || f->mf > hl)
            continue;
        int32_t reach = Mv3bReach(m, MV3B_REACH_DEF), arc = Mv3bArc(m);
        int t = f->target;
        if (t == MV3B_NONE) {
            t = Mv3bNearest(w, i, reach, arc);
            if (t >= 0)
                f->target = (uint8_t) t;
        }
        if (t >= 0 && t < w->n && Mv3bEnemy(f, &w->f[t])) {
            Mv3Fighter *v = &w->f[t];
            if (Mv3bInvuln(v)) {
                c[nc++] = (Mv3bContact) {
                (uint8_t) i, (uint8_t) t, MV3B_C_DODGED, f->move, 0};
                continue;
            }
            int32_t r = Mv3bRel(f, v);
            if (Mv3bDyOk(f, v) && Mv3bDist(f, v) <= reach && r <= arc
                && r >= -arc) {
                c[nc++] = (Mv3bContact) {
                (uint8_t) i, (uint8_t) t, MV3B_C_HIT, f->move, m->dmg};
                continue;
            }
            if (f->mf == hl && v->state == MV3_S_DODGE && v->target == i) {     // dodged out of reach: still the dodger's win
                c[nc++] = (Mv3bContact) {
                (uint8_t) i, (uint8_t) t, MV3B_C_DODGED, f->move, 0};
                continue;
            }
        }
        if (f->mf == hl)
            c[nc++] = (Mv3bContact) {
            (uint8_t) i, (uint8_t) (t < 0 ? MV3B_NONE : t), MV3B_C_WHIFF,
                f->move, 0};
    }
    if (!nc)
        return;
    // outcome events in attacker order; dodges first so a same-frame hit on the attacker (REACT) wins over its MISS
    int16_t hp0[MV3_MAX_F];
    uint8_t first[MV3_MAX_F];
    for (int i = 0; i < MV3_MAX_F; i++) {
        hp0[i] = w->f[i].hp;
        first[i] = MV3B_NONE;
    }
    for (int k = 0; k < nc; k++) {
        Mv3Fighter *f = &w->f[c[k].a];
        f->hit_done = 1;
        if (c[k].kind == MV3B_C_WHIFF) {
            Mv3bEv(ev, w->frame, MV3_E_WHIFF, c[k].a, c[k].b, c[k].move,
                    0);
            continue;
        }
        if (c[k].kind == MV3B_C_DODGED) {
            Mv3Fighter *d = &w->f[c[k].b];
            Mv3bEv(ev, w->frame, MV3_E_DODGED, c[k].b, c[k].a, c[k].move,
                    0);
            uint32_t ce = w->frame + MV3B_COUNTER_WIN;
            const Mv3Move *dm =
                d->state == MV3_S_DODGE ? Mv3bMv(d->move) : 0;
            if (dm && dm->chain1_f > d->mf
                && w->frame - d->mf + dm->chain1_f > ce)
                ce = w->frame - d->mf + dm->chain1_f;   // the dodge's own window
            d->dodged_by = c[k].a;
            d->counter_end = (uint16_t) (ce & 0xffff);
            int mm = Mv3bFindWs(MV3_K_MISS, f->wset);
            if (mm >= 0) {
                Mv3bBegin(f, MV3_S_MISS, mm, c[k].b);
                f->hit_done = 1;
                f->chain = 0;
            }
            continue;
        }
        Mv3Fighter *v = &w->f[c[k].b];
        Mv3bEv(ev, w->frame, MV3_E_HIT, c[k].a, c[k].b, c[k].move,
                c[k].dmg);
        v->hp = (int16_t) (v->hp - c[k].dmg);
        if (first[c[k].b] == MV3B_NONE)
            first[c[k].b] = (uint8_t) k;
    }
    // victims in index order: DOWN or REACT (the counterer's own scripted contact plays no reaction: the countered clip is the reaction)
    for (int v = 0; v < w->n; v++) {
        if (first[v] == MV3B_NONE)
            continue;
        Mv3Fighter *f = &w->f[v];
        const Mv3bContact *k = &c[first[v]];
        if (f->hp <= 0 && hp0[v] > 0) {
            Mv3bEv(ev, w->frame, MV3_E_DOWN, v, k->a, k->move, f->hp);
            Mv3bBegin(f, MV3_S_OUT, -1, MV3B_NONE);
            f->buf = 0;
            f->buf_age = 0;
            f->chain = 0;
            f->dodged_by = MV3B_NONE;
            continue;
        }
        if (f->state == MV3_S_OUT)
            continue;
        if (f->state == MV3_S_COUNTERED && f->target == k->a
            && w->f[k->a].state == MV3_S_COUNTER)
            continue;
        Mv3bStartReact(w, v, k->a, k->move, ev);
    }
}

// ---- pass C: motion
static void Mv3bMotion(Mv3World *w, int i)
{
    Mv3Fighter *f = &w->f[i];
    if (f->slot == MV3B_NONE || f->state == MV3_S_IDLE
        || f->state == MV3_S_OUT)
        return;
    const Mv3Move *m = Mv3bMv(f->move);
    if (!m || !m->dur_f || f->mf >= Mv3bEnd(m))
        return;
    int t = f->target < w->n ? f->target : -1;
    if (f->state == MV3_S_SWING && t >= 0 && f->mf < m->hit0_f && w->f[t].slot != MV3B_NONE) {  // SP soft lock: windup turn, then locked
        int32_t d = Mv3bWrap(Mv3bYawTo(f, &w->f[t]) - f->yaw);
        if (d > MV3B_TURN)
            d = MV3B_TURN;
        else if (d < -MV3B_TURN)
            d = -MV3B_TURN;
        f->yaw = (int16_t) Mv3bWrap(f->yaw + d);
        int64_t gap = Mv3bDist(f, &w->f[t]) - MV3B_LUNGE_TO;   // lunge: close the gap evenly over the frames left to contact
        if (gap > 0) {
            int64_t step = gap / ((int64_t) m->hit0_f - f->mf + 1);
            if (step > MV3B_LUNGE_MAX)
                step = MV3B_LUNGE_MAX;
            int64_t dx = (int64_t) w->f[t].px - f->px, dz =
                (int64_t) w->f[t].pz - f->pz, dd = Mv3bDist(f, &w->f[t]);
            if (dd > 0 && step > 0) {
                f->px += (int32_t) (dx * step / dd);
                f->pz += (int32_t) (dz * step / dd);
            }
        }
    }
    int32_t f0, r0, f1, r1;     // this frame's slice of the cumulative curve: the slices sum to the total exactly
    Mv3bRm(f->move, f->mf, &f0, &r0);
    Mv3bRm(f->move, f->mf + 1, &f1, &r1);
    int64_t df = f1 - f0, dr = r1 - r0;
    if (!df && !dr)
        return;
    int64_t s = Mv3bSin(f->yaw), c = Mv3bCos(f->yaw); // forward = (sin, cos); right = yaw - 90 = (-cos, sin)
    int32_t nx = f->px + (int32_t) ((df * s - dr * c + 8192) >> 14);
    int32_t nz = f->pz + (int32_t) ((df * c + dr * s + 8192) >> 14);
    if ((f->state == MV3_S_SWING || f->state == MV3_S_COUNTER) && t >= 0
        && w->f[t].slot != MV3B_NONE) {
        Mv3Fighter moved = *f;
        moved.px = nx;
        moved.pz = nz;
        int64_t d1 = Mv3bDist(&moved, &w->f[t]);
        if (d1 < MV3B_MIN_SEP && d1 < Mv3bDist(f, &w->f[t]))
            return;             // never walk into the target
    }
    f->px = nx;
    f->pz = nz;
}

// ---- API
void Mv3SimInit(Mv3World *w, uint32_t seed)
{
    memset(w, 0, sizeof *w);
    w->rng = seed ? seed : 0x9e3779b9u;
    for (int i = 0; i < MV3_MAX_F; i++) {
        w->f[i].slot = MV3B_NONE;
        w->f[i].move = -1;
        w->f[i].react = -1;
        w->f[i].target = MV3B_NONE;
        w->f[i].dodged_by = MV3B_NONE;
    }
}

int Mv3SimAdd(Mv3World *w, uint8_t slot, uint8_t team, const Mv3In *in)
{
    for (int i = 0; i < MV3_MAX_F; i++) {
        if (w->f[i].slot != MV3B_NONE)
            continue;
        Mv3Fighter *f = &w->f[i];
        memset(f, 0, sizeof *f);
        f->slot = slot;
        f->team = team;
        f->state = MV3_S_IDLE;
        f->move = -1;
        f->react = -1;
        f->target = MV3B_NONE;
        f->dodged_by = MV3B_NONE;
        f->hp = MV3B_HP0;
        if (in) {
            f->px = in->px;
            f->pz = in->pz;
            f->py = in->py;
            f->yaw = (int16_t) Mv3bWrap(in->yaw);
            f->wset = in->wset < MV3_WS_N ? in->wset : MV3_WS_FIST;
            f->prev_btn = in->btn;      // a button already held on join is not a press
        }
        if (i >= w->n)
            w->n = (uint8_t) (i + 1);
        return i;
    }
    return -1;
}

void Mv3SimRemove(Mv3World *w, int idx)
{
    if (idx < 0 || idx >= MV3_MAX_F)
        return;
    for (int i = 0; i < MV3_MAX_F; i++) {
        Mv3Fighter *f = &w->f[i];
        if (f->target == idx)
            f->target = MV3B_NONE;
        if (f->dodged_by == idx)
            f->dodged_by = MV3B_NONE;
        if ((f->state == MV3_S_COUNTER || f->state == MV3_S_COUNTERED)
            && f->target == MV3B_NONE)
            f->hit_done = 1;
    }
    memset(&w->f[idx], 0, sizeof w->f[idx]);
    w->f[idx].slot = MV3B_NONE;
    w->f[idx].move = -1;
    w->f[idx].react = -1;
    w->f[idx].target = MV3B_NONE;
    w->f[idx].dodged_by = MV3B_NONE;
    while (w->n && w->f[w->n - 1].slot == MV3B_NONE)
        w->n--;
}

void Mv3SimStep(Mv3World *w, const Mv3In in[MV3_MAX_F], Mv3Evbuf *ev)
{
    Mv3World o;                // the world after pass A: what every fighter's decisions read about the others
    if (ev)
        ev->n = 0;
    w->frame++;
    for (int i = 0; i < w->n; i++)
        if (w->f[i].slot != MV3B_NONE)
            Mv3bAdvance(w, i, &in[i]);
    memcpy(&o, w, sizeof o);
    for (int i = 0; i < w->n; i++)
        if (w->f[i].slot != MV3B_NONE)
            Mv3bInput(w, &o, i, &in[i], ev);
    Mv3bContacts(w, ev);
    for (int i = 0; i < w->n; i++)
        Mv3bMotion(w, i);
    for (int i = 0; i < w->n; i++)
        if (w->f[i].slot != MV3B_NONE)
            w->f[i].prev_btn = in[i].btn;
}

static uint32_t mv3b_h(uint32_t h, uint32_t v, int bytes)
{
    for (int k = 0; k < bytes; k++) {
        h ^= (v >> (8 * k)) & 0xff;
        h *= 16777619u;
    } return h;
}

uint32_t Mv3SimHash(const Mv3World *w)
{
    uint32_t h = 2166136261u;
    h = mv3b_h(h, w->frame, 4);
    h = mv3b_h(h, w->rng, 4);
    h = mv3b_h(h, w->n, 1);
    for (int i = 0; i < w->n; i++) {
        const Mv3Fighter *f = &w->f[i];
        h = mv3b_h(h, f->slot, 1);
        h = mv3b_h(h, f->team, 1);
        h = mv3b_h(h, f->state, 1);
        h = mv3b_h(h, f->wset, 1);
        h = mv3b_h(h, (uint16_t) f->move, 2);
        h = mv3b_h(h, f->mf, 2);
        h = mv3b_h(h, (uint32_t) f->px, 4);
        h = mv3b_h(h, (uint32_t) f->pz, 4);
        h = mv3b_h(h, (uint32_t) f->py, 4);
        h = mv3b_h(h, (uint16_t) f->yaw, 2);
        h = mv3b_h(h, f->iframe_end, 2);
        h = mv3b_h(h, f->chain, 1);
        h = mv3b_h(h, f->buf, 1);
        h = mv3b_h(h, f->buf_age, 1);
        h = mv3b_h(h, f->target, 1);
        h = mv3b_h(h, f->hit_done, 1);
        h = mv3b_h(h, f->dodged_by, 1);
        h = mv3b_h(h, f->counter_end, 2);
        h = mv3b_h(h, (uint16_t) f->react, 2);
        h = mv3b_h(h, (uint16_t) f->hp, 2);
        h = mv3b_h(h, f->prev_btn, 1);
        h = mv3b_h(h, MV3B_CDIR(f), 1);
    }
    return h;
}
