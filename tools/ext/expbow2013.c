// ---------------------------------------------------------------- EXPLOSIVE BOW = the 2013 Launcher, dressed as TLOU2's explosive bow
// (included from t2r-ext.c after explosives2013.c / placedbomb2013.c)
//
// WHAT IT IS. A purchasable (GEAR UP row 9): weapon id EXPBOW_ID "bow-explosive" = a clone of Ellie's bow (dc_data_patches.EXPBOW: the bow's model,
// overlays, draw, sounds, stow) whose default ammo list is [ammo-arrow-explosive] only - TLOU2's own explosive arrow (arrow-explosive actor, tracer
// loop sfx-bow-explo-arrow-projectile-lp, SP explosion *arrow-explosive-explosion* with its fx script, air distortion / shockwave, camera shake,
// physics impulse, sfx-bow-explo-arrow-explode). The regular bow keeps ammo-arrow. 2013 Launcher numbers (wiki + net10.bin): 3 LP, 375 parts,
// magazine 1, start 3, 3 arrows for 450, reserve cap 11 (net10 ammo record ammo-grenade-launcher {4, 11}).
//
// HOW THE BLAST HAPPENS (shooter). TLOU2 SP detonates an explosive arrow from the player script ss-explosive-arrow (script-callbacks.bin
// spawned-player-callback spawns it for the player): arrow-on-impact with ammo-arrow-explosive -> 0.2 s timer -> spawn-explosion(point,
// *arrow-explosive-explosion*, 0, 0, -1.0, weapon id, player, 0.0, 0) + broadcast-event, then it kills the arrow. We hook the native
// spawn-explosion (hook_rec8, the same record call scripts use) and, when that explosion is the arrow's, take the exact point of the SCRIPT's own call:
// the engine's own explosion (fx / shockwave / shake / impulse / sound) plays on the shooter's screen as in SP, and the point is
//   1. judged on our own body (ex13 EX13_LAUNCH: the Launcher's ring, 6 body probes, armour soak - a blast near the shooter hurts the shooter),
//   2. broadcast (MSG_EXPBOW_BURST 0x7B, [f32 pos 3][u8 0]) so every other screen bursts at the same point.
// OBSERVERS (expbow_recv). Like a nail bomb copy: a fresh throwable of the carrier weapon pipe-bomb-manny (whose bullet-settings
// thrown-pipe-bomb-bullet-manny the build points at *arrow-explosive-explosion*; nothing else in MP uses it) is put AT the point and exploded 2 frames
// later (rb_visual: owner 0, the engine's explosion fx + sound), and the blast is judged from that point by the victim's own screen (ex13_pend):
// every client decides its own damage, nothing damage-like is sent. No friendly fire (ex13_start), the shooter's own blast hurts the shooter.
//
// WHAT IS NOT DONE. The arrow in flight is the engine's own projectile on the SHOOTER's screen only (as for the regular bow: arrows are not
// replicated); other screens see the shot animation and the burst. Aim arc: bow_arc_tick draws it for the Explosive Bow too, from its own bullet settings.
// Logs: "expbow: ...". t2r-launcher-cur-hp.txt next to the exe = the nail-bomb rule (share of CURRENT health) instead of the wiki's two-shot rule.

#define MSG_EXPBOW_BURST 0x7B   // peer msg (ours): [f32 pos 3][u8 0] the sender's explosive arrow blew up there on its screen
#define EXPBOW_CARRIER "pipe-bomb-manny"        // weapon id of the observers' burst projectile (bullet-settings repointed by dc_data_patches.make_bullet_overlay)
#define EXPBOW_BASE_HP 100.0f   // the 2013 player has 100 hp: the ring multipliers x 0.55 / 0.30 apply to it (wiki: "downs an enemy in 2 shots": 55 + 55 >= 100)

static uint64_t ExpbowWeaponSid(void)
{
    static uint64_t w;
    if (!w)
        w = FNV64(EXPBOW_ID);
    return w;
}

