// ---------------------------------------------------------------- 'blocked' notice: picker refusals as a panel, not a white line
// Included by t2r-ext.c after killfeed.c (one translation unit). Log prefix 'denynote:'. Widgets: denynote_widgets.py - group
// anim-group.denyNote on the four pickers (picker-skill = the loadout list, picker-primary / -secondary / -purchase) and t2rDenyNote in
// hud-root (the spawn-time "Too many of X on team. Switched to Y." has no picker under it). Panel + red accent bar + circle-slash icon +
// the sentence wrapped over up to 4 lines (we insert the line breaks ourselves from the feed's per-letter width estimate, so the panel
// height = lines * 60 + 60 matches what the engine draws; the text box is 890 wide against a 790 wrap width = 12 % slack).
// Callers: ui_deny (every picker refusal: ui_deny_lp "Not enough Loadout Points!") and gunlimit.c gl_say ("Too many of GUN on team."). When no
// notice widgets are reachable (older pak, other page) dn_show returns 0 and the caller keeps the old msg_hook line.
// Timing = the old message line: 150 ms fade in (0.1 -> 1), last 300 ms fade out; alpha goes to every child (a group alpha alone is not
// known to reach its children on menu pages). Strings are written while hidden and shown a frame later (gui2 text engine trait).
// Stale handles: the page / match is keyed (top page + the page's anim-group handle, or the hud group + substate 6); a change drops the
// notice at once, widgets are looked up afresh every tick while one is up.
// Restyled (the black card + red bar + red circle-slash looked generic): PANEL / BAR / SLASH are now three stacked copies of the game's
// soft blob texture (a soft uneven dark smudge, no edge), RING is the thin warning badge icon, TEXT off-white (denynote_widgets.py). DN_BLOBS = same
// language and numbers as notify.c's NTF_BLOBS, in group units (box = x 0..DN_NOTE_W, y 0..H). Picker notices sit at the screen's vertical centre now
// (over the list, which is dark; the old spot covered the page title, which a smudge cannot hide).
enum { DNW_GRP, DNW_PANEL, DNW_BAR, DNW_RING, DNW_SLASH, DNW_TEXT, DNW_N };
#define DN_NOTE_W   1100.0f
#define DN_BLOB_W0  1000.0f
#define DN_BLOB_H0  400.0f
#define DN_PK_CY    1010.0f
static const float DN_BLOBS[3][7] =
    { { 1.55f, 380.0f, 2.60f, 200.0f, 0.00f, 0.00f, 1.00f }, { 1.20f,
                                                              240.0f,
                                                              2.30f,
                                                              140.0f,
                                                              -0.10f,
                                                              0.04f,
                                                              1.00f },
{ 1.00f, 200.0f, 2.00f, 110.0f, 0.12f, -0.03f, 1.00f }
};                              // wm, wa, hm, ha (units), dx, dy, peak - dense + wide: the list rows must not read through

#define DN_X        1370.0f
#define DN_TOP_PK   270.0f
#define DN_TOP_HUD  300.0f
#define DN_MAXH     270.0f
#define DN_ICON     96.0f
#define DN_ICON_X   56.0f
#define DN_TEXT_X   190.0f
#define DN_TEXT_H   250.0f
#define DN_LINE_H   60.0f
#define DN_PAD_V    30.0f
#define DN_MINH     130.0f
#define DN_WRAP_W   790.0f
#define DN_PT       36.0f
#define DN_MAXL     4
static struct {
    int st, hud, lines;
    uint64_t t0, until, set_frame, key;
    char text[220];
    float la;
} g_dn;
static int DnPageOk(uint64_t tp)
{
    static uint64_t p[5];
    if (!p[0]) {
        p[0] = PageId("picker-skill");
        p[1] = PageId("picker-primary");
        p[2] = PageId("picker-secondary");
        p[3] = PageId("picker-purchase");
        p[4] = PageId("gearup13");
    }
    for (int i = 0; i < 5; i++)
        if (p[i] && tp == p[i])
            return 1;
    return 0;
}

