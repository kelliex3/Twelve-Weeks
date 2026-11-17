// clan13.c - the in-game CLAN page. Included after gearup13.c (it reuses its fit / fade / repeat helpers). The widgets are tools/features/clan13_widgets.py
// (page to1/clan13, built from the same Builder as to1/gearup13: same backdrop, fonts, colours, footer prompts); like the match flow and GEAR UP pages this one is driven
// ENTIRELY from the tick and every text / icon / visibility goes through the dedup writers (ui_text, ui_image, ui_visible, w_alpha: no per-frame rewrites of a value).
// The look target is the launcher's CLAN tab: WLF / WEEK n: DAY n with the day-progress line, STATUS UPDATES, SUPPLIES, CAMP MORALE, SURVIVORS grid, RAIDS.
//
// Data: daemon op 0xB6 [token][lang][section][seen journal id][seen raid day] -> 0xB7 (tools/clan_ingame.py; parsed by clan13.h into g_cl13 by the clan worker):
// section 1 ROSTER, 2 JOURNAL, 3 RAIDS + supplies, 4 STATUS UPDATES, 5 MATCH STATS of one raid (the raid's day in the "seen raid day" slot; asked only when the list is opened). The page asks the sections the open tab needs first, then the others, again every 30 s, and shows LOADING /
// the server error until they are there.
// Tabs of ONE page (a second instance of a page on the stack breaks it; a widget page cannot clip a scroll): OVERVIEW (status cards, supplies, camp morale), SURVIVORS (4 x 5 grid
// + the focused member), RAIDS (4 rows + the focused raid), JOURNAL (week pages - < WEEK n OF 12 > with one row per clan day, and the focused day's entry: headline,
// the day's facts, its paragraphs; daemon section 6, tools/clan_journal.py. Left / right (A / D, arrows, d-pad, stick) or a click on < > turn the week, up / down / wheel / click pick
// the day and run on into the next week at the ends).
// Keys: W / S move (OVERVIEW and SURVIVORS: up / down by row), Q / E and the bumpers LB / RB (menu-prev / next) change tab, A / D / the arrow keys / stick / d-pad move across a grid row,
// SELECT on a status card = the journal, menu-back / Esc = out. F / the pad's menu-action (Triangle / Y), SELECT (Enter / A) and the PLAYER STATS button in the RAIDS header = the player stats of the focused raid (US / THE ENEMY, "MOST <stat>" awards, faces; t2r-clan-xs-old.txt = the old flat MATCH STATS list) (section 5: every player of that
// match with team and stats; F / BACK / Esc / B closes the list, not the page). Mouse: hover lights, click focuses / opens a tab, wheel scrolls.
//
// MOTION (re/clan-ui-motion.md, translated to what a widget page can do: alpha + position + scale written per frame by a small tween table, all eased out with the launcher's
// curve): the page fades in 220 ms and out 140 ms; a tab's blocks enter staggered (left / right / up 96 units + fade, 560 ms, step 45-110 ms, spread capped at 720 ms); the old tab
// fades out in 90 ms; bars grow from their left edge (header day line, camp morale, raid bars: 900 ms), supply numbers and the stock days count up (700 ms), the raid verdict pops;
// the detail panels (focused member / raid / journal thread) cross-fade (80 ms out, 150 ms in with an 8 unit slide) when the selection changes; focus fills / frames ease
// (gu_fade); every NEW thing is simply part of its row's entrance. t2r-gu-noanim.txt = no tweens (the plain page).
// Portraits: the head of the member in the asked expression (streaming/t2r/icons/c13p-<model>_<expr>.png), soft edges baked in, no tile; t2r-no-clan-portraits.txt = none.
// Entry: the CLAN row of the MP main menu / FINDING MATCH menu with a living clan (select / click, cl13_open_check, called from feature_clan). NEW markers: the highest journal id / raid day seen is kept in
// t2r-clan13-seen.txt next to the exe. t2r-no-clan13.txt next to the exe: off. Log prefix "clan13:".
#define C13_ST 4
#define C13_OVST 2              // the overview shows the newest 2 status cards (one row): the rest is in the journal; room for SUPPLIES / CAMP / RECENT below
#define C13_SV_N 20
#define C13_JD_ROWS 7           // JOURNAL: day rows of the shown week (clan13_widgets.py NJD) and their geometry
#define C13_JD_Y0 540.0f
#define C13_JD_DY 176.0f
#define C13_JD_H 166.0f
#define C13_JP 5                // paragraph slots of the reading pane (NJP); pane x / width, first paragraph top, bottom limit, body point size, line pitch, caption height, gap
#define C13_JT_X 1700.0f
#define C13_JT_W 1840.0f
#define C13_JT_Y0 776.0f
#define C13_JT_Y1 1810.0f
#define C13_JT_PT 36.0f
#define C13_JT_LINE 62.0f       // (the game's menu face: a line is ~1.71 x the point size tall - measured on CHOOSE YOUR SIDE, tools/features/widget_preview.py)
#define C13_JT_CAP_H 44.0f
#define C13_JT_GAP 30.0f
#define C13_RDS_ROWS 4
#define C13_NTAB 5
#define C13_MS_ROWS 8
#define NBAR_C 4
#define C13_REFRESH_MS 30000
#define C13_XF_MS 150           // a tab switch: the old tab fades out while the new one fades in (NR: ~9 frames, a true crossfade; nothing is written or replayed)
#define C13_GATE_MS 100         // the page's first reveal waits for the preloaded art (NR re-sets an image ~100 ms after the first set)
// Gui2 image budget (t2r-ext.c g2_filled): the engine keeps at most 32 different on-screen images. The page itself (backdrops,
// glyphs, a tab's portraits and icons) plus the 32 always-visible preload widgets went past that with a big roster (37 members: the guard evicted a drawn
// image every 1-3 frames from 0.6 s after the page opened) and the overflowing images took turns vanishing (the clan page flicker). Now the strip preloads in waves while the table has room and
// is hidden for the rest of the visit once the table is nearly full; a tab crossfade (two tabs drawn) is cut short when the table is nearly full.
#define C13_G2_RETIRE 24        // filled slots at which the preload strip goes away
#define C13_PRE_WAVE 6          // preload images asked per wave
#define C13_PRE_GAP_F 20        // frames between waves
#define C13_G2_XFCUT 30         // filled slots at which a running tab crossfade ends at once
#define C13_DET_GAP_MS 400      // detail panel: no new cross-fade within this time after the last one ended (held W / S repeats every 110 ms: the panel strobed ~9 times a second)
enum { T_OV, T_SV, T_RD, T_JR, T_MS };
static int Cl13FlickerOld(void)
{                               // t2r-clan13-flicker-old.txt next to the exe: the old behaviour (strip always drawn, crossfade always full, a detail cross-fade on every focus step)
    static int on = -1;
    static uint64_t chk;
    uint64_t n = GetTickCount64();
    if (on < 0 || n - chk > 5000) {
        chk = n;
        FILE *f = ExeSideOpen("t2r-clan13-flicker-old.txt", "r");
        int o = f != NULL;
        if (f)
            fclose(f);
        if (o != on)
            Xlog("clan13: image budget / detail rate cap %s",
                 o ? "OFF (t2r-clan13-flicker-old.txt)" : "on");
        on = o;
    }
    return on;
}

static int g_cl13Off = -1;
static uint64_t Cl13PageId(void)
{
    static uint64_t id;
    if (!id)
        id = PageId("clan13");
    return id;
}

static int g_c13Test;
static uint64_t g_c13TestMs;  // t2r-clan-test.txt (cl13_open_check)
static uint64_t Cl13xPageId(void)
{
    static uint64_t id;
    if (!id)
        id = PageId("clan13x");
    return id;
}                               // PLAYER STATS (its own page: clan13.widget is at gui2's single page limit)

static struct {
    int on, seen, post, h_sel;
    uint64_t f0;
    int h_act, h_f, h_esc;
} g_c13x;                       // the stats page was pushed / reached the top
#define C13_FXMAX 140
typedef struct {
    char path[104];
    float bx, by;
    int kind;
    float delay;
    uint64_t w;
    int done;
} C13fxT;                      // an entrance: kind 1 left, 2 right, 3 up, 4 down, 5 pop (fade + small rise), 6 fade
typedef struct {
    char path[104];
    float frac, delay;
    uint64_t w;
    int done;
    float kx, ky;
    int pin;
} C13grT;                      // a bar growing from its left edge (pin: also moves a marker leaf at kx, ky)
typedef struct {
    char path[104];
    int val, from, cur;
    char fin[96];
    float delay;
    uint64_t t0, wr_f;
    int done;
} C13cnT;                      // a number counting (cur = what the widget shows, -1 none yet), its final text; lives for the whole page visit (not per tab entrance)
static struct {
    int ms_auto, up, tab, prev_tab, foc[C13_NTAB], off[C13_NTAB], dirty,
        closing, stage;
    uint64_t enter_ms, back_f, click_f, sfx_ms, t_u, t_d, t_l, t_r, t0,
        tab_t0, out_t0;
    int h_sel, h_u, h_d, h_l, h_r, h_back, h_esc, h_pv, h_nx, h_ka, h_kd;
    int hv_row, hv_tab, hv_back, hv_sel, hv_prev, hv_next;      // the element under the cursor (cl13_scan)
    uint64_t next_ask[8], ok_at[8];
    int seen_ver[8], ask_ver[8], tries[8];
    int jw, jr_init, hv_jp, hv_jn;      // JOURNAL: the week shown (0 = the newest), the first fill put the focus on the newest entry, the cursor is over < / >
    int xs_open, xs_day, xs_win, xs_got, xs_wait, xs_tries, h_act, h_f,
        hv_act;
    uint64_t xs_next, xs_t0;
    int xs_post;
    char xs_map[32];            // (xs_post: the page is the POST-MATCH SCOREBOARD, cl13_post_open) the MATCH STATS list of a raid (F / the pad's menu-action on RAIDS)
    long seen_jid;
    int seen_day;
    int seen_loaded;
    Cl13T *V;                  // the snapshot the page draws from
    int played[C13_NTAB];       // the tab's entrance has run this visit (entrances and counts play ONCE per Clan UI opening, not on every tab switch)
    int fx_pending, fx_first, shown_prev;       // an entrance is wanted after the next fill; the page's first one also brings in the header
    int hd_done;                // the header's day-progress bar grew once this visit: later fills (tab switches) write it plain
    int det_key, det_ph, det_hold;
    uint64_t det_t0;            // detail panel cross-fade
    int nfx, ngr, ncn;
    C13fxT fx[C13_FXMAX];
    C13grT gr[48];
    C13cnT cn[24];
    int pre_i, pre_ic, intro_played, sw, quiet, hd_shown;       // icons preloaded; the page's one entrance has run; tab switches so far; closing (no more writes); the header showed data
    int pre_ick, pre_off, g2_peak, g2_peak_tab, xf_cut;
    uint64_t pre_wave_f, det_last;
    LONG ev0;                   // Gui2 image budget (cl13_g2_tick): icons preloaded so far, the strip retired,
    // the visit's table peak, crossfades cut short, the last preload wave, the end of the last detail cross-fade, evictions at entry
    int ms_rows[10], ms_n, ms_chosen, ms_tries, ms_init;
    uint64_t ms_chosen_at;      // the MISSIONS tab: the pick (the same 0xEF choice feature_missions always sent)
} C13;
static Cl13T g_cl13View;      // (the snapshot's storage: 45 KB, not on the stack)
static int Cl13Enabled(void)
{
    if (g_cl13Off < 0) {
        FILE *f = ExeSideOpen("t2r-no-clan13.txt", "r");
        g_cl13Off = f != NULL;
        if (f)
            fclose(f);
        Xlog("clan13: in-game CLAN page %s",
             g_cl13Off ? "off (t2r-no-clan13.txt)" : "on");
    }
    return !g_cl13Off;
}

static int Cl13PortraitsOn(void)
{                               // ON by default; t2r-no-clan-portraits.txt = off
    static int on = -1;
    static uint64_t chk;
    uint64_t n = GetTickCount64();
    if (on < 0 || n - chk > 5000) {
        chk = n;
        FILE *f = ExeSideOpen("t2r-no-clan-portraits.txt", "r");
        on = f == NULL;
        if (f)
            fclose(f);
    }
    return on;
}

static void Cl13Sfx(void)
{                               // the menu's focus sound, at most every 150 ms (NR's hfx-ui-cycle floor)
    uint64_t now = GetTickCount64();
    if (now - C13.sfx_ms >= 150) {
        C13.sfx_ms = now;
        UiSfx("sfx-cycle");
    }
}

// ------------------------------------------------------------------ small writers
static float Cl13Est(const char *t, float pt)
{
    return GuEst(t, pt);
}

#define CL13_TEXT(fmt, i, t) do { char p_[128]; snprintf(p_, sizeof p_, fmt, (i)); UiText(p_, (t)); } while (0)
// gu_est (GEAR UP's per-character widths) runs about 8 % under the game's real menu face with letter spacing (the overlap audit measures the real face): every fit and clip here leaves that margin
// Polish letters were capitalized in the clan UI: the journal headline arrives upper case (daemon str.upper(), so Ą Ć Ę Ł Ń Ó Ś Ź Ż too) and was shown
// as a sentence by lowering only A-Z: the Polish capitals (UTF-8 C3 93, C4 84.., C5 81..) stayed capital inside the lowercase words. The pairs differ by +1 in the
// second byte (same length), so the case is flipped in place. up = 1: lower -> upper (mission names / units in capitals), 0: upper -> lower.
static void Cl13CasePl(char *s, int up)
{
    for (uint8_t * p = (uint8_t *) s; *p; p++) {
        if (p[0] == 0xC3 && (p[1] == (up ? 0xB3 : 0x93))) {
            p[1] = up ? 0x93 : 0xB3;
            p++;
            continue;
        }
        if (p[0] == 0xC4 && p[1]) {
            static const uint8_t L[] = { 0x85, 0x87, 0x99 };    // ą ć ę (capitals = lower - 1)
            for (unsigned i = 0; i < sizeof L; i++)
                if (p[1] == (up ? L[i] : L[i] - 1)) {
                    p[1] = up ? L[i] - 1 : L[i];
                    break;
                }
            p++;
            continue;
        }
        if (p[0] == 0xC5 && p[1]) {
            static const uint8_t L[] = { 0x82, 0x84, 0x9B, 0xBA, 0xBC };        // ł ń ś ź ż
            for (unsigned i = 0; i < sizeof L; i++)
                if (p[1] == (up ? L[i] : L[i] - 1)) {
                    p[1] = up ? L[i] - 1 : L[i];
                    break;
                }
            p++;
            continue;
        }
    }
}

// a headline as a sentence: the first letter stays, the rest lower case (ASCII and Polish)
static void Cl13LowerTail(char *t)
{
    uint8_t *q = (uint8_t *) t;
    if (*q) {
        q++;
        while ((*q & 0xC0) == 0x80)
            q++;
    }                           // (the first character may be a 2-byte Polish capital)
    for (char *r = (char *)q; *r; r++)
        if (*r >= 'A' && *r <= 'Z')
            *r = (char) (*r + 32);
    Cl13CasePl((char *) q, 0);
}

static void Cl13Fit(const char *path, const char *t, float box, float pt)
{
    GuFit(path, t, box * 0.92f, pt);
}

static void Cl13Clipped(const char *path, const char *src, float boxw,
                         float pt, int lines)
{
    char b[700];
    Cl13Clip(b, sizeof b, src ? src : "", boxw * 0.88f, pt, lines,
              Cl13Est);
    UiText(path, b);
}

static void Cl13ClippedLong(const char *path, const char *src,
                              float boxw, float pt, int lines)
{                               // the journal's detail pane: a whole day entry (~1500 chars) wrapped in up to `lines` lines
    static char b[2200];
    Cl13Clip(b, sizeof b, src ? src : "", boxw * 0.88f, pt, lines,
              Cl13Est);
    UiText(path, b);
}

static int g_cl13PicLogged;   // one "portrait <path>" line per page open
// Portrait gate (a blue square for ~4 frames; NR re-sets the image ~100 ms after the first set): a new slot stays hidden until 100 ms after its image was set, then the SAME image is
// set again (bypassing the write dedup) and the slot is shown. A slot whose face changes later (focus, refresh) swaps the image only - no visibility toggle - and gets the 100 ms re-set too.
typedef struct {
    char path[120];
    char img[100];
    uint64_t set_ms;
    int st;
} C13picT;                     // st 0 hidden, waiting for its re-set; 1 shown; 2 shown, re-set due
static C13picT g_c13Pic[48];
static int g_c13Npic;
static void Cl13PicReset(void)
{
    g_c13Npic = 0;
}

static void Cl13PicTick(uint64_t now)
{
    for (int i = 0; i < g_c13Npic; i++) {
        C13picT *q = &g_c13Pic[i];
        if (q->st == 1 || !q->img[0] || now - q->set_ms < 100)
            continue;
        g_uiTextRaw = 1;
        UiImage(q->path, q->img);
        g_uiTextRaw = 0;
        if (q->st == 0)
            UiVisible(q->path, 1);
        q->st = 1;
    }
}

static void Cl13Pic(const char *base, const char *pid)
{                               // the portrait slot "<base>Im": the member's head in the expression asked (the daemon fitted it to what the head has), else nothing
    char p[200], img[160], a[16], s[16], m[16], e[20];
    snprintf(p, sizeof p, "%sIm", base);
    C13picT *q = NULL;
    for (int i = 0; i < g_c13Npic; i++)
        if (!strcmp(g_c13Pic[i].path, p)) {
            q = &g_c13Pic[i];
            break;
        }
    if (!Cl13PortraitsOn() || !pid || !pid[0]
        || sscanf(pid, "%15[^/]/%15[^/]/%15[^/]/%19s", a, s, m, e) != 4) {
        UiVisible(p, 0);
        if (q) {
            q->img[0] = 0;
            q->st = 0;
        }                       // (a face that comes later is gated again)
        return;
    }
    snprintf(img, sizeof img, "streaming/t2r/icons/c13p-%s_%s.png", m, e);      // = make_icons.clan13_portraits
    if (!g_cl13PicLogged) {
        g_cl13PicLogged = 1;
        Xlog("clan13: portrait %s (from id %s)", img, pid);
    }
    if (!q) {
        if (g_c13Npic >= 48 || strlen(p) >= sizeof q->path
            || strlen(img) >= sizeof q->img) {
            UiImage(p, img);
            UiVisible(p, 1);
            return;
        }
        q = &g_c13Pic[g_c13Npic++];
        snprintf(q->path, sizeof q->path, "%s", p);
        q->img[0] = 0;
        q->st = 0;
        q->set_ms = 0;
        UiVisible(p, 0);
    }
    if (strcmp(q->img, img)) {  // a new face for the slot
        snprintf(q->img, sizeof q->img, "%s", img);
        UiImage(p, img);
        q->set_ms = GetTickCount64();
        if (q->st == 1)
            q->st = 2;          // (shown already: swap only, the re-set follows)
    }
}

static void Cl13Band(const char *grp, const char *gpre, const char *pre,
                      int i, const char *tone)
{                               // the card's tone band: one of five visible
    int t =
        !strcmp(tone, "good") ? 1 : !strcmp(tone,
                                            "warn") ? 2 : !strcmp(tone,
                                                                  "bad") ?
        3 : !strcmp(tone, "info") ? 4 : 0;
    char p[160];
    char im[80];
    snprintf(p, sizeof p, "anim-group.%s.%s%d.%sBd%d", grp, gpre, i, pre,
             i);
    snprintf(im, sizeof im, "streaming/t2r/icons/c13-band%d.png", t);   // (one image per card, its texture = the tone: the page is at gui2's 1024-widget limit)
    UiImage(p, im);
}

static int Cl13Dow(int d)
{
    return d > 0 ? (d - 1) % 7 + 1 : 0;
}                               // a clan day (1..) as the day of its week: the header says WEEK n: DAY d

// the launcher's status look (clan-status.js): text colour, the 10 tile colours are in the widgets; names in clan_status.NAMES order
static const char *const C13_STN[10] =
    { "healthy", "hungry", "sad", "sick", "dead", "injured", "recovered",
"new", "depressed", "shock" };
static const char *const C13_STTXT[10] =
    { "7FD07A", "F0B429", "7FA8F5", "F2625A", "A9AFB8", "F29A4A", "7FD07A",
"55C6E8", "9DB0C4", "DCE7F0" };
static const char *const C13_STICON[10] =
    { "healthbar", "food", "person", "pills", "person", "bandage",
"medicine", "person", "person", "person" };
static int Cl13Stidx(const char *n)
{
    for (int i = 0; i < 10; i++)
        if (!strcmp(n, C13_STN[i]))
            return i;
    return 1;
}

static int Cl13Pcode(char st)
{
    return st == 'u' ? 1 : st == 's' ? 3 : st == 'd' ? 8 : st ==
        'k' ? 9 : 0;
}                               // roster state letter -> status index

static int Cl13Pbar(char st)
{
    return st == 'h' ? 0 : st == 'u' ? 1 : st == 's' ? 2 : st ==
        'd' ? 3 : 4;
}

static void Cl13DaysText(char *b, size_t n, int d)
{
    if (d == 1)
        snprintf(b, n, "1 DAY");
    else
        snprintf(b, n, "%d DAYS", d);
}

