#pragma once

#include "engine/pcm.hpp"

#include <cstdint>

// Stinger sink for the TWELVE WEEKS entry sequence. The ripped candidate
// (tools/rip_load_stinger.py -> private loadseq.h / loadseq.pcm) does not ship in
// this repository, just like the death sting. The whole native playback path is
// here; build with TO1_LOADSEQ_STINGER_PCM and supply a private
// menu/loadseq_sting_data.hpp to hear it. Without the data the sequence is silent,
// never ext-owned: no DLL is needed to animate or play it.
//
// The data header must declare, in this namespace:
//   extern const engine::pcm::Sample kLoadseqSting[];  // mono s16
//   inline constexpr int kLoadseqStingFrames = ...;
//   inline constexpr int kLoadseqStingRate = ...;      // e.g. 48000
namespace menu::loadseq::sting {

#if defined(TO1_LOADSEQ_STINGER_PCM)
#  include "loadseq_sting_data.hpp"

    inline engine::pcm::Voice g_voice;

    inline bool Play(float musicVolume)
    {
        Stop();
        g_voice = engine::pcm::PlayRaw(kLoadseqSting, kLoadseqStingFrames,
            0.9f * musicVolume, kLoadseqStingRate);
        return engine::pcm::Live(g_voice);
    }

    inline void Stop(int fadeMs = 0)
    {
        if (engine::pcm::Live(g_voice)) engine::pcm::Cut(g_voice, fadeMs);
        engine::pcm::Invalidate(g_voice);
    }

#else

    inline bool Play(float)     { return false; }
    inline void Stop(int)       {}

#endif

}
