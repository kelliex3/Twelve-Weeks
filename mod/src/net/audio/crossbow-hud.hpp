#pragma once

// Native sole owner of the CROSSBOW BOLT HUD sub-feature of
// tools/ext/alert_sting.c (lines 169-200), ported as xbhud_tick. While a bolt
// stays in us during play, the impact-1 one-shot plays on each new hit id and the
// tonal impaled pulse repeats back to back until the bolt is cleared; pulling it
// out plays impact-complete-02. All sounds are 2-D at the ext's 0.8 / 0.55 gains
// through engine::pcm::Play (which applies the SFX slider itself).
//
// Who feeds it:
//  - NoteBolt(slot, hitMs) when MSG_XBOLT 0x6E says a bolt is stuck in us with a
//    new hit id (wired in net-replication.cpp);
//  - NoteClear(pulledOut) when the bolt leaves (pulledOut = xb_clear by a pull);
//  - Tick() once per frame from boot::FrameTick; Reset() on replication reset.
//
// Data: xbow_hud.pcm is private game-derived audio. Every read of it lives behind
// TO1_XBOW_HUD_PCM (crossbow-hud-pcm.hpp). With the macro off no voice starts, no
// hit_seen / pulse cooldown is consumed and no log claims playback; the feed API
// and pure rules stay live. The ext has no sidecar opt-out for this sub-feature,
// so none is invented here.
//
// boot::ExtLoaded() yields every entry while the legacy ext is loaded. Logs are
// tagged "[XBOWHUD]". Pure state machine: crossbow-hud-core.hpp, covered by
// mod/tests/stun_audio_core_test.cpp.

#include "crossbow-hud-core.hpp"

#include <cstdint>

namespace net::audio::crossbow_hud {

    void NoteBolt(int slot, uint64_t hitMs);
    void NoteClear(bool pulledOut);
    void Tick();
    void Reset();

    bool Active() noexcept;   // a bolt is in us during a live match (ext g_xb.on && substate 6)

} // namespace net::audio::crossbow_hud
