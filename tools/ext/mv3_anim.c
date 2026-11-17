// mv3_anim.c - melee v3 presentation of the bodies (spec re/melee-v3.md, contract mv3.h). Included after melee_v2.c in t2r-ext.c. Log prefix "mv3 anim:".
//
// Every frame (mv3_anim_apply) each bubble fighter's body shows the sim: the clip of its move at the sim's phase, its root on the sim's position / yaw.
//  - Clip: animate_ on ANIM_LAYER with blend-in (key 28) + start phase (key 58) - mhr_animate / mdg_puppet_animate's argument array (melee_hitreact.c, melee_dodge.c).
//    The clip time is (move frame + sub-frame) / 60 * rate; the engine plays it on its own and is re-seeked (set-animation-phase, psync_hold_end_pose's call)
//    only when the layer's phase drifts > MV3A_SEEK_F frames from the sim (a rollback moved the move start) - no per-frame phase writes, no jitter.
//  - Root: the sim moves the fighter by the clip's root motion itself, so the engine's must not add on top. Same answer as mdg's pinned combat dodge
//    (melee_dodge.c mdg_mv_tick, m->pin: "every frame the position is OUR path ... and nothing else", live since v11): set-object-locator on the body every
//    frame overrides whatever the clip's align channel moved it by since the last write. Small errors (rollback corrections, the puppet's net pose at the hold)
//    fade with a critically damped spring (MV3A_SMOOTH_S); a jump > MV3A_SNAP_M snaps.
//  - Puppets: while a remote fighter is out of MV3_S_IDLE its puppet is HELD with psync's channel suspension (paired.c g_ps_pup[s].on: winmm's AnimCmdList replay,
//    root write, pose writer gate and net locator are bypassed for it - psync_replay_bypass / psync_root_bypass / psync_gate_slot / psync_locator_skip), so our
//    clip + position win over the streamed pose. Back in IDLE the hold ends exactly as psync_pup_release ends one: clip blended out, winmm's replay restarted
//    (+0x77e4), the locator glides onto the net position (psync_hb_arm -> psync_locator_handback).
//  - Bodies owned by psync / ctxdown / finishers / downed / death are left alone (mv3_anim_body_busy, also for the glue).
//  - Engine melee off inside a bubble (mv3_anim_suppress_engine): player-disable-melee + player-disable-roll every frame (melee_pace.c / the dead-body lock in
//    paired.c), the melee-attack input bit hidden from the engine's C++ combo reader (padinj_block, melee_pace.c's lock), an engine swing / dodge already running
//    is cancelled (cancel-melee-action, mdg_combat_play). All three are leases refreshed every frame: a glue that stops calling releases them by itself.
// Test hook (live check, off by default): t2r-mv3-animtest.txt next to the exe = "<clip name> [pin]" - our body plays the clip frozen at phases 0 .. 0.9, then
// runs it at speed 1 with a phase jump half way; logs read-back phase, drift and how far the body moved ("mv3 animtest:").
#include "mv3.h"

#define MV3A_SEEK_F 2.0f        // re-seek when the layer is off the sim by more than this (frames)
#define MV3A_SMOOTH_S 0.08f     // spring time of the position / yaw error (critically damped)
#define MV3A_SNAP_M 1.0f        // a larger position error snaps
#define MV3A_VMAX 9.0f          // m/s: per-frame sim motion up to this is followed 1:1, more is a correction (smoothed)
#define MV3A_YMAX 900.0f        // deg/s, same for yaw
#define MV3A_IN_IDLE 0.12f      // blend-in from locomotion (s)
#define MV3A_IN_MOVE 0.08f      // blend-in from another bubble move (chain / reaction interrupt)
#define MV3A_OUT_OWN 0.20f      // blend-out back to locomotion, our body (mhr's is 0.45 for a lone reaction; a bubble returns to the stick faster)
#define MV3A_OUT_PUP 0.15f      // blend-out of a puppet onto its streamed pose (the hand-back glide moves its root)
#define MV3A_US 0xffffffffu     // mv3_anim_body_busy: our own body
#ifndef MV3_MOVE_RATE
#define MV3_MOVE_RATE(m) 1.0f   // the move table has no playback rate yet (interface request): clips run at authored speed
#endif
// later modules (melee_dodge.c, padinject.c, melee_hitreact.c are included after this file)
static const char *MdgBusy(void);
static int MdgIsDodgeMove(uint64_t m);
static int PadinjBlock(const char *name, uint64_t until);
static int MhrRunning(void);

enum { MV3A_FREE, MV3A_B_NOMATCH, MV3A_B_OWN_BUSY, MV3A_B_MHR,
        MV3A_B_DOWNED, MV3A_B_DEAD, MV3A_B_PSYNC, MV3A_B_HELD,
        MV3A_B_NOBODY };
static const char *const MV3A_BUSY_N[] =
    { "free", "not in a match", "own body busy", "SP hit reaction running",
"downed", "dead", "psync / ctxdown / finisher pair", "held by psync (corpse / pair)", "no body" };

