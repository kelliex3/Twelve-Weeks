#pragma once

#include <cstdint>

// Native home for the post-match / menu screen latches that hold requeue's settle.
// Ported from tools/ext/t2r-ext.c (clan13.c's Cl13PostActive / g_c13x.post, the
// g_pm.pending clan-postmatch state, g_ev.pending clan-event state, the GEAR UP
// navigation state g_loState != LO_NONE, and g_mmBack) as consumed by the
// "every post-match screen done" gate in tools/ext/requeue.c.
//
// The ext keeps these as feature-local bools driven by its page handlers. Here they
// are derived from the menu stack (engine::widget / MENU2) once per frame, so any
// consumer can ask one question -- ScreensDone() -- without re-deriving the pages.
// No page is pushed or popped by default (kPopOnMmBack == false): the ext still owns
// the page behaviour while it is the bridge, and the mod's own find-match flow
// (net-gui2.cpp) already pops the page it refuses.
//
//   Cl13PostActive()  to1/clan13x scoreboard is up     (ext Cl13PostActive)
//   ClanPostActive()  to1/clan-postmatch counter is up (ext g_pm.pending)
//   ClanEventActive() to1/clan-event screen is up      (ext g_ev.pending)
//   GearUpNavActive() any GEAR UP / loadout / picker page is up (ext g_loState != LO_NONE)
//   MmBackActive()    a FIND MATCH press was refused and the page is being popped
//                     (ext g_mmBack)
//
// Honest gates (TODO): the ext sets g_pm.pending / g_ev.pending when the page is DUE,
// before it is pushed; the mod can only observe the page itself, so that short
// pre-push window is not latched (requeue's 1.5 s settle absorbs it). The 2013 CLAN
// page data, the clan-event decision and GEAR UP's own g_loState transitions stay in
// the ext. g_mmBack's second producer (Steam helper not connected) has no mod
// equivalent; the party-gate producer is wired in net-gui2.cpp.

namespace net::ui::postmatch {

    // Once per frame from boot::FrameTick, BEFORE requeue (advances the mmBack
    // frame counter and samples the menu stack).
    void Tick();

    // Session reset: clear every latch (page ids are cached and survive).
    void Reset();

    bool Cl13PostActive();
    bool ClanPostActive();
    bool ClanEventActive();
    bool GearUpNavActive();
    bool MmBackActive();

    // Single gate for requeue: true when no post-match / refused-search screen is up.
    bool ScreensDone();

    // Producer: a FIND MATCH press was refused (party gate; the ext also sets this
    // when the Steam helper is not connected, which the mod has no equivalent for).
    // Mirrors the ext's MmreqHook setting g_mmBack = 1; the latch clears after three
    // frames, as in the ext.
    void NoteMmRefused();

}
