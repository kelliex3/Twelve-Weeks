#pragma once

// Guarded private PCM sink for the post-match score sounds (tools/
// rip_postmatch_sfx.py -> postmatch_sfx.h / postmatch_sfx.pcm, mono s16 at
// PMS_RATE). The generator and wave are not in this tree, so every sample read
// lives behind TO1_PMSFX_PCM, like alertsting-pcm.hpp.
//
// Build with -DTO1_PMSFX_PCM and supply a private
// net/audio/postmatch_sfx_pcm_data.hpp declaring these UNQUALIFIED names:
//   extern const engine::pcm::Sample kSamples[];  // the whole mono blob
//   extern const int kOff[5];        // PMS_{ENTER,COUNT,END,STAMP,PAGE}_OFF
//   extern const int kN[5];          // the matching frame counts
//   extern const int kCountN;        // PMS_COUNT_N (frames available to the loop)
//   extern const int kRate;          // PMS_RATE (48 kHz in the recorded rip)
//
// Without the macro the sink is inert: the page hooks / one-shot state and logs
// stay live, but no voice is started and no scratch buffer is allocated.

#include "pmsfx-core.hpp"

#include "engine/pcm.hpp"

namespace net::audio::pmsfx::data {

#if defined(TO1_PMSFX_PCM)
#  include "postmatch_sfx_pcm_data.hpp"

    inline constexpr bool kHave = true;

    inline engine::pcm::Sample* CountBuffer()
    {
        static engine::pcm::Sample buf[kCountN];
        return buf;
    }

    inline bool Play(core::Kind kind, engine::pcm::Voice& voice) noexcept
    {
        const int k = static_cast<int>(kind);
        if (k < 0 || k >= static_cast<int>(core::Kind::kCountKinds))
            return false;
        if (kOff[k] < 0 || kN[k] <= 0)
            return false;
        voice = engine::pcm::PlayRaw(kSamples + kOff[k], kN[k],
                                     core::kGains[k], kRate);
        return engine::pcm::Live(voice);
    }

    inline bool CountStart(int ms, engine::pcm::Voice& voice) noexcept
    {
        const int n = core::CountSamples(ms, kRate, kCountN);
        const int off = kOff[static_cast<int>(core::Kind::kCount)];
        if (n <= 0 || off < 0 || off + n > kCountN)
            return false;
        engine::pcm::Sample* buf = CountBuffer();
        for (int i = 0; i < n; ++i)
            buf[i] = kSamples[off + i];
        const int k = core::TailFrames(n, kRate);
        for (int i = 0; i < k; ++i)
            buf[n - k + i] = static_cast<engine::pcm::Sample>(
                static_cast<float>(buf[n - k + i]) * static_cast<float>(k - i)
                / static_cast<float>(k));
        voice = engine::pcm::PlayRaw(buf, n, core::kCountGain, kRate);
        return engine::pcm::Live(voice);
    }

#else

    inline constexpr bool kHave = false;

    inline bool Play(core::Kind, engine::pcm::Voice&) noexcept { return false; }
    inline bool CountStart(int, engine::pcm::Voice&) noexcept { return false; }

#endif

} // namespace net::audio::pmsfx::data