typedef struct {
    int on, held, us, layer_lost;
    uint64_t ent, sid;
    int16_t move;
    uint32_t start_f;           // the clip on the body: move index + world frame it started
    float dur, rate;            // engine clip length (s), playback rate
    int skip, frozen;           // frames the drift check waits (a phase write lands at the next anim update); clip held on its end pose
    float off[2], ov[2], offy, ovy;     // displayed - sim (m / deg) and its spring velocity
    float tgt[3], tyaw;
    int tgt_ok;                 // last sim target (x z y m, yaw deg)
    float y_own;                // our body: the engine's ground height
    double last_ms, t0_ms;
    uint32_t n_frames, n_seek, n_snap, n_clip;
    uint64_t wpn;               // our held weapon id at the take (the clip must keep it in the hand)
} Mv3aBody;
static struct {
    Mv3aBody b[T2R_MAX_PEERS]; // by network slot (our own slot included)
    int sup;
    uint64_t sup_cancel_ms;
    uint32_t sup_n;
    int sup_blk;
    double last_apply_ms;
} g_mv3a;
static __attribute__((aligned(16)))
PsyncBf g_mv3aBf[T2R_MAX_PEERS];

// ---- engine calls (argument conventions of the existing call sites)
// animate_(ent, sid, [28 blend-in, 58 start phase]) - mhr_animate (melee_hitreact.c), no ap-ref: the clip plays in the body's own frame
static uint64_t Mv3aAnimate(uint64_t ent, uint64_t sid, float phase,
                             float blend)
{
    int n = phase > 0.0005f ? 2 : 1;
    uint64_t aa[4] = { FNV64("single-frame"), 16, (uint64_t) n, 1 };
    uint64_t arr = DcCall(FNV64("#%alloc-array"), 4, aa);
    if (!arr || arr == ~0ULL || IsBadReadPtr((void *) arr, 8))
        return ~0ULL;
    uint64_t *tab = *(uint64_t **) arr;
    if (!tab || IsBadReadPtr(tab, 8 * (size_t) n))
        return ~0ULL;
    for (int i = 0; i < n; i++)
        if (!tab[i] || AllocEntryBad((void *) arr, (void *) tab[i], 16))
            return ~0ULL;
    uint8_t *e0 = (uint8_t *) tab[0];
    *(int32_t *) e0 = 28;
    *(uint64_t *) (e0 + 8) = 0;
    memcpy(e0 + 8, &blend, 4);
    if (n == 2) {
        uint8_t *e1 = (uint8_t *) tab[1];
        *(int32_t *) e1 = 58;
        *(uint64_t *) (e1 + 8) = 0;
        memcpy(e1 + 8, &phase, 4);
    }
    uint64_t a[3] = { ent, sid, arr };
    uint64_t r = DcCall(FNV64("animate_"), 3, a);
    FaNote(ent, NULL, sid, r, 0);      // visdiag's pose-pop hints see our starts
    return r;
}

static uint64_t Mv3aSetF(const char *fn, uint64_t ent, uint64_t sid,
                           float v)
{                               // set-animation-phase / -speed (ent, value, layer, sid): psync_hold_end_pose / psync_rate
    uint64_t b[4] = { ent, 0, ANIM_LAYER, sid };
    memcpy(&b[1], &v, 4);
    return DcCall(Fnv64Rt(fn), 4, b);
}

static int Mv3aYawQ(float yaw_deg, float *q)
{                               // uc4_yaw's convention: q = (0, sin y/2, 0, cos y/2), yaw = atan2(fwd x, fwd z)
    float h = yaw_deg * 0.00872665f;
    q[0] = 0.0f;
    q[1] = __builtin_sinf(h);
    q[2] = 0.0f;
    q[3] = __builtin_cosf(h);
    return 1;
}

static float Mv3aWrap(float a)
{
    while (a > 180.0f)
        a -= 360.0f;
    while (a < -180.0f)
        a += 360.0f;
    return a;
}

// critically damped spring of x toward 0 (SmoothDamp form): stable for any dt, no overshoot
static void Mv3aSpring(float *x, float *v, float st, float dt)
{
    float om = 2.0f / st, k = om * dt, e =
        1.0f / (1.0f + k + 0.48f * k * k + 0.235f * k * k * k);
    float tmp = (*v + om * *x) * dt;
    *v = (*v - om * tmp) * e;
    *x = (*x + tmp) * e;
}

// ---- who owns a body
static int Mv3aPsyncHas(uint32_t s)
{
    return g_ps.role && (g_ps.att == (int) s || g_ps.vic == (int) s);
}

