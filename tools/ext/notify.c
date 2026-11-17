// ---------------------------------------------------------------- the styled HUD notification
// Included by t2r-ext.c after denynote.c (one translation unit). Log prefix 'notify:'. Widgets: notify_widgets.py (t2rNtL / t2rNtM / t2rNtR =
// soft dark feathered rectangle in three pieces, t2rNtIc = icon, all in hud-root). Off: t2r-old-notify.txt next to the exe (polled every 2 s)
// = the old paths (t2rMsg line / nametag-style line / blocked panel / engine message) as before.
//
// Every gameplay notification of ours ends here: msg_hook (the HUD message line: "Teammate is healing you", loadout, interrogation, supply
// box ... and winmm's own calls), msg_tag (nametag-style: marked, Covert Training refusal), notice.c (server notices), dn_show in the match HUD
// (gun limit "Too many of X on team", "Something is in the way"). On the picker pages dn_show keeps its panel. Outside the match HUD
// (substate != 6) the old plain message path stays.
//
// Look (restyled: the flat card looked generic; now TLOU2's own soft backing language): ONE box at a time, horizontally centred,
// vertically on the radar's centre (the radar's centre vector at winmm .rdata 0x1800A79D0, lane 0 = screen y in the 1280 x 720 text space,
// after install_radar_shift's +72), no panel and no bar: a soft uneven dark smudge = three stacked copies of the game's own soft blob texture
// (hud/status-effects/radial-gradient-arrow.png, black-tinted; NTF_BLOBS), the game's own notification badge as icon, the text
// drawn with the nametags' own routine (winmm 0x1800039D0, font + outline of every nametag; measured with 0x1800051B0) wrapped to at most 2 lines
// inside NTF_MAXW, a small icon per kind at the left. A new one that arrives while one shows waits in a queue of 4 (identical text is dropped).
// Timing like the HUD's message line: 150 ms fade in, 300 ms fade out. Strings / icon path are written while hidden and shown one frame later.
#define NTF_BLOB_W0  1000.0f    // = notify_widgets.BLOB_W (unscaled blob widget, canvas units)
#define NTF_BLOB_H0  400.0f     // = notify_widgets.BLOB_H
#define NTF_TXT_PX   18.0f      // text line height in the 1280 x 720 text space (a nametag is ~16; the killfeed 18)
#define NTF_PITCH    23.0f      // two-line pitch
#define NTF_PADX     26.0f      // padding left / right of the content (the smudge reaches well beyond it)
#define NTF_PADY     14.0f
#define NTF_ICON_PX  28.0f      // = notify_widgets.ICON / 3
// the smudge: per blob (width = box w * wm + wa, height = box h * hm + ha, centre offset dx * box w / dy * box h, peak alpha); same numbers as
// notify_preview.py NT_BLOBS (the offline preview of this look)
static const float NTF_BLOBS[3][7] = { { 1.18f, 70.0f, 1.45f, 16.0f, 0.00f, 0.00f, 0.60f }, { 0.78f, 36.0f, 1.30f, 10.0f, -0.20f, 0.07f, 0.36f },       // lighter, smaller, flatter (was 0.94 / 0.60 / 0.55)
{ 0.64f, 24.0f, 1.20f, 8.0f, 0.24f, -0.06f, 0.32f }
};

#define NTF_TOP_Y    105.0f     // vertical centre in the 1280 x 720 text space
#define NTF_GAP      10.0f
#define NTF_MAXW     430.0f     // widest line before wrapping
#define NTF_HARDW    760.0f     // a line wider than this (one long word) shrinks the whole text
#define NTF_QN       6
// (the 2013 Interrogation banner): a LONG thin bar with the SAME background as the toast (the three soft blobs), ONE long line of gold text, no icon, centred
// at 1080p x 960 / text cap top y 286 .. baseline 310 (centre line 298 px = 198.7 in the 1280 x 720 text space), font 33 px at 1080p (= 22 text px), colour #E1B23C. Pops in on one
// frame (no fade-in), holds, fades in 0.15 s; queued like the toasts so no two overlap (ntf_banner). The geometry numbers are mirrored by tools/lockbox_banner_preview.py.
#define NTF_BAN_PX   26.5f
#define NTF_BAN_CY   198.7f
#define NTF_BAN_PADX 34.0f
#define NTF_BAN_PADY 4.5f
// the banner's bar = three flat overlapping pieces of the same blob (a long rectangle with straight top / bottom, feathered ends) instead of the toast's oval smudge
static const float NTF_BAN_BLOBS[3][7] =
    { { 0.62f, 60.0f, 0.0f, 34.0f, -0.30f, -0.08f, 0.62f }, { 0.62f, 60.0f,
                                                             0.0f, 34.0f,
                                                             0.0f, -0.08f,
                                                             0.62f },
    { 0.62f, 60.0f, 0.0f, 34.0f, 0.30f, -0.08f, 0.62f } };
