#pragma once

#include <cstdint>

namespace gameplay::barrelcheck {

    extern bool g_enabled;
    extern bool g_drawDebug;
    extern float g_rHandVOffset;
    extern float g_rHandHOffset;

    void Tick();

    bool ShotBlocked();

    void LatchProjectile(uintptr_t process);

    bool ImpactBlocked(uintptr_t bulletArgs);

    void NotifyDeniedImpact();
}
