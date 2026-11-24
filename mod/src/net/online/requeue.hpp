#pragma once

#include <cstdint>

// Ported from tools/ext/requeue.c: the queue stays after a match. When the match ends while
// we were in it we remember that, and once the post-match screens are done and we are back on
// the MP menu we ask the daemon (0xBC, mode-count poll) whether it still holds our requeue
// ticket. If it does, we push `to1/find-match-menu` ourselves (winmm's page tick starts the
// search with our saved mode pick). While that search is held for the other players of the
// last match, the status label reads WAITING FOR PLAYERS... instead of FINDING MATCH...
//
// Wire (tools/daemon.py / tools/requeue.py):
//   0xBC [64B token][u8 sub = 0][u8 mode = 0]       mode-count poll, no side effect
//   0xBC [64B token][u8 sub = 3][u8 0]               "my game window is closing" (see below)
//   0xBD [text payload]                             "... rq=1 rqw=<n> rqs=<s> rqb=<n> rqp=<n> ..."
//                                                   rqs seconds until the match forms anyway,
//                                                   rqb members back, rqp members on the screens.
//                                                   An older daemon sends only rq / rqw.
//
// The build-201 close-watch is a single thread-local WH_CALLWNDPROC hook on the game
// window thread (never a keyboard hook): the sent WM_SYSCOMMAND SC_CLOSE / WM_CLOSE is
// seen before the window procedure runs, and the daemon gets the sub-3 notice before
// the session drop so a window close in a match counts as the player's own leave.
//
// `t2r-no-requeue.txt` turns the whole feature off (the ext honours the same file). The
// server-side ticket is issued when the 0xE3 JIP clock report carries flag 0x20; net::jip
// now sends that flag while this module is enabled.
//
// The ext's exact match-end byte (LobbyObj+0x2ca4 / MatchSubstate()==6) is now read directly by
// engine/lobby.* and latched once per frame in net::match::liveness (mod match / menu state is
// only a fallback when the engine byte is not readable). Kick
// detection (g_kickMs) is now native via net/match/kick-state.* (the notice kind-'p' line is the
// mod's producer for the ext's op 0x11 session note); the kicknote box and the team-move rejoin
// remain in the ext. Party leadership / follow (FrInParty, me_leader)
// and the party-follow search gate (g_pqAuto) are now native via net::friends13. The exact
// post-match screen latches (clan13x / clan-postmatch / clan event / GEAR UP nav / g_mmBack)
// remain in the ext while it is the bridge; the top page being stable stands in for them.
// See the .cpp.

namespace net::requeue {

    // Once a frame from boot::FrameTick, after net::jip::Tick.
    void Tick();

    // The persistent-lobby latch, shared with chat's LOBBY tab: the ext's exact
    // `g_rq_back + g_rq_post > 0` from the 0xBD rqb / rqp fields. True only while
    // the daemon confirms a held ticket -- a fresh arm or a pushed page alone are
    // not enough. Counts zero when the search is released or the ticket times out.
    bool TicketHeld();

    // Session / match reset: drop the armed ticket and any answer in flight.
    void Reset();

}
