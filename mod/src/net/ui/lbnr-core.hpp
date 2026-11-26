#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Pure row / cadence / name rules for tools/ext/lbnr.c (build 191). The native
// owner in net-gui2.cpp reads the native leaderboard data and LocalName(); the
// ext settext hook and its g_ui_gen counter are not needed by the port.

namespace net::ui::lbnr::core {

    inline constexpr int   kRows = 10;
    inline constexpr int   kNameCap = 16;       // ext mfnr's cached local name
    inline constexpr uint64_t kResolveMs = 3000;
    inline constexpr uint64_t kOwnMs = 250;
    inline constexpr float kChipX = 1700.0f;
    inline constexpr float kChipPt = 25.5f;

    [[nodiscard]] inline char AsciiLower(char c) noexcept
    {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }

    inline bool EqualsNoCase(const char* a, const char* b) noexcept
    {
        if (!a || !b) return false;
        while (*a && *b) {
            if (AsciiLower(*a) != AsciiLower(*b)) return false;
            ++a; ++b;
        }
        return *a == 0 && *b == 0;
    }

    // ext mfnr_is_me(): exact match or a "[TAG] name" suffix after ' ' / ']'.
    [[nodiscard]] inline bool IsMe(const char* rowName, const char* local) noexcept
    {
        if (!rowName || !rowName[0] || !local || !local[0]) return false;
        const size_t a = std::strlen(rowName), b = std::strlen(local);
        if (a == b) return EqualsNoCase(rowName, local);
        if (a <= b + 1) return false;
        const char sep = rowName[a - b - 1];
        return (sep == ' ' || sep == ']') && EqualsNoCase(rowName + (a - b), local);
    }

    [[nodiscard]] inline bool ShouldResolve(uint64_t now, uint64_t lastAt,
                                            unsigned generation,
                                            unsigned seenGeneration,
                                            bool up) noexcept
    {
        return !up || generation != seenGeneration
            || lastAt == 0 || now - lastAt >= kResolveMs;
    }

    [[nodiscard]] inline bool ShouldRefreshOwn(uint64_t now, uint64_t lastAt) noexcept
    {
        return lastAt == 0 || now - lastAt >= kOwnMs;
    }

    inline bool RowPath(char* out, int cap, int row) noexcept
    {
        if (!out || cap <= 0 || row < 0 || row >= kRows) return false;
        return std::snprintf(out, static_cast<size_t>(cap),
                             "anim-group.table.row%d_onlineid", row) < cap;
    }

    inline bool HighlightPath(char* out, int cap, int row) noexcept
    {
        if (!out || cap <= 0 || row < 0 || row >= kRows) return false;
        return std::snprintf(out, static_cast<size_t>(cap),
                             "anim-group.table.lbMe%d", row) < cap;
    }

    inline bool ChipPath(char* out, int cap, int row) noexcept
    {
        if (!out || cap <= 0 || row < 0 || row >= kRows) return false;
        return std::snprintf(out, static_cast<size_t>(cap),
                             "anim-group.table.lbMe%d.lbMe%dYo", row, row) < cap;
    }

} // namespace net::ui::lbnr::core
