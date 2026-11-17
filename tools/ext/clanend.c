// clanend.c - the clan end screens (re/win-screen-1112/PLAN.md), included by t2r-ext.c before side_tick.
// to1/clan-side group "end" (tools/features/clanend_widgets.py). When the clan is gone (st 2) or survived 12 weeks (st 3), the side page
// shows this instead of the old end card, paced by the player:
//   1. key art (five parallax layers, re/clan-end/tex) fades in from black, the camera drifting back (win) / creeping in (lost) - never frozen;
//      DAY n + YOUR CLAN SURVIVED / IS GONE, the roll call (daemon op 0xB6 section 7: the survivors who walked out / the last who died,
//      their faces struck through in chalk); CONTINUE fades in and the page waits.
//   2. continue: the UI fades, the camera trucks right (win) / dollies in (lost), the dark rank page closes over it while it still moves.
//   3. win: the old rank is rubbed out to a ghost, the new rank chalked in stroke by stroke (nib, dust, scrape / tap per stroke), RANK n +
//      YOUR nTH CLAN TO SURVIVE; lost: the 12-week tally breaks at the week the clan fell, a red slash, one toll, WEEK n OF 12.
//   4. stats, START A NEW CLAN (select: the faction pick, as before) / READ THE JOURNAL (menu-action: to1/clan13 on its journal tab).
// Continue during an animation skips to its end (never past a waiting point); back does nothing until the stats are up.
// Sound through the ext PCM mixer (clanend.pcm, gen_clanend_pcm.py): the music pick per screen (ducks under the chalk), wind / fire bed.
// Off: t2r-no-clanend.txt (the old end card). Test: t2r-ce-test.txt "win|lost wlf|ser [rank|day]" pushes the page on the MP menu with
// demo faces. Chalk strokes reveal through disjoint pieces (clanend_seg.h; gui2 Image masks render nothing); t2r-ce-nomask.txt: whole
// strokes fade in instead. Log lines: "clanend:".
#include "clanend_data.h"
#include "clanend_seg.h"
#include "clanend_pcm.h"
__asm__
    (".section .rdata,\"dr\"\n.balign 4\n.globl ce_blob\nce_blob:\n.incbin \"clanend.pcm\"\n.globl ce_blob_end\nce_blob_end:\n.text\n");
extern const int16_t ce_blob[], ce_blob_end[];
static int Cl13Ask(int sec);   // clan13.c (op 0xB6; section 7 = this page's roll call)
#define CE_SEC 7                // (5 = a raid's match stats, 6 = the journal's week pages)
static int g_entSt;            // (tentative: defined with the entry hand-off; 0 = no hand-off running, its t2rEnter cover is not up)
static int g_ldsHold;          // (tentative: loadseq.c; 1 while the TWELVE WEEKS loading page is up / fading out over the first page)
static int g_ceJournalTab;    // clan13.c opens on its JOURNAL tab once (READ THE JOURNAL)

#define CE_ROLL 7
#define CE_NSLOT 6
#define CE_NDUST 16
#define CE_GX 1920.0f
#define CE_GY 940.0f
#define CE_TALLY_X0 1120.0f
#define CE_TALLY_STEP (1600.0f / 11.0f)
#define CE_TALLY_Y 940.0f
#define CE_FPX 3200.0f          // the key art camera's focal length in units (1600 px at 1080p)

// ---- roll call (worker thread -> main): op 0xB6 section 7
static struct {
    int kind, n, more, remembered, raids_won, raids;
    char name[CE_ROLL][28], pid[CE_ROLL][24];
} g_ceRoll;
static volatile LONG g_ceRollVer;
static void CeIngest(char *text)
{                               // the worker (g_clan_busy): "H ...\nE kind n more remembered won played\nR name pid ..."
    int kind = 0, n = 0, more = 0, rem = 0, rw = 0, rp = 0, k = 0;
    char nm[CE_ROLL][28], pd[CE_ROLL][24];
    for (char *ln = text; ln && *ln;) {
        char *nx = strchr(ln, '\n');
        if (nx)
            *nx++ = 0;
        char *f[8];
        int nf = 0;
        for (char *p = ln; nf < 8;) {
            f[nf++] = p;
            char *t = strchr(p, '\t');
            if (!t)
                break;
            *t = 0;
            p = t + 1;
        }
        if (f[0][0] == 'E' && !f[0][1] && nf >= 7) {
            kind = f[1][0];
            n = atoi(f[2]);
            more = atoi(f[3]);
            rem = atoi(f[4]);
            rw = atoi(f[5]);
            rp = atoi(f[6]);
        } else if (f[0][0] == 'R' && !f[0][1] && nf >= 3 && k < CE_ROLL) {
            snprintf(nm[k], 28, "%s", f[1]);
            snprintf(pd[k], 24, "%s", f[2]);
            k++;
        }
        ln = nx;
    }
    EnterCriticalSection(&g_clanCs);
    g_ceRoll.kind = kind;
    g_ceRoll.n = k;
    g_ceRoll.more = more;
    g_ceRoll.remembered = rem;
    g_ceRoll.raids_won = rw;
    g_ceRoll.raids = rp;
    memcpy(g_ceRoll.name, nm, sizeof nm);
    memcpy(g_ceRoll.pid, pd, sizeof pd);
    LeaveCriticalSection(&g_clanCs);
    InterlockedIncrement(&g_ceRollVer);
    Xlog("clanend: roll call reply: kind %c, %d faces (said %d), %d more, %d remembered, raids %d / %d", kind ? kind : '-', k, n, more, rem, rw, rp);
}

// ---- state
enum { CE_ART = 1, CE_WAIT, CE_GO, CE_RANK, CE_DONE };
static struct {
    int ph, win, fac, si, rank, old, day, rl, rw, mx, alive, test, nomask,
        skip_to_wait, finished, key;
    uint64_t t0, tp, tr;        // page open, continue pressed, rank beat start (ms)
    int h_sel, h_act, h_back;
    uint64_t fired[2];          // sound events fired (bit per event id) - art phase / rank phase
    int roll_ver, roll_n, roll_ok, roll_filled, asks;
    uint64_t ask_at;
    float roll_wait;            // when the roll call starts (s): waits a moment for the server's faces
    int ns, sk[CE_NSLOT];       // the new rank's strokes (indices into CE_STROKES)
    float s_t0[CE_NSLOT], s_t1[CE_NSLOT], t_done;
    struct {
        int k;
        float u, vx, vy, life, sc;
    } dust[CE_NDUST * 4];
    int ndust;
    uint64_t mus_dk;            // music duck until (ms)
    uint64_t bed_at;
    int bed_n;
    float last_t;
    int fade_in;                // the end group rises from transparent (pushed over a visible page, no entry black)
    int hold;
    uint64_t hold_at;           // the first page after launch: black (clock held at 0) until the entry hand-off is over
} CE;
static const char *const CE_ORD[10] =
    { "FIRST", "SECOND", "THIRD", "FOURTH", "FIFTH", "SIXTH", "SEVENTH",
"EIGHTH", "NINTH", "TENTH" };

static float CeEase(float u)
{
    u = u < 0 ? 0 : u > 1 ? 1 : u;
    return u * u * (3 - 2 * u);
}

static float CeRamp(float t, float a, float b)
{
    return CeEase((t - a) / (b - a));
}

static int CeEnabled(void)
{
    static int off = -1;
    static uint64_t chk;
    uint64_t n = GetTickCount64();
    if (off < 0 || n - chk > 5000) {
        chk = n;
        FILE *f = ExeSideOpen("t2r-no-clanend.txt", "r");
        off = f != NULL;
        if (f)
            fclose(f);
    }
    return !off;
}

static uint64_t CeW(const char *name)
{                               // a widget of the end group by its name under anim-group.end
    char p[128];
    snprintf(p, sizeof p, "anim-group.end.%s", name);
    return UiW(p);
}

static void CeA(const char *name, float a)
{
    WAlpha(CeW(name), a < 0 ? 0 : a > 1 ? 1 : a);
}

static void CeV(const char *name, int on)
{
    WVisible(CeW(name), on);
}

// chalk reveal (make_segments.py): stroke progress u 0..1 over K disjoint pieces; piece j fades in while the nib crosses it
static float CePiece(float u, int K, int j)
{
    float a = (u * K - j) * 1.4f;
    return a < 0 ? 0 : a > 1 ? 1 : a;
}

