// ported from tools/ext/t2r-ext.c (AnimateFrom / AnimateAt / AnimLen / AllocEntryBad)

#include "anim-prims.hpp"

#include "log.hpp"             // TO1_LOG must precede engine/script.hpp
#include "ptr.hpp"
#include "script.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace engine::anim {

    namespace {

        // tools/ext/paired.h: the engine rejects an animate_ ap-reference whose first
        // two dwords are not this magic. The frame is unbound (world space); the
        // parent locator stays identity and the local locator is the apReference.
        struct alignas(16) BoundFrame {
            uint32_t magic[2];
            uint64_t binding;
            float    parent[8];
            float    local[8];
        };

        static_assert(sizeof(BoundFrame) == 0x50);

        void FillFrame(BoundFrame& bf, const float ap7[7])
        {
            bf.magic[0] = bf.magic[1] = 0x7fccccccu;
            bf.binding = 0;
            std::memset(bf.parent, 0, sizeof bf.parent);
            bf.parent[7] = 1.0f;
            std::memset(bf.local, 0, sizeof bf.local);
            bf.local[0] = ap7[0];
            bf.local[1] = ap7[1];
            bf.local[2] = ap7[2];
            bf.local[3] = 1.0f;
            for (int k = 0; k < 4; ++k) bf.local[4 + k] = ap7[3 + k];
        }

    } // namespace

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

    uint64_t AnimateAt(StringId64 entity, const char* anim, float blend, const float ap7[7])
    {
        if (!anim || !ap7)
            return ~0ULL;
        const uint64_t arr = MEMORY::ALLOC_ARRAY(16, 3, 1);
        if (!arr || arr == ~0ULL || !engine::ptr::Readable(static_cast<uintptr_t>(arr), 8))
            return ~0ULL;
        uint64_t* tab = *reinterpret_cast<uint64_t**>(static_cast<uintptr_t>(arr));
        if (!tab || !engine::ptr::Readable(reinterpret_cast<uintptr_t>(tab), 24))
            return ~0ULL;
        uint8_t* e[3] = {
            reinterpret_cast<uint8_t*>(tab[0]),
            reinterpret_cast<uint8_t*>(tab[1]),
            reinterpret_cast<uint8_t*>(tab[2]),
        };
        for (uint8_t* p : e)
            if (!p || AllocEntryBad(reinterpret_cast<void*>(static_cast<uintptr_t>(arr)), p, 16))
                return ~0ULL;

        static std::atomic<uint32_t> s_frame{ 0 };
        alignas(16) static BoundFrame s_frames[4];
        BoundFrame& frame = s_frames[s_frame.fetch_add(1, std::memory_order_relaxed) & 3];
        FillFrame(frame, ap7);

        const int32_t k0 = 28;      // blend-in seconds
        std::memcpy(e[0], &k0, 4);
        std::memcpy(e[0] + 8, &blend, 4);
        const int32_t k1 = 10;      // ap-reference (BoundFrame)
        std::memcpy(e[1], &k1, 4);
        const uint64_t framePtr = reinterpret_cast<uintptr_t>(&frame);
        std::memcpy(e[1] + 8, &framePtr, 8);
        const int32_t k2 = 32;      // hold the final pose
        std::memcpy(e[2], &k2, 4);
        std::memset(e[2] + 8, 1, 1);

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
