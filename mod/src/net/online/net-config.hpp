#pragma once

#include <cstdint>

namespace net::config {

    struct MapInfo {
        const char* name;
        const char* preview;  // preview image path
        const char* task;     // task to load when the match starts
        uint8_t     spawns;   // how many "to1-spawn-point-N"
    };

    // The client pings each so the matchmaker can pick the fairest one.
    struct RelayInfo {
        const char* ip;
        uint16_t    port; // relay UDP port
    };

    enum class State { Idle, Fetching, Ready, Failed };

    void  Begin();
    void  Tick();
    void  Reset();
    State GetState();

    bool  VersionRejected();
    bool  TokenRejected();

    int     MapCount();
    MapInfo Map(int index);

    int       RelayCount();
    RelayInfo Relay(int index);

}