// Damage base of a Launcher blast. The nail bomb / molotov rule decoded from the PS3 code is "ring multiplier x the target's CURRENT health"
// (rules/molotov-nailbomb-2013.md section 3, [code] + [inferred]); with the Launcher's 0.55 that needs 4 hits to down a full-health player, while the
// wiki says 2 shots ("Area of effect weapon that downs an enemy in 2 shots"). Default = the wiki (a share of the 100 hp MAXIMUM: 55 in the
// 3.25 m ring, 30 in the 5 m ring, x the probe ratio); t2r-launcher-cur-hp.txt = the code rule.
static float ExpbowDmgBase(float hp)
{
    static int cur;
    static uint64_t at;
    if (!at || GetTickCount64() - at > 2000) {
        FILE *f = ExeSideOpen("t2r-launcher-cur-hp.txt", "r");
        cur = f != NULL;
        if (f)
            fclose(f);
        at = GetTickCount64()? GetTickCount64() : 1;
    }
    return cur ? hp : EXPBOW_BASE_HP;
}

static fn_dccall g_ebOrig;
static struct {
    volatile int n;
    struct {
        float pos[3];
        uint64_t w;
    } e[4];
} g_ebPend;                    // script explosions of the arrow seen by the hook, handled in expbow_tick
static uint64_t g_eb_hook_calls, g_eb_arrow_calls;

static void ExpbowExplosionHook(uint64_t *ret, int argc, uint64_t *argv,
                                  uint64_t sec)
{
    static uint64_t arrow_ex;
    static int nl;
    if (!arrow_ex)
        arrow_ex = FNV64("*arrow-explosive-explosion*");
    g_eb_hook_calls++;
    if (argc >= 2 && argv && !IsBadReadPtr(argv, 16)) {
        int ours = argv[1] == arrow_ex;
        if (ours || nl < 4) {   // the first few explosions of anything (argument layout check) + every arrow one
            if (nl < 24) {
                nl++;
                Xlog("expbow: spawn-explosion argc %d: arg0 %llx settings %llx %s weapon %llx owner %llx", argc, (unsigned long long) argv[0], (unsigned long long) argv[1], ours ? "(= *arrow-explosive-explosion*)" : "(another explosion)", argc > 5 ? (unsigned long long) argv[5] : 0ULL, argc > 6 ? (unsigned long long) argv[6] : 0ULL);
            }
        }
        if (ours) {
            g_eb_arrow_calls++;
            const float *pp = (const float *) (uintptr_t) argv[0];
            if (argv[0] && !IsBadReadPtr(pp, 12)
                && __builtin_isfinite(pp[0] + pp[1] + pp[2])
                && __builtin_fabsf(pp[0]) < 1e6f
                && __builtin_fabsf(pp[1]) < 1e6f
                && __builtin_fabsf(pp[2]) < 1e6f) {
                int n = g_ebPend.n;
                if (n < 4) {
                    memcpy(g_ebPend.e[n].pos, pp, 12);
                    g_ebPend.e[n].w = argc > 5 ? argv[5] : 0;
                    g_ebPend.n = n + 1;
                }
            } else {
                static int nb;
                if (LOGCAP(nb, 6))
                    Xlog("expbow: the arrow's spawn-explosion has no readable point in arg0 (%llx) - not shared", (unsigned long long) argv[0]);
            }
        }
    }
    if (g_ebOrig)
        g_ebOrig(ret, argc, argv, sec);
}

static void ExpbowInstall(void)
{
    if (!ExpbowBuilt())
        return;
    g_ebOrig = HookRec8(FNV64("spawn-explosion"), ExpbowExplosionHook);
    Xlog("expbow: spawn-explosion native %s (the explosive arrow's SP script ss-explosive-arrow calls it at impact + 0.2 s)", g_ebOrig ? "hooked" : "NOT found - the Explosive Bow's blast cannot be shared");
}

