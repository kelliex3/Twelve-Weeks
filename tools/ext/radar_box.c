// ---------------------------------------------------------------- supply boxes on the radar as the lootbox icon
// The icon replaces the dots on the radar. The icon is TLOU2's own
// No Return crate marker: hud/interactions/marker-lootbox.png (hud-buddy-status/rogue-attachable icon0, re/nr-supply-box-trace-1102.md,
// extracted to re/radar/marker-lootbox-1080.png; white chest glyph, 162 x 132).
// winmm's radar (FUN_180060e60) draws a box as a text glyph with 0x1800039d0 at the call 0x1800611cf (r9 = 0xffdcdcdc, one call per box
// slot whose time has come = not opened / refilling; the item dots are the other call 0x180061269, left as they are). That call is
// pointed at radar_box_blip: it only RECORDS the blip's centre (1280 x 720 text space, before g_text_xoff) and draws nothing while our
// widgets exist. radar_box_frame (HUD draw hook, every frame) puts the pooled hud-root images t2rRbx0..11 (radar_box_widgets.py) on those
// centres exactly like the teammate health bars (w_pivot: x from the screen centre * 3, y from the top * 3), alpha = the radar's own fade.
// Old gui2-widgets.pak without them: the call forwards to winmm's text draw, the dot stays. t2r-old-radar-box.txt next to the exe = dots.
// Log 'radar box:'.
#define RBX_N 12
#define RBX_W 72.0f             // = radar_box_widgets.W (canvas units: 24 px of the 1280 x 720 box; the texture has padding round the glyph), height W * 132 / 162
#define RBX_H 59.0f
static uint64_t g_rbx_h[RBX_N], g_rbx_page, g_rbx_first_try;    // g_rbx_first_try: the frame the widget lookup started in this match (race fix)
static int g_rbxPg;            // 1: the icons are currency-hud page widgets (top-left anchored, the page carries the radar fade)
static int g_rbx_ok, g_rbx_miss, g_rbx_vis[RBX_N], g_rbx_old;
static uint64_t g_rbxChk;
static volatile LONG g_rbxN;   // blips recorded since the last radar_box_frame
static float g_rbxX[RBX_N], g_rbx_y[RBX_N];
// A box on another floor = a greyed-out radar icon with a small white arrow: per blip three image widgets, the plain crate t2rRbx<k>, the grey crate + arrow up t2rRbxU<k>, the
// grey crate + arrow down t2rRbxD<k> (radar_box_widgets.add_radar_box_floor, textures radar-box-up / radar-box-down = make_icons.radar_box_floor); the one for the box's state (nrbox.c
// g_nrb[].flr, nrbox_floor_update) is shown, the other two hidden. Old pak without U/D: plain crates as before. winmm draws one blip per box record in table order, closed ones only
// (opened flag +0x14 = 0), so blip k = the k-th such record; a count that does not match = every icon plain (logged).
static uint64_t g_rbxHu[RBX_N], g_rbx_hd[RBX_N];
static int g_rbx_ud, g_rbx_st[RBX_N], g_rbx_vs[RBX_N];  // g_rbx_vs: which of the three is visible (-1 none, 0 plain, 1 up, 2 down)
// A box outside the radar's range = an arched line on the rim depending on its direction, no crate icon: winmm pins an out-of-range blip on a circle round the radar centre
// (the -64 units of radar_edge_inside, t2r-ext.c), so a blip at >= RBX_PIN_D from the centre is such a box. Its crate is hidden and the rim arc of its bearing is shown instead: 60 copies
// of one arc texture turned 6 degrees apart (t2rRbxA<k> white = same floor, t2rRbxG<k> grey = another floor; radar_box_widgets.add_radar_box_arcs, make_icons.radar_box_arc), centred
// on the disc, so ext only toggles visibility. Boxes on one step share its arc (the arcs are 24 degrees long, neighbouring steps overlap into one line). Old pak: pinned crates as before.
// TUNINGS (not 2013 data): RBX_PIN_D, the arc length / thickness / softness (make_icons.radar_box_arc), the 6 degree step.
#define RBX_ARC_N 60
#define RBX_PIN_D 62.5f         // TUNING: blip distance (units) from the radar centre from which a blip counts as pinned = out of range (pins sit at 64, in-range blips end below ~60)
#define RBX_ARC_CW 1            // layout rotation turns clockwise on screen (widget_preview's reading; 0 = mirrored if the A/B check shows the arcs on the wrong side)
static uint64_t g_rbxAh[2][RBX_ARC_N];
static int g_rbx_arc, g_rbx_av[2][RBX_ARC_N];   // [0] white (same floor), [1] grey (another floor)
static void RadarBoxStates(int n, int *st)
{
    int idx[RBX_N], m = 0;
    for (int i = 0; i < n; i++)
        st[i] = 0;
    uint8_t *beg = *(uint8_t **) T2R_SYM(kPeerListBegin), *end =
        *(uint8_t **) T2R_SYM(kPeerListEnd);
    if (!g_rbx_ud || !beg || end <= beg || (end - beg) / 0x48 > 64
        || IsBadReadPtr(beg, (size_t) (end - beg)))
        return;
    for (uint8_t * e = beg; e < end && m < RBX_N; e += 0x48) {
        if (*(uint64_t *) (e + 0x18) == *(uint64_t *) (e + 0x20)
            || e[0x14])
            continue;
        int j = -1;
        for (int k = 0; k < NRB_MAX; k++)
            if (g_nrb[k].hash == *(uint64_t *) e) {
                j = k;
                break;
            }
        idx[m++] = j;
    }
    static int lastbad = -1;
    static int nl;
    if (m != n) {
        if (lastbad != n * 100 + m) {
            lastbad = n * 100 + m;
            if (LOGCAP(nl, 20))
                Xlog("box floor: %d radar blips but %d closed box records - radar icons stay plain", n, m);
        }
        return;
    }
    lastbad = -1;
    for (int i = 0; i < n; i++)
        st[i] = idx[i] >= 0 ? g_nrb[idx[i]].flr : 0;
}

