#pragma once

#include "pmunlock-core.hpp"

#include <cstdint>

// Native owner of tools/ext/pmunlock.c: stage 3 of to1/clan-postmatch. The
// pure snapshot / list / stagger rules are in pmunlock-core.hpp. This module
// reads the shared CUSTOMIZE profile and the achievements the daemon reported
// since the match began; the post-match page owner calls Fill / Frame at the
// stage-2 -> stage-3 hand-off and owns the page widgets. No sidecar is written.
namespace net::ui::pmunlock {

    // Once per frame (boot::FrameTick). Detects the match-start edge so the
    // unlock grid is snapshotted before any reward lands, and yields entirely
    // while the legacy ext is loaded.
    void Tick(uint64_t now);

    // Anything for stage 3? Rebuilt at most every 250 ms, like the ext.
    int Count(int clanFac, uint64_t now);

    // Write the two columns ("anim-group.unlocks."), dropping the listed
    // achievements from the unlock-card queue.
    void Fill(int clanFac, uint64_t now);

    // Per-frame stagger / alpha, with a late table reply refilling the page.
    void Frame(int clanFac, uint64_t now, uint64_t elapsed);

    // How long the stagger takes; the page owner uses this as its select gate.
    uint64_t AnimMs();

    void Reset();

} // namespace net::ui::pmunlock
