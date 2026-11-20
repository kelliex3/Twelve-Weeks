// ported from tools/ext/melee_dodge.c (the timing / state core)

#include "melee-dodge.hpp"

#include <algorithm>
#include <cstring>

namespace gameplay::melee_dodge {

    namespace {

        Sink* g_sink = nullptr;

        struct Swing {
            uint64_t ms = 0;       // ext g_mdg.sw[s].ms (arrival of the swing announce)
            uint64_t landedMs = 0; // ext landed_ms
            uint64_t openMs = 0;   // ext open_ms (first moment it could be dodged)
            uint64_t holdUntil = 0;// ext g_mdh.until (late-blow grace)
            uint16_t seq = 0;
            uint8_t  kind = 0;
        };

        Swing    g_swings[kMaxPeers];
        uint16_t g_seqNext = 0;
        uint16_t g_seqUsed = 0;
        uint64_t g_winEnd = 0;
        uint64_t g_lastDodgeMs = 0;
        uint64_t g_hitMs = 0;
        uint64_t g_pressMs = 0;
        uint64_t g_iframeUntil = 0;
        float    g_hpLast = -1.0f;
        int      g_charges = kCharges;
        int      g_streakN = 0;
        bool     g_strict = true;
        struct { uint64_t until = 0; uint8_t dir = 0xff; } g_imm[kMaxPeers];

        uint64_t CooldownMs() { return g_strict ? 2500 : 1500; }

        uint16_t StreakWindow(uint64_t nowMs)
        {
            if (!g_lastDodgeMs || nowMs - g_lastDodgeMs >= kStreakMs)
                return kWindowMs;
            static constexpr uint16_t kStreak[] = { kWindowMs, kWindow2Ms, 180, 130 };
            int n = std::clamp(g_streakN, 1, 3);
            return kStreak[n];
        }

        void Log(const char* text)
        {
            if (g_sink)
                g_sink->Log(text);
        }

    } // namespace

    void SetSink(Sink* sink) { g_sink = sink; }

    void NoteSwing(int slot, uint64_t nowMs, uint16_t seq)
    {
        if (slot < 0 || slot >= kMaxPeers)
            return;
        Swing& s = g_swings[slot];
        s.ms = nowMs;
        s.openMs = 0;
        s.landedMs = 0;
        s.holdUntil = 0;
        s.seq = seq ? seq : ++g_seqNext;
    }

    void NoteSwingLanded(int slot, uint64_t nowMs)
    {
        if (slot < 0 || slot >= kMaxPeers)
            return;
        g_swings[slot].landedMs = nowMs;
    }

    void HoldSwing(int slot, uint64_t untilMs)
    {
        if (slot < 0 || slot >= kMaxPeers)
            return;
        g_swings[slot].holdUntil = untilMs;
    }

    int Window(uint64_t nowMs, uint16_t* winMs)
    {
        const uint16_t streakWin = StreakWindow(nowMs);
        const uint16_t win = streakWin;
        const uint16_t react = win < kWindowMs ? win : kReactMs;
        if (winMs)
            *winMs = win;
        int best = -1;
        uint64_t bestMs = 0, bestEnd = 0;
        for (int s = 0; s < kMaxPeers; ++s) {
            Swing& sw = g_swings[s];
            const bool held = sw.holdUntil && nowMs < sw.holdUntil;
            if (!sw.ms || (nowMs - sw.ms > kWindowHardMs && !held)
                || sw.seq == g_seqUsed)
                continue;
            if (sw.landedMs >= sw.ms)
                continue;                       // this blow already resolved
            if (!sw.openMs || sw.openMs < sw.ms)
                sw.openMs = nowMs;
            uint64_t end = sw.ms + win;
            if (sw.openMs + react > end)
                end = sw.openMs + react;
            if (end > sw.ms + kWindowHardMs)
                end = sw.ms + kWindowHardMs;
            if (nowMs > end && !held)
                continue;
            if (!held && streakWin < kWindowMs && nowMs + streakWin < end)
                continue;                       // a streak dodge opens only in its last `win` ms
            if (sw.ms > bestMs) {
                bestMs = sw.ms;
                best = s;
                bestEnd = end;
            }
        }
        g_winEnd = best >= 0 ? bestEnd : 0;
        return best;
    }

