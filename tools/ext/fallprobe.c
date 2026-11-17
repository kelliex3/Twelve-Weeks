// fallprobe.c - dev test log for the engine's lethal-fall decision (Bookstore: vaulting a 2nd-floor rail and dropping ~5 m kills the player).
//
// Static finding (re/ghidra of exe+b56500, the "fall-die" state of the player fall controller, vtable at exe+2ca79e0 beside fall-to-ground-die exe+b55d40):
// exe+b56500(this, float f) -> bool. Depth d = 12.0 (or the y of a per-player setting vec3, key 0x7e74b1b04f4cae63). With f == 0 it casts a ray from the player
// (this+0x118 -> +0x100 position) straight down d metres (cast filter {0x61da06000e, 0x1000000000400}); no hit: ground = y - d; hit: ground = hit.y + 0.5.
// Result true (the fall kills: 'fall-die' / 'fall-to-ground-die') when  d <= y - ground,  i.e. NO hit within d, or the first ground is >= d + 0.5 m below.
// So a floor the probe cannot hit (Part I collision whose shape layer / material bytes are not in that cast filter) kills at ANY height, mid-fall.
//
// Hook: exe+b56500 entry. Logs 'fallprobe:' lines: result, f, player y, the highest y of the last 3 s (fall height so far), return address. Result 1 while the
// player is only a few metres above a floor = the probe misses the floor. Off unless t2r-fallprobe.txt exists next to the exe (polled); not in T2R_RELEASE.
// Reading: true at y~5 over the ground floor y -1 -> probe blind to the Part I floor (fix = shape layer / material record of that collision item);
// false all the way down but the player still dies on landing -> look at the landing records (player-settings *player-ground-landing-settings*).
#ifndef T2R_FALLPROBE_C
#define T2R_FALLPROBE_C

// ---- the lethal predicted-drop threshold (all builds) ----
// Bookstore atrium rail: the kill depends on the distance to the ground (half stairs are fine, a bigger drop kills mid air). The player fall
// controller (exe+b8acb0, fall-to-ground-die entry exe+b55d40 called from exe+b8bb19) holds a ground-probe result at this+0x350 {pos, normal, distance at +0x370;
// -1 = none}. It enters fall-to-ground-die (the death) in mid-air when  FALL_DIE_MIN < distance < 16.0  and the ground normal y > 0.2 and the fall speed is
// below -6 m/s. FALL_DIE_MIN is the stock 6.0 m (a code constant at exe+2b0a788 shared with other code: the compare at exe+b8ba37 is repointed at our own float,
// the constant itself is not touched). The probe starts at the player, so a vault over a 1 m rail from a floor 5.4 m above the ground already reads ~7.2 m at the
// top of the arc. Stock drops of 6.0..FALL_DIE_MIN m now survive (with the hard landing: tools/features/dc_data_patches.py patch_player_landing, same bound).
#define FALL_DIE_MIN 8.0f
#define FALL_LETHAL_M 8.0f      // the +0x278 fall-height compare (exe+b876d3); landing tier covers it (8.5 m)
#define FALL_DIE_SPEED 14.0f    // = the hard-landing tier speed bound (dc_data_patches.LAND_MAX_V)
static float *g_fhCave;