// 0 = free for the bubble; else a MV3A_B_* reason (MV3A_BUSY_N). slot_or_us = a network slot, or MV3A_US / our own slot for our body
static int Mv3AnimBodyBusy(uint32_t slot_or_us)
{
    uint32_t me = PsyncSelf();
    if (slot_or_us == MV3A_US || slot_or_us == me) {
        const char *b = MdgBusy();     // match / downed / dying / stun / execution / psync_own / finisher / intro / revive / backpack
        if (b)
            return MatchSubstate() !=
                6 ? MV3A_B_NOMATCH : MV3A_B_OWN_BUSY;
        if (MhrRunning())
            return MV3A_B_MHR;  // melee_hitreact's own reaction pins the body (v1/v2 path) - should be stood down by the glue
        return MV3A_FREE;
    }
    uint32_t s = slot_or_us;
    if (s >= T2R_MAX_PEERS)
        return MV3A_B_NOBODY;
    if (MatchSubstate() != 6)
        return MV3A_B_NOMATCH;
    if (g_peerDowned[s])
        return MV3A_B_DOWNED;
    if (g_puppetDeadHere[s])
        return MV3A_B_DEAD;
    if (Mv3aPsyncHas(s))
        return MV3A_B_PSYNC;
    if (g_ps_pup[s].h
        && !(g_mv3a.b[s].held && g_ps_pup[s].h == g_mv3a.b[s].ent))
        return MV3A_B_HELD;
    uint64_t h = PuppetHandle(s);
    if (!h)
        return MV3A_B_NOBODY;
    if (PsyncDead(h))
        return MV3A_B_DEAD;
    return MV3A_FREE;
}

// ---- puppet hold (psync's channel suspension, psync_pup_take minus its collider / hit-reaction changes: a bubble body stays solid and hittable)
static int Mv3aHold(int s, uint64_t h)
{
    if (g_ps_pup[s].h && g_ps_pup[s].h != h)
        return 0;
    if (Mv3aPsyncHas((uint32_t) s))
        return 0;
    if (g_ps_pup[s].on && g_ps_pup[s].h == h)
        return 1;
    g_ps_pup[s].corpse = 0;
    g_ps_pup[s].h = h;
    g_ps_pup[s].rep = g_ps_pup[s].root = g_ps_pup[s].gate =
        g_ps_pup[s].loc = g_ps_pup[s].drv = 0;
    g_ps_pup[s].early = 0;
    g_ps_pup[s].coll_off = g_ps_pup[s].hr_off = 0;
    g_ps_hb[s].h = 0;           // a hand-back still gliding from an earlier hold: the hold owns the locator now
    g_ps_pup[s].since = PsyncMs();
    PsyncPupCache(s);
    g_ps_pup[s].on = !g_ps_no_suspend;
    return 1;
}

static void Mv3aUnhold(int s, uint64_t h, const char *why)
{
    if (g_ps_pup[s].h != h)
        return;                 // psync took it meanwhile (mv3a_handoff) - its own release ends it
    long rep = g_ps_pup[s].rep, root = g_ps_pup[s].root, gate =
        g_ps_pup[s].gate, loc = g_ps_pup[s].loc, drv = g_ps_pup[s].drv;
    g_ps_pup[s].on = 0;
    if (s < T2R_ENGINE_PEERS)
        InterlockedOr((volatile LONG *) (PA_SLOT(s) + 0x77e4), 3);      // winmm's replay restarts clean (psync_pup_release)
    if (!PsyncDead(h))
        PsyncHbArm(s, h, why);        // locator glides onto the net position
    g_ps_pup[s].h = 0;
    g_ps_pup[s].proc = NULL;
    g_ps_pup[s].ac = NULL;
    static int nl;
    if (LOGCAP(nl, 200))
        Xlog("mv3 anim: slot %d puppet hold released (%s): replay bypassed %ld, root %ld, gate %ld, net locator %ld, placed %ld", s, why, rep, root, gate, loc, drv);
}

// ---- one body
static void Mv3aRelease(int s, const char *why)
{
    Mv3aBody *b = &g_mv3a.b[s];
    if (!b->on)
        return;
    int psync_took = !b->us && Mv3aPsyncHas((uint32_t) s);
    if (b->sid && b->ent && !psync_took)
        PsyncStop(b->ent, b->sid, b->us ? MV3A_OUT_OWN : MV3A_OUT_PUP);
    if (b->held && !psync_took)
        Mv3aUnhold(s, b->ent, why);
    Xlog("mv3 anim: %s %d released (%s) after %.0f ms, %u frames: %u clips, %u re-seeks, %u snaps%s", b->us ? "our body, slot" : "puppet slot", s, why, PsyncMs() - b->t0_ms, b->n_frames, b->n_clip, b->n_seek, b->n_snap, psync_took ? " - psync owns it now (its pair plays, its release hands it back)" : "");
    memset(b, 0, sizeof *b);
}

// psync started a pair on a puppet we hold: psync_pup_take saw our .on + same handle and returned early, so its collider / hit-reaction setup did not run - redo it
static void Mv3aHandoff(int s)
{
    Mv3aBody *b = &g_mv3a.b[s];
    if (!b->held || !Mv3aPsyncHas((uint32_t) s))
        return;
    if (g_ps_pup[s].h == b->ent && g_ps_pup[s].on) {
        g_ps_pup[s].on = 0;
        PsyncPupTake(s, b->ent, "mv3 bubble hand-off");
    }
    b->held = 0;
    Mv3aRelease(s, "psync pair took the body");
}

