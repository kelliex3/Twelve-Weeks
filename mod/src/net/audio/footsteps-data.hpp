#pragma once

// Guarded private-data sink for the remote-footstep tiers (tools/ext/footsteps.c,
// from the game-derived footsteps.h / footsteps.pcm). The tier table, wave table,
// sample rate and PCM blob do not ship here, so every read lives behind
// TO1_FOOTSTEPS_DATA, like exec-music-pcm.hpp / downed-vox-data.hpp.
//
// Build with -DTO1_FOOTSTEPS_DATA and supply a private
// net/audio/footsteps_data.hpp declaring these UNQUALIFIED names. The include
// below is inside the already-open `net::audio::footsteps::data` namespace, so
// the shim must NOT wrap them in another namespace:
//   extern const Wave kTierWave[];        // ext FS_WAVE (all footstep waves)
//   inline constexpr int kTierWaveCount = ...;
//   extern const Tier kTier[];            // ext FS_TIER (sprint, run, walk, ...)
//   inline constexpr int kTierCount = ...;
//   inline constexpr uint32_t kRate = ...;  // ext FS_RATE
//   extern const engine::pcm::Sample kBlob[];       // footsteps.pcm
//   extern const engine::pcm::Sample kBlobEnd[];
// Wave { off, n, rate } offsets are in mono frames from the blob's start; Tier
// { first, count } slices kTierWave by index.
//
// Without the macro the sink is inert: Data() returns an empty view, Have() is
// false and no mixer voice starts. The pure rules and the owner API stay live.

#include "engine/pcm.hpp"
#include "footsteps-core.hpp"

#include <cstddef>
#include <cstdint>

namespace net::audio::footsteps::data {

    // The private table's Wave / Tier / View layouts are exactly the core's, so
    // the guarded include can name them directly and the owner passes Data()
    // straight into core::Tick().
    using Wave = core::Wave;
    using Tier = core::Tier;
    using View = core::DataView;

#if defined(TO1_FOOTSTEPS_DATA)
#  include "footsteps_data.hpp"

    inline constexpr bool Have = true;

    inline View Data() noexcept
    {
        View v;
        v.waves = kTierWave;
        v.waveCount = kTierWaveCount;
        v.tiers = kTier;
        v.tierCount = kTierCount;
        v.rate = kRate;
        v.samples = static_cast<uint32_t>(kBlobEnd - kBlob);
        return v;
    }

    inline const engine::pcm::Sample* Frames(int wave) noexcept
    {
        if (wave < 0 || wave >= kTierWaveCount || !kBlob)
            return nullptr;
        return kBlob + static_cast<std::size_t>(kTierWave[wave].off);
    }

#else

    inline constexpr bool Have = false;

    inline View Data() noexcept { return View{}; }
    inline const engine::pcm::Sample* Frames(int) noexcept { return nullptr; }

#endif

} // namespace net::audio::footsteps::data
