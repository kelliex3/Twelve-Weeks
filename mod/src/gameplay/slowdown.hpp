#pragma once

// Native read-only slowdown owner (tools/ext/slowdown.h). The ext's other
// "calmed systems" (blend / rate constants) have no native producer here; this
// only publishes its locomotion factor for the native footsteps gait feed.

#include "slowdown-core.hpp"

namespace gameplay::slowdown {

    // Effective locomotion multiplier: 0.90, or 1.0 with
    // t2r-no-slowdown.txt present next to the exe (2 s cache).
    float Mul() noexcept;
    bool  Off() noexcept;

    // Force the next Mul()/Off() call to re-read the sidecar (match reset).
    void  Reset() noexcept;

}
