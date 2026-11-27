#pragma once

// Native sole owner of tools/ext/intfoley.c: the extra shaking body foley of a
// 2013 interrogation. Window / gap / no-double / voice-yield rules live in
// intfoley-core.hpp; the shared cue recipes live in explus-core.hpp; the
// paired-move, sidecar and explus sinks stay in intfoley.cpp. No new network
// message.
//
// Who calls it: boot::FrameTick() calls Tick() every frame (after intvox has
// fed the line); net-replication.cpp calls Reset() on a level / session reset.
// native intvox feeds SetLine() once per frame, so the voice clash / victim
// line guards now yield while either body is speaking.

#include "intfoley-core.hpp"

namespace net::audio::intfoley {

    // The intvox line feed. An empty feed (on = false) keeps every voice yield
    // answer false, exactly like a move with no voice line.
    void SetLine(const core::LineFeed& line);

    void Tick();
    void Reset();

} // namespace net::audio::intfoley
