// dpadlog.c - weapon cross / d-pad / hover wheel logger (the package was never added to the dpad ui, so a logger detects everything).
//
// What the d-pad UI reads (dc1/player-weapon-cross.bin + the exe): the engine loads ONE slot table, hard-coded in the exe
// (tlou-ii.exe +0xbc0d05: mov rcx, sid *player-inventory-slots-v2*) - 10 slots: longgun primary / secondary, shortgun primary / secondary and
// *slot-hover-8* .. *slot-hover-13* (each slot = up to 4 item ids at +0x18; a slot shows the carried item of its list). The Crafter gift's slot
// *hud2-slot-up-three-factions* is in the three *player-inventory-slots-t2-factions** tables only, which nothing loads - so the gift never had a
// slot. Fixed in the data (dc_data_patches.patch_weapon_cross: `gift` replaces the SP-only fire-trap in *slot-hover-12*, every table).
//
// This logger (on by default in dev builds, t2r-no-dpad-log.txt = off; release builds: t2r-dpad-log.txt = on) logs, on change only, line prefix 'dpad:':
//   - the engine commands of the cross / wheel (joypad-command-active?, keyboard + mouse + pad): open / toggle / nav (= the mouse wheel and the d-pad) /
//     select / slot shortcuts / sub-cross, the raw middle mouse button, and the script natives that answer (open?, highlighted index, focused item ...)
//     - which of them the engine knows is logged once ('dpad: probe')
//   - the item in the hand (get-player-weapon-id) and every carried item of the v2 slot lists + the gift + the shiv (inv_qty), when it changes - the engine's
//     inventory list + has-weapon? over the guns / melee weapons (dpl_carried), and the cross widgets themselves: hover / state / visibility per slot (dpw_tick)
// So one pass through the wheel (scroll through every slot, then hold the middle mouse button and hover over each) shows what the UI does.
static int DplOn(void)
{
#ifdef T2R_RELEASE
    static uint64_t at;
    static int v;
    return FeelFlag("t2r-dpad-log.txt", &at, &v, 120);
#else
    static uint64_t at;
    static int v;
    return !FeelFlag("t2r-no-dpad-log.txt", &at, &v, 120);
#endif
}

static const char *const DPL_CMD[] = {
    "weapon-cross-open", "weapon-cross-open-press",
        "weapon-cross-open-toggle", "weapon-cross-mkb-open-toggle",
        "weapon-cross-close",
    "weapon-cross-nav-pos", "weapon-cross-nav-neg", "weapon-cross-up",
        "weapon-cross-down", "weapon-cross-left", "weapon-cross-right",
    "weapon-cross-up-alt", "weapon-cross-down-alt", "weapon-cross-select",
        "weapon-cross-select-hold", "weapon-cross-select-release",
    "weapon-cross-select-rmb", "weapon-cross-select-secondary",
        "weapon-cross-open-subcross", "weapon-cross-open-subcross-hold",
    "weapon-cross-close-subcross", "weapon-cross-submenu-exit",
        "swap-weapon-cycle", "weapon-quick-swap", "quick-swap-weapon",
    "weapon-cross-slot0", "weapon-cross-slot1", "weapon-cross-slot2",
        "weapon-cross-slot3", "weapon-cross-slot4", "weapon-cross-slot5",
    "weapon-cross-slot6", "weapon-cross-slot7", "weapon-cross-slot8",
        "inventory-slot0", "inventory-slot1", "inventory-slot2",
        "inventory-slot3",
    "inventory-slot4", "inventory-slot5", "inventory-slot6",
        "inventory-slot7", "weapon-cross-craft-press",
        "weapon-cross-ammo-swap",
};

