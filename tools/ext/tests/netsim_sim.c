// Offline netsmooth simulator (re/netcode.md section 10): compiles the REAL tools/ext/netsmooth.c against stubs (virtual clock, fake peer
// record, winmm's 3-deep list-time ring) and feeds it synthetic packet traces: sender fps 60/90/144 -> host relay (one msg 5 per host frame,
// msg 7 forwarded at the host frame) -> receiver at 60/144 fps, rtt / jitter / hold-and-burst spikes / Gilbert-Elliott loss, a host migration
// (new sender clock domain, new seq counters) and a respawn teleport. Metrics per rendered frame: jerk (the live 'stutter' definition, |dv| > 3 m/s),
// stalls, velocity wobble vs the truth, visual latency, extrapolation, delay, winmm time ring stuck.
// Build (from tools/ext/tests):  gcc -std=gnu2x -O2 -DT2R_RELEASE -o netsim netsim_sim.c        [-DNS_SRC='"other/netsmooth.c"' to compare versions]
// Run: ./netsim (matrix) | ./netsim ring | ./netsim mig <new sender clock ms>      SIMDBG=1 prints the slot state every 2 s
#ifndef NS_SRC
#define NS_SRC "../netsmooth.c"
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define T2R_MAX_PEERS 8
static int g_enginePeers = 8;
#define T2R_ENGINE_PEERS g_enginePeers
static double g_simMs;
#define QueryPerformanceCounter SimQpc
#define QueryPerformanceFrequency SimQpf
static BOOL SimQpc(LARGE_INTEGER *c)
{
    c->QuadPart = (long long) (g_simMs * 1e6);
    return 1;
}

static BOOL SimQpf(LARGE_INTEGER *c)
{
    c->QuadPart = 1000000000LL;
    return 1;
}

static void Xlog(const char *f, ...)
{
    (void) f;
}

static uint8_t *g_s8Pa;
static uint8_t g_dummy[1 << 20];
static uint32_t g_dummy32;
static uint8_t g_peerbuf[8][0x800];
static uint8_t g_builderOut[64];
static float g_alphaVal;
static uint8_t g_puppetDeadHere[8], g_peerDowned[8];
static uint64_t g_frame;
static int g_oneway = 50;
static uint8_t *T2rPeer(uint32_t slot)
{
    return slot < 8 ? g_peerbuf[slot] : NULL;
}

static int PsyncOneway(int s)
{
    (void) s;
    return g_oneway;
}

static uint8_t MatchSubstate(void)
{
    return 6;
}

static void *AllocNear(uint8_t *b, size_t n)
{
    (void) b;
    (void) n;
    return NULL;
}

static FILE *ExeSideOpen(const char *n, const char *m)
{
    (void) n;
    (void) m;
    return NULL;
}

static float StubAlpha(uint8_t *p)
{
    (void) p;
    return g_alphaVal;
}

static void *StubBuilder(float *p, float *q)
{
    memcpy(g_builderOut, p, 12);
    memcpy(g_builderOut + 12, q, 16);
    return g_builderOut;
}

static uintptr_t SimVma(uint64_t a)
{
    if (a == 0x18004C450ULL)
        return (uintptr_t) StubAlpha;
    if (a == 0x18000A370ULL)
        return (uintptr_t) StubBuilder;
    if (a == 0x18013B1D0ULL)
        return (uintptr_t) & g_dummy32;
    return (uintptr_t) g_dummy;
}

#define VMA(x) SimVma(x)
#define PCB_BEGIN(i, mask) do {} while (0)      // perf30b path buckets (t2r-ext.c): not in the simulator
#define PCB_END(i) do {} while (0)
#include "../txstat.h"
#include "../hostbw.h"
#include NS_SRC

// ---------------------------------------------------------------- scenario
typedef struct {
    const char *name;
    double sfps, hfps, hfps_jit;        // sender fps, host relay fps, host frame time jitter (fraction)
    double rtt, jit, spike_ms, spike_per_s;     // rtt (ms), jitter scale (ms), spike hold ms, spikes per second
    double loss, burst;         // percent, mean burst
    double rfps;                // receiver render fps
    double dur;                 // seconds
    double t_mig, t_resp;       // host migration / respawn time (0 = none)
    int quant;                  // sender clock quantised to 15.625 ms
    int flips;                  // vec4 sign flips
    double mig_new_up;          // sender raw time offset after migration (ms)
    double m7_iv, m7_iv_still;  // host-relay msg 7 pacing to this receiver: min ms between forwarded lists (0 = every list); _still = while the sender stands still
    int m7_mode;                // 0 = time pacing as above, 1 = delta (still senders: one list per m7_iv_still, moving: all)
} ScnT;

static uint32_t g_rng = 12345;
static double Rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return (g_rng >> 8) / 16777216.0;
}

#define TMAX 200000
static float TX[TMAX], TZ[TMAX], TY[TMAX], TS[TMAX];    // truth per ms
static void GenTruth(double dur, double t_resp)
{
    double x = 0, z = 0, h = 0, sp = 0;
    int n = (int) (dur * 1000.0) + 1000;
    double phase_t = 0;
    int ph = 0;
    double target = 0;
    for (int i = 0; i < n && i < TMAX; i++) {
        if (i >= phase_t) {
            ph = (ph + 1) % 6;
            static const double tg[6] = { 0, 2.0, 5.5, 5.5, 1.2, 0 };
            static const double dd[6] =
                { 1500, 4000, 5000, 3000, 2500, 1500 };
            target = tg[ph];
            phase_t = i + dd[ph];
        }
        sp += (target - sp) * (1.0 - exp(-1.0 / 150.0));
        h += 1.1 * sin(i * 0.0007) * 0.001 * (sp > 0.3);
        if (t_resp > 0 && i == (int) (t_resp * 1000.0)) {
            x += 40;
            z -= 25;
            sp = 0;
            ph = 5;
            phase_t = i + 2000;
            target = 0;
        }
        x += sp * sin(h) * 0.001;
        z += sp * cos(h) * 0.001;
        TX[i] = (float) x;
        TZ[i] = (float) z;
        TY[i] = 0.0f;
        TS[i] = (float) sp;
        (void) TY;
    }
}

