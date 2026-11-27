#pragma once

// Native sole owner of tools/ext/pmsfx.c: the No Return end-of-run score sounds
// (page enter, rolling parts count, count-land accent, total stamp and the next
// page's whoosh). The cue / one-shot / duration rules live in pmsfx-core.hpp;
// the raw PCM waves stay behind the guarded pmsfx-data.hpp sink. No new wire
// message.
//
// Who calls it: net/ui/postmatch-screens.cpp's PmTick() reports the same edges
// the ext's pm_tick used (entered, select-skip, count window, count landed and
// page advance). boot/reset clears it on a match / replication reset. While
// t2r-ext.dll is loaded every entry yields to the ext.

#include "pmsfx-core.hpp"

#include <cstdint>

namespace net::audio::pmsfx {

    void OnEnter(uint64_t now);
    void OnSkip(uint64_t now);
    void OnTick(uint64_t now, bool ready, int64_t elapsedMs, int countDurMs);
    void OnFinish(uint64_t now);
    void OnPage(uint64_t now);

    void Reset();
    bool Active() noexcept;

} // namespace net::audio::pmsfx