static void FhInstall(void)
{
    
 {
        FILE *sw = ExeSideOpen("t2r-no-fall-fix.txt", "r");
        if (sw) {
            fclose(sw);
            Xlog("fall fix: off (t2r-no-fall-fix.txt) - stock fall kill thresholds");
            return;
        }
    }
    
uint8_t * exe = (uint8_t *) GetModuleHandleW(NULL), *site =
        exe + 0xb8ba37;
    
static const uint8_t expect[8] = { 0xc5, 0xf8, 0x2f, 0x0d, 0x49, 0xed, 0xf7, 0x01 };       // vcomiss xmm1, [rip + 0x1f7ed49]  (-> exe+2b0a788 = 6.0)
    if (memcmp(site, expect, sizeof expect)) {
        Xlog("HOOK FAIL fall-die distance: exe+b8ba37 differs, not patched");
        return;
    }
    
if (*(const float *) (exe + 0x2b0a788) != 6.0f) {
        Xlog("HOOK FAIL fall-die distance: constant at exe+2b0a788 is not 6.0");
        return;
    }
    
uint8_t * c = AllocNear(exe + 0xb80000, 0x1000);
    
if (!c) {
        Xlog("fall-die distance: no memory near the exe");
        return;
    }
    
g_fhCave = (float *) c;
    *g_fhCave = FALL_DIE_MIN;
    
int64_t rel = (int64_t) (c - (site + 8));
    
if (rel > 0x7fffffffLL || rel < -0x80000000LL) {
        Xlog("fall-die distance: cave out of rip range");
        return;
    }
    
DWORD old;
    VirtualProtect(site, 8, PAGE_EXECUTE_READWRITE, &old);
    
*(int32_t *) (site + 4) = (int32_t) rel;
    
VirtualProtect(site, 8, old, &old);
    
FlushInstructionCache(GetCurrentProcess(), site, 8);
    
Xlog
        ("fall-die distance: the predicted-drop kill threshold is %.1f m (stock 6.0)",
         FALL_DIE_MIN);
    
        // second kill path of the same function: exe+b8bb8c  vmovups xmm0,[rip+..] (-> exe+2c06480 = -10.0 m/s x4) then vucomiss -> when the fall speed is below it
        // (and the first band did not catch it) the controller calls machine->vf88 (exe+b56500, the fall-die probe) and, when that returns 0, machine->vf38(1,0) +
        // the holster + state request 0x701: the death seen at vy -10.8 after a ~5 m free fall (probe distance 6.35 m set at the fall start, ground contact
        // one frame later, dead the frame after).  Repointed at our own -FALL_DIE_SPEED x4 (the landing tier takes everything up to 14 m/s).
        uint8_t * site2 = exe + 0xb8bb8c;
    
static const uint8_t expect2[8] = { 0xc5, 0xf8, 0x10, 0x05, 0xec, 0xa8, 0x07, 0x02 };      // vmovups xmm0, [rip + 0x207a8ec]
    const float *cst = (const float *) (exe + 0x2c06480);
    
if (memcmp(site2, expect2, sizeof expect2) || cst[0] != -10.0f
         || cst[3] != -10.0f) {
        Xlog("HOOK FAIL fall-die speed: exe+b8bb8c differs, not patched");
        return;
    }
    
float *vec = g_fhCave + 4;        // the cave is 0x1000 B, the first float is the distance
    for (int i = 0; i < 4; i++)
        vec[i] = -FALL_DIE_SPEED;
    
int64_t rel2 = (int64_t) ((uint8_t *) vec - (site2 + 8));
    
if (rel2 > 0x7fffffffLL || rel2 < -0x80000000LL) {
        Xlog("fall-die speed: cave out of rip range");
        return;
    }
    
VirtualProtect(site2, 8, PAGE_EXECUTE_READWRITE, &old);
    
*(int32_t *) (site2 + 4) = (int32_t) rel2;
    
VirtualProtect(site2, 8, old, &old);
    
FlushInstructionCache(GetCurrentProcess(), site2, 8);
    
Xlog
        ("fall-die speed: the fast-fall kill threshold is %.1f m/s (stock 10.0)",
         FALL_DIE_SPEED);
    
        // third kill path, the one that remained (3.7 m and 4.5 m drops survive, 5.35 m dies on the landing frame with no landing record chosen):
        // the movement component's query +0x278 (exe+b87620) = (player+0x2e40 position - player+0x100 position) . up > G, G = the global float at exe+4103e9c (~5 m;
        // shared by ~25 functions, so NOT touched).  The compare at exe+b876d3  vcomiss xmm2,[rip+..]  is repointed at our own FALL_LETHAL_M.  The control update
        // (exe+b89360) tests it (with q280 / q3b0) when the fall ends and issues net-kill-fall (exe+b8a076).
        uint8_t * site3 = exe + 0xb876d3;
    
static const uint8_t expect3[8] = { 0xc5, 0xf8, 0x2f, 0x15, 0xc1, 0xc7, 0x57, 0x03 };      // vcomiss xmm2, [rip + 0x357c7c1]  (-> exe+4103e9c = G)
    if (memcmp(site3, expect3, sizeof expect3)) {
        Xlog("HOOK FAIL fall-lethal height: exe+b876d3 differs, not patched");
        return;
    }
    
g_fhCave[8] = FALL_LETHAL_M;
    
int64_t rel3 = (int64_t) ((uint8_t *) & g_fhCave[8] - (site3 + 8));
    
if (rel3 > 0x7fffffffLL || rel3 < -0x80000000LL) {
        Xlog("fall-lethal height: cave out of rip range");
        return;
    }
    
VirtualProtect(site3, 8, PAGE_EXECUTE_READWRITE, &old);
    
*(int32_t *) (site3 + 4) = (int32_t) rel3;
    
VirtualProtect(site3, 8, old, &old);
    
FlushInstructionCache(GetCurrentProcess(), site3, 8);
    
Xlog
        ("fall-lethal height: the +0x278 fall-height query now compares against %.1f m (stock G = float at exe+4103e9c, ~5 m)",
         FALL_LETHAL_M);
    
        // (Bookstore atrium, 6.36 m drop, dev tp to y 5.4): still died - q280 and q3b0 flipped 1.2 m into the fall, i.e. the readers of G itself saw 5.9 (the game rewrites
        // G; our per-frame set races it): q280 (exe+b876f0, threshold 1.0 or 2.5 + max(0, G - 5.9)) and the fall controller's latch (exe+b8b26a: probe distance vs G -> vf3b8 sets
        // the +0x5e0 flag = q3b0).  Both G reads are repointed at our own 8.0 (same cave trick), independent of what the game does to G.  t2r-no-fall-sites.txt = off (bisect).
    {
        FILE *sw = ExeSideOpen("t2r-no-fall-sites.txt", "r");
        if (sw) {
            fclose(sw);
            Xlog("fall G sites: off (t2r-no-fall-sites.txt)");
            return;
        }
    }
    
static const struct {
        uint32_t off;
        uint8_t b[4];
        const char *what;
    } gs[] = { 
            { 0xb8779a, { 0xc5, 0xfa, 0x10, 0x05},
             "q280 (exe+b876f0) lethal height" }, 
{ 0xb8b26a, { 0xc5,
                                                                0xfa, 0x10,
                                                                0x05},
                                                    "fall controller lethal latch (exe+b8b26a)"
                                                    }, 
{ 0xe07037, { 0xc5,
                                                                     0x78,
                                                                     0x2f,
                                                                     0x25},
                                                         "landing function (exe+e06c90): fall height >= G skips the landing record chooser call (exe+e07183)"
                                                         }, 
{ 0xe07ae9,
                                                              { 0xc5, 0xf8,
                                                               0x2f, 0x15},
                                                              "landing function 2 (exe+e07780): fall height > G flag"
                                                              },
        
{ 0xb87604, { 0xc5, 0xf8, 0x2f, 0x15},
          "fall height > G query (exe+b87604)" }
    };
    
g_fhCave[9] = FALL_LETHAL_M;
    
for (int k = 0; k < (int) (sizeof gs / sizeof gs[0]); k++) {
        
uint8_t * s = exe + gs[k].off;
        
int32_t dd = *(int32_t *) (s + 4);
        
if (memcmp(s, gs[k].b, 4)
             || (int64_t) (s + 8) + dd != (int64_t) (exe + 0x4103e9c)) {
            Xlog("HOOK FAIL fall G site %s: exe+%x differs, not patched",
                 gs[k].what, gs[k].off);
            continue;
        }
        
int64_t r = (int64_t) ((uint8_t *) & g_fhCave[9] - (s + 8));
        
if (r > 0x7fffffffLL || r < -0x80000000LL) {
            Xlog("fall G site: cave out of rip range");
            continue;
        }
        
VirtualProtect(s, 8, PAGE_EXECUTE_READWRITE, &old);
        
*(int32_t *) (s + 4) = (int32_t) r;
        
VirtualProtect(s, 8, old, &old);
        
FlushInstructionCache(GetCurrentProcess(), s, 8);
        
Xlog
            ("fall G site: %s now reads %.1f m from our cave (stock: the global G, 5.9)",
             gs[k].what, FALL_LETHAL_M);
    
}

}


 
// G (exe+4103e9c) = 5.9 at runtime = the MP lethal fall height. The staircase death latches its +0x5e0 'lethal' flag at fall start (q3b0, writer not found)
// while our +0x278 repoint held, so the value itself is raised once the game has set it: every reader (fall intro choice, the latch, NPC falls) sees one consistent
// lethal height. Off with t2r-no-fall-fix.txt or t2r-no-fall-g.txt.
static void FhGTick(void)
{
    
static int st;             // 0 = not checked, 1 = watching, 2 = done / off
    if (st == 2)
        return;
    
if (!st) {
        FILE *a = ExeSideOpen("t2r-no-fall-fix.txt", "r"), *b =
            a ? NULL : ExeSideOpen("t2r-no-fall-g.txt", "r");
        
if (a || b) {
            if (a)
                fclose(a);
            if (b)
                fclose(b);
            st = 2;
            Xlog("fall lethal G: off (switch file) - stock 5.9 m");
            return;
        }
        st = 1;
    }
    
float *g = (float *) ((uint8_t *) GetModuleHandleW(NULL) + 0x4103e9c);
    
        // The game writes 5.9 again after our frame-1 set (runtime globals line read 5.900 at the first fall) -> watched every frame, re-set each time
        if (*g > 5.85f && *g < 5.95f) {
        static int nset;
        *g = FALL_LETHAL_M;
        if (nset < 8) {
            nset++;
            Xlog("fall lethal G: exe+4103e9c 5.9 -> %.1f m (the MP lethal fall height, all readers; set #%d)", FALL_LETHAL_M, nset);
        }
    }

}


