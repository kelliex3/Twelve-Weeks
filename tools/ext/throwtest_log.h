// ---------------------------------------------------------------- throwable test harness: structured log lines (DEV builds only)
// Included at the top of explosives2013.c. Every line starts with "throwtest:" and carries tms = GetTickCount64() (the two A/B instances
// share the machine clock, so tools/win/throwtest_report.py joins A's "sent" lines to B's lines by time). Release builds: all macros empty.
#ifndef T2R_RELEASE
static uint64_t g_ttLastMs;   // last throwtest line of the target side (the target heal waits for quiet)
static struct {
    int reached, best;
    float ratio, dmg;
} g_ttRes;
static void TtResReset(void)
{
    memset(&g_ttRes, 0, sizeof g_ttRes);
    g_ttRes.best = -1;
}

static const char *TtKn(int kind)
{
    return kind == 2 ? "molotov" : kind == 4 ? "placed" : kind ==
        5 ? "xbowarrow" : "nail";
}

static void TtLogBurst(int kind, int slot, const float *pos,
                         const float *pl, float d, float reach,
                         float scale)
{
    g_ttLastMs = GetTickCount64();
    Xlog("throwtest: burst kind=%s slot=%d at=%.3f,%.3f,%.3f feet=%.3f,%.3f,%.3f dist=%.2f reach=%.2f scale=%.2f downed=%d tms=%llu", TtKn(kind), slot, pos[0], pos[1], pos[2], pl[0], pl[1], pl[2], d, reach, scale, g_downed, (unsigned long long) g_ttLastMs);
}

static void TtLogResult(int kind, int slot, const float *pos,
                          const float *feet, float hp0, float hp1,
                          float v0, float v1)
{
    g_ttLastMs = GetTickCount64();
    float dx = pos[0] - feet[0], dy = pos[1] - (feet[1] + 0.9f), dz =
        pos[2] - feet[2];
    Xlog("throwtest: result kind=%s slot=%d dist=%.2f probes=%d/6 best=%d ratio=%.2f dmg_calc=%.0f hp_before=%.1f hp_after=%.1f vest_before=%.0f vest_after=%.0f downed=%d killed=%d tms=%llu", TtKn(kind), slot, __builtin_sqrtf(dx * dx + dy * dy + dz * dz), g_ttRes.reached, g_ttRes.best, g_ttRes.ratio, g_ttRes.dmg, hp0, hp1, v0, v1, g_downed, hp1 <= 0.0f, (unsigned long long) g_ttLastMs);
}

#define TT_LOG_BURST(kind, slot, pos, pl, d, reach, s) TtLogBurst((kind), (slot), (pos), (pl), (d), (reach), (s))
#define TT_LOG_FRIENDLY(kind, slot) Xlog("throwtest: friendly kind=%s slot=%d tms=%llu", TtKn(kind), (slot), (unsigned long long)GetTickCount64())
#define TT_NOTE_PROBES(r, b, ra) (g_ttRes.reached = (r), g_ttRes.best = (b), g_ttRes.ratio = (ra))
#define TT_NOTE_DMG(d) (g_ttRes.dmg = (d))
#define TT_LOG_FIRE(ticks, total, secs) do { g_ttLastMs = GetTickCount64(); Xlog("throwtest: fire ticks=%d dmg=%.1f secs=%.2f hp=%.1f tms=%llu", (ticks), (total), (secs), LocalHp(), (unsigned long long)g_ttLastMs); } while (0)
#else
#define TT_LOG_BURST(kind, slot, pos, pl, d, reach, s) ((void)0)
#define TT_LOG_FRIENDLY(kind, slot) ((void)0)
#define TT_NOTE_PROBES(r, b, ra) ((void)0)
#define TT_NOTE_DMG(d) ((void)0)
#define TT_LOG_FIRE(ticks, total, secs) ((void)0)
#endif
