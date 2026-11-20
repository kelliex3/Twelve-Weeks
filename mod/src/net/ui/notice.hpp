#pragma once

#include <cstdint>

// Ported from tools/ext/notice.c: server notices polled every ~6 s on a short TCP
// connection -- "New update available", "Server restarting in N min", the final
// "Server is shutting down", and free text from the admin site, shown as a toast.
//
//   mod -> daemon  0xBA [64B token][u8 ver=1][u8 lang 0 en / 1 pl][u8 flags: 1 = this
//                        game has the in-game FRIENDS panel: request / invite lines
//                        then say "Press P"]
//   daemon -> mod  0xBB "n=<k>\n<id>|<kind>|<param>|<text>\n..."
//
// kind u update (id 0) / r restart (param = seconds left) / s shutting down / t free
// text / p friends + party line (param = seconds on screen). The client renders its own
// text from kind + param (English, or Polish when the language is Polish). A restart
// countdown is shown when it first arrives and again at 5 / 2 / 1 min. An older server
// never answers 0xBA: after 3 unanswered tries the mod stops asking. t2r-no-notice.txt
// next to the exe = off.
//
// The ext's styled notify widgets (notify.c) and its cross-feature toasts (friends13 /
// kicknote) are reached through the optional Hooks below, so this compiles and runs
// standalone; a hook left nullptr keeps the plain-toast path.

namespace net::notice {

    struct Hooks {
        // friends13: the friend / party card takes a kind 'p' line; true = consumed.
        bool (*friendsToast)(const char* text, float secs) = nullptr;
        // friends13: a friends / party line was shown or queued.
        void (*friendsNoteSeen)(const char* text) = nullptr;
        // friends13: the in-game FRIENDS panel exists (the server words invites "press P").
        bool (*friendsEnabled)() = nullptr;
        // kick state: is this line an admin kick / team move? (net::match::kick)
        bool (*isKick)(const char* text) = nullptr;
        // kicknote: route the kick to the MP-menu notice box; true = taken.
        // Not ported: this hook stays null and the kick keeps its toast.
        bool (*kickSet)(const char* text) = nullptr;
        // kick state: GetTickCount64() of the last kick text (0 = none).
        uint64_t (*kickShownAt)() = nullptr;
    };

    // Merges: a null field keeps its previous value, so each feature installs only
    // its own hooks and a later install cannot clear another feature's hooks.
    void SetHooks(const Hooks& hooks);

    // Once per frame (boot::FrameTick).
    void Tick();

    // Test / level-reload helper.
    void Reset();

}