static void RadarBoxBlip(uint64_t x, uint64_t y, uint64_t size,
                           uint64_t col, const char *fmt,
                           const char *glyph)
{
    LONG n = g_rbxN;
    if (g_rbx_ok && n < RBX_N && glyph) {
        float fx, fy, fs;
        uint32_t a = (uint32_t) x, b = (uint32_t) y, c = (uint32_t) size;
        memcpy(&fx, &a, 4);
        memcpy(&fy, &b, 4);
        memcpy(&fs, &c, 4);
        uint64_t wh = ((fn_measure) T2R_SYM(kFnMeasureText)) (glyph, fs);
        float w, h;
        uint32_t lo = (uint32_t) wh, hi = (uint32_t) (wh >> 32);
        memcpy(&w, &lo, 4);
        memcpy(&h, &hi, 4);
        if (w > 0.0f && w < 200.0f && h > 0.0f && h < 200.0f
            && fx > -2000.0f && fx < 4000.0f) {
            g_rbxX[n] = fx + w * 0.5f;
            g_rbx_y[n] = fy + h * 0.5f;
            g_rbxN = n + 1;
            return;
        }
    }
    ((fn_text_va) T2R_SYM(kFnDrawTextVa)) (x, y, size, col, fmt, glyph);
}

static void RadarBoxInstall(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    uint8_t *call = (uint8_t *) T2R_CODE(kCodeRadarCall);
    if (call[0] != 0xE8
        || call + 5 + *(int32_t *) (call + 1) !=
        (uint8_t *) T2R_SYM(kFnDrawTextVa)) {
        HOOK_FAIL("radar box: call site differs, not installed");
        return;
    }
    uint8_t *stub = AllocNear((uint8_t *) g_winmm, 0x1000);
    if (!stub)
        return;
    stub[0] = 0x48;
    stub[1] = 0xB8;
    *(uint64_t *) (stub + 2) = (uint64_t) RadarBoxBlip;       // mov rax, imm64
    stub[10] = 0xFF;
    stub[11] = 0xE0;            // jmp rax
    DWORD old;
    VirtualProtect(stub, 0x1000, PAGE_EXECUTE_READ, &old);
    VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old);
    *(int32_t *) (call + 1) = (int32_t) (stub - (call + 5));
    VirtualProtect(call, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    Xlog("radar box: supply box blips go through radar_box_blip (lootbox icon widgets when the pak has them)");
}

