#pragma once

#include <cstddef>
#include <cstdint>

// Native mirror of the ext's g_srchTab / g_srchName producers (t2r-ext.c
// SrchNameWrite / SettextHook): the SEARCHING page's rows carry " [T1]" / " [T2]"
// after a searcher's name (T1 = our team, T2 = the enemy), and a party mate who
// stopped searching shows " (party lead - not searching)". The ext cuts those
// markers off the name and keeps one tab byte + shown name per row for the FRIENDS
// panel's lobby-row decorations.
//
// Wire / engine reads stay read-only. The mod observes the text writes made through
// engine::widget::SetText (its own rows); the engine's / winmm's writes still pass
// through the ext's pinned settext hook while the ext is the bridge, so a competing
// hook is deliberately NOT installed here. MATCH FOUND names are resolved from the
// read-only engine lobby (engine::lobby::RosterSorted / RosterName) exactly as in
// SrchNameWrite, so those rows are producer-complete.

namespace net::social::search_rows {

    // Call once per frame (boot FrameTick): resolve the eight row handles on
    // find-match-menu, apply writes seen before a row was found, and clear on leave.
    void Tick();

    // Observer entry point for engine::widget::SetTextObserver.
    void Note(uint64_t widget, const char* text, void* user = nullptr);

    void Reset();

    // 0 = no marker, 1 = our team ([T1]), 2 = enemy ([T2]),
    // 3 = party mate not searching.
    int         Tab(int row);
    const char* Name(int row);         // "" when the row has no captured name

} // namespace net::social::search_rows
