// ---------------------------------------------------------------- netsmooth: remote players' motion (re/netcode.md)
// Included by t2r-ext.c before the "puppet anim smoothing" section. Static RE + compile checks only.
//
// winmm (checked in winmm.dll):
//  - The sender sends msg 5 (full state: pos +0x68, quat +0x74, 5 root vec4 +0xc5, hp, weapon, plus
//    the piggy-backed reliable queue and acks) and msg 7 (AnimCmdList, its header seq = msg 5's seq,
//    its body = the sender's timeGetTime) in EVERY frame (pump 0x18005b5b0, seq counter 0x180177f44
//    incremented at 0x18005d09b). So the send rate is the sender's fps.
//  - The receiver applies msg 5 in 0x18004c490 (newer seq only; +0x517 = previous state, +0x9cc =
//    time since it, +0x9d0 = EWMA of the arrival interval clamped 8..200 ms, by the pump's local clock).
//  - The puppet is placed every frame at 0x180035d5c..0x180035ddf:
//        pos = cur + min(elapsed / interval, 0.7) * (cur - prev),   rot = newest quat (no interpolation)
//    i.e. it EXTRAPOLATES past the newest packet up to 0.7 of an interval, stops, and jumps on the
//    next packet. No buffer: every arrival jitter (two packets in one frame, then none) is visible
//    as a freeze + jump ("jitter: slot N jumped 0.6 m" in bursts in test logs).
//  - The animation (msg 7 replay, 0x180027c00) runs a separate clock (slot+0x74c8) in SENDER ms:
//    target = newest list time - 25 ms, clock += half the gap per frame, clamped to the newest.
//    Those times go through cvtsi2ss of the raw timeGetTime: a sender up for 1-2 days (timeGetTime
//    ~1e8 ms) gets its anim clock quantized to 8-16 ms steps.
//
// What this adds (each with an off switch next to the exe):
//  1. Sender-time rebase (t2r-no-anim-rebase.txt, read at start): msg 7's time is passed to winmm
//     minus a per-slot base, so its float math is exact again.
//  2. Jitter buffer (t2r-no-net-smooth.txt, re-read every 5 s): every msg 5 sample gets the sender's
//     time (msg 7 with the same seq; estimated from the sender frame time when that list is missing).
//     A playout clock per slot runs at real time (+-8 % slew) at
//        R = now + offset - delay,  offset = max(sender t - arrival) over 4 s (least-delayed packet),
//        delay = 1.5 x jitter peak + one sender frame + 2 ms, 16..150 ms (t2r-net-delay.txt: fixed ms)
//     The puppet's locator is interpolated at R (lerp + slerp); past the newest sample it is
//     extrapolated with the last velocity (max 10 m/s) for at most 100 ms, then held. When samples
//     come back after an extrapolation the difference is absorbed (decays, tau 100 ms) instead of
//     snapped; above 1.5 m (respawn, teleport) it snaps. Sample pairs more than 4 m apart are never
//     interpolated. The anim replay's clock target is set to the same R (patched load at 0x180027e10),
//     so pose and root use one timeline.
//  3. Send cap (t2r-net-sendcap.txt = Hz, opt-in, re-read every 5 s): msg 5 + 7 at most at that rate
//     instead of every frame (decision at 0x18005d09b, so the reliable queue / acks stay intact).
//     Off by default: msg 5 may carry one-frame state fields that a skipped frame would lose (not
//     verified).
//  4. "net stats:" every 10 s: per slot rx rates, loss, jitter, delay, extrapolation, snaps, and a
//     stutter count for winmm's formula and for the smoothed output; tx per message type.
// All messages and their formats stay as they are: old peers are unaffected.

// anim job per puppet slot (animjob.h): a stale job after a body change was the cause of "anim clock advanced 0/s"
#define AJ_READABLE(p, n) (!IsBadReadPtr((p), (n)))
#define AJ_XCHG(pp, v) InterlockedExchangePointer((PVOID volatile *)(pp), (PVOID)(v))
#include "animjob.h"
static AjCache g_aj[8];
static volatile int32_t g_ajCur[8];    // table id of the puppet's last replay call (0 = never seen)
static volatile uint32_t g_ajSwapped[8], g_aj_dropped[8];      // jobs swapped back from the cache / cleared for winmm to rebuild

#define NS_RING 64
#define NS_M7 32
#define NS_DMIN 16.0
#define NS_DFLOOR 24.0          // with the prediction lead the playout delay never goes below this
#define NS_DMAX 200.0
#define NS_EXTRAP_MS 100.0
#define NS_MAX_SPEED 0.010f     // m per ms (10 m/s)
#define NS_TELEPORT_M 4.0f
#define NS_SNAP_M 1.5f
#define NS_TAU_MS 100.0
#define NS_BLEND_MS 100.0       // winmm placement -> buffered placement hand-over (a puppet's first frames / after a gap)
#define NS_STUTTER_MPS 3.0f     // per-frame speed change that counts as a stutter frame
// playout delay tuning (overridable with -D for the offline sweeps in tools/ext/tests/netsim_sim.c)
#ifndef NS_JK
#define NS_JK 1.3               // delay = NS_JK x jitter peak + one sample interval + 2 ms (was 1.5; offline sweep: -4..-17 ms, +0.0..+0.6 pt extrapolation)
#endif
#ifndef NS_JPK_TAU_MS
#define NS_JPK_TAU_MS 2000.0    // the jitter peak decays with this time constant (was 0.5 % per msg 7 = 1.4 s at 144 lists/s, 3.3 s at 60/s)
#endif
#ifndef NS_DCUR_DOWN
#define NS_DCUR_DOWN 0.008      // the stable delay comes down this many ms per ms (8 ms/s; was 4: after a spike the latency stayed up for tens of seconds)
#endif
#ifndef NS_DCUR_UP_HYST
#define NS_DCUR_UP_HYST 6.0
#endif
#ifndef NS_DCUR_DN_HYST
#define NS_DCUR_DN_HYST 12.0
#endif

typedef struct {
    uint32_t seq;
    uint8_t t_ok, tele, v_ok;
    double t, rl;
    float p[3], q[4], v[20];
} NsSamp;                      // t_ok: 0 none, 1 estimated, 2 msg 7; v = the 5 root vec4 (peer+0xc5) of that msg 5
typedef struct {
    NsSamp s[NS_RING];
    uint32_t n;
    uint32_t m7seq[NS_M7];
    double m7t[NS_M7];
    uint32_t m7n;
    uint32_t a_seq;
    double a_t;
    int have_anchor;            // last msg 7 (anchor)
    double sdt;                 // sender ms per seq step
    int64_t bk_id[8];
    double bk_max[8];
    uint8_t bk_ok[8];
    double oref;
    int synced;
    double gavg;                // host-paced msg 7 (hostbw.h): EWMA of the seq step between two received lists (0 = unset = 1)
    double jpk, jmean, loss, j_rl;      // loss: EWMA of the msg 7 loss fraction (seq gaps); j_rl: arrival time of the last list (time-based jpk decay)
    double R, R_last, fdt, f_last;
    int R_ok;
    double delay, dcur;         // dcur: the stable base delay (fast up, slow down)
    int out_ok, extrap;
    double out_last;
    float out_p[3], out_q[4], E[3], hold_q[4], hold_b, vel[3];
    float w_last[3], w_v, o_last[3], o_v;
    int w_ok, o_ok;             // stutter metric state (winmm formula / ours)
    uint32_t base;
    int have_base;
    int ours;
    float xb;                   // ours: last frame's locator was ours; xb: weight of winmm's own placement still blended in (1 -> 0 over NS_BLEND_MS)
    // 10 s stats
    uint32_t st_m5, st_m5_rej, st_lost, st_m7, st_m7_est, st_frames,
        st_extrap, st_hold, st_snap, st_corr, st_resync, st_tele;
    uint32_t st_stut_w, st_stut_o, st_moving;
    // render-path diagnostics (puppets looked choppy at 144 Hz): is each stage advancing per RENDERED frame?
    float d_last[3], d_lastq[4];
    int d_ok;
    uint32_t st_dist;           // distinct final root placements (locator calls = st_frames)
    float ck_last;
    int ck_ok;
    int32_t stp_last[2];
    uint32_t st_ck_adv, st_ck_still, st_ck_irreg, st_ck_snap, st_stp;   // anim replay clock (slot+0x74c8) + pose stamps (+0x74d4/8)
    double st_ck_ratio;
    uint32_t st_ck_n;
    uint8_t v_last[80];
    int v_ok;
    uint32_t st_v_calls, st_v_dist;
    float v_dpos_sum, v_dpos_min, v_dpos_max;
    uint32_t v_dn;              // the five root vec4 (peer+0xc5, pose writer)
    float v_samp[20], v_samp_p[3];
    int v_samp_ok;
    float st_corr_max, st_jmax;
    double st_dsum, st_lagsum;
    uint32_t st_lagn;
    // prediction error: the next real sample vs the position predicted from the samples before it (metres)
    double st_pe_cv, st_pe_hold, st_pe_gap_cv, st_pe_gap_hold;
    float st_pe_max_cv, st_pe_max_hold;
    uint32_t st_pe_n, st_pe_gap_n;
    // Frame-rate regression fixes: (a) msg 5 'loss' was the host relay forwarding a state once per HOST frame (a sender faster than the host shows
    // gaps that are no lost packets): real loss now counts msg 7 seq gaps (msg 7 is forwarded as received), si = measured msg 5 sample spacing;
    // (b) the 5 root vec4 are interpolated on the playout clock (stepped at the msg 5 rate before); (c) smoothness per rendered frame + a dump of
    // the replay state when a slot's anim clock stands still.
    double si;
    uint32_t st_lost7, st_vf_dist, st_vf_used, st_vf_calls;
    float vf_last[20];
    int vf_ok;
    uint32_t ck_run, t_raw, t_rb, t_dq, st_frozen_ep;
} NsSlot;
static NsSlot g_ns[T2R_MAX_PEERS];
static SRWLOCK g_nsLock = SRWLOCK_INIT;
static int g_nsOff = -1, g_ns_rebase = 1, g_ns_inst;
static double g_nsLeadK = 0.5, g_ns_lead_max = 60.0;  // t2r-net-lead.txt: prediction lead = k x measured one-way, capped (ms); k 0 = off
static double g_nsFixedDelay; // t2r-net-delay.txt (0 = adaptive)
static volatile int g_nsCapHz;        // t2r-net-sendcap.txt (0 = every frame, winmm)
static volatile uint32_t g_nsM7seq;    // msg 7 header seq (r14d at 0x18005bf6c, stored by the call stub)
typedef struct {
    float tgt;
    int32_t valid;
} NsAnimT;
static volatile NsAnimT g_nsAnim[T2R_MAX_PEERS];     // read by the anim clock stub (anim job threads)
static volatile uint32_t g_nsTxN[256], g_ns_tx_b[256];
// host capacity report (re/host-capacity-1019.md): what the 30 s performance report (mem_probe.c perf_report_tick, op 0xCA trailer) says about OUR upload and
// the loss we saw through the host's relay. Filled every 10 s stat window in a match; the report takes and clears it.
static volatile uint32_t g_cap_tx_sum_kb, g_cap_tx_peak_kb, g_cap_n,
    g_cap_loss10;
static volatile uint64_t g_nsTxTotal; // never reset: every datagram byte winmm's low send got (hostbw.h demand)
// One-way link detector (re/oneway-p2p-1019.md): in a match we SEND (>= OW_TX_BYTES since the last datagram we received) but nothing has arrived for
// OW_QUIET_MS = the Steam P2P inbound path is dead (tester des: host, rx 0, the match ended 4-6 s after the start). Logs once per quiet spell and
// asks the 30 s performance report (mem_probe.c) to go out at once with flag bit1 + the numbers, so the daemon sees it even when the match is over
// before the next regular report. Pure observation: nothing is changed in the game. t2r-no-oneway.txt = off.
#define OW_QUIET_MS 4000.0
#define OW_TX_BYTES 20000u
static volatile LONG g_owAlarm;        // 1 = a one-way spell was seen, not reported yet
static volatile LONG g_connLostMatch; // 1 = OUR link died in this match (inbound dead for seconds / the host went quiet and the session closed): the clan report carries flag bit 2 so the daemon does not book 'left early'; reset at each match start (feature_clan)
static volatile uint32_t g_ow_streak_s, g_ow_rx_pkts, g_ow_tx_kb, g_ow_in_s;    // numbers of the last alarm (report trailer)
static int g_ow_in, g_ow_fired, g_ow_off = -1;
static double g_ow_t0, g_ow_last_rx_t;
static uint64_t g_ow_last_rx, g_ow_rx0, g_ow_tx_at_rx;
static void OwTick(double now, uint64_t rx)
{
    if (g_ow_off < 0) {
        FILE *f = ExeSideOpen("t2r-no-oneway.txt", "r");
        g_ow_off = f != NULL;
        if (f)
            fclose(f);
    }
    if (g_ow_off)
        return;
    if (MatchSubstate() != 6) {
        g_ow_in = 0;
        return;
    }
    if (!g_ow_in) {
        g_ow_in = 1;
        g_ow_fired = 0;
        g_ow_t0 = g_ow_last_rx_t = now;
        g_ow_last_rx = g_ow_rx0 = rx;
        g_ow_tx_at_rx = g_nsTxTotal;
        return;
    }
    if (rx != g_ow_last_rx) {
        g_ow_last_rx = rx;
        g_ow_last_rx_t = now;
        g_ow_tx_at_rx = g_nsTxTotal;
        g_ow_fired = 0;
        return;
    }
    if (g_ow_fired || now - g_ow_last_rx_t < OW_QUIET_MS)
        return;
    uint64_t sent = g_nsTxTotal - g_ow_tx_at_rx;
    if (sent < OW_TX_BYTES)
        return;                 // nobody to talk to (alone in the lobby): not one-way
    g_ow_fired = 1;
    g_ow_streak_s = (uint32_t) ((now - g_ow_last_rx_t) / 1000.0);
    g_ow_in_s = (uint32_t) ((now - g_ow_t0) / 1000.0);
    g_ow_rx_pkts = (uint32_t) (g_ow_last_rx - g_ow_rx0);
    g_ow_tx_kb = (uint32_t) (sent >> 10);
    Xlog("oneway: NO datagram received for %u s (%u received in this match, %u s in) while we sent %u KB since the last one - the inbound path is dead (Steam P2P / helper); reporting to the server", g_ow_streak_s, g_ow_rx_pkts, g_ow_in_s, g_ow_tx_kb);
    g_owAlarm = 1;
    g_connLostMatch = 1;
}