static void TruthAt(double t, float *p, float *yaw, float *sp)
{
    int i = (int) t;
    if (i < 0)
        i = 0;
    if (i >= TMAX - 2)
        i = TMAX - 2;
    double f = t - i;
    if (f < 0)
        f = 0;
    p[0] = (float) (TX[i] + (TX[i + 1] - TX[i]) * f);
    p[1] = 0;
    p[2] = (float) (TZ[i] + (TZ[i + 1] - TZ[i]) * f);
    *sp = TS[i];
    float dx = TX[i + 1] - TX[i], dz = TZ[i + 1] - TZ[i];
    *yaw = (dx * dx + dz * dz > 1e-14f) ? atan2f(dx, dz) : 0.0f;
}

static void YawQuat(float yaw, float *q)
{
    q[0] = 0;
    q[1] = sinf(yaw / 2);
    q[2] = 0;
    q[3] = cosf(yaw / 2);
}

typedef struct {
    double at;
    int type;
    uint32_t seq;
    uint32_t raw_t;
    double ts;
    float p[3], q[4], v[20];
    int tele;
} EvT;                         // type 5 / 7
static EvT *ev;
static int nev, capev;
static void AddEv(EvT e)
{
    if (nev >= capev) {
        capev = capev ? capev * 2 : 4096;
        ev = realloc(ev, sizeof(EvT) * capev);
    }
    ev[nev++] = e;
}

static int CmpEv(const void *a, const void *b)
{
    double d = ((const EvT *) a)->at - ((const EvT *) b)->at;
    return d < 0 ? -1 : d > 0;
}

typedef struct {
    double bad_p_stay, bad_p_enter;
    int bad;
} GeT;
static int GeLose(GeT *g)
{
    g->bad = Rnd() < (g->bad ? g->bad_p_stay : g->bad_p_enter);
    return g->bad;
}

static void GeInit(GeT *g, double loss, double burst)
{
    double L = loss / 100.0, B = burst < 1.0 ? 1.0 : burst;
    g->bad = 0;
    g->bad_p_stay = 1.0 - 1.0 / B;
    g->bad_p_enter =
        L >= 0.99 ? 1.0 : (L > 0 ? (1.0 / B) * L / (1.0 - L) : 0.0);
}

// one-way jitter: base + heavy-ish tail
static double JitDraw(double j)
{
    double u = Rnd();
    double e = -log(1.0 - u * 0.999) * 0.4;
    if (e > 4.0)
        e = 4.0;
    return j * e;
}

typedef struct {
    double hold_from, hold_to;
} SpikeT;
static SpikeT g_spikes[4096];
static int g_nspikes;
static double hold_until(double t)
{
    for (int i = 0; i < g_nspikes; i++)
        if (t >= g_spikes[i].hold_from && t < g_spikes[i].hold_to)
            return g_spikes[i].hold_to;
    return 0;
}

