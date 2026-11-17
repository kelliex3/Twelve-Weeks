// Launch notice: once per game start, the unfinished-state notice (to1/launch-notice, tools/features/notice_widgets.py). It is
// shown once per run. It is pushed on the MP menu while the TWELVE WEEKS entry page (loadseq.c, TITLE stack, drawn over the
// menu stack) still covers the screen, and loadseq holds its hand-off fade until the notice is up - so when the title fades out the notice
// is already there, under the TWELVE WEEKS loading screen.
//   - I ACKNOWLEDGE is locked for LN_READ_MS from the moment the title has gone (countdown in its label, a bar along the box's bottom edge
//     empties); CLOSE / CLOSE, DON'T SHOW AGAIN are greyed until it is ticked. Back does nothing (the notice must be acknowledged).
//   - CLOSE, DON'T SHOW AGAIN writes t2r-notice.txt ("seen <LN_VER>") next to the exe; LN_VER goes up when the text changes enough to show
//     it to everyone again. After the first close the notice never comes back in this run (not after matches).
//   - while it is due / up, the clan pages (side pick, event, post-match ...) wait (feature_clan: ln_busy()).
//   - text: English with <font color> emphasis; the loc layer translates it piece by piece (tools/loc/pl/11-notice.txt).
//   - keyboard / pad: up / down / left / right + select; mouse: hover focuses, a click acts on the control under the cursor.
// t2r-test-autofind.txt (A/B automation): ticks + closes itself once unlocked.   Logs: 'notice: ...'.   Included from t2r-ext.c before the clan event code.
#define LN_READ_MS 10000
#define LN_VER 1
#define LN_EM "<font color=\"#ECE9E2\">"
static struct {
    int st, pushes;
    uint64_t pushed_at, hold_t0;
} g_ln;                         // st: 0 unknown, 1 due, 2 pushed, 3 up, 4 done
static uint64_t g_lnPage;

static int LnSeen(void)
{
    
FILE * f = ExeSideOpen("t2r-notice.txt", "r");
    
if (!f)
        return 0;
    
int v = 0;
    if (fscanf(f, "seen %d", &v) != 1)
        v = 0;
    
fclose(f);
    
return v >= LN_VER;

}


static void LnInit(void)
{
    
if (g_ln.st)
        return;
    
if (!g_lnPage)
        g_lnPage = PageId("launch-notice");
    
if (LnSeen()) {
        g_ln.st = 4;
        Xlog("notice: not shown (t2r-notice.txt: seen, version %d)",
             LN_VER);
    }
    
    else {
        g_ln.st = 1;
        Xlog("notice: due this launch (version %d)", LN_VER);
    }

}


static int LnBusy(void)
{
    LnInit();
    return g_ln.st >= 1 && g_ln.st <= 3;
}


// loadseq.c: hold the entry page's hand-off fade while the notice is due but not up yet (at most 4 s: a notice that never comes must not
// keep the title on screen)
static int LnHoldFade(uint64_t now)
{
    
LnInit();
    
if (g_ln.st != 1 && g_ln.st != 2)
        return 0;
    
if (!g_ln.hold_t0) {
        g_ln.hold_t0 = now;
        Xlog("notice: the title's hand-off fade waits for the notice");
    }
    
if (now - g_ln.hold_t0 < 4000)
        return 1;
    
static int nl;
    if (LOGCAP(nl, 1))
        Xlog("notice: not up 4 s after the hand-off - the title fades anyway");
    
return 0;

}


static int UiSelectIsMouse(void);

