#pragma once

// Native sole owner of the DOWNED VOX sub-feature of tools/ext/downed_vox.c
// (lines 9-97: dv_cut / dv_sid / dv_one / dv_tick) and its headshot-death half
// (lines 108-164: hs_mark / hs_unmark / hs_recent / hs_arm / hs_tick). Every
// downed body is judged locally from state already on this client (the mod's
// downed state and the replicated isProne flag); there is NO new wire message.
//
// Lying bodies groan on a 6-11 s cadence, crawling bodies breathe every
// 2.6-4.2 s, never over another line of the same body. The first 2600 ms of a
// down, paired moves, dying and the two sidecars are silent, and a body that
// stops qualifying cuts a still-playing line with stop-vox. A headshot death
// cuts every vocal on the body and emits at most one engine gargle effort event
// (never the voiced hdsht dialogue take).
//
// Who calls it: MarkHeadshot / UnmarkHeadshot from the hit / kill paths,
// ArmHeadshot when a headshot kill becomes known (optionally forced), Tick()
// once per frame, Reset() on match / replication reset. Active() reports
// whether any state is still live.
//
// Honest gaps: the ext's faction / look -> voice mapping (vx_own_fac,
// vx_fac_of_slot, vx_voice_of_body) is NOT ported. The feed therefore passes
// fac = -1 / voice = -1 for every body, so with private data built no groan or
// breath take is selected yet: the 3000 ms no-line retry is taken instead.
// LookOrDefault() is consulted only for a log-only female hint. Crawling is the
// replicated PlayerNetState.isProne for peers (the ext read the peer's 0x86
// stance byte); our own body has no stored local PlayerNetState, so its crawl
// feed is false until a local stance producer exists. The dying, psync_busy and
// gx_slot_running gates are approximated by LocalDownState() and
// LocalInPairedMove(). All of these only affect which line is chosen, not the
// headshot path.
//
// boot::ExtLoaded() yields every entry point. The private VXD_* line table is
// read only through downed-vox-data.hpp / TO1_DOWNED_VOX_DATA. Pure rules and
// the unit test live in downed-vox-core.hpp and
// mod/tests/downed_vox_core_test.cpp.

#include "downed-vox-core.hpp"

#include <cstdint>

namespace net::audio::downed_vox {

    void MarkHeadshot(uint64_t ent);
    void UnmarkHeadshot(uint64_t ent);
    void ArmHeadshot(uint64_t ent, bool force, int delayMs);
    void Tick();
    void Reset();

    bool Active() noexcept;

} // namespace net::audio::downed_vox
