#pragma once

#include <cstdint>

// Integration seam for the ported downed core (net/match/downed-state.*). This is the
// single place that installs its sink and fans the frame tick / net-event dispatch / session
// reset out to it, mirroring net/match/melee-bridge.*.
//
// The bridge is now the NATIVE owner when t2r-ext.dll is absent. boot::ExtLoaded() is the
// only cutover: every producer, send and engine effect here no-ops while the legacy ext is
// loaded, so the ext keeps its own wire / state and nothing is sampled or written twice.
// No t2r-*.txt toggle is flipped and no low 0..18 net event is touched.
//
// Wire IDs are the ext's exact bytes reserved in net-protocol.hpp: NEVT_DOWNED 0x43,
// NEVT_REVIVE 0x44 ([u8 revived slot]) and NEVT_REACH 0x47 ([downed][on][secs x10][pair]).

namespace net::match::downed_bridge {

    // Master gate. The native producers live in this module; the legacy ext still wins
    // whenever it is loaded.
    inline constexpr bool kEnabled = true;

    void Install();
    void Tick();
    void OnEvent(uint8_t sender, uint8_t type, const uint8_t* data, uint8_t size);
    void Reset();
    bool Installed() noexcept;

    // Called by the local damage receiver before it applies the engine hit. The decision
    // keeps native death UI authority: a would-kill hit becomes a 50 hp down, a lethal hit
    // while down hands off to the single existing Death-edge death path, and everything else
    // passes through untouched.
    enum class DamageOutcome : uint8_t { kNormal, kDowned, kDying, kIgnored };

    struct DamageDecision {
        DamageOutcome outcome = DamageOutcome::kNormal;
        float         applyDamage = 0.0f;  // damage the caller may still feed to FireAttack
    };

    DamageDecision NoteLocalDamage(uint8_t attacker, uint64_t weapon, float damage,
                                   bool lethalClaim);
    void CommitDownPool();  // re-assert the 50 hp pool after the clamped engine hit

} // namespace net::match::downed_bridge
