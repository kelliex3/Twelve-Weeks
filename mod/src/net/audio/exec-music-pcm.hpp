#pragma once

// Guarded private PCM sink for the execution-victim cues (tools/ext/exec_music.c,
// from the private exec_music.h / exec_music.pcm). The five wave cues are
// game-derived audio and do not ship here, so every read of them lives in this
// header behind TO1_EXEC_MUSIC_PCM, like death-theme-pcm.hpp / downed-music-pcm.hpp.
//
// Build with -DTO1_EXEC_MUSIC_PCM and supply a private
// net/audio/exec_music_pcm_data.hpp declaring these UNQUALIFIED names. The
// include below is inside the already-open `pcm` namespace, so the shim must NOT
// wrap them in another namespace:
//   extern const engine::pcm::Sample kBlob[];   // all five cues, back to back
//   inline constexpr int kOff[5] = { ... };     // ext EXMUS_OFF: sample offset of each cue's LEFT channel
//   inline constexpr int kN = ...;              // ext EXMUS_N: frames per channel, per cue
//   inline constexpr int kRate = ...;           // ext EXMUS_RATE: source sample rate in Hz
// The blob is laid out as [L(0), R(0), L(1), R(1), ...] with kN samples in each
// channel: cue p's left is kBlob + kOff[p] and its right is that pointer + kN.
// kOff is authored by tools/rip_exec_music.py (do not assume p * 2 * kN); the
// blob must hold at least max(kOff) + 2 * kN samples. Ext uses the two pointers
// as a hard-panned stereo pair and starts at seek * kRate frames in.
//
// Without the macro the sink is inert: both channel pointers are null, the
// frame count and rate are 0 and kHave is false, so no voice starts. The pure
// rules and the owner API stay live. This feature has no sidecar flag.

#include "engine/pcm.hpp"

#include <cstddef>

namespace net::audio::exec_music::pcm {

    inline constexpr int kCount = 5;

#if defined(TO1_EXEC_MUSIC_PCM)
#  include "exec_music_pcm_data.hpp"

    inline constexpr bool kHave = true;

    inline int Frames() noexcept { return kN; }
    inline int Rate() noexcept { return kRate; }

    inline const engine::pcm::Sample* Left(int cue) noexcept
    {
        if (cue < 0 || cue >= kCount)
            return nullptr;
        return kBlob + static_cast<std::size_t>(kOff[cue]);
    }

    inline const engine::pcm::Sample* Right(int cue) noexcept
    {
        const engine::pcm::Sample* l = Left(cue);
        return l ? l + static_cast<std::size_t>(kN) : nullptr;
    }

#else

    inline constexpr bool kHave = false;

    inline int Frames() noexcept { return 0; }
    inline int Rate() noexcept { return 0; }

    inline const engine::pcm::Sample* Left(int) noexcept { return nullptr; }
    inline const engine::pcm::Sample* Right(int) noexcept { return nullptr; }

#endif

} // namespace net::audio::exec_music::pcm
