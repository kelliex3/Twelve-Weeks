// gearup13.c - the 2013-layout GEAR UP page (built with the in-game uis the SAME way as the map vote and loading screen,
// keeping the style of the other two, and not a fake ui).
// Included after skills.c / gunlimit.c (it uses their loadout helpers). The widgets are tools/features/gearup13_widgets.py (page to1/gearup13, built
// from the same Builder as to1/matchflow); like the match flow pages this one is driven ENTIRELY from the tick - no winmm page handler runs for it -
// and every text / icon / visibility goes through the dedup writers (ui_text, ui_image, ui_visible, w_alpha: no per-frame rewrites, no flicker).
//
// Two views in ONE page (a second instance of a page on the stack breaks it):
//   LIST  the 6 custom loadouts g_lo[] + the 8 CLASSES presets, the focused loadout's gear and points on the right.
//         CREATE LOADOUT under the custom loadouts adds one (up to N_CUSTOM_MAX: a name box, then the new loadout's edit view).
//         SELECT on a class equips it; on a custom loadout it equips it and opens the edit view; pre-match (CHOOSE LOADOUT) / mid-match (CHANGE
//         LOADOUT) it only equips (mid-match through g_lo_pending, the lobby object keeps this life's loadout: lo_snap_tick).
//   EDIT  CUSTOM LOADOUT n: 7 slots (small, large, 4 skills, purchasable) | the focused slot's options listed at once |
//         the hovered option's details. A click / SELECT on an option equips it and STAYS in the options list (no page change); every change is
//         written straight into the lobby object (+0x2c58 guns, sk_slot_set skills) and into g_lo[active] / t2r-loadout.txt.
// Keys: W / S (menu-up / down) move, A / D (menu-left / right) slots <-> options (a skill's level inside the options), SELECT / left click
// equips, menu-back / Esc / right click: options -> slots -> list -> out. Mouse: hover focuses, click equips, wheel scrolls the options.
// t2r-no-gearup13.txt next to the exe: off (the old picker-skill flow runs unchanged). Log prefix "gearup13:".
// The list (CREATE LOADOUT): lsRow0..5 show six custom loadouts (more than six scroll in them: G.coff, the lsSb scroll bar), the CREATE
// LOADOUT button lsNew under them, the gap, the classes in lsRow6..13. G.lst / G.pv / g_gu_hov_lst are list POSITIONS: the custom slots 0..g_lo_n-1,
// then CREATE LOADOUT (g_lo_n), then the classes (gu_pos_idx / gu_idx_pos map them to loadout indexes).
#define GU_CUST_ROWS 6
#define GU_N_ROWW (GU_CUST_ROWS + N_CLASSES)    // lsRow widgets
#define GU_W_NEW GU_N_ROWW      // the CREATE LOADOUT button as a "widget index" (hover / nearest)
#define GU_SB_Y0 396.0f         // the scroll bar's track (gearup13_widgets.py SB_*): the six custom rows
#define GU_SB_H 510.0f
#define GU_SB_X 1186.0f
#define GU_SLOTS 7
#define GU_OPT_ROWS 10
#define GU_HINT_W 1040.0f       // the key hint pHint (right aligned at x 3500, shrunk by gu_fit): its room left of RENAME (gearup13_widgets.py HINT_W)
#define GU_HINT_W_REN 1250.0f   // ... and while a name is typed (RENAME hidden: the whole box)
#define GU_MAX_OPT 80
#define GU_NONE_ICON "streaming/t2r/icons/blocked.png"
#define GU_EMPTY_ICON "hud/weapon-cross/empty-slot.svg"
enum { GS_SMALL, GS_LARGE, GS_SK1, GS_SK2, GS_SK3, GS_SK4, GS_PURCH };  // the slot column's order (2013factionsloadout1)
enum { GO_NONE, GO_GUN, GO_PURCH, GO_SKILL };
typedef struct {
    int kind;
    uint32_t row;
    uint32_t base;
    int supp;
    int sj;
} GuOptT;                     // gun: table row, its base row, suppressed row?, its suppressed row (-1); skill: row = skill id
static GuOptT g_guOpt[GU_MAX_OPT];
static int g_guNopt;
static int g_guLvl[SK_COUNT];  // the level shown (and equipped on a click) per skill
static int g_guHovLst = -1, g_gu_hov_slot = -1, g_gu_hov_opt = -1, g_gu_hov_lvl = -1, g_gu_hov_back, g_gu_hov_sel, g_gu_hov_rst, g_gu_hov_tg, g_gu_hov_ren;   // gu13_scan (once a frame, from the click gate)
static struct {
    int up, view, col, lst, slot, opt, off, dirty;
    uint64_t click_f;           // view 0 list / 1 edit, col 0 slots / 1 options, lst = focused loadout, slot / opt / off = edit focus + options window
    int h_sel, h_u, h_d, h_l, h_r, h_back, h_esc, h_act, h_r_key;       // held states of the menu actions (edges)
    uint64_t t_u, t_d, enter_ms, back_f, sfx_ms;
    int det_key, det_ph, det_hold, view_anim, pre_i;
    uint64_t det_t0, view_t0;   // animation state (gu_anim_tick)
    int pv, pv_want, pv_ph, pv_ign, pv_lasthov, pv_lastlst, pv_lastwant;
    float pv_a;
    uint64_t pv_t0, pv_chg;     // list preview: shown row / wanted row / tween phase (0 idle, 1 out, 2 in)
    int stage, closing, hv_slot, hv_opt, mm_last, h_nx, h_e;    // h_nx / h_e: menu-next / the E key (RENAME)        // mm_last / hv_*: the mouse state of the last frame (hover acts on a change)
    int coff;                   // the custom slot shown in lsRow0 (more than six customs scroll)
} G;
static struct {
    int ok, view, col, slot, opt, off;
} g_guKeep;                    // the edit view's place, kept across the engine's own pop + our push on menu-back
// RENAME state (the name being typed: code points, cursor, the pad's held states; see the RENAME section)
// mode 0: RENAME on the edit view; 1: the CREATE LOADOUT name box on the list. sel: the prefilled default is selected (the first letter typed replaces it),
// dflt: the text is still the prefilled default (saved as "" = the default label, so it follows the language)
static struct {
    int on, mode, sel, dflt, n, cur, pad, h_u, h_d, h_l, h_r, h_sel, h_act,
        h_back, h_nx;
    uint32_t cp[LO_NM_MAX];
    uint64_t t_u, t_d, t0, f0, click_f;
} GN;
static volatile LONG g_ktOn;   // 1 = a name is typed: the keyboard hook eats the keys (kt_ll_proc)

static uint64_t Gu13PageId(void)
{
    static uint64_t id;
    if (!id)
        id = PageId("gearup13");
    return id;
}

static int GuNpos(void)
{
    return g_loN + 1 + N_CLASSES;
}

static int GuPosIdx(int pos)
{                               // list position -> loadout index (-1: CREATE LOADOUT / nothing)
    if (pos < 0)
        return -1;
    if (pos < g_loN)
        return LoSlotIdx(pos);
    if (pos == g_loN)
        return -1;
    return pos - g_loN - 1 < N_CLASSES ? N_CUSTOM + pos - g_loN - 1 : -1;
}

static int GuIdxPos(int idx)
{
    if (LoCustom(idx))
        return LoSlot(idx);
    return LoCls(idx) >= 0 ? g_loN + 1 + LoCls(idx) : 0;
}

static int GuWidPos(int w)
{                               // lsRow widget (GU_W_NEW: lsNew) -> list position (-1: none)
    if (w < 0)
        return -1;
    if (w < GU_CUST_ROWS)
        return G.coff + w < g_loN ? G.coff + w : -1;
    if (w < GU_N_ROWW)
        return g_loN + 1 + w - GU_CUST_ROWS;
    return w == GU_W_NEW ? g_loN : -1;
}

static void GuScrollTo(int pos)
{                               // keep the focused custom row inside the six (CREATE LOADOUT: the last six, the button follows them)
    const int mx = g_loN > GU_CUST_ROWS ? g_loN - GU_CUST_ROWS : 0;
    if (pos == g_loN)
        G.coff = mx;
    else if (pos >= 0 && pos < g_loN) {
        if (pos < G.coff)
            G.coff = pos;
        if (pos >= G.coff + GU_CUST_ROWS)
            G.coff = pos - GU_CUST_ROWS + 1;
    }
    if (G.coff > mx)
        G.coff = mx;
    if (G.coff < 0)
        G.coff = 0;
}

// ------------------------------------------------------------------ data helpers (real tables, no mock text)
static void GuReadCur(LoadoutT *l)
{                               // the lobby object's loadout (what the edit view changes); the 4th skill = g_sk4_cur via sk4_slot
    memset(l->w, 0xff, sizeof l->w);
    memset(l->k, 0, sizeof l->k);
    LoReadObj(l);
}

static int GuWs(int s)
{
    return s == GS_LARGE ? 0 : s == GS_SMALL ? 1 : 2;
}                               // slot -> lobby weapon slot (0 large, 1 small, 2 purchasable)

static const char *GuHead(int s)
{
    static const char *const H[GU_SLOTS] =
        { "SMALL FIREARM", "LARGE FIREARM", "SURVIVAL SKILL 1",
"SURVIVAL SKILL 2", "SURVIVAL SKILL 3", "SURVIVAL SKILL 4", "PURCHASABLE" };
    return H[s];
}

static const char *GuNoneName(int s)
{
    static const char *const N[GU_SLOTS] =
        { "NO SMALL FIREARM", "NO LARGE FIREARM", "NOTHING", "NOTHING",
"NOTHING", "NOTHING", "NO PURCHASABLE" };
    return N[s];
}

static const char *GuStr(const char *s)
{
    return s && !IsBadReadPtr(s, 1) ? s : "";
}

static void GuCostStr(char *b, size_t n, int lp)
{
    if (lp > 0)
        snprintf(b, n, "%d LP", lp);
    else
        snprintf(b, n, "FREE");
}

static const uint8_t *GuRow(int ws, uint32_t idx)
{                               // a gun / purchasable table row (a blank row when out of range)
    if (ws == 2) {
        const uint32_t *cnt =
            (const uint32_t *) T2R_SYM(kLargeWeaponCounts);
        return idx < cnt[2] ? LoPurchTab() + idx * 0x40 : WeaponRow(0,
                                                                       ~0u);
    }
    return WeaponRow(ws, idx);
}

static int GuRowOk(int ws, uint32_t idx)
{
    return idx < ((const uint32_t *) T2R_SYM(kLargeWeaponCounts))[ws]
        && !(ws == 0 && !g_largeTab);
}

static int GuRowCost(int ws, uint32_t idx)
{
    return GuRowOk(ws,
                     idx) ? (int) *(const uint32_t *) (GuRow(ws,
                                                              idx) +
                                                       0x18) : 0;
}

static const SkRecT *GuSkRec(uint64_t v)
{
    return (const SkRecT *) SkLookup(v);
}

static uint64_t GuSkVal(const LoadoutT *l, int k)
{
    return SkNorm(k < 3 ? l->k[k] : *Sk4Slot(l));
}                               // skill row k (0..3) of a loadout

// What a slot of loadout l holds: name, icon, LP cost (0 + NULL name = empty)
static const char *GuSlotItem(int s, const LoadoutT *l,
                                const char **icon, int *cost)
{
    *icon = GU_NONE_ICON;
    *cost = 0;
    if (s == GS_SK1 || s == GS_SK2 || s == GS_SK3 || s == GS_SK4) {
        const SkRecT *r = GuSkRec(GuSkVal(l, s - GS_SK1));
        if (!r)
            return NULL;
        *icon = r->icon;
        *cost = (int) r->cost;
        return r->name;
    }
    int ws = GuWs(s);
    const char *ic = NULL, *nm = LoWeaponName(ws, l->w[ws], &ic);
    if (!nm)
        return NULL;
    *icon = ic ? ic : GU_EMPTY_ICON;
    *cost = GuRowCost(ws, l->w[ws]);
    return nm;
}

// the loadout shown for list row r: the live lobby loadout for the custom loadout being edited (g_lo[] catches up twice a second), else the saved one
static const LoadoutT *GuListLo(int r, LoadoutT *tmp)
{
    if (r == g_loActive && LoSlot(r) >= 0 && g_loState != LO_MATCH
        && MatchSubstate() != 6) {
        GuReadCur(tmp);
        return tmp;
    }
    return LoOf(r);
}

static int GuLeftLp(const LoadoutT *l)
{
    return 13 - LoCost(l);
}

static void GuLpStr(char *b, size_t n, int left)
{
    if (left <= 0)
        snprintf(b, n,
                 "<font color=\"#d23c32\">%d/13 Loadout Points</font>",
                 left < 0 ? 0 : left);
    else
        snprintf(b, n, "%d/13 Loadout Points", left);
}

// ------------------------------------------------------------------ the options of a slot
static void GuAdd(int kind, uint32_t row, uint32_t base, int supp, int sj)
{
    if (g_guNopt >= GU_MAX_OPT)
        return;
    g_guOpt[g_guNopt++] = (GuOptT) {
    kind, row, base, supp, sj};
}

static void GuBuildOpts(int s)
{
    g_guNopt = 0;
    GuAdd(GO_NONE, 0xffffffffu, 0, 0, -1);
    if (s == GS_SMALL || s == GS_LARGE) {
        int ws = GuWs(s);
        if (ws == 0 && (!g_largeTab || !g_pickerBase[0]))
            return;
        // one row per gun; a gun with a suppressed variant keeps it in sj and the details panel's UNSUPPRESSED / SUPPRESSED toggle switches to it
        for (uint32_t i = 0; i < g_pickerBase[ws] && i < 40; i++)
            GuAdd(GO_GUN, i, i, 0, SuppRow(ws, i));
    } else if (s == GS_PURCH) {
        const uint32_t n =
            ((const uint32_t *) T2R_SYM(kLargeWeaponCounts))[2];
        for (uint32_t i = 0; i < n && i < 40; i++)
            GuAdd(GO_PURCH, i, i, 0, -1);
    } else {
        for (int id = 1; id < SK_COUNT; id++)
            GuAdd(GO_SKILL, (uint32_t) id, 0, 0, -1);
    }
}

static void GuLvlReset(void)
{                               // each skill shows its equipped level, else its first level that does something
    for (int id = 1; id < SK_COUNT; id++) {
        g_guLvl[id] = 1;
        for (int k = 0; k < SK[id].nlv; k++)
            if (SK[id].avail >> k & 1) {
                g_guLvl[id] = k + 1;
                break;
            }
        int in = SkInSlot(id);
        if (in >= 0)
            g_guLvl[id] = SkLv(SkNorm(SkSlotGet(in)));
    }
}

static int GuOptEq(int s, const GuOptT *o, const LoadoutT *cur)
{                               // is this option what the slot holds now?
    if (o->kind == GO_NONE) {
        if (s >= GS_SK1 && s <= GS_SK4)
            return !GuSkVal(cur, s - GS_SK1);
        return cur->w[GuWs(s)] == 0xffffffffu
            || !GuRowOk(GuWs(s), cur->w[GuWs(s)]);
    }
    if (o->kind == GO_SKILL)
        return SkInSlot((int) o->row) == s - GS_SK1
            && SkLv(GuSkVal(cur, s - GS_SK1)) == g_guLvl[o->row];
    return cur->w[GuWs(s)] == o->row || (o->sj >= 0 && cur->w[GuWs(s)] == (uint32_t) o->sj);  // either variant counts as the gun
}

// the variant of a gun option the slot holds now: 1 = suppressed, 0 = plain, -1 = this gun is not equipped (neither toggle button is the active one)
static int GuOptVar(int s, const GuOptT *o, const LoadoutT *cur)
{
    if (!o || o->kind != GO_GUN || o->sj < 0)
        return -1;
    uint32_t w = cur->w[GuWs(s)];
    return w == (uint32_t) o->sj ? 1 : w == o->row ? 0 : -1;
}

static uint32_t GuOptRowNow(int s, const GuOptT *o,
                               const LoadoutT *cur)
{
    return GuOptVar(s, o, cur) == 1 ? (uint32_t) o->sj : o->row;
}

static int GuEqIndex(int s, const LoadoutT *cur)
{                               // the equipped option (the NONE row when nothing is; a skill: the row of the equipped skill whatever its shown level)
    if (s >= GS_SK1 && s <= GS_SK4) {
        int id = SkId(GuSkVal(cur, s - GS_SK1));
        for (int i = 0; i < g_guNopt; i++)
            if (g_guOpt[i].kind == GO_SKILL
                && (int) g_guOpt[i].row == id)
                return i;
        return 0;
    }
    for (int i = 0; i < g_guNopt; i++)
        if (GuOptEq(s, &g_guOpt[i], cur))
            return i;
    return 0;
}

