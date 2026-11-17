// ============================================================================
//  anim-hooks.cpp -- the two engine anim hooks (hk_CreateAnimCmds and
//  hk_PostAnim), NetPuppetAnimInstall, NetAnimTickEarly, and the frame clock
//  the anim thread reads.
//
//  Both hook bodies run OFF the game thread (the async anim job and the
//  post-anim job). The hooks are installed once and NEVER removed: an inline
//  hook cannot be torn down while a job may still be inside it.
// ============================================================================
#include "anim.hpp"
#include "anim-internal.hpp"
#include "net-debug.hpp"
#include <safetyhook.hpp>
#include <intrin.h>            // _InterlockedExchange / _ReadWriteBarrier

// ============================================================================
//  Shared CreateAnimCmds hook. One machine is BOTH a sender (its own local
//  player -> capture the list to ship) and a receiver (the remote's NPC puppet
//  -> replay the received list). The two AnimControls are distinct objects, so
//  a pointer compare on `this` decides which role this call plays. Every other
//  character's CreateAnimCmds runs untouched.
// ============================================================================
// >>> KILL SWITCH: false = the whole AnimCmdList pipeline is inert (no hooks
// installed, nothing captured, sent, replayed, or written). Set false to take the
// whole feature out of the picture (e.g. to attribute a crash to it or rule it out).
bool g_animSyncEnabled = true;

static SafetyHookInline g_hkCreateAnimCmds;

// Frame clock for the anim thread's staleness ladder, published once per frame
// by NetAnimTickEarly below. A plain monotonic MILLISECOND counter, NOT
// GetTickCount64 called from the anim job -- the anim path must stay
// syscall-free; it only ever reads this long. Truncated to 31 bits so the
// subtraction in the hook stays well-defined for ~24 days of uptime.
volatile long g_nowMs = 0;

// The game thread's id, stamped by NetAnimTickEarly -- backs the thread assert
// on AnimReplay::Detach (drive-pointer clears must be synchronous game-thread
// writes that precede the puppet kill).
DWORD g_gameThreadId = 0;

// How long a puppet may hold its last pose before we stop pretending the stream is alive.
// Holding covers ordinary loss (several dropped packets) and is invisible. Past kStaleMs the
// stream is gone: we RELEASE the puppet to the engine's own CreateAnimCmds so it plays its
// native idle. A breathing idle at a replicated position is not "masking a failure" -- it is the
// only non-broken thing to show, and the RELEASED log line makes it far louder than a silent
// statue would be. Hold-vs-stale is this single threshold; there is no intermediate tier.
static constexpr long kAnimStaleMs = 1500;

// See NetSetSlotStreamAlive in anim.hpp. Plain volatile long: one game-thread writer, one
// anim-thread reader, single machine word -- no barrier or interlock is required for a
// value that is only ever a hint about whether to release.
static volatile long g_slotStreamAlive[kMaxNetPlayers] = {};

void NetSetSlotStreamAlive(int slot, bool alive)
{
    if (slot < 0 || slot >= kMaxNetPlayers) return;
    g_slotStreamAlive[slot] = alive ? 1 : 0;
}

static bool SlotStreamAlive(int slot)
{
    return slot >= 0 && slot < kMaxNetPlayers && g_slotStreamAlive[slot] != 0;
}
// Consecutive accepted frames required before a released puppet is re-latched, so a stream
// hovering at the boundary cannot flip between replay and native every frame.
static constexpr long kAnimReacquireFrames = 3;

// The ONLY thing the puppet ever gets besides a replayed list: a minimal, valid
// bind-pose list ({header count=1} + {empty-pose -> slot 0} + {terminator}).
// This is deliberately NOT a second anim-sync method -- it is the "nothing" case.
// Letting the original run instead would hand the puppet its own native NPC anims,
// which is a silent fallback that both masks failures and (because the NPC then
// animates itself) drags it around on root motion.
static uintptr_t EmitBindPoseList(uintptr_t a3) {
    uintptr_t buf = Rd<uintptr_t>(a3);
    uint32_t  cursorIn = Rd<uint32_t>(a3 + 12);
    if (!San(buf)) return 0;

    uint32_t out[6];
    out[0] = MakeCmd(OP_HEADER, 2); out[1] = 1;   // exactly one pose slot
    out[2] = MakeCmd(OP_EMPTY,  2); out[3] = 0;   // real producer -> slot 0, always finite
    out[4] = MakeCmd(OP_TERM,   2); out[5] = 0;   // terminator last; publishes slot 0
    if (!SafeRW(buf + 4ull * cursorIn, out, sizeof(out), true)) return 0;

    Wr<uint32_t>(a3 + 12, cursorIn + 6);
    Wr<uintptr_t>(a3 + 16, 0);
    Wr<uint32_t>(a3 + 24, 0);
    return buf + 4ull * (cursorIn + 1);
}