// Host upload budget (hostbw.h), DEFAULT OFF: t2r-host-bw.txt = 'budget KB/s' (default 600) or env T2R_HOST_BW. g_ns_gap_tol: receivers read a regular
// seq step in msg 7 as a paced relay, not as loss (otherwise the playout delay grows by the loss term, up to +80 ms; offline sim).
static HbwT g_hbw;
static volatile int g_nsGapTol;
static volatile uint32_t g_ns_cap_ok, g_ns_cap_skip;
static double g_nsNextSend;
// msg 7 level of detail: the anim list is the big datagram (430-850 B, 60/s to each of 7 peers). To a peer that is far
// (> 25 m) or out of our view and not close (> 12 m) it goes out at every 2nd frame (30 Hz); near peers keep every frame. msg 5 (state) is never
// touched. g_ns_m7_half[dest slot] is refreshed by ns_far_tick (main thread, t2r-ext.c); t2r-no-m7-lod.txt = off.
static volatile uint8_t g_nsM7Half[T2R_MAX_PEERS];
static volatile int g_nsM7LodOff;
static uint8_t g_nsM7Tog[16];
static volatile uint32_t g_ns_m7_full_n, g_ns_m7_half_n, g_ns_m7_skip_n;

static double NsNowMs(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double) c.QuadPart * 1000.0 / (double) f.QuadPart;
}

static float NsDot4(const float *a, const float *b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
}

static void NsNorm4(float *q)
{
    float l = sqrtf(NsDot4(q, q));
    if (l > 1e-6f)
        for (int i = 0; i < 4; i++)
            q[i] /= l;
}

static void NsSlerp(float *o, const float *a, const float *b0, float f)
{
    float b[4] = { b0[0], b0[1], b0[2], b0[3] }, d = NsDot4(a, b);
    if (d < 0.0f) {
        for (int i = 0; i < 4; i++)
            b[i] = -b[i];
        d = -d;
    }
    float wa = 1.0f - f, wb = f;
    if (d < 0.9995f) {
        float th = acosf(d), s = sinf(th);
        wa = sinf((1.0f - f) * th) / s;
        wb = sinf(f * th) / s;
    }
    for (int i = 0; i < 4; i++)
        o[i] = wa * a[i] + wb * b[i];
    NsNorm4(o);
}

static int NsFinite(const float *v, int n)
{
    for (int i = 0; i < n; i++)
        if (!(v[i] == v[i]) || v[i] > 1e6f || v[i] < -1e6f)
            return 0;
    return 1;
}

static void NsReset(NsSlot *s)
{
    uint32_t keep_base = s->base;
    int kb = s->have_base;
    memset(s, 0, sizeof *s);
    s->base = keep_base;
    s->have_base = kb;
    s->sdt = 16.7;
    s->fdt = 16.7;
}

static double NsEstT(NsSlot *s, uint32_t seq, double rl)
{
    if (s->have_anchor) {
        double d = (double) (int32_t) (seq - s->a_seq) * s->sdt;
        if (d > -500.0 && d < 500.0)
            return s->a_t + d;
    }
    return s->synced ? rl + s->oref : -1.0;     // no recent list: by arrival (least-delay offset)
}

// velocity of the sample at absolute index `top` over >= 60 ms of history (never across a teleport); returns the reference sample or NULL
static NsSamp *NsVel(NsSlot *s, uint32_t top, float v[3])
{
    NsSamp *nw = &s->s[top % NS_RING], *ref = NULL;
    v[0] = v[1] = v[2] = 0.0f;
    if (!nw->t_ok || nw->tele)
        return NULL;
    uint32_t j = top, lo = s->n > NS_RING ? s->n - NS_RING : 0;
    for (;;) {
        if (j <= lo || s->s[j % NS_RING].tele)
            break;              // a teleport sample is the jump from its predecessor
        j--;
        NsSamp *x = &s->s[j % NS_RING];
        if (!x->t_ok || nw->t - x->t > 200.0)
            break;
        ref = x;
        if (nw->t - x->t >= 60.0)
            break;
    }
    if (!ref || nw->t - ref->t < 1.0 || nw->t - ref->t > 250.0)
        return NULL;
    for (int i = 0; i < 3; i++)
        v[i] = (nw->p[i] - ref->p[i]) / (float) (nw->t - ref->t);
    float sp = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (sp > NS_MAX_SPEED)
        for (int i = 0; i < 3; i++)
            v[i] *= NS_MAX_SPEED / sp;
    return ref;
}

// ---- receive side (pump thread)
static void NsOnState(uint32_t slot, uint32_t seq, const float *p,
                        const float *q, const uint8_t *vraw)
{
    if (slot >= T2R_MAX_PEERS || !NsFinite(p, 3) || !NsFinite(q, 4))
        return;
    double rl = NsNowMs();
    AcquireSRWLockExclusive(&g_nsLock);
    NsSlot *s = &g_ns[slot];
    s->st_m5++;
    NsSamp *pv = s->n ? &s->s[(s->n - 1) % NS_RING] : NULL;
    if (pv && (int32_t) (seq - pv->seq) <= 0) {
        NsReset(s);
        pv = NULL;
        s->st_m5 = 1;
    }                           // winmm took an older seq: reset/new sender
    if (pv && seq - pv->seq > 1 && seq - pv->seq < 1000)
        s->st_lost += seq - pv->seq - 1;
    NsSamp *x = &s->s[s->n % NS_RING];
    memset(x, 0, sizeof *x);
    x->seq = seq;
    x->rl = rl;
    memcpy(x->p, p, 12);
    memcpy(x->q, q, 16);
    NsNorm4(x->q);
    if (vraw) {
        memcpy(x->v, vraw, 80);
        x->v_ok = NsFinite(x->v, 20);
    }
    // (the loss EWMA that drove the playout delay used to be fed from these seq gaps; it is fed from msg 7 gaps now - see ns_on_anim)
    if (pv) {
        // a teleport (respawn) is a jump no player can run: the threshold grows with the time since the previous sample, so a
        // 700 ms gap of a sprinting player (4 m) interpolates instead of freezing and snapping (100-250 ms rtt, 20-35 % loss)
        float dx = p[0] - pv->p[0], dy = p[1] - pv->p[1], dz =
            p[2] - pv->p[2];
        double gap_ms = (double) (int32_t) (seq - pv->seq) * s->sdt;
        if (gap_ms < 120.0)
            s->si = s->si > 0.0 ? s->si + (gap_ms - s->si) * 0.05 : gap_ms;     // sample spacing the buffer has to bracket
        float thr = NS_TELEPORT_M;
        if (gap_ms > 250.0) {
            float t2 =
                2.0f + 0.008f * (float) (gap_ms >
                                         1500.0 ? 1500.0 : gap_ms);
            if (t2 > thr)
                thr = t2;
        }
        x->tele = dx * dx + dy * dy + dz * dz > thr * thr;
        if (x->tele)
            s->st_tele++;
    }
    for (uint32_t k = 0; k < NS_M7 && k < s->m7n; k++)  // its msg 7 came first
        if (s->m7seq[k] == seq) {
            x->t = s->m7t[k];
            x->t_ok = 2;
            break;
        }
    if (!x->t_ok) {
        double t = NsEstT(s, seq, rl);
        if (t >= 0.0) {
            x->t = t;
            x->t_ok = 1;
        }
    }
    if (x->t_ok && pv && pv->t_ok && x->t <= pv->t)
        x->t = pv->t + 0.5;
    if (pv && pv->t_ok && !x->tele && s->n >= 2) {      // how well would dead reckoning from the earlier samples have predicted this one?
        float v[3];
        double dts =
            x->t_ok ? x->t - pv->t : (double) (int32_t) (seq -
                                                         pv->seq) * s->sdt;
        NsVel(s, s->n - 1, v);
        if (dts > 1.0 && dts < 1000.0) {
            float e_cv = 0.0f, e_h = 0.0f;
            for (int i = 0; i < 3; i++) {
                float pr =
                    pv->p[i] + v[i] * (float) (dts >
                                               NS_EXTRAP_MS +
                                               100.0 ? NS_EXTRAP_MS +
                                               100.0 : dts);
                e_cv += (x->p[i] - pr) * (x->p[i] - pr);
                e_h += (x->p[i] - pv->p[i]) * (x->p[i] - pv->p[i]);
            }
            e_cv = sqrtf(e_cv);
            e_h = sqrtf(e_h);
            s->st_pe_n++;
            s->st_pe_cv += e_cv;
            s->st_pe_hold += e_h;
            if (e_cv > s->st_pe_max_cv)
                s->st_pe_max_cv = e_cv;
            if (e_h > s->st_pe_max_hold)
                s->st_pe_max_hold = e_h;
            if (seq - pv->seq > 1) {
                s->st_pe_gap_n++;
                s->st_pe_gap_cv += e_cv;
                s->st_pe_gap_hold += e_h;
            }
        }
    }
    s->n++;
    ReleaseSRWLockExclusive(&g_nsLock);
}

static void NsOnAnim(uint32_t slot, uint32_t seq, double t)
{
    if (slot >= T2R_MAX_PEERS)
        return;
    double rl = NsNowMs();
    AcquireSRWLockExclusive(&g_nsLock);
    NsSlot *s = &g_ns[slot];
    s->st_m7++;
    if (s->have_anchor && (int32_t) (seq - s->a_seq) <= 0
        && (int32_t) (seq - s->a_seq) > -1000) {
        ReleaseSRWLockExclusive(&g_nsLock);
        return;
    }
    if (s->have_anchor) {       // real loss: msg 7 is forwarded by the host as it arrives, so its seq gaps are lost datagrams
        int32_t g = (int32_t) (seq - s->a_seq) - 1;
        if (g >= 0 && g < 1000) {
            s->st_lost7 += (uint32_t) g;
            if (g_nsGapTol) { // a step the stream has had all along (paced relay) is not loss: only what exceeds it
                double G = (double) g + 1.0, A =
                    s->gavg > 1.0 ? s->gavg : 1.0;
                int eg =
                    G > 1.5 * A ? (int) (G - (double) (int) (A + 0.5)) : 0;
                s->gavg = A + ((G > 8.0 ? 8.0 : G) - A) * 0.05;
                g = eg;
            }
            s->loss += ((double) g / (double) (g + 1) - s->loss) * 0.02;
        }
    }
    if (s->have_anchor && seq != s->a_seq) {
        double d = (t - s->a_t) / (double) (int32_t) (seq - s->a_seq);
        if (d > 3.0 && d < 150.0)
            s->sdt += (d - s->sdt) * 0.05;
    }
    if (s->have_anchor && (t - s->a_t > 60000.0 || t < s->a_t))
        NsReset(s);            // a different sender clock
    s->a_seq = seq;
    s->a_t = t;
    s->have_anchor = 1;
    s->m7seq[s->m7n % NS_M7] = seq;
    s->m7t[s->m7n % NS_M7] = t;
    s->m7n++;
    // least-delay offset over 4 s (8 buckets of 500 ms)
    double o = t - rl;
    int64_t b = (int64_t) (rl / 500.0);
    int i = (int) (b & 7);
    if (!s->bk_ok[i] || s->bk_id[i] != b) {
        s->bk_id[i] = b;
        s->bk_max[i] = o;
        s->bk_ok[i] = 1;
    } else if (o > s->bk_max[i])
        s->bk_max[i] = o;
    double oref = -1e300;
    for (int k = 0; k < 8; k++)
        if (s->bk_ok[k] && b - s->bk_id[k] < 8 && s->bk_max[k] > oref)
            oref = s->bk_max[k];
    s->oref = oref;
    double j = oref - o;
    if (!s->synced) {
        s->synced = 1;
        s->jpk = 8.0;
        s->jmean = 4.0;
    }
    {
        double jd = s->j_rl > 0.0 ? rl - s->j_rl : 10.0;
        if (jd < 0.0)
            jd = 0.0;
        if (jd > 500.0)
            jd = 500.0;
        s->j_rl = rl;
        if (j > s->jpk)
            s->jpk += (j - s->jpk) * 0.3;
        else
            s->jpk -= (s->jpk - j) * (1.0 - exp(-jd / NS_JPK_TAU_MS));
    }
    s->jmean += (j - s->jmean) * 0.02;
    if ((float) j > s->st_jmax)
        s->st_jmax = (float) j;
    // pair with the msg 5 sample of the same seq; re-estimate the samples after it
    int found = -1;
    for (uint32_t k = 0; k < 24 && k < s->n; k++) {
        NsSamp *x = &s->s[(s->n - 1 - k) % NS_RING];
        if (x->seq == seq) {
            x->t = t;
            x->t_ok = 2;
            found = (int) k;
            break;
        }
        if ((int32_t) (x->seq - seq) < 0)
            break;
    }
    for (int k = found - 1; k >= 0; k--) {
        NsSamp *x = &s->s[(s->n - 1 - (uint32_t) k) % NS_RING];
        if (x->t_ok != 2) {
            x->t = t + (double) (int32_t) (x->seq - seq) * s->sdt;
            x->t_ok = 1;
        }
    }
    ReleaseSRWLockExclusive(&g_nsLock);
}

