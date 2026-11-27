#pragma once

// Native sole owner of tools/ext/pullout_sting.c: the victim's prone pull-out
// feel (grab kick + grunt, SP struggle beats, lens swing, fail blow + spike,
// escape line and the combat-music requests). Pure rules live in
// pullout-sting-core.hpp; this is the DC / sidecar / music / guarded PCM seam.
// No new wire message: every edge is reported by the native pull-out bridge.
//
// Who calls it: net/match/melee-pullout-bridge.cpp reports the victim-side drag
// start (with a verified puller puppet handle), the counter verdict, the
// authored fail blow and every frame of the drag, and cuts it when the drag
// ends, we die or the match leaves play. The same bridge consumes FovMultiplier
// and MeasuredShake in its SP camera phase. Reset() is reached through the
// bridge's match / replication reset.
//
// The riser / decay waves stay behind TO1_PULLOUT_STING_PCM; without the private
// combat-music data the hold / escape / fail requests still drive music_hold.

#include "pullout-sting-core.hpp"

#include <cstdint>

namespace net::audio::pullout_sting {

    void OnGrab(uint64_t now, int pullerSlot, uint64_t pullerHandle);
    void OnVerdict(uint64_t now, bool ok);
    void OnOutcome(uint64_t now, bool ok, float blowS);
    void OnTick(uint64_t now, bool dragging, bool dead, bool matchLive);

    void Reset();
    bool Active() noexcept;

    // Native producer for the ext's g_xcam_fov_mul: 0 = no victim lens override.
    float FovMultiplier() noexcept;

    // Measured PS32 kick pitch (degrees) for the victim camera's current frame.
    float MeasuredShake(uint64_t now) noexcept;

} // namespace net::audio::pullout_sting