#ifdef T2R_RELEASE
static void FpInstall(void)
{
    FhInstall();
} 
#else   /* 
 */
static uint8_t(*g_fp_tramp) (void *self, float f);

static int FpOn(void)
{
    
static uint64_t at;
    static int v;
    return FeelFlag("t2r-fallprobe.txt", &at, &v, 60);

}


static uint64_t FpOff(const void *p)
{
    return (uint64_t) (uintptr_t) p -
        (uint64_t) (uintptr_t) GetModuleHandleW(NULL);
}


static void FpHpHookInstall(void *player);

static uint8_t FpHook(void *self, float f)
{
    
uint8_t r = g_fp_tramp(self, f);
    
if (!FpOn())
        return r;
    
FpHpHookInstall(self ? *(void **) ((uint8_t *) self + 0x118) :
                        NULL);
    
static uint64_t last;
    static int nres;
    
uint64_t now = GetTickCount64();
    
float b[16];
    const float *pl = PlayerLocator(b);
    
if (now >= last + 150 || (r && nres < 40)) {
        
last = now;
        if (r)
            nres++;
        
int g_ = g_xlogNolimit;
        g_xlogNolimit = 1;
        
Xlog
            ("fallprobe: die-probe result %d f %.3f | player %.2f %.2f %.2f | state %p ret exe+%llx",
             r, f, pl ? pl[0] : 0.0f, pl ? pl[1] : 0.0f, pl ? pl[2] : 0.0f,
             self,
             
(unsigned long long) FpOff(__builtin_return_address(0)));
        
g_xlogNolimit = g_;
    
}
    
return r;

}


