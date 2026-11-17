// clipfx.c - TLOU2's own anim `part-effect` events fired through the ENGINE's event path. Included from t2r-ext.c before ctxdown.c.
//
// Takedown / melee blood (blood streams, ribbons, sprays, wall splatters, skin blood) must be TLOU2 SP's exactly. The SP sandbox
// (re/sp-sandbox-runs, round 3) shows every SP takedown's blood comes from the clips' own `part-effect` anim events
// (type 0xc0d9313c7f55b23e), each a {type, frame, count, params {key, value}} entry with the particle group, joint, offset, per-effect knobs
// (rateMult / gravMult / emitLife / thickness / spriteScale ...), blood-mask UVs, align-offset world splatters. Before this file MP only
// spawned the joint-bound ones through spawn-particles-at-joint (default knobs, no world splatters, no skin blood).
//
// What the engine does with such an event (static RE, exe 1.6.10721):
//   NdGameObject::HandleEvent 0x141b04500, default case (0x141b04824): memset(info, 0, 0x3b0); EffectAnimInfo ctor 0x141c2ec30(info);
//   obj->vtable[0x508](obj, msg, info) (the per-class effect-info builder: Player 0x140db9ca0, Character 0x141b0c160 -> EffectControl
//   vtable +0x40 = 0x140393f80, which copies msg+0 -> info+0x28, msg+8 (the event entry) -> info+0x388, msg+0x10 -> info+0x390,
//   msg+0x18 (mirror) -> info+0x86, the owner handle -> info+0x58); then EffectControl ([obj+0x1c8])->vtable[0x48](ec, &out, info) =
//   EffectControl::FireEffect 0x14038a260 (game vtable 0x142b27490), which reads joint / offsets / knobs from the entry's params and spawns
//   the particle (directly, src effect-control.cpp:4222, or through an nd-particle-tracker process, nd-particle-tracker.cpp:72).
//   The spawn-particles-at-joint native (0x140d6f100) runs the same FireEffect with an info it fills itself (no params).
// clipfx_fire() does exactly the default case, with a persistent copy of the clip's event entry (static: the tracker keeps reading it).
//
// Engine or ours: FireEffect's vtable slot is wrapped (clipfx_fe_hook). While a takedown runs, every part-effect the engine fires is
// recorded {name, owner object}; ours are tagged by thread. Per owner kind (our own player / a puppet) the first event of a takedown is fired
// CFXE_LAG_MS late: if the engine fired the same effect on that body in the meantime the engine plays the clip events itself (verdict 1, we
// never fire for that kind again - no double blood); if not (verdict 0) every later event of that kind fires on its frame. Logged once:
//   "clipfx: verdict ..." and every engine part-effect seen during a takedown (first 60): "clipfx: ENGINE part-effect ...".
// Files next to the exe: t2r-clipfx-off.txt = the old spawn-particles-at-joint path; t2r-clipfx-no-att-masks.txt = no blood-mask (bm-*)
// events on the ATTACKER (their posU / posV are Abby's / Ellie's UV layout; MP attackers wear NPC looks).

#define CFXE_TYPE 0xc0d9313c7f55b23eULL
#define CFXE_NAME 0xc4bcadba8e631b86ULL // "name"
#define CFXE_LAG_MS 70.0
typedef struct {
    uint64_t type;
    float frame;
    uint32_t n;
    const uint64_t(*pairs)[2];
} CfxeEntry;                   // the clip's effect event (24 bytes)
_Static_assert(sizeof(CfxeEntry) == 24,
               "anim effect event entry is 24 bytes");
static int g_cfxeOk = -1;      // engine path verified
static void *(*g_cfxe_fe_orig)(void *, void *, uint8_t *);
static volatile DWORD g_cfxeOursTid;
static volatile int g_cfxeWatch;       // record engine part-effects (a takedown runs)
static struct {
    uint64_t name;
    uint8_t *obj;
    uint64_t t60;
} g_cfxe_ring[64];
static volatile LONG g_cfxeRi;
static int g_cfxeVerdict[2] = { -1, -1 };      // [0] our own player, [1] puppets: -1 unknown, 0 the engine is silent, 1 the engine fires

static uint64_t CfxeEntryName(const CfxeEntry *e)
{
    if (!e || !e->pairs)
        return 0;
    for (uint32_t i = 0; i < e->n && i < 64; i++)
        if (e->pairs[i][0] == CFXE_NAME)
            return e->pairs[i][1];
    return 0;
}

static uint8_t *CfxeHandleObj(const uint8_t *h)
{                               // {ptr, u32 id} -> the object (FireEffect's own resolve: [ptr+8], id == [obj+0x94])
    uint8_t *p = *(uint8_t * const *) h;
    uint32_t id = *(const uint32_t *) (h + 8);
    if (!p || !id)
        return NULL;
    uint8_t *o = *(uint8_t **) (p + 8);
    return o && *(uint32_t *) (o + 0x94) == id ? o : NULL;
}