static void GuFocusOpt(int i)
{                               // option i is the focus: the window follows
    if (i < 0)
        i = 0;
    if (i >= g_guNopt)
        i = g_guNopt - 1;
    G.opt = i;
    if (G.opt < G.off)
        G.off = G.opt;
    else if (G.opt >= G.off + GU_OPT_ROWS)
        G.off = G.opt - GU_OPT_ROWS + 1;
    if (G.off > g_guNopt - GU_OPT_ROWS)
        G.off = g_guNopt > GU_OPT_ROWS ? g_guNopt - GU_OPT_ROWS : 0;
    if (G.off < 0)
        G.off = 0;
    G.dirty = 1;
}

static void GuEnterSlot(int s)
{                               // slot s is the focus: its options listed, the equipped one focused
    LoadoutT cur;
    GuReadCur(&cur);
    G.slot = s;
    if (s >= GS_SK1 && s <= GS_SK4)
        GuLvlReset();
    GuBuildOpts(s);
    G.off = 0;
    GuFocusOpt(GuEqIndex(s, &cur));
}

// Name fit (names stuck out when they were too long): the name widgets (gearup13_widgets.tlm) pivot at
// their left middle, so w_scale shrinks a too-long name toward its left edge, centred in the row. Width estimate per character at
// point size pt: capitals / digits 0.80 pt (detail_title: 32.2 units at 40), lower case 0.62 pt, space 0.40 pt; floor 0.6.
static float GuCw(const unsigned char *q)
{
    return *q == ' ' ? 0.40f : (*q >= 'a' && *q <= 'z')
        || *q >= 0x80 ? 0.62f : 0.80f;
}

static float GuEst(const char *t, float pt)
{                               // estimated width of t (translated, colour tags ignored) at point size pt
    const char *tr = LocTr(t ? t : "");
    float Est = 0.0f;
    int tag = 0;
    for (const unsigned char *q =
         (const unsigned char *)(tr ? tr : t ? t : ""); *q; q++) {
        if (*q == '<')
            tag = 1;
        if (tag) {
            if (*q == '>')
                tag = 0;
            continue;
        }
        if ((*q & 0xC0) == 0x80)
            continue;
        Est += GuCw(q);
    }
    return Est * pt;
}

static struct {
    uint64_t w;
    float s;
} g_gu_fit[192];                // per widget handle: the scale last written (cleared with the page: a rebuilt page can reuse a freed handle at scale 1)
#define c g_gu_fit
static void GuFit(const char *path, const char *t, float box, float pt)
{
    if (!t)
        t = "";
    const char *tr = LocTr(t);
    const char *src = tr ? tr : t;
    float Est = 0.0f;
    int tag = 0;
    for (const unsigned char *q = (const unsigned char *)src; *q; q++) {
        if (*q == '<')
            tag = 1;
        if (tag) {
            if (*q == '>')
                tag = 0;
            continue;
        }
        if ((*q & 0xC0) == 0x80)
            continue;
        Est += GuCw(q);
    }
    Est *= pt;
    // option / slot names are written past the dedup and their scale re-applied on every fill: a row's name widget (the 7th visible one, every tab)
    // stayed blank while the cache believed it already held its text / scale
    int raw = strstr(path, "opNm") || strstr(path, "slNm");
    float sc = Est > box ? box / Est : 1.0f;
    if (sc < 0.6f)
        sc = 0.6f;
    // even at the smallest scale too wide (a long name in a narrow row): cut it with "..." so it never reaches the cost or the frame.
    // Only plain names (the coloured tags are short and never this long). The full name stays in the details panel.
    char cut[96];
    if (Est * sc > box * 1.02f && !strchr(src, '<')) {
        float lim = box / (pt * 0.6f) - 3 * 0.80f, acc = 0.0f;  // width budget in "pt units" minus the three dots
        size_t n = 0;
        for (const unsigned char *q = (const unsigned char *)src;
             *q && n < sizeof cut - 4; q++) {
            if ((*q & 0xC0) != 0x80) {
                acc += GuCw(q);
                if (acc > lim)
                    break;
            }
            cut[n++] = (char) *q;
        }
        while (n > 0 && ((unsigned char) cut[n - 1] & 0xC0) == 0x80)
            n--;                // never half a UTF-8 sequence
        if (n > 0 && ((unsigned char) cut[n - 1] & 0x80)
            && ((unsigned char) cut[n - 1] & 0xC0) == 0xC0)
            n--;
        while (n > 0 && cut[n - 1] == ' ')
            n--;
        memcpy(cut + n, "...", 4);
        if (raw)
            UI_TEXT_RAW(path, cut);
        else
            UiText(path, cut);
    } else if (raw)
        UI_TEXT_RAW(path, t);
    else
        UiText(path, t);
    uint64_t w = UiW(path);
    if (!w)
        return;
    int k, fr = -1;
    for (k = 0; k < 192 && c[k].w != w; k++)
        if (fr < 0 && !c[k].w)
            fr = k;
    if (!raw && k < 192 && c[k].s == sc)
        return;                 // already at this scale
    if (k == 192) {
        k = fr >= 0 ? fr : (int) (w >> 4) % 192;
        c[k].w = w;
    }
    c[k].s = sc;
    WScale(w, sc, sc);
}

#undef c

// ------------------------------------------------------------------ focus fades (the tick-driven page has none of the engine's focus motion)
// Focus frames / fills / arrows fade in and out with a short exponential ease instead of popping: the row that loses the focus fades out while the
// new one fades in (a crossfade, ~150 ms). Per widget handle; a write happens only when the quantised alpha changes (no flicker, no per-frame writes).
#define GU_FADERS 128
#define GU_FQ 24
// keyed by the widget PATH, not the handle (the 7th option row's name was blank in every tab, log "ui alpha 0: widget <opNm6>"
// from this tick every 6 s): widget handles are recycled when the engine rebuilds / re-pushes the page, so a fader that kept a handle from an
// earlier resolution kept writing its alpha 0 to whatever widget owns that handle now - the 7th row's name. The handle is re-resolved on every
// gu_fade call and an entry that no fill has touched for 200 frames is dropped without a write.
static struct {
    uint64_t ph, w, seen;
    float a, t, mx;
    int q;
    char path[72];
} g_gf[GU_FADERS];
// w_alpha SETS the widget's alpha (it replaces the colour alpha baked by the page builder), so a fade must top out at the baked value:
// fill 14 / frame 180 / arrow 220 of 255 (gearup13_widgets.py HILITE_A / FOCUS_A). Fading to 1.0 drew the highlight as a solid white bar.
static float GuFadeMax(const char *path)
{
    size_t n = strlen(path);
    if (n >= 8 && strstr(path, "Fill"))
        return 14.0f / 255.0f;
    if (strstr(path, "Frame"))
        return 180.0f / 255.0f;
    if (strstr(path, "Arrow"))
        return 220.0f / 255.0f;
    return 1.0f;
}

static uint64_t g_gfMs;
static void GuFade(const char *path, int on)
{
    uint64_t w = UiW(path);
    if (!w)
        return;
    uint64_t ph = 1469598103934665603ULL;
    for (const char *c = path; *c; c++)
        ph = (ph ^ (uint8_t) * c) * 1099511628211ULL;
    int i, fr = -1;
    for (i = 0; i < GU_FADERS && g_gf[i].ph != ph; i++)
        if (fr < 0 && !g_gf[i].ph)
            fr = i;
    if (i == GU_FADERS) {
        if (fr < 0) {
            UiVisible(path, on);
            return;
        }                       // (table full: the plain switch)
        i = fr;
        g_gf[i].ph = ph;
        g_gf[i].w = w;
        snprintf(g_gf[i].path, sizeof g_gf[i].path, "%s", path);
        {
            static int tr = -1;
            if (tr < 0) {
                FILE *f = ExeSideOpen("t2r-gu-trace.txt", "r");
                tr = f != NULL;
                if (f)
                    fclose(f);
            }
            if (tr)
                Xlog("gearup13: gu trace: fader %d %s = %llx", i, path,
                     (unsigned long long) w);
        } g_gf[i].a = 0.0f;
        g_gf[i].q = -1;
        g_gf[i].mx = GuFadeMax(path);
    }
    if (g_gf[i].w != w) {       // the path resolves to another widget now: start that one over
        static int nl;
        if (LOGCAP(nl, 20))
            Xlog("gearup13: fader %s moved from widget %llx to %llx", path,
                 (unsigned long long) g_gf[i].w, (unsigned long long) w);
        g_gf[i].w = w;
        g_gf[i].q = -1;
    }
    g_gf[i].seen = g_frame;
    g_gf[i].t = on ? 1.0f : 0.0f;
}

static void GuFadeReset(void)
{
    memset(g_gf, 0, sizeof g_gf);
    g_gfMs = 0;
}

static void GuFadeReassert(void)
{
    for (int i = 0; i < GU_FADERS; i++)
        g_gf[i].q = -1;
}

static void GuFadeTick(uint64_t now)
{
    float dt = g_gfMs ? (float) (now - g_gfMs) : 16.0f;
    g_gfMs = now;
    if (dt > 50.0f)
        dt = 50.0f;
    float k = 1.0f - expf(-dt / 45.0f);
    for (int i = 0; i < GU_FADERS; i++) {
        if (!g_gf[i].w)
            continue;
        if (g_frame - g_gf[i].seen > 200) {
            memset(&g_gf[i], 0, sizeof g_gf[i]);
            continue;
        }                       // no fill has asked for it lately: its handle may belong to another widget now
        float d = g_gf[i].t - g_gf[i].a;
        g_gf[i].a = fabsf(d) < 0.03f ? g_gf[i].t : g_gf[i].a + d * k;
        int q = (int) (g_gf[i].a * GU_FQ + 0.5f);
        if (q == g_gf[i].q)
            continue;
        g_gf[i].q = q;
        uint64_t cw = UiW(g_gf[i].path);       // a write is rare: resolve the path again, never trust a kept handle
        if (!cw)
            continue;
        g_gf[i].w = cw;
        WVisible(g_gf[i].w, q > 0);
        WAlpha(g_gf[i].w, (float) q / GU_FQ * g_gf[i].mx);
    }
}

// ------------------------------------------------------------------ animation (everything switched 0 -> 1 instantly)
// The engine's own menus animate with per-widget keyframe tracks (widget json "animation": keyframes + labels, e.g. animations/fade-default:
// labels off^active / active^off over 10 frames) that its menu scripts start; no DC native to start a label is known to this mod, so the page keeps
// its OWN small tweens, frame-time based (GetTickCount64), eased out, written through the same setters (w_alpha / w_pivot: both skip a repeat
// value). Nothing is written while nothing animates. t2r-gu-noanim.txt next to the exe = no tweens (the plain instant page).
static int GuNoanim(void)
{
    static int on = -1;
    static uint64_t chk;
    uint64_t n = GetTickCount64();
    if (on < 0 || n - chk > 5000) {
        chk = n;
        FILE *f = ExeSideOpen("t2r-gu-noanim.txt", "r");
        on = f != NULL;
        if (f)
            fclose(f);
    }
    return on;
}

static float GuEase(float t)
{
    if (t <= 0.0f)
        return 0.0f;
    if (t >= 1.0f)
        return 1.0f;
    float u = 1.0f - t;
    return 1.0f - u * u * u;
}                               // ease-out cubic

// a group's alpha + horizontal slide (the group's pivot is its own top left: 0 = in place)
static void GuGrp(const char *path, float a, float dx)
{
    uint64_t w = UiW(path);
    if (!w)
        return;
    WAlpha(w, a);
    WPivot(w, dx, 0.0f);
}

// a leaf widget (text / image) inside a group: alpha only. Its pivotAs is its own layout position, NOT a 0-based offset like a group's, so a pivot write of
// (dx, 0) sent every previewed text and icon to the top left corner of the page.
static void GuLeaf(const char *path, float a)
{
    uint64_t w = UiW(path);
    if (w)
        WAlpha(w, a);
}

// text pops: a value that changed (LP numbers, EQUIPPED / state words, costs) fades in quickly instead of switching
#define GU_POPS 40
static struct {
    uint64_t w, h, t0;
} g_gu_pop[GU_POPS];
static void GuPop(const char *path, const char *text)
{
    uint64_t w = UiW(path);
    if (!w || !text)
        return;
    uint64_t h = 1469598103934665603ULL;
    for (const char *p = text; *p; p++)
        h = (h ^ (uint8_t) * p) * 1099511628211ULL;
    int i, fr = -1;
    for (i = 0; i < GU_POPS && g_gu_pop[i].w != w; i++)
        if (fr < 0 && !g_gu_pop[i].w)
            fr = i;
    if (i == GU_POPS) {
        if (fr < 0)
            return;
        g_gu_pop[fr].w = w;
        g_gu_pop[fr].h = h;
        g_gu_pop[fr].t0 = 0;
        return;
    }                           // (first sight: no pop)
    if (g_gu_pop[i].h != h) {
        g_gu_pop[i].h = h;
        if (!GuNoanim())
            g_gu_pop[i].t0 = GetTickCount64();
    }
}

static void GuAnimTick(uint64_t now)
{
    if (GuNoanim()) {
        G.det_hold = 0;
        if (G.view == 0 && G.pv != G.pv_want) {
            G.pv = G.pv_want;
            G.dirty = 1;
        }
        return;
    }
    // ---- view change (list <-> edit): the incoming group fades in with a 14 unit slide, 170 ms
    if (G.view_anim) {
        float p = GuEase((float) (now - G.view_t0) / 170.0f);
        GuGrp(G.view ? "anim-group.ed" : "anim-group.lst", p,
               (1.0f - p) * 14.0f);
        if (p >= 1.0f)
            G.view_anim = 0;
    }
    // ---- details panel: the old content fades out fast (80 ms, the refill of the details waits), the new fades in with a 8 unit slide (150 ms)
    if (G.view == 1) {
        int key = (G.slot + 1) * 1000 + (G.opt + 1);
        if (G.det_key == -1)
            G.det_key = key;
        else if (key != G.det_key && G.det_ph != 1) {
            G.det_key = key;
            G.det_ph = 1;
            G.det_t0 = now;
            G.det_hold = 1;
        } else if (key != G.det_key)
            G.det_key = key;    // (changed again during the fade-out: the refill uses the latest)
        if (G.det_ph == 1) {
            float p = (float) (now - G.det_t0) / 80.0f;
            GuGrp("anim-group.ed.dt", 1.0f - GuEase(p), 0.0f);
            if (p >= 1.0f) {
                G.det_ph = 2;
                G.det_t0 = now;
                G.det_hold = 0;
                G.dirty = 1;
            }
        } else if (G.det_ph == 2) {
            float p = GuEase((float) (now - G.det_t0) / 150.0f);
            GuGrp("anim-group.ed.dt", p, (1.0f - p) * 8.0f);
            if (p >= 1.0f)
                G.det_ph = 0;
        }
    } else
        G.det_key = -1;
    // ---- list view: the right panel previews the hovered loadout (or the focused one), quick out 80 ms, in 150 ms with an 8 unit slide
    if (G.view == 0) {
        static const char *const PV[] =
            { "lgIc0", "lgIc1", "lgIc2", "lgNm0", "lgNm1", "lgNm2",
"lskIc0", "lskIc1", "lskIc2", "lskIc3", "lskNm0", "lskNm1", "lskNm2", "lskNm3",
"lsName", "lsDesc", "lsCur", "lsLP" };
        float a = -1.0f;
        if (G.pv < 0)
            G.pv = G.pv_want;
        if (G.pv_ph == 0 && G.pv_want != G.pv && now - G.pv_chg >= 60) {
            G.pv_ph = 1;
            G.pv_t0 = now;
        }
        if (G.pv_ph == 1) {
            float p = (float) (now - G.pv_t0) / 80.0f;
            a = G.pv_a * (1.0f - GuEase(p));
            if (p >= 1.0f) {
                G.pv = G.pv_want;
                G.pv_ph = 2;
                G.pv_t0 = now;
                G.dirty = 1;
                a = 0.0f;
            }
        } else if (G.pv_ph == 2) {
            float p = GuEase((float) (now - G.pv_t0) / 150.0f);
            a = p;
            if (G.pv_want != G.pv && now - G.pv_chg >= 60) {
                G.pv_ph = 1;
                G.pv_t0 = now;
                G.pv_a = a;
                a = G.pv_a;
            }                   // retarget from where the fade-in is
            else if (p >= 1.0f) {
                G.pv_ph = 0;
                a = 1.0f;
            }
        }
        if (a >= 0.0f) {
            if (G.pv_ph != 1)
                G.pv_a = a;
            char pp[160];
            for (unsigned i = 0; i < sizeof PV / sizeof PV[0]; i++) {
                snprintf(pp, sizeof pp, "anim-group.lst.%s", PV[i]);
                GuLeaf(pp, a);
            }
        }
    } else if (G.pv_ph) {       // left the list mid-tween: the panel is back to plain
        static const char *const PV2[] =
            { "lgIc0", "lgIc1", "lgIc2", "lgNm0", "lgNm1", "lgNm2",
"lskIc0", "lskIc1", "lskIc2", "lskIc3", "lskNm0", "lskNm1", "lskNm2", "lskNm3",
"lsName", "lsDesc", "lsCur", "lsLP" };
        char pp[160];
        for (unsigned i = 0; i < sizeof PV2 / sizeof PV2[0]; i++) {
            snprintf(pp, sizeof pp, "anim-group.lst.%s", PV2[i]);
            GuLeaf(pp, 1.0f);
        }
        G.pv_ph = 0;
        G.pv = -1;
    }
    // ---- text pops
    for (int i = 0; i < GU_POPS; i++) {
        if (!g_gu_pop[i].w || !g_gu_pop[i].t0)
            continue;
        float p = (float) (now - g_gu_pop[i].t0) / 140.0f;
        if (p >= 1.0f) {
            WAlpha(g_gu_pop[i].w, 1.0f);
            g_gu_pop[i].t0 = 0;
            continue;
        }
        WAlpha(g_gu_pop[i].w, 0.2f + 0.8f * GuEase(p));
    }
}

