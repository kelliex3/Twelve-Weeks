// ---------------------------------------------------------------- 2013 PLACED nail bomb (proximity trap)
// (re/factions2013/rules/molotov-nailbomb-2013.md section 11; included from t2r-ext.c after explosives2013.c)
//
// 2013 had the nail bomb as a THROWN bullet (thrown-nail-bomb-bullet-online, 1.5 s timed fuse) and as a PLACED bomb
// (dropped-nail-bomb-bullet-online, process-mine.cpp "ProcessMine", player state "PlaceMine", sound bank wpn-trap-bomb). What the
// data/code say (EBOOT 0x69dc00..0x6a0400, weapon-art.bin 0xff4, explosions.bin 0xa2c), numbers below:
//   arming delay   PB_ARM_S = 1.0 s      bullet-settings +0xb4 = 1.0 is copied into the mine's arming timer (+0xa5c) at 0x69df60/0x69e508 [code+data]
//   trigger radius PB_RADIUS = 3.0 m     bullet-settings +0xb0 = 3.0 next to the timer fields [data]; "radius" meaning INFERRED
//   delay to blast PB_FUSE_S = 1.25 s    bullet-settings +0xb8 = 1.25 and *bomb-explosion-online* +0x10 = 1.25 (thrown bomb: +0xb8 = 1.5 = its fuse) [data]
//   blast          *bomb-explosion-online* PLAYER ring: kill 3.0 / 4.5 x0.6 / 6.0 x0.2 / flinch 9.0 (-> explosives2013.c EX13_PBOMB) [data]
//   who triggers   enemies of the owner only: the mine's damage handler (0x69f920, event 0x539335e) ignores hits whose attacker is on
//                  the mine's team; the trigger itself is script/animation driven (event 0x2476221a, bools arm / trigger) so the
//                  exact proximity test is INFERRED (3-D distance to the chest, no ray) [code+inferred]
//   enemy fire     the 2013 mine also detonates at once when an ENEMY shoots / explodes / burns it (damage kinds 0, 2, 5): done (pb_shot_scan from the
//                  shot-resolve hook = OUR shots that hit the bomb; pb_blast_near = enemy nail bomb / pipe bomb / molotov bursts next to it); a hit from the
//                  owner's team does nothing (the 2013 handler). Every client has the bomb, the shooter's client tells the others (MSG_MINE op 4)
//   pick-up        2013: an interact event gives the bomb back (event 0x34c6c9de) - not done here
//   cap            no per-player cap in the data (limited by the carried bombs); PB_CAP_OWNER is a guard of ours
//   owner death    nothing removes mines on death in the data (only net-kill-all-mines, called by ss-gungame): they stay
// The visual is now SP's trap mine itself, placed by SP's spawn-trap-mine native (see pb_spawn_sp); t2r-old-trap.txt = the pipe-bomb
// look described next. The bomb's visual (old look) is a real pipe-bomb projectile (the same spawn the remote throws use) at rest, SET on the floor by the planter's hand
// (TLOU2's trap-mine setting clip, see pb_plant_start); its natural fuse explode is SUPPRESSED in expl_hook (pb_hook). When the blast is
// due it goes off with SP's TRAP-MINE effects (pipe bomb model, trap mine effects): spawn-particles-at-bound-frame trap-mine +
// spore-push-field at the bomb and sfx-trap-bomb on it, as SP's mine (sp-sandbox round 2); the damage stays the 2013 ring (ex13).
// t2r-pb-pipe-blast.txt = the engine's pipe-bomb explode instead (also the fallback when the trap-mine particles do not spawn). Sound: the REAL 2013 nail bomb
// (PS3 wpn-bomb.bnk: bomb-place, bomb-timed-idle-mp loop, bomb-timed-countdown-mp; tools/rip_t1_trap_audio.py, re/trap-audio/) through the
// PCM mixer, 3-D at the bomb. Quiet on purpose (the TLOU2 stand-ins were far too loud); t2r-trap-vol.txt = 0..1 multiplier.
// Network: MSG_MINE 0x62 [op][u16 id]...: 1 place (+ x y z), 2 trigger, 3 remove, 4 shot, 5 marked, 6 plant start (the setting clip on the
// owner's puppet); every client has the bomb at the same spot, the
// first trigger wins (a client checks only ITSELF against the mines, so each player triggers on their own screen and tells the others).
// Logs: "pbomb:". t2r-no-placed-bomb.txt = the feature off.
#include "trap2013.h"
__asm__
    (".section .rdata,\"dr\"\n.balign 4\n.globl pb_blob\npb_blob:\n.incbin \"trap2013.pcm\"\n.text\n");
extern const int16_t pb_blob[];

#define MSG_MINE 0x62
#define PB_MAX 12
#define PB_CAP_OWNER 3
#define PB_ARM_S 1.0f
#define PB_RADIUS 3.0f
#define PB_FUSE_S 1.25f
#define PB_AHEAD 0.46f          // the spot: where the setting clip's right hand puts the bomb down (PB_PLANT_CLIP: r_hand_prop at the
#define PB_RIGHT 0.085f         // release frame 0.435 m ahead / 0.080 m right of the root on Ellie's rig; x 1.06 for the MP body)
#define PB_PIPE_SID 0xcf576d288d014eacULL
#define PB_LISTEN_R 8.0f        // how far listen mode reveals a placed bomb (highlight) = how far its sounds carry (fade over the last PB_FADE_M)
#define PB_FADE_M 2.0f
#define PB_MARK_S 10.0f         // (unused: a marked enemy bomb stays red for the team until it is gone)

static struct {
    int used, armed, boomed, hl, by1;
    uint16_t id;
    int owner;
    float pos[3];
    uint64_t placed_ms, trig_ms, boom_ms, h, wick_ms, outline_ms,
        respawn_ms, arm_snd, mark_until, pin_ms;
    int held, still_n;
    uint64_t spawn_ms, contact_ms;
    float last[3];              // settling: lies down on its own (held = 0), then is held on its spot
    int fx;                     // the blast: 1 = SP trap-mine particles + sfx (bomb hidden), 2 = the engine's pipe-bomb explode
    uint64_t lm;                // listen stand-in: SP's trap-mine object on the bomb's spot (pb_lm_*), 0 = none
    float rad;
    uint64_t pj[2], pj_frame;
    uint64_t oj;
    int oj_slot;
    uint64_t oj_frame;          // owner-side probe (pb_owner_check): ray to the enemy puppet inside the sphere                               // 2013 trigger sphere radius (3.0 x owner's Explosion Expert scale, fixed at creation) / the two LOS probe rays
    int pipe, nresp;            // the visual is the pipe-bomb model (old look / SP spawn failed) / how often it was re-spawned
    int q_pipe, q_trap;         // our pipe-bomb / trap-bomb item counts right after placing it (own bombs: the pickup test)
    uint64_t gone_ms;           // first tick the visual was seen gone (own bomb: the pickup's item gets 250 ms to show)
} g_pb[PB_MAX];
static int PbMineNear(const float *p, float r)
{                               // the spawn picker: no spawner within r m of a placed bomb that has not gone off (any team's)
    for (int i = 0; i < PB_MAX; i++) {
        if (!g_pb[i].used || g_pb[i].boomed)
            continue;
        float dx = g_pb[i].pos[0] - p[0], dy = g_pb[i].pos[1] - p[1], dz =
            g_pb[i].pos[2] - p[2];
        if (dx * dx + dy * dy + dz * dz < r * r)
            return 1;
    }
    return 0;
}

static int g_pbAllow;          // we are calling the engine's explode on a placed bomb ourselves (pb_hook lets it through)
static uint8_t g_pbSeq;
// the plant spot (placing sometimes failed for several tries - the old 3-ray test refused a good floor whenever ONE side
// sample 0.25-0.33 m off the spot hit a curb / crate edge, even though the spot itself was flat): one ground ray per point of a grid in front
// of the feet (PB_ROWS ahead x PB_COLS sideways, around the setting clip's hand spot), each point judged by its own floor height and its slope
// to the grid neighbours (0.08-0.10 m away, the bomb's own footprint). The valid point nearest the hand spot wins, like 2013, where the bomb
// is set down on any floor in front of you; a steeper (rubble / ramp) point only when no flat one exists; refused only with no floor at all.
// Placing only worked while moving, not while stationary: a log showed 16 refusals in a row standing still at supply
// box 1 (feet 37.97, every ray of the grid +0.47..+0.50 over the feet = the raised floor around the box, 0.4-0.5 m high, right in front of
// the toes; the floor bake has that box's floor at +0.37/+0.43 over the navmesh), then a plant as soon as the player had stepped away.
// Players buy / craft the bomb at a box and plant it on the spot, facing the box or a wall / curb, so the grid now starts at the toes
// (0.06 / 0.14 m ahead, the floor the player stands on) and a step or wall RISING beside a point no longer makes that point an edge (the
// bomb lies on the lower floor at its foot; only a neighbour more than a step LOWER = a drop-off makes it a ledge lip). The hand spot
// still wins when it is valid (2013: set down on the floor in front of you; at your feet when there is no room in front).
#define PB_FLOOR_BELOW 1.5f     // a mine more than this below our feet is on a lower floor (no trigger, no blast)
#define PB_FLOOR_ABOVE 1.8f     // ... more than this above our feet on an upper floor / ledge
#define PB_ROWS 7
#define PB_COLS 5
#define PB_GRID (PB_ROWS * PB_COLS)
static const float PB_ROW_AHEAD[PB_ROWS] =
    { 0.06f, 0.14f, 0.22f, 0.30f, 0.38f, PB_AHEAD, 0.54f };
#define PB_WALL_UP 0.10f        // a neighbour this much HIGHER is a wall / step beside the spot (not counted for its slope)
static const float PB_COL_SIDE[PB_COLS] = { -0.20f, -0.10f, 0.0f, 0.10f, 0.20f };       // + = right, around PB_RIGHT

#define PB_FLOOR_DOWN 0.30f     // the hand of the setting clip reaches the feet's floor: a point more than a step below / above is no plant spot
#define PB_FLOOR_UP 0.25f
#define PB_SLOPE_FLAT 0.58f     // rise per metre to a neighbour (tan 30 deg) of a flat spot
#define PB_SLOPE_STEEP 1.0f     // ... (tan 45 deg) of the fallback spot when no flat one exists
static struct {
    int st;
    uint64_t job[PB_GRID], job2[PB_GRID];
    float top, len, top2, len2;
    float fwd[2];
    float pt[PB_GRID][2];
    uint64_t frame;
    float feet[3];
} g_pbp;
static void PbKickSpot(void)
{                               // the ground rays of the grid in front of the feet (g_pbp.feet / fwd)
    float fx = g_pbp.fwd[0], fz = g_pbp.fwd[1];
    g_pbp.top = g_pbp.feet[1] + 0.9f;
    g_pbp.len = 1.8f;
    g_pbp.top2 = g_pbp.feet[1] + PB_FLOOR_UP + 0.05f;
    g_pbp.len2 = PB_FLOOR_UP + 0.05f + PB_FLOOR_DOWN + 0.15f;
    for (int r = 0; r < PB_ROWS; r++)
        for (int c = 0; c < PB_COLS; c++) {
            int k = r * PB_COLS + c;
            float side = PB_RIGHT + PB_COL_SIDE[c];
            g_pbp.pt[k][0] =
                g_pbp.feet[0] + fx * PB_ROW_AHEAD[r] - fz * side;
            g_pbp.pt[k][1] =
                g_pbp.feet[2] + fz * PB_ROW_AHEAD[r] + fx * side;
            float a[3] =
                { g_pbp.pt[k][0], g_pbp.top, g_pbp.pt[k][1] }, b[3] =
                { g_pbp.pt[k][0], g_pbp.top - g_pbp.len, g_pbp.pt[k][1] };
            g_pbp.job[k] = WorldRayKick(a, b);
            // GRASS (all 35 rays +0.36..+0.55 m over the feet in a smooth plateau = the foliage layer, "step / ledge" refusals, then a spot
            // with -0.04 m worked): the high ray stops on the grass top. A second ray starts only just above the plant window (feet +0.30), under the
            // blades, and finds the static ground; it is used for a point whose high ray lands outside the window.
            float a2[3] = { a[0], g_pbp.top2, a[2] }, b2[3] =
                { a[0], g_pbp.top2 - g_pbp.len2, a[2] };
            g_pbp.job2[k] = WorldRayKick(a2, b2);
        }
    g_pbp.st = 1;
    g_pbp.frame = g_frame;
}                               // placement rays

static uint64_t g_pbOffAt;
static int g_pbOff;

static float PbFuseS(int owner)
{                               // Bomb Expert shortens placed bombs too (wiki; same table as the thrown pipe bomb)
    // PS4 v1.11 mine.md (0x69df60 area): trigger-to-blast = bullet-settings 1.25 s x the owner's Bomb Expert placed-bomb multiplier (sid 0x766d2060: II 0.9, III 0.7)
    static const float PBM[4] = { 1.0f, 1.0f, 0.9f, 0.7f };
    uint32_t self = *(uint32_t *) T2R_SYM(kMySlot);
    int lv =
        (uint32_t) owner == self ? Skl(SK_BOMB) : SkPeer((uint32_t) owner,
                                                          SK_BOMB);
    if (lv < 0)
        lv = 0;
    return PB_FUSE_S * PBM[lv & 3];
}

static float PbOwnerRadius(int owner)
{                               // 3.0 m x the owner's Explosion Expert scale 1.0 / 1.2 / 1.4 (mine.md +0xdc8)
    static const float B[3] = { 1.0f, 1.2f, 1.4f };
    uint32_t self = *(uint32_t *) T2R_SYM(kMySlot);
    int lv =
        (uint32_t) owner ==
        self ? Skl(SK_EXPLO) : SkPeer((uint32_t) owner, SK_EXPLO);
    if (lv < 0)
        lv = 0;
    if (lv > 2)
        lv = 2;
    return PB_RADIUS * B[lv];
}

static int PbFind(uint16_t id)
{
    for (int i = 0; i < PB_MAX; i++)
        if (g_pb[i].used && g_pb[i].id == id)
            return i;
    return -1;
}

// The bomb is SET on the floor (it is not dropped from the hand; the trap mine's setting anims are used): the planter's body
// plays TLOU2's trap-mine setting clip (pb_plant_start), whose right hand carries the held pipe bomb down to the floor; at the clip's release
// frame the projectile is put AT REST where the hand left it - the hand's x / z (r_hand_prop of our body; the owner's measured spot on the
// other screens), PB_REST_H over the checked ground, zero velocity, no fall. It lies down on its own within a few frames (a few mm), then is held.
#define PB_REST_H 0.045f
// The placed object can be SP's trap mine (ProjectileThrowable of weapon trap-bomb, bullet trap-bomb-bullet(-upgraded), look trap-bomb / art
// trap-mine-upgrade; actors trap-mine / trap-mine-upgrade are MP level adds), spawned by weapon id trap-bomb through the same spawn (exe 0x654cd0).
// The planted bomb IS SP's trap-mine object (default: it glows in listen mode like SP's, no stand-in needed);
// t2r-pb-pipe-look.txt = the pipe bomb model with the trap mine's effects (+ the listen stand-in).
#define PB_TRAP_SID 0x15e921059eff70c9ULL       // trap-bomb
static int PbFile(const char *name, int *cache, uint64_t *at)
{                               // an opt-out / opt-in marker file next to the exe, re-read every 2 s
    if (*cache < 0 || GetTickCount64() - *at > 2000) {
        FILE *f = ExeSideOpen(name, "r");
        *cache = f != NULL;
        if (f)
            fclose(f);
        *at = GetTickCount64();
    }
    return *cache;
}

// Trap mines work exactly like SP visually and audio-wise, and exactly like 2013 on the technical side.
// The planted object is SP's trap mine placed the way SP places it: the script native spawn-trap-mine (exe 0x6502c0; the trap-bomb-test / Ellie
// fight scripts call spawn-trap-mine(trap-bomb-bullet, <owner>, <locator>)). It builds a ProjectileThrowable of weapon trap-bomb / look trap-mine
// itself (spawn info, process type from the bullet, owner = the owner object) and then, instead of the throw spawn (0x654cd0: start / target /
// bullet speed -> a flying projectile, which is what dropped ~0.5 m through the floor on every plant), calls the DEPLOY setter
// 0x1411cb7d0(proj, pos, normal, 0.0f, parent): it stores the deploy transform (obj+0x16d0 = pos + 0.05 * normal, obj+0x16e0 = the rotation
// built from the normal), no velocity at all, and attaches the object to the locator's parent frame. normal = the locator's up axis (the
// native rotates (0,1,0) by the locator's quaternion), so the mine stands on the floor with the floor normal; the plant spot is already
// judged flat (pb_kick_spot grid), so the frame is upright. SP's player plant never calls the native (round-2 sandbox log: no NAT line, only
// sfx-trap-bomb-deploy on the new object) - it reaches the same deploy setter from C++.
// t2r-old-trap.txt = the previous look: the pipe-bomb model (throw spawn, landing + hold logic) with the trap mine as listen stand-in and the
// 2013 PCM idle sounds.
static int PbOldTrap(void)
{
    static int c = -1;
    static uint64_t at;
    return PbFile("t2r-old-trap.txt", &c, &at);
}

static int g_pbSpBad;         // SP's spawn did not give a usable object / it sank: pipe bomb look for the rest of the session
static int g_pbSpawnPipe;     // out of pb_spawn: the visual it returned is the pipe-bomb model
static int g_pbPipeOnce;      // ONE bomb's re-spawn goes pipe-look (its mine keeps being freed), the session stays on SP's mine
static int PbPipeLook(void)
{
    if (g_pbPipeOnce) {
        g_pbPipeOnce = 0;
        return 1;
    }
    return PbOldTrap() || g_pbSpBad;
}

