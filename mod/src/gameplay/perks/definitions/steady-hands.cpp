#include "steady-hands.hpp"

#include "net-common.hpp"
#include "net-gui2.hpp"

namespace gameplay {

    void SteadyHandsTick(float dt)
    {
        DC::Call<"player-disable-weapon-sway">();
    }
}