// Icon residency (icons updated with a noticeable delay): the engine streams a texture in when a widget first shows it, so the first
// selection of each item showed a blank / the old icon for a while. Every icon the page can show is put on a tiny always-visible widget of the
// page (pre0..pre95, a few per frame after the page opens) so the textures are requested while the page is still fading in.
#define GU_PRE_N 96
static void GuPreloadTick(void)
{
    static const char *const AMMO[] =
        { "hud/weapons/ammo/ammo-arrow-explosive_small.png",
"hud/weapons/ammo/ammo-arrow_small.png",
        "hud/weapons/ammo/ammo-firethrower_health-hud.png",
            "hud/weapons/ammo/ammo-shotgun_small.png",
            "hud/weapons/ammo/ammo-pistol-light.png",
        "hud/weapons/ammo/ammo-rifle-light.png"
    };
    if (G.pre_i < 0)
        return;
    // A client crashed in the engine 35 frames after the page opened (EXCEPTION c0000005 at exe+13ac870 on a non-tick thread, an icon path and an 80x80
    // size in the registers) right after this pool shipped: off until proven safe; t2r-gu-preload.txt next to the exe turns it on for tests
    static int on = -1;
    if (on < 0) {
        FILE *f = ExeSideOpen("t2r-gu-preload.txt", "r");
        on = f != NULL;
        if (f)
            fclose(f);
    }
    if (!on) {
        G.pre_i = -1;
        return;
    }
    const uint32_t *cnt = (const uint32_t *) T2R_SYM(kLargeWeaponCounts);
    int a = (int) cnt[0], b = (int) cnt[1], c = (int) cnt[2], nsk =
        SK_COUNT - 1;
    for (int n = 0; n < 8 && G.pre_i >= 0; n++) {
        int i = G.pre_i++;
        const char *ic = NULL;
        if (i >= GU_PRE_N || i >= a + b + c + nsk + 6) {
            G.pre_i = -1;
            break;
        }
        if (i < a) {
            if (GuRowOk(0, (uint32_t) i))
                ic = *(const char **) (GuRow(0, (uint32_t) i) + 8);
        } else if (i < a + b)
            ic = *(const char **) (GuRow(1, (uint32_t) (i - a)) + 8);
        else if (i < a + b + c)
            ic = *(const char **) (GuRow(2, (uint32_t) (i - a - b)) + 8);
        else if (i < a + b + c + nsk)
            ic = SK[i - a - b - c + 1].icon;
        else
            ic = AMMO[i - a - b - c - nsk];
        if (ic && !IsBadReadPtr(ic, 1) && ic[0]) {
            char p[64];
            snprintf(p, sizeof p, "anim-group.pre.pre%d", i);
            UiImage(p, ic);
        }
    }
}

// ------------------------------------------------------------------ list view
static void GuFillList(void)
{
    char p[128], b[200], nm[48];
    LoadoutT tmp;
    GuScrollTo(G.lst);
    for (int w = 0; w < GU_N_ROWW; w++) {
        const int pos = GuWidPos(w), i = GuPosIdx(pos);
        snprintf(p, sizeof p, "anim-group.lst.lsRow%d", w);
        UiVisible(p, i >= 0);
        if (i < 0)
            continue;
        int blocked = GlRowBlocked(i, NULL);
        int tagged = blocked || i == g_loActive;
        snprintf(p, sizeof p, "anim-group.lst.lsRow%d.lsName%d", w, w);
        GuFit(p, LoName(i, nm, sizeof nm), tagged ? 420.0f : 590.0f,
               44.0f);
        snprintf(p, sizeof p, "anim-group.lst.lsRow%d.lsTag%d", w, w);
        UiText(p,
                blocked ? "<font color=\"#d23c32\">TOO MANY</font>" : i ==
                g_loActive ? "EQUIPPED" : "");
        snprintf(p, sizeof p, "anim-group.lst.lsRow%d.lsFill%d", w, w);
        GuFade(p, pos == G.lst || pos == g_guHovLst);
        snprintf(p, sizeof p, "anim-group.lst.lsRow%d.lsFrame%d", w, w);
        GuFade(p, pos == G.lst);
        snprintf(p, sizeof p, "anim-group.lst.lsRow%d", w);
        WAlpha(UiW(p), blocked ? 0.35f : 1.0f);
    }
    {                           // CREATE LOADOUT (menu GEAR UP only: a new loadout opens the editor) + the custom rows' scroll bar
        const int pn = g_loN, can = g_loN < N_CUSTOM_MAX
            && g_loState == LO_LIST;
        GuFade("anim-group.lst.lsNew.lsNewFill", pn == G.lst
                || pn == g_guHovLst);
        GuFade("anim-group.lst.lsNew.lsNewFrame", pn == G.lst);
        snprintf(b, sizeof b, "%d/%d", g_loN, N_CUSTOM_MAX);
        UiText("anim-group.lst.lsNew.lsNewTag", b);
        WAlpha(UiW("anim-group.lst.lsNew"), can ? 1.0f : 0.35f);
        const int sb = g_loN > GU_CUST_ROWS;
        UiVisible("anim-group.lst.lsSbTrack", sb);
        UiVisible("anim-group.lst.lsSbThumb", sb);
        if (sb) {
            static int lk = -1;
            const int k = g_loN << 8 | G.coff;
            if (k != lk) {
                uint64_t th = UiW("anim-group.lst.lsSbThumb");
                if (th) {
                    lk = k;
                    WScale(th, 1.0f,
                            (float) GU_CUST_ROWS / (float) g_loN);
                    WPivot(th, GU_SB_X,
                            GU_SB_Y0 +
                            GU_SB_H * (float) G.coff / (float) g_loN);
                }
            }
        }
    }
    UiVisible("anim-group.nb", GN.on && GN.mode == 1);
    const int pvp = G.pv >= 0 && G.pv < GuNpos()? G.pv : G.lst;        // the previewed list position (hover / focus, tweened in gu_anim_tick)
    const int pv = GuPosIdx(pvp), pv_new = pv < 0;    // (CREATE LOADOUT: an empty loadout, its own text)
    static const LoadoutT nl = { { 0xffffffffu, 0, 0xffffffffu }, { 0, 0, 0 } };       // what CREATE LOADOUT makes (gu_create): the 9mm only
    const LoadoutT *l = pv_new ? &nl : GuListLo(pv, &tmp);
    static const int GS3[3] = { GS_SMALL, GS_LARGE, GS_PURCH };
    for (int k = 0; k < 3; k++) {       // small, large | purchasable (2013 order)
        const char *ic;
        int cost;
        const char *n = GuSlotItem(GS3[k], l, &ic, &cost);    // (guns only: nl is no g_lo[] slot, its 4th skill would read the lobby's)
        snprintf(p, sizeof p, "anim-group.lst.lgIc%d", k);
        UiImage(p, n ? ic : GU_NONE_ICON);
        snprintf(p, sizeof p, "anim-group.lst.lgNm%d", k);
        GuFit(p, n ? n : GuNoneName(GS3[k]), 930.0f, 44.0f);
    }
    for (int k = 0; k < 4; k++) {
        const char *ic;
        int cost;
        const char *n =
            pv_new ? NULL : GuSlotItem(GS_SK1 + k, l, &ic, &cost);
        snprintf(p, sizeof p, "anim-group.lst.lskIc%d", k);
        UiImage(p, n ? ic : GU_EMPTY_ICON);
        snprintf(p, sizeof p, "anim-group.lst.lskNm%d", k);
        GuFit(p, n ? n : "", 930.0f, 40.0f);
    }
    GuFit("anim-group.lst.lsName",
           pv_new ? "CREATE LOADOUT" : LoName(pv, nm, sizeof nm), 880.0f,
           56.0f);
    int n = 0;
    if (pv_new)
        snprintf(b, sizeof b, "%s",
                 g_loN >=
                 N_CUSTOM_MAX ?
                 "You have the most custom loadouts there can be." :
                 g_loState !=
                 LO_LIST ?
                 "New loadouts are made in GEAR UP from the menu." :
                 "Make a new custom loadout. SELECT to name it, then choose its gear.");
    else {
        {
            char gp[120];
            n += snprintf(b + n, SNLEFT(b, n), "%s",
                          GlDetailPrefix(pv, gp, sizeof gp));
        }
        snprintf(b + n, SNLEFT(b, n), "%s",
                 LoCls(pv) >=
                 0 ? CLASSES[LoCls(pv)].
                 desc : "Your own loadout. SELECT to change its gear.");
    }
    {                           // the clan's supply perks (clanperk.c): "YOUR CLAN SENT: medkit x2 (Tyler), ..." under the equipped loadout's description
        const char *cpl = pv == g_loActive ? CpGearupLine() : "";
        char cb[420];
        if (cpl[0]) {
            snprintf(cb, sizeof cb, "%s\n\n%s", b, cpl);
            UiText("anim-group.lst.lsDesc", cb);
        } else
            UiText("anim-group.lst.lsDesc", b);
    }
    UiText("anim-group.lst.lsCur", pv == g_loActive ? "This is your current loadout" : "");   // (2013: under the gear of the equipped loadout)
    GuLpStr(b, sizeof b,
              pv_new ? 13 - LoCost(&nl) + Sk4Cost(&nl) : GuLeftLp(l));
    UiText("anim-group.lst.lsLP", b);  // (no text pop: the panel tween covers the change)
    UiVisible("anim-group.pRenIc", 0);
    UiVisible("anim-group.pRenTx", 0);
    if (GN.on && GN.mode == 1) {        // the CREATE LOADOUT name box: its own prompts (CREATE / CANCEL / DELETE, the device's glyphs)
        UiVisible("anim-group.pRstIc", 1);
        UiVisible("anim-group.pRstTx", 1);
        UiText("anim-group.pSelTx", "CREATE");
        UiText("anim-group.pBackTx", "CANCEL");
        UiText("anim-group.pRstTx", "DELETE");
        GuFit("anim-group.pHint",
               GN.
               pad ? "UP / DOWN  LETTER     LEFT / RIGHT  MOVE" :
               "TYPE A NAME     ENTER  CREATE     ESC  CANCEL",
               GU_HINT_W_REN, 38.0f);
        return;
    }
    const int lst_i = GuPosIdx(G.lst);
    UiText("anim-group.pSelTx",
            G.lst == g_loN ? "CREATE" : g_loState == LO_LIST
            && LoSlot(lst_i) >= 0 ? "EDIT" : "EQUIP");
    GuFit("anim-group.pHint",
           g_padMode ? "STICK / D-PAD  TO MOVE" : g_loState == LO_LIST
           && LoSlot(lst_i) >=
           0 ? "W / S  TO MOVE     CLICK AGAIN TO EDIT" : "W / S  TO MOVE",
           GU_HINT_W, 38.0f);
    UiVisible("anim-group.pRstIc", 0);
    UiVisible("anim-group.pRstTx", 0);
    UiText("anim-group.pBackTx", "BACK");
}

// ------------------------------------------------------------------ edit view
static void GuFillSlots(const LoadoutT *cur)
{
    char p[128], b[48];
    for (int s = 0; s < GU_SLOTS; s++) {
        const char *ic;
        int cost;
        const char *n = GuSlotItem(s, cur, &ic, &cost);
        snprintf(p, sizeof p, "anim-group.ed.sl%d.slIc%d", s, s);
        UiImage(p, n ? ic : GU_NONE_ICON);
        snprintf(p, sizeof p, "anim-group.ed.sl%d.slNm%d", s, s);
        GuFit(p, n ? n : GuNoneName(s), 470.0f, 40.0f);
        snprintf(p, sizeof p, "anim-group.ed.sl%d.slCo%d", s, s);
        if (n)
            snprintf(b, sizeof b, "%d", cost);
        else
            b[0] = 0;
        UiText(p, b);
        snprintf(p, sizeof p, "anim-group.ed.sl%d.slFill%d", s, s);
        GuFade(p, s == G.slot || s == g_gu_hov_slot);
        snprintf(p, sizeof p, "anim-group.ed.sl%d.slFrame%d", s, s);
        GuFade(p, s == G.slot && G.col == 0);
        snprintf(p, sizeof p, "anim-group.ed.sl%d.slArrow%d", s, s);
        GuFade(p, s == G.slot);
    }
}

// one option row's texts: name, cost / state word, icon, equipped, dimmed
typedef struct {
    char nm[80], co[40];
    const char *ic;
    int eq, dim;
} GuOrowT;
static void GuOptRow(int s, const GuOptT *o, const LoadoutT *cur,
                       GuOrowT *r)
{
    memset(r, 0, sizeof *r);
    r->ic = GU_NONE_ICON;
    r->eq = GuOptEq(s, o, cur);
    if (o->kind == GO_NONE) {
        snprintf(r->nm, sizeof r->nm, "%s", GuNoneName(s));
        snprintf(r->co, sizeof r->co, "%s", r->eq ? "EQUIPPED" : "FREE");
        return;
    }
    int cost = 0;
    LoadoutT t = *cur;
    if (o->kind == GO_SKILL) {
        int id = (int) o->row, lv = g_guLvl[id], sk = s - GS_SK1, in =
            SkInSlot(id);
        cost = SK[id].cost[lv - 1];
        snprintf(r->nm, sizeof r->nm, "%s%s%s", SK[id].name,
                 SK[id].nlv > 1 ? " " : "",
                 SK[id].nlv > 1 ? SK_ROMAN[lv] : "");
        r->ic = SK[id].icon;
        if (!(SK[id].avail >> (lv - 1) & 1)) {
            snprintf(r->co, sizeof r->co, "LOCKED");
            r->dim = 1;
        } else if (in >= 0 && in != sk) {
            snprintf(r->co, sizeof r->co, "IN SKILL %d", in + 1);
            r->dim = 1;
        } else if (r->eq)
            snprintf(r->co, sizeof r->co, "EQUIPPED");
        else {
            GuCostStr(r->co, sizeof r->co, cost);
            r->dim = SkCostWith(sk, SK_V(id, lv)) > 13;
        }
        return;
    }
    int ws = GuWs(s);
    // one row per gun: the plain gun's name and icon whichever variant is equipped (the toggle / details show the variant)
    const uint8_t *row = GuRow(ws, o->kind == GO_GUN
                                && o->sj >= 0 ? o->row : GuOptRowNow(s,
                                                                        o,
                                                                        cur));
    snprintf(r->nm, sizeof r->nm, "%s", GuStr(*(const char **) row));
    r->ic =
        *(const char **) (row + 8) ? *(const char **) (row +
                                                       8) : GU_EMPTY_ICON;
    cost = (int) *(const uint32_t *) (row + 0x18);
    if (r->eq)
        snprintf(r->co, sizeof r->co, "EQUIPPED");
    else {
        GuCostStr(r->co, sizeof r->co, cost);
        t.w[ws] = o->row;
        r->dim = LoCost(&t) > 13;
    }
}