static void (*volatile g_cfxe_seen_cb)(const uint8_t * info);   // sp_parity.c census (NULL unless t2r-sp-snd-census.txt)
// Sidearm draw / put-away foley (secondaries had no holster / draw sound). SP plays them from the draw / put-away clips' own
// anim events (re/sp-sandbox-runs round 3: EVT sound-effect sfx-gear-pistol-unholster-canvas at r_palm / -holster-canvas at
// pelvis_gun_holster_attach_offset; anim-player-gun-abby: abby-p-unholster^idle-stand-part frame 1 / abby-p-holster^idle-stand-part frame 13
// sound-effect, the abby-p-second-* (hip holster) clips gear-effect-lower gear-lower-pistol-leg-(un)holster at frames 1 / 10). Offline, every
// link of MP's chain checks out: the clips are the SP clips (MP mounts sp-common's anim-player-gun-abby unchanged), all six sounds
// (sfx-gear-pistol-(un)holster-canvas, sfx-gear-lower-pistol-leg-(un)holster-military / -jeans / -leather, sfx-gear-upper-pistol-*) are
// in wpn-weapon-foley (the global "hud" level's bank, loaded in MP: the bottle silencer's thunk from it was audible) with sfx-info in the
// global aud-sfx2.bin, category foley-gear (mix-default -18 / -20 dB, as SP), the MP leg meshes' mesh-audio-config +0x38 suffixes
// (-military / -jeans / -leather) complete the gear names (FireEffect "sfx-%s%s", exe 0x14038b0b8), and FireEffect has no MP-only gate
// on sound-effect / gear-effect (0x14038de0b: the options-valid check only).
// Runs (A / B t2r-ext logs, "sidearm sfx: engine ..."): the events DO reach FireEffect on every draw / put-away. Our own player:
// FireEffect builds the sound process, but the gear-lower (leg / hip holster) ones were gone 100 ms later while the canvas / gear-upper
// ones lived. Puppets: mostly no process at all (handle 0).
// ROOT CAUSE (static RE, exe 1.6.10721): the clip events name joints MP's skeletons do not have. The SP clips play the sound at
// r_thigh_pistol_holster_attach(_offset) (every stand / relaxed leg-holster clip), pelvis_gun_holster_attach(_offset) (the relaxed /
// crouch / prone / supine canvas draws) - abby-skel has all four, base-male-skel / base-female-skel none (r_wrist / r_palm / pelvis /
// r_hand_prop(_attachment), the joints of the surviving events, both have). FireEffect (0x14038a1d0 -> 0x141c30650) spawns an SfxProcess
// attached by joint NAME (mode 3, spawn info 0x141dd7630). Its spawn cull (AudioManager::IsSfxParamsCulled 0x141a90640) falls back to
// the owner's locator when the joint is missing, so the process is created - but its per-frame position update (0x141ddc230, SfxProcess
// vtable 0x142b0e400 +0x238) only debug-prints on a failed lookup (0x141ddc747), the position stays FLT_MAX, and the process update
// (0x141dd7e50, 0x141dd81a9) ends a process without a position on its first frame (vtable +0x1e8 -> GoState 0x141342560): no voice.
// The engine's own owner-change path (0x141dd88c0) already handles a missing joint by playing at the owner (mode 1, 0x141dd8953).
// FIX (install_sfx_joint_fallback): the failed-lookup branch does exactly that - mode 1, position from the owner's locator (0x141ddc779).
// (c6644d1 never installed it: its guard expected test edx,edx right after 0x141ddc539; it is at 0x141ddc557. Fixed.)
// Every engine sound an SP clip names on a joint the MP skeleton lacks plays now (not only the sidearm foley), at the body's origin.
// Puppets with handle 0: the SfxSpawnInfo cull (0x141ddbc10 -> 0x141a90640 -> 0x141a8fc60) refused the spawn - distance from the
// listener over the sfx-info range + 3 m (foley-gear: 20 m) or height; the same rule SP applies to every NPC (the level cull against
// audio.bin mixer-stem-lod-config, foley-gear -60 dB idle .. -45 dB combat, runs only in PlaySfx). Within range the puppet's process is
// built and now plays like our own. The log names each refusal (below).
// Seraphite torso (-leather at +0x30): SP has no sfx-gear-upper-pistol-*-leather (and no -leather aim raise / lower) for a PLAYER body
// (only sfx-npc-gear-upper-pistol-*-leather); the suffix stays (sfx-gear-move-upper-leather, the coat's rustle, needs it), so a Seraphite
// draw plays the holster event (leg -leather / canvas) without the extra torso cloth layer. No data fix without a new bank cue.
// Diagnostics (log): "sfx joint fallback #n: <sound> on <body>" (a process the engine would have ended), "sfx spawn culled: <sound>
// <reason> (value / limit)" (SfxSpawnInfo cull, replaces the exe's no-op cull logger 0x141a92110), "sfx play failed: <sound> result n"
// (AudioManager::PlaySfx 0x141a83e20 from the process update), "sfx play started: <sound> - voice n", "sidearm sfx: ... 100 ms later:
// live (started, voice, playing per AudioManager 0x141a8fa70, position) / gone". Files next to the exe:
// t2r-sfx-joint-fallback-off.txt = unpatched engine (the A/B), t2r-sfx-diag-off.txt = no cull / play hooks,
// t2r-sidearm-sfx-replay.txt = also replay each sidearm event through play-sound-at-joint (the 16f64af workaround, now opt-in).
#define SASFX_SOUND 0xe46e61567f7193feULL       // sound-effect
#define SASFX_GLOW 0x1415b308f53e0accULL        // gear-effect-lower
#define SASFX_GUP 0x07cf0d54e310e1b1ULL // gear-effect-upper
static const struct {
    uint64_t sid;
    const char *name;
} SASFX_NAMES[] = {
    { 0x1c868031f39f0db2ULL, "sfx-gear-pistol-unholster-canvas" },
        { 0x93fcd5a6ee3d67a5ULL, "sfx-gear-pistol-holster-canvas" },
    { 0x3468ca48dcde7940ULL, "gear-lower-pistol-leg-unholster" },
        { 0xa159eb78c51d3e31ULL, "gear-lower-pistol-leg-holster" },
    { 0x1e3a41d7311e70e2ULL, "gear-upper-pistol-unholster" },
        { 0x10b18d989a75115fULL, "gear-upper-pistol-holster" }
};

#define SASFX_NK ((int)(sizeof SASFX_NAMES / sizeof *SASFX_NAMES))
static volatile LONG g_sasfxN;
// queued engine dispatches: {kind = SASFX_NAMES index, body, own?, the engine's sound handle, the event joint}
typedef struct {
    volatile LONG ready;
    int kind, own;
    uint8_t *obj;
    uint64_t eh;
    uint32_t eid;
    uint64_t joint;
} SasfxEv;
static SasfxEv g_sasq[16];
static volatile LONG g_sasqW;
static void SasfxQueue(int kind, uint8_t *obj, int own, const void *out,
                        uint64_t joint)
{
    if (!obj || g_cfxeOursTid == GetCurrentThreadId())
        return;
    SasfxEv *e = &g_sasq[InterlockedIncrement(&g_sasqW) & 15];
    if (e->ready)
        return;                 // (16 in one frame: drop)
    e->kind = kind;
    e->obj = obj;
    e->own = own;
    e->joint = joint;
    e->eh = out ? *(const uint64_t *) out : 0;
    e->eid = out ? *((const uint32_t *) out + 2) : 0;
    MemoryBarrier();
    e->ready = 1;
}

// the joints the SP sidearm clips name (logs; the first five are missing on base-male-skel / base-female-skel)
static const char *SasfxJointName(uint64_t sid)
{
    static const char *const N[] =
        { "r_thigh_pistol_holster_attach",
"r_thigh_pistol_holster_attach_offset", "pelvis_gun_holster_attach",
        "pelvis_gun_holster_attach_offset", "r_leg_holster", "r_wrist",
            "r_palm", "pelvis", "pelvis_helper", "r_hand_prop",
        "r_hand_prop_attachment"
    };
    static uint64_t h[sizeof N / sizeof *N];
    static char hx[8][20];
    static unsigned hi;
    for (unsigned i = 0; i < sizeof N / sizeof *N; i++) {
        if (!h[i])
            h[i] = Fnv64Rt(N[i]);
        if (h[i] == sid)
            return N[i];
    }
    if (!sid)
        return "(no joint)";
    char *o = hx[hi++ & 7];
    snprintf(o, 20, "%016llx", (unsigned long long) sid);
    return o;
}