static double fwd7_sent, fwd7_skipped;
static void BuildEvents(const ScnT *c)
{
    nev = 0;
    double T = c->dur * 1000.0, d1 = c->rtt / 4.0, d2 = c->rtt / 4.0;   // sender->host, host->client halves of the one-way... (rtt/2 one way total)
    g_nspikes = 0;
    for (double t = 0; t < T; t += 100.0)
        if (Rnd() < c->spike_per_s * 0.1 && g_nspikes < 4000) {
            g_spikes[g_nspikes].hold_from = t;
            g_spikes[g_nspikes].hold_to = t + c->spike_ms * (0.5 + Rnd());
            g_nspikes++;
        }
    GeT g1, g2, g3;
    GeInit(&g1, c->loss, c->burst);
    GeInit(&g2, c->loss, c->burst);
    GeInit(&g3, c->loss, c->burst);
    // sender -> host arrival list (msg 5 / msg 7), in sender frames
    typedef struct {
        double tarr;
        int is7;
        uint32_t seq;
        uint32_t raw;
        double ts;
        float p[3], q[4], v[20];
        int tele;
    } HpT;
    int cap = (int) (T / 1000.0 * c->sfps * 2) + 100;
    HpT *hq = calloc(cap, sizeof(HpT));
    int nh = 0;
    double up0 = 1.2e8 + 12345.0, up1 = c->mig_new_up;
    uint32_t seq = 100;
    double ts = 0;
    int migrated = 0;
    double blackout_end = 0;
    float prev_p[3] = { 0 };
    while (ts < T) {
        double dt = 1000.0 / c->sfps * (1.0 + 0.04 * (Rnd() - 0.5) * 2.0);
        ts += dt;
        if (c->t_mig > 0 && !migrated && ts >= c->t_mig * 1000.0) {
            migrated = 1;
            seq = 7;
            blackout_end = ts + 700.0;
        }
        int tele = 0;
        float p[3], yaw, sp;
        TruthAt(ts, p, &yaw, &sp);
        (void) tele;
        double raw_d = migrated ? up1 + ts : up0 + ts;
        if (c->quant)
            raw_d = floor(raw_d / 15.625) * 15.625;
        uint32_t raw = (uint32_t) raw_d;
        seq++;
        float q[4];
        YawQuat(yaw, q);
        float v[20];
        for (int j = 0; j < 5; j++) {
            float yy =
                j ==
                0 ? 0.8f * sinf((float) ts * 0.0015f) : 0.05f * (j +
                                                                 1) *
                sinf((float) ts * 0.003f + j);
            float qq[4];
            YawQuat(yy, qq);
            memcpy(v + 4 * j, qq, 16);
            if (c->flips && (((int) (ts / 400.0) + j) & 1) && j == 0)
                for (int k = 0; k < 4; k++)
                    v[k] = -v[k];
        }
        if (ts < blackout_end)
            continue;
        for (int ty = 0; ty < 2; ty++) {
            if (GeLose(ty ? &g2 : &g1))
                continue;
            HpT *h = &hq[nh++];
            double d = d1 + JitDraw(c->jit * 0.5);
            double ta = ts + d, hu = hold_until(ts);
            if (hu > ta)
                ta = hu + d1;
            h->tarr = ta;
            h->is7 = ty;
            h->seq = seq;
            h->raw = raw;
            h->ts = ts;
            memcpy(h->p, p, 12);
            memcpy(h->q, q, 16);
            memcpy(h->v, v, 80);
            if (nh >= cap)
                break;
        }
        (void) prev_p;
    }
    // host: ticks; forwards msg 7 as it arrives (at its next frame), msg 5 only the newest not yet relayed
    // (arrival order for the host: sort by tarr)
    for (int i = 0; i < nh; i++)
        for (int j = i + 1; j < nh && j < i + 40; j++)
            if (hq[j].tarr < hq[i].tarr) {
                HpT t = hq[i];
                hq[i] = hq[j];
                hq[j] = t;
            }
    double fwd7_last = -1e9;
    fwd7_sent = fwd7_skipped = 0;
    double ht = 0;
    int pos = 0;
    uint32_t last_relayed = 0;
    int have_last = 0;
    HpT best5;
    int have5 = 0;
    double hper = 1000.0 / c->hfps;
    while (ht < T + 1000.0) {
        ht += hper * (1.0 + c->hfps_jit * (Rnd() - 0.5) * 2.0);
        if (c->t_mig > 0 && ht > c->t_mig * 1000.0 && hper == 1000.0 / c->hfps && c->hfps_jit >= 0) {   /* new host after migration: another frame rate */
            hper = 1000.0 / (c->hfps * 0.8);
            have_last = 0;
        }
        while (pos < nh && hq[pos].tarr <= ht) {
            HpT *h = &hq[pos++];
            if (h->is7) {
                if (c->m7_iv > 0 || c->m7_iv_still > 0) {
                    int still = TS[(int) (h->ts < 0 ? 0 : h->ts)] < 0.1f;
                    double iv = still
                        && c->m7_iv_still > 0 ? c->m7_iv_still : c->m7_iv;
                    if (c->m7_mode == 1 && !still)
                        iv = 0;
                    if (iv > 0 && ht - fwd7_last < iv - 1.5) {
                        fwd7_skipped++;
                        continue;
                    }
                    fwd7_last = ht;
                }
                fwd7_sent++;
                if (GeLose(&g3))
                    continue;
                double d = d2 + JitDraw(c->jit * 0.5), ta = ht + d, hu =
                    hold_until(ht);
                if (hu > ta)
                    ta = hu + d2;
                EvT e = { 0 };
                e.at = ta;
                e.type = 7;
                e.seq = h->seq;
                e.raw_t = h->raw;
                e.ts = h->ts;
                AddEv(e);
            } else if (!have5 || (int32_t) (h->seq - best5.seq) > 0
                       || (c->t_mig > 0 && h->seq < 1000
                           && best5.seq > 1000)) {
                best5 = *h;
                have5 = 1;
            }
        }
        if (have5 && (!have_last || best5.seq != last_relayed)) {
            have_last = 1;
            last_relayed = best5.seq;
            if (!GeLose(&g3)) {
                double d = d2 + JitDraw(c->jit * 0.5), ta = ht + d, hu =
                    hold_until(ht);
                if (hu > ta)
                    ta = hu + d2;
                EvT e = { 0 };
                e.at = ta;
                e.type = 5;
                e.seq = best5.seq;
                e.ts = best5.ts;
                memcpy(e.p, best5.p, 12);
                memcpy(e.q, best5.q, 16);
                memcpy(e.v, best5.v, 80);
                AddEv(e);
            }
        }
    }
    free(hq);
    qsort(ev, nev, sizeof(EvT), CmpEv);
}

// ---- winmm's time ring (receiver 0x1800293a0) and what we measure about it
static void WinmmRingPush(uint8_t *b, uint32_t t)
{
    int32_t idx = *(int32_t *) (b + 0x2838), cnt =
        *(int32_t *) (b + 0x283c);
    int32_t d = (int32_t) t - *(int32_t *) (b + 0x2820 + 8 * idx);
    if (cnt > 0 && d <= 0)
        return;
    int ni = cnt > 0 ? (idx + 1) % 3 : 0;
    *(int32_t *) (b + 0x2820 + 8 * ni) = (int32_t) t;
    *(int32_t *) (b + 0x2838) = ni;
    if (cnt < 3)
        *(int32_t *) (b + 0x283c) = cnt + 1;
}

typedef struct {
    double n, jerk, stall, wob_sum, wob_n, lat_sum, lat_n, lat_min,
        lat_max, step_max;
    double stutter_w;
} MetT;
static double pct(double *v, int n, double p)
{
    if (!n)
        return 0;
    int k = (int) (p * (n - 1));
    return v[k];
}

static int Cmpd(const void *a, const void *b)
{
    double d = *(const double *) a - *(const double *) b;
    return d < 0 ? -1 : d > 0;
}