static void GuDumpHandles(void);
static void GuFillOpts(const LoadoutT *cur)
{
    char p[128];
    UiText("anim-group.ed.edHead", GuHead(G.slot));
    for (int k = 0; k < GU_OPT_ROWS; k++) {
        int i = G.off + k;
        snprintf(p, sizeof p, "anim-group.ed.op%d", k);
        UiVisible(p, i < g_guNopt);
        if (i >= g_guNopt)
            continue;
        GuOrowT r;
        GuOptRow(G.slot, &g_guOpt[i], cur, &r);
        int foc = G.col == 1 && i == G.opt;
        snprintf(p, sizeof p, "anim-group.ed.op%d.opNm%d", k, k);
        {                       // the cost / state word is fitted first (right pivot: "ZABLOKOWANE" shrinks toward the row end), the name gets what is left of the row
            float cw = GuEst(r.co, 36.0f);
            if (cw > 250.0f)
                cw = 250.0f * 1.0f;
            else if (cw < 70.0f)
                cw = 70.0f;
            GuFit(p, r.nm, 882.0f - 150.0f - cw - 30.0f, 38.0f);
        }
        snprintf(p, sizeof p, "anim-group.ed.op%d.opCo%d", k, k);
        GuFit(p, r.co, 250.0f, 36.0f);
        GuPop(p, r.co);
        snprintf(p, sizeof p, "anim-group.ed.op%d.opIc%d", k, k);
        UiImage(p, r.ic);
        snprintf(p, sizeof p, "anim-group.ed.op%d.opEq%d", k, k);
        UiVisible(p, r.eq);
        snprintf(p, sizeof p, "anim-group.ed.op%d.opFill%d", k, k);
        GuFade(p, foc || i == g_gu_hov_opt);
        snprintf(p, sizeof p, "anim-group.ed.op%d.opFrame%d", k, k);
        GuFade(p, foc);
        snprintf(p, sizeof p, "anim-group.ed.op%d", k);
        WAlpha(UiW(p), r.dim ? 0.45f : 1.0f);
    }
    UiVisible("anim-group.ed.opUp", G.off > 0);
    UiVisible("anim-group.ed.opDn", G.off + GU_OPT_ROWS < g_guNopt);
    GuDumpHandles();
}

// dev (t2r-gu-trace.txt next to the exe): the 7th visible option row's name stays blank in every tab. Logs every page widget handle once
// per page and flags two paths that resolve to the same widget (a write to one would overwrite the other).
static void GuDumpHandles(void)
{
    static int done;
    if (done)
        return;
    FILE *f = ExeSideOpen("t2r-gu-trace.txt", "r");
    if (!f)
        return;
    fclose(f);
    done = 1;
    static const char *SUB[] =
        { "opNm", "opCo", "opIc", "opEq", "opFill", "opFrame", "opHit" };
    char paths[96][80];
    uint64_t hw[96];
    int n = 0;
    for (int k = 0; k < GU_OPT_ROWS; k++) {
        snprintf(paths[n], 80, "anim-group.ed.op%d", k);
        hw[n] = UiW(paths[n]);
        n++;
        for (int j = 0; j < 7; j++) {
            snprintf(paths[n], 80, "anim-group.ed.op%d.%s%d", k, SUB[j],
                     k);
            hw[n] = UiW(paths[n]);
            n++;
        }
    }
    static const char *DT[] =
        { "anim-group.ed.dt.dtIc", "anim-group.ed.dt.dtNm",
"anim-group.ed.dt.dtCo", "anim-group.ed.dt.dtDesc", "anim-group.ed.dt.dtVar",
        "anim-group.ed.edHead", "anim-group.ed.opUp", "anim-group.ed.opDn"
    };
    for (int j = 0; j < 8 && n < 96; j++) {
        snprintf(paths[n], 80, "%s", DT[j]);
        hw[n] = UiW(paths[n]);
        n++;
    }
    for (int i = 0; i < n; i++) {
        Xlog("gearup13: gu trace: handle %s = %llx", paths[i],
             (unsigned long long) hw[i]);
        for (int j = 0; j < i; j++)
            if (hw[i] && hw[i] == hw[j])
                Xlog("gearup13: gu trace: SAME WIDGET %s == %s (%llx)",
                     paths[i], paths[j], (unsigned long long) hw[i]);
    }
}

// the HUD ammo icon of a purchasable's ammo (the textures the in-game weapon cross / ammo counter use), by weapon id
static const char *GuAmmoIcon(const char *id)
{
    if (!id)
        return "hud/weapons/ammo/ammo-rifle-light.png";
    if (strstr(id, "explosive"))
        return "hud/weapons/ammo/ammo-arrow-explosive_small.png";
    if (strstr(id, "bow"))
        return "hud/weapons/ammo/ammo-arrow_small.png";
    if (strstr(id, "flame"))
        return "hud/weapons/ammo/ammo-firethrower_health-hud.png";
    if (strstr(id, "shotgun"))
        return "hud/weapons/ammo/ammo-shotgun_small.png";
    if (strstr(id, "pistol") || strstr(id, "revolver"))
        return "hud/weapons/ammo/ammo-pistol-light.png";
    return "hud/weapons/ammo/ammo-rifle-light.png";
}

static void GuFillDetails(const LoadoutT *cur)
{
    char p[128], b[700];
    const GuOptT *o = G.opt >= 0
        && G.opt < g_guNopt ? &g_guOpt[G.opt] : NULL;
    int s = G.slot, show_st = 0, show_lv = 0, show_tg = 0, tgv =
        -1, show_pu = 0;
    unsigned pu_cost = 0, pu_acost = 0, pu_start = 0;
    const char *pu_ic = GU_NONE_ICON, *pu_am = NULL;
    char tg_co[2][24] = { "", "" };
    int v[4] = { 0, 0, 0, 0 };
    const char *nm = "", *ic = GU_NONE_ICON, *ds = "", *var = "";
    char cs[40] = "", vr[96] = "";
    if (!o || o->kind == GO_NONE) {
        nm = GuNoneName(s);
        snprintf(b, sizeof b, "%s",
                 s ==
                 GS_PURCH ?
                 "No purchasable: you keep your points for the other slots."
                 : s >= GS_SK1
                 && s <=
                 GS_SK4 ?
                 "Nothing in this survival skill slot: the points stay for the rest of the loadout."
                 :
                 "Nothing in this slot: the points stay for the rest of the loadout.");
        ds = b;
        snprintf(cs, sizeof cs, "FREE");
    } else if (o->kind == GO_SKILL) {
        int id = (int) o->row, lv = g_guLvl[id];
        nm = SK[id].name;
        ic = SK[id].icon;
        snprintf(cs, sizeof cs,
                 (SK[id].
                  avail >> (lv - 1) & 1) ? "%s  %u LP" : "%s  LOCKED",
                 SK_ROMAN[lv], SK[id].cost[lv - 1]);
        show_lv = 1;
    } else {
        int ws = GuWs(s);
        const uint32_t er = GuOptRowNow(s, o, cur);
        const uint8_t *row = GuRow(ws, er), *base = GuRow(ws, o->base);
        nm = GuStr(*(const char **) row);
        ic = *(const char **) (row + 8) ? *(const char **) (row +
                                                            8) :
            GU_EMPTY_ICON;
        GuCostStr(cs, sizeof cs, (int) *(const uint32_t *) (row + 0x18));
        if (o->kind == GO_GUN) {
            const char *d0 = GuStr(*(const char **) (row + 0x28));
            if (o->sj >= 0 && er == (uint32_t) o->sj)
                snprintf(b, sizeof b,
                         "%s%sHides shots from appearing on enemy radar.",
                         d0, d0[0] ? "\n" : "");
            else
                snprintf(b, sizeof b, "%s", d0);
            ds = b;
            StatValues(GuStr(*(const char **) (base + 0x10)), v);
            show_st = 1;
            if (o->sj >= 0) {   // the toggle under the description: UNSUPPRESSED / SUPPRESSED with their prices
                show_tg = 1;
                tgv = GuOptVar(s, o, cur);
                GuCostStr(tg_co[0], sizeof tg_co[0],
                            GuRowCost(ws, o->row));
                GuCostStr(tg_co[1], sizeof tg_co[1],
                            GuRowCost(ws, (uint32_t) o->sj));
            }
        } else {                // purchasable: its blurb + what it costs in a match (the picker's text, feature_purch_details)
            const char *d0 = GuStr(*(const char **) (row + 0x30));
            uint32_t start = *(const uint32_t *) (row + 0x1c), wcost =
                *(const uint32_t *) (row + 0x20), acost =
                *(const uint32_t *) (row + 0x24), per =
                *(const uint32_t *) (row + 0x28);
            // the prices are a block of widgets (dtPu: item -> parts, ammo -> parts, Starts with: ammo n), the text keeps one sentence
            start =
                SpawnStartRounds(GuStr(*(const char **) (row + 0x10)),
                                   start);
            snprintf(b, sizeof b,
                     "%s%sOnce bought it is yours again at every spawn.",
                     d0, d0[0] ? "\n\n" : "");
            show_pu = 1;
            pu_cost = wcost;
            pu_acost = (acost && per) ? acost : 0;
            pu_start = pu_acost || start ? start : 0;
            pu_ic = ic;
            pu_am = GuAmmoIcon(GuStr(*(const char **) (row + 0x10)));
            ds = b;
        }
    }
    (void) cur;
    var = vr;
    UiImage("anim-group.ed.dt.dtIc", ic);
    GuFit("anim-group.ed.dt.dtNm", nm, 900.0f, 52.0f);
    UiText("anim-group.ed.dt.dtCo", cs);
    UiText("anim-group.ed.dt.dtDesc", show_lv ? "" : ds);
    UiText("anim-group.ed.dt.dtVar", var);
    UiVisible("anim-group.ed.dt.dtSt", show_st);
    UiVisible("anim-group.ed.dt.dtTg", show_tg);
    UiVisible("anim-group.ed.dt.dtPu", show_pu);
    if (show_pu) {
        char nb[24];
        UiImage("anim-group.ed.dt.dtPu.puRow0.puIc0", pu_ic);
        snprintf(nb, sizeof nb, "%u", pu_cost);
        UiText("anim-group.ed.dt.dtPu.puRow0.puN0", nb);
        UiVisible("anim-group.ed.dt.dtPu.puRow0", pu_cost != 0);
        UiVisible("anim-group.ed.dt.dtPu.puRow1", pu_acost != 0);
        if (pu_acost) {
            UiImage("anim-group.ed.dt.dtPu.puRow1.puIc1", pu_am);
            snprintf(nb, sizeof nb, "%u", pu_acost);
            UiText("anim-group.ed.dt.dtPu.puRow1.puN1", nb);
        }
        UiVisible("anim-group.ed.dt.dtPu.puStLb", pu_start != 0);
        UiVisible("anim-group.ed.dt.dtPu.puStIc", pu_start != 0);
        UiVisible("anim-group.ed.dt.dtPu.puStN", pu_start != 0);
        if (pu_start) {
            UiText("anim-group.ed.dt.dtPu.puStLb", "Starts with:");
            UiImage("anim-group.ed.dt.dtPu.puStIc", pu_am);
            snprintf(nb, sizeof nb, "%u", pu_start);
            UiText("anim-group.ed.dt.dtPu.puStN", nb);
        }
    }
    for (int k = 0; k < 2; k++) {
        snprintf(p, sizeof p, "anim-group.ed.dt.dtTg.tgB%d.tgNm%d", k, k);
        GuFit(p, k ? "SUPPRESSED" : "UNSUPPRESSED", 320.0f, 30.0f);
        snprintf(p, sizeof p, "anim-group.ed.dt.dtTg.tgB%d.tgCo%d", k, k);
        UiText(p, tg_co[k]);
        snprintf(p, sizeof p, "anim-group.ed.dt.dtTg.tgB%d.tgFrame%d", k,
                 k);
        GuFade(p, show_tg && tgv == k);
        snprintf(p, sizeof p, "anim-group.ed.dt.dtTg.tgB%d.tgFill%d", k,
                 k);
        GuFade(p, show_tg && (tgv == k || g_gu_hov_tg == k));
        snprintf(p, sizeof p, "anim-group.ed.dt.dtTg.tgB%d.tgEq%d", k, k);
        UiVisible(p, show_tg && tgv == k);
    }
    UiVisible("anim-group.ed.dt.dtLv", show_lv);
    if (show_st)
        for (int st = 0; st < 4; st++)
            for (int k = 0; k < 10; k++) {
                snprintf(p, sizeof p, "anim-group.ed.dt.dtSt.stF%d_%d", st,
                         k);
                UiVisible(p, k < v[st]);
            }
    if (show_lv) {
        int id = (int) o->row, sk = s - GS_SK1, lv = g_guLvl[id], in =
            SkInSlot(id);
        uint64_t curv = GuSkVal(cur, sk);
        for (int k = 0; k < 3; k++) {
            int on = k < SK[id].nlv;
            char hd[96];
            if (!on)
                hd[0] = 0;
            else if (!(SK[id].avail >> k & 1))
                snprintf(hd, sizeof hd, "%s  -  LOCKED", SK_ROMAN[k + 1]);
            else if (in == sk && SkLv(curv) == k + 1)
                snprintf(hd, sizeof hd, "%s  -  %u LP  -  EQUIPPED",
                         SK_ROMAN[k + 1], SK[id].cost[k]);
            else
                snprintf(hd, sizeof hd, "%s  -  %u LP", SK_ROMAN[k + 1],
                         SK[id].cost[k]);
            snprintf(p, sizeof p, "anim-group.ed.dt.dtLv.lvHd%d", k);
            UiText(p, hd);
            snprintf(p, sizeof p, "anim-group.ed.dt.dtLv.lvTx%d", k);
            UiText(p, on ? GuStr(SK[id].desc[k]) : "");
            float a = (k + 1 == lv || k == g_gu_hov_lvl) ? 1.0f : 0.45f;
            snprintf(p, sizeof p, "anim-group.ed.dt.dtLv.lvHd%d", k);
            WAlpha(UiW(p), on ? a : 0.0f);
            snprintf(p, sizeof p, "anim-group.ed.dt.dtLv.lvTx%d", k);
            WAlpha(UiW(p), on ? a : 0.0f);
        }
    }
}

static void GuFillEdit(void)
{
    LoadoutT cur;
    char b[96];
    GuReadCur(&cur);
    GuFillSlots(&cur);
    GuFillOpts(&cur);
    if (!G.det_hold)
        GuFillDetails(&cur);
    GuLpStr(b, sizeof b, GuLeftLp(&cur));
    UiText("anim-group.ed.edLP", b);
    GuPop("anim-group.ed.edLP", b);
    UiVisible("anim-group.pRstIc", 1);
    UiVisible("anim-group.pRstTx", 1);
    UiVisible("anim-group.pRenIc", !GN.on);
    UiVisible("anim-group.pRenTx", !GN.on);
    UiVisible("anim-group.nb", GN.on); // RENAME: the NAME YOUR LOADOUT box (as CREATE LOADOUT's)
    if (GN.on) {                // RENAME: the footer prompts are the name's (SAVE / CANCEL / DELETE, the device's own glyphs)
        UiText("anim-group.pSelTx", "SAVE");
        UiText("anim-group.pBackTx", "CANCEL");
        UiText("anim-group.pRstTx", "DELETE");
        GuFit("anim-group.pHint",
               GN.
               pad ? "UP / DOWN  LETTER     LEFT / RIGHT  MOVE" :
               "TYPE A NAME     ENTER  SAVE     ESC  CANCEL",
               GU_HINT_W_REN, 38.0f);
        return;
    }
    UiText("anim-group.pSelTx", G.col == 0 ? "CHOOSE" : "EQUIP");
    UiText("anim-group.pBackTx", "BACK");
    UiText("anim-group.pRstTx", "RESET LOADOUT");
    UiText("anim-group.pRenTx", "RENAME");
    const GuOptT *o = G.opt >= 0
        && G.opt < g_guNopt ? &g_guOpt[G.opt] : NULL;
    if (g_padMode)             // the pad's words (the stick / d-pad are menu-up / down / left / right)
        GuFit("anim-group.pHint",
               G.col == 0 ? "STICK / D-PAD  TO MOVE     RIGHT  OPTIONS" : o
               && o->kind == GO_SKILL
               && SK[o->row].nlv >
               1 ? "STICK / D-PAD  TO MOVE     LEFT / RIGHT  LEVEL" : o
               && o->kind == GO_GUN
               && o->sj >=
               0 ? "STICK / D-PAD  TO MOVE     LEFT / RIGHT  SUPPRESSOR" :
               "STICK / D-PAD  TO MOVE     LEFT  SLOTS", GU_HINT_W, 38.0f);
    else
        GuFit("anim-group.pHint",
               G.col == 0 ? "W / S  TO MOVE     D  OPTIONS" : o
               && o->kind == GO_SKILL
               && SK[o->row].nlv >
               1 ? "W / S  TO MOVE     A / D  LEVEL" : o
               && o->kind == GO_GUN
               && o->sj >=
               0 ? "W / S  TO MOVE     A / D  SUPPRESSOR" :
               "W / S  TO MOVE     A  SLOTS", GU_HINT_W, 38.0f);
}

