#pragma once

#include <cmath>
#include <cstdint>

// Pure selection / timing rules for the native producers that feed
// net::ui::prompt_anim::Show(). No engine or game headers: standalone-testable.
//
// The ext's grey wait ring used `prog` as the elapsed share of the wait, capped
// at 0.99 so the ring never quite reaches the key before the window opens
// (tools/ext/melee_duel_pin.c, melee_pullout.c). The native melee-dodge core has
// no separate lead-in phase: its window is open as soon as a peer swing is
// recorded, so the same curve is used as a countdown to the window's deadline
// -- the ring contracts onto the key as the dodge chance runs out.
namespace net::ui::prompt_producer::core {

    inline constexpr float    kWaitCap       = 0.99f;
    inline constexpr float    kInteractReach = 1.5f;   // ext / native supply-box reach, metres

    // Elapsed share of a window of `winMs` that ends at `end`. 0 at first sight,
    // kWaitCap as the deadline arrives. Independent of clock epoch: now/end are
    // both in the melee core's engine::frame::NowMs() domain.
    inline float WaitFill(uint64_t now, uint64_t end, uint16_t winMs) noexcept
    {
        if (!end || !winMs) return 0.0f;
        if (now >= end) return kWaitCap;
        const uint64_t remain = end - now;
        const uint64_t span = winMs;
        const uint64_t elapsed = remain >= span ? 0ull : span - remain;
        const float f = static_cast<float>(elapsed) / static_cast<float>(span);
        return f < 0.0f ? 0.0f : f > kWaitCap ? kWaitCap : f;
    }

    // The native supply-box bridge shows an interact prompt only for an
    // openable box inside the 1.5 m reach, and never while aiming
    // (net/match/net-player.cpp::UpdateSupplyBoxesInteractBridge).
    inline bool SupplyPrompt(float distanceM, bool aiming, bool openable) noexcept
    {
        return openable && !aiming && std::isfinite(distanceM) &&
               distanceM >= 0.0f && distanceM <= kInteractReach;
    }

} // namespace net::ui::prompt_producer::core
