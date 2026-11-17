// TWELVE WEEKS entry sequence: the page shown after the menu entry is pressed (to1/connecting, tools/features/connecting_widgets.py
// variant L) is a sequence, driven from here (the JSON starts with the black at alpha 0 and everything else hidden):
//   0 - LDS_BLACK_MS            short fade to black (bg alpha 0 -> 1, smoothstep)
//   - LDS_TITLE_AT              black hold
//   LDS_TITLE_AT + LDS_TITLE_MS the title "TWELVE WEEKS" fades in, slow smootherstep, as a DIM title (group t2rLoad, 20 % white) with the stinger; the
//                               same fade brings in the bright fill (group progress): the white title clipped to the real load progress, written by winmm
//                               to anim-group.progress; the engine's IProgressBar resizes its child barFull, a mask group (isMaskOrClip) around the bright
//                               title, so it clips, never scales; a feather strip at the mask's right edge washes the edge
//   end                         the world is ready (and the fade-in is over): hold LDS_HOLD_MS with the title white, then title + fill fade out over
//                               LDS_OUT_MS onto the black (the page then leaves into the menu scene's own black -> fade-in). A load faster than the
//                               fade-in only shows the page while it is on screen: the engine pops it (nothing here can hold a page winmm pops) - logged.
// t2r-load-bar.txt next to the exe: the fallback, the full white title + a 12 px torn bar along the bottom edge (the progress group is moved / scaled
// there, the mask clips the bar image).
// Stinger: candidate 1 of tools/rip_load_stinger.py (loadseq.pcm, mono s16 through the PCM mixer, music volume), 480_infected_horde_unaware_endtag_b
//   t2r-no-load-stinger.txt = silent.   t2r-no-load-seq.txt: the old behaviour (black page at once, title and fill shown, no sound).
// Logs: 'load seq: ...'.  Included from t2r-ext.c after pullout_sting.c.
#include "loadseq.h"
__asm__
    (".section .rdata,\"dr\"\n.balign 4\n.globl lds_blob\nlds_blob:\n.incbin \"loadseq.pcm\"\n.text\n");
extern const int16_t lds_blob[];
#define LDS_BLACK_MS 400
#define LDS_TITLE_AT 700
#define LDS_TITLE_MS 2600
#define LDS_BAR_AT (LDS_TITLE_AT + 900)
#define LDS_BAR_MS 1200
#define LDS_HOLD_MS 300
#define LDS_RW 2800             // progress rect width = loading_art.INK_W + FEATHER
#define LDS_PH 760              // its height = loading_art.TITLE_H
#define LDS_BARH 24             // fallback bar height (units) = loading_art.BAR_H
#define LDS_OUT_MS 700
#define LDS_RESUME_OUT_MS 1600  // the hand-off fade of the held page
#define LDS_MUTE_MS 1500        // the session fade-out at the resume (menu world load); the appear click comes seconds later
#define LDS_MUSIC_RAMP_MS 2000  // the menu music's start (cue slices start mid-stream: a decoder click) is ramped in from silence
typedef struct {
    int on, off, barmode, stage, snd, out;
    uint64_t t0, key, out_t0, leave_chk, rs_t0, fpath, fkey, rel_t0,
        last_t, mute_t0;
    int resume, popped, mute;
    float fill_w, bar_w;
    uint64_t bg, title, prog, full, bright, feather, barimg;
    float a_bg, a_t, a_b, a_p;
} LdsT;
static LdsT g_lds;
// The entry page opened from the TITLE screen's TWELVE WEEKS button lives on the TITLE page stack (0x1f5962a2ce9803c8), not
// the menu stack top_page() / ui_w() read, and page_id("connecting") hashes the menu stack's path - so the sequence never saw it. lds_w looks widgets up on the
// stack the page is on (g_lds_stack); the title-stack page is detected by its t2rLoad widget (only this page has it), no page hash needed.
static uint64_t g_ldsStack = MENU_STACK;
static uint64_t LdsW(const char *path)
{
    return ((fn_widget_t) T2R_SYM(kFnWidgetFind)) (path, g_ldsStack);
}

