#pragma once

// Guarded private PCM sink for the bullet-by whizz (tools/ext/immersion.c's
// sp_bullet_by.h / sp_bullet_by.pcm). The wave table is game-derived and does
// not ship here, so the whole playback path lives in this header behind
// TO1_BULLET_BY_PCM, exactly like menu/sendoff-pcm.hpp.
//
// Build with -DTO1_BULLET_BY_PCM and supply a private
// net/audio/bullet_by_pcm_data.hpp declaring the following UNQUALIFIED names.
// The header is included below from inside the already-open `pcm` namespace, so
// it must NOT wrap them in another namespace:
//   extern const engine::pcm::Sample kBlob[];     // all waves, back to back
//   inline constexpr int kOffsets[] = { ... };   // ext SB_OFF
//   inline constexpr int kLengths[] = { ... };   // ext SB_LEN
//   inline constexpr int kRate = ...;            // ext SB_RATE
//   inline constexpr int kCount = ...;           // ext SB_COUNT
//
// Without the macro the sink is inert: the shot-ray wire, geometry, hit check,
// LOS and alert-sting handoff stay live; no RNG is consumed and no cooldown is
// armed by a whizz that cannot be heard.

#include "engine/pcm.hpp"

namespace net::audio::near_miss::pcm {

#if defined(TO1_BULLET_BY_PCM)
#  include "bullet_by_pcm_data.hpp"

    inline constexpr bool kHave = true;

    inline bool Play(int wave, float gl, float gr, engine::pcm::Voice& out)
    {
        if (wave < 0 || wave >= kCount)
            return false;
        out = engine::pcm::Play(kBlob + kOffsets[wave], kLengths[wave], gl, gr, kRate);
        return engine::pcm::Live(out);
    }

#else

    inline constexpr bool kHave = false;
    inline constexpr int kCount = 0;

    inline bool Play(int, float, float, engine::pcm::Voice&) { return false; }

#endif

} // namespace net::audio::near_miss::pcm