// Streaming images: set, then the SAME image set again 100 ms later past the write cache (clan13.c's portrait gate: a new
// texture can show a blue square for its first frames; NR re-sets ~100 ms after the first set). Layers, glyph strokes + cells, faces: <= 80.
#define CE_NIMG 96
static struct {
    char path[96], img[96];
    uint64_t at;
} g_ce_img[CE_NIMG];
static int g_ceNimg;
static void CeImg(const char *path, const char *img)
{
    UiImage(path, img);
    int i = 0;
    while (i < g_ceNimg && strcmp(g_ce_img[i].path, path))
        i++;
    if (i == g_ceNimg) {
        if (g_ceNimg >= CE_NIMG || strlen(path) >= 96)
            return;
        g_ceNimg++;
        snprintf(g_ce_img[i].path, 96, "%s", path);
    }
    snprintf(g_ce_img[i].img, 96, "%s", img);
    g_ce_img[i].at = GetTickCount64() + 100;
}

static void CeImgTick(uint64_t now)
{
    for (int i = 0; i < g_ceNimg; i++)
        if (g_ce_img[i].at && now >= g_ce_img[i].at) {
            g_uiTextRaw = 1;
            UiImage(g_ce_img[i].path, g_ce_img[i].img);
            g_uiTextRaw = 0;
            g_ce_img[i].at = 0;
        }
}

static uint64_t CeCl13Page(void)
{
    static uint64_t id;
    if (!id)
        id = PageId("clan13");
    return id;
}                               // (clan13.c is included later)

// ---- sound
static const int16_t *CePcm(int off)
{
    return ce_blob + off;
}

static void CeSnd(int off, int n, int rate, float gain)
{
    if (PcmPlayRate(CePcm(off), n, gain, rate))
        CE.mus_dk = GetTickCount64() + 450;     // the music ducks under it (mixlib: side-chain ~-5 dB, 0.45 s release)
}

#define CE_PLAY(nm, g) CeSnd(CE_##nm##_OFF, CE_##nm##_N, CE_##nm##_RATE, (g))
static int CeAt(int set, int id, float tev, float t)
{                               // fire event `id` once when t passes tev (silently while skipping)
    uint64_t b = 1ULL << id;
    if (t < tev || (CE.fired[set] & b))
        return 0;
    CE.fired[set] |= b;
    return !CE.skip_to_wait && t - tev < 0.25f; // (a skip jumps the clock: those events are passed, not played)
}

static float g_ceMusGain = 0.55f, g_ce_sfx_gain = 1.0f;       // measured in the A/B capture (sound fired != audible)
static void CeVoices(int bed_only, float gm, float gb, int fade_ms)
{                               // set the music (gm) / bed (gb) voices' gains or fade them out (fade_ms > 0)
    if (g_mixState != 1)
        return;
    const int16_t *m0 =
        CePcm(CE.win ? CE_MUS_WIN_OFF : CE_MUS_LOST_OFF), *m1 =
        m0 + (CE.win ? CE_MUS_WIN_N : CE_MUS_LOST_N);
    const int16_t *b0 = CePcm(CE.win ? CE_BED_WIND_OFF : CE_BED_FIRE_OFF);
    float vol = SfxUserVolume();
    EnterCriticalSection(&g_mixCs);
    for (int v = 0; v < MIX_VOICES; v++) {
        if (!g_mixv[v].on)
            continue;
        const int16_t *s = g_mixv[v].src;
        int mus = s == m0 || s == m1, bed = s == b0;
        if (!(mus || bed) || (bed_only && mus))
            continue;
        if (fade_ms > 0) {
            if (!g_mixv[v].fadeN) {
                g_mixv[v].fadeN = g_mixv[v].fadeLeft = fade_ms * 48;
            }
            continue;
        }
        if (mus) {
            float g = gm * vol;
            if (s == m0) {
                g_mixv[v].tgl = g;
                g_mixv[v].tgr = 0;
            } else {
                g_mixv[v].tgl = 0;
                g_mixv[v].tgr = g;
            }
        } else {
            g_mixv[v].tgl = g_mixv[v].tgr = gb * vol;
        }
    }
    LeaveCriticalSection(&g_mixCs);
}

static void CeAudioStop(int ms)
{
    CeVoices(0, 0.0f, 0.0f, ms);
}

// ---- demo (t2r-ce-test.txt)
static const char *const CE_DEMO_PID[CE_ROLL] = { "W/f/204/happy", "W/m/118/happy", "W/f/123/happy", "W/m/117/happy", "W/m/101/happy", "W/f/128/happy", "W/m/104/neutral" };    // (heads with all 7 expressions: the lost demo swaps in "dead"; 182 has neutral only)
static const char *const CE_DEMO_NAME[CE_ROLL] =
    { "LILA", "RUDY", "SOFIA", "ABEL", "GUS", "LEAH", "LUIS" };
static int CeTestRead(ClanT *o)
{                               // t2r-ce-test.txt: "win|lost wlf|ser [rank or day]"
    FILE *f = ExeSideOpen("t2r-ce-test.txt", "r");
    if (!f)
        return 0;
    char a[16] = "", b[16] = "";
    int n = 0;
    int k = fscanf(f, "%15s %15s %d", a, b, &n);
    fclose(f);
    if (k < 2)
        return 0;
    memset(o, 0, sizeof *o);
    o->ok = 1;
    o->fac = !strcmp(b, "ser") ? 2 : 1;
    if (!strcmp(a, "win")) {
        o->st = 3;
        o->day = 83;
        o->rw = n > 0 ? n : 2;
        o->rl = 1;
        o->mx = 41;
        o->h = 23;
    } else {
        o->st = 2;
        o->day = n > 0 ? n : 47;
        o->rw = 1;
        o->rl = 2;
        o->mx = 34;
    }
    return 1;
}

static int CeTestPush(void)
{                               // MP menu: push the end page once per change of the test file
    static uint64_t chk;
    static int was;
    uint64_t n = GetTickCount64();
    if (n - chk < 1500)
        return 0;
    chk = n;
    ClanT t;
    int on = CeTestRead(&t);
    static int last_key;        // a new spec in the file (other kind / faction / rank or day) plays again without removing it
    int key =
        on ? t.st << 24 | t.fac << 20 | (t.rw & 0xfff) << 8 | (t.
                                                               day & 0xff)
        : 0;
    if (on && was && key != last_key)
        was = 0;
    if (on)
        last_key = key;
    if (on && !was) {
        was = 1;
        Xlog("clanend: test file - pushing to1/clan-side (%s, faction %d)",
             t.st == 3 ? "won" : "lost", t.fac);
        CE.finished = 0;
        PushPage("to1/clan-side");     /* (a demo always plays from the start) */
        return 1;
    }
    if (!on)
        was = 0;
    return 0;
}

static float CeRnd(uint32_t *rs)
{
    *rs = *rs * 1664525u + 1013904223u;
    return (float) (*rs >> 8 & 0xffff) / 65536.0f;
}

// ---- the rank glyph strokes and their timing (re/win-screen-1112 make_video.chalk_schedule)
static void CeSchedule(float t0)
{
    CE.ns = 0;
    int gi = (CE.rank < 1 ? 1 : CE.rank > 10 ? 10 : CE.rank) - 1;
    float t = t0;
    for (int i = 0; i < CE_NSTROKE && CE.ns < CE_NSLOT; i++) {
        if (CE_STROKES[i].rank != gi)
            continue;
        float dur =
            CE_STROKES[i].line ? 0.14f +
            CE_STROKES[i].ref * 0.0085f : 0.07f;
        CE.sk[CE.ns] = i;
        CE.s_t0[CE.ns] = t;
        CE.s_t1[CE.ns] = t + dur;
        t += dur + (CE_STROKES[i].line ? 0.17f : 0.15f);
        CE.ns++;
    }
    CE.t_done = CE.ns ? CE.s_t1[CE.ns - 1] : t0;
    uint32_t rs = 23u + (uint32_t) gi * 7u;
#define CE_RND() CeRnd(&rs)
    CE.ndust = 0;
    for (int s = 0; s < CE.ns; s++) {   // dust shed by the nib: 7 motes a bar, 3 a dot (the preview's 46 / 14, fewer and bigger)
        int line = CE_STROKES[CE.sk[s]].line;
        for (int j = 0; j < (line ? 7 : 3) && CE.ndust < CE_NDUST * 4; j++) {
            float g1 = CE_RND() + CE_RND() + CE_RND() - 1.5f;
            CE.dust[CE.ndust].k = s;
            CE.dust[CE.ndust].u = line ? CE_RND() : CE_RND() * 0.3f;
            CE.dust[CE.ndust].vx = g1 * (line ? 52.0f : 120.0f);
            CE.dust[CE.ndust].vy =
                line ? -80.0f + CE_RND() * 100.0f : -140.0f +
                CE_RND() * 180.0f;
            CE.dust[CE.ndust].life = 0.6f + CE_RND() * 0.8f;
            CE.dust[CE.ndust].sc = 0.8f + CE_RND() * 1.0f;
            CE.ndust++;
        }
    }
#undef CE_RND
}

