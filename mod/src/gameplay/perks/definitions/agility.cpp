#include "agility.hpp"
#include "../perk-registry.hpp"

#include "net-common.hpp"
#include "net-gui2.hpp"
#include "net-debug.hpp"

#include <safetyhook.hpp>

namespace gameplay {

    static constexpr float kCrouchSpeedMult = 1.3f;
    static constexpr float kClimbMult = 1.4f;
    static constexpr long long kClimbWindowMs = 120;

    static SafetyHookInline s_hkPhaseUpdate, s_hkClamberPost, s_hkLedgePost;
    static volatile long long s_climbStampMs = 0;
    static bool s_installed = false;

    static void __fastcall hk_ClamberPost(uintptr_t self, uintptr_t player)
    {
        s_climbStampMs = static_cast<long long>(GetTickCount64());
        s_hkClamberPost.fastcall<void>(self, player);
    }

    static void __fastcall hk_LedgePost(uintptr_t self, uintptr_t player)
    {
        s_climbStampMs = static_cast<long long>(GetTickCount64());
        s_hkLedgePost.fastcall<void>(self, player);
    }

    static void __fastcall hk_PhaseUpdate(uintptr_t inst, float dt, bool isTop, uintptr_t fx)
    {
        if (perks::IsActive(PerkId::AGILITY) && dt > 0.0f && inst > 0x10000 &&
            (static_cast<long long>(GetTickCount64()) - s_climbStampMs) <= kClimbWindowMs)
        {
            static auto getBaseLayer = GameFn<uintptr_t(__fastcall*)(uintptr_t)>(0x14440D0);
            const uintptr_t pl = DcManager::LookupObject(SID("player"));
            const uintptr_t ac = pl ? *reinterpret_cast<uintptr_t*>(pl + 0x1B8) : 0;
            const uintptr_t base = ac ? getBaseLayer(ac) : 0;
            if (base && *reinterpret_cast<uintptr_t*>(inst + 0x140) == base)
                dt *= kClimbMult;
        }
        s_hkPhaseUpdate.unsafe_fastcall<void>(inst, dt, isTop, fx);
    }

    void AgilityTick(float dt)
    {
        DC::Call<"player-set-motion-match-crouch-speed-multiplier/f">(kCrouchSpeedMult);
        
        if (s_installed) return;
        s_hkPhaseUpdate = safetyhook::create_inline(reinterpret_cast<void*>(g_base + 0x15FDCE0), reinterpret_cast<void*>(hk_PhaseUpdate));
        s_hkClamberPost = safetyhook::create_inline(reinterpret_cast<void*>(g_base + 0xDFBCE0), reinterpret_cast<void*>(hk_ClamberPost));
        s_hkLedgePost = safetyhook::create_inline(reinterpret_cast<void*>(g_base + 0xBF3270), reinterpret_cast<void*>(hk_LedgePost));
        s_installed = true;
        TO1_LOG("[agility] climb hooks installed\n");
    }

    void AgilityReset()
    {
        if (!s_installed) return;
        s_hkPhaseUpdate.reset();
        s_hkClamberPost.reset();
        s_hkLedgePost.reset();
        s_installed = false;
        s_climbStampMs = 0;
        TO1_LOG("[agility] climb hooks removed\n");
    }
}