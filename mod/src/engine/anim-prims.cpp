// ported from tools/ext/t2r-ext.c (AnimateFrom / AnimLen / AllocEntryBad)

#include "anim-prims.hpp"

#include "ptr.hpp"
#include "script.hpp"

#include "log.hpp"

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace engine::anim {

    bool AllocEntryBad(const void* hdr, const void* entry, size_t n)
    {
        if (!entry)
            return true;
        // The entries of an array the engine's script allocator just handed us live in the
        // same arena as the array header; a fresh page was a cold bad-write probe the ext
        // measured stalling frames. Anything within 1 MB of the header is trusted.
        const intptr_t d = reinterpret_cast<intptr_t>(entry) - reinterpret_cast<intptr_t>(hdr);
        if (d > -(1 << 20) && d < (1 << 20))
            return false;
        return !engine::ptr::Writable(reinterpret_cast<uintptr_t>(entry), n);
    }

    uint64_t AnimateFrom(StringId64 entity, const char* anim, float blend, float phase)
    {
        if (!anim)
            return ~0ULL;
        // #%alloc-array(single-frame, 16, 2, 1): two 16-byte entries.
        const uint64_t arr = MEMORY::ALLOC_ARRAY(16, 2, 1);
        if (!arr || arr == ~0ULL || !engine::ptr::Readable(static_cast<uintptr_t>(arr), 8))
            return ~0ULL;
        uint64_t* tab = *reinterpret_cast<uint64_t**>(static_cast<uintptr_t>(arr));
        if (!tab || !engine::ptr::Readable(reinterpret_cast<uintptr_t>(tab), 16))
            return ~0ULL;
        uint8_t* e0 = reinterpret_cast<uint8_t*>(tab[0]);
        uint8_t* e1 = reinterpret_cast<uint8_t*>(tab[1]);
        if (!e0 || !e1 || AllocEntryBad(reinterpret_cast<void*>(static_cast<uintptr_t>(arr)), e0, 16) ||
            AllocEntryBad(reinterpret_cast<void*>(static_cast<uintptr_t>(arr)), e1, 16))
            return ~0ULL;
        const int32_t k0 = 28;      // blend-in seconds
        std::memcpy(e0, &k0, 4);
        std::memcpy(e0 + 8, &blend, 4);
        const int32_t k1 = 58;      // start phase 0..1
        std::memcpy(e1, &k1, 4);
        std::memcpy(e1 + 8, &phase, 4);
        return DC::Call<"animate_">(entity, SID(anim), arr);
    }

    float Len(StringId64 entity, const char* clip)
    {
        if (!clip)
            return 0.0f;
        const uint64_t rd = DC::Call<"get-animation-duration">(entity, SID(clip));
        float dur = 0.0f;
        const uint32_t dl = static_cast<uint32_t>(rd);
        std::memcpy(&dur, &dl, 4);
        return rd != ~0ULL && dur > 0.2f && dur < 30.0f ? dur : 0.0f;
    }

}