static float CeSprog(int s, float t)
{                               // stroke s reveal 0..1: starts a touch slow, runs, eases out
    float u = (t - CE.s_t0[s]) / (CE.s_t1[s] - CE.s_t0[s]);
    u = u < 0 ? 0 : u > 1 ? 1 : u;
    return 0.35f * u + 0.65f * CeEase(u);
}

// ---- enter
static void CeEnter(const ClanT *c, int test)
{
    int key = c->st << 24 | (c->rw & 0xfff) << 12 | (c->rl & 0xfff);
    int again = CE.finished && CE.key == key;   // back from the journal: the finished page, no replay
    memset(&CE, 0, sizeof CE);
    g_ceNimg = 0;
    CE.key = key;
    CE.test = test;
    CE.win = c->st == 3;
    CE.fac = c->fac == 2;
    CE.si = (CE.win ? 0 : 2) + CE.fac;
    CE.rw = c->rw;
    CE.rl = c->rl;
    CE.mx = c->mx;
    CE.alive = c->h + c->hu + c->s;
    CE.day = c->day;
    CE.rank = c->rw < 1 ? 1 : c->rw;
    CE.old = CE.rank - 1;
    {
        FILE *f = ExeSideOpen("t2r-ce-nomask.txt", "r");
        CE.nomask = f != NULL;
        if (f)
            fclose(f);
    }
    CE.t0 = GetTickCount64();
    CE.ph = CE_ART;
    CE.h_sel = CE.h_act = CE.h_back = 1;
    CE.roll_wait = 1.9f;
    static const char *const SC[4] =
        { "wlf", "ser", "lost-wlf", "lost-ser" };
    char p[64], img[96];
    for (int i = 0; i < 5; i++) {
        snprintf(p, sizeof p, "anim-group.end.ceArt.ceL%d", i);
        snprintf(img, sizeof img, "streaming/t2r/clanend/ce-%s-L%d.png",
                 SC[CE.si], i);
        CeImg(p, img);
    }
    int gi = (CE.rank > 10 ? 10 : CE.rank);
    snprintf(img, sizeof img, "streaming/t2r/clanend/ce-r%d.png",
             CE.old > 10 ? 10 : CE.old < 1 ? 1 : CE.old);
    CeImg("anim-group.end.cePage.ceRank.ceOld", img);
    snprintf(img, sizeof img, "streaming/t2r/clanend/ce-r%d-g.png",
             CE.old > 10 ? 10 : CE.old < 1 ? 1 : CE.old);
    CeImg("anim-group.end.cePage.ceRank.ceOldG", img);
    CeSchedule(0.0f);
    for (int s = 0; s < CE_NSLOT; s++) {
        char sp[96];
        snprintf(sp, sizeof sp,
                 "anim-group.end.cePage.ceRank.ceS%d.ceS%dImg", s, s);
        WAlpha(UiW(sp), 0.0f);
        if (s >= CE.ns)
            continue;
        const ce_seg_t *sg = &CE_SEG[CE.sk[s]];
        int full = CE.nomask || !sg->n; // dots (and t2r-ce-nomask.txt): the whole stroke image fades in
        snprintf(img, sizeof img, "streaming/t2r/clanend/ce-r%d-s%d.png",
                 gi, CE_STROKES[CE.sk[s]].k + 1);
        CeImg(sp, img);        // (lines: once drawn)
        for (int j = 0; j < CE_KR; j++) {       // line strokes: the pieces, placed where they were cut
            snprintf(sp, sizeof sp,
                     "anim-group.end.cePage.ceRank.ceS%d.ceS%dC%d", s, s,
                     j);
            uint64_t w = UiW(sp);
            WAlpha(w, 0.0f);
            if (full || j >= sg->n)
                continue;
            snprintf(img, sizeof img,
                     "streaming/t2r/clanend/ce-r%d-s%d-c%d.png", gi,
                     CE_STROKES[CE.sk[s]].k + 1, j);
            CeImg(sp, img);
            WPivot(w, CE_GX + sg->c[j][0], CE_GY + sg->c[j][1]);
        }
    }
    {                           // a fresh page: every rank / tally / stats piece starts hidden (a second page in one session showed the last run's texts and
        // buttons through the page fade-in)
        static const char *const Z[] =
            { "cePage.ceRank.ceGlow", "cePage.ceRank.ceOldG",
"cePage.ceRank.ceOld", "cePage.ceRank.ceOldT",
            "cePage.ceRank.ceNib", "cePage.ceRank.ceRankT",
                "cePage.ceRank.ceRankS", "cePage.ceTally.ceTGlow",
                "cePage.ceTally.ceWk",
            "cePage.ceTally.ceWkS", "cePage.ceBtns", "ceUI.ceCont",
                "ceUI.ceRollSub", "ceUI.ceRollHdr", "ceUI.ceTitle",
                "ceUI.ceDay", "ceUI.ceDayLine"
        };
        for (unsigned i = 0; i < sizeof Z / sizeof *Z; i++)
            CeA(Z[i], 0);
        static const char *const V[] = { "ceArt", "ceScratch", "ceVig", "ceGrain", "ceUI" };    // (START hides them for its crossfade)
        for (unsigned i = 0; i < sizeof V / sizeof *V; i++)
            CeV(V[i], 1);
        char q[64];
        for (int i = 0; i < 4; i++) {
            snprintf(q, sizeof q, "cePage.ceStats.ceSV%d", i);
            CeA(q, 0);
            snprintf(q, sizeof q, "cePage.ceStats.ceSL%d", i);
            CeA(q, 0);
            if (i) {
                snprintf(q, sizeof q, "cePage.ceStats.ceSD%d", i);
                CeA(q, 0);
            }
        }
        for (int j = 0; j < CE_NDUST; j++) {
            snprintf(q, sizeof q, "cePage.ceRank.ceDust%d", j);
            CeA(q, 0);
        }
        for (int s = 0; s < CE_NSLOT; s++) {
            snprintf(q, sizeof q, "cePage.ceRank.ceS%d", s);
            CeV(q, 0);
        }
        for (int i = 0; i < 12; i++) {
            snprintf(q, sizeof q, "cePage.ceTally.ceT%d", i);
            CeV(q, 0);
            snprintf(q, sizeof q, "cePage.ceTally.ceTg%d", i);
            CeA(q, 0);
            snprintf(q, sizeof q, "cePage.ceTally.ceTn%d", i);
            CeA(q, 0);
            snprintf(q, sizeof q, "cePage.ceTally.ceT%d.ceT%dImg", i, i);
            CeV(q, 1);
            CeA(q, 0);
            for (int j = 0; j < CE_TK; j++) {
                snprintf(q, sizeof q, "cePage.ceTally.ceT%d.ceT%dC%d", i,
                         i, j);
                CeA(q, 0);
            }
        }
        CeV("cePage.ceTally.ceSl", 0);
        CeV("cePage.ceTally.ceSl.ceSlImg", 1);
        CeA("cePage.ceTally.ceSl.ceSlImg", 0);
        for (int j = 0; j < CE_SK; j++) {
            snprintf(q, sizeof q, "cePage.ceTally.ceSl.ceSlC%d", j);
            CeA(q, 0);
        }
        for (int i = 0; i < CE_ROLL; i++) {
            snprintf(q, sizeof q, "ceUI.ceP%d.ceP%dStr.ceP%dStrImg", i, i,
                     i);
            CeV(q, 1);
            CeA(q, 0);
            for (int j = 0; j < CE_PK; j++) {
                snprintf(q, sizeof q, "ceUI.ceP%d.ceP%dStr.ceP%dStrC%d", i,
                         i, i, j);
                CeA(q, 0);
            }
        }
    }
    // texts
    char b[160];
    snprintf(b, sizeof b, "DAY %d", CE.win ? 84 : CE.day);
    UiText("anim-group.end.ceUI.ceDay", b);
    UiText("anim-group.end.ceUI.ceTitle",
            CE.
            win ? "YOUR CLAN SURVIVED" :
            "<font color=\"#e04848\">YOUR CLAN IS GONE</font>");
    UiText("anim-group.end.ceUI.ceRollHdr",
            CE.win ? "THEY MADE IT" : "THE LAST OF THEM");
    UiText("anim-group.end.ceUI.ceRollSub", "");
    snprintf(b, sizeof b, "RANK %d", CE.rank);
    UiText("anim-group.end.cePage.ceRank.ceRankT", b);
    if (CE.rank <= 10)
        snprintf(b, sizeof b, "YOUR %s CLAN TO SURVIVE",
                 CE_ORD[CE.rank - 1]);
    else
        snprintf(b, sizeof b, "YOUR %dTH CLAN TO SURVIVE", CE.rank);
    UiText("anim-group.end.cePage.ceRank.ceRankS", b);
    snprintf(b, sizeof b, "RANK %d", CE.old);
    UiText("anim-group.end.cePage.ceRank.ceOldT", CE.old >= 1 ? b : "");
    snprintf(b, sizeof b, "WEEK %d OF 12",
             CE.day / 7 + 1 > 12 ? 12 : CE.day / 7 + 1);
    UiText("anim-group.end.cePage.ceTally.ceWk", b);
    snprintf(b, sizeof b, "THE CLAN FELL %d DAYS SHORT",
             84 - CE.day > 0 ? 84 - CE.day : 0);
    UiText("anim-group.end.cePage.ceTally.ceWkS", b);
    // visibility: the end group up, the first-launch groups down, the pieces at their start
    UiVisible("anim-group.intro", 0);
    UiVisible("anim-group.side", 0);
    UiVisible("anim-group.end", 1);
    CE.fade_in = !again && g_entSt == 0;       // under the entry black the cover already rises with the page: no second fade
    // The first page after launch came up under the TWELVE WEEKS loading page, whose title dissolved over YOUR CLAN SURVIVED and
    // the faces for ~1 s. The page now holds black until the hand-off is done (the loading page fades into black), then the art rises from it.
    CE.hold = !again && (g_entSt != 0 || g_ldsHold);
    CE.hold_at = CE.t0;
    WAlpha(UiW("anim-group.end"), CE.fade_in ? 0.003f : 1.0f);
    CeV("cePage.ceRank", CE.win);
    CeV("cePage.ceTally", !CE.win);
    for (int i = 0; i < CE_ROLL; i++) {
        char q[64];
        snprintf(q, sizeof q, "ceUI.ceP%d", i);
        CeA(q, 0);
        snprintf(q, sizeof q, "ceUI.ceP%d.ceP%dStr", i, i);
        CeV(q, 0);
    }
    EnterCriticalSection(&g_clanCs);
    g_ceRoll.n = 0;
    g_ceRoll.kind = 0;
    LeaveCriticalSection(&g_clanCs);
    CE.roll_ver = (int) g_ceRollVer;
    if (!test && !again && Cl13Ask(CE_SEC)) {
        CE.asks = 1;
        CE.ask_at = CE.t0;
    }
    Xlog("clanend: %s page up (%s, faction %s, day %d, rank %d -> %d, clans won %d lost %d, %d strokes, mask %s)%s", CE.win ? "SURVIVED" : "CLAN GONE", test ? "test" : "live", CE.fac ? "SER" : "WLF", CE.day, CE.old, CE.rank, CE.rw, CE.rl, CE.ns, CE.nomask ? "off" : "on", again ? " - back from the journal: the finished page" : "");
    if (again) {                // straight to the stats (the music played already)
        CE.ph = CE_DONE;
        CE.finished = 1;
        CE.tp = CE.t0 - 4000;
        CE.tr = CE.t0 - 20000;
        CE.fired[0] = CE.fired[1] = ~0ULL;
        CE.roll_ok = 0;
        return;
    }
    if (PcmPlayStereo
        (CePcm(CE.win ? CE_MUS_WIN_OFF : CE_MUS_LOST_OFF),
         CePcm((CE.win ? CE_MUS_WIN_OFF : CE_MUS_LOST_OFF) +
                (CE.win ? CE_MUS_WIN_N : CE_MUS_LOST_N)),
         CE.win ? CE_MUS_WIN_N : CE_MUS_LOST_N, g_ceMusGain, 32000))
        Xlog("clanend: music %s",
             CE.win ? "388 far-farm-outro" : "687 pat-rescue-joel-fail-a");
    CE.bed_at = CE.t0;
}