// ------------------------------------------------------------------ equipping (real data: the lobby object, sk_slot_set, the saved loadout)
static void GuCommit(void)
{                               // an edit is done: the custom loadout is saved NOW (feature_loadout_persist only looks twice a second - a class picked
    LoadoutT cur;              // right after would overwrite the lobby object first)
    GuReadCur(&cur);
    uint8_t *ob = LobbyObj();
    if (ob)
        *(uint64_t *) (ob + 0x2c70) = cur.k[0] | cur.k[1] | cur.k[2];
    LjUserEdit();
    if (LoSlot(g_loActive) >= 0 && LoValid(&cur)) {
        g_lo[LoSlot(g_loActive)] = cur;
        LoSave();
        Xlog("gearup13: custom %d saved (%d LP)", LoSlot(g_loActive) + 1,
             LoCost(&cur));
    }
    G.dirty = 1;
}

static void GuSfx(const char *name)
{
    UiSfx(name);
}

static void GuEquip(int s, const GuOptT *o)
{
    LoadoutT cur;
    uint8_t *ob = LobbyObj();
    GuReadCur(&cur);
    if (!ob)
        return;
    if (o->kind == GO_SKILL) {
        int sk = s - GS_SK1, id = (int) o->row, lv = g_guLvl[id];
        uint64_t v = SK_V(id, lv), now = SkNorm(SkSlotGet(sk));
        int other = SkInSlot(id);
        char dm[160];
        if (!(SK[id].avail >> (lv - 1) & 1)) {
            snprintf(dm, sizeof dm, "%s %s is not available yet.",
                     SK[id].name, SK_ROMAN[lv]);
            UiDeny(dm);
            Xlog("gearup13: skill %d %s %s refused (locked)", sk + 1,
                 SK[id].name, SK_ROMAN[lv]);
            return;
        }
        if (other >= 0 && other != sk) {
            snprintf(dm, sizeof dm, "%s is already in Survival Skill %d.",
                     SK[id].name, other + 1);
            UiDeny(dm);
            Xlog("gearup13: skill %d %s refused (in skill %d)", sk + 1,
                 SK[id].name, other + 1);
            return;
        }
        if (now == v) {
            GuSfx("sfx-select");
            Xlog("gearup13: select ignored: skill %d already holds %s %s",
                 sk + 1, SK[id].name, SK_ROMAN[lv]);
            return;
        }
        if (SkCostWith(sk, v) > 13) {
            UiDenyLp(SK[id].name, (int) SK[id].cost[lv - 1],
                       13 - SkCostWith(sk, 0));
            Xlog("gearup13: skill %d %s %s refused (points)", sk + 1,
                 SK[id].name, SK_ROMAN[lv]);
            return;
        }
        SkSlotSet(sk, v);
        SkSound(1);
        Xlog("gearup13: skill %d = %s %s", sk + 1, SK[id].name,
             SK_ROMAN[lv]);
    } else if (o->kind == GO_NONE && s >= GS_SK1 && s <= GS_SK4) {
        int sk = s - GS_SK1;
        if (!SkNorm(SkSlotGet(sk))) {
            GuSfx("sfx-select");
            Xlog("gearup13: select ignored: skill %d is already empty",
                 sk + 1);
            return;
        }
        SkSlotSet(sk, 0);
        GuSfx("sfx-select");
        Xlog("gearup13: skill %d emptied", sk + 1);
    } else {
        int ws = GuWs(s);
        uint32_t want = o->kind == GO_NONE ? 0xffffffffu : o->row;
        if (cur.w[ws] == want
            || (o->kind == GO_NONE && !GuRowOk(ws, cur.w[ws]))) {
            GuSfx("sfx-select");
            Xlog("gearup13: select ignored: slot %d already holds row %u",
                 ws, want);
            return;
        }                       // already what the slot holds
        LoadoutT t = cur;
        t.w[ws] = want;
        if (LoCost(&t) > 13) {
            t.w[ws] = 0xffffffffu;
            const uint8_t *row = GuRow(ws, want);
            UiDenyLp(GuStr(*(const char **) row),
                       (int) *(const uint32_t *) (row + 0x18),
                       13 - LoCost(&t));
            Xlog("gearup13: slot %d row %u refused (points: %d LP)", ws,
                 want, LoCost(&cur));
            return;
        }
        *(uint32_t *) (ob + 0x2c58 + 4 * ws) = want;
        GuSfx("sfx-select");
        Xlog("gearup13: %s = row %d (%s)",
             ws == 0 ? "large" : ws == 1 ? "small" : "purchasable",
             (int) want,
             want ==
             0xffffffffu ? "none" : GuStr(*(const char **)
                                           GuRow(ws, want)));
    }
    GuCommit();
}

// the suppressed / plain variant of the focused gun: equipped like a click on an option (same lobby write + save), the options list stays
static void GuSetVariant(int s, const GuOptT *o, int v)
{
    GuOptT t = *o;
    if (v && o->sj >= 0)
        t.row = (uint32_t) o->sj;
    GuEquip(s, &t);
}

// RESET LOADOUT (2013 edit page, square): the custom loadout back to its empty default (9mm only, no skills) - the same lobby-object write + commit
// path a click on an option uses. There is no engine action known for it: the prompt listens to menu-action, the R key and a click on the prompt.
static void GuReset(void)
{
    LoadoutT d;
    memset(&d, 0, sizeof d);
    d.w[0] = 0xffffffffu;
    d.w[1] = 0;
    d.w[2] = 0xffffffffu;
    LoadoutT cur;
    GuReadCur(&cur);
    if (cur.w[0] == d.w[0] && cur.w[1] == d.w[1] && cur.w[2] == d.w[2]
        && !(cur.k[0] | cur.k[1] | cur.k[2]) && !SkNorm(SkSlotGet(3))) {
        GuSfx("sfx-select");
        return;
    }
    LoWriteObj(&d);
    SkSlotSet(3, 0);
    GuSfx("sfx-select");
    Xlog("gearup13: custom %d reset to the empty loadout",
         g_loActive + 1);
    GuCommit();
    GuEnterSlot(G.slot);      // the options of the focused slot again, the equipped one focused
}

// ------------------------------------------------------------------ list actions
static void GuToEdit(void)
{
    G.view = 1;
    G.col = 0;
    G.closing = 0;
    G.view_anim = !GuNoanim();
    G.view_t0 = GetTickCount64();
    G.det_key = -1;
    G.det_ph = 0;
    G.det_hold = 0;
    GuEnterSlot(GS_LARGE);
    G.dirty = 1;
    UiVisible("anim-group.lst", 0);
    UiVisible("anim-group.ed", 1);
    Xlog("gearup13: edit view (custom %d)", LoSlot(g_loActive) + 1);
}

static void GuToList(const char *why)
{
    G.view_anim = !GuNoanim();
    G.view_t0 = GetTickCount64();
    G.view = 0;
    G.col = 0;
    G.lst = GuIdxPos(g_loActive);
    GuScrollTo(G.lst);
    G.dirty = 1;
    UiVisible("anim-group.lst", 1);
    UiVisible("anim-group.ed", 0);
    Xlog("gearup13: list view (%s)", why);
}

static void GuListSelect(int r, int edit)
{                               // edit 0: only choose the loadout (mouse first click), 1: choose + open the editor
    char b[48];
    if (!LoCustom(r) && LoCls(r) < 0)
        return;
    if (GlPickRefused(r))
        return;                 // gunlimit.c: a gun limited per team that a teammate has taken - denied buzzer + message, nothing equipped
    const LoadoutT *l = LoOf(r);
    if (g_loState != LO_MATCH && LoCls(r) >= 0 && !LoValid(l)) {
        UiDeny("That loadout can't be equipped right now.");
        Xlog("gearup13: %s refused by lo_valid (%d LP)",
             LoName(r, b, sizeof b), LoCost(l));
        return;
    }
    g_loActive = r;
    // mid-match: winmm's store / our box ammo read the lobby object, so the pick waits for the next spawn (lo_snap_tick keeps this life's loadout)
    if (g_loState == LO_MATCH)
        g_loPending = r + 1;
    else if (LoValid(l))
        LoWriteObj(l);
    else
        LoWriteObjForced(l); // (a custom loadout with an unresolved row: edit it anyway)
    LoSave();
    GuSfx("sfx-select");
    Xlog("gearup13: %s selected (state %d, %d LP)",
         LoName(r, b, sizeof b), g_loState, LoCost(l));
    if (g_loState == LO_MATCH) {       // applies at the next respawn (2013)
        PopPage();             // the list
        PopPage();             // the pause menu under it: back to the game
        g_loState = LO_NONE;
        G.closing = 1;
        char m[96];
        snprintf(m, sizeof m,
                 "%s equipped - you'll spawn with it next life", LoName(r,
                                                                         b,
                                                                         sizeof
                                                                         b));
        MsgHook(m, 3.0f, 0, 0);
        return;
    }
    if (edit && LoSlot(r) >= 0 && g_loState == LO_LIST)
        GuToEdit();
    else
        G.dirty = 1;
}

static void GnCreateBegin(void);
// a list position chosen (SELECT / click): CREATE LOADOUT opens the name box, anything else is gu_list_select
static void GuPosSelect(int pos, int edit)
{
    if (pos != g_loN) {
        GuListSelect(GuPosIdx(pos), edit);
        return;
    }
    if (g_loN >= N_CUSTOM_MAX) {
        UiDeny("You have the most custom loadouts there can be.");
        Xlog("gearup13: CREATE LOADOUT refused (%d custom loadouts)",
             g_loN);
        return;
    }
    if (g_loState != LO_LIST) {
        UiDeny("New loadouts are made in GEAR UP from the menu.");
        Xlog("gearup13: CREATE LOADOUT refused (state %d)", g_loState);
        return;
    }
    GnCreateBegin();
}

// CREATE LOADOUT confirmed in the name box: a new custom slot (9mm only, no skills), named, equipped, and its editor opened
static void GuCreate(const char *nm)
{
    if (g_loN >= N_CUSTOM_MAX)
        return;
    const int k = g_loN;
    g_lo[k] = (LoadoutT) { {
    0xffffffffu, 0, 0xffffffffu}, {
    0, 0, 0}};
    *Sk4Slot(&g_lo[k]) = 0;
    g_loN++;
    LoNmSet(k, "", "create (clean slot)");
    LoNmSet(k, nm, "gear up create");
    LoSave();
    Sk4Save();
    LjUserEdit();             // (t2r-loadout.json gets the new loadout on the next lj_tick: the launcher adopts it)
    Xlog("gearup13: custom %d created (%d custom loadouts)", k + 1,
         g_loN);
    GuListSelect(LoSlotIdx(k), 1);
    G.lst = GuIdxPos(g_loActive);
    GuScrollTo(G.lst);
}

// ------------------------------------------------------------------ mouse (from the click gate, once a frame)
static int Gu13ScanReal(void);
// perf: the scan armed + hit-tested ~30 widgets by path string EVERY frame (2 engine script calls + 3 name lookups each). The answer only changes when the
// cursor moves, a button is down, or the layout / focus changes, so it is reused (and the cursor outside the window = nothing hovered, no widget touched);
// a full re-scan at least every 15 frames (arms new rows too).
static int Gu13Scan(void)
{
    static float lcx = -1.0f, lcy = -1.0f;
    static int lkey = -1, lhit;
    static uint64_t lfr;
    static struct {
        int lst, slot, opt, lvl, back, sel, rst, tg, ren;
    } Sv;
    float cx, cy;
    if (!UiCursorCanvas(&cx, &cy)) {
        g_guHovLst = g_gu_hov_slot = g_gu_hov_opt = g_gu_hov_lvl =
            g_gu_hov_tg = -1;
        g_gu_hov_back = g_gu_hov_sel = g_gu_hov_rst = g_gu_hov_ren = 0;
        lkey = -1;
        return 0;
    }
    int key =
        G.view | G.off << 1 | G.opt << 9 | g_guNopt << 17 | G.
        slot << 25 | G.coff << 28;
    if (cx == lcx && cy == lcy && key == lkey && !G.dirty
        && g_frame - lfr < 15
        && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
        g_guHovLst = Sv.lst;
        g_gu_hov_slot = Sv.slot;
        g_gu_hov_opt = Sv.opt;
        g_gu_hov_lvl = Sv.lvl;
        g_gu_hov_back = Sv.back;
        g_gu_hov_sel = Sv.sel;
        g_gu_hov_rst = Sv.rst;
        g_gu_hov_tg = Sv.tg;
        g_gu_hov_ren = Sv.ren;
        return lhit;
    }
    lhit = Gu13ScanReal();
    lcx = cx;
    lcy = cy;
    lkey = key;
    lfr = g_frame;
    Sv.lst = g_guHovLst;
    Sv.slot = g_gu_hov_slot;
    Sv.opt = g_gu_hov_opt;
    Sv.lvl = g_gu_hov_lvl;
    Sv.back = g_gu_hov_back;
    Sv.sel = g_gu_hov_sel;
    Sv.rst = g_gu_hov_rst;
    Sv.tg = g_gu_hov_tg;
    Sv.ren = g_gu_hov_ren;
    return lhit;
}

// The nearest clickable rect to the cursor (canvas units, distance 0 = inside). kind: 0 list row / 1 slot / 2 option (idx = row index in the full list) /
// 3 level line / 4 toggle button. Rows are laid out with gaps (option rows 18 units, slots 15) and a click that lands in one hit NOTHING: a live log
// had "click ignored: nothing under the cursor" while the lists were being clicked. A click within GU_SNAP of a rect now counts as on it.
#define GU_SNAP 36.0f
static float GuNearest(float cx, float cy, int *kind, int *idx, char *nm,
                        size_t nmn, float *rc)
{
    float best = 1e9f;
    char p[96];                 // (the widget paths are < 60 chars; 96 = the caller's nm buffer, so the copy below cannot truncate)
    *kind = -1;
    *idx = -1;
    if (nm)
        nm[0] = 0;
#define GU_CAND(K, I, ...) do { snprintf(p, sizeof p, __VA_ARGS__); uint64_t w_ = UiW(p); float x0, y0, x1, y1;         if (w_ && UiRectSs(w_, &x0, &y0, &x1, &y1) && x1 > x0 && y1 > y0) {             float dx = cx < x0 ? x0 - cx : cx > x1 ? cx - x1 : 0.0f, dy = cy < y0 ? y0 - cy : cy > y1 ? cy - y1 : 0.0f, d = sqrtf(dx * dx + dy * dy);             if (d < best) { best = d; *kind = (K); *idx = (I); if (nm) snprintf(nm, nmn, "%s", p); if (rc) { rc[0] = x0; rc[1] = y0; rc[2] = x1; rc[3] = y1; } } } } while (0)
    if (G.view == 0) {
        for (int w = 0; w < GU_N_ROWW; w++)
            if (GuWidPos(w) >= 0)
                GU_CAND(0, GuWidPos(w), "anim-group.lst.lsRow%d.lsHit%d",
                        w, w);
        GU_CAND(0, g_loN, "anim-group.lst.lsNew.lsNewHit");
    } else {
        for (int s_ = 0; s_ < GU_SLOTS; s_++)
            GU_CAND(1, s_, "anim-group.ed.sl%d.slHit%d", s_, s_);
        for (int k = 0; k < GU_OPT_ROWS && G.off + k < g_guNopt; k++)
            GU_CAND(2, G.off + k, "anim-group.ed.op%d.opHit%d", k, k);
        const GuOptT *o = G.opt >= 0
            && G.opt < g_guNopt ? &g_guOpt[G.opt] : NULL;
        if (o && o->kind == GO_SKILL)
            for (int k = 0; k < SK[o->row].nlv && k < 3; k++)
                GU_CAND(3, k, "anim-group.ed.dt.dtLv.lvHit%d", k);
        if (o && o->kind == GO_GUN && o->sj >= 0)
            for (int k = 0; k < 2; k++)
                GU_CAND(4, k, "anim-group.ed.dt.dtTg.tgB%d.tgHit%d", k, k);
    }
#undef GU_CAND
    return best;
}

