// Host upload budget. Pure C: tests/netsim_sim.c ("hostbw" / "bwmodel") and tests/hostbw_test.c compile this file.
//
// Problem: a host with N players relays every other player's msg 7 (the anim list, ~625 B) to every client at the sender's frame rate; 4v4 hosts upload
// 0.9 - 2.5 MB/s and a home line that cannot sustain that lags for everyone. This module decides, for ONE msg 7 datagram the host is about to send
// (relayed list of player `src` to client `dst`), whether to send it. It is default OFF (t2r-host-bw.txt) and does nothing until the host's own
// measured upload DEMAND (bytes it sends + bytes this module dropped) exceeds a budget, then it paces only the pairs that matter least:
//   near pair (src and dst within 12 m)                  never touched, full rate
//   mid  pair (12 - 25 m)                                level 2: 30 Hz, level 3: 20 Hz
//   far  pair (> 25 m)                                   level 1: 30 Hz, level 2: 20 Hz, level 3: 15 Hz
// The host's own list (src == host) and every msg 5 (position, the thing the eye follows) are never touched. Level is picked from demand / budget with
// hysteresis (up at once, down after two quiet windows); demand includes the dropped bytes so the controller does not oscillate.
// Wire format unchanged: a dropped list is a seq gap exactly like a lost datagram (the anim list is a full snapshot).
#ifndef HOSTBW_H
#define HOSTBW_H
#include <stdint.h>
#include <string.h>

#define HBW_N 8
#define HBW_NEAR_M 12.0f
#define HBW_FAR_M 25.0f
#define HBW_WIN_MS 500.0

typedef struct {
    int on;                     // 0 = every call passes
    double budget_Bps;          // upload budget, bytes per second
    int level;                  // 0 = off, 1..3
    double last[HBW_N][HBW_N];  // [dst][src] ms of the last forwarded list
    double win_t, demand_Bps;   // controller window start, smoothed demand
    uint64_t win_tx0, win_drop; // tx bytes counter at the window start, bytes dropped in the window
    int quiet;                  // consecutive windows that asked for a lower level
    // stats (reset by hbw_take_stats)
    uint32_t n_offered, n_sent, n_dropped;
    uint64_t b_dropped;
    int lvl_changes;
    double lvl_time[4], lvl_t0;
} HbwT;

static const double HBW_IV_MS[4][3] = { { 0, 0, 0 }, { 0, 0, 33.3 }, { 0, 33.3, 50.0 }, { 0, 50.0, 66.7 } };    // [level][near, mid, far]

static void HbwInit(HbwT *h, int on, double budget_KBps)
{
    memset(h, 0, sizeof *h);
    h->on = on;
    h->budget_Bps = budget_KBps * 1024.0;
}

// level from demand / budget with hysteresis; returns the new level
static int HbwLevelFor(int cur, double r)
{
    static const double up[3] = { 1.00, 1.25, 1.60 };
    int want = r >= up[2] ? 3 : r >= up[1] ? 2 : r >= up[0] ? 1 : 0;
    if (want > cur)
        return want;
    // stay while within 5 % of the threshold that gave the current level
    if (cur > 0 && want < cur && r >= up[cur - 1] * 0.95)
        want = cur;
    return want;
}

// advance the controller; tx_total = cumulative bytes this host really put on the wire (all datagram types)
static void HbwTick(HbwT *h, double now, uint64_t tx_total)
{
    if (h->win_t == 0.0) {
        h->win_t = now;
        h->win_tx0 = tx_total;
        h->lvl_t0 = now;
        return;
    }
    double dt = now - h->win_t;
    if (dt < HBW_WIN_MS)
        return;
    double sent = (double) (tx_total - h->win_tx0), drop =
        (double) h->win_drop;
    double d = (sent + drop) / (dt * 0.001);
    h->demand_Bps =
        h->demand_Bps >
        0.0 ? h->demand_Bps + (d - h->demand_Bps) * 0.5 : d;
    h->win_t = now;
    h->win_tx0 = tx_total;
    h->win_drop = 0;
    int nl = HbwLevelFor(h->level, h->demand_Bps / h->budget_Bps);
    if (nl < h->level) {
        if (++h->quiet < 2)
            nl = h->level;
    } else
        h->quiet = 0;
    if (nl != h->level) {
        h->lvl_time[h->level] += now - h->lvl_t0;
        h->lvl_t0 = now;
        h->level = nl;
        h->quiet = 0;
        h->lvl_changes++;
    }
}

// the decision for one relayed msg 7: src = origin slot, dst = receiving client slot, dist in metres (< 0 unknown), wire = datagram bytes incl. header
static int HbwMsg7(HbwT *h, double now, int src, int dst, float dist,
                    uint32_t wire, uint64_t tx_total)
{
    if (!h->on)
        return 1;
    HbwTick(h, now, tx_total);
    h->n_offered++;
    if (h->level == 0 || src < 0 || src >= HBW_N || dst < 0 || dst >= HBW_N
        || !(dist >= 0.0f)) {
        h->n_sent++;
        return 1;
    }
    int tier = dist < HBW_NEAR_M ? 0 : dist < HBW_FAR_M ? 1 : 2;
    double iv = HBW_IV_MS[h->level][tier];
    double *l = &h->last[dst][src];
    if (iv > 0.0 && now - *l < iv - 1.5) {
        h->n_dropped++;
        h->b_dropped += wire;
        h->win_drop += wire;
        return 0;
    }
    *l = now;
    h->n_sent++;
    return 1;
}

static void HbwTakeStats(HbwT *h, double now, uint32_t *off,
                           uint32_t *sent, uint32_t *drop, uint64_t *bdrop,
                           double *lvl_frac)
{
    h->lvl_time[h->level] += now - h->lvl_t0;
    h->lvl_t0 = now;
    double tot =
        h->lvl_time[0] + h->lvl_time[1] + h->lvl_time[2] + h->lvl_time[3];
    for (int i = 0; i < 4; i++)
        lvl_frac[i] = tot > 0.0 ? h->lvl_time[i] / tot : 0.0;
    *off = h->n_offered;
    *sent = h->n_sent;
    *drop = h->n_dropped;
    *bdrop = h->b_dropped;
    h->n_offered = h->n_sent = h->n_dropped = 0;
    h->b_dropped = 0;
    memset(h->lvl_time, 0, sizeof h->lvl_time);
}
#endif