static double g_vstep_max, g_vstep_p99, g_vsteps_big;
typedef struct {
    double jerk_pct, stall_pct, wob, lat_mean, lat_p5, lat_p95, extrap_pct,
        delay, ring_stuck_pct, step_cv, snaps, ck_still_pct, big_step, jm,
        jpk;
} ResT;

static ResT run(const ScnT *c, int use_ns, int log_lines)
{
    g_rng = 777;
    g_nsGapTol = getenv("SIMNOTOL") ? 0 : 1;
    GenTruth(c->dur + 2, c->t_resp);
    BuildEvents(c);
    memset(g_dummy, 0, sizeof g_dummy);
    free(g_s8Pa);
    g_s8Pa = calloc(8, 0x77e8);
    memset(g_peerbuf, 0, sizeof g_peerbuf);
    memset(g_ns, 0, sizeof g_ns);
    memset(g_nsDq, 0, sizeof g_nsDq);
    memset((void *) g_nsAnim, 0, sizeof g_nsAnim);
    for (int i = 0; i < 8; i++)
        NsReset(&g_ns[i]);
    g_ns_inst = use_ns;
    g_nsOff = !use_ns;
    g_ns_rebase = 1;
    g_oneway = (int) (c->rtt / 2);
    g_nsDqOff = 0;
    const uint32_t slot = 2;
    uint8_t *peer = g_peerbuf[slot];
    *(uint64_t *) (peer + 0x60) = 0x1000 + slot;
    uint8_t *pa = g_s8Pa + (size_t) slot * 0x77e8;
    double T = c->dur * 1000.0, fdt = 1000.0 / c->rfps;
    double now = 0;
    int ei = 0;
    uint32_t last_seq5 = 0;
    int have5 = 0, mig_done = 0;
    double last_apply = 0, ewma = 16.7;
    int apply_n = 0;
    float vprev[4] = { 0 };
    int vprev_ok = 0;
    double *vst = malloc(sizeof(double) * (size_t) (T / fdt * 1.5 + 100));
    int nvst = 0;
    g_vsteps_big = 0;
    float prev_out[3] = { 0 };
    double prev_t = 0;
    int have_prev = 0;
    float prev_v = 0;
    double *lat = malloc(sizeof(double) * (size_t) (T / fdt * 1.5 + 100));
    int nlat = 0;
    double *steps =
        malloc(sizeof(double) * (size_t) (T / fdt * 1.5 + 100));
    int nst = 0;
    double jerk = 0, stall = 0, wsum = 0, wn = 0, mov = 0, ring_stuck =
        0, frames = 0, big = 0, snaps_pre = 0, ck_still = 0, ck_n = 0;
    uint32_t t_in_last = 0;
    int have_tin = 0;
    double next_dbg = 0;
    int dbg = getenv("SIMDBG") != NULL;
    float ck = 0;
    int ck_ok = 0;
    while (now < T) {
        now +=
            fdt * (1.0 + 0.08 * (Rnd() - 0.5) * 2.0) * (Rnd() <
                                                        0.004 ? 3.0 : 1.0);
        g_simMs = now;
        g_frame++;
        while (ei < nev && ev[ei].at <= now) {
            EvT *e = &ev[ei++];
            if (e->at < 0)
                continue;
            if (e->type == 7) {
                uint32_t t = e->raw_t;
                uint32_t seq = e->seq;
                if (use_ns) {
                    uint32_t t_raw = t;
                    t = NsRebase(slot, t, pa);
                    uint32_t t_rb = t;
                    t = NsDequant(slot, seq, t);
                    NsNoteT(slot, t_raw, t_rb, t);
                    NsOnAnim(slot, seq, (double) t);
                }
                WinmmRingPush(pa, t);
                t_in_last = t;
                have_tin = 1;
            } else {
                if (c->t_mig > 0 && !mig_done
                    && e->ts >= c->t_mig * 1000.0) {
                    mig_done = 1;
                    have5 = 0;
                    apply_n = 0;
                }               // the peer record is rebuilt by the new host handshake (seq counters restart); the msg 7 time ring is NOT cleared (worst case)
                if (have5 && (int32_t) (e->seq - last_seq5) <= 0)
                    continue;   // winmm rejects older seqs
                have5 = 1;
                last_seq5 = e->seq;
                memcpy(peer + 0x517, peer + 0x68, 12);
                memcpy(peer + 0x68, e->p, 12);
                memcpy(peer + 0x74, e->q, 16);
                memcpy(peer + 0xc5, e->v, 80);
                if (apply_n) {
                    double iv = now - last_apply;
                    if (iv < 8)
                        iv = 8;
                    if (iv > 200)
                        iv = 200;
                    ewma += (iv - ewma) * 0.2;
                } else
                    memcpy(peer + 0x517, e->p, 12);
                last_apply = now;
                apply_n++;
                if (use_ns)
                    NsOnState(slot, e->seq,
                                (const float *) (peer + 0x68),
                                (const float *) (peer + 0x74),
                                peer + 0xc5);
            }
        }
        if (dbg && now >= next_dbg) {
            next_dbg = now + 2000;
            NsSlot *s = &g_ns[slot];
            printf
                ("  t=%6.0f synced %d n %u R_ok %d ours %d delay %.0f jpk %.0f a_t %.0f base %u ringcnt %d ringnew %d t_in %u extrap %u frames %u\n",
                 now, s->synced, s->n, s->R_ok, s->ours, s->delay, s->jpk,
                 s->a_t, s->base, *(int32_t *) (pa + 0x283c),
                 *(int32_t *) (pa + 0x2820 +
                               8 ** (int32_t *) (pa + 0x2838)), t_in_last,
                 s->st_extrap, s->st_frames);
        }
        if (!apply_n)
            continue;
        g_alphaVal = (float) ((now - last_apply) / ewma);
        // winmm's placement
        float a = g_alphaVal > 0.7f ? 0.7f : g_alphaVal;
        const float *cur = (const float *) (peer + 0x68), *prv =
            (const float *) (peer + 0x517);
        float out[3] =
            { cur[0] + a * (cur[0] - prv[0]), cur[1],
     cur[2] + a * (cur[2] - prv[2]) };
        if (use_ns) {
            void *r = NsLocator(0x1000 + slot);
            if (r) {
                memcpy(out, r, 12);
            }
            NsSlot *s = &g_ns[slot];
            // anim clock model: chases the target by half the gap per frame
            if (g_nsAnim[slot].valid) {
                if (!ck_ok) {
                    ck = g_nsAnim[slot].tgt;
                    ck_ok = 1;
                }
                float d = g_nsAnim[slot].tgt - ck;
                if (d > 500 || d < -500)
                    ck = g_nsAnim[slot].tgt;
                else {
                    if (d > 0)
                        ck += d * 0.5f;
                }
                ck_n++;
                if (d <= 0.01f)
                    ck_still++;
            }
            (void) s;
        }
        {
            float iv[20];
            const float *vv = (const float *) (peer + 0xc5);
            if (use_ns && NsVecInterp(slot, iv))
                vv = iv;
            if (now > 3000.0 && vprev_ok) {
                float d =
                    fabsf(vprev[0] * vv[0] + vprev[1] * vv[1] +
                          vprev[2] * vv[2] + vprev[3] * vv[3]);
                if (d > 1)
                    d = 1;
                double ang = 2.0 * acos(d);
                vst[nvst++] = ang;
                if (ang > 0.012)
                    g_vsteps_big++;
            }
            memcpy(vprev, vv, 16);
            vprev_ok = 1;
        }
        if (now < 3000.0) {
            memcpy(prev_out, out, 12);
            prev_t = now;
            have_prev = 1;
            continue;
        }
        frames++;
        if (have_tin) {
            int32_t rn =
                *(int32_t *) (pa + 0x2838) >=
                0 ? *(int32_t *) (pa + 0x2820 +
                                  8 * *(int32_t *) (pa + 0x2838)) : 0; {
                int32_t dd = (int32_t) (t_in_last - (uint32_t) rn);
                if (dd > 2000 || dd < -2000)
                    ring_stuck++;
            }
        }
        if (have_prev) {
            double dt = now - prev_t;
            double dx = out[0] - prev_out[0], dz =
                out[2] - prev_out[2], st = sqrt(dx * dx + dz * dz);
            float v = (float) (st / (dt * 0.001));
            // truth lag: find the truth time whose position is nearest to the output (+-600 ms)
            double best = 1e9, bl = 0;
            int ti0 = (int) (now - 600.0), ti1 = (int) now;
            for (int ti = ti0; ti <= ti1; ti++) {
                if (ti < 0)
                    continue;
                float ddx = TX[ti] - out[0], ddz = TZ[ti] - out[2];
                double dd = ddx * ddx + ddz * ddz;
                if (dd < best) {
                    best = dd;
                    bl = now - ti;
                }
            }
            float tp[3], yy, sp;
            TruthAt(now - bl, tp, &yy, &sp);
            if (sqrt(best) < 1.0 && sp > 1.0f) {
                lat[nlat++] = bl;
            }
            if (st > 1.5)
                big++;
            int moving = v > 1.0f || prev_v > 1.0f;
            if (moving && dt > 1 && dt < 100) {
                mov++;
                steps[nst++] = st;
                if (fabsf(v - prev_v) > 3.0f && v < 40 && prev_v < 40)
                    jerk++;
                if (v < 0.25f && sp > 2.0f)
                    stall++;
                float tvx = 0; {
                    int i0 = (int) (now - bl - dt), i1 = (int) (now - bl);
                    if (i0 < 0)
                        i0 = 0;
                    if (i1 <= i0)
                        i1 = i0 + 1;
                    float ex = TX[i1] - TX[i0], ez = TZ[i1] - TZ[i0];
                    tvx =
                        sqrtf(ex * ex +
                              ez * ez) / (float) ((i1 - i0) * 0.001);
                }
                wsum += fabs(v - tvx);
                wn++;
            }
            prev_v = v;
        }
        memcpy(prev_out, out, 12);
        prev_t = now;
        have_prev = 1;
    }
    ResT r = { 0 };
    qsort(vst, nvst, sizeof(double), Cmpd);
    g_vstep_p99 = nvst ? vst[(int) (0.99 * (nvst - 1))] : 0;
    g_vstep_max = nvst ? vst[nvst - 1] : 0;
    free(vst);
    r.jerk_pct = mov ? 100.0 * jerk / mov : 0;
    r.stall_pct = mov ? 100.0 * stall / mov : 0;
    r.wob = wn ? wsum / wn : 0;
    qsort(lat, nlat, sizeof(double), Cmpd);
    double ls = 0;
    for (int i = 0; i < nlat; i++)
        ls += lat[i];
    r.lat_mean = nlat ? ls / nlat : 0;
    r.lat_p5 = pct(lat, nlat, 0.05);
    r.lat_p95 = pct(lat, nlat, 0.95);
    NsSlot *s = &g_ns[slot];
    r.extrap_pct = s->st_frames ? 100.0 * s->st_extrap / s->st_frames : 0;
    r.delay = s->st_frames ? s->st_dsum / s->st_frames : 0;
    r.ring_stuck_pct = frames ? 100.0 * ring_stuck / frames : 0;
    r.snaps = s->st_snap;
    r.big_step = big;
    double sm = 0, ss = 0;
    for (int i = 0; i < nst; i++) {
        sm += steps[i];
    } sm = nst ? sm / nst : 0;
    for (int i = 0; i < nst; i++)
        ss += (steps[i] - sm) * (steps[i] - sm);
    r.step_cv = nst > 1 && sm > 0 ? sqrt(ss / nst) / sm : 0;
    r.jm = s->jmean;
    r.jpk = s->jpk;
    r.ck_still_pct = ck_n ? 100.0 * ck_still / ck_n : 0;
    (void) log_lines;
    (void) snaps_pre;
    free(lat);
    free(steps);
    return r;
}