// exe+e0abd0 (landing record chooser, once per landing from exe+e08780): (player, vertical speed, horizontal speed, fall height, list sid) -> the record
// (player-settings.bin *player-ground-landing-settings*: +8 min vertical speed, +c max horizontal speed, +10 max height, +50 damage flag, +54 max damage,
// +58 health kept, +5c special-mode flag). The args ARE the engine's own fall height / speed (replaces the old unreliable 'fell' estimate).
static uint8_t *(*g_fpc_tramp)(void *self, float v, float hs, float h,
                               uint64_t sid);

static uint8_t *FpcHook(void *self, float v, float hs, float h,
                          uint64_t sid)
{
    
uint8_t * r = g_fpc_tramp(self, v, hs, h, sid);
    
if (!FpOn())
        return r;
    
float b[16];
    const float *pl = PlayerLocator(b);
    
int g_ = g_xlogNolimit;
    g_xlogNolimit = 1;
    
if (r && !IsBadReadPtr(r, 0x60)) {
        
const float *f = (const float *) r;
        
Xlog
            ("fallprobe: landing v %.2f hs %.2f height %.2f (list %llx) | player %.2f %.2f %.2f -> record %p: v>%.2f hs<%.2f h<%.2f flag50 %u maxdmg %.1f keep %.1f flag5c %u",
             
v, hs, h, (unsigned long long) sid, pl ? pl[0] : 0.0f,
             pl ? pl[1] : 0.0f, pl ? pl[2] : 0.0f, (void *) r, f[2], f[3],
             f[4], r[0x50], f[0x15], f[0x16], r[0x5c]);
    
} else
        Xlog("fallprobe: landing v %.2f hs %.2f height %.2f (list %llx) -> no record", v, hs, h, (unsigned long long) sid);
    
g_xlogNolimit = g_;
    
return r;

}


 
// ---- controller probe: exe+b8acb0 (the fall controller whose this+0x350 {ground pos, normal(+0x360), distance(+0x370), valid byte +0x380} the kill compare reads) ----
// A register-preserving asm stub (the function takes xmm args we do not know) calls fp_ctl_log(this), then jumps to the trampoline.
    
// ---- evidence for "no predicted ground" (atrium: probe pos = FLT_MAX, the fall state never ends on the floor): our own downward world rays (shot mask 0x1b07 and static
// 0x207, 16 m) kicked at the first fall frame and again when q258 drops to 0 (landed); results are read ~60 ms later ----
    static struct {
    uint64_t j[2];
    uint64_t at;
    float p[3];
    const char *tag;
} g_fpr[2];