// msg 7's sender time: rebased per slot so winmm's float math is exact (raw timeGetTime ~1e8 ms ->
// 8-16 ms float steps).
// winmm keeps the last three list times of a slot in a ring (slot+0x2820 + 8 i, newest index +0x2838, count +0x283c) and pushes a time only when
// it is NEWER than the newest (int32 compare, 0x1800296e3); the ring is emptied only by NetAnimClear (a player leaving / the session ending).
// A time domain change on a slot that is not cleared (host migration with the slot kept, a joiner taking it over without the clear) used to be
// handled as: ring not empty -> pass the raw time through. That is harmful whenever the new times are SMALLER than the ring's newest (a sender
// with less uptime than the old base's age, or just the rebased value of the old base): the ring never accepts them again, its newest stays
// frozen (model: 28 s on a 60 s run, tools/ext/tests/netsim) until the new clock overtakes it, which is the list time the replay's pose stamps
// and blend factor come from. Now a backward jump of the sender's clock (> 2 s, confirmed by 3 consecutive lists so one stale datagram cannot trigger
// it) or a forward jump into another domain (> 60 s and far beyond the base, the same rule ns_on_anim uses) is a clean rebase: the ring is emptied
// exactly like NetAnimClear does (count 0, index 0) and the interpolation state is flagged for a reset (+0x77e4 bit 0), then the new base is taken.
// A time that is only a little older than the base (a reordered datagram from before our first list) is passed as 0 (the ring ignores it).
static uint32_t g_nsLastraw[T2R_MAX_PEERS], g_ns_cand_last[T2R_MAX_PEERS];
static uint8_t g_nsLastrawOk[T2R_MAX_PEERS], g_ns_cand_n[T2R_MAX_PEERS];
static volatile uint32_t g_nsRbClears[T2R_MAX_PEERS];
static void NsRebaseClearRing(uint8_t *pslot)
{
    InterlockedExchange((volatile LONG *) (pslot + 0x283c), 0);
    InterlockedExchange((volatile LONG *) (pslot + 0x2838), 0);
    InterlockedOr((volatile LONG *) (pslot + 0x77e4), 1);
}

static uint32_t NsRebase(uint32_t slot, uint32_t t, uint8_t *pslot)
{
    if (!g_ns_rebase || slot >= T2R_MAX_PEERS)
        return t;
    NsSlot *s = &g_ns[slot];
    int ring_empty = *(volatile int32_t *) (pslot + 0x283c) <= 0;
    int32_t rel = (int32_t) (t - s->base);
    int change = 0;
    if (s->have_base && g_nsLastrawOk[slot]) {
        int32_t d = (int32_t) (t - g_nsLastraw[slot]);
        if (d < -2000) {        // the sender's clock went backwards: confirm with consecutive lists
            if (g_ns_cand_n[slot]
                && (int32_t) (t - g_ns_cand_last[slot]) > -1000
                && (int32_t) (t - g_ns_cand_last[slot]) < 1000)
                g_ns_cand_n[slot]++;
            else
                g_ns_cand_n[slot] = 1;
            g_ns_cand_last[slot] = t;
            if (g_ns_cand_n[slot] >= 3 || ring_empty)
                change = 1;     // (an emptied ring cannot get stuck: no confirmation needed)
            else
                return 0;       // not (yet) a new domain: the ring ignores it
        } else {
            g_ns_cand_n[slot] = 0;
            if (rel > (1 << 24) && d > 60000)
                change = 1;     // far ahead in another domain: rebase (precision + ring)
        }
    }
    if (!s->have_base || change
        || ((rel < 0 || rel > (1 << 24)) && ring_empty)) {
        if (s->have_base)
            Xlog("net stats: slot %u sender clock rebased (old base %u, t %u%s)", slot, s->base, t, change && !ring_empty ? ", winmm time ring emptied" : "");
        if (change && !ring_empty) {
            NsRebaseClearRing(pslot);
            g_nsRbClears[slot]++;
        }
        s->base = t - 1000;
        s->have_base = 1;
        g_ns_cand_n[slot] = 0;
        g_nsLastraw[slot] = t;
        g_nsLastrawOk[slot] = 1;
        AcquireSRWLockExclusive(&g_nsLock);
        NsReset(s);
        ReleaseSRWLockExclusive(&g_nsLock);
        rel = 1000;
    }
    if (!g_nsLastrawOk[slot] || (int32_t) (t - g_nsLastraw[slot]) > 0) {
        g_nsLastraw[slot] = t;
        g_nsLastrawOk[slot] = 1;
    }
    if (rel < 0)
        return 0;               // a little older than our base (reordered, from before our first list)
    return (uint32_t) rel;
}

// ---- sender clock de-quantization (remote players looked choppy at any fps)
// msg 7's time is the sender's timeGetTime. Every net stats line shows "sender frame 15.6-15.8 ms" for every peer, whatever its fps (60..135):
// that is the 15.625 ms Windows timer tick (the game never asks for a finer one). The staircase makes consecutive frames carry the SAME
// time (or 15/31 ms steps for a 60 fps sender), so the interpolation of position (ns_locator) and of the anim replay (pose stamps
// +0x74d4/8 -> blend factor) runs at wildly uneven speeds: holds then double steps (a nameplate steps 0,26,-1,23,12,21,...).
// Fix: (a) this process asks for a 1 ms timer (NtSetTimerResolution) so OUR sends are exact; (b) for senders that are still quantized the
// time is rebuilt from the frame sequence: least-squares line through the last 24 (seq, t) pairs, evaluated at the newest seq (+ half a tick,
// the quantizer floors). A sender is judged quantized when >= 90 % of the last 24 times sit on one 15.625 ms grid (|frac| < 0.09);
// a 1 ms sender is untouched (its times are uniform on the grid, p ~ 1e-20). t2r-no-dequant.txt = off, t2r-no-timerres.txt = no timer request.
#define NS_DQ_N 24
#define NS_DQ_Q 15.625
typedef struct {
    uint32_t seq[NS_DQ_N];
    double t[NS_DQ_N];
    uint32_t n, last_out;
    int have_out, q;
    uint32_t st_total, st_q;
} NsDqT;
static NsDqT g_nsDq[T2R_MAX_PEERS];
static volatile int g_nsDqOff;
static uint32_t NsDequant(uint32_t slot, uint32_t seq, uint32_t t)
{
    if (slot >= T2R_MAX_PEERS || g_nsDqOff)
        return t;
    NsDqT *d = &g_nsDq[slot];
    if (d->n) {
        uint32_t li = (d->n - 1) % NS_DQ_N;
        int32_t ds = (int32_t) (seq - d->seq[li]);
        double dt = t - d->t[li];
        if (ds <= 0 && ds > -1000)
            return t;           // an old / repeated list: no new information
        if (ds >= 1000 || dt < 0.0 || dt > 5000.0) {
            d->n = 0;
            d->have_out = 0;
            d->q = 0;
        }
    }
    d->seq[d->n % NS_DQ_N] = seq;
    d->t[d->n % NS_DQ_N] = t;
    d->n++;
    d->st_total++;
    uint32_t m = d->n < NS_DQ_N ? d->n : NS_DQ_N;
    if (m < 12)
        return t;
    uint32_t i0 = d->n - m;
    double t0 = d->t[i0 % NS_DQ_N];
    uint32_t s0 = d->seq[i0 % NS_DQ_N];
    uint32_t on = 0;
    for (uint32_t k = 0; k < m; k++) {
        double x = (d->t[(i0 + k) % NS_DQ_N] - t0) / NS_DQ_Q, fr =
            x - floor(x + 0.5);
        if (fr < 0.0)
            fr = -fr;
        if (fr < 0.09)
            on++;
    }
    if (d->q ? on * 10 < m * 6 : on * 10 < m * 9) {
        d->q = 0;
        return t;
    }                           // hysteresis: in at 90 % on the grid, out below 60 %
    d->q = 1;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (uint32_t k = 0; k < m; k++) {
        double x =
            (double) (int32_t) (d->seq[(i0 + k) % NS_DQ_N] - s0), y =
            d->t[(i0 + k) % NS_DQ_N] - t0;
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
    }
    double den = m * sxx - sx * sx;
    if (den < 1e-6)
        return t;
    double b = (m * sxy - sx * sy) / den, a = (sy - b * sx) / m;
    if (b < 3.0 || b > 150.0)
        return t;               // not a sane frame time: leave it
    double xn = (double) (int32_t) (seq - s0);
    double out = t0 + a + b * xn + NS_DQ_Q * 0.5;
    uint32_t o = (uint32_t) (out + 0.5);
    if (d->have_out && (int32_t) (o - d->last_out) <= 0)
        o = d->last_out + 1;    // stamps stay strictly increasing
    d->last_out = o;
    d->have_out = 1;
    d->st_q++;
    return o;
}

typedef long (__stdcall * fn_nt_set_tr) (unsigned long, unsigned char,
                                         unsigned long *);
typedef long (__stdcall * fn_nt_query_tr) (unsigned long *,
                                           unsigned long *,
                                           unsigned long *);
static void NsRequestTimer(void)
{
    FILE *f = ExeSideOpen("t2r-no-timerres.txt", "r");
    if (f) {
        fclose(f);
        Xlog("timer: 1 ms request skipped (t2r-no-timerres.txt)");
        return;
    }
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    fn_nt_set_tr set =
        nt ? (fn_nt_set_tr) GetProcAddress(nt,
                                           "NtSetTimerResolution") : NULL;
    fn_nt_query_tr q =
        nt ? (fn_nt_query_tr) GetProcAddress(nt,
                                             "NtQueryTimerResolution") :
        NULL;
    unsigned long mn = 0, mx = 0, c0 = 0, c1 = 0, cur = 0;
    if (q)
        q(&mn, &mx, &c0);
    if (set)
        set(10000, 1, &cur);
    if (q)
        q(&mn, &mx, &c1);
    Xlog("timer: resolution was %.3f ms, requested 1 ms, now %.3f ms (best %.3f ms)%s", c0 / 10000.0, c1 / 10000.0, mx / 10000.0, set ? "" : " [NtSetTimerResolution missing]");
}

// ps52 the last ACCEPTED position state per slot. A downed victim whose states stopped (slot 3 rx msg5 0/s, all rejected,
// then dropped from the roster) never answers an execution START: 11 executions started and aborted on it in 20 s. ns52_stale_ms = ms since the last
// accepted state (0 = none yet / unknown).
static uint64_t g_ns52OkMs[T2R_MAX_PEERS];
static uint64_t Ns52StaleMs(uint32_t slot)
{
    if (slot >= T2R_MAX_PEERS || !g_ns52OkMs[slot])
        return 0;
    return GetTickCount64() - g_ns52OkMs[slot];
}

typedef uint64_t(*fn_ns_apply) (uint8_t *, void *, uint32_t, float);
static uint64_t NsApplyBody(uint8_t *peer, void *data, uint32_t seq,
                              float t)
{
    uint64_t r =
        ((fn_ns_apply) T2R_SYM(kFnNetSmoothApply)) (peer, data, seq, t);
    if (!peer)
        return r;
    uint32_t slot = *(uint32_t *) peer;
    if (slot >= T2R_MAX_PEERS)
        return r;
    if (!(r & 0xff)) {
        g_ns[slot].st_m5_rej++;
        return r;
    }
    g_ns52OkMs[slot] = GetTickCount64();
    NsOnState(slot, seq, (const float *) (peer + 0x68),
                (const float *) (peer + 0x74), peer + 0xc5);
    return r;
}

static uint64_t NsApplyHook(uint8_t *peer, void *data, uint32_t seq,
                              float t)
{                               // perf30b 'paths:' wrapper (cost incl. winmm's own apply)
    PCB_BEGIN(PCB_APPLY5, 0);
    uint64_t r = NsApplyBody(peer, data, seq, t);
    PCB_END(PCB_APPLY5);
    return r;
}

// ---- per-frame playout (per-peer tick, locator call 0x180035ddf)
// distinct-placement counter (a root that is written every frame but with the same value steps at the data rate)
static void NsNotePlace(NsSlot *s, const float *p, const float *q)
{
    if (s->d_ok) {
        float d =
            fabsf(p[0] - s->d_last[0]) + fabsf(p[1] - s->d_last[1]) +
            fabsf(p[2] - s->d_last[2]) + fabsf(q[0] - s->d_lastq[0]) +
            fabsf(q[1] - s->d_lastq[1]) + fabsf(q[2] - s->d_lastq[2]) +
            fabsf(q[3] - s->d_lastq[3]);
        if (d > 1e-5f)
            s->st_dist++;
    }
    memcpy(s->d_last, p, 12);
    memcpy(s->d_lastq, q, 16);
    s->d_ok = 1;
}

