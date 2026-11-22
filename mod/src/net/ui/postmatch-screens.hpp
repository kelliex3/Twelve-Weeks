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
// stack is still sampled for pages the mod cannot push itself, so any consumer can
// ask one question -- ScreensDone() -- without re-deriving the pages. No page is
// pushed or popped by default (kPopOnMmBack == false): the ext still owns the widget
// pages while it is the bridge, and the mod's find-match flow pops the page it refuses.
//
//   Cl13PostActive()  to1/clan13x scoreboard is up     (ext Cl13PostActive)
//   ClanPostActive()  to1/clan-postmatch due or up    (ext g_pm.pending)
//   ClanEventActive() to1/clan-event due or up        (ext g_ev.pending)
//   GearUpNavActive() GEAR UP navigation state != None (ext g_loState != LO_NONE)
//   MmBackActive()    a FIND MATCH press was refused and the page is being popped
//                     (ext g_mmBack: party gate + Steam-helper gate)
//
// Honest gate: the mod has no to1/clan-postmatch / to1/clan-event / to1/clan13 widget
// page, so it cannot push one or observe its enter/exit edges directly; the ext keeps
// doing that. The *decisions* and the pre-push latches are native now: requeue no
// longer has to absorb that window with its settle.

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