// The chest icons were grey and the pinned ones stuck out of the ring. winmm's own dark radar disc (the glyph drawn first in FUN_180060e60,
// colour 0xA0000000, two draw paths at 0x180061097 / 0x1800610a4) is a winmm TEXT draw = after every widget page, so it always sat over the icons. While the widgets
// exist its alpha byte is zeroed and the disc is part of the radarVig texture (make_icons.radar_vignette: alpha 160 flat + the rim vignette) = a widget of the page
// BEFORE the icons. Back to the glyph disc with the old pak / t2r-old-radar-box.txt.
static void RadarBoxDisc(int on)
{
    static int cur = -1;
    if (cur == on)
        return;
    uint8_t *a = (uint8_t *) T2R_CODE(kCodeRadarA), *b =
        (uint8_t *) T2R_CODE(kCodeRadarB);
    if (a[-2] != 0x41 || a[-1] != 0xB9
        || *(uint32_t *) a != (on ? 0xA0000000u : 0x00000000u)
        || b[-4] != 0xC7 || b[-3] != 0x44 || b[-2] != 0x24 || b[-1] != 0x20
        || *(uint32_t *) b != (on ? 0xA0000000u : 0x00000000u)) {
        HOOK_FAIL("radar box: disc sites differ, glyph disc stays");
        cur = on;
        return;
    }
    DWORD old;
    VirtualProtect(a, 4, PAGE_EXECUTE_READWRITE, &old);
    *(uint32_t *) a = on ? 0u : 0xA0000000u;
    VirtualProtect(a, 4, old, &old);
    VirtualProtect(b, 4, PAGE_EXECUTE_READWRITE, &old);
    *(uint32_t *) b = on ? 0u : 0xA0000000u;
    VirtualProtect(b, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), a, 4);
    FlushInstructionCache(GetCurrentProcess(), b, 4);
    cur = on;
    Xlog("radar box: winmm glyph disc %s (the disc is the radarVig widget)", on ? "off" : "on");
}

static void RadarBoxHideAll(void)
{
    for (int i = 0; i < RBX_N; i++) {
        if (g_rbx_vis[i]) {
            WVisible(g_rbx_h[i], 0);
            g_rbx_vis[i] = 0;
        }
        if (g_rbx_vs[i] > 0) {
            WVisible(g_rbx_vs[i] == 1 ? g_rbxHu[i] : g_rbx_hd[i], 0);
        }
        g_rbx_vs[i] = -1;
    }
    for (int c = 0; c < 2; c++)
        for (int k = 0; k < RBX_ARC_N; k++)
            if (g_rbx_av[c][k]) {
                WVisible(g_rbxAh[c][k], 0);
                g_rbx_av[c][k] = 0;
            }
}