static int g_ldsOff = -1;
// (1) the fill is OWNED here: written every tick on the page's own stack, monotonic (never down), a time curve (ease-out toward ~92 % over the
// whole TWELVE WEEKS load, page + menu world) - winmm's value jumped to 100 % in one frame and restarted per load stage; 100 % only when the world is ready (the fade-out).
// (2) the engine pops the page when the menu world load starts, which showed black + the ext's own ring: the page is pushed again on the TITLE stack (silent) and shown
// complete (resume mode, fill carries on) until the entry hand-off releases the native black, then crossfades out with the world and is popped. g_lds_hold hides the ring.
static int g_ldsHold, g_lds_resume;
static uint64_t g_ldsFt0;
static float g_ldsFill;
#define LDS_FILL_TAU_MS 9000.0f
#define LDS_FILL_CAP 0.92f
static float LdsSmooth(float u)
{
    u = u < 0 ? 0 : u > 1 ? 1 : u;
    return u * u * u * (u * (u * 6.0f - 15.0f) + 10.0f);
}

static void LdsPut(uint64_t h, float *last, float a)
{
    if (!h)
        return;
    if (*last >= 0.0f && fabsf(a - *last) < 0.004f
        && !(a >= 1.0f && *last < 1.0f) && !(a <= 0.0f && *last > 0.0f))
        return;
    *last = a;
    WAlpha(h, a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a);
}

static int LdsResolve(uint64_t key)
{
    LdsT *L = &g_lds;
    L->bg = LdsW("anim-group.bg");
    L->title = LdsW("anim-group.t2rLoad");
    L->prog = LdsW("anim-group.progress");
    L->full = LdsW("anim-group.t2rLoad.t2rLoadTitleFull");
    L->bright = LdsW("anim-group.progress.barFull.t2rBright");
    L->feather = LdsW("anim-group.progress.barFull.t2rFeather");
    L->barimg = LdsW("anim-group.progress.barFull.t2rBarImg");
    L->key = key;
    L->a_bg = L->a_t = L->a_b = L->a_p = -1.0f;
    return L->bg && L->title && L->prog && L->bright && L->feather
        && L->barimg && L->full;
}

static void LdsFillWrite(uint64_t key, float v)
{
    LdsT *L = &g_lds;
    if (L->fkey != key || !L->fpath) {
        uint64_t pa[9] =
            { key, (uint64_t) "progress", 0, 0, 0, 0, 0, 0, 0 };
        uint64_t path = DcCall(FNV64("gui2-build-widget-path"), 9, pa);
        L->fpath = path == ~0ULL ? 0 : path;
        L->fkey = key;
        L->fill_w = -1.0f;
        static int n;
        if (LOGCAP(n, 4))
            Xlog("load seq: fill path %llx (the ext writes the progress bar percent itself)", (unsigned long long) L->fpath);
    }
    if (!L->fpath)
        return;
    if (L->fill_w >= 0.0f && fabsf(v - L->fill_w) < 0.002f && v < 1.0f)
        return;
    L->fill_w = v;
    float z = 0.0f;
    uint64_t pb[4] = { L->fpath, 0, 0, 12 };
    memcpy(&pb[1], &v, 4);
    memcpy(&pb[2], &z, 4);
    DcCall(FNV64("gui2-set-progress-bar-percent"), 4, pb);
}

static int LdsPick(void)
{                               // 0 = the shipped stinger, -1 = opted out (t2r-no-load-stinger.txt)
    FILE *f = ExeSideOpen("t2r-no-load-stinger.txt", "r");
    if (!f)
        return 0;
    fclose(f);
    return -1;
}

static int LdsWorldReady(void)
{                               // only a real answer counts (ms_world_ready treats 'cannot ask' as ready)
    uint64_t a[8] = { 0 };
    uint64_t r =
        DcCall(0x66637535ebbc34dfULL
                /* task-want-load-levels-fully-spawned? */ , 0, a);
    return r != ~0ULL && r != 0;
}