static int SppAgilityVoxDrop(uint8_t * obj, uint64_t name);  // sp_parity.c
static volatile uint8_t *g_pul_vox_me, *g_pul_vox_po;   // melee_pullout.c: pull-out bodies (ours / the other half's puppet) whose clip voice events drop
static uint8_t *g_pulVoxPoKeep;
static uint64_t g_pul_vox_me_until, g_pul_vox_po_until;
static int SppAgilityFxDrop(const uint8_t * info);   // sp_parity.c: foot / cloth events of an Agility II puppet
static void *ClipfxFeHook(void *ec, void *out, uint8_t *info)
{
    if (g_cfxe_seen_cb)
        g_cfxe_seen_cb(info);
    if (info && out && *(uint64_t *) (info + 0x30) == 0x36b7cf45b65a6305ULL && GetCurrentThreadId() != g_cfxeOursTid) {       // voice-effect: Agility II puppets' movement grunts
        uint64_t nm =
            CfxeEntryName(*(const CfxeEntry **) (info + 0x388));
        if (nm && SppAgilityVoxDrop(CfxeHandleObj(info + 0x58), nm)) {
            memset(out, 0, 16);
            return out;
        }
        uint8_t *vo = (g_pul_vox_me
                       || g_pul_vox_po) ? CfxeHandleObj(info +
                                                          0x58) : NULL;
        if (vo
            && (vo == (uint8_t *) g_pul_vox_me
                || vo == (uint8_t *) g_pul_vox_po)) {
            static LONG nl;
            if (InterlockedIncrement(&nl) <= 60)
                Xlog("pullout vox: clip voice-effect %016llx dropped on %s", (unsigned long long) nm, vo == (uint8_t *) g_pul_vox_me ? "our body" : "the other half's puppet");
            memset(out, 0, 16);
            return out;
        }
    }
    if (info && out && GetCurrentThreadId() != g_cfxeOursTid
        && SppAgilityFxDrop(info)) {
        memset(out, 0, 16);
        return out;
    }
    if (info && out) {
        uint64_t ty = *(uint64_t *) (info + 0x30);
        if (ty == SASFX_SOUND || ty == SASFX_GLOW || ty == SASFX_GUP) {
            uint64_t nm =
                CfxeEntryName(*(const CfxeEntry **) (info + 0x388));
            for (int k = 0; nm && k < SASFX_NK; k++)
                if (SASFX_NAMES[k].sid == nm) {
                    void *r = g_cfxe_fe_orig(ec, out, info);
                    uint8_t *o = CfxeHandleObj(info + 0x58);
                    int own = o && o == BpMe();
                    if (g_sasfxN < 24
                        && InterlockedIncrement(&g_sasfxN) <= 24)
                        Xlog("sidearm sfx: engine %s %s at %s on %s -> sound handle %llx/%x", ty == SASFX_SOUND ? "sound-effect" : ty == SASFX_GLOW ? "gear-effect-lower" : "gear-effect-upper", SASFX_NAMES[k].name, SasfxJointName(*(const uint64_t *) info), own ? "our own player" : "another body", (unsigned long long) *(const uint64_t *) out, *((const uint32_t *) out + 2));
                    SasfxQueue(k, o, own, out, *(const uint64_t *) info);
                    return r;
                }
        }
    }
    if (g_cfxeWatch && info && *(uint64_t *) (info + 0x30) == CFXE_TYPE
        && GetCurrentThreadId() != g_cfxeOursTid) {
        const CfxeEntry *e = *(const CfxeEntry **) (info + 0x388);    // only anim events carry an entry (the spawn natives leave it 0)
        if (e) {
            uint64_t nm = CfxeEntryName(e);
            uint8_t *o = CfxeHandleObj(info + 0x58);
            LONG i = InterlockedIncrement(&g_cfxeRi) & 63;
            g_cfxe_ring[i].t60 = 0;
            g_cfxe_ring[i].name = nm;
            g_cfxe_ring[i].obj = o;
            g_cfxe_ring[i].t60 = g_t60 ? g_t60 : 1;
        }
    }
    return g_cfxe_fe_orig(ec, out, info);
}

static int CfxeCheck(void)
{
    if (g_cfxeOk >= 0)
        return g_cfxeOk;
    uint8_t *exe = (uint8_t *) GetModuleHandleW(NULL);
    static const uint8_t ctor[24] =
        { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48,
0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7c, 0x24, 0x20,
        0x41, 0x54, 0x41, 0x56
    };                          // 141c2ec30 EffectAnimInfo ctor
    static const uint8_t fire[17] = { 0x40, 0x53, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xec, 0xb8, 0x04, 0x00 };   // 14038a260
    static const uint8_t dflt[16] = { 0x33, 0xd2, 0x48, 0x8d, 0x4d, 0x50, 0x41, 0xb8, 0xb0, 0x03, 0x00, 0x00, 0xe8, 0x6b, 0xcc, 0xd6 }; // 141b04824
    uint64_t slot = *(uint64_t *) (exe + 0x2b274d8);
    g_cfxeOk = !memcmp(exe + 0x1c2ec30, ctor, 24)
        && !memcmp(exe + 0x38a260, fire, 17)
        && !memcmp(exe + 0x1b04824, dflt, 16)
        && (slot == (uint64_t) (exe + 0x38a260)
            || slot == (uint64_t) ClipfxFeHook);
    if (!g_cfxeOk)
        HOOK_FAIL
            ("clipfx: exe code differs (EffectAnimInfo / FireEffect / event handler) - clip events fall back to spawn-particles-at-joint");
    return g_cfxeOk;
}

static void InstallClipfxHook(void)
{
    if (!CfxeCheck())
        return;
    uint8_t *exe = (uint8_t *) GetModuleHandleW(NULL);
    uint64_t *slot = (uint64_t *) (exe + 0x2b274d8);    // game EffectControl vtable 0x142b27490 + 0x48
    if (*slot == (uint64_t) ClipfxFeHook)
        return;
    g_cfxe_fe_orig = (void *(*)(void *, void *, uint8_t *)) *slot;
    DWORD old;
    if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) {
        HOOK_FAIL
            ("clipfx: FireEffect slot not writable - engine part-effects not watched");
        g_cfxe_fe_orig = NULL;
        return;
    }
    InterlockedExchange64((volatile LONG64 *) slot,
                          (LONG64) (uint64_t) ClipfxFeHook);
    VirtualProtect(slot, 8, old, &old);
    Xlog("clipfx: EffectControl::FireEffect watched (vtable slot 0x142b274d8): engine part-effects during takedowns are logged and never doubled");
}

static int CfxeFile(const char *f, int *cache, uint64_t *chk)
{
    if (*cache < 0 || GetTickCount64() - *chk > 2000) {
        *chk = GetTickCount64();
        *cache = GetFileAttributesA(f) != INVALID_FILE_ATTRIBUTES;
    }
    return *cache;
}

static int ClipfxOff(void)
{
    static int c = -1;
    static uint64_t k;
    return CfxeFile("t2r-clipfx-off.txt", &c, &k);
}

static int ClipfxNoAttMasks(void)
{
    static int c = -1;
    static uint64_t k;
    return CfxeFile("t2r-clipfx-no-att-masks.txt", &c, &k);
}

static uint8_t *CfxeObjOf(uint64_t ent)
{
    if (!ent)
        return NULL;
    if (ent == SID_PLAYER)
        return BpMe();
    return (uint8_t *) ((void *(*)(uint64_t))
                        T2R_SYM(kFnEntityFromHandle))(ent);
}

// engine fired `name` on `obj` at or after t60 `since` (not one of ours)
static int CfxeEngineHad(uint64_t name, const uint8_t *obj,
                           uint64_t since)
{
    for (int i = 0; i < 64; i++)
        if (g_cfxe_ring[i].t60 >= since && g_cfxe_ring[i].obj == obj
            && g_cfxe_ring[i].name == name)
            return 1;
    return 0;
}

static int CfxeEngineAny(const uint8_t *obj, uint64_t since)
{
    for (int i = 0; i < 64; i++)
        if (g_cfxe_ring[i].t60 >= since && g_cfxe_ring[i].obj == obj
            && g_cfxe_ring[i].name)
            return 1;
    return 0;
}

