// ---------------------------------------------------------------- clan perk pop + status list
// Included by t2r-ext.c before clanperk.c (one translation unit). Log prefix 'clancard:'. The pure core (status tail parse, moment gate, card text) is clancard.h, tested by
// tests/clancard_test.c. Python side: tools/features/clancard_widgets.py (widgets), tools/clan_status.py (status wire tail 'sc=..').
//
// 1. PERK POP (clanperk.c cp_announce -> cc_show), look from the mockup clan-perk-pop-v5-zoom.png: NO circle / frame - the member's clan-journal portrait (own soft
//    alpha edges) sits straight on a grainy smoky dark band (clan-pop-band.png), "<NAME> PROVIDED YOU WITH A <ITEM>!" (item word in the perk's colour) + the dim "ROLE - FROM YOUR
//    CLAN" subline to its right and a faint grey item icon at the far end. Left side, mid-height (3840 x 2160 canvas, scales with the resolution). Slides in from the left, holds,
//    fades. ONLY at a spawn (cc_allowed): any other moment is dropped silently. Several perks of one spawn show one after another. Never over the buy hint (g_buyhint_active /
//    g_cc_busy). The stinger and the member's voice line start when the card starts to slide (cp_card_started, clanperk.c).
//    cc_show() returns 1 queued, 0 = no widgets in this pak (the caller falls back to the ntf_push toast), 2 = not a spawn moment (dropped).
// 2. STATUS UPDATES in game: ONLY the ESC (pause) menu list CLAN NEWS (cc_pause_tick; widgets pauseStatus on to1/pause-menu).
// Switches next to the exe: t2r-no-clan-status.txt (no CLAN NEWS list), t2r-no-clan-card.txt (no perk pop card: the toast), t2r-clan-portraits.txt (draw the portrait slot).
#include "clancard.h"
#define CC_QN 4
#define CC_POP_X 60.0f          // resting x of the card (3840-unit canvas, 2400 wide)
// the card sat over the 4v4 team icons. The HUD's left column (2160-unit canvas, ref shot p29g at 2560x1440): team icons + "20:20" end at y 0.473 (1022), the parts counter
// starts at y 0.867 (1872); the card (520 x 0.55 = 286 tall) is centred in that gap: centre (1022 + 1872) / 2 = 1447, top 1447 - 143 = 1304 (0.604 .. 0.736 of the screen height).
// It shares this lane with the buy hint (centre 1370, 170 tall): never at the same time (g_buyhint_active / g_cc_busy, the hint queues behind the card).
#define CC_POP_Y 1304.0f        // = clancard_widgets row y (resting y of the scaled row's top)
#define CC_SLIDE_MS 550
#define CC_FADE_IN_MS 250
#define CC_HOLD_MS 3500         // (was 5000 - it took up too much of the screen)
#define CC_SCALE 0.55f          // the whole card scaled about its top-left (2400 x 520 -> 1320 x 286 canvas units); t2r-clan-card-scale.txt = own factor
#define CC_FADE_OUT_MS 450
static void CpCardStarted(int perk, int female);      // clanperk.c: stinger + the member's voice line (when the card starts to slide)
static CcStatusT g_ccSt;     // the status updates of the last state reply (guarded by g_clan_cs)
static void CcStatusSetFrom(const char *t)
{                               // worker thread (clan_parse)
    CcStatusT s;
    CcParseStatus(t, &s);
    EnterCriticalSection(&g_clanCs);
    g_ccSt = s;
    LeaveCriticalSection(&g_clanCs);
}

static int CcFlag(const char *file, int *c, uint64_t *at)
{
    uint64_t now = GetTickCount64();
    if (*c < 0 || now - *at > 2000) {
        FILE *f = ExeSideOpen(file, "r");
        *c = f != NULL;
        if (f)
            fclose(f);
        *at = now;
    }
    return *c;
}

