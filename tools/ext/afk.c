// afk.c - anti-AFK in a running match. Included after requeue.c. Log prefix 'afk:'.
//
// IDLE = no real input from the player: no key, no mouse (move / look / click / wheel), no pad button, no stick. Counted only while it matters:
// in a running match (state 6) that is not over, alive (not on the death / spectate screen, not downed), not in a menu that is part of play (the
// backpack / store, the in-match GEAR UP). Those moments freeze the count (an AFK player who dies and respawns keeps the idle time he had); any input
// resets it to 0. A new match starts at 0.
// Input sources:
//   - keyboard + mouse: GetLastInputInfo (the session's last hardware input tick: keys, clicks, raw mouse movement), counted only while the game window
//     is the foreground window (alt-tabbed into a browser = idle in the game);
//   - any pad (DualSense / Steam Input / XInput): the engine's own Pad as Joypad::Update reads it (padinject.c padinj_upd_hook: button mask Pad+0x528
//     changed or a stick past 0.3), the same object every gameplay command reads (memory controller-parity: never raw XInput);
//   - the dev bots' injected presses / sticks (padinj btn_applied / st_applied counters): a bot that plays is not AFK.
// Without the pad hook (t2r-no-pad-inject.txt / not installed) the whole feature stays off: a pad player could not be told from an idle one.
// At AFK_WARN_MS idle: a centred warning with a countdown (YOU ARE INACTIVE / MOVE OR YOU WILL BE REMOVED FROM THE MATCH IN n). The countdown runs only while
// the warning can be SEEN: a menu over the HUD (the pause menu) hides every text draw (g_text_menu), so there the count stops at the warning - nobody is
// removed without having had the full 30 s of visible warning. The reported idle = idle until the warning, then 60 s + the time the warning was visible. It rides on every
// op 0xE3 clock report (jip_ask: [76] 0xAF [77..78] u16 idle s [79] flags bit0 counting, bit1 warning up); the DAEMON removes the player at
// T2R_AFK_KICK_S (default 90 s) through its kick path (back to the menu with the kick text, counts as leaving early, no requeue).
// Off: t2r-no-afk.txt next to the exe (no warning, idle reported as 0xFFFF = unknown: the daemon never kicks).
#define AFK_WARN_MS 60000       // warning after ~60 s idle
#define AFK_KICK_MS 90000       // removal at ~90 s (the daemon's T2R_AFK_KICK_S default; only drives the countdown here)
#define AFK_STICK 0.3f          // a stick past this counts as input (rest noise of a worn stick stays well below)
static struct {
    int off;
    uint64_t chk;
    uint64_t prev, idle, warn_at, logged_s, vis;        // vis: ms the warning was visible (counts the removal down)
    DWORD lii;
    unsigned btn, st;
    int counting, warn, dead_c;
    uint64_t dead_f;
    uint8_t was_live;
} g_afk = {.off = -1 };

static int AfkOff(void)
{
    uint64_t now = GetTickCount64();
    if (g_afk.off < 0 || now - g_afk.chk > 5000) {
        g_afk.chk = now;
        FILE *f = ExeSideOpen("t2r-no-afk.txt", "r");
        int o = f != NULL;
        if (f)
            fclose(f);
        if (o != g_afk.off)
            Xlog("afk: %s",
                 o ? "off (t2r-no-afk.txt)" :
                 "on (warning at 60 s idle, the server removes at 90 s)");
        g_afk.off = o;
    }
    return g_afk.off || (g_srvTestFlags & 0x40);      // (the test server removes nobody for inactivity - no warning either)
}

static int AfkCountable(void)
{
    if (MatchSubstate() != 6)
        return 0;
    uint8_t *lo = LobbyObj();
    if (!lo || IsBadReadPtr(lo + 0x2ca4, 1) || lo[0x2ca4])
        return 0;               // the match is over (end banner / scoreboard)
    if (g_downed || DuDeadNow())
        return 0;               // downed / death screen / spectating
    if (g_frame - g_afk.dead_f >= 15) {
        g_afk.dead_f = g_frame;
        g_afk.dead_c = BpOwnDead();
    }                           // (a native call: every 15 frames)
    if (g_afk.dead_c)
        return 0;
    if (g_tabOpen || g_gu13Up || PlayerMenuActive() == 1)
        return 0;               // backpack / store / GEAR UP: menus that are part of play
    return 1;
}

static uint64_t AfkRepMs(void)
{                               // the idle time that counts toward the removal (see the header: the warning must have been visible)
    if (!g_afk.warn)
        return g_afk.idle;
    uint64_t r = AFK_WARN_MS + g_afk.vis;
    return r < g_afk.idle ? r : g_afk.idle;
}