// A clip event names a joint of the body (key 0x52e7fc8056bc65d7). On OUR body as the victim of a paired move (posed by the clip + ground IK) a joint's locator
// can be non-finite for a frame (bm-blood-spray-dense on r_hand_prop, 433 ms into shiv-ground-execution-left; the engine's
// nd-particle-tracker then asserts IsFinite(bf) = int3). Before such an event is fired on our own player the joint's world position is read; a bad one
// is replaced by the nearest anatomical joint that reads finite (r_wrist, neck, spineb, pelvis) in a persistent copy of the entry, and logged once per joint;
// when none reads finite the event is skipped.
#define CFXE_JOINT 0x52e7fc8056bc65d7ULL
static int CfxeJointSidOk(uint64_t ent, uint64_t jsid)
{
    typedef char (*fn_lpos)(void *, float *);
    uint64_t a[2] = { ent, jsid };
    uint64_t r =
        DcCall(0x6b8812bd30c5cd5dULL /* get-joint-locator */ , 2, a);
    float t[4];
    if (!r || r == ~0ULL || IsBadReadPtr((void *) r, 16)
        || !((fn_lpos) T2R_SYM(kFnLocatorPos)) ((void *) r, t))
        return 0;
    return __builtin_isfinite(t[0]) && __builtin_isfinite(t[1])
        && __builtin_isfinite(t[2]) && __builtin_fabsf(t[0]) < 1e6f
        && __builtin_fabsf(t[1]) < 1e6f && __builtin_fabsf(t[2]) < 1e6f;
}

static const CfxeEntry *CfxeSafeJoint(uint64_t ent, const CfxeEntry *e)
{
    if (!e || !e->pairs)
        return e;
    uint32_t ji = ~0u;
    for (uint32_t i = 0; i < e->n && i < 64; i++)
        if (e->pairs[i][0] == CFXE_JOINT) {
            ji = i;
            break;
        }
    if (ji == ~0u)
        return e;
    uint64_t j = e->pairs[ji][1];
    if (CfxeJointSidOk(ent, j))
        return e;
    static const char *const FB[] =
        { "r_wrist", "neck", "spineb", "pelvis" };
    static struct {
        const CfxeEntry *src;
        uint64_t fb;
        CfxeEntry e;
    } cache[32];
    static int ncache;
    static uint64_t logged[16];
    static int nlogged;
    uint64_t fb = 0;
    for (int k = 0; k < 4; k++) {
        uint64_t c = FNV64(FB[k]);
        if (c != j && CfxeJointSidOk(ent, c)) {
            fb = c;
            break;
        }
    }
    int seen = 0;
    for (int i = 0; i < nlogged; i++)
        if (logged[i] == j)
            seen = 1;
    if (!seen && nlogged < 16) {
        logged[nlogged++] = j;
        Xlog("clipfx: joint %016llx of an event on a body (ours or a puppet) reads non-finite - %s", (unsigned long long) j, fb ? "fired on the nearest finite joint instead" : "event skipped (no finite joint)");
    }
    if (!fb)
        return NULL;
    for (int i = 0; i < ncache; i++)
        if (cache[i].src == e && cache[i].fb == fb)
            return &cache[i].e;
    if (ncache >= 32)
        return NULL;
    uint64_t(*p)[2] = (uint64_t(*)[2]) calloc(e->n ? e->n : 1, 16);
    if (!p)
        return NULL;
    for (uint32_t i = 0; i < e->n; i++) {
        p[i][0] = e->pairs[i][0];
        p[i][1] = e->pairs[i][1];
    }
    p[ji][1] = fb;
    cache[ncache].src = e;
    cache[ncache].fb = fb;
    cache[ncache].e = *e;
    cache[ncache].e.pairs = (const uint64_t(*)[2]) p;
    return &cache[ncache++].e;
}

// fire one clip event on obj exactly as NdGameObject::HandleEvent does (default case); returns the FireEffect result's first word
// own_builder: 1 = the object's own effect-info builder (vtable +0x508; for our own player that is Player's 0x140db9ca0, which adds weapon /
// gear names and two player flags and is run by the engine for every anim event of ours), 0 = the NdGameObject / Character base builder
// 0x141b0c160 (EffectControl populate only - used for puppets: everything a part-effect reads, no remote-player state touched)
static uint64_t ClipfxFire(uint8_t *obj, uint64_t clip,
                            const CfxeEntry *e, int own_builder)
{
    if (!obj || !e || !CfxeCheck())
        return 0;
    uint8_t *exe = (uint8_t *) GetModuleHandleW(NULL);
    uint64_t lo = (uint64_t) exe, hi = lo + 0x4800000;
    static int base_ok = -1;
    if (base_ok < 0) {
        static const uint8_t base[14] = { 0x48, 0x83, 0xec, 0x48, 0x48, 0x8b, 0x81, 0xc8, 0x01, 0x00, 0x00, 0x4d, 0x8b, 0xd8 }; // 141b0c160
        base_ok = !memcmp(exe + 0x1b0c160, base, 14);
        if (!base_ok)
            HOOK_FAIL
                ("clipfx: base effect-info builder 0x141b0c160 differs - puppets use their own builder");
    }
    // The IsFinite(bf) tracker assert at exe+1b5ecd0 also hit when the shiv ATTACKER was a puppet,
    // so the own-body-only check above never ran - a puppet's joint goes non-finite the same way. Puppets: the same check by their handle (proc +0xc0).
    if (obj == BpMe()) {
        e = CfxeSafeJoint(SID_PLAYER, e);
        if (!e)
            return 0;
    } else if (!IsBadReadPtr(obj + 0xc0, 8) && *(uint64_t *) (obj + 0xc0)) {
        e = CfxeSafeJoint(*(uint64_t *) (obj + 0xc0), e);
        if (!e)
            return 0;
    }
    void **vt = *(void ***) obj;
    uint8_t *ec = *(uint8_t **) (obj + 0x1c8);
    if (!vt || (uint64_t) vt[0x508 / 8] < lo
        || (uint64_t) vt[0x508 / 8] >= hi || !ec)
        return 0;
    void *builder = own_builder
        || !base_ok ? vt[0x508 / 8] : (void *) (exe + 0x1b0c160);
    void **evt = *(void ***) ec;
    // slot 0x48 is the exe's FireEffect OR our clipfx_fe_hook (install_clipfx_hook patches the vtable slot, so with the hook in - always,
    // once installed - the slot points into our DLL, outside [lo, hi): every call here returned 0 and no part-effect / blood mask ever fired)
    if (!evt
        || ((uint64_t) evt[0x48 / 8] != (uint64_t) ClipfxFeHook
            && ((uint64_t) evt[0x48 / 8] < lo
                || (uint64_t) evt[0x48 / 8] >= hi)))
        return 0;
    static __attribute__((aligned(32))) uint8_t info[0x400];
    struct {
        uint64_t anim;
        const CfxeEntry *ev;
        uint64_t aux;
        uint8_t mirror, pad[7];
    } msg = { clip, e, 0, 0, { 0 } };
    uint64_t out[2] = { 0, 0 };
    memset(info, 0, 0x3b0);
    g_cfxeOursTid = GetCurrentThreadId();
    ((void (*)(void *)) (exe + 0x1c2ec30)) (info);
    ((void (*)(void *, void *, void *)) builder) (obj, &msg, info);
    ((void *(*)(void *, void *, void *)) evt[0x48 / 8])(ec, out, info);
    g_cfxeOursTid = 0;
    return out[0] ? out[0] : 1;
}