// the engine-side replay clock of the slot (winmm slot base 0x18013b2a0 + slot * 0x77e8): +0x74c8 clock (sender ms), +0x74d4/+0x74d8 pose stamps
static void NsLogFrozen(NsSlot *s, uint32_t slot, const uint8_t *b,
                          const uint8_t *peer, float ck)
{
    int32_t cnt = *(const volatile int32_t *) (b + 0x283c), idx =
        *(const volatile int32_t *) (b + 0x2838);
    int32_t r3[3] =
        { *(const volatile int32_t *) (b + 0x2820),
*(const volatile int32_t *) (b + 0x2828),
*(const volatile int32_t *) (b + 0x2830) };
    int32_t newest = idx >= 0 && idx < 3 ? r3[idx] : -1;
    uint32_t now = *(volatile uint32_t *) T2R_SYM(kAnimClock), at =
        *(const volatile uint32_t *) (b + 0x2840);
    Xlog("net stats: slot %u anim clock STANDS STILL for %u frames: clock %.1f, newest list time %d (ring n %d idx %d: %d %d %d), our target %.1f (valid %d), playout R %.1f, " "last msg 7 time raw %u rebased %u dequantized %u, list seq %u (last list %u ms ago), clock-init %d, pose stamps idx %d [%d %d], npc fallback %u, slot key %p, " "peer locks +4d %u +51 %u, puppet dead here %u, downed %u, ours %d, " "anim job %p (table id %d, puppet's table id %d, swapped %u dropped %u)", slot, s->ck_run, (double) ck, newest, cnt, idx, r3[0], r3[1], r3[2], (double) g_nsAnim[slot].tgt, g_nsAnim[slot].valid, s->R_ok ? s->R : -1.0, s->t_raw, s->t_rb, s->t_dq, *(const volatile uint32_t *) (b + 0x281c), at ? now - at : 0u, *(const volatile int32_t *) (b + 0x74cc), *(const volatile int32_t *) (b + 0x74d0), *(const volatile int32_t *) (b + 0x74d4), *(const volatile int32_t *) (b + 0x74d8), *(const volatile uint32_t *) (b + 0x60b4), *(void *const volatile *) b, peer ? peer[0x4d] : 0u, peer ? peer[0x51] : 0u, (uint32_t) g_puppetDeadHere[slot], (uint32_t) g_peerDowned[slot], s->ours, *(void *const volatile *) (b + AJ_SLOT_JOB), *(void *const volatile *) (b + AJ_SLOT_JOB) && !IsBadReadPtr(*(uint8_t * const volatile *) (b + AJ_SLOT_JOB) + AJ_JOB_ID, 4) ? *(const int32_t *) (*(uint8_t * const volatile *) (b + AJ_SLOT_JOB) + AJ_JOB_ID) : 0, slot < 8 ? (int) g_ajCur[slot] : 0, slot < 8 ? g_ajSwapped[slot] : 0u, slot < 8 ? g_aj_dropped[slot] : 0u);
    s->st_frozen_ep++;
}

static void NsNoteClock(NsSlot *s, uint32_t slot, double dt,
                          const uint8_t *peer)
{
    const uint8_t *b = (g_s8Pa ? g_s8Pa : (const uint8_t *) T2R_SYM(kPaSlotBase)) + (size_t) slot * 0x77e8;   // (the 8-slot winmm relocates the slots - the stats read the old table)
    if (IsBadReadPtr(b + 0x74c8, 0x14))
        return;
    float ck = *(const volatile float *) (b + 0x74c8);
    int32_t st[2] =
        { *(const volatile int32_t *) (b + 0x74d4),
*(const volatile int32_t *) (b + 0x74d8) };
    if (s->ck_ok && dt > 1.0 && dt < 100.0 && ck == ck) {
        float dc = ck - s->ck_last;
        if (dc > 100.0f || dc < -100.0f)
            s->st_ck_snap++;
        else {
            if (dc > 0.0f) {
                s->st_ck_adv++;
                s->ck_run = 0;
            } else {
                s->st_ck_still++;
                if (++s->ck_run == 45 || s->ck_run == 1500)
                    NsLogFrozen(s, slot, b, peer, ck);
            }
            double r = dc / dt;
            s->st_ck_ratio += r;
            s->st_ck_n++;
            if (r < 0.5 || r > 1.5)
                s->st_ck_irreg++;
        }
        if (st[0] != s->stp_last[0] || st[1] != s->stp_last[1])
            s->st_stp++;
    }
    s->ck_last = ck;
    s->ck_ok = 1;
    s->stp_last[0] = st[0];
    s->stp_last[1] = st[1];
}

// called by the pose writer hook (t2r-ext.c) with peer+0xc5 (80 bytes) once per frame per peer
static void NsNoteVec(uint32_t slot, const void *data,
                        const float *peerpos, const void *fin)
{
    if (slot >= T2R_MAX_PEERS || !data)
        return;
    NsSlot *s = &g_ns[slot];
    s->st_v_calls++;
    if (fin) {
        if (!s->vf_ok || memcmp(s->vf_last, fin, 80))
            s->st_vf_dist++;
        memcpy(s->vf_last, fin, 80);
        s->vf_ok = 1;
    }
    if (!s->v_ok || memcmp(s->v_last, data, 80))
        s->st_v_dist++;
    memcpy(s->v_last, data, 80);
    s->v_ok = 1;
    const float *v = (const float *) data;
    if (peerpos && NsFinite(v, 20)) {
        float dx = v[0] - peerpos[0], dy = v[1] - peerpos[1], dz =
            v[2] - peerpos[2], d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (!s->v_dn || d < s->v_dpos_min)
            s->v_dpos_min = d;
        if (!s->v_dn || d > s->v_dpos_max)
            s->v_dpos_max = d;
        s->v_dpos_sum += d;
        s->v_dn++;
        if (!s->v_samp_ok) {
            memcpy(s->v_samp, v, 80);
            memcpy(s->v_samp_p, peerpos, 12);
            s->v_samp_ok = 1;
        }
    }
}

static void NsStutter(float *last, float *lv, int *ok, const float *p,
                       double dt, uint32_t *cnt, uint32_t *moving)
{
    if (*ok && dt > 1.0 && dt < 100.0) {
        float dx = p[0] - last[0], dz = p[2] - last[2];
        float v = sqrtf(dx * dx + dz * dz) / (float) (dt * 0.001);
        if (moving && (v > 1.0f || *lv > 1.0f))
            (*moving)++;
        if ((v > 1.0f || *lv > 1.0f) && fabsf(v - *lv) > NS_STUTTER_MPS
            && v < 40.0f && *lv < 40.0f)
            (*cnt)++;
        *lv = v;
    }
    memcpy(last, p, 12);
    *ok = 1;
}

// returns a new locator (winmm's builder 0x18000a370) or NULL = keep winmm's
static void *NsLocator(uint64_t h)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    typedef float (*fn_alpha)(uint8_t *);
    if (!g_ns_inst || !h)
        return NULL;
    uint8_t *peer = NULL;
    uint32_t slot = 0;
    for (; slot < T2R_MAX_PEERS; slot++) {
        peer = ((fn_peer) T2rPeer) (slot);
        if (peer && *(uint64_t *) (peer + 0x60) == h)
            break;
    }
    if (slot >= T2R_MAX_PEERS)
        return NULL;
    double now = NsNowMs();
    NsSlot *s = &g_ns[slot];
    // winmm's own placement (for the stutter comparison)
    float a = ((fn_alpha) T2R_SYM(kFnNetSmoothAlpha)) (peer);
    if (a > 0.7f)
        a = 0.7f;
    const float *cur = (const float *) (peer + 0x68), *prv =
        (const float *) (peer + 0x517);
    float wp[3] =
        { cur[0] + a * (cur[0] - prv[0]), cur[1] + a * (cur[1] - prv[1]),
  cur[2] + a * (cur[2] - prv[2]) };
    double dt = s->f_last > 0.0 ? now - s->f_last : 1000.0;
    if (dt < 0.0)
        dt = 0.0;
    s->f_last = now;
    NsStutter(s->w_last, &s->w_v, &s->w_ok, wp, dt, &s->st_stut_w,
               &s->st_moving);
    s->st_frames++;
    NsNoteClock(s, slot, dt, peer);
    if (g_nsOff) {
        s->R_ok = 0;
        s->out_ok = 0;
        s->ours = 0;
        g_nsAnim[slot].valid = 0;
        NsNotePlace(s, wp, (const float *) (peer + 0x74));
        return NULL;
    }
    AcquireSRWLockExclusive(&g_nsLock);
    void *ret = NULL;
    if (!s->synced || s->n < 2)
        goto out;
    {
        double sfr = s->si > s->sdt ? s->si : s->sdt;   // one sample interval: the host relays a state once per host frame (never faster than that)
        double D =
            g_nsFixedDelay >
            0.0 ? g_nsFixedDelay : NS_JK * s->jpk + sfr + 2.0;
        if (g_nsFixedDelay <= 0.0 && s->loss > 0.03) {        // bursts of lost packets: keep a bracketing sample ahead of the playout time
            double l = s->loss > 0.7 ? 0.7 : s->loss, run = log(0.05) / log(l); // 95th percentile of the run of lost packets
            if (run > 6.0)
                run = 6.0;
            double lt = sfr * (run - 1.0);
            D += lt < 0.0 ? 0.0 : lt > 80.0 ? 80.0 : lt;
        }
        if (D < NS_DMIN)
            D = NS_DMIN;
        if (D > NS_DMAX && g_nsFixedDelay <= 0.0)
            D = NS_DMAX;
        if (g_nsFixedDelay <= 0.0) {
            // a stable delay (2013: a fixed receiver delay): up at once when the link needs it, down only 4 ms per second, and
            // only when the target is well below (no breathing with every jitter spike)
            if (s->dcur <= 0.0 || dt > 300.0)
                s->dcur = D;
            else if (D > s->dcur + NS_DCUR_UP_HYST)
                s->dcur = D;
            else if (D < s->dcur - NS_DCUR_DN_HYST) {
                s->dcur -= dt * NS_DCUR_DOWN;
                if (s->dcur < D)
                    s->dcur = D;
            }
            D = s->dcur;
            // prediction lead: the puppet is shown that much closer to where the player is NOW (dead reckoning covers the gap)
            if (g_nsLeadK > 0.0) {
                // the lead never eats the buffer below one sample interval + the jitter peak: 4v4 logs had the
                // delay pinned at the 24 ms floor with 30-40 ms jitter peaks = 25-45 % of frames extrapolated (the stutter)
                double floor_ms = sfr + s->jpk + 2.0;
                if (floor_ms < NS_DFLOOR)
                    floor_ms = NS_DFLOOR;
                double lead =
                    g_nsLeadK * (double) PsyncOneway((int) slot);
                if (lead > g_ns_lead_max)
                    lead = g_ns_lead_max;
                if (lead > D - floor_ms)
                    lead = D - floor_ms;
                if (lead > 0.0)
                    D -= lead;
            }
        }
        s->delay = D;
        double Rt = now + s->oref - D;
        if (!s->R_ok || dt > 300.0 || fabs(Rt - (s->R + dt)) > 300.0) { // first frame, a pause (gate / melee), or 300 ms off
            if (s->R_ok)
                s->st_resync++;
            s->R = Rt;
            s->R_ok = 1;
            s->out_ok = 0;
        } else {
            double err = Rt - (s->R + dt), rate = err / 250.0;
            if (rate > 0.08)
                rate = 0.08;
            if (rate < -0.08)
                rate = -0.08;
            s->R += dt * (1.0 + rate);
            s->fdt += (dt - s->fdt) * 0.1;
        }
        s->st_dsum += D;
        // samples around R
        uint32_t cnt = s->n < NS_RING ? s->n : NS_RING;
        NsSamp *nw = NULL, *A = NULL, *B = NULL, *old = NULL;
        uint32_t nwi = 0;
        for (uint32_t k = 0; k < cnt; k++) {
            NsSamp *x = &s->s[(s->n - 1 - k) % NS_RING];
            if (!x->t_ok)
                continue;
            if (!nw) {
                nw = x;
                nwi = s->n - 1 - k;
            }
            old = x;
            if (x->t <= s->R) {
                A = x;
                break;
            }
            B = x;
        }
        if (!nw)
            goto out;
        float P[3], Q[4];
        int ex = 0;
        if (A && B) {
            if (B->tele) {
                memcpy(P, A->p, 12);
                memcpy(Q, A->q, 16);
            } else {
                float f = (float) ((s->R - A->t) / (B->t - A->t));
                if (f < 0.0f)
                    f = 0.0f;
                if (f > 1.0f)
                    f = 1.0f;
                for (int i = 0; i < 3; i++)
                    P[i] = A->p[i] + (B->p[i] - A->p[i]) * f;
                NsSlerp(Q, A->q, B->q, f);
            }
        } else if (A) {         // past the newest: extrapolate, capped
            double ahead = s->R - nw->t;
            ex = 1;
            s->st_extrap++;
            if (ahead > NS_EXTRAP_MS) {
                ahead = NS_EXTRAP_MS;
                s->st_hold++;
            }
            float v[3];
            NsSamp *ref = NsVel(s, nwi, v);   // velocity over >= 60 ms, never across a teleport
            // the speed eases to 0 at the extrapolation limit (a smooth stop instead of a sudden hold)
            float ea =
                (float) (ahead - ahead * ahead / (2.0 * NS_EXTRAP_MS));
            for (int i = 0; i < 3; i++)
                P[i] = nw->p[i] + v[i] * ea;
            memcpy(Q, nw->q, 16);
            if (ref) {          // keep turning at the last angular speed (max 0.7 rad extra)
                float d = NsDot4(ref->q, nw->q);
                float ang =
                    2.0f * acosf(d < 0.0f ? -d : d > 1.0f ? 1.0f : d);
                double rdt = nw->t - ref->t;
                float fx = 1.0f + (float) (ea / rdt);
                if (ang > 1e-4f && (fx - 1.0f) * ang > 0.7f)
                    fx = 1.0f + 0.7f / ang;
                if (ang > 1e-4f && fx > 1.0f)
                    NsSlerp(Q, ref->q, nw->q, fx);
            }
        } else {                // before the oldest kept sample
            memcpy(P, old->p, 12);
            memcpy(Q, old->q, 16);
        }
        // smooth error correction instead of snaps
        double odt = s->out_ok ? now - s->out_last : 0.0;
        if (s->out_ok && odt > 0.0 && odt < 300.0) {
            float dec = (float) exp(-odt / NS_TAU_MS);
            for (int i = 0; i < 3; i++)
                s->E[i] *= dec;
            s->hold_b *= dec;
            if (s->extrap && !ex) {     // samples came back: keep the motion continuous
                float e[3], el = 0.0f;
                for (int i = 0; i < 3; i++) {
                    e[i] = s->out_p[i] + s->vel[i] * (float) odt - P[i];
                    el += e[i] * e[i];
                }
                el = sqrtf(el);
                if (el > NS_SNAP_M) {
                    memset(s->E, 0, 12);
                    s->hold_b = 0.0f;
                    s->st_snap++;
                } else {
                    memcpy(s->E, e, 12);
                    memcpy(s->hold_q, s->out_q, 16);
                    s->hold_b = 1.0f;
                    s->st_corr++;
                    if (el > s->st_corr_max)
                        s->st_corr_max = el;
                }
            }
            float o[3], jump = 0.0f;
            for (int i = 0; i < 3; i++) {
                o[i] = P[i] + s->E[i];
                jump += (o[i] - s->out_p[i]) * (o[i] - s->out_p[i]);
            }
            if (jump > NS_TELEPORT_M * NS_TELEPORT_M) {
                memset(s->E, 0, 12);
                s->hold_b = 0.0f;
                s->st_snap++;
            }
        } else {
            memset(s->E, 0, 12);
            s->hold_b = 0.0f;
            odt = 0.0;
        }
        float O[3], OQ[4];
        for (int i = 0; i < 3; i++)
            O[i] = P[i] + s->E[i];
        if (s->hold_b > 0.01f)
            NsSlerp(OQ, Q, s->hold_q, s->hold_b);
        else
            memcpy(OQ, Q, 16);
        if (odt > 1.0)
            for (int i = 0; i < 3; i++)
                s->vel[i] = (O[i] - s->out_p[i]) / (float) odt;
        else
            memset(s->vel, 0, 12);
        if (ex) {               // the velocity used while extrapolating stays capped
            float sp =
                sqrtf(s->vel[0] * s->vel[0] + s->vel[1] * s->vel[1] +
                      s->vel[2] * s->vel[2]);
            if (sp > NS_MAX_SPEED)
                for (int i = 0; i < 3; i++)
                    s->vel[i] *= NS_MAX_SPEED / sp;
        }
        memcpy(s->out_p, O, 12);
        memcpy(s->out_q, OQ, 16);
        s->out_ok = 1;
        s->out_last = now;
        s->extrap = ex;
        NsStutter(s->o_last, &s->o_v, &s->o_ok, O, dt, &s->st_stut_o,
                   NULL);
        float lag =
            sqrtf((O[0] - wp[0]) * (O[0] - wp[0]) +
                  (O[2] - wp[2]) * (O[2] - wp[2]));
        s->st_lagsum += lag;
        s->st_lagn++;
        // anim replay clock target: same timeline (+ one frame: winmm's clock trails its target by a frame)
        if (g_ns_rebase) {
            NsAnimT at = { (float) (s->R + s->fdt), 1 };
            g_nsAnim[slot] = at;
        }
        if (NsFinite(O, 3) && NsFinite(OQ, 4)) {
            // hand over from winmm's own placement (used until the buffer is primed / after a gap) instead of switching in one frame
            float FO[3] = { O[0], O[1], O[2] }, FQ[4] =
                { OQ[0], OQ[1], OQ[2], OQ[3] };
            if (!s->ours)
                s->xb = 1.0f;
            if (s->xb > 0.0f && NsFinite(wp, 3)) {
                float xb = s->xb > 1.0f ? 1.0f : s->xb, wq[4];
                memcpy(wq, peer + 0x74, 16);
                for (int i = 0; i < 3; i++)
                    FO[i] = O[i] + (wp[i] - O[i]) * xb;
                if (NsFinite(wq, 4)) {
                    NsNorm4(wq);
                    NsSlerp(FQ, OQ, wq, xb);
                }
                s->xb -= (float) (dt / NS_BLEND_MS);
                if (s->xb < 0.0f)
                    s->xb = 0.0f;
            }
            NsNotePlace(s, FO, FQ);
            ret =
                ((void *(*)(float *, float *))
                 T2R_SYM(kFnXformCompose))(FO, FQ);
        }
    }
  out:
    s->ours = ret != NULL;
    ReleaseSRWLockExclusive(&g_nsLock);
    if (!ret)
        g_nsAnim[slot].valid = 0;
    return ret;
}

