#pragma once

#include <cstdint>

// Shared match-end latch ("g_wasLive" in the ext). The ext reads its match substate
// (MatchSubstate() == 6 / LobbyObj()+0x2ca4) once per tick; several features use that
// one read: requeue arms when the match ended while we were in it, friends13 hides its
// page and toasts during the end sequence, and the party follow logic stops pushing.
//
// The exact engine reads are ported in engine/lobby.* (substate at kNetCtx+0x2b80,
// end banner at LobbyObj()+0x2ca4). Tick() prefers them and falls back to the mod's own
// match phase (net::gui2::InMatch, MatchExpired, ScoreboardShowing) when the engine data
// is not readable. Those collapse to the same one state read: Live() now, WasLive() last
// frame, and MatchEndHold() true while the end sequence is up for a session that was live
// at some point. Latched per frame so every consumer sees the same answer; Tick() runs
// once, first, in boot::FrameTick. The reads are observation-only: no engine state is
// written, so the ext stays the behaviour owner until a feature is cut over.

namespace net::match::liveness {

    // Once per frame, before any consumer in boot::FrameTick.
    void Tick();

    bool Live();          // this frame: in a running match
    bool WasLive();       // last frame: in a running match
    bool MatchOver();     // this frame: lobby end banner (or the mod's end-screen fallback)
    bool MatchEndHold();  // this frame: end sequence up for a session we were in

    void Reset();         // session reset: drop the latch

}