// spawn-trap-mine (exe 0x6502c0) refused SID_PLAYER + trap-bomb-bullet + a psync_bf in an A/B test (returned 0, "no object"). Its early-outs
// (disassembled): owner = the object-argument decoder 0x140402a60 (handle / sid lookups, NULL = return 0), then the spawn info's process type is
// [bullet def]+0 (0x141166b00(bullet sid); NULL = ProjectileThrowable), the spawn 0x1414cd790 and the ProjectileThrowable cast 0x1411c7070 (NULL =
// return 0). So the attempts are, in order, each logged with the probes of the preconditions:
//   1 native, owner = SID_PLAYER      2 native, owner = the player's process handle (*(me + 0xc0))
//   3 the same deploy as the native does it on a projectile of our own spawn (weapon trap-bomb through 0x654cd0, which is known to spawn):
//     0x1411cb7d0(proj, &pos, &normal(0,1,0,0), 0.0f, parent = 0x1415f9980(bf + 8)) = the native's last call (deploy transform, no throw)
// The method that worked is remembered for the session.
static uint64_t PbSpawnSp(const float *pos)
{                               // SP's trap mine standing on the floor spot pos (the deploy lifts it 0.05 m along the normal)
    typedef void *(*fn_proc)(uint64_t);
    typedef void *(*fn_def)(uint64_t);
    typedef uint8_t *(*fn_me)(void);
    typedef void *(*fn_spawn)(float *, float *, uint64_t, uint64_t, float,
                              void *);
    typedef void (*fn_deploy)(void *, float *, float *, float, void *);
    typedef void *(*fn_par)(void *);
    uint8_t *exe = (uint8_t *) GetModuleHandleW(NULL);
    static PsyncBf bfs[32];
    static unsigned bn;
    static int method;          // 0 = not decided, 1..3 = the one that worked
    static int nl;
    PsyncBf *bf = &bfs[bn++ & 31];     // static ring: the engine may keep the frame's parent link
    float ap[7] = { pos[0], pos[1], pos[2], 0.0f, 0.0f, 0.0f, 1.0f };
    PsyncBfMake(bf, ap);
    uint8_t *me = ((fn_me) T2R_SYM(kFnLocalPlayer)) ();
    uint64_t meh = me
        && !IsBadReadPtr(me, 0xd0) ? *(uint64_t *) (me + 0xc0) : 0;
    uint64_t bullet = FNV64("trap-bomb-bullet");
    if (nl < 6) {
        void *def = ((fn_def) (exe + 0x1166b00)) (bullet);
        void *def2 =
            ((fn_def) (exe + 0x1166b00)) (FNV64
                                          ("trap-bomb-bullet-upgraded"));
        void *own1 =
            ((fn_proc) T2R_SYM(kFnEntityFromHandle)) (SID_PLAYER), *own2 =
            meh ? ((fn_proc) T2R_SYM(kFnEntityFromHandle)) (meh) : NULL;
        Xlog("pbomb: spawn-trap-mine probes: bullet def trap-bomb-bullet %p (type word %llx), -upgraded %p, owner by SID_PLAYER %p, player handle %llx -> %p (me %p)", def, def && !IsBadReadPtr(def, 8) ? (unsigned long long) *(uint64_t *) def : 0ULL, def2, own1, (unsigned long long) meh, own2, (void *) me);
    }
    uint64_t r = 0;
    int used = 0;
    for (int m = 1; m <= 3 && !r; m++) {
        if (method && m != method)
            continue;
        if (m <= 2) {
            uint64_t owner = m == 1 ? SID_PLAYER : meh;
            if (!owner)
                continue;
            uint64_t a[3] = { bullet, owner, (uint64_t) bf };
            g_extSpawn++;
            uint64_t h = DcCall(FNV64("spawn-trap-mine"), 3, a);
            g_extSpawn--;
            if (h && h != ~0ULL
                && ((fn_proc) T2R_SYM(kFnEntityFromHandle)) (h))
                r = h;
            if (nl < 10) {
                nl++;
                Xlog("pbomb: spawn-trap-mine(trap-bomb-bullet, %s, floor spot %.2f %.2f %.2f) -> %llx%s", m == 1 ? "SID_PLAYER" : "player handle", pos[0], pos[1], pos[2], (unsigned long long) h, r ? " (object ok)" : " - no object");
            }
        } else {
            if (!g_spawnHooked
                && memcmp(exe + 0x654cd0, (const uint8_t[9]) { 0x4c, 0x89,
                          0x44, 0x24, 0x18, 0x55, 0x57, 0x41, 0x54 }, 9))
                continue;
            float st[4] = { pos[0], pos[1] + 0.05f, pos[2], 0 }, tg[4] =
                { pos[0] + 0.3f, pos[1] + 0.05f, pos[2], 0 }, nrm[4] =
                { 0, 1, 0, 0 }, pp[4] = { pos[0], pos[1], pos[2], 0 };
            g_extSpawn++;
            uint8_t *p =
                ((fn_spawn) (exe + 0x654cd0)) (st, tg, PB_TRAP_SID,
                                               0xed417f0ec4a41b7eULL
                                               /* ProjectileThrowable */ ,
                                               -1.0f, NULL);
            g_extSpawn--;
            if (p) {
                void *par =
                    ((fn_par) (exe + 0x15f9980)) ((uint8_t *) bf + 8);
                void *q = ((fn_par) (exe + 0x11c7070)) (p);     // cast to the throwable: the setter's object
                if (q)
                    ((fn_deploy) (exe + 0x11cb7d0)) (q, pp, nrm, 0.0f,
                                                     par);
                r = *(uint64_t *) (p + 0xc0);
                if (nl < 10) {
                    nl++;
                    Xlog("pbomb: trap-bomb projectile (0x654cd0) + deploy setter at floor spot %.2f %.2f %.2f: obj %p throwable %p parent %p -> %llx", pos[0], pos[1], pos[2], (void *) p, q, par, (unsigned long long) r);
                }
                if (!q) {
                    DcCall(FNV64("kill-entity"), 1, &r);
                    r = 0;
                }
            } else if (nl < 10) {
                nl++;
                Xlog("pbomb: trap-bomb projectile (0x654cd0) did not spawn");
            }
        }
        if (r) {
            used = m;
        }
    }
    if (r && !method) {
        method = used;
        Xlog("pbomb: SP trap-mine spawn method %d works (1 native + SID_PLAYER, 2 native + player handle, 3 projectile + deploy setter)", method);
    }
    return r;
}

static uint64_t PbSpawn(const float *pos)
{                               // SP's trap mine (spawn-trap-mine) at rest on the floor spot pos, else a pipe-bomb projectile there
    typedef void *(*fn_spawn)(float *, float *, uint64_t, uint64_t, float,
                              void *);
    typedef void *(*fn_cast)(void *);
    typedef void (*fn_vel)(void *, float *);
    uint8_t *exe = (uint8_t *) GetModuleHandleW(NULL);
    static const uint8_t expect[9] =
        { 0x4c, 0x89, 0x44, 0x24, 0x18, 0x55, 0x57, 0x41, 0x54 };
    g_pbSpawnPipe = 0;
    if (!PbPipeLook()) {
        uint64_t h = PbSpawnSp(pos);
        if (h)
            return h;
        g_pbSpBad = 1;
        Xlog("pbomb: SP's trap mine did not spawn - pipe bomb look for the rest of the session");
    }
    g_pbSpawnPipe = 1;
    if (!g_spawnHooked && memcmp(exe + 0x654cd0, expect, sizeof expect))
        return 0;
    // start = on the ground (+ the pipe's half thickness), "target" level with it: the projectile is made lying flat, not pointing down
    float start[4] = { pos[0], pos[1] + PB_REST_H, pos[2], 0 }, target[4] =
        { pos[0] + 0.3f, pos[1] + PB_REST_H, pos[2], 0 }, vel[4] =
        { 0, 0, 0, 0 };
    uint8_t *p = NULL;
    g_extSpawn++;
    p = ((fn_spawn) (exe + 0x654cd0)) (start, target, PB_PIPE_SID,
                                       0xed417f0ec4a41b7eULL
                                       /* ProjectileThrowable */ , -1.0f,
                                       NULL);
    g_extSpawn--;
    void *q = p ? ((fn_cast) (exe + 0x11c7070)) (p) : NULL;
    if (q)
        ((fn_vel) (exe + 0x11c8b00)) (q, vel);  // at rest: no throw velocity
    static int nl;
    if (p && nl < 12) {
        nl++;
        Xlog("pbomb: pipe-bomb projectile set at rest at (%.2f %.2f %.2f) = %.3f m over the ground, no velocity", start[0], start[1], start[2], PB_REST_H);
    }
    return p ? *(uint64_t *) (p + 0xc0) : 0;
}

static float PbVol(void)
{                               // t2r-trap-vol.txt next to the exe: 0..1 (re-read every 2 s, tune without a build)
    static float v = 1.0f;
    static uint64_t at;
    uint64_t now = GetTickCount64();
    if (now - at > 2000) {
        at = now;
        v = 1.0f;
        FILE *f = fopen("t2r-trap-vol.txt", "r");
        if (f) {
            float x;
            if (fscanf(f, "%f", &x) == 1)
                v = x < 0 ? 0 : (x > 2 ? 2 : x);
            fclose(f);
        }
    }
    return v;
}

// levels: the 2013 waves peak at -3 dBFS and run 1.1-1.8k RMS; the TLOU2 stand-ins were gain 1.0-2.0 on 2.2-8k RMS waves, so these gains
// are <= 30% of the old loudness (the "70% quieter" ask) by RMS
static void PbSnd(int off, int n, float gain, const float *pos)
{
    // audible only as far as listen mode shows the bomb (PB_LISTEN_R), fading out over the last PB_FADE_M so it never pops
    float lb[16];
    const float *pl = PlayerLocator(lb);
    if (pl) {
        float dx = pos[0] - pl[0], dy = pos[1] - pl[1], dz =
            pos[2] - pl[2], d =
            __builtin_sqrtf(dx * dx + dy * dy + dz * dz);
        if (d >= PB_LISTEN_R)
            return;
        if (d > PB_LISTEN_R - PB_FADE_M)
            gain *= (PB_LISTEN_R - d) / PB_FADE_M;
    }
    PcmPlayAt2(pb_blob + off, n, gain * PbVol(), TRAP_RATE, 0, pos);
}

// the armed bomb's own sounds (subtle enough not to distract, loud enough to notice): 2013 plays
// bomb-timed-idle-mp (arming clicks + sizzle in ONE wave, so at ONE level) as a near-field sound; SP's trap mine has no idle sound at all.
// The old gains had the clicks 3.3x over the sizzle (-37 dBFS vs -52) and the sizzle on the 3 - 30 m camera curve (heard across a room).
// Now: clicks and sizzle at the 2013 wave's own relation (one gain), each with its own short range by the distance of OUR body to the bomb
// (full within PB_NEAR_FULL, quadratic to silent at PB_NEAR_FAR) - you notice it when you are about to step on it, not from across the room.
// 2013 data (PS3 dc1/audio.bin sfx-info of sfx-bomb-timed-idle-mp, index 0x39bd8 -> record 0x67ec4, half floats at +8): {level -24 dB, full within 3 m,
// silent at 16 m, 0.7}; countdown-mp / place {-6 dB, 5 m, 20 m, 0.8}; the clicks are in the same wave = same event = same 3 / 16 m. (No separate
// longer-range fuse event: bomb-timed-idle-mp is the one wave, clicks + sizzle loop.) The curve shape is not in the record: quadratic kept.
#define PB_NEAR_FULL 3.0f
#define PB_NEAR_FAR 16.0f
#define PB_IDLE_GAIN 0.09f      // the sizzle: -57 dBFS RMS at full (was 0.09 = -52 dBFS on the long curve)
#define PB_CLICK_GAIN 0.08f     // the arming clicks: ~-52 dBFS RMS, single ticks (was 0.30 = -37 dBFS) - the planter hears them, not the map
#define PB_COUNT_GAIN 0.22f     // the trigger countdown: the warning, kept clear (was 0.15; -42 / -44 dBFS), normal range (pb_snd)
static void PbSndNear(int off, int n, float gain, const float *pos,
                        float rfar)
{
    float lb[16];
    const float *pl = PlayerLocator(lb);
    if (!pl)
        return;
    float dx = pos[0] - pl[0], dy = pos[1] - pl[1], dz = pos[2] - pl[2];
    if (dx * dx + dy * dy + dz * dz > (rfar + 3.0f) * (rfar + 3.0f))
        return;                 // far away: not started (a voice in reach follows us live)
    PcmPlayAtRange(pb_blob + off, n, gain * PbVol(), TRAP_RATE, pos,
                      PB_NEAR_FULL, rfar);
}

// SP's own trap-mine sounds (module aud-sfx-wpn-trap-bomb, bank wpn-trap-bomb = MP level add in dc_data_patches.py): the plant's
// sfx-trap-bomb-arm-short / -arm-end on the hand and sfx-trap-bomb-deploy on the mine (SP: SND o=<the mine> j=-). Native mix, played with
// play-sound-at-joint like the other game sfx (play-sound-at-position ignores the game volume). t2r-no-trap-sp-sfx.txt = the 2013 place
// wave instead and no arm sounds.
static int PbIdle2013(void)
{
    static int c = -1;
    static uint64_t at;
    return !PbFile("t2r-pb-no-idle.txt", &c, &at);
}                               // The armed mine sizzles like a pipe bomb by default (was opt-in t2r-pb-2013-idle.txt)   // the 2013 clicks + sizzle on SP's (silent) mine

static int PbSpSfxOff(void)
{
    static int off = -1;
    static uint64_t at;
    if (off < 0 || GetTickCount64() - at > 2000) {
        FILE *f = ExeSideOpen("t2r-no-trap-sp-sfx.txt", "r");
        off = f != NULL;
        if (f)
            fclose(f);
        at = GetTickCount64();
    }
    return off;
}

static uint64_t PbGameSfx(const char *name, uint64_t ent,
                            const char *joint)
{
    static int nl;
    uint64_t a[5] = { FNV64(name), ent, joint ? FNV64(joint) : 0, 0, 0 };
    uint64_t r = ent ? DcCall(FNV64("play-sound-at-joint"), 5, a) : ~0ULL;
    if (nl < 40) {
        nl++;
        Xlog("pbomb: sp sfx %s on %llx/%s -> %llx", name,
             (unsigned long long) ent, joint ? joint : "-",
             (unsigned long long) r);
    }
    return r;
}

// ---- the blast = SP's trap mine (the pipe bomb model with the trap mine's effects)
// SP (sp-sandbox round 2, 3 mines going off): spawn-particles-at-bound-frame(trap-mine, <the mine's bound frame>, none) and
// spawn-particles-at-bound-frame(spore-push-field, ..., none), sfx-trap-bomb on the mine object (mode 3). trap-mine is in part module
// part-actor-trap-mine (loaded with actor trap-mine, an MP level add; held resident by feel.c's module pin), spore-push-field in
// part-game-parts-module-1, sfx-trap-bomb in bank wpn-trap-bomb (MP level add). The pipe bomb is hidden from the blast on (pb_tick) and
// killed when the sound has rung out. No engine explode: the damage is the 2013 ring only (ex13), so the puppet invincibility guard is
// not needed either. 1 = the trap-mine particles spawned; 0 = they did not (the caller falls back to the engine's pipe-bomb explode).
// t2r-pb-pipe-blast.txt = the pipe bomb's own explode (engine) as before.
static int PbPipeBlast(void)
{
    static int on = -1;
    static uint64_t at;
    if (on < 0 || GetTickCount64() - at > 2000) {
        FILE *f = ExeSideOpen("t2r-pb-pipe-blast.txt", "r");
        on = f != NULL;
        if (f)
            fclose(f);
        at = GetTickCount64();
    }
    return on;
}

static int PbTrapBlast(int i, uint8_t *p)
{
    static PsyncBf bf[2];      // the engine reads the frame during the call; static so it stays valid either way
    float at[3] =
        { g_pb[i].pos[0], g_pb[i].pos[1], g_pb[i].pos[2] }, cp[3];
    if (p && ProcPos(p, cp) && __builtin_fabsf(cp[0] - at[0]) < 1.0f
        && __builtin_fabsf(cp[2] - at[2]) < 1.0f
        && __builtin_fabsf(cp[1] - at[1]) < 0.5f)
        memcpy(at, cp, 12);
    if (!__builtin_isfinite(at[0]) || !__builtin_isfinite(at[1])
        || !__builtin_isfinite(at[2])) {
        Xlog("pbomb: bomb %u trap-mine blast (pb_trap_blast): non-finite position - spawn skipped (IsFinite(bf) guard)", g_pb[i].id);
        return 0;
    }
    float ap[7] = { at[0], at[1], at[2], 0.0f, 0.0f, 0.0f, 1.0f };      // upright, as the resting mine's frame
    PsyncBfMake(&bf[0], ap);
    PsyncBfMake(&bf[1], ap);
    uint64_t a[3] =
        { FNV64("trap-mine"), (uint64_t) & bf[0], FNV64("none") };
    uint64_t r = DcCall(FNV64("spawn-particles-at-bound-frame"), 3, a);
    int ok = r && r != ~0ULL;
    uint64_t r2 = ~0ULL, s = ~0ULL;
    if (ok) {
        uint64_t b[3] =
            { FNV64("spore-push-field"), (uint64_t) & bf[1],
    FNV64("none") };
        r2 = DcCall(FNV64("spawn-particles-at-bound-frame"), 3, b);
        if (g_pb[i].h)
            s = PbGameSfx("sfx-trap-bomb", g_pb[i].h, NULL);
    }
    Xlog("pbomb: bomb %u trap-mine blast at (%.2f %.2f %.2f): particles trap-mine -> %llx, spore-push-field -> %llx, sfx-trap-bomb -> %llx%s", g_pb[i].id, at[0], at[1], at[2], (unsigned long long) r, (unsigned long long) r2, (unsigned long long) s, ok ? "" : " - trap-mine did not spawn (part-actor-trap-mine not loaded?): pipe-bomb explode instead");
    return ok;
}

// ---- listen mode = SP's trap-mine glow (drop the white object-highlight, use the system SP's mine uses)
// SP (sp-sandbox round 3, LISTEN add at the plant-mine event): the deployed ProjectileThrowable is put into the engine's listen-mode list ONCE,
// at deploy, outside listen mode, by its show vfunc +0x920 = 0x141b0f610 (sets bit 32 of obj+0x178, ListenModeMgr add 0x1413dfc10(mgr
// 0x14361e310, obj, parent 0, flags 0, fade 0.0)), no buddy draw mode, and it stays there until it is picked up / explodes (remove 0x1413e6d20
// via +0x928). listen-mode-visible (native 0x140d50350) resolves its argument as ANY process (0x141c46740) and calls exactly that vfunc
// (edx 0 = no parent, xmm2 = fade), and the ProjectileThrowable vtable (exe+0x2dbb870) has 0x141b0f610 at +0x920 - so the old call reached
// the same add. What differed: the old code (9614f0f / df6d429) added the bomb only WHILE listen was held (the add came after the listen pulse
// had started), with a fade of 0.3 / -1 and buddy-force-interactive, and took it out again on every release - SP's object is registered
// before listen starts and never toggled. Now: SP's call (listen-mode-visible(h, 1, 0, 0.0)) when the bomb is within PB_LISTEN_R of us
// (the 8 m reveal range; out again past PB_LISTEN_R + 1 m), ENEMY bombs only (own / teammates' bombs never glow); the engine draws it only while listen
// mode is on. pb_lm_diag logs the manager's entry for the bomb (registered index, meshes, fade, flags) next to an enemy puppet's, so a
// bomb that still draws nothing shows why. Enemy bombs, once marked by anyone on the team (mark button, MSG_MINE op 5): also RED
// for the whole team until the bomb is gone (listen-mode-set-color 0x140d50250 is an assert stub in the retail exe, and the buddy draw mode
// native 0x14062f500 only sets the draw-control flag 0x40000000 = the buddy look, so the red is the object-highlight pass as the marks).
// A dump of the enemy bomb on the other instance (build bdb645e) has it registered exactly as SP's deployed
// mine - entry 11, flags 8c (0x80 live, 0x08 rows added, 0x04 = add flags bit 0 clear), alpha 1.0, fade 0, all 3 mesh rows with bit 4 and
// their listen slot 12 (= index + 1) written; bit 30 is NOT a draw gate (the add 0x1413dfc10 sets it only for meshes whose parent resolves
// to a character, i.e. a character's cloth / weapons). The CPU side is complete; the gate is on the GPU side:
// THE DRAW GATE IS THE MESH'S MATERIAL (static RE + sp-common.psarc actor97/*.pak). The per-instance listen slot that 0x14162fa40
// writes ([[flags + 0x68] + 0x258], then dirty bit 0x10 at flags + 4) is a SHADER CONSTANT of the mesh instance (the 0x280-byte per-instance
// block 0x14162cb70 makes at the add); the listen glow is drawn by the material's own listen-mode techniques (MainVsListenModeVisible /
// MainPsListenModeVisible / ...Hidden, ListenModeVisibleV2). Those exist in physical-character.fx materials only:
//   trap-mine(-basic / -upgrade).pak  material objects/scavenge/canister/trapmine:default  = physical-character.fx  -> ListenModeVisible/Hidden(V2)
//   pipe-bomb(-basic / -upgrade).pak  material objects/weapons/pipe-bomb/pipe-bomb:default = physical-blend.fx      -> no ListenMode* technique
//   (characters' bodies, cloth and weapons are physical-character too - that is why puppets draw and the pipe bomb never did)
// So SP's mine draws because its MESH can, and nothing the CPU sets makes the pipe bomb draw.
// Second pass (pipe bombs still did not show up in listen mode): the material swap
// (change-object-material to the trap mine's, f23f7d6) drew nothing either, and the static trace says why. SP's deployed mine (sp-sandbox round 3,
// TRACK add ... why=listen_mode_add) has obj+0x178 = 0x400900040c00, a thrown bottle in the same list 0x900040400. Bit 11 (0x800) is set at
// object init by 0x141b08db5 from the MESH's own render flags ([mesh instance + 0x1e0] -> +0x1e0 / +0x1e4 bit 14, 0x14162e930), i.e. a
// property baked into the mesh with its material in the pak (bits 9 / 10 / 12 likewise from bits 4 / 5 / 7) - a per-instance material
// override cannot change it, and the pipe bomb's mesh does not have it. (Bit 46 is only the pickup / interact registration, 0x141b0f690.)
// So the listen draw is a property of the trap mine's MESH. SP's data, used as SP uses it: an ENEMY bomb in reach gets a stand-in = SP's
// trap-mine object (art trap-mine-upgrade, an MP level add, spawn-object SimpleAttachable as the lockbox / armour props - no physics, so it
// never pushes the pipe bomb) on the bomb's spot, put into the listen-mode list exactly like SP's deployed mine (listen-mode-visible(o, 1,
// 0, 0.0) once, outside listen; the base show vfunc 0x141b0f610 is shared by 134 vtables, ProjectileThrowable's and the props' alike). Each
// frame one of the two is hidden (hide-object-for-one-frame): outside listen the stand-in (only the pipe bomb is seen), while
// listen mode is on the pipe bomb (the listen pass draws the trap mine's glow, SP's look). t2r-pb-no-listen-standin.txt = the old route
// (the pipe bomb itself in the list, draws nothing). t2r-pb-mine-look.txt (the bomb IS the trap mine) needs no stand-in.
static int PbNoStandin(void)
{
    static int off = -1;
    static uint64_t at;
    if (off < 0 || GetTickCount64() - at > 2000) {
        FILE *f = ExeSideOpen("t2r-pb-no-listen-standin.txt", "r");
        off = f != NULL;
        if (f)
            fclose(f);
        at = GetTickCount64();
    }
    return off;
}