static float g_c13Tabx[C13_NTAB], g_c13_tabw[C13_NTAB];
static void Cl13TabLayout(void)
{                               // x / width of each tab label from its measured (translated) text, equal gaps
    static const char *const NM[C13_NTAB] =
        { "OVERVIEW", "SURVIVORS", "RAIDS", "JOURNAL", "MISSIONS" };
    static const char *const TP[C13_NTAB] =
        { "anim-group.tabs.tab0", "anim-group.tabs.tab1",
"anim-group.tabs.tab2", "anim-group.tabs.tab3", "anim-group.tabs.tab4" };
    static const char *const TU[C13_NTAB] =
        { "anim-group.tabs.tabU0", "anim-group.tabs.tabU1",
"anim-group.tabs.tabU2", "anim-group.tabs.tabU3", "anim-group.tabs.tabU4" };
    float x = 300.0f;
    for (int k = 0; k < C13_NTAB; k++) {
        float w = GuEst(NM[k], 46.0f) * 1.06f + 8.0f;  // (the estimate is for the game's face; a little over)
        g_c13Tabx[k] = x;
        g_c13_tabw[k] = w;
        uint64_t t = UiW(TP[k]), u = UiW(TU[k]);
        if (t)
            WPivot(t, x, 292.0f);
        if (u) {
            WPivot(u, x, 352.0f);
            WScale(u, w / 100.0f, 1.0f);
        }
        x += w + 110.0f;
    }
}

static const char *Cl13Tr(const char *en, char *b, size_t n)
{
    const char *t = LocTr(en);
    if (t && t != b)
        snprintf(b, n, "%s", t);
    return b;
}                               // (b may be en itself)

static void Cl13Icon(const char *path, const char *id)
{
    char img[96];
    snprintf(img, sizeof img, "streaming/t2r/icons/c13-%s.png", id
             && id[0] ? id : "keepsake");
    UiImage(path, img);
}

// ------------------------------------------------------------------ the tween table (entrances, growing bars, counting numbers)
static float Cl13Eo(float t)
{
    if (t <= 0.0f)
        return 0.0f;
    if (t >= 1.0f)
        return 1.0f;
    float u = 1.0f - t;
    return 1.0f - u * u * u * u;
}                               // ease out (about the launcher's cubic-bezier(.16, 1, .3, 1))

static int Cl13AnimOff(void)
{
    return GuNoanim();
}

static void Cl13FxAdd(const char *path, float bx, float by, int kind,
                        float delay)
{
    if (C13.nfx >= C13_FXMAX)
        return;
    C13fxT *f = &C13.fx[C13.nfx++];
    snprintf(f->path, sizeof f->path, "%s", path);
    f->bx = bx;
    f->by = by;
    f->kind = kind;
    f->delay = delay;
    f->w = 0;
    f->done = 0;
}

// where a number / bar / entrance stands NOW: while an entrance runs (or is about to) the value goes through the table, afterwards it is written plain
static int Cl13Busy(uint64_t now)
{
    return !Cl13AnimOff() && (C13.fx_pending
                                || (!C13.sw && now - C13.t0 < 2600));
}                               // (the one entrance of the visit; after the first tab switch numbers and bars are written plain)

static void Cl13Grow(const char *path, float frac, float delay, int pin,
                      float kx, float ky)
{
    uint64_t now = GetTickCount64();
    if (frac < 0.0f)
        frac = 0.0f;
    if (frac > 1.0f)
        frac = 1.0f;
    if (!Cl13Busy(now)) {
        uint64_t w = UiW(path);
        if (w)
            WScale(w, frac < 0.004f ? 0.004f : frac, 1.0f);
        return;
    }
    for (int i = 0; i < C13.ngr; i++)
        if (!strcmp(C13.gr[i].path, path)) {
            C13.gr[i].frac = frac;
            return;
        }                       // (a refill: the target moves, the timing stays)
    if (C13.ngr >= 48)
        return;
    C13grT *g = &C13.gr[C13.ngr++];
    snprintf(g->path, sizeof g->path, "%s", path);
    g->frac = frac;
    g->delay = delay;
    g->w = 0;
    g->done = 0;
    g->pin = pin;
    g->kx = kx;
    g->ky = ky;
}

// A number is never hidden or blanked while it changes: the widget keeps showing its last value and is only re-written when the integer moves, at most every 3rd frame
// (a gui2 text written every frame misses draws - the same engine trait as the post-match parts counter). The first appearance counts up from 0 inside the
// entrance; a later change of the value (a refresh) counts from what is shown to the new value. The table lives for the whole visit, so a tab switch never restarts it.
static void Cl13Count(const char *path, int val, const char *fin,
                       float delay)
{
    uint64_t now = GetTickCount64();
    C13cnT *c = NULL;
    for (int i = 0; i < C13.ncn; i++)
        if (!strcmp(C13.cn[i].path, path)) {
            c = &C13.cn[i];
            break;
        }
    if (!c) {
        if (C13.ncn >= 24) {
            UiText(path, fin);
            return;
        }
        c = &C13.cn[C13.ncn++];
        memset(c, 0, sizeof *c);
        snprintf(c->path, sizeof c->path, "%s", path);
        c->cur = -1;
        c->val = val;
        c->done = 1;
        if (Cl13Busy(now) && val >= 2) {
            c->from = 0;
            c->delay = delay;
            c->t0 = now;
            c->done = 0;
        }
    } else if (c->val != val && !Cl13AnimOff()) {
        c->from = c->cur >= 0 ? c->cur : 0;
        c->delay = 0.0f;
        c->t0 = now;
        c->done = val < 2;
    }
    c->val = val;
    snprintf(c->fin, sizeof c->fin, "%s", fin);
    if (c->done) {
        UiText(path, fin);
        c->cur = val;
    }
}

static void Cl13FxReset(void)
{
    C13.nfx = C13.ngr = 0;
}

static void Cl13FxTick(uint64_t now)
{
    const float DUR = 560.0f, DIST = 96.0f;
    for (int i = 0; i < C13.nfx; i++) {
        C13fxT *f = &C13.fx[i];
        if (f->done)
            continue;
        if (!f->w)
            f->w = UiW(f->path);
        if (!f->w)
            continue;
        float p = ((float) (now - C13.t0) - f->delay) / DUR;
        float e = Cl13Eo(p), a = e;
        float dx = 0.0f, dy = 0.0f, inv = 1.0f - e;
        if (f->kind == 1)
            dx = -inv * DIST;
        else if (f->kind == 2)
            dx = inv * DIST;
        else if (f->kind == 3)
            dy = inv * DIST * 0.6f;
        else if (f->kind == 4)
            dy = -inv * DIST * 0.5f;
        else if (f->kind == 5)
            dy = inv * 28.0f;
        int q = (int) (a * 24.0f + 0.5f);
        WAlpha(f->w, (float) q / 24.0f);
        if (f->kind >= 1 && f->kind <= 5)
            WPivot(f->w, f->bx + dx, f->by + dy);
        if (p >= 1.0f) {
            f->done = 1;
            WAlpha(f->w, 1.0f);
            if (f->kind >= 1 && f->kind <= 5)
                WPivot(f->w, f->bx, f->by);
        }
    }
    for (int i = 0; i < C13.ngr; i++) {
        C13grT *g = &C13.gr[i];
        if (g->done)
            continue;
        if (!g->w)
            g->w = UiW(g->path);
        if (!g->w)
            continue;
        float p = ((float) (now - C13.t0) - g->delay) / 900.0f, fr =
            g->frac * Cl13Eo(p);
        WScale(g->w, fr < 0.004f ? 0.004f : fr, 1.0f);
        if (g->pin) {
            uint64_t pw = UiW("anim-group.ov.ovCamp.mo.moPin");
            if (pw)
                WPivot(pw, g->kx + g->pin * fr * 1.0f - 5.0f, g->ky);
        }
        if (p >= 1.0f)
            g->done = 1;
    }
    for (int i = 0; i < C13.ncn; i++) {
        C13cnT *c = &C13.cn[i];
        if (c->done)
            continue;
        float p = ((float) (now - c->t0) - c->delay) / 700.0f;
        if (p >= 1.0f) {
            UiText(c->path, c->fin);
            c->cur = c->val;
            c->done = 1;
            continue;
        }
        if (p < 0.0f)
            p = 0.0f;
        int v =
            c->from + (int) ((float) (c->val - c->from) * Cl13Eo(p) +
                             0.5f);
        if (v == c->cur)
            continue;           // (NR writes the number every frame while it counts; here: every frame the integer moved, never blanked)
        char b[120];
        const char *d0 = c->fin;
        while (*d0 && (*d0 < '0' || *d0 > '9'))
            d0++;
        const char *d1 = d0;
        while (*d1 >= '0' && *d1 <= '9')
            d1++;
        snprintf(b, sizeof b, "%.*s%d%s", (int) (d0 - c->fin), c->fin, v, d1);  // (the final text's own markup around the number: no colour change or width jump when it lands)
        UiText(c->path, b);
        c->cur = v;
        c->wr_f = g_frame;
    }
}

// ------------------------------------------------------------------ requests
static void Cl13SeenLoad(void)
{
    C13.seen_jid = 0;
    C13.seen_day = 0;
    C13.seen_loaded = 1;
    FILE *f = ExeSideOpen("t2r-clan13-seen.txt", "r");
    if (!f)
        return;
    char b[64] = { 0 };
    if (fgets(b, sizeof b, f)) {
        Cl13SeenT s;
        Cl13SeenParse(b, &s);
        C13.seen_jid = s.jid;
        C13.seen_day = s.day;
    }
    fclose(f);
}

static void Cl13SeenSave(void)
{                               // everything on the screen counts as seen once the page is left
    if (!C13.V)
        return;
    long jid = C13.seen_jid;
    int day = C13.seen_day;
    for (int i = 0; i < C13.V->nj; i++)
        if (C13.V->j[i].id > jid)
            jid = C13.V->j[i].id;
    for (int i = 0; i < C13.V->nd; i++)
        if (C13.V->d[i].id > jid)
            jid = C13.V->d[i].id;
    for (int i = 0; i < C13.V->nm; i++)
        if (C13.V->m[i].day > day)
            day = C13.V->m[i].day;
    if (jid == C13.seen_jid && day == C13.seen_day)
        return;
    FILE *f = ExeSideOpen("t2r-clan13-seen.txt", "w");
    if (f) {
        fprintf(f, "%ld %d\n", jid, day);
        fclose(f);
    }
}

static int Cl13Ask(int sec)
{                               // 1 = a request went out
    uint8_t ex[9];
    ex[0] = (uint8_t) FeedLang();
    ex[1] = (uint8_t) sec;
    long id = C13.seen_jid;
    int day = sec == 5 ? C13.xs_day : sec == 6 ? C13.jw : C13.seen_day; // (section 6, the journal's week pages: the week whose text is wanted, 0 = the newest)       // section 5 (a raid's match stats) reuses the "seen raid day" slot for the raid's day
    ex[2] = (uint8_t) id;
    ex[3] = (uint8_t) (id >> 8);
    ex[4] = (uint8_t) (id >> 16);
    ex[5] = (uint8_t) (id >> 24);
    ex[6] = (uint8_t) day;
    ex[7] = (uint8_t) (day >> 8);
    ex[8] = 1;                  // capability: our reply buffer is 64 KB (older builds read 24 KB and drop anything bigger)
    return ClanSend(0xB6, ex, 9);
}

static void Cl13NetTick(uint64_t now)
{
    static const int ORD[C13_NTAB][4] = { { 4, 3, 1, 2 }, { 1, 4, 3, 2 }, { 3, 1, 4, 2 }, { 6, 2, 1, 3 }, { 4, 1, 3, 2 } };     // (the MISSIONS tab was missing: its zeroed row asked for section 0 over and over, every reply 'did not parse')      // the tab's sections first
    for (int s = 1; s <= 6; s++) {      // replies that arrived: the page's snapshot (a lock + 45 KB copy, only on a change)
        if ((int) g_cl13Ver[s] != C13.seen_ver[s]) {
            C13.seen_ver[s] = (int) g_cl13Ver[s];
            EnterCriticalSection(&g_clanCs);
            int chg = memcmp(&g_cl13View, &g_cl13, sizeof g_cl13View) != 0;   // (a refresh with the same content changes nothing on screen: no refill)
            if (chg)
                memcpy(&g_cl13View, &g_cl13, sizeof g_cl13View);
            LeaveCriticalSection(&g_clanCs);
            C13.ok_at[s] = now;
            C13.tries[s] = 0;
            if (chg || s == 5)
                C13.dirty = 1;
            if (s == 5 && C13.xs_open && C13.xs_wait) { // the answer to our match stats request (xday 0 = an empty reply: an older daemon)
                C13.xs_wait = 0;
                C13.xs_got = g_cl13View.xday == C13.xs_day
                    || g_cl13View.xday == 0;
                if (!C13.xs_got)
                    C13.xs_next = now;  // the reply of another raid (opened and closed quickly): ask again
                else if (C13.xs_post && g_cl13View.xn == 0
                         && now - C13.xs_t0 < 25000) {
                    C13.xs_got = 0;
                    C13.xs_tries = 0;
                    C13.xs_next = now + 2500;
                    Xlog("clan13: post-match scoreboard: the server has no stats of day %d yet - asking again", C13.xs_day);
                }               // (the server books the match when the host's end arrives)
                Xlog("clan13: match stats of day %d: %d player(s) (reply for day %d)", C13.xs_day, g_cl13View.xn, g_cl13View.xday);
            }
        }
    }
    if (C13.xs_open && !C13.xs_got && !C13.xs_wait && !g_clan_busy
        && g_locProbed && now >= C13.xs_next) {
        if (C13.xs_tries >= 4) {
            C13.xs_got = 1;
            g_cl13View.xday = 0;
            g_cl13View.xn = 0;
            C13.dirty = 1;
            Xlog("clan13: match stats of day %d: no answer after %d tries",
                 C13.xs_day, C13.xs_tries);
        } else if (Cl13Ask(5)) {
            C13.xs_wait = 1;
            C13.xs_tries++;
            C13.xs_next = now + 3000;
            Xlog("clan13: asked match stats of day %d (try %d)",
                 C13.xs_day, C13.xs_tries);
        }
    }
    if (C13.xs_wait && now >= C13.xs_next)
        C13.xs_wait = 0;        // (no reply within 3 s: asked again)
    if (g_clan_busy || !g_locProbed)
        return;
    for (int k = 0; k < 4; k++) {
        int s = ORD[C13.tab][k];
        int need = !g_cl13View.have[s]
            || now - C13.ok_at[s] >= C13_REFRESH_MS;
        if (!need || now < C13.next_ask[s])
            continue;
        if (Cl13Ask(s)) {
            C13.next_ask[s] = now + (g_cl13View.have[s] ? 5000 : 2500);        // (re-asked until it came)
            C13.ask_ver[s] = (int) g_cl13Ver[s];
            C13.tries[s]++;
            static int nl;
            if (LOGCAP(nl, 30))
                Xlog("clan13: asked section %d (try %d)", s, C13.tries[s]);
        }
        break;                  // one request at a time
    }
}

// ------------------------------------------------------------------ filling
static void Cl13Window(int tab, int n, int rows, int per)
{                               // keep the focus inside the window, the window inside the list (per = items per window step: a grid row)
    if (n <= 0) {
        C13.foc[tab] = 0;
        C13.off[tab] = 0;
        return;
    }
    if (C13.foc[tab] >= n)
        C13.foc[tab] = n - 1;
    if (C13.foc[tab] < 0)
        C13.foc[tab] = 0;
    int fr = C13.foc[tab] / per, orow = C13.off[tab] / per, nrows =
        (n + per - 1) / per;
    if (fr < orow)
        orow = fr;
    if (fr >= orow + rows)
        orow = fr - rows + 1;
    if (orow > nrows - rows)
        orow = nrows - rows;
    if (orow < 0)
        orow = 0;
    C13.off[tab] = orow * per;
}

static void Cl13Header(const Cl13T *V)
{
    char a[96], b[64], t[160];
    UiText("anim-group.hd.hdFac", V->fk == 'S' ? "SERAPHITES" : "WLF");
    snprintf(a, sizeof a, "WEEK %d: DAY %d", V->week, V->dow);
    if (C13.xs_post) {
        int d = C13.xs_day - 1;
        d = d < 0 ? 0 : d > 83 ? 83 : d;
        snprintf(a, sizeof a, "WEEK %d: DAY %d", d / 7 + 1, d % 7 + 1);
    }                           // post-match: the day just played, the same numbers as the supply counter (pm_fill; the clan's own day has moved on by now)
    {                           // opened over FIND MATCH: the search runs under the page - say so (fix1025-clanopen)
        static uint64_t PFM2;
        if (!PFM2)
            PFM2 = PageId("find-match-menu");
        if (PFM2 && PageOnStack(PFM2)) {
            size_t n = strlen(a);
            snprintf(a + n, SNLEFT(a, n),
                     "   -   SEARCHING FOR A MATCH%.*s",
                     (int) ((GetTickCount64() / 500) % 4), "...");
        }
    }
    UiText("anim-group.hd.hdWeek", a);
    int done = V->done, wk = V->week;
    snprintf(t, sizeof t, "%d OF 84 DAYS SURVIVED - WEEK %d OF 12", done,
             wk);
    {
        ClanT rc = ClanGet();
        if (rc.ok && rc.wk > 0) {
            size_t n = strlen(t);
            snprintf(t + n, SNLEFT(t, n),
                     "   -   YOUR RANK: %d WEEKS SURVIVED IN TOTAL",
                     rc.wk);
        }
    }                           // 2013 rank number (the page is at gui2's widget limit: text only here; the symbol is in PLAYER STATS)
    UiText("anim-group.hd.hdTxt", t);
    (void) b;
    if (!C13.hd_done) {
        Cl13Grow("anim-group.hd.hdBar", (float) done / 84.0f, 220.0f, 0,
                  0, 0);
        return;
    }
    uint64_t w = UiW("anim-group.hd.hdBar");   // (the bar re-grew from zero on every tab switch; it animates on the page's first entrance only)
    float fr = (float) done / 84.0f;
    if (w)
        WScale(w, fr < 0.004f ? 0.004f : fr > 1.0f ? 1.0f : fr, 1.0f);
}

static void Cl13StatusRow(const Cl13T *V, int i)
{
    char g[96], p[170];
    snprintf(g, sizeof g, "anim-group.ov.ovSt.stRow%d", i);
    if (i >= V->nu || i >= C13_OVST) {
        UiVisible(g, 0);
        return;
    }
    UiVisible(g, 1);
    const Cl13UT *u = &V->u[i];
    int si = Cl13Stidx(u->state);
    for (int q = 0; q < 10; q++) {
        snprintf(p, sizeof p, "%s.stTn%d_%d", g, q, i);
        UiVisible(p, q == si);
    }
    snprintf(p, sizeof p, "%s.stPt%d", g, i);
    Cl13Pic(p, u->pid);
    snprintf(p, sizeof p, "%s.stRole%d", g, i);
    UiText(p, u->role);
    snprintf(p, sizeof p, "%s.stName%d", g, i);
    Cl13Fit(p, u->name, 760.0f, 60.0f);
    snprintf(p, sizeof p, "%s.stIc%d", g, i);
    UiVisible(p, si != 0);
    if (si)
        Cl13Icon(p, C13_STICON[si]);
    char w[140];
    snprintf(w, sizeof w, "<font color=\"#%s\">%s</font>", C13_STTXT[si],
             u->stw);
    snprintf(p, sizeof p, "%s.stWord%d", g, i);
    UiText(p, w);
    snprintf(p, sizeof p, "%s.stLine%d", g, i);
    Cl13Fit(p, u->line, 1220.0f, 34.0f);
    snprintf(p, sizeof p, "%s.stRead%d", g, i);
    UiText(p, "READ IN THE JOURNAL ->");
    snprintf(p, sizeof p, "%s.stNew%dBg", g, i);
    UiVisible(p, i < 2);
    snprintf(p, sizeof p, "%s.stNew%dTx", g, i);
    UiVisible(p, i < 2);
    snprintf(p, sizeof p, "%s.stFill%d", g, i);
    GuFade(p, C13.tab == T_OV && (i == C13.foc[T_OV] || C13.hv_row == i));
    snprintf(p, sizeof p, "%s.stFrame%d", g, i);
    GuFade(p, C13.tab == T_OV && i == C13.foc[T_OV]);
}

#define C13_NREC 4
static void Cl13OvStack(int nu, int ns, int nrec, int *so, int *co,
                          int *rt, int *rshow)
{                               // = clan13_widgets.ov_stack
    int rows = (nu + 1) / 2;
    int st_h = rows == 0 ? 100 : 116 + (rows - 1) * 300 + 270;
    int sup_top = 424 + st_h + 60, sup_h = ns ? 340 : 150, camp_top =
        sup_top + sup_h + 60, rec_top = camp_top + 230 + 40;
    int room = (1830 - rec_top - 100) / 68;
    *so = sup_top - 1170;
    *co = camp_top - 1560;
    *rt = rec_top;
    *rshow = room < 0 ? 0 : room < nrec ? room : nrec;
}

