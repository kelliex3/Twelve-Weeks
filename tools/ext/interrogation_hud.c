// interrogation_hud.c - the Interrogation HUD as widgets (2013 interui.png in our modern style). Included from t2r-ext.c right after
// interrogation.c; it only READS interrogation.c's state: g_int (server reply: p[] intel per team, need, rev[] lockbox located, st[] unlock
// stages done, hold = seconds per stage, box[], map, team), int_lockbox(team, pos), g_int_target + g_int_hold_frames (interrogating),
// g_lb_hold_frames (unlocking), int_live(), g_int_ended_sent.
// Widgets: hud-root t2rIntHud / t2rIntMk0 / t2rIntMk1 / t2rIntPr (tools/features/interrogation_hud_widgets.py). A pak without them falls back
// to interrogation.c's text HUD (feature_int_hud returns 0). Opt out of the widget HUD: t2r-no-int-hud.txt. Log: 'inthud:'.
//   - left block under the radar: INTERROGATIONS, two 5-segment bars (ours blue / theirs red; a segment per interrogation), a padlock at
//     the right end and three pips = the unlock stages that team got through
//   - ATTACK marker on the enemy lockbox once our team located it, DEFEND marker on ours once the ENEMY located it; off-screen = pinned to the screen edge
//     with a chevron; the ring around the circle fills with the unlock progress (stages done + the hold in progress)
//   - hold prompt bottom centre while interrogating / unlocking: "UNLOCKING..." + RELEASE (interact glyph) TO CANCEL + a progress bar
#define IH_SEGS 5
#define IH_PIPS 3
#define IH_RN 24                // the old round progress widget (t2rIntRn) segments
#define IH_RP 24                // fill pieces per marker arc (= widgets RING_PIECES); with the leading piece's alpha the ring fills continuously
#define IH_RING (3 * IH_RP)
#define IH_MK_CX 120.0f         // circle centre inside the marker box (= interrogation_hud_widgets.MK_C; the marker is 1.5x the earlier size)
#define IH_MK_CY 105.0f
#define IH_PR_W 900.0f          // prompt bar width (= the widget's 900 units)
static struct {
    uint64_t page, blk, f[2][IH_SEGS], pf[2][IH_PIPS], mk[2],
        rg[2][IH_RING], ar[2][4], pr, prl, pfil, rn, rnr[IH_RN], lbl[2],
        dist[2], tr[2][6];
    int mdist[2];               // the distance (m) written to the marker's text last (-1 = none)
    int miss, ok;
    // what is on screen now (-1 = unknown): written only on change
    int vis, seg[2], pip[2], mvis[2], arrow[2], prvis, prmode;
    signed char pv[2][IH_RING]; // marker ring pieces lit (1) / dark (0) now
    int lead[2], trvis[2];      // the piece with a partial alpha (-1 none); the grey track shown
    float mal[2], fade[2], disp[2], leada[2], ring_until[2];    // alpha last written; the near-crosshair / ADS fade; displayed progress; lead piece alpha; ring linger (s)
    float prfrac;
    int rnvis, rnseg;           // the round interrogation progress widget (t2rIntRn): visible, lit ring segments
    int al;                     // alpha (0..255) last written to the block / markers / prompt (-1 = unknown)
} g_ih;
static float g_ihT[2];
static uint64_t IhFind(const char *rel)
{                               // handle of a widget under canvas.hudRoot (= the hash of its path), 0 when the pak has no such widget
    uint64_t pa[9] =
        { FNV64("canvas.hudRoot"), (uint64_t) rel, 0, 0, 0, 0, 0, 0, 0 };
    uint64_t w = DcCall(FNV64("gui2-build-widget-path"), 9, pa);
    return w == ~0ULL ? 0 : w;
}

