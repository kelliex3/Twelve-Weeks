#include "first-aid-training.hpp"
#include "../perk-registry.hpp"

#include "net-common.hpp"
#include "net-gui2.hpp"
#include "net-debug.hpp"

#include <safetyhook.hpp>

namespace gameplay {

    static constexpr uint64_t kHealSpeedUpgrade = SID("health-kit-speed-increase");
    static constexpr float kHealSpeed = 2.5f; // 1.0 = stock

    static SafetyHookInline s_hkHasUpgrade{};
    static bool s_installed = false;

    static char __fastcall hk_HasUpgrade(uint64_t id, char noSuppressMask)
    {
        if (id == kHealSpeedUpgrade && perks::IsActive(PerkId::FIRSTAIDTRAINING)) {
            *reinterpret_cast<float*>(g_base + 0x4104340) = kHealSpeed;
            return 1;
        }
        return s_hkHasUpgrade.unsafe_fastcall<char>(id, noSuppressMask);
    }

    void FirstAidTrainingTick(float dt)
    {
        if (s_installed) return;
        s_hkHasUpgrade = safetyhook::create_inline(
            reinterpret_cast<void*>(g_base + 0xD2D2B0), hk_HasUpgrade);
        s_installed = true;
        TO1_LOG("[firstaidtraining] heal speed hook installed\n");
    }

    void FirstAidTrainingReset()
    {
        if (!s_installed) return;
        s_hkHasUpgrade = {};
        s_installed = false;
        TO1_LOG("[firstaidtraining] heal speed hook removed\n");
    }
}