static void FpRayKick(int slot, const char *tag, const float *pl,
                         float up, float len)
{
    
float a[3] = { pl[0], pl[1] + up, pl[2] }, b[3] =
        { pl[0], pl[1] + up - len, pl[2] };
    
g_fpr[slot].j[0] = WorldRayKickM(a, b, 0, 0x1b07);
    g_fpr[slot].j[1] = WorldRayKickM(a, b, 0, 0x207);
    
g_fpr[slot].at = GetTickCount64();
    g_fpr[slot].tag = tag;
    memcpy(g_fpr[slot].p, pl, 12);
    
g_fpr[slot].p[1] = pl[1] + up;
    g_fpr[slot].p[2] = len;     // (p[2] reused below for the length: x kept in p[0])
} 
static void FpRayRead(void)
{
    
for (int k = 0; k < 2; k++) {
        
if (!g_fpr[k].at || GetTickCount64() < g_fpr[k].at + 60)
            continue;
        
float t0 = g_fpr[k].j[0] ? RayT(g_fpr[k].j[0]) : -2.0f, t1 =
            g_fpr[k].j[1] ? RayT(g_fpr[k].j[1]) : -2.0f, len =
            g_fpr[k].p[2];
        
Xlog
            ("fallprobe: ray %s from y %.2f down %.0f m: shot-mask hit %s %.2f m, static-mask hit %s %.2f m",
             g_fpr[k].tag, g_fpr[k].p[1], len, 
t0 >= 0 ? "at" : "NONE",
             t0 >= 0 ? t0 * len : 0.0f, t1 >= 0 ? "at" : "NONE",
             t1 >= 0 ? t1 * len : 0.0f);
        
g_fpr[k].at = 0;

} 
} 
static volatile uint64_t g_fpFallMs;


void *g_fp_ctl_tramp;

void __attribute__((used)) fp_ctl_log(const uint8_t *c)
{
    
static uint64_t last, lt;
    static float ly;
    static int n;
    
if (!c || !FpOn())
        return;
    
uint64_t now = GetTickCount64();
    
float b[16];
    const float *pl = PlayerLocator(b);
    
if (!pl)
        return;
    
float vy = lt
        && now > lt ? (pl[1] - ly) * 1000.0f / (float) (now - lt) : 0.0f;
    
ly = pl[1];
    lt = now;
    
if (vy > -3.0f || now < last + 33 || IsBadReadPtr(c, 0x400))
        return;
    
last = now;
    g_fpFallMs = now;
    
 {
        static int glog;
        if (!glog) {
            glog = 1;
            const uint8_t *ex = (const uint8_t *) GetModuleHandleW(NULL);
            
Xlog
                ("fallprobe: runtime globals G (exe+4103e9c) = %.3f, G2 (exe+4103ea0) = %.3f",
                 *(const float *) (ex + 0x4103e9c),
                 *(const float *) (ex + 0x4103ea0));
        }
    } 
FpHpHookInstall(*(void **) (c + 8));
    
const float *f = (const float *) (c + 0x340);
    
int g_ = g_xlogNolimit;
    g_xlogNolimit = 1;
    
        // the player-control update (exe+b89360, caller of the impact kill: 'net-kill-fall', stack exe+b8a076) fires its fall kill when  q3b0 || (q278 && !q3c0) || q280  of
        // the movement component (player+0x63c*8 = +0x31e0; vtable slots +0x278 +0x280 +0x3b0 +0x3c0 +0x3c8). Logged per frame during the fall: the one that flips at the impact is the cause
    {
        
uint8_t * pl_obj = *(uint8_t **) (c + 8);
        
void *mc = pl_obj
            && !IsBadReadPtr(pl_obj + 0x31e0,
                             8) ? *(void **) (pl_obj + 0x31e0) : NULL;
        
void **mvt = mc && !IsBadReadPtr(mc, 8) ? *(void ***) mc : NULL;
        
if (mvt && !IsBadReadPtr(mvt, 0x3d0)) {
            
static const int slots[6] =
                { 0x278, 0x280, 0x3b0, 0x3c0, 0x3c8, 0x258 };
            
int q[6];
            for (int k = 0; k < 6; k++)
                q[k] = ((uint8_t(*)(void *)) mvt[slots[k] / 8]) (mc) != 0;
            
 {
                static int infall, was258 = 1;
                static float lastpl[3];
                
if (!infall) {
                    infall = 1;
                    FpRayKick(0, "at the first fall frame", pl, 0.3f,
                                16.0f);
                }
                
if (was258 && !q[5])
                    FpRayKick(1, "when q258 dropped to 0 (landed)", pl,
                                0.4f, 3.0f);
                
was258 = q[5];
                memcpy(lastpl, pl, 12);
                FpRayRead();
            }
            
Xlog
                ("fallprobe: moveq %d %d %d %d %d (278 280 3b0 3c0 3c8) q258 %d (the gate: the kill needs it 0) | funcs exe+%llx exe+%llx exe+%llx exe+%llx exe+%llx",
                 
q[0], q[1], q[2], q[3], q[4], q[5],
                 (unsigned long long) FpOff(mvt[0x278 / 8]),
                 (unsigned long long) FpOff(mvt[0x280 / 8]),
                 (unsigned long long) FpOff(mvt[0x3b0 / 8]),
                 
(unsigned long long) FpOff(mvt[0x3c0 / 8]),
                 (unsigned long long) FpOff(mvt[0x3c8 / 8]));
        
}
    
} 
if (n++ < 4000)
        
Xlog
            ("fallprobe: G now %.3f | ctl %p | player y %.2f vy %.1f | probe pos %.2f %.2f %.2f | normal %.2f %.2f %.2f | dist(+370) %.3f valid(+380) %u | +340 %.2f %.2f %.2f %.2f | +378 %llx",
             
*(const float *) ((const uint8_t *) GetModuleHandleW(NULL) +
                                0x4103e9c), (const void *) c, pl[1], vy,
             f[4], f[5], f[6], f[8], f[9], f[10], f[12], c[0x380], f[0],
             f[1], f[2], f[3],
             (unsigned long long) *(const uint64_t *) (c + 0x378));
    
g_xlogNolimit = g_;

} 
void fp_ctl_stub(void);


