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
// Ported: the report cadence (2 s host / 3 s client), the exact engine substate / end-banner
// reads (engine/lobby.*) behind the clock-valid / match-over flags, the viewer-relative team +
// lives, the AFK trailer, one short bounded TCP request per cadence, the reply's test-server
// flags (published through net::daemon::TestFlags()) and "enabled = 0" stop, the no-answer
// backoff, and the 0xE5 roster merge / 0xA7 own-team swap / ghost-peer rules below.
//
// The 0xE5 roster is merged into a native SHADOW (seeded from the read-only engine lobby on
// each reply): stale entries pruned, new slots appended, known slots replaced, fallback
// "PLAYER n" names never overwrite a real name, and our own entry preserved / restored.
// The trailing [0xA7][k][k x (slot, team)] block is parsed into the shadow's own team with
// the same three-field apply rule. Ghost reaping uses the listed mask plus accepted-anim
// silence and the session role. All engine writes and actual peer drops are behind the
// kWriteLobby / kApplyGhostPeers / kRequestTeamSwap gates in jip-report.cpp (false while the
// ext is the bridge), so this is observation + decision logic, not a cutover. The requeue
// flag 0x20 IS sent while `t2r-no-requeue.txt` is absent: the ticket check / auto-search lives
// in net/online/requeue.* now. `t2r-no-jip.txt` turns the report off (the ext honours the
// same file). Logs "jip: ..." / "ghost peer: ...".
//
// Honest gates (TODO): the late-join query (flags 0x04, kit carry) is still ext-owned.

namespace net::jip {

    // Once a frame from boot::FrameTick, AFTER net::afk::Tick so the trailer carries
    // this frame's idle count.
    void Tick();

    // Session / level reset: clear per-match state and abort any pending report. The
    // server's test flags survive (they are server-wide, not match state).
    void Reset();

    // ---- read-only view of the merged roster shadow ---------------------------------------
    // These expose the native 0xE5 merge result without touching the engine. Entries are the
    // exact 0x32-byte lobby layout; team lives at entry +0x31. 0 / nullptr when no reply.
    int         RosterCount();
    const uint8_t* RosterEntry(int index);
    int         TeamOfSlot(int slot);   // 0 / 1, or -1 when the slot is not in the shadow
    int         OwnTeam();              // from the 0xA7 apply, or -1 when not swapped
    uint32_t    ListedSlots();          // mask of slots in the newest server roster
    uint64_t    ListedMs();             // when that roster arrived (0 = none)

}
