#pragma once

// Native sole owner of tools/ext/death_theme.c's death drone: one of the
// TLOU2 SP tonal beds, picked at random (never the same twice in a row), played
// 2-D through the PCM mixer at the MUSIC volume as a hard-panned stereo pair.
// It starts at the death edge under the death / execution sting, is sidechain
// ducked by whatever else plays, and crossfade-loops over the baked wave fades
// until the death screen leaves.
//
// Feed: death-ui.cpp calls Arm(stingMs, execCue) once on the death edge and
// Tick(nowMs, phase, final) each frame (phase: 0 none, 1 dead, 2 respawned,
// 3 match end; final = the countdown's fade to black). Cut() is the shared
// hard/soft stop and Reset() the session reset. No new wire message: the cue
// and all timings are local, exactly like the ext.
//
// Data: death_drone.pcm is private game-derived audio. Every read lives behind
// TO1_DEATH_DRONE_PCM (death-theme-pcm.hpp); with the macro off no voice starts
// and Arm logs once. The ext's two sidecars (t2r-no-death-drone.txt, legacy
// t2r-no-death-theme.txt) are only checked in Arm, 120-frame cache, and every
// public entry yields while boot::ExtLoaded(). Logs are tagged "death drone:".
// Pure rules: death-theme-core.hpp, covered by death_theme_core_test.cpp.

#include "death-theme-core.hpp"

#include <cstdint>

namespace net::audio::death_theme {

    // Death edge: pick the bed and arm the immediate first copy. stingMs and
    // execCue are log-only; the sidechain follows whatever actually plays.
    void Arm(int stingMs, bool execCue);

    // Per frame from death-ui.cpp. phase / final come from the caller; match
    // liveness is read here (no new wire message or state).
    void Tick(uint64_t nowMs, int phase, bool final);

    // 0 = stop now, >0 = one edge-triggered fade-out (600 ms from Tick).
    void Cut(int fadeMs, const char* why);

    void Reset();

    bool Active() noexcept;

} // namespace net::audio::death_theme