static void Cl13FillOv(const Cl13T *V)
{
    char b[160], p[170];
    // the blocks are STACKED (each top = the previous bottom + a gap): offsets of the supplies / camp / recent groups from their design tops (= clan13_widgets.ov_stack)
    int nrec = 0, recs[C13_NREC];
    for (int i = 0; i < V->nj && nrec < C13_NREC; i++)
        if (V->j[i].title[0])
            recs[nrec++] = i;
    int so, co, rt, rshow;
    Cl13OvStack(V->nu < C13_OVST ? V->nu : C13_OVST, V->ns, nrec, &so,
                  &co, &rt, &rshow);
    {
        uint64_t a1 = UiW("anim-group.ov.ovSup"), a2 =
            UiW("anim-group.ov.ovCamp"), a3 = UiW("anim-group.ov.ovRec");
        if (a1)
            WPivot(a1, 0.0f, (float) so);
        if (a2)
            WPivot(a2, 0.0f, (float) co);
        if (a3)
            WPivot(a3, 0.0f, (float) rt);
    }
    UiText("anim-group.ov.ovRec.ovRecTitle",
            rshow ? "RECENTLY IN CAMP" : "");
    for (int i = 0; i < C13_NREC; i++) {
        snprintf(p, sizeof p, "anim-group.ov.ovRec.ovRecLn%d", i);
        UiVisible(p, i < rshow);
        snprintf(p, sizeof p, "anim-group.ov.ovRec.ovRecDay%d", i);
        if (i < rshow) {
            snprintf(b, sizeof b, "DAY %d", V->j[recs[i]].day);
            UiText(p, b);
        } else
            UiText(p, "");
        snprintf(p, sizeof p, "anim-group.ov.ovRec.ovRecTx%d", i);
        if (i < rshow) {
            char t[100];
            snprintf(t, sizeof t, "%s", V->j[recs[i]].title);   // the headline is upper case: shown as a sentence
            Cl13LowerTail(t);
            Cl13Fit(p, t, 2900.0f, 38.0f);
        } else
            UiText(p, "");
    }
    UiText("anim-group.ov.ovSt.ovStTitle", "STATUS UPDATES");
    if (V->nu > C13_OVST) {
        snprintf(b, sizeof b,
                 "SINCE YOU LAST LOOKED  -  %d MORE IN THE JOURNAL",
                 V->nu - C13_OVST);
        UiText("anim-group.ov.ovSt.ovStSub", b);
    } else
        UiText("anim-group.ov.ovSt.ovStSub",
                V->
                nu ? "SINCE YOU LAST LOOKED" :
                "NOTHING NEW SINCE YOU LAST LOOKED");
    Cl13Window(T_OV, V->nu < C13_OVST ? V->nu : C13_OVST, 1, 2);
    for (int i = 0; i < C13_ST; i++)
        Cl13StatusRow(V, i);
    UiText("anim-group.ov.ovSup.ovSupTitle", "SUPPLIES");
    UiText("anim-group.ov.ovSup.ovSupNone",
            V->
            ns ? "" :
            "NO SUPPLIES BOOKED YET - THEY ARE COUNTED AFTER EACH RAID");
    int any_stock = 0;
    for (int k = 0; k < V->ns; k++)
        if (V->s[k].stock > 0)
            any_stock = 1;
    if (V->ns && any_stock && V->worst >= 0) {
        UiText("anim-group.ov.ovSup.ovLastLb", "THE STOCK LASTS");
        Cl13DaysText(b, sizeof b, V->worst);
        Cl13Count("anim-group.ov.ovSup.ovLast", V->worst, b, 520.0f);
    } else {
        UiText("anim-group.ov.ovSup.ovLastLb", "");
        UiText("anim-group.ov.ovSup.ovLast", "");
    }
    static const char *const CAT[5] =
        { "food", "water", "medicine", "ammo", "materials" };
    for (int k = 0; k < 5; k++) {
        int has = k < V->ns;
        snprintf(p, sizeof p, "anim-group.ov.ovSup.tlRow%d", k);
        UiVisible(p, has);
        if (!has)
            continue;
        const Cl13ST *s = &V->s[k];
        snprintf(p, sizeof p, "anim-group.ov.ovSup.tlRow%d.tlIc%d", k, k);
        Cl13Icon(p, CAT[k]);
        snprintf(p, sizeof p, "anim-group.ov.ovSup.tlRow%d.tlLb%d", k, k);
        UiText(p, s->label);
        snprintf(p, sizeof p, "anim-group.ov.ovSup.tlRow%d.tlNum%d", k, k);
        if (s->stock > 0) {
            int low = s->need > 0 && s->days <= 1;
            snprintf(b, sizeof b, "%s%d%s",
                     low ? "<font color=\"#FD6D51\">" : "", s->stock,
                     low ? "</font>" : "");
            Cl13Count(p, s->stock, b, 600.0f + 70.0f * k);
        } else
            UiText(p, "NONE");
        char d[40] = "";
        if (s->days >= 0 && s->need > 0)
            Cl13DaysText(d, sizeof d, s->days);
        else if (s->stock > 0)
            snprintf(d, sizeof d, "IN CAMP");
        snprintf(p, sizeof p, "anim-group.ov.ovSup.tlRow%d.tlDays%d", k,
                 k);
        UiText(p, d);
    }
    // CAMP: the morale line (the launcher's scale: band-coloured fill to the value, ticks, the state word) + clans won / lost
    UiText("anim-group.ov.ovCamp.ovCampTitle", "CAMP");
    snprintf(b, sizeof b,
             "CAMPAIGNS COMPLETED %d - CLANS LOST %d - LARGEST CLAN %d",
             V->won, V->lost, V->maxsz > V->alive ? V->maxsz : V->alive);
    UiText("anim-group.ov.ovCamp.ovClans", b);
    int v = V->morale < 0 ? 0 : V->morale > 100 ? 100 : V->morale;
    int band = v >= 70 ? 3 : v >= 50 ? 2 : v >= 30 ? 1 : 0;     // grim, strained, steady, hopeful
    static const char *const COL[4] =
        { "F2625A", "F0B429", "7FA8F5", "7FD07A" };
    UiText("anim-group.ov.ovCamp.mo.moLb", "CAMP MORALE");
    for (int k = 0; k < 4; k++) {
        snprintf(p, sizeof p, "anim-group.ov.ovCamp.mo.moFill%d", k);
        UiVisible(p, k == band);
    }
    snprintf(p, sizeof p, "anim-group.ov.ovCamp.mo.moFill%d", band);
    Cl13Grow(p, (float) (v < 3 ? 3 : v) / 100.0f, 950.0f, 1500, 760.0f,
              1678.0f);
    if (!Cl13Busy(GetTickCount64())) {
        uint64_t pin = UiW("anim-group.ov.ovCamp.mo.moPin");
        if (pin)
            WPivot(pin, 760.0f + 1500.0f * (float) v / 100.0f - 5.0f,
                    1678.0f);
    }
    const char *word =
        band == 3 ? "HOPEFUL" : band == 2 ? "STEADY" : band ==
        1 ? "STRAINED" : "GRIM";
    const char *tw = LocTr(word);
    snprintf(b, sizeof b, "<font color=\"#%s\">%s</font>", COL[band],
             tw ? tw : word);
    UiText("anim-group.ov.ovCamp.mo.moWord", b);
}

static void Cl13FillSvDetail(const Cl13T *V)
{
    if (V->np <= 0)
        return;
    const Cl13PT *m = &V->p[C13.foc[T_SV]];
    char b[300], w[120];
    Cl13Pic("anim-group.sv.svDt.svDt", m->pid);
    snprintf(w, sizeof w, "%s", m->name);
    Cl13Fit("anim-group.sv.svDt.svDtName", w, 1100.0f, 72.0f);
    snprintf(b, sizeof b, "%s  -  <font color=\"#%s\">%s</font>", m->role,
             C13_STTXT[Cl13Pcode(m->st)], m->stw);
    Cl13Fit("anim-group.sv.svDt.svDtSub", b, 1100.0f, 38.0f);
    Cl13Clipped("anim-group.sv.svDt.svDtTag", m->tag, 1100.0f, 36.0f, 3);
    UiText("anim-group.sv.svDt.svDtCloseLb",
            m->bond[0] ? "CLOSE TO" : "");
    const char *bp = "";
    for (int i = 0; i < V->np; i++)
        if (!strcmp(V->p[i].name, m->bond)) {
            bp = V->p[i].pid;
            break;
        }
    Cl13Pic("anim-group.sv.svDt.svDtB", m->bond[0] ? bp : "");
    Cl13Fit("anim-group.sv.svDt.svDtBond", m->bond, 700.0f, 44.0f);
    UiText("anim-group.sv.svDt.svDtBondTy", m->bondty);
    UiText("anim-group.sv.svDt.svDtLatelyLb",
            m->lately[0] ? "LATELY" : "");
    Cl13Clipped("anim-group.sv.svDt.svDtLately", m->lately, 1590.0f,
                 34.0f, 3);
}

static void Cl13FillSv(const Cl13T *V)
{
    int n = V->np;
    Cl13Window(T_SV, n, 5, 4);
    char b[96];
    snprintf(b, sizeof b, "SURVIVORS");
    Cl13Tr(b, b, sizeof b);
    {
        char t[140];
        snprintf(t, sizeof t, "%s  %d", b, n);
        UiText("anim-group.sv.svTitle", t);
    }
    for (int i = 0; i < C13_SV_N; i++) {
        int idx = C13.off[T_SV] + i;
        char g[96], p[170];
        snprintf(g, sizeof g, "anim-group.sv.svRow%d", i);
        if (idx >= n) {
            UiVisible(g, 0);
            continue;
        }
        UiVisible(g, 1);
        const Cl13PT *m = &V->p[idx];
        int si = Cl13Pcode(m->st);
        snprintf(p, sizeof p, "%s.svPt%d", g, i);
        Cl13Pic(p, m->pid);
        snprintf(p, sizeof p, "%s.svRole%d", g, i);
        UiText(p, m->role);
        snprintf(p, sizeof p, "%s.svName%d", g, i);
        Cl13Fit(p, m->name, 580.0f, 42.0f);
        snprintf(p, sizeof p, "%s.svIc%d", g, i);
        UiVisible(p, m->st != 'h');
        if (m->st != 'h')
            Cl13Icon(p, C13_STICON[si]);
        char w[140] = "";
        if (m->st != 'h')
            snprintf(w, sizeof w, "<font color=\"#%s\">%s</font>",
                     C13_STTXT[si], m->stw);
        snprintf(p, sizeof p, "%s.svWord%d", g, i);
        UiText(p, w);
        for (int k = 0; k < 5; k++) {
            snprintf(p, sizeof p, "%s.svBar%d_%d", g, k, i);
            UiVisible(p, k == Cl13Pbar(m->st));
        }
        snprintf(p, sizeof p, "%s.svNew%dBg", g, i);
        UiVisible(p, 0);
        snprintf(p, sizeof p, "%s.svNew%dTx", g, i);
        UiVisible(p, 0);
        snprintf(p, sizeof p, "%s.svFill%d", g, i);
        GuFade(p, C13.tab == T_SV
                && (idx == C13.foc[T_SV] || C13.hv_row == i));
        snprintf(p, sizeof p, "%s.svFrame%d", g, i);
        GuFade(p, C13.tab == T_SV && idx == C13.foc[T_SV]);
    }
    {                           // diagnostics (one roster portrait blinked on and off, every other steady): who wears which texture, once per page open; "dup" = the same texture on two rows
        static int logged_gen = -1;
        if (logged_gen != (int) C13.enter_ms) {
            logged_gen = (int) C13.enter_ms;
            for (int i = 0; i < C13_SV_N; i += 5) {
                char ln[600];
                int o = 0;
                for (int k = i;
                     k < i + 5 && k < C13_SV_N && C13.off[T_SV] + k < n;
                     k++) {
                    const Cl13PT *q = &V->p[C13.off[T_SV] + k];
                    int dup = 0;
                    for (int j = 0; j < n; j++)
                        if (j != C13.off[T_SV] + k
                            && !strcmp(V->p[j].pid, q->pid))
                            dup = 1;
                    o += snprintf(ln + o, sizeof ln - (size_t) o,
                                  " %d:%s=%s%s", k, q->name, q->pid,
                                  dup ? " dup" : "");
                    if (o >= (int) sizeof ln - 60)
                        break;
                }
                Xlog("clan13: roster portraits%s", ln);
            }
        }
    }
    if (!C13.det_hold)
        Cl13FillSvDetail(V);
}
static const char *const C13_CAT_ICON[5] =
    { "food", "water", "medicine", "ammo", "materials" };
static void Cl13FillRdDetail(const Cl13T *V)
{
    int n = V->nm;
    if (n <= 0)
        return;
    const Cl13MT *m = &V->m[C13.foc[T_RD]];
    char b[160], p[170];
    UiText("anim-group.rds.rdDt.rdDtLb", "SUPPLIES BROUGHT HOME");
    snprintf(b, sizeof b, "%d", m->sup);
    Cl13Count("anim-group.rds.rdDt.rdDtBig", m->sup, b, 120.0f);
    snprintf(b, sizeof b, "/ %d NEEDED", m->need);
    UiText("anim-group.rds.rdDt.rdDtOf", b);
    int ok = m->sup >= m->need;
    if (ok)
        snprintf(b, sizeof b,
                 m->sup > m->need ? "ENOUGH, +%d OVER" : "ENOUGH",
                 m->sup - m->need);
    else
        snprintf(b, sizeof b, "SHORT BY %d SUPPLIES", m->need - m->sup);
    UiText("anim-group.rds.rdDt.rdVerd", b);
    UiVisible("anim-group.rds.rdDt.rdVerdFr", ok);
    UiVisible("anim-group.rds.rdDt.rdVerdFrS", !ok);
    static const int CI[NBAR_C] = { 0, 1, 2, 4 };
    for (int k = 0; k < NBAR_C; k++) {
        int ci = CI[k], got = m->got[ci], need = m->needc[ci], mx =
            got > need ? got : need;
        if (mx < 1)
            mx = 1;
        snprintf(p, sizeof p, "anim-group.rds.rdDt.rdBIc%d", k);
        Cl13Icon(p, C13_CAT_ICON[ci]);
        const char *lb =
            ci == 0 ? "FOOD" : ci == 1 ? "WATER" : ci ==
            2 ? "MEDICINE" : "MATERIALS";
        CL13_TEXT("anim-group.rds.rdDt.rdBLb%d", k, lb);
        if (need)
            snprintf(b, sizeof b, "%d / %d", got, need);
        else
            snprintf(b, sizeof b, "%d", got);
        CL13_TEXT("anim-group.rds.rdDt.rdBNum%d", k, b);
        if (need > 0 && got > need)
            snprintf(b, sizeof b, "+%d", got - need);
        else
            b[0] = 0;
        CL13_TEXT("anim-group.rds.rdDt.rdBDelta%d", k, b);
        snprintf(p, sizeof p, "anim-group.rds.rdDt.rdB%d", k);
        Cl13Grow(p, (float) got / (float) mx, 300.0f + 90.0f * k, 0, 0,
                  0);
        snprintf(p, sizeof p, "anim-group.rds.rdDt.rdBNeed%d", k);
        UiVisible(p, need > 0);
        uint64_t nw = UiW(p);
        if (nw && need > 0)
            WPivot(nw,
                    760.0f + 1900.0f * (float) need / (float) mx - 2.0f,
                    (float) (1180 + 88 * k + 22));
    }
    int ln = 0;
    for (int i = 0; i < V->nt && ln < 4; i++)
        if (V->t[i].day == m->day) {
            char g[96], q[170];
            snprintf(g, sizeof g, "anim-group.rds.rdDt.ltRow%d", ln);
            UiVisible(g, 1);
            snprintf(q, sizeof q, "%s.ltIc%d", g, ln);
            Cl13Icon(q, V->t[i].icon);
            snprintf(b, sizeof b, "x%s", V->t[i].qty);
            snprintf(q, sizeof q, "%s.ltQty%d", g, ln);
            UiText(q, b);
            snprintf(q, sizeof q, "%s.ltNm%d", g, ln);
            Cl13Clipped(q, V->t[i].name, 620.0f, 28.0f, 2);
            snprintf(q, sizeof q, "%s.ltNew%dBg", g, ln);
            UiVisible(q, V->t[i].fresh);
            snprintf(q, sizeof q, "%s.ltNew%dTx", g, ln);
            UiVisible(q, V->t[i].fresh);
            ln++;
        }
    for (int k = ln; k < 4; k++) {
        snprintf(p, sizeof p, "anim-group.rds.rdDt.ltRow%d", k);
        UiVisible(p, 0);
    }
    UiText("anim-group.rds.rdDt.rdLootLb", ln ? "BROUGHT HOME" : "");
}

static void Cl13FillRd(const Cl13T *V)
{
    char b[160], p[170];
    int n = V->nm;
    Cl13Window(T_RD, n, C13_RDS_ROWS, 1);
    UiText("anim-group.rds.rdTitle", "RAIDS");
    UiText("anim-group.rds.rdHint",
            n ? (g_padMode ?
                 "MOVE TO A RAID TO SEE WHAT IT BROUGHT HOME.  THE PLAYER STATS OF ITS MATCH: THE BUTTON AT THE RIGHT."
                 :
                 "CLICK A RAID TO SEE WHAT IT BROUGHT HOME.  THE PLAYER STATS OF ITS MATCH: THE BUTTON AT THE RIGHT.")
            : "NO RAIDS YET - PLAY A MATCH TO SEND A PARTY OUT");
    UiVisible("anim-group.rds.rdDt", n > 0);
    for (int i = 0; i < C13_RDS_ROWS; i++) {
        int idx = C13.off[T_RD] + i;
        char g[96];
        snprintf(g, sizeof g, "anim-group.rds.rrRow%d", i);
        if (idx >= n) {
            UiVisible(g, 0);
            continue;
        }
        UiVisible(g, 1);
        const Cl13MT *m = &V->m[idx];
        char d[40];
        snprintf(d, sizeof d, "DAY %d", Cl13Dow(m->day));
        snprintf(p, sizeof p, "%s.rrDay%d", g, i);
        UiText(p, d);
        snprintf(p, sizeof p, "%s.rrMap%d", g, i);
        Cl13Fit(p, m->map, 1000.0f, 42.0f);
        const char *wl =
            m->win ==
            1 ? "<font color=\"#7CB494\">MATCH WON</font>" : m->win ==
            0 ? "<font color=\"#FD6D51\">MATCH LOST</font>" : "";
        snprintf(p, sizeof p, "%s.rrWl%d", g, i);
        UiText(p, wl);
        snprintf(b, sizeof b, "<font color=\"#%s\">%d / %d</font>",
                 m->sup >= m->need ? "7CB494" : "E0B04A", m->sup, m->need);
        snprintf(p, sizeof p, "%s.rrNum%d", g, i);
        UiText(p, b);
        snprintf(p, sizeof p, "%s.rrBar%d", g, i);
        Cl13Grow(p, m->need > 0 ? (float) m->sup / (float) m->need : 1.0f,
                  200.0f + 70.0f * i, 0, 0, 0);
        Cl13Band("rds", "rrRow", "rr", i,
                  m->sup >= m->need ? "good" : "warn");
        snprintf(p, sizeof p, "%s.rrNew%dBg", g, i);
        UiVisible(p, m->fresh);
        snprintf(p, sizeof p, "%s.rrNew%dTx", g, i);
        UiVisible(p, m->fresh);
        snprintf(p, sizeof p, "%s.rrFill%d", g, i);
        GuFade(p, C13.tab == T_RD
                && (idx == C13.foc[T_RD] || C13.hv_row == i));
        snprintf(p, sizeof p, "%s.rrFrame%d", g, i);
        GuFade(p, C13.tab == T_RD && idx == C13.foc[T_RD]);
    }
    if (!C13.det_hold)
        Cl13FillRdDetail(V);
}

// ------------------------------------------------------------------ MISSIONS: the old to1/mission-choose pick in the clan page's look (the choice sent is still op 0xEF [mission id])
static int Cl13PickDue(void);
static void Cl13MsRows(void)
{
    MetaT m = MetaGet();
    int n = 0;
    if (Cl13PickDue() || C13.ms_chosen) {
        for (int id = 1; id <= 6; id++)
            if (MIS[id].live && m.mr[id] < 5)
                C13.ms_rows[n++] = id;  // (a mission completed 5 times is greyed out, 2013 list)
        if (!n)
            for (int id = 1; id <= 6; id++)
                if (MIS[id].live)
                    C13.ms_rows[n++] = id;
        for (int k = 0; k < 3 && n < C13_MS_ROWS; k++)  // + the random weapon-execution missions the server rolled for this choice
            if (m.p[k] > 0 && MIS_IS_EXEC(m.p[k]) && MIS_VALID(m.p[k])
                && m.mr[m.p[k]] < 5)
                C13.ms_rows[n++] = m.p[k];
    }
    if (n != C13.ms_n) {
        C13.ms_n = n;
        if (C13.foc[T_MS] >= n)
            C13.foc[T_MS] = n ? n - 1 : 0;
        Xlog("clan13: missions: %d rows (weapon-execution offers %d / %d / %d)", n, m.p[0], m.p[1], m.p[2]);
    }
}

static void Cl13MsUpper(char *d, size_t n, const char *src)
{
    size_t i = 0;
    for (; src[i] && i + 1 < n; i++)
        d[i] = src[i] >= 'a'
            && src[i] <= 'z' ? (char) (src[i] - 32) : src[i];
    d[i] = 0;
    Cl13CasePl(d, 1);
}

// The MISSIONS tab shows the current mission status instead of MISSION PICKED (no need to back out and read the small clan UI at the bottom left). With a mission running the tab is its status page: the three tiers as the rows (goal,
// DONE / how many to go), the detail pane with the live count (the server's + this match's own g_mis_cnt, like the HUD widget), the day of three,
// what the tiers pay the running event and how many times it was done before.
static int Cl13MsRunning(void)
{
    MetaT m = MetaGet();
    return m.ok && MIS_VALID(m.mi) && !C13.ms_n;
}

