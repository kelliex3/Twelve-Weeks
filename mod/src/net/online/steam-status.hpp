#pragma once

// Port of the ext's Steam-only FIND MATCH gate (tools/ext/t2r-ext.c:
// g_steamOnly / SteamConnected). The helper writes
// %TEMP%\t2r-steam-status.txt = "<1|0> <unix time>" every 20 s; 1 with a fresh
// stamp means Steam is up and registered with the server. A refused press sets
// g_mmBack in the ext, so this module feeds postmatch::NoteMmRefused().

#include <cstdint>

namespace net::steam {

    // Release builds and t2r-steam-only.txt, unless t2r-linux.txt is present
    // (the Linux client has no helper and the server allows it).
    bool SteamOnly();

    // Cached for 1 s like the ext; false when the file is missing or stale.
    bool Connected();

    // true when a FIND MATCH press must be refused for a missing helper.
    bool RefuseFindMatch();

} // namespace net::steam
