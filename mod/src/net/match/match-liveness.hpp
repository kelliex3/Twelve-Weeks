#pragma once

#include <cstdint>

// Shared match-end latch ("g_wasLive" in the ext). The ext reads its match substate
// (MatchSubstate() == 6 / LobbyObj()+0x2ca4) once per tick; several features use that
// one read: requeue arms when the match ended while we were in it, friends13 hides its
// page and toasts during the end sequence, and the party follow logic stops pushing.
//
// The mod has no raw substate / lobby byte, only its own match phase
// (net::gui2::InMatch, MatchExpired, ScoreboardShowing). Those collapse to the same
// one state read: Live() now, WasLive() last frame, and MatchEndHold() true while the
// end sequence is up for a session that was live at some point. Latched per frame so
// every consumer sees the same answer; Tick() runs once, first, in boot::FrameTick.
//
// TODO(match-liveness): the exact ext substate 6 / LobbyObj end banner is not ported.
// The observable gates here are the mod's own ones, which is why the ext keeps its own
// value while it is the bridge.

namespace net::match::liveness {

    // Once per frame, before any consumer in boot::FrameTick.
    void Tick();

    bool Live();          // this frame: in a running match
    bool WasLive();       // last frame: in a running match
    bool MatchEndHold();  // this frame: end sequence up for a session we were in

    void Reset();         // session reset: drop the latch

}
