// ============================================================================
//  anim-tx.cpp -- TX capture side: local-player capture state, the portable
//  template build, the tx clip dictionary/epoch, and NetCaptureLocalAnim /
//  NetGetLocalAnimCmds / NetAnimTxReset.
//
//  CaptureLocalList and TxPublish run on the ANIM THREAD; the packet path
//  (NetGetLocalAnimCmds, the tx dictionary) runs on the GAME thread. The two
//  meet only through the g_txGen seqlock.
// ============================================================================
#include "anim.hpp"
#include "anim-internal.hpp"

static const uint64_t SID_base = 0x9A7CE19BAA54C278ull; // SID("base")
static uint64_t GetLayer(uint64_t obj, uint64_t nameSID) {
    uintptr_t go = (uintptr_t)obj; if (!San(go)) return 0;
    uintptr_t ac = Rd<uintptr_t>(go + GO_ANIMCONTROL); if (!San(ac)) return 0;
    uint32_t n = Rd<uint32_t>(ac + AC_NUMLAYERS); uintptr_t ent = Rd<uintptr_t>(ac + AC_LAYERARR);
    if (n > 64 || !San(ent)) return 0;
    for (uint32_t i = 0; i < n; i++) {
        uintptr_t e = ent + (uintptr_t)i * LE_STRIDE;
        if (!Rd<uint8_t>(e + LE_VALID)) continue;
        if (Rd<uint64_t>(e + LE_NAME) != nameSID) continue;
        uintptr_t L = Rd<uintptr_t>(e + LE_PTR); return San(L) ? L : 0;
    }
    return 0;
}

volatile uintptr_t g_localAC = 0; // local player's AnimControl (capture source)
volatile uintptr_t g_localAnimTable = 0; // local player's base-layer AnimTable* (skeleton-matched resolve)
volatile uintptr_t g_localSkel = 0; // local player's ArtItemSkeleton* (skeleton-matched feather masks)
volatile uintptr_t g_localFgAnimData = 0; // local player's FgAnimData* (LoginFeatherBlend needs it)

// Captured local-player inner list (written anim thread, read game thread) -- SENDER, single
// instance, so it is a file global rather than a per-slot AnimReplay row.
//
// SEQLOCK, not an ABA-prone length compare: a "g_txLen still == n after the memcpy" tear check
// misses two consecutive captures of the SAME length lapping the copy, so a torn (half-old/
// half-new) list gets encoded and sent -- and a plausible splice can pass the receiver's
// AnimListValid, i.e. a WRONG pose, not a dropped frame. The writer brackets every publish
// with g_txGen++ (odd = in progress); the reader retries whenever the gen was odd or changed
// across its copy. fetch_add is a full barrier on x64 (lock xadd), so the data stores cannot
// cross the brackets.
static uint32_t g_txCmd[kMaxAnimCmdDwords];
static volatile long g_txLen = 0;               // dwords; 0 = unavailable this frame
static std::atomic<uint32_t> g_txGen{ 0 };        // seqlock generation (even = stable)

// ANIM THREAD: the single publish point for {g_txCmd, g_txLen}. Every path through
// CaptureLocalList funnels here (including the failure paths that publish len 0) so the
// seqlock bracket can never be forgotten at one site.
static void TxPublish(const uint32_t* list, long n)
{
    g_txGen.fetch_add(1, std::memory_order_acq_rel);        // odd: publish in progress
    if (n > 0) memcpy(g_txCmd, list, (size_t)n * 4);
    g_txLen = n;
    g_txGen.fetch_add(1, std::memory_order_release);        // even: publish complete
}

// ArtItemAnim layout (confirmed in the anim-group login path, sub_142910FE0):
//   +0x18  u64 nameId (StringId64)   +0x50  u32 skelId
// The AnimTable is a hash keyed by (0x100000001B3 * nameId) ^ skelId, built from
// exactly those two fields -- so the nameId is PRECOMPUTED on the anim and is the
// literal lookup key. Never re-derive it by hashing the anim's name string.
enum : uintptr_t { ANIM_NAMEID = 0x18, ANIM_SKELID = 0x50 };