#define NTF_BAN_COL  0x00DCE6EAu        // the toast's own text colour (not yellow)
// the game's own notification badges where it has one (TLOU2 hud/notification/notifications_icons: confirmed decodable in gui2-textures-common.pak, the
// pak the picker pages draw from too - empty-slot.png is used there), our thin copies of their ring style for the '!' / 'i' otherwise
static const char *const NTF_ICON[NTF_N] = {
    "streaming/t2r/icons/notify-info.png",      // INFO
    "streaming/t2r/icons/notify-heal.png",      // HEAL
    "streaming/t2r/icons/notify-revive.png",    // REVIVE (handshake)
    "streaming/t2r/icons/notify-warn.png",      // WARN ('!' ring)
    "streaming/t2r/icons/notify-info.png",      // SERVER (the info icon, not the notice book)
    "streaming/t2r/icons/notify-loadout.png",   // LOADOUT (wrench + screwdriver)
    "streaming/t2r/icons/notify-covert.png",    // COVERT (hooded)
    "streaming/t2r/icons/notify-move.png",      // MOVE (chevrons: vault / traversal)
    "streaming/t2r/icons/notify-craft.png",     // CRAFT (supplies jar)
    "streaming/t2r/icons/notify-blade.png",     // BLADE (shiv: interrogation / execute)
    "streaming/t2r/icons/notify-bomb.png",      // BOMB
    "streaming/t2r/icons/notify-supply.png"
};                              // SUPPLY (the clan supply satchel: 2013 supplies glyph, off-white, satchel_icon.py)

static const char *NtfIconPath(int kind)
{
    return kind >= 0 && kind < NTF_N ? NTF_ICON[kind] : NTF_ICON[NTF_INFO];
}
static const char *const NTF_KIND[NTF_N] =
    { "info", "heal", "revive", "warn", "server", "loadout", "covert",
"move", "craft", "blade", "bomb", "supply" };
typedef struct {
    int kind;
    float secs;
    char text[200];
    uint64_t t_enq;
    int banner;
} NtfItemT;
static NtfItemT g_ntfq[NTF_QN];
static int g_ntfqn;
static volatile LONG g_ntfLock;
static uint64_t g_ntfw[4];      // t2rNtL, t2rNtM, t2rNtR, t2rNtIc
static int g_ntfOk;            // widgets resolved for this match
static int g_ntfOldFlag;
static uint64_t g_ntfOldChk;
static struct {
    int st, kind, nl, banner;
    char text[200], line[2][160];
    float lw[2], tbw, size, th, innerw, boxh, cy, a, la;
    uint64_t t0, until, set_frame, key;
    int menu;
    NtfItemT it;
} g_ntf;                        // st 0 idle, 1 written (hidden, waits a frame), 2 shown
static void NtfLock(void)
{
    while (__sync_lock_test_and_set(&g_ntfLock, 1))
        YieldProcessor();
}

static void NtfUnlock(void)
{
    __sync_lock_release(&g_ntfLock);
}

static int NtfHas(const char *s, const char *w)
{                               // case-insensitive substring
    size_t n = strlen(w);
    for (; *s; s++)
        if (!_strnicmp(s, w, n))
            return 1;
    return 0;
}

