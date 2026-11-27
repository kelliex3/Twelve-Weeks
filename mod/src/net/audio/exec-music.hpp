#pragma once

// Native sole owner of tools/ext/exec_music.c: while WE are the execution victim,
// one of five TLOU2 cues (picked by the xcam bridge) rises from silence and its
// authored hit second lands on the death screen. It plays 2-D through our PCM
// mixer as a hard-panned raw stereo pair at EXMUS_LEVEL x the options MUSIC
// volume, one cue at a time, faded out at the death screen, the 12.5 s cap or a
// match-left, with the combat music held cut and resumed 1.5 s later.
//
// Feed: boot.cpp calls Tick(now, MatchLive()) each frame; xcam-bridge.cpp calls
// Start(now, killS, pick) on the victim camera edge and reads kCues; net-replication
// calls Reset() on a level/session reset. SetDuck() is the seam for the upcoming
// exec_beg port (1 = original level, 0.35 = under the plea). No new wire message.
//
// Data: exec_music.pcm is private game-derived audio. Every read lives behind
// TO1_EXEC_MUSIC_PCM (exec-music-pcm.hpp); with the macro off no voice starts and
// the cue stays state-only. The ext has NO sidecar for this feature: this owner
// writes no flags. boot::ExtLoaded() yields every public entry to the legacy ext.
// Logs are tagged "exec music:". Pure rules and their test live in
// exec-music-core.hpp / mod/tests/exec_music_core_test.cpp.

#include "exec-music-core.hpp"

#include <cstdint>

namespace net::audio::exec_music {

    namespace core = net::audio::exec_music::core;

    using core::kLevel;
    using core::kCapMs;
    using core::kFadeMs;
    using core::kResumeMs;
    using core::kDeathDelayMs;
    using core::kCues;
    using core::kHit;

    // ext execmus_start: pick already drawn by the caller (xcam bridge); the owner
    // schedules the cue and holds the combat-music cut through its cap + resume.
    void Start(uint64_t nowMs, float killS, int pick);

    // ext execmus_tick, fed by boot::FrameTick with the native match liveness.
    void Tick(uint64_t nowMs, bool inPlayState);

    // ext execmus_fade: cancel a pending cue, fade a live one and hold the resume.
    void Fade(uint64_t nowMs, const char* why);

    // 1.0 = original level, 0.35 = ducked under the plea (ext eb_music_duck).
    // The first non-unity call captures both live voices' raw target gains.
    void SetDuck(float duck) noexcept;

    // True once the pending cue is due (ext g_exmus_pend.at && now >= at).
    bool     Pending(uint64_t nowMs) noexcept;
    bool     Playing() noexcept;
    bool     Fading() noexcept;
    uint64_t PendingAt() noexcept;
    uint64_t StartedAt() noexcept;

    // Last duck request (1.0 when none), and the cue being played / pending.
    float Duck() noexcept;
    int   Cue() noexcept;

    void Reset();

} // namespace net::audio::exec_music