static int CcStatusOff(void)
{
    static int c = -1;
    static uint64_t at;
    return CcFlag("t2r-no-clan-status.txt", &c, &at);
}

static int CcCardOff(void)
{
    static int c = -1;
    static uint64_t at;
    return CcFlag("t2r-no-clan-card.txt", &c, &at);
}

static int CcPortraitsOn(void)
{
    static int c = -1;
    static uint64_t at;
    return CcFlag("t2r-clan-portraits.txt", &c, &at);
}

static int CcPolish(void)
{
    return g_locCur >= 0 && !strcmp(LOC_LANGS[g_locCur].code, "pl");
}

// ---- the perk pop
// Layers (clancard_widgets.add_perk_pop, bottom to top): popBand smoke, popBig item icon (grey, faint), popPt portrait, popTx text, popSb subline. The reveal is driven per frame
// with alpha / pivot only:
//   0 ms     row slides in from 240 units to the left (ease-out, 550 ms); the band fades in 0-350 ms; the stinger starts (cp_card_started)
//   200 ms   the portrait fades in (350 ms); 500 ms the member's voice line (clanperk.c)
//   350 ms   the text fades in with a 14-unit settle (400 ms), the item word in the perk's colour (<font color> markup); 520 ms the dim subline
//   450 ms   the item icon fades in to 30 %
//   exit     after the 5 s hold: fade 450 ms while sliding 16 units down
enum { PW_ROW, PW_BAND, PW_BIG, PW_PT, PW_TX, PW_SB, PW_N };
static const char *const CC_LEAF[PW_N] =
    { NULL, "popBand", "popBig", "popPt", "popTx", "popSb" };
#define CC_PT_U 380.0f
#define CC_PT_X 120.0f
#define CC_PT_Y ((520.0f - CC_PT_U) / 2.0f)
#define CC_BIG_X 2020.0f
#define CC_BIG_Y (260.0f - 112.5f - 20.0f)
#define CC_TX_X (CC_PT_X + CC_PT_U - 30.0f)
#define CC_TX_Y (260.0f - 80.0f - 50.0f)
#define CC_SB_Y (260.0f - 15.0f - 22.0f)
static int g_ccWait;           // logged: the head of the queue waits for the buy hint
static struct {
    CcCardT c;
} g_ccq[CC_QN];
static int g_ccqn;
static struct {
    int st;
    CcCardT c;
    uint64_t w[PW_N], page, t0, until, set_frame;
    float la[PW_N];
} g_cc;                         // st 0 idle, 1 written (hidden one frame), 2 shown
static int CcResolve(void)
{
    static uint64_t last_try;
    uint64_t page = *(uint64_t *) T2R_SYM(kUiPage);
    if (!page)
        return 0;
    if (page == g_cc.page && g_cc.w[PW_ROW])
        return 1;
    uint64_t now = GetTickCount64();
    if (last_try && now - last_try < 1000)
        return 0;
    last_try = now;
    memset(g_cc.w, 0, sizeof g_cc.w);
    g_cc.page = 0;
    for (int i = 0; i < PW_N; i++) {
        g_cc.w[i] = HudWidget("perkPop", "perkPopRow", CC_LEAF[i]);
        if (!g_cc.w[i]) {
            static int nl;
            if (LOGCAP(nl, 2))
                Xlog("clancard: perkPop/perkPopRow%s%s not found (older gui2-widgets.pak) - perk pops keep the toast", CC_LEAF[i] ? "." : "", CC_LEAF[i] ? CC_LEAF[i] : "");
            memset(g_cc.w, 0, sizeof g_cc.w);
            return 0;
        }
    }
    g_cc.page = page;
    Xlog("clancard: perkPop widgets found (page %llx) - perk pop card on",
         (unsigned long long) page);
    return 1;
}

