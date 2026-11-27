#pragma once

// Guarded private PCM sink for the pull-out riser / decay (tools/ext/t2r-ext.c
// `g_ms_pull` requests, driven by pullout_sting.c). The ext asks the combat
// music set for a riser on the grab and for its transition-down on a failed
// counter; those two waves are game-derived and are not in this tree. Every
// sample read therefore lives behind TO1_PULLOUT_STING_PCM, exactly like
// pmsfx-data.hpp / exec-music-pcm.hpp.
//
// Build with -DTO1_PULLOUT_STING_PCM and supply a private
// net/audio/pullout_sting_pcm_data.hpp declaring these UNQUALIFIED names. The
// include below is already inside `net::audio::pullout_sting::data`, so the shim
// must NOT wrap them in another namespace:
//
//   extern const engine::pcm::Sample kRiserL[];  // riser, left channel
//   extern const engine::pcm::Sample kRiserR[];  // riser, right channel
//   extern const int  kRiserN;                   // frames per channel
//   extern const int  kRiserRate;                // sample rate
//   extern const float kRiserGain;               // raw music gain (no sfx slider)
//   extern const engine::pcm::Sample kDecayL[];  // set's transition-down, left
//   extern const engine::pcm::Sample kDecayR[];  // set's transition-down, right
//   extern const int  kDecayN;                   // frames per channel
//   extern const int  kDecayRate;                // sample rate
//   extern const float kDecayGain;               // raw music gain
//
// The owner starts one raw stereo pair per request and cuts it on the verdict /
// drag end / reset; the music_hold requests stay live with or without this data.
// With the macro off the sink is inert: no voice starts and no private header is
// needed at build or run time.

#include "engine/pcm.hpp"

namespace net::audio::pullout_sting::data {

#if defined(TO1_PULLOUT_STING_PCM)
#  include "pullout_sting_pcm_data.hpp"

    inline constexpr bool kHave = true;

    inline bool PlayRiser(engine::pcm::Voice& l, engine::pcm::Voice& r) noexcept
    {
        if (kRiserN <= 0 || kRiserRate <= 0)
            return false;
        return engine::pcm::PlayStereoRaw(kRiserL, kRiserR, kRiserN,
                                          kRiserGain, kRiserRate, l, r);
    }

    inline bool PlayDecay(engine::pcm::Voice& l, engine::pcm::Voice& r) noexcept
    {
        if (kDecayN <= 0 || kDecayRate <= 0)
            return false;
        return engine::pcm::PlayStereoRaw(kDecayL, kDecayR, kDecayN,
                                          kDecayGain, kDecayRate, l, r);
    }

#else

    inline constexpr bool kHave = false;

    inline bool PlayRiser(engine::pcm::Voice&, engine::pcm::Voice&) noexcept { return false; }
    inline bool PlayDecay(engine::pcm::Voice&, engine::pcm::Voice&) noexcept { return false; }

#endif

} // namespace net::audio::pullout_sting::data