static int Gu13ScanReal(void)
{
    char p[128];
    int hit = 0;
    g_guHovLst = g_gu_hov_slot = g_gu_hov_opt = g_gu_hov_lvl = -1;
    g_gu_hov_back = UiMouseOver("anim-group.pBackHit", 1);
    g_gu_hov_sel = UiMouseOver("anim-group.pSelHit", 1);
    g_gu_hov_rst = (G.view == 1 || (GN.on && GN.mode == 1))
        && UiMouseOver("anim-group.pRstHit", 1);
    g_gu_hov_ren = G.view == 1 && !GN.on
        && UiMouseOver("anim-group.pRenHit", 1);
    g_gu_hov_tg = -1;
    if (G.view == 0) {
        for (int w = 0; w < GU_N_ROWW; w++) {
            if (GuWidPos(w) < 0)
                continue;
            snprintf(p, sizeof p, "anim-group.lst.lsRow%d.lsHit%d", w, w);
            if (UiMouseOver(p, 1)) {
                g_guHovLst = GuWidPos(w);
                break;
            }
        }
        if (g_guHovLst < 0
            && UiMouseOver("anim-group.lst.lsNew.lsNewHit", 1))
            g_guHovLst = g_loN;
        hit = g_guHovLst >= 0;
    } else {
        for (int s = 0; s < GU_SLOTS; s++) {
            snprintf(p, sizeof p, "anim-group.ed.sl%d.slHit%d", s, s);
            if (UiMouseOver(p, 1)) {
                g_gu_hov_slot = s;
                break;
            }
        }
        for (int k = 0; k < GU_OPT_ROWS && G.off + k < g_guNopt; k++) {
            snprintf(p, sizeof p, "anim-group.ed.op%d.opHit%d", k, k);
            if (UiMouseOver(p, 1)) {
                g_gu_hov_opt = G.off + k;
                break;
            }
        }
        const GuOptT *o = G.opt >= 0
            && G.opt < g_guNopt ? &g_guOpt[G.opt] : NULL;
        if (o && o->kind == GO_SKILL)
            for (int k = 0; k < SK[o->row].nlv && k < 3; k++) {
                snprintf(p, sizeof p, "anim-group.ed.dt.dtLv.lvHit%d", k);
                if (UiMouseOver(p, 1)) {
                    g_gu_hov_lvl = k;
                    break;
                }
            }
        if (o && o->kind == GO_GUN && o->sj >= 0)
            for (int k = 0; k < 2; k++) {
                snprintf(p, sizeof p,
                         "anim-group.ed.dt.dtTg.tgB%d.tgHit%d", k, k);
                if (UiMouseOver(p, 1)) {
                    g_gu_hov_tg = k;
                    break;
                }
            }
        hit = g_gu_hov_slot >= 0 || g_gu_hov_opt >= 0 || g_gu_hov_lvl >= 0
            || g_gu_hov_tg >= 0;
    }
    if (g_gu_hov_sel || g_gu_hov_rst || g_gu_hov_ren)
        hit = 1;
    if (!hit && !g_gu_hov_back) {       // between rows: snap to the nearest rect within GU_SNAP
        float cx, cy, rc[4];
        int kd, ix;
        if (UiCursorCanvas(&cx, &cy)
            && GuNearest(cx, cy, &kd, &ix, NULL, 0, rc) <= GU_SNAP
            && kd >= 0) {
            if (kd == 0)
                g_guHovLst = ix;
            else if (kd == 1)
                g_gu_hov_slot = ix;
            else if (kd == 2)
                g_gu_hov_opt = ix;
            else if (kd == 3)
                g_gu_hov_lvl = ix;
            else
                g_gu_hov_tg = ix;
            hit = 1;
        }
    }
    return hit;
}

// ------------------------------------------------------------------ the driver
static int GuRep(int *held, uint64_t *t0, uint64_t sid)
{                               // menu action edge + key repeat (first after 400 ms, then every 110 ms)
    uint64_t now = GetTickCount64();
    int on =
        ((char (*)(uint64_t)) T2R_SYM(kFnActionActive)) (sid) != 0, fire =
        0;
    if (on && !*held) {
        fire = 1;
        *t0 = now + 400;
    } else if (on && *held && now >= *t0) {
        fire = 1;
        *t0 = now + 110;
    }
    *held = on;
    return fire;
}

static void GuCycle(void)
{                               // focus moved: the menu's focus sound (sfx-cycle), at most every 60 ms
    uint64_t now = GetTickCount64();
    if (now - G.sfx_ms >= 60) {
        G.sfx_ms = now;
        UiSfx("sfx-cycle");
    }
}

// ------------------------------------------------------------------ RENAME (custom loadouts can be named both in game and in the launcher)
// The edit view's title becomes the name being typed (t2r-ext.c lo_nm_*: one cleaning rule with the launcher). Started by the RENAME prompt
// (menu-next: RB / the bumper glyph; the E key; a click on the prompt). While a name is typed:
//   keyboard  a low-level keyboard hook (kt_*) eats every key down (the engine sees none: W / S / Backspace / Esc / Enter do not move the menu
//             or pop the page) and turns it into text with the player's own layout (ToUnicodeEx: AltGr Polish letters work). Enter saves,
//             Esc cancels, Backspace / Delete, Left / Right / Home / End move the cursor. Modifiers, Alt+Tab and the Windows key pass through.
//   pad       up / down change the letter under the cursor (GN_PAD letters), left / right move it, SELECT saves, the menu-action button
//             deletes, menu-back cancels (the footer prompts read SAVE / DELETE / CANCEL with the device's own glyphs).
//   mouse     a click on SAVE saves, on CANCEL cancels, on DELETE deletes (the RENAME prompt hides while a name is typed).
// The hook lives only while a name is typed (+1.5 s), and passes everything through once the page stops polling it (500 ms): a stuck page can
// never leave the keyboard dead. The ups of eaten downs are eaten too (the engine never sees an up without its down).
static volatile LONG g_ktState;
static volatile ULONGLONG g_ktSeen;
static struct {
    volatile LONG w, r;
    struct {
        uint16_t vk, ch;
    } q[64];
} g_kt;
static volatile uint8_t g_ktDown[256];
static void KtPush(int vk, uint16_t ch)
{                               // (two producers: the hook thread and the game thread's raw input; a key reaches only one of them)
    static volatile LONG lock;
    while (InterlockedCompareExchange(&lock, 1, 0))
        YieldProcessor();
    LONG w = g_kt.w;
    if (w - g_kt.r < 64) {
        g_kt.q[w & 63].vk = (uint16_t) vk;
        g_kt.q[w & 63].ch = ch;
        InterlockedExchange(&g_kt.w, w + 1);
    }
    InterlockedExchange(&lock, 0);
}

static int KtFg(void)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

static LRESULT CALLBACK KtLlProc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION) {
        const KBDLLHOOKSTRUCT *k = (const KBDLLHOOKSTRUCT *) lp;
        int vk = (int) (k->vkCode & 0xFF);
        int down = wp == WM_KEYDOWN
            || wp == WM_SYSKEYDOWN, up = wp == WM_KEYUP
            || wp == WM_SYSKEYUP;
        if (up && g_ktDown[vk]) {
            g_ktDown[vk] = 0;
            return 1;
        }
        int mod = vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT
            || vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL
            || vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU
            || vk == VK_LWIN || vk == VK_RWIN || vk == VK_CAPITAL
            || vk == VK_NUMLOCK;
        int ctl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0, alt =
            (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        int win =
            ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) &
             0x8000) != 0;
        if (down && !mod && g_ktOn && GetTickCount64() - g_ktSeen < 500
            && !(alt && !ctl) && !win && KtFg()) {
            BYTE ks[256];
            memset(ks, 0, sizeof ks);
            if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
                ks[VK_SHIFT] = ks[VK_LSHIFT] = 0x80;
            if (ctl)
                ks[VK_CONTROL] = ks[VK_LCONTROL] = 0x80;
            if (alt)
                ks[VK_MENU] = ks[VK_RMENU] = 0x80;
            if (GetKeyState(VK_CAPITAL) & 1)
                ks[VK_CAPITAL] = 1;
            WCHAR wb[4] = { 0 };
            HKL hkl =
                GetKeyboardLayout(GetWindowThreadProcessId
                                  (GetForegroundWindow(), NULL));
            int r =
                ToUnicodeEx((UINT) vk, k->scanCode, ks, wb, 4,
                            4
                            /* do not change the keyboard state (dead keys) */
                            , hkl);
            KtPush(vk, r == 1 ? (uint16_t) wb[0] : 0);
            g_ktDown[vk] = 1;
            return 1;
        }
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

static DWORD WINAPI KtThread(void *arg)
{
    (void) arg;
    HHOOK h =
        SetWindowsHookExW(WH_KEYBOARD_LL, KtLlProc,
                          GetModuleHandleW(NULL), 0);
    Xlog("gearup13: rename keyboard hook %s", h ? "installed" : "FAILED");
    MSG m;
    if (h)
        SetTimer(NULL, 1, 250, NULL);
    while (h && GetMessageW(&m, NULL, 0, 0) > 0) {
        if (m.message == WM_TIMER && GetTickCount64() - g_ktSeen > 1500)
            break;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (h)
        UnhookWindowsHookEx(h);
    InterlockedExchange(&g_ktState, 0);
    return 0;
}

// While a name is typed, the keyboard only types (no menu navigation by letter keys) until Enter: the game reads the keyboard through raw
// input (tlou-ii.exe imports RegisterRawInputDevices + GetRawInputData, no DirectInput / GetAsyncKeyState). On some systems the hook above was
// installed but never ate a key (W / S walked the letters as menu-up / down, Esc popped as menu-back). So the game's own read is gated too:
// its GetRawInputData import is pointed here, and while a name is typed every keyboard down (and the up of an eaten down) becomes text for the
// box and reaches the game as "no key" (VKey 0xFF, overrun make code). Modifiers, Alt+key, the Windows key and the ups of keys the game saw
// down pass. Mouse and pad input are untouched. A key the hook ate never comes here (no double letters). Log 'gearup13: raw input gate ...'.
static volatile uint8_t g_ktRdown[256];
typedef UINT(WINAPI * fn_grid) (HRAWINPUT, UINT, LPVOID, PUINT, UINT);
static fn_grid g_gridReal;
static int KtRawEat(RAWKEYBOARD *kb)
{
    int vk = kb->VKey & 0xFF, up = (kb->Flags & RI_KEY_BREAK) != 0;
    if (!vk || vk == 0xFF)
        return 0;
    if (up) {
        if (g_ktRdown[vk]) {
            g_ktRdown[vk] = 0;
            return 1;
        }
        return 0;
    }
    if (!g_ktOn || GetTickCount64() - g_ktSeen >= 500)
        return 0;
    int mod = vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT
        || vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL
        || vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU
        || vk == VK_LWIN || vk == VK_RWIN || vk == VK_CAPITAL
        || vk == VK_NUMLOCK;
    if (mod)
        return 0;
    int ctl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0, alt =
        (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    int win =
        ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000)
        != 0;
    if ((alt && !ctl) || win)
        return 0;
    BYTE ks[256];
    memset(ks, 0, sizeof ks);
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
        ks[VK_SHIFT] = ks[VK_LSHIFT] = 0x80;
    if (ctl)
        ks[VK_CONTROL] = ks[VK_LCONTROL] = 0x80;
    if (alt)
        ks[VK_MENU] = ks[VK_RMENU] = 0x80;
    if (GetKeyState(VK_CAPITAL) & 1)
        ks[VK_CAPITAL] = 1;
    WCHAR wb[4] = { 0 };
    UINT sc = kb->MakeCode | ((kb->Flags & RI_KEY_E0) ? 0xE000 : 0);
    int r = ToUnicodeEx((UINT) vk, sc, ks, wb, 4, 4, GetKeyboardLayout(0));
    KtPush(vk, r == 1 ? (uint16_t) wb[0] : 0);
    g_ktRdown[vk] = 1;
    static int n;
    if (LOGCAP(n, 3))
        Xlog("gearup13: raw input gate - key %02x taken for the name (the hook did not see it)", vk);
    return 1;
}

static UINT WINAPI KtGridHook(HRAWINPUT h, UINT cmd, LPVOID data,
                                PUINT sz, UINT hs)
{
    UINT r = g_gridReal(h, cmd, data, sz, hs);
    if (cmd != RID_INPUT || !data || r == (UINT) - 1
        || r < sizeof(RAWINPUTHEADER) + sizeof(RAWKEYBOARD))
        return r;
    RAWINPUT *ri = (RAWINPUT *) data;
    if (ri->header.dwType == RIM_TYPEKEYBOARD
        && KtRawEat(&ri->data.keyboard)) {
        ri->data.keyboard.VKey = 0xFF;
        ri->data.keyboard.MakeCode = KEYBOARD_OVERRUN_MAKE_CODE;
        ri->data.keyboard.Message = 0;
    }
    return r;
}

static void KtRawInstall(void)
{                               // once: the game's GetRawInputData import -> kt_grid_hook
    static int done;
    if (done)
        return;
    done = 1;
    uint8_t *b = (uint8_t *) GetModuleHandleW(NULL);
    FARPROC real =
        GetProcAddress(GetModuleHandleA("user32.dll"), "GetRawInputData");
    const IMAGE_NT_HEADERS64 *nt =
        (const IMAGE_NT_HEADERS64 *) (b +
                                      ((const IMAGE_DOS_HEADER *) b)->
                                      e_lfanew);
    IMAGE_DATA_DIRECTORY dd =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    void **slot = NULL;
    for (const IMAGE_IMPORT_DESCRIPTOR * id =
         dd.VirtualAddress ? (const IMAGE_IMPORT_DESCRIPTOR *)(b +
                                                               dd.
                                                               VirtualAddress)
         : NULL; id && id->Name && !slot; id++) {
        if (_stricmp((const char *) (b + id->Name), "user32.dll"))
            continue;
        IMAGE_THUNK_DATA64 *ft =
            (IMAGE_THUNK_DATA64 *) (b + id->FirstThunk);
        const IMAGE_THUNK_DATA64 *ot =
            id->OriginalFirstThunk ? (const IMAGE_THUNK_DATA64 *) (b +
                                                                   id->
                                                                   OriginalFirstThunk)
            : NULL;
        for (int i = 0; ft[i].u1.Function && !slot; i++) {
            if (ot && !IMAGE_SNAP_BY_ORDINAL64(ot[i].u1.Ordinal)
                &&
                !strcmp((const char
                         *) ((const IMAGE_IMPORT_BY_NAME *) (b +
                                                             ot[i].u1.
                                                             AddressOfData))->
                        Name, "GetRawInputData"))
                slot = (void **) &ft[i].u1.Function;
            else if (real && (FARPROC) ft[i].u1.Function == real)
                slot = (void **) &ft[i].u1.Function;
        }
    }
    if (!slot) {
        Xlog("gearup13: raw input gate NOT installed (no GetRawInputData import found) - only the keyboard hook eats keys");
        return;
    }
    DWORD old;
    if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) {
        Xlog("gearup13: raw input gate NOT installed (import not writable)");
        return;
    }
    g_gridReal = (fn_grid) * slot;
    InterlockedExchangePointer(slot, (void *) KtGridHook);
    VirtualProtect(slot, sizeof *slot, old, &old);
    Xlog("gearup13: raw input gate installed (import at exe+%llx, was %p, user32 %p)", (unsigned long long) ((uint8_t *) slot - b), (void *) g_gridReal, (void *) real);
}

static void KtKeepalive(void)
{                               // every frame while a name is typed
    g_ktSeen = GetTickCount64();
    if (!g_ktState && InterlockedCompareExchange(&g_ktState, 1, 0) == 0) {
        HANDLE t = CreateThread(NULL, 0, KtThread, NULL, 0, NULL);
        if (t)
            CloseHandle(t);
        else
            InterlockedExchange(&g_ktState, 0);
    }
}

static int KtNext(int *vk, uint32_t *ch)
{
    LONG r = g_kt.r;
    if (r == g_kt.w)
        return 0;
    *vk = g_kt.q[r & 63].vk;
    *ch = g_kt.q[r & 63].ch;
    InterlockedExchange(&g_kt.r, r + 1);
    return 1;
}

static const char GN_PAD_UTF8[] =
    " ABCDEFGHIJKLMNOPQRSTUVWXYZ\xC4\x84\xC4\x86\xC4\x98\xC5\x81\xC5\x83\xC3\x93\xC5\x9A\xC5\xB9\xC5\xBB"
    "0123456789-.'!?#&+";
static uint32_t g_gnPad[64];
static int g_gnNpad;
static int GnOk(uint32_t c)
{                               // a letter the name can hold (upper case already)
    char o[8];
    int n = LoNmPut(o, 0, c);
    o[n] = 0;
    char t[LO_NM_BYTES + 2];
    return c != ' ' ? LoNmClean(o, t, -1) == 1 : 1;
}

static int GnBytes(void)
{
    int b = 0;
    for (int i = 0; i < GN.n; i++)
        b += GN.cp[i] < 0x80 ? 1 : 2;
    return b;
}

static void GnUtf8(char *out, int from, int to)
{                               // cp[from..to) -> UTF-8
    int o = 0;
    for (int i = from; i < to && i < GN.n; i++)
        o = LoNmPut(out, o, GN.cp[i]);
    out[o] = 0;
}

static int GnInsert(uint32_t c)
{
    c = LoNmUp(c);
    if (!c || !GnOk(c))
        return 0;
    if (GN.n >= LO_NM_MAX || GnBytes() + (c < 0x80 ? 1 : 2) > LO_NM_BYTES) {
        UiSfx("sfx-menu-error");
        return 0;
    }
    memmove(&GN.cp[GN.cur + 1], &GN.cp[GN.cur],
            (size_t) (GN.n - GN.cur) * sizeof GN.cp[0]);
    GN.cp[GN.cur++] = c;
    GN.n++;
    return 1;
}

