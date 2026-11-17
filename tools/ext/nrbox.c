// nrbox.c - No Return's supply crate (actor rl-cache-crate-military) over every MP supply box. Included from t2r-ext.c after interrogation.c.
// Data + the whole analysis: re/nr-supply-box.md, tools/maps/data/nr_supply_box.json.
//
// The MP box is winmm's: a level spawner `to1-supplybox-N` drawn with the actor `pickup-chest` (0.55 x 0.23 x 0.27 m, hinge animation
// idle / open / close) and 12 `-itempos-M` spawners; winmm's table (0x18013A4A8, 0x48 per box: +0 spawner hash, +0x10 next-available time,
// +0x14 opened flag, +0x18/+0x20 itempos list) runs the open (FUN_18004ee40: event `open` to the spawner, player-force-crouch,
// t2r-abby-scav-low-crate-open-start, sfx-t2r-igc-loot-crate-open) and the refill. All of that stays: this file only DRESSES each box
// with No Return's crate prop at the same spot and mirrors the box state onto it:
//   opened flag 0 -> 1 : animate `open` on the crate (NR clip: 15 frames = 0.5 s, lid ~70 degrees about the back edge, latch 12 degrees)
//   opened flag 1 -> 0 : animate `close` (16 frames = 0.53 s)
// The crate is 1.31 x 0.49 x 0.49 m (the chest 0.55 x 0.23 x 0.27), so the little chest sits inside it, hidden when closed.
// Needs the actors rl-cache-crate-military + rl-cache-crate-skel in the MP level (tools/features/dc_data_patches.py MP_LEVEL_ADDS,
// T2R_NRBOX=0 leaves them out); a failed spawn (actor not loaded) is logged once and the stock chest stays as it is.
// The stock chest is hidden every frame (hide-object-for-one-frame on its spawner, placedbomb2013.c recipe) and the box's itempos spawners are
// moved into the crate's frame (NR's 4 lining offsets, then a 4 x 3 grid), so the loot lies IN the crate. The crate's own Havok collision
// (collision.rl-cache-crate-military, 4 convex shapes) comes with the actor. Delayed: backpack insert sound + unlock-crate-haptic.
// t2r-old-supplybox.txt next to the exe = off (the stock chest only).
//
// (nrbox3, SP trace re/nr-supply-box-trace-1102.md): the open now follows SP's timeline, driven from the open EDGE, not a poll:
//   press (E) -> reach gesture (supplybox.c) -> +0.35 s the box opens (supplybox.c BOXG_OPEN_MS): boxfill_hook(from_open) calls nrbox_open_start:
//   crate `open` clip + the three crate sounds (hinge@lid, latch@latch, lid@root, on the CRATE, not the player) in the same frame; the loot is held
//   hidden until +0.12 s (the lid is moving), the unlock-crate-haptic comes at +0.12 s, the backpack insert sound at +0.37 s (all measured from the anim
//   start). A box another player opened (flag edge seen here, polled EVERY frame now) gets the same anim + sounds + reveal at the edge (no backpack
//   sound / haptic: not our body). Floating marker (listen-mode-visible + buddy-draw 1, SP's two natives) on a closed crate, off at the open, on again at the refill.
//   t2r-old-crate-open.txt next to the exe = the earlier behaviour (15-frame poll, sounds on the player, hub-style delays, no marker / hidden loot).
//   t2r-no-crate-marker.txt = no marker. t2r-crate-clip-sounds.txt = do not play the three sounds ourselves (the open clip's own events do it).
#define NRB_MAX 16
static struct {
    uint64_t hash, obj;
    uint8_t open;
    int tried;
    float pos[4], q[4];
    uint64_t loot_at;
    uint64_t reveal_ms;
    int marker;
    uint8_t *e;
    int kin, kin_tries;
    uint32_t fix_n;
    uint64_t anim_until;
    int flr;
    int pn;
    uint64_t ph[32];
    float pp[32][4], pq[32][4];
} g_nrb[NRB_MAX];
static int g_nrbN;             // crates standing
static uint64_t g_nrb_snd_at, g_nrb_hap_at;     // frames (old mode) / ms (new mode) of the delayed backpack-insert sound / haptic after our own box open
static int g_nrbMs;            // 1 = g_nrb_*_at are GetTickCount64 ms (new timeline)
static int NrboxFlag(const char *name, int *cache, uint64_t *at)
{
    uint64_t now = GetTickCount64();
    if (*cache < 0 || now - *at > 2000) {
        FILE *f = ExeSideOpen(name, "r");
        *cache = f != NULL;
        if (f)
            fclose(f);
        *at = now;
    }
    return *cache;
}

static int NrboxOldOpen(void)
{
    static int c = -1;
    static uint64_t at;
    return NrboxFlag("t2r-old-crate-open.txt", &c, &at);
}

