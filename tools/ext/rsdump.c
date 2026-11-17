// rsdump.c: full render-settings logger for the Bookstore lighting A/B comparison.
// xlog cuts repeated lines, so RSZFULL / RSZLFULL lost most keys; this writes its own file, t2r-rsdump.txt next to the exe, never through xlog.
// In a match, every 500 ms: the live value (get-render-setting-value-float) of all 1234 keys Part I registers (1059 float, 175 int) (rsdump_keys.h,
// tools/maps/gen_rsdump.py), compared with the previous sample:
//   "T <ms> cam x y z vram <used>/<budget> MB rs-level <n>"   header of every sample that has a change, and of every full dump
//   "C <key> <old> -> <new>"                                    each key that changed
//   "F <key> <value> p1def <Part I default>"                    every key, at the first sample and then every 10 s
// Diff two clients with tools/maps/rsdump_diff.py. Included from immersion.c after rszl.c; dev and release. Opt-out: t2r-no-rsdump.txt.
#include "rsdump_keys.h"
static void ImmRsdumpTick(int in_match)
{
    static uint64_t at, full_at, nf_a, sid[RSDUMP_N];
    static float prev[RSDUMP_N];
    static int have, nf_v;
    static FILE *f;
    if (!in_match) {
        have = 0;
        if (f) {
            fflush(f);
        }
        return;
    }
    uint64_t t = GetTickCount64();
    if (t - at < 500 || ImmNoflag("t2r-no-rsdump.txt", &nf_a, &nf_v))
        return;
    at = t;
    float cp[3], cd[3];
    if (!CamPosDir(cp, cd))
        return;
    if (!f) {
        f = fopen("t2r-rsdump.txt", "ab");
        if (!f)
            return;
        fprintf(f, "# rsdump start (tick %llu)\r\n",
                (unsigned long long) t);
    }
    if (!sid[0])
        for (int i = 0; i < RSDUMP_N; i++)
            sid[i] = FNV64(g_rsdump_keys[i].name);
    static float v[RSDUMP_N];
    uint64_t fn = FNV64("get-render-setting-value-float");
    for (int i = 0; i < RSDUMP_N; i++) {
        uint64_t a = sid[i], r = DcCall(fn, 1, &a);
        uint32_t u = (uint32_t) r;
        float x;
        memcpy(&x, &u, 4);
        v[i] = r == ~0ULL ? NAN : g_rsdump_keys[i].is_int ? (float) (int32_t) u : x;    // int keys: the getter's raw low 32 bits as an integer
    }
    char head[200];
    {
        MpVmi l = { 0 };
        int okv = g_mpVramState == 1 && MpVram(0, &l);
        snprintf(head, sizeof head,
                 "T %llu cam %.2f %.2f %.2f vram %llu/%llu MB rs-level %d",
                 (unsigned long long) t, (double) cp[0], (double) cp[1],
                 (double) cp[2],
                 okv ? (unsigned long long) (l.usage >> 20) : 0ULL,
                 okv ? (unsigned long long) (l.budget >> 20) : 0ULL,
                 g_immRsLevel);
    }
    int full = !have || t - full_at >= 10000, headed = 0;
    if (full) {
        full_at = t;
        fprintf(f, "%s full\r\n", head);
        headed = 1;
        for (int i = 0; i < RSDUMP_N; i++)
            fprintf(f, "F %s %g p1def %g\r\n", g_rsdump_keys[i].name,
                    (double) v[i], (double) g_rsdump_keys[i].p1def);
    }
    if (have)
        for (int i = 0; i < RSDUMP_N; i++) {
            float a = prev[i], b = v[i];
            if (__builtin_isnan(a) && __builtin_isnan(b))
                continue;
            if (!__builtin_isnan(a) && !__builtin_isnan(b)
                && fabsf(a - b) <= 0.0005f * fabsf(a) + 1e-6f)
                continue;
            if (!headed) {
                fprintf(f, "%s\r\n", head);
                headed = 1;
            }
            fprintf(f, "C %s %g -> %g\r\n", g_rsdump_keys[i].name,
                    (double) a, (double) b);
        }
    if (headed)
        fflush(f);
    memcpy(prev, v, sizeof v);
    have = 1;
}
