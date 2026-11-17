// ---------------------------------------------------------------- SP-style key prompt animation (spec re/sp-prompts-1031/ANIMATION.md)
// Included in t2r-ext.c right after the two old self-draw routines (renamed *_old). Self-drawn prompts (the grey wait / counter-window prompts,
// and the opt-in t2r-wp-selfdraw.txt label) get SP's key behaviour: the key outline grows in (0.12 s), the letter fades in grey and goes white by
// 0.35 s, text fades in with a small upward settle and everything fades out in 0.1 s.
// WAIT (the MASH prompt's ring, not the hold brackets): the key plus a thin light-grey ring that starts at 1.5x the key and
// CONTRACTS at a steady rate onto the key border, landing exactly when the window opens (a timer, slow and readable), no text. MASH: the same
// ring flashes on every press (1.5x -> key border in ~0.18 s, fading). HOLD: the key border fills clockwise with the progress (no NN%).
// Shapes are dots of winmm's text glyph '=' (the dodge cooldown bar's trick: the only 2D primitive the HUD draw hook has). The key letter follows
// the active device (E / Y). The engine's own prompt widgets (the native interactable request) carry SP's animations and hold ring themselves.
// t2r-prompt-anim-old.txt = the old texts ("...wait", NN%, plain grey text).
#define PA_KEY_HALF 17.0f       // key half size in the 1280 x 720 box
#define PA_RING_START 1.5f      // ring radius at the start of a wait / a press flash, in key sizes
#define PA_FLASH_S 0.18f        // per-press ring contraction
enum { PA_TEXT = 0, PA_WAIT = 1, PA_MASH = 2, PA_PRESS = 3, PA_HOLD = 4 };
static int PaOld(void)
{
    static int v = -1;
    if (v < 0) {
        FILE *f = ExeSideOpen("t2r-prompt-anim-old.txt", "r");
        v = f != NULL;
        if (f)
            fclose(f);
        Xlog("prompt anim: %s",
             v ? "OFF (t2r-prompt-anim-old.txt: old texts)" :
             "ON (SP key appear / contracting wait ring / mash ring flash / hold fill, self-drawn prompts)");
    }
    return v;
}

static void PaPathLog(const char *label, const char *path)
{
    static char seen[24][48];
    static int n;
    for (int i = 0; i < n; i++)
        if (!strcmp(seen[i], label))
            return;
    if (n < 24) {
        snprintf(seen[n++], 48, "%s", label);
        Xlog("prompt anim: '%s' -> %s", label, path);
    }
}

static const char *PaKeyLetter(void)
{
    static int pad;
    WORD b;
    BYTE l, r;
    SHORT x, y;
    if (PadState(&b, &l, &r, &x, &y)
        && (b || l > 60 || r > 60 || x > 12000 || x < -12000 || y > 12000
            || y < -12000))
        pad = 1;
    else if ((GetAsyncKeyState('W') | GetAsyncKeyState('A') |
              GetAsyncKeyState('S') | GetAsyncKeyState('D') |
              GetAsyncKeyState('E') | GetAsyncKeyState('F') |
              GetAsyncKeyState(VK_SPACE) | GetAsyncKeyState(VK_LSHIFT) |
              GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON))
             & 0x8000)
        pad = 0;
    return pad ? "Y" : "E";
}

// a press edge of the interact / melee key or any pad button (drives the mash ring flash)
static int PaPressEdge(void)
{
    static int was;
    WORD b;
    BYTE l, r;
    SHORT x, y;
    int d =
        ((GetAsyncKeyState('E') | GetAsyncKeyState('F')) & 0x8000) != 0;
    if (!d && PadState(&b, &l, &r, &x, &y) && (b & 0xF000))
        d = 1;                  // face buttons
    int e = d && !was;
    was = d;
    return e;
}

