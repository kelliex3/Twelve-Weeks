// SCOPE GLINT: a less prominent version of the sniper glare of SP's Tommy sniper section, for anyone aiming through a scope. Included from t2r-ext.c (after flame2013.c). OFFLINE-WRITTEN: nothing here has run in the game; what is read from the
// game's files is marked READ, everything else INFERRED. Every log line starts "glint: ".
//
// The SP effect (READ, dc1/ss-savelev/sle-tommy-sniper-script-funcs.bin + ss-sle-tommy-sniper-approach-tommy-controller.bin, disassembled
// with tools/win/ext/dconstruct):
//   spawn-tommy-lens-flare(owner, flag) (called from sle-tommy-set-weapon-loadout every time Tommy gets a loadout, always flag 0 in the listed calls):
//     lens-flare-effect(owner) = spawn-particles-at-joint(sniper-scope-flare-1 [flag 1: -flare-2], npc-get-weapon-process-id(npc-tommy-fight-1),
//                                                         pump_mod, 0, 16, root, 2, <static ptr 0x1552>, 2, 0)
//     - i.e. a particle on the NPC's WEAPON process at the weapon skeleton's joint pump_mod (rifle-base has the joint: common.psarc actor97/rifle-base.pak)
//   ss-sle-tommy-sniper--draw-flare(tommy) (per frame, from the approach controller): if npc-on-screen?(tommy) set-effect-alpha(lens-flare-effect, 1.0) and
//     the screen-space "offscreen-effect" (spawn-particles-at-world-origin(sniper-scope-offscreen-flare-1), driven through set-effect-float posx/posy
//     from gui2-point-ss-from-ws(tommy's position)) to alpha 0; off screen the other way round (the flare is pinned to the screen edge).
//   Which Tommy rifle flare shows is gated by his aim logic (fact sle-tommy-saw-player); the flare is Tommy's "I see you" tell.
//   The particles come from the particle module part-rifle-tommy (pak68/part-rifle-tommy.pak, world-ellie-flashback-patrol.psarc; every
//   tsk-sle-tommy-sniper-* task of dc1/level-sets/level-sets-savelev.json lists the level "part-rifle-tommy"; its only SLE-specific particle shader
//   is general/particles/levels/save-lev/sle-offscreen-fake-lensflare). INFERRED: the group names sniper-scope-flare-1/-2 live in that module (the effect
//   names are not stored as plain strings or sid64 in the pak, so this could not be grepped; the module is the only one the sniper-fight tasks add).
//   MP mounts only core / common / sp-common: the pak is shipped (build_dll.stage_glint_part) and the MP level lists it (dc_data_patches.GLINT_LEVEL_ADDS).
//
// Here (the MP version, "less prominent"):
//  - the effect is the SP one: sniper-scope-flare-1 on the shooter's puppet weapon at pump_mod (fallback joints scope0, root: the pistol / crossbow
//    skeletons have no pump_mod, the spawn native's result is logged per joint tried), spawned ONCE per sniper slot when it starts scoping and killed
//    (kill-particles) when the sender stops, its alpha driven every frame with set-effect-alpha (the SP call) = max alpha (0.5) * facing * line of sight:
//      facing = how directly the gun's barrel (+z, as feature_weapon_pose_diag) points at the viewer's camera: 1 within 12 deg, 0 beyond 60 deg
//      (smoothstep), line of sight = a world ray gun <- camera (los_gate's world_ray_kick), blocked = fade to 0, faded in/out over ~0.25 s.
//  - never on the sniper's own screen (only peer puppets are touched).
//  - who: ANY gun with its scope on that is actually SCOPED IN (the engine's own state, READ below), teammates and enemies alike.
//      scoped = the aim component Player+0x2F68 byte +0x698 (exe 0x140aea270 sets it on the scope press / always-scope weapons; its vtable slot
//      0x138 = exe 0x140ae7000 is literally `movzx eax, byte [rcx+0x698]` and 0x1408469fd stores that into the player state +0x5e84 each frame).
//      The engine only scopes a weapon that has a scope (the hunting rifle / Military Sniper always-scope hook, the Vepr / El Diablo / crossbow scope
//      bundles, rifle-m14 / burst / frontier are blocked by scope_btn_hook): so "scoped" IS the weapon instance's scope state, no weapon list. The
//      native player-has-weapon-upgrade?(weapon, scope) is logged once per weapon next to it for the A/B (never a gate: bow / Vepr clones differ).
//  - gates: sender alive, not downed / dying, in a match that is not over, not in a paired move, scoped while aiming. Receiver: puppet alive, not downed.
// Peers (MSG_GLINT 0x91): [u8 on] on every edge, a keep-alive every 0.5 s while scoped, the stop sent 3x; the receiver expires it after 1.25 s.
// Opt-out: t2r-no-scope-glint.txt next to the exe (no message, no effect). Tunable: t2r-scope-glint.txt "max_alpha [cone_full_deg cone_zero_deg]".
#define MSG_GLINT 0x91          // peer msg (ours): [u8 1 the sender is scoped in / 0 stopped]
#define SGL_EFFECT "sniper-scope-flare-1"
static int g_sglOff = -1;
static float g_sglAlpha = 0.5f, g_sgl_full = 12.0f, g_sgl_zero = 60.0f;
static uint64_t g_sglCfgChk;
static int g_sgl_sending, g_sgl_offs;   // we are scoped / the stop sends still to do
static uint64_t g_sglSendT60;
#ifndef T2R_RELEASE
static uint64_t g_sglDevUntil;
static int g_sglDevOn;        // t2r-dev-glint.txt: the scoped state is forced until then
#endif
static struct {
    uint8_t on;
    uint64_t t60;               // MSG_GLINT state of that slot
    uint64_t fx, h, wp;         // the effect handle, the puppet handle and weapon process it was spawned for
    float cur;
    uint64_t ray, ray_frame, ray_next;  // smoothed alpha factor, the pending line-of-sight job
    uint8_t vis;
    uint64_t fail_until;
    char joint[12];
} g_sgl[T2R_MAX_PEERS];

