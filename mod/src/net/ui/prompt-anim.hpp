#pragma once

#include "prompt-anim-core.hpp"

#include <cstdint>

// Native owner of tools/ext/prompt_anim.c's self-drawn key prompts. The ext got
// its frames from world_prompt_draw / world_prompt_grey_draw through the winmm
// text hook and from g_wpr / g_wpg (produced by the still-ext-owned melee pullout,
// duel pin and gift modules). Those producers have no native counterpart yet, so
// this module owns the animation core and the HUD pass, and exposes Show() as the
// seam a native producer can call. With no producer it draws nothing; it never
// installs a winmm / VA hook and never samples a second keyboard hook.
//
// `t2r-prompt-anim-old.txt` (read-only) restores the old plain texts by leaving
// the self-draw pass off; `t2r-wp-selfdraw.txt` is the existing opt-in for the
// regular interact prompt slot.
namespace net::ui::prompt_anim {

    // Two slots, as the ext: 0 = grey wait / mash / plain text, 1 = regular
    // interact prompt. A producer passes either a screen anchor (1280x720 text
    // space) or no anchor (fixed clamped fallback), plus progress 0..1.
    void Show(int slot, core::Kind kind, const char* label, float progress,
              float x = -1.0f, float y = -1.0f);

    // World -> screen anchor via the engine's own projection (the same native
    // call music-sight uses). false = behind the camera / unavailable.
    bool Project(float worldX, float worldY, float worldZ,
                 float& x, float& y);

    void Tick(uint64_t now);   // shared pad / key facts for the mash edge and key letter
    void Draw(uint64_t now);   // HUD text pass; no-op while t2r-ext is loaded
    bool Active();             // any slot still visible (for diagnostics / producers)
    void Reset();

}