static void GnDelete(int before)
{
    int at = before ? GN.cur - 1 : GN.cur;
    if (at < 0 || at >= GN.n)
        return;
    memmove(&GN.cp[at], &GN.cp[at + 1],
            (size_t) (GN.n - at - 1) * sizeof GN.cp[0]);
    GN.n--;
    if (before)
        GN.cur--;
}

static void GnPadLetter(int dir)
{                               // pad up / down: the letter under the cursor walks GN_PAD (at the end: a new letter)
    if (!g_gnNpad) {
        const uint8_t *p = (const uint8_t *) GN_PAD_UTF8;
        while (*p && g_gnNpad < 64)
            p = LoNmDec(p, &g_gnPad[g_gnNpad++]);
    }
    if (GN.cur >= GN.n) {
        if (!GnInsert(dir > 0 ? 'A' : '+'))
            return;
        GN.cur--;
        return;
    }
    int k = 0;
    for (int i = 0; i < g_gnNpad; i++)
        if (g_gnPad[i] == GN.cp[GN.cur]) {
            k = i;
            break;
        }
    for (int t = 0; t < g_gnNpad; t++) {
        k = (k + (dir > 0 ? 1 : g_gnNpad - 1)) % g_gnNpad;
        uint32_t c = g_gnPad[k];
        int w = (c < 0x80 ? 1 : 2) - (GN.cp[GN.cur] < 0x80 ? 1 : 2);
        if (GnBytes() + w <= LO_NM_BYTES) {
            GN.cp[GN.cur] = c;
            return;
        }
    }
}

static void GnStart(int mode, const char *init, const char *how)
{
    const uint8_t *p = (const uint8_t *) init;
    memset(&GN, 0, sizeof GN);
    GN.mode = mode;
    while (*p && GN.n < LO_NM_MAX)
        p = LoNmDec(p, &GN.cp[GN.n++]);
    GN.cur = GN.n;
    GN.sel = GN.dflt = mode == 1 && GN.n > 0;
    GN.on = 1;
    GN.t0 = GetTickCount64();
    GN.f0 = GN.click_f = g_frame;
    GN.pad = how[0] == 'p';
    GN.h_u = GN.h_d = GN.h_l = GN.h_r = GN.h_sel = GN.h_act = GN.h_back = GN.h_nx = 1;  // whatever is held now
    GN.t_u = GN.t_d = ~0ULL;
    g_kt.r = g_kt.w;
    KtRawInstall();
    InterlockedExchange(&g_ktOn, 1);
    KtKeepalive();
    UiSfx("sfx-select");
    G.dirty = 1;
    if (mode == 1)
        Xlog("gearup13: CREATE LOADOUT name box (custom %d, %s)",
             g_loN + 1, how);
    else
        Xlog("gearup13: rename custom %d started (%s)",
             LoSlot(g_loActive) + 1, how);
}

// RENAME (naming loadouts 1-6 and CREATE LOADOUT must work the same way): RENAME used to type into the page title in place (an unnamed loadout started EMPTY, a named one appended at its end), while CREATE LOADOUT
// (loadouts 7-12) opens the NAME YOUR LOADOUT box with the name selected. RENAME now opens the same box: prefilled with the loadout's name (an unnamed
// one: its default label CUSTOM LOADOUT n, in the player's language), all selected - typing replaces it, Backspace empties it, the arrows keep it.
// The default label kept untouched saves as the default (an empty name), as in CREATE LOADOUT.
static void GnBegin(const char *how)
{
    LoNmLoad();
    const int k = LoSlot(g_loActive);
    char d[32], c[LO_NM_BYTES + 2];
    const int dflt = k < 0 || !g_loNm[k][0];
    if (dflt) {
        snprintf(d, sizeof d, "CUSTOM LOADOUT %d", k + 1);
        const char *tr = LocTr(d);
        LoNmClean(tr ? tr : d, c, -1);
    }
    GnStart(0, dflt ? c : g_loNm[k], how);
    GN.sel = GN.n > 0;
    GN.dflt = dflt && GN.n > 0;
}

static void GnCreateBegin(void)
{                               // CREATE LOADOUT: the box opens on the default name, selected
    char d[32], c[LO_NM_BYTES + 2];
    snprintf(d, sizeof d, "CUSTOM LOADOUT %d", g_loN + 1);
    const char *tr = LocTr(d); // (in the player's language; kept unchanged it is saved as the default label)
    LoNmClean(tr ? tr : d, c, -1);
    GnStart(1, c, G.h_sel ? "menu-select" : "click");
}

static void GnUnsel(int clear)
{                               // the prefilled default stops being selected (clear: it is replaced)
    if (!GN.sel)
        return;
    GN.sel = 0;
    if (clear) {
        GN.n = GN.cur = 0;
    }
}

static void GnEnd(int save, const char *how)
{
    if (!GN.on)
        return;
    InterlockedExchange(&g_ktOn, 0);
    GN.on = 0;
    const int mode = GN.mode;
    char nm[LO_NM_BYTES * 2 + 8];
    GnUtf8(nm, 0, GN.n);
    if (GN.dflt)
        nm[0] = 0;              // (the default untouched: the default label)
    if (save && mode == 1)
        GuCreate(nm);
    else if (save) {
        if (LoNmSet(LoSlot(g_loActive), nm, "gear up"))
            LjUserEdit();     // (t2r-loadout.json is rewritten by the next lj_tick: the launcher adopts the name)
    }
    UiSfx("sfx-select");
    // the menu edges seen while typing belong to the name: start every held state over
    G.h_sel = G.h_u = G.h_d = G.h_l = G.h_r = G.h_back = G.h_act =
        G.h_esc = G.h_r_key = 1;
    G.t_u = G.t_d = ~0ULL;
    G.dirty = 1;
    if (mode == 1)
        Xlog("gearup13: CREATE LOADOUT %s (%s)",
             save ? "confirmed" : "cancelled", how);
    else
        Xlog("gearup13: rename custom %d %s (%s)",
             LoSlot(g_loActive) + 1, save ? "saved" : "cancelled", how);
}

// one frame of typing; returns 1 while the name owns the input (the rest of the page's input is skipped)
static int GnTick(int click)
{
    if (!GN.on)
        return 0;
    if (GN.mode == 0
        && (G.view != 1 || g_loState != LO_LIST
            || LoSlot(g_loActive) < 0)) {
        GnEnd(0, "edit view left");
        return 0;
    }
    if (GN.mode == 1
        && (G.view != 0 || g_loState != LO_LIST
            || g_loN >= N_CUSTOM_MAX)) {
        GnEnd(0, "list left");
        return 0;
    }
    KtKeepalive();
    int vk;
    uint32_t ch;
    int moved = 0;
    while (KtNext(&vk, &ch)) {
        moved = 1;
        GN.pad = 0;
        if (vk == VK_RETURN) {
            GnEnd(1, "Enter");
            return 1;
        }
        if (vk == VK_ESCAPE) {
            GnEnd(0, "Esc");
            return 1;
        }
        if (vk == VK_BACK || vk == VK_DELETE) {
            if (GN.sel)
                GnUnsel(1);
            else
                GnDelete(vk == VK_BACK);
            GN.dflt = 0;
        } else if (vk == VK_LEFT) {
            GnUnsel(0);
            if (GN.cur > 0)
                GN.cur--;
        } else if (vk == VK_RIGHT) {
            GnUnsel(0);
            if (GN.cur < GN.n)
                GN.cur++;
        } else if (vk == VK_HOME) {
            GnUnsel(0);
            GN.cur = 0;
        } else if (vk == VK_END) {
            GnUnsel(0);
            GN.cur = GN.n;
        } else if (ch >= 0x20) {
            uint32_t u = LoNmUp(ch);
            if (u && GnOk(u)) {
                GnUnsel(1);
                GN.dflt = 0;
            }
            GnInsert(ch);
        }
    }
    // the pad (the keyboard's menu actions never reach the engine while the hook eats them)
    int up = GuRep(&GN.h_u, &GN.t_u, FNV64("menu-up")), dn =
        GuRep(&GN.h_d, &GN.t_d, FNV64("menu-down"));
    int lf = MenuEdge(FNV64("menu-left"), &GN.h_l), rt =
        MenuEdge(FNV64("menu-right"), &GN.h_r);
    int sel =
        MenuEdge(0x6695c5b7921bb5ddULL /* menu-select */ , &GN.h_sel),
        act = MenuEdge(FNV64("menu-action"), &GN.h_act);
    int back = MenuEdge(FNV64("menu-back"), &GN.h_back), nx =
        MenuEdge(FNV64("menu-next"), &GN.h_nx);
    if (click)
        GN.click_f = g_frame;
    if (sel && g_frame - GN.click_f <= 8)
        sel = 0;                // (a left click also sends menu-select, sometimes a frame late: the click alone decides)
    if (!g_padMode)
        up = dn = lf = rt = act = back = nx = sel = 0;  // (the keyboard only types; letters by up / down are the pad's)
    if (up || dn || lf || rt || act) {
        GN.pad = 1;
        moved = 1;
    }
    if (up || dn || act)
        GN.dflt = 0;
    if (act && GN.sel) {
        GnUnsel(1);
        act = 0;
    }                           // (DELETE on the selected default: the box empties)
    if (up || dn || lf || rt)
        GnUnsel(0);
    if (up)
        GnPadLetter(1);
    if (dn)
        GnPadLetter(-1);
    if (lf && GN.cur > 0)
        GN.cur--;
    if (rt && GN.cur < GN.n)
        GN.cur++;
    if (act)
        GnDelete(GN.cur >= GN.n);
    if (moved) {
        UiSfx("sfx-cycle");
        GN.t0 = GetTickCount64();
        G.dirty = 1;
    }
    if (back) {
        g_gu13RepushUntil = g_frame + 10;
        GnEnd(0, "menu-back");
        return 1;
    }                           // (the engine may pop the page on menu-back: it comes back on the same view)
    if ((sel && !click) || nx) {
        GnEnd(1, nx ? "menu-next" : "menu-select");
        return 1;
    }
    if (click && g_gu_hov_sel) {
        GnEnd(1, "SAVE clicked");
        return 1;
    }
    if (click && g_gu_hov_rst) {
        GN.dflt = 0;
        if (GN.sel)
            GnUnsel(1);
        else
            GnDelete(GN.cur >= GN.n);
        UiSfx("sfx-cycle");
        GN.t0 = GetTickCount64();
        G.dirty = 1;
        return 1;
    }
    if (click && g_gu_hov_back) {
        GnEnd(0, "CANCEL clicked");
        return 1;
    }
    return 1;
}

// the title while typing: the name with the cursor (a blinking _ at the end, the letter under it in blue inside the name)
static void GnText(char *t, size_t tn)
{                               // the name with the cursor (a blinking _ at the end, the letter under it in blue; the selected default all blue)
    char a[LO_NM_BYTES * 2 + 8], c[8], z[LO_NM_BYTES * 2 + 8];
    int blink = ((GetTickCount64() - GN.t0) / 530) & 1;
    if (GN.sel) {
        GnUtf8(a, 0, GN.n);
        snprintf(t, tn, "<font color=%c#3fa9f5%c>%s</font>%s", 34, 34, a,
                 blink ? " " : "_");
        return;
    }
    GnUtf8(a, 0, GN.cur);
    if (GN.cur < GN.n) {
        int n = LoNmPut(c, 0, GN.cp[GN.cur]);
        c[n] = 0;
        GnUtf8(z, GN.cur + 1, GN.n);
    } else {
        snprintf(c, sizeof c, "%s", blink ? " " : "_");
        z[0] = 0;
    }
    snprintf(t, tn, "%s<font color=%c#3fa9f5%c>%s</font>%s", a, 34, 34, c,
             z);
}

static void GnTitle(void)
{                               // CREATE LOADOUT and RENAME: the NAME YOUR LOADOUT box's field
    char t[256];
    GnText(t, sizeof t);
    UI_TEXT_RAW("anim-group.nb.nbField", t);
}

static void GuTitle(void)
{
    char b[64];
    if (GN.on)
        GnTitle();             // (the name box: the page title stays)
    if (G.view == 1) {
        UiTextDb("anim-group.title", LoName(g_loActive, b, sizeof b));
        return;
    }
    if (g_loState == LO_CHOOSE) {
        uint8_t *o = LobbyObj();
        int secs = o ? *(int *) (o + 0x1b8) : 0;
        MetaT mt = MetaGet(); // the booster stage + loading follow: MATCH STARTS IN keeps counting the real time to the level start
        int left = (secs >= 100 ? secs - 100 : secs) + (mt.ok ? mt.mf : 0);
        if (left > 99)
            left = 99;
        MfTimerText("anim-group.title", "CHOOSE LOADOUT - ", left);
        return;
    }
    UiTextDb("anim-group.title",
               g_loState == LO_MATCH ? "CHANGE LOADOUT" : "GEAR UP");
}

static void GuEnter(void)
{
    memset(&G, 0, sizeof G);
    G.up = 1;
    G.enter_ms = GetTickCount64();
    G.h_sel = G.h_u = G.h_d = G.h_l = G.h_r = G.h_back = 1;     // whatever is held when the page opens
    G.t_u = G.t_d = ~0ULL;
    G.h_esc = 1;
    G.h_act = 1;
    G.h_r_key = 1;
    G.h_nx = 1;
    G.h_e = 1;
    if (GN.on) {
        InterlockedExchange(&g_ktOn, 0);
        GN.on = 0;
        Xlog("gearup13: rename dropped (page up again)");
    }
    G.hv_slot = G.hv_opt = -1;
    G.det_key = -1;
    G.pre_i = 0;
    G.pv = -1;
    G.pv_ign = -1;
    G.pv_lasthov = -1;
    G.pv_lastlst = -1;
    G.pv_lastwant = -1;
    g_guHovLst = g_gu_hov_slot = g_gu_hov_opt = g_gu_hov_lvl = -1;
    g_gu_hov_back = g_gu_hov_sel = g_gu_hov_rst = 0;
    g_gu_hov_tg = -1;           // (the scan of this frame read the previous instance's view)
    G.lst = GuIdxPos(g_loActive);
    GuScrollTo(G.lst);
    G.dirty = 1;
    if (g_loState == LO_NONE && MatchSubstate() != 6)
        g_loState = LO_LIST;
    GuFadeReset();
    memset(g_gu_fit, 0, sizeof g_gu_fit);
    WAlpha(UiW("anim-group"), 0.0f);  // the page fades in (see the end of feature_gearup13); an instance the match horn faded out starts at 0 as well
    UiVisible("anim-group.lst", 1);
    UiVisible("anim-group.ed", 0);
    UiVisible("anim-group.nb", 0);
    UiText("anim-group.pBackTx", "BACK");
    UiText("anim-group.pSelTx", "SELECT");
    Xlog("gearup13: page up (state %d, active %d)", g_loState,
         g_loActive);
    if (g_guKeep.ok) {         // pushed again after the engine's own pop on menu-back: the edit view where it was
        g_guKeep.ok = 0;
        if (g_guKeep.view == 1 && LoSlot(g_loActive) >= 0
            && g_loState == LO_LIST) {
            G.view = 1;
            G.closing = 0;
            GuEnterSlot(g_guKeep.slot);
            G.col = g_guKeep.col;
            GuFocusOpt(g_guKeep.opt);
            UiVisible("anim-group.lst", 0);
            UiVisible("anim-group.ed", 1);
            Xlog("gearup13: edit view restored (slot %d, %s)", G.slot,
                 G.col ? "options" : "slots");
        }
    }
}

static void GuBack(const char *how)
{
    if (G.view == 1) {
        if (G.col == 1) {
            G.col = 0;
            G.dirty = 1;
            GuCycle();
            Xlog("gearup13: back to the slots (%s)", how);
            return;
        }
        GuToList(how);
        GuCycle();
        return;
    }
    // list: the engine pops a pushed menu page on menu-back by itself; Esc it does not. Judged 6 frames after the press (a second pop would take the page under with it)
    G.back_f = g_frame ? g_frame : 1;
    Xlog("gearup13: back from the list (%s)", how);
}

static void GuLogMiss(const char *where)
{                               // a click that hit nothing: everything needed to see why
    float cx = -1, cy = -1, rc[4] = { 0, 0, 0, 0 };
    int kd, ix;
    char nm[96];
    POINT pt = { -1, -1 };
    RECT cr = { 0, 0, 0, 0 };
    HWND hw = GetForegroundWindow();
    if (hw) {
        GetCursorPos(&pt);
        ScreenToClient(hw, &pt);
        GetClientRect(hw, &cr);
    }
    int in = UiCursorCanvas(&cx, &cy);
    float d = in ? GuNearest(cx, cy, &kd, &ix, nm, sizeof nm, rc) : -1.0f;
    Xlog("gearup13: click ignored: nothing under the cursor (%s; client px %ld,%ld of %ldx%ld; canvas %.0f,%.0f in %d; view %d col %d slot %d opt %d off %d nopt %d; nearest %s [%.0f,%.0f-%.0f,%.0f] dist %.0f)", where, (long) pt.x, (long) pt.y, (long) cr.right, (long) cr.bottom, cx, cy, in, G.view, G.col, G.slot, G.opt, G.off, g_guNopt, in ? nm : "-", rc[0], rc[1], rc[2], rc[3], d);
}

