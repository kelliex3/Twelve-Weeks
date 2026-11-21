// ported from tools/ext/t2r-ext.c (g_stun_until / g_stunEndedT60 / the shiv victim check)

#include "stun-lock.hpp"

#include <cstdio>

namespace gameplay::stun_lock {

    namespace {
        Sink*    g_sink     = nullptr;
        uint64_t g_until    = 0;
        uint64_t g_endedAt  = 0;
        uint64_t g_total    = 0;
        uint64_t g_blendOut = 0;
        uint64_t g_cough    = 0;
        uint64_t g_nextCough = 0;
        int      g_from     = -1;
        bool     g_fresh    = true;

        template<typename... A>
        void Log(const char* fmt, A... args)
        {
            if (!g_sink)
                return;
            char buf[192];
            std::snprintf(buf, sizeof buf, fmt, args...);
            g_sink->Log(buf);
        }
    }

    void SetSink(Sink* sink) noexcept { g_sink = sink; }
    Sink* SinkInstance() noexcept { return g_sink; }

    void Stun(uint64_t nowMs, int fromSlot)
    {
        const bool fresh = !g_until || nowMs >= g_until;
        g_fresh    = fresh;
        g_total    = kTotalMs;
        g_blendOut = kBlendOutMs;
        g_cough    = kCoughMs ? kCoughMs : 1;
        // ext: g_stunCnext = (total - 1) / cough * cough -- first cough multiple below the start.
        g_nextCough = (g_total - 1) / g_cough * g_cough;
        g_until = nowMs + g_total;
        g_from  = fromSlot;
        Log("[STUN] by slot %d for %.1f s (%s)", fromSlot, kStunSeconds,
             fresh ? "fresh" : "re-stun: timer restarted, clip kept");
        (void)g_nextCough;   // the host's cough-tone producer reads CoughMs() itself
    }

    void Tick(uint64_t nowMs, bool inPlayState)
    {
        if (!g_until)
            return;
        if (inPlayState && nowMs < g_until)
            return;
        g_until = 0;
        g_endedAt = nowMs ? nowMs : 1;   // ext: g_t60 ? g_t60 : 1
        Log("[STUN] phase end, %.2f s; grace %llu ms", g_total / 1000.0,
             static_cast<unsigned long long>(kGraceMs));
    }

    void Cut(uint64_t nowMs)
    {
        if (!g_until)
            return;
        g_until = 0;
        // The shiv arrives right at the cut, so the victim check's grace covers it too.
        g_endedAt = nowMs ? nowMs : 1;
        Log("[STUN] cut by shiv at %llu", static_cast<unsigned long long>(nowMs));
    }

    bool Active(uint64_t nowMs) noexcept
    {
        return g_until != 0 && nowMs < g_until;
    }

    bool RecentlyEnded(uint64_t nowMs) noexcept
    {
        if (Active(nowMs))
            return true;
        // An expired lock that has not been ticked yet still owes the grace (the ext stamps
        // g_stunEndedT60 in stun_local's phase end; its tick runs every frame).
        if (g_until != 0 && nowMs >= g_until && nowMs - g_until < kGraceMs)
            return true;
        return g_endedAt != 0 && nowMs >= g_endedAt && nowMs - g_endedAt < kGraceMs;
    }

    bool VictimOk(bool front, float angleDeg, float distM, uint64_t nowMs) noexcept
    {
        const bool stunned = RecentlyEnded(nowMs);
        if (front)
            return stunned;
        return (distM <= kBackRangeM
                && (angleDeg >= kBackAngleHiDeg || angleDeg <= kBackAngleLoDeg)) || stunned;
    }

    uint64_t Until() noexcept    { return g_until; }
    uint64_t EndedAt() noexcept  { return g_endedAt; }
    uint32_t TotalMs() noexcept  { return static_cast<uint32_t>(g_total ? g_total : kTotalMs); }
    uint32_t BlendOutMs() noexcept { return static_cast<uint32_t>(g_blendOut ? g_blendOut : kBlendOutMs); }
    uint32_t CoughMs() noexcept  { return static_cast<uint32_t>(g_cough ? g_cough : kCoughMs); }
    int      FromSlot() noexcept { return g_from; }
    bool     Fresh() noexcept    { return g_fresh; }

    void Reset()
    {
        g_until = g_endedAt = g_total = g_blendOut = g_cough = g_nextCough = 0;
        g_from = -1;
        g_fresh = true;
    }

} // namespace gameplay::stun_lock
