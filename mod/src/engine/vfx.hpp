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

}
