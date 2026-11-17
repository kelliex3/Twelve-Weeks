// rpcstate.c - in-game Discord Rich Presence: current map, game mode, Supply Raid lives, interrogations done, the safebox fight.
// The ext does not talk to Discord itself: the launcher already holds the Discord connection
// (release/launcher/lib/rpc, also on Linux where the game runs under Proton and cannot reach the native Discord socket). The ext only describes
// what is happening in one small file next to the exe, and the launcher turns it into the card (lib/rpc/ingame.js: texts, translations, images,
// Discord's rate limit). Contract (key=value lines, every key optional, unknown keys ignored by the launcher):
//
//   t2r-rpc-state.txt   v=1  st=menu|search|lobby|vote|gearup|load|match|dead|spec|post
//                       map=<map image slug> mapn=<map name, English>  mode=sr|int  moden=<mode name, English>
//                       lv / elv     Supply Raid team lives, ours / theirs (viewer side)
//                       in / ein / inn  Interrogation: done ours / theirs / needed;  box=1 we are at the enemy safebox, ebox=1 they are at ours
//                       dn / x       my downs (the scoreboard's enemies-downed column) and executions this match
//                       clan / wk / day / sv   faction (WLF / Seraphites), week 1-12, day 1-7, survivors
//                       t0           unix seconds the match started (the card's elapsed timer)
//                       np / npmax   players in the lobby / match of 8, or party members in the menus
//                       win          post match: 1 won, 0 lost (absent: draw / unknown)
//
// The tick only composes the text once a second (plain reads of state the ext already keeps); a worker thread writes it (temp file + rename, so the
// launcher never reads half a file) when it changed, and at least every 10 s as a heartbeat (the launcher ignores a file older than 45 s: a crashed
// game's leftover). English only: the launcher translates, so the card follows the launcher's language. Off: t2r-no-rpc.txt next to the exe (the state
// file is removed). Log: 'rpc:'. Included from t2r-ext.c after death_ui.c (uses g_du, g_int, g_f13, clan_get, mf_get, page_name).

#define RPC_FILE L"t2r-rpc-state.txt"
#define RPC_HEARTBEAT_MS 10000

static struct {
    CRITICAL_SECTION cs;
    HANDLE ev, th;
    char buf[640];              // the text the worker writes next (guarded by cs)
    int dirty, off, init;
    // last match, kept for the post-match pages after the match state leaves 6
    char last_map[40], last_mapn[64], last_mode[8];
    int last_win, last_dn, last_x;
    int64_t t0;                 // unix seconds the running match started (0 = not in a match)
    char st_prev[8];
} g_rpc;

static DWORD WINAPI RpcWriter(LPVOID unused)
{
    (void) unused;
    wchar_t path[MAX_PATH + 32], tmp[MAX_PATH + 40], off[MAX_PATH + 32];
    char last[640] = "", cur[640];
    uint64_t wrote_at = 0;
    for (;;) {
        WaitForSingleObject(g_rpc.ev, 2000);
        if (!g_mlDir[0])
            continue;
        _snwprintf(path, MAX_PATH + 31, L"%ls%ls", g_mlDir, RPC_FILE);
        _snwprintf(tmp, MAX_PATH + 39, L"%ls%ls.tmp", g_mlDir, RPC_FILE);
        _snwprintf(off, MAX_PATH + 31, L"%lst2r-no-rpc.txt", g_mlDir);
        if (GetFileAttributesW(off) != INVALID_FILE_ATTRIBUTES) {
            if (!g_rpc.off) {
                g_rpc.off = 1;
                DeleteFileW(path);
                Xlog("rpc: off (t2r-no-rpc.txt) - no in-game Discord card");
            }
            continue;
        }
        g_rpc.off = 0;
        EnterCriticalSection(&g_rpc.cs);
        memcpy(cur, g_rpc.buf, sizeof cur);
        int dirty = g_rpc.dirty;
        g_rpc.dirty = 0;
        LeaveCriticalSection(&g_rpc.cs);
        uint64_t now = GetTickCount64();
        if (!cur[0]
            || (!dirty && !strcmp(cur, last)
                && now - wrote_at < RPC_HEARTBEAT_MS))
            continue;
        FILE *f = _wfopen(tmp, L"wb");
        if (!f)
            continue;
        size_t n = strlen(cur);
        int ok = fwrite(cur, 1, n, f) == n;
        ok = !fclose(f) && ok;
        if (ok && MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
            if (strcmp(cur, last)) {
                const char *st = strstr(cur, "st="), *e =
                    st ? strchr(st, '\n') : NULL;
                static char st_logged[16];
                char stn[16] = "";
                if (st && e && e - st - 3 < 15) {
                    memcpy(stn, st + 3, e - st - 3);
                    stn[e - st - 3] = 0;
                }
                if (strcmp(stn, st_logged)) {
                    Xlog("rpc: state %s (%u bytes)", stn, (unsigned) n);
                    strcpy(st_logged, stn);
                }
            }
            strcpy(last, cur);
            wrote_at = now;
        }
    }
    return 0;
}