static void LdsTick(void)
{
    LdsT *L = &g_lds;
    if (g_ldsOff < 0) {
        FILE *f = ExeSideOpen("t2r-no-load-seq.txt", "r");
        g_ldsOff = f != NULL;
        if (f)
            fclose(f);
        if (g_ldsOff)
            Xlog("load seq: off (t2r-no-load-seq.txt)");
    }
    uint64_t now = GetTickCount64();
    static uint64_t poll, pg;
    if (!L->on && now < poll)
        return;
    poll = now + (L->on || g_lds_resume ? 50 : 150);    // (idle in the MP menu: a cheap probe 6 times a second)
    if (!pg)
        pg = PageId("connecting");
    int up = TopPage() == pg;
    if (up)
        g_ldsStack = MENU_STACK;
    else if (L->on ? g_ldsStack == TITLE_STACK : 1) {  // the title stack's entry page (TWELVE WEEKS pressed on the title screen)
        g_ldsStack = TITLE_STACK;
        up = ((uint64_t(*)(uint64_t)) T2R_SYM(kFnStackTop)) (TITLE_STACK) != 0 && LdsW("anim-group.t2rLoad") != 0;     // (the string lookup only when the title stack has a page)
        static int nl;
        if (up && !L->on && LOGCAP(nl, 3))
            Xlog("load seq: entry page found on the TITLE stack (top %llx)", (unsigned long long) ((uint64_t(*)(uint64_t)) T2R_SYM(kFnStackTop)) (TITLE_STACK));
    }
    if (!up) {
        if (g_lds_resume && !L->on && now - L->rs_t0 > 4000) {
            g_lds_resume = 0;
            g_ldsHold = 0;
            Xlog("load seq: resume - the page did not come back on the title stack, giving up (stock load screen)");
        }
        if (L->on) {
            float u =
                LdsSmooth((float) (now - L->t0 - LDS_TITLE_AT) /
                           LDS_TITLE_MS);
            Xlog("load seq: page left after %llu ms (title %.0f %%, stage %d%s)", (unsigned long long) (now - L->t0), u * 100.0f, L->stage, L->out ? ", faded out" : (now - L->t0 < LDS_TITLE_AT + LDS_TITLE_MS ? ", BEFORE the fade-in finished" : ""));
            L->on = 0;
            if (L->mute) {
                L->mute = 0;
                SessionVolume(1.0f);
            }
            if (L->resume)
                g_ldsHold = 0;
            else if (L->out || L->off || MatchSubstate() != 1)
                g_ldsHold = 0;
            else if (!L->out && !L->off && MatchSubstate() == 1) {     // the engine popped the page into the menu world load: keep the title up (no second loader)
                uint64_t a[2] = { TITLE_STACK, 0 };
                DcCall(FNV64("menu2-page-stack-enable-sounds"), 2, a);
                ((void (*)(uint64_t, const char *))
                 T2R_SYM(kFnStackPush)) (TITLE_STACK, "to1/connecting");
                g_lds_resume = 1;
                g_ldsHold = 1;
                L->rs_t0 = now;
                Xlog("load seq: resume - page pushed again on the title stack, the title stays up through the menu world load (the ring is hidden)");
            }
            L->resume = 0;
        }
        return;
    }
    uint64_t key = LdsW("anim-group");
    if (!key)
        return;
    if (!L->on || L->key != key) {
        if (!LdsResolve(key)) {
            static int n;
            if (LOGCAP(n, 3))
                Xlog("load seq: page up but the sequence widgets are missing (old gui2-widgets.pak?) - nothing animated");
            L->on = L->on ? 1 : 0;
            return;
        }
        if (!L->on) {
            L->on = 1;
            L->t0 = now;
            L->stage = 0;
            L->snd = 0;
            L->out = 0;
            L->off = g_ldsOff;
            L->popped = 0;
            L->resume = g_lds_resume;
            g_lds_resume = 0;
            if (L->resume) {
                L->t0 = now - (LDS_TITLE_AT + LDS_TITLE_MS) - 100;
                L->snd = 1;
                L->stage = 1;
                Xlog("load seq: resume - page up again (fill %.0f %%)",
                     (double) g_ldsFill * 100.0);
            } else {
                g_ldsHold = L->off ? 0 : 1;
                g_ldsFt0 = 0;
                g_ldsFill = 0.0f;
            }                   // (hold from the first page on: the ext ring must not flash in the 2 frames between the engine's pop and the re-push)
            L->mute = 0;
            L->rel_t0 = 0;
            L->last_t = 0;
            // An audio click ~0.7 s BEFORE the release = the menu world's puppet foley (SfxProcess ctor pairs
            // f5487-f5501, exe+1c307ac / 1c2fe67) starting under the black. The whole session is silent from the resume (menu world load) on, not from the release.
            if (L->resume) {
                L->mute = 1;
                L->mute_t0 = now;
                Xlog("load seq: resume - session muted for the whole menu world load (the menu scene's first foley is the appear click)");
            }
            L->fpath = 0;
            L->fill_w = -1.0f;
            // The title-clipped fill never moved (winmm's value won, the bright title jumped to full at once). The default is now the plain bar along the bottom
            // edge under the full white title, its width scaled by THIS file's monotonic value (no dependence on the progress mask); t2r-load-fill.txt = the title-clipped fill again.
            FILE *bf = ExeSideOpen("t2r-load-fill.txt", "r");
            L->barmode = bf == NULL;
            if (bf)
                fclose(bf);
            L->bar_w = -1.0f;
            if (!L->resume)
                TrOpen(60000);
            TrLogf("lds page up (match state %d, resume %d, bar %d)",
                    MatchSubstate(), L->resume, L->barmode);
            Xlog("load seq: entry page up (match state %d)%s%s",
                 MatchSubstate(), L->off ? " - sequence off" : "",
                 L->
                 barmode ?
                 " - bottom bar under the white title (default; t2r-load-fill.txt = title fill)"
                 : " - title fill");
        }
    }
    uint64_t t = now - L->t0;
    float bg, ta, ba;
    if (L->off) {
        bg = 1.0f;
        ta = 1.0f;
        ba = 1.0f;
        t = 1000000;
    } else {
        bg = LdsSmooth((float) t / LDS_BLACK_MS);
        ta = t <
            LDS_TITLE_AT ? 0.0f : LdsSmooth((float) (t - LDS_TITLE_AT) /
                                             LDS_TITLE_MS);
        ba = L->barmode ? (t < LDS_BAR_AT ? 0.0f : LdsSmooth((float) (t - LDS_BAR_AT) / LDS_BAR_MS)) : ta;     // title fill: the fill comes in with the title
    }
    // the end: world ready after the fade-in; hold LDS_HOLD_MS (title fully white), then fade out
    if (L->resume && !L->out) { // the hand-off: the native black released (ent_tick) = the world and the MP menu rise; the title crossfades out with them
        // 165o trace: the native black is released at the hand-off (ent 2) but winmm re-holds it (black 1.00) until the match state flips to 2, then it drops to 0 in one frame - the page
        // faded onto black and the menu snapped in. The page now stays up (bg 1, title, bar) until the native black READS 0, and only then fades (LDS_RESUME_OUT_MS) over the live menu.
        uint64_t a8[8] = { 0 };
        uint64_t bfr =
            DcCall(0xb55ca68cc38e541aULL /* get-black-fade */ , 0, a8);
        float bfv = 1.0f;
        uint32_t bu = (uint32_t) bfr;
        if (bfr != ~0ULL)
            memcpy(&bfv, &bu, 4);
        if (g_entSt == 2 && !L->rel_t0) {
            L->rel_t0 = now;
            if (!L->mute) {
                L->mute = 1;
                L->mute_t0 = now;
            }
            Xlog("load seq: resume - entry released the native black; session stays muted until the page fades");
        }
        // A client loaded into a match while the TWELVE WEEKS loading screen was stuck: the entry gave up ("first page not up
        // after 5000 ms - plain fade-in", g_ent_st back to 0) so it never reached state 2, the release above never came and the page stayed up for the 120 s cap - through
        // the vote, gear up and into the match. The entry's plain fade-in is a release too: the menu is live (state >= 2) and the native black reads 0.
        if (!L->rel_t0 && g_entSt == 0 && bfv < 0.02f
            && MatchSubstate() >= 2) {
            L->rel_t0 = now;
            Xlog("load seq: resume - the entry fell back to the plain fade-in (menu live, black 0): released");
        }
        int go = (L->rel_t0 && bfv < 0.02f && MatchSubstate() != 1)
            || (L->rel_t0 && now - L->rel_t0 > 4000)
            || now - L->rs_t0 > 120000;
        if (go && L->rel_t0 && LnHoldFade(now))
            go = 0;             // launchnote.c: the launch notice goes on under the title first (<= 4 s)
        if (go) {
            L->out = 1;
            L->out_t0 = now;
            Xlog("load seq: resume - native black reads %.2f (state %d, %llu ms after the release): the page fades out over the live menu (%d ms)", (double) bfv, (int) MatchSubstate(), (unsigned long long) (L->rel_t0 ? now - L->rel_t0 : 0), LDS_RESUME_OUT_MS);
        }
    } else if (!L->off && !L->out && t >= LDS_TITLE_AT + LDS_TITLE_MS
               && now >= L->leave_chk) {
        L->leave_chk = now + 250;
        if (LdsWorldReady()) {
            L->out = 1;
            L->out_t0 = now + LDS_HOLD_MS;
            Xlog("load seq: world ready after %llu ms - %d ms hold, then title and fill fade out", (unsigned long long) t, LDS_HOLD_MS);
        }
    }
    if (L->out && now >= L->out_t0) {
        int om = L->resume ? LDS_RESUME_OUT_MS : LDS_OUT_MS;
        float o =
            1.0f - LdsSmooth((float) (now - L->out_t0) / (float) om);
        ta *= o;
        ba *= o;
        if (L->resume) {
            bg *= o;
            if (now - L->out_t0 >= (uint64_t) om + 50 && !L->popped) {
                uint64_t a[2] = { TITLE_STACK, 0 };
                L->popped = 1;
                g_ldsHold = 0;
                DcCall(0x60e80fc914dc95ceULL /* menu2-pop-page */ , 2, a);
                Xlog("load seq: resume - faded out, page popped");
            }
        }
    }
    if (ta > 0.0f || L->resume) {       // the fill: ours, monotonic, never down
        if (!g_ldsFt0 && (L->barmode ? ba > 0.02f : ta > 0.0f))
            g_ldsFt0 = now;    // (165o clip: the clock ran from the title, so the bar faded in already at 21 %: it starts with the bar now)
        float tgt = g_ldsFill;
        if (g_ldsFt0) {
            float e =
                1.0f - expf(-(float) (now - g_ldsFt0) / LDS_FILL_TAU_MS);
            if (e * LDS_FILL_CAP > tgt)
                tgt = e * LDS_FILL_CAP;
        }
        if (L->out)
            tgt = 1.0f;         // world ready / hand-off: the last stretch, eased below
        float dt = L->last_t
            && now >
            L->last_t ? (float) (now - L->last_t) / 1000.0f : 0.0f;
        if (dt > 0.1f)
            dt = 0.1f;
        L->last_t = now;
        float v = g_ldsFill, step = dt * (L->out ? 0.6f : 0.12f);      // never faster than 12 % / s (60 % / s for the last stretch): no cut, never down
        if (tgt > v)
            v += tgt - v < step ? tgt - v : step;
        if (v > 1.0f)
            v = 1.0f;
        g_ldsFill = v;
        if (L->barmode) {
            LdsFillWrite(key, 1.0f);  // the mask stays open (winmm may still write it: trace shows)
            if (L->bar_w < 0.0f || fabsf(v - L->bar_w) > 0.002f) {
                L->bar_w = v;
                WScale(L->prog,
                        (v < 0.002f ? 0.002f : v) * 3840.0f / LDS_RW,
                        (float) LDS_BARH / LDS_PH);
            }
        } else
            LdsFillWrite(key, v);
    }
    if (!L->off && L->stage == 0 && t >= LDS_BLACK_MS) {
        L->stage = 1;
        Xlog("load seq: black");
    }
    if (ta > 0.0f) {
        if (L->stage < 2) {
            L->stage = 2;
            WVisible(L->title, 1);
            if (L->barmode) {   // fallback: white title, the progress group becomes the 12 px bar along the bottom edge (the mask clips the bar image)
                WVisible(L->full, 1);
                WVisible(L->bright, 0);
                WVisible(L->feather, 0);
                WVisible(L->barimg, 1);
                WPivot(L->prog, 0.0f, 2160.0f - LDS_BARH);
                WScale(L->prog, 3840.0f / LDS_RW,
                        (float) LDS_BARH / LDS_PH);
                WVisible(L->title, 1);
            }
            WVisible(L->prog, 1);
            Xlog("load seq: title fade-in (%d ms)%s", LDS_TITLE_MS,
                 L->
                 barmode ? ", fallback bar" :
                 ", fill follows the load progress");
        }
    }
    if (!L->snd && (t >= LDS_TITLE_AT || L->off)) {
        L->snd = 1;
        int k = L->off ? -1 : LdsPick();
        if (k >= 0) {
            float g = 0.9f * ExmusUserVolume();
            int ok = g > 0.0f
                && PcmPlayRate(lds_blob + LDS_OFF[k], LDS_LEN[k], g,
                                 LDS_RATE[k]);
            Xlog("load seq: stinger %s (%.1f s, gain %.2f) %s",
                 LDS_NAME[k], (double) LDS_LEN[k] / LDS_RATE[k],
                 (double) g,
                 ok ? "playing" :
                 "NOT played (mixer off / music volume 0)");
        } else if (k == -1 && !L->off) {
            static int n;
            if (LOGCAP(n, 1))
                Xlog("load seq: no stinger (t2r-no-load-stinger.txt)");
        }
    }
    if (L->mute) {              // the menu music (whole game mix, only the music is audible under the duck) comes in from silence after the page starts to fade
        if (L->out) {
            float u =
                LdsSmooth((float) (now - L->out_t0) /
                           (float) LDS_MUSIC_RAMP_MS);
            SessionVolume(u);
            if (u >= 1.0f) {
                L->mute = 0;
                SessionVolume(1.0f);
                Xlog("load seq: resume - music session volume back to 1");
            }
        } else
            SessionVolume(1.0f - LdsSmooth((float) (now - L->mute_t0) / (float) LDS_MUTE_MS));        // fade out, never cut
    }
    // The mute is gradual, so the Hillcrest audio fades out instead of cutting out: from the first loading page on, the game's own
    // faded under-black duck (everything but music; the stinger is waveOut, not in the engine mix) takes the title world's ambience down smoothly
    if (L->on && !L->resume && !L->off)
        MixUnderBlack();
    if (L->resume && !L->popped) {
        WVisible(key, 1);
        WAlpha(key, 1.0f);
    }                           // the page's own open animation may not run during the menu world load: root forced visible
    LdsPut(L->bg, &L->a_bg, bg);
    LdsPut(L->title, &L->a_t, ta);
    LdsPut(L->prog, &L->a_p, ba);
}

