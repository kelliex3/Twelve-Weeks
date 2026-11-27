#pragma once

// Ported from tools/ext/immersion.c's NEAR MISS producer: the shooter replicates the line its
// shot actually flew (ext MSG_SHOTRAY 0xAF, our NEVT_SHOTRAY), and a receiver that is not the
// shooter or a teammate computes the segment's closest approach to its own head. If the line
// passed within 2.5 m, at least 1 m from the muzzle, with ext-exact two-ray line of sight and
// no damaging hit from that slot within 250 ms of the line, a delayed whizz is played and the
// alert sting fires ("near miss out of the quiet"). Ext calls AlertstingCombat even when the
// PCM voice fails; that is preserved here.
//
// The whizz PCM wave table (private sp_bullet_by.h / sp_bullet_by.pcm, SB_* waves) is NOT in
// this repo, so playback stays behind TO1_BULLET_BY_PCM (near-miss-pcm.hpp). The alert-sting
// call and the whole geometry / gate path are always live. Pan uses the ext's camera-right
// basis and 0.85 clamp, wave picks use the ext's xorshift32, and the gain keeps the ext's
// unfloored distance tail.
//
// Opt-outs next to the exe (ext): t2r-no-shotray.txt (sender only), t2r-no-near-miss.txt
// (playback / receiver). The two are independent in ext despite a comment suggesting both.
// boot::ExtLoaded() makes all three producers yield while the legacy ext is loaded.
// Sidecars next to the exe are read-only here: the flags are only queried, never written.
//
// The shot source is now ext's: Tick() polls the held clip for a decrease on the same gun
// with a recent player-is-firing? stamp, then reads the engine's aim helper (player+0x3278
// via FUN_140c72440), slides the origin toward our chest and sends the unchanged 17-byte
// NEVT_SHOTRAY payload at most every 66 ms. The receiver's ring is ext's interlocked
// 16-entry sequence-indexed ring with per-record publish/unpublish, and LOS is ext's
// 8-entry cache keyed by the puppet handle (not the net slot). The native receive path
// still runs on the game thread, although the ring layout is now safe for the ext's
// job-thread writers too.
//
// REMAINING GAP: the private PCM wave table is not in this repo, so no whizz voice is
// available until TO1_BULLET_BY_PCM is built (the alert sting still fires, like ext).
// There is no local runtime verification of the raw RVAs above, and no live A/B against
// the ext client; the pure rules are covered by mod/tests/near_miss_core_test.cpp only.

#include "near-miss-core.hpp"

namespace net::audio::near_miss {

    // The pure helpers remain reachable through the owner's namespace for existing callers.
    using core::ClosestApproach;
    using core::ClosestPointOnSegment;
    using core::ShotIsNear;
    using core::QuantDir;
    using core::DequantDir;
    using core::DirValid;
    using core::AimRayValid;
    using core::ClipDecrementShot;
    using core::WhizzGain;

    // ---- engine-facing API -------------------------------------------------------

    // Deprecated no-op shim: ext has no shot hook. The true producer is Tick()'s
    // clip-decrement poll; remove the net-hooks.cpp call when integrating.
    void OnLocalShot();
    void OnShotRay(int slot, const ShotRayEvent& evt);   // receiver, from net-replication
    void NoteHitFrom(int slot, uint64_t nowMs) noexcept; // recv_hit_hook per-slot hit stamp
    void Tick();                                         // per-frame sender + receiver, before alertsting::Tick
    void Reset();

} // namespace net::audio::near_miss
