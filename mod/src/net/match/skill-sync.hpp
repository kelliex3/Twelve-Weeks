#pragma once

// Native peer-skill owner (ext MSG_SK_SYNC 0x50). The core owns the validated
// rows, LP check and effective-skill expansion; this header is the only seam
// the audio consumers need.
//
// No new network message: the pinned byte is NEVT_SKILLS in net-protocol.hpp.
// Ticks belong in boot::FrameTick(), resets in net::replication::ResetRuntime().

#include "skill-sync-core.hpp"

#include <cstdint>

namespace net::match::skill_sync {

    void Tick();
    void OnEvent(uint8_t senderSlot, uint8_t type,
                 const uint8_t* data, uint8_t size);

    // Effective level of a peer's skill; 0 for an unknown / stale peer. The
    // Agility accessor is what remote footsteps use (byte 8 includes Lone Wolf).
    int AgilityLevel(int slot) noexcept;
    int SkillLevel(int slot, int id) noexcept;

    void Reset();

}
