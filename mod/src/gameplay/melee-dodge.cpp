// ported from tools/ext/melee_dodge.c (the timing / state core)

#include "melee-dodge.hpp"

#include "melee-tell.hpp"

#include <algorithm>
#include <cmath>

namespace gameplay::melee_dodge {

    namespace {

        Sink* g_sink = nullptr;
        WindowOverrides g_overrides;

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
        bool     g_windowOpen = false;  // ext g_mdg.window_open (the loose geometry flag)
        bool     g_held = false;        // ext g_mdh.on
        bool     g_replay = false;      // ext g_mdhReplay (a replayed blow is never held again)
        HeldBlow g_heldBlow;
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

    } // namespace

    void SetSink(Sink* sink) { g_sink = sink; }
    void SetWindowOverrides(const WindowOverrides& overrides) { g_overrides = overrides; }

    bool GeometryOk(const Pose& self, const Pose& peer, uint64_t swingMs, uint64_t nowMs,
                    bool loose, float* dist)
    {
        for (float v : self.pos)
            if (!std::isfinite(v))
                return false;
        for (float v : peer.pos)
            if (!std::isfinite(v))
                return false;
        for (float v : peer.rot)
            if (!std::isfinite(v))
                return false;
        const float dx = self.pos[0] - peer.pos[0];
        const float dz = self.pos[2] - peer.pos[2];
        const float d = std::sqrt(dx * dx + dz * dz);
        if (d > (loose ? kLooseReachM : kReachM)
            || std::fabs(self.pos[1] - peer.pos[1]) > kHeightTolM)
            return false;
        if (dist)
            *dist = d;
        if (d < kNearDistM)
            return true;
        const float* q = peer.rot;    // ext q = locator + 4, quaternion x,y,z,w
        const float fx = 2.0f * (q[0] * q[2] + q[3] * q[1]);
        const float fz = 1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]);
        const float fl = std::sqrt(fx * fx + fz * fz);
        if (!(fl > 1e-3f))
            return false;
        const float cosv = (fx * dx + fz * dz) / (fl * d);
        // The puppet body is still turning towards us for MD_TURN_MS after the swing note.
        if (swingMs && nowMs >= swingMs && nowMs - swingMs < kTurnMs)
            return cosv >= 0.0f;
        return cosv >= (loose ? kLooseConeCos : kConeCos);
    }

    bool HoldBlow(const HeldBlow& blow, uint64_t nowMs, float onewayMs, float playoutMs)
    {
        if (g_replay || g_held || !(blow.damage > 0.0f) || blow.damage >= 200.0f
            || blow.slot < 0 || blow.slot >= kMaxPeers || Iframes(nowMs))
            return false;
        if (Window(nowMs, nullptr) != blow.slot)
            return false;          // only a blow of the swing whose window is open right now
        float ow = onewayMs;
        if (!(ow > 0.0f) || ow > 250.0f)
            ow = 40.0f;            // ext PsyncOneway fallback
        float po = playoutMs > 0.0f ? playoutMs : 40.0f;
        if (po > 150.0f)
            po = 150.0f;           // ext g_ns[slot].dcur clamp
        int grace = static_cast<int>(std::lround(ow + po));
        grace = std::clamp(grace, 40, 90);
        g_heldBlow = blow;
        g_heldBlow.graceMs = grace;
        g_heldBlow.t0 = nowMs;
        g_heldBlow.until = nowMs + static_cast<uint64_t>(grace);
        g_held = true;
        g_swings[blow.slot].holdUntil = g_heldBlow.until;
        return true;
    }

    void HeldTick(uint64_t nowMs, bool matchLive, bool busy)
    {
        if (!g_held)
            return;
        const bool pressed = g_pressMs >= g_heldBlow.t0 && Iframes(nowMs);
        const bool gone = !matchLive;
        if (!pressed && !gone && nowMs < g_heldBlow.until && !busy)
            return;
        const HeldBlow h = g_heldBlow;
        g_held = false;
        if (h.slot >= 0 && h.slot < kMaxPeers
            && g_swings[h.slot].holdUntil == h.until)
            g_swings[h.slot].holdUntil = 0;   // no longer keeps the dodge window open
        if (gone)
            return;
        g_replay = true;                       // a replayed blow must never be held again
        if (g_sink)
            g_sink->ReplayBlow(h);
        g_replay = false;
    }

    bool HeldActive() noexcept { return g_held; }

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
        uint16_t win = streakWin;
        uint16_t react = win < kWindowMs ? win : kReactMs;
        {
            const uint64_t tw = melee_tell::WinMs(), tr = melee_tell::ReactMs();
            if (tw && win > tw)
                win = static_cast<uint16_t>(tw);
            if (tr && react > tr)
                react = static_cast<uint16_t>(tr);
        }
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
            if (g_overrides.geometry
                && !g_overrides.geometry(s, nowMs, g_windowOpen, nullptr))
                continue;                       // ext MdgGeomOk
            if (!sw.openMs || sw.openMs < sw.ms)
                sw.openMs = nowMs;
            uint64_t end = sw.ms + win;
            if (sw.openMs + react > end)
                end = sw.openMs + react;
            if (end > sw.ms + kWindowHardMs)
                end = sw.ms + kWindowHardMs;
            uint64_t xend = 0;
            int xr = 0;
            if (g_overrides.exchange) {
                xr = g_overrides.exchange(s, &xend);  // ext MexWindowEnd
                if (xr == 2)
                    continue;
                if (xr == 1)
                    end = xend;
            }
            if (!xr && g_overrides.duel) {
                uint64_t dend = 0;
                const int dr = g_overrides.duel(s, nowMs, sw.ms, &dend); // ext MduWindowEnd
                if (dr == 1)
                    end = dend;
                else if (dr == 2 && !held)
                    continue;
            }
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
        g_windowOpen = best >= 0;
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
        for (Swing& s : g_swings)
            s = Swing{};
        g_seqNext = g_seqUsed = 0;
        g_winEnd = g_lastDodgeMs = g_hitMs = g_pressMs = g_iframeUntil = 0;
        g_windowOpen = g_held = g_replay = false;
        g_heldBlow = HeldBlow{};
        g_hpLast = -1.0f;
        g_charges = kCharges;
        g_streakN = 0;
        for (auto& e : g_imm)
            e = { 0, 0xff };
    }

} // namespace gameplay::melee_dodge
