// ============================================================================
//  anim-fx.cpp -- puppet effect regeneration and the off-hand grip gate.
//
//  Both run on the ANIM THREAD, reached from ReplayOntoNpc / hk_CreateAnimCmds.
//  Muting a puppet's own effects instead of regenerating them is a documented
//  crash -- DO NOT re-add it; the minidump analysis, and the RVAs for the
//  camera/render-settings and weapon-attach probes, are in
//  docs/engine-notes.md.
// ============================================================================
#include "anim.hpp"
#include "anim-internal.hpp"
#include "net-debug.hpp"
#include <intrin.h>            // _InterlockedIncrement (grip log throttle)

// ===========================================================================
//  ANIM-EFFECT REGENERATION  (why clip phase is on the wire at all)
//
//  The puppet's own (wrong, idle-state) effects fire off its native anim state before our
//  pose replacement, and the correct ones are not carried in the AnimCmdList at all. We
//  regenerate the RIGHT ones locally from the clips we already replay: for each clip in the
//  puppet's pose, if its phase advanced this frame, ask the engine to fire every effect-track
//  entry the phase just crossed. No new wire data -- we already ship {clip identity, phase}.
//
//  This is exactly ND's model (NetAnimCmdGenerator::Step 0x1408F8280): they keep a 32-entry list
//  of {SID, ArtItemAnim*, phase} and, per frame, if the SET of playing anims is UNCHANGED, scan
//  each for phase crossings. If the set CHANGED they rebuild the cache and fire NOTHING -- that is
//  what stops a state transition from dumping a whole clip's effects at once, and it is the one
//  rule that must not be skipped.
//
//  Effect scan: sub_14161C1E0(effectArray, anim, prevPhase, newPhase, ..., dirFlag, ..., list, 0)
//  where effectArray = *(u64*)(ArtItemAnim+0x58). Phases are NORMALISED 0..1; ours are absolute
//  frames, so divide by (numFrames-1).
//
//  Known and expected: effects tagged `player-only` are dropped on an NPC, exactly as they are on
//  ND's own NetCharacter. Not a bug.
// ===========================================================================
// ON: puppets fire footsteps/foley from the clips we replay. Kill switch retained because
// this is a live engine call made on the anim thread.
volatile long g_animEffects = 1;

// sub_14161C1E0 - anim effect-track phase-range scan.
// MS x64 assigns arguments by POSITION with a shared int/float index, so a pointer in slot 1 and
// floats in slots 2/3/4 land in exactly RCX / XMM1 / XMM2 / XMM3 - rdx/r8/r9 are skipped, and the
// callee never reads them. Slot layout matched byte-for-byte against ND's own call at 0x1408F85B3.
using EffectScan_t = void(__fastcall*)(
    uintptr_t pEffectAnim,   // RCX        *(u64*)(anim+0x58); null -> callee bails
    float     rangeStart,    // XMM1       prev phase, in effect-track units
    float     rangeEnd,      // XMM2       new phase, same units
    float     clipLen,       // XMM3       length of the phase domain
    uint8_t   forward,       // [rsp+0x20] 1 = phase increasing
    uint8_t   loopFlag,      // [rsp+0x28] anim+0x34 & 1
    uint8_t   tag,           // [rsp+0x30] opaque, copied to record+0x18
    uint8_t   isPrimary,     // [rsp+0x38] -> record+0x19, read by the drain
    float     blend,         // [rsp+0x40] FLOAT. entries with a min-blend fire only if <= this
    float     minBlendOvr,   // [rsp+0x48] FLOAT. MUST be >= 0: negative takes a lazy global-init
    //            branch (0x14161BCE1) - unsynchronised, on the anim thread
    uint64_t  stateSid,      // [rsp+0x50] compared to one SID, never dereferenced; 0 is safe
    uintptr_t pSink,         // [rsp+0x58] THE SINK == the AnimControl. null -> silent bail, so a
    //            declaration short enough to leave this slot unwritten
    //            lets stack garbage decide the outcome.
    uintptr_t pAnimInstance);// [rsp+0x60] optional; null takes the explicit zero-fill branch