__asm__(
".text\n" 
         ".globl fp_ctl_stub\n" 
         "fp_ctl_stub:\n" 
         "  sub $0x98, %rsp\n" 
         "  mov %rcx, 0x20(%rsp)\n" 
         "  mov %rdx, 0x28(%rsp)\n" 
         "  mov %r8, 0x30(%rsp)\n" 
         "  mov %r9, 0x38(%rsp)\n" 
         "  movups %xmm0, 0x40(%rsp)\n" 
         "  movups %xmm1, 0x50(%rsp)\n" 
         "  movups %xmm2, 0x60(%rsp)\n" 
         "  movups %xmm3, 0x70(%rsp)\n" 
         "  call fp_ctl_log\n" 
         "  mov 0x20(%rsp), %rcx\n" 
         "  mov 0x28(%rsp), %rdx\n" 
         "  mov 0x30(%rsp), %r8\n" 
         "  mov 0x38(%rsp), %r9\n" 
         "  movups 0x40(%rsp), %xmm0\n" 
         "  movups 0x50(%rsp), %xmm1\n" 
         "  movups 0x60(%rsp), %xmm2\n" 
         "  movups 0x70(%rsp), %xmm3\n" 
         "  add $0x98, %rsp\n" 
 "  jmp *g_fp_ctl_tramp(%rip)\n");

 
// ---- the 5.0 s compare in the control update (exe+b89e14: this+0x24 > 5.0 -> net-kill-fall, line 0x19a): log this+0x24 / +0x20 while a fall is in progress ----
void *g_fp_tm_tramp;

void __attribute__((used)) fp_tm_log(const uint8_t *o)
{
    
static uint64_t last;
    
uint64_t now = GetTickCount64();
    
if (!o || now > g_fpFallMs + 8000 || now < last + 250 || !FpOn()
         || IsBadReadPtr(o, 0x40))
        return;
    
last = now;
    
int g_ = g_xlogNolimit;
    g_xlogNolimit = 1;
    
Xlog
        ("fallprobe: ctl-update %p timer +0x24 %.3f s (kill when > 5.0 s) +0x20 %.3f",
         (const void *) o, *(const float *) (o + 0x24),
         *(const float *) (o + 0x20));
    
g_xlogNolimit = g_;

} 
void fp_tm_stub(void);


__asm__(
".text\n" 
         ".globl fp_tm_stub\n" 
         "fp_tm_stub:\n" 
         "  sub $0x98, %rsp\n" 
         "  mov %rcx, 0x20(%rsp)\n" 
         "  mov %rdx, 0x28(%rsp)\n" 
         "  mov %r8, 0x30(%rsp)\n" 
         "  mov %r9, 0x38(%rsp)\n" 
         "  movups %xmm0, 0x40(%rsp)\n" 
         "  movups %xmm1, 0x50(%rsp)\n" 
         "  movups %xmm2, 0x60(%rsp)\n" 
         "  movups %xmm3, 0x70(%rsp)\n" 
         "  call fp_tm_log\n" 
         "  mov 0x20(%rsp), %rcx\n" 
         "  mov 0x28(%rsp), %rdx\n" 
         "  mov 0x30(%rsp), %r8\n" 
         "  mov 0x38(%rsp), %r9\n" 
         "  movups 0x40(%rsp), %xmm0\n" 
         "  movups 0x50(%rsp), %xmm1\n" 
         "  movups 0x60(%rsp), %xmm2\n" 
         "  movups 0x70(%rsp), %xmm3\n" 
         "  add $0x98, %rsp\n" 
 "  jmp *g_fp_tm_tramp(%rip)\n");

 
