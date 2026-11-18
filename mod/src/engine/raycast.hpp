#pragma once

#include "sid/sid.hpp"

#include <cstdint>

// Ported from tools/ext/t2r-ext.c's world-ray helpers (WorldRayKick / RayT / SglCam and the
// LocAt offset-locator builder). A world-geometry ray the way the game's own scripts build one:
// both ends are locators offset from the camera-through-reticle locator, never bare points
// (rays from points hit at t 0.06 everywhere - see the ext's notes). The result is async: kick
// on one frame, read the hit fraction on a later one.
//
// The engine asserts (a hard crash) on a non-finite / huge cast start, so every entry point
// here refuses a bad point before it reaches the engine.

namespace engine::raycast {

    // Collision masks. 0x207 is the scripts' static-world mask; 0x1b07 is winmm's shot mask
    // (characters + foliage too).
    inline constexpr uint32_t kMaskStaticWorld = 0x207;
    inline constexpr uint32_t kMaskShot        = 0x1b07;

    // camera-through-reticle-locator: the ray origin the game's own scripts use. 0 = none.
    uint64_t CameraLocator();

    // World position / rotation of the camera locator. false when unavailable or non-finite.
    bool CameraPosition(Vector3& out);
    bool CameraRotation(Quaternion& out);

    // World ray from `from` to `to`, both world points. Returns the async job id; 0 = refused
    // (non-finite input, no camera, engine refused). Read it with RayT() on a later frame.
    // `ignore` is the object the cast passes through (kick-raycast's 3rd arg); `mask` its 4th.
    uint64_t KickWorldRay(const Vector3& from, const Vector3& to,
                          uint32_t mask = kMaskStaticWorld,
                          uint64_t ignore = SID("player"));

    // wait-raycast-t of a job: the hit fraction along the ray (0..1). -1.0f = no result,
    // -2.0f = no job was given.
    float RayT(uint64_t job);

}
