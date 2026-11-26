#pragma once

#include "customize-stage-core.hpp"

#include <cstdint>

// Native owner for tools/ext/customize_stage.c (build 191). This module owns
// the whole CUSTOMIZE stage: the read-only sidecar state, scene / front
// faction, derived geometry, the fade + camera cut, composed NPC spawn / pose,
// photo-mode lights, the stage freecam and the compaction-time kill / reap.
// Pure rules live in customize-stage-core.hpp.
//
// It yields while the legacy ext is loaded (the ext is the deprecated path).
// The camera writer, pad / key samplers and engine pointer seams are the same
// ones the other native owners use; their parity with the ext's final render
// camera track cannot be checked without a live game build.

namespace net::ui::customize::stage {

    // Called once from boot::FrameTick, before the CUSTOMIZE page input ticks
    // so a freecam frame can suppress their input. `pageUp` covers both the
    // CUSTOMIZE and CUSTOMIZE | CHALLENGES pages; `editFac` is 0 WLF / 1
    // Seraphites; `currentSide` is the faction the player plays (-1 unknown).
    void Install(void* gameModule);       // stores the module; hook installed on first tick
    void Tick(uint64_t now, bool pageUp, int editFac, int currentSide);

    // Level reload / test helper. Keeps the installed hook.
    void Reset();

    [[nodiscard]] bool Active();
    [[nodiscard]] bool FreecamActive();  // true only while the stage freecam flies
    [[nodiscard]] bool Off();            // t2r-no-customize-stage.txt, read-only

    // Live photo-mode light controls driven by the CUSTOMIZE page's [ / ] / L.
    void LightStep(int dir);
    [[nodiscard]] float LightPercent();
    [[nodiscard]] const char* CineStep();

    // One queued removal, called from the compaction hook before the pass.
    void KillDrain();

    [[nodiscard]] const core::Tuning& TuningState();
    [[nodiscard]] const core::LightSettings& Lights();
    [[nodiscard]] const core::Layout& SceneLayout(int scene);
    [[nodiscard]] const core::Derived& Derived();
    [[nodiscard]] const core::SwapFrame& Swap();
    [[nodiscard]] int FrontFaction();

} // namespace net::ui::customize::stage