static uint64_t PbLmSpawn(int i)
{                               // SP's trap-mine object on the bomb's ground spot, or 0
    static const char *const ART[2] = { "trap-mine-upgrade", "trap-mine" };
    static int nl;
    for (int k = 0; k < 2; k++) {
        uint64_t a[9] =
            { FNV64(ART[k]), FNV64("SimpleAttachable"), 0, 0, 1, 0, 0, 0,
    0 };
        uint64_t o = DcCall(FNV64("spawn-object"), 9, a);
        if (!o || o == ~0ULL) {
            if (nl < 12) {
                nl++;
                Xlog("pbomb: listen stand-in: spawn-object %s failed (actor not in the MP level?)", ART[k]);
            }
            continue;
        }
        static __attribute__((aligned(16))) float tp[4], tq[4];
        memcpy(tp, g_pb[i].pos, 12);
        tp[3] = 1.0f;
        tq[0] = tq[1] = tq[2] = 0.0f;
        tq[3] = 1.0f;           // upright on the floor, as SP's mine lies
        uint64_t bfa[5] = { (uint64_t) tp, (uint64_t) tq, 0, 0, 0 };
        uint64_t bf =
            DcCall(0x871ec52cc86b7427ULL /* alloc-bound-frame */ , 5,
                    bfa);
        uint64_t la[2] = { o, bf };
        uint64_t r = bf
            && bf != ~0ULL ? DcCall(FNV64("set-object-locator"), 2,
                                     la) : 0;
        if (nl < 12) {
            nl++;
            Xlog("pbomb: bomb %u listen stand-in = SP's %s object %llx at (%.2f %.2f %.2f) (set-object-locator -> %llx)", g_pb[i].id, ART[k], (unsigned long long) o, tp[0], tp[1], tp[2], (unsigned long long) r);
        }
        return o;
    }
    return 0;
}

static void PbListen(uint64_t h, int on)
{
    uint64_t a[4] = { h, (uint64_t) (on ? 1 : 0), 0, 0 };       // fade 0.0 (SP), no attached children
    DcCall(0x7f1369bacaaca1e5ULL /* listen-mode-visible */ , 4, a);
}

static uint8_t *PbLmMgr(uint8_t **gflag)
{                               // ListenModeMgr (from the lea in the show vfunc) + its global skip byte
    uint8_t *exe = (uint8_t *) GetModuleHandleW(NULL);
    static const uint8_t show[7] =
        { 0x48, 0x83, 0xec, 0x38, 0x48, 0xb8, 0x00 };
    static const uint8_t find[9] =
        { 0x44, 0x8b, 0xd2, 0x4c, 0x8b, 0xc9, 0x45, 0x33, 0xc0 };
    if (memcmp(exe + 0x1b0f610, show, 6)
        || memcmp(exe + 0x13e5da0, find, 9) || exe[0x1b0f62b] != 0x48
        || exe[0x1b0f62c] != 0x8d || exe[0x13dfc3f] != 0x80)
        return NULL;
    if (gflag)
        *gflag = exe + 0x13dfc46 + *(int32_t *) (exe + 0x13dfc41);
    return exe + 0x1b0f632 + *(int32_t *) (exe + 0x1b0f62e);
}

// ---- THE MISSING PIECE (static RE of the ListenModeMgr per-frame update 0x1413e8c70): being in the listen-mode list
// draws NOTHING by itself. Every frame the update zeroes each entry's intensity (entry +0x24 = mgr +0x13c, and the sum +0x20 = +0x138,
// at 0x1413e9038 / 0x1413e90d4) and raises it again only from the manager's REVEAL SOURCES (mgr +0x7118, 0x90 bytes each, count at
// mgr +0x90, max 0x800): a source names an entry (+0x2c), a position (+0x00), a radius (+0x14), an alpha (+0x18 / +0x1c) and an expiry
// (+0x28; expired / orphaned sources are dropped at 0x1413e8e03); intensity = max over the entry's sources (0x1413e94c6). An entry
// without a source stays at alpha 0 = invisible. That is exactly the dump of the stand-in while listen was held: entry +0x24
// 1.0 -> 0.0 (the add writes 1.0, the first update zeroes it, no source raises it).
// SP's deployed mine HAS a source: ProjectileThrowable vfunc +0xaf8 = 0x1411ce2b0 (run by the projectile's settle state enter
// 0x1411cd890) calls the reveal add 0x1413e03a0(mgr, obj, params) with params = {its own position, ints 0 0, radius 10.0, duration
// FLT_MAX (never expires), fade-in 0.0, alpha 1.0, 1.0, key sid 0x9dcfd4d3b3ca125d, an empty DC ref (+0x38), a DC ref by sid
// 0x3a1b17f266072f78 (+0x60, built by 0x141574ae0)}, gated by a flag of the projectile's settings ([obj+0x6c0] -> +0x18c) that our
// pipe bomb / stand-in never pass through. The same add is behind the native listen-mode-add-reveal (0x140d4fcc0) - also what makes
// sound reveals glow. 0x1413e03a0 itself: object-age fade-in (0x1413e6300), alpha x vfunc +0x428 / min vfunc +0x420 (both 1.0 for
// non-characters: 0x140304290 / 0x1403042a0), under the manager lock, then 0x1413e0130 (dedupes by key sid + pid). Its one gate:
// game mode 5 + flag 0x14416bf98 + faction relation (obj +0x200 byte vs faction 2) == 2 (friendly) -> skip; the call below sets the
// object's faction byte to 0xff (no faction: relation 0) for the call only, so a teammate-coloured stand-in is never skipped.
static int PbLmSources(uint8_t *m, uint32_t pid)
{                               // how many reveal sources name this object
    uint32_t n = *(uint32_t *) (m + 0x90), c = 0;
    for (uint32_t k = 0; k < n && k < 0x800; k++)
        if (*(uint32_t *) (m + 0x7118 + 0x90 * k + 0x30) == pid)
            c++;
    return (int) c;
}

static void PbLmReveal(uint64_t h, const float *pos, const char *who)
{
    typedef void *(*fn_proc)(uint64_t);
    static int nl;
    uint8_t *exe = (uint8_t *) GetModuleHandleW(NULL), *m =
        PbLmMgr(NULL);
    uint8_t *p = h ? ((fn_proc) T2R_SYM(kFnEntityFromHandle)) (h) : NULL;
    static const uint8_t add[10] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10 };      // 0x1413e03a0
    static const uint8_t ref[6] = { 0x53, 0x48, 0x83, 0xec, 0x20, 0x33 };       // 0x141574ae0
    if (!m || !p || IsBadReadPtr(p, 0x400)
        || memcmp(exe + 0x13e03a0, add, 10)
        || memcmp(exe + 0x1574ae0, ref, 6) || exe[0x13e03e7] != 0x0f
        || exe[0x13e03e8] != 0xb6
        || *(uint32_t *) (exe + 0x13e03ea) != 0x200) {
        if (nl < 12) {
            nl++;
            Xlog("pbomb: listen reveal %s - manager / object / code not as expected, no reveal source", who);
        }
        return;
    }
    static __attribute__((aligned(16))) uint8_t prm[0x90];
    memset(prm, 0, sizeof prm);
    float *f = (float *) prm;
    f[0] = pos[0];
    f[1] = pos[1];
    f[2] = pos[2];
    f[3] = 1.0f;
    *(float *) (prm + 0x18) = 10.0f;    // radius (SP's value)
    *(float *) (prm + 0x1c) = 3.402823466e+38f; // duration: never expires (SP: FLT_MAX)
    *(float *) (prm + 0x20) = 0.0f;
    *(float *) (prm + 0x24) = 1.0f;
    *(float *) (prm + 0x28) = 1.0f;
    *(uint64_t *) (prm + 0x30) = 0x9dcfd4d3b3ca125dULL; // SP's key sid
    *(int32_t *) (prm + 0x38 + 0x20) = -1;      // empty DC ref (0x141574b20)
    ((void *(*)(void *, uint64_t))(exe + 0x1574ae0))(prm + 0x60,
                                                     0x3a1b17f266072f78ULL);
    uint32_t pid = *(uint32_t *) (p + 0x94);
    int before = PbLmSources(m, pid);
    uint8_t fac = p[0x200];
    p[0x200] = 0xff;
    ((void (*)(void *, void *, void *)) (exe + 0x13e03a0)) (m, p, prm);
    {                           // the object may have been moved by the engine's heap during the call: the faction byte goes back onto the object as it stands NOW (a moved
        // copy carries our 0xff), never into the old block (that would write into whatever now lives there)
        uint8_t *p2 = ((fn_proc) T2R_SYM(kFnEntityFromHandle)) (h);
        if (p2)
            p2[0x200] = fac;
    }
    int after = PbLmSources(m, pid);
    if (nl < 24) {
        nl++;
        Xlog("pbomb: listen reveal %s pid %u: SP's mine source (r 10, forever) at (%.2f %.2f %.2f) -> sources of it %d -> %d (mgr %u total), faction byte was %02x", who, pid, pos[0], pos[1], pos[2], before, after, *(uint32_t *) (m + 0x90), fac);
    }
}

static void PbLmState(uint64_t h, const char *who)
{                               // the entry's per-frame result while listen is on
    typedef void *(*fn_proc)(uint64_t);
    typedef int (*fn_find)(void *, uint32_t);
    uint8_t *exe = (uint8_t *) GetModuleHandleW(NULL), *m =
        PbLmMgr(NULL);
    uint8_t *p = h ? ((fn_proc) T2R_SYM(kFnEntityFromHandle)) (h) : NULL;
    if (!m || !p || IsBadReadPtr(p, 0x200))
        return;
    uint32_t pid = *(uint32_t *) (p + 0x94);
    int idx = ((fn_find) (exe + 0x13e5da0)) (m, pid);
    float op[3] = { 0, 0, 0 };
    ProcPos(p, op);
    if (idx < 0) {
        Xlog("pbomb: listen state %s pid %u - not in the list", who, pid);
        return;
    }
    uint8_t *en = m + 0x118 + 0x60 * idx;
    Xlog("pbomb: listen state %s pid %u idx %d: alpha %.2f sum %.2f mesh pos (%.2f %.2f %.2f) object (%.2f %.2f %.2f) in-range %d tint %.2f %.2f %.2f flags %x, %d sources | mgr on %d", who, pid, idx, *(float *) (en + 0x24), *(float *) (en + 0x20), *(float *) (en + 0x38), *(float *) (en + 0x3c), *(float *) (en + 0x40), op[0], op[1], op[2], en[0x50], *(float *) (en + 0x44), *(float *) (en + 0x48), *(float *) (en + 0x4c), en[0x5c], PbLmSources(m, pid), m[0]);
}

static void PbLmEntry(const char *who, uint8_t *p)
{
    typedef int (*fn_find)(void *, uint32_t);
    uint8_t *gf = NULL, *m = PbLmMgr(&gf);
    if (!m || !p || IsBadReadPtr(p, 0x200)) {
        Xlog("pbomb: listen diag %s - manager / object not readable", who);
        return;
    }
    uint8_t *exe = (uint8_t *) GetModuleHandleW(NULL);
    int idx = ((fn_find) (exe + 0x13e5da0)) (m, *(uint32_t *) (p + 0x94));
    uint32_t rows = *(uint32_t *) (m + 0x8c), n = 0;
    for (uint32_t k = 0; idx >= 0 && k < rows && k < 0x400; k++)
        if (*(uint16_t *) (m + 0x3120 + 16 * k) == (uint16_t) idx)
            n++;
    uint8_t *dc = *(uint8_t **) (p + 0x1b0);
    char e[160] = "not in the list";
    if (idx >= 0) {
        uint8_t *en = m + 0x118 + 0x60 * idx;
        snprintf(e, sizeof e,
                 "entry %d: flags %x alpha0 %.2f fade %.2f mode %x joint %d",
                 idx, en[0x5c], *(float *) (en + 0x28),
                 *(float *) (en + 0x2c), *(uint32_t *) (en + 0x14),
                 *(int32_t *) (en + 0x1c));
    }
    Xlog("pbomb: listen diag %s pid %u obj+0x178 bit32 %d draw-ctrl %p meshes %d - %s, %u mesh rows of it | mgr skip %d byte5 %d objects %u mesh rows %u/1024", who, *(uint32_t *) (p + 0x94), (int) ((*(uint64_t *) (p + 0x178) >> 32) & 1), (void *) dc, dc && !IsBadReadPtr(dc, 0x18) ? *(int32_t *) (dc + 0x14) : -1, e, n, gf ? *gf : -1, m[5], *(uint32_t *) (m + 0x88), rows);
    if (idx >= 0) {             // the whole entry + its mesh rows' flags, same format as the SP sandbox's LMENT line (compare with SP's trap mine)
        typedef uint32_t *(*fn_iflags)(void *);
        uint8_t *en = m + 0x118 + 0x60 * idx;
        char hx[0x60 * 2 + 1];
        for (int j = 0; j < 0x60; j++)
            sprintf(hx + 2 * j, "%02x", en[j]);
        char rw[512];
        int q = 0;
        rw[0] = 0;
        for (uint32_t r = 0; r < rows && r < 0x400 && q < 200; r++) {
            if (*(uint16_t *) (m + 0x3120 + 16 * r) != (uint16_t) idx)
                continue;
            void *inst = *(void **) (m + 0x3118 + 16 * r);
            uint32_t *fp =
                inst ? ((fn_iflags) (exe + 0x13e5eb0)) (inst) : NULL;
            // Static RE of the add 0x1413dfc10): each mesh row gets its listen slot (entry index + 1) written into the instance by
            // 0x14162fa40 = [[flags + 0x68] + 0x258], but ONLY when bit 4 of the instance flags is set; bit 30 is the per-instance draw bit the
            // add sets for meshes of a character parent (0x1413dfe47) and the refresh 0x1413e66a0 keeps. Logged: slot (-1 = not readable)
            int slot = -1;
            if (fp && !IsBadReadPtr(fp, 0x70)) {
                uint8_t *rd = *(uint8_t **) ((uint8_t *) fp + 0x68);
                if (rd && !IsBadReadPtr(rd + 0x258, 4))
                    slot = *(int32_t *) (rd + 0x258);
            }
            q += snprintf(rw + q, SNLEFT(rw, q),
                          " %x/%x b4=%d b30=%d slot=%d", fp
                          && !IsBadReadPtr(fp, 4) ? *fp : 0,
                          *(uint16_t *) (m + 0x3122 + 16 * r), fp
                          && !IsBadReadPtr(fp,
                                           4) ? (int) ((*fp >> 4) & 1) :
                          -1, fp
                          && !IsBadReadPtr(fp,
                                           4) ? (int) ((*fp >> 30) & 1) :
                          -1, slot);
        }
        Xlog("pbomb: listen entry %s idx=%d (slot %d expected) entry=%s meshrows(flags/bits b4 b30 slot)=%s", who, idx, idx + 1, hx, q ? rw : " -");
    }
}

static void PbLmDiag(uint64_t h, const char *why)
{                               // the bomb's entry and, to compare, an enemy puppet's
    typedef void *(*fn_proc)(uint64_t);
    typedef uint8_t *(*fn_peer)(uint32_t);
    static int nl;
    if (nl >= 16)
        return;
    nl++;
    char w[64];
    snprintf(w, sizeof w, "bomb (%s)", why);
    PbLmEntry(w,
                h ? ((fn_proc) T2R_SYM(kFnEntityFromHandle)) (h) : NULL);
    // to compare: a puppet that IS in the list (bit 32 of +0x178: a teammate - winmm keeps their outline there - or a shown enemy);
    // a dump compared with a hidden enemy (not in the list), which showed nothing
    uint8_t *cmp = NULL;
    uint32_t cs = 0;
    for (uint32_t sl = 0; sl < T2R_MAX_PEERS && !cmp; sl++) {
        uint8_t *pe = ((fn_peer) T2rPeer) (sl);
        uint64_t ph = pe ? *(uint64_t *) (pe + 0x60) : 0;
        uint8_t *pp = ph
            && pe[4] ? ((fn_proc) T2R_SYM(kFnEntityFromHandle)) (ph) :
            NULL;
        if (pp && !IsBadReadPtr(pp, 0x180)
            && ((*(uint64_t *) (pp + 0x178) >> 32) & 1)) {
            cmp = pp;
            cs = sl;
        }
    }
    if (cmp) {
        snprintf(w, sizeof w, "%s puppet slot %u (in the list)",
                 IsTeammate(cs) ? "teammate" : "enemy", cs);
        PbLmEntry(w, cmp);
    } else
        Xlog("pbomb: listen diag - no puppet in the listen-mode list to compare with");
}

static void PbAdd(uint16_t id, int owner, const float *pos, int announce)
{
    int i = PbFind(id);
    if (i < 0)
        for (i = 0; i < PB_MAX; i++)
            if (!g_pb[i].used)
                break;
    if (i < 0 || i >= PB_MAX) {
        Xlog("pbomb: no free slot for bomb %u of slot %d", id, owner);
        return;
    }
    memset(&g_pb[i], 0, sizeof g_pb[i]);
    g_pb[i].used = 1;
    g_pb[i].id = id;
    g_pb[i].owner = owner;
    memcpy(g_pb[i].pos, pos, 12);
    g_pb[i].rad = PbOwnerRadius(owner);
    Xlog("pbomb: bomb %u of slot %d: trigger sphere %.1f m (3.0 x Explosion Expert scale), blast %.3f s after the trigger (x Bomb Expert)", id, owner, g_pb[i].rad, PbFuseS(owner));
    g_pb[i].placed_ms = g_pb[i].spawn_ms = GetTickCount64();
    g_pb[i].h = PbSpawn(pos);
    g_pb[i].pipe = g_pbSpawnPipe;     // set down at rest where the hand left it
    g_pb[i].q_pipe = (int) InvQty(PB_PIPE_SID);
    g_pb[i].q_trap = (int) InvQty(PB_TRAP_SID);
    memcpy(g_pb[i].last, pos, 12);
    g_pb[i].last[1] += PB_REST_H;
    {                           // SP: sfx-trap-bomb-deploy on the mine at the plant-mine event (= now); heard as far as the bomb's other sounds (PB_LISTEN_R)
        float lb[16];
        const float *pl = PlayerLocator(lb);
        float d = 0;
        if (pl) {
            float dx = pos[0] - pl[0], dy = pos[1] - pl[1], dz =
                pos[2] - pl[2];
            d = __builtin_sqrtf(dx * dx + dy * dy + dz * dz);
        }
        if (PbSpSfxOff() || !g_pb[i].h)
            PbSnd(TRAP_PLACE0_OFF, TRAP_PLACE0_N, 0.135f, pos);
        else if (d < PB_LISTEN_R)
            PbGameSfx("sfx-trap-bomb-deploy", g_pb[i].h, NULL);
    }
    Xlog("pbomb: bomb %u of slot %d placed at (%.2f %.2f %.2f), projectile %llx%s", id, owner, pos[0], pos[1], pos[2], (unsigned long long) g_pb[i].h, announce ? " (ours)" : "");
}

