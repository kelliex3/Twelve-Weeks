#pragma once

// Native port of the ext's FINAL RENDER CAMERA capture (tools/ext/t2r-ext.c:
// InstallIntroCamHook / t2r_icam_thunk / icam_filter / rcam_capture / RcamGet, with
// CamPosDir and SfxSpatial as the two consumers).
//
// The ext detours FUN_141477750 (RVA 0x1477750), the engine call that builds each
// RenderCamera from the camera manager's blended CameraLocation. Its filter chain
// (xcam / gsh / cam2 / continuity / ...) replaces the location pointer; the final
// camera block is [0..2] = world position, [4..7] = quaternion x/y/z/w, and its
// view axis is +Z of that quaternion.
//
// This module installs a READ-ONLY observer just past the ext's 15-byte prologue
// detour (RVA 0x1477750 + 15). At that point RDX holds the final camera pointer:
//   * with t2r-ext.dll loaded, its thunk replaced RDX with icam_filter's result;
//   * without it, RDX is the engine's blended location, which equals the final
//     camera in normal play (no cinematic filter active).
// The observer never writes the camera, so ext stays the behaviour owner for the
// intro / melee / down / execution camera chain. It runs on the render thread and
// publishes a seqlock snapshot that net::audio::music_sight and engine::pcm read
// on the gameplay thread; both prefer it and keep their existing fallbacks.
//
// Opt out with t2r-no-render-camera.txt next to the exe: Get() then answers false
// and both consumers use their old camera sources. The read-only hook stays
// installed either way (it cannot change camera behaviour).
//
// DIVERGENCE that remains: the hook observes the final camera but does not drive
// camera *modification* or a track evaluator (that larger gap is in PHASE1.md §5).
// FOV is not captured here; the ext reads it separately from camera-get-fov-degrees.

#include <cmath>
#include <cstdint>

namespace engine::render_camera {

    // ---- pure core (no engine headers; unit-testable) ------------------------------

    // ext RcamGet rejects a snapshot older than 300 ms.
    inline constexpr uint64_t kMaxAgeMs = 300;

    // o = q * {0,0,1} * q^-1, the ext's QRot(q, Z) with q = {x,y,z,w}. The engine
    // camera's forward axis is +Z (xcam / intro-cam calibration in ext), matching
    // Quaternion::rotate(Vector3(0,0,1)) in sid.hpp.
    inline void ForwardFromQuaternion(const float q[4], float out[3]) noexcept
    {
        const float tx = 2.0f * q[1];       // 2 * (q.y*vz - q.z*vy), v = +Z
        const float ty = -2.0f * q[0];      // 2 * (q.z*vx - q.x*vz)
        const float tz = 0.0f;              // 2 * (q.x*vy - q.y*vx)
        out[0] = q[3] * tx + (q[1] * tz - q[2] * ty);
        out[1] = q[3] * ty + (q[2] * tx - q[0] * tz);
        out[2] = 1.0f + q[3] * tz + (q[0] * ty - q[1] * tx);
    }

    // ext rcam_capture: the captured quaternion must be finite and near unit length.
    inline bool QuaternionOk(const float q[4]) noexcept
    {
        const float n = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
        return std::isfinite(n) && n > 0.9f && n < 1.1f;
    }

    // A published (timestamp != 0) snapshot younger than kMaxAgeMs.
    inline bool Fresh(uint64_t nowMs, uint64_t stampMs) noexcept
    {
        return stampMs != 0 && nowMs >= stampMs && nowMs - stampMs <= kMaxAgeMs;
    }

    // ---- engine-facing API --------------------------------------------------------

    // Install the read-only observer. Safe to call more than once; logs and returns
    // false if this exe build does not match the ext's pinned call-site signature.
    bool Install();

    // Latest final camera (position + unit forward). false when opted out, stale,
    // never captured, or captured with an invalid quaternion. Thread-safe.
    bool Get(float pos[3], float fwd[3]);

    // Clear the snapshot (level reload / test helper).
    void Reset();

} // namespace engine::render_camera
