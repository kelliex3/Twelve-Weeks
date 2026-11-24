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
// Ported here: the exact menu-* pad bindings (edge / repeat split), the frToast card's
// per-widget keyframes (260 ms slide-in from +170, 220 ms slide-out to +70, alpha in
// 1/32 steps, root pivot at FT_X + qx*0.5), the rank badge's build-191 role path via
// rank-badge.*, and the MATCH FOUND lobby rows / SEARCHING tabs via engine.lobby.* and
// net::social::search_rows.* (g_srchTab / g_srchName mirror, raw kPartyStatus read).
//
// Honest gates: engine / winmm text writes for the SEARCHING rows still pass through
// the ext's pinned settext hook (no second hook); the mirror only observes writes made
// through engine::widget::SetText plus the read-only roster. Lobby object writes, the
// scoreboard's pink rim (no scoreboard settext note) and the own-badge clan 0xE1 role
// stay ext-owned. Every widget path resolves through engine::widget and is inert when
// the widget defs are absent. The party / search state itself is fully native (parsed
// from the daemon) and drives requeue + the search gate.

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

    // The ESC chat block's P FRIENDS prompt uses these instead of a second friends
    // state reader: open the existing page over the match, and share its unread count.
    void OpenFromPause();
    int  UnreadTotal();

    // notice.c kind 'p' lines: the card takes them when it can, else a plain toast.
    bool Toast(const char* text, float secs);
    void NoteSeen(const char* text);

    // Install the notice hooks once (boot). Leaves the other hooks untouched.
    void InstallNoticeHooks();

}
