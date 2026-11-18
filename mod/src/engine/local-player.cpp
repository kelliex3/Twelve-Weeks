// ported from tools/ext/t2r-ext.c (kFnLocalPlayer, SglScoped, LocalHp)

#include "local-player.hpp"

#include "log.hpp"          // TO1_LOG must precede engine/script.hpp (it uses it in a template)
#include "engine/ptr.hpp"
#include "engine/script.hpp"

#include <bit>
#include <cmath>

namespace engine::local_player {

    namespace {

        // Player -> PlayerAimController. The same offset net-hooks.cpp's shoulder namespace
        // uses (OFF_AIM_CTRL), duplicated here so this engine module does not depend on net code.
        constexpr uintptr_t kOffAimCtrl    = 0x2F68;
        constexpr uintptr_t kOffScopeZoom  = 0x0698;   // PlayerAimController::m_scopeZoomedIn

    }

    uintptr_t Ptr()
    {
        return static_cast<uintptr_t>(DcManager::LookupObject(SID("player")));
    }

    bool ScopedIn()
    {
        const uintptr_t me = Ptr();
        if (!me || !engine::ptr::Readable(me + kOffAimCtrl, 8))
            return false;

        const uintptr_t fa = *reinterpret_cast<const uintptr_t*>(me + kOffAimCtrl);
        if (!fa || !engine::ptr::Readable(fa, kOffScopeZoom + 1))
            return false;

        const uint8_t zoom = *reinterpret_cast<const uint8_t*>(fa + kOffScopeZoom);
        return zoom <= 1 && zoom != 0;
    }

    bool IsAiming()
    {
        const uint64_t a = DC::Call<"player-is-aiming?">();
        return a != static_cast<uint64_t>(-1) && (a & 0xff);
    }

    float Health()
    {
        const uint64_t r = DC::Call<"get-player-health">();
        if (r == static_cast<uint64_t>(-1))
            return -1.0f;

        const uint32_t lo = static_cast<uint32_t>(r);
        const float f = std::bit_cast<float>(lo);
        if (lo > 0x10000 && f > 0.0001f && f < 100000.0f)
            return f;                       // a float
        return static_cast<float>(static_cast<int32_t>(lo));   // an int
    }

}
