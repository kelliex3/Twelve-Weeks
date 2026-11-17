// requeue.c - the queue stays after a match: after a match ends, everybody in that
// queue goes straight to finding match (the lobby waits for everyone to be done with the post-match summary screens etc.), and the text changes from
// "Finding match.." to "Waiting for players.." for that duration. Included after friends13.c. Log prefix 'requeue:'.
//
// Server side: tools/requeue.py + daemon MatchMaker.end - every player still in the match at its end whose ext asked with 0xE3 flag 0x20 (this file is
// compiled in, jip_ask) gets a ticket; its next search is HELD until every other ticket holder of that match searches too (or went offline / cancelled /
// 120 s passed). Client side, here:
//   1. armed when the match ended while we were in it (match state 6 + the match-over byte lobby +0x2ca4: the end banner / scoreboard hold). A kick (admin /
//      AFK: msg_hook g_kick_ms) or leaving before the end never arms it; 300 s after the end it is given up (the server's ticket is gone by then too).
//   2. back on the MP menu (to1/main-menu on top, session state 2) with every post-match screen done - the scoreboard (clan13x), the supply counter
//      (to1/clan-postmatch), a clan event screen, no GEAR UP navigation - and 1.5 s settled: ask the daemon (op 0xBC sub 0, the mode-count poll; its 0xBD
//      reply carries "rq=1" while we hold a ticket). Yes: push to1/find-match-menu like move_rejoin_tick / the party follow do (winmm's page tick starts the
//      search; the daemon keeps our mode pick). An old daemon (no rq key) / no ticket: nothing happens, the player presses FIND MATCH as before.
//      A party member never pushes: the leader's search takes the ready members along (friends13.c fr_party_tick).
//   3. on the search page while a ticket is open: the same poll every 1.5 s; "rqw=<n>" (n > 0: our search waits for n players still on their post-match
//      screens) = the status label reads WAITING FOR PLAYERS... instead of winmm's FINDING MATCH... (settext_hook swaps the text, PL row in loc_pl.h).
// Cancelling the search (back on the search page) works as before: winmm sends 0x02, the daemon drops the ticket, the next poll clears the text.
// Off: t2r-no-requeue.txt next to the exe (no flag 0x20, no auto search, no text swap).
#define RQ_ARM_MS 300000        // = the daemon's T2R_REQUEUE_TICKET_S default (tools/requeue.py ticket_s)
#define RQ_SETTLE_MS 1500       // MP menu on top this long before the push (move_rejoin_tick waits 1.2 s; the clan pages push within 1 s of the menu)
#define RQ_POLL_MS 1500         // the waiting-text poll on the search page (modepick polls its counts at the same rate)
static struct {
    uint64_t armed, end_at, menu_since, asked_at, poll_at, chk;
    int off, ans_rq, ans_w, asked, pushed;
} g_rq = {.off = -1,.ans_rq = -1 };

static volatile LONG g_rqp_busy, g_rqp_have;
static uint8_t g_rqpReq[66];
static char g_rqpRep[160];
static int RqOff(void)
{
    uint64_t now = GetTickCount64();
    if (g_rq.off < 0 || now - g_rq.chk > 5000) {
        g_rq.chk = now;
        FILE *f = ExeSideOpen("t2r-no-requeue.txt", "r");
        int o = f != NULL;
        if (f)
            fclose(f);
        if (o != g_rq.off)
            Xlog("requeue: %s",
                 o ? "off (t2r-no-requeue.txt)" :
                 "on (the queue stays after a match)");
        g_rq.off = o;
    }
    return g_rq.off;
}

