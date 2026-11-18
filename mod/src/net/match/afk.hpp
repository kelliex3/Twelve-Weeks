#pragma once

#include <cstdint>

// Ported from tools/ext/afk.c: the anti-AFK watchdog. IDLE = no real input from the
// player -- no key, no mouse (move / look / click / wheel), no pad button, no stick.
// Counted only while it matters: in a running match that is not over, alive (not on the
// death / spectate screen, not downed), not in a menu that is part of play (the
// backpack / store, the in-match GEAR UP). Those moments freeze the count (an AFK player
// who dies and respawns keeps the idle time he had); any input resets it to 0. A new
// match starts at 0.
//
// Input sources:
//   - keyboard + mouse: engine::window::LastInputTick (the session's last hardware input
//     tick), counted only while engine::window::Focused();
//   - any pad: engine::pad::ActivityMs (the engine's own Pad -> Joypad pipeline);
//   - the dev bots' injected input: NOT ported (the bot / injection path is unported).
//
// Without the pad hook the whole feature stays off (a pad player could not be told from
// an idle one), matching the ext.
//
// At kWarnMs idle: a centred warning with a countdown. The countdown runs only while the
// warning can be SEEN (a menu over the HUD hides every text draw, so there the count
// stops at the warning). The reported idle rides on the JIP 0xE3 clock report
// (ReqTail): [0xAF][u16 idle s][flags bit0 counting, bit1 warning up]; the daemon
// removes the player at T2R_AFK_KICK_S (default 90 s). Off: t2r-no-afk.txt next to the
// exe (idle reported as 0xFFFF = unknown: the daemon never kicks).

namespace net::afk {

    // Once a frame, from boot::FrameTick.
    void Tick();

    // Once a frame from the HUD draw block (in-match only). Draws the warning.
    void Draw();

    // The 0xE3 clock-report trailer. `p` must have room for 4 bytes. Writes
    // [0xAF][u16 idle seconds, 0xFFFF = unknown / off][flags].
    void ReqTail(uint8_t* p);

    // Test / level-reload helper: clear the idle / warning state.
    void Reset();

}
