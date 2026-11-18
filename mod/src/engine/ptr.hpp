#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

// Ported from tools/ext/t2r-ext.c's IsBadReadPtr guards, which every feature used before
// touching an engine pointer it did not own. VirtualQuery answers the same question
// ("is [p, p+bytes) committed, readable memory?") without the deprecated API's
// exception-swallowing behaviour.

namespace engine::ptr {

    inline bool Readable(uintptr_t p, size_t bytes)
    {
        if (!p)
            return false;
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<const void*>(p), &mbi, sizeof(mbi)))
            return false;
        if (mbi.State != MEM_COMMIT)
            return false;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
            return false;
        const uintptr_t regionEnd =
            reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        return p + bytes <= regionEnd;
    }

}
