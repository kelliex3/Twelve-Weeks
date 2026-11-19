#pragma once

#include "sid/sid.hpp"

#include <cstdint>

// Ported from tools/ext/t2r-ext.c's particle helpers (the ext's own fx spawns and the
// scopeglint flare). Three engine natives in one reusable place:
//
//   spawn-particles-at-joint(effect, process, joint, a3, flags, root, a6, a7, a8, a9)
//   kill-particles(handle, 0)
//   set-effect-alpha(handle, alpha)
//
// The 10-argument spawn is the shape the SP sniper flare uses (spawn-tommy-lens-flare):
// the effect sid, the process to hang it on, the joint sid, then a fixed flag / root tail.

namespace engine::vfx {

    // spawn-particles-at-joint. Returns the effect handle (0 when refused). `flags` is 16
    // for the sniper flare's first try; the ext retried the same call with flags 2 for its
    // plain spawns, so a caller that wants that passes it explicitly.
    uint64_t SpawnParticlesAtJoint(StringId64 effect, uintptr_t process, StringId64 joint,
                                   uint64_t a3 = 0, uint64_t flags = 16,
                                   StringId64 root = SID("root"),
                                   uint64_t a6 = 2, uint64_t a7 = 0,
                                   uint64_t a8 = 2, uint64_t a9 = 0);

    // kill-particles(handle, 0). No-op on 0.
    void KillParticles(uint64_t handle);

    // set-effect-alpha(handle, alpha). No-op on 0.
    void SetEffectAlpha(uint64_t handle, float alpha);

    // ---- GPU-compute particle entries ------------------------------------------------
    //
    // Ported from tools/ext/t2r-ext.c (CFX_N / CfxRtmgr / CfxUsed / CfxOwner / CfxDressing /
    // CfxMakeRoom / CfxStream / CfxSpawnRaw / CfxFallback / CfxRetryTick / ArmorFxSpawn).
    //
    // Some particle groups (the self-pulsing blood streams) hold one of the engine's 23
    // GPU-compute entries while they live. The entry table lives on the particle runtime
    // manager; when it is full a stream spawn silently fails. The ext ended one short-lived
    // "dressing" group (a per-hit ribbon / drip / mask) to free an entry, retried the spawn
    // over the next frames, and fell back to SP's slash ribbon + drop burst if no entry came
    // back. All of that is here so any feature can spawn a stream.

    inline constexpr int kComputeEntries = 23;   // ext CFX_N

    // A particle group that needs a compute entry (pulse / splurt stream). ext CfxStream.
    bool Stream(uint64_t particle);

    // The particle runtime manager (exe+0x37dee90), or nullptr when the exe code differs
    // (ext CfxRtmgr).
    uint8_t* Rtmgr();

    // Entries in use (ext CfxUsed).
    int Used(const uint8_t* m);

    // End one dressing effect holding an entry (ext CfxMakeRoom). true = one was ended.
    bool MakeRoom(uint8_t* m);

    // spawn-particles-at-bound-frame, then spawn-particles-at-joint; a non-finite joint is
    // swapped for a finite one of the same body or the spawn is skipped (ext CfxSpawnRaw).
    uint64_t SpawnRaw(uint64_t particle, uint64_t chr, const char* joint);

    // SpawnRaw, and for a compute stream that did not spawn: make room / queue a retry, or
    // fall back to SP's slash ribbon + drop burst (ext ArmorFxSpawn). Returns 1 when queued.
    uint64_t ArmorFxSpawn(uint64_t particle, uint64_t chr, const char* joint);

    // Retry queued streams once their compute entry may have come back. Once per frame
    // (ext CfxRetryTick).
    void RetryTick();

}