static void AfkTick(void)
{
    uint64_t now = GetTickCount64();
    uint64_t dt = g_afk.prev && now > g_afk.prev ? now - g_afk.prev : 0;
    g_afk.prev = now;
    if (dt > 1000)
        dt = 1000;              // (a hitch / load stall counts at most 1 s)
    int live = MatchSubstate() == 6;
    if (live && !g_afk.was_live) {
        g_afk.idle = 0;
        g_afk.warn = 0;
        g_afk.vis = 0;
        g_afk.logged_s = 0;
    }                           // a new match starts at 0
    g_afk.was_live = (uint8_t) live;
    if (AfkOff() || !PadinjReady()) {
        g_afk.idle = 0;
        g_afk.warn = 0;
        g_afk.vis = 0;
        g_afk.counting = 0;
        return;
    }
    int input = 0;
    LASTINPUTINFO li = { sizeof li, 0 };
    if (GetLastInputInfo(&li)) {
        if (li.dwTime != g_afk.lii && GameFocused())
            input = 1;          // keyboard / mouse while the game has the focus
        g_afk.lii = li.dwTime;
    }
    if (g_padActMs && now - g_padActMs < 250)
        input = 1;              // any pad
    unsigned b = PadinjApplied(), s = g_padi.st_applied;
    if (b != g_afk.btn || s != g_afk.st)
        input = 1;              // a dev bot's injected input
    g_afk.btn = b;
    g_afk.st = s;
    if (input) {
        if (g_afk.warn)
            Xlog("afk: input after %.0f s idle - warning cleared",
                 g_afk.idle / 1000.0);
        else if (g_afk.idle >= 30000)
            Xlog("afk: input after %.0f s idle", g_afk.idle / 1000.0);
        g_afk.idle = 0;
        g_afk.warn = 0;
        g_afk.vis = 0;
        g_afk.logged_s = 0;
    }
    g_afk.counting = !input && AfkCountable();
    if (g_afk.counting)
        g_afk.idle += dt;
    if (g_afk.counting && g_afk.warn && !HUD_MENU_UP())
        g_afk.vis += dt;
    if (!g_afk.warn && g_afk.idle >= AFK_WARN_MS) {
        g_afk.warn = 1;
        g_afk.warn_at = now;
        g_afk.vis = 0;
        Xlog("afk: no input for %.0f s - warning shown (the server removes us at %d s)", g_afk.idle / 1000.0, AFK_KICK_MS / 1000);
    }
    if (g_afk.warn && g_afk.idle / 10000 != g_afk.logged_s) {
        g_afk.logged_s = g_afk.idle / 10000;
        Xlog("afk: idle %.0f s, warning visible %.0f s%s",
             g_afk.idle / 1000.0, g_afk.vis / 1000.0,
             AfkRepMs() >=
             AFK_KICK_MS ? " - removal due, waiting for the server" : "");
    }
}

// the 0xE3 trailer (jip_ask): [0xAF][u16 idle seconds, 0xFFFF = unknown / off][flags: 1 counting, 2 warning up]
static void AfkReqTail(uint8_t *p)
{
    int known = !AfkOff() && PadinjReady() && MatchSubstate() == 6;
    uint64_t s = AfkRepMs() / 1000;
    if (s > 0xFFFE)
        s = 0xFFFE;
    uint16_t v = known ? (uint16_t) s : 0xFFFF;
    p[0] = 0xAF;
    p[1] = (uint8_t) v;
    p[2] = (uint8_t) (v >> 8);
    p[3] = (uint8_t) ((g_afk.counting ? 1 : 0) | (g_afk.warn ? 2 : 0));
}

static void AfkDraw(void)
{
    typedef void (*fn_txt)(...);
    typedef uint64_t(*fn_meas) (const char *, float);
    if (!g_afk.warn || !g_afk.counting || MatchSubstate() != 6
        || HUD_MENU_UP())
        return;                 // (dead / downed / in the store: the count is frozen, nothing shown)
    int left = (int) ((AFK_KICK_MS - (int64_t) AfkRepMs() + 999) / 1000);
    if (left < 0)
        left = 0;
    float a = (float) (GetTickCount64() - g_afk.warn_at) / 300.0f;
    if (a > 1.0f)
        a = 1.0f;               // 0.3 s fade in
    uint32_t al = (uint32_t) (a * 255.0f + 0.5f) << 24;
    char l2[128];
    const char *h = LocTrDraw("YOU ARE INACTIVE");
    snprintf(l2, sizeof l2,
             "MOVE OR YOU WILL BE REMOVED FROM THE MATCH IN %d", left);
    const char *t2 = LocTrDraw(l2);
    const char *L[2] = { h, t2 };
    const float S[2] = { 1.2f, 0.9f }, Y[2] = { 236.0f, 266.0f };
    const uint32_t C[2] = { 0x00E1B23Cu, 0x00DCE6EAu };
    for (int i = 0; i < 2; i++) {
        uint64_t wh = ((fn_meas) T2R_SYM(kFnMeasureText)) (L[i], S[i]);
        float w;
        uint32_t lw = (uint32_t) wh;
        memcpy(&w, &lw, 4);
        if (!(w > 0.0f && w < 1280.0f))
            continue;
        ((fn_txt) T2R_SYM(kFnDrawTextVa)) (Fbits(640.0f - w * 0.5f),
                                           Fbits(Y[i]), Fbits(S[i]),
                                           (uint64_t) (al | C[i]), "%s",
                                           L[i]);
    }
}
