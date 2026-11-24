#pragma once

#include <cmath>
#include <cstdint>

// Pure scoreboard-placement core of tools/ext/sbteam.c. The ext moved winmm's
// two fixed team blocks 474 canvas units when our roster team was team 1. The
// native scoreboard already fills display team 0 with OUR team (net-gui2.cpp),
// so the native owner deliberately omits the physical move and only owns the
// place-coloured bands. These helpers keep the ext rule and the native rule
// explicit and testable.
namespace net::ui::sbteam::core {

    inline constexpr float kBlockOffset = 474.0f;
    inline constexpr float kBaseSplit   = 1250.0f;
    inline constexpr int   kRows        = 4;

    inline constexpr const char* const kBands[] = { "band", "name", "col_parts",
        "col_downs", "col_executions", "col_deaths", "col_revives" };
    inline constexpr const char* const kCells[] = { "bg", "name", "parts",
        "downs", "executions", "deaths", "revives", "ping", "input",
        "rkIcon", "rkNum" };

    // The ext rule: a team-1 player needs the blocks swapped.
    inline bool WantsPhysicalSwap(int ownTeam) noexcept { return ownTeam == 1; }

    // The native rule: FillScoreboardTeam(0, myTeam) already puts us on top.
    inline bool NativeFillOwnTeamOnTop(int ownTeam, int displayTeam0Team) noexcept
    {
        return ownTeam >= 0 && displayTeam0Team == ownTeam;
    }

    // Absolute, repeatable placement used by the ext (SbtMove), kept as a pure
    // function for tests / diagnostics. The native port never calls it.
    inline float ExtBlockY(int team, float y, bool swap) noexcept
    {
        const float base = team == 0
            ? (y >= kBaseSplit ? y - kBlockOffset : y)
            : (y < kBaseSplit ? y + kBlockOffset : y);
        const float want = swap
            ? (team == 0 ? base + kBlockOffset : base - kBlockOffset)
            : base;
        return want;
    }

    inline bool NeedsMove(float want, float y) noexcept
    {
        return std::fabs(want - y) > 0.5f;
    }

    // Colours belong to the PLACE, so they never follow the roster team.
    inline bool PlaceColoursFixed(int ownTeam) noexcept
    {
        (void)ownTeam;
        return true;
    }

} // namespace net::ui::sbteam::core
