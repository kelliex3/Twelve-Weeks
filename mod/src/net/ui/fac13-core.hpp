#pragma once

#include <cstdio>

// Pure geometry / text rules for tools/ext/fac13ui.c (build 191). The native
// owner in postmatch-screens.cpp owns widget resolution, alpha/pivot writes and
// the locale; nothing here touches the engine or a widget pak.

namespace net::ui::fac13::core {

    inline constexpr int    kDays = 84;
    inline constexpr float  kPipStep = 8.0f;
    inline constexpr float  kWeekStep = 68.0f;
    inline constexpr float  kPipX0 = 3700.0f - 12.0f * kWeekStep + 12.0f;
    inline constexpr float  kPipY = 198.0f;
    inline constexpr float  kPipOn = 230.0f / 255.0f;
    inline constexpr float  kPipOff = 40.0f / 255.0f;

    [[nodiscard]] inline int ClampDay(int day) noexcept
    {
        return day < 0 ? 0 : day >= kDays ? kDays - 1 : day;
    }

    [[nodiscard]] inline int ClampSurvived(int survived) noexcept
    {
        return survived < 0 ? 0 : survived > kDays ? kDays : survived;
    }

    // ext f13_pip_x(): 12 week columns, 7 day ticks per column.
    [[nodiscard]] inline float PipX(int k) noexcept
    {
        const int d = ClampDay(k);
        return kPipX0 + (d / 7) * kWeekStep + (d % 7) * kPipStep;
    }

    [[nodiscard]] inline bool PipOn(int k, int survived) noexcept
    {
        return k >= 0 && k < ClampSurvived(survived);
    }

    [[nodiscard]] inline bool TodayVisible(int survived) noexcept
    {
        return ClampSurvived(survived) < kDays;
    }

    [[nodiscard]] inline float TodayX(int survived) noexcept
    {
        return PipX(TodayVisible(survived) ? survived : kDays - 1);
    }

    [[nodiscard]] inline float TodayY() noexcept { return kPipY - 4.0f; }

    [[nodiscard]] inline bool IconW(int fac) noexcept { return fac != 2; }
    [[nodiscard]] inline bool IconS(int fac) noexcept { return fac == 2; }

    inline bool WeekDay(char* out, int cap, int day) noexcept
    {
        if (!out || cap <= 0) return false;
        const int d = ClampDay(day);
        return std::snprintf(out, static_cast<size_t>(cap), "WEEK %d: DAY %d",
                             d / 7 + 1, d % 7 + 1) < cap;
    }

    inline bool Survived(char* out, int cap, int survived) noexcept
    {
        if (!out || cap <= 0) return false;
        return std::snprintf(out, static_cast<size_t>(cap),
                             "DAYS SURVIVED %d | 84",
                             ClampSurvived(survived)) < cap;
    }

    inline bool PipPath(char* out, int cap, int k) noexcept
    {
        if (!out || cap <= 0 || k < 0 || k >= kDays) return false;
        return std::snprintf(out, static_cast<size_t>(cap),
                             "anim-group.f13Pip%d", k) < cap;
    }

    // Event page label. EXT's "EVENT ART TEST" is a debug gate that the native
    // owner does not expose, so the shipped label set is mode-based only.
    [[nodiscard]] inline const char* EventLabel(int mode) noexcept
    {
        return mode == 1 ? "INCOMING EVENT" : "EVENT RESULT";
    }

} // namespace net::ui::fac13::core
