#pragma once

#include <cstddef>
#include <cstdint>

// Gated integration seams for the melee logic cores ported in this batch
// (hitrel, melee-dodge, melee-hit-react, melee-exchange). The cores themselves are pure
// modules behind Sink interfaces; this bridge is the ONE place that installs those sinks and
// fans the frame tick / net-event dispatch / session reset out to them.
//
// IMPORTANT: kEnabled is false. Nothing here runs in a shipping build yet, so the ext remains
// the behaviour owner and no t2r-*.txt cutover is flipped. The call sites in boot::FrameTick and
// net-replication are unconditional and cheap: each bridge function no-ops while disabled.
// When a feature's real producers are ported, enable that feature here (and only then disable
// the corresponding ext module via its toggle), rather than sprinkling sinks across the codebase.
//
// The sinks installed here are deliberately INERT: they supply the real clock / local slot /
// match-liveness so a core's bookkeeping is consistent, but every engine / transport effect is a
// no-op and the clip table / RTT / peer-state gates return conservative defaults. So even when
// enabled this bridge is a shadow: it tracks state without changing gameplay, pending the actual
// pad / contact / animation / relay producers.
//
// Wire IDs are the exact ext bytes reserved in net-protocol.hpp; no low 0..18 event is touched.

namespace net::match::melee_bridge {

    // Master gate. false = the whole bridge is dormant (the current, review-first state).
    // Change to true per-feature as each producer lands; the ext toggle is flipped separately.
    inline constexpr bool kEnabled = false;

    // Idempotent. With kEnabled == false this is a no-op. Otherwise installs one inert sink per
    // core (the clock / liveness are real, all effects are gated).
    void Install();

    // Called once per frame from boot::FrameTick (after engine::frame::Tick, so NowMs is valid).
    void Tick();

    // Called from net-replication's HandleNetEvent for the reserved melee opcodes. Each branch
    // feeds the matching core's Receive/Recv*; the MV3 / PSYNC wire cores stay gated (their
    // engine-free cores need an Mv3nIo / placement bridge that is not ported yet).
    void OnEvent(uint8_t sender, uint8_t type, const uint8_t* data, uint8_t size);

    // Called from net-replication's ResetRuntime (session reset / leaving the match).
    void Reset();

    // True once the inert sinks have been installed (diagnostics / future consumers).
    bool Installed() noexcept;

} // namespace net::match::melee_bridge
