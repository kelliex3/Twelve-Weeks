// titlebg.c - the title (boat) screen's background is Ellie's Hillcrest overlook (tools/features/menu_title.py swaps the level lists
// of menu-main-pre/post-completion in level-sets-menu.bin; re/menu-title-hillcrest.md). Included from t2r-ext.c after loadseq.c; titlebg_tick() runs
// right after feature_menu_cam().
//
// The title world (levels menu + menu-beach-script stay, the beach levels are gone) still runs the stock title script ss-main-menu-boat with its camera
// object cam-main-menu-boat-1 (at the origin, 476 m from the overlook), so the render camera is ours, through the same hook as the MP menu camera
// (mcam_filter via g_mcam_watch / g_mcam_on / g_mcam_pos / g_mcam_q): active only while the TITLE stack (0x1f5962a2ce9803c8, see the test autofind code)
// has a page up and the match page is still on substate 0 (the menu world of the MP flow is substate 1 / 2 = feature_menu_cam's).
//   pose        default TITLE_POSE (a freecam shot, fov 46.94 through override-fov every frame);
//               t2r-title-cam.txt next to the exe: "x y z yaw pitch [fov]" (yaw deg 0 = +Z atan2(fx,fz), pitch deg DOWN), re-read every 0.5 s
//   sway        a slow, subtle drift: yaw +-TB_YAW_DEG over TB_YAW_S, pitch +-TB_PIT_DEG over TB_PIT_S, position +-TB_POS_M on three incommensurate periods
//   fade-in     the sway amplitude ramps in over 4 s so the first frame is exactly the pose
//   render set  *res-rs-005-towerhill-vista* (SP's own vista layer of the overlook) every frame through override-render-settings (imm_rs_call, priority
//               TB_RS_PRIO); t2r-title-rs.txt "<symbol> <priority> <strength>" per line (up to 4, '#' ignored, re-read every 2 s) replaces it
//   t2r-menu-boat.txt   next to the exe: the ext does nothing here (the game's own title camera, no render layer). The level lists are a build property
//               (T2R_MENU_BOAT=1 builds the stock boat); an SP-only launch parks the whole mod (launcher SP switch), so it never sees any of this.
// Log lines ("title bg:"): active / inactive with the game's own camera, pose source, render-layer result, hook counters (first minutes only).
// The shot: x y z, yaw deg (0 = +Z, atan2(fx, fz)), pitch deg DOWN-positive (-3.23 = 3.2 deg UP), the last F9, fov deg.
static const float TITLE_POSE[6] =
    { 14.855f, 2.957f, 530.968f, 150.19f, -3.23f, 46.94f };
#define TB_YAW_DEG 0.15f
#define TB_YAW_S   47.0f
#define TB_PIT_DEG 0.06f
#define TB_PIT_S   31.0f
#define TB_POS_M   0.04f
#define TB_RS_PRIO 100
static int TbTitleUp(void)
{                               // a page is up on the title stack, the MP flow has not started its own menu world
    if (MatchSubstate() != 0)
        return 0;
    typedef uint64_t(*fn_top) (uint64_t);
    return ((fn_top) T2R_SYM(kFnStackTop)) (0x1f5962a2ce9803c8ULL) != 0;
}

// Boot cover: the stock title camera (cam-main-menu-boat-1 at the origin = the boat) renders
// until the hook has hits, and the world is white until the vista render settings + the streamed levels are in. From the first ticks the screen is held black
// (fade_to_black re-armed every 0.5 s, as ms_fade_tick) and our camera + render layer already run (no title-stack / frame>60 wait); the fade up starts when
//  (a) the camera hook has hit TB_BOOT_HITS frames, (b) the render settings call returned a handle TB_BOOT_RS times, (c) task-want-load-levels-fully-spawned? answers yes,
// then TB_BOOT_SETTLE_MS more (LOD / texture pop-in). Fallback TB_BOOT_MAX_MS. Logs 'title bg: boot ...'.
#define TB_BOOT_HITS 6
#define TB_BOOT_RS 3
#define TB_BOOT_SETTLE_MS 2500  // 165k: task-want-load-levels-fully-spawned? answers 0 for the whole title (never usable here): a fixed settle after hits + render settings
#define TB_BOOT_MAX_MS 9000
#define TB_BOOT_FADE_S 1.2f
static int g_tbUiFound;
static void TbUiAlpha(float a)
{                               // the title page's root group on the title stack (same lookup as ui_w on the menu stack); 0 when the page has no 'anim-group'
    uint64_t w =
        ((fn_widget_t) T2R_SYM(kFnWidgetFind)) ("anim-group", TITLE_STACK);
    static int nl;
    if (!w) {
        if (nl < 2) {
            nl++;
            Xlog("title bg: title stack page has no 'anim-group' widget - buttons stay visible");
        }
        g_tbUiFound = 0;
        return;
    }
    g_tbUiFound = 1;
    WAlpha(w, a);
}