static void SglCfg(void)
{
    if (g_sglOff >= 0 && g_frame - g_sglCfgChk < 120)
        return;
    g_sglCfgChk = g_frame ? g_frame : 1;
    FILE *f = ExeSideOpen("t2r-no-scope-glint.txt", "r");
    int off = f != NULL;
    if (f)
        fclose(f);
    if (off != g_sglOff)
        Xlog("glint: %s (t2r-no-scope-glint.txt)",
             off ? "OFF - no message, no effect" : "on");
    g_sglOff = off;
    f = ExeSideOpen("t2r-scope-glint.txt", "r");
    if (f) {
        float a, c0 = g_sgl_full, c1 = g_sgl_zero;
        int n = fscanf(f, "%f %f %f", &a, &c0, &c1);
        if (n >= 1 && a >= 0 && a <= 1 && c0 >= 0 && c1 > c0 && c1 <= 179) {
            if (a != g_sglAlpha || c0 != g_sgl_full || c1 != g_sgl_zero)
                Xlog("glint: t2r-scope-glint.txt - max alpha %.2f, full within %.0f deg, gone beyond %.0f deg", a, c0, c1);
            g_sglAlpha = a;
            g_sgl_full = c0;
            g_sgl_zero = c1;
        }
        fclose(f);
    }
}

// ---- sender: our own scoped state (aim component +0x698, READ above)
static int SglScoped(uint64_t *wid)
{
    typedef uint8_t *(*fn_me)(void);
    uint8_t *me = ((fn_me) T2R_SYM(kFnLocalPlayer)) ();
    if (!me || IsBadReadPtr(me + 0x2F68, 8))
        return 0;
    uint8_t *fa = *(uint8_t **) (me + 0x2F68);
    if (!fa || IsBadReadPtr(fa, 0x6a0) || fa[0x698] > 1)
        return 0;
    if (!fa[0x698])
        return 0;
    uint64_t none = 0, a = DcCall(FNV64("player-is-aiming?"), 0, &none);
    if (a == ~0ULL || !(a & 0xff))
        return 0;
    uint64_t w = CurrentWeapon();
    if (!w || w == ~0ULL)
        return 0;
    *wid = w;
    return 1;
}

