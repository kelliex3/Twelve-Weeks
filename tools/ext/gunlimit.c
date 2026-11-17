// gunlimit.c - per-team gun limits for t2r-ext (e.g. a Tactical Shotgun limit of 1 per team, to prevent the tac shotgun
// meta). Included from t2r-ext.c next to interrogation.c (uses its statics: loadout_t, lo_of, lo_read_obj, lo_write_obj, ...).
//
// The DAEMON is the arbiter (tools/gunlimit.py, op 0xDE / 0xDF): this file reports the limited guns of the loadout the player
// intends to spawn with (the pending pick, else the lobby object's) every 2 s in a match - 0.5 s while the loadout list is open -
// and at once when the pick changes, and shows what the daemon answers:
//   - full = limited guns the other teammates hold up to the max: every class / custom loadout containing one is dimmed in the
//     CHOOSE LOADOUT / CHANGE LOADOUT list (row cost "TOO MANY", the description starts "Too many of <GUN> on team."), and
//     selecting it is refused with a buzzer. Outside a match (GEAR UP from the main menu: g_lo_state LO_LIST / LO_EDIT, or no
//     live match) nothing is restricted.
//   - deny = a claim the daemon refused (two teammates picked at the same instant / an old pick / a custom loadout loaded at
//     match start): the server decision wins - the player is switched to the first preset class without a taken limited gun
//     (before the spawn: written at once; after it: pending for the next life) with a message.
//   - spawnkit_hook calls gl_spawn_fix() first: a loadout that would spawn with a gun the cache says is taken is swapped for that
//     fallback class before the spawn kit runs.
// The holder is released by claiming a loadout without the gun, leaving, or the match ending (the daemon prunes it).
// PRE-MATCH: the claims start the moment the match is formed (lobby countdown = team known: gl_countdown) and the
// daemon ranks them (earliest claim wins, tie = lowest slot; answers `pend` for ~0.6 s first). A refused claim switches the player to
// the first preset class without the gun BEFORE the first spawn, with "Only one Tactical Shotgun per team - a teammate already has it.
// Your loadout was switched to <CLASS>." CHOOSE LOADOUT (and GEAR UP opened during the countdown) dims 'TOO MANY' rows. gl_spawn_fix
// uses the answer gl_tick prefetched on the worker thread (gl_sync is non-blocking); no cached decision
// = a held gun is kept, else the fallback class (never risk a second gun). Log lines: "gunlimit: pre-match ...".
// Switch: t2r-no-gunlimit.txt next to the exe = off (nothing is claimed, nothing dimmed).
// The table order is the wire's bit order and must equal tools/gunlimit.py LIMITS (tools/tests/test_gunlimit.py compares).
static const struct {
    const char *ids[3];
    int max;
    const char *name;
} GL_TABLE[] = {
    { { "shotgun-tactical", "shotgun-tactical-supp"}, 1,
     "TACTICAL SHOTGUN" },
};

#define GL_N ((int)(sizeof GL_TABLE / sizeof *GL_TABLE))

static struct {
    int on, team;
    uint32_t held, deny, full, pend, asked, answered;
    uint64_t at, sent;
} g_gl;
static uint32_t g_glReqmask;   // the mask of the request in flight (the reply answers exactly this)
static volatile LONG g_gl_busy, g_gl_have;
static uint8_t g_glReq[66];
static char g_glRep[256];
static int g_noGl = -1;

static int GlOff(void)
{
    if (g_noGl < 0) {
        FILE *f = ExeSideOpen("t2r-no-gunlimit.txt", "r");
        g_noGl = f != NULL;
        if (f)
            fclose(f);
        Xlog("gunlimit: %s",
             g_noGl ? "off (t2r-no-gunlimit.txt)" :
             "on - per-team gun limits arbitrated by the server");
    }
    return g_noGl;
}

