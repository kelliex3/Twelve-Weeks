#pragma once

#include <cstdint>

// Native port of main's tools/ext/namerim.c (nrim_tick): the lead-dev pink name
// outline on the MENU name widgets. Rank badges change colour by role, but the
// in-match overhead tags' rim was the only place role 4 (lead dev) had a pink
// outline; the menu rows and scoreboards had none. gui2 text widgets have no
// outline colour, so the ext draws 8 hidden pink text clones per name at offsets
// (dx, dy) * R = 1.0, just below the name, and shows them only for role 4.
//
// Honest gates:
//   * Lobby rows (MP menu / find match) use the g_srchTab / g_srchName mirror from
//     net/social/search-rows.* plus the read-only engine lobby roster, so their
//     shown names and roles are producer-complete.
//   * Scoreboard rows (ESC / end of match) use net-gui2's read-only mirror of the
//     eight team%d_row%d_name texts it writes itself. No second settext hook is
//     installed: engine::widget's single observer cannot see the DC-driven writes.
//     A GUI pak without the generated nmRim clones makes every rim write inert.
//   * Our own role comes from net::clan::OwnRole() (0xBF/0xE1 state); SetOwnRole
//     is a producer / test override and stays inert at 0.
//   * The ext's 128-slot ring of engine writes to unknown row handles is not
//     ported. It exists only because the ext sees winmm's single early row write
//     through a global settext twin at exe+0x180064450 but resolves row handles
//     only twice a second; row 0 (us) had no roster fallback and stayed blank
//     (namerim.c:39-42). Native makes that write itself in net-gui2::fillLobby
//     with Username() == net::session::LocalName(), so row 0 falls back to that
//     direct producer value (LocalNameFallback) and rows 1..7 to the read-only
//     roster. Porting the ext hook instead would detour the same pinned prologue
//     t2r-ext already owns, double-notify search-rows' single SetTextObserver,
//     and refill an unfiltered ring with unrelated DC writes (128 of them evict
//     the wanted name). No second hook, no ring, same visible result.

namespace net::name_rim {

    // Drive the rim poll. Call once per frame (boot FrameTick); it self-throttles
    // to every 150 ms like the ext.
    void Tick(uint64_t now);

    // Set our own role (0 none .. 4 lead dev) when a clan-state reader is ported.
    void SetOwnRole(int role);

    void Reset();

} // namespace net::name_rim
