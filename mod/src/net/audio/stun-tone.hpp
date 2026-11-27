#pragma once

// Native sole owner of the STUN CONCUSSION TONE sub-feature of
// tools/ext/alert_sting.c (lines 95-167), ported as alert_sting.c's stuntone_*.
// The enemy stun bomb's burst on our screen starts a 3.45 s bed (impact + tinnitus
// ring) ~0.1 s before the stun is judged; stun_local confirms it, or starts it when
// no burst was seen (thrower-side burst only). Unconfirmed within 700 ms it fades
// in 150 ms; a confirmed voice that outlives the stun fades in 250 ms.
//
// Who feeds it:
//  - Burst(slot, at, radius) from the bomb-burst receiver (the placed stun bomb's
//    burst event), with the burst world position and radius; the owner reads the
//    local root itself and applies every ext liveness gate.
//  - Stunned() from the local stun start (gameplay::stun_lock::Stun / the stun
//    receiver's local confirmation).
//  - Tick() once per frame from boot::FrameTick; Reset() from replication reset.
//
// Data: stun_tone.pcm / stun_ring.pcm are private game-derived waves. The bed is
// built once into owner-owned vectors behind TO1_STUN_TONE_PCM (stun-tone-pcm.hpp).
// With the macro off no voice starts, no t0 / confirmation state is consumed and
// no log claims playback; the event API and pure rules remain live. The ext has
// no sidecar opt-out for this sub-feature, so none is invented here.
//
// boot::ExtLoaded() yields every entry while the legacy ext is loaded. Logs are
// tagged "[STUNTONE]". Pure rules / bed builder: stun-tone-core.hpp, covered by
// mod/tests/stun_audio_core_test.cpp.

#include "stun-tone-core.hpp"

namespace net::audio::stun_tone {

    // Bomb burst seen on our screen (ext stuntone_burst). `at` is the burst world
    // position, `radius` the stun radius; null `at` is ignored.
    void Burst(int slot, const float at[3], float radius);

    // We were stunned (ext stuntone_stunned): confirms a live burst voice or starts
    // a confirmed one when no burst was seen locally.
    void Stunned();

    // Per frame, after the stun lock tick (ext stuntone_tick).
    void Tick();

    // Drop clocks / voices on a match reset. The immutable bed is kept.
    void Reset();

    bool     Active() noexcept;      // a started voice still live in the mixer
    uint64_t StartedAt() noexcept;   // ext g_stt.t0 (0 when no voice was started)

} // namespace net::audio::stun_tone
