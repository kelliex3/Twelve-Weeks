#pragma once

#include <cstdint>

// Pure core of tools/ext/launchnote.c. The GUI / page side lives in
// launch-notice.cpp; this header is shared with mod/tests/launch_notice_core_test.cpp.
namespace net::ui::launch_notice::core {

    constexpr int      kVersion     = 1;
    constexpr uint64_t kReadMs      = 10000;
    constexpr uint64_t kFadeMs      = 300;
    constexpr uint64_t kRetryMs     = 1500;
    constexpr uint64_t kHoldMaxMs   = 4000;
    constexpr int      kMaxPushes   = 3;
    constexpr int      kRefillFrames = 10;

    enum class Stage : int { Unknown = 0, Due = 1, Pushed = 2, Up = 3, Done = 4 };

    inline bool Seen(int storedVersion) noexcept { return storedVersion >= kVersion; }

    inline bool Busy(Stage st) noexcept
    {
        const int v = static_cast<int>(st);
        return v >= static_cast<int>(Stage::Due) && v <= static_cast<int>(Stage::Up);
    }

    struct Lock {
        uint64_t elapsed = 0;
        bool     locked  = true;
        int      leftSec = static_cast<int>(kReadMs / 1000);
    };

    // The reading clock only starts once the TWELVE WEEKS title is gone.
    inline Lock ReadLock(uint64_t now, uint64_t readAt) noexcept
    {
        Lock r;
        if (!readAt) return r;
        r.elapsed = now >= readAt ? now - readAt : 0;
        r.locked = r.elapsed < kReadMs;
        r.leftSec = r.locked
            ? static_cast<int>((kReadMs - r.elapsed + 999) / 1000)
            : 0;
        return r;
    }

    // ext LnHoldFade: hold the title's hand-off fade while the notice is due /
    // pushed, at most 4 s so a page that never arrives cannot keep the title up.
    inline bool HoldFade(Stage st, uint64_t now, uint64_t& holdAt) noexcept
    {
        if (st != Stage::Due && st != Stage::Pushed) return false;
        if (!holdAt) holdAt = now ? now : 1;
        return now - holdAt < kHoldMaxMs;
    }

    inline bool ShouldPush(Stage st, uint64_t now, uint64_t pushedAt, int pushes) noexcept
    {
        if (st != Stage::Due && st != Stage::Pushed) return false;
        if (st == Stage::Pushed) {
            if (now <= pushedAt || now - pushedAt <= kRetryMs) return false;
            if (pushes >= kMaxPushes) return false;
        }
        return true;
    }

    inline bool DropPushes(Stage st, uint64_t now, uint64_t pushedAt, int pushes) noexcept
    {
        return st == Stage::Pushed && pushedAt && now > pushedAt
            && now - pushedAt > kRetryMs && pushes >= kMaxPushes;
    }

    inline float Ramp(uint64_t now, uint64_t t0, uint64_t ms) noexcept
    {
        if (!t0 || now <= t0) return 0.0f;
        const float f = static_cast<float>(now - t0) / static_cast<float>(ms);
        return f < 1.0f ? f : 1.0f;
    }

    inline int FocusMove(bool ticked, int focus, bool up, bool down, bool left, bool right) noexcept
    {
        if (!ticked) return 0;
        if (up && focus > 0) return 0;
        if (down && focus == 0) return 1;
        if (left && focus == 2) return 1;
        if (right && focus == 1) return 2;
        return focus;
    }

    inline float TimerScale(const Lock& l) noexcept
    {
        if (!l.elapsed) return 1.0f;
        if (l.locked) {
            const float f = 1.0f - static_cast<float>(l.elapsed) / static_cast<float>(kReadMs);
            return f < 0.0f ? 0.0f : f;
        }
        return 0.0f;
    }

} // namespace net::ui::launch_notice::core