// the notice widgets reachable right now: 1 = on a picker page (ui_w), 2 = in the match HUD (hudw), 0 = none. key = what identifies this instance.
static int DnResolve(uint64_t w[DNW_N], uint64_t *key)
{
    static const char *const SUF[DNW_N] =
        { "", ".panel", ".bar", ".ring", ".slash", ".text" };
    char p[64];
    uint64_t tp = TopPage();
    if (tp && DnPageOk(tp)) {
        uint64_t rw = UiW("anim-group");
        if (!rw)
            return 0;
        for (int i = 0; i < DNW_N; i++) {
            snprintf(p, sizeof p, "anim-group.denyNote%s", SUF[i]);
            w[i] = UiW(p);
            if (!w[i])
                return 0;
        }
        *key = tp ^ rw * 0x9E3779B97F4A7C15ULL ^ 1;
        return 1;
    }
    if (MatchSubstate() == 6) {
        for (int i = 0; i < DNW_N; i++) {
            snprintf(p, sizeof p, "t2rDenyNote%s", SUF[i]);
            w[i] = Hudw(p);
            if (!w[i])
                return 0;
        }
        *key = w[DNW_GRP] ^ 2;
        return 2;
    }
    return 0;
}

// greedy word wrap of s into out (lines joined by '\n'); returns the line count (capped at DN_MAXL, the rest of the sentence stays on the last line)
static int DnWrapW(const char *s, char *out, size_t n, float wrapw)
{
    const float sc = DN_PT / 30.0f;
    size_t o = 0;
    int lines = 1;
    float lw = 0.0f;
    const char *p = s;
    while (*p && o + 8 < n) {
        const char *e = p;
        while (*e && *e != ' ' && *e != '\n')
            e++;
        char word[100];
        size_t wl = (size_t) (e - p);
        if (wl > sizeof word - 1)
            wl = sizeof word - 1;
        memcpy(word, p, wl);
        word[wl] = 0;
        float ww = FeedTextW(word, sc), sp = FeedTextW(" ", sc);
        if (lw > 0.0f && lw + sp + ww > wrapw && lines < DN_MAXL) {
            out[o++] = '\n';
            lines++;
            lw = 0.0f;
        } else if (lw > 0.0f) {
            out[o++] = ' ';
            lw += sp;
        }
        for (size_t i = 0; i < wl && o + 2 < n; i++)
            out[o++] = word[i];
        lw += ww;
        p = e;
        if (*p == '\n') {
            if (lines < DN_MAXL) {
                out[o++] = '\n';
                lines++;
                lw = 0.0f;
            }
            p++;
        } else
            while (*p == ' ')
                p++;
    }
    out[o] = 0;
    return lines;
}

static int DnWrap(const char *s, char *out, size_t n)
{
    return DnWrapW(s, out, n, DN_WRAP_W);
}

static void DnAlpha(const uint64_t *w, float a)
{
    FeedAlpha(w[DNW_PANEL], a * DN_BLOBS[0][6]);
    FeedAlpha(w[DNW_BAR], a * DN_BLOBS[1][6]);
    FeedAlpha(w[DNW_SLASH], a * DN_BLOBS[2][6]);
    FeedAlpha(w[DNW_RING], a);
    FeedAlpha(w[DNW_TEXT], a);
}