// bombs this client removed lately (blast done / owner's remove / picked up): the peer resync re-announces a bomb with op 1, and one whose trigger or remove
// message never reached its owner (event channel reset at a migration) is still in the owner's list - it must not come back on a screen that already blew it up
static struct {
    uint16_t id;
    uint8_t owner;
    uint64_t until;
} g_pb_gone[8];
static int g_pbGoneN;
#define PB_GONE_MS 20000u
static int PbRecentlyGone(uint16_t id, uint32_t owner)
{
    uint64_t now = GetTickCount64();
    for (int k = 0; k < 8; k++)
        if (g_pb_gone[k].until > now && g_pb_gone[k].id == id
            && g_pb_gone[k].owner == (uint8_t) owner)
            return 1;
    return 0;
}

// (a bomb could be picked up after destroying it): a bomb that went off / was removed leaves its engine objects (the visual and the listen
// stand-in are SP's native trap-mine interactables) to kill-entity, and the pickup guard below stops with the g_pb row. Every object we kill is kept on a
// tombstone: while the handle still resolves (the engine frees it late, or not at all) the spot stays interact-off within reach and the kill is repeated.
// t2r-no-bomb-tomb.txt = off.
static struct {
    uint64_t h;
    float pos[3];
    uint64_t until, kill_ms;
} g_pb_tomb[8];
static int g_pbTombN;
static void PbTombAdd(uint64_t h, const float *pos)
{
    if (!h || h == ~0ULL)
        return;
    int k = g_pbTombN++ & 7;
    g_pb_tomb[k].h = h;
    memcpy(g_pb_tomb[k].pos, pos, sizeof g_pb_tomb[k].pos);
    g_pb_tomb[k].until = GetTickCount64() + 15000;
    g_pb_tomb[k].kill_ms = GetTickCount64();
}

static void PbRemove(int i, const char *why)
{
    if (!g_pb[i].used)
        return;
    if (strcmp(why, "picked up by its owner")) {
        PbTombAdd(g_pb[i].h, g_pb[i].pos);
        PbTombAdd(g_pb[i].lm, g_pb[i].pos);
    }
    int gk = g_pbGoneN++ & 7;
    g_pb_gone[gk].id = g_pb[i].id;
    g_pb_gone[gk].owner = (uint8_t) g_pb[i].owner;
    g_pb_gone[gk].until = GetTickCount64() + PB_GONE_MS;
    uint64_t h = g_pb[i].h;
    if (h) {
        if ((g_pb[i].hl & 2) && !g_pb[i].lm)
            PbListen(h, 0);
        DcCall(FNV64("kill-entity"), 1, &h);
    }
    if (g_pb[i].lm) {
        uint64_t o = g_pb[i].lm;
        PbListen(o, 0);
        DcCall(FNV64("kill-entity"), 1, &o);
    }
    Xlog("pbomb: bomb %u removed (%s)", g_pb[i].id, why);
    memset(&g_pb[i], 0, sizeof g_pb[i]);
}

static void PbTrigger(int i, int owner_of_msg)
{
    if (!g_pb[i].used || g_pb[i].trig_ms)
        return;
    g_pb[i].trig_ms = GetTickCount64();
    g_pb[i].by1 = owner_of_msg + 1;     // who set it off (the blast is judged on it: see the goes-off block)
    // bomb-timed-countdown-mp: its two layers (alert + the first 1.3 s of the beeping run). Symptom: the mine sounds that trigger when an
    // enemy walks by don't play for anyone but the victim - every client gets the trigger (MSG_MINE op 2) and played it through pb_snd, whose
    // 8 m listen-reveal gate is judged by OUR body at the start: the victim stands within 3 m of the bomb, the planter / teammates / other
    // enemies watching from further than 8 m got nothing (A's log: the owner 9.5 m away). 2013 plays the countdown as an ordinary positional
    // sound of the bomb (the warning everyone around hears), so: the normal 3 - 30 m spatial curve at the bomb, followed live, no 8 m gate.
    pcm_play_at2(pb_blob + TRAP_COUNT0_OFF, TRAP_COUNT0_N,
                 PB_COUNT_GAIN * PbVol(), TRAP_RATE, 0, g_pb[i].pos);
    PcmPlayAt2(pb_blob + TRAP_COUNT1_OFF, TRAP_COUNT1_N,
                 PB_COUNT_GAIN * PbVol(), TRAP_RATE, 0, g_pb[i].pos);
    Xlog("pbomb: bomb %u of slot %d TRIGGERED by slot %d - blast in %.2f s", g_pb[i].id, g_pb[i].owner, owner_of_msg, PbFuseS(g_pb[i].owner));
}

// a bomb shot / blown up / burnt by an enemy of its owner goes off at once (no countdown)
static void PbDetonate(int i, int by, const char *why)
{
    if (!g_pb[i].used || g_pb[i].trig_ms || g_pb[i].boomed)
        return;
    g_pb[i].trig_ms =
        GetTickCount64() - (uint64_t) (PbFuseS(g_pb[i].owner) * 1000) -
        1;
    g_pb[i].by1 = by + 1;
    Xlog("pbomb: bomb %u of slot %d DETONATED by slot %d (%s) - goes off now", g_pb[i].id, g_pb[i].owner, by, why);
}

static int PbFriendlyTo(int i, uint32_t shooter)
{                               // `shooter` is a teammate of the bomb's owner (and not the owner)
    int o = g_pb[i].owner;
    return (uint32_t) o < T2R_MAX_PEERS && shooter < T2R_MAX_PEERS
        && (uint32_t) o != shooter && SameTeam((uint32_t) o, shooter);
}

static int PbEnemyOf(int i, uint32_t slot)
{                               // the bomb is hostile to this slot (2013: the owner's team never sets it off)
    uint32_t self = *(uint32_t *) T2R_SYM(kMySlot);
    int o = g_pb[i].owner;
    if ((uint32_t) o == slot)
        return 0;
    if ((uint32_t) o < T2R_MAX_PEERS && slot < T2R_MAX_PEERS
        && SameTeam((uint32_t) o, slot))
        return 0;
    if (slot == self && (uint32_t) o == self)
        return 0;
    return 1;
}

static void PbBlastNear(int kind, int slot, const float *pos)
{                               // ex13_pend: an explosion / fire burst of `slot` at pos
    if (g_pbOff || slot < 0)
        return;
    float r = kind == EX13_MOLO ? 2.5f : 3.0f;
    for (int i = 0; i < PB_MAX; i++) {
        if (!g_pb[i].used || g_pb[i].boomed || g_pb[i].trig_ms
            || !PbEnemyOf(i, (uint32_t) slot))
            continue;
        float dx = g_pb[i].pos[0] - pos[0], dy =
            g_pb[i].pos[1] - pos[1], dz = g_pb[i].pos[2] - pos[2];
        if (dx * dx + dy * dy + dz * dz <= r * r)
            PbDetonate(i, slot, kind == EX13_MOLO ? "fire" : "explosion");
    }
}

// our shot's pellet/bullet hits (shot_resolve_hook, every firearm): the nearest hit of a ray on a placed bomb detonates it (the shooter's client judges)
static void PbShotScan(uint8_t *shot)
{
    typedef void *(*fn_proc)(uint64_t);
    typedef uint8_t *(*fn_peer)(uint32_t);
    typedef uint8_t *(*fn_me)(void);
    if (g_pbOff)
        return;
    int any = 0;
    for (int i = 0; i < PB_MAX; i++)
        if (g_pb[i].used && !g_pb[i].boomed && !g_pb[i].trig_ms)
            any = 1;
    if (!any)
        return;
    uint32_t self = *(uint32_t *) T2R_SYM(kMySlot);
    uint32_t n = *(uint32_t *) (shot + 0x548), stride =
        *(uint32_t *) (shot + 0x54c);
    uint8_t *cnt = *(uint8_t **) (shot + 0x5d0), *hits =
        *(uint8_t **) (shot + 0x5d8);
    static int nl;
    if (nl < 8) {
        nl++;
        Xlog("pbomb: shot scan - %u rays, stride %u", n, stride);
    }
    if (n > 64)
        n = 64;
    if (stride > 64 || !n || !stride || !cnt || !hits
        || IsBadReadPtr(cnt, 4 * n)
        || IsBadReadPtr(hits, (size_t) n * stride * 0x30))
        return;
    uint8_t *people[T2R_MAX_PEERS + 1] = { 0 };
    for (uint32_t sl = 0; sl < T2R_MAX_PEERS; sl++) {
        uint8_t *pe = ((fn_peer) T2rPeer) (sl);
        uint64_t h = pe ? *(uint64_t *) (pe + 0x60) : 0;
        people[sl] =
            h ? ((fn_proc) T2R_SYM(kFnEntityFromHandle)) (h) : NULL;
    }
    people[T2R_MAX_PEERS] = ((fn_me) T2R_SYM(kFnLocalPlayer)) ();
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = *(uint32_t *) (cnt + 4 * i);
        if (!c)
            continue;
        if (c > stride)
            c = stride;
        uint8_t *best = NULL;
        for (uint32_t j = 0; j < c; j++) {
            uint8_t *r = hits + ((size_t) i * stride + j) * 0x30;
            if (!best || *(float *) (r + 0x20) < *(float *) (best + 0x20))
                best = r;
        }
        uint64_t body = 0;
        uint8_t *proc = TacProcOf(*(uint64_t *) (best + 0x10), &body);
        if (!proc)
            continue;
        int person = 0;
        for (int k = 0; k <= T2R_MAX_PEERS; k++)
            if (people[k] == proc)
                person = 1;
        float pp[3];
        int have = !person && ProcPos(proc, pp);
        for (int b = 0; b < PB_MAX; b++) {
            if (!g_pb[b].used || g_pb[b].boomed || g_pb[b].trig_ms)
                continue;
            uint8_t *bp =
                g_pb[b].
                h ? ((fn_proc) T2R_SYM(kFnEntityFromHandle)) (g_pb[b].
                                                              h) : NULL;
            int hit = bp && bp == proc;
            if (!hit && have) {
                float dx = pp[0] - g_pb[b].pos[0], dy =
                    pp[1] - g_pb[b].pos[1], dz = pp[2] - g_pb[b].pos[2];
                hit = dx * dx + dy * dy + dz * dz <= 0.45f * 0.45f;
            }
            if (!hit)
                continue;
            PbDetonate(b, (int) self, "our shot");
            if (g_sendTramp) {
                uint8_t m[3];
                m[0] = 4;
                memcpy(m + 1, &g_pb[b].id, 2);
                g_sendTramp(MSG_MINE, m, sizeof m, 0xff);
            }
        }
    }
}

// the engine's explode hook asks first: 1 = a placed bomb waiting (swallow the natural fuse explode), 2 = our own explode of one (let it go)
static int PbHook(uint64_t hnd)
{
    if (!hnd)
        return 0;
    for (int i = 0; i < PB_MAX; i++) {
        if (!g_pb[i].used || g_pb[i].h != hnd)
            continue;
        return g_pbAllow ? 2 : 1;
    }
    return 0;
}

static void PbPupPlant(uint32_t from);
static void PbPupRelease(uint32_t from, const float *pos);
static void PbRecv(uint32_t from, const uint8_t *buf, uint64_t len)
{
    if (g_pbOff || !buf || len < 3 || from >= T2R_MAX_PEERS)
        return;
    uint16_t id;
    memcpy(&id, buf + 1, 2);
    if (buf[0] == 1 && len >= 15) {
        float p[3];
        memcpy(p, buf + 3, 12);
        // peer msg trust: the id carries its placer's slot (id >> 8, every build), the position is finite, and one player cannot fill the table (PB_MAX) with mines
        int live = 0;
        for (int i = 0; i < PB_MAX; i++)
            if (g_pb[i].used && g_pb[i].owner == (int) from)
                live++;
        if ((!PtMineIdOk(from, id, p) && PtDeny(PTK_mine_id, from))
            || (!PtMineCapOk(live, PB_CAP_OWNER)
                && PtDeny(PTK_mine_cap, from))) {
            static uint64_t bad_ms;
            uint64_t tn = GetTickCount64();
            if (PtLogDue(&bad_ms, tn))
                Xlog("pbomb: placement of bomb %u from slot %u dropped (id names slot %u / bad position / %d live bombs of that player)", id, from, id >> 8, live);
            return;
        }
        if (PbFind(id) < 0 && MatchSubstate() == 6
            && !PbRecentlyGone(id, from)) {
            PbAdd(id, (int) from, p, 0);
            PbPupRelease(from, p);
        }
    } else if (buf[0] == 6) {   // the owner starts its plant: the setting clip on its puppet
        if (MatchSubstate() == 6
            && from != *(uint32_t *) T2R_SYM(kMySlot))
            PbPupPlant(from);
    } else if (buf[0] == 2) {   // trigger: from the owner's screen, or from an enemy of the owner standing at the bomb
        int i = PbFind(id);
        if (i >= 0
            && ((uint32_t) g_pb[i].owner == from
                || (PbEnemyOf(i, from)
                    && PtNearSender(from, g_pb[i].pos,
                                      g_pb[i].rad + 10.0f, 8.0f))
                || !PtDeny(PTK_mine_auth, from)))
            PbTrigger(i, (int) from);
        else if (i >= 0) {
            static uint64_t bad_ms;
            uint64_t tn = GetTickCount64();
            if (PtLogDue(&bad_ms, tn))
                Xlog("pbomb: trigger of bomb %u from slot %u dropped (not its owner, not an enemy at the bomb)", id, from);
        }
    } else if (buf[0] == 3) {   // remove: its owner only
        int i = PbFind(id);
        if (i >= 0
            && ((uint32_t) g_pb[i].owner == from
                || !PtDeny(PTK_mine_auth, from)) && !g_pb[i].boomed)
            PbRemove(i, "owner's message");
        else if (i >= 0 && (uint32_t) g_pb[i].owner != from) {
            static uint64_t bad_ms;
            uint64_t tn = GetTickCount64();
            if (PtLogDue(&bad_ms, tn))
                Xlog("pbomb: remove of bomb %u (owner slot %d) from slot %u dropped", id, g_pb[i].owner, from);
        }
    } else if (buf[0] == 4) {   // shot: the owner or an enemy of the owner
        int i = PbFind(id);
        if (i >= 0
            && ((uint32_t) g_pb[i].owner == from || PbEnemyOf(i, from)
                || !PtDeny(PTK_mine_auth, from)))
            PbDetonate(i, (int) from, "shot (message)");
        else if (i >= 0) {
            static uint64_t bad_ms;
            uint64_t tn = GetTickCount64();
            if (PtLogDue(&bad_ms, tn))
                Xlog("pbomb: shot message for bomb %u from its owner's teammate slot %u dropped", id, from);
        }
    } else if (buf[0] == 5) {   // a teammate marked an enemy bomb
        uint32_t self = *(uint32_t *) T2R_SYM(kMySlot);
        int i = PbFind(id);
        if (i >= 0 && from != self && SameTeam(from, self)
            && PbEnemyOf(i, self)) {
            g_pb[i].mark_until = ~0ULL;
            Xlog("pbomb: bomb %u marked by teammate slot %u", id, from);
        }
    }
}

// peer resync (re/jip-state-resync-1019.md): the placement (op 1) goes out ONCE, so a player who joins a running match / comes back after a migration
// never had our bombs: invisible, and - the trigger is a per-client check of ITSELF against its own list - it could walk through them without
// setting them off. Our own live bombs are announced again to that slot (same op 1: pb_recv ignores an id it already has), and the bombs the team
// marked go to a teammate as op 5. Bombs of players who left cannot be repeated by anyone (the owner slot is part of the message): a documented gap.
static void PbResync(uint32_t to)
{
    if (g_pbOff || !g_sendTramp || to >= T2R_MAX_PEERS)
        return;
    uint32_t self = *(uint32_t *) T2R_SYM(kMySlot);
    int n = 0, nm = 0;
    for (int i = 0; i < PB_MAX; i++) {
        if (!g_pb[i].used || g_pb[i].boomed || g_pb[i].trig_ms)
            continue;
        if (g_pb[i].owner == (int) self) {
            uint8_t m[15];
            m[0] = 1;
            memcpy(m + 1, &g_pb[i].id, 2);
            memcpy(m + 3, g_pb[i].pos, 12);
            g_sendTramp(MSG_MINE, m, sizeof m, (uint8_t) to);
            n++;
        }
        if (g_pb[i].mark_until == ~0ULL && IsTeammate(to)) {
            uint8_t m[3];
            m[0] = 5;
            memcpy(m + 1, &g_pb[i].id, 2);
            g_sendTramp(MSG_MINE, m, sizeof m, (uint8_t) to);
            nm++;
        }
    }
    if (n || nm)
        Xlog("pbomb: resync to slot %u - %d own bomb(s), %d marked", to, n,
             nm);
}

/// ---- the plant: TLOU2's trap-mine SETTING animation on the planter's body (the trap mine's own SETTING animation is used for the plant)
// Why the gesture route never showed: SP plants with a GESTURE (ellie-plant-trap-mine-gesture -> the stance's -part clip on the gesture layer
// s_gesture-1 + its -add over the crouch idle; the hand is put on the floor by PlayerPlantMineController's hand IK from use-hand-ik on).
// Decoded offline (re/uc4ps4 ClipDecoder + t2_strip FK, abby-/ellie-skel): abby-/ellie-crouch-plant-trap-mine-part animate the ARMS only
// (spine / legs at bind: pelvis at standing height, the hand never below 1.35 m) - played full-body (animate_, the earlier fallback) that was
// the "wrong animation". gesture_ (exe 0x14210d170) returns the request's result sid (0 = taken, 0x9417267ed3882d2e locked-out,
// 0x43436c3a4206d93a gesture-anims-not-found); our plant got 0, but the revive hand-out's gesture-get-phase reads -1 on our body a frame after
// a taken request and a frozen 0.000 on puppets (seen in a log): MP bodies do not run that gesture layer, nothing became visible.
// Now TLOU2's own FULL-BODY trap-mine setting clip, ellie-boss-crouch-plant-trap-mine (anim-ellie-boss.pak: the NPC Ellie of the theater
// fight sets her trap bombs with it, EllieBossSneakSkill::PlaceTrapBomb; ellie-skel 0x5abe1b4c = the rig of the player's own clips, which
// the engine retargets to the puppets): 49 f @30, crouched (left foot forward, right knee down), the right hand carries the mine down: its
// r_hand_prop reaches the floor at f23 (0.767 s; 0.062 m up, 0.435 m ahead / 0.080 m right of the root) and stays there while the hand
// withdraws (f24-48); the clip's own sfx-trap-bomb-deploy is at f22. Played with animate_ (layer 2: the route of every visible scripted body
// clip of the ext - stun, backpack craft, takedowns) on our body and, through MSG_MINE op 6, on our puppet on every other screen. The pak is
// in no MP archive (world-abby-fights-militia.psarc): build_dll ships it, dc_data_patches adds "actor anim-ellie-boss" to the MP level.
// The pipe bomb is the held weapon, so it rides r_hand_prop down. At the release (PB_PLANT_REL_S) it is set at rest at the hand's x / z (our
// r_hand_prop, when within PB_HAND_SNAP of the checked spot) on the spot's ground; the held one is hidden until SP's grab-next-consumable
// (PB_GRAB_NEXT_S); the clip blends out at PB_PLANT_OUT_S. From standing the body is crouched for the plant and stood up after it.
// Prone (the clip is crouched) and a body without the clip (pak missing): the old gesture call, the bomb at the spot.
// t2r-no-trap-gesture.txt = the bomb at once, no animation.
// Logs "pbomb: plant ..." / "pbomb: release ...": animate_'s answer, get-animation-playing? (layer 2) at +0.15 s and at the release, and where
// the right hand is at the release (height over the spot's ground, distance to it): ~0.06 m up at the spot = the clip is on the body.
#define PB_PLANT_CLIP "ellie-boss-crouch-plant-trap-mine"
#define PB_PLANT_BLEND 0.25f
#define PB_PLANT_REL_S 0.767f   // f23: r_hand_prop on the floor (the clip's deploy sfx f22)
#define PB_PLANT_OUT_S 1.25f    // blend back out (f37: the hand is back at the knee)
#define PB_PLANT_OUT_BLEND 0.35f
#define PB_GRAB_NEXT_S 1.133f   // SP's grab-next-consumable (f34 of the -part clips): the next bomb shows in the hand again
#define PB_HAND_SNAP 0.15f
#define PB_PLANT_S 0.651f       // gesture route: SP stand / crouch plant-mine event (Ellie clip)
#define PB_PLANT_PRONE_S 0.685f // SP prone
#define PB_STAND_AFTER_S 0.50f
#define PB_ARM0_S 0.10f         // the arming sounds (SP's light clip has them when the mine is drawn; the MP item is the pipe bomb)
#define PB_ARM1_S 0.47f
// on: 1 = before the release, 2 = after it (hiding the held bomb, blending out); mode: 1 = the setting clip, 2 = the gesture route
static struct {
    int on, snd, mode, logged;
    uint64_t at, t0, out_at, hide_until;
    float pos[3];
} g_pbPend;
static int PbAnimPlaying(uint64_t ent, const char *clip)
{                               // get-animation-playing?(obj, anim, 2): 1 / 0, -1 = no answer
    uint64_t a[3] = { ent, FNV64(clip), ANIM_LAYER };
    uint64_t r =
        ent ? DcCall(FNV64("get-animation-playing?"), 3, a) : ~0ULL;
    return r == ~0ULL ? -1 : (r & 0xff) != 0;
}

