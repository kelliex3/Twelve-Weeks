#pragma once

// Native asker / snapshot for the 2013 in-game CLAN page data
// (tools/ext/clan13.c Cl13Ask / Cl13NetTick / Cl13Ingest, parsed by the pure
// clan-page-core.hpp port of clan13.h). Wire: daemon 0xB6 request ->
// 0xB7 section text, exact ext bytes pinned in net-protocol.hpp.
//
// The mod has no to1/clan13 widget page, so rendering stays ext-owned; the
// request loop, the bounded 64 KB replies and the parsed snapshot are native,
// so any mod-side consumer can read the same data without the ext.

#include "clan-page-core.hpp"

#include <cstdint>

namespace net::clan::page {

    void Tick();
    void Reset();

    const Data& Snapshot();

    bool Have(int section);            // sections 1..6
    int  Version(int section);         // replies seen (drives a UI refill)
    bool Busy();                       // a request is waiting on its timer

    // The section-5 reply is for the raid of that day; 0 = no stats wanted.
    void NoteStatsDay(int day);
    // The section-6 reply carries the paragraphs of that week (0 = newest).
    void NoteJournalWeek(int week);
    // Newest journal id / raid day seen (the "NEW" markers).
    void NoteSeen(long journalId, int day);
    // 0 = English, 1 = Polish (ext FeedLang()).
    void SetLang(uint8_t lang);

} // namespace net::clan::page