// The five root vec4 (msg 5 +0xc5, written into the replay's root buffer by the pose writer once per rendered frame) are 80 bytes that only change
// when a msg 5 arrives (50/s at the host's frame rate, whatever the viewer's fps: "root vec4 distinct 49/s" at 81-135 fps in test logs) while the
// locator and the anim replay run on the playout clock R: the root joints stepped at the packet rate against a smooth body, and they also led it by the
// whole playout delay. Here each vec4 is blended between the two samples around R (same timeline as the locator). Unit-length vec4 are renormalised
// (all 1010 vec4 in the bug-report 'vec4 sample' lines have |v| 0.999..1.001, w in +-[0.79, 1]: unit quaternions); a pair in opposite hemispheres (the
// same rotation with the other sign, 38 % of the samples have w < 0) is blended towards -b and gets the sign of the nearer sample (it used to jump
// from the one sample to the other at the midpoint); a pair that is not finite is not blended (the nearer sample is used); anything
// that cannot be placed between two samples keeps winmm's value (the newest). t2r-no-net-smooth.txt turns it off with the rest of the buffer.
static int NsVecInterp(uint32_t slot, float *out)
{
    if (!g_ns_inst || g_nsOff || slot >= T2R_MAX_PEERS)
        return 0;
    NsSlot *s = &g_ns[slot];
    double now = NsNowMs();
    int ok = 0;
    AcquireSRWLockExclusive(&g_nsLock);
    s->st_vf_calls++;
    if (s->synced && s->R_ok && s->ours && s->n >= 2
        && now - s->f_last < 60.0) {
        uint32_t cnt = s->n < NS_RING ? s->n : NS_RING;
        NsSamp *A = NULL, *B = NULL;
        for (uint32_t k = 0; k < cnt; k++) {
            NsSamp *x = &s->s[(s->n - 1 - k) % NS_RING];
            if (!x->t_ok || !x->v_ok)
                continue;
            if (x->t <= s->R) {
                A = x;
                break;
            }
            B = x;
        }
        if (A && B && B->t > A->t) {
            float f = (float) ((s->R - A->t) / (B->t - A->t));
            if (f < 0.0f)
                f = 0.0f;
            if (f > 1.0f)
                f = 1.0f;
            if (B->tele)
                memcpy(out, A->v, 80);
            else
                for (int i = 0; i < 5; i++) {
                    const float *a = A->v + 4 * i, *b = B->v + 4 * i;
                    float *o = out + 4 * i;
                    float la = sqrtf(NsDot4(a, a)), lb =
                        sqrtf(NsDot4(b, b));
                    int unit = la > 0.98f && la < 1.02f && lb > 0.98f
                        && lb < 1.02f;
                    if (NsDot4(a, b) < 0.0f && unit) { // q and -q are one rotation: blend towards -b (the short way), give the sign of the nearer sample
                        for (int c = 0; c < 4; c++)
                            o[c] = a[c] + (-b[c] - a[c]) * f;
                        NsNorm4(o);
                        if (f >= 0.5f)
                            for (int c = 0; c < 4; c++)
                                o[c] = -o[c];
                        continue;
                    }
                    for (int c = 0; c < 4; c++)
                        o[c] = a[c] + (b[c] - a[c]) * f;
                    if (unit)
                        NsNorm4(o);
                }
            ok = 1;
            s->st_vf_used++;
        }
    }
    ReleaseSRWLockExclusive(&g_nsLock);
    return ok;
}

// last msg 7 time as it went through the three stages (raw sender timeGetTime, per-slot rebase, de-quantisation): shown when a slot's anim clock stands still
static void NsNoteT(uint32_t slot, uint32_t raw, uint32_t rb,
                      uint32_t dq)
{
    if (slot >= T2R_MAX_PEERS)
        return;
    g_ns[slot].t_raw = raw;
    g_ns[slot].t_rb = rb;
    g_ns[slot].t_dq = dq;
}

// ---- send side
// counts every datagram winmm puts on the wire (0x18005a9f0: conn, type, ?, seq, payload, len)
typedef uint64_t(*fn_ns_lowsend) (void *, uint64_t, uint64_t, uint64_t,
                                  void *, uint64_t);
static fn_ns_lowsend g_nsLowsendTramp;
// Host relay dedup (4v4 stutter): winmm's SendPhase (spec net-session.cpp) re-sends EVERY other player's last state to
// every peer in EVERY host frame, new or not (receivers reject the old seq: the "rejected" count in net stats). With the 30 Hz send
// cap that is half of the relayed states, and the host's upload grows with players squared (4v4: ~2.8 MB/s at 60 Hz, ~400 KB/s per
// Steam connection = queueing / loss / jitter for everyone). A relayed state (type 5) whose (peer, sender slot, seq) was already sent
// is dropped; one copy every 250 ms still goes out as a keepalive. Our own state always carries a new seq, so it is never dropped.
// t2r-no-relay-dedup.txt = off (read with the other switches).
static volatile int g_nsDedupOff;
static volatile uint32_t g_nsDedupDrop;
typedef struct {
    void *peer;
    uint32_t seq[16];
    double at[16];
    uint8_t ok[16];
} NsDdT;
static NsDdT g_nsDd[16];
// distance between two other players from their newest NET positions (peer record +0x68), cached 100 ms per slot; < 0 = unknown (pair is sent)
static float NsHbwDist(uint32_t a, uint32_t b)
{
    static float pos[T2R_MAX_PEERS][3];
    static double at[T2R_MAX_PEERS];
    static uint8_t ok[T2R_MAX_PEERS];
    double now = NsNowMs();
    uint32_t k[2] = { a, b };
    for (int i = 0; i < 2; i++) {
        uint32_t s = k[i];
        if (s >= T2R_MAX_PEERS)
            return -1.0f;
        if (now - at[s] < 100.0 && at[s] > 0.0)
            continue;
        uint8_t *p = T2rPeer(s);
        ok[s] = 0;
        at[s] = now;
        if (p && !IsBadReadPtr(p + 0x68, 12)) {
            const float *np = (const float *) (p + 0x68);
            if (np[0] == np[0] && np[1] == np[1] && np[2] == np[2]
                && __builtin_fabsf(np[0]) < 1e5f
                && __builtin_fabsf(np[1]) < 1e5f
                && __builtin_fabsf(np[2]) < 1e5f) {
                memcpy(pos[s], np, 12);
                ok[s] = 1;
            }
        }
    }
    if (!ok[a] || !ok[b])
        return -1.0f;
    float dx = pos[a][0] - pos[b][0], dy = pos[a][1] - pos[b][1], dz =
        pos[a][2] - pos[b][2];
    return __builtin_sqrtf(dx * dx + dy * dy + dz * dz);
}

static uint64_t NsLowsendBody(void *c, uint64_t type, uint64_t a3,
                                uint64_t seq, void *buf, uint64_t len)
{
    uint8_t t = (uint8_t) type, src = (uint8_t) a3;
    if (t == 5 && c && src < 16 && !g_nsDedupOff) {
        NsDdT *d = NULL;
        for (int i = 0; i < 16; i++)
            if (g_nsDd[i].peer == c) {
                d = &g_nsDd[i];
                break;
            }
        if (!d)
            for (int i = 0; i < 16; i++)
                if (!g_nsDd[i].peer) {
                    d = &g_nsDd[i];
                    memset(d, 0, sizeof *d);
                    d->peer = c;
                    break;
                }
        if (d) {
            double now = NsNowMs();
            if (d->ok[src] && d->seq[src] == (uint32_t) seq
                && now - d->at[src] < 250.0) {
                g_nsDedupDrop++;
                return 0;
            }
            d->seq[src] = (uint32_t) seq;
            d->at[src] = now;
            d->ok[src] = 1;
        }
    }
    if (t == 7 && g_hbw.on && c && src < T2R_MAX_PEERS
        && !IsBadReadPtr(c, 2) && ((uint8_t *) c)[0] == 1
        && ((uint8_t *) c)[1] < T2R_MAX_PEERS && src != ((uint8_t *) c)[1]
        && src != *(uint32_t *) T2R_SYM(kMySlot)) {     // host only: a RELAYED list (origin slot != the host's own, != the receiver)
        float dist = NsHbwDist(src, ((uint8_t *) c)[1]);
        if (!HbwMsg7
            (&g_hbw, NsNowMs(), src, ((uint8_t *) c)[1], dist,
             (uint32_t) len + 12, g_nsTxTotal))
            return 0;
    }
    if (t == 7 && c && !g_nsM7LodOff && !IsBadReadPtr(c, 2) && ((uint8_t *) c)[0] == 1 && ((uint8_t *) c)[1] < T2R_MAX_PEERS) {      // NetPeer: active, slot (spec net-session.cpp)
        uint8_t ds = ((uint8_t *) c)[1];
        if (g_nsM7Half[ds]) {
            g_ns_m7_half_n++;
            if ((g_nsM7Tog[ds] ^= 1) == 0) {
                g_ns_m7_skip_n++;
                return 0;
            }                   // every 2nd list to this far peer is not sent
        } else
            g_ns_m7_full_n++;
    }
    g_nsTxN[t]++;
    g_ns_tx_b[t] += (uint32_t) len + 12;
    g_nsTxTotal += (uint64_t) len + 12;
    return g_nsLowsendTramp(c, type, a3, seq, buf, len);
}

static uint64_t NsLowsendHook(void *c, uint64_t type, uint64_t a3,
                                uint64_t seq, void *buf, uint64_t len)
{                               // perf30b 'paths:' wrapper: 1 call in 8 timed (up to ~100 per host frame)
    PCB_BEGIN(PCB_LOWSEND, 7);
    uint64_t r = NsLowsendBody(c, type, a3, seq, buf, len);
    PCB_END(PCB_LOWSEND);
    return r;
}

static TxsT g_txs;             // txstat.h: every message that reaches winmm's send (winmm's own events + all ext ids), counted in send_gate
static void NsCountExt(uint64_t type, uint64_t len, uint64_t dst)
{
    TxsNote(&g_txs, type, len, dst);
}