static int NtfKindOf(const char *t)
{
    // the icon follows the CONTEXT: the '!' ring only for real alerts (marked, interrogated, lockbox stolen / leaking, defend);
    // checked in this order, first match wins
    if (!t)
        return NTF_INFO;
    if (!_strnicmp(t, "Gift:", 5) || NtfHas(t, "crafted a gift"))
        return NTF_CRAFT;       // Crafter gifts (gift.c)
    if (NtfHas(t, "Covert"))
        return NTF_COVERT;      // "Player can't be marked due to Covert Training III"
    if (NtfHas(t, "been marked"))
        return NTF_WARN;        // "You've been marked!"
    if (NtfHas(t, "revive") || NtfHas(t, "reviving"))
        return NTF_REVIVE;
    if (NtfHas(t, "heal") || NtfHas(t, "health") || NtfHas(t, "medkit")
        || NtfHas(t, "first aid") || NtfHas(t, "Second Chance"))
        return NTF_HEAL;
    if (NtfHas(t, "bomb"))
        return NTF_BOMB;        // "TOO MANY BOMBS PLACED", "CAN'T PLACE THE BOMB HERE"
    if (NtfHas(t, "Loadout") || NtfHas(t, "Too many")
        || NtfHas(t, "Not enough") || NtfHas(t, "per team")
        || NtfHas(t, "equipped") || NtfHas(t, "on team")
        || NtfHas(t, "limit"))
        return NTF_LOADOUT;     // refusals of the pickers / gun limits, loadout changes
    if (NtfHas(t, "in the way") || NtfHas(t, "vault"))
        return NTF_MOVE;        // "Something is in the way. Vault it first."
    if (NtfHas(t, "being interrogated")
        || NtfHas(t, "you were interrogated") || NtfHas(t, "stole")
        || NtfHas(t, "is leaking") || NtfHas(t, "knows where")
        || NtfHas(t, "defend"))
        return NTF_WARN;        // real alerts
    if (NtfHas(t, "interrogat") || NtfHas(t, "execute")
        || NtfHas(t, "shiv"))
        return NTF_BLADE;       // interrogation / execution / shiv prompts
    if (NtfHas(t, "Crafter") || NtfHas(t, "Scavenger")
        || NtfHas(t, "Lucky Break") || NtfHas(t, "Gift box")
        || NtfHas(t, "lockbox") || NtfHas(t, "crafting")
        || NtfHas(t, "supplies") || NtfHas(t, "ammo"))
        return NTF_CRAFT;
    if (NtfHas(t, "can't") || NtfHas(t, "cannot"))
        return NTF_MOVE;
    return NTF_INFO;
}

static int NtfResolve(void)
{
    static uint64_t last_try;
    uint64_t now = GetTickCount64();
    if (last_try && now - last_try < 1000)
        return 0;
    last_try = now;
    static const char *const N[4] =
        { "t2rNtL", "t2rNtM", "t2rNtR", "t2rNtIc" };
    for (int i = 0; i < 4; i++) {
        g_ntfw[i] = Hudw(N[i]);
        if (!g_ntfw[i]) {
            static int nl;
            if (LOGCAP(nl, 2))
                Xlog("notify: %s not in hud-root (older gui2-widgets.pak) - old notification paths stay", N[i]);
            return 0;
        }
    }
    g_ntfOk = 1;
    Xlog("notify: widgets found - styled notifications on");
    return 1;
}

// Called by every notification path. Returns 1 when the styled notification takes the message (the caller does nothing more), 0 for the old path.
// (May be called off the main thread: only queues; the widgets are driven from feature_notify.)
static int NtfPush2(int kind, const char *text, float secs, int banner);
static int NtfPush(int kind, const char *text, float secs)
{
    return NtfPush2(kind, text, secs, 0);
}

static int NtfBanner(const char *text, float secs)
{
    return NtfPush2(NTF_INFO, text, secs, 1);
}                               // the 2013 centre banner (see NTF_BAN_*)

static int NtfPush2(int kind, const char *text, float secs, int banner)
{
    if (!text || !text[0] || IsBadReadPtr(text, 2))
        return 0;
    if (OvOff("t2r-old-notify.txt", &g_ntfOldFlag, &g_ntfOldChk))
        return 0;
    // in the match HUD every kind; on the menu pages server notices only - the other callers keep their menu paths (picker panel, plain line)
    if (MatchSubstate() == 6 ? !g_ntfOk : kind != NTF_SERVER)
        return 0;
    if (kind < 0 || kind >= NTF_N)
        kind = NtfKindOf(text);
    NtfLock();
    uint64_t now = GetTickCount64();
    int dup = g_ntf.st && !strncmp(g_ntf.text, text, sizeof g_ntf.text - 1)
        && now + 500 < g_ntf.until;
    for (int i = 0; i < g_ntfqn && !dup; i++)
        dup = !strncmp(g_ntfq[i].text, text, sizeof g_ntfq[i].text - 1);
    if (!dup) {
        if (g_ntfqn >= NTF_QN) {
            memmove(g_ntfq, g_ntfq + 1, sizeof g_ntfq[0] * (NTF_QN - 1));
            g_ntfqn = NTF_QN - 1;
        }                       // full: the oldest waiting one gives way
        int at = g_ntfqn;
        if (banner) {
            at = 0;
            while (at < g_ntfqn && g_ntfq[at].banner)
                at++;
            memmove(g_ntfq + at + 1, g_ntfq + at,
                    sizeof g_ntfq[0] * (size_t) (g_ntfqn - at));
        }                       // banners go before the toasts, in order among themselves
        g_ntfqn++;
        NtfItemT *q = &g_ntfq[at];
        q->kind = kind;
        q->secs = secs;
        q->t_enq = now;
        q->banner = banner;
        snprintf(q->text, sizeof q->text, "%s", text);
    }
    NtfUnlock();
    if (!dup)
        Xlog("notify: %s %s \"%.120s\" %.1f s", banner ? "BANNER" : "kind",
             NTF_KIND[kind], text, secs);
    return 1;
}

