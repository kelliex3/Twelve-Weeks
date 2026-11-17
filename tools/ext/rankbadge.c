// rankbadge.c - the 2013 rank badge (chalk symbol + weeks survived) in FRONT OF EVERY PLAYER NAME, one look on every screen
// (the badge shows in the lobby and on every screen, consistently).
// Look: tools/features/rank_badge_widgets.py (one geometry function: symbol = the name's point size, weeks number 0.62 x, the name moves right by the same width on every
// screen). Drawing: rank_ui (rank13.h) on an image + text widget pair; this file only finds out WHOSE rank it is.
// Data: the daemon's rank table (op 0xBF -> reply 0xBF "n=<k> HHHHHHHH:r:w ...", tools/rankbadge.py: one entry per recently seen player, keyed by a hash of the SHOWN
// name = ASCII letters / digits only, lower case, so "[TAG] Name", the vote page's "_TAG__Name" and the lobby's " (party lead)" suffix reach the same key). Polled
// every 12 s (2.5 s while a name was missing) as long as some screen asked for a rank in the last 15 s. The map vote's roster carries its ranks itself (0xEB pk<i> / pw<i>).
// Included after rank13.h, before clan13.c / friends13.c (the lobby rows and the friends panel call rkb_draw / rkb_draw_me / rkb_draw_at).
#define RKB_MAX 400
static struct {
    uint32_t h;
    uint8_t rk;
    uint16_t wk;
} g_rkb[RKB_MAX];               // guarded by g_clan_cs
static int g_rkbN;
static volatile LONG g_rkb_ver, g_rkbDead;     // g_rkb_dead: an older daemon ignores the op (2 unanswered tries stop the polling)
static volatile uint64_t g_rkbWantAt; // GetTickCount64 of the last lookup
static volatile LONG g_rkbMiss;        // the last lookup found nothing: ask again soon

static uint32_t RkbHash(const char *name)
{                               // = tools/rankbadge.py key()
    uint32_t h = 2166136261u;
    int any = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (*p == '<') {
            const char *e = strchr((const char *) p, '>');
            if (e) {
                p = (const unsigned char *) e;
                continue;
            }
        }                       // <font ...> markup
        if (p[0] == ' ') {      // suffixes "  (YOU)" (friends panel: two spaces), " (party lead)", " (not searching)", "  (TY)"
            const unsigned char *q = p;
            while (*q == ' ')
                q++;
            if (*q == '('
                && (!strncmp((const char *) q + 1, "party", 5)
                    || !strncmp((const char *) q + 1, "YOU", 3)
                    || !strncmp((const char *) q + 1, "TY", 2)
                    || !strncmp((const char *) q + 1, "not searching",
                                13)))
                break;
        }
        if (p[0] == ' ' && p[1] == '[' && p[2] == 'T' && p[3] >= '1'
            && p[3] <= '2' && p[4] == ']') {
            p += 4;
            continue;
        }                       // " [T1]"
        int c = *p;
        if (c >= 'A' && c <= 'Z')
            c += 32;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
            continue;
        h = (h ^ (uint32_t) c) * 16777619u;
        any = 1;
    }
    return any ? h : 0;
}

static void RkbParse(const char *t)
{                               // worker thread (clan_worker)
    static struct {
        uint32_t h;
        uint8_t rk;
        uint16_t wk;
    } tmp[RKB_MAX];
    int n = 0;
    while (*t && n < RKB_MAX) {
        while (*t == ' ')
            t++;
        unsigned h;
        int r, w, used = 0;
        if (sscanf(t, "%8x:%d:%d%n", &h, &r, &w, &used) == 3 && used > 0) {
            tmp[n].h = h;
            tmp[n].rk = (uint8_t) (r < 0 ? 0 : r > 9 ? 9 : r);
            tmp[n].wk = (uint16_t) (w < 0 ? 0 : w > 999 ? 999 : w);
            n++;
            t += used;
        } else {
            const char *s = strchr(t, ' ');
            if (!s)
                break;
            t = s;
        }                       // "n=<k>" or junk: skip the word
    }
    EnterCriticalSection(&g_clanCs);
    memcpy(g_rkb, tmp, sizeof(tmp[0]) * (size_t) n);
    g_rkbN = n;
    LeaveCriticalSection(&g_clanCs);
    g_rkbDead = 0;
    InterlockedIncrement(&g_rkb_ver);
    static int lg;
    if (LOGCAP(lg, 6))
        Xlog("rankbadge: table of %d players", n);
}

// 1 = known (rk / wk filled). A name the table does not know sets the "ask again soon" flag.
static int RkbLookup(const char *name, int *rk, int *wk)
{
    g_rkbWantAt = GetTickCount64();
    uint32_t h = name ? RkbHash(name) : 0;
    if (!h)
        return 0;
    int hit = 0;
    EnterCriticalSection(&g_clanCs);
    for (int i = 0; i < g_rkbN; i++)
        if (g_rkb[i].h == h) {
            *rk = g_rkb[i].rk;
            *wk = g_rkb[i].wk;
            hit = 1;
            break;
        }
    LeaveCriticalSection(&g_clanCs);
    if (!hit)
        g_rkbMiss = 1;
    return hit;
}

// a row with no player: empty, the page's template texts ("SEARCHING", "PLAYER 3", "-") - never a badge (a stray badge showed on every empty SEARCHING row)
static int RkbPlaceholder(const char *name)
{
    if (!name || !name[0] || !strcmp(name, "-")
        || !strncmp(name, "PLAYER", 6))
        return 1;
    static uint32_t hs;
    if (!hs)
        hs = RkbHash("SEARCHING");
    return RkbHash(name) == hs || !RkbHash(name);
}