static DWORD WINAPI RqpWorker(LPVOID arg)
{
    (void) arg;
    HMODULE ws = LoadLibraryA("ws2_32.dll");
    T2rSin a;
    T2rNet net;
    LONG res = 2;
    if (ws && HmServer(&a) && T2rNetLoad(&net, ws)) {
        uint8_t frame[3 + sizeof g_rqpReq];
        int n = (int) sizeof g_rqpReq + 1;
        frame[0] = (uint8_t) n;
        frame[1] = 0;
        frame[2] = 0xBC;
        memcpy(frame + 3, g_rqpReq, sizeof g_rqpReq);
        char rep[sizeof g_rqpRep + 4];
        int got =
            T2rRpc(&net, &a, frame, n + 2, rep, (int) sizeof rep - 1,
                    2000, 2000, 2000, NULL);
        int ln = got >= 3 ? (rep[0] & 0xff) + ((rep[1] & 0xff) << 8) : 0;
        if (ln >= 1 && got >= ln + 2 && (uint8_t) rep[2] == 0xBD
            && ln - 1 < (int) sizeof g_rqpRep) {
            memcpy(g_rqpRep, rep + 3, (size_t) ln - 1);
            g_rqpRep[ln - 1] = 0;
            res = 1;
        }
    }
    InterlockedExchange(&g_rqp_have, res);
    InterlockedExchange(&g_rqp_busy, 0);
    return 0;
}

static int RqpAsk(void)
{                               // op 0xBC [token][sub 0 = poll][0]: no side effect on the daemon
    static char tok[65];
    if (!tok[0] && !ClanToken(tok)) {
        tok[0] = 0;
        return 0;
    }
    if (g_rqp_have || InterlockedCompareExchange(&g_rqp_busy, 1, 0))
        return 0;
    memcpy(g_rqpReq, tok, 64);
    g_rqpReq[64] = 0;
    g_rqpReq[65] = 0;
    HANDLE h = CreateThread(NULL, 0, RqpWorker, NULL, 0, NULL);
    if (!h) {
        InterlockedExchange(&g_rqp_busy, 0);
        return 0;
    }
    CloseHandle(h);
    return 1;
}

static void RqpParse(const char *t, int *rq, int *w)
{                               // "... rq=<0|1> rqw=<n>" (an old daemon sends neither: no ticket)
    char k[16];
    int v, n;
    *rq = 0;
    *w = 0;
    while (sscanf(t, " %15[^= ]=%d%n", k, &v, &n) == 2) {
        if (!strcmp(k, "rq"))
            *rq = v;
        else if (!strcmp(k, "rqw"))
            *w = v;
        t += n;
    }
}

// the status label of the search page shows winmm's text again through settext_hook (which swaps it while g_rq_waiting is set)
static void RqLabelRefresh(void)
{
    typedef void (*fn_settext)(uint64_t, const char *);
    uint64_t w = UiW("anim-group.statusLabel");
    if (w)
        ((fn_settext) T2R_SYM(kFnSetText)) (w, "FINDING MATCH...");
}

static void RqDisarm(const char *why)
{
    if (g_rq.armed)
        Xlog("requeue: %s", why);
    g_rq.armed = 0;
    g_rq.menu_since = 0;
    g_rq.asked = 0;
}