static void PbHideObj(uint64_t h)
{
    if (h && h != ~0ULL) {
        uint64_t a[2] = { h, 0 };
        DcCall(FNV64("hide-object-for-one-frame"), 1, a);
    }
}

static const char *PbGestureAnswer(uint64_t r)
{
    return r == 0 ? "taken" : r ==
        0x9417267ed3882d2eULL ? "locked-out" : r ==
        0x43436c3a4206d93aULL ? "gesture-anims-not-found" : r ==
        ~0ULL ? "no call" : "other";
}

// the hand at the release: where r_hand_prop of `ent` is against the spot (logged); *snap = the hand's x / z when it is within PB_HAND_SNAP
static int PbHandAtRelease(uint64_t ent, const char *who,
                              const float *spot, float *snap)
{
    float hp[3];
    int pl = PbAnimPlaying(ent, PB_PLANT_CLIP);
    if (!GxJointPos(ent, "r_hand_prop", hp)) {
        Xlog("pbomb: release (%s) - no r_hand_prop on %llx, clip playing %d", who, (unsigned long long) ent, pl);
        return 0;
    }
    float dx = hp[0] - spot[0], dz = hp[2] - spot[2], hd =
        __builtin_sqrtf(dx * dx + dz * dz), up = hp[1] - spot[1];
    int ok = hd <= PB_HAND_SNAP && up > -0.15f && up < 0.30f;
    Xlog("pbomb: release (%s) - %s playing %d (layer %d), r_hand_prop (%.2f %.2f %.2f) = %.3f m over the spot's ground, %.3f m beside it -> %s", who, PB_PLANT_CLIP, pl, ANIM_LAYER, hp[0], hp[1], hp[2], up, hd, ok ? "the hand is down at the spot (clip on the body), bomb set under the hand" : "hand NOT at the spot (clip not on the body?), bomb at the checked spot");
    if (ok && snap) {
        snap[0] = hp[0];
        snap[2] = hp[2];
    }
    return ok;
}

// ---- the owner's puppet on the other screens: the same clip (MSG_MINE op 6 at its start), the held bomb hidden after the release (op 1)
static struct {
    uint64_t h, t0, hide_until;
    int on;
} g_pb_pup[T2R_MAX_PEERS];
static uint64_t PbPuppet(uint32_t slot)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    uint8_t *pe =
        slot < T2R_MAX_PEERS ? ((fn_peer) T2rPeer) (slot) : NULL;
    return pe && pe[4]
        && !IsBadReadPtr(pe + 0x60, 8) ? *(uint64_t *) (pe + 0x60) : 0;
}

static void PbPupPlant(uint32_t from)
{
    uint64_t h = PbPuppet(from);
    if (!h || g_peerDowned[from]) {
        Xlog("pbomb: slot %u plants - no live puppet here", from);
        return;
    }
    float len = AnimLen(h, PB_PLANT_CLIP);
    if (len <= 0.0f) {
        Xlog("pbomb: slot %u plants - its puppet %llx has no %s (anim-ellie-boss not loaded?) - no animation", from, (unsigned long long) h, PB_PLANT_CLIP);
        return;
    }
    uint64_t r = AnimateBlend(h, PB_PLANT_CLIP, PB_PLANT_BLEND);
    g_pb_pup[from].h = h;
    g_pb_pup[from].t0 = GetTickCount64();
    g_pb_pup[from].hide_until = 0;
    g_pb_pup[from].on = 1;
    Xlog("pbomb: slot %u plants - puppet %llx plays %s (%.2f s, blend %.2f) -> %llx, playing %d", from, (unsigned long long) h, PB_PLANT_CLIP, len, PB_PLANT_BLEND, (unsigned long long) r, PbAnimPlaying(h, PB_PLANT_CLIP));
}

static void PbPupRelease(uint32_t from, const float *pos)
{                               // its bomb arrived (op 1): it left the puppet's hand now
    if (from >= T2R_MAX_PEERS || !g_pb_pup[from].on)
        return;
    uint64_t now = GetTickCount64();
    g_pb_pup[from].hide_until =
        now + (uint64_t) ((PB_GRAB_NEXT_S - PB_PLANT_REL_S) * 1000.0f);
    char w[48];
    snprintf(w, sizeof w, "puppet of slot %u, %.3f s in", from,
             (now - g_pb_pup[from].t0) / 1000.0);
    PbHandAtRelease(g_pb_pup[from].h, w, pos, NULL);
}

static void PbPupTick(void)
{
    uint64_t now = GetTickCount64();
    for (uint32_t s = 0; s < T2R_MAX_PEERS; s++) {
        if (!g_pb_pup[s].on && !g_pb_pup[s].hide_until)
            continue;
        uint64_t h = PbPuppet(s);
        if (h != g_pb_pup[s].h || g_peerDowned[s]) {
            if (g_pb_pup[s].on)
                Xlog("pbomb: slot %u puppet changed / downed mid plant - clip left to it", s);
            memset(&g_pb_pup[s], 0, sizeof g_pb_pup[s]);
            continue;
        }
        if (g_pb_pup[s].hide_until) {
            if (now < g_pb_pup[s].hide_until) {
                uint64_t a = h, w =
                    DcCall(FNV64("npc-get-weapon-process-id"), 1, &a);
                PbHideObj(w);
            } else
                g_pb_pup[s].hide_until = 0;
        }
        if (g_pb_pup[s].on
            && now - g_pb_pup[s].t0 >=
            (uint64_t) (PB_PLANT_OUT_S * 1000.0f)) {
            AnimEndBlend(h, PB_PLANT_CLIP, PB_PLANT_OUT_BLEND);
            g_pb_pup[s].on = 0;
        }
    }
}

static void PbPlaceCommit(const float *pos)
{
    uint32_t self = *(uint32_t *) T2R_SYM(kMySlot);
    if (!InvQty(PB_PIPE_SID)) {
        Xlog("pbomb: no bomb left at the plant frame");
        return;
    }
    if (g_downed || LocalHp() <= 0.0f) {
        Xlog("pbomb: plant cancelled (downed / dead)");
        return;
    }
    {
        int mine = 0;
        for (int i = 0; i < PB_MAX; i++)
            if (g_pb[i].used && g_pb[i].owner == (int) self)
                mine++;
        if (mine >= PB_CAP_OWNER) {
            T2R_PROMPT_MSG("TOO MANY BOMBS PLACED", 1.5f, 0, 0);
            return;
        }
    }
    uint16_t id = (uint16_t) ((self << 8) | g_pbSeq++);
    uint64_t r =
        DcDirect(FNV64("player-inventory-consume-item"), PB_PIPE_SID, 1,
                  0);
    Xlog("pbomb: bomb consumed (%llx), %d left", (unsigned long long) r,
         (int) InvQty(PB_PIPE_SID));
    Xlog("placed bomb: id %u at %.2f %.2f %.2f, %d bomb(s) left in the pack, own throw %lld ms ago", id, pos[0], pos[1], pos[2], (int) InvQty(PB_PIPE_SID), g_ownThrowMs ? (long long) (GetTickCount64() - g_ownThrowMs) : -1LL);
    PbAdd(id, (int) self, pos, 1);
    if (g_sendTramp) {
        uint8_t m[15];
        m[0] = 1;
        memcpy(m + 1, &id, 2);
        memcpy(m + 3, pos, 12);
        g_sendTramp(MSG_MINE, m, sizeof m, 0xff);
    }
}

// standing plant (the character crouches down and places it on the floor): a plant from standing forces the crouch
// (player-force-crouch every frame, as the reach crouch does) for the whole plant and stands back up after it (player-force-stand once)
static struct {
    uint64_t until;
    int stand_after;
} g_pbCrouch;
static void PbCrouchTick(void)
{
    if (!g_pbCrouch.until)
        return;
    uint64_t none = 0;
    if (GetTickCount64() < g_pbCrouch.until && !g_downed
        && LocalHp() > 0.0f) {
        DcCall(FNV64("player-force-crouch"), 0, &none);
        return;
    }
    if (g_pbCrouch.stand_after && !g_downed && LocalHp() > 0.0f)
        DcCall(FNV64("player-force-stand"), 0, &none);
    g_pbCrouch.until = 0;
    g_pbCrouch.stand_after = 0;
}

