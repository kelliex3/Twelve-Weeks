// ported from tools/ext/alert_sting.c (the alert sting; the stun tone / crossbow HUD / stun
// cough sub-features are separate and not ported)

#include "alertsting.hpp"

#include "engine/flag.hpp"
#include "engine/pcm.hpp"

#include "net/game/player-status.hpp"

#include "log.hpp"

#include <windows.h>

#include <cstdint>

#ifdef T2R_DATA_ALERT_STING
#include "alert_sting.h"    // generated (private): als_blob, AL_N, AL_RATE
#endif

namespace net::audio::alertsting {

    namespace {

        constexpr uint64_t kQuietMs = 15000;   // ext ALS_QUIET_MS
        constexpr uint64_t kGapMs   = 6000;    // ext ALS_GAP_MS
        constexpr uint64_t kDelayMs = 100;     // ext ALS_DELAY_MS

        uint64_t    g_alsLast      = 0;
        uint64_t    g_alsCalmMs    = 0;        // last tick the combat music was calm (tier 0)
        uint64_t    g_alsThreatMs  = 0;        // last enemy-caused music input (hit / near miss)
        uint64_t    g_alsDue       = 0;        // when the delayed sting is due
        const char* g_alsDueWhy    = nullptr;

    }

    void Play(float level, const char* why)
    {
        (void)why;      // only the (debug-only) logs use it
        if (engine::flag::Present("t2r-no-alert-sting.txt"))
            return;
        if (!net::player_status::MatchLive())
            return;
        const uint64_t now = GetTickCount64();
        if (g_alsLast && now - g_alsLast < kGapMs) {
            TO1_LOG("[ALERTSTING] %s ignored - played %llu ms ago\n", why ? why : "?",
                static_cast<unsigned long long>(now - g_alsLast));
            return;
        }
        const float g = level * engine::pcm::MusicUserVolume();
        if (g <= 0.0f)
            return;
#ifdef T2R_DATA_ALERT_STING
        engine::pcm::Voice l, r;
        if (!engine::pcm::PlayStereoRaw(als_blob, als_blob + AL_N, AL_N, g, AL_RATE, l, r)) {
            TO1_LOG("[ALERTSTING] %s - mixer off / voices busy\n", why ? why : "?");
            return;
        }
        g_alsLast = now;
        TO1_LOG("[ALERTSTING] %s - playing (%.2f s, gain %.2f)\n", why ? why : "?",
            AL_N / static_cast<double>(AL_RATE), static_cast<double>(g));
#else
        (void)g;
        TO1_LOG("[ALERTSTING] %s - data not built (T2R_DATA_ALERT_STING off)\n", why ? why : "?");
#endif
    }

    void Combat(const char* why)
    {
        const uint64_t now = GetTickCount64(), prev = g_alsThreatMs;
        g_alsThreatMs = now;
        // TODO(music): the ext also requires g_msTier == 0 (combat music calm) and
        // !g_msDead / !g_downed. The music layer is not ported, so g_alsCalmMs is never
        // refreshed and this never arms. The bookkeeping is kept for when the tier lands.
        if (!net::player_status::MatchLive())
            return;
        if (!g_alsCalmMs || now - g_alsCalmMs > 1500)
            return;
        if (prev && now - prev < kQuietMs)
            return;
        g_alsDue = now + kDelayMs;
        g_alsDueWhy = why;      // delayed ~0.1 s: it played too soon, right after getting hit
    }

    void Tick()
    {
        // TODO(music): g_alsCalmMs would be refreshed here on every frame the combat tier
        // is 0 (the ext's AlertstingTick).
        if (g_alsDue && GetTickCount64() >= g_alsDue) {
            g_alsDue = 0;
            if (!net::player_status::LocalDowned())
                Play(0.8f, g_alsDueWhy ? g_alsDueWhy : "shot at");
        }
        // (the ext also ticks the stun tone, the crossbow HUD and the stun cough here; those
        // are separate features and are not ported)
    }

}
