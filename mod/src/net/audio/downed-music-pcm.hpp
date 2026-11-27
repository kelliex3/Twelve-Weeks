#pragma once

// Guarded private PCM sink for the downed music. The game-derived wave is
// private (tools/ext/downed_music.c incbin'd "downed_music.pcm" plus the
// generated downed_music.h), so it does not ship here. Build with
// -DTO1_DOWNED_MUSIC_PCM and supply a private
// net/audio/downed_music_pcm_data.hpp declaring these UNQUALIFIED names. The
// include below is inside the already-open `pcm` namespace, so the shim must
// NOT wrap them in another namespace:
//
//   extern const engine::pcm::Sample kBlob[];      // all channels, interleaved by stage
//   inline constexpr int kStages = 4;              // asserted here (ext DM_STAGES)
//   inline constexpr int kOffsets[kStages];        // ext DM_OFF, in samples
//   inline constexpr int kLoopFrames[kStages];     // ext DM_LEN, loop length per stage
//   inline constexpr int kTailFrames;              // ext DM_TAIL, appended start samples
//   inline constexpr int kRate;                    // ext DM_RATE
//   inline constexpr int kEndOff;                  // ext DM_END_OFF, one-shot offset
//   inline constexpr int kEndFrames;               // ext DM_END_N
//   inline constexpr float kRmsDb[kStages];        // ext DM_RMS_DB
//
// Stage L/R live at kBlob + kOffsets[s] and one voice-length later
// (kLoopFrames[s] + kTailFrames); the ending tail is one stereo pair at
// kBlob + kEndOff / + kEndFrames. A tiny shim over the generator's contiguous
// dm_blob / DM_* constants works.
//
// Without the macro the sink is inert: kHave is false, Rate() is 1, every
// pointer helper is null and every frame count is 0, RmsDb() is 0 and no voice
// is ever started. The rules in downed-music-core.hpp remain live.

#include "engine/pcm.hpp"

namespace net::audio::downed_music::pcm {

#if defined(TO1_DOWNED_MUSIC_PCM)
#  include "downed_music_pcm_data.hpp"

    static_assert(kStages == 4, "downed music private data must carry exactly four stages");

    inline constexpr bool kHave = true;

    inline int Rate() noexcept { return kRate; }

    inline int StageLoopFrames(int s) noexcept
    {
        return s >= 0 && s < kStages ? kLoopFrames[s] : 0;
    }

    inline int StageVoiceFrames(int s) noexcept
    {
        return s >= 0 && s < kStages ? kLoopFrames[s] + kTailFrames : 0;
    }

    inline const engine::pcm::Sample* StageL(int s) noexcept
    {
        if (s < 0 || s >= kStages)
            return nullptr;
        return kBlob + kOffsets[s];
    }

    inline const engine::pcm::Sample* StageR(int s) noexcept
    {
        if (s < 0 || s >= kStages)
            return nullptr;
        return kBlob + kOffsets[s] + StageVoiceFrames(s);
    }

    inline const engine::pcm::Sample* EndL() noexcept { return kBlob + kEndOff; }
    inline const engine::pcm::Sample* EndR() noexcept { return EndL() + kEndFrames; }
    inline int EndFrames() noexcept { return kEndFrames; }
    inline float RmsDb(int s) noexcept
    {
        return s >= 0 && s < kStages ? kRmsDb[s] : 0.0f;
    }

#else

    inline constexpr bool kHave = false;
    inline constexpr int kStages = 0;

    inline int Rate() noexcept { return 1; }
    inline int StageLoopFrames(int) noexcept { return 0; }
    inline int StageVoiceFrames(int) noexcept { return 0; }
    inline const engine::pcm::Sample* StageL(int) noexcept { return nullptr; }
    inline const engine::pcm::Sample* StageR(int) noexcept { return nullptr; }
    inline const engine::pcm::Sample* EndL() noexcept { return nullptr; }
    inline const engine::pcm::Sample* EndR() noexcept { return nullptr; }
    inline int EndFrames() noexcept { return 0; }
    inline float RmsDb(int) noexcept { return 0.0f; }

#endif

} // namespace net::audio::downed_music::pcm
