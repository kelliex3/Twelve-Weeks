#pragma once

#include <cstdint>

// The kick timestamp ("g_kickMs" in tools/ext/t2r-ext.c), ported as shared state.
// The daemon sends a removal (admin kick, ping limit, the AFK removal decided by
// the already-ported net/match/afk.* watchdog) as the kick text right before it
// takes us back to the menu, both as an op 0x11 session note and as the notice
// poll's kind-'p' line (tools/daemon.py kick_token -> SOCIAL.note). The ext
// stamps g_kickMs in MsgHook (its winmm message hook); the mod has no such hook,
// so the kind-'p' line is the producer here -- notice::Apply calls Note().
//
// Consumers:
//   - net/ui/notice.cpp: a kick line seen within 120 s of the stamp is the
//     duplicate of the one already shown, and is dropped;
//   - net/online/requeue.cpp: a kick stamped after the match ended disarms the
//     auto-search (the daemon has dropped our ticket too).
//
// An admin team move ("TEAMS: ..." / "DRUŻYNY: ...") carries the same text shape
// and stamps this too, as in the ext: requeue steps aside so the move's rejoin can
// run. That rejoin (ext g_moveMs / move_rejoin_tick) is still owned by the ext.
// The kicknote box (ext kicknote.c / KnSet) is also unported: notice::Hooks::kickSet
// stays null and the kick keeps its toast.
//
// There is no dedicated t2r-*.txt file for this in the ext; it is shared
// infrastructure, gated at its consumers by t2r-no-notice.txt / t2r-no-requeue.txt.

namespace net::match::kick {

    // The ext's MsgIsMove / MsgIsKick text tests (byte-for-byte, including the
    // source-encoded Polish team-move prefix).
    bool IsMove(const char* text);
    bool IsKick(const char* text);

    // Stamp now. Called by the notice kind-'p' producer; a non-kick text is ignored.
    void Note(const char* text);

    uint64_t ShownAt();          // GetTickCount64 of the last kick text (0 = none)
    bool Since(uint64_t when);   // stamped after `when` (requeue's g_kickMs > armed)

    // Install the notice hooks that read this state (isKick / kickShownAt).
    // SetHooks merges, so this cannot clear the friends13 hooks or vice versa.
    void InstallNoticeHooks();

    void Reset();                // session reset: drop the stamp

}