static void FireEffectRange(uintptr_t anim, float prevFrames, float curFrames, uintptr_t ac) {
    const uintptr_t clip = Rd<uintptr_t>(anim + 0x38);
    const uintptr_t arr = Rd<uintptr_t>(anim + 0x58);
    if (!San(clip) || !San(arr) || !San(ac)) return;
    if (Rd<int32_t>(arr + 0x10) <= 0) return;              // no entries; callee would bail anyway
    const uint32_t nF = Rd<uint16_t>(clip + 0x22);
    if (nF < 2) return;                                    // ND's (nF-1) divisor is unguarded
    const float spf = Rd<float>(clip + 0x2C);
    if (!(spf > 0.0f) || spf > 1.0f) return;               // implausible seconds-per-frame

    // ND's scale (0x1408F8506/852E/853A): TOTAL = 30 * secondsPerFrame * (numFrames-1), and the
    // phases passed are phase01 * TOTAL. Our d5 is ALREADY in frames, so phase01*TOTAL collapses
    // to frames * 30 * spf. Dividing by (numFrames-1) is the exact inverse of what is wanted,
    // and omitting the length argument leaves XMM3 holding whatever was last in it.
    const float k = 30.0f * spf;
    const float len = k * ((float)nF - 1.0f);
    const float s0 = prevFrames * k;
    const float e0 = curFrames * k;
    if (!(len > 0.0f) || s0 == e0) return;                 // the callee's own first early-out

    const uint32_t cap = Rd<uint32_t>(ac + 0x08);          // AC+0x08 capacity
    const uint32_t n0 = Rd<uint32_t>(ac + 0x0C);          // AC+0x0C count (Step zeroes it)
    if (n0 >= cap) return;                                 // record list full this frame

    __try {
        reinterpret_cast<EffectScan_t>(g_base + 0x161C1E0)(
            arr, s0, e0, len,
            1,                                              // forward
            (uint8_t)(Rd<uint8_t>(anim + 0x34) & 1),        // loopFlag
            0,                                              // tag
            1,                                              // isPrimary
            1.0f,                                           // blend
            0.0f,                                           // minBlendOvr - never negative
            0ull,                                           // stateSid
            ac,                                             // sink
            0ull);                                          // pAnimInstance
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
}

// `cur` is this frame's {sid, anim, phaseFrames} set for one puppet, in emission order.
// ANIM THREAD (called from ReplayOntoNpc after a clean commit); the per-slot phase cache
// lives in AnimReplay (fxPrev/fxPrevN -- anim-thread-owned, cleared via the deferred-clear
// protocol so a slot handed to a new peer can never fire a stale phase-delta).
void RegenEffects(int slot, const TrackedAnim* cur, int n, uintptr_t effectList) {
    if (slot < 0 || slot >= kMaxNetPlayers || !g_animEffects) return;
    if (n > kMaxTrackedAnims) n = kMaxTrackedAnims;
    AnimReplay& rep = s_replay[slot];

    // Same SET as last frame, in the same order? Only then is a phase delta meaningful -- across a
    // state change the "previous" phase belongs to a different clip entirely.
    bool same = (n == rep.fxPrevN);
    for (int i = 0; same && i < n; ++i) same = (cur[i].sid == rep.fxPrev[i].sid);

    if (same) {
        for (int i = 0; i < n; ++i) {
            const float p0 = rep.fxPrev[i].phase, p1 = cur[i].phase;
            if (!(p1 > p0)) continue;                       // no advance (or looped) -> nothing to fire
            bool dup = false;                               // ND skips a repeated SID earlier in the list
            for (int k = 0; k < i && !dup; ++k) dup = (cur[k].sid == cur[i].sid);
            if (dup) continue;
            FireEffectRange(cur[i].anim, p0, p1, effectList);   // phases stay in FRAMES
        }
    }
    // set changed -> re-seed below, fire nothing (ND's one unskippable rule)

    for (int i = 0; i < n; ++i) rep.fxPrev[i] = cur[i];
    rep.fxPrevN = n;
}

// Open the puppet's own NPC weapon-grip IK.
//
// An Npc does not grip via the Player action at all -- it has a "joint modifier" whose opcode-26
// command sub_1417B8B80 emits from FgAnimData+0x250 AFTER CreateAnimCmds returns. That walk reads
// the JOB RECORD, not the AnimControl we swap, so our replay never removed it: the apparatus is
// built at spawn (sub_1404F9FF0 -> sub_1406AA9E0) and dispatched to the NPC grip solver
// (sub_14079DFE0 -> sub_1416868D0 -> sub_141922B30) on the puppet, every frame, already.
//
// The one thing missing is the WEIGHT. The modifier's Update (sub_1406AB1A0) only opens the gate
// after a chain of gameplay-state virtuals a puppet never satisfies, and the solver reads that
// weight and bails on zero (read 0x141922BFB, bail 0x141922C0F). Probed in-game and confirmed:
// w=0.000 every frame, while everything else was ALREADY correct -- flags=0x2A (bit1 set),
// joints=[12,14,16,22,13,15,17,23] (both hands resolved from the puppet's own skeleton),
// feather=1.0 (outside the band, so the main inline grip path runs rather than the extra
// SolveWeaponFeatherIk pass), and B+0x8C0 changing per frame, i.e. the solver genuinely running
// right up to the gate. So we write the gate and nothing else -- the engine owns the rest.
//
// Here, rather than the post-anim hook, because the opcode-26 command is emitted immediately
// after this returns: no frame lag, and later in the frame than the Update that zeroes it.
// Zero allocation and zero puppet-owned state, so nothing here can outlive a Despawn -- which
// matters given three prior crashes from async anim jobs touching torn-down puppet state.

// ---- puppet off-hand grip -------------------------------------------------------------------
// Symptom: the puppet's support hand floats in mid-air instead of sitting on a long gun's foregrip.
// The NPC grip solver sub_141922B30 already runs on the puppet every frame and bails at its last
// gate on a zero weight (141922BFB reads B+0xA0, 141922C0F skips the solve when it is <= FLT_EPSILON).
// Past that gate it does a 3-bone IK on the OFF hand onto the primary hand's prop attachment - it
// moves the ARM to the gun, and is NOT a weapon-placement fix.
//
// Do NOT force B+0xA0 = 1.0 directly. That does drive the solver, but it pins the arm at full
// stretch permanently and drags the off arm onto one-handed weapons -- both symptoms of jamming
// the OUTPUT instead of letting the engine's own mechanism run. See below for what drives it.
// Restore the grip-weight SEED the AI weapon controller keeps zeroing on our puppets.
//
// The engine's own modifier Update sub_1406AB1A0 DOES run on the puppet every frame (driver chain:
// PerformAnimAdataCallback -> character vtbl+0x600 PostAnimBlending -> sub_141D55F00 ->
// sub_141631C70 -> modifier vtbl+0x98). It is not failing to run - it computes 0.0 and writes it.
//
// The target weight is SEEDED from M+0x2C and then only ever narrowed:
//     1406AB246  vmovss xmm7, dword ptr [rdi+2Ch]     ; seed
//     1406AB34C  vminss xmm7, xmm0, xmm7              ; ramp can only reduce it
// (IDA's pseudocode renders that seed as a literal 0.0 - the asm is authoritative.)
//
// M+0x2C is driven by AiWeaponController sub_1406805C0, EDGE-TRIGGERED on want != enabled:
//     140680D64  call [rax+60h]   ; IsEnabled -> al
//     140680D67  cmp  sil, al     ; want ('has weapon controller flag' && GetWeapon())
//     140680D6A  jz   short       ; equal -> leave alone
//     140680D77  call [rax+50h]   ; Enable
//     140680D80  call [rax+80h]   ; slot16: M+0x2C = 1.0
//     140680D88  call [rax+78h]   ; slot15: M+0x2C = 0.0    <-- and does NOT Disable
// A puppet reports want == 0 (it is animation-driven, it has no AI weapon controller state), while
// Start (sub_1406AAB80) already left enabled == 1. So want != enabled is true FOREVER, slot15 fires
// EVERY FRAME, and the seed is pinned at 0. That is exactly the flags=0x2A (enabled) + w=0.000 the
// in-game probe recorded, and it is why the solver always bails.
//
// So we restore the seed and NOTHING else. The engine then owns everything that a forced write
// gets wrong:
//   - the ENGAGE/DISENGAGE test, which is the live distance between the two *_hand_prop_attachment
//     joints (B+0x8C0) against a 0.1 m band at M+0x34/M+0x38. This is also the engine's real
//     one-handed/two-handed discriminator: a long gun brings the hands inside 10 cm, a pistol never
//     does. That is why no weapon-type test of our own is needed - and why widening that
//     band would be a mistake.
//   - the 0.2 s ramp (M+0x24 accumulator over M+0x3C)
//   - the spring-damper with 5.0/20.0 s time constants (M+0x40/M+0x44) that eases the arm on and
//     lets it relax, instead of pinning it at full stretch
//   - the disqualifier chain (scripted anim, stairs, etc.)
//
// Must be written every frame because slot15 re-zeros it every frame; it is one aligned 4-byte
// store to an object we re-derive from the puppet each time, so there is nothing cached and nothing
// puppet-owned that could outlive a Despawn.
static volatile long g_dGripSeed = 0;    // live: throttles the [grip] log line below
static volatile long g_gripDiag = 0;    // 1 = log the engine's working values once/sec/slot
volatile long g_gripGateOn = 1;          // kill switch (matches the other g_* toggles)

void OpenPuppetGripGate(uintptr_t /*a2*/, int slot)
{
    if (!g_gripGateOn) return;
    const uintptr_t npc = (slot >= 0 && slot < kMaxNetPlayers) ? (uintptr_t)s_replay[slot].npcObj : 0;
    if (!San(npc)) return;
    const uintptr_t vt = Rd<uintptr_t>(npc);
    if (!San(vt)) return;
    const uintptr_t fn = Rd<uintptr_t>(vt + 0x540);        // GetJointModifiers
    if (!San(fn)) return;

    uintptr_t C = 0;                                        // the modifier container
    __try { C = reinterpret_cast<uintptr_t(__fastcall*)(uintptr_t)>(fn)(npc); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!San(C)) return;

    const uintptr_t M = Rd<uintptr_t>(C + 16);              // index 2 (sub_1416319E0(C,2))
    if (!San(M)) return;
    // Identify it by its Update slot rather than by index: vtbl+0x98 == sub_1406AB1A0 is unique to
    // the weapon modifier, so we can never write into a different modifier that moved into slot 2.
    const uintptr_t mvt = Rd<uintptr_t>(M);
    if (!San(mvt)) return;
    if (Rd<uintptr_t>(mvt + 0x98) != g_base + 0x6AB1A0) return;

    if (Rd<float>(M + 0x2C) < 1.0f) {
        Wr<float>(M + 0x2C, 1.0f);                          // restore the seed slot15 keeps clearing
        const long n = _InterlockedIncrement(&g_dGripSeed);
        if (n == 1 || (n % 600) == 0)
            TO1_LOG("[grip] slot%d re-seeded grip weight (M+0x2C 0 -> 1) (%ld)\n", slot, n);
    }

    // If the arm still never engages, these say which half is at fault: seed vs the distance ramp.
    // dist >= 0.1 with seed 1.0 means the replayed pose simply never brings the hands together.
    if (g_gripDiag) {
        static uint32_t lastLog[kMaxNetPlayers] = {};
        if (g_nowMs - lastLog[slot] >= 1000) {
            lastLog[slot] = (uint32_t)g_nowMs;
            const uintptr_t B = Rd<uintptr_t>(C + 80);
            TO1_LOG("[gripDiag] slot%d seed=%.3f accum=%.3f w=%.3f | dist=%.4f engage=%.3f "
                "ramp=%.3f | Bw=%.3f\n",
                slot, Rd<float>(M + 0x2C), Rd<float>(M + 0x24), Rd<float>(M + 0x28),
                San(B) ? Rd<float>(B + 0x8C0) : -1.0f,
                Rd<float>(M + 0x34), Rd<float>(M + 0x3C),
                San(B) ? Rd<float>(B + 0xA0) : -1.0f);
        }
    }
}

// ============================================================================
//  WEAPON-IK FEATHER SYNTHESIS  (RECEIVER, anim thread)
//
//  Why this exists at all: the local player's captured AnimCmdList can NEVER contain
//  a weapon-ik-feather command, because on the player the engine calls
//  WeaponIKFeather::Apply directly (armik.cpp:0x1A9) and emits nothing. On an NPC the
//  same solve is driven by an anim command emitted from the anim-state layer walk.
//  Our puppets take neither route -- hk_CreateAnimCmds replaces the layer walk with
//  the replayed list -- so their arms are whatever the player's captured pose says,
//  with no correction onto the weapon they are actually holding. That is the
//  "holding it wrong" residue left after the attach point itself was fixed.
//
//  So we synthesise the NPC-route command here, using ND's own parameters, and blend
//  it in with the one thing we genuinely cannot derive locally: the player's fade.
//
//  Full dword-by-dword derivation is in anim-internal.hpp above the declaration.
// ============================================================================
int EmitWeaponIkFeather(uint32_t* out, uint32_t slot, float weight)
{
    // <= 0 means the weapon is stowed (or we have no fade yet): emitting a 0-weight
    // solve would be pure cost, and ND's own emitter is gated the same way
    // (sub_1420D6EA0: `if (v10 > 0.0)`). NaN falls through this test too.
    if (!(weight > 0.0f)) return 0;
    if (weight > 1.0f) weight = 1.0f;

    auto F = [](uint32_t* p, float v) { *reinterpret_cast<float*>(p) = v; };

    out[0] = MakeCmd(OP_PLUGIN, kWeaponIkDwords);
    out[1] = 0;                                              // ND: stale stack. Zeroed.
    *reinterpret_cast<uint64_t*>(&out[2]) = SID_WEAPONIKFTHR;
    out[4] = 0;
    out[5] = 0;
    out[6] = slot;                                           // pose slot in / out
    out[7] = 0;                                              // mode
    F(&out[8], 0.5f);                                        // unread by the executor
    F(&out[9], 1.0f);                                        // per-chain solve weight
    F(&out[10], weight);                                      // MASTER blend weight
    out[11] = 4;
    out[12] = 18;
    F(&out[13], 0.9f);
    out[14] = 1;                                              // two-chain path
    out[15] = 0;                                              // ND: stale stack. Zeroed.
    return kWeaponIkDwords;
}