// Kick notice: an admin kick or the AFK
// removal was a 12 s toast next to the menu title (in the match it was cut by the match end the next frame, then it came back ~27 s later
// on the menu) - easy to miss. Now the kick text opens to1/kick-notice (tools/features/notice_widgets.py build_kick_page) on the MP menu:
// dim + box (KICKED FROM THE MATCH, the reason / the AFK seconds), one CLOSE button. It stays until closed (select / back / click; the first
// KN_LOCK_MS ignore input so a held button cannot close it unread).
//   - the text comes from the daemon in the game's language (op 0x11 right before the 0x0d back-to-menu, msg_hook; or the notice poll's
//     kind-p note when the session socket was gone, notice.c); both are parsed (English + Polish) into English lines the loc layer
//     translates (tools/loc/pl/11-notice.txt). The admin's reason is shown as typed.
//   - the admin team move ("TEAMS: ...") is not a kick: it keeps its toast and searches again by itself.
//   - while due / up the clan pages wait (feature_clan: kn_busy()); it waits for the TWELVE WEEKS title and the launch notice.
//   - older gui2-widgets.pak without the page: the toast as before.
// t2r-test-autofind.txt: closes itself after 3 s.   Logs: 'kick notice: ...'.   Included from t2r-ext.c after launchnote.c.
#define KN_LOCK_MS 800
static struct {
    int st, pushes;
    uint64_t pushed_at, set_at;
    char text[300], msg[400];
    const char *title;
} g_kn;                         // st: 0 none, 1 due, 2 pushed, 3 up
static uint64_t g_knPage;

static int KnBusy(void)
{
    return g_kn.st >= 1 && g_kn.st <= 3;
}


// daemon text (tools/daemon.py SRV_TEXT kicked / kicked_why / afk_kicked, EN + PL) -> the box's message
static void KnParse(const char *t, char *out, size_t n)
{
    
char r[200] = "";
    
const char *q = strstr(t, "Reason: \"");
    
if (!q)
        q = strstr(t, "Pow\xc3\xb3" "d: \"");
    
if (q) {
        
q = strchr(q, '"') + 1;
        
const char *e = strrchr(q, '"');
        
size_t k = 0;
        
for (const char *p = q; *p && p != e && k + 1 < sizeof r; p++)
            if (*p != '<' && *p != '>')
                r[k++] = *p;
        
r[k] = 0;
    
}
    
        // ping limit: "... your ping to the host was <ms> ms for over <s> seconds (limit <lim> ms)." / PL "... ping do hosta wynosi\xc5\x82 <ms> ms przez ponad <s> s (limit <lim> ms)."
    const char *pg = strstr(t, "ping");
    
if (pg) {
        
int v[3] = { 0, 0, 0 }, k = 0;
        
for (const char *c = pg; *c && k < 3; c++)
            if (*c >= '0' && *c <= '9') {
                v[k++] = atoi(c);
                while (c[1] >= '0' && c[1] <= '9')
                    c++;
            }
        
g_kn.title = "KICKED FOR HIGH PING";
        
if (k == 3)
            snprintf(out, n,
                     "YOUR PING TO THE HOST WAS <font color=\"#ECE9E2\">%d MS</font> FOR OVER %d SECONDS. THE LIMIT IS %d MS.",
                     v[0], v[1], v[2]);
        
        else
            snprintf(out, n, "YOUR PING TO THE HOST WAS TOO HIGH.");
        
return;
    
}
    
g_kn.title = "KICKED FROM THE MATCH";
    
int secs = 0;
    
const char *a = strstr(t, "no input for ");
    
if (a)
        secs = atoi(a + 13);
    
    else if ((a = strstr(t, "przez ")) && strstr(t, "(AFK)"))
        secs = atoi(a + 6);
    
if (secs > 0)
        snprintf(out, n, "NO INPUT FOR %d SECONDS (AFK).", secs);
    
    else if (r[0])
        snprintf(out, n,
                 "AN ADMIN REMOVED YOU FROM THE MATCH. REASON: <font color=\"#ECE9E2\">%s</font>",
                 r);
    
    else
        snprintf(out, n, "AN ADMIN REMOVED YOU FROM THE MATCH.");

}


// msg_hook / notice.c: a kick text came in. 1 = the box takes it (the caller shows no toast), 0 = no page in this build (toast as before).
static int KnSet(const char *t)
{
    
if (!t || MsgIsMove(t))
        return 0;
    
if (!g_knPage)
        g_knPage = PageId("kick-notice");
    
if (!g_knPage) {
        static int nl;
        if (LOGCAP(nl, 2))
            Xlog("kick notice: no to1/kick-notice page (older gui2-widgets.pak) - toast");
        return 0;
    }
    
uint64_t now = GetTickCount64();
    
if (!strcmp(g_kn.text, t) && now - g_kn.set_at < 120000ULL)
        return 1;               // the same kick twice (0x11 + the notice poll note)
    snprintf(g_kn.text, sizeof g_kn.text, "%s", t);
    
KnParse(t, g_kn.msg, sizeof g_kn.msg);
    
g_kn.set_at = now;
    g_kn.pushes = 0;
    
if (g_kn.st != 3)
        g_kn.st = 1;
    
Xlog("kick notice: \"%.120s\" -> due (substate %d)", t,
          (int) MatchSubstate());
    
return 1;

}