// every frame: our own arrow's explosion -> judge it on us + tell the others; log what we hold
static void ExpbowTick(void)
{
    static uint64_t last_w;
    if (!ExpbowBuilt())
        return;
    if (MatchSubstate() != 6) {
        g_ebPend.n = 0;
        last_w = 0;
        return;
    }
    if (!(g_frame % 15)) {
        uint64_t w = CurrentWeapon();
        if (w != last_w) {
            last_w = w;
            if (w && w != ~0ULL && w == ExpbowWeaponSid())
                Xlog("expbow: Explosive Bow in hand (ammo-arrow-explosive %d, hook %s, %llu explosions seen so far, %llu of them the arrow's) - an arrow that lands must log " "'own explosive arrow blew up' within ~0.5 s", (int) InvQty(FNV64("ammo-arrow-explosive")), g_ebOrig ? "on" : "OFF", (unsigned long long) g_eb_hook_calls, (unsigned long long) g_eb_arrow_calls);
        }
    }
    int n = g_ebPend.n;
    g_ebPend.n = 0;
    uint32_t self = *(uint32_t *) T2R_SYM(kMySlot);
    for (int i = 0; i < n && i < 4; i++) {
        uint8_t m[13];
        memcpy(m, g_ebPend.e[i].pos, 12);
        m[12] = 0;
        if (g_sendTramp)
            g_sendTramp(MSG_EXPBOW_BURST, m, sizeof m, 0xff);
        Xlog("expbow: own explosive arrow blew up at (%.2f %.2f %.2f) (weapon %llx) - MSG_EXPBOW_BURST sent, judged on us as slot %u", g_ebPend.e[i].pos[0], g_ebPend.e[i].pos[1], g_ebPend.e[i].pos[2], (unsigned long long) g_ebPend.e[i].w, self);
        Ex13Pend(EX13_LAUNCH, (int) self, g_ebPend.e[i].pos);
    }
}

// another player's arrow blew up (MSG_EXPBOW_BURST from `s`): the burst fx + sound at the exact point, and the damage judged from it on our own body
static void ExpbowRecv(uint32_t s, const uint8_t *buf, uint64_t len)
{
    static struct {
        uint64_t ms;
        float at[3];
    } last[T2R_MAX_PEERS];
    if (!ExpbowBuilt() || s >= T2R_MAX_PEERS || !buf || len < 12
        || MatchSubstate() != 6)
        return;
    uint32_t self = *(uint32_t *) T2R_SYM(kMySlot);
    if (s == self)
        return;
    float at[3];
    memcpy(at, buf, 12);
    if (PtV3Reject(at, 1e6f, s))
        return;
    uint64_t now = GetTickCount64();
    // peer msg trust: the burst is judged on OUR body from this point - it comes from within an arrow's flight of its shooter, never faster than one per 0.4 s
    int far_ = PtOn()
        && !PtNearSender(s, at, 170.0f, 110.0f), soon = last[s].ms
        && now - last[s].ms < 400;
    if ((far_ && PtDeny(PTK_burst_near, s))
        || (soon && PtDeny(PTK_burst_rate, s))) {
        static uint64_t bad_ms;
        if (PtLogDue(&bad_ms, now))
            Xlog("expbow: MSG_EXPBOW_BURST from slot %u at (%.1f %.1f %.1f) dropped (too far from the shooter / too soon)", s, at[0], at[1], at[2]);
        return;
    }
    if (last[s].ms && now - last[s].ms < 600
        && __builtin_fabsf(last[s].at[0] - at[0]) +
        __builtin_fabsf(last[s].at[1] - at[1]) +
        __builtin_fabsf(last[s].at[2] - at[2]) < 0.5f) {
        Xlog("expbow: MSG_EXPBOW_BURST from slot %u at the same point %llu ms ago - duplicate dropped", s, (unsigned long long) (now - last[s].ms));
        return;
    }
    last[s].ms = now;
    memcpy(last[s].at, at, 12);
    Xlog("expbow: MSG_EXPBOW_BURST from slot %u - its explosive arrow blew up at (%.2f %.2f %.2f) -> burst (carrier %s) + damage judged from there", s, at[0], at[1], at[2], EXPBOW_CARRIER);
    g_ebBurstFrame = g_frame ? g_frame : 1;
    // the carrier copy is a REAL engine explosion - near us it killed us 4 frames after the 2013 ring's 55 (no damage hook sees it).
    // Within its reach of our body: no carrier copy (the ring judges the damage; camera shake / tinnitus / muffle still play); farther: the copy as before.
    {
        float pb[16];
        const float *me = PlayerLocator(pb);
        float d2 =
            me ? (me[0] - at[0]) * (me[0] - at[0]) + (me[1] -
                                                      at[1]) * (me[1] -
                                                                at[1]) +
            (me[2] - at[2]) * (me[2] - at[2]) : 1e9f;
        if (d2 > 9.0f * 9.0f)
            RbVisual(FNV64(EXPBOW_CARRIER), at);
        else
            Xlog("expbow: burst %.1f m from us - no carrier copy (it would hurt us; the 2013 ring is the damage)", __builtin_sqrtf(d2));
    }
    Ex13Pend(EX13_LAUNCH, (int) s, at);
}

