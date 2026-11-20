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
//   0xBC [64B token][u8 sub = 0][u8 mode = 0]       no side effect on the daemon
//   0xBD [text payload]                             "... rq=1 rqw=<n> ..." when a ticket is held
//
// `t2r-no-requeue.txt` turns the whole feature off (the ext honours the same file). The
// server-side ticket is issued when the 0xE3 JIP clock report carries flag 0x20; net::jip
// now sends that flag while this module is enabled.
//
// Honest gates (TODO): the ext's exact match-end byte (LobbyObj+0x2ca4 / MatchSubstate()==6)
// is approximated by the shared net::match::liveness latch (mod match / menu state). Kick
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

    // Session / match reset: drop the armed ticket and any answer in flight.
    void Reset();

}