static float NtfW(const char *s, float size)
{
    typedef uint64_t(*fn_meas) (const char *, float);
    if (!s[0])
        return 0.0f;
    uint64_t m = ((fn_meas) T2R_SYM(kFnMeasureText)) (s, size);
    float w;
    uint32_t ml = (uint32_t) m;
    memcpy(&w, &ml, 4);
    return w > 0.0f && w < 4000.0f ? w : 0.0f;
}

static float NtfTextSize(float px)
{                               // nametag-font size for a text line height of `px` in the 1280 x 720 text space (interrogation.c's persistent lines)
    typedef uint64_t(*fn_meas) (const char *, float);
    static float lineh;
    if (!lineh) {
        uint64_t m = ((fn_meas) T2R_SYM(kFnMeasureText)) ("Mg", 2.0f);
        float hh;
        uint32_t mh = (uint32_t) (m >> 32);
        memcpy(&hh, &mh, 4);
        lineh = hh > 1.0f && hh < 400.0f ? hh / 2.0f : 40.0f;
    }
    return px / lineh;
}

static void NtfStart(const NtfItemT *it, uint64_t now)
{
    typedef uint64_t(*fn_meas) (const char *, float);
    static float lineh;
    if (!lineh) {               // the nametag font's line height per size unit ("Mg" at size 2, as kf_measure)
        uint64_t m = ((fn_meas) T2R_SYM(kFnMeasureText)) ("Mg", 2.0f);
        float hh;
        uint32_t mh = (uint32_t) (m >> 32);
        memcpy(&hh, &mh, 4);
        lineh = hh > 1.0f && hh < 400.0f ? hh / 2.0f : 40.0f;
        Xlog("notify: nametag font line height %.2f px per size unit -> text size %.3f for %.0f px", lineh, NTF_TXT_PX / lineh, NTF_TXT_PX);
    }
    float size = (it->banner ? NTF_BAN_PX : NTF_TXT_PX) / lineh;
    const char *tr = LocTrDraw(it->text);     // localisation (diacritics folded for the raw draw)
    char buf[200];
    snprintf(buf, sizeof buf, "%s", tr ? tr : it->text);
    // one line when it fits NTF_MAXW, else the most balanced split into 2 lines (no orphan last word)
    char l0[160] = "", l1[160] = "";
    int nl = 1;
    snprintf(l0, sizeof l0, "%s", buf);
    if (!it->banner && NtfW(l0, size) > NTF_MAXW) {    // (a banner is ONE long line)
        char *sp[48];
        int ns = 0;
        for (char *q = buf; *q && ns < 48; q++)
            if (*q == ' ' && q > buf && q[-1] != ' ')
                sp[ns++] = q;
        float best = 1e9f;
        int bk = -1;
        for (int k = 0; k < ns; k++) {
            char c0[160], c1[160];
            size_t n0 = (size_t) (sp[k] - buf);
            if (n0 > sizeof c0 - 1)
                n0 = sizeof c0 - 1;
            memcpy(c0, buf, n0);
            c0[n0] = 0;
            snprintf(c1, sizeof c1, "%s", sp[k] + 1);
            float m0 = NtfW(c0, size), m1 = NtfW(c1, size), mm =
                m0 > m1 ? m0 : m1;
            if (mm < best) {
                best = mm;
                bk = k;
            }
        }
        if (bk >= 0) {
            size_t n0 = (size_t) (sp[bk] - buf);
            if (n0 > sizeof l0 - 1)
                n0 = sizeof l0 - 1;
            memcpy(l0, buf, n0);
            l0[n0] = 0;
            snprintf(l1, sizeof l1, "%s", sp[bk] + 1);
            nl = 2;
        }
    }
    float w0 = NtfW(l0, size), w1 = NtfW(l1, size);
    float mx = w0 > w1 ? w0 : w1;
    if (mx > NTF_HARDW) {
        size *= NTF_HARDW / mx;
        w0 *= NTF_HARDW / mx;
        w1 *= NTF_HARDW / mx;
        mx = NTF_HARDW;
    }                           // one huge word: shrink, never run off the screen
    memcpy(g_ntf.line[0], l0, sizeof l0);
    memcpy(g_ntf.line[1], l1, sizeof l1);
    g_ntf.nl = nl;
    g_ntf.lw[0] = w0;
    g_ntf.lw[1] = w1;
    g_ntf.tbw = mx;
    g_ntf.size = size;
    g_ntf.th = (it->banner ? NTF_BAN_PX : NTF_TXT_PX) * size / ((it->banner ? NTF_BAN_PX : NTF_TXT_PX) / lineh);        // = the nominal px unless shrunk
    g_ntf.banner = it->banner;
    g_ntf.kind = it->kind;
    g_ntf.menu = 0;
    g_ntf.it = *it;
    g_ntf.banner = it->banner;
    snprintf(g_ntf.text, sizeof g_ntf.text, "%s", it->text);
    float icw = it->banner ? 0.0f : NTF_ICON_PX + NTF_GAP;
    float innerw = mx + icw;
    float innerh = (float) (nl - 1) * NTF_PITCH + g_ntf.th;
    if (innerh < NTF_ICON_PX && !it->banner)
        innerh = NTF_ICON_PX;
    float boxw = innerw + 2.0f * (it->banner ? NTF_BAN_PADX : NTF_PADX);
    float boxh = innerh + 2.0f * (it->banner ? NTF_BAN_PADY : NTF_PADY);
    g_ntf.innerw = innerw;
    g_ntf.boxh = boxh;
    // vertical centre = the radar's centre (text space y of the radar's centre vector, read live)
    // moved up to the top centre (the old match timer's place is free) - was the radar's centre (winmm .rdata 0x1800A79D0, ~177)
    float cy = it->banner ? NTF_BAN_CY : NTF_TOP_Y;
    g_ntf.cy = cy;
    // widgets (canvas units = 3 x text px; x is the offset from the screen centre; the blobs are top-left pivoted, scaled about it)
    uint64_t *w = g_ntfw;
    for (int i = 0; i < 4; i++)
        WVisible(w[i], 0);
    for (int i = 0; i < 4; i++)
        FeedAlpha(w[i], 0.0f);
    for (int i = 0; i < 3; i++) {
        const float *b = it->banner ? NTF_BAN_BLOBS[i] : NTF_BLOBS[i];
        float wp = (boxw * b[0] + b[1]) * 3.0f, hp =
            (boxh * b[2] + b[3]) * 3.0f;
        WScale(w[i], wp / NTF_BLOB_W0, hp / NTF_BLOB_H0);
        WPivot(w[i], b[4] * boxw * 3.0f - wp * 0.5f,
                (cy + b[5] * boxh) * 3.0f - hp * 0.5f);
    }
    ((fn_settext_t) T2R_SYM(kFnSetImage)) (w[3], NTF_ICON[it->kind]);
    WPivot(w[3], -innerw * 1.5f, (cy - NTF_ICON_PX * 0.5f) * 3.0f);
    g_ntf.st = 1;
    g_ntf.set_frame = g_frame;
    g_ntf.la = -1.0f;
    g_ntf.a = 0.0f;
    float secs = it->secs;
    g_ntf.until =
        now +
        (uint64_t) ((secs > 0
                     && secs < (it->kind ==
                                NTF_SERVER ? 600.0f : 30.0f) ? secs : 5.0f)
                    * 1000);
    if (it->banner)
        ((fn_settext_t) T2R_SYM(kFnSetImage)) (w[3], NTF_ICON[NTF_INFO]);       // (the icon widget stays at alpha 0 for a banner)
    Xlog("notify: show %s \"%.100s\" %d line(s) box %.0f x %.0f px at y %.1f (radar centre), %.1f s", NTF_KIND[it->kind], g_ntf.text, nl, boxw, boxh, cy, (double) (g_ntf.until - now) / 1000.0);
}