// the running mission's icon (layout A): a weapon-specific mission shows that weapon's own HUD icon (the death banner's table, death_ui.c: stock TLOU2 guns/ + the mod's
// streaming/t2r/icons), the others the matching icon of the mod's skill set (round TLOU2-style badges) or the launcher's clan icons (c13-*)
static const char *Cl13MsIcon(int id)
{
    switch (id) {
    case 1:
        return "streaming/t2r/icons/skill-damage-marker.png";   // down enemies: the downed-target chevron badge
    case 2:
    case 7:
        return "streaming/t2r/icons/skill-executioner.png";
    case 3:
        return "streaming/t2r/icons/skill-reviver.png";
    case 4:
        return "streaming/t2r/icons/skill-sharpshooter.png";
    case 5:
        return "streaming/t2r/icons/c13-binoculars.png";
    case 6:
        return "streaming/t2r/icons/skill-firstaid.png";
    case 8:
        return "streaming/t2r/icons/c13-molotov.png";
    case 9:
        return "streaming/t2r/icons/c13-pipebomb.png";
    case 10:
        return "streaming/t2r/icons/skill-crafter.png";
    case 11:
        return "streaming/t2r/icons/skill-brawler.png";
    case 12:
        return "streaming/t2r/icons/c13-shiv.png";
    case 13:
        return "streaming/t2r/icons/skill-hawk.png";
    case 14:
    case 48:
#if defined(T2R_COLT_BUILT) && T2R_COLT_BUILT
        return "streaming/t2r/icons/colt.png";
#else
        return "hud/weapons/guns/pistol-beretta/beretta.png";
#endif
    case 15:
    case 49:
        return "hud/weapons/guns/pistol-revolver-taurus/revolver-brl.png";
    case 16:
    case 52:
        return "streaming/t2r/icons/shorty.png";
    case 17:
    case 41:
        return "hud/weapons/guns/rifle-remington-bolt/remington-bolt.png";
    case 18:
    case 44:
        return "streaming/t2r/icons/m14.png";
    case 19:
    case 47:
        return "streaming/t2r/icons/burst-rifle.png";
    case 20:
    case 42:
        return "hud/weapons/guns/bow-ellie/bow.png";
    case 40:
        return "hud/weapons/guns/rifle-vepr/vepr-scp.png";
    case 43:
        return "hud/weapons/guns/rifle-mpx5/mpx5.png";
    case 45:
        return "streaming/t2r/icons/tac-shotgun.png";
    case 46:
        return "streaming/t2r/icons/frontier.png";
    case 50:
        return "hud/weapons/guns/pistol-glock/glock.png";
    case 51:
        return "streaming/t2r/icons/cz75.png";
    }
    return "streaming/t2r/icons/skill-damage-marker.png";
}

#define C13_RUN_BAR_L 384.0f    // the running view's progress bar (clan13_widgets.py BAR_L / BAR_W2 / BAR_T)
#define C13_RUN_BAR_W 3072.0f
#define C13_RUN_BAR_T 982.0f
#define C13_RUN_Y 559.0f
static void Cl13FillMsRun(void)
{
    char b[240], p[170], u[100], un[40];
    ClanT c = ClanGet();
    MetaT m = MetaGet();
    int id = m.mi, cnt = m.mp + g_misCnt[id], rep = m.mr[id], top =
        MisGoalM(&m, id, 2), reached = 0;
    for (int k = 0; k < 3; k++)
        if (cnt >= MisGoalM(&m, id, k))
            reached = k + 1;
    int match = 3 - m.ml + 1;   // the match we are in (the day calc of the old view)
    if (match < 1)
        match = 1;
    else if (match > 3)
        match = 3;
    Cl13MsUpper(u, sizeof u, MIS[id].name);
    Cl13MsUpper(un, sizeof un, MIS[id].unit);
    int size = c.h + c.hu + c.s, es = EvWinStart(c.day), ek =
        es >= 0 ? EvKindAt(es) : 0;
    int atk = m.mev && ek == 2, sur = m.mev && ek == 1;
    // the pick list is gone: its rows / detail pane / sub line are hidden, the header card + 3 tier cards of layout A stand
    UiText("anim-group.ms.msTitle", "CURRENT MISSION");
    UiText("anim-group.ms.msSub", "");
    for (int i = 0; i < C13_MS_ROWS; i++) {
        snprintf(p, sizeof p, "anim-group.ms.msRow%d", i);
        UiVisible(p, 0);
    }
    UiVisible("anim-group.ms.msDt", 0);
    UiVisible("anim-group.ms.msPrg", 1);
    // header card: icon, name, the event line (COMPLETE in green when every tier is in), MATCH n OF 3 + the pips
    UiImage("anim-group.ms.msPrg.msRunIc", Cl13MsIcon(id));
    Cl13Fit("anim-group.ms.msPrg.msRunNm", u, 2000.0f, 80.0f);
    if (reached >= 3)
        snprintf(b, sizeof b, "<font color=\"#7FD07A\">COMPLETE</font>");
    else
        snprintf(b, sizeof b, "%s",
                 atk ?
                 "ENEMY ATTACK - THE FURTHER YOU GET, THE FEWER YOU LOSE" :
                 sur ? "NEW SURVIVORS - THE FURTHER YOU GET, THE MORE JOIN"
                 : "THE FURTHER YOU GET, THE MORE BOOSTERS");
    UiText("anim-group.ms.msPrg.msPrgDay", b);
    snprintf(b, sizeof b, "MATCH %d OF 3", match);
    UiText("anim-group.ms.msPrg.msRunMatch", b);
    for (int k = 0; k < 3; k++) {
        snprintf(p, sizeof p, "anim-group.ms.msPrg.msRunPip%d", k);
        snprintf(b, sizeof b, "streaming/t2r/icons/c13-ms-pip%d.png", k + 1 < match ? 1 : k + 1 == match ? 2 : 0);      // played: filled, this one: ringed + dot, left: outline
        UiImage(p, b);
    }
    // the live count (the server's + this match's own g_mis_cnt, like the HUD widget) + its unit, ONE bar to the top goal with a tick + number per tier
    snprintf(b, sizeof b, "%d", cnt);
    UiText("anim-group.ms.msPrg.msPrgNum", b);
    uint64_t wl = UiW("anim-group.ms.msPrg.msPrgLb");
    UiText("anim-group.ms.msPrg.msPrgLb", un);
    if (wl)
        WPivot(wl, C13_RUN_BAR_L + GuEst(b, 104.0f) + 36.0f,
                C13_RUN_Y + 321.0f);
    Cl13Grow("anim-group.ms.msPrg.msBar",
              top > 0 ? (float) (cnt <
                                 top ? cnt : top) / (float) top : 1.0f,
              200.0f, 0, 0, 0);
    for (int k = 0; k < 3; k++) {
        int goal = MisGoalM(&m, id, k);
        float x =
            C13_RUN_BAR_L + (top >
                             0 ? C13_RUN_BAR_W * (float) goal /
                             (float) top : 0.0f);
        snprintf(p, sizeof p, "anim-group.ms.msPrg.msTk%d", k);
        uint64_t w = UiW(p);
        if (w) {
            WPivot(w, x - 3.0f, C13_RUN_BAR_T - 28.0f);
            WAlpha(w, cnt >= goal ? 1.0f : 0.47f);
        }
        snprintf(p, sizeof p, "anim-group.ms.msPrg.msTkLb%d", k);
        snprintf(b, sizeof b, "<font color=\"#%s\">%d</font>",
                 cnt >= goal ? "E6E6E6" : "8D897B", goal);
        UiText(p, b);
        w = UiW(p);
        if (w)
            WPivot(w, x - 100.0f, C13_RUN_BAR_T + 54.0f);
    }
    // the 3 tier cards: reached = green check, the next one = ringed dot + cream stripe + N TO GO, later = lock, dimmed
    for (int k = 0; k < 3; k++) {
        int goal = MisGoalM(&m, id, k), st =
            k < reached ? 0 : k == reached ? 1 : 2, dim = st == 2, bo =
            MisBoost(rep, k);
        const char *tx = dim ? "8D897B" : "E6E6E6";
        snprintf(p, sizeof p, "anim-group.ms.msPrg.msRtSt%d", k);
        snprintf(b, sizeof b, "streaming/t2r/icons/c13-ms-%s.png",
                 st == 0 ? "check" : st == 1 ? "cur" : "lock");
        UiImage(p, b);
        snprintf(p, sizeof p, "anim-group.ms.msPrg.msRtEd%d", k);
        UiVisible(p, st == 1);
        snprintf(p, sizeof p, "anim-group.ms.msPrg.msRtLb%d", k);
        snprintf(b, sizeof b, "TIER %d", k + 1);
        UiText(p, b);
        snprintf(p, sizeof p, "anim-group.ms.msPrg.msRtTog%d", k);
        if (st == 1)
            snprintf(b, sizeof b, "%d TO GO", goal - cnt);
        else
            b[0] = 0;
        UiText(p, b);
        snprintf(p, sizeof p, "anim-group.ms.msPrg.msRtGoal%d", k);
        snprintf(b, sizeof b, "<font color=\"#%s\">%d %s</font>", tx, goal,
                 un);
        Cl13Fit(p, b, 838.0f, 58.0f);
        const char *ri[2] = { 0, 0 };
        char rt[2][24] = { { 0 }, { 0 } };
        int nr = 0;
        if (atk) {
            ri[nr] = "streaming/t2r/icons/c13-ms-lose.png";
            snprintf(rt[nr++], 24, "%d%%", EvLossPct(es, k + 1));
        } else if (sur) {
            ri[nr] = "streaming/t2r/icons/c13-person.png";
            snprintf(rt[nr++], 24, "+%d", MisGrowth(size, k));
        }
        ri[nr] = "streaming/t2r/icons/booster-ammo-start.png";
        snprintf(rt[nr++], 24, "x%d", bo);
        for (int q = 0; q < 2; q++) {
            snprintf(p, sizeof p, "anim-group.ms.msPrg.msRtI%d_%d", q, k);
            UiVisible(p, q < nr);
            if (q < nr) {
                UiImage(p, ri[q]);
                uint64_t w = UiW(p);
                if (w)
                    WAlpha(w, dim ? 0.43f : 1.0f);
            }
            snprintf(p, sizeof p, "anim-group.ms.msPrg.msRtT%d_%d", q, k);
            UiVisible(p, q < nr);
            if (q < nr) {
                snprintf(b, sizeof b, "<font color=\"#%s\">%s</font>", tx,
                         rt[q]);
                UiText(p, b);
            }
        }
    }
    if (rep > 0)
        snprintf(b, sizeof b,
                 "NOTHING IS PAID BELOW TIER 1 - COMPLETED %d TIME%s BEFORE: GOALS ARE HIGHER",
                 rep, rep == 1 ? "" : "S");
    else
        snprintf(b, sizeof b, "NOTHING IS PAID BELOW TIER 1");
    UiText("anim-group.ms.msPrg.msFoot", b);
}

static void Cl13FillMs(const Cl13T *V)
{
    (void) V;
    if (Cl13MsRunning()
        && GetFileAttributesA("t2r-clan-ms-old.txt") ==
        INVALID_FILE_ATTRIBUTES) {
        Cl13FillMsRun();
        return;
    }
    char b[200], p[170], u[100];
    ClanT c = ClanGet();
    MetaT m = MetaGet();
    Cl13Window(T_MS, C13.ms_n, C13_MS_ROWS, 1);
    UiVisible("anim-group.ms.msPrg", 0);
    UiText("anim-group.ms.msTitle", "CHOOSE A MISSION");
    UiText("anim-group.ms.msSub",
            C13.ms_n ? "YOU HAVE 3 MATCHES TO COMPLETE YOUR MISSION" : "");
    UiVisible("anim-group.ms.msDt", C13.ms_n > 0);
    for (int i = 0; i < C13_MS_ROWS; i++) {
        char g[96];
        snprintf(g, sizeof g, "anim-group.ms.msRow%d", i);
        UiVisible(g, i < C13.ms_n);
        if (i >= C13.ms_n)
            continue;
        int id = C13.ms_rows[i], rep = m.mr[id];
        Cl13MsUpper(u, sizeof u, MIS[id].name);
        snprintf(p, sizeof p, "%s.msNm%d", g, i);
        Cl13Fit(p, u, 860.0f, 44.0f);
        snprintf(b, sizeof b, "DONE %dX", rep);
        snprintf(p, sizeof p, "%s.msRp%d", g, i);
        UiText(p, rep ? b : "");
        snprintf(p, sizeof p, "%s.msFill%d", g, i);
        GuFade(p, C13.tab == T_MS
                && (i == C13.foc[T_MS] || C13.hv_row == i));
        snprintf(p, sizeof p, "%s.msFrame%d", g, i);
        GuFade(p, C13.tab == T_MS && i == C13.foc[T_MS]);
    }
    if (!C13.ms_n)
        return;
    int id = C13.ms_rows[C13.foc[T_MS]], rep = m.mr[id];
    Cl13MsUpper(u, sizeof u, MIS[id].name);
    Cl13Fit("anim-group.ms.msDt.msDtName", u, 1640.0f, 60.0f);
    UiText("anim-group.ms.msDt.msDtSub", "3 MATCHES");
    int size = c.h + c.hu + c.s, es = EvWinStart(c.day), ek =
        es >= 0 ? EvKindAt(es) : 0;
    UiText("anim-group.ms.msDt.msRewLb", "REWARDS");
    UiText("anim-group.ms.msDt.msRewSub", "");
    for (int k = 0; k < 3; k++) {
        int bo = MisBoost(rep, k);
        char un[40];
        Cl13MsUpper(un, sizeof un, MIS[id].unit);
        snprintf(p, sizeof p, "anim-group.ms.msDt.msTr%d.msTrLb%d", k, k);
        snprintf(b, sizeof b, "%d", k + 1);
        UiText(p, b);
        snprintf(p, sizeof p, "anim-group.ms.msDt.msTr%d.msTrGoal%d", k,
                 k);
        snprintf(b, sizeof b, "%d %s", MisGoalM(&m, id, k), un);
        Cl13Fit(p, b, 700.0f, 46.0f);
        if (ek == 2)
            snprintf(b, sizeof b, "LOSE %d%%   +%d BOOSTER%s",
                     EvLossPct(es, k + 1), bo, bo == 1 ? "" : "S");
        else {
            int Sv = MisGrowth(size, k);
            snprintf(b, sizeof b, "+%d SURVIVOR%s   +%d BOOSTER%s", Sv,
                     Sv == 1 ? "" : "S", bo, bo == 1 ? "" : "S");
        }
        snprintf(p, sizeof p, "anim-group.ms.msDt.msTr%d.msTrRew%d", k, k);
        UiText(p, b);
    }
    if (ek == 2)
        snprintf(b, sizeof b, "NO MARK: LOSE %d%%", EvLossPct(es, 0));
    else
        b[0] = 0;
    UiText("anim-group.ms.msDt.msRep", b);
    UiText("anim-group.ms.msDt.msRep2", "");
    UiText("anim-group.ms.msDt.msStatus",
            C13.ms_chosen ? "MISSION STARTED" : "");
}

// ------------------------------------------------------------------ JOURNAL: week pages. The left column is ONE week
// (< WEEK n OF 12 >, its won / lost count) with a row per clan day - day, headline, map, verdict, supplies / need; the right pane is the focused day: headline, two lines of facts
// (raid, supplies, the clan's counts after the day) and the entry's paragraphs, each under its caption. The paragraphs are wrapped HERE (cl13_wrap: explicit line breaks at 88 % of the
// box, so the engine never wraps again and the line count is known) and stacked by their real heights. Data: section 6 (every day's index row + the text of the shown week).
static int Cl13Jw(const Cl13T *V)
{
    int top = V && V->jtop > 0 ? V->jtop : 1;
    return C13.jw >= 1 && C13.jw <= top ? C13.jw : top;
}                               // the week shown (C13.jw 0 = the newest)

static int Cl13JwSet(int w, int foc_last, const char *why)
{                               // turn to week w (1 .. the newest); the focus lands on its first (or last) day; its text is asked for at once
    const Cl13T *V = C13.V;
    if (!V)
        return 0;
    int top = V->jtop > 0 ? V->jtop : 1;
    if (w < 1 || w > top || w == Cl13Jw(V))
        return 0;
    int idx[C13_ND], n = Cl13WeekRows(V, w, idx, C13_ND);
    C13.jw = w;
    C13.off[T_JR] = 0;
    C13.foc[T_JR] = foc_last && n ? n - 1 : 0;
    C13.next_ask[6] = 0;
    C13.ok_at[6] = 0;
    C13.dirty = 1;
    C13.det_key = -1;
    Cl13Sfx();
    Xlog("clan13: journal week %d of %d (%s)", w, top, why);
    return 1;
}

static size_t Cl13App(char *b, size_t cap, size_t o, const char *fmt, ...)
{                               // append to b at o, never past cap; returns the new length
    if (o + 1 >= cap)
        return o;
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(b + o, cap - o, fmt, ap);
    va_end(ap);
    if (k < 0)
        return o;
    return o + (size_t) k < cap ? o + (size_t) k : cap - 1;
}

static void Cl13JdVerdict(char *b, size_t cap, const Cl13DT *e)
{                               // "WON" green / "LOST" red ("" = no verdict)
    char w[40];
    b[0] = 0;
    if (e->win < 0)
        return;
    snprintf(w, sizeof w, "%s", e->win ? "WON" : "LOST");
    Cl13Tr(w, w, sizeof w);
    snprintf(b, cap, "<font color=\"#%s\">%s</font>",
             e->win ? "7CB494" : "FD6D51", w);
}

static void Cl13FillJrDay(const Cl13T *V, const Cl13DT *e)
{
    static const char *const PC[C13_JP] =
        { "anim-group.jrn.jtPane.jtCap0", "anim-group.jrn.jtPane.jtCap1",
"anim-group.jrn.jtPane.jtCap2", "anim-group.jrn.jtPane.jtCap3", "anim-group.jrn.jtPane.jtCap4" };
    static const char *const PB[C13_JP] =
        { "anim-group.jrn.jtPane.jtBo0", "anim-group.jrn.jtPane.jtBo1",
"anim-group.jrn.jtPane.jtBo2", "anim-group.jrn.jtPane.jtBo3", "anim-group.jrn.jtPane.jtBo4" };
    char b[420], t[96], v[96];
    if (!e)
        return;
    int wk = Cl13Jw(V);
    if (e->day > 0) {
        snprintf(b, sizeof b, "WEEK %d  -  DAY %d", wk, e->dow);
        UiText("anim-group.jrn.jtPane.jtLb", b);
    } else
        UiText("anim-group.jrn.jtPane.jtLb", "THE START");
    Cl13Fit("anim-group.jrn.jtPane.jtTitle", e->title, C13_JT_W, 72.0f);
    size_t o = 0;               // facts, line 1: the raid and the supplies
    b[0] = 0;
    if (e->map[0]) {
        snprintf(t, sizeof t, "RAID:");
        Cl13Tr(t, t, sizeof t);
        Cl13JdVerdict(v, sizeof v, e);
        o = Cl13App(b, sizeof b, o, "%s  %s%s%s", t, e->map,
                     v[0] ? "  " : "", v);
    }
    if (e->need > 0) {
        snprintf(t, sizeof t, "SUPPLIES:");
        Cl13Tr(t, t, sizeof t);
        const char *col =
            e->sup >= e->need ? "7CB494" : (e->need - e->sup) * 3 <
            e->need ? "E0B04A" : "FD6D51";
        o = Cl13App(b, sizeof b, o,
                     "%s%s  <font color=\"#%s\">%d / %d</font>",
                     o ? "      " : "", t, col, e->sup, e->need);
    }
    UiText("anim-group.jrn.jtPane.jtF0", b);
    b[0] = 0;
    o = 0;                      // line 2: the clan after the day (an entry from before the day entries has no counts)
    if (e->h >= 0) {
        int n = e->h + e->u + e->s;
        snprintf(t, sizeof t, "%d SURVIVORS", n);
        Cl13Tr(t, t, sizeof t);
        o = Cl13App(b, sizeof b, o, "%s", t);
        if (e->dn)
            o = Cl13App(b, sizeof b, o, " (%+d)", e->dn);
        snprintf(t, sizeof t, "%d HEALTHY", e->h);
        Cl13Tr(t, t, sizeof t);
        o = Cl13App(b, sizeof b, o, "      %s", t);
        snprintf(t, sizeof t, "%d HUNGRY", e->u);
        Cl13Tr(t, t, sizeof t);
        o = Cl13App(b, sizeof b, o, "      <font color=\"#%s\">%s</font>",
                     e->u ? "E0B04A" : "8D897B", t);
        snprintf(t, sizeof t, "%d SICK", e->s);
        Cl13Tr(t, t, sizeof t);
        o = Cl13App(b, sizeof b, o, "      <font color=\"#%s\">%s</font>",
                     e->s ? "FD6D51" : "8D897B", t);
    }
    UiText("anim-group.jrn.jtPane.jtF1", b);
    int pi[C13_NG], np = V->jweek == wk ? Cl13DayParas(V, e->day, pi, C13_NG) : 0, wait = V->jweek != wk;     // (the text of another week is on its way: LOADING)
    float y = C13_JT_Y0;
    static char src[4200], out[2400];
    for (int k = 0; k < C13_JP; k++) {
        int room = (int) ((C13_JT_Y1 - y - C13_JT_CAP_H) / C13_JT_LINE);
        int on = (k < np || (wait && k == 0)) && room >= 1;
        UiVisible(PC[k], on);
        UiVisible(PB[k], on);
        if (!on)
            continue;
        const char *cap = "";
        if (wait)
            snprintf(src, sizeof src, "%s", LocTr("LOADING..."));
        else {
            cap = V->g[pi[k]].cap;
            size_t so =
                Cl13App(src, sizeof src, 0, "%s", V->g[pi[k]].text);
            if (k == C13_JP - 1)
                for (int q = C13_JP; q < np; q++)
                    so = Cl13App(src, sizeof src, so, "\n\n%s", V->g[pi[q]].text);     // more paragraphs than slots (two matches on one clan day): the rest follows here
        }
        int n =
            Cl13Wrap(out, sizeof out, src, C13_JT_W * 0.88f, C13_JT_PT,
                      room, Cl13Est);
        uint64_t wc = UiW(PC[k]), wb = UiW(PB[k]);
        if (wc)
            WPivot(wc, C13_JT_X, y);
        if (wb)
            WPivot(wb, C13_JT_X, y + C13_JT_CAP_H);
        UiText(PC[k], cap);
        UiText(PB[k], out);
        y += C13_JT_CAP_H + (float) n *C13_JT_LINE + C13_JT_GAP;
    }
}