static int IhLoad(void)
{
    uint64_t page = *(uint64_t *) T2R_SYM(kUiPage);
    if (page != g_ih.page) {
        memset(&g_ih, 0, sizeof g_ih);
        g_ih.page = page;
        memset(g_ih.seg, -1, sizeof g_ih.seg);
        g_ih.vis = g_ih.prvis = g_ih.prmode = g_ih.al = g_ih.rnvis =
            g_ih.rnseg = -1;
        g_ih.pip[0] = g_ih.pip[1] = g_ih.mvis[0] = g_ih.mvis[1] =
            g_ih.arrow[0] = g_ih.arrow[1] = -1;
        memset(g_ih.pv, -1, sizeof g_ih.pv);
    }
    if (!page)
        return 0;
    if (g_ih.miss)
        return 0;
    if (g_ih.ok)
        return 1;
    char n[48];
    g_ih.blk = IhFind("t2rIntHud");
    if (!g_ih.blk) {
        g_ih.miss = 1;
        Xlog("inthud: widget t2rIntHud not in hud-root (old gui2-widgets.pak) - text HUD fallback");
        return 0;
    }
    for (int r = 0; r < 2; r++) {
        for (int i = 0; i < IH_SEGS; i++) {
            snprintf(n, sizeof n, "t2rIntHud.f%d%d", r, i);
            g_ih.f[r][i] = IhFind(n);
        }
        for (int k = 0; k < IH_PIPS; k++) {
            snprintf(n, sizeof n, "t2rIntHud.pf%d%d", r, k);
            g_ih.pf[r][k] = IhFind(n);
        }
        snprintf(n, sizeof n, "t2rIntMk%d", r);
        g_ih.mk[r] = IhFind(n);
        for (int k = 0; k < IH_RING; k++) {
            snprintf(n, sizeof n, "t2rIntMk%d.rg%d", r, k);
            g_ih.rg[r][k] = IhFind(n);
        }
        for (int k = 0; k < 6; k++) {
            snprintf(n, sizeof n, "t2rIntMk%d.tr%d%d", r, k / 2, k % 2);
            g_ih.tr[r][k] = IhFind(n);
        }                       // grey track arcs (an arc that passes 0 deg is 2 pieces)
        g_ih.lead[r] = -1;
        g_ih.mal[r] = -1.0f;
        g_ih.fade[r] = 1.0f;
        g_ih.disp[r] = -1.0f;
        g_ih.trvis[r] = -1;
        snprintf(n, sizeof n, "t2rIntMk%d.lbl", r);
        g_ih.lbl[r] = IhFind(n);
        snprintf(n, sizeof n, "t2rIntMk%d.dist", r);
        g_ih.dist[r] = IhFind(n);
        g_ih.mdist[r] = -1;
        static const char *const A[4] = { "aL", "aR", "aU", "aD" };
        for (int k = 0; k < 4; k++) {
            snprintf(n, sizeof n, "t2rIntMk%d.%s", r, A[k]);
            g_ih.ar[r][k] = IhFind(n);
        }
    }
    g_ih.pr = IhFind("t2rIntPr");
    g_ih.prl = IhFind("t2rIntPr.lbl");
    g_ih.pfil = IhFind("t2rIntPr.fil");
    g_ih.rn = IhFind("t2rIntRn");
    for (int k = 0; k < IH_RN; k++) {
        snprintf(n, sizeof n, "t2rIntRn.rn%d", k);
        g_ih.rnr[k] = IhFind(n);
    }
    if (!g_ih.rn || !g_ih.rnr[IH_RN - 1]) {
        g_ih.miss = 1;
        Xlog("inthud: t2rIntRn (round interrogation progress) not in hud-root - text HUD fallback");
        return 0;
    }
    if (!g_ih.mk[0] || !g_ih.mk[1] || !g_ih.pr || !g_ih.prl || !g_ih.pfil
        || !g_ih.f[0][0] || !g_ih.rg[1][IH_RING - 1] || !g_ih.ar[1][3]) {
        g_ih.miss = 1;
        Xlog("inthud: the interrogation widgets are incomplete in hud-root - text HUD fallback");
        return 0;
    }
    g_ih.ok = 1;
    Xlog("inthud: widget HUD up (bars, padlock + stage pips, ATTACK / DEFEND markers, hold prompt)");
    return 1;
}

static void IhHideAll(void)
{
    if (!g_ih.ok)
        return;
    if (g_ih.vis != 0) {
        WVisible(g_ih.blk, 0);
        g_ih.vis = 0;
    }
    for (int r = 0; r < 2; r++)
        if (g_ih.mvis[r] != 0) {
            WVisible(g_ih.mk[r], 0);
            g_ih.mvis[r] = 0;
        }
    if (g_ih.prvis != 0) {
        WVisible(g_ih.pr, 0);
        g_ih.prvis = 0;
    }
    if (g_ih.rnvis != 0) {
        WVisible(g_ih.rn, 0);
        g_ih.rnvis = 0;
    }
}