static void TitlebgTick(void)
{
    static int init, off, owned, was;
    static int boot_st, rs_ok, boot_post;       // boot_st: 0 not started, 1 black hold, 2 released
    static uint64_t boot_t0, boot_rearm, ready_at, ui_fade_t0, title_t0;
    static uint32_t boot_hits0;
    static uint64_t t0, file_at, rs_at, log_at, rs_file_at;
    static float fp[6];
    static int file_ok;
    static struct {
        uint64_t sid;
        int prio;
        float str;
        char name[96];
    } L[4];
    static int nL;
    static char dir[MAX_PATH];
    uint64_t now = GetTickCount64();
    if (!init) {
        init = 1;
        GetModuleFileNameA(NULL, dir, MAX_PATH);
        char *s = strrchr(dir, '\\');
        if (!s) {
            off = 1;
            return;
        }
        s[1] = 0;
        char p[MAX_PATH + 32];
        snprintf(p, sizeof p, "%st2r-menu-boat.txt", dir);
        off = GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
        if (off)
            Xlog("title bg: t2r-menu-boat.txt - the game's own title camera and render settings");
    }
    if (off)
        return;
    int base_ok = g_icam_tramp && !g_icam_on && !g_xcamOn && !g_fcam_on
        && g_frame > 5;
    // Starting the hold at the title page left the frames before it
    // (white world + stock boat camera) on screen. The black starts at the first ticks; only the ready check / timeout wait for the page.
    if (boot_st == 0 && base_ok && MatchSubstate() == 0) {
        boot_st = 1;
        boot_t0 = now;
        boot_rearm = 0;
        boot_hits0 = g_mcam_hits;
        rs_ok = 0;
        ready_at = 0;
        FadeToBlack(0.0f, 1.0f, 0.0f);
        Xlog("title bg: boot black hold from frame %llu (hook hits %u) - fade up when camera hits >= %d, render settings x%d, levels spawned", (unsigned long long) g_frame, (unsigned) g_mcam_hits, TB_BOOT_HITS, TB_BOOT_RS);
    }
    int up = base_ok && (boot_st == 1 || (g_frame > 60 && TbTitleUp()));
    if (boot_st == 1 && (!base_ok || MatchSubstate() != 0)) {
        boot_st = 2;
        FadeToBlack(0.0f, 0.0f, 0.3f);
        Xlog("title bg: boot black hold ended early (match substate %d)",
             (int) MatchSubstate());
        TbUiAlpha(1.0f);
    }
    if (!up) {
        if (owned) {
            owned = 0;
            g_mcamOn = 0;
            if (!g_xcamOn && !g_fcam_on)
                g_mcamWatch = 0;
            Xlog("title bg: off (match substate %d)",
                 (int) MatchSubstate());
        }
        was = 0;
        return;
    }
    if (!was) {
        was = 1;
        t0 = now;
        file_at = rs_at = log_at = rs_file_at = 0;
        Xlog("title bg: on (title stack up, substate 0) - Hillcrest overlook camera, hook %s", g_icam_tramp ? "installed" : "MISSING");
    }
    owned = 1;
    g_mcamMgr = IcamManager();
    g_mcamWatch = 1;
    if (now >= file_at) {       // t2r-title-cam.txt
        file_at = now + 500;
        char p[MAX_PATH + 32];
        float v[6] = { 0, 0, 0, 0, 0, 0 };
        snprintf(p, sizeof p, "%st2r-title-cam.txt", dir);
        FILE *f = fopen(p, "r");
        int n =
            f ? fscanf(f, "%f %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3],
                       &v[4], &v[5]) : 0;
        if (f)
            fclose(f);
        if (n >= 5 && (!file_ok || memcmp(v, fp, sizeof v))) {
            memcpy(fp, v, sizeof v);
            Xlog("title bg: t2r-title-cam.txt -> %.2f %.2f %.2f yaw %.1f pitch %.1f fov %.1f", v[0], v[1], v[2], v[3], v[4], v[5]);
        }
        file_ok = n >= 5;
    }
    const float *b = file_ok ? fp : TITLE_POSE;
    float fov = b[5] > 0.0f ? b[5] : TITLE_POSE[5];
    float t = (float) (now - t0) / 1000.0f, k = t < 4.0f ? t / 4.0f : 1.0f;
    k = k * k * (3.0f - 2.0f * k);
    const float TAU = 6.28318530f;
    float yaw = b[3] + k * TB_YAW_DEG * sinf(TAU * t / TB_YAW_S);
    float pit = b[4] + k * TB_PIT_DEG * sinf(TAU * t / TB_PIT_S + 1.3f);
    float pos[3] =
        { b[0] + k * TB_POS_M * sinf(TAU * t / 61.0f),
 b[1] + k * TB_POS_M * 0.5f * sinf(TAU * t / 37.0f + 0.7f),
        b[2] + k * TB_POS_M * sinf(TAU * t / 53.0f + 2.1f)
    };
    float q[4];
    McamQuat(yaw, pit, q);
    for (int i = 0; i < 3; i++)
        g_mcamPos[i] = pos[i];
    for (int i = 0; i < 4; i++)
        g_mcam_q[i] = q[i];
    g_mcamOn = 1;
    {
        uint32_t fb;
        memcpy(&fb, &fov, 4);
        uint64_t a = fb;
        uint64_t r = DcCall(FNV64("override-fov"), 1, &a);     // per frame, as the freecam / execution cameras
        static int nf;
        if (nf < 3) {
            nf++;
            Xlog("title bg: override-fov %.2f -> %llx", (double) fov,
                 (unsigned long long) r);
        }
    }
    // render settings layer(s)
    if (now >= rs_file_at) {
        rs_file_at = now + 2000;
        char p[MAX_PATH + 32];
        snprintf(p, sizeof p, "%st2r-title-rs.txt", dir);
        FILE *f = fopen(p, "r");
        int n = 0;
        if (f) {
            char ln[160];
            while (n < 4 && fgets(ln, sizeof ln, f)) {
                char nm[96];
                int pr;
                float st;
                if (ln[0] == '#'
                    || sscanf(ln, "%95s %d %f", nm, &pr, &st) != 3)
                    continue;
                if (strcmp(L[n].name, nm)) {
                    snprintf(L[n].name, sizeof L[n].name, "%s", nm);
                    L[n].sid = FNV64(nm);
                }
                L[n].prio = pr;
                L[n].str = st;
                n++;
            }
            fclose(f);
        }
        if (!n) {
            snprintf(L[0].name, sizeof L[0].name,
                     "*res-rs-005-towerhill-vista*");
            L[0].sid = FNV64(L[0].name);
            L[0].prio = TB_RS_PRIO;
            L[0].str = 1.0f;
            n = 1;
        }
        nL = n;
    }
    for (int i = 0; i < nL; i++) {
        uint64_t r = ImmRsCall(L[i].sid, L[i].prio, L[i].str);
        if (i == 0 && r && r != ~0ULL && rs_ok < 1000)
            rs_ok++;
        static int nl_;
        if (nl_ < 6) {
            nl_++;
            Xlog("title bg: override-render-settings %s prio %d strength %.2f -> %llx", L[i].name, L[i].prio, (double) L[i].str, (unsigned long long) r);
        }
    }
    if (boot_st == 1) {
        uint32_t hits = g_mcam_hits - boot_hits0;
        uint64_t wa[8] = { 0 };
        uint64_t wr =
            DcCall(0x66637535ebbc34dfULL
                    /* task-want-load-levels-fully-spawned? */ , 0, wa);
        int spawned = wr != ~0ULL && wr != 0;
        int tu = TbTitleUp();
        if (tu && !title_t0) {
            title_t0 = now;
            Xlog("title bg: boot - title page up after %llu ms of black",
                 (unsigned long long) (now - boot_t0));
        }
        int cond = tu && hits >= TB_BOOT_HITS && rs_ok >= TB_BOOT_RS;   // (spawned only logged: 0 all through the title)
        if (cond && !ready_at) {
            ready_at = now;
            Xlog("title bg: boot ready after %llu ms - camera hits %u, render settings x%d, levels spawned; settle %d ms", (unsigned long long) (now - boot_t0), (unsigned) hits, rs_ok, TB_BOOT_SETTLE_MS);
        }
        if (!cond)
            ready_at = 0;
        if (tu)
            TbUiAlpha(0.0f);  // the title buttons sit above the native black: hold them invisible too
        int timeout = (title_t0 && now - title_t0 >= TB_BOOT_MAX_MS)
            || now - boot_t0 >= 60000;
        if ((ready_at && now - ready_at >= TB_BOOT_SETTLE_MS) || timeout) {
            boot_st = 2;
            boot_post = 2;
            FadeToBlack(0.0f, 0.0f, TB_BOOT_FADE_S);
            ui_fade_t0 = now;
            Xlog("title bg: boot FADE UP%s at frame %llu after %llu ms: hook hits %u done %u (game camera %.1f %.1f %.1f, ours %.2f %.2f %.2f), render settings x%d, levels spawned %d", timeout && !ready_at ? " (TIMEOUT)" : "", (unsigned long long) g_frame, (unsigned long long) (now - boot_t0), (unsigned) hits, (unsigned) g_mcam_done, (double) g_mcamSeen[0], (double) g_mcamSeen[1], (double) g_mcamSeen[2], (double) pos[0], (double) pos[1], (double) pos[2], rs_ok, spawned);
        } else if (now - boot_rearm > 500) {
            boot_rearm = now;
            FadeToBlack(0.0f, 1.0f, 0.0f);
            static int nw;
            if (nw < 40 && (nw++ % 4) == 0)
                Xlog("title bg: boot waiting %llu ms: hits %u, render settings x%d, levels spawned %d", (unsigned long long) (now - boot_t0), (unsigned) hits, rs_ok, spawned);
        }
    } else if (ui_fade_t0) {    // buttons come up with the world, same ease as the native fade (1.2 s)
        float u = (float) (now - ui_fade_t0) / (TB_BOOT_FADE_S * 1000.0f);
        if (u >= 1.0f) {
            TbUiAlpha(1.0f);
            ui_fade_t0 = 0;
            Xlog("title bg: boot buttons fully up (title stack widget %s)",
                 g_tbUiFound ? "found" :
                 "NOT FOUND - they were never hidden");
        } else
            TbUiAlpha(u * u * (3.0f - 2.0f * u));
    }
    if (boot_st != 1 && boot_post > 0) {        // the first frames after the fade start
        boot_post--;
        Xlog("title bg: boot frame +%d after fade start: frame %llu, hook hits %u done %u, ours %.2f %.2f %.2f, render settings x%d", 2 - boot_post, (unsigned long long) g_frame, (unsigned) g_mcam_hits, (unsigned) g_mcam_done, (double) pos[0], (double) pos[1], (double) pos[2], rs_ok);
    }
    if (now >= log_at && now - t0 < 120000) {
        log_at = now + 5000;
        Xlog("title bg: t=%.0f s cam %.2f %.2f %.2f yaw %.2f pitch %.2f | hook hits %u done %u | game camera %.1f %.1f %.1f", (double) t, (double) pos[0], (double) pos[1], (double) pos[2], (double) yaw, (double) pit, (unsigned) g_mcam_hits, (unsigned) g_mcam_done, (double) g_mcamSeen[0], (double) g_mcamSeen[1], (double) g_mcamSeen[2]);
    }
}
