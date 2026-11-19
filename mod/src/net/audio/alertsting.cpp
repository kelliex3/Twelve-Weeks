// ported from tools/ext/alert_sting.c (the alert sting; the stun tone / crossbow HUD / stun
// cough sub-features are separate and not ported)

#include "alertsting.hpp"

#include "engine/flag.hpp"
#include "engine/pcm.hpp"

#include "music-tier.hpp"
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
        // ext AlertstingCombat: the tier-0 requirement is the calm clock below -- music_tier::
        // Tick refreshes it on every frame the tier is calm, so a fresh g_alsCalmMs means the
        // combat music was tier 0 when the shot landed. The two explicit gates are the ext's
        // own g_msDead / g_downed checks.
        if (!net::player_status::MatchLive())
            return;
        if (music_tier::Dead() || net::player_status::LocalDowned())
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
        // ext AlertstingTick: the music layer's calm frames (g_msTier == 0) keep this clock
        // fresh. music_tier::Tick runs first in boot::FrameTick, so this is the same frame's
        // tier, matching the ext (FeatureCombatMusic -> AlertstingTick).
        const uint64_t now = GetTickCount64();
        if (music_tier::Tier() == 0)
            g_alsCalmMs = now;
        if (g_alsDue && now >= g_alsDue) {
            g_alsDue = 0;
            if (!music_tier::Dead() && !net::player_status::LocalDowned())
                Play(0.8f, g_alsDueWhy ? g_alsDueWhy : "shot at");
        }
        // (the ext also ticks the stun tone, the crossbow HUD and the stun cough here; those
        // are separate features and are not ported)
    }

}
