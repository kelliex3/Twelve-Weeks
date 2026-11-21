#include "net-smooth-bridge.hpp"

#include "engine/frame.hpp"
#include "log.hpp"

namespace net::netsmooth::bridge {

    namespace {

        double NowMs()
        {
            const double ms = engine::frame::NowMs();
            return ms > 0.0 ? ms : 0.0;
        }

        // ---- the ext's 10 s "net stats:" line (netsmooth.c:2104) ---------------------------
        double g_lastLogMs = 0.0;

        void LogStats(double now)
        {
            for (uint8_t slot = 0; slot < kSlots; ++slot) {
                const Stats& s = StatsFor(slot);
                if (!s.states && !s.anims && !s.frames) continue;

                const double window = (now - g_lastLogMs) / 1000.0;
                const double sec = window > 0.1 ? window : 1.0;
                const double frames = s.frames > 0 ? static_cast<double>(s.frames) : 1.0;
                const double loss5 = static_cast<double>(s.states + s.lost5);
                const double loss7 = static_cast<double>(s.anims + s.lost7);

                TO1_LOG("[NETSMOOTH] slot %u: rx state %.0f/s anim %.0f/s, loss5 %u (%.1f%%) "
                    "loss7 %u (%.1f%%); sender frame %.1f ms spacing %.1f ms; jitter mean %.0f "
                    "peak %.0f ms; delay %.0f ms loss-ewma %.1f%%; extrap %.0f%% (held %u) "
                    "corr %u (max %.2f m) snaps %u tele %u resync %u; stutter native %u / "
                    "smooth %u of %u moving; lag %.2f m; pred (n %u) cv %.0f cm hold %.0f cm",
                    static_cast<unsigned>(slot),
                    s.states / sec, s.anims / sec,
                    s.lost5, loss5 > 0.0 ? 100.0 * s.lost5 / loss5 : 0.0,
                    s.lost7, loss7 > 0.0 ? 100.0 * s.lost7 / loss7 : 0.0,
                    s.frameMs, s.sampleMs,
                    s.jitterMean, s.jitterMax,
                    s.delaySum / frames, s.lossEwma * 100.0,
                    s.frames > 0 ? 100.0 * s.extrap / frames : 0.0, s.hold,
                    s.corr, s.corrMax, s.snap, s.tele, s.resync,
                    s.stutterNative, s.stutterSmooth, s.moving,
                    s.lagN ? s.lagSum / s.lagN : 0.0,
                    s.predN,
                    s.predN ? 100.0 * s.predCvSum / s.predN : 0.0,
                    s.predN ? 100.0 * s.predHoldSum / s.predN : 0.0);
                ResetStats(slot);
            }
        }

    } // namespace

    void OnState(uint8_t slot, uint32_t seq, const float pos[3], const float quat[4])
    {
        netsmooth::OnState(slot, seq, pos, quat, NowMs());
    }

    void OnAnim(uint8_t slot, uint32_t seq, uint32_t sendMs)
    {
        netsmooth::OnAnim(slot, seq, sendMs, NowMs());
    }

    bool Apply(uint8_t slot, const Transform& native, Transform& out, double onewayMs)
    {
        return netsmooth::Sample(slot, NowMs(), onewayMs, out, &native);
    }

    void Tick()
    {
        const double now = NowMs();
        if (g_lastLogMs <= 0.0) {
            g_lastLogMs = now;
            return;
        }
        if (now - g_lastLogMs >= kLogEveryMs) {
            LogStats(now);
            g_lastLogMs = now;
        }
    }

    void Reset(uint8_t slot)
    {
        netsmooth::Reset(slot);
    }

    void ResetAll()
    {
        netsmooth::ResetAll();
        g_lastLogMs = 0.0;
    }

} // namespace net::netsmooth::bridge