// ---- the art: five layers moved by the camera (X right, A toward the scene, Y up; metres)
static void CeArt(float t)
{
    float X = 0, A, Y = 0;
    float sway = 0.05f * sinf(t * 0.25f);       // never frozen while the page waits
    if (CE.win) {
        A = 0.5f * expf(-t / 2.6f);     // starts 0.5 m nearer, drifts back into the key-art framing
        X = sway;
        if (CE.tp) {
            float d = (GetTickCount64() - CE.tp) / 1000.0f, a =
                2.9f / 6.75f;
            X += d <= 1.5f ? a * d * d : a * 2.25f + 3.0f * a * (d - 1.5f);
            Y = 0.25f * X / 3.7f;
        }
    } else {
        A = 0.6f * (1.0f - expf(-t / 6.0f));    // creeps in on the machete body
        X = -0.25f * A + sway * 0.6f;
        if (CE.tp) {
            float d = (GetTickCount64() - CE.tp) / 1000.0f;
            A += 0.55f * d * d;
            if (A > 4.4f)
                A = 4.4f;
        }
    }
    for (int i = 0; i < 5; i++) {
        float dz = CE_DEPTH[CE.si][i], dd = dz - A;
        if (dd < 0.6f)
            dd = 0.6f;
        float s = dz / dd;
        float sx = -CE_FPX * X / dd, sy = CE_FPX * Y / dd;
        float lo = -1920.0f * (s - 1.0f), hi = 1920.0f * (s - 1.0f);    // keep the frame covered: left edge <= 0 ...
        float rr = 1920.0f - (CE.win ? 3200.0f : 1920.0f) * s;  // ... right edge (pivot 1920 + sx + frame right * s) >= 3840 (win layers run
        // on to 5120 units, lost ones stop at 3840; 3840 here pushed every
        // layer right by 1920 / 640 units, the frame's left edge showed black)
        if (rr > lo)
            lo = rr;
        if (sx > hi)
            sx = hi;
        if (sx < lo)
            sx = lo;
        float vy = 1080.0f * (s - 1.0f);
        if (sy > vy)
            sy = vy;
        if (sy < -vy)
            sy = -vy;
        char p[32];
        snprintf(p, sizeof p, "ceArt.ceL%d", i);
        uint64_t w = CeW(p);
        WScale(w, s, s);
        WPivot(w, 1920.0f + sx, 1080.0f + sy);
    }
    static uint32_t gr = 77;    // film grain: the tile jumps every other frame
    if (!(g_frame & 1)) {
        gr = gr * 1664525u + 1013904223u;
        WPivot(CeW("ceGrain"), -512.0f + (float) (gr >> 8 & 511),
                -512.0f + (float) (gr >> 20 & 511));
    }
}

// ---- phase 1: title + roll call
static float CeRollEnd(void)
{                               // when the roll call (with its strikes) is done
    float r = CE.roll_wait;
    if (!CE.roll_ok)
        return r + 0.6f;
    float e = r + 0.6f + 0.16f * (CE.roll_n - 1);
    if (!CE.win)
        e = r + 0.85f + 0.16f * 6 + 0.2f * (CE.roll_n - 1) + 0.2f;
    return e + 0.3f;
}