static DWORD WINAPI GlWorker(LPVOID arg)
{
    (void) arg;
    HMODULE ws = LoadLibraryA("ws2_32.dll");
    T2rSin a;
    T2rNet net;
    LONG res = 2;
    if (ws && HmServer(&a) && T2rNetLoad(&net, ws)) {
        uint8_t frame[3 + sizeof g_glReq];
        int n = (int) sizeof g_glReq + 1;
        frame[0] = (uint8_t) n;
        frame[1] = 0;
        frame[2] = 0xDE;
        memcpy(frame + 3, g_glReq, sizeof g_glReq);
        char rep[sizeof g_glRep];
        int got = T2rRpc(&net, &a, frame, n + 2, rep, (int) sizeof rep - 1, 2000, 2000, 2000, NULL);   // (the old plain connect() blocked ~21 s on a dead tunnel)
        int ln = got >= 3 ? (rep[0] & 0xff) + ((rep[1] & 0xff) << 8) : 0;
        if (ln >= 1 && got >= ln + 2 && (uint8_t) rep[2] == 0xDF
            && ln < (int) sizeof g_glRep) {
            memcpy(g_glRep, rep + 3, ln - 1);
            g_glRep[ln - 1] = 0;
            res = 1;
            SrvMarkAlive();
        }
    }
    InterlockedExchange(&g_gl_have, res);
    InterlockedExchange(&g_gl_busy, 0);
    return 0;
}

static int GlAsk(uint32_t mask)
{                               // 1 = request started
    static char tok[65];
    if (!tok[0] && !ClanToken(tok)) {
        tok[0] = 0;
        return 0;
    }
    if (g_gl_have || InterlockedCompareExchange(&g_gl_busy, 1, 0))
        return 0;
    memcpy(g_glReq, tok, 64);
    g_glReq[64] = (uint8_t) mask;
    g_glReq[65] = (uint8_t) (mask >> 8);
    HANDLE h = CreateThread(NULL, 0, GlWorker, NULL, 0, NULL);
    if (!h) {
        InterlockedExchange(&g_gl_busy, 0);
        return 0;
    }
    CloseHandle(h);
    g_gl.asked = mask;
    g_glReqmask = mask;
    return 1;
}

// ---- which limited guns a loadout carries (bit i = GL_TABLE[i]); the rows' weapon ids are what the spawn kit gives
static const char *GlRowId(int slot, uint32_t row)
{
    const uint32_t *cnt = (const uint32_t *) T2R_SYM(kLargeWeaponCounts);
    if (slot < 0 || slot > 2 || row == 0xffffffffu || row >= cnt[slot])
        return NULL;
    const uint8_t *r = slot == 2 ? LoPurchTab() + row * 0x40
        : slot == 0
        && !g_largeTab ? (const uint8_t *) T2R_SYM(kLargeWeaponTab) +
        row * 0x38 : WeaponRow(slot, row);
    return r ? *(const char **) (r + 0x10) : NULL;
}

static uint32_t GlMask(const LoadoutT *l)
{
    uint32_t m = 0;
    for (int s = 0; s < 3; s++) {
        const char *id = GlRowId(s, l->w[s]);
        if (!id)
            continue;
        for (int i = 0; i < GL_N; i++)
            for (int k = 0; k < 3 && GL_TABLE[i].ids[k]; k++)
                if (!strcmp(id, GL_TABLE[i].ids[k]))
                    m |= 1u << i;
    }
    return m;
}

static uint32_t GlIntended(void)
{                               // the limited guns of the loadout the player will spawn with
    if (g_loPending)
        return GlMask(LoOf(g_loPending - 1));
    LoadoutT cur;
    memset(&cur, 0xff, sizeof cur.w);
    LoReadObj(&cur);
    return GlMask(&cur);
}

