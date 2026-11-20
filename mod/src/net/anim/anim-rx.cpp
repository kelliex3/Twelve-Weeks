// ============================================================================
//  anim-rx.cpp -- RX replay side: the per-author-slot AnimReplay state
//  (banks/ring-meta/playclock/snapshot stores/rx dictionaries -- see the
//  writer table in anim-internal.hpp), ingest + replay, the clear/reset tiers,
//  the rx sequence gate, and the snapshot-node service.
//
//  TWO THREADS meet here. Packet ingest, the reset tiers and the snapshot-node
//  allocator are GAME THREAD; ReplayOntoNpc and everything it calls run on the
//  async anim job. Which side owns each field is the writer table.
// ============================================================================
#include "anim.hpp"
#include "anim-internal.hpp"
#include "net-debug.hpp"
#include <intrin.h>            // _InterlockedExchange (publish received lists)
#include <malloc.h>            // _aligned_malloc (pose-snapshot node buffers)

// ===========================================================================
//  THE per-slot state. One struct per ORIGINAL author's net slot
//  (0..kMaxNetPlayers-1) -- a 2v2's three remotes each animate from their own
//  replicated AnimCmdList. STATIC STORAGE: addresses are process-stable, which
//  is what keeps every anim-thread lifetime guarantee intact. The sender side
//  (anim-tx.cpp) stays single-instance -- there is only ever one local player.
//
//  Field-by-field threading/ownership lives in the writer table on the struct
//  (anim-internal.hpp). What each channel is:
//
//  - npcAC/npcObj: the puppet's AnimControl (replay match key, published on
//    the GAME thread from NetApplyRemoteAnim) and its NdGameObject, published
//    beside it (same guard, same clear paths) so the anim thread can reach the
//    object without a DcManager lookup, which it must not do. npcObj is read
//    by OpenPuppetGripGate. npcAC is the ONE match key per puppet -- both
//    hk_CreateAnimCmds and hk_PostAnim compare against it, so there is no
//    second key that can drift out of step with it.
//
//  - rxCmd/rxLenB/rxBank: received list to replay, DOUBLE-BUFFERED per slot:
//    the game thread fills the inactive bank and flips rxBank atomically, so
//    the anim thread never observes a list mid-copy.
//
//  - rxGen: SEQLOCK generation, bumped by the writer AFTER it flips the bank.
//    Two banks tolerate ONE writer lap during a read, but the receive pump can
//    drain 2+ anim packets for the same sender in a single frame (no
//    per-sender cap; the host also re-forwards every client's AnimCmd),
//    flipping 0->1->0 -- the 2nd memcpy then overwrites the very bank an
//    in-flight ReplayOntoNpc (on the async anim-job fiber) is still reading
//    => torn list => bail => a one-frame bind-pose SNAP. The reader copies the
//    latched bank into rxScratch, then re-reads the gen; if it changed, a
//    writer lapped mid-copy, so it discards the copy and holds the last good
//    list. rxScratch/rxLast are SINGLE-CONSUMER per slot (the engine's
//    AnimWorker claims each character with a CAS, so only one anim job ever
//    touches a given puppet at a time -- no lock needed). rxLast = the last
//    list that replayed cleanly, re-emitted on any transient miss so the
//    puppet NEVER shows a bind pose mid-play (ND's own underrun rule: hold the
//    last pose, never drop to a default one).
//
//  - ringMeta/ringNewest/ringCount: the receive-side time buffer that makes
//    interpolation possible. Every accepted list is stamped with the AUTHOR's
//    capture time and the replay picks the samples that bracket a playback
//    clock running slightly behind real time. METADATA ONLY -- the
//    interpolator blends the snapshot STORES, so only the newest sendMs and
//    the count are ever read.
//
//  - rxStampMs/rxReleased/rxReacquire: STALENESS -- an age on last-good, so
//    "hold" can never mean "forever". Holding the last pose is the RIGHT
//    answer for a dropped frame and the WRONG answer for a dead stream: a peer
//    alive on the wire but permanently undecodable would otherwise render as a
//    statue for the rest of the match. rxStampMs is stamped by the game thread on
//    every ACCEPTED publish and read by the anim thread against g_nowMs
//    (published once per frame -- anim-hooks.cpp -- because the anim path must
//    stay syscall-free). ND has no equivalent: their "Stopped receiving
//    snapshots" path (sub_14050F460 -> sub_141CE5B80) is a dead-ragdoll
//    physics handoff gated on IsMultiplayer, and cannot unfreeze a live
//    puppet.
// ===========================================================================
AnimReplay s_replay[kMaxNetPlayers];

// ---------------------------------------------------------------------------
//  Reset tiers. Public entry points (NetAnim*) keep their names and forward.
// ---------------------------------------------------------------------------

// Stop this puppet's replay (both banks, so no stale list can be latched
// afterwards). GAME THREAD. The anim-thread-owned fields
// (playInit/curStore/storeMs/rxReacquire/rxLastLen) are deliberately NOT
// written here: the game thread flipping those under a running ReplayOntoNpc
// races it, and a store that is conceptually invalidated can still feed
// EmitBlendTail. They are cleared via pendingClear at the anim thread's next
// entry for this slot, i.e. before any subsequent replay reads them; an anim
// job already past its entry finishes on the old values, which is one frame
// and harmless.
void AnimReplay::ClearBanks()
{
    _InterlockedExchange(&rxLenB[0], 0);
    _InterlockedExchange(&rxLenB[1], 0);
    _InterlockedExchange(&rxStampMs, 0);
    _InterlockedExchange(&rxReleased, 0);          // both-sides-interlocked flag (benign race, see writer table)
    for (int r = 0; r < kSampleRing; ++r) _InterlockedExchange(&ringMeta[r].len, 0);
    _InterlockedExchange(&ringCount, 0);
    _InterlockedExchange(&ringNewest, 0);
    // Anim-owned interp/hold state -- deferred to the anim thread.
    // kAnimClearLastGood drops last-good too, so a reused slot can't replay
    // the previous peer's pose.
    pendingClear.fetch_or(kAnimClearAnimOwned | kAnimClearLastGood, std::memory_order_release);
}

// Wipe this sender's learned rx dictionary. GAME THREAD (the dictionary is
// only ever touched on the game thread -- the decode path).
void AnimReplay::ClearDict()
{
    memset(rxHave, 0, sizeof(rxHave));
    rxEpochSet = false;
}

// Drop this puppet's drive pointers -- hk_CreateAnimCmds / hk_PostAnim compare
// an incoming AnimControl against npcAC, so zeroing it makes both hooks pass
// that puppet's own call straight through instead of replaying onto it. Its
// receive dictionary/banks are left intact: the same peer resumes after
// respawn and re-publishes via NetApplyRemoteAnim. NEVER touches the rx
// sequence baseline (a respawn is not a stream restart).
//
// LIFETIME CONTRACT: this zeroing is the ONE thing standing between the async
// anim job and a freed AnimControl. It MUST be a synchronous game-thread write
// that completes strictly BEFORE the puppet is killed/despawned -- every
// caller (net-replication: own-respawn NetClearRemoteAnimAll, remote respawn,
// puppet-despawn path) is ordered clear-then-kill. The invariant lives in
// those call sites; the thread assert below at least catches the "wrong
// thread" half of a violation.
void AnimReplay::Detach(bool dropLastGood)
{
#ifdef TO1_DEBUG
    if (g_gameThreadId && GetCurrentThreadId() != g_gameThreadId)
        TO1_LOG("[anim] R4 VIOLATION: Detach off the game thread (tid %lu)\n", GetCurrentThreadId());
#endif
    npcAC = 0;
    npcObj = 0;   // cleared with the AC so nothing can read a dead puppet's object
    rxSpineValid = false;
    // A slot handed to a different peer must not inherit the previous occupant's IK
    // weight; and a dead/despawning puppet should not be solving arms onto a weapon.
    _InterlockedExchange(&wpnIkBits, 0);

    // Last-good is the pose the puppet renders while a packet is in flight. Dropping it means
    // the bind pose until the next accepted list arrives.
    //
    // That is RIGHT when this slot is being torn down or handed to a different peer -- a reused
    // slot must never replay the previous occupant's pose. It is WRONG when we are merely
    // re-creating the puppet objects (our own respawn): the list is keyed to the SENDER's rig
    // and has nothing to do with our death, so wiping it snaps every remote to bind pose for a
    // few frames every single time the local player respawns.
    //
    // rxLastLen is anim-owned, so the drop is deferred to the anim thread's next entry for this
    // slot. With npcAC zeroed above there IS no next entry until the slot is re-published, so
    // the clear lands exactly at the first replay after re-attach -- before any list is held.
    if (dropLastGood) {
        _InterlockedExchange(&rxStampMs, 0);   // and forget its freshness with it
        pendingClear.fetch_or(kAnimClearLastGood, std::memory_order_release);
    }
}

