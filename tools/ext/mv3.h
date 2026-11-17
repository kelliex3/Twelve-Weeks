// mv3.h - melee v3 contract (spec re/melee-v3.md). Shared by mv3_sim.c (pure, no engine calls), mv3_net.c, mv3_anim.c, mv3_fx.c, mv3_glue.c and
// tools/mv3/simtest.c. Parts: mv3_world / mv3_fighter / mv3_ev = sim; mv3_move = data.
//
// Space conventions (every mv3 module; the engine's own, no conversion anywhere):
//   - px / pz = engine x / z (ground plane), py = engine y (up), millimetres. yaw = uc4_yaw x 100 = atan2(forward x, forward z) in 1/100 deg:
//     0 faces +z, +9000 faces +x. Quaternion back: (0, sin y/2, 0, cos y/2) (mv3_anim.c mv3a_yaw_q).
//   - handedness: +x is the LEFT of a body facing +z, so yaw + 90 points to the body's LEFT and yaw - 90 to its RIGHT.
//   - dir (dodges, reactions, misses): 0 front 1 left 2 right 3 back, in the body's own frame (reactions: where the attacker stands).
//   - root_fwd / root_side and the RM curves: + = forward / the character's RIGHT (the clips' align channel, sign-flipped by gen_moves.py).
//   - mv3_in.sx / sy: the stick in the body frame like a pad, sy+ = forward, sx+ = RIGHT (mv3_glue.c mv3g_stick_body rotates the camera-relative stick).
//   So a stick-left dodge plays a dir-1 (left) clip and the sim's root motion carries the body toward yaw + 90 (its left); a blow from the victim's
//   right (attacker toward yaw - 90) plays a dir-2 reaction whose root_side < 0 carries it to its left, away from the blow.
#pragma once
#include <stdint.h>

#define MV3_HZ          60      // the sim step (frames per second, fixed)
#define MV3_MAX_F       8       // fighters in one bubble
#define MV3_HIST        96      // rollback history (frames kept) - 1.6 s
#define MV3_DELAY_MIN   2       // input delay, frames (raised with ping by mv3_net.c, MV3_DELAY_MAX cap)
#define MV3_DELAY_MAX   6
#define MV3_EV_MAX      64      // events per step

// ---- weapon sets (SP's melee sets; the dodge-counter rows of ctxdown.c use the same split)
enum { MV3_WS_FIST, MV3_WS_1HBLUNT, MV3_WS_1HBLADE, MV3_WS_2H,
        MV3_WS_KNIFE, MV3_WS_N };

// ---- input: one per fighter per frame, sent by its owner. The owner's own pose rides along (locomotion stays the engine's: the sim takes the owner's
// position / yaw as truth while the fighter is in MV3_IDLE, and moves it by root motion itself in every other state)
enum { MV3_B_ATK = 1, MV3_B_DODGE = 2, MV3_B_HEAVY =
        4 /* reserved */ , MV3_B_BLOCK = 8 /* reserved: parry */  };
typedef struct {
    uint8_t btn;                // MV3_B_* held this frame (edges are derived by the sim from the previous frame's input)
    int8_t sx, sy;              // left stick, -127..127, in the body frame: sy+ forward, sx+ right (the glue rotates the camera-relative stick)
    uint8_t wset;               // MV3_WS_* of the weapon in hand
    int32_t px, pz, py;         // owner position, millimetres
    int16_t yaw;                // owner facing, 1/100 degree (-18000..18000)
    uint16_t flags;             // bit0 can-melee (not downed / not in a paired move / not reloading etc., from the engine), bit1 sprinting
} Mv3In;

// ---- fighter + world (all plain data: memcpy = snapshot)
enum { MV3_S_IDLE, MV3_S_SWING, MV3_S_DODGE, MV3_S_REACT, MV3_S_COUNTER,
        MV3_S_COUNTERED, MV3_S_MISS, MV3_S_OUT };
