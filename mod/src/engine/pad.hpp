#pragma once

#include <cstdint>

// Ported from tools/ext/padinject.c's PadinjUpdHook -- the *activity* half only.
//
// The ext's pad injection is a much bigger machine (it ORs a virtual button mask and
// sticks into the engine's Pad before Joypad::Update so a dev bot gets a real press
// edge, plus a command-reader hook that learns which input bit each command uses).
// That whole path is unported. This module keeps only the part AFK needs: it watches
// the engine's own Pad -> Joypad pipeline and stamps the last time a *real* pad did
// anything, so an idle player can be told from an active one on any controller
// (DualSense / Steam Input / XInput) -- the same object every gameplay command reads,
// never raw XInput.
//
// A button-mask change (Pad+0x528) or any stick past 0.3 (Pad+0x00..0x1c) counts as
// input; a per-Pad last-mask table means two Pads read on alternate frames (the engine
// calls Joypad::Update from several device paths) never look like a press.
//
// Installed lazily with safetyhook on Joypad::Update (RVA 0x1593350, prologue-checked);
// if the prologue differs (a game update) the hook is not installed and Ready() stays
// false, which keeps every caller (AFK) off rather than guessing.

namespace engine::pad {

    // ext PadinjReady(): the Joypad::Update activity hook is installed and live.
    bool Ready();

    // GetTickCount64() of the last real pad input seen (button edge or stick out of rest).
    // 0 = nothing seen yet. Callers compare against "now" (the ext used a 250 ms window).
    uint64_t ActivityMs();

}