#define DPL_NC (int)(sizeof DPL_CMD / sizeof *DPL_CMD)
// script natives that may tell the UI state (0 args); the ones that answer ~0 are not natives (logged once, then skipped)
static const char *const DPL_Q[] = {
    "hud2-weapon-cross-active?", "hud2-weapon-cross-submenu-active?",
        "hud2-get-highlighted-weapon-cross-index",
        "weapon-cross-get-curr-focused-item",
    "get-weapon-cross-submenu-focused-item-id",
        "get-weapon-cross-inventory-count", "weapon-cross-get-scroll-mode",
        "weapon-cross-get-swap-mode",
    "get-weapon-cross-scroll-mode", "weapon-cross-available?",
        "player-can-access-weapon-cross?", "disable-weapon-cross?",
        "is-weapon-cross-hud-enabled?",
    "weapon-cross-mkb-selection-mode", "weapon-cross-selection-mode",
};

#define DPL_NQ (int)(sizeof DPL_Q / sizeof *DPL_Q)
// the items of *player-inventory-slots-v2* (after patch_weapon_cross) + what we give
static const char *const DPL_ITEM[] =
    { "pipe-bomb", "brick", "molotov", "bottle", "health-kit",
"tennis-ball", "stun-bomb", "smoke-bomb", "trap-bomb",
    "fire-trap", "gift", "shiv", "trap-mine", "melee-upgrade"
};

#define DPL_NI (int)(sizeof DPL_ITEM / sizeof *DPL_ITEM)
static const char *const DPL_WPN[] =
    { "pistol-beretta", "pistol-glock", "pistol-revolver-taurus",
"pistol-xcaliber", "pistol-cz75-enforcer", "pistol-el-diablo",
    "shotgun-shorty", "rifle-remington-bolt", "rifle-vepr",
        "rifle-m14-semi", "rifle-burst", "rifle-mpx5",
        "rifle-assault-firefly", "rifle-galil",
    "rifle-frontier", "rifle-milsniper", "shotgun-baserri-over-under",
        "shotgun-remington-pump", "shotgun-tactical", "bow-ellie",
        "bow-explosive", "crossbow",
    "flamethrower", "machete", "hatchet", "two-by-four", "baseball-bat",
        "fireman-axe", "pipe", "hammer", "crowbar", "switchblade",
        "dagger-scar-lev-player",
    "knife-survival-mel-player", "abby-unarmed", "ellie-unarmed"
};

#define DPL_NW (int)(sizeof DPL_WPN / sizeof *DPL_WPN)
static const char *DplName(uint64_t s)
{
    if (!s || s == ~0ULL)
        return "none";
    for (int i = 0; i < DPL_NI; i++)
        if (s == Fnv64Rt(DPL_ITEM[i]))
            return DPL_ITEM[i];
    for (int i = 0; i < DPL_NW; i++)
        if (s == Fnv64Rt(DPL_WPN[i]))
            return DPL_WPN[i];  // (abby-unarmed = 6ac05fba9c921900, the fists)
    const char *n = CraftSidName(s);
    if (n)
        return n;
    n = DcName(s);
    return n && n[0] != '?' ? n : NULL;
}

