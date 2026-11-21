#pragma once

#include <cstdint>

#include "net-smooth.hpp"

// Native integration of net::netsmooth, replacing the ext's winmm hooks / fixed-VA patches.
// This is a LIVE CUTover: there is no kEnabled flag. The smoother owns the remote puppet's
// locator whenever its buffer is primed; net-replication falls back to the existing native
// interpolation only while the buffer is empty/unusable (startup, first few lists, a sender
// clock change). The engine netsim/live-A-B check is still outstanding -- see PHASE1.md.
//
// Threading: the game thread drains UDP in net::session::Update() and feeds OnState/OnAnim
// there; Apply/Tick run later in the same frame from replication::Tick(). Single-threaded,
// so the core needs no lock (the ext needed an SRWLOCK because its apply hook ran on winmm's
// render thread).

namespace net::netsmooth::bridge {

    // Clock source for all three: engine::frame::NowMs(), the native QPC monotonic ms clock.
    void OnState(uint8_t slot, uint32_t seq, const float pos[3], const float quat[4]);
    void OnAnim(uint8_t slot, uint32_t seq, uint32_t sendMs);

    // Produce the placement to write to LOCATOR::SET_OBJECT_LOCATOR. `native` is the existing
    // interpolation result, used for the 100 ms handover and the stutter comparison.
    // `onewayMs` is NetPlayer::PingMs()/2 (0 when unknown). Returns false while unprimed.
    bool Apply(uint8_t slot, const Transform& native, Transform& out, double onewayMs);

    // Rolls the 10 s per-slot stats window (mirrors the ext's "net stats:" log).
    void Tick();

    void Reset(uint8_t slot);
    void ResetAll();

} // namespace net::netsmooth::bridge
