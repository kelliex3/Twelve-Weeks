#pragma once

// Native producer for the ext's MusicSlotSeen / MusicSlotCos sampler seams in music-tier.
// Faithful port of the two ext samplers (t2r-ext.c ~85796):
//   * camera = ext CamPosDir(): final render camera (engine/render-camera.*, the ext's icam
//     filter hook) first, then the real camera-through-reticle locator + reticle-dir native
//     fallback, with the same 0.5 < n < 1.5 normalization gate. Both reticle calls are real in
//     ext; the dead call was "camera-position", which neither side uses;
//   * chest = the peer PUPPET LOCATOR + 1.1 m (engine::object::EntityWorld(p.npcSid), the same
//     exe+0x133DB30(proc+0x100) read ext makes through EntityFromHandle), never the replicated
//     net position. No locator -> no sight / no cosine, exactly like ext;
//   * "seen" = !tagHidden(slot) + the ext depth/cone gate + TagScreen inside the HudX0()/HudX1()/y
//     box. TagScreen is ext's exact pair: the engine attachable projection (FUN_141393b00 with the
//     camera from FUN_1413904d0(0), signature-checked as in ext) when available, else the
//     world-space-to-screen-space native; both are rescaled with HudProjX into the 16:9 text box.
//     t2r-no-tag-attach-proj.txt forces the native path, as in ext;
//   * tagHidden(slot) is ext's default tag body-visibility job ported locally: 12 weighted points
//     (joint locators where available, else root offsets), static-world rays kicked every 6th
//     frame per slot and read back next frame, clear when (1-t)*dist > 0.35 m, visible when the
//     weighted clear fraction > 5%, two hidden samples before the slot goes hidden
//     (g_tagHidden = !visible). Stance comes from the same wire flags ext's peer bytes carry:
//     PlayerNetState.isProne / .isCrouched (ext peer+0x86 / +0x85) plus PuppetDowned for the
//     down/dying term. The tier's 500 ms sight dwell / 300 ms break still absorb the one-frame
//     async-ray latency.
//
// Camera, chest, stance, frustum, projection, on-screen box and visibility sampling all match ext.
// t2r-no-render-camera.txt makes the reticle tier primary -- a useful A/B against the observer.
//
// Enabled by default (ext's MusicSlotSeen / MusicSlotCos are always on within the tier). Opt out
// with t2r-no-music-sight.txt next to the exe (checked each Install / Tick); disabling it just
// clears the sampler state, it does not take any producer away from ext. Because the tier also
// gates the alert sting, an opt-out here can let Combat() fire where the ext would have gone hot.

namespace net::audio::music_sight {

    inline constexpr bool kEnabled = true;

    // ---- pure ext geometry helpers (no engine headers; unit-testable) ------------------

    // ext MusicSlotSeen frustum gate: depth >= 0.5 m, len >= 0.1 m, depth/len >= 0.6.
    inline bool SeenFrustumOk(float depth, float len) noexcept
    {
        return len >= 0.1f && depth >= 0.5f && depth / len >= 0.6f;
    }

    // ext BodyVisTick clear test: a hit at fraction t is unoccluded when the remaining distance
    // (1 - t) * dist is more than 0.35 m past the sampled body point.
    inline bool BodyVisClear(float t, float dist) noexcept
    {
        return t >= 0.0f && t < 1.0f && (1.0f - t) * dist > 0.35f;
    }

    // ext BodyVisTick verdict: weighted share of clear points above 5% == body visible.
    inline bool BodyVisFraction(float clearWeight, float totalWeight) noexcept
    {
        return totalWeight > 0.0f && clearWeight / totalWeight > 0.05f;
    }

    // ext MusicSlotSeen on-screen box after TagScreen's HudProjX (x in the 16:9 text box,
    // widened by the ultrawide text offset; y in the fixed 720-high viewport).
    inline bool TagOnScreen(float x, float y, float textXoff) noexcept
    {
        return x >= -textXoff && x <= 1280.0f + textXoff
            && y >= 0.0f && y <= 720.0f;
    }

    // Install the sampler callbacks into music_tier (no-op when opted out via sidecar flag).
    void Install();

    // Per frame, BEFORE music_tier::Tick(): resolve the camera, refresh tag body-visibility,
    // then sample the peer cones / visibility into the samplers. Opted out: clears sampler state.
    void Tick();

    void Reset();

} // namespace net::audio::music_sight
