#pragma once

#include <cstdint>

// Ported from tools/ext/t2r-ext.c's FeatureJoinInProgress / JipAsk / JipWorker: the
// daemon 0xE3 CLOCK REPORT and its 0xE5 reply. The daemon uses the clock to offer a
// running match to joiners; the trailer written by net::afk::ReqTail() is what lets it
// remove an idle player. Both were needed to make the AFK port work end to end.
//
// Wire (tools/daemon.py):
//   0xE3 [64B token][u8 flags][f32 seconds left][f32 duration]
//        [u8 my team][u8 ally lives][u8 enemy lives][0xAF][u16 idle s][afk flags]
//   0xE5 [u8 enabled][u8 n][n x 0x32 roster entries][u8 wave byte]
//        wave byte: 0x80 present, bit0 wave respawn, bits 1-4 test-server flags
//
// Ported: the report cadence (2 s host / 3 s client), the clock-valid / match-over
// flags, the viewer-relative team + lives, the AFK trailer, one short bounded TCP
// request per cadence, the reply's test-server flags (published through
// net::daemon::TestFlags()) and "enabled = 0" stop, and the no-answer backoff.
//
// Honest gates (TODO): the 0xE5 roster merge into the lobby object, the in-match team
// swap block (0xA7), the late-join query (flags 0x04), ghost peer reaping and the
// requeue flag (0x20) are separate unported features -- they write engine state the
// ext still owns while it is the bridge. Consequently this report does NOT set 0x10
// (we do not apply team swaps) or 0x20 (no requeue), and only consumes the reply's
// server settings. `t2r-no-jip.txt` turns the report off (the ext honours the same
// file). Logs "[JIP] ...".

namespace net::jip {

    // Once a frame from boot::FrameTick, AFTER net::afk::Tick so the trailer carries
    // this frame's idle count.
    void Tick();

    // Session / level reset: clear per-match state and abort any pending report. The
    // server's test flags survive (they are server-wide, not match state).
    void Reset();

}