static const char *CfxeKind(int k)
{
    return k ? "puppets" : "our own player";
}

// `actor player` (the world splatters of the attacker half: align-offset / align-orient on the actor's align): the info builder (0x140393f80,
// at 0x14039421c) resolves "player" by NAME (0x141b0a970) to the local player unless a global flag is set - right in SP, where the attacker
// is the player, wrong in MP when the attacker half is a puppet (the splatter would land at OUR feet). For a body that is not our own player
// the entry is used without that pair: the owner is then the body itself, which is what "player" means in the SP clip.
#define CFXE_ACTOR 0xdcccc5391c47d4daULL
#define CFXE_PLAYER 0x4580fab03b7eb9c0ULL
static int CfxeHasActorPlayer(const CfxeEntry *e)
{
    for (uint32_t i = 0; e->pairs && i < e->n; i++)
        if (e->pairs[i][0] == CFXE_ACTOR && e->pairs[i][1] == CFXE_PLAYER)
            return 1;
    return 0;
}

// persistent copy of e without the actor pair (built once per entry; never freed: the trackers may keep the pointer)
static const CfxeEntry *CfxeSelfVariant(const CfxeEntry *e,
                                           CfxeEntry *slot)
{
    if (slot->type)
        return slot;
    uint64_t(*p)[2] = (uint64_t(*)[2]) calloc(e->n ? e->n : 1, 16);
    if (!p)
        return e;
    uint32_t n = 0;
    for (uint32_t i = 0; i < e->n; i++)
        if (e->pairs[i][0] != CFXE_ACTOR) {
            p[n][0] = e->pairs[i][0];
            p[n][1] = e->pairs[i][1];
            n++;
        }
    slot->frame = e->frame;
    slot->n = n;
    slot->pairs = (const uint64_t(*)[2]) p;
    slot->type = e->type;
    return slot;
}

// the engine's own part-effect anim events recorded since the last call (first 60 of the session): which bodies the engine dispatches
// clip events on in MP at all. name_of: the effect's name if the caller knows it.
static void CfxeLogEngine(const char *(*name_of)(uint64_t),
                            uint64_t att_obj, uint64_t vic_obj)
{
    static LONG seen;
    static int nl;
    LONG now = g_cfxeRi;
    if (now - seen > 64)
        seen = now - 64;
    for (; seen < now && nl < 60; seen++) {
        int i = (int) ((seen + 1) & 63);
        const char *nm = name_of ? name_of(g_cfxe_ring[i].name) : NULL;
        uint8_t *o = g_cfxe_ring[i].obj;
        char hx[24];
        snprintf(hx, sizeof hx, "%016llx",
                 (unsigned long long) g_cfxe_ring[i].name);
        nl++;
        Xlog("clipfx: ENGINE part-effect %s on %s (%p)", nm ? nm : hx,
             o
             && o == BpMe()? "our own player" : (uint64_t) o ==
             att_obj ? "the attacker puppet" : (uint64_t) o ==
             vic_obj ? "the victim puppet" : "another body", (void *) o);
    }
    seen = now;
}

// ---------------------------------------------------------------- SfxProcess joint fallback (the engine fix; see the note above SASFX_SOUND)
#define SJF_SITE 0x1ddc747      // SfxProcess position update 0x141ddc230: the failed joint lookup branch (test r15b,r15b; je)
#define SJF_LOCATOR 0x1ddc779   // ... its mode-1 path: position = the owner's locator (+0x100)
static volatile LONG *g_sjfCnt;        // processes the fallback kept (the stub's data)
static void *volatile *g_sjfLast;      // the last of them
// diagnostics queue: the cull / play hooks run on job threads, the main thread logs (clipfx_sidearm_tick)
typedef struct {
    volatile LONG ready;
    int kind;
    uint32_t code;
    float v, l;
    char name[72];
} SfxdEv;                      // kind 0 spawn culled, 1 play failed, 2 started
static SfxdEv g_sfxd[32];
static volatile LONG g_sfxdW;
// every pistol draw / put-away sound an SP clip can build on an MP body (canvas, leg x lower suffix, torso x upper suffix): sid -> name
static char g_sfxwName[24][48];
static uint64_t g_sfxwSid[24];
static int g_sfxwN;
static void SfxwInit(void)
{
    static const char *const LO[] =
        { "-military", "-jeans", "-leather" }, *const UP[] =
        { "-cotton", "-wind", "-skin", "-parka", "-leather" };
    static const char *const HU[] = { "holster", "unholster" };
    g_sfxwN = 0;
    for (int h = 0; h < 2; h++) {
        snprintf(g_sfxwName[g_sfxwN++], 48, "sfx-gear-pistol-%s-canvas",
                 HU[h]);
        for (int k = 0; k < 3; k++)
            snprintf(g_sfxwName[g_sfxwN++], 48,
                     "sfx-gear-lower-pistol-leg-%s%s", HU[h], LO[k]);
        for (int k = 0; k < 5; k++)
            snprintf(g_sfxwName[g_sfxwN++], 48,
                     "sfx-gear-upper-pistol-%s%s", HU[h], UP[k]);
    }
    for (int i = 0; i < g_sfxwN; i++)
        g_sfxwSid[i] = Fnv64Rt(g_sfxwName[i]);
}

// a spawn params / process name block (+0 text, +0x80 sid) -> the sidearm sound's name, NULL for any other sound
static const char *SfxwName(const uint8_t *n, char *b, size_t nb)
{
    if (!n || IsBadReadPtr(n, 0x88))
        return NULL;
    uint64_t sid = *(const uint64_t *) (n + 0x80);
    for (int i = 0; i < g_sfxwN; i++)
        if (sid && sid == g_sfxwSid[i])
            return g_sfxwName[i];
    size_t i = 0;
    for (; i < nb - 1 && n[i] >= 0x20 && n[i] < 0x7f; i++)
        b[i] = (char) n[i];
    b[i] = 0;
    for (int k = 0; i >= 4 && k < g_sfxwN; k++)
        if (!strcmp(b, g_sfxwName[k]))
            return g_sfxwName[k];
    // (hatchet victim silent: native sfx-hatchet-hit-gut on SID_PLAYER returns a handle, nothing heard): the melee weapon hit / breaker / finisher
    // events too, so the cull reason / PlaySfx result / voice of every one shows in the log ("sfx spawn culled / play failed / play started")
    if (i >= 8 && !strncmp(b, "sfx-", 4)
        && (strstr(b, "-hit-") || strstr(b, "-breaker-")
            || strstr(b, "-finisher-")))
        return b;
    return NULL;
}

