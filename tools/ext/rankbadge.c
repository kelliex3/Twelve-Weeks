// rankbadge.c - the 2013 rank badge (chalk symbol + weeks survived) in FRONT OF EVERY PLAYER NAME, one look on every screen
// (the badge shows in the lobby and on every screen, consistently).
// Look: tools/features/rank_badge_widgets.py (one geometry function: symbol = the name's point size, weeks number 0.62 x, the name moves right by the same width on every
// screen). Drawing: rank_ui (rank13.h) on an image + text widget pair; this file only finds out WHOSE rank it is.
// Data: the daemon's rank table (op 0xBF -> reply 0xBF "n=<k> HHHHHHHH:r:w ...", tools/rankbadge.py: one entry per recently seen player, keyed by a hash of the SHOWN
// name = ASCII letters / digits only, lower case, so "[TAG] Name", the vote page's "_TAG__Name" and the lobby's " (party lead)" suffix reach the same key). Polled
// every 12 s (2.5 s while a name was missing) as long as some screen asked for a rank in the last 15 s. The map vote's roster carries its ranks itself (0xEB pk<i> / pw<i>).
// Included after rank13.h, before clan13.c / friends13.c (the lobby rows and the friends panel call rkb_draw / rkb_draw_me / rkb_draw_at).
#define RKB_MAX 400
static struct { uint32_t h; uint8_t rk, ro; uint16_t wk; } g_rkb[RKB_MAX];     // guarded by g_clan_cs; ro = role (tools/roles.py): 0 none 1 OG 2 admin 3 dev 4 lead dev
static int g_rkb_n;
static volatile LONG g_rkb_ver, g_rkb_dead;                                  // g_rkb_dead: an older daemon ignores the op (2 unanswered tries stop the polling)
static volatile uint64_t g_rkb_want_at;                                      // GetTickCount64 of the last lookup
static volatile LONG g_rkb_miss;                                             // the last lookup found nothing: ask again soon