static void UiDeny(const char *msg);
static void PbPlaceTry(void)
{
    static int held;
    static uint64_t last_place;
    PbCrouchTick();
    PbPupTick();
    uint32_t self = *(uint32_t *) T2R_SYM(kMySlot);
    if (g_pbPend.on) {
        uint64_t now = GetTickCount64(), el = now - g_pbPend.t0;
        if (g_downed || LocalHp() <= 0.0f) {
            if (g_pbPend.mode == 1 && g_pbPend.on == 1)
                Xlog("pbomb: plant cancelled (downed / dead) %.3f s in",
                     el / 1000.0);
            g_pbPend.on = 0;   // (the down / death animation takes the body)
            return;
        }
        if (g_pbPend.on == 2) {        // after the release: the hand is empty until grab-next, then the clip blends out
            if (now < g_pbPend.hide_until) {
                uint64_t none = 0, w =
                    DcCall(FNV64("get-player-weapon-process-id"), 0,
                            &none);
                PbHideObj(w);
            }
            if (g_pbPend.mode == 1 && now >= g_pbPend.out_at) {
                AnimEndBlend(SID_PLAYER, PB_PLANT_CLIP,
                               PB_PLANT_OUT_BLEND);
                Xlog("pbomb: plant clip blends out %.3f s after its start (%.2f s blend)", el / 1000.0, PB_PLANT_OUT_BLEND);
                g_pbPend.on = 0;
            } else if (g_pbPend.mode != 1 && now >= g_pbPend.hide_until)
                g_pbPend.on = 0;
            return;
        }
        if (!(g_pbPend.snd & 1) && el >= (uint64_t) (PB_ARM0_S * 1000)) {
            g_pbPend.snd |= 1;
            if (!PbSpSfxOff())
                PbGameSfx("sfx-trap-bomb-arm-short", SID_PLAYER,
                            "r_wrist");
        }
        if (!(g_pbPend.snd & 2) && el >= (uint64_t) (PB_ARM1_S * 1000)) {
            g_pbPend.snd |= 2;
            if (!PbSpSfxOff())
                PbGameSfx("sfx-trap-bomb-arm-end", SID_PLAYER,
                            "r_wrist");
        }
        if (g_pbPend.mode == 1 && !g_pbPend.logged && el >= 150) {    // proof the clip runs on the body (not only that animate_ answered)
            g_pbPend.logged = 1;
            float hp[3];
            int hj = GxJointPos(SID_PLAYER, "r_hand_prop", hp);
            Xlog("pbomb: plant clip +%.3f s: %s playing %d (layer %d), r_hand_prop %.2f m over the spot's ground", el / 1000.0, PB_PLANT_CLIP, PbAnimPlaying(SID_PLAYER, PB_PLANT_CLIP), ANIM_LAYER, hj ? hp[1] - g_pbPend.pos[1] : -99.0f);
        }
        if (now < g_pbPend.at)
            return;
        float pos[3];
        memcpy(pos, g_pbPend.pos, 12);
        if (g_pbPend.mode == 1) {
            char w[40];
            snprintf(w, sizeof w, "own body, %.3f s in", el / 1000.0);
            PbHandAtRelease(SID_PLAYER, w, g_pbPend.pos, pos);
        } else
            Xlog("pbomb: release %.3f s after the gesture start (gesture route: bomb at the checked spot)", el / 1000.0);
        PbPlaceCommit(pos);
        g_pbPend.on = 2;
        g_pbPend.hide_until =
            g_pbPend.t0 +
            (uint64_t) ((g_pbPend.mode ==
                         1 ? PB_GRAB_NEXT_S : PB_PLANT_S) * 1000.0f);
        return;
    }
    if (g_pbp.st == 1) {        // rays from last frame: judge the spot
        if (g_frame == g_pbp.frame)
            return;
        g_pbp.st = 0;
        float y[PB_GRID];
        int hit[PB_GRID], nhit = 0, nwin = 0, ngr = 0;
        for (int k = 0; k < PB_GRID; k++) {
            float t = RayT(g_pbp.job[k]);
            hit[k] = t >= 0.0f && t < 0.99f;
            y[k] = hit[k] ? g_pbp.top - t * g_pbp.len : 0.0f;
            if (!(hit[k] && y[k] >= g_pbp.feet[1] - PB_FLOOR_DOWN && y[k] <= g_pbp.feet[1] + PB_FLOOR_UP) && g_pbp.job2[k]) {   // the high ray is on foliage / above the window: the ground under it
                float t2 = RayT(g_pbp.job2[k]);
                if (t2 >= 0.0f && t2 < 0.99f) {
                    float y2 = g_pbp.top2 - t2 * g_pbp.len2;
                    if (y2 >= g_pbp.feet[1] - PB_FLOOR_DOWN
                        && y2 <= g_pbp.feet[1] + PB_FLOOR_UP) {
                        hit[k] = 1;
                        y[k] = y2;
                        ngr++;
                    }
                }
            }
            nhit += hit[k];
            if (hit[k] && y[k] >= g_pbp.feet[1] - PB_FLOOR_DOWN
                && y[k] <= g_pbp.feet[1] + PB_FLOOR_UP)
                nwin++;
        }
        int best = -1;
        float best_cost = 1e9f, best_slope = 0;
        for (int k = 0; k < PB_GRID; k++) {
            if (!hit[k] || y[k] < g_pbp.feet[1] - PB_FLOOR_DOWN
                || y[k] > g_pbp.feet[1] + PB_FLOOR_UP)
                continue;
            int r = k / PB_COLS, c = k % PB_COLS, nr = 0, nc = 0;
            float sl = 0;
            static const int DR[4] = { -1, 1, 0, 0 }, DC[4] =
                { 0, 0, -1, 1 };
            for (int d = 0; d < 4; d++) {       // the slope to each neighbour that has floor
                int r2 = r + DR[d], c2 = c + DC[d];
                if (r2 < 0 || r2 >= PB_ROWS || c2 < 0 || c2 >= PB_COLS
                    || !hit[r2 * PB_COLS + c2])
                    continue;
                if (y[r2 * PB_COLS + c2] - y[k] > PB_WALL_UP)
                    continue;   // a wall / box / step rising beside it: the spot is its foot, not an edge
                float dist =
                    DR[d] ? __builtin_fabsf(PB_ROW_AHEAD[r2] -
                                            PB_ROW_AHEAD[r]) :
                    __builtin_fabsf(PB_COL_SIDE[c2] - PB_COL_SIDE[c]);
                float s1 =
                    __builtin_fabsf(y[r2 * PB_COLS + c2] - y[k]) / dist;
                if (s1 > sl)
                    sl = s1;
                if (DR[d])
                    nr++;
                else
                    nc++;
            }
            if (!nr || !nc || sl > PB_SLOPE_STEEP)
                continue;       // an edge (a neighbour a step off) or no neighbour to judge it by
            float da = PB_ROW_AHEAD[r] - PB_AHEAD, ds = PB_COL_SIDE[c];
            float cost = da * da + ds * ds + (sl > PB_SLOPE_FLAT ? 10.0f : 0.0f);       // the hand spot first; a steep point only without a flat one
            if (cost < best_cost) {
                best_cost = cost;
                best = k;
                best_slope = sl;
            }
        }
        int mine = 0;
        for (int i = 0; i < PB_MAX; i++)
            if (g_pb[i].used && g_pb[i].owner == (int) self)
                mine++;
        if (best < 0) {
            T2R_PROMPT_MSG("CAN'T PLACE THE BOMB HERE", 1.5f, 0, 0);
            Xlog("pbomb: placement refused - %s (%d/%d rays hit floor, %d within -%.2f/+%.2f m of the feet (%.2f %.2f %.2f) facing (%.2f %.2f); none with floor of slope <= %.0f deg around it)", !nhit ? "no floor in front (ledge / gap)" : !nwin ? "the floor in front is a step / ledge away from the feet" : "only edges / steep ground in front", nhit, PB_GRID, nwin, PB_FLOOR_DOWN, PB_FLOOR_UP, g_pbp.feet[0], g_pbp.feet[1], g_pbp.feet[2], g_pbp.fwd[0], g_pbp.fwd[1], atanf(PB_SLOPE_STEEP) * 57.2958f);
            if (nhit) {         // the rows as heights over the feet, near row first ('-' = no floor)
                char b2[512];
                int o = 0;
                for (int k = 0; k < PB_GRID && o < (int) sizeof b2 - 12;
                     k++) {
                    if (k)
                        o += snprintf(b2 + o, SNLEFT(b2, o),
                                      k % PB_COLS ? " " : " | ");
                    o += hit[k] ? snprintf(b2 + o, SNLEFT(b2, o), "%+.2f",
                                           y[k] -
                                           g_pbp.feet[1]) : snprintf(b2 +
                                                                     o,
                                                                     SNLEFT
                                                                     (b2,
                                                                      o),
                                                                     "-");
                }
                Xlog("pbomb: floor grid (rows %.2f..%.2f m ahead, left -> right): %s", PB_ROW_AHEAD[0], PB_ROW_AHEAD[PB_ROWS - 1], b2);
            }
            return;
        }
        {
            int r = best / PB_COLS, c = best % PB_COLS;
            if (r != PB_ROWS - 2 || c != PB_COLS / 2)
                Xlog("pbomb: hand spot %s - placed at the nearest valid floor %.2f m ahead, %+.2f m to the side (%.0f deg)", hit[(PB_ROWS - 2) * PB_COLS + PB_COLS / 2] ? "not flat / not at the feet's floor" : "has no floor", PB_ROW_AHEAD[r], PB_COL_SIDE[c], atanf(best_slope) * 57.2958f);
        }
        if (mine >= PB_CAP_OWNER) {
            T2R_PROMPT_MSG("TOO MANY BOMBS PLACED", 1.5f, 0, 0);
            return;
        }
        if (!InvQty(PB_PIPE_SID))
            return;
        float pos[3] = { g_pbp.pt[best][0], y[best], g_pbp.pt[best][1] };
        Xlog("pbomb: spot (%.2f %.2f %.2f) %.2f m from the feet, %+.2f m over them, %.0f deg to the neighbours (%d/%d grid rays on floor, %d of them the ground under foliage)", pos[0], pos[1], pos[2], __builtin_sqrtf((pos[0] - g_pbp.feet[0]) * (pos[0] - g_pbp.feet[0]) + (pos[2] - g_pbp.feet[2]) * (pos[2] - g_pbp.feet[2])), pos[1] - g_pbp.feet[1], atanf(best_slope) * 57.2958f, nhit, PB_GRID, ngr);
        last_place = GetTickCount64();
        FILE *gf = ExeSideOpen("t2r-no-trap-gesture.txt", "r");
        if (gf) {
            fclose(gf);
            PbPlaceCommit(pos);
            return;
        }
        uint64_t zero = 0, now = GetTickCount64();
        uint64_t sc = DcCall(FNV64("player-is-crouched?"), 0, &zero), sp =
            DcCall(FNV64("player-is-prone?"), 0, &zero);
        int prone = sp != ~0ULL && (sp & 0xff), crouch = !prone
            && sc != ~0ULL && (sc & 0xff), stood = !prone && !crouch;
        float len = prone ? 0.0f : AnimLen(SID_PLAYER, PB_PLANT_CLIP);
        memset(&g_pbPend, 0, sizeof g_pbPend);
        memcpy(g_pbPend.pos, pos, 12);
        g_pbPend.t0 = now;
        if (len > 0.0f) {       // the setting clip (full body), crouched
            if (stood)
                DcCall(FNV64("player-force-crouch"), 0, &zero);
            g_pbCrouch.until =
                now +
                (uint64_t) ((PB_PLANT_OUT_S +
                             PB_PLANT_OUT_BLEND) * 1000.0f);
            g_pbCrouch.stand_after = stood;
            uint64_t r =
                AnimateBlend(SID_PLAYER, PB_PLANT_CLIP, PB_PLANT_BLEND);
            Xlog("pbomb: plant - %s (%.2f s) on our body, %s, blend %.2f -> %llx, playing %d; the bomb is set down at +%.3f s, clip out at +%.2f s", PB_PLANT_CLIP, len, stood ? "standing: crouched down for it, up again after" : "crouched", PB_PLANT_BLEND, (unsigned long long) r, PbAnimPlaying(SID_PLAYER, PB_PLANT_CLIP), PB_PLANT_REL_S, PB_PLANT_OUT_S);
            if (r == ~0ULL) {
                Xlog("pbomb: animate_ refused the plant clip - bomb at once");
                PbPlaceCommit(pos);
                return;
            }
            if (g_sendTramp) {
                uint8_t m[4] = { 6, 0, 0, 0 };
                g_sendTramp(MSG_MINE, m, sizeof m, 0xff);
            }                   // our puppet plays it on the other screens
            g_pbPend.on = 1;
            g_pbPend.mode = 1;
            g_pbPend.at = now + (uint64_t) (PB_PLANT_REL_S * 1000.0f);
            g_pbPend.out_at = now + (uint64_t) (PB_PLANT_OUT_S * 1000.0f);
            return;
        }
        // prone / no clip on the body: Abby's gesture of the stance (the MP player is the Abby rig, winmm forces IsAbby), default params
        if (!prone)
            Xlog("pbomb: %s not on our body (anim-ellie-boss not in the level?) - the gesture route", PB_PLANT_CLIP);
        if (stood) {
            DcCall(FNV64("player-force-crouch"), 0, &zero);
            crouch = 1;
        }
        const char *gname =
            prone ? "abby-prone-plant-trap-mine-gesture" :
            "abby-crouch-plant-trap-mine-gesture";
        uint64_t gr = ~0ULL, pp =
            DcCall(FNV64("alloc-gesture-play-params"), 0, &zero);
        if (pp && pp != ~0ULL && !IsBadReadPtr((void *) pp, 0x90)) {
            ((uint8_t *) pp)[0x20] = 0;
            ((uint8_t *) pp)[0x21] = 0; // defaults only (a script-task flag pair crashed gesture_ outside scripts)
            uint64_t ga[3] = { SID_PLAYER, FNV64(gname), pp };
            gr = DcCall(FNV64("gesture_"), 3, ga);
        }
        float plant_s = prone ? PB_PLANT_PRONE_S : PB_PLANT_S;
        Xlog("pbomb: plant gesture %s (%s) -> %llx = %s; the bomb goes down at the spot in %.3f s", gname, prone ? "prone" : stood ? "standing, crouched for it" : "crouched", (unsigned long long) gr, PbGestureAnswer(gr), plant_s);
        if (stood) {
            g_pbCrouch.until =
                now + (uint64_t) ((plant_s + PB_STAND_AFTER_S) * 1000.0f);
            g_pbCrouch.stand_after = 1;
        }                       // SP: up again 0.5 s after the release
        if (gr == ~0ULL) {
            PbPlaceCommit(pos);
            return;
        }                       // nothing could play: at once
        g_pbPend.on = 1;
        g_pbPend.mode = 2;
        g_pbPend.at = now + (uint64_t) (plant_s * 1000);
        return;
    }
    int lb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 || PadFireDown() || PadCmdOn("fire-press"), edge = lb && !held;   // LMB, or RT (raw: no aim, so the game fire command may not be live) / the fire command
    held = lb;
    // the throw's projectile spawns ~8-13 frames (0.1-0.2 s) AFTER the aim is released and the type-2 throw message comes later still, so a click in
    // that gap saw no throw yet (thr_ago huge, aim already off): the plant clip started, then the thrown bomb took the pack's item (log: 'no bomb left at the plant
    // frame' 3 frames after 'bombfuse: velocity setter call (own throw fresh)') or, with spares, both bombs went. Remember when the pipe bomb was last aimed.
    static uint64_t aim_ms;
    if (!g_pbPend.on && InvQty(PB_PIPE_SID)
        && BaseWeapon(CurrentWeapon()) == PB_PIPE_SID) {
        uint64_t z0 = 0, av = DcCall(FNV64("player-is-aiming?"), 0, &z0);
        if (av != ~0ULL && (av & 0xff))
            aim_ms = GetTickCount64();
    }
    int dev_plant = 0;
#ifndef T2R_RELEASE
    {                           // dev (A/B tests without input): t2r-dev-plant.txt next to the exe = a left click with the pipe bomb in the pack (one is given when none), then the file is removed
        static uint64_t poll;
        if (g_frame - poll >= 30) {
            poll = g_frame;
            FILE *df = ExeSideOpen("t2r-dev-plant.txt", "r");
            if (df) {
                fclose(df);
                char pa[MAX_PATH];
                GetModuleFileNameA(NULL, pa, MAX_PATH);
                char *sa = strrchr(pa, 92);
                if (sa) {
                    strcpy(sa + 1, "t2r-dev-plant.txt");
                    remove(pa);
                }
                if (!InvQty(PB_PIPE_SID))
                    DcDirect(0x7258db3b9e03e625ULL, PB_PIPE_SID, 1, 0);
                Xlog("dev: plant a placed bomb at our feet (pipe bombs in the pack: %lld)", (long long) InvQty(PB_PIPE_SID));
                dev_plant = 1;
            }
        }
    }
#endif
    if (!edge && !dev_plant)
        return;
    static int ncl;
    uint64_t none = 0, aim =
        DcCall(FNV64("player-is-aiming?"), 0, &none), cw =
        CurrentWeapon();
    int why = (!GameFocused()
               && !dev_plant) ? 1 : GetTickCount64() - last_place <
        1000 ? 2 : (g_downed || LocalHp() <= 0.0f || FinisherGuardOn()
                    || (g_execUntil
                        && g_t60 < g_execUntil)) ? 3 : (BaseWeapon(cw) !=
                                                         PB_PIPE_SID
                                                         && !dev_plant) ? 4
        : !InvQty(PB_PIPE_SID) ? 5 : PlayerMenuActive() ==
        1 ? 6 : (!dev_plant && aim != ~0ULL && (aim & 0xff)) ? 7 : 0;
    // Reported: throwing a pipe bomb, then left click to place it, destroys the entity. The engine's throw controller still owns the hand for ~1.2 s after
    // the throw message (the projectile leaves, the next bomb is drawn): a plant then (crouch + plant clip + hand-prop hiding + inventory consume) ran over it and
    // the thrown bomb vanished. A click in that window is refused (denied sound); with no bomb left after the throw it is refused the same way.
    uint64_t thr_ago =
        g_ownThrowMs ? GetTickCount64() - g_ownThrowMs : ~0ULL;
    if (!why
        && (thr_ago < 1200 || (aim_ms && GetTickCount64() - aim_ms < 600)))
        why = 8;                // (aim released < 0.6 s ago = the throw is on its way)
    if (why && (why == 8 || (why == 5 && thr_ago < 3000)) && !dev_plant) {
        UiDeny("");
        Xlog("placed bomb: click refused - %s (own throw %llu ms ago, pipe bombs in the pack %llu)", why == 8 ? "the throw is still in the hand" : "the last bomb was thrown", (unsigned long long) thr_ago, (unsigned long long) InvQty(PB_PIPE_SID));
    }
    if (why && ncl < 12) {
        ncl++;
        static const char *W[] =
            { "", "game not focused", "1 s cooldown",
"downed / dead / finisher", "not holding the pipe bomb", "no pipe bomb left",
"a menu is open", "aiming (normal throw)", "own throw in progress" };
        Xlog("pbomb: click ignored - %s (weapon %llx base %llx qty %llu aim %llx)", W[why], (unsigned long long) cw, (unsigned long long) BaseWeapon(cw), (unsigned long long) InvQty(PB_PIPE_SID), (unsigned long long) aim);
    }
    if (why)
        return;
    float lb2[16];
    const float *pl = PlayerLocator(lb2);
    if (!pl)
        return;
    float qx = pl[4], qy = pl[5], qz = pl[6], qw = pl[7];
    float fx = 2 * (qx * qz + qw * qy), fz =
        1 - 2 * (qx * qx + qy * qy), fl =
        __builtin_sqrtf(fx * fx + fz * fz);
    if (fl < 0.01f)
        return;
    fx /= fl;
    fz /= fl;
    g_pbp.fwd[0] = fx;
    g_pbp.fwd[1] = fz;
    memcpy(g_pbp.feet, pl, 12);
    PbKickSpot();             // the grid around the setting clip's hand spot (PB_AHEAD ahead, PB_RIGHT right)
    int nk = 0;
    for (int k = 0; k < PB_GRID; k++)
        nk += g_pbp.job[k] != 0;
    if (nk < PB_GRID)
        Xlog("pbomb: %d of %d ground rays not kicked (those points count as no floor)", PB_GRID - nk, PB_GRID);
    if (!nk) {
        g_pbp.st = 0;
        Xlog("pbomb: ground rays refused - no spot test");
        return;
    }
}

// marking an enemy bomb: the game's mark button (winmm's latch 0x18013A7D0) with the reticle on a bomb whose line of sight is clear
// highlights it for PB_MARK_S and tells the team (MSG_MINE op 5); winmm's own mark only knows player slots
static struct {
    int st, idx;
    uint64_t job;
    uint64_t frame;
} g_pbm;
// shooting a placed bomb (shooting it as an enemy did not blow it up - pb_shot_scan only runs for shots whose resolve gate
// matches, the shotgun path; rifles / pistols never reached it): every rising edge of player-is-firing? tests the camera ray against every
// placed bomb (the mark's reticle test) and a clear sight ray next frame detonates it - ANY shooter, own and teammate bombs included
static struct {
    int st, idx;
    uint64_t job, frame;
} g_pbf;
static uint64_t g_pbFireMs;   // our last frame with player-is-firing? (a bomb visual the engine destroys right after it = our shot hit it)
static float g_pbFireCp[3], g_pb_fire_cd[3];  // the camera ray of that frame
static float g_pbfLen;         // length of the pending sight ray
static float RayLenTo(int i)
{
    (void) i;
    return g_pbfLen;
}

// (log: "pbomb: shot on bomb 776 (2.0 m) - sight ray out" x3, no detonation; bombs 774 / 775 / 776 "both probes blocked (t 0.34 / 0.30) - no trigger (wall)" for
// minutes while the victim stood 2-3 m from a mine on the Hillside grass at -282 16.1 -416.8; blast "0 of 6 probes reached" with every ray hit 0.0-0.7 m from its
// origin): the mine's rays used world_ray_kick's mask 0x207, which STOPS on tall grass ~0.45 m above the ground (t2r-ext.c world_ray_kick_m: 0x1b07 passes it).
// pb_ray_g = the mine's cover test with 0x1b07. 0x1b07 also stops on characters, so a ray that ends on a body ends min(0.5 m, 40 %) short of it (the old
// 0x207 ray ended inside the body and a character never counted); walls still block.
static uint64_t PbRayG(const float *a, const float *e, uint64_t ign)
{
    float ex = e[0] - a[0], ey = e[1] - a[1], ez = e[2] - a[2], el =
        __builtin_sqrtf(ex * ex + ey * ey + ez * ez);
    float cut =
        el > 0.001f ? (el * 0.4f < 0.5f ? el * 0.4f : 0.5f) / el : 0.0f;
    float b[3] = { e[0] - ex * cut, e[1] - ey * cut, e[2] - ez * cut };
    return WorldRayKickM(a, b, ign, 0x1b07);
}

static void PbFireTick(void)
{
    static int was;
    uint32_t self = *(uint32_t *) T2R_SYM(kMySlot);
    if (g_pbf.st == 1) {
        if (g_frame == g_pbf.frame)
            return;
        g_pbf.st = 0;
        float t = RayT(g_pbf.job);
        int i = g_pbf.idx;
        // (a shot bomb disappeared but stayed armed: the sight ray ends ON the bomb, and the bomb's own collision stopped it
        // short - "blocked", no detonation - while the engine's bullet destroyed the visual. A hit within 0.2 m of the bomb is the bomb itself.)
        float sd = RayLenTo(g_pbf.idx);
        if (!g_pb[i].used || g_pb[i].boomed || g_pb[i].trig_ms
            || (t >= 0.0f && t < 0.97f && (1.0f - t) * sd > 0.2f))
            return;             // 0.2 m: a wall right in front of the bomb still blocks
        PbDetonate(i, (int) self, "our shot");
        if (g_sendTramp) {
            uint8_t m[3];
            m[0] = 4;
            memcpy(m + 1, &g_pb[i].id, 2);
            g_sendTramp(MSG_MINE, m, sizeof m, 0xff);
        }
        return;
    }
    uint64_t none = 0, r = DcCall(FNV64("player-is-firing?"), 0, &none);
    int now = r != ~0ULL && (r & 0xff), edge = now && !was;
    was = now;
    if (now && RcamGet(g_pbFireCp, g_pb_fire_cd))
        g_pbFireMs = GetTickCount64();
    if (!edge || LocalHp() <= 0.0f)
        return;
    float cp[3], cd[3];
    if (!RcamGet(cp, cd))
        return;
    int best = -1;
    float bd = 1e9f;
    for (int i = 0; i < PB_MAX; i++) {
        if (!g_pb[i].used || g_pb[i].boomed || g_pb[i].trig_ms)
            continue;
        float v[3] =
            { g_pb[i].pos[0] - cp[0], g_pb[i].pos[1] + 0.06f - cp[1],
       g_pb[i].pos[2] - cp[2] };
        float along = v[0] * cd[0] + v[1] * cd[1] + v[2] * cd[2];
        if (along < 0.3f || along > 60.0f)
            continue;
        float px = v[0] - cd[0] * along, py = v[1] - cd[1] * along, pz =
            v[2] - cd[2] * along;
        float perp = __builtin_sqrtf(px * px + py * py + pz * pz);
        if (perp > 0.12f + along * 0.012f)
            continue;           // the bomb's size plus a little spread
        if (along < bd) {
            bd = along;
            best = i;
        }
    }
    static int nl;
    if (best < 0) {
        if (nl < 6) {
            nl++;
            Xlog("pbomb: shot - no bomb on the camera ray");
        }
        return;
    }
    float a[3] = { cp[0], cp[1], cp[2] }, b[3] =
        { g_pb[best].pos[0], g_pb[best].pos[1] + 0.12f,
g_pb[best].pos[2] };
    // the sight ray passes grass / foliage (mask 0x1b07, see pb_ray_g); it ends ON the bomb, so no end cut (the 0.2 m rule in the read stays)
    g_pbf.job = WorldRayKickM(a, b, 0, 0x1b07);
    if (!g_pbf.job)
        return;
    g_pbf.st = 1;
    g_pbf.idx = best;
    g_pbf.frame = g_frame;
    g_pbfLen =
        __builtin_sqrtf((b[0] - a[0]) * (b[0] - a[0]) +
                        (b[1] - a[1]) * (b[1] - a[1]) + (b[2] -
                                                         a[2]) * (b[2] -
                                                                  a[2]));
    Xlog("pbomb: shot on bomb %u (%.1f m) - sight ray out", g_pb[best].id,
         bd);
}

// the enemy bomb under the reticle (the mark's aim test, no sight ray): index or -1. Also read by flashlight.c on the press frame - the mark button on a
// markable bomb is a MARK, never a flashlight toggle
static int PbMarkAimed(float *cp, float *cd)
{
    uint32_t self = *(uint32_t *) T2R_SYM(kMySlot);
    if (!RcamGet(cp, cd))
        return -1;
    int best = -1;
    float bd = 1e9f;
    for (int i = 0; i < PB_MAX; i++) {
        if (!g_pb[i].used || g_pb[i].boomed || !PbEnemyOf(i, self))
            continue;
        float v[3] =
            { g_pb[i].pos[0] - cp[0], g_pb[i].pos[1] + 0.1f - cp[1],
       g_pb[i].pos[2] - cp[2] };
        float along = v[0] * cd[0] + v[1] * cd[1] + v[2] * cd[2];
        if (along < 0.5f || along > 40.0f)
            continue;
        float px = v[0] - cd[0] * along, py = v[1] - cd[1] * along, pz =
            v[2] - cd[2] * along;
        float perp = __builtin_sqrtf(px * px + py * py + pz * pz);
        if (perp > 0.15f + along * 0.04f)
            continue;           // ~2.3 degrees of slack plus the bomb's size
        if (along < bd) {
            bd = along;
            best = i;
        }
    }
    return best;
}

static void PbMarkTick(void)
{
    static uint8_t prev;
    uint32_t self = *(uint32_t *) T2R_SYM(kMySlot);
    if (g_pbm.st == 1) {        // the sight ray of last frame
        if (g_frame == g_pbm.frame)
            return;
        g_pbm.st = 0;
        float t = RayT(g_pbm.job);
        int i = g_pbm.idx;
        if (!g_pb[i].used || (t >= 0.0f && t < 0.97f))
            return;
        int again = g_pb[i].mark_until > GetTickCount64();
        g_pb[i].mark_until = ~0ULL;     // shown to the team until the bomb is gone
        Xlog("pbomb: bomb %u of slot %d MARKED by us%s", g_pb[i].id,
             g_pb[i].owner, again ? " (again)" : "");
        // the mark callout (marking gives a voiceline): our own voice, heard like our other callouts (vox_lines.py
        // kind bomb: the WLF / Seraphite warn-an-ally barks - their voices have no trap / mine / bomb line); once per bomb
        if (!again)
            VxEvent(VX_BOMB, "marked an enemy bomb");
        if (g_sendTramp) {
            uint8_t m[3];
            m[0] = 5;
            memcpy(m + 1, &g_pb[i].id, 2);
            g_sendTramp(MSG_MINE, m, sizeof m, 0xff);
        }
        return;
    }
    uint8_t latch = *(volatile uint8_t *) T2R_SYM(kFlashlightLatch);
    int edge = latch && !prev;
    prev = latch;
    if (!edge || LocalHp() <= 0.0f || g_downed)
        return;
    float cp[3], cd[3];
    int best = PbMarkAimed(cp, cd);
    if (best < 0)
        return;
    float a[3] = { cp[0], cp[1], cp[2] }, b[3] =
        { g_pb[best].pos[0], g_pb[best].pos[1] + 0.15f,
g_pb[best].pos[2] };
    g_pbm.job = PbRayG(a, b, 0);      // (grass passes, the ray ends short of the bomb's own collision like the shot test)
    if (!g_pbm.job)
        return;
    g_pbm.st = 1;
    g_pbm.idx = best;
    g_pbm.frame = g_frame;
}