static void SfxdPush(int kind, uint32_t code, float v, float l,
                      const char *n)
{
    char b[72];
    const char *nm = SfxwName((const uint8_t *) n, b, sizeof b);
    if (!nm)
        return;                 // the sidearm draw / put-away foley only
    SfxdEv *e = &g_sfxd[InterlockedIncrement(&g_sfxdW) & 31];
    if (e->ready)
        return;
    e->kind = kind;
    e->code = code;
    e->v = v;
    e->l = l;
    snprintf(e->name, sizeof e->name, "%s", nm);
    MemoryBarrier();
    e->ready = 1;
}

// replaces the exe's cull logger 0x141a92110 (a no-op in this build: it formats the name and drops it), which the SfxSpawnInfo cull
// 0x141ddbc10 calls for every refused spawn: (audio manager, IsSfxParamsCulled code, spawn params (+0 name), flags, value, limit)
static void SfxCullLog(void *mgr, uint32_t code, const char *params,
                         uint64_t flags, float value, float limit)
{
    (void) mgr;
    (void) flags;
    SfxdPush(0, code, value, limit, params);
}

// AudioManager::PlaySfx 0x141a83e20 as the SfxProcess update calls it (0x141dd8368): the voice id, 0 = not started (*res = why)
typedef uint32_t(*fn_playsfx) (void *, int32_t *, const char *, uint64_t,
                               uint64_t, uint64_t, uint64_t, uint64_t);
static uint32_t SfxPlayWrap(void *mgr, int32_t *res, const char *params,
                              uint64_t flags, uint64_t a5, uint64_t a6,
                              uint64_t a7, uint64_t a8)
{
    uint32_t v =
        ((fn_playsfx) ((uint8_t *) GetModuleHandleW(NULL) + 0x1a83e20))
        (mgr, res, params, flags, a5, a6, a7, a8);
    if (!v && res)
        SfxdPush(1, (uint32_t) * res, 0.0f, 0.0f, params);
    else if (v)
        SfxdPush(2, v, 0.0f, 0.0f, params);
    return v;
}

static void InstallSfxJointFallback(void)
{
    uint8_t *exe = (uint8_t *) GetModuleHandleW(NULL), *f = exe + SJF_SITE;
    static const uint8_t site[11] = { 0x45, 0x84, 0xff, 0x74, 0x63, 0x41, 0xb8, 0x80, 0x00, 0x00, 0x00 };       // test r15b,r15b; je +0x63; mov r8d,0x80
    static const uint8_t loc[12] = { 0x48, 0x8d, 0x8f, 0x00, 0x01, 0x00, 0x00, 0x48, 0x8d, 0x54, 0x24, 0x30 };  // lea rcx,[rdi+0x100]; lea rdx,[rsp+0x30]
    static const uint8_t own[10] = { 0x48, 0x8b, 0x83, 0x98, 0x01, 0x00, 0x00, 0x48, 0x85, 0xc0 };      // 0x141ddc32f mov rax,[rbx+0x198]
    static const uint8_t mode[6] = { 0x8b, 0x93, 0xc0, 0x01, 0x00, 0x00 };      // 0x141ddc539 mov edx,[rbx+0x1c0]
    static const uint8_t mode2[8] = { 0x85, 0xd2, 0x0f, 0x84, 0x50, 0x02, 0x00, 0x00 }; // 0x141ddc557 test edx,edx; je
    static const uint8_t cull[12] = { 0x85, 0xd2, 0x74, 0x68, 0x53, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0x05 }; // 0x141a92110
    static const uint8_t play[5] = { 0xe8, 0xb3, 0xba, 0xca, 0xff };    // 0x141dd8368 call 0x141a83e20
    SfxwInit();
    int fb =
        GetFileAttributesA("t2r-sfx-joint-fallback-off.txt") ==
        INVALID_FILE_ATTRIBUTES;
    int dg =
        GetFileAttributesA("t2r-sfx-diag-off.txt") ==
        INVALID_FILE_ATTRIBUTES;
    if (!fb)
        Xlog("sfx joint fallback: off (t2r-sfx-joint-fallback-off.txt) - engine sounds on joints the MP skeleton lacks end on their first frame");
    if (fb
        && (memcmp(f, site, sizeof site)
            || memcmp(exe + SJF_LOCATOR, loc, sizeof loc)
            || memcmp(exe + 0x1ddc32f, own, sizeof own)
            || memcmp(exe + 0x1ddc539, mode, sizeof mode)
            || memcmp(exe + 0x1ddc557, mode2, sizeof mode2))) {
        HOOK_FAIL
            ("sfx joint fallback: exe code differs (SfxProcess position update 0x141ddc230: %02x %02x %02x %02x) - not patched",
             !memcmp(f, site, sizeof site), !memcmp(exe + SJF_LOCATOR, loc,
                                                    sizeof loc),
             !memcmp(exe + 0x1ddc32f, own, sizeof own),
             !memcmp(exe + 0x1ddc539, mode, sizeof mode)
             && !memcmp(exe + 0x1ddc557, mode2, sizeof mode2));
        fb = 0;
    }
    if (dg
        && (memcmp(exe + 0x1a92110, cull, sizeof cull)
            || memcmp(exe + 0x1dd8368, play, sizeof play))) {
        HOOK_FAIL
            ("sfx diag: exe code differs (cull logger 0x141a92110 / PlaySfx call 0x141dd8368) - no cull / play diagnostics");
        dg = 0;
    }
    if (!fb && !dg)
        return;
    uint8_t *t = AllocNear(f, 0x1000);
    DWORD old;
    if (!t || !VirtualProtect(t, 0x1000, PAGE_EXECUTE_READWRITE, &old)) {
        HOOK_FAIL
            ("sfx joint fallback: no stub memory within rel32 reach - not patched");
        return;
    }
    uint8_t *data = t + 0x80;
    g_sjfCnt = (volatile LONG *) data;
    g_sjfLast = (void *volatile *) (data + 8);
    if (fb) {
        int k = 0;
        t[k++] = 0xF0;
        t[k++] = 0xFF;
        t[k++] = 0x05;
        *(int32_t *) (t + k) = (int32_t) (data - (t + k + 4));
        k += 4;                 // lock inc dword [cnt]
        t[k++] = 0x48;
        t[k++] = 0x89;
        t[k++] = 0x1D;
        *(int32_t *) (t + k) = (int32_t) (data + 8 - (t + k + 4));
        k += 4;                 // mov [last], rbx
        static const uint8_t body[] = { 0xC7, 0x83, 0xC0, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,     // mov dword [rbx+0x1c0], 1 (mode 1, as 0x141dd8953)
            0x48, 0x8B, 0x83, 0x98, 0x01, 0x00, 0x00,   // mov rax, [rbx+0x198] (the owner handle)
            0x48, 0x8B, 0x78, 0x08
        };                      // mov rdi, [rax+8] (the owner, checked at 0x141ddc32f)
        memcpy(t + k, body, sizeof body);
        k += sizeof body;
        int64_t d = (exe + SJF_LOCATOR) - (t + k + 5), d2 = t - (f + 5);
        if (d != (int32_t) d || d2 != (int32_t) d2)
            HOOK_FAIL
                ("sfx joint fallback: stub out of rel32 reach - not patched");
        else {
            t[k++] = 0xE9;
            *(int32_t *) (t + k) = (int32_t) d;
            k += 4;             // jmp 0x141ddc779
            FlushInstructionCache(GetCurrentProcess(), t, k);
            VirtualProtect(f, 5, PAGE_EXECUTE_READWRITE, &old);
            f[0] = 0xE9;
            *(int32_t *) (f + 1) = (int32_t) d2;        // the 5 bytes of test / je
            VirtualProtect(f, 5, old, &old);
            FlushInstructionCache(GetCurrentProcess(), f, 5);
            Xlog("sfx joint fallback: SfxProcess position update patched (0x141ddc747): a sound on a joint the skeleton lacks plays at its owner " "(mode 1, as the engine's own re-attach 0x141dd8953) instead of ending on its first frame");
        }
    }
    if (dg) {
        uint8_t *c = exe + 0x1a92110, *s2 = t + 0x100, *call =
            exe + 0x1dd8368;
        s2[0] = 0x48;
        s2[1] = 0xB8;
        *(uint64_t *) (s2 + 2) = (uint64_t) SfxPlayWrap;
        s2[10] = 0xFF;
        s2[11] = 0xE0;          // mov rax, wrap; jmp rax
        FlushInstructionCache(GetCurrentProcess(), s2, 12);
        VirtualProtect(c, 12, PAGE_EXECUTE_READWRITE, &old);
        c[0] = 0x48;
        c[1] = 0xB8;
        *(uint64_t *) (c + 2) = (uint64_t) SfxCullLog;
        c[10] = 0xFF;
        c[11] = 0xE0;           // the logger -> ours
        VirtualProtect(c, 12, old, &old);
        FlushInstructionCache(GetCurrentProcess(), c, 12);
        int64_t d = s2 - (call + 5);
        if (d == (int32_t) d) {
            VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old);
            *(int32_t *) (call + 1) = (int32_t) d;
            VirtualProtect(call, 5, old, &old);
            FlushInstructionCache(GetCurrentProcess(), call, 5);
        }
        Xlog("sfx diag: refused sound spawns (cull logger 0x141a92110) and SfxProcess PlaySfx failures (0x141dd8368) of the pistol holster foley are logged");
    }
}

