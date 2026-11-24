#pragma once

#include <cmath>
#include <cstdint>

// Pure timing / easing core of tools/ext/loadseq.c (build-201 working tree).
// The stack, widget, PCM and menu-input side lives in menu/loadseq.cpp.
namespace menu::loadseq::core {

    constexpr uint64_t kBlackMs       = 400;
    constexpr uint64_t kTitleAtMs     = 700;
    constexpr uint64_t kTitleMs       = 2600;
    constexpr uint64_t kBarAtMs       = 1600;
    constexpr uint64_t kBarMs         = 1200;
    constexpr uint64_t kHoldMs        = 300;
    constexpr uint64_t kOutMs         = 700;
    constexpr uint64_t kResumeOutMs   = 1600;
    constexpr uint64_t kMuteMs        = 1500;
    constexpr uint64_t kMusicRampMs   = 2000;
    constexpr uint64_t kFillTauMs     = 9000;
    constexpr float    kFillCap       = 0.92f;
    constexpr float    kFillRate      = 12.0f / 1000.0f;  // per ms
    constexpr float    kFillOutRate   = 60.0f / 1000.0f;
    constexpr float    kFillEps       = 0.002f;

    inline float Clamp01(float u) noexcept
    {
        return u <= 0.0f ? 0.0f : u >= 1.0f ? 1.0f : u;
    }

    // ext LdsSmooth: clamped smootherstep (zero first and second derivative at both ends).
    inline float Smooth(float u) noexcept
    {
        u = Clamp01(u);
        return u * u * u * (u * (u * 6.0f - 15.0f) + 10.0f);
    }

    inline float Fade(uint64_t now, uint64_t t0, uint64_t atMs, uint64_t ms) noexcept
    {
        if (!t0 || now < t0 || now - t0 < atMs) return 0.0f;
        return Smooth(static_cast<float>(now - t0 - atMs) / static_cast<float>(ms));
    }

    inline float Target(uint64_t elapsed) noexcept
    {
        if (!elapsed) return 0.0f;
        const float v = kFillCap * (1.0f - std::exp(-static_cast<float>(elapsed) / static_cast<float>(kFillTauMs)));
        return v > kFillCap ? kFillCap : v;
    }

    // The ext's monotonic fill: never move backwards, faster during the fade-out,
    // and go all the way to 1 once the title starts leaving.
    inline float Advance(float v, uint64_t elapsed, uint64_t dt, bool fadingOut) noexcept
    {
        float target = fadingOut ? 1.0f : Target(elapsed);
        if (target > 1.0f) target = 1.0f;
        const float maxStep = (fadingOut ? kFillOutRate : kFillRate) * static_cast<float>(dt);
        if (target > v) {
            const float want = target - v;
            v += want < maxStep ? want : maxStep;
        }
        if (v > 1.0f) v = 1.0f;
        return v;
    }

    inline float BottomBarScaleX(float v) noexcept { return v * 3840.0f / 2800.0f; }
    inline constexpr float kBarScaleY = 24.0f / 760.0f;

    inline bool ReadyForFade(uint64_t elapsed, bool worldReady, uint64_t& lastCheck, uint64_t now) noexcept
    {
        if (!worldReady || elapsed < kTitleAtMs + kTitleMs) return false;
        if (lastCheck && now - lastCheck < 250) return false;
        lastCheck = now;
        return true;
    }

    inline float OutMultiplier(uint64_t now, uint64_t outAt) noexcept
    {
        if (!outAt || now < outAt) return 1.0f;
        return 1.0f - Smooth(static_cast<float>(now - outAt) / static_cast<float>(kOutMs));
    }

    inline float ResumeMultiplier(uint64_t now, uint64_t outAt) noexcept
    {
        if (!outAt || now < outAt) return 1.0f;
        return 1.0f - Smooth(static_cast<float>(now - outAt) / static_cast<float>(kResumeOutMs));
    }

    inline float SessionVolume(uint64_t now, uint64_t muteAt, bool fadingIn) noexcept
    {
        if (!muteAt) return 1.0f;
        const uint64_t dt = now >= muteAt ? now - muteAt : 0;
        if (!fadingIn)
            return 1.0f - Clamp01(static_cast<float>(dt) / static_cast<float>(kMuteMs));
        return Clamp01(static_cast<float>(dt) / static_cast<float>(kMusicRampMs));
    }

} // namespace menu::loadseq::core
