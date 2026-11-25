#pragma once

#include "choose-core.hpp"

#include <cstdint>

// Native owner for the build-191 CHOOSE YOUR SIDE screen (tools/ext/choose.c +
// side_tick in t2r-ext.c). The pure stage/focus/payoff machine lives in
// choose-core.hpp; this owner is the widget/input seam.
//
// Yields while t2r-ext.dll is loaded. Camera, mixer/vox and the intro
// cinematic assets are inert seams in this native pass (no camera track
// evaluator, no PCM mixer ownership here); the owner only drives widget state
// that is present in the loaded GUI pak. Read-only sidecars:
// t2r-no-choose-art.txt, t2r-no-choose-intro.txt, t2r-no-choose-vox.txt,
// t2r-no-choose-music.txt.
namespace net::ui::choose {

    void Tick(uint64_t now);
    void Reset();

    bool Active();
    int  Stage();

} // namespace net::ui::choose