// The sound of a toast: our toasts had none, so it is the game's own "new item / journal entry added" chime
// (sfx-tutorial-journal-entry-added, bank gbl-hud, re/journal-notify-1020.md), falling back to the stock menu tick (sfx-cycle, gbl-menu) where that
// event is not loaded (menus). Played once when the toast turns visible. Logs "notify: sound ..." (first 12).
static void NtfSound(void)
{
    static const char *const S[] =
        { "sfx-tutorial-journal-entry-added", "sfx-cycle" };
    static int nl;
    for (int i = 0; i < 2; i++) {
        uint64_t a[2] = { FNV64(S[i]), 0 };
        uint64_t r = DcCall(FNV64("play-sound"), 2, a);
        if (r != 0 && r != ~0ULL) {
            if (LOGCAP(nl, 12))
                Xlog("notify: sound %s -> %llx", S[i],
                     (unsigned long long) r);
            return;
        }
        if (LOGCAP(nl, 12))
            Xlog("notify: sound %s not available (%llx)", S[i],
                 (unsigned long long) r);
    }
}

// Menu pages (notify_widgets.add_notify_menu): the same five widgets under each page's anim-group. key = page + its anim-group handle (a rebuilt page = new key).
#define NTF_MENU_CY 95.0f       // vertical centre in text px: top centre - the title blocks are top-left and the lists start at ~170 px (510 units); a 2-line toast ends at ~130
static int NtfMenuResolve(uint64_t w[5], uint64_t *key)
{
    static const char *const N[5] =
        { "anim-group.t2rNfL", "anim-group.t2rNfM", "anim-group.t2rNfR",
"anim-group.t2rNfIc", "anim-group.t2rNfTx" };
    uint64_t tp = TopPage();
    if (!tp)
        return 0;
    for (int i = 0; i < 5; i++) {
        w[i] = UiW(N[i]);
        if (!w[i])
            return 0;
    }
    *key = tp ^ UiW("anim-group") * 0x9E3779B97F4A7C15ULL ^ 3;
    return 1;
}