static void nrim_tick(uint64_t now);                                        // namerim.c (included after friends13.c)
// 11-23: a name with no ASCII letter / digit (">,..,<", all-Cyrillic) hashed to 0 = drawn as an empty row (no badge). It is keyed on its other bytes instead
// (> 0x20, not one of " _[]()<>"), the same fallback as tools/rankbadge.py key().
static uint32_t rkb_hash(const char *name) {                                // = tools/rankbadge.py key()
    uint32_t h = 2166136261u, h2 = 2166136261u; int any = 0, any2 = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (*p == '<') { const char *e = strchr((const char *)p, '>'); if (e) { p = (const unsigned char *)e; continue; } }      // <font ...> markup
        if (p[0] == ' ') {                                                  // suffixes "  (YOU)" (friends panel: two spaces), " (party lead)", " (not searching)", "  (TY)"
            const unsigned char *q = p; while (*q == ' ') q++;
            if (*q == '(' && (!strncmp((const char *)q + 1, "party", 5) || !strncmp((const char *)q + 1, "YOU", 3) || !strncmp((const char *)q + 1, "TY", 2) || !strncmp((const char *)q + 1, "not searching", 13))) break;
        }
        if (p[0] == ' ' && p[1] == '[' && p[2] == 'T' && p[3] >= '1' && p[3] <= '2' && p[4] == ']') { p += 4; continue; }              // " [T1]"
        int c = *p;
        if (c >= 'A' && c <= 'Z') c += 32;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) {
            if (*p > 0x20 && !strchr("_[]()<>", *p)) { h2 = (h2 ^ (uint32_t)*p) * 16777619u; any2 = 1; }
            continue;
        }
        h = (h ^ (uint32_t)c) * 16777619u; any = 1;
    }
    return any ? h : any2 ? h2 : 0;
}
static void rkb_parse(const char *t) {                                       // worker thread (clan_worker)
    static struct { uint32_t h; uint8_t rk, ro; uint16_t wk; } tmp[RKB_MAX];
    int n = 0;
    while (*t && n < RKB_MAX) {
        while (*t == ' ') t++;
        unsigned h; int r, w, used = 0;
        if (sscanf(t, "%8x:%d:%d%n", &h, &r, &w, &used) == 3 && used > 0) {
            tmp[n].h = h; tmp[n].rk = (uint8_t)(r < 0 ? 0 : r > 9 ? 9 : r); tmp[n].wk = (uint16_t)(w < 0 ? 0 : w > 999 ? 999 : w); tmp[n].ro = 0;
            t += used;
            int ro, u2 = 0;                                                  // optional 4th field ":role" (tools/rankbadge.py wire)
            if (*t == ':' && sscanf(t, ":%d%n", &ro, &u2) == 1 && u2 > 0) { tmp[n].ro = (uint8_t)(ro < 0 || ro > 4 ? 0 : ro); t += u2; }
            n++;
        } else { const char *s = strchr(t, ' '); if (!s) break; t = s; }       // "n=<k>" or junk: skip the word
    }
    EnterCriticalSection(&g_clan_cs);
    memcpy(g_rkb, tmp, sizeof(tmp[0]) * (size_t)n);
    g_rkb_n = n;
    LeaveCriticalSection(&g_clan_cs);
    g_rkb_dead = 0;
    InterlockedIncrement(&g_rkb_ver);
    static int lg; if (LOGCAP(lg, 6)) xlog("rankbadge: table of %d players", n);
}
// 1 = known (rk / wk / ro filled). A name the table does not know sets the "ask again soon" flag.
static int rkb_lookup_ro(const char *name, int *rk, int *wk, int *ro) {
    g_rkb_want_at = GetTickCount64();
    uint32_t h = name ? rkb_hash(name) : 0;
    if (!h) return 0;
    int hit = 0;
    EnterCriticalSection(&g_clan_cs);
    for (int i = 0; i < g_rkb_n; i++) if (g_rkb[i].h == h) { *rk = g_rkb[i].rk; *wk = g_rkb[i].wk; if (ro) *ro = g_rkb[i].ro; hit = 1; break; }
    LeaveCriticalSection(&g_clan_cs);
    if (!hit) g_rkb_miss = 1;
    return hit;
}
static int rkb_lookup(const char *name, int *rk, int *wk) { return rkb_lookup_ro(name, rk, wk, NULL); }
// the role of the player shown as `name` (0 = none / unknown); the nametags ask for it (feature_nametags: the lead dev's pink outline)
static int rkb_role_of(const char *name) { int rk, wk, ro = 0; return rkb_lookup_ro(name, &rk, &wk, &ro) ? ro : 0; }
// a row with no player: empty, the page's template texts ("SEARCHING", "PLAYER 3", "-") - never a badge (a stray badge showed on every empty SEARCHING row)
static int rkb_placeholder(const char *name) {
    if (!name || !name[0] || !strcmp(name, "-") || !strncmp(name, "PLAYER", 6)) return 1;
    static uint32_t hs; if (!hs) hs = rkb_hash("SEARCHING");
    return rkb_hash(name) == hs || !rkb_hash(name);
}
// THE badge call: the symbol image `icon` + weeks text `num` (full widget paths) for the player shown as `name`. No player = both hidden. A real name the table does not
// list (yet): the lowest rank (symbol 0, 0 weeks) once the daemon has answered at least once - a new player starts at rank 0 (otherwise unknown players showed no badge).
static void rkb_draw(const char *icon, const char *num, const char *name) {
    int rk = -1, wk = 0, ro = 0;
    if (rkb_placeholder(name)) { rank_ui(icon, num, -1, 0); return; }
    if (!rkb_lookup_ro(name, &rk, &wk, &ro)) { rk = g_rkb_ver > 0 ? 0 : -1; wk = 0; ro = 0; }
    rank_ui_role(icon, num, rk, wk, ro);
}
// our own badge, straight from the clan state (0xE1 rk= / wk= / ro=), no name needed
static void rkb_draw_me(const char *icon, const char *num) {
    clan_t c = clan_get();
    g_rkb_want_at = GetTickCount64();
    rank_ui_role(icon, num, c.ok && c.rk >= 0 ? c.rk : 0, c.ok && c.rk >= 0 ? c.wk : 0, c.ok ? c.ro : 0);      // no clan state yet = a new player = the lowest rank, never no badge
}
// row helper for a widget group "<base>" holding <leaf_icon> / <leaf_num> (the lobby rows, the friends rows ...)
static void rkb_draw_at(const char *base, const char *leaf_icon, const char *leaf_num, const char *name, int me) {
    char i[200], n[200];
    snprintf(i, sizeof i, "%s.%s", base, leaf_icon); snprintf(n, sizeof n, "%s.%s", base, leaf_num);
    if (me) rkb_draw_me(i, n); else rkb_draw(i, n, name);
}