static void CeRollFill(void)
{
    EnterCriticalSection(&g_clanCs);
    int n = g_ceRoll.n, more = g_ceRoll.more, rem = g_ceRoll.remembered;
    char nm[CE_ROLL][28], pd[CE_ROLL][24];
    memcpy(nm, g_ceRoll.name, sizeof nm);
    memcpy(pd, g_ceRoll.pid, sizeof pd);
    LeaveCriticalSection(&g_clanCs);
    if (CE.test) {
        n = CE_ROLL;
        more = 16;
        rem = 38;
        for (int i = 0; i < n; i++) {
            snprintf(nm[i], 28, "%s", CE_DEMO_NAME[i]);
            snprintf(pd[i], 24, "%s", CE_DEMO_PID[i]);
            if (!CE.win) {
                char *e = strrchr(pd[i], '/');
                if (e)
                    strcpy(e + 1, "dead");
            }
        }
    }
    CE.roll_n = n;
    CE.roll_ok = n > 0;
    float x0 = 1920.0f - (float) (n - 1) * 276.0f / 2.0f - 112.0f;      // centred whatever the count
    for (int i = 0; i < CE_ROLL; i++) {
        char p[96], img[96], a[16], s[16], m[16], e[20];
        snprintf(p, sizeof p, "anim-group.end.ceUI.ceP%d", i);
        if (i >= n) {
            UiVisible(p, 0);
            continue;
        }
        UiVisible(p, 1);
        WPivot(UiW(p), x0 + i * 276.0f, 1440.0f);
        if (sscanf(pd[i], "%15[^/]/%15[^/]/%15[^/]/%19s", a, s, m, e) == 4) {
            snprintf(img, sizeof img, "streaming/t2r/icons/c13p-%s_%s.png",
                     m, e);
            snprintf(p, sizeof p, "anim-group.end.ceUI.ceP%d.ceP%dImg", i,
                     i);
            CeImg(p, img);
        }
        for (char *q = nm[i]; *q; q++)
            if (*q >= 'a' && *q <= 'z')
                *q -= 32;
        snprintf(p, sizeof p, "anim-group.end.ceUI.ceP%d.ceP%dName", i, i);
        UiText(p, nm[i]);
        if (!CE.win) {
            snprintf(p, sizeof p, "anim-group.end.ceUI.ceP%d.ceP%dImg", i,
                     i);
            WAlpha(UiW(p), 0.85f);
        }
    }
    char b[128];
    if (CE.win)
        snprintf(b, sizeof b,
                 more ? "+ %d MORE WALKED OUT    -    %d REMEMBERED" :
                 "%d REMEMBERED", more ? more : rem, rem);
    else
        snprintf(b, sizeof b, "%d REMEMBERED    -    NONE WALKED OUT",
                 rem);
    if (!n)
        snprintf(b, sizeof b, CE.win ? "%d WALKED OUT" : "NONE WALKED OUT",
                 CE.alive);
    if (CE.win && more)
        snprintf(b, sizeof b, "+ %d MORE WALKED OUT    -    %d REMEMBERED",
                 more, rem);
    UiText("anim-group.end.ceUI.ceRollSub", b);
    UiVisible("anim-group.end.ceUI.ceRollHdr", n > 0);
    Xlog("clanend: roll call %d faces (%s)", n,
         CE.test ? "demo" : "server");
}

static void CeUi(float t)
{
    float ui = 1.0f;
    if (CE.tp)
        ui = 1.0f - CeRamp((GetTickCount64() - CE.tp) / 1000.0f, 0.0f,
                            0.8f);
    CeA("ceUI", ui);
    if (ui <= 0)
        return;
    float a1 = CeRamp(t, 0.5f, 1.1f), a2 = CeRamp(t, 0.8f, 1.6f);
    CeA("ceUI.ceDay", a1 * 0.8f);
    CeA("ceUI.ceDayLine", a1);
    CeA("ceUI.ceTitle", a2);
    WPivot(CeW("ceUI.ceTitle"), 320.0f, 290.0f + 28.0f * (1.0f - a2));
    // the faces: wait up to 0.7 s past 1.9 s for the server's reply
    if (!CE.roll_ok && CE.roll_ver != (int) g_ceRollVer) {
        CE.roll_ver = (int) g_ceRollVer;
        CeRollFill();
    }
    if (CE.test && !CE.roll_ok && t > 0.3f)
        CeRollFill();
    if (!CE.test && !CE.roll_ok && CE.roll_ver == (int) g_ceRollVer
        && t < 2.6f && CE.asks < 4 && GetTickCount64() - CE.ask_at > 600
        && Cl13Ask(CE_SEC)) {
        CE.asks++;
        CE.ask_at = GetTickCount64();
    }
    if (!CE.roll_ok && !CE.roll_filled && t >= 2.6f) {
        CE.roll_filled = 1;
        CeRollFill();
    }
    if (!CE.roll_ok && t < 2.6f)
        CE.roll_wait = t > 1.9f ? t : 1.9f;
    float r0 = CE.roll_wait;
    CeA("ceUI.ceRollHdr", CeRamp(t, r0, r0 + 0.4f));
    for (int i = 0; i < CE.roll_n; i++) {
        float ti = r0 + 0.25f + i * 0.16f, ap = CeRamp(t, ti, ti + 0.35f);
        char p[48];
        snprintf(p, sizeof p, "ceUI.ceP%d", i);
        uint64_t w = CeW(p);
        WAlpha(w, ap);
        WPivot(w,
                1920.0f - (float) (CE.roll_n - 1) * 138.0f - 112.0f +
                i * 276.0f, 1440.0f + 36.0f * (1.0f - ap));
        if (CeAt(0, i, ti, t))
            CE_PLAY(TICK, 0.32f * g_ce_sfx_gain);
        if (!CE.win) {          // one chalk strike through each dead face, left to right
            float ts = r0 + 0.85f + 0.16f * 6 + i * 0.2f, us =
                (t - ts) / 0.14f;
            snprintf(p, sizeof p, "ceUI.ceP%d.ceP%dStr", i, i);
            CeV(p, us > 0);
            if (us > 0) {
                float u = us > 1 ? 1 : us;
                int dn = CE.nomask || u >= 1.0f;        // a drawn stroke shows its whole image (the pieces' joins filter to faint seams)
                snprintf(p, sizeof p, "ceUI.ceP%d.ceP%dStr.ceP%dStrImg", i,
                         i, i);
                CeA(p, CE.nomask ? u : (float) dn);
                for (int j = 0; j < CE_PK; j++) {
                    snprintf(p, sizeof p,
                             "ceUI.ceP%d.ceP%dStr.ceP%dStrC%d", i, i, i,
                             j);
                    CeA(p, dn ? 0.0f : CePiece(u, CE_PK, j));
                }
            }
            if (CeAt(0, 8 + i, ts, t))
                CE_PLAY(STRIKE, 0.30f * g_ce_sfx_gain);
        }
    }
    float re = CeRollEnd();
    CeA("ceUI.ceRollSub", CeRamp(t, re - 0.3f, re + 0.1f));
    float ac = CeRamp(t, re + 0.2f, re + 0.6f);
    if (CE.tp) {
        float d = (GetTickCount64() - CE.tp) / 1000.0f;
        ac *= 1.0f + 0.6f * expf(-d * 9.0f);
    }
    CeA("ceUI.ceCont", ac);
    if (CE.ph == CE_ART && t >= re + 0.2f) {
        CE.ph = CE_WAIT;
        Xlog("clanend: waiting for continue (%.1f s)", t);
    }
}

