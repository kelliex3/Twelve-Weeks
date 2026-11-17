// paired.h - "psync": paired moves (executions, finishers, tackles, revive hand-outs) played in sync on every
// client (implementation paired.c, included near the end of t2r-ext.c; design and RE in re/paired-sync.md).
//
// The model is TLOU2's own MP melee sync (NetEventStartMelee, re/uc4net/NOTES.txt): the initiator picks ONE
// apReference (a world BoundFrame) and a start time and broadcasts them; every client then plays BOTH halves
// locally around that apReference - its own body's half on itself, the other half(s) on the puppet(s) - with the
// receiver's start phase advanced by the one-way latency. While a puppet plays a half locally, the three winmm
// channels that normally drive it from the network are suspended for it (net locator + pose writer, the
// AnimCmdList replay, the root-transform write). Only the owner of a body decides what happens to it (the victim's
// client kills the victim; nobody else applies damage or death from the move).
#pragma once

// ---- BoundFrame (exe layout, 0x50 bytes; ctor 0x14133d780, validity check 0x14133e2c0, world getter 0x14133db30)
// The animate_ argument (animate-arg ap-ref) (key 10), set-object-locator and every DC "locator" value are
// BoundFrames, NOT 8-float Locators: animate_'s parser (0x14187e250) rejects anything whose first two dwords are not
// 0x7fcccccc ("(animate-arg ap-ref) -- invalid bound-frame!") and plays the clip with NO apReference.
typedef struct __attribute__((aligned(16))) {
    uint32_t magic[2];          // 0x7fcccccc, 0x7fcccccc
    uint64_t binding;           // u32 index + u32 generation into the binding table (0 = unbound, world space)
    float parent[8];            // parent locator (pos xyzw, quat xyzw): identity when unbound
    float local[8];             // local locator: with no binding = the world locator
} PsyncBf;

_Static_assert(sizeof(PsyncBf) == 0x50, "BoundFrame is 0x50 bytes");
// ap7 = pos xyz + quat xyzw
static inline void PsyncBfMake(PsyncBf *bf, const float *ap7)
{
    bf->magic[0] = bf->magic[1] = 0x7fccccccu;
    bf->binding = 0;
    for (int i = 0; i < 8; i++)
        bf->parent[i] = 0.0f;
    bf->parent[7] = 1.0f;
    bf->local[0] = ap7[0];
    bf->local[1] = ap7[1];
    bf->local[2] = ap7[2];
    bf->local[3] = 1.0f;
    bf->local[4] = ap7[3];
    bf->local[5] = ap7[4];
    bf->local[6] = ap7[5];
    bf->local[7] = ap7[6];
}

// from an 8-float locator (pos xyzw, quat xyzw) as the ext reads them (0x14133db30)
static inline void PsyncBfFromLoc(PsyncBf *bf, const float *loc8)
{
    float a[7] =
        { loc8[0], loc8[1], loc8[2], loc8[4], loc8[5], loc8[6], loc8[7] };
    PsyncBfMake(bf, a);
}

// ---- wire (peer message MSG_PSYNC, ours; winmm's own types are below 0x20)
#define MSG_PSYNC 0x4B
enum { PS_OP_START = 1, PS_OP_ABORT = 2, PS_OP_END = 3, PS_OP_PING =
        4, PS_OP_PONG = 5, PS_OP_ACK = 6 };
enum { PS_GUN_EXEC = 1, PS_MELEE_EXEC = 2, PS_FINISHER = 3, PS_TACKLE = 4, PS_REVIVE = 5, PS_CTXDOWN = 6 };     // PS_CTXDOWN: contextual melee down (ctxdown.c), non-lethal; kill_s = the hand-off move time
#define PS_F_LETHAL 0x01        // the victim's client kills the victim at kill_s (owner-authorised death)
#define PS_F_ANCHOR_VIC 0x02    // the ap was solved on the victim's body (it stays, the attacker comes to it)
typedef struct __attribute__((packed)) {
    uint8_t op, ver, kind, flags;
    uint8_t att, vic, pair, resend;     // attacker / victim slot, gun pair index (0xff none, 0xfe melee), copy number
    uint32_t uid;               // attacker slot << 24 | counter
    uint64_t att_clip, vic_clip;        // clip sids (fnv64 of the clip names), att = the initiator's half
    float ap[7];                // the apReference: world pos xyz + quat xyzw (BoundFrame, unbound)
    float kill_s;               // move time of the owner's kill (lethal) / of the effect; < 0 none
    float dur_s;                // move length (the longer half)
    uint32_t sent_ms;           // the initiator's move time when this copy was sent (resends > 0)
    uint64_t weapon, move;      // the attacker's held weapon / the melee move id (finisher table)
} PsyncWire;

_Static_assert(sizeof(PsyncWire) == 84, "psync_wire layout");

// ---- API (paired.c)
static int PsyncStart(int kind, uint32_t vic_slot, const char *att_clip, const char *vic_clip, const float *ap7, float kill_s, int flags, int pair, uint64_t weapon, uint64_t move);   // initiator: 1 = started + sent
static int PsyncReceiveLegacy(uint32_t att_slot, int kind, const char *att_clip, const char *vic_clip, const float *ap7, float kill_s, int pair, uint64_t weapon, uint64_t move);     // old MSG_GUN_EXEC start
static void PsyncAbort(const char *why, int notify);   // stop the running move (both halves), release the puppets
static int PsyncActive(void);
static int PsyncRole(void);    // 0 none, 1 attacker (we started the move), 2 receiver
static int PsyncAttackedSlot(void);
static int PsyncExecHoldSlot(void);  // attacker side: the victim slot while EITHER half of the move plays (the held lethal hp), else -1
static void PsyncReleaseVictimPup(const char *why);  // attacker side: stop placing the victim puppet on its half (the death takes it)               // attacker side: the victim slot of the move that plays, else -1
static void DownedDeathNoanim(const char *why);       // this death came from the downed state: no TLOU2 death anim (paired.c vdh_start)
static int PsyncSkipFrames(void);     // receiver: the start phase advance, in 60 Hz frames
static int PsyncBusySlot(uint32_t slot);      // that slot's puppet plays a half locally
static void PsyncKeepVictim(const char *why); // the victim died at the kill frame: its half is kept playing to its end
static int PsyncVicKillPassed(uint32_t slot);        // that slot is the victim of the running lethal move and its kill time has passed
static int PsyncGateSlot(int32_t slot);       // winmm's pose / locator gate for the slot: 1 = suspended
static uint64_t PsyncReplayBypass(uint64_t ac, uint64_t a2, uint64_t a3, int *done);  // winmm replay hook
static uint64_t PsyncRootBypass(uint64_t proc, uint64_t a2, uint64_t a3, uint64_t a4, int *done);     // root hook
static int PsyncRecv(uint32_t from, const uint8_t * buf, uint64_t len);        // MSG_PSYNC
static void PsyncTick(void);
static int PsyncLocatorSkip(uint64_t h);      // winmm's net locator call (0x180035ddf) for that puppet: skip
static int PsyncLocatorHandback(uint64_t h, void *loc);       // a puppet just released: 1 = psync set the net locator blended from the clip's end
static int PsyncSolveAp(uint64_t ent, uint64_t sid, const float *anchor, float *ap7); // ap so that half starts ON anchor