// ---- what is carried (night61: inv_qty over the slot item names logged "nothing" all session, weapons were not in the list at all). Now the engine's own
// inventory list: player-inventory-get-num-items / -get-item-at-index (each entry's quantity: inv_qty, a weapon: player-inventory-is-item-weapon?), plus
// inv_qty over the slot item names and has-weapon? over the MP guns + melee weapons. Which calls are natives is logged once ('dpad: carried probe').
// Logged when the set changes: 'dpad: carried (inventory list, N entries): <item>[w] x<n> ... | by name: ... | weapons: ...'.
static void DplCarried(int first)
{
    static uint64_t last_sig = 1;
    static int probed, nat_n, nat_at, nat_w;
    uint64_t none = 0;
    char buf[900];
    int o = 0;
    uint64_t sig = 1469598103934665603ULL;
    uint64_t n =
        DcCall(FNV64("player-inventory-get-num-items"), 0, &none);
    if (!probed)
        nat_n = n != ~0ULL;
    int cnt = n != ~0ULL && n < 200 ? (int) n : 0, listed = 0;
    for (int i = 0; i < cnt && o < (int) sizeof buf - 80; i++) {
        uint64_t ix = (uint64_t) i, it =
            DcCall(FNV64("player-inventory-get-item-at-index"), 1, &ix);
        if (!probed && i == 0)
            nat_at = it != ~0ULL;
        if (!it || it == ~0ULL)
            continue;
        uint64_t q = InvQty(it), iw =
            DcCall(FNV64("player-inventory-is-item-weapon?"), 1, &it);
        if (!probed && i == 0)
            nat_w = iw != ~0ULL;
        const char *nm = DplName(it);
        o += snprintf(buf + o, sizeof buf - (size_t) o, " %s%s x%llu",
                      nm ? nm : "?", iw != ~0ULL
                      && (iw & 0xff) ? "[w]" : "", (unsigned long long) q);
        if (!nm)
            o += snprintf(buf + o, sizeof buf - (size_t) o, "(%llx)",
                          (unsigned long long) it);
        sig = (sig ^ it ^ (q << 48)) * 1099511628211ULL;
        listed++;
    }
    o += snprintf(buf + o, sizeof buf - (size_t) o, " | by name:");
    int nn = 0;
    for (int i = 0; i < DPL_NI && o < (int) sizeof buf - 40; i++) {
        uint64_t q = InvQty(Fnv64Rt(DPL_ITEM[i]));
        if (q) {
            o += snprintf(buf + o, sizeof buf - (size_t) o, " %s x%llu",
                          DPL_ITEM[i], (unsigned long long) q);
            sig =
                (sig ^ Fnv64Rt(DPL_ITEM[i]) ^ (q << 40)) *
                1099511628211ULL;
            nn++;
        }
    }
    if (!nn)
        o += snprintf(buf + o, sizeof buf - (size_t) o, " none");
    o += snprintf(buf + o, sizeof buf - (size_t) o, " | weapons:");
    int nw = 0;
    for (int i = 0; i < DPL_NW && o < (int) sizeof buf - 40; i++) {
        uint64_t id = Fnv64Rt(DPL_WPN[i]), hw =
            DcCall(FNV64("has-weapon?"), 1, &id);
        if (hw != ~0ULL && (hw & 0xff)) {
            o += snprintf(buf + o, sizeof buf - (size_t) o, " %s",
                          DPL_WPN[i]);
            sig = (sig ^ id) * 1099511628211ULL;
            nw++;
        }
    }
    if (!nw)
        o += snprintf(buf + o, sizeof buf - (size_t) o, " none");
    if (!probed) {
        probed = 1;
        Xlog("dpad: carried probe - player-inventory-get-num-items %s (%llx), -get-item-at-index %s, -is-item-weapon? %s; has-weapon? over %d guns / melee", nat_n ? "native" : "NOT a native", (unsigned long long) n, cnt ? (nat_at ? "native" : "NOT a native") : "(not asked: no items)", cnt ? (nat_w ? "native" : "NOT a native") : "-", DPL_NW);
    }
    if (sig == last_sig)
        return;
    last_sig = sig;
    Xlog("dpad: carried (inventory list, %d entries):%s%s", listed, buf,
         first ? " [first]" : "");
}

