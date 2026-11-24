#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

// Pure core of tools/ext/killfeed.c: queue, row lifetime / ordering, down-to-kill
// conversion windows and animation curves. The widget / wire side is in
// net/ui/kill-feed-icons.cpp; mod/tests/kill_feed_core_test.cpp covers this header.
namespace net::ui::killfeed::core {

    constexpr int      kRows          = 5;
    constexpr int      kQueue         = 8;
    constexpr uint64_t kLifeMs        = 6000;
    constexpr uint64_t kFadeInMs      = 150;
    constexpr uint64_t kFadeOutMs     = 1000;
    constexpr float    kTop           = 150.0f;
    constexpr float    kPitch         = 84.0f;
    constexpr uint64_t kWaitMs        = 700;
    constexpr uint64_t kWeaponCacheMs = 3000;
    constexpr uint64_t kSelfDownMs    = 500;
    constexpr uint64_t kRedownMs      = 3000;
    constexpr uint64_t kKillBeforeDownMs = 1500;
    constexpr uint64_t kPopMs         = 200;
    constexpr uint64_t kOutMs         = 90;

    enum class Icon : int { Skull = 0, Fist = 1, Weapon = 2 };

    struct Event {
        char     killer[44]{};
        char     victim[44]{};
        bool     haveKiller = false;
        bool     kf = false, vf = false, own = false;
        uint8_t  kslot = 0xFF, vslot = 0xFF;
        uint64_t weapon = 0;
        bool     isDown = false;
        bool     iconKnown = false;
        const char* icon = nullptr;   // resolved weapon / skull / fist image path
        uint64_t waitUntil = 0;
        uint64_t born = 0;
    };

    inline void CopyName(char* out, const char* name) noexcept
    {
        if (!name) { out[0] = '\0'; return; }
        size_t n = 0;
        while (name[n] && n + 1 < 44) { out[n] = name[n]; ++n; }
        out[n] = '\0';
    }

    // ext KfQueue: cap 8, keep the newest event when the queue is full.
    inline bool Push(Event* q, int& n, const Event& e) noexcept
    {
        if (n >= kQueue) {
            std::memmove(q, q + 1, sizeof(Event) * (kQueue - 1));
            --n;
        }
        if (n < 0) n = 0;
        q[n++] = e;
        return true;
    }

    inline bool Waiting(const Event& e, uint64_t now) noexcept
    {
        return e.waitUntil && now < e.waitUntil;
    }

    // A kill may never overtake an earlier same-victim down that is still waiting
    // for its weapon message (ext's 0x94 ordering rule).
    inline bool Blocked(const Event* q, int n, int killIndex, uint64_t now) noexcept
    {
        const Event& k = q[killIndex];
        for (int i = 0; i < n; ++i) {
            const Event& d = q[i];
            if (d.isDown && d.vslot == k.vslot && Waiting(d, now) && d.born <= k.born)
                return true;
        }
        return false;
    }

    inline Icon PickIcon(bool haveKiller, bool iconKnown) noexcept
    {
        if (!haveKiller) return Icon::Skull;
        return iconKnown ? Icon::Weapon : Icon::Fist;
    }

    // ext KfTryConvert refuses to turn a down into a kill if it was downed after
    // the victim's recent kill (or the victim revived in between).
    inline bool CanConvert(bool isDown, uint64_t born, uint64_t killBorn,
                           bool sameAttacker, bool sameVictim, uint64_t closedAt) noexcept
    {
        if (!isDown || !sameAttacker || !sameVictim) return false;
        if (closedAt && closedAt >= born) return false;
        if (killBorn && born >= killBorn && born - killBorn < kKillBeforeDownMs)
            return false;
        return true;
    }

    inline float Alpha(uint64_t now, uint64_t born, bool expired) noexcept
    {
        if (expired || !born || now <= born) return 0.0f;
        const uint64_t age = now - born;
        if (age < kFadeInMs)
            return static_cast<float>(age) / static_cast<float>(kFadeInMs);
        if (age > kLifeMs - kFadeOutMs) {
            const float f = static_cast<float>(kLifeMs - age) / static_cast<float>(kFadeOutMs);
            return f > 0.0f ? f : 0.0f;
        }
        return 1.0f;
    }

    inline float Glide(float y, float target, uint64_t dtMs) noexcept
    {
        if (y < 0.0f) return target;
        const float k = 1.0f - std::exp(-static_cast<float>(dtMs) / 70.0f);
        return y + (target - y) * k;
    }

} // namespace net::ui::killfeed::core
