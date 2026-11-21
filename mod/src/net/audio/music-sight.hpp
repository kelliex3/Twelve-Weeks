#pragma once

// Native producer for the ext's MusicSlotSeen / MusicSlotCos sampler seams in music-tier.
// This is the "no mod equivalent" gap from the old TODO, now implemented against the APIs the
// mod actually has:
//   * the final render camera captured by engine/render-camera.* (the ext's icam filter hook)
//     as PRIMARY, with the camera-through-reticle locator + "camera-get-dir-through-reticle"
//     native as an extra robustness tier -- the same order engine::pcm's listener uses;
//   * a world-space chest point from the peer's replicated position + 1.1 m (the ext reads the
//     puppet locator + 1.1 m);
//   * occlusion via the async static-world ray (KickWorldRay / RayT), the mod's equivalent of
//     the ext's tag body-visibility ray behind g_tagHidden.
//
// DIVERGENCE from ext (honest): the reticle camera tier does not exist in the ext (its native
// camera-tier call was dead), it only adds robustness when the observer has no fresh snapshot;
// t2r-no-render-camera.txt makes it the primary again. The chest comes from replicated net state
// rather than the exact puppet locator. The tier's 500 ms sight dwell and 300 ms break absorb a
// frame or two of async-ray latency.
//
// Enabled by default (ext's MusicSlotSeen / MusicSlotCos are always on within the tier). Opt out
// with t2r-no-music-sight.txt next to the exe (checked each Install / Tick); disabling it just
// clears the sampler state, it does not take any producer away from ext. Because the tier also
// gates the alert sting, an opt-out here can let Combat() fire where the ext would have gone hot.

namespace net::audio::music_sight {

    inline constexpr bool kEnabled = true;

    // Pure ext MusicSlotSeen frustum gate: the chest must be in front (>= 0.5 m), past the
    // minimum range, and within the forward cone (cos >= 0.6). The visibility ray is applied
    // after this in the producer. Exposed so the geometry can be tested without game headers.
    inline bool SeenFrustumOk(float depth, float len) noexcept
    {
        return len >= 0.1f && depth >= 0.5f && depth / len >= 0.6f;
    }

    // Install the sampler callbacks into music_tier (no-op when opted out via sidecar flag).
    void Install();

    // Per frame, BEFORE music_tier::Tick(): refresh the camera, sample the peer cones and kick
    // / resolve the occlusion rays. When opted out, clears the sampler state instead.
    void Tick();

    void Reset();

} // namespace net::audio::music_sight
