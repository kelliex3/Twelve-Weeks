#pragma once

#include <cstdint>
#include "net-matchmaker-protocol.hpp"

namespace net::relayping {

    void Tick();
    void Fill(uint16_t out[MM_MAX_RELAYS]);

}