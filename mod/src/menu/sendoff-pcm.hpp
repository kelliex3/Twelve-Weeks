#pragma once

#include "engine/pcm.hpp"

#include <cstdint>

// Private PCM sink for the menu send-off (tools/ext/sendoff.c). The ripped clips
// (tools/rip_sendoff.py -> sendoff_pcm.h / sendoff_pcm) are private data and do
// not ship here, just like the death sting. The whole native playback path lives
// in this header; build with TO1_SENDOFF_PCM and supply a private
// menu/sendoff_pcm_data.hpp to hear it. Without the data the birds, fades and
// session duck still run, but silently.
//
// The data header must declare, in this namespace:
//   extern const engine::pcm::Sample kCrows[];
//   extern const engine::pcm::Sample kWlf[];
//   extern const engine::pcm::Sample kSer[];
//   extern const engine::pcm::Sample kHorn[];
//   inline constexpr int kCrowsFrames = ..., kWlfFrames = ..., kSerFrames = ...,
//                       kHornFrames = ...;
//   inline constexpr int kRate = ...;                 // e.g. 44100
//   inline constexpr float kCrowsGain = ..., kWlfGain = ..., kSerGain = ...;
// (mono s16 clips, matching the ext's SENDOFF_*_PCM / SENDOFF_GAIN_* / _RATE.
// kHorn is the private match_horn.h clip the ext's feature_match_horn plays.)

namespace menu::sendoff::pcm {

#if defined(TO1_SENDOFF_PCM)
#  include "sendoff_pcm_data.hpp"

    inline engine::pcm::Voice g_crows, g_fac, g_horn;
    inline bool g_facOnWlf = false;
    inline constexpr float kHornGain = 0.9f;

    // The ext starts these with pcm_play_rate (the sfx slider is applied by
    // engine::pcm::Play) and retargets every frame with pcm_voice_ctl.
    inline bool PlayCrows(float envelope)
    {
        StopCrows(0);
        g_crows = engine::pcm::Play(kCrows, kCrowsFrames,
                                    kCrowsGain * envelope, kCrowsGain * envelope,
                                    kRate);
        return engine::pcm::Live(g_crows);
    }

    inline void CrowsGain(float envelope)
    {
        if (engine::pcm::Live(g_crows))
            engine::pcm::SetGain(g_crows, kCrowsGain * envelope);
    }

    inline bool PlayFaction(int scene, float envelope)
    {
        StopFaction(0);
        const engine::pcm::Sample* src = scene == 1 ? kWlf : kSer;
        const int n = scene == 1 ? kWlfFrames : kSerFrames;
        const float gain = scene == 1 ? kWlfGain : kSerGain;
        g_fac = engine::pcm::Play(src, n, gain * envelope, gain * envelope, kRate);
        g_facOnWlf = scene == 1;
        return engine::pcm::Live(g_fac);
    }

    inline void FactionGain(float envelope)
    {
        if (!engine::pcm::Live(g_fac)) return;
        const float gain = g_facOnWlf ? kWlfGain : kSerGain;
        engine::pcm::SetGain(g_fac, gain * envelope);
    }

    inline bool PlayHorn(float envelope)
    {
        StopHorn(0);
        g_horn = engine::pcm::Play(kHorn, kHornFrames,
                                   kHornGain * envelope, kHornGain * envelope,
                                   kRate);
        return engine::pcm::Live(g_horn);
    }

    inline void HornGain(float envelope)
    {
        if (engine::pcm::Live(g_horn))
            engine::pcm::SetGain(g_horn, kHornGain * envelope);
    }

    inline void StopHorn(int fadeMs)
    {
        if (engine::pcm::Live(g_horn)) engine::pcm::Cut(g_horn, fadeMs);
        engine::pcm::Invalidate(g_horn);
    }

    inline void StopCrows(int fadeMs)
    {
        if (engine::pcm::Live(g_crows)) engine::pcm::Cut(g_crows, fadeMs);
        engine::pcm::Invalidate(g_crows);
    }

    inline void StopFaction(int fadeMs)
    {
        if (engine::pcm::Live(g_fac)) engine::pcm::Cut(g_fac, fadeMs);
        engine::pcm::Invalidate(g_fac);
    }

    inline bool Have() { return true; }

#else

    inline bool PlayCrows(float)     { return false; }
    inline void CrowsGain(float)     {}
    inline bool PlayFaction(int, float) { return false; }
    inline void FactionGain(float)   {}
    inline bool PlayHorn(float)      { return false; }
    inline void HornGain(float)      {}
    inline void StopHorn(int)        {}
    inline void StopCrows(int)       {}
    inline void StopFaction(int)     {}
    inline bool Have()               { return false; }

#endif

} // namespace menu::sendoff::pcm
