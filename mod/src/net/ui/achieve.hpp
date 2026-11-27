#pragma once

#include "achieve-core.hpp"

#include <cstdint>

// Native owner of the build-191 achievements feature: the client half of
// tools/ext/achieve.c (0xA9 match-end report, 0xAA table, 0xAB live request)
// and the to1/clanach page / unlock cards of tools/ext/achieve_ui.c. The daemon
// alone decides every unlock; this module only counts, asks for the table and
// drives what it is given. Wire bytes are the ext's exact opcodes (names added
// in net-protocol.hpp, nothing renumbered).
//
// Everything yields while t2r-ext.dll is loaded. Sidecars are read-only here:
// the seen stamps are consumed but never written, so no opt-in toggle is
// flipped. Widget writes go through engine::widget, which returns 0 for a
// widget an older GUI pak does not carry -- the state machine still runs.
namespace net::ui::achieve {

    // The post-match UNLOCKS stage lists the achievements the daemon's reply
    // marks new since the match started. MatchNew() is a read-only view of that
    // bounded queue; the UNLOCKS owner calls DropNew() so the same unlock does
    // not pop up again as a card when the achievement page opens later.
    const core::Toast* MatchNew(int* count);
    void NoteMatchStart();                // clear the queue and start capturing replies
    void DropNew(const char* id);

    // Producers call this for an achievement-only counter (labels 64..127).
    // It is inert outside a live match, with the opt-out sidecar, and while the
    // ext is loaded -- the same guard as ext ach_note().
    void Note(int label, int amount);

    void Tick(uint64_t now);
    void Reset();             // session reset (counters / wire state / cards)

    bool Active();            // the native owner currently owns the feature
    const core::Table& CurrentTable();

} // namespace net::ui::achieve
