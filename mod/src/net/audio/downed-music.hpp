#pragma once

// Native sole owner of tools/ext/downed_music.c: TLOU2's Nora interrogation
// themes play in 2D while WE are downed, escalating with the bleed-out clock.
// A stage change lands on the music's next bar (or the next beat just after a
// hit that cut the bleed time), crossfaded at the same phase; the loop's
// appended start is taken over linearly at the loop point. Death can hand the
// mix to the set's own ending tail (ducking the death sting under it); a
// revive, an execution or leaving the match fades the music out instead.
//
// Every frame boot::FrameTick calls Tick(now) (the ext ran dnmus_tick from
// execmus_tick). The owner reads only facts already native-owned:
// net::match::downed::{IsDown,LocalDying,DownMs,BleedFraction}, player-status
// liveness / paired move, net::anim::paired::Get().role == 2 for the execution
// victim, engine::pcm::MusicUserVolume, music_hold for the combat-music cut,
// and engine::pcm::{Position,PlayStereoRawAt,SetTargetGainRaw,RawGains,Live,Cut}
// for the phase-matched voices. The ending tail ducks the death sting through
// net::ui::death_sting::SetDuck (1 = normal). No new wire message is added.
//
// Data is private: downed-music-pcm.hpp / TO1_DOWNED_MUSIC_PCM. Without the
// macro the state machine still ticks but no voice starts. Opt-out:
// t2r-no-downed-music.txt next to the exe (read-only, 120-frame cache).
// boot::ExtLoaded() yields every public entry to the legacy ext. Logs are
// prefixed "downed music:". Pure rules and their test live in
// downed-music-core.hpp / mod/tests/downed_music_core_test.cpp.

#include "downed-music-core.hpp"

#include <cstdint>

namespace net::audio::downed_music {

    // One per-frame tick with the same `now` boot uses for the other audio owners.
    void Tick(uint64_t nowMs);

    // Drop every voice / pending tail / duck on a match or replication reset.
    void Reset();

    // True while a down is armed, the music is playing, a cut/tail is running,
    // or the death-sting duck is held. False while the ext owns the feature.
    bool Active() noexcept;

} // namespace net::audio::downed_music