// send cap: called from the stub at 0x18005d09b in frames that would send msg 5 (+7); 0 = skip this frame
static int NsSendGate(void)
{
    int hz = g_nsCapHz;
    if (hz <= 0) {
        g_ns_cap_ok++;
        return 1;
    }
    double now = NsNowMs(), iv = 1000.0 / hz;
    if (now >= g_nsNextSend - 1.5) {
        g_nsNextSend =
            now - g_nsNextSend > iv ? now + iv : g_nsNextSend + iv;
        g_ns_cap_ok++;
        return 1;
    }
    g_ns_cap_skip++;
    return 0;
}

static void NsStubDone(uint8_t *stub, size_t n)
{
    DWORD old;
    VirtualProtect(stub, n, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), stub, n);
}

static void NsPatch(uint8_t *at, const uint8_t *bytes, size_t n)
{
    DWORD old;
    VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
}

#define NS_EMIT(...) do { const uint8_t b_[] = {__VA_ARGS__}; memcpy(st + o, b_, sizeof b_); o += sizeof b_; } while (0)
#define NS_EMIT64(v) do { uint64_t v_ = (uint64_t)(v); memcpy(st + o, &v_, 8); o += 8; } while (0)
#define NS_EMIT32(v) do { uint32_t v_ = (uint32_t)(v); memcpy(st + o, &v_, 4); o += 4; } while (0)
static void NsCallTo(uint8_t *c, uint8_t *st)
{
    int32_t rel = (int32_t) (st - (c + 5));
    uint8_t call[5] = { 0xE8 };
    memcpy(call + 1, &rel, 4);
    NsPatch(c, call, 5);
}

// msg 7 receive call site 0x18005bf6c -> stub: mov r11, &g_ns_m7seq; mov [r11], r14d (the header seq);
// mov rax, hook; jmp rax
static int NsRedirectRecv7(void *hook)
{
    uint8_t *c = (uint8_t *) T2R_CODE(kCodeNs5BF6C);
    if (c[0] != 0xE8
        || c + 5 + *(int32_t *) (c + 1) !=
        (uint8_t *) T2R_CODE(kCodePaRecv))
        return 0;
    uint8_t *st = AllocNear(c, 64);
    size_t o = 0;
    if (!st)
        return 0;
    NS_EMIT(0x49, 0xBB);
    NS_EMIT64(&g_nsM7seq);     // mov r11, imm64
    NS_EMIT(0x45, 0x89, 0x33);  // mov [r11], r14d
    NS_EMIT(0x48, 0xB8);
    NS_EMIT64(hook);            // mov rax, hook
    NS_EMIT(0xFF, 0xE0);        // jmp rax
    NsStubDone(st, 64);
    NsCallTo(c, st);
    return 1;
}

static int NsRedirectCall(uint64_t site, uint64_t target, void *hook)
{
    uint8_t *c = (uint8_t *) T2R_LEGACY_VA(site);
    if (c[0] != 0xE8
        || c + 5 + *(int32_t *) (c + 1) !=
        (uint8_t *) T2R_LEGACY_VA(target))
        return 0;
    uint8_t *st = AllocNear(c, 32);
    size_t o = 0;
    if (!st)
        return 0;
    NS_EMIT(0x48, 0xB8);
    NS_EMIT64(hook);
    NS_EMIT(0xFF, 0xE0);
    NsStubDone(st, 32);
    NsCallTo(c, st);
    return 1;
}

// anim clock target (0x180027e10: movss xmm4,[-25.0]; addss xmm4,xmm1 = newest list time - 25 ms) ->
// call stub: the slot (rsi = slot * 0x77e8 there) has a valid target -> xmm4 = our playout time, else
// winmm's. Only rax and flags are touched (both dead at that point).
static int NsPatchAnimClock(void)
{
    uint8_t *c = (uint8_t *) T2R_CODE(kCodeNs27E10);
    static const uint8_t expect[12] =
        { 0xF3, 0x0F, 0x10, 0x25, 0xEC, 0xA0, 0x07, 0x00, 0xF3, 0x0F, 0x58,
0xE1 };
    if (memcmp(c, expect, 12))
        return 0;
    uint8_t *st = AllocNear(c, 192);
    size_t o = 0, j[T2R_MAX_PEERS - 1], jo;
    if (!st)
        return 0;
    NS_EMIT(0x48, 0xB8);
    NS_EMIT64(&g_nsAnim[0]);   // mov rax, &g_ns_anim[0]
    for (int k = 1; k < T2R_MAX_PEERS; k++) {   // (was k <= 3 from the 4-slot winmm - slots 4..7 all ran on slot 3's clock)
        NS_EMIT(0x48, 0x81, 0xFE);
        NS_EMIT32(k * 0x77e8);  // cmp rsi, k*0x77e8
        NS_EMIT(0x72, 0x00);
        j[k - 1] = o - 1;       // jb L
        NS_EMIT(0x48, 0x83, 0xC0, 0x08);        // add rax, 8
    }
    size_t L = o;
    for (int k = 0; k < T2R_MAX_PEERS - 1; k++)
        st[j[k]] = (uint8_t) (L - (j[k] + 1));
    NS_EMIT(0x83, 0x78, 0x04, 0x00);    // cmp dword [rax+4], 0
    NS_EMIT(0x74, 0x00);
    jo = o - 1;                 // je orig
    NS_EMIT(0xF3, 0x0F, 0x10, 0x20);    // movss xmm4, [rax]
    NS_EMIT(0xC3);              // ret
    st[jo] = (uint8_t) (o - (jo + 1));
    NS_EMIT(0x48, 0xB8);
    NS_EMIT64(T2R_SYM(kConstNeg25));    // mov rax, &-25.0
    NS_EMIT(0xF3, 0x0F, 0x10, 0x20);    // movss xmm4, [rax]
    NS_EMIT(0xF3, 0x0F, 0x58, 0xE1);    // addss xmm4, xmm1
    NS_EMIT(0xC3);
    NsStubDone(st, 192);
    uint8_t p[12] = { 0xE8, 0, 0, 0, 0, 0x0F, 0x1F, 0x80, 0, 0, 0, 0 }; // call stub; 7-byte nop
    int32_t rel = (int32_t) (st - (c + 5));
    memcpy(p + 1, &rel, 4);
    NsPatch(c, p, 12);
    return 1;
}

// send decision (0x18005d09b: inc dword [seq]; mov r15b,1) -> jmp stub: ns_send_gate() ? (inc, r15b=1, back
// to 0x18005d0a4) : 0x18005d0de (xor r15d,r15d: this frame sends no msg 5 / 7). rsp is 16-aligned there;
// no volatile register is live across the site.
static int NsPatchSendGate(void)
{
    uint8_t *c = (uint8_t *) T2R_CODE(kCodeNs5D09B);
    static const uint8_t expect[9] =
        { 0xFF, 0x05, 0xA3, 0xAE, 0x11, 0x00, 0x41, 0xB7, 0x01 };
    uint8_t *deny = (uint8_t *) T2R_CODE(kCodeNsDeny);
    if (memcmp(c, expect, 9) || deny[0] != 0x45 || deny[1] != 0x31
        || deny[2] != 0xFF)
        return 0;
    uint8_t *st = AllocNear(c, 128);
    size_t o = 0, jd;
    if (!st)
        return 0;
    NS_EMIT(0x48, 0x83, 0xEC, 0x20);    // sub rsp, 0x20
    NS_EMIT(0x48, 0xB8);
    NS_EMIT64(NsSendGate);    // mov rax, gate
    NS_EMIT(0xFF, 0xD0);        // call rax
    NS_EMIT(0x48, 0x83, 0xC4, 0x20);    // add rsp, 0x20
    NS_EMIT(0x84, 0xC0);        // test al, al
    NS_EMIT(0x74, 0x00);
    jd = o - 1;                 // je deny
    NS_EMIT(0x48, 0xB9);
    NS_EMIT64(T2R_SYM(kSeqVar));        // mov rcx, &seq
    NS_EMIT(0xFF, 0x01);        // inc dword [rcx]
    NS_EMIT(0x41, 0xB7, 0x01);  // mov r15b, 1
    NS_EMIT(0xFF, 0x25, 0, 0, 0, 0);
    NS_EMIT64(T2R_CODE(kCodeNs5D0A4));
    st[jd] = (uint8_t) (o - (jd + 1));
    NS_EMIT(0xFF, 0x25, 0, 0, 0, 0);
    NS_EMIT64(deny);
    NsStubDone(st, 128);
    uint8_t p[9] = { 0xE9, 0, 0, 0, 0, 0x0F, 0x1F, 0x40, 0x00 };        // jmp stub; 4-byte nop
    int32_t rel = (int32_t) (st - (c + 5));
    memcpy(p + 1, &rel, 4);
    NsPatch(c, p, 9);
    return 1;
}

#ifndef T2R_RELEASE
// ---------------------------------------------------------------- DEV latency / loss simulator (compiled out of releases)
// Every datagram the game receives (winmm's recv FUN_1800603e0(buf, size, from-out) -> length / -1; hello, acks, state, events,
// ours) goes through a delay queue with random loss, so an A/B test on one PC behaves like the online games (rtt 130-220 ms,
// up to ~36 % loss to one peer). Config: t2r-netsim.txt next to the exe (re-read every 5 s; deleted / empty = off), or the
// environment variable T2R_NETSIM (used when the file is absent):
//     <delay ms> [<jitter ms> [<loss %> [<mean loss burst, packets>]]]        e.g.   110 40 20 3
// Applies to THIS client's incoming packets only (one-way): rtt between two simulated clients = 2 x delay. Only in a match
// (lobby / handshake traffic passes untouched). Delay = delay + uniform(0, jitter) per packet (so packets reorder, like the
// internet; winmm drops older seqs); loss = Gilbert-Elliott with the given mean loss % and mean burst length.
// "netsim:" lines in t2r-ext.log: config changes and, every 10 s, delivered / dropped / queued / max time in the queue.
#define NSIM_Q 768
#define NSIM_MAXLEN 0x1040
typedef struct {
    double due, enq;
    int32_t len;
    uint8_t used, addr[16];
    uint8_t data[NSIM_MAXLEN];
} NsimPkt;
static NsimPkt *g_nsimQ;
static SRWLOCK g_nsimLock = SRWLOCK_INIT;
static volatile double g_nsim_delay, g_nsim_jit, g_nsim_loss,
    g_nsim_burst = 1.0;
static uint32_t g_nsimRng =
    0x9e3779b9u, g_nsim_bad, g_nsim_queued, g_nsim_seed;
static uint32_t g_nsim_pass, g_nsim_drop, g_nsim_overflow;
static double g_nsimMaxage;
typedef int32_t(*fn_nsim_recv) (void *, int32_t, void *);
static fn_nsim_recv g_nsimTramp;
static double NsimRand(void)
{
    g_nsimRng ^= g_nsimRng << 13;
    g_nsimRng ^= g_nsimRng >> 17;
    g_nsimRng ^= g_nsimRng << 5;
    return (g_nsimRng >> 8) / 16777216.0;
}

static int32_t NsimRecvHook(void *buf, int32_t size, void *from)
{
    int on = g_nsim_delay > 0.0 || g_nsim_jit > 0.0 || g_nsim_loss > 0.0;
    if (!g_nsimQ || (!on && !g_nsim_queued))
        return g_nsimTramp(buf, size, from);
    int live = on && MatchSubstate() == 6;
    double now = NsNowMs();
    AcquireSRWLockExclusive(&g_nsimLock);
    for (int n = 0; n < 128; n++) {     // everything waiting in the socket goes into the queue
        int slot = -1;
        for (int i = 0; i < NSIM_Q; i++)
            if (!g_nsimQ[i].used) {
                slot = i;
                break;
            }
        if (slot < 0) {
            g_nsim_overflow++;
            break;
        }                       // queue full: the rest stays in the socket
        NsimPkt *q = &g_nsimQ[slot];
        memset(q->addr, 0, 16);
        int32_t r =
            g_nsimTramp(q->data, size < NSIM_MAXLEN ? size : NSIM_MAXLEN,
                         q->addr);
        if (r <= 0)
            break;
        if (!live)
            q->due = 0.0;
        else {
            double L = g_nsim_loss / 100.0, B =
                g_nsim_burst < 1.0 ? 1.0 : g_nsim_burst;
            int lose = 0;
            if (L > 0.0) {      // Gilbert-Elliott: mean burst B, long-run loss L
                double p_bb = 1.0 - 1.0 / B, p_gb =
                    L >= 0.99 ? 1.0 : (1.0 / B) * L / (1.0 - L);
                g_nsim_bad = NsimRand() < (g_nsim_bad ? p_bb : p_gb);
                lose = (int) g_nsim_bad;
            }
            if (lose) {
                g_nsim_drop++;
                continue;
            }
            q->due = now + g_nsim_delay + NsimRand() * g_nsim_jit;
        }
        q->enq = now;
        q->len = r;
        q->used = 1;
        g_nsim_queued++;
    }
    int best = -1;              // the earliest packet that is due
    for (int i = 0; i < NSIM_Q; i++)
        if (g_nsimQ[i].used && g_nsimQ[i].due <= now
            && (best < 0 || g_nsimQ[i].due < g_nsimQ[best].due
                || (g_nsimQ[i].due == g_nsimQ[best].due
                    && g_nsimQ[i].enq < g_nsimQ[best].enq)))
            best = i;
    int32_t ret = -1;
    if (best >= 0) {
        NsimPkt *q = &g_nsimQ[best];
        int32_t n = q->len < size ? q->len : size;
        memcpy(buf, q->data, (size_t) n);
        if (from)
            memcpy(from, q->addr, 16);
        ret = n;
        if (now - q->enq > g_nsimMaxage)
            g_nsimMaxage = now - q->enq;
        q->used = 0;
        g_nsim_queued--;
        g_nsim_pass++;
    }
    ReleaseSRWLockExclusive(&g_nsimLock);
    return ret;
}

