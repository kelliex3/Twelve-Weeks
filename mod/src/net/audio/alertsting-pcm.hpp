#pragma once

// Guarded private PCM sink for the alert sting (tools/rip_alert_sting.py ->
// alert_sting.h / alert_sting.pcm). The wave is game-derived and does not ship
// here, so the whole playback path lives in this header behind
// TO1_ALERT_STING_PCM, exactly like menu/sendoff-pcm.hpp.
//
// Build with -DTO1_ALERT_STING_PCM and supply a private
// net/audio/alert_sting_pcm_data.hpp declaring the following UNQUALIFIED names.
// The header is included below from inside the already-open `pcm` namespace, so
// it must NOT wrap them in another namespace:
//   extern const engine::pcm::Sample kLeft[];   // first channel
//   extern const engine::pcm::Sample kRight[];  // second channel
//   inline constexpr int kFrames = ...;         // ext AL_N
//   inline constexpr int kRate = ...;           // ext AL_RATE
// A tiny shim over the generator's contiguous `als_blob` / AL_N / AL_RATE works:
// kLeft = als_blob, kRight = als_blob + AL_N.
//
// Without the macro the sink is inert: the state machine, wire path and logs
// stay live, but no voice starts and no gap clock is consumed.

#include "engine/pcm.hpp"

namespace net::audio::alertsting::pcm {

#if defined(TO1_ALERT_STING_PCM)
#  include "alert_sting_pcm_data.hpp"

    inline constexpr bool kHave = true;

    inline bool Play(float gain, engine::pcm::Voice& l, engine::pcm::Voice& r)
    {
        return engine::pcm::PlayStereoRaw(kLeft, kRight, kFrames, gain, kRate, l, r);
    }

    inline double Seconds() noexcept
    {
        return static_cast<double>(kFrames) / static_cast<double>(kRate);
    }

#else

    inline constexpr bool kHave = false;

    inline bool Play(float, engine::pcm::Voice&, engine::pcm::Voice&) { return false; }
    inline double Seconds() noexcept { return 0.0; }

#endif

} // namespace net::audio::alertsting::pcm
