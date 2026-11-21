#pragma once

// Gated integration seam for the pure xcam routing core. There is no native final render-camera
// / xcam_start hook in the mod, so OnCameraStart() has no producer yet. The ext remains the
// behaviour owner while kEnabled is false.

namespace net::audio::xcam_bridge {

    inline constexpr bool kEnabled = false;

    // Install the dispatch sink (a no-op while kEnabled is false). Call once at startup.
    void Init();

    // Entry a future xcam / paired-move camera hook calls with the ext's arguments:
    // role 1 = attacker, 2 = victim; `name` the move name; `killS` the authored kill seconds;
    // `upgradedOneShot` the ext's (g_gx.upg && g_gx.role == 2).
    void OnCameraStart(int role, const char* name, float killS, bool upgradedOneShot);

    void Reset();

} // namespace net::audio::xcam_bridge