static void LnFill(int left_s)
{
    
char b[160];
    
UiText("anim-group.lnBox.lnP0",
             "TWELVE WEEKS IS OUT, BUT IT IS STILL " LN_EM
             "UNFINISHED</font>. THIS IS AN EARLY, WORK-IN-PROGRESS RELEASE, "
             LN_EM "NOT THE FINISHED GAME</font>.");
    
UiText("anim-group.lnBox.lnHead", "WHAT THAT MEANS");
    
UiText("anim-group.lnBox.lnB0",
             "PERFORMANCE IS " LN_EM
             "NOT FINAL</font>: STUTTERS, LONG LOADS AND FRAME DROPS CAN HAPPEN.");
    
UiText("anim-group.lnBox.lnB1",
             "GAMEPLAY IS " LN_EM
             "NOT FINAL</font>: WEAPONS, MELEE, MAPS AND BALANCE STILL CHANGE FROM UPDATE TO UPDATE.");
    
UiText("anim-group.lnBox.lnB2",
             "EXPECT " LN_EM
             "BUGS AND CRASHES</font>, DISCONNECTS, BROKEN ANIMATIONS AND ROUGH EDGES.");
    
UiText("anim-group.lnBox.lnB3",
             "SOME FEATURES ARE " LN_EM "MISSING OR PLACEHOLDER</font>.");
    
UiText("anim-group.lnBox.lnP1",
             "BY PLAYING, YOU AGREE TO REPORT THE BUGS YOU FIND WITH "
             LN_EM
             "REPORT A BUG</font> IN THE LAUNCHER. SAY WHAT HAPPENED AND WHERE. EVERY REPORT HELPS.");
    
        // (the countdown flickered every second - the timer fix: double-buffered through the hidden twin lnAckB)
        if (left_s > 0) {
        snprintf(b, sizeof b, "I ACKNOWLEDGE (%d)", left_s);
        UiTextDb("anim-group.lnBox.lnCheck.lnAck", b);
    }
    
    else
        UiTextDb("anim-group.lnBox.lnCheck.lnAck", "I ACKNOWLEDGE");
    
UiText("anim-group.lnBox.lnBtn0.lnBtn0Tx", "CLOSE");
    
UiText("anim-group.lnBox.lnBtn1.lnBtn1Tx",
             "CLOSE, DON'T SHOW AGAIN");

}


static void LnPut(const char *path, float *last, float a)
{                               // alpha writes only on change (re-asserted every 30 frames)
    if (*last == a && g_frame % 30)
        return;
    
*last = a;
    WAlpha(UiW(path), a);

}


static void LnVis(const char *path, int *last, int on)
{
    
if (*last == on && g_frame % 30)
        return;
    
*last = on;
    UiVisible(path, on);

}


static void LnClose(int never)
{
    
if (never) {
        
FILE * f = ExeSideOpen("t2r-notice.txt", "w");
        
if (f) {
            fprintf(f, "seen %d\n", LN_VER);
            fclose(f);
        }
        
Xlog("notice: closed - don't show again (t2r-notice.txt %s)",
              f ? "written" : "NOT written");
    
} else
        Xlog("notice: closed");

}