static int Mv3aBegin(int s, uint64_t ent, int us)
{
    Mv3aBody *b = &g_mv3a.b[s];
    memset(b, 0, sizeof *b);
    b->ent = ent;
    b->us = us;
    b->move = -1;
    b->t0_ms = b->last_ms = PsyncMs();
    if (!us && !Mv3aHold(s, ent)) {
        static int nl;
        if (LOGCAP(nl, 40))
            Xlog("mv3 anim: puppet slot %d - hold refused (psync / corpse owns it)", s);
        return 0;
    }
    b->held = !us;
    if (us) {
        uint64_t none = 0;
        b->wpn = DcCall(FNV64("get-player-weapon-id"), 0, &none);
    }
    b->on = 1;
    return 1;
}

// a new clip on the body (move changed, or the same move restarted / shifted by > MV3A_SEEK_F frames)
static void Mv3aClip(int s, const Mv3Move *m, int16_t mi,
                      uint32_t start_f, float t_s)
{
    Mv3aBody *b = &g_mv3a.b[s];
    float dur = PsyncDur(b->us ? SID_PLAYER : b->ent, m->sid), rate =
        MV3_MOVE_RATE(m);
    if (!(rate > 0.05f))
        rate = 1.0f;
    float ph = dur > 0.0f ? t_s * rate / dur : 0.0f;
    if (ph > 0.98f)
        ph = 0.98f;
    if (ph < 0.0f)
        ph = 0.0f;
    float bl = b->sid ? MV3A_IN_MOVE : MV3A_IN_IDLE;
    uint64_t r = ~0ULL, ent = b->us ? SID_PLAYER : b->ent;
    if (dur > 0.0f) {
        r = Mv3aAnimate(ent, m->sid, ph, bl);
        if (rate != 1.0f)
            Mv3aSetF("set-animation-speed", ent, m->sid, rate);
    }
    // the previous clip of the body is replaced by the new instance's blend-in (same layer); a different sid left playing under it is stopped
    if (b->sid && b->sid != m->sid)
        PsyncStop(ent, b->sid, bl);
    b->sid = dur > 0.0f ? m->sid : 0;
    b->move = mi;
    b->start_f = start_f;
    b->dur = dur;
    b->rate = rate;
    b->skip = 2;
    b->frozen = 0;
    b->layer_lost = 0;
    b->n_clip++;
    static int nl;
    if (dur <= 0.0f) {
        if (LOGCAP(nl, 120))
            Xlog("mv3 anim: %s %d move %d %s - clip NOT LOADED here (get-animation-duration 0): the body is placed, no clip", b->us ? "our body, slot" : "puppet slot", s, mi, m->name ? m->name : "?");
        return;
    }
    if (LOGCAP(nl, 400))
        Xlog("mv3 anim: %s %d clip %s (move %d, start f%u) at phase %.3f (t %.0f ms, clip %.2f s, table %.2f s, rate %.2f, blend %.2f) -> %llx", b->us ? "our body, slot" : "puppet slot", s, m->name ? m->name : "?", mi, start_f, ph, t_s * 1000.0f, dur, m->dur_f / 60.0f, rate, bl, (unsigned long long) r);
}

// phase follow: the engine plays the clip; only a drift > MV3A_SEEK_F frames (a rollback shifted the move) is written back
static void Mv3aPhase(int s, const Mv3Move *m, float t_s, float dt)
{
    Mv3aBody *b = &g_mv3a.b[s];
    if (!b->sid || b->dur <= 0.0f)
        return;
    uint64_t ent = b->us ? SID_PLAYER : b->ent;
    float want = t_s * b->rate / b->dur;
    if (want >= 0.985f) {       // the sim holds the move past the clip's end: keep its last pose (no fall back to the base pose)
        if (!b->frozen) {
            Mv3aSetF("set-animation-phase", ent, b->sid, 0.985f);
            Mv3aSetF("set-animation-speed", ent, b->sid, 0.0f);
            b->frozen = 1;
        }
        return;
    }
    if (b->skip > 0) {
        b->skip--;
        return;
    }
    float ph = PsyncPhase(ent, b->sid);
    if (ph < 0.0f) {            // the layer is gone (another clip / the engine dropped it): a fresh instance at the sim's phase
        uint64_t r = Mv3aAnimate(ent, b->sid, want, 0.05f);
        if (b->rate != 1.0f)
            Mv3aSetF("set-animation-speed", ent, b->sid, b->rate);
        b->skip = 2;
        b->layer_lost++;
        static int nl;
        if (LOGCAP(nl, 60))
            Xlog("mv3 anim: %s %d clip %s layer lost at phase %.3f - re-applied (-> %llx, %d times)", b->us ? "our body, slot" : "puppet slot", s, m->name ? m->name : "?", want, (unsigned long long) r, b->layer_lost);
        return;
    }
    // the read is the last anim update's phase: one frame behind (psync_rate takes this frame's dt out the same way)
    float exp_read = (t_s - dt) * b->rate / b->dur;
    float drift_f = (ph - exp_read) * b->dur / b->rate * 60.0f;
    if (__builtin_fabsf(drift_f) <= MV3A_SEEK_F)
        return;
    uint64_t r = Mv3aSetF("set-animation-phase", ent, b->sid, want);
    b->skip = 2;
    b->n_seek++;
    static int nl;
    if (LOGCAP(nl, 200))
        Xlog("mv3 anim: %s %d re-seek %s: layer %.3f vs sim %.3f (%+.1f frames) -> phase %.3f (-> %llx, %u re-seeks)", b->us ? "our body, slot" : "puppet slot", s, m->name ? m->name : "?", ph, exp_read, drift_f, want, (unsigned long long) r, b->n_seek);
}