// ---- the cross / wheel widgets (night61: none of the UI-state names are natives - they are DC script functions of ss-hud2-weapon-cross-v2, so the
// highlight / focus is not readable by a call). The gui2 tree is read instead: the cross is the widget whose children are named left / right / up / down
// (the slot paths of *player-inventory-slots-v2* are 'left.leftOne' / 'right.rightOne'). It is found by walking canvas (the child / sibling impls of
// hud fade / hudtrace) when a cross-open command or the middle mouse button is down; its subtree (5 levels, up to 160 widgets) is listed once
// ('dpad: ui tree'). Every 2nd frame while it shows, each widget's visible bit (+0x229 bit 4), own / draw alpha (+0x3bc / +0x2d4, edges at 0.5), the engine's
// mouse hover result (+0x410: the gui2 hit test of hoverable widgets = the mouse-drag path) and the gui2 state (gui2-get-widget-state, if it is a native)
// are compared: a change is 'dpad: ui <path> ...'; a widget that turns hovered or changes state is 'dpad: HIGHLIGHT <slot> (<path>) icon <texture>' -
// the texture path names the item. Also logged once: widget names that look like a wheel / cross anywhere under the canvas.
#define DPW_MAX 400
static struct {
    uint64_t root, tried;
    int n, open, names_logged;
    uint64_t h[DPW_MAX];
    char path[DPW_MAX][72];
    uint32_t st[DPW_MAX];
    uint64_t gs[DPW_MAX];
} g_dpw;
static const char *DpwStrTex(const char *p)
{
    if ((uint64_t) p < 0x10000 || (uint64_t) p >> 47 || IsBadReadPtr(p, 8))
        return NULL;
    int n = 0;
    while (n < 150 && !IsBadReadPtr(p + n, 1) && p[n] >= 0x20
           && p[n] < 0x7f)
        n++;
    return n >= 5 && n < 150 && !p[n] && (strstr(p, ".png")
                                          || strstr(p, ".tga")
                                          || strstr(p, ".dds")) ? p : NULL;
}

static const char *DpwTex(uint8_t *w)
{
    for (int o = 0x3c0; o < 0x680; o += 8) {
        if (IsBadReadPtr(w + o, 8))
            break;
        uint64_t v = *(uint64_t *) (w + o);
        if (v < 0x10000 || v >> 47 || IsBadReadPtr((void *) v, 0x20))
            continue;
        const char *t = DpwStrTex((const char *) v);
        for (int k = 0; k < 4 && !t; k++)
            t = DpwStrTex((const char *) ((uint64_t *) v)[k]);
        if (t)
            return t;
    }
    return NULL;
}

