#pragma once

// Native sole owner of tools/ext/intfoley.c: the extra shaking body foley of a
// 2013 interrogation. Window / gap / no-double / voice-yield rules live in
// intfoley-core.hpp; the shared cue recipes live in explus-core.hpp; the
// paired-move, sidecar and explus sinks stay in intfoley.cpp. No new network
// message.
//
// Who calls it: boot::FrameTick() calls Tick() every frame;
// net-replication.cpp calls Reset() on a level / session reset. The voice clash
// / victim-line feeds are inert until a native intvox producer exists; the
// SetLine() API is the seam for that future producer.

#include "intfoley-core.hpp"

namespace net::audio::intfoley {

    // Inert intvox feed. The native tree has no intvox timeline / intwarp shift,
    // so the default (on = false) keeps every voice yield answer false.
    void SetLine(const core::LineFeed& line);

    void Tick();
    void Reset();

} // namespace net::audio::intfoley