// root on the sim: followed 1:1 up to MV3A_VMAX, anything beyond is an error that the spring removes; > MV3A_SNAP_M snaps
static void Mv3aPlace(int s, const Mv3Fighter *f, float dt)
{
    Mv3aBody *b = &g_mv3a.b[s];
    float tx = (float) f->px * 0.001f, tz = (float) f->pz * 0.001f, ty =
        (float) f->py * 0.001f, tyaw = Mv3aWrap((float) f->yaw * 0.01f);
    float lb[16];
    const float *l = b->us ? Uc4MyLoc(lb) : PsyncLoc(b->ent, lb);
    if (!l || !PsLocOk(l))
        return;
    if (b->us)
        ty = l[1];              // our body keeps the engine's ground height (mdg's pin does the same)
    if (!b->tgt_ok) {           // first frame: the error = where the body stands now
        b->off[0] = l[0] - tx;
        b->off[1] = l[2] - tz;
        b->offy = Mv3aWrap(Uc4Yaw(l) - tyaw);
        b->ov[0] = b->ov[1] = b->ovy = 0.0f;
        if (b->us)
            b->y_own = l[1];
        else if (!(__builtin_fabsf(ty - l[1]) < 3.0f))
            ty = l[1];
        b->tgt_ok = 1;
    } else {
        float dx = tx - b->tgt[0], dz = tz - b->tgt[1], d =
            __builtin_sqrtf(dx * dx + dz * dz), mx = MV3A_VMAX * dt;
        if (mx < 0.03f)
            mx = 0.03f;
        if (d > mx) {
            float k = (d - mx) / d;
            b->off[0] -= dx * k;
            b->off[1] -= dz * k;
        }                       // displayed stays continuous, the excess is error
        float dy = Mv3aWrap(tyaw - b->tyaw), my = MV3A_YMAX * dt;
        if (my < 6.0f)
            my = 6.0f;
        if (dy > my)
            b->offy -= dy - my;
        else if (dy < -my)
            b->offy -= dy + my;
        b->offy = Mv3aWrap(b->offy);
        if (!b->us && !(__builtin_fabsf(ty - b->tgt[2]) < 3.0f))
            ty = b->tgt[2];
    }
    float e =
        __builtin_sqrtf(b->off[0] * b->off[0] + b->off[1] * b->off[1]);
    if (e > MV3A_SNAP_M) {
        b->n_snap++;
        static int nl;
        if (LOGCAP(nl, 100))
            Xlog("mv3 anim: %s %d snap - %.2f m off the sim (%.2f %.2f -> %.2f %.2f), %u snaps", b->us ? "our body, slot" : "puppet slot", s, e, tx + b->off[0], tz + b->off[1], tx, tz, b->n_snap);
        b->off[0] = b->off[1] = b->ov[0] = b->ov[1] = 0.0f;
    } else {
        Mv3aSpring(&b->off[0], &b->ov[0], MV3A_SMOOTH_S, dt);
        Mv3aSpring(&b->off[1], &b->ov[1], MV3A_SMOOTH_S, dt);
    }
    if (__builtin_fabsf(b->offy) > 90.0f) {
        b->offy = 0.0f;
        b->ovy = 0.0f;
    } else
        Mv3aSpring(&b->offy, &b->ovy, MV3A_SMOOTH_S, dt);
    b->tgt[0] = tx;
    b->tgt[1] = tz;
    b->tgt[2] = ty;
    b->tyaw = tyaw;
    float l8[8] = { tx + b->off[0], ty, tz + b->off[1], 1.0f, 0, 0, 0, 1 };
    float q[4];
    Mv3aYawQ(tyaw + b->offy, q);
    memcpy(l8 + 4, q, 16);
    if (!PsLocOk(l8))
        return;
    PsyncBfFromLoc(&g_mv3aBf[s], l8);
    mo_write(b->us ? SID_PLAYER : b->ent, &g_mv3aBf[s],
             MO_SRC("mv3.place"), 0);
    if (b->held)
        g_ps_pup[s].drv++;
}