// ---- OUR OWN DETONATION (arrows flew and never blew up - ss-explosive-arrow does not run in MP even with its module in the level).
// The engine's arrow update / impact function FUN_141198510 (rcx = the ProjectileArrow; its debug text "projectile-arrow-broke" / "-not-broken"
// is printed there at the arrow's locator +0x100) is hooked at its entry (eb_arrow_probe: the pointer into a ring). Arrows are not replicated, so
// every arrow on our screen is our own. After an Explosive Bow shot (its ammo count drops while it is held) the arrows seen are tracked by their
// locator; the first one that stops moving for 3 frames, or is no longer updated, is where it hit: spawn-explosion there with the SP explosive
// arrow's own settings (*arrow-explosive-explosion*: fx, shockwave, shake, impulse, sound) - the same call the SP script makes, so the hook above
// judges it on us and sends MSG_EXPBOW_BURST. One blast per shot. t2r-no-expbow-det.txt = off. Logs "expbow det: ...".
static volatile uint64_t g_ebRing[16];
static volatile LONG g_ebRingI;
static void EbArrowProbe(uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                           uint64_t *esp)
{
    (void) b;
    (void) c;
    (void) d;
    (void) esp;
    if (!a)
        return;
    LONG i = InterlockedIncrement(&g_ebRingI);
    g_ebRing[i & 15] = a;
}

static int g_ebDetOn;
static void ExpbowDetInstall(void)
{
    FILE *nf = ExeSideOpen("t2r-no-expbow-det.txt", "r");
    if (nf) {
        fclose(nf);
        Xlog("expbow det: OFF (t2r-no-expbow-det.txt)");
        return;
    }
    // mov rax,rsp; mov [rax+10],rbx; mov [rax+18],rsi; mov [rax+20],rdi; push rbp; push r12; push r13; push r14; push r15 (24 bytes, no rip-relative)
    static const uint8_t pro[24] =
        { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18,
0x48, 0x89, 0x78, 0x20, 0x55,
        0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57
    };
    g_ebDetOn =
        DecalDiagHook((uint8_t *) GetModuleHandleW(NULL) + 0x1198510, 24,
                        pro, (void *) EbArrowProbe,
                        "expbow det: arrow update FUN_141198510");
}

static const float *EbArrowLoc(uint64_t a, float *b16)
{
    typedef const float *(*fn_loc)(void *, float *);
    uint8_t *o = (uint8_t *) a;
    if (!o || IsBadReadPtr(o, 0x180))
        return NULL;
    const float *l =
        ((fn_loc) ((uint8_t *) GetModuleHandleW(NULL) + 0x133db30)) (o +
                                                                     0x100,
                                                                     b16);
    if (!l || !__builtin_isfinite(l[0] + l[1] + l[2])
        || __builtin_fabsf(l[0]) > 1e5f || __builtin_fabsf(l[1]) > 1e5f
        || __builtin_fabsf(l[2]) > 1e5f)
        return NULL;
    return l;
}