// ---- phase 3: the rank page
static void CeRankWin(float t)
{
    float ao = CE.old >= 1 ? CeRamp(t, 0.0f, 0.4f) : 0.0f, sm =
        CeRamp(t, 0.55f, 1.0f);
    float gone = 1.0f - CeRamp(t, CE.t_done + 1.1f, CE.t_done + 1.7f);
    CeA("cePage.ceRank.ceOld", ao * (1.0f - sm));
    CeA("cePage.ceRank.ceOldG", ao * sm * 0.22f * gone);
    WPivot(CeW("cePage.ceRank.ceOld"), CE_GX - 14.0f * sm, CE_GY);
    CeA("cePage.ceRank.ceOldT",
         ao * (1.0f - sm) * CeRamp(t, 0.4f, 0.6f) * 0.8f);
    if (CE.old >= 1 && CeAt(1, 0, 0.55f, t))
        CE_PLAY(RUB, 0.45f * g_ce_sfx_gain);
    float T = 1.1f;             // the new rank, chalked in from 1.1 s (strokes from ce_schedule, offset)
    float nib_a = 0, nx = CE_GX, ny = CE_GY;
    for (int s = 0; s < CE_NSLOT; s++) {
        char p[64], q[64];
        snprintf(p, sizeof p, "cePage.ceRank.ceS%d", s);
        if (s >= CE.ns) {
            CeV(p, 0);
            continue;
        }
        const ce_stroke_t *k = &CE_STROKES[CE.sk[s]];
        float tt = t - T, pr = CeSprog(s, tt);
        int on = tt >= CE.s_t0[s];
        CeV(p, on);
        if (!on)
            continue;
        const ce_seg_t *sg = &CE_SEG[CE.sk[s]];
        snprintf(q, sizeof q, "cePage.ceRank.ceS%d.ceS%dImg", s, s);
        if (CE.nomask || !sg->n)
            CeA(q, CE.nomask ? pr : (pr * 1.6f > 1 ? 1 : pr * 1.6f));
        else {
            int dn = pr >= 1.0f;        // drawn: the whole stroke image, pixel exact
            CeA(q, (float) dn);
            for (int j = 0; j < sg->n; j++) {
                snprintf(q, sizeof q, "cePage.ceRank.ceS%d.ceS%dC%d", s, s,
                         j);
                CeA(q, dn ? 0.0f : CePiece(pr, sg->n, j));
            }
        }
        if (k->line && tt <= CE.s_t1[s] + 0.12f) {
            nib_a =
                tt <= CE.s_t1[s] ? 1.0f : 1.0f - (tt - CE.s_t1[s]) / 0.12f;
            nx = CE_GX + k->x0 + (k->x1 - k->x0) * pr;
            ny = CE_GY + k->y0 + (k->y1 - k->y0) * pr;
        }
        if (CeAt(1, 1 + s, T + CE.s_t0[s], t)) {
            if (k->line) {
                float d = CE.s_t1[s] - CE.s_t0[s];
                int bi = 0;
                for (int j = 1; j < 6; j++)
                    if (fabsf(CE_SCRAPE_SEC[j] - d) <
                        fabsf(CE_SCRAPE_SEC[bi] - d))
                        bi = j;
                CeSnd(CE_SCRAPE_OFF[bi], CE_SCRAPE_N[bi], 48000,
                       0.62f * g_ce_sfx_gain);
            } else
                CE_PLAY(TAP, 0.55f * g_ce_sfx_gain);
        }
    }
    CeA("cePage.ceRank.ceNib", nib_a * 0.9f);
    WPivot(CeW("cePage.ceRank.ceNib"), nx, ny);
    for (int j = 0; j < CE_NDUST; j++) {        // 16 mote widgets: the live motes (newest first) of the schedule's list
        char p[48];
        snprintf(p, sizeof p, "cePage.ceRank.ceDust%d", j);
        WAlpha(CeW(p), 0.0f);
    }
    int used = 0;
    for (int i = CE.ndust - 1; i >= 0 && used < CE_NDUST; i--) {
        int s = CE.dust[i].k;
        const ce_stroke_t *k = &CE_STROKES[CE.sk[s]];
        float ts =
            T + CE.s_t0[s] + CE.dust[i].u * (CE.s_t1[s] - CE.s_t0[s]),
            age = t - ts;
        if (age <= 0 || age > CE.dust[i].life)
            continue;
        float pr = CeSprog(s, ts - T);
        float hx = k->x0 + (k->x1 - k->x0) * (k->line ? pr : 0), hy =
            k->y0 + (k->y1 - k->y0) * (k->line ? pr : 0);
        float x = CE_GX + hx + CE.dust[i].vx * age, y =
            CE_GY + hy + CE.dust[i].vy * age + 0.5f * 320.0f * age * age;
        float al = powf(1.0f - age / CE.dust[i].life, 1.5f) * 0.8f;
        char p[48];
        snprintf(p, sizeof p, "cePage.ceRank.ceDust%d", used++);
        uint64_t w = CeW(p);
        WAlpha(w, al);
        WPivot(w, x, y);
        WScale(w, CE.dust[i].sc, CE.dust[i].sc);
    }
    float td = T + CE.t_done;
    if (t >= td)
        CeA("cePage.ceRank.ceGlow",
             0.55f * expf(-(t - td) * 2.2f) * CeRamp(t, td,
                                                      td + 0.12f) * 0.6f);
    if (CeAt(1, 7, td, t))
        CE_PLAY(SETTLE, 0.30f * g_ce_sfx_gain);
    float tx = td + 0.3f, at = CeRamp(t, tx, tx + 0.6f);
    CeA("cePage.ceRank.ceRankT", at);
    WPivot(CeW("cePage.ceRank.ceRankT"), 1120.0f,
            CE_GY + 330.0f + 32.0f * (1.0f - at));
    CeA("cePage.ceRank.ceRankS", CeRamp(t, tx + 0.35f, tx + 0.85f));
}

static float CeTallyX(int i)
{
    return CE_TALLY_X0 + i * CE_TALLY_STEP;
}

static void CeRankLost(float t, float *dip)
{
    float ah = CeRamp(t, 0.0f, 0.4f);
    int fw = CE.day / 7;
    if (fw > 11)
        fw = 11;                // the week the clan fell (0-based), its days lived
    float cut = (float) (CE.day % 7) / 7.0f;
    float tb = 0.25f + fw * 0.2f + 0.5f + 0.15f, tt_ = tb + 0.35f;      // break, toll
    float fo = CeRamp(t, tt_ + 0.3f, tt_ + 1.3f);
    for (int i = 0; i < 12; i++) {
        char p[64], q[64];
        float f_ = i > fw ? 1.0f - 0.8f * fo : 1.0f;
        snprintf(p, sizeof p, "cePage.ceTally.ceTg%d", i);
        CeA(p, ah * f_ * 0.22f);
        snprintf(p, sizeof p, "cePage.ceTally.ceTn%d", i);
        CeA(p, ah * f_ * 0.6f);
        snprintf(p, sizeof p, "cePage.ceTally.ceT%d", i);
        float t0 = 0.25f + i * 0.2f, du = i == fw ? 0.5f : 0.13f, lim =
            i == fw ? cut : 1.0f;
        float u = (t - t0) / du;
        u = u < 0 ? 0 : u > 1 ? 1 : u;
        int on = i <= fw && t >= t0 && lim > 0;
        CeV(p, on);
        if (on) {
            int dn = CE.nomask || u * lim >= 1.0f;      // (the week the clan fell stays in pieces: cut short)
            snprintf(q, sizeof q, "cePage.ceTally.ceT%d.ceT%dImg", i, i);
            CeA(q, CE.nomask ? u : (float) dn);
            for (int j = 0; j < CE_TK; j++) {
                snprintf(q, sizeof q, "cePage.ceTally.ceT%d.ceT%dC%d", i,
                         i, j);
                CeA(q, dn ? 0.0f : CePiece(u * lim, CE_TK, j));
            }
        }
        if (i <= fw && lim > 0 && CeAt(1, 12 + i, t0, t)) {
            if (i == fw)
                CE_PLAY(TALLY_LONG, 0.40f * g_ce_sfx_gain);
            else
                CE_PLAY(TALLY, 0.36f * g_ce_sfx_gain);
        }
    }
    float xb = CeTallyX(fw), yb = CE_TALLY_Y - 140.0f + 280.0f * cut;
    float ub = (t - tb) / 0.12f;
    ub = ub < 0 ? 0 : ub > 1 ? 1 : ub;
    CeV("cePage.ceTally.ceSl", t >= tb);
    int sdn = CE.nomask || ub >= 1.0f;
    WPivot(CeW("cePage.ceTally.ceSl.ceSlImg"), xb, yb);
    CeA("cePage.ceTally.ceSl.ceSlImg", CE.nomask ? ub : (float) sdn);
    for (int j = 0; j < CE_SK; j++) {
        char q[48];
        snprintf(q, sizeof q, "cePage.ceTally.ceSl.ceSlC%d", j);
        uint64_t w = CeW(q);
        WPivot(w, xb + CE_SL_C[j][0], yb + CE_SL_C[j][1]);
        WAlpha(w, sdn ? 0.0f : CePiece(ub, CE_SK, j));
    }
    if (CeAt(1, 26, tb, t))
        CE_PLAY(SNAP, 0.55f * g_ce_sfx_gain);
    if (t >= tt_) {
        float b = t - tt_;
        *dip = 0.35f * expf(-b * 2.2f);
        float gl =
            (b / 0.25f > 1 ? 1 : b / 0.25f) * expf(-b / 2.2f) * 0.9f;
        uint64_t g = CeW("cePage.ceTally.ceTGlow");
        WAlpha(g, gl * 0.7f);
        WPivot(g, xb, yb);
    }
    if (CeAt(1, 27, tt_, t)) {
        CE_PLAY(TOLL, 0.62f * g_ce_sfx_gain);
        CE.mus_dk = GetTickCount64() + 2200;
    }
    float tx = tt_ + 0.25f, at = CeRamp(t, tx, tx + 0.6f);
    CeA("cePage.ceTally.ceWk", at);
    WPivot(CeW("cePage.ceTally.ceWk"), 1120.0f,
            CE_TALLY_Y - 420.0f - 20.0f * (1.0f - at));
    CeA("cePage.ceTally.ceWkS", CeRamp(t, tx + 0.4f, tx + 0.9f));
}