// ---- 'trace:' per-frame change logger. Windows: from the entry page up (60 s) / the world hand-off (+10 s) / the event page up (+10 s)
// / the clan event closing (+5 s); ~2000 lines (t2r-trace.txt first char 0/n/o = off, default on). One line whenever any field below changes (alphas to 0.02).
static void TrTick(void)
{
    if (!TrActive())
        return;
    LdsT *L = &g_lds;
    typedef uint64_t(*fn_top) (uint64_t);
    uint64_t tt = ((fn_top) T2R_SYM(kFnStackTop)) (TITLE_STACK), mt =
        TopPage();
    uint64_t a8[8] = { 0 };
    uint64_t bf =
        DcCall(0xb55ca68cc38e541aULL /* get-black-fade */ , 0, a8);
    float bfv = 0.0f;
    uint32_t bu = (uint32_t) bf;
    memcpy(&bfv, &bu, 4);
    char b[420];
    snprintf(b, sizeof b,
             "title %llx menu %llx black %.2f state %d ent %d rel %d | lds on %d res %d out %d stage %d hold %d bar %d a(bg %.2f t %.2f p %.2f) fill %.3f wrote-pct %.3f bar-x %.3f | ev pend %d shown %d mode %d | stack-snd %d",
             (unsigned long long) tt, (unsigned long long) mt, bfv,
             (int) MatchSubstate(), g_entSt, g_entRel, L->on, L->resume,
             L->out, L->stage, g_ldsHold, L->barmode, L->a_bg, L->a_t,
             L->a_p, g_ldsFill, L->fill_w, L->bar_w, g_ev.pending,
             g_ev.shown, g_ev.mode, (int) (g_stackMuteUntil != 0));
    for (char *p = b; *p; p++); // (rounding: alphas are logged with 2 decimals, so a fade is one line per 0.01 step at most)
    static char last[420];
    if (strcmp(b, last)) {
        strcpy(last, b);
        TrLogf("%s", b);
    }
}