// ---- scoreboard (the pause menu's and the end-of-match page's team<t>_row<r>_name, written by winmm): the text of each name widget is noted in settext_hook
static uint64_t g_rkb_sbw[8];
static char g_rkb_sbn[8][48];
static void rkb_note(uint64_t w, const char *t) {                          // settext_hook: every text write
    if (!w) return;
    for (int i = 0; i < 8; i++)
        if (g_rkb_sbw[i] == w) { snprintf(g_rkb_sbn[i], sizeof g_rkb_sbn[i], "%s", t ? t : ""); return; }
}
static void rkb_sb_tick(uint64_t now) {
    static uint64_t chk, res_at; static int up; static unsigned gen = ~0u;
    if (now - chk < 120) return;
    // perf (feature_friends13() 0.3 ms/frame avg, peaks 3-7 ms = this probe, a FAILING engine path walk every 120 ms of every match): in a match the scoreboard
    // exists only under a menu (the ESC page, hud_menu_up) or on the end-of-match page (lobby end flag +0x2ca4); with neither up and no rows found, nothing to look for
    if (!up && match_substate() == 6 && !hud_menu_up()) { uint8_t *lc_ = lobby_obj(); if (!(lc_ && !IsBadReadPtr(lc_ + 0x2ca4, 1) && lc_[0x2ca4])) return; }
    chk = now;
    g_settext_in = 1;
    uint64_t probe = ui_w("anim-group.scoreboard.team0_row0_name");
    g_settext_in = 0;
    if (!probe) {
        if (up) {                                                            // the page is gone: forget the handles (they may be reused)
            up = 0; memset(g_rkb_sbw, 0, sizeof g_rkb_sbw); memset(g_rkb_sbn, 0, sizeof g_rkb_sbn); sbt_forget();
        }
        return;
    }
    if (!up || gen != g_ui_gen || now - res_at > 4000) {
        g_settext_in = 1;
        for (int t = 0; t < 2; t++)
            for (int r = 0; r < 4; r++) { char p[96]; snprintf(p, sizeof p, "anim-group.scoreboard.team%d_row%d_name", t, r); g_rkb_sbw[t * 4 + r] = ui_w(p); }
        g_settext_in = 0;
        up = 1; gen = g_ui_gen; res_at = now;
    }
    sbt_tick(now, probe);                                                     // sbteam.c: our team's block on top
    for (int t = 0; t < 2; t++)
        for (int r = 0; r < 4; r++) {
            char ip[120], np[120];
            snprintf(ip, sizeof ip, "anim-group.scoreboard.team%d_row%d_rkIcon", t, r);
            snprintf(np, sizeof np, "anim-group.scoreboard.team%d_row%d_rkNum", t, r);
            const char *nm = g_rkb_sbn[t * 4 + r];
            if (!nm[0] || !strncmp(nm, "PLAYER ", 7) || !strcmp(nm, "-")) { rank_ui(ip, np, -1, 0); continue; }   // template / empty row
            rkb_draw(ip, np, nm);
        }
}

// ---- the poll (any frame, menu or match)
static void rkb_tick(uint64_t now) {
    static uint64_t next, dead_at;
    // (two startup requests timed out while the daemon was busy, g_rkb_dead hit 2 and the table was never asked for again - only our own badge showed
    // in the queue): giving up is not for good, a retry follows every 20 s
    if (g_rkb_dead >= 2) { if (!dead_at) dead_at = now; else if (now - dead_at > 20000) { g_rkb_dead = 1; dead_at = 0; } } else dead_at = 0;
    if (g_rkb_dead < 2 && g_rkb_want_at && now - g_rkb_want_at < 15000 && now >= next && !g_clan_busy) {
        if (clan_send(0xBF, NULL, 0)) { next = now + (g_rkb_miss ? 2500 : 12000); g_rkb_miss = 0; }
    }
    rkb_sb_tick(now);
    nrim_tick(now);                                                           // namerim.c: the lead dev's pink rim on the lobby / scoreboard names
}