static void Cl13FillJr(const Cl13T *V)
{
    int idx[C13_ND], wk = Cl13Jw(V), top = V->jtop > 0 ? V->jtop : 1;
    int n = Cl13WeekRows(V, wk, idx, C13_ND);
    if (!C13.jr_init && n) {
        C13.jr_init = 1;
        C13.foc[T_JR] = n - 1;
    }                           // the tab opens on the newest entry
    Cl13Window(T_JR, n, C13_JD_ROWS, 1);
    char b[260], p[170], g[96];
    snprintf(b, sizeof b, "WEEK %d OF 12", wk);
    UiText("anim-group.jrn.jwTitle", b);
    int won = 0, lost = 0;
    for (int i = 0; i < n; i++) {
        if (V->d[idx[i]].win == 1)
            won++;
        else if (V->d[idx[i]].win == 0)
            lost++;
    }
    snprintf(b, sizeof b, "%d WON  -  %d LOST", won, lost);
    UiText("anim-group.jrn.jwSub", n ? b : "");
    WAlpha(UiW("anim-group.jrn.jwPrev"), wk > 1 ? (C13.hv_jp ? 1.0f : 0.75f) : 0.18f);        // an arrow with nowhere to go is dim
    WAlpha(UiW("anim-group.jrn.jwNext"),
            wk < top ? (C13.hv_jn ? 1.0f : 0.75f) : 0.18f);
    for (int i = 0; i < C13_JD_ROWS; i++) {
        int ci = C13.off[T_JR] + i;
        snprintf(g, sizeof g, "anim-group.jrn.jdRow%d", i);
        if (ci >= n) {
            UiVisible(g, 0);
            continue;
        }
        UiVisible(g, 1);
        const Cl13DT *e = &V->d[idx[ci]];
        snprintf(p, sizeof p, "%s.jdDay%d", g, i);
        if (e->day > 0) {
            snprintf(b, sizeof b, "DAY %d", e->dow);
            UiText(p, b);
        } else
            UiText(p, "THE START");
        snprintf(p, sizeof p, "%s.jdTi%d", g, i);
        Cl13Fit(p, e->title, 1208.0f, 46.0f);
        {                       // the right corner: MAP - WON / LOST - supplies / need
            char v[96];
            size_t o = 0;
            b[0] = 0;
            Cl13JdVerdict(v, sizeof v, e);
            if (e->map[0])
                o = Cl13App(b, sizeof b, o, "%s", e->map);
            if (v[0])
                o = Cl13App(b, sizeof b, o, "%s%s", o ? "  -  " : "", v);
            if (e->need > 0)
                o = Cl13App(b, sizeof b, o, "%s%d / %d", o ? "  -  " : "",
                             e->sup, e->need);
            snprintf(p, sizeof p, "%s.jdRes%d", g, i);
            UiText(p, b);
        }
        int t =
            !strcmp(e->tone, "good") ? 1 : !strcmp(e->tone,
                                                   "warn") ? 2 :
            !strcmp(e->tone, "bad") ? 3 : 0;
        {
            char im[80];
            snprintf(p, sizeof p, "%s.jdBd%d", g, i);
            snprintf(im, sizeof im, "streaming/t2r/icons/c13-band%d.png",
                     t);
            UiImage(p, im);
        }                       // (one band image per row, c13-band0..3: the page is at gui2's 1024-widget limit)
        snprintf(p, sizeof p, "%s.jdNew%dBg", g, i);
        UiVisible(p, e->fresh);
        snprintf(p, sizeof p, "%s.jdNew%dTx", g, i);
        UiVisible(p, e->fresh);
        snprintf(p, sizeof p, "%s.jdFill%d", g, i);
        GuFade(p, C13.tab == T_JR
                && (ci == C13.foc[T_JR] || C13.hv_row == i));
        snprintf(p, sizeof p, "%s.jdFrame%d", g, i);
        GuFade(p, C13.tab == T_JR && ci == C13.foc[T_JR]);
    }
    UiVisible("anim-group.jrn.jtPane", n > 0);
    if (n && !C13.det_hold)
        Cl13FillJrDay(V, &V->d[idx[C13.foc[T_JR]]]);
}

static int Cl13RowsOf(int tab)
{                               // the focus list's length of a tab
    if (!C13.V)
        return 0;
    if (tab == T_OV)
        return C13.V->nu < C13_OVST ? C13.V->nu : C13_OVST;
    if (tab == T_SV)
        return C13.V->np;
    if (tab == T_MS)
        return C13.ms_n;
    if (tab == T_JR) {
        int c[C13_ND];
        return Cl13WeekRows(C13.V, Cl13Jw(C13.V), c, C13_ND);
    }
    return C13.V->nm;
}

// the sections a tab needs before it can be shown
static int Cl13Have(const Cl13T *V, int tab)
{
    if (tab == T_OV)
        return V->have[1] && V->have[2] && V->have[3] && V->have[4];
    if (tab == T_SV)
        return V->have[1];
    if (tab == T_MS)
        return MetaGet().ok;   // the pick runs on the booster / mission data (op 0xE8), not on the clan sections
    if (tab == T_RD)
        return V->have[3];
    return V->have[6];          // (the journal's week pages)
}

static int Cl13PickDue(void)
{                               // a mission is to be chosen now (feature_missions' offer rule): the clan is alive, none runs, an event is in its three matches
    ClanT c = ClanGet();
    MetaT m = MetaGet();
    return c.ok && c.st == 1 && c.fac && m.ok && !m.mi
        && EvWinStart(c.day) >= 0;
}

static const char *Cl13Msg(const Cl13T *V)
{
    if (C13.tab == T_MS) {
        static char b[120];
        MetaT m = MetaGet();
        if (!m.ok)
            return "LOADING THE MISSIONS";
        if (Cl13MsRunning()
            && GetFileAttributesA("t2r-clan-ms-old.txt") ==
            INVALID_FILE_ATTRIBUTES)
            return "";          // (the running mission's status page, cl13_fill_ms_run)
        if (m.mi && MIS_VALID(m.mi) && !C13.ms_chosen) {
            snprintf(b, sizeof b, "MISSION RUNNING: %s", MIS[m.mi].name);
            for (char *q = b + 17; *q; q++)
                if (*q >= 'a' && *q <= 'z')
                    *q = (char) (*q - 32);
            return b;
        }
        if (!C13.ms_n && !C13.ms_chosen)
            return
                "NO MISSION TO CHOOSE RIGHT NOW - THE NEXT CLAN EVENT BRINGS ONE";
        return "";
    }
    if (!Cl13Have(V, C13.tab))
        return !g_clan_busy
            && g_clanFail >=
            2 ? "THE SERVER DID NOT ANSWER" : "LOADING THE CLAN";
    if (!V->ok)
        return "YOU HAVE NO CLAN YET";
    if (C13.tab == T_SV && V->np == 0)
        return "THE CLAN HAS NO MEMBERS YET";
    if (C13.tab == T_JR && V->nd == 0)
        return "NOTHING IS WRITTEN IN THE JOURNAL YET";
    return "";
}

// ------------------------------------------------------------------ the entrance script of the shown tab (what comes from where and when: re/clan-ui-motion.md section 4)
static void Cl13FxBuild(const Cl13T *V)
{
    Cl13FxReset();
    char p[120];
    if (C13.fx_first) {
        Cl13FxAdd("anim-group.hd", 0.0f, 0.0f, 4, 0.0f);
        Cl13FxAdd("anim-group.tabs", 0.0f, 0.0f, 6, 60.0f);
    }
    float step;
    if (C13.tab == T_OV) {
        Cl13FxAdd("anim-group.ov.ovSt.ovStTitle", 300.0f, 424.0f, 1,
                    100.0f);
        Cl13FxAdd("anim-group.ov.ovSt.ovStSub", 1000.0f, 452.0f, 1,
                    160.0f);
        for (int i = 0; i < V->nu && i < C13_OVST; i++) {
            snprintf(p, sizeof p, "anim-group.ov.ovSt.stRow%d", i);
            Cl13FxAdd(p, 300.0f + (i % 2) * 1660.0f,
                        540.0f + (i / 2) * 300.0f, 1, 180.0f + 90.0f * i);
        }
        Cl13FxAdd("anim-group.ov.ovSup.ovSupTitle", 300.0f, 1170.0f, 1,
                    520.0f);
        Cl13FxAdd("anim-group.ov.ovSup.ovSupNone", 300.0f, 1262.0f, 1,
                    580.0f);
        Cl13FxAdd("anim-group.ov.ovSup.ovLastLb", 1900.0f, 1194.0f, 1,
                    540.0f);
        Cl13FxAdd("anim-group.ov.ovSup.ovLast", 3000.0f, 1150.0f, 1,
                    540.0f);
        for (int k = 0; k < V->ns && k < 5; k++) {
            snprintf(p, sizeof p, "anim-group.ov.ovSup.tlRow%d", k);
            Cl13FxAdd(p, 300.0f + k * 655.0f, 1290.0f, 3,
                        560.0f + 70.0f * k);
        }
        Cl13FxAdd("anim-group.ov.ovCamp.ovCampTitle", 300.0f, 1560.0f, 1,
                    900.0f);
        Cl13FxAdd("anim-group.ov.ovCamp.ovClans", 1500.0f, 1588.0f, 2,
                    940.0f);
        Cl13FxAdd("anim-group.ov.ovCamp.mo", 0.0f, 0.0f, 1, 940.0f);
        Cl13FxAdd("anim-group.ov.ovRec", 0.0f, 0.0f, 6, 1050.0f);
    } else if (C13.tab == T_SV) {
        Cl13FxAdd("anim-group.sv.svTitle", 300.0f, 424.0f, 1, 0.0f);
        int n = V->np - C13.off[T_SV];
        if (n > C13_SV_N)
            n = C13_SV_N;
        step = n > 0 ? 720.0f / (float) n : 45.0f;
        if (step > 45.0f)
            step = 45.0f;
        for (int i = 0; i < n; i++) {
            snprintf(p, sizeof p, "anim-group.sv.svRow%d", i);
            Cl13FxAdd(p, 300.0f + (i % 4) * 820.0f,
                        520.0f + (i / 4) * 170.0f, 1, 100.0f + step * i);
        }
        Cl13FxAdd("anim-group.sv.svDt", 0.0f, 0.0f, 3, 700.0f);
    } else if (C13.tab == T_RD) {
        Cl13FxAdd("anim-group.rds.rdTitle", 300.0f, 410.0f, 1, 0.0f);
        Cl13FxAdd("anim-group.rds.rdHint", 300.0f, 504.0f, 1, 60.0f);
        int n = V->nm - C13.off[T_RD];
        if (n > C13_RDS_ROWS)
            n = C13_RDS_ROWS;
        for (int i = 0; i < n; i++) {
            snprintf(p, sizeof p, "anim-group.rds.rrRow%d", i);
            Cl13FxAdd(p, 300.0f, 565.0f + 110.0f * i, 2,
                        150.0f + 70.0f * i);
        }
        if (V->nm > 0) {
            Cl13FxAdd("anim-group.rds.rdDt", 0.0f, 0.0f, 1, 450.0f);
            Cl13FxAdd("anim-group.rds.rdDt.rdVerd", 2780.0f, 1050.0f, 5,
                        900.0f);
            Cl13FxAdd("anim-group.rds.rdDt.rdVerdFr", 2780.0f, 1050.0f,
                        5, 900.0f);
            Cl13FxAdd("anim-group.rds.rdDt.rdVerdFrS", 2780.0f, 1050.0f,
                        5, 900.0f);
            for (int k = 0; k < 4; k++) {
                snprintf(p, sizeof p, "anim-group.rds.rdDt.ltRow%d", k);
                Cl13FxAdd(p, 300.0f + k * 820.0f, 1592.0f, 5,
                            1000.0f + 80.0f * k);
            }
        }
    } else if (C13.tab == T_MS) {
        Cl13FxAdd("anim-group.ms.msTitle", 300.0f, 424.0f, 1, 0.0f);
        Cl13FxAdd("anim-group.ms.msSub", 1140.0f, 450.0f, 1, 60.0f);
        if (Cl13MsRunning()
            && GetFileAttributesA("t2r-clan-ms-old.txt") ==
            INVALID_FILE_ATTRIBUTES)
            Cl13FxAdd("anim-group.ms.msPrg", 0.0f, 0.0f, 1, 100.0f);
        for (int i = 0; i < C13.ms_n && i < C13_MS_ROWS; i++) {
            snprintf(p, sizeof p, "anim-group.ms.msRow%d", i);
            Cl13FxAdd(p, 300.0f, 540.0f + 112.0f * i, 1,
                        80.0f + 50.0f * i);
        }
        if (C13.ms_n)
            Cl13FxAdd("anim-group.ms.msDt", 0.0f, 0.0f, 2, 400.0f);
    } else {
        int n = Cl13RowsOf(T_JR) - C13.off[T_JR];
        if (n > C13_JD_ROWS)
            n = C13_JD_ROWS;
        for (int i = 0; i < n; i++) {
            snprintf(p, sizeof p, "anim-group.jrn.jdRow%d", i);
            Cl13FxAdd(p, 300.0f, C13_JD_Y0 + C13_JD_DY * (float) i, 2,
                        50.0f * i);
        }
        Cl13FxAdd("anim-group.jrn.jtPane", 0.0f, 0.0f, 1, 250.0f);
    }
    uint64_t now = GetTickCount64();
    C13.t0 = now;
    for (int i = 0; i < C13.nfx; i++) { // invisible until their turn (no flash at full strength first)
        uint64_t w = UiW(C13.fx[i].path);
        C13.fx[i].w = w;
        if (w) {
            WAlpha(w, 0.0f);
            if (C13.fx[i].kind != 6)
                WPivot(w,
                        C13.fx[i].bx + (C13.fx[i].kind ==
                                        1 ? -96.0f : C13.fx[i].kind ==
                                        2 ? 96.0f : 0.0f), C13.fx[i].by);
        }                       // (kind 6 = fade only: its group keeps the pivot the fill gave it - ovRec sat at the top over the title for a frame)
    }
    if (!C13.fx_first)
        C13.hd_done = 1;        // a later entrance (tab switch): the first one's header grow is over or cut by cl13_settle
    C13.fx_pending = 0;
    C13.fx_first = 0;
    C13.played[C13.tab] = 1;
    C13.intro_played = 1;
    UiSfx("sfx-hud-rl-score-screen-enter");    // the NR screen-enter cue (to be heard once in game: re/clan-ui-motion.md section 6)
}

// ------------------------------------------------------------------ MATCH STATS: every player of the focused raid's match (F / the pad's menu-action on RAIDS; daemon op 0xB6 section 5)
static void Cl13XsNum(char *o, size_t n, int v)
{
    if (v < 0)
        snprintf(o, n, "0");
    else
        snprintf(o, n, "%d", v);
}

static void Cl13FillXsOld(const Cl13T *V, int show)
{                               // the flat list (t2r-clan-xs-old.txt)
    int on = show && C13.xs_open;
    UiVisible("anim-group.mst", on);
    if (!on)
        return;
    char b[200], p[200];
    const char *wl =
        C13.xs_win ==
        1 ? "<font color=\"#7CB494\">MATCH WON</font>" : C13.xs_win ==
        0 ? "<font color=\"#FD6D51\">MATCH LOST</font>" : "";
    snprintf(b, sizeof b, "DAY %d   -   %s%s%s", Cl13Dow(C13.xs_day),
             C13.xs_map, wl[0] ? "   -   " : "", wl);
    UiText("anim-group.mst.xsSub", b);
    int n = C13.xs_got && V->xday == C13.xs_day ? V->xn : 0, mine = -1;
    for (int i = 0; i < n; i++)
        if (V->x[i].me)
            mine = V->x[i].team;
    for (int i = 0; i < C13_NX; i++) {
        char g[96];
        snprintf(g, sizeof g, "anim-group.mst.xsRow%d", i);
        UiVisible(g, i < n);
        if (i >= n)
            continue;
        const Cl13XT *x = &V->x[i];
        int band = x->team < 0 || mine < 0 ? 0 : x->team == mine ? 1 : 2;
        for (int q = 0; q < 3; q++) {
            snprintf(p, sizeof p, "%s.xsBd%d_%d", g, q, i);
            UiVisible(p, q == band);
        }
        snprintf(p, sizeof p, "%s.xsMe%d", g, i);
        UiVisible(p, x->me);
        snprintf(b, sizeof b, "%s%s", x->name, x->me ? "  (YOU)" : "");
        snprintf(p, sizeof p, "%s.xsNm%d", g, i);
        Cl13Fit(p, b, 760.0f, 40.0f);
        const char *tm =
            x->team < 0 ? "" : mine < 0 ? (x->team ==
                                           0 ? "TEAM 1" : "TEAM 2") : x->
            team == mine ? "YOUR TEAM" : "OPPOSING TEAM";
        snprintf(b, sizeof b, "%s%s", tm,
                 x->
                 left ? (tm[0] ? "   -   LEFT EARLY" : "LEFT EARLY") : "");
        snprintf(p, sizeof p, "%s.xsTm%d", g, i);
        UiText(p, b);
        const int vals[7] =
            { x->kills, x->downs, x->deaths, x->revives, x->execs,
x->heads, x->parts };
        for (int k = 0; k < 7; k++) {
            char v[16];
            Cl13XsNum(v, sizeof v, vals[k]);
            snprintf(p, sizeof p, "%s.xsV%d_%d", g, k, i);
            UiText(p, v);
        }
    }
    const char *em =
        !C13.
        xs_got ? "LOADING..." : n ? "" :
        "NO PLAYER STATS WERE RECORDED FOR THIS MATCH.\nONLY MATCHES PLAYED AFTER THE LATEST SERVER UPDATE ARE LISTED.";
    UiText("anim-group.mst.xsEmpty", em);
    UiVisible("anim-group.mst.xsEmpty", em[0] != 0);
}

// ------------------------------------------------------------------ PLAYER STATS = the SCOREBOARD (proposal A)
// One table: a column-head row (PLAYER AWARD DOWNS EXECUTIONS REVIVES HEADSHOTS DEATHS PARTS), a section row per team (US / THE ENEMY, MATCH WON / LOST, the team totals in the same
// columns), then one row per player (rank badge, name, YOU / LEFT, MOST <STAT> for a stat's leader, five stat columns, PARTS). A stat's leader shows its number in the stat's colour;
// a zero / unreported value is a dim "0"; a player who LEFT is dimmed. The row pool is flat (C13_NX groups xpRow<k>, US rows first); every row is placed by the ext (design = clan13_widgets.py XP_*):
// up to 8 players on a 112 pitch (104 high), more on an 80 pitch (76 high) so that 12 fit. The entrance (NR post-score spec, re/nr_ui_1102/nr-ui-behaviours.md): the head row fades in,
// then the rows reveal top to bottom from 250 ms after the page opened, 120 ms apart (alpha + a 24 unit slide from the left, 180 ms), each row's numbers counting up from 0 over 450 ms (PARTS 700 ms);
// the count is written only when its integer moves, at most every 50 ms, and a number is never blanked. A data refresh only writes what changed (ui_text dedups by content).
#define C13_XP_X0 300.0f
#define C13_XP_SEC_Y 616
#define C13_XP_HD_GAP 104       // section row top -> its first player row (the section row is 96 high + 8)
#define C13_XP_SEC_GAP 28
static const char *const C13_XP_HEX[C13_NAW] =
    { "F08A33", "E0B04A", "7FA8F5", "55C6E8", "5FD068", "A9AFB8" };
static int Cl13XsNew(void)
{                               // the US / THE ENEMY list unless t2r-clan-xs-old.txt exists (checked every 2 s)
    static int off = -1;
    static uint64_t chk;
    uint64_t now = GetTickCount64();
    if (off < 0 || now - chk > 2000) {
        off = 0;
        chk = now;
    }                           // (the old flat list's widgets are gone - clan13.widget hit gui2's page limit - so no opt-out)
    return !off;
}

static struct {
    uint64_t t0;
    uint32_t sig;
    int snd, endsnd, fin, n, cnt[2], pitch, rh, top[C13_NAW],
        tot[2][C13_NAW], win, mine;
    int si[C13_NX], ssec[C13_NX], sfirst[2];
    float ry[C13_NX], secy[2];
} XP;
static const int XP_COLSTAT[5] = { 0, 1, 2, 3, 5 };     // the five stat columns -> stat index (DOWNS EXECUTIONS REVIVES HEADSHOTS DEATHS); PARTS = stat 4

static void Cl13XpReset(void)
{
    memset(&XP, 0, sizeof XP);
}

static void Cl13XpThou(char *o, size_t n, int v)
{
    if (v >= 1000)
        snprintf(o, n, "%d %03d", v / 1000, v % 1000);
    else
        snprintf(o, n, "%d", v);
}