static void SglSend(int on)
{
    if (!g_sendTramp)
        return;
    uint8_t m[1] = { (uint8_t) on };
    g_sendTramp(MSG_GLINT, m, 1, 0xff);
}

static void SglLogWeapon(uint64_t w)
{                               // once per weapon id: the scope-upgrade native next to the engine's scoped state (never a gate)
    static uint64_t seen[16];
    static int n;
    for (int i = 0; i < n; i++)
        if (seen[i] == w)
            return;
    if (n < 16)
        seen[n++] = w;
    uint64_t a[2] = { w, FNV64("scope") }, r =
        DcCall(FNV64("player-has-weapon-upgrade?"), 2, a);
    Xlog("glint: scoped in with weapon %llx (base %llx) - player-has-weapon-upgrade?(.., scope) = %lld", (unsigned long long) w, (unsigned long long) BaseWeapon(w), (long long) (r == ~0ULL ? -1 : (int64_t) (r & 0xff)));
}

static void SglRecv(uint32_t from, const uint8_t *buf, uint64_t len)
{
    if (from >= T2R_MAX_PEERS || !buf || len < 1)
        return;
    static int n;
    if (g_sgl[from].on != (buf[0] != 0) && n++ < 40)
        Xlog("glint: slot %u %s", from,
             buf[0] ? "scoped in" : "scope down");
    g_sgl[from].on = buf[0] != 0;
    g_sgl[from].t60 = g_t60 ? g_t60 : 1;
}

// ---- receiver
static void SglKill(uint32_t sl, const char *why)
{
    if (!g_sgl[sl].fx)
        return;
    uint64_t a[2] = { g_sgl[sl].fx, 0 };
    DcCall(FNV64("kill-particles"), 2, a);
    static int n;
    if (LOGCAP(n, 40))
        Xlog("glint: slot %u effect %llx removed (%s)", sl,
             (unsigned long long) g_sgl[sl].fx, why);
    g_sgl[sl].fx = 0;
    g_sgl[sl].cur = 0;
    g_sgl[sl].ray = 0;
}

static void SglSetAlpha(uint64_t fx, float al)
{
    uint64_t a[2] = { fx, 0 };
    memcpy(&a[1], &al, 4);
    DcCall(FNV64("set-effect-alpha"), 2, a);
}

static uint64_t SglSpawn(uint32_t sl, uint64_t wp)
{
    static const char *const JOINTS[3] = { "pump_mod", "scope0", "root" };      // pump_mod = the SP joint; the others for skeletons without it
    for (int j = 0; j < 3; j++) {
        uint64_t a[10] =
            { FNV64(SGL_EFFECT), wp, FNV64(JOINTS[j]), 0, 16,
   FNV64("root"), 2, 0, 2, 0 };
        uint64_t r = DcCall(FNV64("spawn-particles-at-joint"), 10, a);
        if (!r || r == ~0ULL) { // the plain flags the other fx spawns use
            a[4] = 2;
            r = DcCall(FNV64("spawn-particles-at-joint"), 10, a);
        }
        static int n;
        if (LOGCAP(n, 30))
            Xlog("glint: spawn %s at %s on slot %u's weapon process %llx -> %llx", SGL_EFFECT, JOINTS[j], sl, (unsigned long long) wp, (unsigned long long) r);
        if (r && r != ~0ULL) {
            snprintf(g_sgl[sl].joint, sizeof g_sgl[sl].joint, "%s",
                     JOINTS[j]);
            return r;
        }
    }
    return 0;
}