// this life's loadout (the lobby object's, not a pending pick) carries a Tactical Shotgun (GL_TABLE row 0); re-read once a second (skills.c sprint meter)
static int GlLifeHasTac(void)
{
    static uint64_t at;
    static int v;
    uint64_t now = GetTickCount64();
    if (now - at < 1000)
        return v;
    at = now;
    LoadoutT cur;
    memset(&cur, 0xff, sizeof cur.w);
    LoReadObj(&cur);
    v = (GlMask(&cur) & 1u) != 0;
    return v;
}

static int GlCountdown(void)
{                               // the match is formed and counting down (vote / CHOOSE LOADOUT / booster stages): the team is known
    uint8_t *o = LobbyObj();
    return o && *(int *) (o + 0x1b4) == 2;
}

static int GlInMatchCtx(void)
{
    return g_loState == LO_CHOOSE || g_loState == LO_MATCH
        || MatchSubstate() >= 3 || GlCountdown();
}

static int GlActive(void)
{                               // the restriction is shown / enforced: a live match, a fresh daemon answer, not GEAR UP
    return !GlOff() && g_gl.on && GetTickCount64() - g_gl.at < 8000 && ((g_loState != LO_LIST && g_loState != LO_EDIT) || (g_loState == LO_LIST && GlCountdown()));        // GEAR UP during the match countdown is pre-match: restricted
}

static uint32_t GlBlocked(const LoadoutT *l)
{
    return GlActive()? GlMask(l) & g_gl.full : 0;
}

static const char *GlNameOf(uint32_t m)
{
    for (int i = 0; i < GL_N; i++)
        if (m >> i & 1)
            return GL_TABLE[i].name;
    return NULL;
}

// list rows: 1 + the gun's display name when the row's loadout holds a gun a teammate has taken
static int GlRowBlocked(int row, const char **name)
{
    uint32_t b = GlBlocked(LoOf(row));
    if (name)
        *name = GlNameOf(b);
    return b != 0;
}

// first preset class (then custom loadout) with none of the blocked guns; -1 = none
static int GlFallback(uint32_t blocked)
{
    for (int r = N_CUSTOM; r < N_CUSTOM + N_CLASSES; r++) {
        const LoadoutT *l = LoOf(r);
        if (!(GlMask(l) & blocked) && LoValid(l))
            return r;
    }
    for (int k = 0; k < g_loN; k++) {  // (custom slots: the created ones too)
        const int r = LoSlotIdx(k);
        const LoadoutT *l = LoOf(r);
        if (l->w[0] != 0xffffffffu && !(GlMask(l) & blocked)
            && LoValid(l))
            return r;
    }
    return -1;
}

static void GlSay(uint32_t blocked, int to)
{
    char m[160], b[40];
    const char *nm = GlNameOf(blocked);
    if (to >= 0) {
        char t[48];
        int k = 0, up = 1;      // "TACTICAL SHOTGUN" -> "Tactical Shotgun"
        for (const char *c = nm ? nm : "?"; *c && k < 46; c++, k++) {
            t[k] = up ? *c : (*c >= 'A' && *c <= 'Z' ? *c + 32 : *c);
            up = *c == ' ';
        }
        t[k] = 0;
        int mx = 1;
        for (int i = 0; i < GL_N; i++)
            if (nm && !strcmp(nm, GL_TABLE[i].name))
                mx = GL_TABLE[i].max;
        snprintf(m, sizeof m,
                 "Only %s %s per team - a teammate already has %s. Your loadout was switched to %s.",
                 mx == 1 ? "one" : "a few", t, mx == 1 ? "it" : "them",
                 LoName(to, b, sizeof b));
    } else
        snprintf(m, sizeof m, "Too many of %s on team.", nm ? nm : "?");
    if (!DnShow(m, to >= 0 ? 6.0f : 4.0f))
        MsgHook(m, to >= 0 ? 6.0f : 4.0f, 0, 0);
}

