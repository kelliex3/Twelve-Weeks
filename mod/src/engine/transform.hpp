#pragma once

#include "sid/sid.hpp"

#include <cstdint>

// Ported from tools/ext/t2r-ext.c (ProcWorld). The world transform of a process (a weapon or
// NPC process object) read through the engine's own locator read (exe+0x133DB30) on
// `process+0x100`. The returned buffer is 8 floats: out[0..2] = world position,
// out[4..7] = world rotation quaternion (x, y, z, w).
//
// INFERRED: the engine RVA 0x133DB30 comes from the ext's disassembly of the shipped exe and
// has not been re-derived against the mod's own build. Re-check it before trusting a port that
// leans on it.

namespace engine::transform {

    // true = out filled. false = `process` is null / unreadable, or the engine returned nothing.
    bool ProcessWorld(uintptr_t process, float out[8]);

    // Convenience wrappers over ProcessWorld.
    bool ProcessPosition(uintptr_t process, Vector3& out);
    bool ProcessRotation(uintptr_t process, Quaternion& out);

}