static uintptr_t __fastcall hk_CreateAnimCmds(uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    if (!g_animSyncEnabled)                                      // kill switch: pure pass-through
        return g_hkCreateAnimCmds.call<uintptr_t>(a1, a2, a3);

    // RECEIVER: replay the remote list onto the NPC puppet. THIS OR NOTHING --
    // there is no fallback to any other anim-sync method. Find WHICH remote slot's puppet this
    // AnimControl is (a 4-element search on the anim thread is negligible); each has its own list.
    int rxSlot = -1;
    if (a1) for (int s = 0; s < kMaxNetPlayers; ++s) if (a1 == s_replay[s].npcAC) { rxSlot = s; break; }
    if (rxSlot >= 0) {
        AnimReplay& rep = s_replay[rxSlot];
        // Apply any clears the game thread deferred to us -- HERE, before any read of the
        // anim-owned fields (last-good, interp stores, reacquire), so a cleared slot can never
        // replay stale state and the game thread never writes these fields at all.
        rep.ConsumePendingClear();
        // Open the off-hand grip gate. HERE, not in a post-anim hook: the opcode-26 command that
        // consumes it is emitted immediately after this returns, so there is no frame lag, and this
        // lands LATER in the frame than the modifier Update that zeroes the weight.
        OpenPuppetGripGate(a2, rxSlot);
        // Copy the latched bank into a reader-private buffer, then confirm no writer lapped us
        // mid-copy (seqlock: rxGen unchanged across the copy). Only a whole, untorn snapshot reaches
        // ReplayOntoNpc -- a spliced list would int3 the executor. This closes the 2-bank double-flip
        // race (the receive pump can drain 2+ anim packets for one sender in a single frame).
        const long gen0 = rep.rxGen;
        const long bank = rep.rxBank;
        long len = (bank == 0 || bank == 1) ? rep.rxLenB[bank] : 0;
        if (len > 0 && len <= (long)kMaxAnimCmdDwords) {
            memcpy(rep.rxScratch, rep.rxCmd[bank], (size_t)len * 4);
            _ReadWriteBarrier();                                   // finish the copy before re-reading gen
            if (rep.rxGen == gen0) {                               // no writer lapped us -> copy is untorn
                uintptr_t hdr = ReplayOntoNpc(a1, a2, a3, rep.rxScratch, (int)len, rxSlot);
                if (hdr) {
                    memcpy(rep.rxLast, rep.rxScratch, (size_t)len * 4);   // promote: last-good
                    rep.rxLastLen = len;
                    // Re-acquire hysteresis: a released puppet needs several consecutive good
                    // frames before we take it back, so a stream sitting on the staleness
                    // boundary cannot pop between replay and native every frame. While we are
                    // still counting, fall through to the ENGINE's own CreateAnimCmds (see the
                    // bottom of this function) -- we must never return 0 here, the caller
                    // dereferences the returned header-count pointer.
                    if (rep.rxReleased) {
                        if (++rep.rxReacquire < kAnimReacquireFrames)
                            return g_hkCreateAnimCmds.call<uintptr_t>(a1, a2, a3);
                        _InterlockedExchange(&rep.rxReleased, 0);
                        rep.rxReacquire = 0;
                    }
                    return hdr;                                    // replayed; skip original
                }
            }
            // else: writer lapped mid-copy -> the copy may be torn -> fall through to HOLD LAST-GOOD.
        }
        // HOLD LAST-GOOD (ND's underrun rule): re-emit the last cleanly-replayed list rather than a
        // bind pose. Holding the previous pose for one frame is invisible; the bind-pose snap was the
        // flicker. Only BEFORE the very first packet (no last-good yet) do we fall to the bind pose.
        //
        // BUT BOUNDED. Holding is correct for lost packets and catastrophic for a dead stream: with
        // no age check this branch rendered the same pose for the rest of the match. The ladder:
        //   age <  kAnimStaleMs -> hold (invisible for ordinary loss, degraded past ~250 ms but
        //                          still better than a snap)
        //   age >= kAnimStaleMs -> RELEASE: fall through to the engine's own CreateAnimCmds so the
        //                          NPC plays its native idle. Position is still replicated by
        //                          ApplyRemote, so it stays put rather than wandering off.
        //   ...AND the slot itself is gone. While the owner is still sending STATE packets the
        //   anim stream is merely stuttering, and releasing would hand the puppet to its native
        //   anim-state machine (which npc-set-script-mode does NOT stop) -- it would start
        //   idling and looking around mid-match. Hold last-good however long that takes; only a
        //   really dead slot gets released.
        const long ageMs = g_nowMs - rep.rxStampMs;
        if (rep.rxStampMs != 0 && ageMs >= kAnimStaleMs && !SlotStreamAlive(rxSlot)) {
            if (!rep.rxReleased) {
                _InterlockedExchange(&rep.rxReleased, 1);
                rep.rxReacquire = 0;
                TO1_LOG("[anim] slot%d RELEASED to native anims (no accepted list for %ldms)\n",
                        rxSlot, ageMs);
            }
            // Run the ENGINE's own CreateAnimCmds. NOT `return 0` -- the caller dereferences the
            // returned header-count pointer. A puppet is never the local player, so the sender
            // capture at the tail of this function would be a no-op for it anyway.
            return g_hkCreateAnimCmds.call<uintptr_t>(a1, a2, a3);
        }
        if (rep.rxLastLen > 0) {
            uintptr_t hdr = ReplayOntoNpc(a1, a2, a3, rep.rxLast, (int)rep.rxLastLen, rxSlot);
            if (hdr) return hdr;
        }
        // No usable list AND no last-good yet (pre-first-packet): bind pose. Visibly nothing rather
        // than quietly doing something else.
        if (uintptr_t hdr = EmitBindPoseList(a3)) return hdr;
        // Only if we cannot even write the command buffer do we defer to the engine.
    }

    // SENDER: run the original, then snapshot the local player's inner list.
    bool isLocal = (a1 && a1 == g_localAC);
    if (isLocal) {
        // the player's own ArtItemSkeleton, for skeleton-matched feather masks on replay
        g_localSkel = Rd<uintptr_t>(a2);
    }
    uint32_t cursorIn = isLocal ? Rd<uint32_t>(a3 + 12) : 0;
    uintptr_t r = g_hkCreateAnimCmds.call<uintptr_t>(a1, a2, a3);
    if (isLocal)
        CaptureLocalList(a3, cursorIn, Rd<uint32_t>(a3 + 12));
    return r;
}