// the item: background icon (the game's own HUD textures; the food can is ours), the colour of its name + accent line
static const char *CcPerkIcon(int cat)
{
    static const char *const I[5] =
        { "hud/weapons/consumables/health-kit.png",
"hud/weapons/ammo/ammo.png", "hud/resources/rag.png", "streaming/t2r/icons/perk-food.png",
"hud/weapons/consumables/bottle.png" };
    return cat >= 1 && cat <= 5 ? I[cat - 1] : I[0];
}
static const char *const CC_PERK_HEX[5] = { "#D2352C", "#E0A21A", "#B9822A", "#6FB24F", "#4F8FE0" };    // medkit red, ammo amber, crafting ochre, food green, water blue (= clancard_widgets.POP_ACCENT)

static void CcPerkText(char *out, size_t n, const CcCardT *c, int pl)
{
    static const char *const ART[5] = { "A ", "A ", "", "", "" };
    static const char *const EN[5] =
        { "HEALTH KIT", "SPARE CLIP", "CRAFTING SUPPLIES", "FOOD",
"WATER" };
    static const char *const PL[5] =
        { "APTECZKE", "ZAPASOWY MAGAZYNEK", "SKLADNIKI DO SKLADANIA",
"JEDZENIE", "WODE" };
    char nm[32];
    snprintf(nm, sizeof nm, "%s", c->name);
    for (char *q = nm; *q; q++)
        *q = (char) toupper((unsigned char) *q);
    int i = c->icon >= 1 && c->icon <= 5 ? c->icon - 1 : 0;
    if (pl)
        snprintf(out, n, "%s PRZYSLAL%s CI <font color=%c%s%c>%s</font>!",
                 nm, c->female ? "A" : "", 34, CC_PERK_HEX[i], 34, PL[i]);
    else
        snprintf(out, n,
                 "%s PROVIDED YOU WITH %s<font color=%c%s%c>%s</font>!",
                 nm, ART[i], 34, CC_PERK_HEX[i], 34, EN[i]);
}

static void CcPerkSub(char *out, size_t n, const CcCardT *c, int pl)
{                               // "SCAVENGER - FROM YOUR CLAN" (the role word of the member's faction)
    static const char *const W[10] =
        { "SCAVENGER", "MEDIC", "COOK", "GUARD", "MECHANIC", "SCOUT",
"GARDENER", "KID", "OLD-TIMER", "RADIO OPERATOR" };
    static const char *const S[10] =
        { "FORAGER", "HEALER", "COOK", "WATCH", "TOOLMAKER", "ARCHER",
"GARDENER", "CHILD", "ELDER", "WEAVER" };
    static const char *const P[10] =
        { "ZBIERACZ", "MEDYK", "KUCHARZ", "STRAZNIK", "MECHANIK",
"ZWIADOWCA", "OGRODNIK", "DZIECKO", "STARSZYZNA", "RADIOOPERATOR" };
    int r = c->role >= 0 && c->role < 10 ? c->role : 0;
    int sera = g_clan.ok && g_clan.fac == 2;
    snprintf(out, n, "%s - %s", pl ? P[r] : sera ? S[r] : W[r],
             pl ? "Z TWOJEGO KLANU" : "FROM YOUR CLAN");
}

// the API: 1 = queued, 0 = no widgets (use your own fallback), 2 = not a spawn moment (nothing to show)
static int CcShow(const CcCardT *c, int respawn_moment)
{
    if (!CcAllowed(c->kind, MatchSubstate() == 6, respawn_moment))
        return 2;
    if (CcCardOff() || !CcResolve())
        return 0;
    for (int i = 0; i < g_ccqn; i++)
        if (g_ccq[i].c.icon == c->icon
            && !strcmp(g_ccq[i].c.name, c->name))
            return 1;           // the same perk already waits
    if (g_ccqn >= CC_QN) {
        memmove(g_ccq, g_ccq + 1, sizeof g_ccq[0] * (CC_QN - 1));
        g_ccqn = CC_QN - 1;
    }
    g_ccq[g_ccqn++].c = *c;
    g_ccBusy = 1;
    Xlog("clancard: perk pop queued: perk %d from \"%s\" (head %d, %s)",
         c->icon, c->name, c->look, c->female ? "f" : "m");
    return 1;
}