static void Hdr(void)
{
    printf("%-34s %-6s %6s %6s %6s %7s %7s %7s %6s %6s %6s %7s %5s %6s\n",
           "scenario", "mode", "jerk%", "stall%", "wob", "lat", "lat5",
           "lat95", "extr%", "delay", "ringX%", "stepCV", "snap", "bigst");
}

static void Hdr2(void)
{
}

static void row(const char *n, const char *m, ResT r)
{
    printf
        ("%-34s %-6s %6.2f %6.2f %6.2f %7.1f %7.1f %7.1f %6.1f %6.1f %6.1f %7.3f %5.0f %6.0f jm %.0f jp %.0f\n",
         n, m, r.jerk_pct, r.stall_pct, r.wob, r.lat_mean, r.lat_p5,
         r.lat_p95, r.extrap_pct, r.delay, r.ring_stuck_pct, r.step_cv,
         r.snaps, r.big_step, r.jm, r.jpk);
}

// ---- host upload budget study (re/host-upload-budget-1019.md): how much does pacing the host's msg 7 relay to one receiver cost in smoothness?
static void HostbwMain(void)
{
    static const struct {
        const char *n;
        double iv, ivs;
        int mode;
    } pol[] = {
        { "full (baseline)", 0, 0, 0 }, { "40 Hz", 25, 0, 0 }, { "30 Hz",
                                                                33.3, 0,
                                                                0 },
            { "20 Hz", 50, 0, 0 }, { "15 Hz", 66.7, 0, 0 }, { "10 Hz", 100,
                                                             0, 0 },
            { "5 Hz", 200, 0, 0 },
        { "still: 10 Hz, moving: full", 0, 100, 1 },
            { "still: 5 Hz, moving: full", 0, 200, 1 },
            { "still 10 Hz + moving 30 Hz", 33.3, 100, 0 }
    };
    struct {
        const char *n;
        double rtt, jit, spk, spkps, loss, burst, sfps, hfps;
    } tier[3] =
        { { "clean", 50, 4, 0, 0, 0, 1, 90, 64 }, { "typical", 150, 14, 70,
                                                   0.05, 1, 2, 90, 64 },
        { "bad", 300, 24, 160, 0.2, 4, 3, 144, 56 } };
    printf("%-9s %-28s %7s %6s %6s %6s %6s %7s %7s %7s %6s %6s %7s\n",
           "tier", "msg 7 policy", "sent/s", "jerk%", "stall%", "wob",
           "stepCV", "lat", "lat95", "extr%", "delay", "ringX%",
           "ckStill%");
    for (int t = 0; t < 3; t++)
        for (unsigned k = 0; k < sizeof pol / sizeof *pol; k++) {
            ScnT c =
                { "", tier[t].sfps, tier[t].hfps, 0.08, tier[t].rtt,
              tier[t].jit, tier[t].spk, tier[t].spkps, tier[t].loss,
              tier[t].burst, 144, 60, 0, 0, 0, 0, 3.0e5, pol[k].iv,
              pol[k].ivs, pol[k].mode };
            ResT r = run(&c, 1, 0);
            printf
                ("%-9s %-28s %7.1f %6.2f %6.2f %6.2f %6.3f %7.1f %7.1f %7.1f %6.1f %6.1f %7.1f\n",
                 tier[t].n, pol[k].n, fwd7_sent / c.dur, r.jerk_pct,
                 r.stall_pct, r.wob, r.step_cv, r.lat_mean, r.lat_p95,
                 r.extrap_pct, r.delay, r.ring_stuck_pct, r.ck_still_pct);
            if (getenv("SIMDBG2"))
                printf("          jmean %.1f jpk %.1f lat5 %.1f\n", r.jm,
                       r.jpk, r.lat_p5);
        }
}