static void NtfStartMenu(const NtfItemT *it, uint64_t now, uint64_t *w,
                           uint64_t key)
{
    const char *tr = LocTr(it->text);
    char buf[320];
    int nl =
        DnWrapW(tr ? tr : it->text, buf, sizeof buf, NTF_MAXW * 3.0f);
    float mxu = 0.0f;
    for (char *p = buf; *p;) {
        char *e = strchr(p, '\n');
        char seg[200];
        size_t n = e ? (size_t) (e - p) : strlen(p);
        if (n > sizeof seg - 1)
            n = sizeof seg - 1;
        memcpy(seg, p, n);
        seg[n] = 0;
        float lw = FeedTextW(seg, 36.0f / 30.0f);
        if (lw > mxu)
            mxu = lw;
        if (!e)
            break;
        p = e + 1;
    }
    // measured on A: the 30-glyph line drew ~243 px wide against the 206 estimated (the text widget's font runs wider than the feed's),
    // so the centred text spilled left over the icon - widen the estimate to the measured width
    float mx = mxu / 3.0f * 1.18f;
    float innerw = mx + NTF_ICON_PX + NTF_GAP, innerh = (float) nl * 20.0f;
    if (innerh < NTF_ICON_PX)
        innerh = NTF_ICON_PX;
    float boxw = innerw + 2.0f * NTF_PADX, boxh =
        innerh + 2.0f * NTF_PADY, cy = NTF_MENU_CY;
    g_ntf.innerw = innerw;
    g_ntf.boxh = boxh;
    g_ntf.cy = cy;
    g_ntf.nl = nl;
    for (int i = 0; i < 5; i++) {
        WVisible(w[i], 0);
        FeedAlpha(w[i], 0.0f);
    }
    for (int i = 0; i < 3; i++) {
        const float *b = NTF_BLOBS[i];
        float wp = (boxw * b[0] + b[1]) * 3.0f, hp =
            (boxh * b[2] + b[3]) * 3.0f;
        WScale(w[i], wp / NTF_BLOB_W0, hp / NTF_BLOB_H0);
        WPivot(w[i], b[4] * boxw * 3.0f - wp * 0.5f,
                (cy + b[5] * boxh) * 3.0f - hp * 0.5f);
    }
    ((fn_settext_t) T2R_SYM(kFnSetImage)) (w[3], NTF_ICON[it->kind]);
    WPivot(w[3], -innerw * 1.5f, (cy - NTF_ICON_PX * 0.5f) * 3.0f);
    ((void (*)(uint64_t, const char *)) T2R_SYM(kFnSetText)) (w[4], buf);       // the text widget: 1500 x 250 units, centred in both axes
    WPivot(w[4],
            (-innerw * 0.5f + NTF_ICON_PX + NTF_GAP + mx * 0.5f) * 3.0f -
            750.0f, cy * 3.0f - 125.0f);
    g_ntf.menu = 1;
    g_ntf.key = key;
    g_ntf.it = *it;
    g_ntf.kind = it->kind;
    g_ntf.banner = 0;
    snprintf(g_ntf.text, sizeof g_ntf.text, "%s", it->text);
    g_ntf.st = 1;
    g_ntf.set_frame = g_frame;
    g_ntf.la = -1.0f;
    g_ntf.a = 0.0f;
    g_ntf.until =
        now +
        (uint64_t) ((it->secs > 0
                     && it->secs < 600.0f ? it->secs : 5.0f) * 1000);
    Xlog("notify: show %s \"%.100s\" in the menus (page 0x%llx) %d line(s) box %.0f x %.0f px at y %.1f, %.1f s", NTF_KIND[it->kind], g_ntf.text, (unsigned long long) TopPage(), nl, boxw, boxh, cy, (double) (g_ntf.until - now) / 1000.0);
}

