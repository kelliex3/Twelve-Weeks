#pragma once

#include <cmath>
#include <cstdint>

// Pure scoreboard-placement core of build-191 tools/ext/sbteam.c. winmm fills
// the two blocks by roster team; the ext swaps them when the local player is
// on team 1. It reads the CURRENT state from the two team-name pivots (the
// pause page and the scoreboard page have different y), so the move is
// relative, idempotent and works on any page of that layout. Colour follows
// the place: the blue / red bands are never moved by this rule.
namespace net::ui::sbteam::core {

    inline constexpr int   kRows          = 4;
    inline constexpr float kMinDistance   = 50.0f;

    // sbt_swap's leaf sets (sbteam.c:74-75): the fixed band / column header
    // widgets are NOT part of a block; the tag, rule, totals and own-row bits
    // are.
    inline constexpr const char* const kBlockParts[] = {
        "name", "tag", "rl", "tot_downs", "tot_executions", "tot_revives",
        "tot_deaths", "tot_parts"
    };
    inline constexpr const char* const kRowParts[] = {
        "bg", "name", "parts", "downs", "executions", "deaths", "revives",
        "ping", "input", "rkIcon", "rkNum", "me", "tag"
    };

    // ext: lo[0x1d + i*0x32 + 0x31] & 1
    inline bool WantsPhysicalSwap(int team) noexcept
    {
        return (team & 1) != 0;
    }

    struct SwapRead {
        bool  valid = false;
        bool  swapped = false;   // team 0's name is below team 1's
        float distance = 0.0f;
    };

    inline SwapRead ReadSwap(float y0, float y1) noexcept
    {
        SwapRead r;
        r.valid = std::isfinite(y0) && std::isfinite(y1);
        r.swapped = y0 > y1 + 1.0f;
        r.distance = std::fabs(y1 - y0);
        return r;
    }

    inline bool NeedsSwap(const SwapRead& r, bool want) noexcept
    {
        return r.valid && r.distance >= kMinDistance && r.swapped != want;
    }

    inline float ShiftDelta(const SwapRead& r, bool want, int team) noexcept
    {
        const float dir = (want ? 1.0f : 0.0f) - (r.swapped ? 1.0f : 0.0f);
        return dir * r.distance * (team == 0 ? 1.0f : -1.0f);
    }

} // namespace net::ui::sbteam::core
