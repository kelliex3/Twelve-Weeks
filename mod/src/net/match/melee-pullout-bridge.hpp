#pragma once

#include <cstdint>

// Native owner of tools/ext/melee_pullout.c when t2r-ext.dll is absent. The
// ext is the only cutover: every send, damage event, health / state write and
// prompt feed here no-ops while it is loaded, so the ext keeps its own wire and
// QTE state. The exact supine hand-off uses two signature-guarded VA detours on
// the engine's CharacterProne::SetSupine / GoProne; both are installed only when
// the ext is absent and fall back to the script route on a build mismatch.
//
// The wire stays the ext's MSG_MEX family 5: [u8 5][u8 op][u8 id][u8 0][payload]
// with INTRO 11 (attacker -> victim), RES 12 (victim -> attacker), END 13 and
// CAM 14 (byte 1 = play the native procedural SP camera on both screens).
// Family-5 bodies are fanned here from melee-bridge.cpp;
// every other NEVT_MEX body still goes to gameplay::melee_exchange::Receive().

namespace net::match::melee_pullout_bridge {

    inline constexpr bool kEnabled = true;

    void Install();
    void Tick();
    void PostNetTick();   // camera / pair placement, after replication
    void OnEvent(uint8_t sender, const uint8_t* body, uint8_t size);
    void Reset();
    bool Installed() noexcept;

} // namespace net::match::melee_pullout_bridge