static void LnPage(int entered)
{
    
static uint64_t t0, read_t0, out_at;
    static int focus, ticked, h_sel, h_back, h_u, h_d, h_l, h_r, refill,
        gone, last_left, hov_last, never;
    
static float a_ck, a_b0, a_b1, a_fr, a_grp, a_dim, a_tm;
    static int v_tick, v_fo0, v_fo1, v_icon;
    
uint64_t now = GetTickCount64();
    
if (entered) {
        
g_ln.st = 3;
        g_ln.pushes = 0;
        
t0 = now;
        read_t0 = 0;
        out_at = 0;
        focus = 0;
        ticked = 0;
        gone = 0;
        refill = 10;
        last_left = -1;
        hov_last = -1;
        never = 0;
        
h_sel = h_back = h_u = h_d = h_l = h_r = 1;    // whatever is held when the page opens
        a_ck = a_b0 = a_b1 = a_fr = a_grp = a_dim = a_tm = -1.0f;
        v_tick = v_fo0 = v_fo1 = v_icon = -1;
        
LnFill(LN_READ_MS / 1000);
        
Xlog("notice: page up (%s)",
              g_ldsHold ? "under the TWELVE WEEKS title" :
              "over the MP menu");
    
} else if (refill && !--refill)
        LnFill(last_left > 0 ? last_left : 0);
    
if (gone) {
        WAlpha(UiW("anim-group"), 0.0f);
        WAlpha(UiW("lnDimG"), 0.0f);
        return;
    }
    
        // the reading clock starts once the title has gone (the page may come up under it)
        if (!read_t0 && !g_ldsHold) {
        read_t0 = now;
        Xlog("notice: on screen - %d s reading lock", LN_READ_MS / 1000);
    }
    
int64_t rd = read_t0 ? (int64_t) (now - read_t0) : 0;
    
int locked = !read_t0 || rd < LN_READ_MS;
    
int left = locked ? (int) ((LN_READ_MS - rd + 999) / 1000) : 0;
    
if (left != last_left) {
        last_left = left;
        LnFill(left);
    }
    
int sel = MenuEdge(0x6695c5b7921bb5ddULL /* menu-select */ , &h_sel);
    
int back = MenuEdge(FNV64("menu-back"), &h_back);
    
int up = MenuEdge(0xb470a102b9a3f2d0ULL /* menu-up */ , &h_u), dn =
        MenuEdge(0x14d6eedde980c845ULL /* menu-down */ , &h_d);
    
int lf = MenuEdge(0xe087ad207d99e6a0ULL /* menu-left */ , &h_l), rt =
        MenuEdge(0xa23d5059e72fc835ULL /* menu-right */ , &h_r);
    
if (out_at) {              // leaving: box + dim fade out in 0.3 s, then the page pops
        float f = 1.0f - (float) (now - out_at) / 300.0f;
        
if (f < 0.0f)
            f = 0.0f;
        
WAlpha(UiW("anim-group"), f);
        WAlpha(UiW("lnDimG"), f);
        
if (now - out_at >= 300) {
            gone = 1;
            g_ln.st = 4;
            PopPage();
            LnClose(never);
        }
        
return;
    
}
    
        // mouse: the control under the cursor takes the focus when the cursor moves onto it; a click acts on the control under the cursor
    int hov = -1;
    
if (!locked) {
        
if (UiMouseOver("anim-group.lnBox.lnCheck", 1))
            hov = 0;
        
        else if (ticked && UiMouseOver("anim-group.lnBox.lnBtn0", 1))
            hov = 1;
        
        else if (ticked && UiMouseOver("anim-group.lnBox.lnBtn1", 1))
            hov = 2;
    
}
    
if (hov >= 0 && hov != hov_last && hov != focus) {
        focus = hov;
        UiSfx("sfx-cycle");
    }
    
hov_last = hov;
    
int act = -1;
    
if (g_clickEdge && hov >= 0)
        act = hov;
    
    else if (sel && !UiSelectIsMouse())
        act = focus;
    
if (g_testAutofind && !locked && now - read_t0 > LN_READ_MS + 1000) {
        if (!ticked)
            act = 0;
        else {
            act = 1;
            Xlog("test autofind: notice closed");
        }
    }
    
if (back || (sel && act < 0 && !UiSelectIsMouse()))
        UiSfx("sfx-menu-error");
    
if (act == 0) {
        
if (locked)
            UiSfx("sfx-menu-error");
        
        else {
            ticked = !ticked;
            focus = ticked ? 1 : 0;
            UiSfx("sfx-select");
            Xlog("notice: %s", ticked ? "acknowledged" : "unticked");
        }
    
} else if (act == 1 || act == 2) {
        
if (ticked) {
            never = act == 2;
            out_at = now;
            UiSfx("sfx-select");
        }
        
        else
            UiSfx("sfx-menu-error");
    
}
    
if (!locked && act < 0) {  // keyboard / pad focus moves (the buttons only once ticked)
        int f0 = focus;
        
if (ticked) {
            
if (up && focus > 0)
                focus = 0;
            
            else if (dn && focus == 0)
                focus = 1;
            
            else if (lf && focus == 2)
                focus = 1;
            
            else if (rt && focus == 1)
                focus = 2;
        
} else
            focus = 0;
        
if (focus != f0)
            UiSfx("sfx-cycle");
    
}
    
        // look
    float fin = (float) (now - t0) / 300.0f;
    if (fin > 1.0f)
        fin = 1.0f;
    
LnPut("anim-group", &a_grp, fin);
    
LnPut("lnDimG", &a_dim, fin);
    
LnPut("anim-group.lnBox.lnCheck", &a_ck, locked ? 0.4f : 1.0f);
    
LnPut("anim-group.lnBox.lnCheck.lnCheckFr", &a_fr,
            locked ? 0.35f : focus == 0 ? 1.0f : 0.45f);
    
LnPut("anim-group.lnBox.lnBtn0", &a_b0, ticked ? 1.0f : 0.35f);
    
LnPut("anim-group.lnBox.lnBtn1", &a_b1, ticked ? 1.0f : 0.35f);
    
LnVis("anim-group.lnBox.lnCheck.lnTick", &v_tick, ticked);
    
LnVis("anim-group.lnBox.lnBtn0.lnBtn0Fo", &v_fo0, ticked
            && focus == 1);
    
LnVis("anim-group.lnBox.lnBtn1.lnBtn1Fo", &v_fo1, ticked
            && focus == 2);
    
LnVis("anim-group.lnHint.lnHintIcon", &v_icon, !locked);
    
UiTextDb("anim-group.lnHint.lnHintTx",
                locked ? "READ THE NOTICE" : ticked ? "SELECT" : "TICK");
    
float tm =
        !read_t0 ? 1.0f : locked ? 1.0f - (float) rd / LN_READ_MS : 0.0f;
    
if (fabsf(tm - a_tm) > 0.002f || (tm == 0.0f && a_tm != 0.0f)) {
        
a_tm = tm;
        
uint64_t w = UiW("anim-group.lnBox.lnTimer");
        
if (tm > 0.0f) {
            WVisible(w, 1);
            WScale(w, tm, 1.0f);
        } else
            WVisible(w, 0);
    
}

}


