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

    // Test / session helper.
    void Reset();

}