// our own detonation is a real engine explosion - on OUR screen it killed the other player's puppet outright (gored body) while they only
// went down on theirs (the victim's screen decides). Puppets within 12 m get the cannot-die threshold for 1 s around our explosion.
static uint64_t g_eb_guard_h[T2R_MAX_PEERS], g_eb_guard_until;
static void EbGuardPuppets(const float *p)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    for (uint32_t s = 0; s < T2R_MAX_PEERS; s++) {
        uint8_t *pe = ((fn_peer) T2rPeer) (s);
        uint64_t h = pe && pe[4] && !pe[5] ? *(uint64_t *) (pe + 0x60) : 0;
        if (!h)
            continue;
        float b[16];
        const float *l = PuppetLocator(h, b);
        if (!l
            || (l[0] - p[0]) * (l[0] - p[0]) + (l[1] - p[1]) * (l[1] -
                                                                p[1]) +
            (l[2] - p[2]) * (l[2] - p[2]) > 144.0f)
            continue;
        union {
            float f;
            uint32_t u;
        } one = { 1.0f };
        uint64_t th[2] = { h, one.u };
        DcCall(FNV64("character-set-invincibility-cannot-die-threshold"),
                2, th);
        g_eb_guard_h[s] = h;
    }
    g_eb_guard_until = g_frame + 60;
}

static void EbGuardTick(void)
{
    if (!g_eb_guard_until || g_frame < g_eb_guard_until)
        return;
    g_eb_guard_until = 0;
    for (uint32_t s = 0; s < T2R_MAX_PEERS; s++)
        if (g_eb_guard_h[s]) {
            uint8_t *pe = ((uint8_t * (*)(uint32_t)) T2rPeer) (s);
            if (pe && *(uint64_t *) (pe + 0x60) == g_eb_guard_h[s]
                && !g_peerDowned[s])
                PuppetGuardOff(g_eb_guard_h[s]);      // a downed puppet keeps the downed code's guard
            g_eb_guard_h[s] = 0;
        }
}

static void EbDetonate(const float *p, const char *why)
{
    EbGuardPuppets(p);
    static float pt[4] __attribute__((aligned(16)));
    memcpy(pt, p, 12);
    pt[3] = 1.0f;
    pt[1] += 0.35f;             // a heavy arrow ends IN the ground / grass - from that point every damage probe was blocked (0 of 6 at 2.8 m): burst 35 cm up
    float m1 = -1.0f;
    uint64_t fm1 = 0;
    memcpy(&fm1, &m1, 4);
    uint64_t a[9] =
        { (uint64_t) (uintptr_t) pt, FNV64("*arrow-explosive-explosion*"),
0, 0, fm1, ExpbowWeaponSid(), SID_PLAYER, 0, 0 };
    uint64_t r = DcCall(FNV64("spawn-explosion"), 9, a);
    Xlog("expbow det: arrow %s at (%.2f %.2f %.2f) -> spawn-explosion *arrow-explosive-explosion* -> %llx", why, p[0], p[1], p[2], (unsigned long long) r);
}