static void NtfRequeueFront(const NtfItemT *it, float secs)
{
    NtfLock();
    if (g_ntfqn < NTF_QN) {
        memmove(g_ntfq + 1, g_ntfq, sizeof g_ntfq[0] * (size_t) g_ntfqn);
        g_ntfq[0] = *it;
        g_ntfq[0].secs = secs;
        g_ntfqn++;
    }
    NtfUnlock();
}

// drops a notification by its text: still queued -> removed; showing -> fades out right away (~100 ms). (melee_dodge.c: the "DODGE" fallback must not outlive its window)
static void NtfCancel(const char *text)
{
    if (!text)
        return;
    NtfLock();
    int k = 0;
    for (int i = 0; i < g_ntfqn; i++)
        if (strncmp(g_ntfq[i].text, text, sizeof g_ntfq[i].text - 1))
            g_ntfq[k++] = g_ntfq[i];
    g_ntfqn = k;
    if (g_ntf.st
        && !strncmp(g_ntf.it.text, text, sizeof g_ntf.it.text - 1)) {
        uint64_t now = GetTickCount64();
        if (g_ntf.until > now - 200)
            g_ntf.until = now - 200;
    }
    NtfUnlock();
}

static void NtfDropNonserver(void)
{
    NtfLock();
    int k = 0;
    for (int i = 0; i < g_ntfqn; i++)
        if (g_ntfq[i].kind == NTF_SERVER)
            g_ntfq[k++] = g_ntfq[i];
    g_ntfqn = k;
    NtfUnlock();
}