// The ring = 3 arcs of IH_RP pieces (interrogation_hud_widgets.ring_pieces: rg0..rg(RP-1) arc 1, then arc 2, arc 3; the gaps are the 2 checkpoints + the 12 o'clock start).
// frac 0..1 over the whole safe: arc i holds frac * 3 - i of itself, so a finished stage stays solid (its checkpoint) while the next arc fills. The piece being filled is shown with
// the fraction of it as alpha (the arc used to fill in visible steps): the fill is continuous at any frame rate. ring = 0: the whole ring (track + fill) is hidden (not interacting).
static void IhMarker(int k, int show, float wx, float wy, int edge,
                      float frac, int dist_m, int ring, float fade)
{                               // k 0 ATTACK / 1 DEFEND; wx, wy = the circle's centre in 1280 x 720 units
    if (!show) {
        if (g_ih.mvis[k] != 0) {
            WVisible(g_ih.mk[k], 0);
            g_ih.mvis[k] = 0;
        }
        return;
    }
    // Same projection (tag_project), same frame and same hook as the nametags and the teammate health bars (mhw_show: w_pivot from the draw hook).
    if (edge) {
        if (wy < 45.0f)
            wy = 45.0f;
        if (wy > 680.0f)
            wy = 680.0f;
    }                           // (the ring / the distance under it stay on screen)
    WPivot(g_ih.mk[k], (wx - 640.0f) * 3.0f - IH_MK_CX,
            wy * 3.0f - IH_MK_CY);
    if (g_ih.mvis[k] != 1) {
        WVisible(g_ih.mk[k], 1);
        g_ih.mvis[k] = 1;
        g_ih.mal[k] = -1.0f;
        if (g_ih.lbl[k])
            WVisible(g_ih.lbl[k], 0);
    }                           // no ATTACK / DEFEND text (the 2013 marker is the ring alone)
    {
        float a = g_radar_alpha / 255.0f * fade;
        if (__builtin_fabsf(a - g_ih.mal[k]) > 0.004f) {
            WAlpha(g_ih.mk[k], a);
            g_ih.mal[k] = a;
        }
    }
    if (ring != g_ih.trvis[k]) {
        for (int i = 0; i < 6; i++)
            if (g_ih.tr[k][i])
                WVisible(g_ih.tr[k][i], ring);
        g_ih.trvis[k] = ring;
    }
    int lead = -1;
    float la = 1.0f;
    for (int a = 0; a < 3; a++) {
        float fa = ring ? frac * 3.0f - (float) a : 0.0f;
        float u =
            fa <= 0.0f ? 0.0f : fa >=
            1.0f ? (float) IH_RP : fa * (float) IH_RP;
        int full = (int) u;
        float part = u - (float) full;
        if (part < 0.03f)
            part = 0.0f;
        for (int i = 0; i < IH_RP; i++) {
            int on = i < full || (i == full && part > 0.0f);
            signed char *pv = &g_ih.pv[k][a * IH_RP + i];
            if (*pv != on) {
                WVisible(g_ih.rg[k][a * IH_RP + i], on);
                *pv = (signed char) on;
            }
        }
        if (full < IH_RP && part > 0.0f) {
            lead = a * IH_RP + full;
            la = part;
        }
    }
    if (g_ih.lead[k] >= 0 && g_ih.lead[k] != lead)
        WAlpha(g_ih.rg[k][g_ih.lead[k]], 1.0f);        // the piece that just filled up: solid
    if (lead >= 0
        && (lead != g_ih.lead[k]
            || __builtin_fabsf(la - g_ih.leada[k]) > 0.01f)) {
        WAlpha(g_ih.rg[k][lead], la);
        g_ih.leada[k] = la;
    }
    g_ih.lead[k] = lead;
    if (g_ih.dist[k] && dist_m != g_ih.mdist[k]) {      // "42 m", rewritten only when the metre changes
        char t[16];
        snprintf(t, sizeof t, "%d m", dist_m);
        ((fn_settext_t) T2R_SYM(kFnSetText)) (g_ih.dist[k], t);
        g_ih.mdist[k] = dist_m;
    }
    int ar = -1;                // pinned to the screen edge: a chevron towards it
    if (edge) {
        float dx = wx - 640.0f, dy = wy - 360.0f;
        ar = (dx * dx * 0.5625f > dy * dy) ? (dx < 0 ? 0 : 1) : (dy <
                                                                 0 ? 2 :
                                                                 3);
    }
    if (ar != g_ih.arrow[k]) {
        for (int i = 0; i < 4; i++)
            if (i == ar || i == g_ih.arrow[k])
                WVisible(g_ih.ar[k][i], i == ar);
        g_ih.arrow[k] = ar;
    }
}

// the safebox UI was too hard to read; it only hides when actually aiming towards it: no fade near the
// crosshair and none in a melee duel (the earlier version had both) - the marker goes away only while we aim down sights (aim command / right mouse / zoom blend, the same read as vzoom_tick)
// AND it sits on the aim point (within IH_AIM_R px at 1080p of the screen centre; eased back in over 60 px).
#define IH_AIM_R 130.0f
static int IhAds(void)
{
    static uint64_t fr = ~0ULL;
    static int v;
    if (fr == g_frame)
        return v;
    fr = g_frame;
    uint64_t none = 0, ac = FNV64("aim");
    uint64_t ra = DcCall(FNV64("joypad-command-active?"), 1, &ac), zb =
        DcCall(FNV64("get-current-zoom-blend"), 0, &none);
    float z = 0;
    if (zb != ~0ULL)
        memcpy(&z, &zb, 4);
    v = (ra != ~0ULL && (ra & 0xff)) || (GameFocused()
                                         && (GetAsyncKeyState(VK_RBUTTON) &
                                             0x8000)) || (z > 0.05f
                                                          && z < 8.0f);
    return v;
}

