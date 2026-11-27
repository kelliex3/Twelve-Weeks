// Native read-only slowdown owner. The pure factor lives in slowdown-core.hpp;
// the sidecar, its wall-clock cache and the file seam stay here. No new wire
// message and no engine movement hook: peers are positioned by the replicated
// poses, so the only native consumer is the footsteps speed threshold.

#include "slowdown.hpp"

#include "engine/exe_side.hpp"

#include <windows.h>

#include <cstdint>
#include <cstdio>

namespace gameplay::slowdown {

    namespace {

        constexpr char kOff[] = "t2r-no-slowdown.txt";

        bool     g_off = false;
        uint64_t g_at = 0;

        void Poll() noexcept
        {
            const uint64_t now = GetTickCount64();
            if (!core::CacheDue(now, g_at))
                return;
            g_at = now ? now : 1;
            FILE* f = engine::exe_side::Open(kOff, "r");
            g_off = f != nullptr;
            if (f)
                std::fclose(f);
        }

    } // namespace

    bool Off() noexcept
    {
        Poll();
        return g_off;
    }

    float Mul() noexcept
    {
        return core::Mul(Off());
    }

    void Reset() noexcept
    {
        g_at = 0;
        g_off = false;
    }

} // namespace gameplay::slowdown