static int DpwFind(void)
{
    uint8_t *exe = (uint8_t *) GetModuleHandleW(NULL);
    if (!G2Ok())
        return 0;
    static uint64_t q[5000];
    static int16_t par[5000];
    static uint8_t dp[5000], dirs[5000];
    uint64_t root =
        G2Find(FNV64("canvas")) ? FNV64("canvas") :
        FNV64("canvas.hudRoot");
    int qh = 0, qt = 0, best = -1;
    char cand[700];
    int co = 0, nc = 0;
    q[qt] = root;
    par[qt] = -1;
    dp[qt] = 0;
    dirs[qt] = 0;
    qt++;
    while (qh < qt) {
        int pi = qh;
        uint64_t p = q[qh];
        int d = dp[qh++];
        if (d >= 12)
            continue;
        uint64_t c = 0;
        ((void (*)(uint64_t *, uint64_t)) (exe + RVA_GUI2_CHILD)) (&c, p);
        for (int guard = 0; c && guard < 400 && qt < 5000; guard++) {
            uint8_t *w = G2Find(c);
            const char *nm = w ? G2Name(w) : "?";
            int bit =
                !strcmp(nm, "left") ? 1 : !strcmp(nm,
                                                  "right") ? 2 :
                !strcmp(nm, "up") ? 4 : !strcmp(nm, "down") ? 8 : 0;
            if (bit) {
                dirs[pi] |= (uint8_t) bit;
                if (__builtin_popcount(dirs[pi]) >= 3 && best < 0)
                    best = pi;
            }
            if (!g_dpw.names_logged && nc < 30
                && co < (int) sizeof cand - 70 && (strstr(nm, "over")
                                                   || strstr(nm, "heel")
                                                   || strstr(nm, "adial")
                                                   || strstr(nm, "ross")
                                                   || strstr(nm,
                                                             "eapon"))) {
                co +=
                    snprintf(cand + co, sizeof cand - (size_t) co,
                             " d%d %s[%llx]", d + 1, nm,
                             (unsigned long long) c);
                nc++;
            }
            q[qt] = c;
            par[qt] = (int16_t) pi;
            dp[qt] = (uint8_t) (d + 1);
            dirs[qt] = 0;
            qt++;
            uint64_t nx = 0;
            ((void (*)(uint64_t *, uint64_t)) (exe + RVA_GUI2_SIBLING))
                (&nx, c);
            if (!nx || nx == c)
                break;
            c = nx;
        }
    }
    if (!g_dpw.names_logged) {
        g_dpw.names_logged = 1;
        Xlog("dpad: ui names like a wheel / cross (%d walked):%s", qt,
             nc ? cand : " none");
    }
    if (best < 0) {
        static int nl;
        if (LOGCAP(nl, 6))
            Xlog("dpad: ui - no widget with left / right / up / down children under the canvas (%d walked)", qt);
        return 0;
    }
    // night62 run 2: the slots' hover backgrounds (8 x hoverBg, animGroupHoverBg) are children of cross-root-pc, the PARENT of the listView that holds
    // left / right / up / down - so the watched root is that parent (listView's paths then start with 'listView.')
    if (par[best] >= 0) {
        uint8_t *pw = G2Find(q[par[best]]);
        if (pw && strstr(G2Name(pw), "cross"))
            best = par[best];
    }
    g_dpw.root = q[best];
    {                           // the root's chain of names, for the log
        char chain[200];
        int k = 0, idx = best, n = 0;
        const char *nm[12];
        while (idx >= 0 && n < 12) {
            uint8_t *w = G2Find(q[idx]);
            nm[n++] = w ? G2Name(w) : "?";
            idx = par[idx];
        }
        for (int i = n - 1; i >= 0 && k < (int) sizeof chain - 40; i--)
            k += snprintf(chain + k, sizeof chain - (size_t) k, "%s%s",
                          nm[i], i ? "." : "");
        Xlog("dpad: ui cross root = %s [%llx] (depth %d)", chain,
             (unsigned long long) g_dpw.root, dp[best]);
    }
    // its subtree, dotted names relative to the root
    static uint64_t sq[DPW_MAX + 1];
    static int sp[DPW_MAX + 1];
    static uint8_t sd[DPW_MAX + 1];
    int h = 0, t = 0;
    g_dpw.n = 0;
    sq[t] = g_dpw.root;
    sp[t] = -1;
    sd[t] = 0;
    t++;
    while (h < t) {
        uint64_t p = sq[h];
        int pidx = sp[h], d = sd[h++];
        if (d >= 6)
            continue;
        uint64_t c = 0;
        ((void (*)(uint64_t *, uint64_t)) (exe + RVA_GUI2_CHILD)) (&c, p);
        for (int guard = 0;
             c && guard < 200 && t <= DPW_MAX && g_dpw.n < DPW_MAX;
             guard++) {
            uint8_t *w = G2Find(c);
            int i = g_dpw.n++;
            g_dpw.h[i] = c;
            g_dpw.st[i] = ~0u;
            g_dpw.gs[i] = ~0ULL - 1;
            snprintf(g_dpw.path[i], sizeof g_dpw.path[i], "%s%s%s",
                     pidx >= 0 ? g_dpw.path[pidx] : "",
                     pidx >= 0 ? "." : "", w ? G2Name(w) : "?");
            sq[t] = c;
            sp[t] = i;
            sd[t] = (uint8_t) (d + 1);
            t++;
            uint64_t nx = 0;
            ((void (*)(uint64_t *, uint64_t)) (exe + RVA_GUI2_SIBLING))
                (&nx, c);
            if (!nx || nx == c)
                break;
            c = nx;
        }
    }
    char b[1400];
    int o = 0, part = 1;
    for (int i = 0; i < g_dpw.n; i++) { // (in parts: one log line holds ~1.4 KB)
        o += snprintf(b + o, sizeof b - (size_t) o, " %s", g_dpw.path[i]);
        if (o > (int) sizeof b - 90 || i == g_dpw.n - 1) {
            Xlog("dpad: ui tree (%d widgets) part %d:%s", g_dpw.n, part++,
                 b);
            o = 0;
            b[0] = 0;
        }
    }
    return 1;
}

