#pragma once

#include "sid/sid.hpp"

#include <cstddef>
#include <cstdint>

// Ported from tools/ext/t2r-ext.c (AnimateFrom / AnimLen / AllocEntryBad). The two
// animation primitives every ported feature needs on a body or a prop, kept out of the
// net layer so engine-side features share them:
//
//   AnimateFrom  the ext's animate_ recipe with a blend-in and a start phase (the plain
//                ANIMATION::ANIMATE has neither, so a puppet clip always snapped to 0).
//                The script array the engine's #%alloc-array hands back is written
//                directly (entry 0 = key 28, blend seconds; entry 1 = key 58, phase 0..1).
//   Len          get-animation-duration decoded to a float; 0 = the clip is unknown on
//                that body / not loaded (the ext's gate for "can this clip play here?").
//   AllocEntryBad  a script-array entry the allocator just returned sits in the same arena
//                as its header; anything within 1 MB of the header is trusted without the
//                (cold, slow) writability probe the ext measured stalling frames.

namespace engine::anim {

    // animate_ with a blend-in (seconds) and a start phase (0..1). ~0ULL on failure (no
    // array / bad entries / no native). ext AnimateFrom.
    uint64_t AnimateFrom(StringId64 entity, const char* anim, float blend, float phase);

    // get-animation-duration(entity, clip) as a float. 0 when the call failed or the
    // duration is outside the plausible 0.2..30 s band (the ext's exact gate).
    float Len(StringId64 entity, const char* clip);

    // Is a script-array entry unusable? Entries within 1 MB of `hdr` are trusted (the
    // ext's perf workaround); farther ones get the full writability probe.
    bool AllocEntryBad(const void* hdr, const void* entry, size_t n);

}
