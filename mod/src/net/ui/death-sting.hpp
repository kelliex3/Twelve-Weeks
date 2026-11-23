#pragma once

#include "engine/pcm.hpp"

#include <cstdint>

// Death-sting sink. The ripped TLOU2 SP stinger (tools/rip_t2_death_sting.py) is
// private data: the ext embedded it from death_sting.h / death_sting.pcm, neither of
// which ships here. This module owns the native playback path without depending on
// that data: build with TO1_DEATH_STING_PCM and supply a private
// net/ui/death_sting_data.hpp to hear it; otherwise every call is a no-op.
//
// The data header must declare, in this namespace:
//   extern const engine::pcm::Sample kDeathStingL[];
//   extern const engine::pcm::Sample kDeathStingR[];
//   inline constexpr int kDeathStingFrames = ...;   // samples per channel
//   inline constexpr int kDeathStingRate   = ...;   // e.g. 44100
// (left then right channel, matching the ext's ds_blob + DS_N layout).

namespace net::ui::death_sting {

#if defined(TO1_DEATH_STING_PCM)
#  include "death_sting_data.hpp"

    inline engine::pcm::Voice g_left, g_right;

    // Start the sting. Returns true when at least one voice started.
    inline bool Play()
    {
        Stop();
        if (!engine::pcm::PlayStereo(kDeathStingL, kDeathStingR,
                kDeathStingFrames, 1.0f, kDeathStingRate, g_left, g_right))
            return false;
        return engine::pcm::Live(g_left) || engine::pcm::Live(g_right);
    }

    // Fade out the running sting (0 = immediate), if any.
    inline void Stop(int fadeMs = 0)
    {
        if (engine::pcm::Live(g_left)) engine::pcm::Cut(g_left, fadeMs);
        if (engine::pcm::Live(g_right)) engine::pcm::Cut(g_right, fadeMs);
        engine::pcm::Invalidate(g_left);
        engine::pcm::Invalidate(g_right);
    }

    inline int Frames() { return kDeathStingFrames; }
    inline int Rate()   { return kDeathStingRate; }

#else

    inline bool Play()        { return false; }   // no data compiled in: owned no-op
    inline void Stop(int)     {}
    inline int  Frames()      { return 0; }
    inline int  Rate()        { return 0; }

#endif

} // namespace net::ui::death_sting
