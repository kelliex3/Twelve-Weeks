#pragma once

#include <cstdint>

// Native producers for net::ui::prompt_anim::Show(). The ext got its two prompt
// records (g_wpg / g_wpr) from the winmm text hook and from its own melee pullout,
// duel pin and gift modules. Those facts now arrive from the native pull-out and
// gift bridges; the duel-pin producer is still a pure state machine awaiting a
// paired-move feed. This module reads only facts the mod already owns:
//
//   slot 0 (grey key + contracting ring): the melee-dodge window, plus the
//     pullout / pin wait / early / late state machines when their bridges feed;
//   slot 1 (plain interact prompt, behind the existing t2r-wp-selfdraw.txt
//     opt-in): the nearest openable supply box, a gift in hand, a revive hold,
//     and the counter's "ESCAPE!" press.
//
// No new hook, no VA detour, no second input sampler, no wire or toggle
// change. Everything is inert while t2r-ext.dll is loaded. Call once per frame
// just before prompt_anim::Draw() so the producer sees this frame's net state.
namespace net::ui::prompt_producer {

    void Tick(uint64_t now);   // `now` is the HUD clock (GetTickCount64)

    void Reset();              // session reset: drop all window / linger state

    // ---- feeds from the native bridges ----------------------------------------------------
    // The label state machines are pure; the runtime facts come from the native
    // pull-out bridge (NotePullout, each frame) and the native gift bridge
    // (NoteGiftCarried / SetGiftPeerRoom). Both bridges no-op while t2r-ext.dll is
    // loaded, so their labels are honestly unfed then -- no guessed state, no new
    // wire opcode or toggle.

    void NotePullout(int peer, bool on, uint64_t openMs, uint64_t endMs);

    void NoteGiftCarried(int kind);   // -1 = no gift, 0..3 = ext GIFT_K index

    void SetGiftPeerRoom(int peer, bool known, uint8_t roomMask);

}