static int DpwIsSlot(const char *p)
{                               // listView.<dir>.<dir><n> / listView.left.scaler / listView.right.scaler
    const char *q = !strncmp(p, "listView.", 9) ? p + 9 : p;
    const char *d = strchr(q, '.');
    if (!d || strchr(d + 1, '.'))
        return 0;
    return (!strncmp(q, "left.", 5) || !strncmp(q, "right.", 6)
            || !strncmp(q, "up.", 3) || !strncmp(q, "down.", 5))
        && strcmp(d + 1, "lines");
}

// the slot widget (left.scaler / right.scaler / up.upN / down.downN) whose screen rect holds (x, y), for a hover widget that lives outside the slots (the
// 8 hoverBg of cross-root-pc), else -1
static int DpwSlotAt(float x, float y)
{
    int best = -1;
    float barea = 1e30f;
    for (int i = 0; i < g_dpw.n; i++) {
        if (!DpwIsSlot(g_dpw.path[i]))
            continue;
        float x0, y0, x1, y1;
        if (!UiRectSs(g_dpw.h[i], &x0, &y0, &x1, &y1))
            continue;
        if (x < x0 || x > x1 || y < y0 || y > y1)
            continue;
        float a = (x1 - x0) * (y1 - y0);
        if (a < barea) {
            barea = a;
            best = i;
        }
    }
    return best;
}

static int DpwHoverish(const char *p)
{
    const char *l = strrchr(p, '.');
    l = l ? l + 1 : p;
    return strstr(l, "over") || strstr(l, "rompt") || strstr(l, "ine")
        || strstr(l, "abel") || strstr(l, "ext") || strstr(l, "umber")
        || strstr(l, "ighlight") || strstr(l, "elect")
        || strstr(l, "ocus");
}

