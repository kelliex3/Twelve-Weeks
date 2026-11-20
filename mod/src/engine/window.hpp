#pragma once

#include <cstdint>

// Ported from tools/ext/t2r-ext.c's GameFocused() and the GetLastInputInfo use in
// tools/ext/afk.c. Two OS-level input facts a feature needs when it wants to know
// whether the *player* (not the game) is idle:
//
//   Focused()        our process owns the foreground window. Alt-tabbed into a
//                    browser counts as idle in the game, exactly like the ext.
//   LastInputTick()  the session's last keyboard / mouse input tick (keys, clicks,
//                    raw mouse movement) from GetLastInputInfo. A caller keeps the
//                    previous value and treats a change as input; it is only input
//                    while Focused() is true (the ext's rule).

namespace engine::window {

    // ext GameFocused: the foreground window belongs to this process.
    bool Focused();

    // GetLastInputInfo().dwTime into *out. false = the call failed (leave the caller's
    // stored tick alone). The value is ms since boot, so 0 is only meaningful at boot.
    bool LastInputTick(uint32_t* out);

    // Raw key / mouse state for menu features that need keys the joypad command layer
    // does not expose (the ext's GetAsyncKeyState use in modepick / gearup / friends13).
    // The caller derives an edge from Down() with its own held bit.
    bool KeyDown(int virtualKey);   // high bit of GetAsyncKeyState, false when unfocused

    // The cursor in 3840x2160 canvas units (the centred 16:9 box of the client area),
    // only while our window is focused. Port of the ext's UiCursorCanvas.
    bool CursorCanvas(float* x, float* y);

}
