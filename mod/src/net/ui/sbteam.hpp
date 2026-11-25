#pragma once

#include "sbteam-core.hpp"

#include <cstdint>

// Native owner of build-191 tools/ext/sbteam.c. winmm fills the upper block
// with roster team 0 and the lower with team 1; when our own team is team 1
// the two blocks are swapped by moving every member of each block by the
// measured team-name distance. The state is read back from the pivots, so the
// same code works on the pause page and the scoreboard page. The blue / red
// bands are place-coloured and are never moved. net/ui/net-gui2.cpp fills the
// blocks by roster team as well, so our team ends up on top.
namespace net::ui::sbteam {

    void Tick(uint64_t now);
    bool Active();   // team-name pivots resolved on the current page
    void Reset();

}