// ---- engine melee suppression (leases, refreshed every frame by mv3a_suppress_tick)
static void Mv3AnimSuppressEngine(int on)
{
    if (on && !g_mv3a.sup) {
        g_mv3a.sup = 1;
        g_mv3a.sup_n = 0;
        uint64_t me = SID_PLAYER, r = 0;
        int run = g_msAtk || MdgIsDodgeMove(g_msCurmove);
        if (run) {
            r = DcCall(0x394888944e949d5fULL /* cancel-melee-action */ ,
                        1, &me);
            g_mv3a.sup_cancel_ms = GetTickCount64();
        }
        Xlog("mv3 anim: engine melee OFF (bubble) - %s",
             run ? "an engine swing / dodge was running: cancelled" :
             "no engine swing running");
        if (run)
            Xlog("mv3 anim: cancel-melee-action on move %llx -> %llx",
                 (unsigned long long) g_msCurmove,
                 (unsigned long long) r);
    } else if (!on && g_mv3a.sup) {
        g_mv3a.sup = 0;
        if (g_mv3a.sup_blk)
            PadinjBlock("melee-attack", 0);
        g_mv3a.sup_blk = 0;
        Xlog("mv3 anim: engine melee back ON (bubble over) after %u frames", g_mv3a.sup_n);
    }
}

static void Mv3aSuppressTick(void)
{
    if (!g_mv3a.sup)
        return;
    uint64_t now = GetTickCount64(), none = 0;
    g_mv3a.sup_n++;
    DcCall(0x0fa21391e0e4e478ULL /* player-disable-melee */ , 0, &none);
    DcCall(FNV64("player-disable-roll"), 0, &none);
    // the engine's C++ combo reads the pad masks, not the scripted command (padinject.c): hide the melee bit. The glue reads the real button with
    // padinj_blocked_raw() (melee_pace.c's press read). One mask only in padinject: "evade" stays visible (the glue reads it with pad_cmd_on) and an engine
    // dodge that starts anyway is cancelled below.
    g_mv3a.sup_blk = PadinjBlock("melee-attack", now + 120)
        || g_mv3a.sup_blk;
    if ((g_msAtk || MdgIsDodgeMove(g_msCurmove))
        && now - g_mv3a.sup_cancel_ms > 250) {
        uint64_t me = SID_PLAYER, r =
            DcCall(0x394888944e949d5fULL /* cancel-melee-action */ , 1,
                    &me);
        g_mv3a.sup_cancel_ms = now;
        static int nl;
        if (LOGCAP(nl, 60))
            Xlog("mv3 anim: engine %s started inside the bubble (move %llx, atk %d) - cancelled (-> %llx)", g_msAtk ? "swing" : "dodge", (unsigned long long) g_msCurmove, g_msAtk, (unsigned long long) r);
    }
}

// ---- per frame
static void Mv3AnimRelease(uint32_t slot)
{
    if (slot == MV3A_US)
        slot = PsyncSelf();
    if (slot < T2R_MAX_PEERS)
        Mv3aRelease((int) slot, "released by the glue");
}