// Rx anim sequence baseline reset. Tied to the PEER-DROP / full-teardown tiers
// only (NetAnimClearSlot / NetAnimDictReset) -- the next packet from this
// author re-seeds, whatever it is. Detach must NOT call this.
void AnimReplay::ResetSeq()
{
    lastAnimSeq = 0;
    haveAnimSeq = false;
}

// The anim thread consumes the deferred clears at its hook entry, BEFORE any
// read of the fields being cleared. ANIM THREAD ONLY (plain stores are the
// owner's own writes; the atomic exchange orders them against the game
// thread's fetch_or).
void AnimReplay::ConsumePendingClear()
{
    const uint32_t bits = pendingClear.exchange(0, std::memory_order_acquire);
    if (!bits) return;
    if (bits & kAnimClearAnimOwned) {
        playInit = 0;              // clock re-seeds on next sample (playClock is gated by it)
        curStore = 0;
        storeMs[0] = storeMs[1] = 0;
        rxReacquire = 0;
    }
    if (bits & kAnimClearLastGood)
        rxLastLen = 0;                // body stays; len gates every reader
    if (bits & kAnimClearFxCache) {
        fxPrevN = 0;                 // N=0 alone forces RegenEffects' set-changed resync,
        // so no stale phase-delta can ever fire (no memset needed)
    }
}

// ---------------------------------------------------------------------------
//  The rx anim sequence gate. The state lives in AnimReplay; the wire header
//  parse stays in the session layer. Returns true = fresh packet, baseline
//  advanced; false = duplicate/reorder -- the caller
//  must DROP the packet, and in particular must NOT relay it (drop-before-
//  relay: a duplicate eaten here must never fan out to the other peers).
//
//  The -1024 window mirrors NetPlayer::PushState: a small backwards step is a
//  reorder or a duplicate and is dropped; a large one means the author
//  restarted its sequence, so accept it rather than freezing that puppet for
//  the rest of the match. Baseline resets: peer drop (NetAnimClearSlot) and
//  full teardown (NetAnimDictReset). Detach never resets it.
// ---------------------------------------------------------------------------
bool NetAnimRxIngest(int authorSlot, uint32_t seq)
{
    if (authorSlot < 0 || authorSlot >= kMaxNetPlayers) return false;
    AnimReplay& r = s_replay[authorSlot];
    if (r.haveAnimSeq) {
        const int32_t d = static_cast<int32_t>(seq - r.lastAnimSeq);
        if (d <= 0 && d > -1024) return false;   // reordered/duplicate -> reject (and do not relay)
    }
    r.lastAnimSeq = seq;
    r.haveAnimSeq = true;
    return true;
}

// ===========================================================================
//  POSE SNAPSHOT NODES  (ND-style two-snapshot interpolation)
//
//  The engine can capture the result of a command list into a "snapshot node" and later
//  restore it, without re-evaluating the list:
//    {0x22 sz4, d1=SRC slot, d2:d3=sub-block}   capture  -> handler 0x14169C9C7
//    {0x21 sz4, d1=DEST slot, d2:d3=sub-block}  restore  -> handler 0x14169C97A
//  ND uses exactly this in NetAnimCmdGenerator (0x1408F7380) to blend two received
//  snapshots, so each list is executed at most ONCE and every frame in between is a cheap
//  restore+blend. That is what makes their remote animation smooth instead of stepped.
//
//  VERIFIED against the interpreter:
//   * The terminator publishes POSE SLOT 0, hardcoded inside sub_14218B310 / sub_14218A0A0.
//     There is no "result slot" operand and no result field -- every producer carries its own
//     explicit dest slot. (Mechanically it publishes through the context rather than a named
//     slot, but the slot is fixed at 0, so slot 0 IS the output.)
//   * Both handlers do `mov r9,[cmd+8]; cmp [r9+0x18],eax` with NO NULL CHECK -- a null
//     sub-block pointer is a hard fault. We therefore never let one be emitted.
//   * On id mismatch 0x21 clears the dest slot (sub_142189FC0) and 0x22 is a silent no-op,
//     so a WRONG-but-non-null node degrades safely rather than corrupting.
//   * 0x22 is a genuine deep copy into the node's own three buffers (sub_14218AF30), so a
//     node holds no pointer into the puppet, its FgAnimData, or any engine allocation.
//
//  We deliberately do NOT use the engine's node pool, its ctor, its bind (sub_141612730 --
//  it allocates on every call and never frees, so re-binding leaks), or its sub-block
//  selector. We hand-build the 72-byte object from our own heap using only the four fields
//  the interpreter actually reads. Nodes are allocated ONCE per process and never freed:
//  that is what makes them immune to the async anim-job teardown races that have already
//  produced three crashes in this project.
// ===========================================================================
static constexpr uint32_t OP_SNAP_RESTORE = 0x21;
static constexpr uint32_t OP_SNAP_CAPTURE = 0x22;

struct SnapSub {                 // 32 B -- exactly what sub_14218AA20 / sub_14218AF30 index
    void* pJoints;            // 48 * numJoints
    void* pFloats;            // 4  * numFloatChannels  (may be null when the rig has none)
    void* pMasks;             // 16 * numVecChannels    (MUST be non-null: 0x22 has no guard)
    uint32_t id;                 // AnimHierarchy id -- compared against [hier+4]
    uint32_t pad;
};
// Two sub-blocks over the SAME joint/float buffers, differing only in pMasks:
//   cap - what 0x22 writes into. Its exported mask can legitimately come back zero.
//   rst - what 0x21 reads.
// 0x21 ANDs the mask with -1<<(64-nj) per group, so all-0xFF means exactly "every channel of
// this group is valid" - the same thing opcode 0x0D does (0x14169BB36 memsets 0xFF).
// One pose STORE = the buffers a captured pose lives in.
struct SnapStore {
    SnapSub  cap;
    SnapSub  rst;
};
// TWO stores per puppet: the interpolator blends between them. Allocated ONCE per slot for the
// process lifetime and never freed or re-bound - that is what makes them immune to the async
// anim-job teardown races that have already produced three crashes in this project.
struct SnapNode { SnapStore a, b; };

// The id the interpreter itself compares: hierarchy = *(u64*)(skel+0x50), id = *(u32*)(hier+4).
// NOT skel+0x38 (ND's cached copy).
static inline uint32_t SnapHierId(uintptr_t skel) {
    if (!San(skel)) return 0;
    uintptr_t hier = Rd<uintptr_t>(skel + 0x50);
    return San(hier) ? Rd<uint32_t>(hier + 4) : 0;
}