static int SglCam(float c[3])
{
    uint64_t none = 0, cam =
        DcCall(0x42413e1c037969a8ULL /* camera-through-reticle-locator */
                , 0, &none);
    uint64_t cp = cam
        && cam != ~0ULL ? DcCall(FNV64("get-locator-position"), 1,
                                  &cam) : 0;
    if (!cp || cp == ~0ULL || IsBadReadPtr((void *) cp, 12))
        return 0;
    memcpy(c, (void *) cp, 12);
    return c[0] == c[0] && c[1] == c[1] && c[2] == c[2];
}

static float SglSmooth(float x)
{
    x = x < 0 ? 0 : x > 1 ? 1 : x;
    return x * x * (3.0f - 2.0f * x);
}

static void SglFxTick(void)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    typedef uint8_t *(*fn_proc)(uint64_t);
    uint32_t me = *(uint32_t *) T2R_SYM(kMySlot);
    float cam[3];
    int have_cam = -1;
    for (uint32_t sl = 0; sl < T2R_MAX_PEERS; sl++) {
        if (sl == me)
            continue;           // never on our own screen
        uint8_t *peer = ((fn_peer) T2rPeer) (sl);
        uint64_t h = peer ? *(uint64_t *) (peer + 0x60) : 0;
        int act = g_sgl[sl].on && g_t60 - g_sgl[sl].t60 <= 75;
        if (g_sgl[sl].on && !act)
            g_sgl[sl].on = 0;
        if (!act || !h || !peer[4] || peer[5] || g_peerDowned[sl]
            || g_puppetDeadHere[sl] || g_graceDead[sl]
            || g_t60 < g_graceUntil[sl]) {
            if (g_sgl[sl].fx)
                SglKill(sl,
                         act ? "puppet gone / dead / downed" :
                         "scope down");
            continue;
        }
        uint8_t *chr = ((fn_proc) T2R_SYM(kFnEntityFromHandle)) (h);
        uint64_t a1 = h, wp =
            DcCall(FNV64("npc-get-weapon-process-id"), 1, &a1);
        if (!chr || !wp || wp == ~0ULL)
            continue;
        if (g_sgl[sl].fx && (g_sgl[sl].h != h || g_sgl[sl].wp != wp))
            SglKill(sl, "new puppet / weapon");
        // facing: the barrel (+z) against the direction to the viewer's camera
        uint8_t *w = HeldWeapon(chr);
        float t[8], tgt = 0.0f;
        if (have_cam < 0)
            have_cam = SglCam(cam);
        if (w && have_cam && ProcWorld(w, t)) {
            float f[3], zf[3] = { 0, 0, 1 };
            Qrotv(t + 4, zf, f);
            float v[3] = { cam[0] - t[0], cam[1] - t[1], cam[2] - t[2] };
            float d =
                __builtin_sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (d > 1.5f) {
                float c = (f[0] * v[0] + f[1] * v[1] + f[2] * v[2]) / d;
                float c_full =
                    __builtin_cosf(g_sgl_full * 0.01745329f), c_zero =
                    __builtin_cosf(g_sgl_zero * 0.01745329f);
                tgt = SglSmooth((c - c_zero) / (c_full - c_zero));
                // line of sight: ray camera -> gun, kicked on one frame, read on a later one (as flame_scan)
                if (tgt > 0.01f) {
                    if (!g_sgl[sl].ray) {
                        if (g_frame >= g_sgl[sl].ray_next
                            && g_sgl[sl].ray_frame != g_frame) {
                            g_sgl[sl].ray = WorldRayKick(cam, t);
                            g_sgl[sl].ray_frame = g_frame;
                            g_sgl[sl].ray_next = g_frame + 8;
                            if (!g_sgl[sl].ray)
                                g_sgl[sl].vis = 1;      // (no job = the ray service refused: counts as clear)
                        }
                    } else if (g_sgl[sl].ray_frame != g_frame) {
                        float rt = RayT(g_sgl[sl].ray);
                        g_sgl[sl].ray = 0;
                        int vis = !(rt >= 0.0f && rt < 0.95f);
                        static int nb;
                        if (vis != g_sgl[sl].vis && nb++ < 30)
                            Xlog("glint: slot %u %s (wall at %.0f%%)", sl,
                                 vis ? "in the open" : "behind a wall",
                                 rt * 100.0f);
                        g_sgl[sl].vis = (uint8_t) vis;
                    }
                    if (!g_sgl[sl].vis)
                        tgt = 0.0f;
                } else
                    g_sgl[sl].vis = 1;
            }
        }
        g_sgl[sl].cur += (tgt - g_sgl[sl].cur) * 0.25f;
        if (g_sgl[sl].cur < 0.004f)
            g_sgl[sl].cur = 0.0f;
        if (!g_sgl[sl].fx) {
            if (tgt < 0.03f || g_t60 < g_sgl[sl].fail_until)
                continue;
            uint64_t r = SglSpawn(sl, wp);
            if (!r) {
                g_sgl[sl].fail_until = g_t60 + 180;
                static int nf;
                if (LOGCAP(nf, 6))
                    Xlog("glint: spawn failed on every joint - is part-rifle-tommy (pak68) loaded in this build?");
                continue;
            }
            g_sgl[sl].fx = r;
            g_sgl[sl].h = h;
            g_sgl[sl].wp = wp;
            SglSetAlpha(r, 0.0f);
        }
        static float last[T2R_MAX_PEERS];
        float al = g_sgl[sl].cur * g_sglAlpha;
        if (__builtin_fabsf(al - last[sl]) > 0.01f
            || (al == 0.0f && last[sl] != 0.0f)) {
            SglSetAlpha(g_sgl[sl].fx, al);
            last[sl] = al;
            static uint64_t lg[T2R_MAX_PEERS];
            if (g_t60 - lg[sl] > 60) {
                lg[sl] = g_t60;
                static int nl;
                if (LOGCAP(nl, 60))
                    Xlog("glint: slot %u alpha %.2f (facing target %.2f, joint %s)", sl, al, tgt, g_sgl[sl].joint);
            }
        }
    }
}