static void KnPage(int entered)
{
    
static uint64_t t0, out_at;
    static int h_sel, h_back, gone, hov_last, refill;
    
static float a_grp, a_dim, a_btn;
    
uint64_t now = GetTickCount64();
    
if (entered) {
        
g_kn.st = 3;
        t0 = now;
        out_at = 0;
        gone = 0;
        hov_last = -1;
        refill = 10;
        
h_sel = h_back = 1;    // whatever is held when the page opens
        a_grp = a_dim = a_btn = -1.0f;
        
Xlog("kick notice: page up - \"%.120s\"", g_kn.msg);
    
}
    
if (entered || (refill && !--refill)) {
        
UiText("anim-group.knBox.knTitle",
                 g_kn.title ? g_kn.title : "KICKED FROM THE MATCH");
        
UiText("anim-group.knBox.knMsg", g_kn.msg);
        
UiText("anim-group.knBox.knBtn.knBtnTx", "CLOSE");
        
UiText("anim-group.knHint.knHintTx", "CLOSE");
    
}
    
if (gone) {
        WAlpha(UiW("anim-group"), 0.0f);
        WAlpha(UiW("knDimG"), 0.0f);
        return;
    }
    
int sel = MenuEdge(0x6695c5b7921bb5ddULL /* menu-select */ , &h_sel);
    
int back = MenuEdge(FNV64("menu-back"), &h_back);
    
if (out_at) {              // leaving: box + dim fade out in 0.25 s, then the page pops
        float f = 1.0f - (float) (now - out_at) / 250.0f;
        
if (f < 0.0f)
            f = 0.0f;
        
WAlpha(UiW("anim-group"), f);
        WAlpha(UiW("knDimG"), f);
        
if (now - out_at >= 250) {
            gone = 1;
            g_kn.st = 0;
            PopPage();
            Xlog("kick notice: closed");
        }
        
return;
    
}
    
int locked = now - t0 < KN_LOCK_MS;
    
int hov = UiMouseOver("anim-group.knBox.knBtn", 1);
    
if (hov && hov != hov_last && !locked)
        UiSfx("sfx-cycle");
    
hov_last = hov;
    
int act = !locked && ((g_clickEdge && hov)
                           || (sel && !UiSelectIsMouse()) || back);
    
if (g_testAutofind && now - t0 > 3000) {
        act = 1;
        Xlog("test autofind: kick notice closed");
    }
    
if (act) {
        out_at = now;
        UiSfx("sfx-select");
    }
    
float fin = (float) (now - t0) / 250.0f;
    if (fin > 1.0f)
        fin = 1.0f;
    
LnPut("anim-group", &a_grp, fin);
    
LnPut("knDimG", &a_dim, fin);
    
LnPut("anim-group.knBox.knBtn", &a_btn, locked ? 0.5f : 1.0f);

}


static int KnClickHit(void)
{
    return UiMouseOver("anim-group.knBox.knBtn", 1);
}


// once a frame (main tick, after ln_tick): push the box when the MP menu is on top, the title has gone and the launch notice is done
static void KnTick(void)
{
    
uint64_t top = TopPage(), now = GetTickCount64();
    
static uint64_t PMAIN, last_top;
    
if (!PMAIN)
        PMAIN = PageId("main-menu");
    
int entered = top != last_top;
    
last_top = top;
    
if (g_knPage && top == g_knPage) {
        
if (g_kn.st == 0) {
            WAlpha(UiW("anim-group"), 0.0f);
            WAlpha(UiW("knDimG"), 0.0f);
            return;
        }                       // popped, still drawn 1-2 frames
        KnPage(entered || g_kn.st != 3);
        
return;
    
}
    
if (g_kn.st == 3) {
        g_kn.st = 0;
        Xlog("kick notice: page gone (closed another way)");
        return;
    }
    
if (!g_kn.st || top != PMAIN || g_ldsHold || LnBusy())
        return;
    
if (g_kn.st == 2) {
        
if (now - g_kn.pushed_at <= 1500)
            return;
        
if (g_kn.pushes >= 3) {
            g_kn.st = 0;
            Xlog("kick notice: to1/kick-notice never came up - dropped");
            return;
        }
    
}
    
PushPage("to1/kick-notice");
    
g_kn.st = 2;
    g_kn.pushes++;
    g_kn.pushed_at = now;
    
Xlog("kick notice: to1/kick-notice pushed (try %d)", g_kn.pushes);

}