static void FeatureGearup13(void)
{
    g_gu13Up = 0;
    if (!g_gu13)
        return;
    uint64_t top = TopPage();
    if (top != Gu13PageId()) {
        if (G.up) {
            G.up = 0;
            Xlog("gearup13: page left (top now %s, state %d)",
                 PageName(top), g_loState);
            GnEnd(0, "page left");
            // menu-back in the edit view is a native pop of the page: come back on the list
            if (g_gu13RepushUntil && g_frame <= g_gu13RepushUntil
                && (g_loState == LO_LIST) && top) {
                g_guKeep = (typeof(g_guKeep)) {
                1, G.view, G.col, G.slot, G.opt, G.off};
                PushPageNow("to1/gearup13");
                Xlog("gearup13: the engine popped the page on back - pushed again (list view)");
            }
            g_gu13RepushUntil = 0;
        }
        if (g_gu13RepushUntil && g_frame > g_gu13RepushUntil)
            g_gu13RepushUntil = 0;
        return;
    }
    g_gu13Up = 1;
    if (!G.up)
        GuEnter();
    uint64_t now = GetTickCount64();
    // ---- closing (a mid-match pick pops two pages: nothing more to read from this one)
    if (G.closing)
        return;
    if ((g_loState == LO_CHOOSE || g_loState == LO_MATCH) && G.view == 1)
        GuToList("pre-match / mid-match: equip only");
    // ---- input
    int sel =
        MenuEdge(0x6695c5b7921bb5ddULL /* menu-select */ , &G.h_sel);
    int up = GuRep(&G.h_u, &G.t_u, FNV64("menu-up")), down =
        GuRep(&G.h_d, &G.t_d, FNV64("menu-down"));
    int left = MenuEdge(FNV64("menu-left"), &G.h_l), right =
        MenuEdge(FNV64("menu-right"), &G.h_r);
    int back = MenuEdge(FNV64("menu-back"), &G.h_back);
    int rst_a = MenuEdge(FNV64("menu-action"), &G.h_act);
    int r_now = GameFocused()
        && (GetAsyncKeyState('R') & 0x8000) != 0, rst_k = r_now
        && !G.h_r_key;
    G.h_r_key = r_now;
    int esc_now = GameFocused()
        && (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0, esc = esc_now
        && !G.h_esc;
    G.h_esc = esc_now;
    int wn = WheelNotches();
    {
        static int lpd = -1;
        if (lpd != g_padMode) {
            lpd = g_padMode;
            G.dirty = 1;
        }
    }                           // the hint line follows the device in use
    int click = g_clickEdge, lbtn_back = click && g_gu_hov_back;
    int mouse = g_mouseMode;
    int ren_nx = MenuEdge(FNV64("menu-next"), &G.h_nx);
    int e_now = GameFocused()
        && (GetAsyncKeyState('E') & 0x8000) != 0, ren_k = e_now && !G.h_e;
    G.h_e = e_now;
    const int typing = GnTick(click);  // RENAME owns the input while a name is typed
    if (typing)
        sel = up = down = left = right = back = esc = lbtn_back = rst_a =
            rst_k = click = wn = 0;
    if (!typing && G.view == 1 && g_loState == LO_LIST
        && LoSlot(g_loActive) >= 0 && (ren_nx || ren_k
                                         || (click && g_gu_hov_ren))) {
        GnBegin(ren_nx ? "pad: menu-next" : click ? "click on RENAME" :
                 "E key");
        sel = up = down = left = right = back = esc = lbtn_back = rst_a =
            rst_k = click = wn = 0;
    }
    if (back || esc || lbtn_back) {
        if (G.view == 1 && back)
            g_gu13RepushUntil = g_frame + 10; // (the engine may pop the page itself on menu-back)
        GuBack(esc ? "Esc" : lbtn_back ? "BACK clicked" : "menu-back");
        back = esc = lbtn_back = 0;
        sel = 0;
    }
    if (G.back_f && g_frame >= G.back_f + 6 && !GN.on) {
        G.back_f = 0;
        if (TopPage() == Gu13PageId() && G.view == 0 && !G.closing) {
            PopPage();
            G.closing = 1;
            Xlog("gearup13: list closed");
            return;
        }
    }
    if (G.view == 0) {
        const int np = GuNpos();
        if (up) {
            G.lst = (G.lst + np - 1) % np;
            GuScrollTo(G.lst);
            G.dirty = 1;
            GuCycle();
        }
        if (down) {
            G.lst = (G.lst + 1) % np;
            GuScrollTo(G.lst);
            G.dirty = 1;
            GuCycle();
        }
        if (wn && g_loN > GU_CUST_ROWS) {      // wheel: scroll the custom rows (the focus stays; gu_fill_list keeps a focused custom row in view)
            int c = G.coff - wn, mx = g_loN - GU_CUST_ROWS;
            G.coff = c < 0 ? 0 : c > mx ? mx : c;
            if (G.lst < g_loN
                && (G.lst < G.coff || G.lst >= G.coff + GU_CUST_ROWS))
                G.lst =
                    G.lst < G.coff ? G.coff : G.coff + GU_CUST_ROWS - 1;
            G.dirty = 1;
        }
        if (click && g_guHovLst >= 0 && !lbtn_back) {
            // click selects (the right panel + the chosen loadout), a click on the already chosen custom loadout opens its editor
            const int hi = GuPosIdx(g_guHovLst);
            const int again = hi >= 0 && hi == g_loActive
                && g_guHovLst == G.lst && LoSlot(hi) >= 0
                && g_loState == LO_LIST;
            G.lst = g_guHovLst;
            G.dirty = 1;
            G.click_f = g_frame;
            sel = 0;
            Xlog("gearup13: click on list row %d: %s", G.lst,
                 G.lst ==
                 g_loN ? "CREATE LOADOUT" : again ?
                 "already selected, edit" : "select");
            GuPosSelect(G.lst, again);
        }
        if (sel && G.click_f && g_frame - G.click_f <= 8) {
            Xlog("gearup13: menu-select ignored: the click already acted");
            sel = 0;
        }
        if (sel) {
            Xlog("gearup13: select on list row %d (keyboard / pad)",
                 G.lst);
            GuPosSelect(G.lst, 1);
        }
        if (g_loState == LO_MATCH && LoOwnsPurchasable() && GuPosIdx(G.lst) >= 0) {        // rule: a different class costs the purchasable
            LoadoutT c;
            memset(&c, 0xff, sizeof c.w);
            LoReadObj(&c);
            if (c.w[2] != LoOf(GuPosIdx(G.lst))->w[2]) {
                static int last_logged = -2;
                g_loWarnFrame = g_frame ? g_frame : 1;
                if (last_logged != G.lst) {
                    last_logged = G.lst;
                    Xlog("gearup13: purchasable warning shown for row %d (different class, purchasable owned)", G.lst);
                }
            }
        }
    } else {
        const int nopt = g_guNopt;
        if (rst_a || rst_k || (click && g_gu_hov_rst)) {
            GuReset();
            sel = 0;
        }
        if (wn && nopt > GU_OPT_ROWS) { // wheel: scroll the options window; a focus pushed out follows to the edge
            int off = G.off - wn;
            if (off < 0)
                off = 0;
            if (off > nopt - GU_OPT_ROWS)
                off = nopt - GU_OPT_ROWS;
            G.off = off;
            if (G.opt < off)
                G.opt = off;
            else if (G.opt >= off + GU_OPT_ROWS)
                G.opt = off + GU_OPT_ROWS - 1;
            G.dirty = 1;
        }
        // hover changes NOTHING but its own light highlight: focus, tab, column, details and skill level move only on a click or the keyboard / pad
        // a click is acted on HERE, on the element under the cursor, whatever the focus is: the engine's menu-select edge (sel) can arrive a frame late,
        // be latched high by the previous click, or find the focus elsewhere (a level line clicked while the SLOTS column had the focus only moved to the
        // options; the focus kept where the keyboard left it equipped the wrong row). sel of the same click is then dropped.
        if (click && !lbtn_back) {
            const GuOptT *co = G.opt >= 0
                && G.opt < nopt ? &g_guOpt[G.opt] : NULL;
            if (g_gu_hov_lvl >= 0 && co && co->kind == GO_SKILL) {
                int old_lv = g_guLvl[co->row], nlv = g_gu_hov_lvl + 1;
                g_guLvl[co->row] = nlv;        // (tried: a level the player can't have is denied and never selected)
                if (nlv != old_lv
                    && (!(SK[co->row].avail >> (nlv - 1) & 1)
                        || SkCostWith(G.slot - GS_SK1,
                                        SK_V(co->row, nlv)) > 13)) {
                    GuEquip(G.slot, co);       // (the denied feedback: message + buzzer)
                    g_guLvl[co->row] = old_lv;
                    sel = 0;
                    G.click_f = g_frame;
                    Xlog("gearup13: click on level %d of %s: denied, marker stays on %d", nlv, SK[co->row].name, old_lv);
                } else {
                    g_guLvl[co->row] = nlv;
                    G.col = 1;
                    G.dirty = 1;
                    Xlog("gearup13: click on level %d of %s: applied",
                         g_gu_hov_lvl + 1, SK[co->row].name);
                    GuEquip(G.slot, co);
                    sel = 0;
                    G.click_f = g_frame;
                }
            } else if (g_gu_hov_tg >= 0 && co && co->kind == GO_GUN
                       && co->sj >= 0) {
                G.col = 1;
                G.click_f = g_frame;    // (equipped below with the toggle code)
                Xlog("gearup13: click on the %s button: applied",
                     g_gu_hov_tg ? "SUPPRESSED" : "UNSUPPRESSED");
            } else if (g_gu_hov_opt >= 0) {
                G.col = 1;
                GuFocusOpt(g_gu_hov_opt);
                Xlog("gearup13: click on option %d (%s): applied",
                     g_gu_hov_opt, GuHead(G.slot));
                GuEquip(G.slot, &g_guOpt[G.opt]);
                sel = 0;
                G.click_f = g_frame;
            } else if (g_gu_hov_slot >= 0) {
                Xlog("gearup13: click on slot %d: options column",
                     g_gu_hov_slot);
                if (g_gu_hov_slot != G.slot)
                    GuEnterSlot(g_gu_hov_slot);
                G.col = 1;
                G.dirty = 1;
                sel = 0;
                G.click_f = g_frame;
                GuSfx("sfx-select");
            } else if (!g_gu_hov_rst && !g_gu_hov_back && !g_gu_hov_ren)
                GuLogMiss("view 1");
        }
        if (sel && G.click_f && g_frame - G.click_f <= 8) {
            Xlog("gearup13: menu-select ignored: the click already acted (%d frames ago)", (int) (g_frame - G.click_f));
            sel = 0;
        }
        if (G.col == 0) {
            if (up) {
                GuEnterSlot((G.slot + GU_SLOTS - 1) % GU_SLOTS);
                GuCycle();
            }
            if (down) {
                GuEnterSlot((G.slot + 1) % GU_SLOTS);
                GuCycle();
            }
            if (right) {
                G.col = 1;
                G.dirty = 1;
                GuCycle();
            }
            if (sel) {
                G.col = 1;
                G.dirty = 1;
                GuSfx("sfx-select");
                Xlog("gearup13: options of %s", GuHead(G.slot));
            }
        } else {
            const GuOptT *o = G.opt >= 0
                && G.opt < nopt ? &g_guOpt[G.opt] : NULL;
            int skl_multi = o && o->kind == GO_SKILL && SK[o->row].nlv > 1;
            if (up && nopt) {
                GuFocusOpt((G.opt + nopt - 1) % nopt);
                GuCycle();
            }
            if (down && nopt) {
                GuFocusOpt((G.opt + 1) % nopt);
                GuCycle();
            }
            LoadoutT vc;
            GuReadCur(&vc);
            int has_tg = o && o->kind == GO_GUN && o->sj >= 0;
            int tgv = has_tg ? GuOptVar(G.slot, o, &vc) : -1;
            if (has_tg) {       // A / D: UNSUPPRESSED / SUPPRESSED of the focused gun (never leaves the options column)
                if (left && tgv != 0) {
                    GuSetVariant(G.slot, o, 0);
                    GuCycle();
                }
                if (right && tgv != 1) {
                    GuSetVariant(G.slot, o, 1);
                    GuCycle();
                }
                left = right = 0;
                if (click && g_gu_hov_tg >= 0) {
                    if (tgv != g_gu_hov_tg)
                        GuSetVariant(G.slot, o, g_gu_hov_tg);
                    sel = 0;
                }
            }
            if (skl_multi) {    // A / D: the level of the focused skill (the points / lock show on the lines)
                int id = (int) o->row;
                if (left && g_guLvl[id] > 1) {
                    g_guLvl[id]--;
                    G.dirty = 1;
                    GuCycle();
                } else if (left && g_guLvl[id] <= 1) {
                    G.col = 0;
                    G.dirty = 1;
                    GuCycle();
                }
                if (right && g_guLvl[id] < SK[id].nlv) {
                    g_guLvl[id]++;
                    G.dirty = 1;
                    GuCycle();
                }
            } else if (left) {
                G.col = 0;
                G.dirty = 1;
                GuCycle();
            }
            if (sel && o) {
                Xlog("gearup13: select on option %d (keyboard / pad / late edge)", G.opt);
                GuEquip(G.slot, o);
            }
        }
    }
    // ---- fill: on a change, twice a second, and at the page's first moments (its widgets may be built a frame late)
    static const uint32_t OFF_MS[] = { 33, 100, 250, 670, 1500, 3000 };
    if (G.stage < (int) (sizeof OFF_MS / sizeof OFF_MS[0])
        && now - G.enter_ms >= OFF_MS[G.stage]) {
        G.stage++;
        G.dirty = 1;
        GuFadeReassert();
    }
    {                           // the hover highlight follows the cursor: a changed hovered element refills (fades only)
        static int lh[4] = { -9, -9, -9, -9 };
        static unsigned lv;
        if (lh[0] != g_guHovLst || lh[1] != g_gu_hov_slot
            || lh[2] != g_gu_hov_opt || lh[3] != g_gu_hov_tg
            || lv != (unsigned) G.view) {
            lh[0] = g_guHovLst;
            lh[1] = g_gu_hov_slot;
            lh[2] = g_gu_hov_opt;
            lh[3] = g_gu_hov_tg;
            lv = (unsigned) G.view;
            G.dirty = 1;
        }
    }
    if (G.view == 0) {          // which loadout the right panel previews: the hovered row, else the focused one (a keyboard / pad move wins until the mouse enters another row)
        int hv = -1;            // hover only highlights, the details follow the focus / click (G.lst)
        if (G.pv_lastlst != G.lst) {
            G.pv_lastlst = G.lst;
            if (G.pv_lastwant >= 0)
                G.pv_ign = hv;
        }
        if (hv != G.pv_lasthov) {
            G.pv_lasthov = hv;
            if (hv != G.pv_ign)
                G.pv_ign = -2;
        }
        int want = hv >= 0 && hv != G.pv_ign
            && G.pv_ign == -2 ? hv : G.lst;
        if (want != G.pv_lastwant) {
            G.pv_chg = G.pv_ign == -2 ? now : now - 60;
            G.pv_lastwant = want;
        }
        G.pv_want = want;
        if (G.pv < 0)
            G.pv = want;
    } else
        G.pv = -1;
    if (G.dirty || !(g_frame % 90)) {
        G.dirty = 0;
        if (G.view == 0)
            GuFillList();
        else
            GuFillEdit();
    }
    GuFadeTick(now);
    GuAnimTick(now);
    GuPreloadTick();
    {                           // page fade-in: 0 -> 1 over 220 ms, in 1/24 steps (written only when the step changes)
        static int last_q = -1;
        static uint64_t last_enter;
        if (G.enter_ms != last_enter) {
            last_enter = G.enter_ms;
            last_q = -1;
        }
        float f = (float) (now - G.enter_ms) / 220.0f;
        if (f > 1.0f)
            f = 1.0f;
        f = 1.0f - (1.0f - f) * (1.0f - f);
        int q = (int) (f * 24.0f + 0.5f);
        if (q != last_q) {
            last_q = q;
            WAlpha(UiW("anim-group"), (float) q / 24.0f);
        }
    }
    GuTitle();
}