// The tracking method was switched from the correct, nametag style one to a different one, so the marker lagged behind. The projection itself never changed
// (tag_project = the nametags' call, same draw hook, every frame, the safe's fixed table position). What differs: a nametag is TEXT queued in the frame it is projected, the marker is a
// gui2 WIDGET whose transform is drawn one frame after w_pivot sets it (the marker trailed the camera). An earlier version compensated that frame, a later one dropped the
// compensation ("same hook as the nametags" - but not the same draw path), another hid it with the engine's attachable trackers, and going back to the widget brought the lag again.
// Now the point is projected for the camera of the frame the widget is DRAWN in, so on screen it sits where a nametag on that point would: the render camera (rcam, captured each
// frame) is extrapolated one frame (position + lens axis), and the safe's point is moved by the rotation + translation from that predicted camera to the current one before the same
// tag_project. Off when the frames are not consecutive, the camera cut / jumped (> 25 deg or > 2 m in a frame), or t2r-int-mk-no-lead.txt is next to the exe (test). Log: 'inthud: lead'.
static struct {
    uint64_t fr;
    int ok;
    float cp[3], cd[3], pp[3], pd[3];
} g_ihc;
static int IhLeadOff(void)
{
    static uint64_t at;
    static int off;
    return FeelFlag("t2r-int-mk-no-lead.txt", &at, &off, 120);
}

static void IhCamFrame(void)
{
    if (g_ihc.fr && g_ihc.fr == g_drawFrame)
        return;
    float cp[3], cd[3];
    int have = CamPosDir(cp, cd);
    if (have) {
        float l =
            __builtin_sqrtf(cd[0] * cd[0] + cd[1] * cd[1] + cd[2] * cd[2]);
        if (l < 1e-4f)
            have = 0;
        else
            for (int i = 0; i < 3; i++)
                cd[i] /= l;
    }
    int consec = g_ihc.fr && g_drawFrame - g_ihc.fr == 1 && have;
    if (consec) {
        float c =
            cd[0] * g_ihc.cd[0] + cd[1] * g_ihc.cd[1] +
            cd[2] * g_ihc.cd[2], dx = cp[0] - g_ihc.cp[0], dy =
            cp[1] - g_ihc.cp[1], dz = cp[2] - g_ihc.cp[2];
        if (c < 0.906f || dx * dx + dy * dy + dz * dz > 4.0f)
            consec = 0;         // (a cut / teleport: no prediction across it)
    }
    if (consec) {
        float pd[3], l = 0;
        for (int i = 0; i < 3; i++) {
            g_ihc.pp[i] = 2.0f * cp[i] - g_ihc.cp[i];
            pd[i] = 2.0f * cd[i] - g_ihc.cd[i];
            l += pd[i] * pd[i];
        }
        l = __builtin_sqrtf(l);
        if (l > 1e-4f)
            for (int i = 0; i < 3; i++)
                g_ihc.pd[i] = pd[i] / l;
        else
            consec = 0;
    }
    g_ihc.ok = consec && !IhLeadOff();
    if (have) {
        memcpy(g_ihc.cp, cp, 12);
        memcpy(g_ihc.cd, cd, 12);
        g_ihc.fr = g_drawFrame;
    } else
        g_ihc.fr = 0;
}

// p seen from the predicted camera (pp, pd) = the point q seen from the current camera (cp, cd): q = cp + Rot(pd -> cd) (p - pp), Rot = the shortest rotation between the two axes
static void IhLeadPoint(const float *p, float *q)
{
    memcpy(q, p, 12);
    if (!g_ihc.ok)
        return;
    const float *a = g_ihc.pd, *b = g_ihc.cd;
    float v[3] =
        { p[0] - g_ihc.pp[0], p[1] - g_ihc.pp[1], p[2] - g_ihc.pp[2] };
    float k[3] =
        { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
   a[0] * b[1] - a[1] * b[0] }, c =
a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    float sn =
        __builtin_sqrtf(k[0] * k[0] + k[1] * k[1] + k[2] * k[2]), r[3];
    if (sn > 1e-6f) {           // Rodrigues: v cos + (k x v) sin + k (k.v)(1 - cos), k unit
        for (int i = 0; i < 3; i++)
            k[i] /= sn;
        float kv = k[0] * v[0] + k[1] * v[1] + k[2] * v[2], kx[3] =
            { k[1] * v[2] - k[2] * v[1], k[2] * v[0] - k[0] * v[2],
k[0] * v[1] - k[1] * v[0] };
        for (int i = 0; i < 3; i++)
            r[i] = v[i] * c + kx[i] * sn + k[i] * kv * (1.0f - c);
    } else
        memcpy(r, v, 12);
    for (int i = 0; i < 3; i++)
        q[i] = g_ihc.cp[i] + r[i];
    static int nl;
    static uint64_t lt;
    if (nl < 12 && GetTickCount64() - lt > 5000) {
        float x0, y0, x1, y1;
        if (TagScreen(p, &x0, &y0) && TagScreen(q, &x1, &y1)) {
            nl++;
            lt = GetTickCount64();
            Xlog("inthud: lead - marker set %.1f, %.1f px (1080p) ahead of this frame's projection (%.0f, %.0f) for the frame it is drawn in", (x1 - x0) * 1.5f, (y1 - y0) * 1.5f, x0, y0);
        }
    }
}

