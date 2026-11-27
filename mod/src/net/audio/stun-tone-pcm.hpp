#pragma once

// Guarded private PCM sink for the stun concussion bed (tools/ext/alert_sting.c's
// stun_tone.h / stun_tone.pcm impact and stun_ring.h / stun_ring.pcm ring). Both
// waves are game-derived and do not ship here, so the whole playback path lives in
// this header behind TO1_STUN_TONE_PCM, exactly like alertsting-pcm.hpp.
//
// Build with -DTO1_STUN_TONE_PCM and supply a private
// net/audio/stun_tone_pcm_data.hpp declaring the following UNQUALIFIED names. The
// header is included below from inside the already-open `pcm` namespace, so it
// must NOT wrap them in another namespace:
//   extern const engine::pcm::Sample kImpact[];  // packed stereo impact wave
//   inline constexpr int kImpactFrames = ...;    // ext ST_N (per channel)
//   inline constexpr int kImpactRate = ...;      // ext ST_RATE
//   extern const engine::pcm::Sample kRing[];    // packed stereo ~410 Hz ring
//   inline constexpr int kRingFrames = ...;      // ext SR_N (per channel)
//   inline constexpr int kRingRate = ...;        // ext SR_RATE
// Both rates are assumed equal (the ext builds one bed at ST_RATE); the owner uses
// kImpactRate for the bed and for playback. A shim over the generated contiguous
// blobs works: kImpact = st_blob, kRing = sr_blob.
//
// Without the macro the sink is inert: no bed vector is built, no voice starts,
// no burst/confirmation clock is stamped and no log claims playback. The pure
// rules and the event feeds stay live. The owner is the native sole owner of this
// sub-feature when the ext is absent; the ext is yielded via boot::ExtLoaded().

#include "stun-tone-core.hpp"

#include "engine/pcm.hpp"

#include <cstddef>
#include <vector>

namespace net::audio::stun_tone::pcm {

#if defined(TO1_STUN_TONE_PCM)
#  include "stun_tone_pcm_data.hpp"

    inline constexpr bool kHave = true;

    inline bool Play(const std::vector<core::Sample>& l,
                     const std::vector<core::Sample>& r, int frames,
                     float gain, int rate,
                     engine::pcm::Voice& outL, engine::pcm::Voice& outR)
    {
        if (frames <= 0 || gain <= 0.0f
            || l.size() < static_cast<std::size_t>(frames)
            || r.size() < static_cast<std::size_t>(frames))
            return false;
        return engine::pcm::PlayStereoRaw(l.data(), r.data(), frames, gain, rate,
                                          outL, outR);
    }

#else

    inline constexpr bool kHave = false;
    // Inert stand-ins so the owner's built bed path is well-formed without the
    // private header; no vector is filled and no voice is started (see BuildBed).
    inline constexpr const engine::pcm::Sample* kImpact = nullptr;
    inline constexpr int kImpactFrames = 0;
    inline constexpr int kImpactRate = 0;
    inline constexpr const engine::pcm::Sample* kRing = nullptr;
    inline constexpr int kRingFrames = 0;
    inline constexpr int kRingRate = 0;

    inline bool Play(const std::vector<core::Sample>&,
                     const std::vector<core::Sample>&, int, float, int,
                     engine::pcm::Voice&, engine::pcm::Voice&) { return false; }

#endif

} // namespace net::audio::stun_tone::pcm