static void Mv3AnimApply(const Mv3World *w, int local_idx,
                           uint32_t world_frame_frac_ms)
{
    double now = PsyncMs();
    float dt =
        g_mv3a.last_apply_ms >
        0.0 ? (float) ((now - g_mv3a.last_apply_ms) / 1000.0) : 1.0f /
        60.0f;
    if (!(dt > 0.0f))
        dt = 1.0f / 60.0f;
    if (dt > 0.1f)
        dt = 0.1f;
    g_mv3a.last_apply_ms = now;
    Mv3aSuppressTick();
    uint8_t seen[T2R_MAX_PEERS] = { 0 };
    uint32_t me = PsyncSelf();
    float frac_f = (float) world_frame_frac_ms * 0.06f;
    if (frac_f > 1.0f)
        frac_f = 1.0f;          // ms -> frames, at most one frame ahead
    for (int i = 0; w && i < w->n && i < MV3_MAX_F; i++) {
        const Mv3Fighter *f = &w->f[i];
        if (f->slot == 0xff || f->slot >= T2R_MAX_PEERS)
            continue;
        int s = f->slot, us = i == local_idx;
        if (us && (uint32_t) s != me) {
            static int nl;
            if (LOGCAP(nl, 5))
                Xlog("mv3 anim: local fighter %d has slot %d but we are slot %u - taken as ours", i, s, me);
        }
        seen[s] = 1;
        Mv3aBody *b = &g_mv3a.b[s];
        if (b->on && b->held && Mv3aPsyncHas((uint32_t) s)) {
            Mv3aHandoff(s);
            continue;
        }
        int busy = Mv3AnimBodyBusy(us ? MV3A_US : (uint32_t) s);
        if (busy) {
            if (b->on) {
                char why[64];
                snprintf(why, sizeof why, "busy: %s", MV3A_BUSY_N[busy]);
                Mv3aRelease(s, why);
            }
            continue;
        }
        int16_t mi = f->state == MV3_S_REACT
            && f->react >= 0 ? f->react : f->move;
        int active = f->state != MV3_S_IDLE && f->state != MV3_S_OUT
            && mi >= 0 && mi < MV3_MOVES_N;
        if (!active) {
            if (b->on)
                Mv3aRelease(s,
                             f->state == MV3_S_IDLE ? "idle" : f->state ==
                             MV3_S_OUT ? "out of the bubble" : "no move");
            continue;
        }
        uint64_t ent = us ? SID_PLAYER : PuppetHandle((uint32_t) s);
        if (!ent) {
            if (b->on)
                Mv3aRelease(s, "no body");
            continue;
        }
        if (b->on && b->ent != ent)
            Mv3aRelease(s, "body replaced (respawn)");
        if (b->on && b->held && (g_ps_pup[s].h != ent || !g_ps_pup[s].on)) {    // psync_finish / psync_tick of another pair released every g_ps_pup slot (interface request)
            static int nl;
            if (LOGCAP(nl, 40))
                Xlog("mv3 anim: puppet slot %d hold lost (a psync pair of other bodies released it) - taken again", s);
            if (!Mv3aHold(s, ent)) {
                Mv3aRelease(s, "hold lost");
                continue;
            }
        }
        if (!b->on && !Mv3aBegin(s, ent, us))
            continue;
        if (b->held && g_ps.role) {
            uint64_t a = ent;
            DcCall(FNV64("npc-enable-character-collider"), 1, &a);
        }                       // psync_tick turns colliders off on every g_ps_pup body
        const Mv3Move *m = &MV3_MOVES[mi];
        uint32_t start_f = w->frame - f->mf;
        float t_s = ((float) f->mf + frac_f) / 60.0f;
        int32_t sh = (int32_t) (start_f - b->start_f);
        if (b->move != mi
            || (sh > (int32_t) MV3A_SEEK_F || sh < -(int32_t) MV3A_SEEK_F))
            Mv3aClip(s, m, mi, start_f, t_s);
        else {
            b->start_f = start_f;
            Mv3aPhase(s, m, t_s, dt);
        }                       // a small shift is a drift: the phase check handles it
        Mv3aPlace(s, f, dt);
        if (us && !(b->n_frames % 30)) {
            uint64_t none = 0, wp =
                DcCall(FNV64("get-player-weapon-id"), 0, &none);
            if (wp != b->wpn) {
                static int nl;
                if (LOGCAP(nl, 30))
                    Xlog("mv3 anim: our held weapon changed mid move %s: %llx -> %llx (a clip action holstered / swapped it)", m->name ? m->name : "?", (unsigned long long) b->wpn, (unsigned long long) wp);
                b->wpn = wp;
            }
        }
        b->n_frames++;
        b->last_ms = now;
    }
    for (int s = 0; s < T2R_MAX_PEERS; s++)
        if (g_mv3a.b[s].on && !seen[s])
            Mv3aRelease(s, "left the bubble");
}

// bubble over / glue reset: every body back
static void Mv3AnimReleaseAll(const char *why)
{
    for (int s = 0; s < T2R_MAX_PEERS; s++)
        if (g_mv3a.b[s].on)
            Mv3aRelease(s, why);
    Mv3AnimSuppressEngine(0);
    g_mv3a.last_apply_ms = 0.0;
}

static int Mv3AnimActive(void)
{
    for (int s = 0; s < T2R_MAX_PEERS; s++)
        if (g_mv3a.b[s].on)
            return 1;
    return 0;
}

// ---- live test hook: t2r-mv3-animtest.txt = "<clip name> [pin]"
static struct {
    int on, step, pin;
    char name[96];
    uint64_t sid, poll_ms;
    double t0, step_ms, play_ms;
    float dur, from[8];
    int jumped;
    uint32_t logs;
} g_mv3t;
static void Mv3aTestStop(const char *why)
{
    if (!g_mv3t.on)
        return;
    Mv3aSetF("set-animation-speed", SID_PLAYER, g_mv3t.sid, 1.0f);
    PsyncStop(SID_PLAYER, g_mv3t.sid, MV3A_OUT_OWN);
    Xlog("mv3 animtest: %s stopped (%s)", g_mv3t.name, why);
    g_mv3t.on = 0;
}

static float Mv3aTestMoved(void)
{
    float lb[16];
    const float *l = Uc4MyLoc(lb);
    if (!l)
        return -1.0f;
    float dx = l[0] - g_mv3t.from[0], dz = l[2] - g_mv3t.from[2];
    return __builtin_sqrtf(dx * dx + dz * dz);
}