// The engine's explode (called on EVERY client to show the blast) also applies its own blast damage to the characters around it - on a
// client that is not the victim that killed the victim's PUPPET (a puppet dead on one client, while its owner, hit by our ring on its own screen,
// lived 13 hp). Damage must only come from each victim's own client (the explosives2013 ring), so every puppet is invincible from just
// before our explode until the engine's (delayed, ~4 frames) damage has passed; enemy puppets are made vulnerable again after that
// (teammates' stay invincible: feature_friendly_fire).
static void SetInvincible(uint64_t who, int on);
static uint64_t g_pb_inv_h[T2R_MAX_PEERS], g_pb_inv_until;
static void PbPuppetsGuard(int on)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    for (uint32_t sl = 0; sl < T2R_MAX_PEERS; sl++) {
        if (on) {
            uint8_t *peer = ((fn_peer) T2rPeer) (sl);
            uint64_t h = peer ? *(uint64_t *) (peer + 0x60) : 0;
            g_pb_inv_h[sl] = h && peer[4] ? h : 0;
            if (g_pb_inv_h[sl])
                SetInvincible(g_pb_inv_h[sl], 1);
        } else if (g_pb_inv_h[sl]) {
            if (!IsTeammate(sl))
                SetInvincible(g_pb_inv_h[sl], 0);
            g_pb_inv_h[sl] = 0;
        }
    }
}

static uint64_t g_pbInvRing[8];
static unsigned g_pbInvN;
static uint64_t g_pbTrapRing[8];
// Only the planter picks his mine up (2013: ProcessMine's interact is the owner's). SP's trap mine is a native interactable that
// anyone can take - for a bomb that is not ours (enemy, teammate) the engine's interact is off every frame while we stand within its prompt reach.
static void PbPickupGuard(const float *pl, uint32_t self)
{
    if (!pl || g_pbOff)
        return;
    for (int i = 0; i < PB_MAX; i++) {
        // (shot bombs could be picked back up as placed mines that fill the inventory and have no use): a shot / triggered bomb is
        // armed to go off (trig_ms) and then lies hidden for 6 s while its sfx rings out (boomed, fx 1) - but its engine object is still SP's interactable,
        // and this guard skipped both states (and the owner), so anyone could take it: the trap-bomb item went into the pack, the boomed branch below
        // `continue`d before any pickup handling. A bomb that is going off / has gone off is not takeable by anybody.
        int dying = g_pb[i].boomed || g_pb[i].trig_ms;
        if (!g_pb[i].used || (g_pb[i].owner == (int) self && !dying))
            continue;
        float dx = g_pb[i].pos[0] - pl[0], dy =
            g_pb[i].pos[1] - pl[1], dz = g_pb[i].pos[2] - pl[2];
        // (a teammate's / enemy's placed mine still showed the pickup prompt and pressing it played the pickup, the mine stayed, nothing in the
        // pack; log: "pbomb: pickup of bomb 775 of slot 3 by a non-owner (leak)" at 97601 - the pickup read below fires inside 3.5 m, this guard only reached
        // 3.0 m (4.5 m dying), so a body 3.0-3.5 m away, or a prompt that reaches past 3 m along the camera, was never covered; the throttled "pickup blocked"
        // log had also used up its 6 lines on bombs 768 / 771, so the leak's frames left no trace). Reach is now 6 m for every foreign bomb (the call is
        // one flag per frame, it costs nothing), and the guard runs the WHOLE list: a nearer bomb no longer hides a second one (the old `return` after the
        // first hit - several mines of slot 3 lay within 3 m of each other, ids 768-775).
        if (dx * dx + dy * dy + dz * dz > 6.0f * 6.0f)
            continue;
        uint64_t none = 0;
        DcCall(FNV64("disable-interact-one-frame"), 0, &none);
        // diagnostic only (nothing is disabled by handle: a mine's def id is not known yet and an unrelated interactable must stay usable): the def id of the
        // closest enabled interactable within 3.5 m of a foreign bomb is logged once per distinct id, so the next log names the mine's prompt for a
        // handle-level disable (interactable-disable, as feature_chain_prompts) if the wider one-frame guard still leaks.
        if (dx * dx + dy * dy + dz * dz < 3.5f * 3.5f) {
            uint64_t q[3];
            float r8 = 8.0f;
            for (int k = 0; k < 3; k++) {
                q[k] = 0;
                memcpy(&q[k], &r8, 4);
            }
            uint64_t ih =
                DcCall(FNV64("get-closest-enabled-interactable"), 3, q);
            if (ih && ih != ~0ULL) {
                uint64_t def =
                    DcCall(FNV64("interactable-get-def-id"), 1, &ih);
                static uint64_t seen[8];
                static int ns;
                int known = 0;
                for (int k = 0; k < ns; k++)
                    if (seen[k] == def)
                        known = 1;
                if (!known && ns < 8) {
                    seen[ns++] = def;
                    Xlog("pbomb: closest interactable def %llx within %.1f m of foreign bomb %u (identifies the mine's prompt)", (unsigned long long) def, __builtin_sqrtf(dx * dx + dy * dy + dz * dz), g_pb[i].id);
                }
            }
        }
        static uint64_t at[PB_MAX];
        static int nl[PB_MAX];
        if (nl[i] < 3 && GetTickCount64() - at[i] > 3000) {
            nl[i]++;
            at[i] = GetTickCount64();
            Xlog("pbomb: pickup blocked - bomb %u belongs to slot %d, interact off while within reach (%.1f m)%s", g_pb[i].id, g_pb[i].owner, __builtin_sqrtf(dx * dx + dy * dy + dz * dz), dying ? " (it is going off / has gone off)" : "");
        }
        continue;
    }
}

static void PbTombTick(const float *pl)
{
    static uint64_t cf;
    uint64_t now = GetTickCount64();
    if (!cf || now - cf > 2000) {
        FILE *f = ExeSideOpen("t2r-no-bomb-tomb.txt", "r");
        g_pbTombN = f ? -1000000 : (g_pbTombN < 0 ? 0 : g_pbTombN);
        if (f)
            fclose(f);
        cf = now | 1;
    }
    if (g_pbTombN < 0)
        return;
    typedef void *(*fn_proc)(uint64_t);
    int blocked = 0;
    for (int k = 0; k < 8; k++) {
        if (!g_pb_tomb[k].h)
            continue;
        if (now > g_pb_tomb[k].until
            || !((fn_proc) T2R_SYM(kFnEntityFromHandle)) (g_pb_tomb[k].
                                                          h)) {
            g_pb_tomb[k].h = 0;
            continue;
        }
        static int nl;
        if (LOGCAP(nl, 8))
            Xlog("pbomb: removed bomb object %llx still resolves %llu ms after its kill - interact off near it, kill repeated", (unsigned long long) g_pb_tomb[k].h, (unsigned long long) (now - g_pb_tomb[k].kill_ms));
        if (now - g_pb_tomb[k].kill_ms > 500) {
            uint64_t h = g_pb_tomb[k].h;
            DcCall(FNV64("kill-entity"), 1, &h);
            g_pb_tomb[k].kill_ms = now;
        }
        if (pl && !blocked) {
            float dx = g_pb_tomb[k].pos[0] - pl[0], dy =
                g_pb_tomb[k].pos[1] - pl[1], dz =
                g_pb_tomb[k].pos[2] - pl[2];
            if (dx * dx + dy * dy + dz * dz < 4.5f * 4.5f) {
                uint64_t none = 0;
                DcCall(FNV64("disable-interact-one-frame"), 0, &none);
                blocked = 1;
            }
        }
    }
}