// Test profiles (TUNING labels, not measurements of one player: the range ping 40-120 ms, jitter 10-30 ms, loss 0-2 %). A client delays only what it RECEIVES,
// so with the same profile in both A's and B's file the round trip is 2 x delay (+ jitter): ping40 ~ 2 x 20, ping80 ~ 2 x 40, ping120 ~ 2 x 60. Jitter is uniform 0..J (mean J/2).
// The simulator sits BELOW winmm's transport (it delays / drops the datagrams the game's recv reads), so a dropped reliable packet is resent by winmm's own ack/resend,
// exactly as on the internet; nothing is dropped above that layer.
static const struct {
    const char *name;
    double d, j, l, b;
} NSIM_PROF[] = {
    { "lan", 0, 0, 0, 1 }, { "ping40", 20, 10, 0, 1 }, { "ping80", 40, 20,
                                                        0.5, 1.5 },
        { "ping120", 60, 30, 2, 3 }, { "ping120spiky", 60, 60, 2, 4 },
};

static char g_nsimProf[24];
static void NsimReadConfig(void)
{
    double d = 0, j = 0, l = 0, b = 1;
    char line[160] = "", prof[24] = "";
    unsigned seed = 0;
    FILE *f = ExeSideOpen("t2r-netsim.txt", "r");
    const char *src = "t2r-netsim.txt";
    if (f) {
        if (!fgets(line, sizeof line, f))
            line[0] = 0;
        fclose(f);
    } else {
        const char *e = getenv("T2R_NETSIM");
        if (e) {
            snprintf(line, sizeof line, "%s", e);
            src = "T2R_NETSIM";
        }
    }
    for (char *c = line; *c; c++)
        if (*c == ',' || *c == ';' || *c == '\r' || *c == '\n')
            *c = ' ';
    // line = "[profile] [delay_ms jitter_ms loss_pct burst] [seed N]": a profile name first (fills the four numbers), numbers after it override
    char *p = line;
    while (*p == ' ')
        p++;
    if ((*p >= 'a' && *p <= 'z') && strncmp(p, "seed", 4)) {
        int k = 0;
        while (p[k] && p[k] != ' ' && k < 23) {
            prof[k] = p[k];
            k++;
        }
        prof[k] = 0;
        p += k;
        int hit = 0;
        for (size_t i = 0; i < sizeof NSIM_PROF / sizeof *NSIM_PROF; i++)
            if (!strcmp(prof, NSIM_PROF[i].name)) {
                d = NSIM_PROF[i].d;
                j = NSIM_PROF[i].j;
                l = NSIM_PROF[i].l;
                b = NSIM_PROF[i].b;
                hit = 1;
            }
        if (!hit) {
            Xlog("netsim: unknown profile '%s' (lan ping40 ping80 ping120 ping120spiky) - off", prof);
            prof[0] = 0;
        }
    }
    char *sd = strstr(p, "seed");
    if (sd) {
        seed = (unsigned) strtoul(sd + 4, NULL, 10);
        *sd = 0;
    }
    double v[4];
    int n = sscanf(p, "%lf %lf %lf %lf", &v[0], &v[1], &v[2], &v[3]);
    if (n >= 1)
        d = v[0];
    if (n >= 2)
        j = v[1];
    if (n >= 3)
        l = v[2];
    if (n >= 4)
        b = v[3];
    if (n < 1 && !prof[0]) {
        d = j = l = 0;
        b = 1;
    }
    if (d < 0)
        d = 0;
    if (j < 0)
        j = 0;
    if (l < 0)
        l = 0;
    if (l > 95)
        l = 95;
    if (b < 1)
        b = 1;
    if (seed && seed != g_nsim_seed) {
        g_nsim_seed = seed;
        g_nsimRng = seed * 2654435761u + 0x9e3779b9u;
        if (!g_nsimRng)
            g_nsimRng = 1;
        Xlog("netsim: random seed %u", seed);
    }
    if (d != g_nsim_delay || j != g_nsim_jit || l != g_nsim_loss
        || b != g_nsim_burst || strcmp(prof, g_nsimProf)) {
        if (d > 0 || j > 0 || l > 0) {
            if (prof[0])
                Xlog("netsim: ON (%s) - incoming packets delayed %.0f ms + 0..%.0f ms jitter, %.1f%% loss, mean burst %.1f profile '%s' (one-way; in a match only)", src, d, j, l, b, prof);
            else
                Xlog("netsim: ON (%s) - incoming packets delayed %.0f ms + 0..%.0f ms jitter, %.1f%% loss, mean burst %.1f (one-way; in a match only)", src, d, j, l, b);
        } else
            Xlog("netsim: off");
    }
    snprintf(g_nsimProf, sizeof g_nsimProf, "%s", prof);
    g_nsim_delay = d;
    g_nsim_jit = j;
    g_nsim_loss = l;
    g_nsim_burst = b;
}

static void NsimLog10s(void)
{
    if (g_nsim_delay > 0 || g_nsim_jit > 0 || g_nsim_loss > 0
        || g_nsim_pass)
        Xlog("netsim: 10 s: %u delivered, %u dropped (%.1f%%), %u queued now, max time in queue %.0f ms%s", g_nsim_pass, g_nsim_drop, g_nsim_pass + g_nsim_drop ? 100.0 * g_nsim_drop / (g_nsim_pass + g_nsim_drop) : 0.0, g_nsim_queued, g_nsimMaxage, g_nsim_overflow ? " (QUEUE FULL seen)" : "");
    g_nsim_pass = g_nsim_drop = g_nsim_overflow = 0;
    g_nsimMaxage = 0;
}

static void NsimInstall(void)
{
    static const uint8_t pro[17] =
        { 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x56, 0x57, 0x55,
0x53, 0xB8, 0x88, 0x10, 0x00, 0x00 };
    uint8_t *f = (uint8_t *) T2R_CODE(kCodeNs603E0);
    if (memcmp(f, pro, 17)) {
        HOOK_MISS(f, 17,
                  "netsim: recv prologue mismatch - simulator not installed");
        return;
    }
    g_nsimQ =
        VirtualAlloc(NULL, sizeof(NsimPkt) * NSIM_Q,
                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    uint8_t *t =
        VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE,
                     PAGE_EXECUTE_READWRITE);
    if (!g_nsimQ || !t) {
        g_nsimQ = NULL;
        return;
    }
    memcpy(t, f, 17);
    t[17] = 0xFF;
    t[18] = 0x25;
    *(uint32_t *) (t + 19) = 0;
    *(uint64_t *) (t + 23) = (uint64_t) (f + 17);
    g_nsimTramp = (fn_nsim_recv) t;
    uint8_t p[17] =
        { 0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x90, 0x90,
0x90 };
    uint64_t hk = (uint64_t) NsimRecvHook;
    memcpy(p + 6, &hk, 8);
    NsPatch(f, p, 17);
    NsimReadConfig();
    Xlog("netsim: recv hook installed (dev build; t2r-netsim.txt or T2R_NETSIM = 'delay_ms jitter_ms loss_pct burst')");
}
#endif

static void NsReadSwitches(void)
{
    FILE *f = ExeSideOpen("t2r-no-net-smooth.txt", "r");
    int off = f != NULL;
    if (f)
        fclose(f);
    if (off != g_nsOff)
        Xlog("net smooth: jitter buffer %s (t2r-no-net-smooth.txt)",
             off ? "OFF - winmm's extrapolation" : "on");
    g_nsOff = off;
    {
        FILE *g = ExeSideOpen("t2r-no-dequant.txt", "r");
        int o2 = g != NULL;
        if (g)
            fclose(g);
        if (o2 != g_nsDqOff)
            Xlog("net smooth: sender clock de-quantization %s (t2r-no-dequant.txt)", o2 ? "OFF" : "on");
        g_nsDqOff = o2;
    }
    double fd = 0.0;
    if ((f = ExeSideOpen("t2r-net-delay.txt", "r"))) {
        if (fscanf(f, "%lf", &fd) != 1)
            fd = 0.0;
        fclose(f);
    }
    if (fd != g_nsFixedDelay)
        Xlog("net smooth: playout delay %s %.0f ms (t2r-net-delay.txt)",
             fd > 0.0 ? "fixed" : "adaptive, was",
             fd > 0.0 ? fd : g_nsFixedDelay);
    g_nsFixedDelay = fd;
    double lk = 0.5, lm = 60.0;
    if ((f = ExeSideOpen("t2r-net-lead.txt", "r"))) {
        int n = fscanf(f, "%lf %lf", &lk, &lm);
        if (n < 1)
            lk = 0.5;
        if (n < 2)
            lm = 60.0;
        fclose(f);
    }
    if (lk < 0.0)
        lk = 0.0;
    if (lk > 2.0)
        lk = 2.0;
    if (lm < 0.0)
        lm = 0.0;
    if (lm > 200.0)
        lm = 200.0;
    if (lk != g_nsLeadK || lm != g_ns_lead_max)
        Xlog("net smooth: prediction lead %.2f x one-way, max %.0f ms (t2r-net-lead.txt: 'k max_ms'; k 0 = off)", lk, lm);
    g_nsLeadK = lk;
    g_ns_lead_max = lm;
    // send cap (30 Hz): the anim list is a full snapshot with a loss-safe dictionary (spec anim-tx.cpp:
    // budget charged only on sent packets), events ride winmm's reliable channel, shots are a counter - a skipped frame loses
    // nothing, and the playout buffer interpolates. Halves every sender's and the host relay's traffic (4v4 stutter).
    // t2r-net-sendcap.txt: Hz (20..240), 0 = every frame (winmm's own behaviour)
    int hz = 0;                 // the 30 Hz cap made remote players move choppily - every frame again (t2r-net-sendcap.txt = a cap)
    if ((f = ExeSideOpen("t2r-net-sendcap.txt", "r"))) {
        if (fscanf(f, "%d", &hz) != 1)
            hz = 0;
        if (hz > 0 && hz < 20)
            hz = 20;
        if (hz < 0)
            hz = 0;
        fclose(f);
    }
    if ((f = ExeSideOpen("t2r-no-relay-dedup.txt", "r"))) {
        fclose(f);
        if (!g_nsDedupOff)
            Xlog("net smooth: host relay dedup OFF (t2r-no-relay-dedup.txt)");
        g_nsDedupOff = 1;
    } else
        g_nsDedupOff = 0;
    // opt-IN (t2r-m7-lod.txt): the 30 Hz cap felt choppy and the saving is small (~6-8 %); far peers at full rate by default
    {
        FILE *g = ExeSideOpen("t2r-m7-lod.txt", "r");
        if (g) {
            fclose(g);
            if (g_nsM7LodOff)
                Xlog("net smooth: msg 7 level of detail ON (t2r-m7-lod.txt)");
            g_nsM7LodOff = 0;
        } else
            g_nsM7LodOff = 1;
    }
    {                           // host upload budget, DEFAULT OFF: t2r-host-bw.txt ('budget KB/s', empty = 600) or env T2R_HOST_BW=<KB/s>
        double kb = 0.0;
        int on = 0;
        FILE *g = ExeSideOpen("t2r-host-bw.txt", "r");
        if (g) {
            on = 1;
            if (fscanf(g, "%lf", &kb) != 1 || kb < 50.0)
                kb = 600.0;
            fclose(g);
        } else {
            const char *e = getenv("T2R_HOST_BW");
            if (e && *e && atof(e) >= 0.0 && strcmp(e, "0")) {
                on = 1;
                kb = atof(e) >= 50.0 ? atof(e) : 600.0;
            }
        }
        if (on != g_hbw.on || (on && kb * 1024.0 != g_hbw.budget_Bps)) {
            Xlog("net smooth: host upload budget %s (budget %.0f KB/s; relayed msg 7 paced by pair distance above it; receivers tolerate paced gaps)", on ? "ON" : "OFF", kb);
            HbwInit(&g_hbw, on, on ? kb : 600.0);
        }
        g_nsGapTol = on;
    }
    if (hz != g_nsCapHz)
        Xlog("net smooth: state sends %s %d Hz (t2r-net-sendcap.txt)",
             hz ? "capped at" : "every frame (winmm), cap was",
             hz ? hz : g_nsCapHz);
    g_nsCapHz = hz;
#ifndef T2R_RELEASE
    if (g_nsimQ)
        NsimReadConfig();
#endif
}

static void InstallNetsmooth(void)
{
    FILE *f = ExeSideOpen("t2r-no-anim-rebase.txt", "r");
    g_ns_rebase = f == NULL;
    if (f)
        fclose(f);
    for (int i = 0; i < T2R_MAX_PEERS; i++)
        NsReset(&g_ns[i]);
    NsReadSwitches();
    int a =
        NsRedirectCall(0x18005BD97, 0x18004C490, (void *) NsApplyHook);
    int c = g_ns_rebase ? NsPatchAnimClock() : 0;
    int g = NsPatchSendGate();
    static const uint8_t lp[16] =
        { 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x56, 0x57, 0x55,
0x53, 0x48, 0x83, 0xEC, 0x28 };
    uint8_t *ls = (uint8_t *) T2R_CODE(kCodeNs5A9F0);
    if (!memcmp(ls, lp, 16)) {
        uint8_t *t =
            VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE,
                         PAGE_EXECUTE_READWRITE);
        if (t) {
            memcpy(t, ls, 16);
            t[16] = 0xFF;
            t[17] = 0x25;
            *(uint32_t *) (t + 18) = 0;
            *(uint64_t *) (t + 22) = (uint64_t) (ls + 16);
            g_nsLowsendTramp = (fn_ns_lowsend) t;
            uint8_t p[16] = { 0xFF, 0x25, 0, 0, 0, 0 };
            uint64_t hk = (uint64_t) NsLowsendHook;
            memcpy(p + 6, &hk, 8);
            p[14] = 0x90;
            p[15] = 0x90;
            NsPatch(ls, p, 16);
        }
    }
    g_ns_inst = a;
    NsRequestTimer();
