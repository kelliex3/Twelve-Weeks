#pragma once

#include <cstdint>

// Port of tools/ext/kicknote.c: an admin / AFK / ping / packet-loss removal opens
// to1/kick-notice on the MP menu and stays until the player closes it. A team move
// is not a kick and keeps its toast. This is the native page owner; while the
// legacy t2r-ext.dll is actually loaded its own KnSet/page handler owns the box
// and these calls are inert (no duplicate push or duplicate input handler).
namespace net::ui::kick_note {

    bool Set(const char* text);      // notice::Hooks::kickSet; true = the box took the line
    void Tick(uint64_t now);         // once per frame, after notice::Tick
    bool Busy();                     // due / pushed / up (ext KnBusy)
    bool ClickHit();                 // ext KnClickHit: click-gate seam

    void InstallNoticeHooks();       // installs kickSet (merges with the other hooks)
    void Reset();                    // session / test reset

}