static void ExpbowDetTick(void)
{
    EbGuardTick();
    // the ammo count drops up to ~40 frames AFTER the arrow leaves, so a shot counter armed by it missed the first arrow
    // (it landed while nothing was armed) and then blew the NEXT arrow up on the shooter (armed, the fresh arrow sat at the bow). Now the arrow's
    // own flight is the shot: every arrow updated while the Explosive Bow is held (or up to 2 s after) is tracked; once it is
    // 3 m away from the shooter (where the shooter is now) it is armed, and an armed arrow blows up where it stops / sticks in a body / stops being updated.
    // The arrow on the string (moving with the shooter) never flies 3 m from the shooter, so it can never go off.
    static struct {
        uint64_t a;
        float p[3], vmax, from[3];
        int still, have, armed;
        uint64_t seen_f, born_f, loc_f;
    } tr[8];
    static uint64_t held_until, last_qty = ~0ULL;
    if (!g_ebDetOn)
        return;
    if (MatchSubstate() != 6) {
        memset(tr, 0, sizeof tr);
        held_until = 0;
        last_qty = ~0ULL;
        return;
    }
    uint64_t w = CurrentWeapon(), q =
        InvQty(FNV64("ammo-arrow-explosive"));
    if (w == ExpbowWeaponSid())
        held_until = g_frame + SecsFrames(2.0f);
    if (w == ExpbowWeaponSid() && last_qty != ~0ULL && q != ~0ULL
        && q < last_qty)
        Xlog("expbow det: Explosive Bow ammo %llu -> %llu",
             (unsigned long long) last_qty, (unsigned long long) q);
    last_qty = q;
    float pb[16];
    const float *me = PlayerLocator(pb);
    static LONG seen_i;
    LONG hi = g_ebRingI;
    if (hi - seen_i > 16)
        seen_i = hi - 16;
    for (; seen_i < hi;) {
        seen_i++;
        uint64_t a = g_ebRing[seen_i & 15];
        int k = -1, fr = -1;
        for (int i = 0; i < 8; i++) {
            if (tr[i].a == a)
                k = i;
            else if (!tr[i].a && fr < 0)
                fr = i;
        }
        if (k < 0) {
            if (g_frame > held_until || fr < 0 || !me)
                continue;
            k = fr;
            memset(&tr[k], 0, sizeof tr[k]);
            tr[k].a = a;
            tr[k].born_f = g_frame;
            memcpy(tr[k].from, me, 12);
            static int nb;
            if (LOGCAP(nb, 20))
                Xlog("expbow det: tracking arrow %p", (void *) a);
        }
        tr[k].seen_f = g_frame;
    }
    for (int i = 0; i < 8; i++) {
        if (!tr[i].a)
            continue;
        float lb[16];
        const float *l =
            g_frame - tr[i].seen_f <= 2 ? EbArrowLoc(tr[i].a, lb) : NULL;
        int gone = g_frame - tr[i].seen_f > 4;
        if (l) {
            float dx = l[0] - tr[i].p[0], dy = l[1] - tr[i].p[1], dz =
                l[2] - tr[i].p[2];
            float d2 = dx * dx + dy * dy + dz * dz;
            const float *o = me ? me : tr[i].from;      // from the shooter NOW (a shooter running with an arrow on the string never arms it)
            float fx = l[0] - o[0], fy = l[1] - o[1], fz = l[2] - o[2];
            if (!tr[i].armed && fx * fx + fy * fy + fz * fz > 9.0f) {
                tr[i].armed = 1;
                tr[i].still = 0;
            }
            if (tr[i].have && d2 < 0.0004f)
                tr[i].still++;
            else
                tr[i].still = 0;
            // a direct hit on a player: the arrow sticks in the body and keeps moving with it (never still, still updated) - the sudden drop
            // from flight speed (tens of m/s) to body speed counts as the hit (armed arrows only)
            if (tr[i].have && g_frame > tr[i].loc_f) {
                float v =
                    __builtin_sqrtf(d2) / (float) (g_frame - tr[i].loc_f);
                if (tr[i].armed) {
                    if (v > tr[i].vmax)
                        tr[i].vmax = v;
                    else if (tr[i].vmax > 0.3f && v < tr[i].vmax * 0.2f
                             && tr[i].still < 3)
                        tr[i].still = 3;
                }
            }
            memcpy(tr[i].p, l, 12);
            tr[i].have = 1;
            tr[i].loc_f = g_frame;
        }
        if (tr[i].armed && tr[i].have && (tr[i].still >= 3 || gone)) {
            EbDetonate(tr[i].p,
                        tr[i].still >= 3 ? (tr[i].vmax >
                                            0.3f ?
                                            "stopped (or stuck in a body)"
                                            : "stopped") :
                        "no longer updated");
            tr[i].a = 0;
        } else if (gone || g_frame - tr[i].born_f > SecsFrames(8.0f)) {
            if (tr[i].armed)
                Xlog("expbow det: armed arrow %p dropped without a landing (%s)", (void *) tr[i].a, gone ? "gone, no position" : "8 s");
            tr[i].a = 0;
        }
    }
}