static float CeStatsT(void)
{                               // rank-beat time when the stats start
    if (CE.win)
        return 1.1f + CE.t_done + 0.3f + 1.1f;
    int fw = CE.day / 7;
    if (fw > 11)
        fw = 11;
    return 0.25f + fw * 0.2f + 0.5f + 0.15f + 0.35f + 0.25f + 1.15f;
}

static void CeStats(float t)
{
    float ts = CeStatsT();
    char v[4][48], l[4][40];
    if (CE.win) {
        EnterCriticalSection(&g_clanCs);
        int rem = g_ceRoll.remembered, rwn = g_ceRoll.raids_won, rp =
            g_ceRoll.raids, have = g_ceRoll.kind != 0;
        LeaveCriticalSection(&g_clanCs);
        if (CE.test) {
            have = 1;
            rem = 38;
            rwn = 31;
            rp = 49;
        }
        snprintf(v[0], 48, "%d", CE.alive);
        snprintf(l[0], 40, "SURVIVORS WALKED OUT");
        snprintf(v[1], 48, "%d", CE.mx);
        snprintf(l[1], 40, "MOST AT ONCE");
        if (have) {
            snprintf(v[2], 48, "%d", rem);
            snprintf(l[2], 40, "LOST ON THE WAY");
            snprintf(v[3], 48, "%d / %d", rwn, rp);
            snprintf(l[3], 40, "RAIDS WON");
        } else {
            snprintf(v[2], 48, "%d", CE.rw);
            snprintf(l[2], 40, "CLANS SURVIVED");
            snprintf(v[3], 48, "%d", CE.rl);
            snprintf(l[3], 40, "CLANS LOST");
        }
    } else {
        snprintf(v[0], 48, "%d", CE.day);
        snprintf(l[0], 40, "DAYS SURVIVED");
        snprintf(v[1], 48, "%d", CE.mx);
        snprintf(l[1], 40, "MOST SURVIVORS");
        snprintf(v[2], 48, "%d", CE.rw);
        snprintf(l[2], 40, "CLANS SURVIVED");
        int tick = t >= ts + 0.75f;
        snprintf(v[3], 48, "<font color=\"#e04848\">%d</font>",
                 tick ? CE.rl : (CE.rl > 0 ? CE.rl - 1 : 0));
        snprintf(l[3], 40, "CLANS LOST");
        if (CeAt(1, 30, ts + 0.75f, t))
            CE_PLAY(THUD_HEAVY, 0.55f * g_ce_sfx_gain);
    }
    float ast = CeRamp(t, ts, ts + 0.6f);
    for (int i = 0; i < 4; i++) {
        char p[64];
        float ai = CeRamp(t, ts + i * 0.1f, ts + 0.5f + i * 0.1f);
        float cx = 520.0f + 700.0f * (i + 0.5f);
        snprintf(p, sizeof p, "anim-group.end.cePage.ceStats.ceSV%d", i);
        UiText(p, v[i]);
        WAlpha(UiW(p), ai);
        WPivot(UiW(p), cx - 330.0f, 1620.0f + 20.0f * (1.0f - ai));
        snprintf(p, sizeof p, "anim-group.end.cePage.ceStats.ceSL%d", i);
        UiText(p, l[i]);
        WAlpha(UiW(p), ai);
        if (i) {
            snprintf(p, sizeof p, "cePage.ceStats.ceSD%d", i);
            CeA(p, ast * 0.6f);
        }
        if (CeAt(1, 32 + i, ts + i * 0.1f, t))
            CE_PLAY(THUD, 0.22f * g_ce_sfx_gain);
    }
    float ab = CeRamp(t, ts + 0.5f, ts + 1.0f);
    CeA("cePage.ceBtns", ab);
    if (CE.ph == CE_RANK && t >= ts + 0.5f) {
        CE.ph = CE_DONE;
        CE.finished = 1;
        Xlog("clanend: stats up - START A NEW CLAN / READ THE JOURNAL");
    }
}

// ---- the page was left some other way than our buttons: the music fades (the journal keeps it: back from it shows the finished page)
static void CeAway(int to_journal)
{
    if (!CE.ph)
        return;
    if (!to_journal) {
        CeAudioStop(1200);
        Xlog("clanend: page left - music out");
    }
    CE.ph = 0;
}

// ---- the lobby music fades out under the clan end page, so the two never play over each other.
// (the game's suppress-music request, tried first, left the lobby music audible.) While the end page runs, and after it for as long as
// our end music still plays (READ THE JOURNAL keeps it), the game's music is switched to the empty rogue-player-death playlist (the same cut
// the match uses on our death) and switched back to it whenever winmm restarts the lobby playlist; when both are over, the lobby playlist is
// started again. Checked every 250 ms from feature_menu_music_log. t2r-ce-menu-music.txt = the lobby music plays on (old).
static uint64_t MusicCurrent(void);
static uint64_t MusicSid(const char *name);
static uint64_t MusicPlay(const char *name, const char *why);
static uint64_t g_ceLobbyTick;        // GetTickCount64 of the end page's last frame
static int g_ceLobbyCut;      // we took the lobby playlist down
static void CeMenuMusicHold(uint64_t now)
{
    g_ceLobbyTick = now;
}

static int CeMusicLive(void)
{                               // one of our end music voices (win / lost, both channels) still plays
    if (g_mixState != 1)
        return 0;
    const int16_t *w0 = CePcm(CE_MUS_WIN_OFF), *w1 =
        w0 + CE_MUS_WIN_N, *l0 = CePcm(CE_MUS_LOST_OFF), *l1 =
        l0 + CE_MUS_LOST_N;
    int live = 0;
    EnterCriticalSection(&g_mixCs);
    for (int v = 0; v < MIX_VOICES && !live; v++) {
        const int16_t *s = g_mixv[v].src;
        if (g_mixv[v].on && (s == w0 || s == w1 || s == l0 || s == l1))
            live = 1;
    }
    LeaveCriticalSection(&g_mixCs);
    return live;
}

// winmm UpdateMenuMusic (net-gui2.cpp) re-fires the lobby playlist when it is not current and its
// g_menuMusicFiredMs (winmm 0x180189BD0) is over 2 s old. Keep that stamp fresh while we hold the cut.
// Both code sites that read/write it are checked first, so another winmm build is left alone.
static void CeWinmmRefireHold(uint64_t now)
{
    static int ok = -1;
    if (ok < 0) {
        static const uint8_t rd[] = { 0x48, 0x2b, 0x05, 0x74, 0x69, 0x12, 0x00 };       // sub rax,[rip+..] -> 0x180189BD0
        static const uint8_t wr[] = { 0x48, 0x89, 0x05, 0xbf, 0x68, 0x12, 0x00 };       // mov [rip+..],rax -> 0x180189BD0
        ok = !memcmp((const void *) T2R_CODE(kCodeClanRead), rd, sizeof rd)
            && !memcmp((const void *) T2R_CODE(kCodeClanWrite), wr,
                       sizeof wr);
        Xlog("clan end page: winmm lobby re-fire stamp %s",
             ok ? "found (held while the page runs)" :
             "NOT found (other winmm build) - lobby music may come back");
    }
    if (ok)
        *(volatile uint64_t *) T2R_SYM(kClanTimestamp) = now;
}

