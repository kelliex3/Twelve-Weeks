// ported from tools/ext/melee_tell.c (the timing core)

#include "melee-tell.hpp"

#include <algorithm>
#include <cmath>

namespace gameplay::melee_tell {

    namespace {

        bool     g_enabled = true;  // ext: !g_mtl.off (t2r-no-melee-tell.txt absent)
        bool     g_mv2 = false;     // ext Mv2On()
        int      g_tellMs = kTellMs;
        float    g_auth[4] = { -1.0f, -1.0f, -1.0f, -1.0f }; // ext g_mtl.auth
        float    g_lastSpeed = 1.0f; // ext g_mtl.last_speed
        int      g_lastClass = -1;   // ext g_mtl.last_w

        float Auth(int wcls, uint16_t raw)
        {
            if (wcls < 0 || wcls > 3)
                return static_cast<float>(raw);
            if (g_auth[wcls] < 0.0f)
                g_auth[wcls] = static_cast<float>(raw);
            return g_auth[wcls];
        }

    } // namespace

    void SetEnabled(bool enabled) noexcept { g_enabled = enabled; }
    void SetMeleeV2(bool enabled) noexcept { g_mv2 = enabled; }
    bool Enabled() noexcept { return g_enabled && !g_mv2; }

    void SetTellMs(int ms) noexcept
    {
        g_tellMs = std::clamp(ms, kMinTellMs, kMaxTellMs);
    }

    int TellMs() noexcept { return g_tellMs; }

    uint64_t WinMs() noexcept
    {
        return Enabled() ? static_cast<uint64_t>(g_tellMs + kWinExtraMs) : 0ull;
    }

    uint64_t ReactMs() noexcept
    {
        return Enabled() ? static_cast<uint64_t>(g_tellMs) : 0ull;
    }

    uint16_t Tc(int wcls, uint16_t raw)
    {
        if (!Enabled())
            return raw;
        const float a = Auth(wcls, raw);
        if (a > static_cast<float>(g_tellMs))
            return static_cast<uint16_t>(g_tellMs);
        return static_cast<uint16_t>(a + 0.5f);
    }

    float Speed(int wcls, uint16_t raw)
    {
        if (!Enabled())
            return 0.0f;
        if (g_mv2)
            return 1.0f;  // never reached while Enabled() is false; kept for parity with the ext
        float s = Auth(wcls, raw) / static_cast<float>(g_tellMs);
        s = std::clamp(s, 1.0f, static_cast<float>(kMaxSpeedX100) / 100.0f);
        g_lastSpeed = s;
        g_lastClass = wcls;
        return s;
    }

    void NoteContact(int wcls, int observedMs)
    {
        if (!Enabled() || wcls < 0 || wcls > 3
            || observedMs < kMinObserved || observedMs > kMaxObserved)
            return;
        float est = static_cast<float>(observedMs)
            * (g_lastClass == wcls ? g_lastSpeed : 1.0f);
        est = std::clamp(est, 150.0f, 800.0f);
        const float old = g_auth[wcls] < 0.0f ? est : g_auth[wcls];
        g_auth[wcls] = 0.5f * old + 0.5f * est;
    }

    void Reset()
    {
        g_enabled = true;
        g_mv2 = false;
        g_tellMs = kTellMs;
        for (float& a : g_auth)
            a = -1.0f;
        g_lastSpeed = 1.0f;
        g_lastClass = -1;
    }

} // namespace gameplay::melee_tell
