#pragma once

#include <cstdint>

// Native home for the post-match / menu screen latches that hold requeue's settle.
// Ported from tools/ext/t2r-ext.c (clan13.c's Cl13PostActive / g_c13x.post, the
// g_pm.pending clan-postmatch state, g_ev.pending clan-event state, the GEAR UP
// navigation state g_loState != LO_NONE, and g_mmBack) as consumed by the
// "every post-match screen done" gate in tools/ext/requeue.c.
//
// The ext keeps these as feature-local bools driven by its page handlers. Here the
// producers are native: g_pm.pending / the clan-event decision come from
// net/online/clan-state.* (0xE0/0xE1/0xE2), the 2013 CLAN page data is asked and
// parsed by net/social/clan-page-data.* (0xB6/0xB7), the g_mmBack Steam gate is in
// net/online/steam-status.*, and the party gate is wired in net-gui2.cpp. The menu
// stack is still sampled for pages the mod does not own, so any consumer can ask
// one question -- ScreensDone() -- without re-deriving the pages. The mod owns the
// to1/clan-postmatch and to1/clan-event page lifecycle (push / populate / input /
// pop) in this module; the ext, still loaded and ticked first, may push the same
// page from its own latch, but every push here is behind an IS_PAGE_ON_STACK guard
// so an ext push is adopted rather than duplicated. The mod's find-match flow pops
// the page it refuses, so kPopOnMmBack stays false.
//
//   Cl13PostActive()  to1/clan13x scoreboard is up     (ext Cl13PostActive; ext-owned)
//   ClanPostActive()  to1/clan-postmatch due or up    (ext g_pm.pending)
//   ClanEventActive() to1/clan-event due or up        (ext g_ev.pending)
//   GearUpNavActive() GEAR UP navigation state != None (ext g_loState != LO_NONE)
//   MmBackActive()    a FIND MATCH press was refused and the page is being popped
//                     (ext g_mmBack: party gate + Steam-helper gate)
//
// Honest gates: to1/clan13 (and its clan13x scoreboard) stay ext-owned; the mod
// only observes that page's stack edges. The 0xE2 wire report also stays ext-owned
// while the ext is the bridge -- net::clan::ArmPost() latches the mod's page locally
// at match end and NoteAward() feeds the tally, with no duplicate 0xE2 send. The
// ext's scoreboard-first ordering (g_pm.sb), the pre-main-menu EV gates and the
// mission-meta goal table remain unported and are called out at their sites.

namespace net::ui::postmatch {

    // ext gearup13.c's state enum (LO_NONE / LO_LIST / LO_EDIT / LO_CHOOSE /
    // LO_MATCH). Native transitions; the menu-stack sample is only a fallback.
    enum class LoState : uint8_t {
        None = 0, List = 1, Edit = 2, Choose = 3, Match = 4,
    };

    // Once per frame from boot::FrameTick, BEFORE requeue (advances the mmBack
    // frame counter and samples the menu stack / the native clan latches).
    void Tick();

    // Session reset: clear every latch (page ids are cached and survive).
    void Reset();

    bool Cl13PostActive();
    bool ClanPostActive();
    bool ClanEventActive();
    bool GearUpNavActive();
    LoState Lo();
    bool MmBackActive();

    // Single gate for requeue: true when no post-match / refused-search screen is due.
    bool ScreensDone();

    // GEAR UP state transitions (ext GuEnter / GuListSelect / mid-match F1 /
    // CHOOSE LOADOUT countdown). Callers are the mod's loadout page handlers;
    // Tick also derives the obvious cases from the menu stack.
    void NoteLoadoutOpen(bool inMatch);
    void NoteLoadoutEdit();
    void NoteLoadoutApplied();          // mid-match pick: applied at the next respawn
    void NoteLoadoutClose();
    void NoteCountdownChoose(int phase, int secondsLeft);

    // Producers: ext MmreqHook's two g_mmBack paths. Both clear after three frames.
    void NoteMmRefused();               // FrSearchGate party gate (net-gui2.cpp)
    void NoteSteamRefused();            // g_steamOnly && !SteamConnected()

}