static void CcApply(int i, float a)
{                               // alpha of one leaf, written only on change
    if (HUD_MENU_UP())
        a = 0.0f;               // a menu over the HUD (hud_menu_up): the card keeps its timeline, hidden meanwhile
    if (__builtin_fabsf(a - g_cc.la[i]) > 0.01f) {
        FeedAlpha(g_cc.w[i], a);
        g_cc.la[i] = a;
    }
}

static void CcStart(const CcCardT *c, uint64_t now)
{
    typedef void (*fn_img)(uint64_t, const char *);
    char line[260], sub[96];
    int pl = CcPolish();
    CcPerkText(line, sizeof line, c, pl);
    CcPerkSub(sub, sizeof sub, c, pl);
    ((fn_settext_t) T2R_SYM(kFnSetText)) (g_cc.w[PW_TX], line);
    ((fn_settext_t) T2R_SYM(kFnSetText)) (g_cc.w[PW_SB], sub);
    ((fn_img) T2R_SYM(kFnSetImage)) (g_cc.w[PW_BIG],
                                     CcPerkIcon(c->icon));
    // the card shows the member's clan-journal portrait (c13p-<model>_<expression>.png, the daemon's pkp tail), not the grey silhouette
    int portrait = c->pic[0] && Cl13PortraitsOn();
    if (portrait) {
        char pp[96];
        snprintf(pp, sizeof pp, "streaming/t2r/icons/c13p-%s.png", c->pic);
        ((fn_img) T2R_SYM(kFnSetImage)) (g_cc.w[PW_PT], pp);
    } else
        if ((portrait = CcPortraitsOn() && g_clan.ok && g_clan.fac >= 1
             && g_clan.fac <= 2)) {
        char pp[96];
        snprintf(pp, sizeof pp,
                 "streaming/t2r/portraits/%c/%c/%d_happy.png",
                 g_clan.fac == 2 ? 'S' : 'W', c->female ? 'f' : 'm',
                 c->look & 3);
        ((fn_img) T2R_SYM(kFnSetImage)) (g_cc.w[PW_PT], pp);
    }
    for (int i = 0; i < PW_N; i++) {
        FeedAlpha(g_cc.w[i], 0.0f);
        g_cc.la[i] = 0.0f;
        WVisible(g_cc.w[i], i == PW_PT ? portrait : 1);
    }
    FeedAlpha(g_cc.w[PW_ROW], 0.0f);
    WVisible(g_cc.w[PW_ROW], 0);
    WPivot(g_cc.w[PW_ROW], CC_POP_X - 240.0f, CC_POP_Y);
    {                           // the row, parent of every leaf, is scaled down
        static float sc;
        static uint64_t sat;
        uint64_t n2 = GetTickCount64();
        if (!sc || n2 - sat > 2000) {
            sat = n2;
            sc = CC_SCALE;
            FILE *f = ExeSideOpen("t2r-clan-card-scale.txt", "r");
            if (f) {
                float x = 0;
                if (fscanf(f, "%f", &x) == 1 && x >= 0.2f && x <= 1.5f)
                    sc = x;
                fclose(f);
            }
        }
        WScale(g_cc.w[PW_ROW], sc, sc);
        Xlog("clancard: card scale %.2f", (double) sc);
    }
    g_cc.c = *c;
    g_cc.st = 1;
    g_cc.set_frame = g_frame;
    Xlog("clancard: show \"%s\" / \"%s\"%s", line, sub,
         portrait ? " (portrait)" : "");
}

static void CcEnd(void)
{
    FeedAlpha(g_cc.w[PW_ROW], 0.0f);
    WVisible(g_cc.w[PW_ROW], 0);
    g_cc.st = 0;
}

