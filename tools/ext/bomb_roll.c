// Included from t2r-ext.c after downed_vox.c (uses feel_flag / xlog / t2r_rand / pcm_play_at_range / player_locator / dc_call).
// Thrown bombs make their ground sounds.
// A stun / smoke bomb, nail (pipe) bomb or molotov that lands only had its fuse hiss: the SP impact events (sfx-stunbomb-bounce / sfx-smokebomb-bounce
// in bank wpn-stun-bomb, sfx-pipe-bomb-imp in wpn-pipe-bomb) are fired by the SP physics-contact path, which no MP projectile reaches (ProjectileThrowable
// raises no sound of its own; the only thing the throw scripts start is the fuse loop). So the sound is made from what the ext already sees every frame:
// the bomb's position (OUR throw: ob_tick; every remote thrower's copy: the g_rproj loop - both call br_feed). The velocity between two frames gives
//   impact : the downward speed reverses / stops (floor), or the horizontal velocity changes by > 2.8 m/s (wall, edge, a bounce off a body); strength =
//            the velocity change in m/s -> tier (soft < 1.5 <= mid < 4 <= hard) and gain (0.12 + 0.88 (strength / 9)^0.7)
//   roll   : after the first impact, while it moves on the ground (|vy| < 0.9 m/s, 0.45 - 7 m/s sideways) a grain every 0.3 m of travel (>= 220 ms apart)
// Waves are TLOU2's own (tools/rip_bomb_roll.py: the very waves of those SP events + the weapon-on-surface throwable set, bank srf-weapon-throwables), in the
// PCM mixer at the bomb's point with its own range (full within 3 m, silent at 40 / 26 / 16 m for hard / mid / soft, 10 m for the roll), pitch +-6 %.
// A copy that the trajectory stream steers (bomb_track_recv sets its velocity) is told with br_steer: the two frames after it are not judged.
// Opt-out t2r-no-bomb-roll.txt (re-read every 2 s); t2r-bomb-roll-vol.txt = master gain 0..2 (default 1); t2r-bomb-roll-native.txt = ALSO try the SP event on the
// bomb through play-sound-at-joint and skip our wave when the engine accepted it (A/B probe; the log shows what it returned). Logs 'bomb roll: ...'.
#include "bomb_roll.h"
__asm__
    (".section .rdata,\"dr\"\n.balign 4\n.globl br_blob\nbr_blob:\n.incbin \"bomb_roll.pcm\"\n.text\n");
extern const int16_t br_blob[];

#define BR_N 12
#define BR_MASTER 0.55f
static struct {
    uint64_t h, seen_ms, imp_ms, roll_ms;
    int kind, slot, armed, skip, nimp;
    float last[3], v[3], roll_d;
    long long q;
} g_br[BR_N];
static int g_brLast[12];

static int BrOff(void)
{
    static int off = -1;
    static uint64_t at;
    return FeelFlag("t2r-no-bomb-roll.txt", &at, &off, 120);
}

static int BrNative(void)
{
    static int on = -1;
    static uint64_t at;
    if (on < 0)
        on = 0;
    return FeelFlag("t2r-bomb-roll-native.txt", &at, &on, 120);
}

static float BrVol(void)
{
    static float v = 1.0f;
    static uint64_t at;
    uint64_t now = GetTickCount64();
    if (now - at > 2000) {
        at = now;
        v = 1.0f;
        FILE *f = ExeSideOpen("t2r-bomb-roll-vol.txt", "r");
        if (f) {
            float x;
            if (fscanf(f, "%f", &x) == 1)
                v = x < 0 ? 0 : (x > 2 ? 2 : x);
            fclose(f);
        }
    }
    return v;
}

// one wave of a group (never the same twice in a row), pitch +-6 %, at the point
static int BrPlay(const br_wave *w, int nw, int gi, float gain,
                   const float *pos, float rfar)
{
    float rnear = rfar <= 10.0f ? 1.0f : 2.0f;  // a small can: full only within 1-2 m, then falls off to silence
    int k = (int) (T2rRand() % (uint32_t) nw);
    if (nw > 1 && k == g_brLast[gi])
        k = (k + 1) % nw;
    g_brLast[gi] = k;
    int rate =
        (int) ((float) BR_RATE *
               (0.94f + 0.12f * (float) (T2rRand() & 1023) / 1023.0f));
    float lb[16];
    const float *pl = PlayerLocator(lb);
    if (pl) {
        float dx = pos[0] - pl[0], dy = pos[1] - pl[1], dz =
            pos[2] - pl[2];
        if (dx * dx + dy * dy + dz * dz > (rfar + 3.0f) * (rfar + 3.0f))
            return 2;
    }                           // 2 = out of earshot (not an error)
    return PcmPlayAtRange(br_blob + w[k].off, w[k].n,
                             gain * BR_MASTER * BrVol(), rate, pos, rnear,
                             rfar);
}

