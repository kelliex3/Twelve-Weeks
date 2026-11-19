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
// Honest gates (TODO): the ext's exact match-end byte (LobbyObj+0x2ca4 / MatchSubstate()==6),
// kick detection (g_kickMs), party leader (FrInParty / g_f13.me_leader), the post-match screen
// latches (clan13x / clan-postmatch / clan event / GEAR UP nav / g_mmBack) and the party-follow
// search gate (g_pqAuto) are not ported. The mod uses its own match / menu / matchmaking state
// for the first two families and relies on the top page being stable for the settle window;
// the remaining producers stay in the ext while it is the bridge. See the .cpp.

namespace net::requeue {

    // Once a frame from boot::FrameTick, after net::jip::Tick.
    void Tick();

    // Session / match reset: drop the armed ticket and any answer in flight.
    void Reset();

}