static float PaClamp01(float v)
{
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

static void PaDot(float cx, float cy, float a, float lum)
{
    static float gw, gh, dsz;
    if (!gw) {
        uint64_t wh = ((fn_measure) T2R_SYM(kFnMeasureText)) ("-", 1.0f);
        uint32_t lo = (uint32_t) wh, hi = (uint32_t) (wh >> 32);
        memcpy(&gw, &lo, 4);
        memcpy(&gh, &hi, 4);
        if (!(gw > 0.01f && gw < 50.0f && gh > 0.01f && gh < 50.0f)) {
            gw = 0;
            return;
        }
        dsz = 3.5f / gw;        // (the E button looked wrong: sparse '=' dots read as a dotted box - dense '-' strokes now draw solid lines)
    }
    uint32_t al = (uint32_t) (PaClamp01(a) * 235.0f + 0.5f), lv =
        (uint32_t) (PaClamp01(lum) * 255.0f + 0.5f);
    if (!al)
        return;
    ((fn_text_va) T2R_SYM(kFnDrawTextVa)) (Fbits(cx - gw * dsz * 0.5f),
                                           Fbits(cy - gh * dsz * 0.5f),
                                           Fbits(dsz),
                                           (uint64_t) (al << 24 | lv << 16
                                                       | lv << 8 | lv),
                                           "%s", "-");
}

// point on a square of half size s around (cx,cy), u in [0,1) clockwise from the top middle
static void PaPerim(float cx, float cy, float s, float u, float *px,
                     float *py)
{
    u -= (float) (int) u;
    if (u < 0)
        u += 1.0f;
    float d = u * 8.0f * s, e = 2.0f * s;
    if (d < s) {
        *px = cx + d;
        *py = cy - s;
        return;
    }
    d -= s;
    if (d < e) {
        *px = cx + s;
        *py = cy - s + d;
        return;
    }
    d -= e;
    if (d < e) {
        *px = cx + s - d;
        *py = cy + s;
        return;
    }
    d -= e;
    if (d < e) {
        *px = cx - s;
        *py = cy + s - d;
        return;
    }
    d -= e;
    *px = cx - s + d;
    *py = cy - s;
}

static void PaCircle(float x, float y, float r, float a, float lum)
{
    int n = (int) (r * 6.2832f / 1.4f);
    if (n < 24)
        n = 24;                 // a dot every ~1.4 px: a solid ring
    for (int i = 0; i < n; i++) {
        float an = (float) i * (6.2832f / (float) n);
        PaDot(x + r * __builtin_cosf(an), y + r * __builtin_sinf(an), a,
               lum);
    }
}

static struct {
    int vis, kind;
    uint64_t t0, tlast, flash;
    float x, y, hold;
    char label[48];
} g_pa[2];
// one frame of one prompt slot. active=0: fade the last drawn state out (0.1 s). prog = wait timer 0..1 / hold progress
static int PaFrame(int si, int active, float x, float y, int kind,
                    const char *label, float prog)
{
    uint64_t now = GetTickCount64();
    typeof(g_pa[0]) * s = &g_pa[si];
    if (active) {
        if (!s->vis || s->kind != kind
            || (kind == PA_TEXT && strcmp(s->label, label))) {
            s->vis = 1;
            s->kind = kind;
            s->t0 = now;
            s->flash = 0;
        }
        s->tlast = now;
        s->x = x;
        s->y = y;
        s->hold = prog;
        snprintf(s->label, sizeof s->label, "%s", label ? label : "");
        if (kind == PA_MASH && PaPressEdge())
            s->flash = now;
    } else if (!s->vis || now - s->tlast > 100) {
        s->vis = 0;
        return 0;
    }
    float t = (float) (now - s->t0) * 0.001f, fo =
        active ? 1.0f : 1.0f - (float) (now - s->tlast) * 0.01f;
    if (fo <= 0.0f) {
        s->vis = 0;
        return 0;
    }
    x = s->x;
    y = s->y;
    if (s->kind == PA_TEXT) {   // grey text: fade in 0.15 s with an upward settle, out in 0.1 s
        float a = PaClamp01(t / 0.15f) * fo, k =
            PaClamp01(t / 0.15f), settle = 4.0f * (1.0f - k * (2.0f - k));
        const float size = 0.9f;
        char txt[64];
        snprintf(txt, sizeof txt, "%s", LocFold(s->label));
        uint64_t wh = ((fn_measure) T2R_SYM(kFnMeasureText)) (txt, size);
        float w, hgt;
        uint32_t lo = (uint32_t) wh, hi = (uint32_t) (wh >> 32);
        memcpy(&w, &lo, 4);
        memcpy(&hgt, &hi, 4);
        (void) w;
        ((fn_text_va) T2R_SYM(kFnDrawTextVa)) (Fbits(x),
                                               Fbits(y - hgt * 0.5f +
                                                     settle), Fbits(size),
                                               (uint64_t) ((uint32_t)
                                                           (a * 160.0f +
                                                            0.5f) << 24 |
                                                           0xA0A0A0u),
                                               "%s", txt);
        return 1;
    }
    float S = PA_KEY_HALF, grow = PaClamp01(t / 0.12f), sc =
        0.35f + 0.65f * grow * (2.0f - grow);
    float oa = PaClamp01(t / 0.05f) * fo, la =
        PaClamp01((t - 0.12f) / 0.08f) * fo, lum =
        0.55f + 0.45f * PaClamp01((t - 0.12f) / 0.23f);
    float hs = S * sc;
    int nd = (int) (8.0f * hs / 1.4f);
    if (nd < 8)
        nd = 8;                 // a dot every ~1.4 px: a solid border
    float fill = s->kind == PA_HOLD ? PaClamp01(s->hold) : -1.0f;
    for (int i = 0; i < nd; i++) {      // the key border (dim; a hold fills it clockwise in white)
        float u = (float) i / (float) nd, px, py;
        PaPerim(x, y, hs, u, &px, &py);
        int lit = fill >= 0.0f && u < fill;
        PaDot(px, py, oa * (lit ? 1.0f : 0.7f), lit ? 1.0f : lum * 0.7f);
    }
    if (oa > 0.0f && fill < 0.0f) {     // the key face (SP: a light rounded square with a dark letter), rows 1.6 px apart
        float fa = oa * (0.25f + 0.65f * PaClamp01((t - 0.12f) / 0.23f));
        for (float yy = -hs + 2.0f; yy <= hs - 2.0f; yy += 1.6f)
            for (float xx = -hs + 2.0f; xx <= hs - 2.0f; xx += 1.6f)
                PaDot(x + xx, y + yy, fa, lum * 0.92f);
        lum = 0.08f;            // dark letter on the light face
    }
    if (la > 0.0f) {            // the letter
        const char *k = PaKeyLetter();
        uint64_t wh = ((fn_measure) T2R_SYM(kFnMeasureText)) (k, 1.0f);
        float w, hgt;
        uint32_t lo = (uint32_t) wh, hi = (uint32_t) (wh >> 32);
        memcpy(&w, &lo, 4);
        memcpy(&hgt, &hi, 4);
        uint32_t lv = (uint32_t) (lum * 255.0f + 0.5f);
        ((fn_text_va) T2R_SYM(kFnDrawTextVa)) (Fbits(x - w * 0.5f),
                                               Fbits(y - hgt * 0.5f),
                                               Fbits(1.0f),
                                               (uint64_t) ((uint32_t)
                                                           (la * 255.0f +
                                                            0.5f) << 24 |
                                                           lv << 16 | lv <<
                                                           8 | lv), "%s",
                                               k);
    }
    if (s->kind == PA_WAIT && t > 0.12f) {      // the timer ring: 1.5x -> the key border at a steady rate, landing when the window opens
        float p = PaClamp01(s->hold), r =
            (S + 3.0f) * (PA_RING_START - (PA_RING_START - 1.0f) * p);
        PaCircle(x, y, r, PaClamp01((t - 0.12f) / 0.1f) * fo * 0.8f,
                  0.78f);
    }
    if (s->kind == PA_MASH && s->flash && now - s->flash < (uint64_t) (PA_FLASH_S * 1000.0f)) { // per press: ring 1.5x -> the border, fading
        float p = (float) (now - s->flash) * 0.001f / PA_FLASH_S, r =
            (S + 3.0f) * (PA_RING_START - (PA_RING_START - 1.0f) * p);
        PaCircle(x, y, r, (1.0f - p) * fo * 0.85f, 0.78f);
    }
    if (s->kind == PA_PRESS || s->kind == PA_HOLD) {    // label right of the key: fades in after the key, small upward settle
        float k = PaClamp01((t - 0.12f) / 0.15f), settle =
            4.0f * (1.0f - k * (2.0f - k)), a = k * fo;
        const float size = 0.9f;
        char txt[64];
        snprintf(txt, sizeof txt, "%s", LocFold(s->label));
        uint64_t wh = ((fn_measure) T2R_SYM(kFnMeasureText)) (txt, size);
        float w, hgt;
        uint32_t lo = (uint32_t) wh, hi = (uint32_t) (wh >> 32);
        memcpy(&w, &lo, 4);
        memcpy(&hgt, &hi, 4);
        (void) w;
        if (a > 0.0f)
            ((fn_text_va) T2R_SYM(kFnDrawTextVa)) (Fbits(x + S + 10.0f),
                                                   Fbits(y - hgt * 0.5f +
                                                         settle),
                                                   Fbits(size),
                                                   (uint64_t) ((uint32_t)
                                                               (a *
                                                                255.0f +
                                                                0.5f) << 24
                                                               |
                                                               0xFFFFFFu),
                                                   "%s", txt);
    }
    return 1;
}

// the dodge-prompt attach point (body + 1.1 m up + 0.6 m to the screen's right) as 1280 x 720 px; 0 = not projectable
static int PaAnchor(uint64_t h, float *ox, float *oy, int clampit)
{
    typedef void *(*fn_proc)(uint64_t);
    typedef const float *(*fn_loc)(void *, float *);
    void *proc = ((fn_proc) T2R_SYM(kFnEntityFromHandle)) (h);
    if (!proc)
        return 0;
    float b[16];
    const float *l =
        ((fn_loc) ((uint8_t *) GetModuleHandleW(NULL) + 0x133db30)) ((uint8_t *) proc + 0x100, b);
    if (!l)
        return 0;
    float p0[4] = { l[0], l[1] + 1.1f, l[2], 1.0f }, p1[4] =
        { l[0], l[1] + 2.1f, l[2], 1.0f }, x0, y0, x1, y1;
    if (!TagProject(p0, &x0, &y0, NULL)
        || !TagProject(p1, &x1, &y1, NULL)) {
        if (!clampit)
            return 0;
        x0 = 640.0f;
        y0 = 300.0f;
        y1 = 260.0f;
    }
    float ppm = y0 - y1;
    if (ppm < 0)
        ppm = -ppm;
    if (ppm < 8.0f)
        ppm = 8.0f;
    if (ppm > 400.0f)
        ppm = 400.0f;
    float x = x0 + 0.6f * ppm, y = y0;
    if (clampit) {
        if (x < 60.0f)
            x = 60.0f;
        if (x > 1100.0f)
            x = 1100.0f;
        if (y < 80.0f)
            y = 80.0f;
        if (y > 640.0f)
            y = 640.0f;
    } else if (x < 20.0f || x > 1200.0f || y < 20.0f || y > 700.0f)
        return 0;
    *ox = x;
    *oy = y;
    return 1;
}

static void WorldPromptGreyDraw(void)
{
    if (PaOld()) {
        WorldPromptGreyDrawOld();
        return;
    }
    int act = g_wpg.h && g_wpg.frame && g_frame - g_wpg.frame <= 2;
    float x = 0, y = 0;
    if (act && !PaAnchor(g_wpg.h, &x, &y, 1))
        act = 0;
    int kind =
        g_wpg.icon == 1 ? PA_WAIT : g_wpg.icon == 2 ? PA_MASH : PA_TEXT;
    if (act)
        PaPathLog(kind ==
                    PA_WAIT ? "wait (key + contracting timer ring)" : kind
                    == PA_MASH ? "mash (key + press ring)" : g_wpg.label,
                    "self-draw (SP appear / fade)");
    PaFrame(0, act, x, y, kind, g_wpg.label, g_wpg.prog);
}

static void WorldPromptDraw(void)
{
    static int on = -1;
    if (on < 0) {
        FILE *f = ExeSideOpen("t2r-wp-selfdraw.txt", "r");
        on = f != NULL;
        if (f)
            fclose(f);
    }
    if (!on)
        return;
    if (PaOld()) {
        WorldPromptDrawOld();
        return;
    }
    int act = g_wpr.h && g_wpr.frame && g_frame - g_wpr.frame <= 2
        && g_frame - g_wp_ok_frame > 2;
    float x = 0, y = 0;
    if (act && !PaAnchor(g_wpr.h, &x, &y, 0))
        act = 0;
    if (act) {
        PaPathLog(g_wpr.label,
                    g_wpr.hold >=
                    0.0f ? "self-draw hold (key + border fill)" :
                    "self-draw press (SP appear / fade)");
        g_wpDrawnFrame = g_frame;
    }
    PaFrame(1, act, x, y, g_wpr.hold >= 0.0f ? PA_HOLD : PA_PRESS,
             g_wpr.label, g_wpr.hold);
}
