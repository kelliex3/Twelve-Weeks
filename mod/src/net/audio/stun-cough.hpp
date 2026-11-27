#pragma once

// Native sole owner of the STUN COUGH sub-feature of tools/ext/alert_sting.c
// (lines 201-270), ported as sc_pick / sc_muffled / stuncough_tick. TLOU2's own
// cough-light takes (6 male + 6 female) play through the PCM mixer: our own stun
// coughs once on start and every 1150 ms, 2-D through a ~900 Hz two-pole low-pass
// copy at 0.9; a stunned puppet coughs on its first stamp and at most every
// 1150 ms, placed in the world at 0.85 / 45 m range at gain 1.4. Picks are random
// without repeating a wave for the same gender.
//
// Who feeds it:
//  - NoteOwnStun(nowMs) on the local stun start (Tick also mirrors
//    gameplay::stun_lock::Until() automatically when that lock is live);
//  - NotePeerStun(slot, stunAtMs, female, handle, wpos) from the peer stun wire
//    (ext MSG_STUNNED 0x45 / NEVT_STUNNED, wired in net-replication.cpp);
//  - ClearPeer(slot) on respawn / drop / stun cut, SetOwnFemale(bool) for the
//    local body, Tick() once per frame, Reset() on replication reset.
//
// Data: stun_cough.pcm is private game-derived audio. Every read of it lives
// behind TO1_STUN_COUGH_PCM (stun-cough-pcm.hpp). With the macro off no voice
// starts, no cadence stamp / RNG value / freshness marker is consumed and no log
// claims playback; the feed API and pure rules stay live. The ext has no sidecar
// opt-out for this sub-feature, so none is invented here.
//
// boot::ExtLoaded() yields every entry while the legacy ext is loaded. Logs are
// tagged "[STUNCOUGH]". Pure rules / muffled builder: stun-cough-core.hpp,
// covered by mod/tests/stun_audio_core_test.cpp.

#include "stun-cough-core.hpp"

#include <cstdint>

namespace net::audio::stun_cough {

    // `startMs` is the wall-clock ms the local lock started (ext g_stun_until -
    // 3500 ms). The owner re-reads gameplay::stun_lock::Until() on every Tick.
    void NoteOwnStun(uint64_t startMs);
    void ClearOwnStun();
    void SetOwnFemale(bool female);

    // `stunAtMs` is the peer's wall-clock stun start; `handle` is its puppet sid
    // and `wpos` its replicated root (both optional, but one gives a 3-D position).
    void NotePeerStun(int slot, uint64_t stunAtMs, bool female, uint64_t handle,
                      const float wpos[3] = nullptr);
    void ClearPeer(int slot);

    void Tick();
    void Reset();

    bool Active() noexcept;   // our lock or any peer stun stamp is still live

} // namespace net::audio::stun_cough
