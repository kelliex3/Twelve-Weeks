#pragma once

// Ported from tools/ext/immersion.c's NEAR MISS producer: the shooter replicates the line its
// shot actually flew (ext MSG_SHOTRAY 0xAF, our NEVT_SHOTRAY), and a receiver that is not the
// shooter or a teammate computes the segment's closest approach to its own head. If the line
// passed within 2.5 m, at least 1 m from the muzzle, with clear line of sight, and no damaging
// hit from that slot within 250 ms of the line, a delayed whizz is played and the alert sting
// fires ("near miss out of the quiet"). Ext immersion.c calls AlertstingCombat even when the
// PCM voice fails; that is preserved here.
//
// The whizz PCM wave table (private sp_bullet_by.h / sp_bullet_by.pcm, SB_* waves) is NOT in
// this repo, so playback stays behind T2R_DATA_BULLET_BY. The alert-sting call and the whole
// geometry / gating path are always live.
//
// Opt-outs next to the exe (ext): t2r-no-shotray.txt (sender only), t2r-no-near-miss.txt
// (playback / receiver). The two are independent in ext despite a comment suggesting both.
//
// DIVERGENCE: the ext's shot ray comes from the engine's aim helper (player+0x3278 via
// FUN_140c72440); this port uses the camera-through-reticle position + direction (the same
// pair the ported sight sampler and PCM listener use). Its origin is slid to the point on the
// ray nearest the chest, like the ext, so the segment starts at the muzzle, not the camera.

#include "net/protocol/net-protocol.hpp"

#include <cmath>
#include <cstdint>

namespace net::audio::near_miss {

    // ---- pure core (no engine state; unit-testable) ------------------------------

    struct ClosestApproach {
        float t;        // projection along the ray from origin (upper-clamped to len)
        float lat;      // lateral distance from head to the closest point
        float cp[3];    // closest point on the segment
    };

    // Closest approach of the segment o + t*d, t in [0, len], to `head`. `d` must be unit
    // length. The lower bound (t >= 1 m from the muzzle) is a policy gate applied by the
    // caller via ShotIsNear, kept out of the geometry so it stays pure.
    inline ClosestApproach ClosestPointOnSegment(
        const float o[3], const float d[3], float len, const float head[3]) noexcept
    {
        float t = (head[0] - o[0]) * d[0]
                + (head[1] - o[1]) * d[1]
                + (head[2] - o[2]) * d[2];
        if (t > len)
            t = len;
        const float cp[3] = { o[0] + t * d[0], o[1] + t * d[1], o[2] + t * d[2] };
        const float dx = head[0] - cp[0];
        const float dy = head[1] - cp[1];
        const float dz = head[2] - cp[2];
        return { t, std::sqrt(dx * dx + dy * dy + dz * dz), { cp[0], cp[1], cp[2] } };
    }

    // ext immersion.c: reject a line behind / at the muzzle, or more than 2.5 m from the head.
    inline bool ShotIsNear(const ClosestApproach& a,
                           float minAlong = 1.0f, float maxLat = 2.5f) noexcept
    {
        return a.t >= minAlong && a.lat <= maxLat;
    }

    // int8 direction quantisation used on the wire (d * 127, rounded away from zero).
    inline int8_t QuantDir(float d) noexcept
    {
        const float v = d * 127.0f + (d < 0.0f ? -0.5f : 0.5f);
        if (v >= 127.0f) return 127;
        if (v <= -127.0f) return -127;
        return static_cast<int8_t>(static_cast<int>(v));
    }

    inline float DequantDir(int8_t q) noexcept
    {
        return static_cast<float>(q) / 127.0f;
    }

    // Receiver dir sanity before normalising: squared length in [0.5, 1.5].
    inline bool DirValid(const float d[3]) noexcept
    {
        const float dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        return dd >= 0.5f && dd <= 1.5f;
    }

    // ext gain curve: full within 0.25 m, falls to 0.45x at 2.5 m, and rolls off past 10 m.
    inline float WhizzGain(float lat, float along) noexcept
    {
        float lf = (lat - 0.25f) / 2.25f;
        if (lf < 0.0f) lf = 0.0f;
        if (lf > 1.0f) lf = 1.0f;
        float rf = along < 10.0f ? 1.0f : 1.0f - (along - 10.0f) / 130.0f;
        if (rf < 0.0f) rf = 0.0f;
        return 0.95f * (1.0f - 0.55f * lf) * rf;
    }

    // ---- engine-facing API -------------------------------------------------------

    void OnLocalShot();                                  // sender, called from hk_Shot
    void OnShotRay(int slot, const ShotRayEvent& evt);   // receiver, from net-replication
    void NoteHitFrom(int slot, uint64_t nowMs) noexcept; // recv_hit_hook per-slot hit stamp
    void Tick();                                         // per-frame, before alertsting::Tick
    void Reset();

} // namespace net::audio::near_miss