// one key=value line (skipped when the value is empty); values are clipped and lose any line break
static void RpcKv(char *b, size_t cap, const char *k, const char *v)
{
    if (!v || !v[0])
        return;
    size_t n = strlen(b), kl = strlen(k);
    if (n + kl + 3 >= cap)
        return;
    memcpy(b + n, k, kl);
    n += kl;
    b[n++] = '=';
    for (int i = 0; v[i] && i < 60 && n + 2 < cap; i++)
        b[n++] = v[i] == '\r' || v[i] == '\n' ? ' ' : v[i];
    b[n++] = '\n';
    b[n] = 0;
}

static void RpcKi(char *b, size_t cap, const char *k, int v)
{
    char t[16];
    snprintf(t, sizeof t, "%d", v);
    RpcKv(b, cap, k, t);
}

static void RpcCompose(void)
{
    char b[640];
    uint8_t *lo = LobbyObj();
    if (lo && IsBadReadPtr(lo, 0x2cb0))
        lo = NULL;
    int ss = MatchSubstate();
    int rec = lo && lo[0x1b0] == 2;     // the match record (map byte, roster) is valid
    uint32_t me = *(uint32_t *) T2R_SYM(kMySlot);
    const char *pg = PageName(TopPage());
    const char *st;
    // between two match-flow pages (vote -> CHOOSE LOADOUT -> booster -> loading) the stage reads 0 for a moment while the flow code is still on:
    // the card keeps the last stage instead of blinking back to the lobby (seen as vote, lobby, gearup, lobby within 6 s)
    static int flow_last;
    int flow =
        g_mfStageNow ? g_mfStageNow : g_mfCodeNow >=
        100 ? flow_last : 0;
    flow_last = ss == 6 ? 0 : flow;
    if (ss == 6 && lo)
        st = lo[0x2ca4] ? "post" : lo[0x2c44] ? "spec" : g_du.phase ==
            1 ? "dead" : "match";
    else if (ss >= 3 && ss <= 5)
        st = "load";
    else if (flow == 1)
        st = "vote";
    else if (flow == 2)
        st = "gearup";
    else if (flow == 3)
        st = "load";
    else if (pg && !strcmp(pg, "clan-postmatch") && g_rpc.last_mapn[0])
        st = "post";
    else if (rec && lo && *(int *) (lo + 0x1b4) == 2)
        st = "lobby";
    else if (*(volatile int *) T2R_SYM(kPartyStatus) == 3
             || *(volatile uint8_t *) T2R_SYM(kMatchmakingFlag) || (pg
                                                                    &&
                                                                    !strcmp
                                                                    (pg,
                                                                     "matchmaking-menu")))
        st = "search";
    else
        st = "menu";

    // match start / end edges: elapsed timer, last match kept for the post-match pages
    if (ss == 6 && lo) {
        int64_t now = (int64_t) time(NULL), el = -1;
        float dur = *(float *) (lo + 0x2b8c), rem =
            *(float *) (lo + 0x2b90);
        if (lo[0x2b94] && dur > 0 && dur < 36000 && rem >= 0
            && rem <= dur + 1)
            el = (int64_t) (dur - rem);
        int64_t t0 = el >= 0 ? now - el : (g_rpc.t0 ? g_rpc.t0 : now);
        if (!g_rpc.t0 || llabs(t0 - g_rpc.t0) > 5)
            g_rpc.t0 = t0;      // stable: only a real jump (clock set, late join) moves it
    } else if (strcmp(st, "post"))
        g_rpc.t0 = 0;

    b[0] = 0;
    RpcKv(b, sizeof b, "v", "1");
    RpcKv(b, sizeof b, "st", st);
    int in_match = ss == 6 && lo;
    if (in_match || !strcmp(st, "lobby") || !strcmp(st, "gearup")
        || !strcmp(st, "load")) {
        // map: the match record's map byte; during the vote nothing is decided yet (the winner once the vote closed)
        int mi = -1;
        if (rec)
            mi = lo[0x18];
        if (!in_match && g_mfStageNow) {
            MfT m = MfGet();
            mi = m.ok && m.st == 2 ? m.w : g_mfStageNow == 1 ? -1 : mi;
        }
        if (mi >= 0) {
            const char *nm = MfMapName(mi), *img = MfMapImg(mi);
            RpcKv(b, sizeof b, "map", img);
            RpcKv(b, sizeof b, "mapn", nm);
            if (in_match) {
                snprintf(g_rpc.last_map, sizeof g_rpc.last_map, "%s",
                         img ? img : "");
                snprintf(g_rpc.last_mapn, sizeof g_rpc.last_mapn, "%s",
                         nm ? nm : "");
            }
        }
    } else if (!strcmp(st, "post")) {
        RpcKv(b, sizeof b, "map", g_rpc.last_map);
        RpcKv(b, sizeof b, "mapn", g_rpc.last_mapn);
    }
    if (in_match) {
        // mode: unknown until the server's first Interrogation reply (no Supply Raid flash at an Interrogation start)
        const char *mode = R13IntMode()? "int" : g_int.ok || (lo[0x2b94]
                                                                && *(float
                                                                     *) (lo
                                                                         +
                                                                         0x2b8c)
                                                                -
                                                                *(float
                                                                  *) (lo +
                                                                      0x2b90)
                                                                >
                                                                8) ? "sr" :
            NULL;
        if (mode) {
            RpcKv(b, sizeof b, "mode", mode);
            RpcKv(b, sizeof b, "moden",
                   !strcmp(mode, "int") ? "Interrogation" : "Supply Raid");
            strcpy(g_rpc.last_mode, mode);
        }
        if (mode && !strcmp(mode, "sr")) {
            int a = ((int (*)(void *)) T2R_SYM(kFnTeamLives)) (lo), e =
                ((int (*)(void *)) T2R_SYM(kFnTeamLivesEnabled)) (lo);
            if (a >= 0 && a < 1000 && e >= 0 && e < 1000) {
                RpcKi(b, sizeof b, "lv", a);
                RpcKi(b, sizeof b, "elv", e);
            }
        } else if (mode && g_int.ok) {
            int t = g_int.team & 1, need = g_int.need > 0 ? g_int.need : 5;
            RpcKi(b, sizeof b, "in", g_int.p[t]);
            RpcKi(b, sizeof b, "ein", g_int.p[t ^ 1]);
            RpcKi(b, sizeof b, "inn", need);
            if (!g_int.ended && g_int.p[t] >= need)
                RpcKv(b, sizeof b, "box", "1");        // our intel is complete: fighting for their safebox
            else if (!g_int.ended && g_int.p[t ^ 1] >= need)
                RpcKv(b, sizeof b, "ebox", "1");       // theirs is: defending ours
        }
        if (me < (uint32_t) T2R_ENGINE_PEERS && me < T2R_MAX_PEERS) {
            int dn = *(int32_t *) (SB_STATS + me * 12), x =
                (int) g_sbExec[me * 3];
            if (dn >= 0 && dn < 1000) {
                RpcKi(b, sizeof b, "dn", dn);
                g_rpc.last_dn = dn;
            }
            if (x >= 0 && x < 1000) {
                RpcKi(b, sizeof b, "x", x);
                g_rpc.last_x = x;
            }
        }
        g_rpc.last_win = -1;
        if (lo[0x2ca4] && me < T2R_MAX_PEERS) {
            int win = *(int *) (lo + 0x2ca0);
            g_rpc.last_win = win >= 0
                && win <= 1 ? win == TeamOfSlot(me) : -1;
        }
        if (g_rpc.t0) {
            char t[24];
            snprintf(t, sizeof t, "%lld", (long long) g_rpc.t0);
            RpcKv(b, sizeof b, "t0", t);
        }
    } else if (!strcmp(st, "post")) {
        if (g_rpc.last_mode[0]) {
            RpcKv(b, sizeof b, "mode", g_rpc.last_mode);
            RpcKv(b, sizeof b, "moden",
                   !strcmp(g_rpc.last_mode,
                           "int") ? "Interrogation" : "Supply Raid");
        }
        RpcKi(b, sizeof b, "dn", g_rpc.last_dn);
        RpcKi(b, sizeof b, "x", g_rpc.last_x);
    }
    if (!strcmp(st, "post") && g_rpc.last_win >= 0)
        RpcKi(b, sizeof b, "win", g_rpc.last_win);
    if (!strcmp(st, "menu") || !strcmp(st, "search")) { // a new match: the last one's leftovers go
        if (!strcmp(st, "search")) {
            g_rpc.last_mapn[0] = g_rpc.last_map[0] = g_rpc.last_mode[0] =
                0;
            g_rpc.last_win = -1;
        }
        if (!strcmp(st, "search") && g_mdpk.have && g_mdpk.mode >= 0
            && g_mdpk.mode < MODE_N)
            RpcKv(b, sizeof b, "moden",
                   g_mdpk.mode == 1 ? "Interrogation" : "Supply Raid");
    }
    // players: the match roster (lobby / match), else the party in the menus
    if (rec && (in_match || !strcmp(st, "lobby") || !strcmp(st, "gearup"))) {
        int np = lo[0x1a];
        if (np > 0 && np <= 8) {
            RpcKi(b, sizeof b, "np", np);
            RpcKi(b, sizeof b, "npmax", 8);
        }
    } else if ((!strcmp(st, "menu") || !strcmp(st, "search"))
               && FrInParty()) {
        int pmax = g_f13.pmax ? g_f13.pmax : 4;
        if (g_f13.nm <= pmax) {
            RpcKi(b, sizeof b, "np", g_f13.nm);
            RpcKi(b, sizeof b, "npmax", pmax);
        }
    }
    // clan: faction, week / day of the twelve weeks, survivors
    ClanT c = ClanGet();
    if (c.ok && (c.fac == 1 || c.fac == 2)) {
        RpcKv(b, sizeof b, "clan", c.fac == 1 ? "WLF" : "Seraphites");
        if (c.st == 1) {
            int d = c.day < 84 ? (c.day < 0 ? 0 : c.day) : 83;
            RpcKi(b, sizeof b, "wk", d / 7 + 1);
            RpcKi(b, sizeof b, "day", d % 7 + 1);
            int Sv = c.h + c.hu + c.s;
            if (Sv > 0 && Sv < 10000)
                RpcKi(b, sizeof b, "sv", Sv);
        }
    }
    EnterCriticalSection(&g_rpc.cs);
    if (strcmp(b, g_rpc.buf)) {
        memcpy(g_rpc.buf, b, sizeof b);
        g_rpc.dirty = 1;
    }
    int d = g_rpc.dirty;
    LeaveCriticalSection(&g_rpc.cs);
    if (d && strcmp(st, g_rpc.st_prev))
        SetEvent(g_rpc.ev);     // a new screen goes out at once; other changes on the worker's 2 s wake
    snprintf(g_rpc.st_prev, sizeof g_rpc.st_prev, "%s", st);
}

static void RpcTick(void)
{
    if (!g_rpc.init) {
        if (!g_mlDir[0])
            return;
        g_rpc.init = 1;
        g_rpc.last_win = -1;
        InitializeCriticalSection(&g_rpc.cs);
        g_rpc.ev = CreateEventW(NULL, FALSE, FALSE, NULL);
        g_rpc.th =
            g_rpc.ev ? CreateThread(NULL, 64 * 1024, RpcWriter, NULL, 0,
                                    NULL) : NULL;
        if (!g_rpc.th) {
            g_rpc.init = -1;
            Xlog("rpc: no writer thread - no in-game Discord card");
            return;
        }
        Xlog("rpc: in-game Discord state -> %ls%ls (the launcher shows it)", g_mlDir, RPC_FILE);
    }
    if (g_rpc.init < 0 || g_rpc.off)
        return;
    static uint64_t next;
    uint64_t now = GetTickCount64();
    if (now < next)
        return;
    next = now + 1000;
    RpcCompose();
}