static void FeatureNotify(void)
{
    uint64_t now = GetTickCount64();
    int hud = MatchSubstate() == 6;
    if (!hud && g_ntfOk) {
        g_ntfOk = 0;
        if (g_ntf.st && !g_ntf.menu) {
            // the match HUD is gone: a server toast (admin kick) that was cut moves on to the menu pages instead of vanishing
            float left =
                (float) ((int64_t) g_ntf.until - (int64_t) now) / 1000.0f;
            int kick = MsgIsKick(g_ntf.it.text);
            g_ntf.st = 0;
            if (g_ntf.kind == NTF_SERVER && (kick || left > 1.0f)) {
                NtfRequeueFront(&g_ntf.it, kick
                                  && left < 8.0f ? 8.0f : left);
                Xlog("notify: server toast cut by the match end - carried to the menu (%.1f s)", kick && left < 8.0f ? 8.0f : left);
            }
        }
        NtfDropNonserver();
    }                           // (hud-root is rebuilt per level: re-resolved each match)
    if (hud && g_ntf.st && g_ntf.menu)
        g_ntf.st = 0;           // the menu page's widgets are gone
    if (hud && !g_ntfOk && !NtfResolve()) {
        NtfLock();
        g_ntfqn = 0;
        NtfUnlock();
        return;
    }
    uint64_t mw[5], key = 0, *w = g_ntfw;
    int nw = 4;
    if (!hud) {
        w = mw;
        nw = 5;
        if (g_ntf.st) {         // a toast is up: the page it sits on must still be there
            if (!NtfMenuResolve(mw, &key)) {  // page closed / no widgets: it moves on to the next page
                float left =
                    (float) ((int64_t) g_ntf.until -
                             (int64_t) now) / 1000.0f;
                g_ntf.st = 0;
                if (left > 1.0f)
                    NtfRequeueFront(&g_ntf.it, left);
                return;
            }
            if (key != g_ntf.key) {     // another page (or the same one rebuilt): start again there
                float left =
                    (float) ((int64_t) g_ntf.until -
                             (int64_t) now) / 1000.0f;
                g_ntf.st = 0;
                if (left > 1.0f) {
                    NtfItemT it = g_ntf.it;
                    it.secs = left;
                    NtfStartMenu(&it, now, mw, key);
                }
                return;
            }
        }
    }
    if (!g_ntf.st) {
        NtfItemT it;
        int have = 0;
        if (hud) {
            NtfLock();
            if (g_ntfqn) {
                it = g_ntfq[0];
                memmove(g_ntfq, g_ntfq + 1,
                        sizeof g_ntfq[0] * (size_t) (--g_ntfqn));
                have = 1;
            }
            NtfUnlock();
            if (!have)
                return;
            NtfStart(&it, now);
            return;
        }
        NtfLock();
        if (g_ntfqn) {
            it = g_ntfq[0];
            have = 1;
        }
        NtfUnlock();
        if (!have)
            return;
        if (!NtfMenuResolve(mw, &key)) {      // booting / loading / a page without the widgets: stay queued (never the plain line)
            if (now - it.t_enq > 120000u) {
                NtfLock();
                if (g_ntfqn)
                    memmove(g_ntfq, g_ntfq + 1,
                            sizeof g_ntfq[0] * (size_t) (--g_ntfqn));
                NtfUnlock();
            }                   // kept lifetime 120 s; its own seconds count from when it first shows
            return;
        }
        NtfLock();
        if (g_ntfqn)
            memmove(g_ntfq, g_ntfq + 1,
                    sizeof g_ntfq[0] * (size_t) (--g_ntfqn));
        NtfUnlock();
        NtfStartMenu(&it, now, mw, key);
        return;
    }
    if (g_ntf.st == 1) {
        if (g_frame <= g_ntf.set_frame)
            return;             // the icon path / string has had no frame yet
        for (int i = 0; i < nw; i++)
            WVisible(w[i], 1);
        g_ntf.st = 2;
        g_ntf.t0 = now;
        if (!g_ntf.banner)
            NtfSound();
    }
    float a = 1.0f;
    if (!g_ntf.banner && now - g_ntf.t0 < 150)
        a = 0.1f + 0.9f * (float) (now - g_ntf.t0) / 150.0f;    // (a banner pops in on one frame)
    if (now > g_ntf.until) {
        float o =
            1.0f - (float) (now -
                            g_ntf.until) /
            (g_ntf.banner ? 150.0f : 300.0f);
        if (o < a)
            a = o;
    }
    if (a <= 0.0f) {
        for (int i = 0; i < nw; i++) {
            FeedAlpha(w[i], 0.0f);
            WVisible(w[i], 0);
        } g_ntf.st = 0;
        g_ntf.a = 0.0f;
        return;
    }
    if (HUD_MENU_UP())
        a = 0.0f;               // a menu over the HUD (hud_menu_up): the toast keeps ageing, drawn again when it closes
    g_ntf.a = a;
    if (__builtin_fabsf(a - g_ntf.la) > 0.01f) {
        for (int i = 0; i < nw; i++)
            FeedAlpha(w[i],
                       i <
                       3 ? a *
                       (g_ntf.
                        banner ? NTF_BAN_BLOBS[i][6] : NTF_BLOBS[i][6])
                       : (g_ntf.banner && i == 3) ? 0.0f : a);
        g_ntf.la = a;
    }
}

static void FeatureNotifyDraw(void)
{
    typedef void (*fn_text_va)(...);
    if (g_ntf.st != 2 || g_ntf.menu || g_ntf.a <= 0.0f
        || MatchSubstate() != 6)
        return;
    uint32_t al = (uint32_t) (g_ntf.a * 255.0f + 0.5f) << 24;
    float left = 640.0f - g_ntf.innerw * 0.5f + (g_ntf.banner ? 0.0f : NTF_ICON_PX + NTF_GAP);  // the text block starts right of the icon
    float y0 =
        g_ntf.cy - (float) (g_ntf.nl - 1) * NTF_PITCH * 0.5f -
        g_ntf.th * 0.5f;
    for (int i = 0; i < g_ntf.nl; i++) {
        if (g_ntf.lw[i] <= 0.0f)
            continue;
        float x = left + (g_ntf.tbw - g_ntf.lw[i]) * 0.5f;
        ((fn_text_va) T2R_SYM(kFnDrawTextVa)) (Fbits(x),
                                               Fbits(y0 +
                                                     (float) i *
                                                     NTF_PITCH),
                                               Fbits(g_ntf.size),
                                               (uint64_t) ((g_ntf.
                                                            banner ?
                                                            NTF_BAN_COL :
                                                            0x00DCE6EAu) |
                                                           al), "%s",
                                               g_ntf.line[i]);
    }
}