static float BrGain(float str)
{
    float x = str / 9.0f;
    x = x > 1.0f ? 1.0f : x;
    return 0.12f + 0.88f * __builtin_powf(x, 0.7f);
}

static const char *BrName(int kind)
{
    return kind == 1 ? "nail bomb" : kind ==
        3 ? "molotov" : "stun / smoke bomb";
}

static void BrImpact(int i, float str, const float *pos)
{
    static int nl;
    int kind = g_br[i].kind;
    float g = BrGain(str) * 0.7f;
    const char *tier = str >= 4.0f ? "hard" : str >= 1.5f ? "mid" : "soft";
    int hard = str >= 4.0f, mid = !hard && str >= 1.5f, ok = 0;
    if (BrNative()) {          // probe: the SP event on the bomb itself
        const char *ev =
            kind == 2 ? "sfx-stunbomb-bounce" : kind ==
            1 ? "sfx-pipe-bomb-imp" : NULL;
        if (ev) {
            uint64_t a[5] = { FNV64(ev), g_br[i].h, 0, 0, 0 };
            uint64_t r = DcCall(FNV64("play-sound-at-joint"), 5, a);
            if (LOGCAP(nl, 60))
                Xlog("bomb roll: native %s on %llx -> %llx", ev,
                     (unsigned long long) g_br[i].h,
                     (unsigned long long) r);
            if (r != ~0ULL)
                return;
        }
    }
    if (kind == 2) {
        if (hard)
            ok = BrPlay(SB_HARD_W, SB_HARD_N, 0, g * 1.3f, pos, 24.0f);
        else {
            ok = BrPlay(SB_SOFT_W, SB_SOFT_N, 1, g * (mid ? 1.5f : 1.1f),
                         pos, mid ? 16.0f : 10.0f);
            if (mid)
                BrPlay(SB_HARD_W, SB_HARD_N, 0, g * 0.35f, pos, 16.0f);
        }
    } else if (kind == 1) {
        if (hard) {
            ok = BrPlay(NB_HARD_W, NB_HARD_N, 2, g, pos, 24.0f);
            BrPlay(NB_RATTLE_W, NB_RATTLE_N, 5, g * 0.45f, pos, 16.0f);
        } else if (mid) {
            ok = BrPlay(NB_MID_W, NB_MID_N, 3, g * 1.1f, pos, 16.0f);
            BrPlay(NB_RATTLE_W, NB_RATTLE_N, 5, g * 0.3f, pos, 11.0f);
        } else {
            ok = BrPlay(NB_SOFT_W, NB_SOFT_N, 4, g * 1.3f, pos, 16.0f);
            BrPlay(NB_RATTLE_W, NB_RATTLE_N, 5, g * 0.2f, pos, 8.0f);
        }
    } else {
        ok = BrPlay(MO_HIT_W, MO_HIT_N, 6, g * 1.2f, pos,
                     hard ? 24.0f : mid ? 16.0f : 10.0f);
    }
    if (LOGCAP(nl, 60))
        Xlog("bomb roll: %s %s impact %.1f m/s (%s, gain %.2f) at %.2f %.2f %.2f -> %s", g_br[i].slot < 0 ? "own" : "a remote", BrName(kind), str, tier, g, pos[0], pos[1], pos[2], ok == 2 ? "out of earshot" : ok ? "played" : "not played");
}

static void BrRoll(int i, float hs, const float *pos)
{
    static int nl;
    int kind = g_br[i].kind;
    float g = 0.05f + 0.25f * (hs > 4.0f ? 1.0f : hs / 4.0f);   // a small can: quiet, scaled by speed
    int ok =
        kind == 2 ? BrPlay(SB_ROLL_W, SB_ROLL_N, 7, g * 0.9f, pos,
                            8.0f) : kind == 1 ? BrPlay(NB_ROLL_W,
                                                        NB_ROLL_N, 8,
                                                        g * 0.9f, pos,
                                                        8.0f) :
        BrPlay(MO_ROLL_W, MO_ROLL_N, 9, g * 0.9f, pos, 8.0f);
    if (LOGCAP(nl, 30))
        Xlog("bomb roll: %s %s rolls at %.1f m/s (gain %.2f) -> %s",
             g_br[i].slot < 0 ? "own" : "a remote", BrName(kind), hs, g,
             ok == 2 ? "out of earshot" : ok ? "played" : "not played");
}