// the safe marker was still laggy with the one-frame lead: the marker is now an ATTACHABLE widget (interrogation_hud_widgets.add_marker_attachables:
// hud-buddy-status/t2r-int-mk0 / mk1 = the same disc, track, ring, icon and distance under a Gui2::AttachableWidget root) shown on the lockbox object every frame - the engine
// places it in the frame it draws, as the buddy + and the mark triangle (which the nametags match), so it cannot trail the camera; off screen the engine pins it to the border
// with the stock offscreen arrow (clampType default). The ring / distance / alpha go to the instance's children (gui2-build-widget-path on the instance, as world_prompt's
// textAction). The w_pivot widget above stays the fallback (no lockbox object, an old pak without the files: the show answers 0) and t2r-int-mk-widget.txt next to the exe forces
// it (test). Log: 'inthud: attachable'.
static const char *const IH_ATT_NAME[2] =
    { "hud-buddy-status/t2r-int-mk0", "hud-buddy-status/t2r-int-mk1" };
static struct {
    uint64_t inst, rg[IH_RING], tr[6], dist, frame;
    signed char pv[IH_RING];
    int trvis, lead, mdist, nnew, nfail, dimm;
    float leada, al;
} g_iha[2];
static int IhAttOff(void)
{
    static uint64_t at;
    static int off;
    return FeelFlag("t2r-int-mk-widget.txt", &at, &off, 120);
}

static uint64_t IhChild(uint64_t inst, const char *rel)
{
    uint64_t pa[9] = { inst, (uint64_t) rel, 0, 0, 0, 0, 0, 0, 0 };
    uint64_t w = DcCall(FNV64("gui2-build-widget-path"), 9, pa);
    return w == ~0ULL ? 0 : w;
}

// 1 = the attachable marker is up this frame (the pivot widget is hidden), 0 = not available (fallback to the pivot widget)
static int IhAttMarker(int k, int tm, float frac, int dist_m, int ring,
                         float fade)
{
    uint64_t obj = g_lbObj[tm];
    if (!obj || IhAttOff())
        return 0;
    // the offset from the object's origin to the marker point (= int_lockbox + the height the pivot marker uses): the safe's origin is its front-left-bottom corner
    // (interrogation.c int_lockbox_props: crate centre + (-0.425, 0, +0.646) in the safe's frame), the NR crate's origin is its centre
    float off[3] = { 0.0f, 1.0f, 0.0f };
    if (g_lbSafe[tm]) {
        off[0] = 0.425f;
        off[1] = 1.35f;
        off[2] = -0.646f;
    }
    __typeof__(g_iha[0]) * a = &g_iha[k];
    uint64_t aw =
        AttachWidgetV(obj, IH_ATT_NAME[k], FNV64("root"), off, -1, 0,
                        &a->dimm);
    if (!aw || aw == ~0ULL) {
        if (a->nfail++ < 3)
            Xlog("inthud: attachable %s did not come up on the lockbox object %llx (%s) - pivot widget marker", IH_ATT_NAME[k], (unsigned long long) obj, g_lbSafe[tm] ? "safe" : "crate");
        a->inst = 0;
        return 0;
    }
    if (aw != a->inst || g_frame - a->frame > 3) {      // a new instance: its children, and every cached state unknown
        a->inst = aw;
        for (int i = 0; i < IH_RING; i++) {
            char n[8];
            snprintf(n, sizeof n, "rg%d", i);
            a->rg[i] = IhChild(aw, n);
        }
        for (int i = 0; i < 6; i++) {
            char n[8];
            snprintf(n, sizeof n, "tr%d%d", i / 2, i % 2);
            a->tr[i] = IhChild(aw, n);
        }
        a->dist = IhChild(aw, "dist");
        memset(a->pv, -1, sizeof a->pv);
        a->trvis = -1;
        a->lead = -1;
        a->mdist = -1;
        a->al = -1.0f;
        a->leada = 0.0f;
        if (a->nnew++ < 6)
            Xlog("inthud: attachable %s up on the lockbox (%s, offset %.2f %.2f %.2f) - instance %llx, ring %s, distance %s", IH_ATT_NAME[k], g_lbSafe[tm] ? "safe" : "crate", off[0], off[1], off[2], (unsigned long long) aw, a->rg[IH_RING - 1] ? "found" : "MISSING", a->dist ? "found" : "MISSING");
    }
    a->frame = g_frame;
    {
        float al = g_radar_alpha / 255.0f * fade;
        if (__builtin_fabsf(al - a->al) > 0.004f) {
            WAlpha(aw, al);
            a->al = al;
        }
    }
    if (ring != a->trvis) {
        for (int i = 0; i < 6; i++)
            if (a->tr[i])
                WVisible(a->tr[i], ring);
        a->trvis = ring;
    }
    int lead = -1;
    float la = 1.0f;
    for (int s = 0; s < 3; s++) {
        float fa = ring ? frac * 3.0f - (float) s : 0.0f;
        float u =
            fa <= 0.0f ? 0.0f : fa >=
            1.0f ? (float) IH_RP : fa * (float) IH_RP;
        int full = (int) u;
        float part = u - (float) full;
        if (part < 0.03f)
            part = 0.0f;
        for (int i = 0; i < IH_RP; i++) {
            int on = i < full || (i == full && part > 0.0f);
            signed char *pv = &a->pv[s * IH_RP + i];
            if (*pv != on && a->rg[s * IH_RP + i]) {
                WVisible(a->rg[s * IH_RP + i], on);
                *pv = (signed char) on;
            }
        }
        if (full < IH_RP && part > 0.0f) {
            lead = s * IH_RP + full;
            la = part;
        }
    }
    if (a->lead >= 0 && a->lead != lead && a->rg[a->lead])
        WAlpha(a->rg[a->lead], 1.0f);
    if (lead >= 0 && a->rg[lead]
        && (lead != a->lead || __builtin_fabsf(la - a->leada) > 0.01f)) {
        WAlpha(a->rg[lead], la);
        a->leada = la;
    }
    a->lead = lead;
    if (a->dist && dist_m != a->mdist) {
        char t[16];
        snprintf(t, sizeof t, "%d m", dist_m);
        ((fn_settext_t) T2R_SYM(kFnSetText)) (a->dist, t);
        a->mdist = dist_m;
    }
    return 1;
}