// one number cell: <= 0 / unreported = a dim "-"; the leader of the stat gets its colour once the count is over; `left` dims the player
static void Cl13XpCell(char *o, size_t n, int v, int stat, int leader,
                         int fin, int left)
{
    if (v <= 0) {
        snprintf(o, n, "<font color=\"#6A6760\">0</font>");
        return;
    }                           // a zero is "0", never "-"
    char d[24];
    Cl13XpThou(d, sizeof d, v);
    if (left)
        snprintf(o, n, "<font color=\"#7A776C\">%s</font>", d);
    else if (leader && fin)
        snprintf(o, n, "<font color=\"#%s\">%s</font>", C13_XP_HEX[stat],
                 d);
    else
        snprintf(o, n, "%s", d);
}

// the static part of a row slot (what does not move with the clock): bars, wash, rank badge, name, tag, award
static void Cl13XpSlotStatic(const Cl13T *V, int k,
                                const Cl13AwT *aw)
{
    char g[64], p[96], b[200], t1[96], t2[96];
    const Cl13XT *x = &V->x[XP.si[k]];
    snprintf(g, sizeof g, "anim-group.mst2.xpRow%d", k);
    int compact = XP.rh < 100;
    float by = compact ? 14.0f : 0.0f, sy =
        compact ? (float) XP.rh / 104.0f : 1.0f;
    snprintf(p, sizeof p, "%s.xpMe%d", g, k);
    UiVisible(p, x->me); {
        uint64_t w = UiW(p);
        if (w) {
            WPivot(w, 0.0f, by);
            WScale(w, 1.0f, sy);
        }
    }
    int bar = x->me ? 2 : XP.ssec[k] ? 1 : 0;   // white (US) / red (the enemy) / cream (the own row)
    static const char *const BD[3] = { "xpBdW", "xpBdR", "xpBdM" };
    for (int q = 0; q < 3; q++) {
        snprintf(p, sizeof p, "%s.%s%d", g, BD[q], k);
        UiVisible(p, q == bar);
        uint64_t w = UiW(p);
        if (w) {
            WPivot(w, 0.0f, by);
            WScale(w, 1.0f, sy);
        }
    }
    {
        char ri[120], rn[120];
        snprintf(ri, sizeof ri, "%s.xpRk%d", g, k);
        snprintf(rn, sizeof rn, "%s.xpRn%d", g, k);
        RankUi(ri, rn, x->rk, x->wk);
    }
    snprintf(p, sizeof p, "%s.xpNm%d", g, k);
    Cl13Fit(p, x->name, 740.0f - (x->me || x->left ? 150.0f : 0.0f),
             40.0f);
    if (x->left) {
        snprintf(b, sizeof b, "<font color=\"#7A776C\">%s</font>",
                 x->name);
        UiText(p, b);
    } else
        UiText(p, x->name);
    snprintf(p, sizeof p, "%s.xpTg%d", g, k);
    if (x->me || x->left) {
        float w = Cl13Est(x->name, 40.0f) * 1.18f, wf =
            25.0f * (float) strlen(x->name);
        if (w < wf)
            w = wf;
        if (w > 740.0f - 150.0f)
            w = 740.0f - 150.0f;        // (measured: the game's face runs ~27 units per character at 40 pt; the floor keeps a clear gap)
        Cl13Tr(x->me ? "YOU" : "LEFT", t1, sizeof t1);
        snprintf(b, sizeof b, "<font color=\"#%s\">%s</font>",
                 x->me ? "3FA9F5" : "8D897B", t1);
        UiText(p, b);
        uint64_t tw = UiW(p);
        if (tw)
            WPivot(tw, 206.0f + w + 40.0f, 52.0f);
    } else
        UiText(p, "");
    const Cl13AwT *a = &aw[XP.si[k]];
    snprintf(p, sizeof p, "%s.xpAw%d", g, k);
    if (a->stat >= 0 && a->lead && !x->left) {
        Cl13Tr("MOST", t1, sizeof t1);
        Cl13Tr(Cl13AwWord(a->stat), t2, sizeof t2);
        snprintf(b, sizeof b, "<font color=\"#%s\">%s %s</font>",
                 C13_XP_HEX[a->stat], t1, t2);
        UiText(p, b);
    } else
        UiText(p, "");
    snprintf(p, sizeof p, "%s.xpPm%d", g, k);
    UiVisible(p, x->me);
    snprintf(p, sizeof p, "%s.xpP%d", g, k);
    UiVisible(p, !x->me);
}

// the numbers of one slot at count fraction f / fp (PARTS), fin = the final strings (leader colours)
static void Cl13XpSlotNums(const Cl13T *V, int k, float f, float fp,
                              int fin)
{
    char g[64], p[96], b[100];
    const Cl13XT *x = &V->x[XP.si[k]];
    snprintf(g, sizeof g, "anim-group.mst2.xpRow%d", k);
    for (int c = 0; c < 5; c++) {
        int s = XP_COLSTAT[c], v = Cl13Xv(x, s), shown =
            fin ? v : (int) ((float) v * f + 0.5f);
        Cl13XpCell(b, sizeof b, shown, s, s != 5 && v > 0
                     && v == XP.top[s], fin, x->left);
        snprintf(p, sizeof p, "%s.xpV%d_%d", g, c, k);
        UiText(p, b);
    }
    int pv = x->parts, ps =
        fin ? pv : (int) ((float) pv * fp + 0.5f), lead = pv > 0
        && pv == XP.top[4];
    if (ps <= 0)
        snprintf(b, sizeof b, "<font color=\"#6A6760\">0</font>");
    else {
        char d[24];
        Cl13XpThou(d, sizeof d, ps);
        if (x->left)
            snprintf(b, sizeof b, "<font color=\"#7A776C\">%s</font>", d);
        else if ((x->me || lead) && fin)
            snprintf(b, sizeof b, "<font color=\"#5FD068\">%s</font>", d);
        else
            snprintf(b, sizeof b, "%s", d);
    }
    snprintf(p, sizeof p, "%s.%s%d", g, x->me ? "xpPm" : "xpP", k);
    UiText(p, b);
}

static void Cl13XpSecNums(int sec, float f, float fp, int fin)
{
    char p[96], b[40];
    for (int c = 0; c < 5; c++) {
        int v = XP.tot[sec][XP_COLSTAT[c]], shown =
            fin ? v : (int) ((float) v * f + 0.5f);
        if (shown > 0)
            Cl13XpThou(b, sizeof b, shown);
        else
            snprintf(b, sizeof b, "0");
        snprintf(p, sizeof p, "anim-group.mst2.xpSec%d.xpT%d_%d", sec, c,
                 sec);
        UiText(p, b);
    }
    int v = XP.tot[sec][4], shown =
        fin ? v : (int) ((float) v * fp + 0.5f);
    if (shown > 0)
        Cl13XpThou(b, sizeof b, shown);
    else
        snprintf(b, sizeof b, "0");
    snprintf(p, sizeof p, "anim-group.mst2.xpSec%d.xpTP%d", sec, sec);
    UiText(p, b);
}

static float Cl13XpQ(float a)
{
    int q = (int) (a * 24.0f + 0.5f);
    return (float) q / 24.0f;
}

static int Cl13XpBusy(void)
{
    return XP.sig && !XP.fin;
}                               // the entrance / the count is running: the page's tick fills every frame

static void Cl13FillXp(const Cl13T *V, int on)
{
    if (!g_c13x.seen)
        return;                 // (only the stats page itself fills it: the clan page under it has none of these widgets, and its clock must not start early)
    UiVisible("anim-group.mst2", on);
    if (!on)
        return;
    char b[260], t1[96], p[220];
    int n = C13.xs_got && V->xday == C13.xs_day ? V->xn : 0, mine = -1;
    for (int i = 0; i < n; i++)
        if (V->x[i].me)
            mine = V->x[i].team;
    snprintf(b, sizeof b, "DAY %d",
             C13.xs_post ? (C13.xs_day - 1 < 0 ? 0 : C13.xs_day - 1 >
                            83 ? 83 : C13.xs_day - 1) % 7 +
             1 : Cl13Dow(C13.xs_day));
    Cl13Tr(b, t1, sizeof t1);
    snprintf(b, sizeof b, "%s   -   %s", t1, C13.xs_map);
    UiText("anim-group.mst2.xpSub", b);
    const char *em =
        !C13.
        xs_got ? "LOADING..." : n ? "" :
        "NO PLAYER STATS WERE RECORDED FOR THIS MATCH.\nONLY MATCHES PLAYED AFTER THE LATEST SERVER UPDATE ARE LISTED.";
    UiText("anim-group.mst2.xpEmpty", em);
    UiVisible("anim-group.mst2.xpEmpty", em[0] != 0);
    UiVisible("anim-group.mst2.xpHead", n > 0);
    if (n <= 0) {               // nothing to show (yet): the table is empty
        UiVisible("anim-group.mst2.xpSec0", 0);
        UiVisible("anim-group.mst2.xpSec1", 0);
        for (int k = 0; k < C13_NX; k++) {
            snprintf(p, sizeof p, "anim-group.mst2.xpRow%d", k);
            UiVisible(p, 0);
        }
        XP.sig = 0;
        XP.n = 0;
        return;
    }
    uint64_t now = GetTickCount64();
    uint32_t sig = 2166136261u;
    for (size_t i = 0; i < (size_t) n * sizeof(Cl13XT); i++)
        sig = (sig ^ ((const uint8_t *) V->x)[i]) * 16777619u;
    sig = (sig ^ (uint32_t) (mine + 3)) * 16777619u;
    sig = (sig ^ (uint32_t) (C13.xs_win + 3)) * 16777619u;
    sig = (sig ^ (uint32_t) n) * 16777619u;
    if (sig == XP.sig && XP.fin)
        return;                 // nothing moved, nothing counting: no writes at all
    if (!XP.t0)
        XP.t0 = now;
    int anim = !Cl13AnimOff();
    if (sig != XP.sig) {        // a new (or refreshed) data set: order, place and write the static parts
        Cl13AwT aw[C13_NX];
        memset(aw, 0, sizeof aw);
        Cl13Awards(V->x, n, aw);
        memset(XP.top, 0, sizeof XP.top);
        memset(XP.tot, 0, sizeof XP.tot);
        for (int i = 0; i < n; i++)
            for (int s = 0; s < C13_NAW; s++) {
                int v = Cl13Xv(&V->x[i], s);
                if (v > XP.top[s])
                    XP.top[s] = v;
                if (v > 0)
                    XP.tot[Cl13Sect(&V->x[i], mine)][s] += v;
            }
        int idx[2][C13_NX], k = 0;
        XP.cnt[0] = Cl13Order(V->x, n, mine, 0, idx[0]);
        XP.cnt[1] = Cl13Order(V->x, n, mine, 1, idx[1]);
        XP.n = XP.cnt[0] + XP.cnt[1];
        XP.win = C13.xs_win;
        XP.mine = mine;
        XP.pitch = XP.n <= 8 ? 112 : 80;
        XP.rh = XP.n <= 8 ? 104 : 76;
        int y = C13_XP_SEC_Y;
        for (int sec = 0; sec < 2; sec++) {
            XP.sfirst[sec] = k;
            XP.secy[sec] = (float) y;
            if (!XP.cnt[sec])
                continue;
            int r0 = y + C13_XP_HD_GAP;
            for (int r = 0; r < XP.cnt[sec]; r++, k++) {
                XP.si[k] = idx[sec][r];
                XP.ssec[k] = sec;
                XP.ry[k] =
                    (float) (r0 + r * XP.pitch - (XP.rh < 100 ? 14 : 0));
            }
            y = r0 + XP.cnt[sec] * XP.pitch + C13_XP_SEC_GAP;
        }
        for (int sec = 0; sec < 2; sec++) {
            char g[64], hd[96];
            snprintf(g, sizeof g, "anim-group.mst2.xpSec%d", sec);
            UiVisible(g, XP.cnt[sec] > 0);
            uint64_t gw = UiW(g);
            if (gw)
                WPivot(gw, 0.0f, XP.secy[sec] - (float) C13_XP_SEC_Y);
            Cl13Tr(mine <
                    0 ? (sec ? "TEAM 2" : "TEAM 1") : sec ? "THE ENEMY" :
                    "US", hd, sizeof hd);
            snprintf(p, sizeof p, "%s.xpHd%d", g, sec);
            UiText(p, hd);
            const char *tg = "";
            if (mine >= 0 && (C13.xs_win == 0 || C13.xs_win == 1))
                tg = (C13.xs_win == 1) == (sec ==
                                           0) ?
                    "<font color=\"#7CB494\">MATCH WON</font>" :
                    "<font color=\"#FD6D51\">MATCH LOST</font>";
            snprintf(p, sizeof p, "%s.xpTag%d", g, sec);
            UiText(p, tg);
            float hw_ = Cl13Est(hd, 66.0f) * 1.15f, hf_ =
                50.0f * (float) strlen(hd);
            if (hw_ < hf_)
                hw_ = hf_;      // (measured: THE ENEMY at 66 pt is ~480 units = 53 per character; the floor keeps MATCH LOST clear of it)
            uint64_t tw = UiW(p);
            if (tw)
                WPivot(tw, C13_XP_X0 + 4.0f + hw_ + 44.0f, 32.0f + (float) C13_XP_SEC_Y);      // (the group carries the y offset)
        }
        for (int q = 0; q < C13_NX; q++) {
            snprintf(p, sizeof p, "anim-group.mst2.xpRow%d", q);
            if (q < XP.n) {
                Cl13XpSlotStatic(V, q, aw);
                uint64_t rw = UiW(p);
                if (rw && !XP.fin && anim) {
                    WAlpha(rw, 0.0f);
                    WPivot(rw, C13_XP_X0 - 24.0f, XP.ry[q]);
                }
                UiVisible(p, 1);
            } else
                UiVisible(p, 0);
        }
        XP.sig = sig;
    }
    // ---- the clock: the head row at once, slot k from 250 + 120 k ms
    float el0 = (float) (now - XP.t0);
    float last =
        250.0f + 120.0f * (float) (XP.n > 0 ? XP.n - 1 : 0) + 800.0f;
    int fin = !anim || el0 >= last;
    uint64_t hw = UiW("anim-group.mst2.xpHead");
    if (hw)
        WAlpha(hw, fin ? 1.0f : Cl13XpQ(Cl13Eo(el0 / 220.0f)));
    float qel = (float) (((now - XP.t0) / 50) * 50);    // the count moves in 50 ms steps (a gui2 text written every frame misses draws)
    for (int sec = 0; sec < 2; sec++) {
        if (!XP.cnt[sec])
            continue;
        float tk = 250.0f + 120.0f * (float) XP.sfirst[sec], e =
            fin ? 1.0f : Cl13Eo((el0 - tk) / 180.0f);
        char g[64];
        snprintf(g, sizeof g, "anim-group.mst2.xpSec%d", sec);
        uint64_t gw = UiW(g);
        if (gw)
            WAlpha(gw, fin ? 1.0f : Cl13XpQ(e));
        Cl13XpSecNums(sec, Cl13Eo((qel - tk) / 450.0f),
                         Cl13Eo((qel - tk) / 700.0f), fin);
    }
    for (int k = 0; k < XP.n; k++) {
        float tk = 250.0f + 120.0f * (float) k, e =
            fin ? 1.0f : Cl13Eo((el0 - tk) / 180.0f);
        char g[64];
        snprintf(g, sizeof g, "anim-group.mst2.xpRow%d", k);
        uint64_t rw = UiW(g);
        if (rw) {
            WAlpha(rw, fin ? 1.0f : Cl13XpQ(e));
            WPivot(rw, C13_XP_X0 - 24.0f * (1.0f - e), XP.ry[k]);
        }
        Cl13XpSlotNums(V, k, Cl13Eo((qel - tk) / 450.0f),
                          Cl13Eo((qel - tk) / 700.0f), fin);
    }
    if (g_c13x.post && anim && !XP.snd) {
        XP.snd = 1;
        PmsCountStart((int) (last - 250.0f));
    }                           // post-match: No Return's rolling count while the numbers run
    if (fin && !XP.fin && g_c13x.post && anim) {
        PmsCountCut(60);
        PmsPlay(PMSK_END);
    }                           // ... and its accent when they land
    if (fin)
        XP.fin = 1;
}

static void Cl13FillXs(const Cl13T *V, int show)
{
    int act = show && C13.tab == T_RD && V->nm > 0 && !C13.xs_open;
    UiVisible("anim-group.pActIc", act);
    UiVisible("anim-group.pActTx", act);
    UiVisible("anim-group.rds.rdBtn", act);    // the button in the RAIDS header: the same list, in plain sight
    if (act) {
        UiText("anim-group.pActTx", "PLAYER STATS");
        UiText("anim-group.rds.rdBtn.rdBtnTx", "PLAYER STATS");
    }
    int on = show && C13.xs_open, nw = Cl13XsNew();
    Cl13FillXsOld(V, on && !nw);
    Cl13FillXp(V, on && nw);
}

static void Cl13XsOpen(const char *how)
{
    const Cl13T *V = C13.V;
    int f = C13.foc[T_RD];
    if (C13.xs_open || !V || f < 0 || f >= V->nm)
        return;
    C13.xs_open = 1;
    C13.xs_day = V->m[f].day;
    C13.xs_win = V->m[f].win;
    C13.xs_got = C13.xs_wait = 0;
    C13.xs_tries = 0;
    C13.xs_next = 0;
    snprintf(C13.xs_map, sizeof C13.xs_map, "%s", V->m[f].map);
    Cl13XpReset();
    g_cl13View.xday = 0;
    g_cl13View.xn = 0;
    g_cl13View.have[5] = 0;    // (the snapshot may still hold another raid's list)
    C13.dirty = 1;
    Cl13Sfx();
    g_c13x.on = 1;
    g_c13x.seen = 0;
    g_c13x.f0 = g_frame;
    g_c13x.h_act = g_c13x.h_f = g_c13x.h_esc = 1;       // (keys held from the press that opened it do not close it)
    PushPage("to1/clan13x");
    Xlog("clan13: match stats opened for the raid of day %d (%s) - pushing to1/clan13x", C13.xs_day, how);
}

static void Cl13XsClose(const char *how)
{
    g_c13x.on = 0;
    if (!C13.xs_open)
        return;
    C13.xs_open = 0;
    C13.xs_wait = 0;
    C13.dirty = 1;
    Cl13Sfx();
    Xlog("clan13: match stats closed (%s)", how);
}

// ------------------------------------------------------------------ POST-MATCH SCOREBOARD: the same page as PLAYER STATS, pushed by feature_clan (t2r-ext.c) on the MP menu after a clan
// match, before the supply counter. Data: the day's match stats from the daemon (section 5, asked again until the server has booked the match). It counts up with
// No Return's rolling count sound; confirm / Enter / pad A / click CONTINUE: the first press finishes the count, the next one leaves. Back / Esc / F leave at once.
static int Cl13PostOpen(int day, int won, const char *map)
{
    if (!Cl13Enabled() || g_c13x.on)
        return 0;
    uint64_t now = GetTickCount64();
    memset(&C13, 0, sizeof C13);
    Cl13TabLayout();
    C13.V = &g_cl13View;
    C13.enter_ms = now;
    C13.dirty = 1;
    C13.prev_tab = -1;
    C13.fx_first = 1;
    C13.det_key = -1;
    C13.t0 = now;
    C13.t_u = C13.t_d = C13.t_l = C13.t_r = ~0ULL;
    C13.hv_row = C13.hv_tab = -1;
    C13.h_sel = C13.h_u = C13.h_d = C13.h_l = C13.h_r = C13.h_back =
        C13.h_esc = C13.h_pv = C13.h_nx = C13.h_ka = C13.h_kd = C13.h_act =
        C13.h_f = 1;
    Cl13SeenLoad();
    for (int s = 1; s <= 6; s++)
        C13.seen_ver[s] = (int) g_cl13Ver[s];
    memset(&g_cl13View, 0, sizeof g_cl13View);
    Cl13PicReset();
    Cl13XpReset();
    C13.xs_open = 1;
    C13.xs_post = 1;
    C13.xs_t0 = now;
    C13.xs_day = day + 1;       // the server books a match under the clan's day AFTER it (clan_chronicle log d = days done once resolved); day = days done before (t2r-ext g_pm.day) - tools/tests/test_clan_post_day.py
    C13.xs_win = won;
    C13.xs_got = C13.xs_wait = 0;
    C13.xs_tries = 0;
    C13.xs_next = 0;
    snprintf(C13.xs_map, sizeof C13.xs_map, "%s", map
             && map[0] ? map : "MATCH");
    g_c13x.on = 1;
    g_c13x.post = 1;
    g_c13x.seen = 0;
    g_c13x.f0 = g_frame;
    g_c13x.h_act = g_c13x.h_f = g_c13x.h_esc = g_c13x.h_sel = 1;
    PushPage("to1/clan13x");
    Xlog("clan13: post-match scoreboard for day %d (%s, %s) - pushing to1/clan13x", day, C13.xs_map, won == 1 ? "won" : won == 0 ? "lost" : "no result");
    return 1;
}

static int Cl13PostActive(void)
{
    return g_c13x.on && g_c13x.post;
}

static void Cl13PostEnd(const char *how)
{
    if (!g_c13x.post)
        return;
    g_c13x.on = 0;
    g_c13x.post = 0;
    C13.xs_open = 0;
    C13.xs_post = 0;
    C13.xs_wait = 0;
    PmsCountCut(80);
    Xlog("clan13: post-match scoreboard closed (%s)", how);
}

