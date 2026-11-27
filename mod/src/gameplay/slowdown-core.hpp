#pragma once

// Pure rules for tools/ext/slowdown.h. The ext applies one locomotion factor
// to every client's own body; puppets follow sampled poses, so a native peer
// only needs the same number to classify its gait (footsteps). The owner in
// slowdown.cpp reads the opt-out sidecar and supplies the clock; no engine or
// file I/O happens here. Tests: mod/tests/slowdown_core_test.cpp.

#include <cstdint>

namespace gameplay::slowdown::core {

    inline constexpr float    kMove = 0.90f;    // ext SD_MOVE
    inline constexpr uint64_t kCacheMs = 2000;  // ext re-reads every 2 s

    // ext sd_mul(): the opt-out file restores the original speed.
    inline float Mul(bool off) noexcept { return off ? 1.0f : kMove; }

    inline bool CacheDue(uint64_t now, uint64_t at) noexcept
    {
        return at == 0 || (now >= at && now - at >= kCacheMs);
    }

}