// ---- host upload model (re/host-upload-budget-1019.md): N players move in an arena (random waypoint, or two squads), the host relays msg 7 / msg 5
// to N-1 clients; hostbw.h decides per relayed list. Sizes from the field logs: msg 7 625 B + 12, msg 5 191 B + 12, per sender frame; host frame 60 fps.
static void BwmodelOne(int N, double arena, int squads,
                        double budget_KBps, double host_fps,
                        double sender_fps, int verbose)
{
    static float P[8][3], W[8][3], SP[8], WC[2][3];
    uint32_t rs = 4242;
#define BR() (rs ^= rs << 13, rs ^= rs >> 17, rs ^= rs << 5, (rs >> 8) / 16777216.0)
    for (int i = 0; i < N; i++) {
        P[i][0] = (float) (BR() * arena);
        P[i][2] = (float) (BR() * arena);
        W[i][0] = P[i][0];
        W[i][2] = P[i][2];
        SP[i] = 0;
    }
    for (int t = 0; t < 2; t++) {
        WC[t][0] = (float) (BR() * arena);
        WC[t][2] = (float) (BR() * arena);
    }
    HbwT h;
    HbwInit(&h, 1, budget_KBps);
    double dur = 600.0, dt = 1000.0 / host_fps, now = 1000.0;
    uint64_t tx = 0;
    double drop_b7 = 0, tot_b = 0, off_tot = 0, lvl_t[4] =
        { 0 }, tier_n[3] = { 0 };
    double acc7 = 0;            // msg 7 per pair per host frame = sender_fps / host_fps lists (forwarded as they arrive)
    for (double t = 0; t < dur * 1000.0; t += dt, now += dt) {
        for (int i = 0; i < N; i++) {
            int team = i < N / 2 ? 0 : 1;
            if (squads) {
                if (BR() < 0.0005) {
                    WC[team][0] = (float) (BR() * arena);
                    WC[team][2] = (float) (BR() * arena);
                }
            }
            float tx_ =
                squads ? WC[team][0] + (float) ((i % 4) * 3 - 4) : W[i][0],
                tz_ =
                squads ? WC[team][2] + (float) ((i % 3) * 3 - 3) : W[i][2];
            float dx = tx_ - P[i][0], dz = tz_ - P[i][2], d =
                sqrtf(dx * dx + dz * dz);
            if (!squads && d < 2.0f) {
                W[i][0] = (float) (BR() * arena);
                W[i][2] = (float) (BR() * arena);
                SP[i] = BR() < 0.3 ? 0.0f : (float) (1.5 + BR() * 4.0);
            }
            float sp = d < 1.0f ? 0.0f : (squads ? 4.0f : SP[i]);
            if (sp > 0 && d > 0) {
                P[i][0] += dx / d * sp * (float) dt *0.001f;
                P[i][2] += dz / d * sp * (float) dt *0.001f;
            }
        }
        acc7 += sender_fps / host_fps;
        int n7 = (int) acc7;
        acc7 -= n7;
        for (int dst = 1; dst < N; dst++)
            for (int src = 0; src < N; src++) {
                if (src == dst)
                    continue;
                tx += 203;
                tot_b += 203;
                for (int k = 0; k < n7; k++) {
                    double nw = now + k * (dt / n7);
                    float d = -1.0f;
                    if (src != 0) {
                        float dx = P[src][0] - P[dst][0], dz =
                            P[src][2] - P[dst][2];
                        d = sqrtf(dx * dx + dz * dz);
                    }
                    int ok =
                        src == 0 ? 1 : HbwMsg7(&h, nw, src, dst, d, 637,
                                                tx);
                    if (ok) {
                        tx += 637;
                        tot_b += 637;
                    } else
                        drop_b7 += 637;
                    if (src != 0) {
                        int tr =
                            d < HBW_NEAR_M ? 0 : d < HBW_FAR_M ? 1 : 2;
                        tier_n[tr]++;
                    }
                    off_tot += 637;
                }
            }
        lvl_t[h.level] += dt;
    }
    double s = dur, tn = tier_n[0] + tier_n[1] + tier_n[2];
    printf
        ("%d players %3.0fm %-6s budget %4.0f: offered %6.0f KB/s -> tx %6.0f KB/s (saved %4.1f%% of tx); level time 0/1/2/3 %2.0f/%2.0f/%2.0f/%2.0f%%, changes %d; relayed pairs near/mid/far %2.0f/%2.0f/%2.0f%%\n",
         N, arena, squads ? "squads" : "random", budget_KBps,
         (tot_b + drop_b7) / s / 1024.0, tot_b / s / 1024.0,
         100.0 * drop_b7 / (tot_b + drop_b7),
         100 * lvl_t[0] / (dur * 1000), 100 * lvl_t[1] / (dur * 1000),
         100 * lvl_t[2] / (dur * 1000), 100 * lvl_t[3] / (dur * 1000),
         h.lvl_changes, 100 * tier_n[0] / tn, 100 * tier_n[1] / tn,
         100 * tier_n[2] / tn);
    (void) verbose;
    (void) off_tot;
}