static void Mv3AnimTestTick(void)
{
    uint64_t now = GetTickCount64();
    if (now - g_mv3t.poll_ms >= 2000) {
        g_mv3t.poll_ms = now;
        char ln[128] = "";
        FILE *fp = ExeSideOpen("t2r-mv3-animtest.txt", "r");
        if (fp) {
            if (!fgets(ln, sizeof ln, fp))
                ln[0] = 0;
            fclose(fp);
        }
        char nm[96] = "", opt[16] = "";
        if (ln[0])
            sscanf(ln, "%95s %15s", nm, opt);
        if (!nm[0]) {
            Mv3aTestStop("file gone");
            g_mv3t.name[0] = 0;
            return;
        }
        if (strcmp(nm, g_mv3t.name)) {
            Mv3aTestStop("new clip");
            snprintf(g_mv3t.name, sizeof g_mv3t.name, "%s", nm);
            g_mv3t.step = -1;
            g_mv3t.step_ms = 0;
        }
        g_mv3t.pin = !strcmp(opt, "pin");
    }
    if (!g_mv3t.name[0])
        return;
    if (Mv3AnimActive() || MdgBusy()) {
        if (g_mv3t.on)
            Mv3aTestStop("bubble / busy");
        return;
    }
    double t = PsyncMs();
    if (!g_mv3t.on) {
        if (t < g_mv3t.step_ms)
            return;             // pause between runs
        g_mv3t.sid = Fnv64Rt(g_mv3t.name);
        g_mv3t.dur = PsyncDur(SID_PLAYER, g_mv3t.sid);
        if (g_mv3t.dur <= 0.0f) {
            Xlog("mv3 animtest: %s (%016llx) not loaded here (get-animation-duration 0) - retry in 5 s", g_mv3t.name, (unsigned long long) g_mv3t.sid);
            g_mv3t.step_ms = t + 5000.0;
            return;
        }
        float lb[16];
        const float *l = Uc4MyLoc(lb);
        if (!l)
            return;
        memcpy(g_mv3t.from, l, 32);
        g_mv3t.on = 1;
        g_mv3t.step = 0;
        g_mv3t.t0 = t;
        g_mv3t.step_ms = 0;
        g_mv3t.jumped = 0;
        Xlog("mv3 animtest: %s (%.2f s) - phase sweep 0 .. 0.9 frozen 0.5 s each, then a speed-1 run with a +0.2 jump half way%s", g_mv3t.name, g_mv3t.dur, g_mv3t.pin ? ", body PINNED at the start spot" : ", body free (shows the engine's root motion)");
    }
    if (g_mv3t.pin) {
        static __attribute__((aligned(16))) PsyncBf bf;
        PsyncBfFromLoc(&bf, g_mv3t.from);
        mo_write(SID_PLAYER, &bf, MO_SRC("mv3.pin"), 0);
    }
    if (g_mv3t.step < 10) {     // frozen phases
        if (t < g_mv3t.step_ms)
            return;
        if (g_mv3t.step > 0)
            Xlog("mv3 animtest: %s phase %.1f -> layer reads %.3f, body moved %.2f m", g_mv3t.name, (g_mv3t.step - 1) * 0.1, PsyncPhase(SID_PLAYER, g_mv3t.sid), Mv3aTestMoved());
        float ph = g_mv3t.step * 0.1f;
        uint64_t r =
            g_mv3t.step == 0 ? Mv3aAnimate(SID_PLAYER, g_mv3t.sid, ph,
                                            MV3A_IN_IDLE) :
            Mv3aSetF("set-animation-phase", SID_PLAYER, g_mv3t.sid, ph);
        Mv3aSetF("set-animation-speed", SID_PLAYER, g_mv3t.sid, 0.0f);
        if (g_mv3t.step == 0)
            Xlog("mv3 animtest: animate_ -> %llx", (unsigned long long) r);
        g_mv3t.step++;
        g_mv3t.step_ms = t + 500.0;
        if (g_mv3t.step == 10) {        // run at speed 1 from 0
            Xlog("mv3 animtest: %s phase 0.9 -> layer reads %.3f, body moved %.2f m - now running at speed 1 from phase 0", g_mv3t.name, PsyncPhase(SID_PLAYER, g_mv3t.sid), Mv3aTestMoved());
            Mv3aSetF("set-animation-phase", SID_PLAYER, g_mv3t.sid,
                       0.0f);
            Mv3aSetF("set-animation-speed", SID_PLAYER, g_mv3t.sid,
                       1.0f);
            g_mv3t.play_ms = t;
            g_mv3t.logs = 0;
        }
        return;
    }
    float el = (float) ((t - g_mv3t.play_ms) / 1000.0), want =
        el / g_mv3t.dur + (g_mv3t.jumped ? 0.2f : 0.0f);
    if (!g_mv3t.jumped && el >= g_mv3t.dur * 0.3f) {
        uint64_t r =
            Mv3aSetF("set-animation-phase", SID_PLAYER, g_mv3t.sid,
                       el / g_mv3t.dur + 0.2f);
        g_mv3t.jumped = 1;
        Xlog("mv3 animtest: %s phase jump +0.2 at %.0f ms on a PLAYING instance (-> %llx) - the next lines must read ~0.2 ahead of the clock", g_mv3t.name, el * 1000.0f, (unsigned long long) r);
    }
    if ((uint32_t) (el * 4.0f) >= g_mv3t.logs) {
        g_mv3t.logs++;
        float ph = PsyncPhase(SID_PLAYER, g_mv3t.sid);
        Xlog("mv3 animtest: %s run t %.0f ms: layer %.3f, clock %.3f (drift %+.1f frames), body moved %.2f m", g_mv3t.name, el * 1000.0f, ph, want, (ph - want) * g_mv3t.dur * 60.0f, Mv3aTestMoved());
    }
    if (want >= 0.97f) {
        Mv3aTestStop("run done");
        g_mv3t.step_ms = t + 2000.0;
    }
}