// returns 1 when the notice shows (the caller sends no HUD line), 0 when there are no widgets for it
static int DnShow(const char *msg, float secs)
{
    uint64_t w[DNW_N], key;
    if (!msg || !msg[0])
        return 0;
    int dnk = NtfKindOf(msg);
    if (dnk == NTF_INFO)
        dnk = NTF_LOADOUT;      // a refusal that matches no keyword is a loadout / limit one (the callers: ui_deny, gunlimit)
    {
        uint64_t tp = TopPage();
        if (!(tp && DnPageOk(tp)) && NtfPush(dnk, msg, secs))
            return 1;
    }                           // match HUD: the styled notification (notify.c); pickers keep this panel
    int where = DnResolve(w, &key);
    if (!where) {
        static int nl;
        if (LOGCAP(nl, 3))
            Xlog("denynote: no notice widgets here (page 0x%llx, substate %d) - plain message", (unsigned long long) TopPage(), (int) MatchSubstate());
        return 0;
    }
    uint64_t now = GetTickCount64();
    const char *tr = LocTr(msg);
    if (!tr)
        tr = msg;
    if (g_dn.st && g_dn.key == key && !strcmp(g_dn.text, tr)
        && now + 500 < g_dn.until)
        return 1;               // already up
    if (g_dn.st && g_dn.key != key)
        g_dn.st = 0;
    char buf[320];
    int lines = DnWrap(tr, buf, sizeof buf);
    float H = (float) lines * DN_LINE_H + 2.0f * DN_PAD_V;
    if (H < DN_MINH)
        H = DN_MINH;
    WVisible(w[DNW_GRP], 0);
    DnAlpha(w, 0.0f);
    ((void (*)(uint64_t, const char *)) T2R_SYM(kFnSetText)) (w[DNW_TEXT],
                                                              buf);
    WPivot(w[DNW_GRP], DN_X,
            where == 1 ? DN_PK_CY - H * 0.5f : DN_TOP_HUD);
    {
        const int bw[3] = { DNW_PANEL, DNW_BAR, DNW_SLASH };    // the smudge (group units, centred on the box)
        for (int i = 0; i < 3; i++) {
            const float *b = DN_BLOBS[i];
            float wu = DN_NOTE_W * b[0] + b[1], hu = H * b[2] + b[3];
            WScale(w[bw[i]], wu / DN_BLOB_W0, hu / DN_BLOB_H0);
            WPivot(w[bw[i]],
                    DN_NOTE_W * 0.5f + b[4] * DN_NOTE_W - wu * 0.5f,
                    H * 0.5f + b[5] * H - hu * 0.5f);
    }}
    ((void (*)(uint64_t, const char *)) T2R_SYM(kFnSetImage)) (w[DNW_RING], NtfIconPath(dnk));        // the ring widget is the kind's badge now (written while hidden)
    WPivot(w[DNW_RING], DN_ICON_X, (H - DN_ICON) * 0.5f);
    WPivot(w[DNW_TEXT], DN_TEXT_X, (H - DN_TEXT_H) * 0.5f);
    g_dn.st = 1;
    g_dn.hud = where == 2;
    g_dn.key = key;
    g_dn.lines = lines;
    g_dn.set_frame = g_frame;
    g_dn.la = -1.0f;
    g_dn.until =
        now + (uint64_t) ((secs > 0 && secs < 30 ? secs : 5) * 1000);
    snprintf(g_dn.text, sizeof g_dn.text, "%s", tr);
    Xlog("denynote: \"%.100s\" %.1f s, %d line(s), panel %.0f high (%s)",
         tr, secs, lines, H, where == 1 ? "picker page" : "match HUD");
    return 1;
}

static void FeatureDenynote(void)
{
    if (!g_dn.st)
        return;
    uint64_t w[DNW_N], key, now = GetTickCount64();
    if (!DnResolve(w, &key) || key != g_dn.key) {
        g_dn.st = 0;
        return;
    }                           // page popped / rebuilt / match over: widgets are gone
    if (g_dn.st == 1) {
        if (g_frame <= g_dn.set_frame)
            return;             // the string has had no frame yet
        WVisible(w[DNW_GRP], 1);
        g_dn.st = 2;
        g_dn.t0 = now;
    }
    float a = 1.0f;
    if (now - g_dn.t0 < 150)
        a = 0.1f + 0.9f * (float) (now - g_dn.t0) / 150.0f;
    if (now > g_dn.until) {
        float o = 1.0f - (float) (now - g_dn.until) / 300.0f;
        if (o < a)
            a = o;
    }
    if (a <= 0.0f) {
        DnAlpha(w, 0.0f);
        WVisible(w[DNW_GRP], 0);
        g_dn.st = 0;
        return;
    }
    if (__builtin_fabsf(a - g_dn.la) > 0.01f) {
        DnAlpha(w, a);
        g_dn.la = a;
    }
}