// ---- health: every engine damage of >= 20 on OUR Player, with the call stack (exe-relative). The health component's vtable slot 3 (+0x18; the landing code calls
// it as  health->vt[3](this, damage, 0, 0, bool, bool)) is replaced by a forwarding wrapper. ----
static uint64_t(*g_fh_orig) (void *, float, uint64_t, uint64_t, uint64_t,
                             uint64_t);

static void *g_fh_obj, **g_fh_slot;

static uint64_t FhWrap(void *t, float d, uint64_t a, uint64_t b,
                         uint64_t c, uint64_t e)
{
    
if (d >= 50.0f && FpOn()) {
        
void *bt[14];
        USHORT k = RtlCaptureStackBackTrace(1, 14, bt, NULL);
        
char st[400];
        size_t l = 0;
        st[0] = 0;
        
for (USHORT j = 0; j < k && l + 24 < sizeof st; j++)
            l += (size_t) snprintf(st + l, sizeof st - l, " exe+%llx",
                                   (unsigned long long) FpOff(bt[j]));
        
float bb[16];
        const float *pl = PlayerLocator(bb);
        
int g_ = g_xlogNolimit;
        g_xlogNolimit = 1;
        
Xlog
            ("fallprobe: HEALTH DAMAGE %.1f on %s (%p) player y %.2f from%s",
             d, t == g_fh_obj ? "OUR Player" : "an object", t,
             pl ? pl[1] : 0.0f, st);
        
g_xlogNolimit = g_;
    
}
    
return g_fh_orig(t, d, a, b, c, e);

}


static void FpHpScanInstall(void)
{                               // startup: every .rdata vtable whose slot 3 is the health apply-damage (exe+1feb9a0, seen in the A log)
    uint8_t *exe = (uint8_t *) GetModuleHandleW(NULL);
    
IMAGE_NT_HEADERS * nt =
        (IMAGE_NT_HEADERS *) (exe + ((IMAGE_DOS_HEADER *) exe)->e_lfanew);
    
IMAGE_SECTION_HEADER * sec = IMAGE_FIRST_SECTION(nt);
    
uint64_t want = (uint64_t) (uintptr_t) (exe + 0x1feb9a0);
    
int n = 0;
    
for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        
if (memcmp(sec[i].Name, ".rdata", 6))
            continue;
        
uint64_t * q = (uint64_t *) (exe + sec[i].VirtualAddress);
        size_t cnt = sec[i].Misc.VirtualSize / 8;
        
for (size_t k = 3; k < cnt && n < 6; k++) {
            
if (q[k] != want || q[k - 1] < (uint64_t) (uintptr_t) exe
                 || q[k - 1] > (uint64_t) (uintptr_t) exe + 0x3000000)
                continue;       // slot 3, neighbours are exe code
            if (!g_fh_orig)
                g_fh_orig =
                    (uint64_t(*)
                     (void *, float, uint64_t, uint64_t, uint64_t,
                      uint64_t)) want;
            
DWORD old;
            VirtualProtect(&q[k], 8, PAGE_READWRITE, &old);
            q[k] = (uint64_t) (uintptr_t) FhWrap;
            VirtualProtect(&q[k], 8, old, &old);
            
Xlog("fallprobe: health vtable %p slot 3 wrapped at startup",
                  (void *) &q[k - 3]);
            n++;
    
} 
} 
if (!n)
        Xlog("fallprobe: health apply-damage (exe+1feb9a0) not found in any .rdata vtable - wrapper installs at the first die-probe call");

}


static void FpHpHookInstall(void *player)
{
    
static int done;
    
if (done || !player || IsBadReadPtr((uint8_t *) player + 0x610, 8))
        return;
    
void *obj = *(void **) ((uint8_t *) player + 0x610);
    
if (!obj || IsBadReadPtr(obj, 8))
        return;
    
void **vt = *(void ***) obj;
    
if (!vt || IsBadReadPtr(vt, 0x20))
        return;
    
done = 1;
    
g_fh_obj = obj;
    
if (vt[3] == (void *) FhWrap) {
        Xlog("fallprobe: our health object is %p (vtable already wrapped)",
             obj);
        return;
    }
    g_fh_orig =
        (uint64_t(*)
         (void *, float, uint64_t, uint64_t, uint64_t, uint64_t)) vt[3];
    g_fh_slot = &vt[3];
    
DWORD old;
    VirtualProtect(&vt[3], 8, PAGE_READWRITE, &old);
    
vt[3] = (void *) FhWrap;
    
VirtualProtect(&vt[3], 8, old, &old);
    
Xlog("fallprobe: health vtable slot 3 of %p wrapped (orig exe+%llx)",
          obj, (unsigned long long) FpOff((void *) g_fh_orig));

} 
 

