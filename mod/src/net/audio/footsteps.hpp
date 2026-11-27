#pragma once

// Native sole owner of tools/ext/footsteps.c: remote players' footsteps from
// the authored MLB tiers. All config / tier / gait / ankle / gain rules live in
// footsteps-core.hpp; the engine audio, sidecar, joint and network seams stay in
// footsteps.cpp. No new network message.
//
// Who calls it: boot::FrameTick() calls Tick() every frame;
// net-replication.cpp calls Reset() on a level / session reset. The PCM layer is
// opt-in via t2r-pcm-footsteps.txt and needs the private TO1_FOOTSTEPS_DATA
// build; without either the owner stays inert. The gait feed now reads the
// peer's effective Agility from net::match::skill_sync and the locomotion
// multiplier from gameplay::slowdown.

#include <cstdint>

namespace net::audio::footsteps {

    void Tick();
    void Reset();

    // Read-only smoothed gait speeds (m/s) from the same replicated world
    // samples the step timeline uses. GroundSpeed is XZ, VerticalSpeed is Y;
    // false means that peer has no live gait state yet (or has left / reset).
    bool GroundSpeed(uint8_t slot, float& out) noexcept;
    bool VerticalSpeed(uint8_t slot, float& out) noexcept;

} // namespace net::audio::footsteps