// the trajectory stream put a velocity on this copy: the next two frames' velocity change is ours, not a contact
static void BrSteer(uint64_t h)
{
    for (int i = 0; i < BR_N; i++)
        if (g_br[i].h == h) {
            g_br[i].skip = 2;
            return;
        }
}

// one frame of one bomb: kind 1 nail (pipe) bomb, 2 stun / smoke, 3 molotov; slot < 0 = ours
static void BrFeed(uint64_t h, int kind, int slot, const float *pos)
{
    if (!h || !kind || !pos || BrOff())
        return;
    if (!__builtin_isfinite(pos[0] + pos[1] + pos[2]))
        return;
    uint64_t now = GetTickCount64();
    LARGE_INTEGER q, f;
    QueryPerformanceCounter(&q);
    QueryPerformanceFrequency(&f);
    int i = -1, free_ = -1, old = 0;
    for (int j = 0; j < BR_N; j++) {
        if (g_br[j].h == h) {
            i = j;
            break;
        }
        if (!g_br[j].h || now - g_br[j].seen_ms > 2500) {
            if (free_ < 0)
                free_ = j;
        }
        if (g_br[j].seen_ms < g_br[old].seen_ms)
            old = j;
    }
    if (i < 0) {                // first frame of a bomb: only remembered
        i = free_ >= 0 ? free_ : old;
        memset(&g_br[i], 0, sizeof g_br[i]);
        g_br[i].h = h;
        g_br[i].kind = kind;
        g_br[i].slot = slot;
        g_br[i].seen_ms = now;
        memcpy(g_br[i].last, pos, 12);
        g_br[i].q = q.QuadPart;
        return;
    }
    double dt =
        f.QuadPart ? (double) (q.QuadPart -
                               g_br[i].q) / (double) f.QuadPart : 0.0;
    float d[3] =
        { pos[0] - g_br[i].last[0], pos[1] - g_br[i].last[1],
   pos[2] - g_br[i].last[2] };
    g_br[i].seen_ms = now;
    g_br[i].q = q.QuadPart;
    memcpy(g_br[i].last, pos, 12);
    if (!(dt > 0.003 && dt < 0.2)
        || d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > 16.0f) {
        g_br[i].skip = 2;
        memset(g_br[i].v, 0, 12);
        return;
    }                           // a stall or a jump (re-placed): no velocity
    float v[3] =
        { (float) (d[0] / dt), (float) (d[1] / dt), (float) (d[2] / dt) },
        *vp = g_br[i].v;
    if (g_br[i].skip > 0) {
        g_br[i].skip--;
        memcpy(vp, v, 12);
        return;
    }
    float hs = __builtin_sqrtf(v[0] * v[0] + v[2] * v[2]);
    float dvy = v[1] - vp[1], dvx = v[0] - vp[0], dvz = v[2] - vp[2];
    float hp = __builtin_sqrtf(vp[0] * vp[0] + vp[2] * vp[2]), dvh =
        __builtin_sqrtf(dvx * dvx + dvz * dvz);
    float str = 0.0f;
    if (vp[1] < -1.0f && dvy > 1.0f)
        str = dvy;              // the fall reversed or stopped: the floor
    if (dvh > 2.8f && hp > 2.5f && dvh * dvh > 0.16f * hp * hp) {
        float s2 = dvh * 0.9f;
        if (s2 > str)
            str = s2;
    }                           // sideways: a wall, an edge, a body
    if (str >= 0.9f && now - g_br[i].imp_ms >= 80) {
        g_br[i].imp_ms = now;
        g_br[i].armed = 1;
        g_br[i].nimp++;
        g_br[i].roll_d = 0.0f;
        BrImpact(i, str, pos);
    } else if (g_br[i].armed && hs >= 0.45f && hs <= 7.0f
               && __builtin_fabsf(v[1]) < 0.9f) {
        g_br[i].roll_d += hs * (float) dt;
        if (g_br[i].roll_d >= 0.30f && now - g_br[i].roll_ms >= 220
            && now - g_br[i].imp_ms >= 120) {
            g_br[i].roll_d = 0.0f;
            g_br[i].roll_ms = now;
            BrRoll(i, hs, pos);
        }
    }
    memcpy(vp, v, 12);
}
