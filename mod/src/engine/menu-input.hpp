#pragma once

#include <cstdint>

#include "sid/sid.hpp"

// One shared sampler for the menu input facts the modal notice pages need. The ext
// read these directly from GetAsyncKeyState / a per-page held flag; the mod samples
// them once per frame in boot::FrameTick and every consumer reads the snapshot.
// No OS keyboard hook is installed -- this is the same hook-free input path chat and
// friends13 already use.
//
// `ClickEdge()` must be sampled even while no notice page is up: a button held
// across a page opening must not count as a click (ext g_clickEdge behaviour).
namespace engine::menu_input {

    // Call once per frame, before any feature Tick().
    void BeginFrame(uint64_t now);

    // Left-button rising edge of this frame.
    bool ClickEdge();

    // ext UiSelectIsMouse(): menu-select came from the mouse (LB down, or released
    // in the last 350 ms) rather than Space / Return.
    bool SelectIsMouse();

    // ext MenuEdge(): rising edge of a joypad command. `held` is the per-call-site
    // edge latch (initialise it to true so a key already held on page entry does
    // not fire an action).
    bool CommandEdge(StringId64 command, bool& held);

    // ext gu_rep() (gearup13.c): the first press fires at once, then a held
    // command repeats after 260 ms and every 75 ms. `next` is the caller's
    // absolute next-fire tick; initialise it to ~0ULL on page entry. The `now`
    // passed by boot::FrameTick is the same GetTickCount64 clock the ext used.
    bool CommandActive(StringId64 command);
    bool CommandRepeat(StringId64 command, bool& held, uint64_t& next, uint64_t now);

    // ext wheel_notches(): whole wheel notches since the previous BeginFrame,
    // positive = up. The hook is thread-local (WM_MOUSEWHEEL / raw WM_INPUT on the
    // foreground game window), not the system-wide WH_MOUSE_LL hook.
    int WheelNotches();

    // ext pad_state()'s raw XInput bits for the first connected pad, sampled once
    // per BeginFrame. XINPUT_GAMEPAD_LEFT_THUMB is 0x0040, RIGHT_THUMB 0x0080;
    // L3 + R3 must keep meaning "freecam" to the features that read both.
    uint16_t PadButtons();
    bool RightStick(float& x, float& y);   // -1..1, false when no pad / not focused
    bool LeftStick(float& x, float& y);    // -1..1, y > 0 = forward (freecam fly)
    bool Triggers(float& lt, float& rt);   // 0..1, false when no pad / not focused

    // ext g_pad_mode: the last device used (true = pad). Picker / customize
    // prompts use it to choose between key and glyph labels.
    bool PadMode();

    // Test / session helper.
    void Reset();

}