// ---- the ESC (pause) menu list of status updates (widgets: clancard_widgets.add_pause_status, group pauseStatus under the mission block): the pending updates, one row each,
// <font color> markup per state. The page is rebuilt on every open, so the rows are re-asserted every 3 s and whenever the content changes.
static const char *CcWord(int kind, int female, int pl)
{
    static const char *const EN[CC_KINDS] =
        { "CLAN SUPPLY", "HUNGRY", "GRIEVING", "SICK", "DEAD", "HURT",
"RECOVERED", "NEW MEMBER", "DEPRESSED", "SHOCK" };
    static const char *const PLM[CC_KINDS] =
        { "ZAOPATRZENIE KLANU", "GLODNY", "W ZALOBIE", "CHORY", "NIE ZYJE",
"RANNY", "WYZDROWIAL", "NOWY CZLONEK", "ZALAMANY", "W SZOKU" };
    static const char *const PLF[CC_KINDS] =
        { "ZAOPATRZENIE KLANU", "GLODNA", "W ZALOBIE", "CHORA", "NIE ZYJE",
"RANNA", "WYZDROWIALA", "NOWA CZLONKINI", "ZALAMANA", "W SZOKU" };
    return kind >= 0
        && kind <
        CC_KINDS ? (pl ? (female ? PLF[kind] : PLM[kind]) : EN[kind]) : "";
}

static int g_ccPauseUp;
static void CcPauseTick(void)
{
    static uint64_t grp, at;
    static uint32_t lh;
    static const char *const COL[CC_KINDS] =
        { "#9CA6B5", "#F0B429", "#6C9BF2", "#E8483F", "#9CA6B5", "#E08A3B",
"#6FCF6A", "#5DC0DC", "#7A6FA8", "#C9A0DC" };
    uint64_t g = UiW("anim-group.pauseStatus");
    g_ccPauseUp = g != 0;
    if (!g) {
        grp = 0;
        return;
    }
    CcStatusT s;
    EnterCriticalSection(&g_clanCs);
    s = g_ccSt;
    LeaveCriticalSection(&g_clanCs);
    if (CcStatusOff())
        s.n = 0;
    int pl = CcPolish();
    char row[4][120];
    uint32_t h = 2166136261u;
    int n = s.n > 4 ? 4 : s.n;
    for (int i = 0; i < n; i++) {
        int k = s.e[i].code;
        snprintf(row[i], sizeof row[i], "<font color=%c%s%c>%s</font>  %s",
                 34, COL[k], 34, CcWord(k, s.e[i].female, pl),
                 s.e[i].name);
        for (const char *p = row[i]; *p; p++)
            h = (h ^ (uint8_t) * p) * 16777619u;
    }
    uint64_t now = GetTickCount64();
    if (g == grp && h == lh && now - at < 3000)
        return;
    if (g != grp)
        Xlog("clancard: pause menu list - %d update(s)", n);
    grp = g;
    lh = h;
    at = now;
    if (!n) {
        WVisible(g, 0);
        return;
    }
    for (int i = 0; i < 4; i++) {
        char p[40];
        snprintf(p, sizeof p, "anim-group.pauseStatus.pst%d", i);
        UI_TEXT_RAW(p, i < n ? row[i] : "");
    }
    WVisible(g, 1);
}

static float CcClamp(float k)
{
    return k < 0.0f ? 0.0f : k > 1.0f ? 1.0f : k;
}

static float CcEase(float k)
{
    k = CcClamp(k);
    return 1.0f - (1.0f - k) * (1.0f - k) * (1.0f - k);
}

static float CcSeg(float t, float a, float b)
{
    return CcClamp((t - a) / (b - a));
}                               // 0..1 over [a, b] ms

