#pragma once

#include <cstdint>

// Ported from tools/ext/t2r-ext.c's local-player helpers (kFnLocalPlayer, SglScoped's scoped
// byte read, and LocalHp). The local player object and the few per-frame reads a feature needs
// off it, kept out of the net layer so any engine-side feature can use them.

namespace engine::local_player {

    // The local player object. 0 when there is no player yet (front-end / between levels).
    uintptr_t Ptr();

    // The engine's own scoped-in state: player -> aim controller (Player+0x2F68) -> +0x698.
    // INFERRED offset (ext disassembly of the shipped exe); true only for a 0/1 byte.
    bool ScopedIn();

    // player-is-aiming? (the engine's script call), low byte.
    bool IsAiming();

    // get-player-health decoded: a float when the low dword looks like one, else an int.
    // -1.0f = the call failed / no answer.
    float Health();

}