// exe+b55d40 = fall-to-ground-die Enter(state, ?, float *probe): probe = the controller's ground-probe result {pos xyz, w, normal xyz, w, distance at +0x20}.
static void (*g_fpd_tramp)(void *self, void *p2, float *probe);

static void FpdHook(void *self, void *p2, float *probe)
{
    
if (FpOn()) {
        
float b[16];
        const float *pl = PlayerLocator(b);
        
int g_ = g_xlogNolimit;
        g_xlogNolimit = 1;
        
if (probe && !IsBadReadPtr(probe, 0x24))
            
Xlog
                ("fallprobe: FALL-TO-GROUND-DIE entered | player %.2f %.2f %.2f | probe ground %.2f %.2f %.2f normal.y %.2f distance %.2f m (kill band %.1f..16.0)",
                 
pl ? pl[0] : 0.0f, pl ? pl[1] : 0.0f, pl ? pl[2] : 0.0f,
                 probe[0], probe[1], probe[2], probe[5], probe[8],
                 g_fhCave ? *g_fhCave : 6.0f);
        
g_xlogNolimit = g_;
    
}
    
g_fpd_tramp(self, p2, probe);

}


static void FpInstall(void)
{
    
FhInstall();
    
FpHpScanInstall();
    
static const uint8_t d15[15] = { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x20, 0x55, 0x57, 0x41, 0x56 };       // exe+b55d40
    g_fpd_tramp =
        (void (*)(void *, void *, float *)) VdfInline(0x140b55d40ULL, d15,
                                                       15,
                                                       (void *) FpdHook,
                                                       "fall-to-ground-die log",
                                                       (void **)
                                                       &g_fpd_tramp);
    
Xlog("fallprobe: fall-to-ground-die (exe+b55d40) hook %s",
          g_fpd_tramp ? "OK" : "FAIL (exe code differs)");
    
static const uint8_t k16[16] = { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x55 }; // exe+b8acb0
    VdfInline(0x140b8acb0ULL, k16, 16, (void *) fp_ctl_stub,
               "fall controller log", &g_fp_ctl_tramp);
    
Xlog("fallprobe: fall controller (exe+b8acb0) hook %s",
          g_fp_ctl_tramp ? "OK" : "FAIL (exe code differs)");
    
static const uint8_t t20[20] = { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 }; // exe+b89360
    VdfInline(0x140b89360ULL, t20, 20, (void *) fp_tm_stub,
               "control update timer log", &g_fp_tm_tramp);
    
Xlog("fallprobe: control update (exe+b89360) timer log %s",
          g_fp_tm_tramp ? "OK" : "FAIL (exe code differs)");
    
        // exe+b56500: mov rax,rsp; mov [rax+10],rbx; mov [rax+18],rsi; mov [rax+20],rdi; push rbp; push r14   (18 bytes, no rip-relative operand)
    static const uint8_t e[18] =
        { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18,
0x48, 0x89, 0x78, 0x20, 0x55, 0x41, 0x56 };
    
g_fp_tramp =
        (uint8_t(*)(void *, float)) VdfInline(0x140b56500ULL, e, 18,
                                               (void *) FpHook,
                                               "fall-die probe",
                                               (void **) &g_fp_tramp);
    
Xlog("fallprobe: die-probe (exe+b56500) hook %s",
          g_fp_tramp ? "OK" : "FAIL (exe code differs)");
    
static const uint8_t c[14] = { 0x40, 0x53, 0x41, 0x57, 0x48, 0x83, 0xec, 0x58, 0xc5, 0xf8, 0x29, 0x74, 0x24, 0x40 };       // exe+e0abd0, 14 bytes
    g_fpc_tramp =
        (uint8_t *
         (*)(void *, float, float, float,
             uint64_t)) VdfInline(0x140e0abd0ULL, c, 14,
                                   (void *) FpcHook,
                                   "landing record probe",
                                   (void **) &g_fpc_tramp);
    
Xlog("fallprobe: landing chooser (exe+e0abd0) hook %s",
          g_fpc_tramp ? "OK" : "FAIL (exe code differs)");
    
Xlog
        ("fallprobe: installed (log is written while t2r-fallprobe.txt exists next to the exe; the health wrapper installs at the first die-probe call)");

} 
#endif  /* 
 */
#endif  /* 
 */