static void CcAnimate(uint64_t now)
{
    float t = (float) (now - g_cc.t0);
    float exit_k =
        now >
        g_cc.until ? CcClamp((float) (now - g_cc.until) /
                              CC_FADE_OUT_MS) : 0.0f;
    if (exit_k >= 1.0f) {
        CcEnd();
        return;
    }
    float x = CC_POP_X - 240.0f * (1.0f - CcEase(t / CC_SLIDE_MS));
    float master = (t < 120.0f ? t / 120.0f : 1.0f) * (1.0f - exit_k);
    CcApply(PW_ROW, master);
    if (master > 0.0f)
        WVisible(g_cc.w[PW_ROW], 1);
    WPivot(g_cc.w[PW_ROW], x, CC_POP_Y + 16.0f * exit_k);
    CcApply(PW_BAND, CcSeg(t, 0.0f, 350.0f));
    CcApply(PW_PT, CcSeg(t, 200.0f, 550.0f));
    float kt = CcSeg(t, 350.0f, 750.0f), ks = CcSeg(t, 520.0f, 920.0f);
    WPivot(g_cc.w[PW_TX], CC_TX_X - 14.0f * (1.0f - CcEase(kt)),
            CC_TX_Y);
    CcApply(PW_TX, kt);
    WPivot(g_cc.w[PW_SB], CC_TX_X - 14.0f * (1.0f - CcEase(ks)),
            CC_SB_Y);
    CcApply(PW_SB, ks);
    CcApply(PW_BIG, 0.30f * CcSeg(t, 450.0f, 1250.0f));
}

static void CcFrame(void)
{                               // main tick (PF in the tick list), every frame
    uint64_t now = GetTickCount64();
    if (MatchSubstate() != 6) {
        g_cc.st = 0;
        g_ccqn = 0;
        g_ccBusy = 0;
        g_cc.page = 0;
        return;
    }
    // perf (cc_frame() 0.1-0.4 ms avg, spikes 4-7 ms on a Ryzen 5 2600): the pauseStatus lookup is an engine path walk that FAILS (the dearest kind) on every page that is not the ESC menu; it ran in every match every 20 frames. The group only exists on the pause page: no menu over the HUD and the group not up = no lookup.
    if (!(g_frame % 20) && (g_ccPauseUp || HUD_MENU_UP()))
        CcPauseTick();
    uint64_t page = *(uint64_t *) T2R_SYM(kUiPage);
    if (g_cc.st && page != g_cc.page) {
        g_cc.st = 0;
    }                           // a new match built a new page: the widgets are gone
    if (!g_cc.st) {
        if (!g_ccqn) {
            g_ccBusy = 0;
            return;
        }
        g_ccBusy = 1;
        if (g_buyhintActive && !g_ccWait) {
            g_ccWait = 1;
            Xlog("clancard: card waits for the buy hint");
        }
        if (g_buyhintActive || g_downed || g_downDying || HUD_MENU_UP())
            return;             // the lane is the buy hint's while it shows; never over a menu / down
        if (!CcResolve()) {
            g_ccqn = 0;
            g_ccBusy = 0;
            return;
        }
        g_ccWait = 0;
        CcCardT c = g_ccq[0].c;
        memmove(g_ccq, g_ccq + 1, sizeof g_ccq[0] * (size_t) (--g_ccqn));
        CcStart(&c, now);
        return;
    }
    if (g_cc.st == 1) {
        if (g_frame <= g_cc.set_frame)
            return;             // the strings / paths have had no frame yet
        g_cc.st = 2;
        g_cc.t0 = now;
        g_cc.until = now + CC_SLIDE_MS + CC_HOLD_MS;
        CpCardStarted(g_cc.c.icon - 1, g_cc.c.female);        // stinger on the wipe; the voice line follows 0.5 s later, on the headshot
    }
    g_ccBusy = 1;              // the buy hint (t2r-ext feature_buy_hint) stays queued until the card has fully left
    CcAnimate(now);
}
