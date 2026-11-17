// perf_cost.c - the second 30 s perf line, "perf30b:". Included by t2r-ext.c after mem_probe.c; perf30 itself (mem_probe.c) is
// untouched. Everything here is counters that already exist on the hot paths (QPC accumulators, no allocation, no file / syscall of its own):
//   bw   = IsBadWritePtr replacement (t2r_bad_write): calls, fresh cache hits, stale hits answered while the worker re-validates, SYNCHRONOUS VirtualQuery
//          rounds on the tick thread (count, avg us, worst ms, total ms per frame). The synchronous ones are the multi-ms stalls.
//   dc   = every script native the tick thread called (dc_call): per frame count and ms, the 6 costliest sids (hex; tools/reports/dc_sid_names.py names them)
//   keys = GetAsyncKeyState calls per frame and how many were real syscalls (the rest came from the per-frame cache)
//   paths = the cost buckets that are not PF features: recv_hook (every ext / winmm event received, incl. winmm's handler), send_gate (every event
//           sent, incl. winmm's send), msg 5 apply (ns_apply_hook, incl. winmm's apply), the low send hook (msg 5 / 7 datagrams, 1 call in 8 timed), sq_tick (per frame), xlog
//           (every line, formatting + rate limit + ring + buffer copy): calls per second and mean us per call, plus the file-poll cache (calls / frame, real file-system calls on the
//           asking thread per frame, background re-checks per second)
// Windows are counted only while perf30 counts frames (in a match, 15 s in); one line per 30 s, skipped under 120 frames, counters reset by each line.
static uint64_t g_pcFrames;
static void PerfCostTick(void)
{
    static uint64_t next_at, win_at;
    static LONG64 b_calls, b_fresh, b_stale, b_vq, b_vq_q, b_reval,
        k_calls, k_real;
    static LONG64 p_n[PCB_N], p_q[PCB_N], p_qn[PCB_N], f_n, f_real, f_bg;
    static uint64_t cm_calls0, cm_slow0;
    static int64_t cm_sum0;
    uint64_t now = GetTickCount64();
    if (g_plCounting)
        g_pcFrames++;
    if (now < next_at)
        return;
    if (!next_at) {
        next_at = now + 30000;
        g_pcFrames = 0;
        win_at = now;
        return;
    }
    next_at = now + 30000;
    if (MatchSubstate() != 6 || g_pcFrames < 120 || !g_stallQf) {    // not a match window: drop what menus / loads counted
        b_calls = g_bw_n_calls;
        b_fresh = g_bw_n_fresh;
        b_stale = g_bw_n_stale;
        b_vq = g_bw_n_vq;
        b_vq_q = g_bw_vq_q;
        b_reval = g_bw_n_reval;
        g_bw_vq_max = 0;
        k_calls = g_gk_calls;
        k_real = g_gk_real;
        for (int i = 0; i < PCB_N; i++) {
            p_n[i] = g_pcb[i].n;
            p_q[i] = g_pcb[i].q;
            p_qn[i] = g_pcb[i].qn;
        }
        f_n = g_fac_n;
        f_real = g_fac_real;
        f_bg = g_fac_bg;
        win_at = now;
        cm_calls0 = g_cm_calls;
        cm_slow0 = g_cmSlowN;
        cm_sum0 = g_cm_q_sum;
        g_cm_q_max = 0;
        memset(g_dcs, 0, sizeof g_dcs);
        g_dcs_tot_n = g_dcs_tot_q = 0;
        g_pcFrames = 0;
        return;
    }
    double k = 1000.0 / (double) g_stallQf, n = (double) g_pcFrames;
    LONG64 c = g_bw_n_calls, fr = g_bw_n_fresh, st = g_bw_n_stale, vq =
        g_bw_n_vq, vqq = g_bw_vq_q, rv = g_bw_n_reval;
    char line[1800];
    int o =
        snprintf(line, sizeof line,
                 "perf30b: %.0f frames | bad-write cache: %.1f calls/frame (fresh %.1f, stale %.2f, revalidated %lld), sync VirtualQuery %lld (avg %.0f us, worst %.2f ms, %.4f ms/frame)",
                 n, (double) (c - b_calls) / n,
                 (double) (fr - b_fresh) / n, (double) (st - b_stale) / n,
                 (long long) (rv - b_reval),
                 (long long) (vq - b_vq),
                 vq >
                 b_vq ? (double) (vqq -
                                  b_vq_q) * k * 1000.0 / (double) (vq -
                                                                   b_vq) :
                 0.0, (double) g_bw_vq_max * k,
                 (double) (vqq - b_vq_q) * k / n);
    b_calls = c;
    b_fresh = fr;
    b_stale = st;
    b_vq = vq;
    b_vq_q = vqq;
    b_reval = rv;
    g_bw_vq_max = 0;
    {                           // Why the synchronous queries happened (cold = region never cached / aged = cached but past BW_SERVE_MS) and who asked (dll RVA), then reset
        static LONG64 b_cold, b_aged, b_pro;
        o += snprintf(line + o, SNLEFT(line, o),
                      " | bw misses: cold %lld, aged %lld, proactive refreshes %lld%s | by caller (rva n/cold/ms):",
                      (long long) (g_bw_n_cold - b_cold),
                      (long long) (g_bw_n_aged - b_aged),
                      (long long) (g_bw_n_pro - b_pro),
                      g_bwOld ? " [old cache]" : "");
        b_cold = g_bw_n_cold;
        b_aged = g_bw_n_aged;
        b_pro = g_bw_n_pro;
        for (int i = 0; i < BW_CALLERS; i++)
            if (g_bw_by[i].n)
                o += snprintf(line + o, SNLEFT(line, o),
                              " %llx %lld/%lld/%.1f;",
                              (unsigned long long) g_bw_by[i].ra,
                              (long long) g_bw_by[i].n,
                              (long long) g_bw_by[i].cold,
                              (double) g_bw_by[i].q * k);
        memset(g_bw_by, 0, sizeof g_bw_by);
    }
    {                           // dc_call by sid
        o += snprintf(line + o, SNLEFT(line, o),
                      " | dc_call (tick thread): %.1f calls/frame, %.4f ms/frame | top:",
                      (double) g_dcs_tot_n / n,
                      (double) g_dcs_tot_q * k / n);
        char used[128] = { 0 };
        for (int r = 0; r < 6; r++) {
            int best = -1;
            for (int i = 0; i < 128; i++)
                if (!used[i] && g_dcs[i].sid
                    && (best < 0 || g_dcs[i].q > g_dcs[best].q))
                    best = i;
            if (best < 0)
                break;
            used[best] = 1;
            o += snprintf(line + o, SNLEFT(line, o),
                          " %016llx x%.2f/frame %.4f ms/frame (avg %.1f us, worst %.2f ms);",
                          (unsigned long long) g_dcs[best].sid,
                          (double) g_dcs[best].n / n,
                          (double) g_dcs[best].q * k / n,
                          (double) g_dcs[best].q * k * 1000.0 /
                          (double) g_dcs[best].n,
                          (double) g_dcs[best].max * k);
        }
        memset(g_dcs, 0, sizeof g_dcs);
        g_dcs_tot_n = g_dcs_tot_q = 0;
    }
    {
        LONG64 kc = g_gk_calls, kr = g_gk_real;
        o += snprintf(line + o, SNLEFT(line, o),
                      " | keys: GetAsyncKeyState %.1f/frame (real syscalls %.1f)",
                      (double) (kc - k_calls) / n,
                      (double) (kr - k_real) / n);
        k_calls = kc;
        k_real = kr;
    }
    Xlog("%s", line);
    {                           // 'perf30c:' (own line: the xlog buffer is 1100 characters and perf30b's line is long): calls per second, mean us per call; the QPC pair is taken on 1 call in 8 for the low send hook, on every call for the rest
        static const char *const NM[PCB_N] =
            { "recv_hook", "send_gate", "msg5 apply", "low send",
"sq_tick", "xlog" };
        char l2[700];
        double sec = win_at
            && now > win_at ? (double) (now - win_at) / 1000.0 : 30.0;
        int o2 =
            snprintf(l2, sizeof l2,
                     "perf30c: paths (calls/s, mean us per call, ms per s):");
        for (int i = 0; i < PCB_N; i++) {
            LONG64 cn = g_pcb[i].n - p_n[i], cq =
                g_pcb[i].q - p_q[i], cqn = g_pcb[i].qn - p_qn[i];
            double mean_us =
                cqn > 0 ? (double) cq * k * 1000.0 / (double) cqn : 0.0;
            o2 +=
                snprintf(l2 + o2, SNLEFT(l2, o2),
                         " %s %.0f/s %.2f us %.2f ms/s;", NM[i],
                         (double) cn / sec, mean_us,
                         (double) cn / sec * mean_us / 1000.0);
            p_n[i] = g_pcb[i].n;
            p_q[i] = g_pcb[i].q;
            p_qn[i] = g_pcb[i].qn;
        }
        LONG64 fn = g_fac_n, fr = g_fac_real, fb = g_fac_bg;
        o2 +=
            snprintf(l2 + o2, SNLEFT(l2, o2),
                     " | file polls: %.2f calls/frame, %.3f real calls/frame on the asking thread, %.1f background re-checks/s",
                     (double) (fn - f_n) / n, (double) (fr - f_real) / n,
                     (double) (fb - f_bg) / sec);
        f_n = fn;
        f_real = fr;
        f_bg = fb;
        win_at = now;
        {                       // the engine's own per-frame heap compaction (cm_hook): mean / worst time and the passes over 2 ms - the engine-only hitches' main suspect
            uint64_t cc = g_cm_calls, cs = g_cmSlowN;
            int64_t csum = g_cm_q_sum;
            o2 +=
                snprintf(l2 + o2, SNLEFT(l2, o2),
                         " | heap compaction: %.0f passes, mean %.1f us, worst %.2f ms, over 2 ms %llu",
                         (double) (cc - cm_calls0),
                         cc >
                         cm_calls0 ? (double) (csum -
                                               cm_sum0) * k * 1000.0 /
                         (double) (cc - cm_calls0) : 0.0,
                         (double) g_cm_q_max * k,
                         (unsigned long long) (cs - cm_slow0));
            cm_calls0 = cc;
            cm_slow0 = cs;
            cm_sum0 = csum;
            g_cm_q_max = 0;
        }
        Xlog("%s", l2);
    }
    g_pcFrames = 0;
}