static float IhNowS(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (float) ((double) c.QuadPart / (double) f.QuadPart);
}

// 1 = the widgets drew this frame (interrogation.c's text HUD stays off), 0 = no widgets in the pak
static int FeatureIntHud(void)
{
    static int off = -1;
    if (off < 0) {
        FILE *f = ExeSideOpen("t2r-no-int-hud.txt", "r");
        off = f != NULL;
        if (f)
            fclose(f);
        if (off)
            Xlog("inthud: off (t2r-no-int-hud.txt)");
    }
    if (off)
        return 0;
    if (!IntLive() && !g_ih.ok)
        return 1;               // load the widgets only once an Interrogation match is live (no "widget HUD up" in a Supply Raid match); the text HUD is off outside Interrogation anyway
    if (!IhLoad())
        return 0;
    // match start: the rest of the HUD is held back too - the winmm text layer (g_text_hide: loading screen, intro black hold, HUD option off), the
    // title card (g_tc_t0 .. TC_OUT_TO, feature_hud_fade holds every hudRoot child but the t2r overlays), the intro swoop (g_intro_tags_until, like
    // the nametags) - and fades in with the radar (g_radar_alpha follows the HUD's fade ramp)
    int tc_hold = g_tcHudDelay && g_tcT0
        && GetTickCount64() - g_tcT0 < TC_OUT_TO;
    int intro = g_text_hide || tc_hold || g_icam_on || (g_introTagsUntil
                                                        && GetTickCount64()
                                                        <
                                                        g_introTagsUntil);
    // consistency is key: exactly the native HUD's visibility - hud_hidden() (t2r-ext.c): the ESC menu and ALL its sub-pages (winmm's pause-on-stack gate), the
    // backpack / TAB, loading / intro / HUD option off / freecam, the death black, our death / spectating, the finisher cam, match over - plus this mode's own conditions.
    int hide = HUD_HIDDEN() || !IntLive() || g_int_ended_sent || intro;
    if (hide) {
        IhHideAll();
        return 1;
    }
    if (g_ih.al != (int) g_radar_alpha) {
        float a = g_radar_alpha / 255.0f;
        WAlpha(g_ih.blk, a);
        WAlpha(g_ih.pr, a);
        WAlpha(g_ih.rn, a);
        g_ih.al = g_radar_alpha;
    }
    int team = g_int.team, enemy = team ^ 1;
    if (g_ih.vis != 1) {
        WVisible(g_ih.blk, 1);
        g_ih.vis = 1;
    }
    // bars: a segment per interrogation (intel / need), pips = unlock stages done
    int need = g_int.need > 0 ? g_int.need : IH_SEGS;
    for (int r = 0; r < 2; r++) {
        int tm = r ? enemy : team, p = g_int.p[tm] < 0 ? 0 : g_int.p[tm];
        int seg = p >= need ? IH_SEGS : (p * IH_SEGS + need - 1) / need;
        if (seg != g_ih.seg[r]) {
            for (int i = 0; i < IH_SEGS; i++)
                WVisible(g_ih.f[r][i], i < seg);
            g_ih.seg[r] = seg;
        }
        int st =
            g_int.st[tm] > IH_PIPS ? IH_PIPS : g_int.st[tm] <
            0 ? 0 : g_int.st[tm];
        if (st != g_ih.pip[r]) {
            for (int k = 0; k < IH_PIPS; k++)
                WVisible(g_ih.pf[r][k], k < st);
            g_ih.pip[r] = st;
        }
    }
    // markers: shown ONLY once the safe is discovered - ATTACK on the enemy safe after OUR team located it (g_int.rev[team]: its intel count is full, server rev), DEFEND on
    // our safe after the ENEMY located it (g_int.rev[enemy]; the server's rev for the other team, the same reply field that raises "The enemy knows where your lockbox is").
    float hold = g_int.hold > 0.5f ? g_int.hold : 8.0f;
    float hold_frac =
        g_lb_hold_frames ? (float) g_lb_hold_frames / (hold *
                                                       60.0f) : 0.0f;
    if (hold_frac > 1.0f)
        hold_frac = 1.0f;
    for (int k = 0; k < 2; k++) {
        int tm = k ? team : enemy;      // the lockbox this marker sits on (ATTACK -> theirs, DEFEND -> ours)
        float p[3], x = 0, y = 0;
        int edge = 0, show = 0;
        // a marker with a question mark appeared on the safe: the attachable siege trackers draw hud/interactions/safe.png, which is not resident in the MP
        // texture packs = a tiny circled '?' stuck on the safe. Now: the lockbox marker belongs to the team that EARNED the reveal
        // only (2013: after the interrogations) - ATTACK on the enemy lockbox - and nothing is drawn for our own lockbox / a team that has not earned it. It is our own
        // screen-space marker (tag_project + edge clamp + progress ring) above the safe; t2r-int-tracker-attach.txt next to the exe = the NR attachable trackers again.
        int want =
            (k ? g_int.p[enemy] >=
             (g_int.need > 0 ? g_int.need : 5) : g_int.rev[team])
            && g_int.st[k ? enemy : team] < 3 && IntLockbox(tm, p);
        static int attach_on = -1;
        static uint64_t attach_at;
        if (attach_on < 0 || GetTickCount64() - attach_at > 3000) {
            FILE *fa = ExeSideOpen("t2r-int-tracker-attach.txt", "r");
            attach_on = fa != NULL;
            if (fa)
                fclose(fa);
            attach_at = GetTickCount64();
        }
        // Zero lag: the engine's own No Return siege trackers as ATTACHABLE widgets on the lockbox crate (the same engine mechanism as the mark triangle /
        // buddy ring, which the nametags are matched to): ATTACK = t2r-hud-siege-safe-tracker (ring + safe pictogram), DEFEND = t2r-hud-defense-cover-tracker
        // (shield); off-screen edge clamp + arrow come with them. Shown again every frame (fade 0.25 s once we stop). The w_pivot widgets below are the
        // fallback only when there is no crate object (g_lb_obj == 0: prop spawn failed / not in the MP level) or the attachable did not come up.
        static int dimm[2];
        if (want && attach_on && g_lbObj[tm]) {
            uint64_t aw =
                AttachWidgetJ(g_lbObj[tm],
                                k ?
                                "t2r-hud-rogue/t2r-hud-defense-cover-tracker"
                                :
                                "t2r-hud-rogue/t2r-hud-siege-safe-tracker",
                                FNV64("root"), 0.9f, -1, 0, &dimm[k]);
            if (aw) {
                IhMarker(k, 0, 0, 0, 0, 0, 0, 0, 1.0f);
                continue;
            }
        }
        if (want) {
            p[1] += g_lbSafe[tm] ? 1.35f : 1.0f;
            float q[3];
            IhCamFrame();
            IhLeadPoint(p, q);
            show = TagProject(q, &x, &y, &edge);
        }
        // ring: the stages that team got through (the checkpoints stay filled) + the stage in progress = the highest of our own hold (ATTACK) and the server's live report
        // of that team's unlockers (h[]: a teammate on ATTACK, the enemy on DEFEND); eased so the 1/3 s reports fill smoothly. Letting go loses only the running stage.
        int ut = k ? enemy : team;      // the team that unlocks this safe
        float live = g_int.h[ut] / 100.0f;
        if (k == 0 && hold_frac > live)
            live = hold_frac;
        if (live > 1.0f)
            live = 1.0f;
        float tgt = ((float) g_int.st[ut] + live) / 3.0f;
        // Per-frame display value (the animation update was not smooth, steps every ~6 frames): our own hold is exact (g_lb_hold_frames, real time); a remote unlocker is only
        // reported every 1/3 s, so between reports the arc advances at the known rate (1 / (3 hold) per s), pulled gently to each new report - no steps.
        float tnow = IhNowS(), dt = tnow - g_ihT[k];
        g_ihT[k] = tnow;
        if (dt < 0.0f || dt > 0.1f)
            dt = 0.016f;
        int own = k == 0 && hold_frac > 0.0f
            && hold_frac >= g_int.h[ut] / 100.0f;
        int act = live > 0.0f;
        float *dp = &g_ih.disp[k];
        if (*dp < 0.0f)
            *dp = tgt;
        else if (own)
            *dp = tgt;
        else if (act) {
            float rate = 1.0f / (3.0f * hold), err;
            *dp += rate * dt;
            err = tgt - *dp;
            if (err > 0.0f)
                *dp += err * (dt * 5.0f > 1.0f ? 1.0f : dt * 5.0f);
            else if (*dp > tgt + rate * 0.6f)
                *dp = tgt + rate * 0.6f;        // (a report is 1/3 s old at most; never run far ahead of it)
        } else
            *dp += (tgt - *dp) * (dt * 8.0f > 1.0f ? 1.0f : dt * 8.0f);
        if (__builtin_fabsf(tgt - *dp) < 0.0015f && !act)
            *dp = tgt;
        if (act)
            g_ih.ring_until[k] = tnow + 1.0f;   // the ring shows while somebody unlocks, 1 s after the last (gaps between reports)
        int ring_on = tnow < g_ih.ring_until[k];
        int dist_m = 0;
        {
            float cp[3], cd[3];
            if (want && CamPosDir(cp, cd)) {
                float dx = p[0] - cp[0], dy = p[1] - cp[1], dz =
                    p[2] - cp[2];
                dist_m =
                    (int) (__builtin_sqrtf(dx * dx + dy * dy + dz * dz) +
                           0.5f);
            }
        }
        float ftg = 1.0f;
        if (show && !edge && IhAds()) {        // aiming down sights AT the marker: out of the way of the shot
            float dx = (x - 640.0f) * 1.5f, dy = (y - 360.0f) * 1.5f, d = __builtin_sqrtf(dx * dx + dy * dy);   // px at 1080p
            ftg =
                d < IH_AIM_R ? 0.0f : d <
                IH_AIM_R + 60.0f ? (d - IH_AIM_R) / 60.0f : 1.0f;
        }
        g_ih.fade[k] +=
            (ftg - g_ih.fade[k]) * (dt * 10.0f > 1.0f ? 1.0f : dt * 10.0f);
        if (g_ih.fade[k] < 0.02f && ftg == 0.0f)
            show = 0;
        if (show
            && IhAttMarker(k, tm, *dp > 1.0f ? 1.0f : *dp, dist_m,
                             ring_on, g_ih.fade[k]))
            IhMarker(k, 0, 0, 0, 0, 0, 0, 0, 1.0f);    // the attachable is up: no pivot widget
        else
            IhMarker(k, show, x, y, edge, *dp > 1.0f ? 1.0f : *dp, dist_m,
                      ring_on, g_ih.fade[k]);
    }
    // hold prompt: interrogating a downed enemy. the interrogation is HOLD-to-continue. The button glyph + the clockwise hold ring is the engine's
    // own interactable prompt over the victim (world_prompt, interrogation.c); here the band under it: "LET GO TO STOP INTERROGATING" on the feathered dark panel.
    // (The lockbox UNLOCKING has no text / bar any more: the dial sound + the safe's dial-anim, interrogation.c.)
    int mode = 0;               // the dark band + "LET GO TO STOP INTERROGATING" is gone - the white native hold prompt (world_prompt) is the only UI
    if (g_ih.rnvis != 0) {
        WVisible(g_ih.rn, 0);
        g_ih.rnvis = 0;
        for (int i = 0; i < IH_RN; i++)
            WVisible(g_ih.rnr[i], 0);
        g_ih.rnseg = -1;
    }                           // the old round ?! progress: replaced by the native ring
    if (!mode) {
        if (g_ih.prvis != 0) {
            WVisible(g_ih.pr, 0);
            g_ih.prvis = 0;
            g_ih.prmode = 0;
        }
        return 1;
    }
    if (g_ih.prmode != 1) {
        ((fn_settext_t) T2R_SYM(kFnSetText)) (g_ih.prl,
                                              LocKeep
                                              ("LET GO TO STOP INTERROGATING"));
        WVisible(g_ih.pfil, 0);
        const char *const X[5] =
            { "t2rIntPr.rel", "t2rIntPr.icon", "t2rIntPr.can",
"t2rIntPr.trk", "t2rIntPr.fil" };
        for (int i = 0; i < 5; i++) {
            uint64_t h = IhFind(X[i]);
            if (h)
                WVisible(h, 0);
        }
        g_ih.prmode = 1;
    }
    if (g_ih.prvis != 1) {
        WVisible(g_ih.pr, 1);
        g_ih.prvis = 1;
        WAlpha(g_ih.pr, g_radar_alpha / 255.0f);
    }
    return 1;
}
