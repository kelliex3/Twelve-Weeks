#pragma once
#include "engine/script.hpp"
#include "engine/draw.hpp"
#include "sid/sid.hpp"
#include "sid/sidbase.hpp"
#include "menu/debug-menu.hpp"
#include "loader/boot.hpp"
#include <safetyhook.hpp>

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <functional>


#define TO1_DEV_LOADDEVLEVEL 0 // Loads into the dev level on boot
#define TO1_DEV_MATCHMAKER 0 // Queues on the DEV matchmaker (port 7003)
#define TO1_LOCAL_MATCHMAKER 0 // For hosting servers locally

// #if TO1_LOCAL_MATCHMAKER && !defined(TO1_DEBUG)
// #error "TO1_LOCAL_MATCHMAKER is on in a release build - set it back to 0 before shipping."
// #endif

#if TO1_LOCAL_MATCHMAKER
constexpr const char* kModVersion = "v0.14 BETA LOCAL";
#elif TO1_DEV_MATCHMAKER
constexpr const char* kModVersion = "v0.14 BETA DEV";
#else
// was winmm_patches.py: version-text ("v1.08 BETA" -> "v0.14 BETA" in the base binary's
// .rdata). This is the string the main-menu / pause-menu versionText widget shows.
constexpr const char* kModVersion = "v0.14 BETA";
#endif

extern uintptr_t g_base;

// Convenience for the many "function at RVA" casts.
template <typename Fn>
inline Fn GameFn(uint32_t rva)
{
    return reinterpret_cast<Fn>(g_base + rva);
}

inline bool AllFinite(const float* p, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        if (!std::isfinite(p[i])) return false;
    return true;
}

inline bool IsUnitQuat(const float* q)
{
    if (!AllFinite(q, 4)) return false;
    const float len2 = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    return len2 > 0.81f && len2 < 1.21f;      // |q| in [0.9, 1.1]
}
