#pragma once

#include <cstdint>

namespace net::connect {

    enum class Phase {
        Idle,
        Running,
        Complete,
        Failed,
    };

    void        Begin();
    void        Tick();
    void        Reset();

    Phase       GetPhase();
    float       Progress();
    const char* StatusText();

}