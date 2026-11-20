#pragma once

#include <cstdint>

// Native port of tools/ext/friends13.c: the in-game FRIENDS / PARTY panel (the P
// button under the LOBBY box on the MP menu, to1/friends13), its friend / party
// toasts, the party ready / follow-the-leader rule and the search gate.
//
// Wire (tools/social.py), one short connection per request, like notice.c:
//   0xD8 [64B token][u8 ver=1][u8 lang 0 en / 1 pl][u8 sub][UTF-8 arg]
//   0xD9 [JSON state]                     (a refused write still carries fresh state)
// Sub ops: 1 state, 3 add, 4 accept, 5 remove / decline / cancel, 6 invite,
// 7 join, 8 decline-party, 9 leave, 10 kick, 11 promote, 12 ready.
//
// Poll cadence (ext): 3 s with the panel up, 2.5 s in a party, 15 s on the menus;
// an action goes first and its reply becomes the fresh state. Never in a match.
//
// Opt-outs: t2r-no-ingame-friends.txt (no button / page / poll), and
// t2r-no-friend-toast.txt (plain one-line toasts instead of the cards).
//
// Honest gates (TODO): the rank table (0xBF / rankbadge.c), the raw lobby object /
// kPartyStatus / g_srchTab / g_srchName producers, exact menu-* pad bindings and the
// card's per-widget keyframes are not ported. The panel, badge and cards resolve their
// widget paths through engine::widget and stay inert when the widget defs are absent;
// the ext still owns those producers while it is the bridge. The party / search state
// itself is fully native (parsed from the daemon) and drives requeue + the search gate.

namespace net::friends13 {

    // Once per frame from boot::FrameTick (after match-liveness, before requeue).
    void Tick();

    // Session reset: drop the state, queued action, toasts and panel typing state.
    void Reset();

    bool Enabled();

    // Party rule, from the daemon snapshot (ext FrInParty / FrPartyMember / FrMyReady).
    bool InParty();
    bool IsMember();       // in a party, not the leader
    bool Ready();          // our own ready bit

    // A non-leader's FIND MATCH is a ready toggle: call instead of searching.
    void MemberReadyPress();

    // Called by the menu driver the moment the engine starts a search. true = block
    // it: a member's own press (toggled here), or the follow-the-leader push that is
    // allowed through with NoteAutoPush().
    bool SearchGate();
    void NoteAutoPush();   // ext g_pqAuto: allow the next search request through

    bool PanelOpen();

    // notice.c kind 'p' lines: the card takes them when it can, else a plain toast.
    bool Toast(const char* text, float secs);
    void NoteSeen(const char* text);

    // Install the notice hooks once (boot). Leaves the other hooks untouched.
    void InstallNoticeHooks();

}
