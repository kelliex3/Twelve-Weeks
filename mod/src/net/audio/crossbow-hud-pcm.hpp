#pragma once

// Guarded private PCM sink for the crossbow bolt HUD sounds (tools/ext/alert_sting.c
// 169-200, xbow_hud.h / xbow_hud.pcm). The three waves are game-derived and do not
// ship here, so every read of the blob lives in this header behind
// TO1_XBOW_HUD_PCM, exactly like near-miss-pcm.hpp.
//
// Build with -DTO1_XBOW_HUD_PCM and supply a private
// net/audio/xbow_hud_pcm_data.hpp declaring the following UNQUALIFIED names. The
// header is included below from inside the already-open `pcm` namespace, so it
// must NOT wrap them in another namespace:
//   extern const engine::pcm::Sample kBlob[];   // all three waves, back to back
//   inline constexpr int kImpactOff = ...;      // ext XBI_OFF
//   inline constexpr int kImpactFrames = ...;   // ext XBI_N
//   inline constexpr int kPulseOff = ...;       // ext XBP_OFF
//   inline constexpr int kPulseFrames = ...;    // ext XBP_N
//   inline constexpr int kClearOff = ...;       // ext XBC_OFF
//   inline constexpr int kClearFrames = ...;    // ext XBC_N
//   inline constexpr int kRate = ...;           // ext XBI/XBP/XBC_RATE (shared)
//
// engine::pcm::Play applies the SFX slider itself, so the owner passes the ext's
// raw 0.8 / 0.55 gains. Without the macro no voice starts, the pulse cooldown and
// hit_seen are not consumed and no log claims playback. The owner is the native
// sole owner of this sub-feature when the ext is absent; the ext is yielded via
// boot::ExtLoaded().

#include "crossbow-hud-core.hpp"

#include "engine/pcm.hpp"

namespace net::audio::crossbow_hud::pcm {

#if defined(TO1_XBOW_HUD_PCM)
#  include "xbow_hud_pcm_data.hpp"

    inline constexpr bool kHave = true;

    enum class Wave { kImpact, kPulse, kClear };

    inline const engine::pcm::Sample* Ptr(Wave w) noexcept
    {
        switch (w) {
        case Wave::kImpact: return kBlob + kImpactOff;
        case Wave::kPulse:  return kBlob + kPulseOff;
        case Wave::kClear:  return kBlob + kClearOff;
        }
        return nullptr;
    }

    inline int Frames(Wave w) noexcept
    {
        switch (w) {
        case Wave::kImpact: return kImpactFrames;
        case Wave::kPulse:  return kPulseFrames;
        case Wave::kClear:  return kClearFrames;
        }
        return 0;
    }

    inline uint64_t PulsePeriodMs() noexcept
    {
        return core::PulsePeriodMs(kPulseFrames, kRate);
    }

    inline engine::pcm::Voice Play(Wave w, float gain)
    {
        const engine::pcm::Sample* p = Ptr(w);
        const int n = Frames(w);
        if (!p || n <= 0 || kRate <= 0 || gain <= 0.0f)
            return {};
        return engine::pcm::Play(p, n, gain, gain, kRate);  // sfx volume applied here
    }

#else

    inline constexpr bool kHave = false;
    inline constexpr uint64_t kPulsePeriodMs = 0;

    enum class Wave { kImpact, kPulse, kClear };
    inline uint64_t PulsePeriodMs() noexcept { return kPulsePeriodMs; }
    inline engine::pcm::Voice Play(Wave, float) { return {}; }

#endif

} // namespace net::audio::crossbow_hud::pcm
