#pragma once

#include "challenges-core.hpp"

#include <cstdint>

// Native owner of build-191 CUSTOMIZE | CHALLENGES (tools/ext/customize_challenges.c).
// The grid/focus/equip rules stay in challenges-core.hpp; this owner uses the
// shared CUSTOMIZE profile and widget seam in customize.hpp. It saves the
// profile atomically and sends NEVT_CUSTOM_LOOK on a successful equip, and
// yields while t2r-ext.dll is loaded (the ext is the deprecated path).
namespace net::ui::customize::challenges {

    void Tick(uint64_t now);
    void Reset();

    bool Active();

} // namespace net::ui::customize::challenges
