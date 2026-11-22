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
//   * Scoreboard rims are NOT ported: the mod has no settext note of the winmm
//     scoreboard name widgets (ext g_rkb_sbw / g_rkb_sbn), so there is no name to
//     look up there. Nothing is written for them.
//   * Our own role comes from a setter (default 0): the mod has no clan 0xE1
//     reader yet, so our own lobby row keeps a plain name until one is ported.

namespace net::name_rim {

    // Drive the rim poll. Call once per frame (boot FrameTick); it self-throttles
    // to every 150 ms like the ext.
    void Tick(uint64_t now);

    // Set our own role (0 none .. 4 lead dev) when a clan-state reader is ported.
    void SetOwnRole(int role);

    void Reset();

} // namespace net::name_rim