// ---------------------------------------------------------------- sidearm draw / put-away: engine dispatch checks, diagnostics, opt-in replay
static int SasfxReplayOn(void)
{
    static int c = -1;
    static uint64_t k;
    return CfxeFile("t2r-sidearm-sfx-replay.txt", &c, &k);
}

// the gear suffix FireEffect builds for this body (0x140389e90 over EffectControl +0x218 configs / +0x2b8 types / +0x308 count: type 3 =
// the worn bag skipped, empty strings skipped, the highest priority (int16 +0x1a) wins; field +0x30 = upper, +0x38 = lower)
static const char *SasfxSuffix(const uint8_t *obj, int upper)
{
    if (!obj || IsBadReadPtr(obj + 0x1c8, 8))
        return NULL;
    const uint8_t *ec = *(const uint8_t * const *) (obj + 0x1c8);
    if (!ec || IsBadReadPtr(ec, 0x310))
        return NULL;
    uint32_t n = *(const uint32_t *) (ec + 0x308);
    if (n > 20)
        return NULL;
    int best = INT32_MIN;
    const char *s = NULL;
    for (uint32_t i = 0; i < n; i++) {
        if (*(const uint32_t *) (ec + 0x2b8 + 4 * i) == 3)
            continue;
        const uint8_t *cfg =
            *(const uint8_t * const *) (ec + 0x218 + 8 * i);
        if (!cfg || IsBadReadPtr(cfg, 0x90))
            continue;
        const char *p =
            *(const char *const *) (cfg + (upper ? 0x30 : 0x38));
        if (!p || IsBadReadPtr(p, 1) || !*p)
            continue;
        int pr = *(const int16_t *) (cfg + 0x1a);
        if (pr > best) {
            best = pr;
            s = p;
        }
    }
    return s;
}

// wpn-weapon-foley has gear-lower-pistol-leg-* for -military / -jeans / -leather and gear-upper-pistol-* for -cotton / -wind / -skin / -parka
// (bank cue tables, tools/sp_parity/bank_index.py); any other suffix (the Seraphite torso's -leather) -> the nearest that exists
static const char *SasfxSuffixOk(const char *s, int upper)
{
    static const char *const LO[] =
        { "-military", "-jeans", "-leather" }, *const UP[] =
        { "-cotton", "-wind", "-skin", "-parka" };
    const char *const *t = upper ? UP : LO;
    int n = upper ? 4 : 3;
    for (int i = 0; s && i < n; i++)
        if (!strcmp(s, t[i]))
            return t[i];
    if (upper && s && !strcmp(s, "-leather"))
        return "-parka";        // Seraphite leather coat: the heavy-coat rustle
    return t[0];
}

// a body's dc entity (play-sound-at-joint's second arg): our player = SID_PLAYER, a puppet = its net record's +0x60 handle
static uint64_t SasfxEnt(const uint8_t *obj, int own, int *slot)
{
    *slot = -1;
    if (own)
        return SID_PLAYER;
    uint32_t me = *(uint32_t *) T2R_SYM(kMySlot);
    for (uint32_t s = 0; s < T2R_MAX_PEERS; s++) {
        uint8_t *peer = s != me ? T2rPeer(s) : NULL;
        uint64_t h = peer ? *(uint64_t *) (peer + 0x60) : 0;
        if (h
            && (const uint8_t *) ((void *(*)(uint64_t))
                                  T2R_SYM(kFnEntityFromHandle))(h) ==
            obj) {
            *slot =(int) s;
            return h;
        }
    }
    return 0;
}

// a sound process handle {slot, id} -> live? (+ the process's set-gain field +0x170: FLT_MAX = never set; set by FireEffect from the "vol" param)
typedef struct {
    float gain, pos[3];
    uint32_t mode, voice;
    int playing, started;
} SasfxPinfo;
static int SasfxProcLive(uint64_t h, uint32_t id, SasfxPinfo *pi)
{
    const uint8_t *p = (const uint8_t *) h;
    if (!p || !id || IsBadReadPtr(p + 8, 8))
        return 0;
    const uint8_t *o = *(const uint8_t * const *) (p + 8);
    if (!o || IsBadReadPtr(o, 0x1d8)
        || *(const uint32_t *) (o + 0x94) != id)
        return 0;
    pi->gain = *(const float *) (o + 0x170);
    pi->mode = *(const uint32_t *) (o + 0x1c0);
    pi->voice = *(const uint32_t *) (o + 0x1d0);        // the process update stores PlaySfx's voice id here (0x141dd836d)
    pi->started = (int) (*(const uint64_t *) (o + 0x180) & 1);  // set when the update starts the sound (0x141dd824a)
    memcpy(pi->pos, o + 0x100, sizeof pi->pos);
    uint8_t *exe = (uint8_t *) GetModuleHandleW(NULL);
    void *mgr = ((void *(*)(int, int))(exe + 0x1270a80))(0xb, 0);       // the audio manager (subsystem table [0xb])
    pi->playing = mgr && pi->voice ? ((uint8_t(*)(void *, uint32_t)) (exe + 0x1a8fa70)) (mgr, pi->voice) : 0;   // AudioManager voice live?
    return 1;
}

