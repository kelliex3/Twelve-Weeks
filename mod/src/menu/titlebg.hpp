#pragma once

#include <cstdint>

// Native owner of tools/ext/titlebg.c. The title/main menu stack gets the
// Hillcrest overlook camera, slow sway, FOV and render layer from this module;
// the ext's render-camera hook is not needed. Each frame we write the camera
// through CAMERA::SET_CAMERA_POSITION and count the writes -- those counts are
// the readiness signal the ext took from hook hits.
//
// t2r-menu-boat.txt, t2r-title-cam.txt and t2r-title-rs.txt stay read-only
// compatibility sidecars. No toggle is written or flipped.

namespace menu::titlebg {

    // Called once per frame from boot::FrameTick. Inert while the ext owns the
    // feature, t2r-menu-boat.txt is present, or the title stack is gone.
    void Tick(uint64_t now);

    // Restore the UI / black cover and drop the boot / sway state on session
    // reset (net::replication::ResetRuntime).
    void Reset();

    // Native equivalent of the ext's g_mcam_hits delta (camera writes so far).
    uint32_t CameraWrites();

} // namespace menu::titlebg
