#pragma once

#include <cstdint>

// Native owner of tools/ext/mfnr.c and feature_matchflow (t2r-ext.c): the map
// vote (stage 1), the one-use booster choice (stage 2) and the 2013 LOADING
// screen (stage 3). The pure rules are in mfnr-core.hpp; this module owns the
// daemon RPC (0xEA/0xEC/0xEB and 0xE8/0xEE/0xE9), the matchflow page lifecycle,
// the generated widget writes, the roster rank badges, the click gate, the
// loading-screen hook and the winning-map write to the lobby object.
//
// All wire opcodes are pinned in net-protocol.hpp. No sidecar is written and
// no toggle is flipped; `t2r-no-matchflow.txt` / `t2r-no-nr-ui.txt` remain
// read-only ext compatibility flags.

namespace net::ui::mfnr {

    // Called once from boot::InitCore. Stores the game module; the pinned
    // LoadingScreen::SetEnabled hook is installed on the first settled tick,
    // after ExtLoaded() has had a chance to resolve the deprecated ext.
    void Install(void* gameModule);

    // Called once per frame from boot::FrameTick after the page writers, so the
    // vote / booster / loading widget writes are the last word for this frame.
    void Tick(uint64_t now);

    // Drop the vote / meta state, page latches, optimistic vote and loading
    // ownership on session reset. The hook stays installed.
    void Reset();

} // namespace net::ui::mfnr
