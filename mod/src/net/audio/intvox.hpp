#pragma once

// Native sole owner of tools/ext/intvox.c: the two bodies' own voice sets speak
// the interrogation timeline (fist demand -> answer -> slam -> desperate
// answer -> kill; shiv demand -> answer), with effort-event fallbacks, the
// 140 ms speaking check, victim cut, look-at refresh and the voice-yield feed
// for the shaking foley. Every pick / timing / cut rule lives in
// intvox-core.hpp; this header is only the feed and query surface.
//
// Who calls it: boot::FrameTick() calls Tick() every frame before intfoley so
// the line feed is fresh; net-replication.cpp calls Reset() on a level / session
// reset. The paired-move state is read directly, so no new wire message and no
// producer hook is needed. Quiet() is the ext's g_int_quiet_until suppression
// surface for our own team callouts (no native producer reads it yet).
//
// Honest gaps: there is no native xface_talk / slam-blood producer, and the
// in-tree BEG_INT table stays behind TO1_INTVOX_DATA. Without a matching voice
// / look mapping or with data off, the timeline still runs but every spoken
// beat uses the engine's effort events. The native animation is not slowed, so
// the intwarp clip -> real mapping is present in the core but defaults off.

#include <cstdint>

namespace net::audio::intvox {

    void Tick();
    void Reset();

    bool Active() noexcept;
    bool Quiet(uint64_t now) noexcept;

} // namespace net::audio::intvox