// crate-local lining offsets: NR's 4 (arr-lockbox-pickup-offsets), then 8 grid points (ours) for the other itempos slots
#include "nrbox_slots.h"        // NRB_LOC / NRB_LQ / NRB_KQ: tools/maps/crate_slots.py from data/crate-slots.json (SceneEditor: supply box > Edit loot slots)
static void NrbQmul(const float *a, const float *b, float *o)
{                               // o = a * b (crate rotation * slot rest rotation)
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

static void NrboxAfterOpen(void)
{                               // OLD mode, called from box_open_sound (the local player opened a box)
    if (!g_nrbN)
        return;
    g_nrbMs = 0;
    g_nrb_snd_at = g_frame + 45;
    g_nrb_hap_at = g_frame + 36;        // SP: scav-insert-item ~0.7 s after the crate sounds; haptic at box-opened (~0.6 s)
}

static void NrboxPerFrame(void)
{                               // hide the stock chests, delayed sounds
    if (!g_nrbN)
        return;
    for (int i = 0; i < NRB_MAX; i++)
        if (g_nrb[i].obj && g_nrb[i].hash) {
            uint64_t a[2] = { g_nrb[i].hash, 0 };
            uint64_t r = DcCall(FNV64("hide-object-for-one-frame"), 1, a);
            static int n;
            if (n++ < 3)
                Xlog("nrbox: hide-object-for-one-frame(stock chest %llx) -> %llx", (unsigned long long) g_nrb[i].hash, (unsigned long long) r);
        }
    uint64_t cur = g_nrbMs ? GetTickCount64() : g_frame;
    if (g_nrb_snd_at && cur >= g_nrb_snd_at) {
        g_nrb_snd_at = 0;
        uint64_t a[5] =
            { FNV64("sfx-backpack-abby-scav-insert-item"), SID_PLAYER,
    FNV64("spineb"), 0, 0 };
        uint64_t r = DcCall(FNV64("play-sound-at-joint"), 5, a);
        static int n;
        if (LOGCAP(n, 6))
            Xlog("nrbox: backpack insert sound -> %llx",
                 (unsigned long long) r);
    }
    if (g_nrb_hap_at && cur >= g_nrb_hap_at) {
        g_nrb_hap_at = 0;
        uint64_t a[3] = { FNV64("unlock-crate-haptic"), 0, 2 };
        uint64_t r = DcCall(FNV64("play-hfx-id"), 3, a);
        static int n;
        if (LOGCAP(n, 6))
            Xlog("nrbox: unlock-crate-haptic -> %llx",
                 (unsigned long long) r);
    }
}

static void NrboxMoveSlots(uint8_t *e, const float *pos, const float *q)
{                               // itempos spawners -> crate frame
    uint64_t *b = *(uint64_t **) (e + 0x18), *en =
        *(uint64_t **) (e + 0x20);
    if (!b || en <= b || (en - b) > 16
        || IsBadReadPtr(b, (size_t) ((uint8_t *) en - (uint8_t *) b)))
        return;
    static __attribute__((aligned(16))) float tp[16][4], tq[16][4];
    for (int k = 0; k < en - b; k++) {
        const float *v = NRB_LOC[k % 12];
        // v' = v + 2w(u x v) + 2 u x (u x v), u = (qx qy qz)
        float ux = q[0], uy = q[1], uz = q[2], w = q[3];
        float cx = uy * v[2] - uz * v[1], cy = uz * v[0] - ux * v[2], cz =
            ux * v[1] - uy * v[0];
        float dx = uy * cz - uz * cy, dy = uz * cx - ux * cz, dz =
            ux * cy - uy * cx;
        tp[k][0] = pos[0] + v[0] + 2 * (w * cx + dx);
        tp[k][1] = pos[1] + v[1] + 2 * (w * cy + dy);
        tp[k][2] = pos[2] + v[2] + 2 * (w * cz + dz);
        tp[k][3] = 1.0f;
        NrbQmul(q, NRB_LQ[k % 12], tq[k]);
        uint64_t bfa[5] = { (uint64_t) tp[k], (uint64_t) tq[k], 0, 0, 0 };
        uint64_t bf =
            DcCall(0x871ec52cc86b7427ULL /* alloc-bound-frame */ , 5,
                    bfa);
        uint64_t la[2] = { b[k], bf };
        uint64_t r = bf
            && bf != ~0ULL ? DcCall(FNV64("set-object-locator"), 2,
                                     la) : 0;
        static int n;
        if (LOGCAP(n, 14))
            Xlog("nrbox: itempos %d -> %.2f %.2f %.2f (set-object-locator -> %llx)", k, tp[k][0], tp[k][1], tp[k][2], (unsigned long long) r);
    }
}

static int NrboxOff(void)
{
    static int off = -1;
    static uint64_t at;
    uint64_t now = GetTickCount64();
    if (off < 0 || now - at > 2000) {
        FILE *f = ExeSideOpen("t2r-old-supplybox.txt", "r");
        off = f != NULL;
        if (f)
            fclose(f);
        at = now;
    }
    return off;
}

static void NrboxForget(void)
{                               // level change / match over: the engine already freed the props
    memset(g_nrb, 0, sizeof g_nrb);
    g_nrbN = 0;
}

static uint64_t NrboxSpawn(int i, uint64_t spawner_hash, uint8_t *e)
{
    uint64_t sobj =
        ((uint64_t(*)(uint64_t)) T2R_SYM(kFnLocate)) (spawner_hash);
    if (!sobj)
        return 0;
    float pos[4] = { 0 }, rot[4] = { 0, 0, 0, 1 };
    ((void (*)(float *, uint64_t)) T2R_SYM(kFnObjectPosition)) (pos, sobj);
    ((void (*)(float *, uint64_t)) T2R_SYM(kFnObjectRotation)) (rot, sobj);
    if (!(pos[0] * pos[0] + pos[1] * pos[1] + pos[2] * pos[2] > 1.0f))
        return 0;               // a spawner still at the origin: not placed yet
    uint64_t a[9] =
        { FNV64("rl-cache-crate-military"), FNV64("SimpleAttachable"), 0,
0, 1, 0, 0, 0, 0 };
    uint64_t o = DcCall(FNV64("spawn-object"), 9, a);
    if (!o || o == ~0ULL) {
        Xlog("nrbox: spawn-object rl-cache-crate-military failed for box %d (actor not in the MP level?) - stock chest stays", i);
        return 0;
    }
    static
        __attribute__((aligned(16))) float tp[NRB_MAX][4], tq[NRB_MAX][4];
    memcpy(tp[i], pos, 12);
    tp[i][3] = 1.0f;
    memcpy(tq[i], rot, 16);
    uint64_t bfa[5] = { (uint64_t) tp[i], (uint64_t) tq[i], 0, 0, 0 };
    uint64_t bf =
        DcCall(0x871ec52cc86b7427ULL /* alloc-bound-frame */ , 5, bfa);
    uint64_t la[2] = { o, bf };
    uint64_t r = bf
        && bf != ~0ULL ? DcCall(FNV64("set-object-locator"), 2, la) : 0;
    Xlog("nrbox: box %d (spawner %llx): NR crate obj %llx at %.2f %.2f %.2f quat %.2f %.2f %.2f %.2f (set-object-locator -> %llx)", i, (unsigned long long) spawner_hash, (unsigned long long) o, pos[0], pos[1], pos[2], rot[0], rot[1], rot[2], rot[3], (unsigned long long) r);
    // solid invisible block (SimpleObject, SimpleAttachable fallback) laid on its side under the crate: invisible-block-050x150x100 (0.5 x 1.5 x 1.0, origin = corner) with its
    // 1.5 m axis along the crate length (1.31 m), 0.5 across (0.49 deep), the 1.0 m axis up (0.5 m taller than the crate; no smaller stock block exists, invisible-cube is 0.19 m).
    {
        static
            __attribute__((aligned(16))) float bp[NRB_MAX][4],
            bq[NRB_MAX][4];
        float qx = rot[0], qy = rot[1], qz = rot[2], qw = rot[3];
        float v[3] = { -0.75f, -0.51f, -0.25f };        // sunk 0.51: the 1.0 m side is vertical, top = 0.49 = the crate top                                    // block corner in the crate frame (crate origin: x/z centre, floor y 0)
        float tx = 2 * (qy * v[2] - qz * v[1]), ty =
            2 * (qz * v[0] - qx * v[2]), tz = 2 * (qx * v[1] - qy * v[0]);
        bp[i][0] = pos[0] + v[0] + qw * tx + (qy * tz - qz * ty);
        bp[i][1] = pos[1] + v[1] + qw * ty + (qz * tx - qx * tz);
        bp[i][2] = pos[2] + v[2] + qw * tz + (qx * ty - qy * tx);
        bp[i][3] = 1.0f;
        float h0[4] = { -0.5f, -0.5f, -0.5f, 0.5f };    // block x->crate z, y->crate x, z->crate y
        bq[i][0] = qw * h0[0] + qx * h0[3] + qy * h0[2] - qz * h0[1];
        bq[i][1] = qw * h0[1] - qx * h0[2] + qy * h0[3] + qz * h0[0];
        bq[i][2] = qw * h0[2] + qx * h0[1] - qy * h0[0] + qz * h0[3];
        bq[i][3] = qw * h0[3] - qx * h0[0] - qy * h0[1] - qz * h0[2];
        uint64_t bo =
            LbSpawnSolid("invisible-block-050x150x100",
                           "nr crate block");
        if (bo) {
            uint64_t bfb[5] =
                { (uint64_t) bp[i], (uint64_t) bq[i], 0, 0, 0 };
            uint64_t bbf = DcCall(0x871ec52cc86b7427ULL, 5, bfb);
            uint64_t lb2[2] = { bo, bbf };
            uint64_t rb = bbf
                && bbf != ~0ULL ? DcCall(FNV64("set-object-locator"), 2,
                                          lb2) : 0;
            Xlog("nrbox: box %d block bounds: corner %.2f %.2f %.2f, 1.5 (crate length) x 0.5 (depth) x 1.0 (up) in the crate frame, top y %.2f (crate top %.2f)", i, (double) bp[i][0], (double) bp[i][1], (double) bp[i][2], (double) (pos[1] + 0.49f), (double) (pos[1] + 0.49f));
            Xlog("nrbox: box %d collision block obj %llx (set-object-locator -> %llx)", i, (unsigned long long) bo, (unsigned long long) rb);
        }
    }
    memcpy(g_nrb[i].pos, pos, 16);
    memcpy(g_nrb[i].q, rot, 16);
    NrboxMoveSlots(e, pos, rot);
    return o;
}

// A/B: winmm places the box's pickups from positions it took before nrbox moved the itempos spawners, so the loot lay in the grass in
// front of the crate. After the open, the box's own spawned list (record +0x30, std::vector of handles, as b13_fill_end fills it) is
// moved onto the lining: slot k = NRB_LOC[k] in the crate frame. Run at the open edge and again 15 and 45 frames later (late spawns).
static const char *const NRB_KINDN[5] =
    { "ammo", "ingredient", "throwable", "health", "melee" };
// melee pickup model boxes (weapon pak AABBs, model frame: pivot at the grip end, weapon along +z): name, min xyz, max xyz. Interior of the crate (crate-local) used for the warning.
// measured from the rl-cache-crate-military mesh (LOD0 face normals): inner floor y 0.335, inner end walls x -0.60 / +0.59, inner back wall z -0.21, inner front wall z +0.20 (1.19 x 0.41 m; the longest weapon, 1.0 m, fits straight)
#define NRB_INT_X0 -0.60f
#define NRB_INT_X1 0.59f
#define NRB_INT_Z0 -0.21f
#define NRB_INT_Z1 0.20f
#define NRB_INT_Y0 0.335f
static const struct {
    const char *n;
    float lo[3], hi[3];
} NRB_MELEE_BB[] = {
    { "two-by-four", { -0.035f, -0.150f, -0.056f},
     { 0.029f, 0.076f, 0.880f} }, { "baseball-bat", { -0.046f, -0.121f,
                                                     -0.074f}, { 0.046f,
                                                                0.139f,
                                                                0.882f} },
{ "lead-pipe", { -0.035f, -0.106f, -0.052f}, { 0.031f, 0.194f, 0.879f} },
    { "crowbar", { -0.027f, -0.068f, -0.191f}, { 0.027f, 0.107f, 0.542f} },
        { "machete", { -0.016f, -0.032f, -0.110f},
         { 0.016f, 0.023f, 0.527f} }, { "machete-scar", { -0.013f, -0.042f,
                                                         -0.135f},
                                       { 0.013f, 0.037f, 0.512f} },
    { "hatchet", { -0.027f, -0.126f, -0.120f}, { 0.027f, 0.065f, 0.507f} },
        { "hatchet-modern", { -0.027f, -0.126f, -0.120f},
         { 0.027f, 0.065f, 0.507f} }, { "tomahawk", { -0.027f, -0.126f,
                                                     -0.120f}, { 0.027f,
                                                                0.065f,
                                                                0.507f} },
    { "fireman-axe", { -0.026f, -0.172f, -0.023f},
     { 0.030f, 0.161f, 0.887f} }, { "pipe-wrench", { -0.025f, -0.132f,
                                                    -0.087f}, { 0.026f,
                                                               0.111f,
                                                               0.373f} },
{ "hammer-modern", { -0.031f, -0.135f, -0.061f}, { 0.025f, 0.116f, 0.339f} },
    { "hammer-scar", { -0.033f, -0.134f, -0.079f},
     { 0.028f, 0.145f, 0.345f} }, { "club-1h-police", { -0.026f, -0.038f,
                                                       -0.056f}, { 0.028f,
                                                                  0.028f,
                                                                  0.480f}
                                   }, { "club-1h-scar", { -0.062f, -0.070f,
                                                         -0.121f},
                                       { 0.061f, 0.057f, 0.408f} },
    { "club-2h-scar", { -0.078f, -0.066f, -0.076f},
     { 0.070f, 0.061f, 0.926f} }, { "short-pipe", { -0.033f, -0.131f,
                                                   -0.190f}, { 0.037f,
                                                              0.127f,
                                                              0.476f} },
};

// the weapon's model box rotated by the slot quaternion q (xyzw) into the crate frame; 1 = known weapon
static int NrbMeleeExtent(const char *wn, const float *q, float *lo,
                            float *hi)
{
    for (size_t t = 0; t < sizeof NRB_MELEE_BB / sizeof *NRB_MELEE_BB; t++) {
        if (strcmp(NRB_MELEE_BB[t].n, wn))
            continue;
        for (int a = 0; a < 3; a++) {
            lo[a] = 1e9f;
            hi[a] = -1e9f;
        }
        for (int c = 0; c < 8; c++) {
            float v[3] =
                { (c & 1 ? NRB_MELEE_BB[t].hi : NRB_MELEE_BB[t].lo)[0],
           (c & 2 ? NRB_MELEE_BB[t].hi : NRB_MELEE_BB[t].lo)[1],
           (c & 4 ? NRB_MELEE_BB[t].hi : NRB_MELEE_BB[t].lo)[2] };
            float ux = q[0], uy = q[1], uz = q[2], w = q[3];
            float cx = uy * v[2] - uz * v[1], cy =
                uz * v[0] - ux * v[2], cz = ux * v[1] - uy * v[0];
            float dx = uy * cz - uz * cy, dy = uz * cx - ux * cz, dz =
                ux * cy - uy * cx;
            float r[3] =
                { v[0] + 2 * (w * cx + dx), v[1] + 2 * (w * cy + dy),
           v[2] + 2 * (w * cz + dz) };
            for (int a = 0; a < 3; a++) {
                if (r[a] < lo[a])
                    lo[a] = r[a];
                if (r[a] > hi[a])
                    hi[a] = r[a];
            }
        }
        return 1;
    }
    return 0;
}

static int NrbSlotKind(int k)
{
    return k == 0 ? 1 : k == 1 ? 0 : k == 2 ? 3 : k == 3 ? 4 : k ==
        4 ? 2 : -1;
}                               // B13_K_* -> NRB_KQ index (ammo 0, ingredient 1, throwable 2, health 3, melee 4)

// layout: the crate has ONE melee slot (the weapon lies along the long axis in the lower half), 'item' slots in the upper half and 'spare' slots (lower half) that are used
// only when the box has no melee weapon or more items than item slots. The kind of each loot handle comes from box_spawn (box_kind_of). Logged per open.
// crate-local position of a loot item in slot sl (the slot, a second layer 3 cm up, a melee weapon fitted inside the interior)
static void NrbLocalPose(int i, int sl, int layer, int kind, uint64_t it,
                           uint64_t handle, float *vv)
{
    const float *v0 = NRB_LOC[sl];
    vv[0] = v0[0];
    vv[1] = v0[1] + 0.03f * (float) layer;
    vv[2] = v0[2];
    // (weapons sometimes lay beyond the crate): the melee slot was authored near the crate's left end (x -0.40) for a short weapon. Every melee model's pivot sits at its
    // grip end and the blade / head runs along +z (AABBs measured from the weapon paks, NRB_MELEE_BB), but the grip end reaches 0.05 - 0.19 m BEHIND the pivot and the lengths differ
    // (0.40 - 1.00 m). The model box is rotated by the slot quaternion into the crate frame and the weapon is centred on the crate's long axis (and pulled inside in depth).
    if (kind == 3 && NRB_ROLE[sl] == 1) {
        if (!it)
            for (int j = 0; j < 64; j++)
                if (g_box_kind[j].h == handle)
                    it = g_box_kind[j].item;
        const char *wn = it ? B13Name(it) : "?";
        float lo[3] = { 0, 0, 0 }, hi[3] = { 0, 0, 0 };
        int known = NrbMeleeExtent(wn, NRB_LQ[sl], lo, hi);
        if (known) {
            vv[0] =
                0.5f * (NRB_INT_X0 + NRB_INT_X1) - 0.5f * (lo[0] + hi[0]);
            if (v0[2] + lo[2] < NRB_INT_Z0)
                vv[2] = NRB_INT_Z0 - lo[2];
            if (v0[2] + hi[2] > NRB_INT_Z1)
                vv[2] = NRB_INT_Z1 - hi[2];
            if (vv[1] + lo[1] < NRB_INT_Y0)
                vv[1] = NRB_INT_Y0 - lo[1];
            if (vv[0] + hi[0] > NRB_INT_X1 || vv[0] + lo[0] < NRB_INT_X0
                || vv[2] + lo[2] < NRB_INT_Z0 - 0.001f
                || vv[2] + hi[2] > NRB_INT_Z1 + 0.001f)
                Xlog("nrbox: WARNING box %d melee %s world bounds x %.2f..%.2f z %.2f..%.2f are OUTSIDE the crate interior (x %.2f..%.2f, z %.2f..%.2f)", i, wn, (double) (vv[0] + lo[0]), (double) (vv[0] + hi[0]), (double) (vv[2] + lo[2]), (double) (vv[2] + hi[2]), (double) NRB_INT_X0, (double) NRB_INT_X1, (double) NRB_INT_Z0, (double) NRB_INT_Z1);
        }
        static int nl;
        if (LOGCAP(nl, 20))
            Xlog("nrbox: box %d melee %s (%s) crate-local bounds x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f vs interior x %.2f..%.2f z %.2f..%.2f (slot x %.2f z %.2f -> %.2f %.2f)", i, wn, known ? "model box" : "UNKNOWN weapon, slot kept", (double) (vv[0] + lo[0]), (double) (vv[0] + hi[0]), (double) (vv[1] + lo[1]), (double) (vv[1] + hi[1]), (double) (vv[2] + lo[2]), (double) (vv[2] + hi[2]), (double) NRB_INT_X0, (double) NRB_INT_X1, (double) NRB_INT_Z0, (double) NRB_INT_Z1, (double) v0[0], (double) v0[2], (double) vv[0], (double) vv[2]);
    }
}

static void NrbWorldPose(int i, const float *vv, int sl, int kind,
                           float *tp, float *tq)
{
    const float *pos = g_nrb[i].pos, *q = g_nrb[i].q;
    float ux = q[0], uy = q[1], uz = q[2], w = q[3];
    float cx = uy * vv[2] - uz * vv[1], cy = uz * vv[0] - ux * vv[2], cz =
        ux * vv[1] - uy * vv[0];
    float dx = uy * cz - uz * cy, dy = uz * cx - ux * cz, dz =
        ux * cy - uy * cx;
    tp[0] = pos[0] + vv[0] + 2 * (w * cx + dx);
    tp[1] = pos[1] + vv[1] + 2 * (w * cy + dy);
    tp[2] = pos[2] + vv[2] + 2 * (w * cz + dz);
    tp[3] = 1.0f;
    float sq[4];
    NrbQmul(q, NRB_LQ[sl], sq);        // crate * slot
    int kq = NrbSlotKind(kind);
    if (kq >= 0)
        NrbQmul(sq, NRB_KQ[kq], tq);
    else
        memcpy(tq, sq, 16);     // * the extra rotation of the item kind (NRB_KQ)
}

// (~6 items of an opened box lay on the floor at the old spots): winmm spawns the box pickups at positions it took before our itempos relocation (the stock script's
// transform record, not the itempos objects), so they appeared there and were only teleported into the crate afterwards. The pickup spawn hook (spawn_pickup_hook) now asks nrbox_place_first
// for the slot pose BEFORE the engine spawn and rewrites the transform record's locator + quat, so a pickup is created inside the crate in its first frame. The slot rules are the move_loot ones
// (weapon on the melee slot, items on the item slots, spares only without a weapon / with more items than item slots, extras layered 3 cm up).
static struct {
    uint8_t *rec;
    int ni, nsp, over, has_melee, melee_used, last_i, last_ok;
    float last_p[4], last_q[4];
} g_nrbf;
static int NrboxFindI(const uint8_t *rec)
{
    for (int i = 0; i < NRB_MAX; i++)
        if (g_nrb[i].obj && g_nrb[i].hash && rec
            && *(const uint64_t *) rec == g_nrb[i].hash)
            return i;
    return -1;
}

static void NrboxFillBegin(uint8_t *rec)
{
    memset(&g_nrbf, 0, sizeof g_nrbf);
    g_nrbf.rec = rec;
    g_nrbf.last_i = -1;
    for (int j = 0; j < g_b13.n; j++)
        if (g_b13.e[j].kind == 3)
            g_nrbf.has_melee = 1;
    int i = NrboxFindI(rec);
    if (i >= 0)
        g_nrb[i].pn = 0;
}

// xf = the 80-byte BoundFrame copy about to be spawned with; 1 = its locator + quat now hold the crate slot pose
static int NrboxPlaceFirst(int kind, uint64_t item, float *xf)
{
    g_nrbf.last_ok = 0;
    if (NrboxOff() || NrboxOldOpen() || !g_nrbf.rec
        || *(uint32_t *) xf != 0x7fccccccu)
        return 0;
    int i = NrboxFindI(g_nrbf.rec);
    if (i < 0 || g_nrb[i].pn >= 32)
        return 0;
    int melee_slot = -1, nitem = 0, nspare = 0, items[12], spares[12];
    for (int sl = 0; sl < 12; sl++) {
        if (NRB_ROLE[sl] == 1 && melee_slot < 0)
            melee_slot = sl;
        else if (NRB_ROLE[sl] == 2)
            spares[nspare++] = sl;
        else if (NRB_ROLE[sl] == 0)
            items[nitem++] = sl;
    }
    int sl, layer = 0;
    if (kind == 3 && melee_slot >= 0 && !g_nrbf.melee_used) {
        sl = melee_slot;
        g_nrbf.melee_used = 1;
    } else if (g_nrbf.ni < nitem)
        sl = items[g_nrbf.ni++];
    else if (g_nrbf.nsp < nspare
             && (!g_nrbf.has_melee || g_nrbf.ni >= nitem))
        sl = spares[g_nrbf.nsp++];
    else {
        sl = g_nrbf.over++ % 12;
        layer = 1;
    }
    float vv[3];
    NrbLocalPose(i, sl, layer, kind, item, 0, vv);
    float tp[4], tq[4];
    NrbWorldPose(i, vv, sl, kind, tp, tq);
    memcpy(xf + 12, tp, 16);
    memcpy(xf + 16, tq, 16);
    memcpy(g_nrbf.last_p, tp, 16);
    memcpy(g_nrbf.last_q, tq, 16);
    g_nrbf.last_i = i;
    g_nrbf.last_ok = 1;
    return 1;
}

static void NrboxNoteSpawned(uint64_t h)
{                               // the handle the engine returned for the pickup nrbox_place_first just posed
    if (!g_nrbf.last_ok)
        return;
    g_nrbf.last_ok = 0;
    int i = g_nrbf.last_i;
    if (i < 0 || !h || h == ~0ULL || g_nrb[i].pn >= 32)
        return;
    int k = g_nrb[i].pn++;
    g_nrb[i].ph[k] = h;
    memcpy(g_nrb[i].pp[k], g_nrbf.last_p, 16);
    memcpy(g_nrb[i].pq[k], g_nrbf.last_q, 16);
}

// The one safety pass after the open (frames 1 / 2 / 8 / 15 / 45 also switch late physics bodies to kinematic): every pickup still alive must sit inside the crate interior where it was
// spawned; one that does not is an ERROR (logged with both positions) and is put back.
static void NrboxVerifyLoot(int i, const uint64_t *b, int n)
{
    static __attribute__((aligned(16))) float tp[4], tq[4];
    (void) b;
    (void) n;
    for (int k = 0; k < g_nrb[i].pn; k++) {
        uint64_t h = g_nrb[i].ph[k];
        uint64_t obj = ((uint64_t(*)(uint64_t)) T2R_SYM(kFnLocate)) (h);
        if (!obj)
            continue;           // picked up already
        float cur[4] = { 0 };
        ((void (*)(float *, uint64_t)) T2R_SYM(kFnObjectPosition)) (cur,
                                                                    obj);
        const float *ex = g_nrb[i].pp[k];
        float dx = cur[0] - ex[0], dy = cur[1] - ex[1], dz =
            cur[2] - ex[2];
        // crate-local position (inverse crate rotation)
        const float *q = g_nrb[i].q;
        float rx = cur[0] - g_nrb[i].pos[0], ry =
            cur[1] - g_nrb[i].pos[1], rz = cur[2] - g_nrb[i].pos[2];
        float ux = -q[0], uy = -q[1], uz = -q[2], w = q[3];
        float cx = uy * rz - uz * ry, cy = uz * rx - ux * rz, cz =
            ux * ry - uy * rx;
        float lx = rx + 2 * (w * cx + (uy * cz - uz * cy)), ly =
            ry + 2 * (w * cy + (uz * cx - ux * cz)), lz =
            rz + 2 * (w * cz + (ux * cy - uy * cx));
        int outside = lx < NRB_INT_X0 - 0.05f || lx > NRB_INT_X1 + 0.05f
            || lz < NRB_INT_Z0 - 0.05f || lz > NRB_INT_Z1 + 0.05f
            || ly < NRB_INT_Y0 - 0.1f || ly > 0.7f;
        if (outside || dx * dx + dy * dy + dz * dz > 0.05f * 0.05f) {
            static int ne;
            if (LOGCAP(ne, 40))
                Xlog("nrbox: ERROR box %d loot %llx is %s the crate: at %.2f %.2f %.2f (crate-local %.2f %.2f %.2f), spawned for %.2f %.2f %.2f - put back", i, (unsigned long long) h, outside ? "OUTSIDE" : "off its slot in", (double) cur[0], (double) cur[1], (double) cur[2], (double) lx, (double) ly, (double) lz, (double) ex[0], (double) ex[1], (double) ex[2]);
            memcpy(tp, ex, 16);
            memcpy(tq, g_nrb[i].pq[k], 16);
            uint64_t bfa[5] = { (uint64_t) tp, (uint64_t) tq, 0, 0, 0 };
            uint64_t bf =
                DcCall(0x871ec52cc86b7427ULL /* alloc-bound-frame */ , 5,
                        bfa);
            uint64_t la[2] = { h, bf };
            if (bf && bf != ~0ULL)
                DcCall(FNV64("set-object-locator"), 2, la);
        }
        if (!MdKinOff())
            MdMakeKinematic(h);
    }
}

static void NrboxMoveLoot(int i, uint8_t *e)
{
    uint64_t **v = (uint64_t **) (e + 0x30);
    uint64_t *b = v[0], *en = v[1];
    if (!b || en <= b || (en - b) > 32
        || IsBadReadPtr(b, (size_t) ((uint8_t *) en - (uint8_t *) b)))
        return;
    static __attribute__((aligned(16))) float tp[32][4], tq[32][4];
    int n = (int) (en - b);
    if (n > 32)
        n = 32;
    if (g_nrb[i].pn > 0) {
        NrboxVerifyLoot(i, b, n);
        return;
    }                           // the pickups were spawned straight into their slots (nrbox_place_first): check + kinematic only
    int slot[32], kind[32], used[12] = { 0 }, melee_slot = -1, nitem =
        0, nspare = 0, items[12], spares[12], has_melee = 0;
    for (int s = 0; s < 12; s++) {
        if (NRB_ROLE[s] == 1 && melee_slot < 0)
            melee_slot = s;
        else if (NRB_ROLE[s] == 2)
            spares[nspare++] = s;
        else if (NRB_ROLE[s] == 0)
            items[nitem++] = s;
    }
    for (int k = 0; k < n; k++) {
        kind[k] = b[k] && b[k] != ~0ULL ? BoxKindOf(b[k]) : -1;
        slot[k] = -1;
        if (kind[k] == 3)
            has_melee = 1;
    }
    int ni = 0, nsp = 0, over = 0;
    for (int k = 0; k < n; k++) {       // the weapon first
        if (kind[k] == 3 && melee_slot >= 0 && !used[melee_slot]) {
            slot[k] = melee_slot;
            used[melee_slot] = 1;
        }
    }
    for (int k = 0; k < n; k++) {
        if (slot[k] >= 0 || !b[k] || b[k] == ~0ULL)
            continue;
        if (ni < nitem)
            slot[k] = items[ni++];
        else if (nsp < nspare && (!has_melee || ni >= nitem))
            slot[k] = spares[nsp++];    // spare slots: no melee in the box, or more items than item slots
        else
            slot[k] = over++ % 12;      // everything taken: lay on top of an earlier one (+3 cm per layer)
        if (slot[k] >= 0 && slot[k] < 12 && kind[k] != 3)
            used[slot[k]] = used[slot[k]] ? used[slot[k]] : 1;
    }
    for (int k = 0; k < n; k++) {
        if (!b[k] || b[k] == ~0ULL || slot[k] < 0)
            continue;
        int sl = slot[k] % 12, layer = k >= 12 || (over
                                                   && slot[k] >=
                                                   12) ? 1 : 0;
        float vv[3];
        NrbLocalPose(i, sl, layer, kind[k], 0, b[k], vv);
        NrbWorldPose(i, vv, sl, kind[k], tp[k], tq[k]);
        int kq = NrbSlotKind(kind[k]);
        uint64_t bfa[5] = { (uint64_t) tp[k], (uint64_t) tq[k], 0, 0, 0 };
        uint64_t bf =
            DcCall(0x871ec52cc86b7427ULL /* alloc-bound-frame */ , 5,
                    bfa);
        uint64_t la[2] = { b[k], bf };
        uint64_t r = bf
            && bf != ~0ULL ? DcCall(FNV64("set-object-locator"), 2,
                                     la) : 0;
        // (loot sometimes ended up on the floor after opening a supply box): the spawned crate prop has no Havok body (nr-supply-box.md section 6) and the stock chest's
        // small collision only carries the middle slots, so a loot pickup moved onto the lining was a DYNAMIC body with nothing under it and dropped through to the floor (the
        // outer slots, and every slot once the chest was hidden / the lid animates). Loot is now made KINEMATIC the moment it is placed (the melee drops' md_make_kinematic:
        // Body::RequestMotionType 1, still pickable, nothing pushes it); every pass (frames 1/2/8/15/45 after the open) repeats it, a body created late is caught by the later ones.
        if (r == 0 && !MdKinOff()) {
            int kd = MdMakeKinematic(b[k]);
            static int nk;
            if (LOGCAP(nk, 40))
                Xlog("nrbox: loot %llx kinematic bodies switched: %d",
                     (unsigned long long) b[k], kd);
        }
        static int cnt;
        if (cnt++ < 400)
            Xlog("nrbox: box %d loot %d handle %llx kind %s -> slot %d (%s) at %.2f %.2f %.2f (set-object-locator -> %llx)", i, k, (unsigned long long) b[k], kq >= 0 ? NRB_KINDN[kq] : "unknown", sl + 1, NRB_ROLE[sl] == 1 ? "melee" : NRB_ROLE[sl] == 2 ? "spare" : "item", tp[k][0], tp[k][1], tp[k][2], (unsigned long long) r);
    }
}

static void NrboxMarker(int i, int on)
{                               // SP: listen-mode-visible(crate, 1, 1, -1.0) + listen-mode-set-buddy-draw-mode(crate, 1); both 0 at the open
    static int c = -1;
    static uint64_t at;
    if (!g_nrb[i].obj || g_nrb[i].marker == on
        || NrboxFlag("t2r-no-crate-marker.txt", &c, &at)
        || NrboxOldOpen())
        return;
    g_nrb[i].marker = on;
    uint64_t a[4] =
        { g_nrb[i].obj, (uint64_t) on, (uint64_t) on,
on ? 0xBF800000ULL : 0 };
    uint64_t r1 =
        DcCall(0x7f1369bacaaca1e5ULL /* listen-mode-visible */ , 4, a);
    uint64_t b[2] = { g_nrb[i].obj, (uint64_t) on };
    uint64_t r2 =
        DcCall(0x1624730aed05526dULL /* listen-mode-set-buddy-draw-mode */
                , 2, b);
    static int n;
    if (LOGCAP(n, 12))
        Xlog("nrbox: box %d marker %d (listen-mode-visible -> %llx, buddy-draw -> %llx)", i, on, (unsigned long long) r1, (unsigned long long) r2);
}

// The open edge: anim + sounds in the same frame, loot revealed 0.12 s later. local = the box was opened by this body (backpack sound + haptic too).
static void NrboxStartOpen(int i, int local)
{
    static int ncs = -1;
    static uint64_t cat;
    static const struct {
        const char *sfx, *joint;
    } SN[3] =
        { { "sfx-t2r-igc-loot-crate-open-hinge", "lid" },
        { "sfx-t2r-igc-loot-crate-open-latch", "latch" },
        { "sfx-t2r-igc-loot-crate-open-lid", "root" } };
    uint64_t now = GetTickCount64();
    g_nrb[i].open = 1;
    g_nrb[i].anim_until = now + 2500;
    uint64_t r = AnimateNow(g_nrb[i].obj, "open");
    g_nrb[i].loot_at = g_frame ? g_frame : 1;
    g_nrb[i].reveal_ms = now + 120;
    NrboxMarker(i, 0);
    static int nb = -1;
    static uint64_t bat;
    // "chest open plays double": the crate `open` clip fires the 3 sound events itself (frame 0, same sfx), ours were a second copy. Default = clip only; t2r-crate-own-sounds.txt = play ours too.
    if (NrboxFlag("t2r-crate-own-sounds.txt", &ncs, &cat)
        && !NrboxFlag("t2r-no-boxsound.txt", &nb, &bat))
        for (int k = 0; k < 3; k++) {
            uint64_t a[5] =
                { FNV64(SN[k].sfx), g_nrb[i].obj, FNV64(SN[k].joint), 0,
        0 };
            uint64_t rs = DcCall(FNV64("play-sound-at-joint"), 5, a);
            static int n;
            if (LOGCAP(n, 12))
                Xlog("nrbox: box %d crate sound %s@%s -> %llx", i,
                     SN[k].sfx, SN[k].joint, (unsigned long long) rs);
        }
    if (local) {
        g_nrbMs = 1;
        g_nrb_hap_at = now + 120;
        g_nrb_snd_at = now + 370;
    }                           // SP: haptic at box-opened (+0.12 s), scav-insert-item +0.37 s
    Xlog("nrbox: box %d OPEN %s (open anim -> %llx, loot revealed in 120 ms)", i, local ? "by us" : "seen", (unsigned long long) r);
}

static int NrboxByEntry(uint8_t *e)
{
    uint64_t h = *(uint64_t *) e;
    for (int j = 0; j < NRB_MAX; j++)
        if (g_nrb[j].hash == h && g_nrb[j].obj)
            return j;
    return -1;
}

// boxfill_hook(from_open), before the fill: 1 = handled (crate opens now, its sounds replace the player ones)
static int NrboxOpenStart(uint64_t box)
{
    if (!g_nrbN || NrboxOff() || NrboxOldOpen())
        return 0;
    int i = NrboxByEntry((uint8_t *) box);
    if (i < 0)
        return 0;
    NrboxStartOpen(i, 1);
    return 1;
}

static void NrboxHideLoot(int i, uint8_t *e)
{                               // loot stays invisible until the lid has moved for 0.12 s (SP spawns it then)
    (void) i;
    uint64_t **v = (uint64_t **) (e + 0x30);
    uint64_t *b = v[0], *en = v[1];
    if (!b || en <= b || (en - b) > 32
        || IsBadReadPtr(b, (size_t) ((uint8_t *) en - (uint8_t *) b)))
        return;
    for (int k = 0; k < en - b; k++) {
        if (!b[k] || b[k] == ~0ULL)
            continue;
        uint64_t a[2] = { b[k], 0 };
        DcCall(FNV64("hide-object-for-one-frame"), 1, a);
    }
}

// (the crate tilted 30 degrees after the open, then got stuck in front of the player and followed it): the spawned crate keeps the actor's own
// Havok collision (rl-cache-crate-military, 4 convex shapes) and it is a DYNAMIC body: the loot / melee pickup spawned inside its volume shoved it (tilt), and the player's capsule then
// pushed it along as they walked and turned (always in front of B's camera). Fix: the crate is made KINEMATIC (md_make_kinematic = Body::RequestMotionType 1, retried until its bodies exist)
// and every 3 frames its locator is compared with its placement; a crate that drifted > 3 cm / tilted is put straight back (set-object-locator). t2r-no-crate-lock.txt = off.
// Only bodies that are DYNAMIC (motion type 3) are switched to keyframed: the lid / latch bodies the `open` clip drives are type 1 / 2 already, and md_make_kinematic (every body that is
// not 1) froze them (the crate no longer opened visually). Returns the number of bodies switched, -1 = no object / bodies yet.
static int NrboxKin(uint64_t h)
{
    uint8_t *obj =
        ((uint8_t * (*)(uint64_t)) T2R_SYM(kFnEntityFromHandle)) (h);
    if (!obj || IsBadReadPtr(obj, 0x260))
        return -1;
    uint64_t vt = *(const uint64_t *) obj;
    if (vt < 0x100000000ULL
        || IsBadReadPtr((const void *) (uintptr_t) vt, 0x688))
        return -1;
    uint8_t *bodies[10];
    int nb = 0;
    uint8_t *main_b =
        ((uint8_t * (*)(uint8_t *)) *
         (void **) (uintptr_t) (vt + 0x680)) (obj);
    if (main_b && !IsBadReadPtr(main_b, 0x110))
        bodies[nb++] = main_b;
    uint8_t *cb = *(uint8_t **) (obj + 0x258);
    if (cb && !IsBadReadPtr(cb, 0x68)) {
        uint32_t n = *(uint32_t *) (cb + 0x14);
        uint8_t *arr = *(uint8_t **) (cb + 0x60);
        if (n > 8)
            n = 8;
        for (uint32_t i = 0; arr && i < n && nb < 10; i++) {
            uint8_t *b2 = arr + (size_t) i * 0x110;
            if (IsBadReadPtr(b2, 0x110) || b2 == main_b)
                continue;
            bodies[nb++] = b2;
        }
    }
    int done = 0, any = 0;
    for (int i = 0; i < nb; i++) {
        uint8_t *b2 = bodies[i];
        if ((*(uint32_t *) (b2 + 0x54) & 0xffffff) == 0xffffff)
            continue;           // no havok body yet
        int cur = (*(uint32_t *) (b2 + 0x58) >> 18) & 3;
        any++;
        static int ln;
        if (LOGCAP(ln, 24))
            Xlog("nrbox: crate body %d type %d%s", i, cur,
                 cur == 3 ? " -> keyframed" : " (left alone)");
        if (cur == 3) {
            ((fn_reqmotion)
             ((uint8_t *) GetModuleHandleW(NULL) + 0x1c10fb0)) (b2, 1,
                                                                0.0f);
            done++;
        }
    }
    return any ? done : -1;
}

static void NrboxLockCrates(void)
{
    static int c = -1;
    static uint64_t cat;
    if (NrboxFlag("t2r-no-crate-lock.txt", &c, &cat))
        return;
    for (int i = 0; i < NRB_MAX; i++) {
        if (!g_nrb[i].obj || !g_nrb[i].hash)
            continue;
        if (!g_nrb[i].kin && g_nrb[i].kin_tries < 60 && !MdKinOff()) {
            g_nrb[i].kin_tries++;
            if (NrboxKin(g_nrb[i].obj) >= 0) {
                g_nrb[i].kin = 1;
                Xlog("nrbox: box %d crate is kinematic (try %d)", i,
                     g_nrb[i].kin_tries);
            }
        }
        if (g_frame % 3 || GetTickCount64() < g_nrb[i].anim_until)
            continue;           // never while the open / close clip runs (root locator only, placement tolerance)
        float lb[16];
        const float *l = PuppetLocator(g_nrb[i].obj, lb);
        if (!l || IsBadReadPtr(l, 32) || !KillPosOk(l))
            continue;
        const float *pp = g_nrb[i].pos, *qq = g_nrb[i].q;
        float dx = l[0] - pp[0], dy = l[1] - pp[1], dz = l[2] - pp[2];
        float dq =
            l[4] * qq[0] + l[5] * qq[1] + l[6] * qq[2] + l[7] * qq[3];
        if (dx * dx + dy * dy + dz * dz < 0.03f * 0.03f
            && __builtin_fabsf(dq) > 0.9995f)
            continue;
        static
            __attribute__((aligned(16))) float tp[NRB_MAX][4],
            tq[NRB_MAX][4];
        memcpy(tp[i], pp, 12);
        tp[i][3] = 1.0f;
        memcpy(tq[i], qq, 16);
        uint64_t bfa[5] = { (uint64_t) tp[i], (uint64_t) tq[i], 0, 0, 0 };
        uint64_t bf =
            DcCall(0x871ec52cc86b7427ULL /* alloc-bound-frame */ , 5,
                    bfa);
        uint64_t la[2] = { g_nrb[i].obj, bf };
        if (bf && bf != ~0ULL)
            DcCall(FNV64("set-object-locator"), 2, la);
        g_nrb[i].fix_n++;
        g_nrb[i].kin_tries = g_nrb[i].kin_tries > 20 ? 20 : g_nrb[i].kin_tries; // it moved: kinematic again (a body that was recreated)
        g_nrb[i].kin = 0;
        static int n;
        if (LOGCAP(n, 30))
            Xlog("nrbox: box %d crate drifted %.2f m / tilt dot %.4f - put back (fix %u)", i, __builtin_sqrtf(dx * dx + dy * dy + dz * dz), (double) dq, g_nrb[i].fix_n);
    }
}

// (~6 items of an opened box lay on the floor at the old spots): the move passes ran only at open + 1/2/8/15/45 frames, so a pickup winmm spawned later (or while the
// cached record pointer was stale after the table shifted - the old loop then skipped the box entirely until the next 15-frame refresh) stayed where winmm put it. The record is now looked
// up by spawner hash whenever the cached pointer no longer matches, (the loot itself is now posed before its spawn: nrbox_place_first).
static uint8_t *NrboxFindRec(uint64_t h)
{
    uint8_t *beg = *(uint8_t **) T2R_SYM(kPeerListBegin), *end =
        *(uint8_t **) T2R_SYM(kPeerListEnd);
    if (!beg || end <= beg || (end - beg) / 0x48 > 64
        || IsBadReadPtr(beg, (size_t) (end - beg)))
        return NULL;
    for (uint8_t * e = beg; e < end; e += 0x48)
        if (*(uint64_t *) e == h)
            return e;
    return NULL;
}

static void NrboxTick(void)
{
    if (g_nrbN && !NrboxOff() && MatchSubstate() >= 5)
        NrboxLockCrates();
    if (!NrboxOff())
        NrboxPerFrame();      // regression fix: nrbox3 dropped this call = the stock chests were never hidden (old boxes inside the new ones) and the backpack sound / haptic never fired
    int old = NrboxOldOpen();
    if (!old && g_nrbN && !NrboxOff() && MatchSubstate() >= 5) {     // every frame: the open edge of a box another player opened, loot timeline
        uint64_t now = GetTickCount64();
        for (int i = 0; i < NRB_MAX; i++) {
            if (!g_nrb[i].obj || !g_nrb[i].hash)
                continue;
            uint8_t *e = g_nrb[i].e;
            if (!e || IsBadReadPtr(e, 0x48)
                || *(uint64_t *) e != g_nrb[i].hash) {
                e = NrboxFindRec(g_nrb[i].hash);
                if (!e)
                    continue;
                static int nrf;
                if (LOGCAP(nrf, 10))
                    Xlog("nrbox: box %d record pointer was stale (table shifted) - found again by spawner hash", i);
                g_nrb[i].e = e;
            }
            if (g_nrb[i].open != e[0x14]) {
                if (e[0x14])
                    NrboxStartOpen(i, 0);
                else {
                    g_nrb[i].open = 0;
                    g_nrb[i].loot_at = 0;
                    g_nrb[i].reveal_ms = 0;
                    g_nrb[i].anim_until = GetTickCount64() + 2500;
                    uint64_t r = AnimateNow(g_nrb[i].obj, "close");
                    NrboxMarker(i, 1);
                    static int n;
                    if (LOGCAP(n, 24))
                        Xlog("nrbox: box %d close -> animate_ %llx", i,
                             (unsigned long long) r);
                }
            }
            if (g_nrb[i].loot_at && e[0x14]) {
                uint64_t d = g_frame - g_nrb[i].loot_at;
                if (g_nrb[i].reveal_ms && now < g_nrb[i].reveal_ms)
                    NrboxHideLoot(i, e);
                else
                    g_nrb[i].reveal_ms = 0;
                if (d == 1 || d == 2 || d == 8 || d == 15 || d == 45)
                    NrboxMoveLoot(i, e);
                if (d > 60)
                    g_nrb[i].loot_at = 0;
            }
        }
    }
    if (g_frame % 15)
        return;
    if (NrboxOff() || MatchSubstate() < 5) {
        if (g_nrbN || g_nrb[0].hash)
            NrboxForget();
        return;
    }
    uint8_t *beg = *(uint8_t **) T2R_SYM(kPeerListBegin), *end =
        *(uint8_t **) T2R_SYM(kPeerListEnd);
    if (!beg || end <= beg || (end - beg) / 0x48 > 64
        || IsBadReadPtr(beg, (size_t) (end - beg)))
        return;
    for (uint8_t * e = beg; e < end; e += 0x48) {
        if (*(uint64_t *) (e + 0x18) == *(uint64_t *) (e + 0x20))
            continue;           // an unused slot (no item positions)
        uint64_t h = *(uint64_t *) e;
        // A/B: the winmm table's order/empty slots shift during a match, so keying our state by table position respawned a crate
        // (old one never deleted) on the same spawner -> two crates, one animated open, one closed. Key by spawner hash.
        int i = -1;
        for (int j = 0; j < NRB_MAX; j++)
            if (g_nrb[j].hash == h) {
                i = j;
                break;
            }
        if (i < 0)
            for (int j = 0; j < NRB_MAX; j++)
                if (!g_nrb[j].hash) {
                    i = j;
                    g_nrb[i].hash = h;
                    break;
                }
        if (i < 0)
            continue;
        g_nrb[i].e = e;
        if (!g_nrb[i].obj && g_nrb[i].tried < 3) {
            g_nrb[i].tried++;
            g_nrb[i].obj = NrboxSpawn(i, h, e);
            if (g_nrb[i].obj) {
                g_nrbN++;
                g_nrb[i].open = e[0x14];
                if (g_nrb[i].open)
                    AnimateNow(g_nrb[i].obj, "open");
                else
                    NrboxMarker(i, 1);
            }
        } else if (old && g_nrb[i].obj && g_nrb[i].open != e[0x14]) {
            g_nrb[i].open = e[0x14];
            uint64_t r =
                AnimateNow(g_nrb[i].obj,
                            g_nrb[i].open ? "open" : "close");
            g_nrb[i].loot_at = g_nrb[i].open ? g_frame : 0;
            static int n;
            if (LOGCAP(n, 24))
                Xlog("nrbox: box %d %s -> animate_ %llx", i,
                     g_nrb[i].open ? "open" : "close",
                     (unsigned long long) r);
        }
        if (old && g_nrb[i].obj && g_nrb[i].loot_at && e[0x14]) {
            if (g_frame - g_nrb[i].loot_at <= 60)
                NrboxMoveLoot(i, e);
            else
                g_nrb[i].loot_at = 0;
        }
    }
}

// ---------------------------------------------------------------- supply boxes on another floor: the RADAR icon changes
// Supply boxes that aren't on your level (with some leeway, so you don't need to be exactly the same Y as the box) are grayed out, and the grayed-out boxes get
// a tiny white arrow up or down - on the RADAR (not in the 3D world; the engine's 3D crate marker is untouched). This file only keeps the state: g_nrb[i].flr = 0 same
// floor, 1 = box ABOVE us, 2 = box BELOW us; radar_box.c swaps the radar icon of that box for the grey-crate-with-arrow texture (radar-box-up / radar-box-down, make_icons.radar_box_floor).
//   same level  = |dy| <= 2.0 m (dy = box y - our feet y, player_locator); another floor at |dy| > 2.0, our floor again at |dy| < 1.85 (15 cm hysteresis against flicker on a stair landing).
//   Why 2.0: Bookstore's floors are at y about -1, 4.4 and 8 (storeys 3.6 - 4.4 m apart; a crate stands ~0.25 m above its floor), so a box on the NEXT floor is at least ~3.3 m off, a box
//   on our own floor at most ~1.3 m (crate height + a jump or the middle of a stair flight): 2.0 sits in the middle of that gap = leeway for stairs / ramps / jumping, and no box of an
//   adjacent storey ever counts as ours.
// Log 'box floor:' once per state change. t2r-no-box-floor.txt next to the exe = off (every radar icon the plain crate).
#define BXF_LEVEL 2.0f
#define BXF_BACK 1.85f
static int g_bxfOff;
static uint64_t g_bxfChk;
static void NrboxFloorUpdate(void)
{
    int off = OvOff("t2r-no-box-floor.txt", &g_bxfOff, &g_bxfChk)
        || NrboxOldOpen() || !g_nrbN || NrboxOff()
        || MatchSubstate() != 6;
    float pb[16];
    const float *pl = off ? NULL : PlayerLocator(pb);
    int ok = pl && !IsBadReadPtr(pl, 12) && pl[1] == pl[1];
    for (int i = 0; i < NRB_MAX; i++) {
        if (!g_nrb[i].hash)
            continue;
        int st = 0;
        float dy = 0.0f;
        if (ok && g_nrb[i].obj) {
            dy = g_nrb[i].pos[1] - pl[1];
            float ady = __builtin_fabsf(dy), prev = g_nrb[i].flr;
            int off2 = prev ? ady >= BXF_BACK : ady > BXF_LEVEL;
            st = off2 ? (dy > 0.0f ? 1 : 2) : 0;
        }
        if (st != g_nrb[i].flr) {
            g_nrb[i].flr = st;
            Xlog("box floor: box %d radar icon -> %s (dy %+.2f m, our y %.2f, box y %.2f, level = within %.2f m)", i, st == 1 ? "grey crate + arrow UP (box above us)" : st == 2 ? "grey crate + arrow DOWN (box below us)" : "plain crate (our floor / off)", (double) dy, ok ? (double) pl[1] : 0.0, (double) g_nrb[i].pos[1], (double) BXF_LEVEL);
        }
    }
}
