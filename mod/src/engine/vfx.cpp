// ported from tools/ext/t2r-ext.c's particle helpers

#include "vfx.hpp"

#include "log.hpp"          // TO1_LOG must precede engine/script.hpp (it uses it in a template)
#include "engine/script.hpp"

namespace engine::vfx {

    uint64_t SpawnParticlesAtJoint(StringId64 effect, uintptr_t process, StringId64 joint,
                                   uint64_t a3, uint64_t flags, StringId64 root,
                                   uint64_t a6, uint64_t a7, uint64_t a8, uint64_t a9)
    {
        return DC::Call<"spawn-particles-at-joint">(
            effect, process, joint, a3, flags, root, a6, a7, a8, a9);
    }

    void KillParticles(uint64_t handle)
    {
        if (!handle)
            return;
        DC::Call<"kill-particles">(handle, 0);
    }

    void SetEffectAlpha(uint64_t handle, float alpha)
    {
        if (!handle)
            return;
        DC::Call<"set-effect-alpha">(handle, alpha);
    }

}
