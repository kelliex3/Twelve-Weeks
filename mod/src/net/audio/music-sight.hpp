#pragma once

// Native producer for the ext's MusicSlotSeen / MusicSlotCos sampler seams in music-tier.
// This is the "no mod equivalent" gap from the old TODO, now implemented against the APIs the
// mod actually has:
//   * camera position via engine::raycast::CameraPosition (camera-through-reticle locator),
//     direction via the "camera-get-dir-through-reticle" native -- the same pair engine::pcm's
//     listener uses;
//   * a world-space chest point from the peer's replicated position + 1.1 m (the ext reads the
//     puppet locator + 1.1 m);
//   * occlusion via the async static-world ray (KickWorldRay / RayT), the mod's equivalent of
//     the ext's tag body-visibility ray behind g_tagHidden.
//
// DIVERGENCE from ext (honest): the ext's PRIMARY camera was the final render camera captured
// by its icam hook, which this mod does not port. The reticle camera is the mod's available
// source (engine::pcm documents the same divergence for its listener), and the chest comes from
// replicated net state rather than the exact puppet locator. The tier's 500 ms sight dwell and
// 300 ms break absorb a frame or two of async-ray latency.
//
// Gated: kEnabled == false, so Install() leaves the tier's samplers unset and ext remains the
// behaviour owner. Enabling it only changes the mod's own tier state (alertsting gating); it
// does not take any producer away from ext. Live A/B is still required before flipping it.

namespace net::audio::music_sight {

    inline constexpr bool kEnabled = false;

    // Pure ext MusicSlotSeen frustum gate: the chest must be in front (>= 0.5 m), past the
    // minimum range, and within the forward cone (cos >= 0.6). The visibility ray is applied
    // after this in the producer. Exposed so the geometry can be tested without game headers.
    inline bool SeenFrustumOk(float depth, float len) noexcept
    {
        return len >= 0.1f && depth >= 0.5f && depth / len >= 0.6f;
    }

    // Install the sampler callbacks into music_tier (no-op while kEnabled is false).
    void Install();

    // Per frame, BEFORE music_tier::Tick(): refresh the camera, sample the peer cones and kick
    // / resolve the occlusion rays. No-op while kEnabled is false.
    void Tick();

    void Reset();

} // namespace net::audio::music_sight