static void RequeueTick(void)
{
    static uint64_t P_MM, P_FM;
    if (!P_MM) {
        P_MM = PageId("main-menu");
        P_FM = PageId("find-match-menu");
    }
    uint64_t now = GetTickCount64();
    int off = RqOff();
    uint8_t sub = MatchSubstate();
    uint8_t *lo = LobbyObj();
    if (!off && sub == 6 && lo && !IsBadReadPtr(lo + 0x2ca4, 1) && lo[0x2ca4]) {        // 1. the match ended while we were in it
        if (!g_rq.armed)
            Xlog("requeue: match over - we search again by ourselves once the post-match screens are done (if the server keeps our ticket)");
        g_rq.armed = g_rq.end_at = now;
        g_rq.pushed = 0;
        g_rq.asked = 0;
        g_rq.menu_since = 0;
    }
    int rq = -1, w = 0;
    if (g_rqp_have) {
        if (g_rqp_have == 1)
            RqpParse(g_rqpRep, &rq, &w);
        InterlockedExchange(&g_rqp_have, 0);
        g_rq.ans_rq = rq;
        g_rq.ans_w = rq == 1 ? w : 0;
    }
    if (g_rq.armed && g_kickMs > g_rq.armed)
        RqDisarm("kicked from the match - no requeue");
    if (g_rq.armed && now - g_rq.armed > RQ_ARM_MS)
        RqDisarm("no MP menu within 300 s of the match end - given up");
    int st = *(volatile int *) T2R_SYM(kPartyStatus);
    uint64_t top = TopPage();
    // 3. WAITING FOR PLAYERS... while the daemon holds our search for the others of the last match
    int onfm = !off && top == P_FM && st == 3 && sub != 6 && g_rq.pushed
        && now - g_rq.pushed < RQ_ARM_MS;
    if (onfm && !g_rqp_busy && now >= g_rq.poll_at) {
        g_rq.poll_at = now + RQ_POLL_MS;
        RqpAsk();
    }
    int wt = onfm && g_rq.ans_rq == 1 && g_rq.ans_w > 0;
    if (!onfm && g_rq.pushed && top != P_FM && now - g_rq.pushed > 3000)
        g_rq.pushed = 0;        // (left the search page: cancelled or matched; the push needs a few frames)
    if (wt != g_rqWaiting) {
        g_rqWaiting = wt;
        Xlog("requeue: search page reads %s%s",
             wt ? "WAITING FOR PLAYERS..." : "FINDING MATCH...",
             wt ? "" : " again");
        if (wt) {
            static int nw;
            if (nw++ < 40)
                Xlog("requeue: waiting for %d player(s) still on the post-match screens", g_rq.ans_w);
        }
        if (top == P_FM && st == 3)
            RqLabelRefresh();
    }
    // 2. back on the MP menu, every post-match screen done: search again
    if (!g_rq.armed || g_rq.pushed)
        return;
    if (off) {
        RqDisarm("off");
        return;
    }
    if (sub == 6 || top != P_MM || st != 2 || g_pm.pending
        || Cl13PostActive() || g_ev.pending || g_loState != LO_NONE
        || g_mmBack) {
        g_rq.menu_since = 0;
        g_rq.asked = 0;
        return;
    }
    if (g_steamOnly && !SteamConnected()) {
        g_rq.menu_since = 0;
        return;
    }                           // (FIND MATCH is refused then anyway)
    if (FrInParty() && !g_f13.me_leader) {
        RqDisarm
            ("party member - the leader's search takes us along (ready state kept)");
        return;
    }
    if (!g_rq.menu_since) {
        g_rq.menu_since = now;
        return;
    }
    if (now - g_rq.menu_since < RQ_SETTLE_MS)
        return;
    if (!g_rq.asked) {
        if (!g_rqp_busy && RqpAsk()) {
            g_rq.asked = 1;
            g_rq.asked_at = now;
            g_rq.ans_rq = -2;
        }
        return;
    }
    if (g_rq.ans_rq == -2) {    // the answer is on its way
        if (now - g_rq.asked_at > 8000)
            RqDisarm
                ("the server did not answer the ticket check - press FIND MATCH");
        return;
    }
    if (g_rq.ans_rq != 1) {
        RqDisarm(g_rq.ans_rq ==
                  0 ?
                  "the server has no ticket for us (cancelled / timed out / older server) - nothing pushed"
                  : "no answer from the server - nothing pushed");
        return;
    }
    g_rq.armed = 0;
    g_rq.pushed = now;
    g_rq.poll_at = now + 500;
    g_pqAuto = now;            // (a party leader's own push: fr_search_gate lets it through like the follow push)
    PushPageNow("to1/find-match-menu");
    Xlog("requeue: post-match screens done, %.1f s after the match end - search page pushed (same mode; the lobby waits for the others of the last match)", (now - g_rq.end_at) / 1000.0);
}