typedef struct {
    uint8_t slot;               // network slot (T2R slot), 0xff = empty
    uint8_t team;
    uint8_t state;              // MV3_S_*
    uint8_t wset;
    int16_t move;               // index into MV3_MOVES, -1 none
    uint16_t mf;                // frame inside the current move
    int32_t px, pz, py;         // millimetres
    int16_t yaw;                // 1/100 degree
    uint16_t iframe_end;        // world frame (low 16 bits) the i-frames end, 0 none
    uint8_t chain;              // combo step 0..2
    uint8_t buf;                // buffered press (MV3_B_ATK / MV3_B_DODGE), consumed at the first legal frame
    uint8_t buf_age;            // frames since the buffered press (dropped after 18 = 300 ms)
    uint8_t target;             // fighter index of the current swing's target, 0xff none
    uint8_t hit_done;           // this swing already resolved its contact
    uint8_t dodged_by;          // fighter index whose swing we dodged (counter window), 0xff none
    uint16_t counter_end;       // world frame (low 16) the counter window closes
    int16_t react;              // reaction move index while MV3_S_REACT
    int16_t hp;                 // hit points x10, mirrored from the engine at bubble start / resync (damage itself is applied by the victim's engine)
    uint8_t prev_btn;           // last frame's buttons (edge detection)
    uint8_t pad[3];
} Mv3Fighter;
typedef struct {
    uint32_t frame;             // shared world frame
    uint32_t rng;               // deterministic rng (xorshift32), advanced only by the sim
    uint8_t n;                  // fighters in use
    uint8_t pad[3];
    Mv3Fighter f[MV3_MAX_F];
} Mv3World;

// ---- events (presentation + damage). Key for rollback dedupe = (frame, type, a, move).
enum {
    MV3_E_SWING,                // a = attacker, move = swing move           (swish + effort grunt)
    MV3_E_HIT,                  // a = attacker, b = victim, move = swing    (impact sfx, blood, pain grunt, damage x = dmg)
    MV3_E_WHIFF,                // a = attacker, move                        (whoosh)
    MV3_E_DODGE,                // a = dodger, move = dodge move             (dodge vox / foley)
    MV3_E_DODGED,               // a = dodger, b = attacker                  (attacker miss anim begins)
    MV3_E_REACT,                // a = victim, move = reaction move
    MV3_E_COUNTER,              // a = counterer, b = victim, move = counter pair row
    MV3_E_DOWN                  // a = victim (its hp reached the down threshold in the sim: the glue hands it to the engine's downed flow)
};
typedef struct {
    uint32_t frame;
    uint8_t type, a, b, pad;
    int16_t move;
    int16_t x;
} Mv3Ev;
typedef struct {
    int n;
    Mv3Ev e[MV3_EV_MAX];
} Mv3Evbuf;

// ---- move data (generated: tools/mv3/gen_moves.py -> mv3_moves.h)
enum { MV3_K_SWING1, MV3_K_SWING2, MV3_K_SWING3, MV3_K_DODGE, MV3_K_MISS,
        MV3_K_REACT, MV3_K_COUNTER, MV3_K_COUNTERED };
typedef struct {
    const char *name;           // clip name (SP)
    uint64_t sid;               // fnv64 of the name (the engine's clip id)
    const char *pair;           // the partner's clip for paired moves (counter: victim half), NULL otherwise
    uint64_t pair_sid;
    uint8_t kind;               // MV3_K_*
    uint8_t wset;               // MV3_WS_*
    uint8_t dir;                // dodge / reaction direction: 0 front 1 left 2 right 3 back; swings: 0
    uint8_t strength;           // reactions: 0 light 1 medium 2 heavy; swings: damage class
    uint16_t dur_f;             // clip length in sim frames (at rate 1)
    uint16_t hit0_f, hit1_f;    // contact window (swings / counters), frames from the start
    uint16_t chain0_f, chain1_f;        // a press in [chain0, chain1) chains the next combo step
    uint16_t cancel_f;          // from here the move may be cancelled into a dodge / new swing / locomotion
    uint16_t iframe0_f, iframe1_f;      // dodges: invulnerable frames
    float reach;                // metres: max attacker-victim distance at contact
    float arc_deg;              // half-angle in front of the attacker the target must be in
    float root_fwd, root_side;  // root motion over the whole clip, metres (attacker frame)
    int16_t dmg;                // damage x10 at contact (the engine's melee table, melee_dmg_now)
    uint8_t sfx_n;              // sound events (ctx_sfx_events.h style), see mv3_moves.h
    uint8_t pad;
} Mv3Move;

// ---- sim API (mv3_sim.c, pure)
void Mv3SimInit(Mv3World * w, uint32_t seed);
int Mv3SimAdd(Mv3World * w, uint8_t slot, uint8_t team, const Mv3In * in);  // returns the fighter index
void Mv3SimRemove(Mv3World * w, int idx);
void Mv3SimStep(Mv3World * w, const Mv3In in[MV3_MAX_F], Mv3Evbuf * ev);   // one frame: w->frame++
uint32_t Mv3SimHash(const Mv3World * w);
// ---- move table (mv3_moves.h)
extern const Mv3Move MV3_MOVES[];
extern const int MV3_MOVES_N;
