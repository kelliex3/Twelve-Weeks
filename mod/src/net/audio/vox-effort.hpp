#pragma once

// Native owner of the ext's female effort-SID feed (t2r-ext.c fem_effort_sid).
// The pure pick / no-repeat / look-voice rules and the generated line table
// have their own headers; this is the seam the execution audio owners call.
//
// Ticks are not needed (the pick is lazy and keeps its own wheel); Reset()
// belongs in boot::FrameTick()'s match-reset path via net-replication.cpp.

#include "vox-effort-core.hpp"

#include <cstdint>

namespace net::audio::effort {

    // Female speaker of `ent`: its own authored effort take, or 0 when the
    // body is male / unknown / the generated table is not built. Callers keep
    // their existing engine effort-event fallback on 0.
    uint64_t EffortSid(uint64_t ent, core::Kind kind) noexcept;

    void Reset() noexcept;

}