// the server said no (or the cache says taken at the spawn): put the player on the fallback class. Before the first spawn the
// lobby object is written now; once in the game the pick waits for the next life like any mid-match pick.
static void GlSwitch(uint32_t blocked, const char *why)
{
    int r = GlFallback(blocked);
    Xlog("gunlimit: %s%s - %s taken on the team, %s", GlCountdown()
         || (MatchSubstate() != 6
             && g_loState != LO_MATCH) ? "pre-match " : "", why,
         GlNameOf(blocked) ? GlNameOf(blocked) : "?",
         r >= 0 ? "switching loadout" : "no allowed loadout to switch to");
    if (r < 0) {
        GlSay(blocked, -1);
        return;
    }
    g_loActive = r;
    if (MatchSubstate() == 6 || g_loState == LO_MATCH)
        g_loPending = r + 1;
    else {
        g_loPending = 0;
        LoWriteObj(LoOf(r));
    }
    LoSave();
    g_gl.asked = ~0u;           // claim the new loadout at once
    GlSay(blocked, r);
}

static void GlApplyReply(void);
// Spawn-time decision from the cache - NEVER waits (the old synchronous gl_sync slept up to 2.5 s on the game thread when the
// daemon was slow / unreachable). The claims are prefetched asynchronously by gl_tick (worker thread; as soon as the match is formed /
// the loadout changes / every 0.25-2 s), so by the first spawn the answer is normally cached. 1 = decided (the cache answers for the
// loadout we are about to spawn with), 0 = server unreachable / no token (no restriction without the server), -1 = no usable decision
// yet (no answer / still being ranked `pend`): the caller keeps a gun the server already granted, else switches to the fallback.
static volatile LONG g_glFailedAt;    // low 32 bits of GetTickCount64 of the last failed request (0 = none)
static volatile LONG g_glFails;        // failed requests in a row (reset by an answer): the 'recent failure' window and the ask interval grow with it
static void GlNoteFail(void)
{
    LONG t = (LONG) GetTickCount64();
    InterlockedExchange(&g_glFailedAt, t ? t : 1);
    InterlockedIncrement(&g_glFails);
}

static const char *g_glPath = "";      // decision path of the last gl_sync (for the log)
static int GlSync(void)
{
    uint32_t want = GlIntended();
    if (!want) {
        g_glPath = "no limited gun";
        return 1;
    }
    if (InterlockedCompareExchange(&g_gl_have, 0, 0)) { // a finished request nobody has applied yet: apply it now (no waiting)
        LONG res = InterlockedExchange(&g_gl_have, 0);
        if (res == 1)
            GlApplyReply();
        else {
            GlNoteFail();
            g_glPath = "drained failed reply";
            return 0;
        }
        want = GlIntended();   // (a switch may have changed the loadout)
        if (!want) {
            g_glPath = "switched to a loadout without a limited gun";
            return 1;
        }
    }
    if (g_gl.answered == want && GetTickCount64() - g_gl.at < 4000) {
        if (!g_gl.on) {
            g_glPath = "cached: limits off";
            return 1;
        }
        if (!(want & g_gl.pend)) {
            g_glPath = "cached: server answered";
            return 1;
        }                       // granted / refused (full, handled by the caller)
        g_gl.answered = ~0u;    // still being ranked: ask again now (the spawn waits briefly for it)
        g_glPath = "cached answer still pending (ranking)";
        if (!g_gl_busy && !g_gl_have && GlAsk(want))
            g_gl.sent = GetTickCount64();
        return -1;
    }
    LONG fa = g_glFailedAt;
    if (fa
        && (uint32_t) ((LONG) GetTickCount64() - fa) <
        (4000u << (g_glFails > 3 ? 2 : g_glFails > 1 ? 1 : 0))) {
        g_glPath = "recent failed request (server unreachable)";
        return 0;
    }
    if (!g_gl_busy && !g_gl_have) {     // nothing in flight: start the claim for the next life / retry, do not wait for it
        if (!GlAsk(want)) {
            g_glPath = "no clan token";
            return 0;
        }
        g_gl.sent = GetTickCount64();
        g_glPath = "no cached answer, claim started (async)";
        return -1;
    }
    g_glPath = "no cached answer, request in flight";
    return -1;
}