static void DpwTick(void)
{
    static int gs_ok = -1, nl, nh;
    static uint64_t want_until;
    uint64_t now = GetTickCount64(), none = 0;
    int want = GameFocused() && (GetAsyncKeyState(VK_MBUTTON) & 0x8000);
    for (int i = 0; i < DPL_NC && !want; i++)
        if (!strncmp(DPL_CMD[i], "weapon-cross-open", 17)) {
            uint64_t c = Fnv64Rt(DPL_CMD[i]), r =
                DcCall(FNV64("joypad-command-active?"), 1, &c);
            want = r != ~0ULL && (r & 0xff);
        }
    if (want)
        want_until = now + 8000;
    uint8_t *rw = g_dpw.root ? G2Find(g_dpw.root) : NULL;
    if (!rw && want && now - g_dpw.tried > 3000) {
        g_dpw.tried = now;
        g_dpw.root = 0;
        if (DpwFind())
            rw = G2Find(g_dpw.root);
    }
    if (!rw || IsBadReadPtr(rw, 0x418)) {
        g_dpw.open = 0;
        return;
    }
    // the old 'shown' test on the root (visible bit + alphas of listView) never passed while every slot was hovered, so nothing was
    // compared. Now the whole cross-root-pc subtree is compared every 3rd frame for 8 s after a cross-open command / the middle mouse button, no gate.
    if (now > want_until || g_frame % 3)
        return;
    if (gs_ok < 0) {
        uint64_t a[1] = { g_dpw.root }, r =
            DcCall(FNV64("gui2-get-widget-state"), 1, a);
        gs_ok = r != ~0ULL;
        Xlog("dpad: ui gui2-get-widget-state %s",
             gs_ok ? "answers" : "is NOT a native (states not read)");
    }
    for (int i = 0; i < g_dpw.n; i++) {
        uint8_t *w = G2Find(g_dpw.h[i]);
        if (!w || IsBadReadPtr(w, 0x418))
            continue;
        float own = *(float *) (w + 0x3bc), drw = *(float *) (w + 0x2d4);
        uint32_t qa = own < 0.05f ? 0u : own < 0.5f ? 1u : 2u, qd =
            drw < 0.05f ? 0u : drw < 0.5f ? 1u : 2u;
        uint32_t st =
            ((w[0x229] >> 4) & 1) | (qa << 1) | (qd << 3) | ((uint32_t)
                                                             w[0x410] <<
                                                             8);
        uint64_t gs = 0;
        if (gs_ok > 0) {
            uint64_t a[1] = { g_dpw.h[i] };
            gs = DcCall(FNV64("gui2-get-widget-state"), 1, a);
        }
        if (st == g_dpw.st[i] && gs == g_dpw.gs[i])
            continue;
        uint32_t was = g_dpw.st[i];
        uint64_t wgs = g_dpw.gs[i];
        g_dpw.st[i] = st;
        g_dpw.gs[i] = gs;
        if (was == ~0u)
            continue;           // (the first read after the tree was listed is the baseline)
        const char *sn = gs_ok > 0 ? DcName(gs) : NULL;
        if (LOGCAP(nl, 3000))
            Xlog("dpad: ui %s vis %u own %.2f draw %.2f hover %u%s%s (was vis %u own-level %u draw-level %u hover %u)", g_dpw.path[i], st & 1, (double) own, (double) drw, st >> 8, gs_ok > 0 ? " state " : "", gs_ok > 0 ? (sn ? sn : "?") : "", was & 1, (was >> 1) & 3, (was >> 3) & 3, was >> 8);
        int shown = (st & 1) && qa && qd, wshown = (was & 1)
            && ((was >> 1) & 3) && ((was >> 3) & 3);
        if ((shown && !wshown && DpwHoverish(g_dpw.path[i]))
            || ((st >> 8) && !(was >> 8)) || (gs_ok > 0 && gs != wgs)) {
            float x0, y0, x1, y1, cx = -1, cy = -1;
            if (UiRectSs(g_dpw.h[i], &x0, &y0, &x1, &y1)) {
                cx = (x0 + x1) * 0.5f;
                cy = (y0 + y1) * 0.5f;
            }
            int sl = -1;
            for (int k = i; k >= 0 && sl < 0; k--) {    // the slot this widget belongs to: an ancestor path, else the slot under its centre
                size_t pl = strlen(g_dpw.path[k]);
                if (DpwIsSlot(g_dpw.path[k])
                    && !strncmp(g_dpw.path[i], g_dpw.path[k], pl)
                    && (g_dpw.path[i][pl] == '.' || !g_dpw.path[i][pl]))
                    sl = k;
            }
            if (sl < 0 && cx >= 0) {
                int k = DpwSlotAt(cx, cy);
                if (k >= 0 && DpwIsSlot(g_dpw.path[k]))
                    sl = k;
            }
            const char *tx = NULL;
            if (sl >= 0) {
                size_t pl = strlen(g_dpw.path[sl]);
                for (int k = sl; k < g_dpw.n && !tx; k++)
                    if (!strncmp(g_dpw.path[k], g_dpw.path[sl], pl)
                        && (g_dpw.path[k][pl] == '.'
                            || !g_dpw.path[k][pl])) {
                        uint8_t *cw = G2Find(g_dpw.h[k]);
                        if (cw)
                            tx = DpwTex(cw);
                    }
            }
            const char *hn =
                DplName(DcCall(FNV64("get-player-weapon-id"), 0, &none));
            if (LOGCAP(nh, 800))
                Xlog("dpad: HIGHLIGHT %s via %s (%s) at %.0f,%.0f icon %s - in the hand %s", sl >= 0 ? g_dpw.path[sl] : "slot ?", g_dpw.path[i], (st >> 8) && !(was >> 8) ? "hovered" : gs_ok > 0 && gs != wgs ? "state change" : "shown", (double) cx, (double) cy, tx ? tx : "-", hn ? hn : "?");
        }
    }
}

