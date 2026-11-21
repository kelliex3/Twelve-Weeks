// ported from tools/ext/melee_duel.c (the fair-window / dodge-window core)

#include "melee-duel.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gameplay::melee_duel {

    namespace {

        bool     g_on = false;
        int      g_slot = -1;
        uint64_t g_at[kMaxPeers] = {};   // ext g_mdu.vsw_at[s]
        uint64_t g_end[kMaxPeers] = {};  // ext g_mdu.vsw_end[s]

        int Round(float v)
        {
            return static_cast<int>(std::lround(v));
        }

    } // namespace

    int VicExitMs(float endSeconds, float reactRate)
    {
        if (!(reactRate > 0.0f))
            reactRate = kReactRate;
        const int exitMs = Round(static_cast<float>(kVicDodgeExitMs) / reactRate);
        const int handoffMs = endSeconds > 0.0f ? Round(endSeconds * 1000.0f) : 0;
        return handoffMs > 0 && handoffMs < exitMs ? handoffMs : exitMs;
    }

    int AttLead(float onewayMs, bool oldLead)
    {
        if (!(onewayMs >= 0.0f))
            onewayMs = 0.0f;
        if (onewayMs > kOwlCapMs)
            onewayMs = kOwlCapMs;
        const int r = Round((oldLead ? kAttLeadOldMs : kAttLeadMs) - 2.0f * onewayMs);
        return r < kAttFloorMs ? kAttFloorMs : r;
    }

    int VicLead(float onewayMs, bool oldLead)
    {
        if (!(onewayMs >= 0.0f))
            onewayMs = 0.0f;
        return AttLead(onewayMs, oldLead) + Round(2.0f * onewayMs) + kJitterMs;
    }

    int FairGateMs(float onewayMs, int exitMs, int nextContactMs, int baseMs, int recoveryMs,
                   bool fair45Old, bool uncapped)
    {
        int effective = std::max(baseMs, recoveryMs);
        if (nextContactMs <= 0)
            return effective;
        const int lead = fair45Old
            ? Round(2.0f * onewayMs) + kVicDodgeLeadMs
            : VicLead(onewayMs) + kFairSlackMs;
        int fair = exitMs + lead - nextContactMs;
        if (fair <= baseMs || fair <= recoveryMs)
            return effective;                 // the floor never shortens an earned gate
        if (fair > kFairCapMs)
            fair = kFairCapMs;
        if (!uncapped && fair > baseMs + kFairMaxAddMs)
            fair = baseMs + kFairMaxAddMs;
        return std::max(effective, fair);
    }

    int NextBaseMs(int chainStep, bool breath, bool next2Old)
    {
        if (chainStep < kVicFreeStep)
            return kNextMs;
        return (!breath || next2Old) ? kNext2Ms : kNext2BreathMs;
    }

    void SetEnabled(bool enabled) noexcept { g_on = enabled; }
    void SetSlot(int slot) noexcept { g_slot = slot; }

    void NoteSwing(int slot, uint64_t atMs, uint64_t endMs)
    {
        if (slot < 0 || slot >= kMaxPeers)
            return;
        g_at[slot] = atMs;
        g_end[slot] = endMs;
    }

    void EndSwing(int slot)
    {
        if (slot < 0 || slot >= kMaxPeers)
            return;
        g_at[slot] = g_end[slot] = 0;
    }

    void EndDuel()
    {
        g_on = false;
        g_slot = -1;
        std::memset(g_at, 0, sizeof g_at);
        std::memset(g_end, 0, sizeof g_end);
    }

    int WindowEnd(int slot, uint64_t nowMs, uint64_t dodgeSwingMs, uint64_t* end)
    {
        if (!g_on || slot < 0 || slot >= kMaxPeers || slot != g_slot || !g_end[slot])
            return 0;
        if (!dodgeSwingMs)
            return 0;
        const uint64_t delta = g_at[slot] > dodgeSwingMs
            ? g_at[slot] - dodgeSwingMs : dodgeSwingMs - g_at[slot];
        if (delta > kSwingMatchMs)
            return 0;                         // a different swing (or a stale dodge announce)
        if (nowMs > g_end[slot])
            return 2;                         // the duel window closed at its contact
        if (end)
            *end = g_end[slot];
        return 1;
    }

    bool Active() noexcept { return g_on; }

    void Reset()
    {
        g_on = false;
        g_slot = -1;
        std::memset(g_at, 0, sizeof g_at);
        std::memset(g_end, 0, sizeof g_end);
    }

} // namespace gameplay::melee_duel