// THE badge call: the symbol image `icon` + weeks text `num` (full widget paths) for the player shown as `name`. No player = both hidden. A real name the table does not
// list (yet): the lowest rank (symbol 0, 0 weeks) once the daemon has answered at least once - a new player starts at rank 0 (otherwise unknown players showed no badge).
static void RkbDraw(const char *icon, const char *num, const char *name)
{
    int rk = -1, wk = 0;
    if (RkbPlaceholder(name)) {
        RankUi(icon, num, -1, 0);
        return;
    }
    if (!RkbLookup(name, &rk, &wk)) {
        rk = g_rkb_ver > 0 ? 0 : -1;
        wk = 0;
    }
    RankUi(icon, num, rk, wk);
}

// our own badge, straight from the clan state (0xE1 rk= / wk=), no name needed
static void RkbDrawMe(const char *icon, const char *num)
{
    ClanT c = ClanGet();
    g_rkbWantAt = GetTickCount64();
    RankUi(icon, num, c.ok && c.rk >= 0 ? c.rk : 0, c.ok && c.rk >= 0 ? c.wk : 0);     // no clan state yet = a new player = the lowest rank, never no badge
}

// row helper for a widget group "<base>" holding <leaf_icon> / <leaf_num> (the lobby rows, the friends rows ...)
static void RkbDrawAt(const char *base, const char *leaf_icon,
                        const char *leaf_num, const char *name, int me)
{
    char i[200], n[200];
    snprintf(i, sizeof i, "%s.%s", base, leaf_icon);
    snprintf(n, sizeof n, "%s.%s", base, leaf_num);
    if (me)
        RkbDrawMe(i, n);
    else
        RkbDraw(i, n, name);
}

// ---- scoreboard (the pause menu's and the end-of-match page's team<t>_row<r>_name, written by winmm): the text of each name widget is noted in settext_hook
static uint64_t g_rkbSbw[8];
static char g_rkbSbn[8][48];
static void RkbNote(uint64_t w, const char *t)
{                               // settext_hook: every text write
    if (!w)
        return;
    for (int i = 0; i < 8; i++)
        if (g_rkbSbw[i] == w) {
            snprintf(g_rkbSbn[i], sizeof g_rkbSbn[i], "%s", t ? t : "");
            return;
        }
}

static void RkbSbTick(uint64_t now)
{
    static uint64_t chk, res_at;
    static int up;
    static unsigned gen = ~0u;
    if (now - chk < 120)
        return;
    // perf (feature_friends13() 0.3 ms/frame avg, peaks 3-7 ms = this probe, a FAILING engine path walk every 120 ms of every match): in a match the scoreboard
    // exists only under a menu (the ESC page, hud_menu_up) or on the end-of-match page (lobby end flag +0x2ca4); with neither up and no rows found, nothing to look for
    if (!up && MatchSubstate() == 6 && !HUD_MENU_UP()) {
        uint8_t *lc_ = LobbyObj();
        if (!(lc_ && !IsBadReadPtr(lc_ + 0x2ca4, 1) && lc_[0x2ca4]))
            return;
    }
    chk = now;
    g_settextIn = 1;
    uint64_t probe = UiW("anim-group.scoreboard.team0_row0_name");
    g_settextIn = 0;
    if (!probe) {
        if (up) {               // the page is gone: forget the handles (they may be reused)
            up = 0;
            memset(g_rkbSbw, 0, sizeof g_rkbSbw);
            memset(g_rkbSbn, 0, sizeof g_rkbSbn);
            SbtForget();
        }
        return;
    }
    if (!up || gen != g_uiGen || now - res_at > 4000) {
        g_settextIn = 1;
        for (int t = 0; t < 2; t++)
            for (int r = 0; r < 4; r++) {
                char p[96];
                snprintf(p, sizeof p,
                         "anim-group.scoreboard.team%d_row%d_name", t, r);
                g_rkbSbw[t * 4 + r] = UiW(p);
            }
        g_settextIn = 0;
        up = 1;
        gen = g_uiGen;
        res_at = now;
    }
    SbtTick(now, probe);       // sbteam.c: our team's block on top
    for (int t = 0; t < 2; t++)
        for (int r = 0; r < 4; r++) {
            char ip[120], np[120];
            snprintf(ip, sizeof ip,
                     "anim-group.scoreboard.team%d_row%d_rkIcon", t, r);
            snprintf(np, sizeof np,
                     "anim-group.scoreboard.team%d_row%d_rkNum", t, r);
            const char *nm = g_rkbSbn[t * 4 + r];
            if (!nm[0] || !strncmp(nm, "PLAYER ", 7) || !strcmp(nm, "-")) {
                RankUi(ip, np, -1, 0);
                continue;
            }                   // template / empty row
            RkbDraw(ip, np, nm);
        }
}

// ---- the poll (any frame, menu or match)
static void RkbTick(uint64_t now)
{
    static uint64_t next, dead_at;
    // (two startup requests timed out while the daemon was busy, g_rkb_dead hit 2 and the table was never asked for again - only our own badge showed
    // in the queue): giving up is not for good, a retry follows every 20 s
    if (g_rkbDead >= 2) {
        if (!dead_at)
            dead_at = now;
        else if (now - dead_at > 20000) {
            g_rkbDead = 1;
            dead_at = 0;
        }
    } else
        dead_at = 0;
    if (g_rkbDead < 2 && g_rkbWantAt && now - g_rkbWantAt < 15000
        && now >= next && !g_clan_busy) {
        if (ClanSend(0xBF, NULL, 0)) {
            next = now + (g_rkbMiss ? 2500 : 12000);
            g_rkbMiss = 0;
        }
    }
    RkbSbTick(now);
}