// Returns the node to use for this skeleton, or null if we must not emit anything.
// (snapNode is written by the game thread -- NetSnapNodeService does the allocation, the
// anim job never allocates; snapWantSkel is the anim thread's allocation request.)
static SnapNode* SnapNodeFor(int slot, uintptr_t skel) {
    if (slot < 0 || slot >= kMaxNetPlayers) return nullptr;
    AnimReplay& r = s_replay[slot];
    SnapNode* n = (SnapNode*)r.snapNode;
    if (!n) { r.snapWantSkel = skel; return nullptr; }   // ask the game thread to build it
    const uint32_t id = SnapHierId(skel);
    if (!id || id != n->a.cap.id) return nullptr;        // wrong rig for the allocated node
    return n;
}

// ---- engine primitives (called only in-context, always SEH-guarded) --------
//
// clip StringId64 -> local ArtItemAnim* via AnimTable::LookupAnim(tbl, out24, nameId).
// IMPORTANT: the table is SKELETON-SPECIFIC (its skelId goes into the key), so the
// sender's clips must be resolved against a table for the SENDER's skeleton -- i.e.
// the receiver's own local player, NOT the NPC puppet. The resulting pointers are
// then fed to the puppet's command list.
static uintptr_t LookupAnimPtr(uintptr_t table, uint64_t nameId) {
    if (!San(table) || !nameId) return 0;
    using Fn = uintptr_t(__fastcall*)(uintptr_t tbl, void* out24, uint64_t nameId);
    Fn fn = reinterpret_cast<Fn>(g_base + RVA_LOOKUPANIM);
    uint8_t out[32] = {};
    __try { fn(table, out, nameId); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    uintptr_t p = *reinterpret_cast<uintptr_t*>(out + 16);   // out+16 = ArtItemAnim*
    return San(p) ? p : 0;
}

// RECEIVER: portable StringId64 -> LOCAL slot index, building the entry for our rig if
// needed. Idempotent (hash lookup first). MUST run on the game thread: it takes the
// table lock and allocates from the FixedSizeHeap -- never call it from the anim job.
static uint32_t FeatherSidToIdx(uint64_t sid, uintptr_t fgAnimData) {
    if (!sid || !San(fgAnimData)) return FB_INVALID;
    using Fn = uint32_t(__fastcall*)(uintptr_t tbl, uint64_t sid, uintptr_t fad);
    Fn fn = reinterpret_cast<Fn>(g_base + RVA_FB_LOGIN);
    uint32_t idx = FB_INVALID;
    __try { idx = fn(g_base + OFF_FEATHERTABLE, sid, fgAnimData); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return FB_INVALID; }
    return idx;
}

// Cheap read-only probe: does `idx` really name an entry for `sid` on `skel`'s rig?
// The engine itself only checks the used-bit and the skelId, never the identity -- so
// this is the check that stops a collision from silently masking the wrong joints.
static bool FeatherEntryMatches(uint32_t idx, uint64_t sid, uintptr_t skel) {
    if (idx == FB_INVALID || !sid || !San(skel)) return false;
    using Fn = uintptr_t(__fastcall*)(uintptr_t tbl, uint32_t idx);
    Fn fn = reinterpret_cast<Fn>(g_base + RVA_FB_GETENTRY);
    uintptr_t e = 0;
    __try { e = fn(g_base + OFF_FEATHERTABLE, idx); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!San(e)) return false;
    return Rd<uint64_t>(e + FBE_SID) == sid &&
        Rd<uint32_t>(e + FBE_SKELID) == Rd<uint32_t>(skel + SKEL_SKELID);
}

// Exact (skelId, nameId) hash probe -- the key ResourceTable actually registers anims
// under. Using the SENDER's skelId reproduces the sender's ArtItemAnim exactly.
static uintptr_t LookupAnimExact(uint32_t skelId, uint64_t nameId) {
    if (!skelId || !nameId) return 0;
    using Fn = void* (__fastcall*)(void* out24, uint32_t skelId, uint64_t nameId);
    Fn fn = reinterpret_cast<Fn>(g_base + RVA_LOOKUP_EXACT);
    uint8_t out[32] = {};
    __try { fn(out, skelId, nameId); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    uintptr_t p = *reinterpret_cast<uintptr_t*>(out + 16);
    return San(p) ? p : 0;
}

// Re-derive a feather/partial-blend layer's per-channel mask locally. The mask
// pointers are per-frame single-frame-heap allocations, so they can only be made on
// this machine; the jointSetIdx travels inside the command. Runs inside
// CreateAnimCmds on the anim thread -- the same context the engine calls it in, so
// the single-frame heap + feather table are live.
//
// ABI: the real signature is
//   float FeatherBlendTable::CreateChannelFactorsForAnimCmd(
//       const ArtItemSkeleton*, int jointSetIdx, float blend,
//       const ChannelFactor* const**, const unsigned**, float featherStrength)
// The 3rd parameter is a FLOAT (xmm3 low 32 bits) and is the layer BLEND, not a
// phase. Declaring it double makes the callee read ~0.0f, which multiplies every
// channel factor to zero -> every partial layer (arms/weapon/upper body) replays
// at weight 0. Pass 1.0f, the steady-state value the engine itself uses; the
// command's own d5 already carries the sender's resolved fade weight.
// NOTE the skeleton argument: the feather table is SKELETON-KEYED exactly like the
// AnimTable -- it bails unless entry.skelId == *(u32*)(skel+52). The jointSetIdx in the
// command came from the SENDER's layers, so it must be resolved against the SENDER's
// skeleton. Passing the NPC puppet's skeleton fails 100% of the time (measured: feather
// ok=0 / fail=546 on device), which drops every partial layer and is why the arms do not
// match. We pass the local player's skeleton, which is the same rig.
static bool FeatherDerive(uintptr_t skel, int jointSetIdx,
    uint64_t* outFactors, uint64_t* outCounts) {
    *outFactors = 0; *outCounts = 0;
    if (!San(skel) || jointSetIdx < 0) return false;
    using Fn = float(__fastcall*)(uintptr_t self, uintptr_t skel, int jsi, float blend,
        uint64_t* o1, uint64_t* o2, float featherStrength);
    Fn fn = reinterpret_cast<Fn>(g_base + RVA_FEATHER);
    uintptr_t self = g_base + OFF_FEATHERTABLE;
    __try { fn(self, skel, jointSetIdx, 1.0f, outFactors, outCounts, 1.0f); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *outFactors = 0; *outCounts = 0; return false; }
    return (*outFactors != 0 && *outCounts != 0);
}

namespace animcodec {

    // ---- receiver dictionary ---------------------------------------------------
    // PER SENDER SLOT (inside AnimReplay): each sender owns an independent 10-bit
    // index space + epoch, so sender B's index N must never resolve against sender
    // A's definitions (that would decode the wrong clip or reject the whole packet
    // -> freeze). Keyed by the ORIGINAL author's net slot. Game thread only.

    void RxDictEpoch(int s, uint8_t epoch) {
        AnimReplay& r = s_replay[s];
        if (r.rxEpochSet && epoch == r.rxEpoch) return;
        r.rxEpoch = epoch; r.rxEpochSet = true;
        memset(r.rxHave, 0, sizeof(r.rxHave));
    }
    // false => the sender referenced an entry we never learned (packet must be rejected;
    // the puppet then holds its last good pose, which is far better than a wrong one).
    bool GetRef(int s, BitR& r, uint64_t& key) {
        AnimReplay& rep = s_replay[s];
        const uint32_t hasDef = (uint32_t)r.get(1);
        const uint32_t idx = (uint32_t)r.get(10);
        if (!r.ok) return false;
        if (hasDef) {
            key = r.get(64);
            if (!r.ok) return false;
            if (idx < DICT_CAP) { rep.rxKey[idx] = key; rep.rxHave[idx] = true; }
            return true;
        }
        if (idx >= DICT_CAP || !rep.rxHave[idx]) return false;
        key = rep.rxKey[idx];
        return true;
    }

} // namespace animcodec

// ---------------------------------------------------------------------------
//  PLAYBACK CLOCK + SAMPLE SELECTION
//
//  The clock runs in the AUTHOR's ms timeline and chases (newest - kInterpBufferMs), so we are
//  always rendering a moment that we already have BOTH ends of. Ported from ND's controller
//  (0x1408FAF90): catch up fast when far behind, ease when close, never run past the newest
//  sample. There is deliberately NO slow-down branch - ND has none, and inventing one makes the
//  clock oscillate.
// ---------------------------------------------------------------------------

// ND's default (qword_14409565C, a live dev slider in their build). Small enough that the added
// latency is imperceptible, large enough to cover ordinary inter-packet jitter.
static constexpr float kInterpBufferMs = 25.0f;

// Master switch. OFF: apply the newest sample directly. ON: two-snapshot pose blending --
// each distinct received pose is translated once, captured, and every frame after that we
// restore two and blend on a clock running 25 ms behind the newest. Kept as a kill switch
// because this emits engine snapshot opcodes on the anim thread. It ALSO gates snapshot
// CAPTURE, so turning it OFF stops both.
volatile long g_animInterp = 1;

static void UpdatePlayClock(int slot, float dtMs) {
    AnimReplay& rep = s_replay[slot];
    const long n = rep.ringCount;
    if (n <= 0) return;
    const uint32_t newestMs = rep.ringMeta[rep.ringNewest].sendMs;
    const float    target = (float)newestMs - kInterpBufferMs;

    if (!rep.playInit) { rep.playClock = target; rep.playInit = 1; return; }

    const float lag = target - rep.playClock;
    if (lag > 250.0f || lag < -250.0f) {          // hitch / stream restart -> snap, do not crawl
        rep.playClock = target;
        return;
    }
    if (dtMs > lag)            rep.playClock += lag * 0.5f;
    else if (lag > 0.25f)      rep.playClock += (lag * 0.25f > 0.15f) ? lag * 0.25f : 0.15f;
    else { rep.playClock += dtMs; if (lag > 0.0f) rep.playClock += dtMs * 0.30f; }

    // Never render ahead of the newest sample we hold.
    const float capMs = (float)newestMs;
    if (rep.playClock > capMs) rep.playClock = capMs;
}

// Worst case emitted by EmitBlendTail: restore(4) + restore(4) + blend(6).
// Every caller must have at least this much room left in its buffer.
static constexpr int kMaxBlendTailDwords = 14;

static int EmitBlendTail(uint32_t* out, SnapNode& n, int slot) {
    AnimReplay& rep = s_replay[slot];
    SnapStore& cur = rep.curStore ? n.b : n.a;   // newest captured pose
    SnapStore& prev = rep.curStore ? n.a : n.b;   // the one before it

    const uint32_t tCur = rep.storeMs[rep.curStore];
    const uint32_t tPrev = rep.storeMs[rep.curStore ? 0 : 1];

    int k = 0;
    out[k] = MakeCmd(OP_SNAP_RESTORE, 4); out[k + 1] = 0;
    *reinterpret_cast<uint64_t*>(&out[k + 2]) = (uint64_t)&cur.rst;
    k += 4;

    // Only blend when we genuinely have two DISTINCT poses to blend between. Blending a pose
    // with itself is wasted work; blending against a never-written store is a bind pose.
    if (tPrev && tCur && tPrev != tCur) {
        out[k] = MakeCmd(OP_SNAP_RESTORE, 4); out[k + 1] = 1;
        *reinterpret_cast<uint64_t*>(&out[k + 2]) = (uint64_t)&prev.rst;
        k += 4;

        const float span = (float)(int32_t)(tCur - tPrev);
        float a = (span > 0.0f) ? ((rep.playClock - (float)tPrev) / span) : 1.0f;
        if (!(a >= 0.0f)) a = 0.0f;
        if (a > 1.0f)     a = 1.0f;

        out[k] = MakeCmd(OP_BLEND, 6);
        out[k + 1] = 0;                  // srcA = slot 0 = the newer pose
        out[k + 2] = 1;                  // srcB = slot 1 = the older pose
        out[k + 3] = 0;                  // dst selector: != srcB -> writes slot 0
        out[k + 4] = 1;                  // mode
        *reinterpret_cast<float*>(&out[k + 5]) = 1.0f - a;
        k += 6;
    }
    return k;
}

// RECEIVER, ANIM THREAD: rebuild the remote template into the NPC's command buffer.
// Builds into a scratch buffer and only commits if the WHOLE list translates, so a3
// is never left malformed. Returns the header-slot ptr (CreateAnimCmds' contract) on
// success, or 0 to send the hook down its hold-last-good / bind-pose ladder.
uintptr_t ReplayOntoNpc(uintptr_t ac, uintptr_t ctx, uintptr_t a3,
    const uint32_t* in, int len, int rxSlot) {
    uintptr_t buf = Rd<uintptr_t>(a3);
    uint32_t  cursorIn = Rd<uint32_t>(a3 + 12);
    if (!San(buf)) return 0;
    // Feather masks must be built against the SENDER's rig, not the puppet's (the
    // feather table is skeleton-keyed) -- our own local player is that same rig.
    // Fall back to the puppet's own skeleton (*ctx) only if we haven't seen the
    // player's yet; that at worst reproduces the all-fail behaviour of the wrong rig.
    uintptr_t skel = (uintptr_t)g_localSkel;
    if (!San(skel)) skel = Rd<uintptr_t>(ctx);                 // *a2 = the PUPPET's ArtItemSkeleton*

    // +32, not +16: the terminator site appends the snapshot capture and blend tail
    // AND the synthesised 16-dword weapon-ik-feather command (see EmitWeaponIkFeather).
    static const int SCRATCH = kMaxAnimCmdDwords + 32;
    uint32_t out[kMaxAnimCmdDwords + 32];
    int oi = 0;

    AnimReplay& rep = s_replay[rxSlot >= 0 ? rxSlot : 0];      // rxSlot is >= 0 on every hook call

    // Pose-snapshot node for this puppet, or null if we must not emit 0x21/0x22 at all
    // (node not built yet, or this rig's hierarchy id doesn't match the one we allocated for).
    // Resolved ONCE here so the emit sites below cannot disagree about it.
    SnapNode* snap = (rxSlot >= 0) ? SnapNodeFor(rxSlot, Rd<uintptr_t>(ctx)) : nullptr;

    // Replicated weapon-IK fade for this puppet (0 = weapon stowed / nothing received yet).
    // Read ONCE per call so both emit sites below agree, exactly as `snap` is.
    const long  wpnBits = rep.wpnIkBits;
    const float wpnIk = *reinterpret_cast<const float*>(&wpnBits);

    // Clips replayed this frame, in emission order -- the input to effect regeneration below.
    TrackedAnim fx[kMaxTrackedAnims];
    int         fxN = 0;

    // ---------------------------------------------------------------------------
    //  INTERPOLATION
    //
    //  Each DISTINCT received pose is translated exactly once - clip pointers resolved, feather
    //  masks derived - and captured into one of two stores. Every frame after that we merely
    //  restore both and blend, which costs 14 dwords and no lookups at all. That is what stops
    //  puppets being a step function of packet arrival while their root is smoothly lerped.
    //
    //  Two paths below:
    //    - a NEW sample since our last capture -> fall through, translate it normally, and the
    //      capture site further down appends the capture + restores + blend.
    //    - no new sample -> emit only restores + blend here and return. This is the common case
    //      at any send rate below the frame rate, and it is where the smoothing actually happens.
    // ---------------------------------------------------------------------------
    const bool interpOn = (snap && g_animInterp && rxSlot >= 0 && rep.ringCount > 0);
    uint32_t   newestMs = 0;
    if (interpOn) {
        newestMs = rep.ringMeta[rep.ringNewest].sendMs;
        UpdatePlayClock(rxSlot, TIME::GET_DELTA_TIME() * 1000.0f);

        const bool haveNewSample = (rep.storeMs[rep.curStore] != newestMs);
        if (!haveNewSample && rep.storeMs[0] && rep.storeMs[1]) {
            // 2 header + EmitBlendTail's worst case + 2 terminator. Sized off the
            // constant so a longer tail cannot silently run past the end.
            uint32_t f[2 + kMaxBlendTailDwords + kWeaponIkDwords + 2];
            int k = 0;
            f[k++] = MakeCmd(OP_HEADER, 2); f[k++] = 2;
            k += EmitBlendTail(&f[k], *snap, rxSlot);
            // The blend tail leaves the finished pose in slot 0, so the arm solve goes
            // here -- after it, before the terminator publishes. This path runs on every
            // frame between packets, i.e. MOST frames: skipping it here and emitting only
            // on the slow path would toggle the IK on and off at the packet rate.
            k += EmitWeaponIkFeather(&f[k], 0, wpnIk);
            f[k++] = MakeCmd(OP_TERM, 2);   f[k++] = 0;
            if (SafeRW(buf + 4ull * cursorIn, f, (size_t)k * 4, true)) {
                Wr<uint32_t>(a3 + 12, cursorIn + (uint32_t)k);
                Wr<uintptr_t>(a3 + 16, 0);
                Wr<uint32_t>(a3 + 24, 0);
                return buf + 4ull * (cursorIn + 1);
            }
        }
    }

    // Resolve clips against the LOCAL PLAYER's table, not the puppet's: the AnimTable
    // hash key includes the skeleton id, so the sender's clips only exist under the
    // sender's skeleton. Our own local player shares that skeleton; the puppet does not.
    const uintptr_t table = (uintptr_t)g_localAnimTable;

    for (int i = 0; i < len; ) {
        uint32_t sz = CmdSize(in[i]);
        uint32_t op = CmdOp(in[i]);
        if (sz == 0 || i + (int)sz > len) return 0;            // malformed template
        if (oi + (int)sz + 2 > SCRATCH)   return 0;            // over budget

        if (op == OP_CLIP) {
            uint64_t nameId = *reinterpret_cast<const uint64_t*>(&in[i + 2]);
            // Tiered resolve. Anims are registered under (nameId, skelId), so try the
            // anim's OWN skelId first (shipped in d1) -- that reproduces the sender's
            // ArtItemAnim exactly, including clips authored on a different rig than our
            // base layer, which a base-layer-table-only lookup could never find.
            uint32_t  srcSkelId = in[i + 1];
            uintptr_t p = LookupAnimExact(srcSkelId, nameId);
            if (!p) p = LookupAnimPtr(table, nameId);          // same-rig + retarget sources
            if (p) {
                for (uint32_t k = 0; k < sz; ++k) out[oi + k] = in[i + k];
                *reinterpret_cast<uint64_t*>(&out[oi + 2]) = (uint64_t)p;
                oi += sz;
                // Record it for effect regeneration: {identity, resolved anim, phase in frames}.
                // Emission order is part of the identity test, so append in list order.
                if (fxN < kMaxTrackedAnims) {
                    fx[fxN].sid = nameId;
                    fx[fxN].anim = p;
                    fx[fxN].phase = *reinterpret_cast<const float*>(&in[i + 5]);
                    ++fxN;
                }
            }
            else {
                // Unresolvable clip -> bind pose IN PLACE, same dest slot. This is a
                // legal substitution (0x0D is a real producer with an all-ones mask,
                // always finite) and is exactly what the engine does for a missing
                // clip at 0x14166B49E. It preserves the slot graph; deleting would not.
                out[oi] = MakeCmd(OP_EMPTY, 2);
                out[oi + 1] = in[i + 4];                       // same pose slot
                oi += 2;
            }
        }
        else if (op == OP_FEATHER) {
            // d10 was already translated to a LOCAL, identity-checked index at packet
            // ingest on the game thread (NetSetRemoteAnimCmds) -- this thread must not
            // call LoginFeatherBlend. Deriving the mask itself is lock-free and legal
            // here, in the same context the engine calls it.
            int jsi = (int)in[i + 10];
            uint64_t fac = 0, cnt = 0;
            for (uint32_t k = 0; k < sz; ++k) out[oi + k] = in[i + k];
            if (FeatherDerive(skel, jsi, &fac, &cnt)) {
                *reinterpret_cast<uint64_t*>(&out[oi + 6]) = cnt;   // counts  d6:d7
                *reinterpret_cast<uint64_t*>(&out[oi + 8]) = fac;   // factors d8:d9
                oi += sz;
            }
            else {
                // Entry validated but produced no mask (engine-valid empty mask) ->
                // plain blend over the same slots/weight. First 6 dwords are identical
                // EXCEPT d4: a blend needs the literal 1 there, but the feather holds its
                // MODE. Without this the puppet gets a blend with a garbage 'one' field --
                // and this is the one downgrade site that runs AFTER the last AnimListValid,
                // so nothing downstream catches it. (Same fix as the sender-side downgrade
                // in anim-tx.cpp CaptureLocalList and the game-thread ingest downgrade in
                // NetSetRemoteAnimCmds below; this site was historically missed.)
                out[oi] = MakeCmd(OP_BLEND, 6);
                out[oi + 4] = 1u;
                oi += 6;
            }
        }
        else {
            // CAPTURE the finished pose into our snapshot node, immediately BEFORE the
            // terminator. The terminator publishes slot 0, so slot 0 is the completed pose and
            // is exactly what we want stored. This is inert by design: 0x22 only reads slot 0
            // and writes into our own buffers. Gated on g_animInterp, so interp OFF also stops
            // capture.
            // 24 covers the capture (4) + the blend tail + the terminator, with margin.
            static_assert(24 >= 4 + kMaxBlendTailDwords + 2, "OP_TERM budget too small");
            if (op == OP_TERM && snap && g_animInterp && oi + 24 <= SCRATCH) {
                // We are about to emit the terminator, so slot 0 currently holds the FULLY
                // TRANSLATED pose for the newest sample - clip pointers resolved, feather masks
                // derived. Capturing here is what lets later frames re-use it without redoing any
                // of that work, and it is the only point in the list where the pose is complete.
                SnapStore& dst = interpOn ? (rep.curStore ? snap->a : snap->b)
                    : snap->a;
                out[oi] = MakeCmd(OP_SNAP_CAPTURE, 4);
                out[oi + 1] = 0;                                   // source = the published slot
                *reinterpret_cast<uint64_t*>(&out[oi + 2]) = (uint64_t)&dst.cap;
                oi += 4;

                if (interpOn) {
                    // Ping-pong: the store we just wrote becomes "current", the other holds the
                    // previous distinct pose. Two stores is exactly enough to bracket the clock.
                    const long newCur = rep.curStore ? 0 : 1;
                    rep.curStore = newCur;
                    rep.storeMs[newCur] = newestMs;
                    oi += EmitBlendTail(&out[oi], *snap, rxSlot);
                }
            }

            // Solve both arms onto the weapon, LAST -- after the capture and after the
            // blend tail, so it runs exactly once per frame on the pose that is about to
            // be published, with the fade received this frame.
            //   Deliberately AFTER the capture, not before it: the snapshot stores then
            // hold raw poses, and the interp fast path (which re-emits this same command
            // after its own blend) applies the solve once too. Capturing solved poses
            // instead would apply it twice on every between-packet frame and freeze the
            // weight at whatever it was when the packet landed.
            if (op == OP_TERM && oi + kWeaponIkDwords + (int)sz <= SCRATCH)
                oi += EmitWeaponIkFeather(&out[oi], 0, wpnIk);

            // Everything the sender emits is pointer-free by construction; it already
            // re-encoded or refused anything else. Carry verbatim.
            for (uint32_t k = 0; k < sz; ++k) out[oi + k] = in[i + k];

            // RAISE THE HEADER COUNT for interpolation. The blend tail restores into pose slots
            // 0 AND 1, but this header declares only as many slots as the SAMPLE used -- which can
            // be 1. Slot indices are never bounds-checked by the interpreter (0x14169B778 and the
            // 0x21 handler both index straight off [ctx+0x10]), so writing slot 1 into a 1-slot
            // allocation runs past the initialised region into uninitialised bump-allocator
            // memory. That garbage then gets blended, producing non-finite joints and a
            // non-finite world position -- which surfaces as
            //   ASSERT IsFinite(pointWs) in RayCaster::RayTest::SetSourcePosWs
            // one raycast later, nowhere near the actual cause.
            if (op == OP_HEADER && interpOn && sz >= 2 && out[oi + 1] < 2u)
                out[oi + 1] = 2u;

            oi += sz;
        }
        i += sz;
    }

    // commit scratch -> engine buffer, then replicate CreateAnimCmds' epilogue.
    if (!SafeRW(buf + 4ull * cursorIn, out, (size_t)oi * 4, true)) return 0;
    Wr<uint32_t>(a3 + 12, cursorIn + (uint32_t)oi);
    Wr<uintptr_t>(a3 + 16, 0);
    Wr<uint32_t>(a3 + 24, 0);

    // Fire the effects this frame's clips crossed. AFTER the commit: a frame we end up
    // rejecting must not leave the effect cache advanced AND the audio already played.
    // Frame order is Step -> CreateAnimCmds (us) -> the drain (PostJointUpdate_Async),
    // same thread, so appends made here are consumed this frame.
    //
    // The sink is the ANIMCONTROL itself (`ac`). Verified in AnimControl::Step: each layer-type
    // case does `mov r8, rdi` (rdi = the AnimControl) and then `cmp [layer+0x3E],0 / cmovnz r8,0`
    // -- i.e. the "effect list" argument IS the AnimControl, and muting a layer works by passing
    // null instead of it. Step also zeroes a counter at AC+0x0C on entry, consistent with the
    // list living inside it.
    //
    // Do NOT pass `a3+16` here (the command-buffer descriptor's HEADER SLOT pointer): it is a
    // valid-but-wrong pointer that sails past the callee's null guard and silently fires
    // nothing, i.e. no footsteps at all with the toggle on.
    // Skipped for a released puppet: it is running its own native anims again, so its own
    // effects are correct and ours would double up.
    if (rxSlot >= 0 && g_animEffects && !rep.rxReleased)
        RegenEffects(rxSlot, fx, fxN, ac);

    return buf + 4ull * (cursorIn + 1);                         // header count slot
}

// GAME THREAD ONLY. Allocates the pose-snapshot node the anim thread asked for. Runs once
// per slot for the life of the process -- nodes are never freed and never re-bound, so no
// anim job can ever be looking at one that is being torn down.
//
// Buffer sizes come from the SAME AnimHierarchy whose id we stamp into the node, so the
// interpreter's indexing (joints 48*n, floats 4*n, masks 16*n) is self-consistent with the
// allocation by construction: if the id matches at execute time, the buffers are the right
// size for the rig being captured.
void NetSnapNodeService() {
    for (int s = 0; s < kMaxNetPlayers; ++s) {
        AnimReplay& rep = s_replay[s];
        if (rep.snapNode) continue;
        const uintptr_t skel = rep.snapWantSkel;
        if (!San(skel)) continue;

        const uintptr_t hier = Rd<uintptr_t>(skel + 0x50);
        if (!San(hier)) continue;
        const uint32_t id = Rd<uint32_t>(hier + 4);
        const uint16_t nJ = Rd<uint16_t>(hier + 0x12);
        const uint16_t nF = Rd<uint16_t>(hier + 0x14);
        const uint16_t nV = Rd<uint16_t>(hier + 0x18);
        // Refuse anything that doesn't look like a character rig rather than trusting a
        // possibly-misread header and allocating something absurd.
        if (!id || !nJ || nJ > 1024 || nV > 1024 || nF > 4096) continue;

        // Sizes carry deliberate SLACK. 0x22's float copy (0x14218B138) moves whole 16-byte
        // chunks and can write up to 12 bytes past 4*nF; the joint copy and the mask pair at the
        // top group need one element of headroom each. Without the padding this is a heap
        // overrun, not a fidelity issue.
        const size_t cbJ = 48u * (size_t)nJ + 48u;
        const size_t cbF = nF ? (4u * (size_t)nF + 16u) : 0u;
        const size_t cbM = 16u * ((size_t)(nV ? nV : 1u) + 2u);

        SnapNode* n = (SnapNode*)_aligned_malloc(sizeof(SnapNode), 16);
        if (!n) continue;
        memset(n, 0, sizeof(*n));
        void* pJ = _aligned_malloc(cbJ, 16);
        void* pF = cbF ? _aligned_malloc(cbF, 16) : nullptr;
        void* pMc = _aligned_malloc(cbM, 16);      // capture masks
        if (!pJ || !pMc || (cbF && !pF)) {
            // Leak the partial node rather than free it: an anim job could already have
            // read the pointer we are about to publish. We simply never publish it.
            continue;
        }

        // Seed the joint/float buffers from the BIND POSE, not zeroes. A zeroed joint buffer is
        // all-zero quaternions, which is non-finite once normalised - and this project already
        // has an IsFinite assert crash on record. Bind pose renders cleanly if a capture is ever
        // partial or missing.
        const uint32_t bindOff = Rd<uint32_t>(hier + 0x20);
        const uint32_t floatOff = Rd<uint32_t>(hier + 0x24);
        if (!bindOff) continue;                                     // no bind pose -> refuse
        memcpy(pJ, (const void*)(hier + bindOff), 48u * (size_t)nJ);
        memset((uint8_t*)pJ + 48u * (size_t)nJ, 0, 48u);            // the slack
        if (cbF) {
            if (floatOff) memcpy(pF, (const void*)(hier + floatOff), 4u * (size_t)nF);
            else          memset(pF, 0, 4u * (size_t)nF);
            memset((uint8_t*)pF + 4u * (size_t)nF, 0, 16u);
        }
        memset(pMc, 0x00, cbM);      // capture: 0x22 writes what it actually validated

        // rst uses the CAPTURE's OWN mask, not an all-ones one.
        //
        // A separate memset-0xFF mask would make a restore work even if the capture's exported
        // mask came back zero. That is defensible for a single restore - unwritten channels come
        // back as the seeded bind pose - but it is WRONG for a blend: it
        // asserts that channels holding seed data are real animation, so the blender interpolates
        // genuine poses against bind-pose joints at a continuously moving weight, which makes the
        // puppet meshes explode.
        n->a.cap.pJoints = pJ; n->a.cap.pFloats = pF; n->a.cap.pMasks = pMc; n->a.cap.id = id;
        n->a.rst.pJoints = pJ; n->a.rst.pFloats = pF; n->a.rst.pMasks = pMc; n->a.rst.id = id;

        // Store B: its OWN joint/float/mask buffers. The two stores must be independent or the
        // blend would be reading one pose twice.
        void* pJ2 = _aligned_malloc(cbJ, 16);
        void* pF2 = cbF ? _aligned_malloc(cbF, 16) : nullptr;
        void* pMc2 = _aligned_malloc(cbM, 16);
        if (!pJ2 || !pMc2 || (cbF && !pF2)) continue;   // leak, never publish
        memcpy(pJ2, (const void*)(hier + bindOff), 48u * (size_t)nJ);
        memset((uint8_t*)pJ2 + 48u * (size_t)nJ, 0, 48u);
        if (cbF) {
            if (floatOff) memcpy(pF2, (const void*)(hier + floatOff), 4u * (size_t)nF);
            else          memset(pF2, 0, 4u * (size_t)nF);
            memset((uint8_t*)pF2 + 4u * (size_t)nF, 0, 16u);
        }
        memset(pMc2, 0x00, cbM);
        n->b.cap.pJoints = pJ2; n->b.cap.pFloats = pF2; n->b.cap.pMasks = pMc2; n->b.cap.id = id;
        n->b.rst.pJoints = pJ2; n->b.rst.pFloats = pF2; n->b.rst.pMasks = pMc2; n->b.rst.id = id;

        _InterlockedExchangePointer((void* volatile*)&rep.snapNode, n);
        TO1_LOG("[snap] slot%d node built: hierId=%08X joints=%u float=%u vec=%u (skel+0x38 cache=%08X)\n",
            s, id, nJ, nF, nV, Rd<uint32_t>(skel + 0x38));
    }
}

// Peer/session lifecycle changed: start a fresh dictionary on both sides. A crashed
// relay peer that re-registers into the same slot presents the same virtual address,
// so the reset must be driven from the session layer. FULL-TEARDOWN tier (sole
// caller: session LeaveInternal): TX re-define + every sender's rx dictionary +
// replay banks + the rx sequence baselines.
void NetAnimDictReset()
{
    animcodec::TxDictReset();
    for (int s = 0; s < kMaxNetPlayers; ++s) {
        s_replay[s].ClearDict();
        s_replay[s].ClearBanks();
        s_replay[s].ResetSeq();
    }
}

// The per-session residuals that no reset tier owns. Called from session teardown
// (LeaveInternal) at the same point NetAnimDictReset runs.
//  - fxPrev/N: deferred (kAnimClearFxCache) -- consumed before the
//    next replay on each slot, so a slot handed to a new peer can never fire a
//    phase-delta from the previous occupant's clip set.
//  - g_localSkel: stale ArtItemSkeleton* window across level loads until the next
//    local capture re-publishes it. Zero is the safe direction (every reader
//    validates/bails).
void NetAnimResetSessionResiduals()
{
    for (int s = 0; s < kMaxNetPlayers; ++s)
        s_replay[s].pendingClear.fetch_or(kAnimClearFxCache, std::memory_order_release);
    g_localSkel = 0;
}

// PEER-DROP tier (sole caller: session DropPeer): clear ONE sender's replay bank +
// learned rx dictionary + rx sequence baseline (a peer that rejoins into this slot
// restarts its sequence; a wholesale reset would freeze co-present converged
// senders). Leaves every other sender untouched.
void NetAnimClearSlot(int slot)
{
    if (slot < 0 || slot >= kMaxNetPlayers) return;
    s_replay[slot].ClearDict();
    s_replay[slot].ClearBanks();
    s_replay[slot].ResetSeq();
}

uint32_t NetAnimRxStampMs(int slot)
{
    if (slot < 0 || slot >= kMaxNetPlayers)
        return 0;
    // Aligned 32-bit volatile read: the writer uses _InterlockedExchange, but a torn
    // stamp is harmless here (we only compare it for "changed" once per reap check).
    return static_cast<uint32_t>(s_replay[slot].rxStampMs);
}

// RECEIVER: publish a received list for the puppet replay hook to consume.
void NetSetRemoteAnimCmds(int senderSlot, const uint8_t* data, uint16_t bits, uint32_t skelId, uint8_t epoch,
    uint32_t sendMs)
{
    if (senderSlot < 0 || senderSlot >= kMaxNetPlayers) return;              // stray author slot
    AnimReplay& rep = s_replay[senderSlot];
    if (!g_animSyncEnabled) { rep.ClearBanks(); return; }                    // kill switch
    if (!data || bits == 0) return;                                          // keep last good

    // Unpack to the dword list the replay path expects. A decode failure (including a
    // dictionary index we never learned) rejects the packet, leaving the puppet on its
    // last good pose -- spatially coherent beats temporally fresh.
    static uint32_t s_decoded[kMaxAnimCmdDwords];
    const uint32_t len = NetAnimDecode(senderSlot, data, bits, epoch, s_decoded, kMaxAnimCmdDwords);
    if (len == 0) return;
    const uint32_t* cmd = s_decoded;
    if (len > (uint32_t)kMaxAnimCmdDwords) return;

    // Rig gate: clips and feather masks are both resolved against OUR local player's
    // skeleton. If the sender animated a different rig, the engine would soft-fail
    // each clip to an empty pose -- the exact input that poisons a one-sided blend.
    uintptr_t skel = (uintptr_t)g_localSkel;
    if (!San(skel) || skelId == 0 || Rd<uint32_t>(skel + 52) != skelId) return;
    if (!AnimListValid(cmd, len)) return;

    // Translate each feather blend's transmitted StringId64 into a LOCAL heap index,
    // HERE on the game thread: LoginFeatherBlend takes the table lock and allocates
    // from the FixedSizeHeap, so it must never run on the async anim job. After this
    // the anim thread only ever sees an index valid in THIS process for THIS rig.
    static uint32_t s_x[kMaxAnimCmdDwords];
    memcpy(s_x, cmd, (size_t)len * 4);
    const uintptr_t fad = (uintptr_t)g_localFgAnimData;
    uint32_t xlen = len;

    for (uint32_t i = 0; i < xlen; ) {
        uint32_t sz = CmdSize(s_x[i]);
        if (sz == 0 || i + sz > xlen) return;
        if (CmdOp(s_x[i]) == OP_FEATHER) {
            uint64_t fbSid = *reinterpret_cast<const uint64_t*>(&s_x[i + 10]);
            uint32_t idx = FeatherSidToIdx(fbSid, fad);
            // The identity check the ENGINE ITSELF never performs: it validates only
            // the heap used-bit and the skelId, so a slot can be "valid" yet hold a
            // DIFFERENT feather blend. That collision returned a perfectly-formed mask
            // over the WRONG joints -- every layer scoped to an arbitrary joint subset,
            // which is exactly "all parts of the body doing their own thing".
            if (FeatherEntryMatches(idx, fbSid, (uintptr_t)g_localSkel)) {
                s_x[i + 10] = idx;                            // local index for the anim job
                s_x[i + 11] = 0;
            }
            else {
                // Cannot name this mask locally -> downgrade IN PLACE to a plain blend
                // over the same slots/weight. Never a hole, never a guessed mask.
                // See the sender-side note: d4 must be the literal 1 for a blend, but
                // holds the feather's MODE. Set it explicitly rather than relying on
                // the 6-dword overlap, which produced a malformed blend.
                s_x[i] = MakeCmd(OP_BLEND, 6);
                s_x[i + 4] = 1u;
                memmove(&s_x[i + 6], &s_x[i + 12], (size_t)(xlen - (i + 12)) * 4);
                xlen -= 6;
                sz = 6;
            }
        }
        i += sz;
    }

    // Re-validate the FINAL form. The feather->blend rewrite above happens after the
    // first AnimListValid, and it changes an opcode and a slot field, so nothing may
    // reach the anim thread that the validator has not seen exactly as it will execute.
    if (!AnimListValid(s_x, xlen)) return;

    // Publish into the INACTIVE bank, then flip. A single shared buffer was torn: the
    // anim thread could read rxCmd part-way through this 4KB copy while the old
    // length was still published, executing a spliced list the validator never saw.
    // Only a fully-valid list replaces the current one; a rejected packet leaves the
    // previous one in place, so the puppet holds its last good pose.
    // Ring meta first: keep this sample on the author's timeline for the interpolator. Drop
    // anything not newer than what we already hold - the sequence gate (NetAnimRxIngest)
    // already rejects reorders, this is the second line of defence and also kills exact
    // duplicates.
    {
        const long newest = rep.ringNewest;
        const uint32_t newestMs = rep.ringMeta[newest].sendMs;
        const bool haveAny = (rep.ringCount > 0);
        if (!(haveAny && (int32_t)(sendMs - newestMs) <= 0)) {
            const long slotIdx = haveAny ? ((newest + 1) % kSampleRing) : 0;
            rep.ringMeta[slotIdx].sendMs = sendMs;
            _InterlockedExchange(&rep.ringMeta[slotIdx].len, (long)xlen);
            _InterlockedExchange(&rep.ringNewest, slotIdx);
            if (rep.ringCount < kSampleRing)
                _InterlockedIncrement(&rep.ringCount);
        }
    }

    const long bank = 1 - rep.rxBank;
    memcpy(rep.rxCmd[bank], s_x, (size_t)xlen * 4);
    _InterlockedExchange(&rep.rxLenB[bank], (long)xlen);
    _InterlockedExchange(&rep.rxBank, bank);
    _InterlockedIncrement(&rep.rxGen);                    // seqlock tick: a mid-copy reader detects the lap
    // Freshness stamp for the anim thread's staleness ladder. Only an ACCEPTED list counts --
    // a rejected packet must not look like liveness, or a peer whose every packet is
    // undecodable would never trip the watchdog. That was the freeze.
    _InterlockedExchange(&rep.rxStampMs, g_nowMs);
}

// ============================================================================
//  RECEIVER: drive each remote NPC puppet (per author slot, kMaxNetPlayers) from its replicated anim.
//  The hook replays the full inner AnimCmdList (all layers, exactly like the local
//  swap). There is NO base-clip fallback -- animate_ would start a real anim state
//  whose baked root motion drags the puppet against the replicated locator. Aim
//  (spine) is copied post-anim, exactly as it was locally.
// ============================================================================
void NetApplyRemoteAnim(int slot, uint64_t npcSid, const float* spine20)
{
    if (slot < 0 || slot >= kMaxNetPlayers) return;
    uint64_t obj = npcSid ? DcManager::LookupObject(npcSid) : 0;
    if (!obj) {
        // TRANSIENT miss, NOT a real despawn -- so tear down NOTHING and keep the last-good drive.
        // This function's only caller (ApplyRemote, net-replication.cpp) already passed the
        // AUTHORITATIVE gate ENTITY::DOES_EXIST(npcSid)==true this same frame; per the note there,
        // DOES_EXIST is the reliable existence check while DcManager::LookupObject can momentarily
        // return 0 under this puppet's per-frame registry churn (the bound-frame locator alloc,
        // SET_HEALTH, weapon/skeleton traffic). Zeroing npcAC here would make the anim
        // thread's (a1 == npcAC) test fail for exactly ONE frame -- so the puppet runs its own idle
        // anim, which is the random single-frame flash. Just skip this frame's refresh; npcAC /
        // the banks stay valid so the replay keeps running uninterrupted.
        //   A REAL despawn never reaches here: ApplyRemote's DOES_EXIST==false -> NetClearRemoteAnim
        // handles it first, and the per-peer clip dictionary is reset on lifecycle events by
        // NetAnimDictReset (driven from net-session). Nothing to reset in this transient path.
        return;
    }

    // publish the NPC's AnimControl so the hook recognises the puppet's own call.
    // (the replay list itself arrives via NetSetRemoteAnimCmds, its own packet.)
    // There is deliberately NO base-clip fallback: animate_-ing here would start a
    // real anim state whose baked root motion translates the puppet and fights the
    // replicated locator every frame. No list -> puppet simply holds its pose.
    //   THE 1-FRAME "returns to native" FLASH LIVES HERE. GO+0x1B8 is read from the SAME object during
    // the SAME per-frame registry churn that makes LookupObject return 0 above -- so it too can come
    // back 0 (the AnimControl slot is momentarily cleared during a skeleton/weapon rebind, or SafeRW
    // faults for one frame; Rd<> returns 0 on either). Storing 0 into npcAC makes the anim thread's
    // (a1 == npcAC) match miss for exactly ONE frame -> rxSlot stays -1 -> the hook runs the puppet's
    // ORIGINAL CreateAnimCmds -> its own native NPC idle, whole body, one frame. Identical to the
    // LookupObject guard above. RULE: on a bad read keep the last-good npcAC, NEVER zero the key.
    uintptr_t ac = Rd<uintptr_t>((uintptr_t)obj + GO_ANIMCONTROL);
    if (!San(ac)) return;                          // transient churn read -> hold last-good AC
    AnimReplay& rep = s_replay[slot];
    // ONE match key per puppet: npcAC serves BOTH hk_CreateAnimCmds and hk_PostAnim.
    rep.npcAC = ac;
    rep.npcObj = obj;   // publish AFTER the AC guard above: the two are read as a pair

    // stash the spine pose for the post-anim hook (runs after the NPC's own eval).
    // SEQLOCK: the 80-byte spine array is written here on the game thread and read by
    // hk_PostAnim on the post-anim job, so an unguarded read hands the puppet a half-updated
    // quat set. gen is odd while the write is in flight; the reader retries/skips on a gen
    // mismatch. (fetch_add is a full barrier on x64; the data stores cannot hoist above the
    // first bump or sink below the second.)
    rep.spineGen.fetch_add(1, std::memory_order_acq_rel);       // odd: write open
    for (int i = 0; i < 20; ++i) rep.rxSpine[i] = spine20[i];
    rep.rxSpineValid = true;
    rep.spineGen.fetch_add(1, std::memory_order_release);       // even: write closed
}

// GAME THREAD. Publish one puppet's replicated weapon-IK feather fade (0..1). Plain
// interlocked store of the float's bits: the anim thread reads it with a single load
// and any value it sees is one this function actually wrote, which is all the IK
// weight needs -- a one-frame-stale fade is invisible, a torn one would not be.
void NetSetPuppetWeaponIk(int slot, float fade)
{
    if (slot < 0 || slot >= kMaxNetPlayers) return;
    if (!(fade > 0.0f)) fade = 0.0f;                 // also normalises NaN
    if (fade > 1.0f)    fade = 1.0f;
    long bits;
    memcpy(&bits, &fade, sizeof(bits));
    _InterlockedExchange(&s_replay[slot].wpnIkBits, bits);
}

void NetFreezePuppetOverlays(int slot)
{
    if (slot < 0 || slot >= kMaxNetPlayers) return;
    s_replay[slot].rxSpineValid = false;                  // hk_PostAnim's match key
    _InterlockedExchange(&s_replay[slot].wpnIkBits, 0);   // no arm solve on a corpse
}

void NetClearRemoteAnim(int slot, bool dropLastGood)
{
    if (slot < 0 || slot >= kMaxNetPlayers) return;
    s_replay[slot].Detach(dropLastGood);           // clear-then-kill contract documented at Detach
}

void NetClearRemoteAnimAll(bool dropLastGood)
{
    for (int s = 0; s < kMaxNetPlayers; ++s) NetClearRemoteAnim(s, dropLastGood);
}