static void Cl13Fill(void)
{
    const Cl13T *V = C13.V;
    Cl13MsRows();
    const char *msg = Cl13Msg(V);
    UiText("anim-group.ldTx", msg);
    UiVisible("anim-group.ldTx", msg[0] != 0);
    int show = !msg[0];
    if (show && !C13.shown_prev && !C13.intro_played)
        C13.fx_pending = 1;     // the content just became visible (data came / first look): its entrance
    C13.shown_prev = show;
    int fading = C13.prev_tab >= 0
        && GetTickCount64() - C13.tab_t0 < C13_XF_MS;
    UiVisible("anim-group.ov", show
               && (C13.tab == T_OV || (fading && C13.prev_tab == T_OV)));
    UiVisible("anim-group.sv", show
               && (C13.tab == T_SV || (fading && C13.prev_tab == T_SV)));
    UiVisible("anim-group.rds", show
               && (C13.tab == T_RD || (fading && C13.prev_tab == T_RD)));
    UiVisible("anim-group.jrn", show
               && (C13.tab == T_JR || (fading && C13.prev_tab == T_JR)));
    UiVisible("anim-group.ms", show
               && (C13.tab == T_MS || (fading && C13.prev_tab == T_MS)));
    static const char *const TP[C13_NTAB] =
        { "anim-group.tabs.tab0", "anim-group.tabs.tab1",
"anim-group.tabs.tab2", "anim-group.tabs.tab3", "anim-group.tabs.tab4" };
    static const char *const TU[C13_NTAB] =
        { "anim-group.tabs.tabU0", "anim-group.tabs.tabU1",
"anim-group.tabs.tabU2", "anim-group.tabs.tabU3", "anim-group.tabs.tabU4" };
    Cl13TabLayout();
    for (int k = 0; k < C13_NTAB; k++) {
        UiVisible(TU[k], k == C13.tab);
        WAlpha(UiW(TP[k]), k == C13.tab
                || C13.hv_tab == k ? 1.0f : 0.58f);
    }
    UiText("anim-group.pBackTx", "BACK");
    UiText("anim-group.pTabTx", "TABS");
    int ms_sel = C13.tab == T_MS && C13.ms_n && !C13.ms_chosen;
    int rd_sel = C13.tab == T_RD && V->nm > 0 && !C13.xs_open;
    (void) rd_sel;              // RAIDS showed "PLAYER STATS" twice (select + the F prompt) and the select label ran into TABS - the F prompt alone says it
    UiText("anim-group.pSelTx", ms_sel ? "SELECT" : C13.tab == T_OV
            && V->nu ? "JOURNAL" : "");
    UiVisible("anim-group.pSelIc", ms_sel || (C13.tab == T_OV && V->nu));
    if (!show) {                // (an empty MISSIONS tab's message also blanked the WEEK / DAY header - keep it when the clan itself is loaded)
        if (V && V->ok && Cl13Have(V, T_OV))
            Cl13Header(V);
        else if (!C13.hd_shown) {
            UiText("anim-group.hd.hdFac", "");
            UiText("anim-group.hd.hdWeek", "");
            UiText("anim-group.hd.hdTxt", "");
        }                       // (never blanked once it showed values)
        Cl13FillXs(V, 0);
        return;
    }
    Cl13Header(V);
    C13.hd_shown = 1;
    if (C13.tab == T_OV)
        Cl13FillOv(V);
    else if (C13.tab == T_SV)
        Cl13FillSv(V);
    else if (C13.tab == T_RD)
        Cl13FillRd(V);
    else if (C13.tab == T_MS)
        Cl13FillMs(V);
    else
        Cl13FillJr(V);
    Cl13FillXs(V, 1);
    if (C13.tab == T_JR && V->jtop > 1) {
        UiVisible("anim-group.pActTx", 1);
        UiText("anim-group.pActTx", "LEFT / RIGHT:  WEEK");
    }                           // (no glyph: the keys are A / D, the arrows, the d-pad and the stick; < > can be clicked)
    if (C13.fx_pending)
        Cl13FxBuild(V);
}

// ------------------------------------------------------------------ the mouse: geometry of the rows (canvas units, = clan13_widgets.py) so the gaps between rows count
static void Cl13Scan(void)
{
    static float lcx = -1.0f, lcy = -1.0f;
    static int lkey = -1;
    static uint64_t lfr;
    float cx, cy;
    if (!UiCursorCanvas(&cx, &cy)) {
        C13.hv_row = C13.hv_tab = -1;
        C13.hv_back = C13.hv_sel = C13.hv_prev = C13.hv_next = C13.hv_act =
            C13.hv_jp = C13.hv_jn = 0;
        lkey = -1;
        return;
    }
    int key = C13.tab | C13.off[C13.tab] << 2;
    if (cx == lcx && cy == lcy && key == lkey && g_frame - lfr < 15)
        return;
    lcx = cx;
    lcy = cy;
    lkey = key;
    lfr = g_frame;
    C13.hv_row = C13.hv_tab = -1;
    if (cy >= 212.0f && cy <= 380.0f)
        for (int k = 0; k < C13_NTAB; k++)
            if (cx >= g_c13Tabx[k] - 54
                && cx <= g_c13Tabx[k] + g_c13_tabw[k] + 54)
                C13.hv_tab = k;
    if (C13.tab == T_OV && cy >= 540.0f && cy < 1140.0f && cx >= 300.0f
        && cx <= 3540.0f) {
        int col = cx > 1880.0f ? 1 : 0, row =
            (int) ((cy - 540.0f) / 300.0f);
        if (row >= 0 && row < 2)
            C13.hv_row = row * 2 + col;
    }
    if (C13.tab == T_SV && cx >= 300.0f && cx <= 3540.0f && cy >= 520.0f
        && cy < 520.0f + 170.0f * 5) {
        int col = (int) ((cx - 300.0f) / 820.0f), row =
            (int) ((cy - 520.0f) / 170.0f);
        if (col >= 0 && col < 4)
            C13.hv_row = row * 4 + col;
    }
    if (C13.tab == T_JR && cx >= 300 && cx <= 1580 && cy >= C13_JD_Y0
        && cy < C13_JD_Y0 + C13_JD_DY * C13_JD_ROWS) {
        int r = (int) ((cy - C13_JD_Y0) / C13_JD_DY);
        if (cy - C13_JD_Y0 - C13_JD_DY * (float) r < C13_JD_H)
            C13.hv_row = r;
    }
    C13.hv_jp = C13.tab == T_JR
        && UiMouseOver("anim-group.jrn.jwPrevHit", 1);
    C13.hv_jn = C13.tab == T_JR
        && UiMouseOver("anim-group.jrn.jwNextHit", 1);
    if (C13.tab == T_MS && cx >= 300 && cx <= 1580 && cy >= 540
        && cy < 540 + 112 * C13_MS_ROWS) {
        int r = (int) ((cy - 540) / 112);
        if (cy - 540 - 112 * r < 100)
            C13.hv_row = r;
    }
    if (C13.tab == T_RD && cx >= 300 && cx <= 3540 && cy >= 565
        && cy < 565 + 110 * C13_RDS_ROWS)
        C13.hv_row = (int) ((cy - 565) / 110);
    C13.hv_back = UiMouseOver("anim-group.pBackHit", 1);
    C13.hv_sel = (C13.tab == T_OV
                  || (C13.tab == T_RD && !C13.xs_open && C13.V
                      && C13.V->nm > 0))
        && UiMouseOver("anim-group.pSelHit", 1);
    C13.hv_prev = UiMouseOver("anim-group.pPrevHit", 1);
    C13.hv_next = UiMouseOver("anim-group.pNextHit", 1);
    C13.hv_act = C13.tab == T_RD && !C13.xs_open && C13.V && C13.V->nm > 0
        && (UiMouseOver("anim-group.pActHit", 1)
            || UiMouseOver("anim-group.rds.rdBtn.rdBtnHit", 1));
}

static int Cl13RowIndex(int hv)
{                               // the list index under the hovered slot (-1: none)
    int base = C13.tab == T_SV || C13.tab == T_RD
        || C13.tab == T_JR ? C13.off[C13.tab] : 0;
    int idx = base + hv, rows = Cl13RowsOf(C13.tab);
    return hv >= 0 && idx < rows ? idx : -1;
}

// the click gate (ui_click_gate_tick): the cursor is over something clickable
static int Cl13ClickHit(void)
{
    if (!C13.up || !C13.V)
        return 0;
    Cl13Scan();
    return Cl13RowIndex(C13.hv_row) >= 0 || C13.hv_tab >= 0
        || C13.hv_back || C13.hv_sel || C13.hv_prev || C13.hv_next
        || C13.hv_act || C13.hv_jp || C13.hv_jn || C13.xs_open;
}
static const char *const C13_GRP[C13_NTAB] =
    { "anim-group.ov", "anim-group.sv", "anim-group.rds", "anim-group.jrn",
"anim-group.ms" };
// A switch can come while the last one's fade-out / entrances still run (Q / E held, bumpers hammered): everything that was left half way is put back to its resting state first
// (full alpha, base position), so a tab that is switched to later never starts from a stale alpha 0 / partial fade (content went blank on fast switching).
static void Cl13Settle(void)
{
    for (int k = 0; k < C13_NTAB; k++) {
        uint64_t g = UiW(C13_GRP[k]);
        if (g)
            WAlpha(g, 1.0f);
    }
    for (int i = 0; i < C13.nfx; i++) {
        C13fxT *f = &C13.fx[i];
        uint64_t w = f->w ? f->w : UiW(f->path);
        if (!w)
            continue;
        WAlpha(w, 1.0f);
        if (f->kind >= 1 && f->kind <= 5)
            WPivot(w, f->bx, f->by);
    }
    const char *dp[3] =
        { "anim-group.sv.svDt", "anim-group.rds.rdDt",
"anim-group.jrn.jtPane" };
    for (int k = 0; k < 3; k++)
        GuGrp(dp[k], 1.0f, 0.0f);
    Cl13FxReset();
}

static void Cl13SetTab(int t, const char *why)
{
    t = (t % C13_NTAB + C13_NTAB) % C13_NTAB;
    if (t == C13.tab)
        return;
    Cl13Settle();
    C13.prev_tab = C13.tab;
    C13.tab = t;
    C13.tab_t0 = GetTickCount64();
    C13.sw++;
    if (C13.intro_played)
        C13.fx_pending = 0;     // (intros play once per opening - a switch is a crossfade of two filled tabs)
    C13.dirty = 1;
    C13.det_key = -1;
    C13.det_ph = 0;
    C13.det_hold = 0;
    Cl13Sfx();
    Xlog("clan13: tab %d (%s)", t, why);
}

static struct {
    uint64_t until;
    int apply, tab, foc[C13_NTAB], off[C13_NTAB];
} g_c13Resume;                 // closing the match stats with menu-back (the pad's B / Circle): the engine pops the page on it, it is pushed again
static void Cl13Enter(void)
{
    memset(&C13, 0, sizeof C13);
    Cl13TabLayout();
    C13.V = &g_cl13View;
    C13.up = 1;
    C13.enter_ms = GetTickCount64();
    C13.dirty = 1;
    C13.prev_tab = -1;
    C13.fx_first = 1;
    C13.det_key = -1;
    C13.h_sel = C13.h_u = C13.h_d = C13.h_l = C13.h_r = C13.h_back = C13.h_esc = C13.h_pv = C13.h_nx = C13.h_ka = C13.h_kd = C13.h_act = C13.h_f = 1;   // whatever is held when the page opens
    C13.t_u = C13.t_d = C13.t_l = C13.t_r = ~0ULL;
    C13.hv_row = C13.hv_tab = -1;
    C13.t0 = C13.enter_ms;
    Cl13SeenLoad();
    for (int s = 1; s <= 6; s++) {
        C13.seen_ver[s] = (int) g_cl13Ver[s];
    }
    if (!g_c13Resume.apply)
        memset(&g_cl13View, 0, sizeof g_cl13View);    // the snapshot starts empty: LOADING until this visit's replies are here (a stale roster would flash); back from the stats page the last good one stays
    Cl13PicReset();
    GuFadeReset();
    memset(g_gu_fit, 0, sizeof g_gu_fit);
    if (g_c13Resume.apply) {   // the engine popped the page on the menu-back that closed the match stats: back on the same tab and row
        g_c13Resume.apply = 0;
        g_c13Resume.until = 0;
        C13.intro_played = 1;
        C13.fx_first = 0;
        C13.hd_done = 1;
        C13.sw = 1;
        C13.hd_shown = 1;       // (no second intro for the same opening)
        C13.tab = g_c13Resume.tab;
        memcpy(C13.foc, g_c13Resume.foc, sizeof C13.foc);
        memcpy(C13.off, g_c13Resume.off, sizeof C13.off);
    }
    if (Cl13PickDue()) {
        C13.tab = T_MS;
        Xlog("clan13: a mission is to be chosen - opening on MISSIONS");
    }                           // nobody misses the pick (no pick pending: the page opens as usual)
    WAlpha(UiW("anim-group"), 0.0f);  // the page fades in (the end of feature_clan13)
    g_cl13PicLogged = 0;
    C13.ev0 = g_g2Evicts;
    UiVisible("anim-group.pre", 1);    // (the image budget starts over: the strip is back until the table fills up)
    Xlog("clan13: page up");
}

static void Cl13Close(const char *how)
{
    if (C13.closing || C13.out_t0 || C13.quiet)
        return;
    Xlog("clan13: back (%s)", how);
    // NR 1.3 (exit: ghost of the page + empty frames): the engine's own page transition carries the page out, so everything we drive is put back to rest NOW, in the frame of the
    // pop, and from here on nothing of ours writes alpha / position on this page (quiet): the exit starts from a settled page instead of one that is mid-entrance / mid-crossfade.
    cl13_settle();
    WAlpha(UiW("anim-group"), 1.0f);
    C13.quiet = 1;
    C13.back_f = g_frame ? g_frame : 1; // the engine pops a pushed menu page on menu-back by itself; Esc it does not: judged 6 frames after the press
    // Our own 140 ms fade to alpha 0 + pop left ~10 empty frames and then the engine's page-exit transition replayed the clan page ghosted over
    // the main menu coming in. No private fade any more: the page is popped at full alpha (as menu-back does) and the engine's transition carries it out.
    if (strcmp(how, "menu-back") && TopPage() == Cl13PageId()) {
        PopPage();
        C13.closing = 1;
        C13.back_f = 0;
        Cl13SeenSave();
        Xlog("clan13: closed");
    }
}

// detail cross-fade (as GEAR UP's details panel): the old content fades out fast, the refill waits, the new fades in with a small slide
static const char *Cl13DetPath(void)
{
    return C13.tab == T_SV ? "anim-group.sv.svDt" : C13.tab ==
        T_RD ? "anim-group.rds.rdDt" : C13.tab ==
        T_JR ? "anim-group.jrn.jtPane" : NULL;
}

static void Cl13DetTick(uint64_t now)
{
    const char *dp = Cl13DetPath();
    if (!dp || Cl13AnimOff() || C13.fx_pending || now - C13.t0 < 1500) {
        C13.det_key = dp ? C13.tab * 1000 + C13.foc[C13.tab] : -1;
        C13.det_hold = 0;
        C13.det_ph = 0;
        return;
    }
    int key = C13.tab * 1000 + C13.foc[C13.tab];
    if (C13.det_key == -1)
        C13.det_key = key;
    // A new cross-fade only from rest and not within C13_DET_GAP_MS of the last one: a focus step during a running one, or a quick next step, refills the panel in
    // place at full alpha (det_hold 0: the fill writes it) - held W / S used to restart the fade-out on every 110 ms repeat (a large panel strobing ~9 times a second).
    else if (key != C13.det_key
             && (Cl13FlickerOld()? C13.det_ph != 1 : C13.det_ph == 0
                 && (!C13.det_last
                     || now - C13.det_last >= C13_DET_GAP_MS))) {
        C13.det_key = key;
        C13.det_ph = 1;
        C13.det_t0 = now;
        C13.det_hold = 1;
    } else if (key != C13.det_key) {
        C13.det_key = key;
        if (C13.det_ph == 0)
            C13.det_last = now;
    }                           // (a refill in place also restarts the quiet time: no fade while the keys keep coming)
    if (C13.det_ph == 1) {
        float p = (float) (now - C13.det_t0) / 80.0f;
        GuGrp(dp, 1.0f - Cl13Eo(p), 0.0f);
        if (p >= 1.0f) {
            C13.det_ph = 2;
            C13.det_t0 = now;
            C13.det_hold = 0;
            C13.dirty = 1;
        }
    } else if (C13.det_ph == 2) {
        float p = Cl13Eo((float) (now - C13.det_t0) / 150.0f);
        GuGrp(dp, p, (1.0f - p) * 8.0f);
        if (p >= 1.0f) {
            C13.det_ph = 0;
            C13.det_last = now;
            GuGrp(dp, 1.0f, 0.0f);
        }
    }
}

static void Cl13PreloadTick(void)
{                               // the portraits / icons the page can show go through the tiny always-visible pre<n> widgets (the engine streams a texture in when a widget first shows it)
    if (!C13.V)
        return;
    int old = Cl13FlickerOld(), budget = 8;
    if (!old) {                 // the Gui2 image budget (C13_G2_RETIRE): waves while the table has room, then the strip goes away
        if (C13.pre_off)
            return;
        int f = G2Filled();
        if (f < 0)
            old = 1;            // (table not found: as before)
        else if (f >= C13_G2_RETIRE) {
            UiVisible("anim-group.pre", 0);
            C13.pre_off = 1;
            Xlog("clan13: preload strip retired - Gui2 image table %d/32 filled (%d icons, %d portraits preloaded)", f, C13.pre_ick, C13.pre_i);
            return;
        } else {
            if (C13.pre_wave_f && g_frame - C13.pre_wave_f < C13_PRE_GAP_F)
                return;
            if (f > C13_G2_RETIRE - C13_PRE_WAVE)
                return;         // (no room for a whole wave now)
            budget = C13_PRE_WAVE;
        }
    }
    int asked = 0;
    if (!C13.pre_ic) {          // every icon the page can show (pre22..pre31; the page reveals only after C13_GATE_MS): no icon streams in while the page is already visible
        static const char *const IC[10] =
            { "food", "water", "medicine", "ammo", "materials",
"healthbar", "person", "pills", "bandage", "keepsake" };
        if (!UiW("anim-group.pre.pre31"))
            return;
        for (; C13.pre_ick < 10 && (old || asked < budget);
             C13.pre_ick++, asked++) {
            char path[60], img[80];
            snprintf(path, sizeof path, "anim-group.pre.pre%d",
                     22 + C13.pre_ick);
            snprintf(img, sizeof img, "streaming/t2r/icons/c13-%s.png",
                     IC[C13.pre_ick]);
            UiImage(path, img);
        }
        if (C13.pre_ick >= 10)
            C13.pre_ic = 1;
        if (!old) {
            if (asked)
                C13.pre_wave_f = g_frame;
            if (!C13.pre_ic || asked >= budget)
                return;
        }
    }
    if (!Cl13PortraitsOn())
        return;
    const Cl13T *V = C13.V;
    for (int n = 0; n < (old ? 8 : budget - asked); n++) {
        int i = C13.pre_i;
        if (i >= V->np + V->nu || i >= 22)
            break;              // (the counter moves only when there is something to preload: it used to run past the end while the snapshot was still empty, so nothing was ever preloaded)
        C13.pre_i++;
        if (!old)
            C13.pre_wave_f = g_frame;
        const char *pid = i < V->nu ? V->u[i].pid : V->p[i - V->nu].pid;
        char a[16], s[16], m[16], e[20], path[60], img[120];
        if (sscanf(pid, "%15[^/]/%15[^/]/%15[^/]/%19s", a, s, m, e) != 4)
            continue;
        snprintf(path, sizeof path, "anim-group.pre.pre%d", i % 22);
        snprintf(img, sizeof img, "streaming/t2r/icons/c13p-%s_%s.png", m,
                 e);
        UiImage(path, img);
    }
}

// the PLAYER STATS page on top of the clan page: the net (section 5 answer), the fill, and its keys (menu-back pops it by itself; Esc / F / the
// pad's menu-action / a click on BACK pop it here). Back on to1/clan13 the list counts as closed.
static int AnimOnXp(void)
{
    return !Cl13AnimOff();
}

