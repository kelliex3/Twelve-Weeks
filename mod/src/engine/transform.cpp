// ported from tools/ext/t2r-ext.c (ProcWorld)

#include "transform.hpp"

#include "log.hpp"
#include "engine/ptr.hpp"

#include <windows.h>

#include <cstring>

namespace engine::transform {

    namespace {

        uintptr_t Base()
        {
            static const uintptr_t base =
                reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
            return base;
        }

    }

    bool ProcessWorld(uintptr_t process, float out[8])
    {
        if (!engine::ptr::Readable(process + 0x100, 0x50))
            return false;

        using FnLoc = const float* (__fastcall*)(void*, float*);
        const auto fn = reinterpret_cast<FnLoc>(Base() + 0x133DB30);
        float b[16];
        const float* l = fn(reinterpret_cast<void*>(process + 0x100), b);
        if (!l)
            return false;
        std::memcpy(out, l, 32);
        return true;
    }

    bool ProcessPosition(uintptr_t process, Vector3& out)
    {
        float t[8];
        if (!ProcessWorld(process, t))
            return false;
        out = Vector3{ t[0], t[1], t[2] };
        return true;
    }

    bool ProcessRotation(uintptr_t process, Quaternion& out)
    {
        float t[8];
        if (!ProcessWorld(process, t))
            return false;
        out = Quaternion{ t[4], t[5], t[6], t[7] };
        return true;
    }

}