static void GlSpawnFix(void)
{                               // spawnkit_hook, before the kit: a loadout with a taken gun never spawns
    if (GlOff() || !GlInMatchCtx())
        return;
    if (!GlIntended())
        return;
    int r = GlSync();
    if (r < 0 && (g_gl_busy || g_gl_have)) {    // a reachable daemon answers in ~0.1-0.6 s (settle window); a short bounded wait
        ULONGLONG t0 = GetTickCount64();        // beats stripping the player's pick. An unreachable daemon is caught by g_gl_failed_at above (no wait).
        while (r < 0 && GetTickCount64() - t0 < 700) {  // a first claim is answered `pend` for ~0.6 s (settle window) - keep draining / re-asking inside the 700 ms,
            if (InterlockedCompareExchange(&g_gl_have, 0, 0)) {
                r = GlSync();
                continue;
            }                   // (gl_sync re-asks on pend); one round trip alone ended in a stripped pick
            if (!g_gl_busy)
                break;          // nothing in flight any more (no clan token / failed ask): the fallback below decides
            Sleep(5);
        }
        if (r < 0)
            Xlog("gunlimit: spawn waited %llu ms for the daemon (%s)",
                 (unsigned long long) (GetTickCount64() - t0), g_glPath);
    }
    uint32_t want = GlIntended();
    if (r < 0 && want && (want & g_gl.held) == want) {  // a slow answer must not strip a gun the server already granted us
        Xlog("gunlimit: pre-match - no daemon decision at the spawn (%s), but mask %u is already held - kept", g_glPath, want);
        return;
    }
    if (r < 0) {
        Xlog("gunlimit: pre-match - no daemon decision at the spawn (%s; loadout mask %u): not risking a second limited gun", g_glPath, want);
        GlSwitch(want, "no server decision at the spawn");
        return;
    }
    uint32_t b = want & g_gl.full & ~g_gl.held;
    if (r == 1 && g_gl.on && (want & g_gl.pend))
        b |= want & g_gl.pend;
    if (b)
        GlSwitch(b, "spawn");
    else if (r == 1 && g_gl.on)
        Xlog("gunlimit: pre-match - spawn loadout cleared by the server (mask %u, held %u)", want, g_gl.held);
}

static void GlBuzz(void)
{                               // refused selection: the game's own menu error (gui2-game.bin sfx-menu-error, gbl-menu bank - loaded in menus)
    UiDeny(NULL);              // the shared denied sound (one play per 300 ms; the message follows from gl_say)
}

static void GlDeniedPick(uint32_t blocked)
{
    GlBuzz();
    GlSay(blocked, -1);
    static int nl;
    if (LOGCAP(nl, 20))
        Xlog("gunlimit: pick refused - %s is taken on the team",
             GlNameOf(blocked) ? GlNameOf(blocked) : "?");
}

static int GlPickRefused(int row)
{                               // lo_list_page, on a selection: 1 = refuse (denied feedback given)
    uint32_t b = GlBlocked(LoOf(row));
    if (!b)
        return 0;
    GlDeniedPick(b);
    return 1;
}

// ---- list page look: dimmed rows + the message, re-asserted every few frames
static uint32_t g_glDim;       // rows we have dimmed
static void GlListStyle(int entered)
{
    static uint64_t last;
    if (!entered && g_frame - last < 6)
        return;
    last = g_frame;
    char p[96];
    for (int r = 0; r < N_CUSTOM + N_CLASSES; r++) {
        int bl = GlRowBlocked(r, NULL);
        if (!bl && !(g_glDim >> r & 1))
            continue;
        snprintf(p, sizeof p, "anim-group.list.contents.Skill%d", r);
        uint64_t w = UiW(p);
        if (!w)
            continue;
        WAlpha(w, bl ? 0.35f : 1.0f);
        snprintf(p, sizeof p, "anim-group.list.contents.Skill%d.costText",
                 r);
        UiText(p,
                bl ? "<font color=\"#d23c32\">TOO MANY</font>" : r ==
                g_loActive ? (LoR3()? R3_EQUIPPED : "EQUIPPED") : "");
        if (bl)
            g_glDim |= 1u << r;
        else
            g_glDim &= ~(1u << r);
    }
}

