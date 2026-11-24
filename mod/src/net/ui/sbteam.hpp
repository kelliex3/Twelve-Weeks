#pragma once

#include "sbteam-core.hpp"

#include <cstdint>

// Native owner of tools/ext/sbteam.c. The ext moved winmm's two fixed team
// blocks when our roster team was team 1, because winmm filled the upper block
// with team 0 and the lower with team 1. The native scoreboard already fills
// display team 0 with OUR team (net/ui/net-gui2.cpp::RefreshScoreboard), so this
// owner never repeats the physical swap. It only applies the ext's other half:
// the place-coloured sbBlue / sbRed bands with the stock blue / red bands hidden.
// With an older GUI pak (no sbBlue / sbRed) nothing is touched and the stock
// scoreboard stays as it is. Everything yields while t2r-ext is loaded.
namespace net::ui::sbteam {

    void Tick(uint64_t now);
    bool Active();   // the place-coloured bands are currently owned by this module
    void Reset();

}
