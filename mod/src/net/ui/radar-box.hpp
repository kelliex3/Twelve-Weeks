#pragma once

#include "radar-box-core.hpp"

#include <cstdint>

// Native owner of tools/ext/radar_box.c. The ext detoured winmm's radar blip
// call to capture supply-box centres and placed pooled t2rRbx* widgets; the mod
// already draws its own radar from NetPlayers::supplyBoxes, so this owner takes
// those centres directly. No winmm call patch, no VA detour: BeginFrame resolves
// the pooled widgets, FeedBlip captures a centre (and suppresses the mod's own
// glyph), EndFrame writes the widgets. Everything yields while t2r-ext is loaded.
//
// Floor arrows / rim arcs are pak extras; without the corresponding widgets the
// owner degrades to plain crates / pinned crates, as the ext did with an old pak.
namespace net::ui::radar_box {

    void BeginFrame(uint64_t now);   // call just before DrawRadar()
    bool FeedBlip(float x, float y, bool pinned,
                  core::Floor floor = core::Floor::Same);  // true = glyph suppressed

    // Native supply-box feed: the owner resolves the box's floor from its world
    // Y against the player's feet with the ext's 2.0 / 1.85 m hysteresis, keyed
    // by box SID so a re-sorted box list never mixes two boxes' states. The
    // read-only t2r-no-box-floor.txt opt-out is honoured.
    bool FeedBlip(float x, float y, bool pinned, uint64_t boxSid,
                  float boxY, float playerY);

    void EndFrame(uint64_t now);     // call just after DrawRadar()
    bool Active();                   // widgets own the blips (disc / dots suppressed)
    void Reset();

}