static void DpadlogTick(void)
{
    static int probed, qok[DPL_NQ], cmd_ok[DPL_NC], cmd_on[DPL_NC], mmb;
    static uint64_t qv[DPL_NQ], held, frame0;
    if (!DplOn() || MatchSubstate() != 6)
        return;
    uint64_t none = 0;
    if (!probed) {
        probed = 1;
        char buf[1400];
        int o = 0;
        for (int i = 0; i < DPL_NQ; i++) {
            uint64_t r = DcCall(Fnv64Rt(DPL_Q[i]), 0, &none);
            qok[i] = r != ~0ULL;
            qv[i] = r;
            o += snprintf(buf + o, sizeof buf - (size_t) o, " %s%s",
                          DPL_Q[i], qok[i] ? "" : "(no)");
            if (o >= (int) sizeof buf - 64)
                break;
        }
        Xlog("dpad: probe - script natives (no = not a native, skipped):%s", buf);
        o = 0;
        for (int i = 0; i < DPL_NC; i++) {
            uint64_t c = Fnv64Rt(DPL_CMD[i]), r =
                DcCall(FNV64("joypad-command-active?"), 1, &c);
            cmd_ok[i] = r != ~0ULL;
            cmd_on[i] = cmd_ok[i] && (r & 0xff);
        }
        Xlog("dpad: logger on (t2r-no-dpad-log.txt = off) - the slot table the engine loads is *player-inventory-slots-v2* (exe); %d commands watched", DPL_NC);
        frame0 = g_frame;
        held = ~0ULL - 1;
    }
    // commands: rising / falling edges
    for (int i = 0; i < DPL_NC; i++) {
        if (!cmd_ok[i])
            continue;
        uint64_t c = Fnv64Rt(DPL_CMD[i]), r =
            DcCall(FNV64("joypad-command-active?"), 1, &c);
        int on = r != ~0ULL && (r & 0xff);
        if (on != cmd_on[i]) {
            cmd_on[i] = on;
            Xlog("dpad: command %s %s", DPL_CMD[i], on ? "DOWN" : "up");
        }
    }
    int m = GameFocused()
        && (GetAsyncKeyState(VK_MBUTTON) & 0x8000) ? 1 : 0;
    if (m != mmb) {
        mmb = m;
        Xlog("dpad: middle mouse button %s",
             m ? "DOWN (hover wheel)" : "up");
    }
    // UI state natives
    for (int i = 0; i < DPL_NQ; i++) {
        if (!qok[i])
            continue;
        uint64_t r = DcCall(Fnv64Rt(DPL_Q[i]), 0, &none);
        if (r != qv[i]) {
            const char *n = (r >> 32) ? DplName(r) : NULL;
            Xlog("dpad: %s %llx -> %llx%s%s", DPL_Q[i],
                 (unsigned long long) qv[i], (unsigned long long) r,
                 n ? " = " : "", n ? n : "");
            qv[i] = r;
        }
    }
    // the hand and the pack
    uint64_t h = DcCall(FNV64("get-player-weapon-id"), 0, &none);
    if (h != held) {
        const char *n = DplName(h);
        Xlog("dpad: in the hand %llx (%s)", (unsigned long long) h,
             n ? n : "?");
        held = h;
    }
    if (!(g_frame % 15))
        DplCarried(g_frame - frame0 < 16);
    DpwTick();
}
