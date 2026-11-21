#pragma once

#include <cstdint>

// Ported from tools/ext/exec_music.c's pure timing / hold state. The five victim cues are
// generated private PCM blobs, so playback is a sink; this core owns only the decisions:
// when a pending cue must start, when the combat music is cut, when the hold/resume expires,
// and the fade / cap rules.
//
// Ext numbers kept: EXMUS_LEVEL 0.85, EXMUS_HIT[] (the second of each cue that must land on
// the death screen), the 83 ms death-ui delay + 70 ms cut, EXMUS_CAP_MS 12500, EXMUS_FADE_MS
// 300 and EXMUS_RESUME_MS 1500. A cue that is still pending when a fade fires never starts.

namespace net::audio::exec_music {

    inline constexpr float    kLevel        = 0.85f;
    inline constexpr uint64_t kCapMs        = 12500;
    inline constexpr uint64_t kFadeMs       = 300;
    inline constexpr uint64_t kResumeMs     = 1500;
    inline constexpr uint64_t kDeathDelayMs = 83 + 70;   // death_ui's delay + cut
    inline constexpr int      kCues         = 5;
    inline constexpr float    kHit[kCues]   = { 2.44f, 2.35f, 2.31f, 2.35f, 2.39f };

    struct Sink {
        virtual ~Sink() = default;
        // ext MusicPlay("music-playlist-rogue-player-death", "execution victim: ...").
        virtual void MusicCut() = 0;
        // The PCM voice for `pick` at `seekS` seconds in; gain already includes kLevel.
        virtual void StartCue(int pick, double seekS, float level) = 0;
        virtual bool CueLive() const = 0;
        virtual void CutCue(int fadeMs) = 0;
        virtual void Log(const char* line) = 0;
    };
    void SetSink(Sink* sink) noexcept;
    Sink* SinkInstance() noexcept;

    // ext ExecmusStart: choose a cue (host rand) and place its hit second on the death screen.
    // If a hold is already up the combat music is already cut, so the cue does not re-cut it.
    void Start(uint64_t nowMs, float killS, int pick);

    // ext ExecmusTick: start a due pending cue, then check the mixer / cap / match state.
    void Tick(uint64_t nowMs, bool inPlayState);

    // ext ExecmusFade: cancel a pending cue and fade a live one, holding the music back.
    void Fade(uint64_t nowMs, const char* why);

    bool     Pending(uint64_t nowMs) noexcept;
    bool     Playing() noexcept;
    bool     Fading() noexcept;
    uint64_t PendingAt() noexcept;
    uint64_t StartedAt() noexcept;

    void Reset();

} // namespace net::audio::exec_music
