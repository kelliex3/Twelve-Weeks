#pragma once

#include <cstdint>

// Gated integration seam for the ported downed core (net/match/downed-state.*). This is the
// single place that installs its sink and fans the frame tick / net-event dispatch / session
// reset out to it, mirroring net/match/melee-bridge.*.
//
// IMPORTANT: kEnabled is false. Nothing here runs in a shipping build, so the ext remains the
// behaviour owner and no t2r-*.txt cutover is flipped. The call sites in boot::FrameTick and
// net-replication are unconditional and cheap: every bridge function no-ops while disabled.
//
// The installed sink is deliberately INERT: the real clock / frame counter / local slot /
// match-liveness are supplied so the core's bookkeeping is consistent, but the wire send,
// health read, skill multipliers and every engine / award / camera / animation effect are
// no-ops or conservative defaults. Even when enabled this is a shadow: it tracks state without
// changing gameplay, pending the real producers (hit reception, UC4 reach, puppet handling,
// HUD bar and the ext's send trampoline).
//
// Wire IDs are the exact ext bytes reserved in net-protocol.hpp (NEVT_DOWNED 0x43,
// NEVT_REVIVE 0x44); no low 0..18 event is touched.

namespace net::match::downed_bridge {

    // Master gate. false = the whole bridge is dormant (the current, review-first state).
    inline constexpr bool kEnabled = false;

    void Install();
    void Tick();
    void OnEvent(uint8_t sender, uint8_t type, const uint8_t* data, uint8_t size);
    void Reset();
    bool Installed() noexcept;

} // namespace net::match::downed_bridge