static SafetyHookInline g_hkPostAnim;
static uintptr_t __fastcall hk_PostAnim(uintptr_t obj) {
    uintptr_t r = g_hkPostAnim.call<uintptr_t>(obj);          // engine post-anim runs first
    if (obj && g_animSyncEnabled) {
        uintptr_t ac = Rd<uintptr_t>(obj + GO_ANIMCONTROL);
        int slot = -1;                                        // which remote puppet is this?
        if (ac) for (int s = 0; s < kMaxNetPlayers; ++s)      // npcAC = the ONE match key
            if (s_replay[s].rxSpineValid && ac == s_replay[s].npcAC) { slot = s; break; }
        if (slot >= 0) {
            AnimReplay& rep = s_replay[slot];
            // Take an untorn copy of the spine under its seqlock. On a torn read (the game
            // thread is mid-update), retry once, then SKIP this frame's spine write -- the
            // engine's own pose stands for one frame, which is invisible. An unguarded read
            // would hand the puppet a half-updated quat set.
            float q20[20];
            bool  got = false;
            for (int t = 0; t < 2 && !got; ++t) {
                const uint32_t g0 = rep.spineGen.load(std::memory_order_acquire);
                if (g0 & 1u) continue;                        // write in flight
                memcpy(q20, (const void*)rep.rxSpine, sizeof(q20));
                std::atomic_thread_fence(std::memory_order_acquire);
                got = (rep.spineGen.load(std::memory_order_relaxed) == g0);
            }
            if (got) {
                uintptr_t ad  = Rd<uintptr_t>(obj + 0x1C0);
                uintptr_t arr = San(ad) ? Rd<uintptr_t>(ad + 0xE0) : 0;
                if (San(arr))
                    for (int idx = 1; idx <= 5; ++idx) {      // write spinea..neck local quats
                        float q[4]; for (int k = 0; k < 4; ++k) q[k] = q20[(idx - 1) * 4 + k];
                        SafeRW(arr + 48ull * idx + 16, q, 16, true);
                    }
            }
        }
    }
    return r;
}

// Install both receiver/sender anim hooks once (idempotent). Call from NetTick.
void NetPuppetAnimInstall() {
    static bool s_installed = false;
    if (!g_animSyncEnabled) return;   // kill switch: don't install the hooks at all
    if (s_installed) return;
    g_hkPostAnim = safetyhook::create_inline(
        reinterpret_cast<void*>(g_base + RVA_POSTANIM), reinterpret_cast<void*>(hk_PostAnim));
    g_hkCreateAnimCmds = safetyhook::create_inline(
        reinterpret_cast<void*>(g_base + RVA_CREATEANIMCMDS), reinterpret_cast<void*>(hk_CreateAnimCmds));
    s_installed = true;
    TO1_LOG("[npc-anim] CreateAnimCmds + post-anim hooks installed\n");
}

// The anim engine's early frame steps, in order. Called FIRST by the driver
// (NetTick, match/match.cpp), before menu/matchmaking/session work.
void NetAnimTickEarly()
{
    // Publish the frame clock the anim thread uses for its staleness ladder (see the
    // definition of g_nowMs above: the anim job must never make the syscall itself).
    _InterlockedExchange(&g_nowMs, (long)(GetTickCount64() & 0x7FFFFFFFull));
    if (!g_gameThreadId) g_gameThreadId = GetCurrentThreadId();   // Detach thread-assert baseline

    NetSnapNodeService();        // build any pose-snapshot node the anim thread asked for

    // NPC puppet anim sync -- install the hooks once. The actual capture/apply is
    // driven from net::replication (sender -> NetCaptureLocalAnim, receiver ->
    // NetApplyRemoteAnim).
    NetPuppetAnimInstall();
}