    uint64_t WindowEnd() { return g_winEnd; }

    bool Press(uint64_t nowMs, Direction dir, uint64_t clipSid)
    {
        if (g_charges <= 0 || (g_pressMs && nowMs - g_pressMs < 120))
            return false;
        --g_charges;
        if (g_lastDodgeMs && nowMs - g_lastDodgeMs < kStreakMs)
            ++g_streakN;
        else
            g_streakN = 1;
        g_lastDodgeMs = nowMs;
        g_pressMs = nowMs;
        g_iframeUntil = nowMs + kIframeMs;      // + MD_PRESS_MARGIN_MS on the attacker's copy
        if (g_sink)
            g_sink->SendPress(0xFF, 1, static_cast<uint8_t>(dir), clipSid);
        return true;
    }

    bool Iframes(uint64_t nowMs)
    {
        return g_iframeUntil && nowMs >= g_pressMs && nowMs <= g_iframeUntil;
    }

    int Charges() noexcept { return g_charges; }

    void NoteHit(uint64_t nowMs, float hp)
    {
        if (g_hpLast > 0.0f && hp > 0.0f && hp < g_hpLast - 0.5f) {
            g_hitMs = nowMs;
            g_charges = std::min(g_charges, 1);  // a hit taken cuts the charges to one
        }
        if (hp > 0.0f)
            g_hpLast = hp;
    }

    void SetStrictCooldown(bool strict) { g_strict = strict; }

    void NoteDodged(uint64_t nowMs, int attackerSlot)
    {
        (void)nowMs;
        if (g_sink && attackerSlot >= 0 && attackerSlot < kMaxPeers)
            g_sink->SendResult(static_cast<uint8_t>(attackerSlot));
    }

    bool RecvSwing(int slot, uint64_t nowMs, const uint8_t* buf, size_t len)
    {
        if (slot < 0 || slot >= kMaxPeers)
            return false;
        NoteSwing(slot, nowMs, 0);
        Swing& s = g_swings[slot];
        s.kind = len >= 1 && buf ? (buf[0] & 1u) : 0;
        return true;
    }

    bool RecvPress(int slot, uint64_t nowMs, const uint8_t* buf, size_t len)
    {
        if (slot < 0 || slot >= kMaxPeers || !buf || len < 2)
            return false;
        const bool immune = (buf[0] & 1u) != 0;
        g_imm[slot].dir = buf[1] < 4 ? buf[1] : 0xff;
        g_imm[slot].until = immune ? nowMs + kIframeMs + 120 : 0;
        return true;
    }

    bool RecvResult(int slot, uint64_t nowMs, const uint8_t* buf, size_t len)
    {
        (void)slot; (void)nowMs; (void)buf; (void)len;
        // ext MSG_DODGE_RES is empty: the attacker plays its miss. The animation / FX half is
        // gated; the pure core has no state to update here.
        return true;
    }

    bool SlotImmune(int slot, uint64_t nowMs)
    {
        if (slot < 0 || slot >= kMaxPeers)
            return false;
        return g_imm[slot].until && nowMs <= g_imm[slot].until;
    }

    void Tick(uint64_t nowMs)
    {
        if (g_charges < kCharges
            && g_lastDodgeMs && nowMs >= g_lastDodgeMs + CooldownMs()
            && (!g_hitMs || nowMs >= g_hitMs + CooldownMs()))
            g_charges = kCharges;
    }

    void Reset()
    {
        std::memset(g_swings, 0, sizeof g_swings);
        g_seqNext = g_seqUsed = 0;
        g_winEnd = g_lastDodgeMs = g_hitMs = g_pressMs = g_iframeUntil = 0;
        g_hpLast = -1.0f;
        g_charges = kCharges;
        g_streakN = 0;
        std::memset(g_imm, 0, sizeof g_imm);
        for (auto& e : g_imm)
            e.dir = 0xff;
    }

} // namespace gameplay::melee_dodge