static void SglTick(void)
{
    SglCfg();
    if (MatchSubstate() != 6) {
        if (g_sgl_sending) {
            g_sgl_sending = 0;
            Xlog("glint: left the match");
        }
        for (uint32_t sl = 0; sl < T2R_MAX_PEERS; sl++) {
            if (g_sgl[sl].fx)
                SglKill(sl, "left the match");
            memset(&g_sgl[sl], 0, sizeof g_sgl[sl]);
        }
        return;
    }
    if (g_sglOff) {
        if (g_sgl_sending) {
            g_sgl_sending = 0;
            SglSend(0);
        }
        for (uint32_t sl = 0; sl < T2R_MAX_PEERS; sl++)
            if (g_sgl[sl].fx)
                SglKill(sl, "t2r-no-scope-glint.txt");
        return;
    }
    SglFxTick();
    uint64_t w = 0;
    int on = 0;
    if (!g_downed && !g_downDying && !g_ffEnded && !PsyncActive()
        && LocalHp() > 0 && !(g_frame & 1))
        on = SglScoped(&w);
    else if (g_frame & 1)
        on = g_sgl_sending;     // (the scoped byte is read every 2nd frame; in between the state holds)
#ifndef T2R_RELEASE
    if (g_sglDevUntil && GetTickCount64() < g_sglDevUntil)
        on = g_sglDevOn;
    else
        g_sglDevUntil = 0;
#endif
    if (on != g_sgl_sending) {
        g_sgl_sending = on;
        if (on)
            SglLogWeapon(w);
        Xlog("glint: our scope %s",
             on ? "IN - telling the others" : "out");
        SglSend(on);
        g_sglSendT60 = g_t60;
        g_sgl_offs = on ? 0 : 2;
    } else if (on && g_t60 - g_sglSendT60 >= 30) {
        SglSend(1);
        g_sglSendT60 = g_t60;
    }                           // keep-alive (the receiver expires it after 1.25 s)
    else if (!on && g_sgl_offs > 0 && g_t60 - g_sglSendT60 >= 4) {
        g_sgl_offs--;
        g_sglSendT60 = g_t60;
        SglSend(0);
    }                           // the stop 3x (a lossy relay)
}