static void RadarBoxFrame(void)
{
    RadarBoxInstall();
    uint64_t page = *(uint64_t *) T2R_SYM(kUiPage);
    if (page != g_rbx_page) {
        g_rbx_page = page;
        g_rbx_miss = 0;
        g_rbx_ok = 0;
        memset(g_rbx_vis, 0, sizeof g_rbx_vis);
        memset(g_rbx_h, 0, sizeof g_rbx_h);
        memset(g_rbxHu, 0, sizeof g_rbxHu);
        memset(g_rbx_hd, 0, sizeof g_rbx_hd);
        g_rbx_ud = 0;
        g_rbx_arc = 0;
        memset(g_rbxAh, 0, sizeof g_rbxAh);
        memset(g_rbx_av, 0, sizeof g_rbx_av);
        for (int i = 0; i < RBX_N; i++)
            g_rbx_vs[i] = -1;
    }
    int old = OvOff("t2r-old-radar-box.txt", &g_rbx_old, &g_rbxChk);
    if (old || MatchSubstate() != 6) {
        g_rbx_first_try = 0;
        if (g_rbx_ok) {
            g_rbx_ok = 0;
            RadarBoxHideAll();
            g_rbxN = 0;
        }
        RadarBoxDisc(0);
        return;
    }
    // Radar bug (no arcs, and many supply boxes showing depending on rotation): mid match the
    // first check ran before the currency-hud page had its widgets, pass 0 failed, and the hud-root fallback (the OLD pak path) won for the rest of the match:
    // no arcs (page only), no clamp to the ring, the pinned out-of-range crates on the rim moving with the rotation, dim under the disc. The hud-root fallback is
    // taken only after the page has missed them for 5 s, and a hud-root state keeps looking for the page widgets once a second (then it switches over).
    static uint64_t first_page;
    if (first_page != g_rbx_page || !g_rbx_first_try) {
        first_page = g_rbx_page;
        g_rbx_first_try = g_frame ? g_frame : 1;
    }
    if (g_rbx_ok && !g_rbxPg && g_frame % 60 == 0
        && HudWidget("t2rRbx0", NULL, NULL)) {
        RadarBoxHideAll();
        Xlog("radar box: the currency-hud page widgets appeared - leaving the hud-root fallback");
        g_rbx_ok = 0;
        g_rbx_miss = 0;
        memset(g_rbx_vis, 0, sizeof g_rbx_vis);
        memset(g_rbx_h, 0, sizeof g_rbx_h);
        g_rbxN = 0;
    }
    if (!g_rbx_ok && !g_rbx_miss) {
        int all = 1;
        int may_root = g_frame - g_rbx_first_try >= 300
            || GetFileAttributesA("t2r-radar-box-race-old.txt") !=
            INVALID_FILE_ATTRIBUTES;
        // The chest icons in hud-root were dimmed to ~55 % grey: the radar's dark disc (radarVig) is a widget of the currency-hud PAGE, which is drawn
        // OVER hud-root, so icons in hud-root sat under the disc. The icons are now widgets of that same page, added after the disc (radar_box_widgets.py); hud-root ones
        // (an old pak) are the fallback and stay dim.
        for (int pass = 0; pass < (may_root ? 2 : 1) && !g_rbx_ok; pass++) {
            all = 1;
            for (int i = 0; i < RBX_N && all; i++) {
                char n[16];
                snprintf(n, sizeof n, "t2rRbx%d", i);
                g_rbx_h[i] =
                    pass == 0 ? HudWidget(n, NULL, NULL) : Hudw(n);
                if (!g_rbx_h[i])
                    all = 0;
            }
            if (all) {
                g_rbx_ok = 1;
                g_rbxPg = pass == 0;
                Xlog("radar box: widgets t2rRbx0-%d found in %s - supply boxes show as the lootbox icon on the radar", RBX_N - 1, pass == 0 ? "the currency-hud page (above the radar disc)" : "hud-root (OLD pak: under the disc, dim)");
            }
        }
        if (g_rbx_ok && g_rbxPg) {
            int u = 1;
            for (int i = 0; i < RBX_N && u; i++) {
                char a[16], b[16];
                snprintf(a, sizeof a, "t2rRbxU%d", i);
                snprintf(b, sizeof b, "t2rRbxD%d", i);
                g_rbxHu[i] = HudWidget(a, NULL, NULL);
                g_rbx_hd[i] = HudWidget(b, NULL, NULL);
                if (!g_rbxHu[i] || !g_rbx_hd[i])
                    u = 0;
            }
            g_rbx_ud = u;
            if (!u) {
                memset(g_rbxHu, 0, sizeof g_rbxHu);
                memset(g_rbx_hd, 0, sizeof g_rbx_hd);
            }
            Xlog("box floor: %s",
                 u ?
                 "radar widgets t2rRbxU / t2rRbxD found - boxes on another floor show as the grey crate with an arrow on the radar"
                 :
                 "t2rRbxU / t2rRbxD not in the pak (old gui2-widgets.pak) - radar icons stay plain");
        }
        if (g_rbx_ok && g_rbxPg) {
            int u = 1;
            for (int c = 0; c < 2 && u; c++)
                for (int k = 0; k < RBX_ARC_N && u; k++) {
                    char a[24];
                    snprintf(a, sizeof a, "t2rRbx%c%d", c ? 'G' : 'A', k);
                    g_rbxAh[c][k] = HudWidget(a, NULL, NULL);
                    if (!g_rbxAh[c][k])
                        u = 0;
                }
            g_rbx_arc = u;
            if (!u)
                memset(g_rbxAh, 0, sizeof g_rbxAh);
            Xlog("box arcs: %s",
                 u ?
                 "radar widgets t2rRbxA / t2rRbxG found - boxes outside the radar's range show as an arc on the rim"
                 :
                 "t2rRbxA / t2rRbxG not in the pak (old gui2-widgets.pak) - pinned crate icons stay");
        }
        if (!g_rbx_ok && !may_root)
            return;             // (the page may still be building: winmm's dots meanwhile)
        if (!g_rbx_ok) {
            g_rbx_miss = 1;
            Xlog("radar box: t2rRbx widgets not in the pak (old gui2-widgets.pak) - dots stay");
        }
    }
    if (!g_rbx_ok)
        return;
    RadarBoxDisc(g_rbxPg);   // only with the page widgets (the new radarVig texture carries the disc)
    NrboxFloorUpdate();
    int n = g_rbxN;
    g_rbxN = 0;
    int stt[RBX_N];
    RadarBoxStates(n, stt);
    float al = g_rbxPg ? (float) g_dui_text_alpha / 255.0f : (float) (g_radar_alpha < g_dui_text_alpha ? g_radar_alpha : g_dui_text_alpha) / 255.0f;   // (a page widget: the page alpha already follows the radar fade)   // full white (was x0.92 and a 0xDCDCDC tint = grey)
    if (g_text_hide || HUD_HIDDEN())
        al = 0.0f;
    static int nl, lastn = -1;
    if (n != lastn) {
        lastn = n;
        if (LOGCAP(nl, 30))
            Xlog("radar box: %d supply box icon(s) on the radar", n);
    }
    int want[2][RBX_ARC_N];
    memset(want, 0, sizeof want);
    static int lastarc = -1, na;
    int narc = 0;
    for (int i = 0; i < n && g_rbx_arc; i++) {  // out-of-range (pinned) blips: the arc of the bearing (0 = up, clockwise), not the crate
        float dx = g_rbxX[i] - 104.0f, dy = g_rbx_y[i] - 179.0f;
        if (sqrtf(dx * dx + dy * dy) < RBX_PIN_D)
            continue;
        float deg = atan2f(dx, -dy) * (180.0f / 3.14159265f);
        if (!RBX_ARC_CW)
            deg = -deg;
        int k = (int) floorf(deg * (RBX_ARC_N / 360.0f) + 0.5f);
        k = ((k % RBX_ARC_N) + RBX_ARC_N) % RBX_ARC_N;
        want[stt[i] != 0][k] = 1;
        narc++;
        g_rbxX[i] = -1e6f;     // marks the blip: no crate
    }
    if (narc != lastarc) {
        lastarc = narc;
        if (LOGCAP(na, 30))
            Xlog("box arcs: %d supply box(es) out of the radar's range shown as rim arcs", narc);
    }
    for (int c = 0; c < 2 && g_rbx_arc; c++)
        for (int k = 0; k < RBX_ARC_N; k++) {
            if (want[c][k] != g_rbx_av[c][k]) {
                WVisible(g_rbxAh[c][k], want[c][k]);
                g_rbx_av[c][k] = want[c][k];
            }
            if (want[c][k])
                WAlpha(g_rbxAh[c][k], al);
        }
    for (int i = 0; i < RBX_N; i++) {
        if (i >= n || g_rbxX[i] < -1e5f) {
            if (g_rbx_vis[i]) {
                WVisible(g_rbx_h[i], 0);
                g_rbx_vis[i] = 0;
            }
            if (g_rbx_vs[i] > 0) {
                WVisible(g_rbx_vs[i] == 1 ? g_rbxHu[i] : g_rbx_hd[i], 0);
            }
            g_rbx_vs[i] = -1;
            continue;
        }
        if (g_rbxPg) {         // keep the whole icon inside the ring (disc r 75 units, centre 104 / 179 = clan_widgets RADAR_CX/CY; icon half 12 x 10): centre at most 60 out
            float dx = g_rbxX[i] - 104.0f, dy = g_rbx_y[i] - 179.0f, d =
                sqrtf(dx * dx + dy * dy);
            if (d > 60.0f) {
                g_rbxX[i] = 104.0f + dx * 60.0f / d;
                g_rbx_y[i] = 179.0f + dy * 60.0f / d;
            }
        }
        float px =
            (g_rbxPg ? g_rbxX[i] * 3.0f : (g_rbxX[i] - 640.0f) * 3.0f) -
            RBX_W * 0.5f, py = g_rbx_y[i] * 3.0f - RBX_H * 0.5f;
        int s = g_rbx_ud ? stt[i] : 0;
        uint64_t wh =
            s == 1 ? g_rbxHu[i] : s == 2 ? g_rbx_hd[i] : g_rbx_h[i];
        WPivot(wh, px, py);
        WAlpha(wh, al);
        if (s != g_rbx_vs[i]) { // the one for this state on, the other two off
            if (g_rbx_vs[i] > 0)
                WVisible(g_rbx_vs[i] == 1 ? g_rbxHu[i] : g_rbx_hd[i], 0);
            if (s == 0 && !g_rbx_vis[i]) {
                WVisible(g_rbx_h[i], 1);
                g_rbx_vis[i] = 1;
            } else if (s != 0) {
                if (g_rbx_vis[i]) {
                    WVisible(g_rbx_h[i], 0);
                    g_rbx_vis[i] = 0;
                }
                WVisible(wh, 1);
            }
            g_rbx_vs[i] = s;
        } else if (s == 0 && !g_rbx_vis[i]) {
            WVisible(g_rbx_h[i], 1);
            g_rbx_vis[i] = 1;
        }
    }
}
