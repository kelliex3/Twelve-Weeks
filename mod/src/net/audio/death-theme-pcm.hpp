#pragma once

// Guarded private PCM sink for the death drone (tools/ext/death_theme.c, from
// death_drone.h / death_drone.pcm). The wave bank is game-derived and does not
// ship here, so every read of it lives in this header behind
// TO1_DEATH_DRONE_PCM, exactly like alertsting-pcm.hpp / stun-cough-pcm.hpp.
//
// Build with -DTO1_DEATH_DRONE_PCM and supply a private
// net/audio/death_drone_pcm_data.hpp declaring these UNQUALIFIED names. The
// include below is inside the already-open `pcm` namespace, so the shim must NOT
// wrap them in another namespace:
//   extern const engine::pcm::Sample kDroneBlob[];       // all waves, back to back
//   inline constexpr int kDroneWaves = ...;              // ext DR_K
//   inline constexpr int kDroneFrames = ...;             // ext DR_N (per channel)
//   inline constexpr int kDroneRate = ...;               // ext DR_RATE
//   inline constexpr int kDroneCrossfadeMs = ...;        // ext DR_XF_MS
//   extern const char* const kDroneNames[kDroneWaves];   // ext DR_NAMES
// The blob is laid out as [left(0), right(0), left(1), right(1), ...], each
// channel kDroneFrames samples: cue c's left is kDroneBlob + 2*kDroneFrames*c,
// its right is that pointer + kDroneFrames. Ext uses those two pointers as a
// hard-panned stereo pair and skips both in the sidechain scan.
//
// Without the macro the sink is inert: no voice can start (both channel
// pointers are null, there are no waves), so Arm() never arms a death drone.
// The pure rules and the owner API stay live.

#include "engine/pcm.hpp"

#include <cstddef>

namespace net::audio::death_theme::pcm {

#if defined(TO1_DEATH_DRONE_PCM)
#  include "death_drone_pcm_data.hpp"

    inline constexpr bool kHave = true;

    inline int WaveCount() noexcept { return kDroneWaves; }
    inline int Frames() noexcept { return kDroneFrames; }
    inline int Rate() noexcept { return kDroneRate; }
    inline int CrossfadeMs() noexcept { return kDroneCrossfadeMs; }

    inline const char* Name(int cue) noexcept
    {
        return cue >= 0 && cue < kDroneWaves ? kDroneNames[cue] : "?";
    }

    inline const engine::pcm::Sample* Left(int cue) noexcept
    {
        if (cue < 0 || cue >= kDroneWaves)
            return nullptr;
        return kDroneBlob + static_cast<std::size_t>(2) * static_cast<std::size_t>(kDroneFrames)
                            * static_cast<std::size_t>(cue);
    }

    inline const engine::pcm::Sample* Right(int cue) noexcept
    {
        const engine::pcm::Sample* l = Left(cue);
        return l ? l + kDroneFrames : nullptr;
    }

#else

    inline constexpr bool kHave = false;

    inline int WaveCount() noexcept { return 0; }
    inline int Frames() noexcept { return 0; }
    inline int Rate() noexcept { return 1; }
    inline int CrossfadeMs() noexcept { return 0; }

    inline const char* Name(int) noexcept { return "?"; }
    inline const engine::pcm::Sample* Left(int) noexcept { return nullptr; }
    inline const engine::pcm::Sample* Right(int) noexcept { return nullptr; }

#endif

} // namespace net::audio::death_theme::pcm
