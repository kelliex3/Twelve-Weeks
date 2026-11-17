// ============================================================================
//  anim-internal.hpp -- INTERNAL to src/net/anim. Never include from outside.
//
//  The helpers/constants the anim-*.cpp files share, plus extern declarations
//  for the statics that cross files (one definition each, in the most-owning
//  .cpp). The threading contract is documented at the definitions it governs
//  (seqlock publish order at the rx banks, anim-thread rules at the hooks and
//  snapshot nodes).
// ============================================================================
#pragma once

#include "net-common.hpp"
#include "net-protocol.hpp"    // kMaxNetPlayers / kMaxAnimCmdDwords
#include <atomic>              // seqlock generations + the deferred-clear flag

static bool SafeRW(uintptr_t a, void* buf, size_t n, bool write) {
    __try { if (write) memcpy((void*)a, buf, n); else memcpy(buf, (const void*)a, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static inline bool San(uintptr_t p) { return p >= 0x10000 && p < 0x7FFFFFFFFFFFull; }
template<class T> static T Rd(uintptr_t a) { T v{}; if (San(a)) SafeRW(a, &v, sizeof(T), false); return v; }
template<class T> static void Wr(uintptr_t a, T v) { if (San(a)) SafeRW(a, &v, sizeof(T), true); }

enum : uintptr_t {
    GO_ANIMCONTROL = 0x1B8, AC_NUMLAYERS = 0x20, AC_LAYERARR = 0x28,
    LE_STRIDE = 0x20, LE_NAME = 0x00, LE_PTR = 0x08, LE_VALID = 0x14,
    AL_TYPE = 0x08,                              // AnimLayer.type: 1=Simple 2=State
    ASL_INSTTABLE = 0x638, ASL_NUMINST = 0x640,
    ASI_ANIMSID = 0x108, ASI_RATE = 0x124, ASI_PREVPHASE = 0x19C, ASI_PHASE = 0x1A0,
};

// ============================================================================
//  ENGINE ENTRY POINTS -- the functions the anim engine hooks or calls, as
//  RVAs off g_base. A puppet must share the player skeleton (identical joint
//  indices): clips and feather masks are both resolved against that rig.
//  Aim cannot ride in an AnimCmdList, so it is a separate post-anim pass.
// ============================================================================
static const uintptr_t RVA_POSTANIM = 0x1CEB380;   // PostAnimBlending_Async body (receiver aim)
static const uintptr_t RVA_CREATEANIMCMDS = 0x14427A0;   // AnimControl::CreateAnimCmds (sub_1414427A0)
static const uintptr_t RVA_LOOKUPANIM = 0x167E1F0;   // AnimTable::LookupAnim   (sub_14167E1F0)
static const uintptr_t RVA_FEATHER = 0x14F1110;   // FeatherBlendTable::CreateChannelFactorsForAnimCmd (sub_1414F1110)
static const uintptr_t OFF_FEATHERTABLE = 0x84F8520;   // &g_featherBlendTable (sub_1414F1110 'this' arg)
// Feather-blend identity translation. d10 of a 0x09 command is a FixedSizeHeap SLOT
// INDEX handed out by LoginFeatherBlend in login order -- a per-process handle, as
// non-portable as a raw pointer. Shipping it made the receiver index its OWN heap and
// silently pick a DIFFERENT feather blend that happened to be valid for the same rig,
// producing a correct-looking mask over the WRONG joints. We translate index<->SID.
static const uintptr_t RVA_FB_GETID = 0x14F1B40;   // GetFeatherBlendId(tbl,&outSid,idx)  index -> SID
static const uintptr_t RVA_FB_LOGIN = 0x14F1FC0;   // LoginFeatherBlend(tbl,sid,fgAnimData) -> idx (idempotent)
static const uintptr_t RVA_FB_GETENTRY = 0x14F1AC0;   // GetEntry(tbl,idx) -> Entry* (+0 u32 skelId, +8 u64 sid)
static const uintptr_t RVA_LOOKUP_EXACT = 0x1351A30;   // LookupAnimExact(out24, u32 skelId, u64 nameId)
enum : uintptr_t { FBE_SKELID = 0x00, FBE_SID = 0x08 };  // FeatherBlendTable::Entry fields
enum : uintptr_t { SKEL_SKELID = 0x34 };                 // ArtItemSkeleton + 52 = SkeletonId
// The ACTUAL gate the engine applies to every clip (ProcessAnimSegment op 0x07,
// 0x14169B1B7) is a HIERARCHY-id compare, not a skeleton-id compare:
//     anim->clipData(+0x38)->animHierarchyId(+4)  ==  skel->AnimHierarchy(+0x50)->id(+4)
// It is baked into the ANIM, so the verdict is per-clip-asset, not per-actor -- which is
// exactly why some clips play perfectly and whole states vanish.
enum : uintptr_t { SKEL_HIERARCHY = 0x50, HIER_ID = 0x04 };
static inline uint32_t SkelHierId(uintptr_t skel) {
    if (!San(skel)) return 0;
    uintptr_t h = Rd<uintptr_t>(skel + SKEL_HIERARCHY);
    return San(h) ? Rd<uint32_t>(h + HIER_ID) : 0;
}
static const uint32_t  FB_INVALID = 0xFFFFFFFFu;

// Confirmed AnimStateInstance offsets.
enum : uintptr_t {
    ASI_CLIP = 0x108,   // u64 resolved clip SID
    ASI_CURSOR = 0x128,   // f32 rendered phase (0..1)
    ASI_GATE = 0x1B4,   // u8  freeze gate (clear bits 0-3,7 => & 0x70)
    ASI_PHASEOVR = 0x1E4,   // f32 phase override (engine blanks to -1 each frame)
    ASI_SHAREDTI = 0x1E8,   // i32 shared-time index; must be < 0
};

// ============================================================================
//  AnimCmdList command word = (sizeInDwords << 16) | opcode.
//
//  THE LIST IS A POSE-SLOT DATAFLOW GRAPH, NOT A COMMAND STREAM. Verified against
//  the interpreter (ProcessAnimSegment sub_14169A9D0, loop head 0x14169AF30):
//    * The header (0x03) pre-allocates a buffer for EVERY slot 0..count-1
//      (sub_142188A50) but only zeroes the float channel -- the 48-byte joint
//      transforms are stale bump-allocator memory.
//    * Blend's "empty" short-circuit (0x14169B80E) only fires when BOTH sources
//      have all-zero masks. One-empty/one-full runs the REAL blend over that
//      uninitialised memory and then UNIONs the masks (0x142189400), laundering
//      garbage into "valid". The terminator publishes slot 0 unconditionally.
//    * Slot indices are never bounds-checked: slot >= count reads the pointer
//      array past the initialised region and writes through a stale pointer.
//  => DELETING A PRODUCER WHILE KEEPING ITS CONSUMER IS MEMORY CORRUPTION.
//     "Drop the command and carry on" crashes the moment someone moves:
//     locomotion is what adds the second instance, the feather blends and the
//     IK plugins that would be dropped.
//
//  THE RULE (never partial):
//    every command is either carried verbatim, or replaced IN PLACE by one that
//    writes the SAME destination slot. Nothing is ever deleted, no slot is ever
//    renumbered, the header count is copied verbatim. If any command can be
//    neither translated nor substituted, the WHOLE frame is skipped and the
//    puppet keeps replaying the last good list.
//
//  0x0D (empty/bind pose, 2 dwords) is the universal in-place substitute: it is a
//  REAL producer that writes an all-ones mask + the skeleton's default pose, so it
//  is always finite. The engine itself uses it for a missing clip (0x14166B49E).
// ============================================================================
static inline uint32_t CmdOp(uint32_t d0) { return d0 & 0xFFFF; }
static inline uint32_t CmdSize(uint32_t d0) { return d0 >> 16; }   // dwords
static inline uint32_t MakeCmd(uint32_t op, uint32_t sizeDwords) { return (sizeDwords << 16) | op; }

enum : uint32_t {
    OP_HEADER = 0x03,  // {op, poseSlotCount}                       verbatim, exactly one, first
    OP_TERM = 0x04,  // {op, flags}                               verbatim, exactly one, last
    OP_CLIP = 0x07,  // {op,_,animLo,animHi,slot,phase}           TRANSLATE ptr d2:d3
    OP_BLEND = 0x08,  // {op,srcA,srcB,dst,_,weight}               verbatim (pointer-free)
    OP_FEATHER = 0x09,  // {op,_,slot,slot,mode,w,cntLo,cntHi,facLo,facHi,jointSet,_}  TRANSLATE d6:d7 + d8:d9
    OP_FLIP = 0x0A,  // 2 dwords, pointer-free
    OP_MIRROR = 0x0B,  // 2 dwords, pointer-free
    OP_JOINTPOSE = 0x0C,  // 10 dwords, THREE raw joint-pose ptrs   -> substitute bind-pose clip at d1
    OP_EMPTY = 0x0D,  // {op, slot}  real producer, always finite   verbatim / universal substitute
    OP_0F = 0x0F,  // pointer-free
    OP_PLUGIN = 0x19,  // VARIABLE size, custom plugin (IK etc), SID at d2:d3. leg-fix-ik and
    // ankle-blend-ik are 10 dwords; weapon-ik-feather is 16. Size ALWAYS
    // comes from CmdSize(d0) -- never assume 10 (see anim-tx.cpp).
    OP_RETARGET = 0x1A,  // 20 dwords, ArtItemSkeleton* at d10:d11  -> skip frame
    OP_MARK_LYR = 0x1B,  // 4  dwords, metadata only  -> STRIP (never execute)
    OP_MARK_SUB = 0x1C,  // 2  dwords, metadata only  -> STRIP: EXECUTING IT IS assert+int3
    OP_MARK_ST = 0x1D,  // 6  dwords, metadata only  -> STRIP
    OP_20 = 0x20,  // 10 dwords, raw joint-pose ptrs -> substitute 0x0D at d1
    OP_21 = 0x21,  // 4  dwords, raw snapshot-node ptr -> substitute 0x0D at d1
    OP_22 = 0x22,  // 4  dwords, ditto
};

// Fallback clip used to rebuild 0x0C the way ND's own codec does. NOTE the value is
// `tense-idle`, NOT bindpose (real bindpose = 0x8E80731039E98CC3) -- and that is CORRECT:
// ND picks tense-idle on the isPlayer branch and bindpose only for NPCs, and our capture
// subject is always the local player. The value is WIRE-LOCKED; do not "fix" it to the
// bindpose SID. ND's own 0x0C encoder path is dead -- it omits a bit their own decoder
// reads, a 1-bit desync -- so we rebuild rather than mirror it.
static const uint64_t SID_FALLBACK_CLIP = 0x9900A350C28C2705ull;   // == tense-idle
// joint-spring-node plugin: d6:d7 is a raw AnimSnapshotNode*, and we cannot tell
// which slot it writes from the command alone -> the one plugin we must skip on.
static const uint64_t SID_JOINTSPRING = 0xF2C21B6CF6F13D1Eull;

// ---- anim-plugin (opcode 0x19) SIDs, read out of IDA's own symbolised immediates ------------
// Command layout is fixed: d0={op,size}, d1 NEVER WRITTEN by any emitter (stale stack -- the
// executor ignores it), d2:d3 = plugin SID, d4/d5 always 0, payload starts at d6 (the pointer
// handed to each plugin handler is &d6, not &d0).
static const uint64_t SID_LEGFIXIK = 0xAE11F3FB9EB0B934ull;  // foot planting.  d6/d7 = pose-slot
// indices; d8:d9 = OPTIONAL raw
// float* (nullable, null = engine
// default weight).
static const uint64_t SID_ANKLEBLENDIK = 0x1AF9425621463F95ull;  // d6/d7/d9 = slots, d8 = float
// alpha. No pointers at all.
static const uint64_t SID_SUBSYSIKNODE = 0xFC73468A1FEA1582ull;  // NEVER SHIP: d8:d9 is a pointer
// walked two more derefs to a
// FUNCTION POINTER that is CALLED,
// and it is 16 dwords, not 10.
// weapon-ik-feather: the two-handed weapon GRIP -- both arms solved onto the weapon's
// l_/r_hand_prop_attachment joints. 16 dwords, NOT 10.
//
// The old note here ("never emitted by the player -- the grip is a post-anim action")
// was WRONG, and it is why puppets held their weapons with unposed arms. Both routes
// exist and they are different code:
//
//   PLAYER  WeaponIKFeather::Apply (armik.cpp:0x1A9, RVA 0x1622850) is called straight
//           out of sub_140D81FB0 and emits NO anim command -- which is exactly why the
//           captured player list never contains this opcode, and why there is nothing
//           here for the wire to carry.
//   NPC     AnimStateLayer::CreateAnimCmds -> the +0x688 callback (sub_1420D6EA0) emits
//           this command; the executor is AnimSnapshotNodeWeaponIkFeather::AnimPluginCallback
//           (anim-node-weapon-ik-feather.cpp:0xE0, RVA 0x16850C0).
//
// Our puppets own that +0x688 emitter (NavCharacter::Init installs it) but never reach
// it: hk_CreateAnimCmds returns the replayed list and skips the original layer walk, so
// the callback never runs and emits nothing. So the RECEIVER synthesises it itself -- see
// EmitWeaponIkFeather below. Nothing about this crosses the wire except one weight byte.
static const uint64_t SID_WEAPONIKFTHR = 0x0BD2E9D87D2E194Aull;

// ND themselves ship leg-fix-ik over the wire and hard-assert on every OTHER plugin SID, on both
// the send (FillAnimCmdBitStream 0x1409E1FB0) and receive (NetSnapshot::Preparse 0x1409C76A0)
// side. We mirror that whitelist exactly. ankle-blend-ik was verified pointer-free and safe to
// carry verbatim, but ND deliberately chose not to ship it, so it stays off until asked for.
static const bool kCarryAnkleBlendIk = false;

// ---- weapon-ik-feather synthesis (RECEIVER, anim thread) -------------------------------
// Writes ND's own 16-dword weapon-ik-feather command into `out`, targeting pose slot
// `slot`, blended in at `weight`. Returns dwords written (kWeaponIkDwords), or 0 if it
// declined (weight <= 0). Never partially writes.
//
// The payload is byte-identical to what sub_141686120 produces for sub_1420D6EA0's
// parameter block, verified against both the emitter and the executor's actual reads:
//
//   d0  {op=0x19, size=16}
//   d1  ND leaves this as stale stack; the executor never reads it. We zero it.
//   d2:d3  SID_WEAPONIKFTHR
//   d4,d5  0        (sub_141686120 stores a zeroed double over both)
//   d6  pose-slot index. The executor does *(ctx8+8)[stride*d6] to pick the pose buffer
//       it reads AND writes back, so it must name the slot holding the finished pose.
//       ND's layer path passes 1 because its own layer output is slot 1; OUR lists
//       publish slot 0 (see the terminator note in ReplayOntoNpc), so we pass 0 -- which
//       is additionally the one index that is in-bounds whatever the stride byte is.
//   d7  mode selector, 0. Executor feeds it to sub_141685C50 to look up a blend factor.
//   d8  0.5f  -- present in ND's block, never read by the executor. Carried for fidelity.
//   d9  per-chain solve weight, 1.0f (solve the arms fully; d10 does the blending).
//   d10 MASTER weight -> sub_1414F8450 at 0x1416859E7. This is the replicated fade.
//   d11 4      d12 18      d13 0.9f       (solver iteration / limit constants)
//   d14 1  -> selects the two-chain sub_14161EA50 path (l_/r_ shoulder-elbow-wrist onto
//            the prop attachments). 0 would take the sub_141623710 path instead.
//   d15 ND leaves this as stale stack; never read. We zero it.
//
// We emit the command rather than CALLING sub_141686120 on purpose: the engine function
// writes through an AnimCmdList* and bumps its cursor, while ReplayOntoNpc builds into a
// scratch array and commits only if the WHOLE list translates. Emitting keeps that
// all-or-nothing property and couples us to the payload layout only.
enum : int { kWeaponIkDwords = 16 };
int EmitWeaponIkFeather(uint32_t* out, uint32_t slot, float weight);

// Receive-side clamp. The engine computes the required context size and DISCARDS
// the result (0x14169B03E), then writes into a fixed 4096-byte block: a count of
// ~85+ overflows it, and >=256 truncates a byte counter. A bad count is therefore
// a remote memory-corruption primitive, not merely a bad pose. 64 is comfortably
// under the cliff while leaving room for real gameplay lists.
static const uint32_t kMaxPoseSlots = 64;

enum : int { kMaxTrackedAnims = 32 };
struct TrackedAnim { uint64_t sid; uintptr_t anim; float phase; };

// Dictionary: 10-bit index. 0x3FF is reserved to mean "literal, do not index",
// so a full table degrades to 64-bit ids rather than colliding. (File scope so
// AnimReplay can size its rx dictionary; the codec uses them unqualified from
// inside namespace animcodec.)
enum : uint32_t { DICT_CAP = 1023, DICT_LITERAL = 1023, DICT_REPEAT = 8 };

// ---------------------------------------------------------------------------
//  Sample ring metadata (the receive-side time buffer that makes interpolation
//  possible). Three deep: two to bracket the playback clock + one to absorb a
//  late/reordered packet. METADATA ONLY -- the interpolator blends the two
//  snapshot STORES, so only the newest sample's sendMs and the count are read.
// ---------------------------------------------------------------------------
enum : int { kSampleRing = 3 };
struct AnimSample {
    uint32_t sendMs;    // author's capture time - the ONLY ordering key
    long     len;       // dwords; 0 = empty
};

// ===========================================================================
//  AnimReplay -- ALL per-author-slot receive/replay state, ONE struct.
//  static-storage array s_replay[kMaxNetPlayers] in anim-rx.cpp: addresses are
//  process-stable for the life of the process, which is what preserves every
//  anim-thread lifetime guarantee (never-freed snapshot nodes, drive-pointer
//  ordering). Indexed by the ORIGINAL author's net slot.
//
//  -------------------------- PER-FIELD WRITER TABLE ------------------------
//  Category [G] game-thread-sync -- written synchronously on the game thread;
//               the anim thread only reads.
//    npcAC, npcObj      drive pointers, published as a pair in
//                       NetApplyRemoteAnim. npcAC is the ONE replay match key:
//                       hk_CreateAnimCmds and hk_PostAnim both compare against
//                       it. Detach()'s zeroing MUST be a synchronous
//                       game-thread write strictly BEFORE the puppet kill.
//    rxCmd, rxLenB,     double-buffered received lists. Publish order is
//    rxBank, rxGen      LOAD-BEARING: bank body -> bank len -> bank flip ->
//                       gen bump (entries before counts).
//    ringMeta, ringNewest, ringCount   sample-ring metadata.
//    rxStampMs          freshness stamp of the last ACCEPTED publish.
//    rxSpine, rxSpineValid, spineGen   aim-spine channel; spineGen is a
//                       seqlock generation (writer = game thread, reader =
//                       the post-anim job).
//    rxKey, rxHave, rxEpoch, rxEpochSet   per-sender rx clip dictionary --
//                       game thread ONLY (decode runs there; anim never reads).
//    lastAnimSeq, haveAnimSeq   rx anim sequence gate -- game thread ONLY
//                       (packet pump). Reset tier: peer drop / full teardown,
//                       NEVER Detach.
//    snapNode           pose-snapshot node, game-thread allocator publish;
//                       allocated once, NEVER freed or cleared (teardown-race
//                       immunity).
//  Category [A] anim-thread -- owned by the async anim job; the game thread
//               must not write these outside the pendingClear protocol.
//    rxScratch          reader-private untorn copy actually replayed.
//    rxReleased         release-ladder flag (anim sets/clears via interlocked
//                       ops; ClearBanks' interlocked clear is the ONE
//                       synchronous game write allowed here -- a benign
//                       both-sides-interlocked flag race).
//    snapWantSkel       anim thread's node-allocation request; game reads.
//  Category [D] deferred-clear -- anim-thread-owned fields the game thread
//               must NOT clear directly: doing so under a running
//               ReplayOntoNpc races it. The game thread only ORs a bit into
//               pendingClear; the anim thread applies the actual clears at its
//               next hook entry (ConsumePendingClear), i.e. before any
//               subsequent replay can read them.
//    rxLast, rxLastLen  hold-last-good list (promoted after a clean replay).
//    rxReacquire        consecutive good frames since release.
//    playClock, playInit  playback clock in the author's ms timeline.
//    curStore, storeMs  snapshot-store ping-pong bookkeeping.
//    fxPrev, fxPrevN    effect-regen phase cache (written by RegenEffects on
//                       the anim thread).
//  Protocol field:
//    pendingClear       atomic bitmask (kAnimClear*): game thread fetch_or's,
//                       anim thread exchanges to 0 at hook entry and applies.
//  --------------------------------------------------------------------------
// ===========================================================================
enum : uint32_t {
    kAnimClearAnimOwned = 1u << 0,   // playInit/curStore/storeMs/rxReacquire   (ClearBanks)
    kAnimClearLastGood = 1u << 1,   // rxLastLen                               (ClearBanks, Detach(dropLastGood))
    kAnimClearFxCache = 1u << 2,   // fxPrevN                                 (session-residual reset)
};

struct AnimReplay {
    // ---- [G] game-thread-sync ---------------------------------------------
    volatile uintptr_t npcAC;
    volatile uintptr_t npcObj;
    uint32_t      rxCmd[2][kMaxAnimCmdDwords];
    volatile long rxLenB[2];
    volatile long rxBank;
    volatile long rxGen;
    AnimSample    ringMeta[kSampleRing];
    volatile long ringNewest;
    volatile long ringCount;
    volatile long rxStampMs;
    float         rxSpine[20];
    bool          rxSpineValid;
    std::atomic<uint32_t> spineGen{ 0 };       // seqlock generation (odd = write in progress)
    uint32_t      lastAnimSeq;               // rx anim sequence gate baseline
    bool          haveAnimSeq;
    uint64_t      rxKey[DICT_CAP];
    bool          rxHave[DICT_CAP];
    uint8_t       rxEpoch;
    bool          rxEpochSet;
    void* volatile snapNode;
    // Replicated weapon-IK feather fade for THIS puppet, as raw float bits (the anim
    // thread reads it, the game thread writes it, and a long is the widest thing both
    // can touch atomically without dragging <atomic> into the payload path). 0 = no IK.
    volatile long wpnIkBits;
    // ---- [A] anim-thread --------------------------------------------------
    uint32_t      rxScratch[kMaxAnimCmdDwords];
    volatile long rxReleased;
    uintptr_t volatile snapWantSkel;
    // ---- [D] deferred-clear -----------------------------------------------
    uint32_t      rxLast[kMaxAnimCmdDwords];
    volatile long rxLastLen;
    long          rxReacquire;
    float         playClock;
    long          playInit;
    long          curStore;
    uint32_t      storeMs[2];
    TrackedAnim   fxPrev[kMaxTrackedAnims];
    int           fxPrevN;
    // ---- deferred-clear protocol ------------------------------------------
    std::atomic<uint32_t> pendingClear{ 0 };

    // The reset tiers. Bodies in anim-rx.cpp; all GAME thread.
    void ClearBanks();                 // banks/ring/stamp/release + the deferred anim-owned clears
    void ClearDict();                  // this slot's rx dictionary
    void Detach(bool dropLastGood);    // drive pointers (+ optional last-good)
    void ResetSeq();                   // rx seq baseline (peer-drop / teardown tier ONLY -- never Detach)
    void ConsumePendingClear();        // ANIM THREAD ONLY: applies the [D] clears at hook entry
};
extern AnimReplay s_replay[kMaxNetPlayers];   // static storage, anim-rx.cpp

// ---------------------------------------------------------------------------
//  Cross-file state: extern here, ONE definition in the most-owning .cpp. The
//  per-slot rows live in s_replay above; what is global is the single-instance
//  TX side, the frame clock, and the shared entry points.
// ---------------------------------------------------------------------------

// ---- defined in anim-tx.cpp (capture branch / sender side) ----------------
extern volatile uintptr_t g_localAC, g_localAnimTable, g_localSkel, g_localFgAnimData;
void CaptureLocalList(uintptr_t a3, uint32_t cursorIn, uint32_t cursorOut);

// ---- defined in anim-rx.cpp -----------------------------------------------
uintptr_t ReplayOntoNpc(uintptr_t ac, uintptr_t ctx, uintptr_t a3,
    const uint32_t* in, int len, int rxSlot);
void NetSnapNodeService();

// ---- defined in anim-hooks.cpp (the frame clock the anim thread reads) ----
extern volatile long g_nowMs;
extern DWORD         g_gameThreadId;   // stamped by NetAnimTickEarly; backs Detach's thread assert

// ---- defined in anim-fx.cpp (effects / grip) ------------------------------
void RegenEffects(int slot, const TrackedAnim* cur, int n, uintptr_t effectList);
void OpenPuppetGripGate(uintptr_t a2, int slot);

// ---- defined in anim-codec.cpp (WIRE-LOCKED codec entry points) -----------
uint32_t NetAnimEncode(const uint32_t* cmd, uint32_t n,
    uint8_t* out, uint32_t capBytes, uint8_t* outEpoch);
uint32_t NetAnimDecode(int senderSlot, const uint8_t* data, uint32_t nBits, uint8_t epoch,
    uint32_t* out, uint32_t maxDwords);
bool AnimListValid(const uint32_t* c, uint32_t n);

namespace animcodec {

    // ---- bit I/O (MSB-first) ---------------------------------------------------
    struct BitW {
        uint8_t* buf; uint32_t capBits; uint32_t pos; bool ok;
        BitW(uint8_t* b, uint32_t capBytes) : buf(b), capBits(capBytes * 8u), pos(0), ok(true) {}
        void put(uint64_t v, int n) {
            if (!ok || pos + (uint32_t)n > capBits) { ok = false; return; }
            for (int i = n - 1; i >= 0; --i) {
                const uint32_t b = pos++;
                const uint8_t  m = (uint8_t)(0x80u >> (b & 7));
                if ((v >> i) & 1ull) buf[b >> 3] |= m; else buf[b >> 3] &= (uint8_t)~m;
            }
        }
    };
    struct BitR {
        const uint8_t* buf; uint32_t nBits; uint32_t pos; bool ok;
        BitR(const uint8_t* b, uint32_t bits) : buf(b), nBits(bits), pos(0), ok(true) {}
        uint64_t get(int n) {
            if (!ok || pos + (uint32_t)n > nBits) { ok = false; return 0; }
            uint64_t v = 0;
            for (int i = 0; i < n; ++i) {
                const uint32_t b = pos++;
                v = (v << 1) | (uint64_t)((buf[b >> 3] >> (7 - (b & 7))) & 1u);
            }
            return v;
        }
    };

    // Cross-file surface of the dictionaries: the tx dictionary/epoch is defined
    // in anim-tx.cpp, the per-sender rx dictionaries live inside AnimReplay
    // (anim-rx.cpp); the encoder/decoder in anim-codec.cpp reach them through
    // these.
    extern uint8_t  g_txEpoch;
    void TxDictReset();
    void TxPendBegin();
    void PutRef(BitW& w, uint64_t key);
    void RxDictEpoch(int s, uint8_t epoch);
    bool GetRef(int s, BitR& r, uint64_t& key);

} // namespace animcodec