static void CeLobbyTick(uint64_t now)
{
    static int off = -1;
    static uint64_t chk, fchk;
    if (now - chk < 250)
        return;
    chk = now;
    if (off < 0 || now - fchk > 2000) {
        fchk = now;
        off =
            GetFileAttributesA("t2r-ce-menu-music.txt") !=
            INVALID_FILE_ATTRIBUTES;
    }
    int want = !off && MatchSubstate() != 6
        && (now - g_ceLobbyTick < 600
            || (g_ceLobbyCut && CeMusicLive()));
    uint64_t menu = MusicSid("music-playlist-t2r-credits-in-menu");
    if (want) {
        CeWinmmRefireHold(now);
        uint64_t cur = MusicCurrent();
        if (!g_ceLobbyCut || cur == menu) {
            MusicPlay("music-playlist-rogue-player-death",
                       g_ceLobbyCut ?
                       "clan end page: the lobby playlist came back - cut again"
                       : "clan end page: lobby music out");
            g_ceLobbyCut = 1;
        }
    } else if (g_ceLobbyCut) {
        g_ceLobbyCut = 0;
        if (MatchSubstate() != 6) {
            MusicPlay("music-playlist-t2r-credits-in-menu",
                       "clan end page and its music over: lobby music back");
            CeWinmmRefireHold(now);
        }
    }
}

// ---- per frame. Returns 1 = START A NEW CLAN (the caller crossfades to the faction pick), 2 = back (close the page), 0 = keep going.
static int CeTick(const ClanT *c)
{
    (void) c;
    uint64_t now = GetTickCount64();
    CeMenuMusicHold(now);
    CeImgTick(now);
    if (CE.hold) {
        if ((g_entSt != 0 || g_ldsHold) && now - CE.hold_at < 9000) { // (cap: the entry waits <= 5 s, the loading page fades 1.6 s)
            CE.t0 = now;
            CE.bed_at = now;
            CeA("ceFade", 1.0f);
            return 0;
        }
        CE.hold = 0;
        CE.t0 = now;
        CE.bed_at = now;
        Xlog("clanend: entry hand-off over after %llu ms - the art rises from black", (unsigned long long) (now - CE.hold_at));
    }
    float t = (now - CE.t0) / 1000.0f;
    int sel =
        MenuEdge(0x6695c5b7921bb5ddULL /* menu-select */ , &CE.h_sel);
    int act = MenuEdge(FNV64("menu-action"), &CE.h_act);
    int back = MenuEdge(FNV64("menu-back"), &CE.h_back);
    if (g_clickEdge) {         // a click moves the screens on; on the stats page only the two prompts act
        if (CE.ph != CE_DONE)
            sel = 1;
        else if (UiMouseOver
                 ("anim-group.end.cePage.ceBtns.ceBtnBText", 1)
                 ||
                 UiMouseOver("anim-group.end.cePage.ceBtns.ceBtnBIcon",
                               1))
            act = 1;
        else if (UiMouseOver
                 ("anim-group.end.cePage.ceBtns.ceBtnAText", 1)
                 ||
                 UiMouseOver("anim-group.end.cePage.ceBtns.ceBtnAIcon",
                               1))
            sel = 1;
    }
    if (g_testAutofind && !CE.test) {  // unattended A/B: through the screens one press a second
        static uint64_t af;
        if (!af || now - af > 1000) {
            af = now;
            sel = 1;
        }
    } else if (g_testAutofind) {       // a demo on an unattended A/B: presses only at the waiting points (CONTINUE after 2.5 s,
        static uint64_t wt;
        static int wph;         // the stats after 6 s), so a capture sees every animation whole
        if (CE.ph != wph) {
            wph = CE.ph;
            wt = now;
        }
        if ((CE.ph == CE_WAIT && now - wt > 2500)
            || (CE.ph == CE_DONE && now - wt > 6000)) {
            sel = 1;
            wt = now;
            Xlog("clanend: test autofind press (phase %d)", CE.ph);
        }
    }
    CE.skip_to_wait = 0;
    // input per phase
    if (CE.ph == CE_ART && sel) {       // skip the title / roll call to the waiting point
        float re = CeRollEnd() + 0.6f;
        if (t < re) {
            CE.skip_to_wait = 1;
            CE.t0 -= (uint64_t) ((re - t) * 1000.0f);
            t = re;
        }
        CE.ph = CE_WAIT;
        sel = 0;
        Xlog("clanend: skipped to continue");
    }
    if (CE.ph == CE_WAIT && sel) {
        CE.tp = now;
        CE.ph = CE_GO;
        sel = 0;
        UiSfx("sfx-select");
        if (CE.win)
            CE_PLAY(SWELL_WIN, 0.30f * g_ce_sfx_gain);
        else
            CE_PLAY(SWELL_LOST, 0.30f * g_ce_sfx_gain);
        Xlog("clanend: continue at %.1f s", t);
    }
    float rt = CE.tr ? (now - CE.tr) / 1000.0f : -1.0f;
    if ((CE.ph == CE_GO || CE.ph == CE_RANK) && sel && CE.tr) { // skip the rank beat to the stats
        float ts = CeStatsT() + 1.0f;
        if (rt < ts) {
            CE.skip_to_wait = 1;
            CE.tr -= (uint64_t) ((ts - rt) * 1000.0f);
            rt = ts;
            Xlog("clanend: skipped to the stats");
        }
        sel = 0;
    }
    // the art and the page
    if (CE.fade_in) {           // 0.45 s from the page underneath into the black, then the art comes up (ceFade)
        float fa = CeRamp(t, 0.0f, 0.45f);
        WAlpha(UiW("anim-group.end"), fa < 0.003f ? 0.003f : fa);
        if (fa >= 1.0f)
            CE.fade_in = 0;
    }
    CeArt(t);
    CeA("ceFade", 1.0f - CeRamp(t, 0.0f, 1.2f));
    CeUi(t);
    float pa = 0.0f;
    if (CE.tp) {
        float d = (now - CE.tp) / 1000.0f;
        pa = CE.win ? CeRamp(d, 1.0f, 2.2f) : CeRamp(d, 1.4f, 2.6f);
        if (!CE.win)
            CeA("ceFade", CeRamp(d, 1.2f, 2.6f));     // lost: the art goes to black under the page
        if (!CE.tr && d >= (CE.win ? 2.4f : 2.8f)) {
            CE.tr = now;
            CE.ph = CE_RANK;
            rt = 0;
            Xlog("clanend: rank beat (%s)",
                 CE.win ? "chalk rank" : "tally");
        }
    }
    if (CE.finished && !CE.tp)
        pa = 1.0f;
    CeA("cePage", pa);
    float dip = 0.0f;
    if (rt >= 0) {
        if (CE.win)
            CeRankWin(rt);
        else
            CeRankLost(rt, &dip);
        CeStats(rt);
    } else if (CE.finished) {
        CE.tr = now - 30000;
    }                           // back from the journal: everything at its end next frame
    if (!CE.win)
        CeA("cePage.ceTally", 1.0f - dip);
    // sound: the music ducks under the UI sounds, the bed under the rank beat; a new bed copy before the old one ends
    float gm = g_ceMusGain * (now < CE.mus_dk ? 0.56f : 1.0f);
    float gb = (CE.win ? 0.30f : 0.36f) * (CE.tr ? 0.4f : 1.0f);        // (the bed used to keep the music's gain: too loud under the rank beat)
    CeVoices(0, gm, gb, 0);
    if (!CE.finished || CE.tp) {
        if (CE.bed_at && now >= CE.bed_at) {
            if (PcmPlayRate
                (CePcm(CE.win ? CE_BED_WIND_OFF : CE_BED_FIRE_OFF),
                 CE.win ? CE_BED_WIND_N : CE_BED_FIRE_N, gb, 24000))
                CE.bed_n++;
            CE.bed_at = now + 21500;
        }
    }
    CE.last_t = t;
    if (CE.ph == CE_DONE) {
        if (sel) {
            CeAudioStop(1500);
            Xlog("clanend: START A NEW CLAN");
            // gui2 fades per item, so in the crossfade the 96 % dark page thinned over the art and the key art flashed up for ~0.3 s:
            // the art goes now (the page covers it), the crossfade is dark page -> faction pick
            static const char *const A[] =
                { "ceArt", "ceScratch", "ceVig", "ceGrain", "ceUI" };
            for (unsigned i = 0; i < sizeof A / sizeof *A; i++)
                CeV(A[i], 0);
            return 1;
        }
        if (act) {
            UiSfx("sfx-select");
            g_ceJournalTab = 1;
            Xlog("clanend: READ THE JOURNAL - pushing to1/clan13");
            PushPage("to1/clan13");
        }
        if (back) {
            CeAudioStop(800);
            Xlog("clanend: back - page closed");
            return 2;
        }
    }
    return 0;
}
