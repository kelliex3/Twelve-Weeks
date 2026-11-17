#include "marathon-runner.hpp"

#include "net-common.hpp"
#include "net-gui2.hpp"

namespace gameplay {

    static constexpr float kSprintScale = 1.10f;

    void MarathonRunnerTick(float dt)
    {
        DC::Call<"player-set-motion-match-sprint-speed-multiplier/f">(kSprintScale);
    }
}