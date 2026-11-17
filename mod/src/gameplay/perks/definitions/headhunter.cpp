#include "headhunter.hpp"

#include "engine/script.hpp"
#include "net-debug.hpp"

#include <algorithm>
#include <match.hpp>

#undef min

namespace gameplay {

    static constexpr int kHealPerHeadshot = 15;

    void HeadhunterOnDealHit(const HitContext& ctx)
    {
        TO1_LOG("[perk] entered HeadhunterOnDealHit\n");
        if (!ctx.headshot) return;
        if (net::match::Get().slotState[ctx.victimSlot].dead) return;

        const int before = PLAYER::GET_HEALTH();
        const int after = std::min(before + kHealPerHeadshot, PLAYER::GET_MAX_HEALTH());
        if (after <= before) return;

        PLAYER::SET_HEALTH(after);
        TO1_LOG("[perk] headhunter heal %d -> %d (headshot on slot%d)\n",
            before, after, ctx.victimSlot);
    }
}