// SENDER: local feather-blend slot index -> portable StringId64 name. 0 = the index
// is not a valid local entry (then we must not ship it).
static uint64_t FeatherIdxToSid(uint32_t idx) {
    if (idx == FB_INVALID) return 0;
    using Fn = void(__fastcall*)(uintptr_t tbl, uint64_t* outSid, uint32_t idx);
    Fn fn = reinterpret_cast<Fn>(g_base + RVA_FB_GETID);
    uint64_t sid = 0;
    __try { fn(g_base + OFF_FEATHERTABLE, &sid, idx); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return sid;
}

// AnimOverlay resolve: given an ArtItemAnim*, return the anim that should actually play once the
// character's overlay set is applied, or 0 if there is no override. ND calls this on the capture
// side before reading the anim's identity (0x1409E27E1).
//
// SEH-guarded and behind a live toggle because it walks a system list (GetSystem(21) ->
// sub_1414C2550 -> sub_14185B230) from the ANIM THREAD, where ND calls it from the game thread --
// so "ND does it here" is not a thread-safety argument for us. Default OFF; flip and measure.
using OverlayRemap_t = uintptr_t(__fastcall*)(uintptr_t);

static uintptr_t OverlayRemap(uintptr_t anim) {
    uintptr_t r = 0;
    __try { r = reinterpret_cast<OverlayRemap_t>(g_base + 0x15BA320)(anim); }
    __except (EXCEPTION_EXECUTE_HANDLER) { r = 0; }
    return r;
}

// Live toggle for the overlay remap above, so it can be A/B'd in one session
// without a rebuild.
volatile long g_txOverlayRemap = 0;

// SENDER, ANIM THREAD: copy the just-emitted inner list [cursorIn,cursorOut) and
// translate it into a portable template: clip ptr -> clip SID, feather ptrs -> 0
// (the receiver re-derives them). Any unknown opcode -- which may hide a pointer --
// voids the whole frame.
void CaptureLocalList(uintptr_t a3, uint32_t cursorIn, uint32_t cursorOut) {
    if (cursorOut <= cursorIn) { TxPublish(nullptr, 0); return; }
    uint32_t n = cursorOut - cursorIn;
    uintptr_t buf = Rd<uintptr_t>(a3);
    if (n > (uint32_t)kMaxAnimCmdDwords) { TxPublish(nullptr, 0); return; }
    if (!San(buf)) { TxPublish(nullptr, 0); return; }

    uint32_t t[kMaxAnimCmdDwords];
    if (!SafeRW(buf + 4ull * cursorIn, t, (size_t)n * 4, false)) { TxPublish(nullptr, 0); return; }

    // RE-ENCODE (never filter). Build the portable list into `out`, substituting
    // in place where a command isn't portable. Any command we can neither carry nor
    // substitute aborts the WHOLE frame -- a hole would corrupt the slot graph.
    uint32_t out[kMaxAnimCmdDwords];
    uint32_t o = 0;
    uint32_t headers = 0, terms = 0;

#define TX_SKIP() do { TxPublish(nullptr, 0); return; } while (0)

    for (uint32_t i = 0; i < n; ) {
        uint32_t sz = CmdSize(t[i]);
        uint32_t op = CmdOp(t[i]);
        if (sz == 0 || i + sz > n) TX_SKIP();              // malformed capture
        if (o + sz + 6 > (uint32_t)kMaxAnimCmdDwords) TX_SKIP();

        switch (op) {
        case OP_HEADER:
            if (++headers != 1 || o != 0) TX_SKIP();       // exactly one, first
            memcpy(&out[o], &t[i], (size_t)sz * 4); o += sz;   // count copied verbatim
            break;

        case OP_CLIP: {                                        // ptr d2:d3 -> precomputed nameId
            uintptr_t p = (uintptr_t)(*reinterpret_cast<uint64_t*>(&t[i + 2]));
            // ND resolves the ANIM OVERLAY before reading the identity (0x1409E27E1), so their
            // wire carries the post-overlay anim. Shipping the raw pointer's fields instead is
            // the leading suspect for the residual clip-resolve failures: an overlaid clip is
            // registered under the OVERLAY's nameId, so the receiver looks up a key that does not
            // exist and stamps a bind pose into that limb.
            // Keep the original pointer when the remap returns 0 -- exactly ND's rule at 0x1409E27EF.
            if (g_txOverlayRemap && San(p)) {
                uintptr_t r = OverlayRemap(p);
                if (San(r)) p = r;
            }
            uint64_t  nameId = San(p) ? Rd<uint64_t>(p + ANIM_NAMEID) : 0;
            memcpy(&out[o], &t[i], (size_t)sz * 4);
            if (nameId) {
                *reinterpret_cast<uint64_t*>(&out[o + 2]) = nameId;
                // d1 is unused padding in the engine's clip command, so we reuse it to
                // carry the anim's OWN SkeletonId. Anims are registered under
                // (nameId, skelId), and a clip may belong to a different rig than the
                // receiver's base layer (retargeted/gesture sets) -- without this the
                // receiver literally cannot form the lookup key and the clip is lost.
                out[o + 1] = San(p) ? Rd<uint32_t>(p + ANIM_SKELID) : 0;
                o += sz;
            }
            else {
                // unnameable clip -> bind pose IN PLACE, same dest slot (d4).
                out[o] = MakeCmd(OP_EMPTY, 2); out[o + 1] = t[i + 4]; o += 2;
            }
            break;
        }

        case OP_FEATHER: {
            // d10 is a LOCAL heap slot index -- as non-portable as a pointer. Translate
            // it to the feather blend's StringId64 and ship that in d10:d11 (d11 is
            // engine padding, so this costs nothing). If the index isn't a valid local
            // entry we cannot name it: downgrade to a plain blend rather than ship an
            // index the receiver would resolve to some unrelated mask.
            uint64_t fbSid = FeatherIdxToSid(t[i + 10]);
            if (!fbSid) {
                // Downgrade to a plain blend. 0x08 and 0x09 share the engine's handler
                // and agree on d1=srcA, d2=srcB, d3=dst, d5=weight -- but NOT on d4:
                // for a blend that dword must be the literal 1, whereas the feather
                // holds its MODE there. Copying 6 dwords verbatim silently produced a
                // blend with a garbage 'one' field, so write it explicitly.
                memcpy(&out[o], &t[i], 6 * 4);
                out[o] = MakeCmd(OP_BLEND, 6);
                out[o + 4] = 1u;
                o += 6;
                break;
            }
            memcpy(&out[o], &t[i], (size_t)sz * 4);
            *reinterpret_cast<uint64_t*>(&out[o + 6]) = 0;     // counts  d6:d7 (receiver re-derives)
            *reinterpret_cast<uint64_t*>(&out[o + 8]) = 0;     // factors d8:d9 (receiver re-derives)
            *reinterpret_cast<uint64_t*>(&out[o + 10]) = fbSid;// d10:d11 = portable identity
            o += sz;
            break;
        }

        case OP_JOINTPOSE: {                                   // 3 raw joint-pose ptrs
            // ND's own translation: a bind-pose clip at phase 0 into the same slot.
            out[o] = MakeCmd(OP_CLIP, 6);
            out[o + 1] = 0;
            *reinterpret_cast<uint64_t*>(&out[o + 2]) = SID_FALLBACK_CLIP;
            out[o + 4] = t[i + 1];                             // dest slot
            *reinterpret_cast<float*>(&out[o + 5]) = 0.0f;     // phase 0
            o += 6;
            break;
        }

        case OP_20: case OP_21: case OP_22:                    // raw ptrs, dest slot at d1
            out[o] = MakeCmd(OP_EMPTY, 2); out[o + 1] = t[i + 1]; o += 2;
            break;

        case OP_PLUGIN: {                                      // IK / gesture plugins
            // WHITELIST, mirroring ND's own netcode: they ship leg-fix-ik and ONLY leg-fix-ik,
            // asserting on every other plugin SID at both ends. Plugins are pure IN-PLACE
            // modifiers (their producer already wrote the slot), so dropping one leaves no hole
            // in the dataflow graph -- we only lose that effect. The default is therefore DROP.
            // (The per-SID findings behind this whitelist are recorded on the SID constants in
            // anim-internal.hpp.)
            uint64_t sid = *reinterpret_cast<const uint64_t*>(&t[i + 2]);

            const bool carry = (sid == SID_LEGFIXIK) ||
                (kCarryAnkleBlendIk && sid == SID_ANKLEBLENDIK);
            // Everything else is dropped, and two of them MUST be: joint-spring packs its
            // AnimSnapshotNode `this` into d6:d7, and subsystem-ik-node's d8:d9 is a pointer the
            // handler walks to a FUNCTION POINTER and calls -- shipping either is a remote
            // arbitrary-jump primitive, not merely a bad pose.
            // NOT a whitelisted plugin -> dropped. weapon-ik-feather lands here every
            // frame on an NPC, but never on the player (the player route emits no
            // command at all), so this is not the reason puppets lacked grip IK -- the
            // receiver synthesises that instead. See SID_WEAPONIKFTHR in anim-internal.hpp.
            if (!carry) break;

            // Size is authoritative from d0's high half and is NOT always 10 (subsystem-ik-node
            // is 16). Refuse anything off-spec rather than desync the stream.
            // 10 is the size of the two plugins we DO carry, not of plugins in general
            // (weapon-ik-feather is 16). Both whitelisted SIDs are fixed-size, so a
            // mismatch here means a list we do not understand: drop it.
            if (sz != 10) break;

            memcpy(&out[o], &t[i], 10u * 4);
            out[o + 1] = 0;                                    // never written by any emitter
            out[o + 4] = 0;                                    // always 0 in every emit path
            out[o + 5] = 0;
            if (sid == SID_LEGFIXIK) {
                // d8:d9 is an OPTIONAL raw float* to a per-leg weight array. Null is an
                // explicitly supported in-band value meaning "use the default weight" -- it is
                // null-tested then dereferenced (0x14169009D), and ND's own net path plus most
                // of the game's own emitters already pass 0 here. So this is not a degraded
                // mode, it is the common case.
                *reinterpret_cast<uint64_t*>(&out[o + 8]) = 0;
            }
            // d6/d7 (and d9 on ankle-blend-ik) are output-pose SLOT indices in the same index
            // space as OP_BLEND's src/dst. We never renumber slots -- every command is carried
            // verbatim or replaced IN PLACE writing the SAME slot, and the dropped commands
            // (plugins, 0x1B/0x1C/0x1D metadata) allocate none -- so they stay self-consistent
            // and need no rebasing. The receiver still bounds-checks them in AnimListValid.
            o += 10;
            break;
        }

        case OP_MARK_LYR: case OP_MARK_SUB: case OP_MARK_ST:
            break;                                             // STRIP: metadata, never executed

        case OP_TERM:
            if (++terms != 1) TX_SKIP();
            memcpy(&out[o], &t[i], (size_t)sz * 4); o += sz;
            break;

        case OP_BLEND: case OP_FLIP: case OP_MIRROR: case OP_EMPTY: case OP_0F:
            memcpy(&out[o], &t[i], (size_t)sz * 4); o += sz;   // pointer-free
            break;

        default:
            TX_SKIP();                                         // unknown -> never guess
        }
        i += sz;
    }
#undef TX_SKIP

    if (headers != 1 || terms != 1) { TxPublish(nullptr, 0); return; }
    if (CmdOp(out[o - 2]) != OP_TERM) { TxPublish(nullptr, 0); return; }  // 0x04 must be last

    TxPublish(out, (long)o);
}

// ============================================================================
//  SENDER: capture the local player's inner AnimCmdList (via the hook, published
//  in g_txCmd) plus the aim spine. Also publishes the local player's AnimControl
//  and skeleton-matched AnimTable, which BOTH roles need. Called from replication.
// ============================================================================
void NetCaptureLocalAnim(float* spine20)
{
    for (int i = 0; i < 20; ++i) spine20[i] = 0.0f;

    uint64_t pl = DcManager::LookupObject(SID("player"));
    if (!pl) { g_localAC = 0; g_localAnimTable = 0; return; }

    // publish the local player's AnimControl so the hook knows which call to capture,
    // and its base-layer AnimTable, which is the skeleton-matched table the RECEIVER
    // side uses to resolve a remote's clip nameIds (see ReplayOntoNpc).
    g_localAC = Rd<uintptr_t>((uintptr_t)pl + GO_ANIMCONTROL);
    uint64_t plBase = GetLayer(pl, SID_base);
    g_localAnimTable = San((uintptr_t)plBase) ? Rd<uintptr_t>((uintptr_t)plBase + 0x10) : 0;

    uintptr_t ad = Rd<uintptr_t>((uintptr_t)pl + 0x1C0);          // FgAnimData
    // LoginFeatherBlend resolves a feather SID against a rig via FgAnimData; the
    // receiver needs OUR player's (same rig as the sender's) to build local entries.
    g_localFgAnimData = San(ad) ? ad : 0;
    uintptr_t arr = San(ad) ? Rd<uintptr_t>(ad + 0xE0) : 0;        // local-xform array
    if (San(arr))
        for (int idx = 1; idx <= 5; ++idx)                        // spinea..neck local quats
            for (int k = 0; k < 4; ++k)
                spine20[(idx - 1) * 4 + k] = Rd<float>(arr + 48ull * idx + 16 + 4 * k);

}

namespace animcodec {

    // How often every reference re-carries its 64-bit definition. Bounds how long a receiver
    // can stay poisoned after losing a definition burst: worst case one interval of held pose,
    // then it self-heals. Cost is ~64 bits x distinct clips (typically 6-12) twice a second
    // = ~0.2 KB/s per link, and only on those packets -- steady state is unchanged.
    static constexpr uint64_t kAnimKeyframeIntervalMs = 500;

    // ---- sender dictionary -----------------------------------------------------
    static uint64_t g_txKey[DICT_CAP];
    static uint8_t  g_txSends[DICT_CAP];
    static uint32_t g_txCount = 0;
    // Seeded per-process, NOT zero: two peers must not both start at epoch 0, or a
    // restarted sender's indices would be resolved against the receiver's stale table.
    uint8_t  g_txEpoch = (uint8_t)(GetTickCount64() & 0xFF);

    // Keys whose definition rides in the packet currently being built. The budget is
    // charged ONCE PER PACKET and only when that packet is actually transmitted:
    //  - charging per REFERENCE burned all 8 packets of loss-redundancy inside a single
    //    frame (a mask used by 3 feather commands spent 3 of its 8 immediately);
    //  - charging SPECULATIVELY spent budget on frames that were then shipped as raw
    //    dwords, which carry no definitions at all.
    // Either way the receiver could end up permanently unable to resolve an index, which
    // rejects every later packet and freezes the puppet for the rest of the match.
    enum : uint32_t { TXPEND_MAX = 128 };
    static uint32_t g_txPend[TXPEND_MAX];
    static uint32_t g_txPendN = 0;

    // ---- KEYFRAMES: the dictionary repair path ---------------------------------
    // DICT_REPEAT gives each key 8 definitions, but they ride in 8 CONSECUTIVE packets
    // (~133 ms at 60 Hz, ~55 ms at 144 Hz -- the better your frame rate, the thinner the
    // window). One burst loss over that window and the receiver can never resolve that
    // index again: GetRef fails -> the whole packet is rejected -> the rx gen never advances
    // -> the puppet holds its last pose FOR THE REST OF THE MATCH. The only other re-arm is
    // TxDictReset, whose callers are all lifecycle events (the session layer drives
    // NetAnimDictReset on every teardown), so keyframes are the in-stream repair path.
    //
    // ND avoid this by making their dictionary per-PACKET (stateless), which costs them up
    // to 64 bits x 64 refs on EVERY packet. We keep our per-session indices and instead
    // force every reference in one packet to carry its definition, twice a second. Same
    // recovery guarantee, ~3% of the steady-state cost.
    //
    // This is wire-COMPATIBLE with the existing decoder: PutRef already writes a per-
    // reference needDef bit, and GetRef stores on definition and is idempotent on repeats.
    // So a keyframe is just a packet where every bit happens to be 1. No protocol bump.
    static uint64_t g_txKeyframeMs = 0;
    static bool     g_txForceDefs = false;   // set for one packet by NetGetLocalAnimCmds

    void TxDictReset() {
        g_txCount = 0;
        ++g_txEpoch;
        memset(g_txSends, 0, sizeof(g_txSends));
        g_txPendN = 0;
        g_txKeyframeMs = 0;                   // next packet is a keyframe
    }
    void TxPendBegin() { g_txPendN = 0; }
    static void TxPendCommit() {
        for (uint32_t k = 0; k < g_txPendN; ++k) {
            const uint32_t i = g_txPend[k];
            if (i < DICT_CAP && g_txSends[i] < DICT_REPEAT) ++g_txSends[i];
        }
        g_txPendN = 0;
    }

    // Returns index, and whether the definition must ride along this packet.
    static uint32_t TxDictRef(uint64_t key, bool& needDef) {
        uint32_t idx = DICT_LITERAL;
        for (uint32_t i = 0; i < g_txCount; ++i)
            if (g_txKey[i] == key) { idx = i; break; }

        if (idx == DICT_LITERAL) {
            if (g_txCount >= DICT_CAP) { needDef = true; return DICT_LITERAL; }  // full -> literal
            idx = g_txCount++;
            g_txKey[idx] = key;
            g_txSends[idx] = 0;                       // charged by TxPendCommit, not here
        }
        // Already defined earlier in THIS packet -> index-only (the decoder stores on the
        // first definition and is idempotent on repeats).
        for (uint32_t k = 0; k < g_txPendN; ++k)
            if (g_txPend[k] == idx) { needDef = false; return idx; }

        // Keyframe: define EVERY reference regardless of how many times it has been sent.
        // This is what makes a receiver that missed all 8 original copies recoverable.
        if (g_txForceDefs) {
            needDef = true;
            if (g_txPendN < TXPEND_MAX) g_txPend[g_txPendN++] = idx;
            return idx;
        }

        needDef = (g_txSends[idx] < DICT_REPEAT);
        if (needDef && g_txPendN < TXPEND_MAX) g_txPend[g_txPendN++] = idx;
        return idx;
    }

    void PutRef(BitW& w, uint64_t key) {
        bool needDef = false;
        const uint32_t idx = TxDictRef(key, needDef);
        w.put(needDef ? 1u : 0u, 1);
        w.put(idx, 10);
        if (needDef) w.put(key, 64);
    }

} // namespace animcodec

// SENDER: bit-pack the latest captured list into the outgoing packet. Returns BITS
// written (0 = send nothing). Falls back to a raw dword dump if the packer refuses
// the list, so a frame is never lost to the codec -- only made bigger.
uint16_t NetGetLocalAnimCmds(uint8_t* out, uint16_t maxBytes,
    uint32_t* outSkelId, uint8_t* outEpoch)
{
    using namespace animcodec;
    if (outSkelId) *outSkelId = 0;
    if (outEpoch)  *outEpoch = 0;
    if (!g_animSyncEnabled) return 0;                    // kill switch: send no AnimCmd packet

    const uint64_t nowMs = GetTickCount64();

    // Is this packet a keyframe? Decided BEFORE encoding because TxDictRef reads it.
    // If this frame then bails before the encode it stays armed, which is what we want --
    // we still owe a keyframe. It cannot stick on, because the timer is only stamped once
    // a keyframe has actually been encoded and returned.
    g_txForceDefs = (nowMs - g_txKeyframeMs) >= kAnimKeyframeIntervalMs;

    // Snapshot the anim-thread capture buffer into a game-thread-private copy BEFORE encoding.
    // Encoding g_txCmd LIVE and bailing at the tail (return 0 -> send NOTHING) whenever the anim
    // thread re-captures mid-encode loses a large fraction of frames: the encode is slow, so that
    // race fires often and the puppet HOLDS its pose instead of getting a fresh list -- the "anim
    // skips a frame" symptom. (Anim-job load shifts the capture's phase against the encode window,
    // which is why simple/idle anims stall worst.) Snapshotting first shrinks the race window to a
    // fast memcpy and, on a race, RE-snapshots instead of dropping the frame. Capture is once per
    // frame, so this converges in 1-2 tries. The tear check is the g_txGen seqlock, not a length
    // compare: two same-length captures lapping the copy would otherwise ship a spliced list.
    static uint32_t txSnap[kMaxAnimCmdDwords];           // game-thread only; the stable list we encode
    long n;
    for (int tries = 0; ; ++tries) {
        const uint32_t gen0 = g_txGen.load(std::memory_order_acquire);
        if (!(gen0 & 1u)) {                                   // even = no publish in flight
            n = g_txLen;
            if (n <= 0 || n > (long)kMaxAnimCmdDwords) return 0;   // no list this frame / too big
            memcpy(txSnap, g_txCmd, (size_t)n * 4);
            std::atomic_thread_fence(std::memory_order_acquire);   // finish the copy before re-reading gen
            if (g_txGen.load(std::memory_order_relaxed) == gen0)   // untorn, whole snapshot
                break;
        }
        if (tries >= 7) return 0;                             // capture churning absurdly fast this frame (very rare)
    }

    uintptr_t skel = (uintptr_t)g_localSkel;
    if (!San(skel)) return 0;                            // no rig id -> receiver can't gate
    if (outSkelId) *outSkelId = Rd<uint32_t>(skel + 52); // ArtItemSkeleton id

    uint8_t epoch = 0;
    uint32_t bits = NetAnimEncode(txSnap, (uint32_t)n, out, maxBytes, &epoch);
    // txSnap is a stable private copy, so there is no mid-encode race to abort on.

    if (bits == 0) {
        // Packer refused (or ran out of room): ship the raw dwords instead. Same data,
        // old size. Correctness first -- the codec must never cost us a frame. NOTE the
        // raw dump carries NO dictionary definitions, so the pending budget must be
        // discarded, not committed, or the receiver would be charged for definitions
        // it never received and could never resolve those indices again.
        TxPendBegin();
        BitW w(out, maxBytes);
        w.put(0, 1); w.put((uint64_t)n, 16);
        for (long k = 0; k < n; ++k) w.put(txSnap[k], 32);
        if (!w.ok) return 0;
        bits = w.pos;
    }
    if (bits > 0xFFFFu) { TxPendBegin(); return 0; }
    if (outEpoch) *outEpoch = epoch;

    // The packet is going out: NOW charge one definition per key that rode in it.
    TxPendCommit();

    // A keyframe only counts once it has actually been encoded and is being returned for
    // transmission. Stamping it earlier would let a frame that bailed (packer refused,
    // no rig, oversized list) reset the timer without any definitions reaching the wire.
    if (g_txForceDefs) g_txKeyframeMs = nowMs;
    g_txForceDefs = false;

    return (uint16_t)bits;
}

// TX-ONLY re-definition. Bumps our transmit epoch + re-arms every clip to be defined
// again, so ALL receivers re-learn our clips (the epoch change drives RxDictEpoch on
// their side). Use this for INCREMENTAL lifecycle events (a peer joins, a roster grows,
// a keepalive re-Hello) instead of NetAnimDictReset: it must NOT wipe other senders'
// learned rx dictionaries -- a converged co-present sender never re-emits its definitions,
// so a wholesale rx clear would freeze that sender's puppet permanently. A genuinely
// restarted relay peer comes back with a fresh random tx epoch, which RxDictEpoch handles
// per-sender on its first post-restart packet, so no rx clear is needed here.
void NetAnimTxReset()
{
    animcodec::TxDictReset();
}