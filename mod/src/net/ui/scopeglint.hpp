#pragma once

#include <cstdint>

// Ported from tools/ext/scopeglint.c: a less prominent version of the SP sniper glare, shown on
// anyone aiming through a scope. The effect is the SP one (sniper-scope-flare-1) spawned on the
// shooter's puppet weapon at joint pump_mod (fallbacks scope0, root), its alpha driven every
// frame by max_alpha * facing * line-of-sight. Never on the sniper's own screen.
//
// Peers (NEVT_SCOPE_GLINT 0x91): [u8 on] on every edge, a keep-alive every 0.5 s while scoped,
// the stop sent 3x; the receiver expires it after 1.25 s.
//
// Opt-out: t2r-no-scope-glint.txt next to the exe (no message, no effect).
// Tunable: t2r-scope-glint.txt "max_alpha [cone_full_deg cone_zero_deg]" (default 0.5 12 60).
//
// The ext's dev override (t2r-dev-glint.txt, forcing the scoped state) is deliberately NOT
// ported. The reusable dependencies live in engine/{frame,exe_side,flag,vfx,raycast,transform,
// local-player} and net/game/player-status.

namespace net::scopeglint {

    // boot::FrameTick, once per frame.
    void Tick();

    // net::replication::HandleNetEvent, NEVT_SCOPE_GLINT.
    void OnEvent(uint8_t senderSlot, const uint8_t* data, uint8_t size);

    // Session teardown / leave the match.
    void Reset();

}