// description text for a blocked row, prepended by lo_list_detail
static const char *GlDetailPrefix(int row, char *o, size_t n)
{
    const char *nm;
    if (!GlRowBlocked(row, &nm))
        return "";
    snprintf(o, n,
             "<font color=\"#d23c32\">Too many of %s on team.</font>\n\n",
             nm);
    return o;
}

static void GlApplyReply(void)
{
    char k[16], v[32];
    int n;
    const char *t = g_glRep;
    int on = 0, team = 0;
    uint32_t held = 0, deny = 0, full = 0, pend = 0;
    while (sscanf(t, " %15[^= ]=%31s%n", k, v, &n) == 2) {
        int x = atoi(v);
        if (!strcmp(k, "on"))
            on = x;
        else if (!strcmp(k, "team"))
            team = x & 1;
        else if (!strcmp(k, "mask"))
            held = (uint32_t) x;
        else if (!strcmp(k, "deny"))
            deny = (uint32_t) x;
        else if (!strcmp(k, "full"))
            full = (uint32_t) x;
        else if (!strcmp(k, "pend"))
            pend = (uint32_t) x;
        t += n;
    }
    uint32_t prev_full = g_gl.full;
    InterlockedExchange(&g_glFails, 0);
    g_gl.on = on;
    g_gl.at = GetTickCount64();
    g_gl.team = team;
    g_gl.held = held;
    g_gl.deny = deny;
    g_gl.full = on ? full : 0;
    g_gl.pend = on ? pend : 0;
    g_gl.answered = g_glReqmask;
    if (on && deny && GlCountdown())
        Xlog("gunlimit: pre-match - the server refused our claim (mask %u, team %d)", deny, team);
    if (on && prev_full != g_gl.full)
        Xlog("gunlimit: team %d - taken by teammates: %u (we hold %u)",
             team, g_gl.full, held);
    if (on && deny && (GlIntended() & deny))
        GlSwitch(deny, "the server refused our claim");
}

static void GlTick(void)
{
    if (GlOff())
        return;
    uint64_t now = GetTickCount64();
    if (InterlockedCompareExchange(&g_gl_have, 0, 0)) {
        LONG res = InterlockedExchange(&g_gl_have, 0);
        if (res == 1)
            GlApplyReply();   // (a failed request: the cache just ages out, no restriction without the server)
        else
            GlNoteFail();
    }
    if (!GlInMatchCtx()) {
        if (g_gl.on || g_gl.asked) {
            g_gl.on = 0;
            g_gl.full = 0;
            g_gl.asked = 0;
            g_gl.held = 0;
            Xlog("gunlimit: left the match context - restriction off");
        }
        return;
    }
    uint32_t want = GlIntended();
    int list_open = g_loState == LO_CHOOSE || g_loState == LO_MATCH;
    uint64_t every = (g_gl.pend & want) ? 250 : list_open ? 500 : GlCountdown()? 1000 : 2000;  // (a claim still being ranked pre-match: ask again shortly)
    if (g_glFails >= 3) {      // daemon unreachable: jittered backoff (2 s -> 10 s) instead of a connection every 0.25-2 s
        uint64_t bo = T2rBackoffMs((int) g_glFails - 2, 2000, 10000);
        if (every < bo)
            every = bo;
    }
    if ((want != g_gl.asked || now - g_gl.sent >= every) && !g_gl_busy
        && !g_gl_have && GlAsk(want))
        g_gl.sent = now;
}
