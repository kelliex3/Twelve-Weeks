#pragma once

#include <cstdint>

// Native owner of tools/ext/sendoff.c. The shared match-found horn (mfnr) is the
// only trigger: its t=0 drives the birds, crows, faction clip, UI fade and black
// fade. Pure timing / camera / flock rules live in sendoff-core.hpp; every DC
// spawn, widget write and PCM voice stays here.
//
// t2r-no-sendoff.txt stays read-only (ext compatibility), as does
// t2r-sendoff-birds.txt for the optional config overrides. There is no test
// sidecar trigger in the native path and no sidecar is written or flipped.

namespace menu::sendoff {

    // Called once per frame from boot::FrameTick, after mfnr::Tick has sampled
    // the horn latch. Inert while the ext owns the feature or the opt-out file
    // is present.
    void Tick(uint64_t now);

    // Cancel the sequence, fade its voices, kill its birds and restore the UI /
    // session volume. Called on session reset (net::replication::ResetRuntime).
    void Reset();

} // namespace menu::sendoff
