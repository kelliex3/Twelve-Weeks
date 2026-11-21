#pragma once

#include <cstdint>

// The feature half of the 2013 rank badge (tools/ext/rankbadge.c + rank13.h), ported
// into mod/ as native C++. The pure table (name key, bounded 0xBF reply parser,
// lookup, placeholder and symbol / weeks helpers) lives in net/social/friends-state.*
// (social::RankTable / RankParse / RankLookup / RankPlaceholder / Rank*); this module
// owns the async daemon poll and the widget drawing, consumed by friends13.
//
// Honest gate: the table is only asked for while some screen actually draws a badge
// (ext g_rkbWantAt within 15 s). With the FRIENDS panel closed nothing polls, so an
// older daemon that ignores op 0xBF (two unanswered tries) is never a cost. No ext
// cutover / toggle is touched; the widget pair is absent (handle 0) on a GUI pak that
// does not carry rank_badge_widgets.py and every write is then inert.

namespace net::rank_badge {

    // Drive the async 0xBF poll. Call once per frame (boot FrameTick).
    void Tick(uint64_t now);

    // Drop the table and the poll state on a session / match reset.
    void Reset();

    // Draw the badge on a row's <icon> (image) / <num> (weeks text) widget pair for
    // the shown player `name`. Both paths are page-relative (ext full paths are built
    // by the caller from the row base + leaf). `playerRow` is the ext's g_frPlayer:
    // a real player row (not an action row) gets the lowest rank (0 / 0 weeks) once
    // the daemon has answered at least once, so a new player still has a badge.
    // Returns true when a badge is shown -- the caller then reserves 120 units in
    // front of the 46 pt name and shifts its pivot by that width (ext RKB_RES46).
    bool DrawBadge(const char* iconPath, const char* numPath, const char* name,
        bool playerRow);

}