static void Cl13xTick(void)
{
    uint64_t now = GetTickCount64();
    int first = !g_c13x.seen;
    g_c13x.seen = 1;
    Cl13NetTick(now);
    const Cl13T *V = C13.V;
    int post = g_c13x.post;
    if (first && post)
        PmsPlay(PMSK_ENTER);
    if (V && (C13.dirty || g_frame % 6 == 0)) {
        Cl13Header(V);
        Cl13FillXp(V, 1);
        UiText("anim-group.pBackTx", "BACK");
    } else if (V && Cl13XpBusy())
        Cl13FillXp(V, 1);     // the entrance / the counts run every frame
    UiVisible("anim-group.pBackIc", !post);
    UiVisible("anim-group.pBackTx", !post);    // post-match: one prompt, CONTINUE (confirm)
    UiVisible("anim-group.pCtIc", post);
    UiVisible("anim-group.pCtTx", post);
    if (post)
        UiText("anim-group.pCtTx", "CONTINUE");
    int f_now = GameFocused()
        && (GetAsyncKeyState('F') & 0x8000) != 0, esc_now = GameFocused()
        && (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    int act = MenuEdge(FNV64("menu-action"), &g_c13x.h_act) | (f_now
                                                                && !g_c13x.
                                                                h_f), esc =
        esc_now && !g_c13x.h_esc;
    int sel =
        post ? MenuEdge(0x6695c5b7921bb5ddULL /* menu-select */ ,
                         &g_c13x.h_sel) : 0;
    g_c13x.h_f = f_now;
    g_c13x.h_esc = esc_now;
    int clk = g_clickEdge && (UiMouseOver("anim-group.pBackHit", 1)
                               || (post
                                   && UiMouseOver("anim-group.pCtHit",
                                                    1)));
    if (sel && g_frame > g_c13x.f0 + 10 && post && Cl13XpBusy() && AnimOnXp()) {    // the first confirm finishes the count (NR locks input until it is done; ours skips it)
        XP.t0 = now - 100000;
        Cl13Sfx();
        sel = 0;
    } else if (clk && post && Cl13XpBusy() && AnimOnXp()
               && !UiMouseOver("anim-group.pBackHit", 1)) {
        XP.t0 = now - 100000;
        clk = 0;
    }
    if ((act || esc || clk || sel) && g_frame > g_c13x.f0 + 10) {
        Cl13Sfx();
        PopPage();
        Xlog("clan13: match stats page closed (%s)",
             sel ? "confirm" : act ? "F / action" : esc ? "Esc" :
             "BACK / CONTINUE clicked");
        if (post)
            Cl13PostEnd(sel ? "confirm" : act ? "F / action" : esc ?
                          "Esc" : "click");
    }
}

static void FeatureClan13(void)
{
    g_cl13Up = 0;
    if (!Cl13Enabled())
        return;
    uint64_t top = TopPage();
    if (g_c13x.on && g_c13x.post && top != Cl13xPageId()
        && (g_c13x.seen ? g_frame > g_c13x.f0 + 10 : g_frame >
            g_c13x.f0 + 120))
        Cl13PostEnd(g_c13x.
                      seen ? "page popped (menu-back)" :
                      "never reached the top");
    if (g_c13x.on && top == Cl13xPageId() && top) {
        g_cl13Up = 1;
        Cl13xTick();
        return;
    }                           // (the clan page stays "up" under its stats page)
    if (top != Cl13PageId()) {
        if (C13.up) {
            C13.up = 0;
            Cl13SeenSave();
            Xlog("clan13: page left (top now %s)", PageName(top));
            Xlog("clan13: Gui2 image table peak %d/32 (tab %d), %ld evictions this visit, preload strip %s, %d crossfades cut short%s", C13.g2_peak, C13.g2_peak_tab, (long) (g_g2Evicts - C13.ev0), C13.pre_off ? "retired" : "kept", C13.xf_cut, Cl13FlickerOld()? " (t2r-clan13-flicker-old.txt)" : "");
            if (g_c13Resume.until && g_frame <= g_c13Resume.until && top) {
                g_c13Resume.apply = 1;
                PushPageNow("to1/clan13");
                Xlog("clan13: the engine popped the page on the back that closed the match stats - pushed again");
            }
        }
        if (g_c13Resume.until && g_frame > g_c13Resume.until)
            g_c13Resume.until = 0;
        return;
    }
    g_cl13Up = 1;
    if (!C13.up) {
        Cl13Enter();
        if (g_ceJournalTab) {
            g_ceJournalTab = 0;
            C13.tab = T_JR;
            C13.dirty = 1;
            Xlog("clan13: opened on the journal (clan end screen)");
        }
    }
    if (g_c13Test && GetTickCount64() - g_c13TestMs > 1500) {
        if (C13.tab != T_RD) {
            Cl13SetTab(T_RD, "t2r-clan-test.txt");
            C13.foc[T_RD] = 0;
        } else if (g_c13Test == 2 && C13.V && C13.V->nm > 0
                   && GetTickCount64() - g_c13TestMs > 3000) {
            g_c13Test = 0;
            Cl13XsOpen("t2r-clan-test.txt");
        } else if (g_c13Test == 1
                   || GetTickCount64() - g_c13TestMs > 15000)
            g_c13Test = 0;
    }
    if (C13.xs_open && (g_c13x.seen || g_frame > g_c13x.f0 + 90)) {     // back from the stats page (or it never came up): the list is closed
        Cl13XsClose(g_c13x.
                      seen ? "stats page popped" :
                      "stats page never reached the top");
    }
    uint64_t now = GetTickCount64();
    if (C13.closing)
        return;
    Cl13NetTick(now);
    // ---- input
    int sel =
        MenuEdge(0x6695c5b7921bb5ddULL /* menu-select */ , &C13.h_sel);
    int up = GuRep(&C13.h_u, &C13.t_u, FNV64("menu-up")), down =
        GuRep(&C13.h_d, &C13.t_d, FNV64("menu-down"));
    int left = GuRep(&C13.h_l, &C13.t_l, FNV64("menu-left")), right = GuRep(&C13.h_r, &C13.t_r, FNV64("menu-right")); // (the stick / d-pad repeat across a grid row like up / down)
    int back = MenuEdge(FNV64("menu-back"), &C13.h_back);
    int f_now = GameFocused()
        && (GetAsyncKeyState('F') & 0x8000) != 0, act_k = f_now
        && !C13.h_f;
    C13.h_f = f_now;
    int act = MenuEdge(FNV64("menu-action"), &C13.h_act) || act_k;     // F / the pad's menu-action (Triangle / Y): the match stats of the focused raid
    int esc_now = GameFocused()
        && (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0, esc = esc_now
        && !C13.h_esc;
    C13.h_esc = esc_now;
    int wn = WheelNotches();
    {
        static int lpd = -1;
        if (lpd != g_padMode) {
            lpd = g_padMode;
            C13.dirty = 1;
        }
    }                           // the hint lines follow the device in use
    Cl13Scan();
    int click = g_clickEdge;
    if (C13.xs_open) {          // the match stats list is up: it owns the keys; F again / BACK / Esc / B closes it (the page stays)
        if (back || esc || act || (click && C13.hv_back)) {
            if (back) {         // (the engine pops the page on menu-back by itself: pushed again at once, same tab and row)
                g_c13Resume.until = g_frame + 10;
                g_c13Resume.tab = C13.tab;
                memcpy(g_c13Resume.foc, C13.foc, sizeof C13.foc);
                memcpy(g_c13Resume.off, C13.off, sizeof C13.off);
            }
            Cl13XsClose(act ? "action" : esc ? "Esc" : back ? "menu-back"
                          : "BACK clicked");
            back = esc = 0;
            click = 0;
        }
        act = 0;
        up = down = left = right = 0;
        sel = 0;
        click = 0;
        wn = 0;
    } else if (act && C13.tab == T_RD && C13.V && C13.V->nm > 0
               && now - C13.enter_ms >= 500 && !C13.out_t0) {
        Cl13XsOpen(act_k ? "F" : "menu-action");
        act = 0;
    } else if (click && C13.hv_act) {
        Cl13XsOpen("clicked");
        click = 0;
        sel = 0;
    }
    if (back || esc || (click && C13.hv_back)) {
        Cl13Close(esc ? "Esc" : click
                   && C13.hv_back ? "BACK clicked" : "menu-back");
        back = esc = 0;
        sel = 0;
        click = 0;
    }
    if (C13.back_f && !C13.out_t0 && g_frame >= C13.back_f + 6) {
        C13.back_f = 0;
        if (TopPage() == Cl13PageId() && !C13.closing) {
            PopPage();
            C13.closing = 1;
            Cl13SeenSave();
            Xlog("clan13: closed");
            return;
        }
    }
    if (C13.quiet)
        return;                 // leaving: the engine transition owns the page (no fade-in, crossfade, entrance or preload writes any more)
    int rows = Cl13RowsOf(C13.tab), t = C13.tab, foc = C13.foc[t];
    int grid = t == T_OV ? 2 : t == T_SV ? 4 : 1;       // items per row of the tab's list
    if (t == T_JR && C13.V && !C13.xs_open) {
        int wk = Cl13Jw(C13.V), chg = 0;
        if (left)
            chg = Cl13JwSet(wk - 1, 1, "left");
        else if (right)
            chg = Cl13JwSet(wk + 1, 0, "right");
        else if (up && rows && foc == 0) {
            chg = Cl13JwSet(wk - 1, 1, "up past the first day");
            up = 0;
        } else if (down && rows && foc == rows - 1) {
            chg = Cl13JwSet(wk + 1, 0, "down past the last day");
            down = 0;
        } else if (wn > 0 && rows && foc == 0) {
            chg = Cl13JwSet(wk - 1, 1, "wheel");
            wn = 0;
        } else if (wn < 0 && rows && foc == rows - 1) {
            chg = Cl13JwSet(wk + 1, 0, "wheel");
            wn = 0;
        }
        if (chg) {
            foc = C13.foc[T_JR];
            rows = Cl13RowsOf(T_JR);
            up = down = 0;
            wn = 0;
        }
        left = right = 0;
    }
    if (up && rows) {
        foc =
            grid > 1 ? (foc - grid >=
                        0 ? foc - grid : foc) : (foc + rows - 1) % rows;
        Cl13Sfx();
    }
    if (down && rows) {
        foc =
            grid > 1 ? (foc + grid <
                        rows ? foc + grid : foc) : (foc + 1) % rows;
        Cl13Sfx();
    }
    if (wn && rows) {
        foc -= wn * (t == T_SV ? 4 : 1);
        if (foc < 0)
            foc = 0;
        if (foc >= rows)
            foc = rows - 1;
        Cl13Sfx();
    }
    // TABS: Q / E (keyboard keys) and the bumpers LB / RB (the engine's menu-prev / menu-next, the glyphs in the footer). A / D / W / S, the arrow keys and the pad's stick / d-pad only
    // MOVE the selection (menu-left / menu-right: across a grid row; menu-up / menu-down: by row) and never change the tab. Q + a bumper in one frame = ONE step.
    int kq = GameFocused()
        && (GetAsyncKeyState('Q') & 0x8000) != 0, ke = GameFocused()
        && (GetAsyncKeyState('E') & 0x8000) != 0;
    int tab_l = kq && !C13.h_ka, tab_r = ke && !C13.h_kd;
    C13.h_ka = kq;
    C13.h_kd = ke;
    int bp = MenuEdge(FNV64("menu-prev"), &C13.h_pv), bn =
        MenuEdge(FNV64("menu-next"), &C13.h_nx);
    if (C13.xs_open)
        tab_l = tab_r = bp = bn = 0;
    if (left && grid > 1 && rows && foc % grid > 0) {
        foc--;
        Cl13Sfx();
    }
    if (right && grid > 1 && rows && foc % grid < grid - 1
        && foc + 1 < rows) {
        foc++;
        Cl13Sfx();
    }
    if (tab_l || bp)
        Cl13SetTab(C13.tab - 1, bp ? "left bumper" : "Q");
    if (tab_r || bn)
        Cl13SetTab(C13.tab + 1, bn ? "right bumper" : "E");
    if (click) {
        int ri = Cl13RowIndex(C13.hv_row);
        if (C13.hv_tab >= 0) {
            Cl13SetTab(C13.hv_tab, "click");
            C13.click_f = g_frame;
            sel = 0;
        } else if (C13.hv_prev) {
            Cl13SetTab(C13.tab - 1, "previous tab clicked");
            C13.click_f = g_frame;
            sel = 0;
        } else if (C13.hv_next) {
            Cl13SetTab(C13.tab + 1, "next tab clicked");
            C13.click_f = g_frame;
            sel = 0;
        } else if (C13.hv_jp || C13.hv_jn) {
            if (Cl13JwSet
                (Cl13Jw(C13.V) + (C13.hv_jp ? -1 : 1), C13.hv_jp,
                 C13.hv_jp ? "< clicked" : "> clicked")) {
                foc = C13.foc[T_JR];
                rows = Cl13RowsOf(T_JR);
            }
            C13.click_f = g_frame;
            sel = 0;
        } else if (C13.hv_sel) {
            if (t == T_RD)
                Cl13XsOpen("SELECT clicked");
            else
                Cl13SetTab(T_JR, "JOURNAL clicked");
            C13.click_f = g_frame;
            sel = 0;
        } else if (ri >= 0) {
            if (ri == foc && t == T_OV)
                Cl13SetTab(T_JR, "card clicked again");
            foc = ri;
            C13.click_f = g_frame;
            sel = 0;
            Cl13Sfx();
            Xlog("clan13: click on row %d (tab %d)", foc, t);
        }
    }
    if (sel && C13.click_f && g_frame - C13.click_f <= 8)
        sel = 0;                // the click already acted
    if (g_testAutofind && C13.tab == T_MS && C13.ms_n && !C13.ms_chosen
        && !C13.ms_auto && now - C13.enter_ms >= 3000) {
        C13.ms_auto = 1;
        sel = 1;
        Xlog("test autofind: mission picked");
    }                           // A/B test aid: the first mission 3 s after the page opened on MISSIONS (the old picker's auto-pick never ran: the clan page owns the pick)
    if (sel && C13.tab == T_OV && C13.V->nu)
        Cl13SetTab(T_JR, "select");
    if (sel && C13.tab == T_RD && C13.V->nm > 0 && !C13.xs_open
        && now - C13.enter_ms >= 500)
        Cl13XsOpen("select");
    if ((sel || (click && C13.hv_row >= 0 && C13.hv_row == C13.foc[T_MS] && foc == C13.foc[T_MS])) && C13.tab == T_MS && C13.ms_n && !C13.ms_chosen && !g_clan_busy) {  // SELECT (or a second click) on the focused mission: the same op 0xEF as ever
        uint8_t id = (uint8_t) C13.ms_rows[C13.foc[T_MS]];
        if (ClanSend(0xEF, &id, 1)) {
            C13.ms_chosen = 1;
            C13.ms_chosen_at = now;
            C13.ms_tries = 1;
            C13.dirty = 1;
            UiSfx("sfx-select");
            Xlog("clan13: missions: chose %s", MIS[id].name);
        }
    }
    if (C13.ms_chosen) {        // the choice waits for the server's reply (m.mi): asked again after 6 s, given up after 12 s (as the old page)
        MetaT mm = MetaGet();
        if (mm.mi) {            // (the page stays on MISSIONS and turns into the running mission's status page)
            int stay = !g_testAutofind
                && GetFileAttributesA("t2r-clan-ms-old.txt") ==
                INVALID_FILE_ATTRIBUTES;
            Xlog("clan13: missions: %s running - %s", MIS[mm.mi].name,
                 stay ? "the tab shows its status" : "closing");
            C13.ms_chosen = 0;
            C13.dirty = 1;
            if (!stay)
                Cl13Close("mission chosen");
        } else if (!g_clan_busy && now - C13.ms_chosen_at >= 6000) {
            if (C13.ms_tries < 2) {
                uint8_t id = (uint8_t) C13.ms_rows[C13.foc[T_MS]];
                if (ClanSend(0xEF, &id, 1)) {
                    C13.ms_tries++;
                    C13.ms_chosen_at = now;
                    Xlog("clan13: missions: no answer for 6 s - asking again");
                }
            } else {
                Xlog("clan13: missions: the server never answered the choice - back to the page");
                C13.ms_chosen = 0;
                C13.dirty = 1;
            }
        }
    }
    if (t == C13.tab && foc != C13.foc[t]) {
        C13.foc[t] = foc;
        C13.dirty = 1;
    }
    // ---- fill: on a change, twice a second, and at the page's first moments (its widgets may be built a frame late)
    static const uint32_t OFF_MS[] = { 33, 100, 250, 670, 1500, 3000 };
    if (C13.stage < (int) (sizeof OFF_MS / sizeof OFF_MS[0])
        && now - C13.enter_ms >= OFF_MS[C13.stage]) {
        C13.stage++;
        C13.dirty = 1;
        GuFadeReassert();
    }
    {                           // the hover highlight follows the cursor
        static int lh = -9, lt = -9, lb;
        int hb =
            C13.tab | C13.hv_back << 4 | C13.hv_jp << 5 | C13.hv_jn << 6;
        if (lh != C13.hv_row || lt != C13.hv_tab || lb != hb) {
            lh = C13.hv_row;
            lt = C13.hv_tab;
            lb = hb;
            C13.dirty = 1;
        }
    }
    {                           // the Gui2 image budget: the visit's peak (logged when the page is left) and a crossfade that would draw more images than the engine keeps ends at once
        int g2 = G2Filled();
        if (g2 > C13.g2_peak) {
            C13.g2_peak = g2;
            C13.g2_peak_tab = C13.tab;
        }
        if (g2 >= C13_G2_XFCUT && C13.prev_tab >= 0
            && now - C13.tab_t0 < C13_XF_MS && !Cl13FlickerOld()) {
            C13.tab_t0 = now - C13_XF_MS;
            C13.xf_cut++;
            static int nl;
            if (LOGCAP(nl, 20))
                Xlog("clan13: crossfade %d -> %d cut short - Gui2 image table %d/32 filled", C13.prev_tab, C13.tab, g2);
        }
    }
    if (C13.prev_tab >= 0 && now - C13.tab_t0 >= C13_XF_MS) {   // the crossfade is over: the old tab is hidden (by the fill), the new one rests at full alpha
        uint64_t og = UiW(C13_GRP[C13.prev_tab]), ng =
            UiW(C13_GRP[C13.tab]);
        if (og)
            WAlpha(og, 1.0f);
        if (ng)
            WAlpha(ng, 1.0f);
        C13.prev_tab = -1;
        C13.dirty = 1;
    }
    Cl13DetTick(now);
    if (C13.dirty || !(g_frame % 90)) {
        C13.dirty = 0;
        Cl13Fill();
    }
    if (C13.prev_tab >= 0) {    // the tab we leave fades out
        const char *const *GP = C13_GRP;
        float p = (float) (now - C13.tab_t0) / (float) C13_XF_MS;
        if (!Cl13AnimOff()) {
            WAlpha(UiW(GP[C13.prev_tab]),
                    p >= 1.0f ? 1.0f : 1.0f - Cl13Eo(p));
            WAlpha(UiW(GP[C13.tab]), p >= 1.0f ? 1.0f : Cl13Eo(p));
        }
    }
    GuFadeTick(now);
    Cl13FxTick(now);
    Cl13PreloadTick();
    Cl13PicTick(now);
    {                           // page fade-in: 0 -> 1 over 220 ms (after the art gate), in 1/24 steps
        static int last_q = -1;
        static uint64_t last_enter;
        if (C13.enter_ms != last_enter) {
            last_enter = C13.enter_ms;
            last_q = -1;
        }
        float f =
            now <
            C13.enter_ms + C13_GATE_MS ? 0.0f : (float) (now -
                                                         C13.enter_ms -
                                                         C13_GATE_MS) /
            220.0f;
        if (f > 1.0f)
            f = 1.0f;
        f = 1.0f - (1.0f - f) * (1.0f - f);
        int q = (int) (f * 24.0f + 0.5f);
        if (q != last_q) {
            last_q = q;
            WAlpha(UiW("anim-group"), (float) q / 24.0f);
        }
    }
}

// ------------------------------------------------------------------ the way in: C or the pad's menu-action on the MP main menu with a living clan (called from feature_clan)
static void Cl13OpenCheck(void)
{
    static int h_act, armed;
    // the way in is the CLAN row of the MP menu list (modepick_widgets.add_clan_row, same native button as FIND MATCH): select on it (Enter / pad A) or a click.
    // The F key / the pad's menu-action and the "OPEN THE CLAN" hint are gone. Keyboard, pad and mouse all work through the menu row.
    int pad = 0;
    if (!Cl13Enabled())
        UiVisible("anim-group.menuList.Clan", 0);
    else {
        int sel =
            MenuEdge(0x6695c5b7921bb5ddULL /* menu-select */ , &h_act);
        int clk = g_clickEdge
            && UiMouseOver("anim-group.menuList.Clan", 1);
        if (!armed) {
            armed = 1;
            sel = 0;
        }                       // a button held on arrival does not open it
        if (g_mdpk.open || g_mdpk.chosen) {
            sel = 0;
            clk = 0;
        }                       // (the mode box is modal: its clicks / selects are its own, never the CLAN row's)
        if ((sel && MkFocused("anim-group.menuList.Clan")
             && MkSelFor("anim-group.menuList.Clan")) || clk)
            pad = 1;
        if (pad)
            Xlog("clan13 open: via %s, LBUTTON %d, cursor over CLAN %d, FIND MATCH %d, focused CLAN %d, click edge %d", clk ? "click at press" : "select (key/pad)", g_lbNow, UiMouseOver("anim-group.menuList.Clan", 1), UiMouseOver("anim-group.menuList.FindMatch", 1), MkFocused("anim-group.menuList.Clan"), g_clickEdge);
    }
    int test = 0;               // dev (screenshots with no keyboard focus): t2r-clan-test.txt next to the exe =
    {
        static uint64_t tchk;
        uint64_t tn = GetTickCount64(); // open the page, RAIDS tab, newest raid, PLAYER STATS ("stats") or just the page ("open"); deleted once used
        if (tn - tchk > 1000) {
            tchk = tn;
            FILE *f = ExeSideOpen("t2r-clan-test.txt", "r");
            if (f) {
                char b[16] = { 0 };
                fgets(b, sizeof b, f);
                fclose(f);
                test = !strncmp(b, "stats", 5) ? 2 : 1;
            }
        }
    }
    if (!(pad || test) || !Cl13Enabled() || g_loState != LO_NONE)
        return;
    // fix1025-clanopen: never once the pre-match sequence (vote / gear up / loading) or a match is running, or while a clan screen is due
    if (g_mfCodeNow >= 100 || g_mhLoad || g_mhSt || g_lsOwn
        || !MsOnMenu() || g_pm.pending)
        return;
    static uint64_t last;
    uint64_t now = GetTickCount64();
    if (now - last < 1500)
        return;
    last = now;
    if (test) {
        g_c13Test = test;
        g_c13TestMs = now;
        DeleteFileA("t2r-clan-test.txt");
    }
    Xlog("clan13: %s - pushing to1/clan13",
         test ? "t2r-clan-test.txt" : "CLAN row of the MP menu selected");
    if (pad)
        UiSfx("sfx-select");
    PushPage("to1/clan13");
}