static const char *SfxdCullWhy(uint32_t c)
{
    switch (c) {
    case 1:
        return "out of range (distance m / max range + 3 m)";
    case 2:
        return "out of vertical range (height m / range m)";
    case 3:
        return
            "too quiet (level dB / the category's stem-LOD threshold dB)";
    case 5:
        return "audio off";
    case 8:
        return "no sfx-info / params";
    case 0xb:
        return "owner object gone";
    default:
        return "other";
    }
}

// a process pointer the fallback stub recorded -> its sound name (+0x1d8) and whether it is on our own player (owner handle +0x198)
static int SjfDescribe(const uint8_t *p, char *name, size_t nn,
                        const char **who)
{
    *who = "?";
    name[0] = 0;
    if (!p || IsBadReadPtr(p, 0x260))
        return 0;
    const char *w = SfxwName(p + 0x1d8, name, nn);
    if (w)
        snprintf(name, nn, "%s", w);
    else if (strlen(name) < 4)
        snprintf(name, nn, "sound %016llx",
                 (unsigned long long) *(const uint64_t *) (p + 0x1d8 +
                                                           0x80));
    const uint8_t *h = *(const uint8_t * const *) (p + 0x198);
    uint32_t id = *(const uint32_t *) (p + 0x1a0);
    if (h && id && !IsBadReadPtr(h + 8, 8)) {
        const uint8_t *o = *(const uint8_t * const *) (h + 8);
        if (o && !IsBadReadPtr(o + 0x94, 4)
            && *(const uint32_t *) (o + 0x94) == id)
            *who = o == BpMe()? "our own player" : "another body";
    }
    return 1;
}

static void ClipfxSidearmTick(void)
{
    static struct {
        uint64_t at, h, joint;
        uint32_t id;
        int kind, own;
    } chk[8];
    static struct {
        const uint8_t *obj;
        int kind;
        uint64_t t60;
    } last[8];
    static int nlog, nchk, nfb, nd;
    static LONG fb_seen;
    uint8_t ms = MatchSubstate();
    for (int i = 0; i < 16; i++) {
        SasfxEv *e = &g_sasq[i];
        if (!e->ready)
            continue;
        MemoryBarrier();
        int kind = e->kind, own = e->own;
        uint8_t *obj = e->obj;
        uint64_t eh = e->eh, ejoint = e->joint;
        uint32_t eid = e->eid;
        MemoryBarrier();
        e->ready = 0;
        if (ms < 3 || ms > 7 || kind < 0 || kind >= SASFX_NK)
            continue;
        if (eh && eid && nchk < 32)     // diagnosis: is the engine's process still there 100 ms later
            for (int j = 0; j < 8; j++)
                if (!chk[j].at) {
                    chk[j].at = g_t60 + 6;
                    chk[j].h = eh;
                    chk[j].id = eid;
                    chk[j].kind = kind;
                    chk[j].own = own;
                    chk[j].joint = ejoint;
                    break;
                }
        if (!SasfxReplayOn())
            continue;           // (opt-in since the joint fallback: the engine plays these itself)
        if (!own && (eh || eid))
            continue;           // the engine built this puppet's sound: nothing to add
        int dup = 0, lk = 0;
        for (int j = 0; j < 8; j++) {
            if (last[j].obj == obj && last[j].kind == kind
                && g_t60 - last[j].t60 < 12)
                dup = 1;
            if (last[j].t60 < last[lk].t60)
                lk = j;
        }
        if (dup)
            continue;           // (one sound per event: the -part / -add layers carry the same clip)
        int slot;
        uint64_t ent = SasfxEnt(obj, own, &slot);
        if (!ent)
            continue;           // not a player body (menu scene NPCs ...)
        last[lk].obj = obj;
        last[lk].kind = kind;
        last[lk].t60 = g_t60;
        char name[96];
        const char *joint = "r_wrist";
        if (kind < 2)
            snprintf(name, sizeof name, "%s", SASFX_NAMES[kind].name);
        else {
            int upper = kind >= 4;
            const char *suf = SasfxSuffix(obj, upper);
            snprintf(name, sizeof name, "sfx-%s%s", SASFX_NAMES[kind].name,
                     SasfxSuffixOk(suf, upper));
            if (!upper)
                joint = "r_upper_leg";
        }
        uint64_t a[5] = { Fnv64Rt(name), ent, Fnv64Rt(joint), 0, 0 };
        uint64_t r = DcCall(FNV64("play-sound-at-joint"), 5, a);
        if (nlog++ < 40) {
            char who[32];
            if (own)
                snprintf(who, sizeof who, "our own player");
            else
                snprintf(who, sizeof who, "puppet slot %d", slot);
            Xlog("sidearm sfx: replay %s on %s at %s -> %llx (engine handle %llx/%x)", name, who, joint, (unsigned long long) r, (unsigned long long) eh, eid);
        }
    }
    for (int j = 0; j < 8; j++) {
        if (!chk[j].at || g_t60 < chk[j].at)
            continue;
        SasfxPinfo pi = { 0 };
        int live = SasfxProcLive(chk[j].h, chk[j].id, &pi);
        if (nchk++ < 32) {
            const char *who =
                chk[j].own ? "our own player" : "another body", *jn =
                SasfxJointName(chk[j].joint);
            if (live)
                Xlog("sidearm sfx: engine process (%s at %s) on %s 100 ms later: live, position mode %u%s; started %d, voice %x %s, " "at (%.2f %.2f %.2f), gain %g", SASFX_NAMES[chk[j].kind].name, jn, who, pi.mode, pi.mode == 1 && chk[j].joint ? " (the joint is not on this skeleton: plays at the body via the joint fallback)" : "", pi.started, pi.voice, pi.playing ? "playing" : "NOT playing", pi.pos[0], pi.pos[1], pi.pos[2], pi.gain);
            else
                Xlog("sidearm sfx: engine process (%s at %s) on %s 100 ms later: gone", SASFX_NAMES[chk[j].kind].name, jn, who);
        }
        chk[j].at = 0;
    }
    if (g_sjfCnt && *g_sjfCnt != fb_seen) {   // the joint fallback kept a process the engine would have ended
        LONG n = *g_sjfCnt;
        const uint8_t *p = (const uint8_t *) *g_sjfLast;
        fb_seen = n;
        char nm[72];
        const char *who;
        if (nfb < 40 && SjfDescribe(p, nm, sizeof nm, &who)) {
            nfb++;
            Xlog("sfx joint fallback #%ld: %s on %s - its joint is not on this skeleton: plays at the body (unpatched: the process ended on its " "first frame, silent)", (long) n, nm, who);
        }
    }
    for (int i = 0; i < 32; i++) {      // refused spawns / failed plays of the pistol holster foley (job threads)
        SfxdEv *e = &g_sfxd[i];
        if (!e->ready)
            continue;
        MemoryBarrier();
        if (nd < 240) {
            nd++;
            if (e->kind == 0)
                Xlog("sfx spawn culled: %s - code %u %s: %.2f / %.2f",
                     e->name, e->code, SfxdCullWhy(e->code), e->v, e->l);
            else if (e->kind == 1)
                Xlog("sfx play failed: %s - PlaySfx result %u (no voice: the process ends)", e->name, e->code);
            else
                Xlog("sfx play started: %s - voice %x", e->name, e->code);
        }
        MemoryBarrier();
        e->ready = 0;
    }
}
