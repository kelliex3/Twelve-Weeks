// ---------------------------------------------------------------- the scoreboards list OUR team on top, in blue
// In game, in the ESC menu your team is always on the blue side above, with the faction corresponding to your own faction, and the enemy in the red team.
// winmm fills the two fixed blocks of to1/pause-menu and to1/scoreboard (the ESC page and the end-of-match page; names scoreboard.team0_* on top, team1_* below) by the
// roster team index, so a player of team 1 saw his own team in the lower block. Both blocks have the same layout 474 canvas units apart (widget JSON: team0_band y 803,
// team1_band y 1277, every member the same distance), so when our team is team 1 the two blocks swap places (gui2-rxfm-set-pivot-as on every member, w_pivot): the block
// of our team is the upper one, always. The colour follows the PLACE, not the team: sbBlue / sbRed (tools/features/sbteam_widgets.py) are the two bands, the stock team
// bands are hidden, and the row texts wear one neutral colour. The faction names are already ours (fl_sb_team: our row wears our clan side), written by winmm into the block
// of each team, so they travel with it. Log 'sbteam: ...'. Off: t2r-no-sbteam.txt.
#define SBT_D 474.0f
static int SbtOff(void)
{
    static int off = -1;
    static uint64_t chk;
    return OvOff("t2r-no-sbteam.txt", &off, &chk);
}

static int SbtPos(uint64_t w, float *x, float *y)
{
    uint64_t g[1] = { w };
    uint64_t xf = DcCall(SID_GUI2_GET_WIDGET_TRANSFORM, 1, g);
    if (!xf || xf == ~0ULL || IsBadReadPtr((void *) (uintptr_t) xf, 0x28))
        return 0;
    const float *p = (const float *) (uintptr_t) (xf + 0x20);
    *x = p[0];
    *y = p[1];
    return 1;
}

// one member of block t: moved to the place of its block (swap = the blocks changed places). The current y says where it stands now (a block member is at its base y,
// or SBT_D further up / down), so the move is absolute and repeatable.
static int SbtMove(const char *leaf, int t, int swap)
{
    char p[120];
    snprintf(p, sizeof p, "anim-group.scoreboard.team%d_%s", t, leaf);
    g_settextIn = 1;
    uint64_t w = UiW(p);
    g_settextIn = 0;
    float x, y;
    if (!w || !SbtPos(w, &x, &y))
        return 0;
    float base =
        t == 0 ? (y >= 1250.0f ? y - SBT_D : y) : (y <
                                                   1250.0f ? y +
                                                   SBT_D : y);
    float want = swap ? (t == 0 ? base + SBT_D : base - SBT_D) : base;
    if (fabsf(want - y) > 0.5f)
        WPivot(w, x, want);
    return 1;
}

static uint64_t sbt_last_probe, sbt_at;
static int sbt_last = -1;
static unsigned sbt_gen = ~0u;
static void SbtForget(void)
{
    sbt_last = -1;
}                               // the page is gone: its next instance starts at the template places

static void SbtTick(uint64_t now, uint64_t probe)
{
    int swap = 0, known = 0;
    if (!SbtOff() && MatchSubstate() >= 3) {
        uint32_t me = *(uint32_t *) T2R_SYM(kMySlot);
        uint8_t *lo = LobbyObj();
        if (me < T2R_MAX_PEERS && lo && !IsBadReadPtr(lo, 0x1b8)
            && lo[0x1a] > 0 && lo[0x1a] <= 8) {
            for (int i = 0; i < lo[0x1a]; i++)
                if (lo[0x1d + i * 0x32] == me) {
                    known = 1;
                    swap = lo[0x1d + i * 0x32 + 0x31] & 1;
                }
        }
    }
    if (!known)
        swap = 0;
    if (swap == sbt_last && probe == sbt_last_probe && sbt_gen == g_uiGen
        && now - sbt_at < 2000)
        return;
    static const char *const M[] =
        { "band", "name", "col_parts", "col_downs", "col_executions",
"col_deaths", "col_revives" };
    static const char *const R[] =
        { "bg", "name", "parts", "downs", "executions", "deaths",
"revives", "ping", "input", "rkIcon", "rkNum" };
    int n = 0;
    for (int t = 0; t < 2; t++) {
        for (unsigned k = 0; k < sizeof M / sizeof *M; k++)
            n += SbtMove(M[k], t, swap);
        for (int r = 0; r < 4; r++)
            for (unsigned k = 0; k < sizeof R / sizeof *R; k++) {
                char leaf[40];
                snprintf(leaf, sizeof leaf, "row%d_%s", r, R[k]);
                n += SbtMove(leaf, t, swap);
            }
    }
    if (swap != sbt_last)
        Xlog("sbteam: our team is team %d - %s block on top (%d widgets placed)", swap, swap ? "team 1" : "team 0", n);
    sbt_last = swap;
    sbt_last_probe = probe;
    sbt_gen = g_uiGen;
    sbt_at = now;
}