static void BwmodelMain(void)
{
    static const double ar[3] = { 60, 100, 150 };
    for (int N = 4; N <= 8; N += 2)
        for (int a = 0; a < 3; a++)
            for (int sq = 0; sq < 2; sq++)
                BwmodelOne(N, ar[a], sq, 600, 60, 90, 0);
    printf
        ("-- 8 players, arena 100, budget sweep (squads, then random)\n");
    static const double bud[5] = { 300, 450, 600, 900, 1500 };
    for (int b = 0; b < 5; b++) {
        BwmodelOne(8, 100, 1, bud[b], 60, 90, 0);
        BwmodelOne(8, 100, 0, bud[b], 60, 90, 0);
    }
}

int main(int argc, char **argv)
{
    int only_ring = argc > 1 && !strcmp(argv[1], "ring");
    if (argc > 1 && !strcmp(argv[1], "mig")) {
        ScnT e =
            { "", 144, 60, 0.08, 150, 20, 100, 0.1, 1, 2, 144, 60, 30, 0,
          0, 0, atof(argv[2]) };
        ResT r = run(&e, 1, 0);
        row("mig", "ns", r);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "bwmodel")) {
        BwmodelMain();
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "hostbw")) {
        HostbwMain();
        return 0;
    }
    ScnT base =
        { "", 144, 60, 0.08, 120, 12, 0, 0, 0, 1, 144, 60, 0, 0, 0, 0,
   3.0e5 };
    Hdr();
    char nm[120];
    if (!only_ring) {
        static const double sf[3] = { 60, 90, 144 }, hf[3] =
            { 56, 64, 72 };
        // tiers: clean LAN-ish, typical EU<->EU/NA (jm~17 jp~34 like the logs), bad EU<->NA with loss and spikes
        struct {
            const char *n;
            double rtt, jit, spk, spkps, loss, burst;
        } tier[3] =
            { { "clean", 50, 4, 0, 0, 0, 1 }, { "typical", 150, 14, 70,
                                               0.05, 1, 2 }, { "bad", 300,
                                                              24, 160, 0.2,
                                                              4, 3 } };
        struct {
            double jerk, stall, wob, lat, ex, delay, p95, n;
        } acc[3] = { { 0 } };
        for (int t = 0; t < 3; t++)
            for (int a = 0; a < 3; a++)
                for (int b = 0; b < 3; b++) {
                    if (!(a == b || (a == 2 && b == 0)))
                        continue;
                    ScnT c = base;
                    c.sfps = sf[a];
                    c.hfps = hf[b];
                    c.rtt = tier[t].rtt;
                    c.jit = tier[t].jit;
                    c.spike_ms = tier[t].spk;
                    c.spike_per_s = tier[t].spkps;
                    c.loss = tier[t].loss;
                    c.burst = tier[t].burst;
                    c.rfps = (a == 0 ? 60 : 144);
                    snprintf(nm, sizeof nm, "%s s%.0f h%.0f r%.0f",
                             tier[t].n, c.sfps, c.hfps, c.rfps);
                    if (getenv("SIMSUM") == NULL) {
                        if (getenv("SIMNOWINMM") == NULL)
                            row(nm, "winmm", run(&c, 0, 0));
                        row(nm, "ns", run(&c, 1, 0));
                    } else {
                        ResT r = run(&c, 1, 0);
                        acc[t].jerk += r.jerk_pct;
                        acc[t].stall += r.stall_pct;
                        acc[t].wob += r.wob;
                        acc[t].lat += r.lat_mean;
                        acc[t].ex += r.extrap_pct;
                        acc[t].delay += r.delay;
                        acc[t].p95 += r.lat_p95 - r.lat_p5;
                        acc[t].n++;
                    }
                }
        if (getenv("SIMSUM"))
            for (int t = 0; t < 3; t++)
                printf
                    ("%-8s jerk %.3f stall %.3f wob %.3f lat %.1f latspread %.1f extr %.2f delay %.1f\n",
                     tier[t].n, acc[t].jerk / acc[t].n,
                     acc[t].stall / acc[t].n, acc[t].wob / acc[t].n,
                     acc[t].lat / acc[t].n, acc[t].p95 / acc[t].n,
                     acc[t].ex / acc[t].n, acc[t].delay / acc[t].n);
    }
    if (getenv("SIMSUM"))
        return 0;
    {                           // stress rows (the log windows with jx 800-1800 ms, 40 % relay loss, a slow host)
        ScnT b2 = base;
        b2.rtt = 150;
        b2.jit = 14;
        b2.spike_ms = 70;
        b2.spike_per_s = 0.05;
        b2.loss = 1;
        b2.burst = 2;
        b2.rfps = 144;
        ScnT x;
        x = b2;
        x.spike_ms = 800;
        x.spike_per_s = 0.08;
        snprintf(nm, sizeof nm, "stall 400-1200 ms @0.08/s");
        row(nm, "winmm", run(&x, 0, 0));
        row(nm, "ns", run(&x, 1, 0));
        x = b2;
        x.loss = 25;
        x.burst = 4;
        snprintf(nm, sizeof nm, "loss 25%% bursts of 4");
        row(nm, "winmm", run(&x, 0, 0));
        row(nm, "ns", run(&x, 1, 0));
        x = b2;
        x.hfps = 24;
        x.sfps = 90;
        snprintf(nm, sizeof nm, "host 24 fps, sender 90");
        row(nm, "winmm", run(&x, 0, 0));
        row(nm, "ns", run(&x, 1, 0));
        x = b2;
        x.sfps = 30;
        x.hfps = 30;
        x.rfps = 60;
        snprintf(nm, sizeof nm, "sender 30 host 30 recv 60");
        row(nm, "winmm", run(&x, 0, 0));
        row(nm, "ns", run(&x, 1, 0));
        x = b2;
        x.flips = 0; {
            ResT r = run(&x, 1, 0);
            printf
                ("vec4[0] angular step per frame, no sign flips: p99 %.4f max %.4f rad, frames > 0.012 rad: %.0f\n",
                 g_vstep_p99, g_vstep_max, g_vsteps_big);
        }
        x = b2;
        x.flips = 1;
        snprintf(nm, sizeof nm, "vec4 sign flips"); {
            ResT r = run(&x, 1, 0);
            row(nm, "ns", r);
            printf
                ("vec4[0] angular step per frame, sign flips every 400 ms: p99 %.4f max %.4f rad, frames > 0.012 rad: %.0f\n",
                 g_vstep_p99, g_vstep_max, g_vsteps_big);
        }
    }
    // events
    ScnT e = base;
    e.rtt = 150;
    e.jit = 20;
    e.spike_ms = 100;
    e.spike_per_s = 0.1;
    e.loss = 1;
    e.burst = 2;
    ScnT m = e;
    m.t_mig = 30;
    m.mig_new_up = 2.0e4;
    snprintf(nm, sizeof nm, "migration (new clock 2e4, back)");
    row(nm, "ns", run(&m, 1, 0));
    m.mig_new_up = 3.0e5;
    snprintf(nm, sizeof nm, "migration (new clock 3e5 < base)");
    row(nm, "ns", run(&m, 1, 0));
    m.mig_new_up = 2.0e8;
    snprintf(nm, sizeof nm, "migration (new clock 2e8 fwd)");
    row(nm, "ns", run(&m, 1, 0));
    ScnT rs = e;
    rs.t_resp = 25;
    snprintf(nm, sizeof nm, "respawn 47 m teleport");
    row(nm, "ns", run(&rs, 1, 0));
    row(nm, "winmm", run(&rs, 0, 0));
    ScnT q = e;
    q.quant = 1;
    q.sfps = 143;
    snprintf(nm, sizeof nm, "quantised sender 143 fps");
    row(nm, "ns", run(&q, 1, 0));
    return 0;
}