// the click gate (ui_click_gate_tick): the notice's clickable controls under the cursor
static int LnClickHit(void)
{
    
return UiMouseOver("anim-group.lnBox.lnCheck", 1)
        || UiMouseOver("anim-group.lnBox.lnBtn0", 1)
        || UiMouseOver("anim-group.lnBox.lnBtn1", 1);

}


// once a frame (main tick): push the notice when the MP menu is on top
static void LnTick(void)
{
    
LnInit();
    
uint64_t top = TopPage(), now = GetTickCount64();
    
if (g_ln.st == 4) {        // done: a popped page can still be drawn 1-2 frames - kept at alpha 0 (no ghost, never re-entered)
        if (top == g_lnPage) {
            WAlpha(UiW("anim-group"), 0.0f);
            WAlpha(UiW("lnDimG"), 0.0f);
        }
        
return;
    
}
    
static uint64_t PMAIN, last_top;
    
if (!PMAIN)
        PMAIN = PageId("main-menu");
    
int entered = top != last_top;
    
last_top = top;
    
if (top == g_lnPage) {
        LnPage(entered || g_ln.st != 3);
        return;
    }
    
if (g_ln.st == 3) {
        g_ln.st = 4;
        Xlog("notice: page gone (closed another way)");
        return;
    }
    
if (top != PMAIN)
        return;
    
if (g_ln.st == 2) {
        
if (now - g_ln.pushed_at <= 1500)
            return;
        
if (g_ln.pushes >= 3) {
            g_ln.st = 4;
            Xlog("notice: to1/launch-notice never came up - dropped");
            return;
        }
    
}
    
PushPage("to1/launch-notice");
    
g_ln.st = 2;
    g_ln.pushes++;
    g_ln.pushed_at = now;
    
Xlog("notice: to1/launch-notice pushed (try %d, entry %s)",
          g_ln.pushes, g_ldsHold ? "title still up" : "title gone");

}