#ifndef T2R_RELEASE
    NsimInstall();
#endif
    Xlog("net smooth: installed - msg 5 hook %s, anim clock %s, send gate %s, tx counter %s, sender-time rebase %s", a ? "ok" : "MISSING (no jitter buffer)", c ? "ok" : "off/MISSING", g ? "ok" : "MISSING", g_nsLowsendTramp ? "ok" : "MISSING", g_ns_rebase ? "on" : "off (t2r-no-anim-rebase.txt)");
}

static void FeatureNetsmooth(void)
{
    static uint64_t last_sw;
    static double last_log;
    static uint64_t rx0, fr0;
    if (!g_ns_inst)
        return;
    if (!last_sw || g_frame - last_sw >= 300) {
        last_sw = g_frame;
        NsReadSwitches();
    }
    double now = NsNowMs();
    // the anim clock target is only good while the locator runs for that puppet each frame (winmm skips
    // it under a peer lock / melee gate): a stale target would freeze the replayed pose
    for (int sl = 0; sl < T2R_MAX_PEERS; sl++)
        if (g_nsOff || now - g_ns[sl].f_last > 60.0)
            g_nsAnim[sl].valid = 0;
    OwTick(now, *(volatile uint64_t *) T2R_SYM(kRxCounter));
    if (last_log <= 0.0) {
        last_log = now;
        rx0 = *(volatile uint64_t *) T2R_SYM(kRxCounter);
        fr0 = g_frame;
        return;
    }
    double el = now - last_log;
    if (el < 10000.0)
        return;
    uint64_t rx = *(volatile uint64_t *) T2R_SYM(kRxCounter);
    double sec = el / 1000.0;
    if (MatchSubstate() == 6) {
        char line[1400];
        int n = 0;
        uint32_t tot_n = 0, tot_b = 0;
        line[0] = 0;
        for (int t = 0; t < 256; t++) {
            tot_n += g_nsTxN[t];
            tot_b += g_ns_tx_b[t];
            if (g_nsTxN[t] && n < 1100)
                n += snprintf(line + n, SNLEFT(line, n),
                              " %02x:%.0f/s %.0fB", t, g_nsTxN[t] / sec,
                              (double) g_ns_tx_b[t] / g_nsTxN[t]);
        }
        {
            uint32_t kb = (uint32_t) (tot_b / sec / 1024.0);
            g_cap_tx_sum_kb += kb;
            if (kb > g_cap_tx_peak_kb)
                g_cap_tx_peak_kb = kb;
            g_cap_n++;
        }
        Xlog("net stats: %.0f fps, rx %.0f pkt/s, tx %.0f pkt/s %.1f KB/s (type:rate avg size incl. 12 B header):%s", (g_frame - fr0) / sec, (rx - rx0) / sec, tot_n / sec, tot_b / sec / 1024.0, line);
        {
            uint32_t em, eb;
            TxsTotals(&g_txs, &em, &eb);
            TxsFmt(&g_txs, sec, 6, line, sizeof line);
            Xlog("net stats: event messages (to winmm's send, ours + winmm's, one hop at the sender, 8 B record header counted): %.1f msg/s %.2f KB/s = %.1f%% of tx; top 6 by bytes (id rate, bytes per msg, bytes/s, uN = %% sent to one peer):%s; state send frames %u sent / %u skipped (cap %d Hz); relayed duplicate states dropped %u", em / sec, eb / sec / 1024.0, tot_b ? 100.0 * eb / tot_b : 0.0, em ? line : " none", g_ns_cap_ok, g_ns_cap_skip, g_nsCapHz, g_nsDedupDrop);
        }
        g_nsDedupDrop = 0;
        {
            uint32_t nh = 0;
            char hb[64];
            int hn = 0;
            hb[0] = 0;
            for (uint32_t k = 0; k < T2R_MAX_PEERS; k++)
                if (g_nsM7Half[k]) {
                    nh++;
                    hn += snprintf(hb + hn, SNLEFT(hb, hn), " %u", k);
                }
            Xlog("net stats: msg 7 rate by distance: %.0f/s full-rate sends to near peers, %.0f/s offered to far peers of which %.0f/s sent (30 Hz) and %.0f/s skipped; far peers now: %u%s%s (msg 5 stays at full rate)%s", g_ns_m7_full_n / sec, g_ns_m7_half_n / sec, (g_ns_m7_half_n - g_ns_m7_skip_n) / sec, g_ns_m7_skip_n / sec, nh, nh ? " (slots" : "", nh ? hb : "", g_nsM7LodOff ? " [LOD OFF]" : "");
            g_ns_m7_full_n = g_ns_m7_half_n = g_ns_m7_skip_n = 0;
        }
        if (g_hbw.on) {
            uint32_t o, sn, dr;
            uint64_t bd;
            double lf[4];
            HbwTakeStats(&g_hbw, NsNowMs(), &o, &sn, &dr, &bd, lf);
            Xlog("net stats: host upload budget %.0f KB/s: demand %.0f KB/s, tx %.0f KB/s; level now %d (time at 0/1/2/3: %.0f/%.0f/%.0f/%.0f %%); relayed msg 7 offered %.0f/s, sent %.0f/s, dropped %.0f/s = %.1f KB/s saved (%.1f%% of tx); level changes %d", g_hbw.budget_Bps / 1024.0, g_hbw.demand_Bps / 1024.0, tot_b / sec / 1024.0, g_hbw.level, lf[0] * 100, lf[1] * 100, lf[2] * 100, lf[3] * 100, o / sec, sn / sec, dr / sec, bd / sec / 1024.0, tot_b ? 100.0 * bd / (tot_b + (double) bd) : 0.0, g_hbw.lvl_changes);
            g_hbw.lvl_changes = 0;
        }
        for (uint32_t sl = 0; sl < T2R_MAX_PEERS; sl++) {
            NsSlot *s = &g_ns[sl];
            if (!s->st_m5 && !s->st_frames)
                continue;
            double fr = s->st_frames ? (double) s->st_frames : 1.0;
            if (s->st_m7 >= 100) {
                uint32_t l10 =
                    (uint32_t) (1000.0 * s->st_lost7 /
                                (s->st_m7 + s->st_lost7));
                if (l10 > g_cap_loss10)
                    g_cap_loss10 = l10 > 255 ? 255 : l10;
            }
            Xlog("net stats: slot %u rx msg5 %.0f/s (rejected %u, seq gaps %u = %.1f%% - the host relays a state once per host frame, so these are mostly states it skipped, not lost datagrams; " "REAL loss = msg7 seq gaps %u = %.1f%%), msg7 %.0f/s; sender frame %.1f ms, msg5 sample spacing %.1f ms; jitter mean %.0f peak %.0f max %.0f ms; " "delay %.0f ms, loss ewma (msg7) %.0f%%; extrapolating %.1f%% of frames (held >100 ms %u), corrections %u (max %.2f m), snaps %u, teleports %u, clock resyncs %u; " "stutter frames winmm %u / smoothed %u of %u moving; smoothed %.2f m behind winmm's; prediction error (n %u): dead reckoning mean %.1f cm max %.0f cm vs hold-last %.1f cm / %.0f cm, after a lost packet (n %u) %.1f cm vs %.1f cm%s", sl, s->st_m5 / sec, s->st_m5_rej, s->st_lost, s->st_m5 + s->st_lost ? 100.0 * s->st_lost / (s->st_m5 + s->st_lost) : 0.0, s->st_lost7, s->st_m7 + s->st_lost7 ? 100.0 * s->st_lost7 / (s->st_m7 + s->st_lost7) : 0.0, s->st_m7 / sec, s->sdt, s->si, s->jmean, s->jpk, (double) s->st_jmax, s->st_dsum / fr, s->loss * 100.0, 100.0 * s->st_extrap / fr, s->st_hold, s->st_corr, (double) s->st_corr_max, s->st_snap, s->st_tele, s->st_resync, s->st_stut_w, s->st_stut_o, s->st_moving, s->st_lagn ? s->st_lagsum / s->st_lagn : 0.0, s->st_pe_n, s->st_pe_n ? 100.0 * s->st_pe_cv / s->st_pe_n : 0.0, 100.0 * s->st_pe_max_cv, s->st_pe_n ? 100.0 * s->st_pe_hold / s->st_pe_n : 0.0, 100.0 * s->st_pe_max_hold, s->st_pe_gap_n, s->st_pe_gap_n ? 100.0 * s->st_pe_gap_cv / s->st_pe_gap_n : 0.0, s->st_pe_gap_n ? 100.0 * s->st_pe_gap_hold / s->st_pe_gap_n : 0.0, g_nsOff ? " (smoothing OFF)" : "");
            Xlog("net stats: slot %u render path (game %.0f fps): locator calls %.0f/s, distinct root placements %.0f/s (%.0f%%); anim clock advanced %.0f/s still %.0f/s irregular %u snaps %u, mean clock/real %.2f; pose stamp changes %.0f/s; " "root vec4 (pose writer) calls %.0f/s distinct %.0f/s, xyz of vec4[0] vs net pos mean %.2f min %.2f max %.2f m; sender clock quantized (15.6 ms tick) on %.0f%% of %u lists; " "SMOOTHNESS per rendered frame: root placement %.0f%% distinct, anim clock advanced %.0f%%, pose vec4 %.0f%% distinct (interpolated on %.0f%%; winmm's own %.0f%%)%s", sl, (g_frame - fr0) / sec, s->st_frames / sec, s->st_dist / sec, s->st_frames ? 100.0 * s->st_dist / s->st_frames : 0.0, s->st_ck_adv / sec, s->st_ck_still / sec, s->st_ck_irreg, s->st_ck_snap, s->st_ck_n ? s->st_ck_ratio / s->st_ck_n : 0.0, s->st_stp / sec, s->st_v_calls / sec, s->st_v_dist / sec, s->v_dn ? s->v_dpos_sum / s->v_dn : 0.0, (double) s->v_dpos_min, (double) s->v_dpos_max, g_nsDq[sl].st_total ? 100.0 * g_nsDq[sl].st_q / g_nsDq[sl].st_total : 0.0, g_nsDq[sl].st_total, s->st_frames ? 100.0 * s->st_dist / s->st_frames : 0.0, s->st_frames ? 100.0 * s->st_ck_adv / s->st_frames : 0.0, s->st_v_calls ? 100.0 * s->st_vf_dist / s->st_v_calls : 0.0, s->st_vf_calls ? 100.0 * s->st_vf_used / s->st_vf_calls : 0.0, s->st_v_calls ? 100.0 * s->st_v_dist / s->st_v_calls : 0.0, g_nsDqOff ? " (de-quantization OFF)" : "");
            g_nsDq[sl].st_total = g_nsDq[sl].st_q = 0;
            if (s->v_samp_ok) {
                static uint32_t nv;
                if (LOGCAP(nv, 12))
                    Xlog("net stats: slot %u vec4 sample: net pos (%.2f %.2f %.2f) v0 (%.3f %.3f %.3f %.3f) v1 (%.3f %.3f %.3f %.3f) v2 (%.3f %.3f %.3f %.3f) v3 (%.3f %.3f %.3f %.3f) v4 (%.3f %.3f %.3f %.3f)", sl, s->v_samp_p[0], s->v_samp_p[1], s->v_samp_p[2], s->v_samp[0], s->v_samp[1], s->v_samp[2], s->v_samp[3], s->v_samp[4], s->v_samp[5], s->v_samp[6], s->v_samp[7], s->v_samp[8], s->v_samp[9], s->v_samp[10], s->v_samp[11], s->v_samp[12], s->v_samp[13], s->v_samp[14], s->v_samp[15], s->v_samp[16], s->v_samp[17], s->v_samp[18], s->v_samp[19]);
            }
        }
    }
    AcquireSRWLockExclusive(&g_nsLock);
    for (int sl = 0; sl < T2R_MAX_PEERS; sl++) {
        NsSlot *s = &g_ns[sl];
        s->st_vf_dist = s->st_vf_used = s->st_vf_calls = s->st_lost7 = 0;
        s->st_dist = s->st_ck_adv = s->st_ck_still = s->st_ck_irreg =
            s->st_ck_snap = s->st_stp = s->st_ck_n = s->st_v_calls =
            s->st_v_dist = s->v_dn = 0;
        s->st_ck_ratio = 0.0;
        s->v_dpos_sum = 0.0f;
        s->v_samp_ok = 0;
        s->st_m5 = s->st_m5_rej = s->st_lost = s->st_m7 = s->st_m7_est =
            s->st_frames = s->st_extrap = s->st_hold = s->st_snap = 0;
        s->st_corr = s->st_resync = s->st_tele = s->st_stut_w =
            s->st_stut_o = s->st_moving = s->st_lagn = 0;
        s->st_corr_max = s->st_jmax = 0.0f;
        s->st_dsum = s->st_lagsum = 0.0;
        s->st_pe_cv = s->st_pe_hold = s->st_pe_gap_cv = s->st_pe_gap_hold =
            0.0;
        s->st_pe_max_cv = s->st_pe_max_hold = 0.0f;
        s->st_pe_n = s->st_pe_gap_n = 0;
    }
    ReleaseSRWLockExclusive(&g_nsLock);
    memset((void *) g_nsTxN, 0, sizeof g_nsTxN);
    memset((void *) g_ns_tx_b, 0, sizeof g_ns_tx_b);
    memset(&g_txs, 0, sizeof g_txs);
    g_ns_cap_ok = g_ns_cap_skip = 0;
#ifndef T2R_RELEASE
    if (g_nsimQ)
        NsimLog10s();
#endif
    last_log = now;
    rx0 = rx;
    fr0 = g_frame;
}