static void PbTick(void)
{
    if (g_pb_inv_until && GetTickCount64() > g_pb_inv_until) {
        g_pb_inv_until = 0;
        PbPuppetsGuard(0);
        Xlog("pbomb: puppets vulnerable again after the blast");
    }
    if (!g_pbOffAt || GetTickCount64() - g_pbOffAt > 2000) {
        FILE *f = ExeSideOpen("t2r-no-placed-bomb.txt", "r");
        g_pbOff = f != NULL;
        if (f)
            fclose(f);
        g_pbOffAt = GetTickCount64() | 1;
    }
    typedef void *(*fn_proc)(uint64_t);
    typedef void (*fn_explode)(void *, void *, void *, void *);
    typedef uint8_t *(*fn_me)(void);
    uint8_t *exe = (uint8_t *) GetModuleHandleW(NULL);
    if (g_pbOff || MatchSubstate() != 6) {
        for (int i = 0; i < PB_MAX; i++)
            if (g_pb[i].used)
                PbRemove(i, g_pbOff ? "feature off" : "match over");
        if (g_pbTombN >= 0) {
            const float *pl0 = NULL;
            float lb0[16];
            if (MatchSubstate() == 6)
                pl0 = PlayerLocator(lb0);
            PbTombTick(pl0);
        }
        memset(g_pb_gone, 0, sizeof g_pb_gone);
        g_pbGoneN = 0;        // (ids restart with the next match)
        g_pbp.st = 0;
        g_pbPend.on = 0;       // (a plant pending at the match end never lands in the next one)
        memset(g_pb_pup, 0, sizeof g_pb_pup);
        g_pbCrouch.until = 0;
        g_pbCrouch.stand_after = 0;
        return;
    }
    uint32_t self = *(uint32_t *) T2R_SYM(kMySlot);
    PbPlaceTry();
    PbMarkTick();
    PbFireTick();
    uint64_t now = GetTickCount64();
    float lb[16];
    const float *pl = PlayerLocator(lb);
    int listen_on = -1;
    g_pbInvN++;
    g_pbInvRing[g_pbInvN & 7] = InvQty(PB_PIPE_SID);       // pack count of the last 8 frames: the baseline of a pickup
    g_pbTrapRing[g_pbInvN & 7] = InvQty(PB_TRAP_SID);
    PbPickupGuard(pl, self);
    PbTombTick(pl);
    for (int i = 0; i < PB_MAX; i++) {
        if (!g_pb[i].used)
            continue;
        int ownerme = g_pb[i].owner == (int) self;
        uint8_t *p =
            g_pb[i].h ? ((fn_proc) T2R_SYM(kFnEntityFromHandle)) (g_pb[i].
                                                                  h) :
            NULL;
        if (g_pb[i].boomed) {   // trap-mine blast: the bomb is gone at once (hidden), its sfx-trap-bomb rings out on it
            if (g_pb[i].fx == 1)
                PbHideObj(g_pb[i].h);
            if (now - g_pb[i].boom_ms > (g_pb[i].fx == 1 ? 6000u : 2500u))
                PbRemove(i, "blast done");
            continue;
        }
        if (!p && g_pb[i].h && pl) {    // the engine object is gone: someone PICKED IT UP (SP's mine is a native interactable)
            float dx = g_pb[i].pos[0] - pl[0], dy =
                g_pb[i].pos[1] - pl[1], dz = g_pb[i].pos[2] - pl[2];
            if (dx * dx + dy * dy + dz * dz < 3.5f * 3.5f
                && LocalHp() > 0.0f && !g_downed) {
                // (dupe bug: a mine set against a wall, the visual freed 1 frame after the spawn, read as
                // "taken back by its owner" -> "pipe bombs 4 -> 3, gave +2" = one free bomb every plant): a pickup is only a pickup when the engine's
                // interact really put its trap-bomb item in the pack since the plant. Else the
                // visual was freed: the re-spawn below puts it back, the bomb stays armed.
                int gained = (int) InvQty(PB_TRAP_SID) > g_pb[i].q_trap;       // the engine interact gives the trap-bomb item (a crafted pipe bomb must not count)
                if (ownerme && !gained && !g_pb[i].gone_ms)
                    g_pb[i].gone_ms = now;
                if (ownerme && !gained && now - g_pb[i].gone_ms < 250)
                    continue;   // the engine's pickup may put the item in a frame later
                if (ownerme && !gained) {
                    static int ng;
                    if (LOGCAP(ng, 8))
                        Xlog("pbomb: bomb %u visual gone without a pickup (no item gained since the plant) - re-spawned, no item given", g_pb[i].id);
                } else if (ownerme) {   // once only - the tracker, the listen stand-in and the others' copies go with it, no substitute is respawned, the pack gets exactly +1
                    int base = g_pb[i].q_pipe, now_q =
                        (int) InvQty(PB_PIPE_SID), d = now_q - base;
                    int tq = (int) InvQty(PB_TRAP_SID), tb =
                        g_pb[i].q_trap;
                    Xlog("pbomb: pickup - bomb %u taken back by its owner (engine interact), pipe bombs %d -> %d, trap-bomb items %d", g_pb[i].id, base, now_q, tq);
                    if (tq > tb) {
                        uint64_t r =
                            DcDirect(FNV64
                                      ("player-inventory-consume-item"),
                                      PB_TRAP_SID, (uint64_t) (tq - tb),
                                      0);
                        Xlog("pbomb: pickup - %d trap-bomb item(s) removed again -> %llx", tq - tb, (unsigned long long) r);
                    }
                    if (d < 1) {
                        DcDirect(0x7258db3b9e03e625ULL, PB_PIPE_SID,
                                  (uint64_t) (1 - d), 0);
                        Xlog("pbomb: pickup - gave the pipe bomb (+%d)",
                             1 - d);
                    } else if (d > 1) {
                        DcDirect(FNV64("player-inventory-consume-item"),
                                  PB_PIPE_SID, (uint64_t) (d - 1), 0);
                        Xlog("pbomb: pickup - removed %d extra pipe bomb(s)", d - 1);
                    }
                    if (g_sendTramp) {
                        uint8_t m[3];
                        m[0] = 3;
                        memcpy(m + 1, &g_pb[i].id, 2);
                        g_sendTramp(MSG_MINE, m, sizeof m, 0xff);
                    }
                    PbRemove(i, "picked up by its owner");
                    continue;
                }
                // an enemy / teammate got it through the prompt (blocked below, so only a leak): the mine stays (re-spawned) and the item it gave is taken back
                if (!ownerme) {
                    int base =
                        (int) g_pbInvRing[(g_pbInvN + 1) & 7], now_q =
                        (int) InvQty(PB_PIPE_SID), d = now_q - base;
                    int tq = (int) InvQty(PB_TRAP_SID), tb =
                        (int) g_pbTrapRing[(g_pbInvN + 1) & 7];
                    Xlog("pbomb: pickup of bomb %u of slot %d by a non-owner (leak) - item taken back (pipe %d -> %d, trap-bomb %d)", g_pb[i].id, g_pb[i].owner, base, now_q, tq);
                    if (d > 0)
                        DcDirect(FNV64("player-inventory-consume-item"),
                                  PB_PIPE_SID, (uint64_t) d, 0);
                    if (tq > tb)
                        DcDirect(FNV64("player-inventory-consume-item"),
                                  PB_TRAP_SID, (uint64_t) (tq - tb), 0);
                }
            }
        }
        if (p)
            g_pb[i].gone_ms = 0;
        if (!p && g_pb[i].h && !g_pb[i].trig_ms && g_pbFireMs
            && now - g_pbFireMs < 600 && now - g_pb[i].spawn_ms > 500) {
            // the engine's bullet destroyed the visual (SP's mine is shootable) within 0.6 s of our shot, on a bomb in front of us within 60 m:
            // that was our shot - it goes off for everyone now (it stayed armed and invisible before)
            // only a bomb on our aim line (a visual freed for any other reason - wall, lifetime - while we shoot elsewhere must not go off)
            const float *cp = g_pbFireCp, *cd = g_pb_fire_cd;
            float v[3] =
                { g_pb[i].pos[0] - cp[0], g_pb[i].pos[1] + 0.06f - cp[1],
           g_pb[i].pos[2] - cp[2] };
            float along = v[0] * cd[0] + v[1] * cd[1] + v[2] * cd[2];
            float px = v[0] - cd[0] * along, py =
                v[1] - cd[1] * along, pz = v[2] - cd[2] * along;
            if (along > 0.3f && along < 60.0f
                && __builtin_sqrtf(px * px + py * py + pz * pz) <
                0.3f + along * 0.02f) {
                PbDetonate(i, (int) self,
                            "our shot destroyed its visual");
                if (g_sendTramp) {
                    uint8_t m[3];
                    m[0] = 4;
                    memcpy(m + 1, &g_pb[i].id, 2);
                    g_sendTramp(MSG_MINE, m, sizeof m, 0xff);
                }
            }
        }
        if (!p && now - g_pb[i].respawn_ms > 1000) {    // the engine freed the visual (lifetime?): put a new one there
            g_pb[i].respawn_ms = now;
            if (!g_pb[i].pipe && g_pb[i].nresp >= 1 && !g_pbSpBad) {
                g_pbPipeOnce = 1;
                Xlog("pbomb: bomb %u: SP's trap mine was freed again after a re-spawn - pipe bomb look for THIS bomb only (was: the rest of the session, R-PNZS)", g_pb[i].id);
            }
            g_pb[i].nresp++;
            g_pb[i].h = PbSpawn(g_pb[i].pos);
            g_pb[i].pipe = g_pbSpawnPipe;
            g_pb[i].held = 0;
            g_pb[i].still_n = 0;
            g_pb[i].contact_ms = 0;
            g_pb[i].spawn_ms = now;
            g_pb[i].hl = 0;
            memcpy(g_pb[i].last, g_pb[i].pos, 12);
            g_pb[i].last[1] += PB_REST_H;
            Xlog("pbomb: bomb %u visual was gone - respawned %llx",
                 g_pb[i].id, (unsigned long long) g_pb[i].h);
            p = g_pb[i].
                h ? ((fn_proc) T2R_SYM(kFnEntityFromHandle)) (g_pb[i].
                                                              h) : NULL;
        }
        if (p && !g_pb[i].trig_ms) {    // shots / blasts / bumps push the resting projectile: hold it on its spot
            float cp[3];
            if (ProcPos(p, cp)) {
                float dx = cp[0] - g_pb[i].pos[0], dz =
                    cp[2] - g_pb[i].pos[2], d2 = dx * dx + dz * dz;
                if (!g_pb[i].pipe && cp[1] < g_pb[i].pos[1] - 0.25f && !g_pbSpBad) {  // SP's mine sank (the earlier failure): one fresh pipe bomb there, no loop
                    g_pbSpBad = 1;
                    Xlog("pbomb: bomb %u: SP's trap mine is %.2f m under its spot (%.2f %.2f %.2f) - pipe bomb look from now on", g_pb[i].id, g_pb[i].pos[1] - cp[1], cp[0], cp[1], cp[2]);
                    uint64_t old = g_pb[i].h;
                    if ((g_pb[i].hl & 2) && !g_pb[i].lm)
                        PbListen(old, 0);
                    DcCall(FNV64("kill-entity"), 1, &old);
                    g_pb[i].hl = 0;
                    g_pb[i].h = PbSpawn(g_pb[i].pos);
                    g_pb[i].pipe = g_pbSpawnPipe;
                    g_pb[i].held = 0;
                    g_pb[i].still_n = 0;
                    g_pb[i].contact_ms = 0;
                    g_pb[i].spawn_ms = now;
                    memcpy(g_pb[i].last, g_pb[i].pos, 12);
                    g_pb[i].last[1] += PB_REST_H;
                    continue;
                }
                if (!g_pb[i].held) {
                    // landing (SP: 0.15 s from the hand to rest): free until it lies still (3 frames under 1.5 mm), or 0.35 s after it reached
                    // the floor (a roll on a slope), or 0.9 s after the spawn. Holding it from the spawn on (the old code) zeroed the drop every
                    // frame: it sank ~3 mm a frame and hovered for ~0.8 s; held at first contact it can stay on one edge.
                    float mx = cp[0] - g_pb[i].last[0], my =
                        cp[1] - g_pb[i].last[1], mz =
                        cp[2] - g_pb[i].last[2];
                    float mv =
                        __builtin_sqrtf(mx * mx + my * my + mz * mz);
                    memcpy(g_pb[i].last, cp, 12);
                    g_pb[i].still_n =
                        mv < 0.0015f ? g_pb[i].still_n + 1 : 0;
                    if (!g_pb[i].contact_ms
                        && cp[1] <= g_pb[i].pos[1] + 0.10f)
                        g_pb[i].contact_ms = now;
                    int st = g_pb[i].still_n >= 3
                        && now - g_pb[i].spawn_ms >=
                        80 ? 1 : g_pb[i].contact_ms
                        && now - g_pb[i].contact_ms >=
                        350 ? 2 : now - g_pb[i].spawn_ms > 900 ? 3 : 0;
                    if (st) {
                        g_pb[i].held = 1;
                        static const char *W[4] =
                            { "", "lies still", "0.35 s on the floor",
          "0.9 s timeout" };
                        Xlog("pbomb: bomb %u landed (%s) %.2f s after the drop at (%.2f %.2f %.2f), %.3f m over the spot's ground, %.2f m off the spot", g_pb[i].id, W[st], (now - g_pb[i].spawn_ms) / 1000.0, cp[0], cp[1], cp[2], cp[1] - g_pb[i].pos[1], __builtin_sqrtf(d2));
                    }
                }
                if (g_pb[i].held) {     // it kept rolling, then the re-place teleported it (collision sound): hold it still EVERY frame from landing on
                    typedef void *(*fn_cast)(void *);
                    typedef void (*fn_vel)(void *, float *);
                    float zero[4] = { 0, 0, 0, 0 };
                    void *q = ((fn_cast) (exe + 0x11c7070)) (p);
                    if (q)
                        ((fn_vel) (exe + 0x11c8b00)) (q, zero); // stop its slide
                }
                if (d2 > 1.0f && now - g_pb[i].pin_ms > 300) {  // still knocked far away (a blast): a fresh visual on the spot
                    g_pb[i].pin_ms = now;
                    uint64_t old = g_pb[i].h;
                    if ((g_pb[i].hl & 2) && !g_pb[i].lm)
                        PbListen(old, 0);
                    DcCall(FNV64("kill-entity"), 1, &old);
                    if (g_pb[i].lm) {
                        uint64_t o = g_pb[i].lm;
                        PbListen(o, 0);
                        DcCall(FNV64("kill-entity"), 1, &o);
                        g_pb[i].lm = 0;
                    }
                    g_pb[i].h = PbSpawn(g_pb[i].pos);
                    g_pb[i].pipe = g_pbSpawnPipe;
                    g_pb[i].hl = 0;
                    g_pb[i].held = 0;
                    g_pb[i].still_n = 0;
                    g_pb[i].contact_ms = 0;
                    g_pb[i].spawn_ms = now;
                    memcpy(g_pb[i].last, g_pb[i].pos, 12);
                    g_pb[i].last[1] += PB_REST_H;
                    Xlog("pbomb: bomb %u visual knocked %.2f m away - re-placed %llx", g_pb[i].id, __builtin_sqrtf(d2), (unsigned long long) g_pb[i].h);
                    p = g_pb[i].
                        h ? ((fn_proc)
                             T2R_SYM(kFnEntityFromHandle)) (g_pb[i].
                                                            h) : NULL;
                }
            }
        }
        if (!g_pb[i].armed
            && now - g_pb[i].placed_ms >= (uint64_t) (PB_ARM_S * 1000)) {
            g_pb[i].armed = 1;
            int idle = g_pb[i].pipe || PbIdle2013();  // SP's mine is silent once set (only sfx-trap-bomb-deploy, arm-short / -end on the hand)
            if (idle)
                PbSndNear(TRAP_IDLE_CLICK_OFF, TRAP_IDLE_CLICK_N, PB_CLICK_GAIN, g_pb[i].pos, PB_NEAR_FAR);   // the arming clicks, ONCE (first second of bomb-timed-idle-mp)
            g_pb[i].wick_ms = idle ? now + TRAP_IDLE_CLICK_N * 1000ull / TRAP_RATE : ~0ULL;     // then only the sizzle loops
            Xlog("pbomb: bomb %u armed", g_pb[i].id);
        }
        if (g_pb[i].armed && !g_pb[i].trig_ms && now >= g_pb[i].wick_ms) {
            PbSndNear(TRAP_IDLE_LOOP_OFF, TRAP_IDLE_LOOP_N, PB_IDLE_GAIN,
                        g_pb[i].pos, PB_NEAR_FAR);
            g_pb[i].wick_ms =
                now + TRAP_IDLE_LOOP_N * 1000ull / TRAP_RATE - 8;
        }                       // the sizzle loop (seamless back to back)
        // listen mode (SP's trap-mine behaviour, not the old white outline): ENEMY bombs within PB_LISTEN_R are
        // in the engine's listen-mode list through SP's trap-mine object as a stand-in on their spot (SP's call, registered outside listen as SP's
        // mine at deploy; own / teammates' bombs never); while listen mode is on the stand-in shows and the pipe bomb is hidden, otherwise the
        // other way round (see pb_lm_spawn). The object highlight is only the team's RED mark of an enemy bomb.
        if (g_pb[i].h) {
            int marked = g_pb[i].mark_until > now;
            if (listen_on < 0) {
                uint64_t none = 0, r =
                    DcCall(FNV64("player-listen-mode-active?"), 0, &none);
                listen_on = r != ~0ULL && (r & 0xff);
            }
            float d = 1e9f;
            if (pl) {
                float dx = g_pb[i].pos[0] - pl[0], dy =
                    g_pb[i].pos[1] - pl[1], dz = g_pb[i].pos[2] - pl[2];
                d = sqrtf(dx * dx + dy * dy + dz * dz);
            }
            int want = PbEnemyOf(i, self) && (d <= PB_LISTEN_R
                                                || ((g_pb[i].hl & 2)
                                                    && d <=
                                                    PB_LISTEN_R + 1.0f));
            if (g_pb[i].pipe && !PbNoStandin()) {
                // SP's trap-mine stand-in (it has the listen-mode material, the pipe bomb has none), now ONLY while listen mode is on: spawned
                // at the listen start, listed + given SP's reveal source (pb_lm_reveal: without a source an entry stays at alpha 0), killed at
                // the release. The pipe bomb is hidden while it exists. The stand-in itself is never hidden: an earlier dump read its entry's
                // mesh position as (0 0 0) after it had been hidden every frame since its spawn (hide-object-for-one-frame), which would
                // also put it outside its own source's radius.
                int need = want && listen_on;
                if (need && !g_pb[i].lm) {
                    g_pb[i].lm = PbLmSpawn(i);
                    if (g_pb[i].lm) {
                        PbListen(g_pb[i].lm, 1);
                        PbLmReveal(g_pb[i].lm, g_pb[i].pos, "stand-in");
                        g_pb[i].hl |= 2;
                        g_pb[i].outline_ms = now;
                        Xlog("pbomb: bomb %u of slot %d: listen on (%.1f m) - SP's trap-mine stand-in listed + revealed, pipe bomb hidden", g_pb[i].id, g_pb[i].owner, d);
                        PbLmDiag(g_pb[i].lm, "added");
                    }
                } else if (!need && g_pb[i].lm) {
                    uint64_t o = g_pb[i].lm;
                    PbListen(o, 0);
                    DcCall(FNV64("kill-entity"), 1, &o);
                    g_pb[i].lm = 0;
                    g_pb[i].hl &= ~2;
                    Xlog("pbomb: bomb %u: stand-in gone (%s, %.1f m)",
                         g_pb[i].id,
                         listen_on ? "out of reach" : "listen off", d);
                }
                if (g_pb[i].lm) {
                    uint64_t hide = g_pb[i].h;
                    DcCall(FNV64("hide-object-for-one-frame"), 1, &hide);
                    static int ns;
                    if (ns < 40 && now >= g_pb[i].outline_ms + 150) {
                        ns++;
                        g_pb[i].outline_ms = now + 850;
                        PbLmState(g_pb[i].lm, "stand-in");
                    }           // 0.15 s in, then every 1 s
                }
            } else {            // the bomb itself in the list (the trap-mine look, or t2r-pb-no-listen-standin.txt)
                if (want && !(g_pb[i].hl & 2)) {
                    PbListen(g_pb[i].h, 1);
                    PbLmReveal(g_pb[i].h, g_pb[i].pos, "bomb");
                    g_pb[i].hl |= 2;
                    g_pb[i].outline_ms = now;
                    Xlog("pbomb: bomb %u of slot %d in the listen-mode list (%.1f m) as itself + SP's reveal source", g_pb[i].id, g_pb[i].owner, d);
                    PbLmDiag(g_pb[i].h, "added");
                } else if (!want && (g_pb[i].hl & 2)) {
                    PbListen(g_pb[i].h, 0);
                    g_pb[i].hl &= ~2;
                    Xlog("pbomb: bomb %u out of the listen-mode list (%.1f m)", g_pb[i].id, d);
                }
                if ((g_pb[i].hl & 2) && listen_on) {
                    static int ns2;
                    if (ns2 < 40 && now >= g_pb[i].outline_ms + 1000) {
                        ns2++;
                        g_pb[i].outline_ms = now;
                        PbLmState(g_pb[i].h, "bomb");
                    }
                }
            }
            if (marked) {
                PbHighlight(g_pb[i].h, 1.0f, 0.1f, 0.1f, 1.0f);
                if (g_pb[i].lm)
                    PbHighlight(g_pb[i].lm, 1.0f, 0.1f, 0.1f, 1.0f);
                if (!(g_pb[i].hl & 1))
                    Xlog("pbomb: enemy bomb %u highlighted (marked by the team, red)", g_pb[i].id);
                g_pb[i].hl |= 1;
            } else if (g_pb[i].hl & 1) {
                PbHighlight(g_pb[i].h, 1.0f, 1.0f, 1.0f, 0.0f);
                if (g_pb[i].lm)
                    PbHighlight(g_pb[i].lm, 1.0f, 1.0f, 1.0f, 0.0f);
                g_pb[i].hl &= ~1;
            }
        }
        // trigger: we only judge ourselves (an enemy of the owner, alive, standing inside the radius)
        if (g_pb[i].armed && !g_pb[i].trig_ms && pl && !ownerme
            && LocalHp() > 0.0f && !g_downed
            && !(g_pb[i].owner < T2R_MAX_PEERS
                 && SameTeam((uint32_t) g_pb[i].owner, self))) {
            // 2013 (PS4 mine.md): 3-D sphere of 3.0 m x the owner's Explosion Expert scale around the mine to the victim's body, AND a clear
            // probe (static world, mask 0x207) from the mine + 0.4 m to the upper body OR the body position; walls block, a hidden-feet /
            // visible-chest victim triggers. The probes are async (kick one frame, read the next).
            float rad = g_pb[i].rad > 0.0f ? g_pb[i].rad : PB_RADIUS;
            float dx = g_pb[i].pos[0] - pl[0], dy =
                g_pb[i].pos[1] - (pl[1] + 1.0f), dz =
                g_pb[i].pos[2] - pl[2];
            // (a player died from a trap mine one level above): the probe to the upper body ran past the upper floor's edge; a mine only
            // reacts to a body on its own floor (mine at most PB_FLOOR_BELOW under / PB_FLOOR_ABOVE over the feet)
            float fdy = g_pb[i].pos[1] - pl[1];
            if (dx * dx + dy * dy + dz * dz <= rad * rad
                && fdy > -PB_FLOOR_BELOW && fdy < PB_FLOOR_ABOVE) {
                if ((g_pb[i].pj[0] || g_pb[i].pj[1])
                    && g_frame > g_pb[i].pj_frame) {
                    float t1 = RayT(g_pb[i].pj[0]), t2 =
                        RayT(g_pb[i].pj[1]);
                    g_pb[i].pj[0] = g_pb[i].pj[1] = 0;
                    int b1 = t1 >= 0.0f && t1 < 0.95f, b2 = t2 >= 0.0f
                        && t2 < 0.95f;
                    if (!(b1 && b2)) {
                        Xlog("pbomb: bomb %u: victim inside the %.1f m sphere (%.2f m), probe upper body t %.2f / body t %.2f - clear, TRIGGERED", g_pb[i].id, rad, __builtin_sqrtf(dx * dx + dy * dy + dz * dz), t1, t2);
                        PbTrigger(i, (int) self);
                        if (g_sendTramp) {
                            uint8_t m[3];
                            m[0] = 2;
                            memcpy(m + 1, &g_pb[i].id, 2);
                            g_sendTramp(MSG_MINE, m, sizeof m, 0xff);
                        }
                    } else {
                        static uint64_t bl;
                        if (now - bl > 2000) {
                            bl = now;
                            Xlog("pbomb: bomb %u: victim inside the sphere but both probes blocked (t %.2f / %.2f) - no trigger (wall)", g_pb[i].id, t1, t2);
                        }
                    }
                }
                if (!g_pb[i].trig_ms && !g_pb[i].pj[0] && !g_pb[i].pj[1]) {
                    // (A/B throw test: a mine 2 m from a standing target in the open never triggered - "both probes blocked (t 0.02 / 0.03)":
                    // the rays started inside the mine's own projectile collision. The rays now ignore the bomb object and start 0.3 m along
                    // their way from mine + 0.4 m.)
                    float up[3] = { pl[0], pl[1] + 1.4f, pl[2] }, bd[3] =
                        { pl[0], pl[1] + 1.0f, pl[2] };
                    float a0[3] =
                        { g_pb[i].pos[0], g_pb[i].pos[1] + 0.4f,
                  g_pb[i].pos[2] }, a1[3], a2[3];
                    for (int k = 0; k < 2; k++) {
                        const float *e = k ? bd : up;
                        float *o = k ? a2 : a1;
                        float ex = e[0] - a0[0], ey = e[1] - a0[1], ez =
                            e[2] - a0[2], el =
                            __builtin_sqrtf(ex * ex + ey * ey + ez * ez);
                        float sk = el > 0.9f ? 0.3f / el : 0.0f;
                        o[0] = a0[0] + ex * sk;
                        o[1] = a0[1] + ey * sk;
                        o[2] = a0[2] + ez * sk;
                    }
                    g_pb[i].pj[0] = PbRayG(a1, up, g_pb[i].h);
                    g_pb[i].pj[1] = PbRayG(a2, bd, g_pb[i].h);        // grass no longer blocks (pb_ray_g) g_pb[i].pj_frame = g_frame;
                }
            } else
                g_pb[i].pj[0] = g_pb[i].pj[1] = 0;
        }
        // (a player ran past a trap without triggering it - the trigger was judged ONLY on the victim's own client; a mine the victim's client
        // never got / got elsewhere never fires. The OWNER now also checks the enemy puppets it sees against its own armed mines - same
        // sphere, same wall probe from the mine to the puppet's chest - and triggers + tells everyone (first trigger wins, as before).)
        if (g_pb[i].armed && !g_pb[i].trig_ms && ownerme) {
            typedef uint8_t *(*fn_peer_o)(uint32_t);
            float rad = g_pb[i].rad > 0.0f ? g_pb[i].rad : PB_RADIUS;
            if (g_pb[i].oj && g_frame > g_pb[i].oj_frame) {
                float tt = RayT(g_pb[i].oj);
                g_pb[i].oj = 0;
                if (!(tt >= 0.0f && tt < 0.95f)) {
                    Xlog("pbomb: bomb %u: enemy slot %d inside the %.1f m sphere on the OWNER's screen, probe clear - TRIGGERED (owner side)", g_pb[i].id, g_pb[i].oj_slot, rad);
                    PbTrigger(i, g_pb[i].oj_slot);
                    if (g_sendTramp) {
                        uint8_t m[3];
                        m[0] = 2;
                        memcpy(m + 1, &g_pb[i].id, 2);
                        g_sendTramp(MSG_MINE, m, sizeof m, 0xff);
                    }
                }
            }
            if (!g_pb[i].trig_ms && !g_pb[i].oj) {
                for (uint32_t s = 0; s < T2R_MAX_PEERS; s++) {
                    if (s == self || SameTeam(s, self) || g_peerDowned[s]
                        || g_puppetDeadHere[s])
                        continue;
                    uint8_t *pe = ((fn_peer_o) T2rPeer) (s);
                    uint64_t h = pe && pe[4]
                        && !pe[5] ? *(uint64_t *) (pe + 0x60) : 0;
                    float lb2[16];
                    const float *q = h ? PuppetLocator(h, lb2) : NULL;
                    if (!q || !__builtin_isfinite(q[0]))
                        continue;
                    float dx = g_pb[i].pos[0] - q[0], dy =
                        g_pb[i].pos[1] - (q[1] + 1.0f), dz =
                        g_pb[i].pos[2] - q[2];
                    if (dx * dx + dy * dy + dz * dz > rad * rad
                        || g_pb[i].pos[1] - q[1] <= -PB_FLOOR_BELOW
                        || g_pb[i].pos[1] - q[1] >= PB_FLOOR_ABOVE)
                        continue;       // own floor only
                    float a0[3] =
                        { g_pb[i].pos[0], g_pb[i].pos[1] + 0.4f,
                  g_pb[i].pos[2] }, e[3] = { q[0], q[1] + 1.2f, q[2] };
                    float ex = e[0] - a0[0], ey = e[1] - a0[1], ez =
                        e[2] - a0[2], el =
                        __builtin_sqrtf(ex * ex + ey * ey + ez * ez);
                    float sk = el > 0.9f ? 0.3f / el : 0.0f, a1[3] =
                        { a0[0] + ex * sk, a0[1] + ey * sk,
a0[2] + ez * sk };
                    g_pb[i].oj = PbRayG(a1, e, g_pb[i].h);    // grass no longer blocks (pb_ray_g) g_pb[i].oj_slot = (int)s; g_pb[i].oj_frame = g_frame;
                    break;
                }
            }
        }
        if (g_pb[i].trig_ms
            && now - g_pb[i].trig_ms >=
            (uint64_t) (PbFuseS(g_pb[i].owner) * 1000)) {
            g_pb[i].boomed = 1;
            g_pb[i].boom_ms = now;
            Xlog("pbomb: bomb %u of slot %d goes off", g_pb[i].id,
                 g_pb[i].owner);
            g_pb[i].fx = !PbPipeBlast() && PbTrapBlast(i, p) ? 1 : 0;       // SP's trap-mine particles + sfx, else the pipe bomb's
            if (!g_pb[i].fx && p) {
                PbPuppetsGuard(1);
                g_pb_inv_until = now + 2500;
                Xlog("pbomb: engine pipe-bomb explode - puppets invincible for its blast (damage only from each victim's own client)");
                g_pbAllow = 1;
                ((fn_explode) (exe + 0x11c6420)) (p, NULL, NULL, NULL);
                g_pbAllow = 0;
                g_pb[i].fx = 2;
            }
            // A bomb set off by a TEAMMATE of its owner (the 2013 rule
            // allows the hit) hurts no teammate - ex13 spares the owner's teammates on their own clients, but the OWNER's own client judges his blast with
            // slot == self (full damage, log: bomb 1027 of slot 4 shot by teammate slot 3 -> "award: slot 4 down by slot 4"). The blast is
            // visual/sound only on the owner's client then (enemies' clients still judge it); an enemy's shot, or the owner's own, damages as before.
            if (g_pb[i].owner == (int) *(uint32_t *) T2R_SYM(kMySlot)
                && g_pb[i].by1 > 0 && g_pb[i].by1 - 1 != g_pb[i].owner
                && PbFriendlyTo(i, (uint32_t) (g_pb[i].by1 - 1)))
                Xlog("pbomb: bomb %u set off by teammate slot %d of its owner (slot %d) - no blast damage for the owner", g_pb[i].id, g_pb[i].by1 - 1, g_pb[i].owner);
            else
                Ex13Pend(EX13_PBOMB, g_pb[i].owner, g_pb[i].pos);
            if (g_pb[i].lm) {
                uint64_t o = g_pb[i].lm;
                PbListen(o, 0);
                DcCall(FNV64("kill-entity"), 1, &o);
                g_pb[i].lm = 0;
                g_pb[i].hl &= ~2;
            } else if (g_pb[i].h && (g_pb[i].hl & 2)) {
                PbListen(g_pb[i].h, 0);
                g_pb[i].hl &= ~2;
            }
        }
    }
}
