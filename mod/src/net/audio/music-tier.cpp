// ported from tools/ext/t2r-ext.c (MusicEvent + the tier derivation in FeatureCombatMusic;
// the ext's stinger layer g_msDead read in the surviving-music section)

#include "music-tier.hpp"

#include "engine/flag.hpp"
#include "engine/frame.hpp"
#include "engine/local-player.hpp"
#include "engine/script.hpp"

#include "player-status.hpp"

#include "log.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace net::audio::music_tier {

    namespace {

        constexpr float   kHeatCap      = 30.0f;   // ext: g_musHeat clamped to 30
        constexpr uint64_t kHotEndMs     = 900;     // ext NR_END_MS
        constexpr uint64_t kDwellMs      = 6500;    // ext: minimum combat dwell (t2r-music-dwell-old.txt disables)
        constexpr uint64_t kCalmDownMs   = 4000;    // ext: calm 4 s (contact lost), NR-off path
        constexpr uint64_t kHoldMs       = 1500;    // ext: g_msHoldUntil after death

        float    g_heat       = 0.0f;
        uint64_t g_evtMs      = 0;
        uint64_t g_awareMs    = 0;
        uint64_t g_contactMs  = 0;
        uint64_t g_hitRxMs    = 0;   // ext g_musHitRxMs: a relayed hit we took
        uint64_t g_holdUntil  = 0;
        uint64_t g_tierAt     = 0;
        uint64_t g_lastMs     = 0;
        uint64_t g_calmSince  = 0;
        uint64_t g_logMs      = 0;
        int      g_tier       = 0;
        bool     g_dead       = false;
        bool     g_inMatch    = false;
        bool     g_wasFiring  = false;
        bool     g_wasMelee   = false;
        bool     g_wasPaired  = false;
        float    g_lastHp     = -1.0f;
        char     g_cause[64]  = "?";
        char     g_awareWhy[48] = "?";

        // ext MusicEvent's awareness list. These set nr_hot_at / the "enemy is aware" clock.
        // Our own shots / melee swings are deliberately NOT in it (ext: "merely being near /
        // a mark / a downed enemy / our own shots is not [awareness]").
        bool IsAwareCause(const char* c)
        {
            static const char* const kAware[] = {
                "spotted by an enemy (spot call)", "we took a hit", "we hit an enemy",
                "enemy shot heard", "enemy fire continues", "enemy fired near",
                "enemy at close quarters", "we are stunned", "enemy stunned",
                "enemy revived",
            };
            for (const char* a : kAware)
                if (std::strcmp(c, a) == 0)
                    return true;
            return false;
        }

        bool NrRules()
        {
            // ext: nr = !t2r-music-nr-rules-old.txt (cached per call site there; the flag cache
            // here does the same without touching the disk every frame).
            return engine::flag::Absent("t2r-music-nr-rules-old.txt");
        }

        void Log(float add, const char* cause)
        {
            const uint64_t now = GetTickCount64();
            if (now - g_logMs <= 250)
                return;
            g_logMs = now;
            TO1_LOG("[MUSIC-TIER] input +%.1f %s (heat %.1f, tier %d)\n",
                add, cause, g_heat, g_tier);
        }

    }

    void NoteEvent(float add, const char* cause)
    {
        if (!::net::player_status::MatchLive())
            return;                              // ext: MatchSubstate() != 6
        const uint64_t now = GetTickCount64();
        g_heat = std::min(kHeatCap, g_heat + add);
        g_evtMs = now;
        if (std::strcmp(cause, "we took a hit") == 0)
            g_hitRxMs = now;                     // ext recv_hit_hook: g_musHitRxMs
        if (std::strcmp(cause, "health dropped") != 0
            && std::strstr(cause, "teammate") == nullptr)
            g_contactMs = now;                   // ext g_mus_contact_ms
        if (IsAwareCause(cause)) {
            g_awareMs = now;                     // ext g_musAwareMs / nr_hot_at
            std::snprintf(g_awareWhy, sizeof g_awareWhy, "%s", cause);
        }
        std::snprintf(g_cause, sizeof g_cause, "%s", cause);
        Log(add, cause);
    }

    void Tick()
    {
        const uint64_t now = GetTickCount64();
        const bool live = ::net::player_status::MatchLive();

        // ext g_msDead: the own alive-peer byte + HP <= 0.5. The mod has no peer byte here;
        // our own HP at / below zero in a live match is the equivalent (LocalAlive is > 0).
        g_dead = live && !::net::player_status::LocalAlive();
        if (g_dead)
            g_holdUntil = now + kHoldMs;

        if (!live) {
            if (g_tier || g_heat > 0.0f)
                TO1_LOG("[MUSIC-TIER] match left playing state - tier %d -> 0, heat %.1f\n",
                    g_tier, g_heat);
            g_tier = 0;
            g_heat = 0.0f;
            g_awareMs = g_evtMs = g_contactMs = g_calmSince = g_hitRxMs = 0;
            g_lastMs = 0;
            g_inMatch = false;
            g_wasFiring = g_wasMelee = g_wasPaired = false;
            g_lastHp = -1.0f;
            return;
        }
        if (!g_inMatch) {
            g_inMatch = true;
            g_awareMs = 0;   // ext: no "FIGHT START" at spawn off a stale awareness sign
            g_lastMs = now;
            TO1_LOG("[MUSIC-TIER] live (event heat, tiers 2 / 6 / 12; No Return rules %s)\n",
                NrRules() ? "on" : "off");
        }

        const float dt = (g_lastMs && now - g_lastMs < 500)
            ? static_cast<float>(now - g_lastMs) / 1000.0f : 0.016f;
        g_lastMs = now;
        g_heat *= std::pow(0.5f, dt / 3.0f);   // ext: heat halves every 3 s

        // ---- portable local producers (ext FeatureCombatMusic) --------------------------
        const bool firing = DC::Call<"player-is-firing?">() != 0;
        if (firing && !g_wasFiring && !::net::player_status::LocalDowned())
            NoteEvent(1.5f, "we fired");
        g_wasFiring = firing;

        const bool melee = DC::Call<"character-in-melee-attack?">(SID("player")) != 0;
        if (melee && !g_wasMelee && !::net::player_status::LocalDowned())
            NoteEvent(3.0f, "we melee");
        g_wasMelee = melee;

        const bool paired = ::net::player_status::LocalInPairedMove();
        if (paired && !g_wasPaired) {
            NoteEvent(3.0f, "paired move (shiv / execution / finisher)");
            g_awareMs = now;   // ext exec_near: an execution in progress is a hot sign
            std::snprintf(g_awareWhy, sizeof g_awareWhy, "%s", "execution in progress");
        }
        g_wasPaired = paired;

        // ext: own HP polled every 6th frame; a drop is a music input only when contact was
        // recent (the relayed hit itself is already an input, so don't double count it).
        if (::engine::frame::Count() % 6 == 0) {
            const float hp = ::engine::local_player::Health();
            if (hp > 0.0f && g_lastHp > 0.0f && hp < g_lastHp - 0.5f
                && now - g_contactMs <= 10000 && now - g_hitRxMs > 400)
                NoteEvent((g_lastHp - hp) / 25.0f, "health dropped");
            if (hp > 0.0f)
                g_lastHp = hp;
        }
        // TODO(music): the enemy-aware floor (enemy in sight / close / marked, spot calls,
        // enemy shots, close quarters, stuns, revives) and the lives / final-30 s floors are
        // unported. The ext's floor_heat holds at 6 s; none of it can be derived here yet.

        // ---- tier derivation (ext FeatureCombatMusic) ----------------------------------
        // TODO(music): last stand / last man standing (ext tiers 4 / 5) need the lives input.
        int byHeat;
        if (NrRules()) {
            // ext: one combat level -- every cue of a fight is the top playlist, never a
            // 3-step ladder. Hot = any enemy-aware sign in the last 0.9 s.
            byHeat = (g_awareMs && now - g_awareMs <= kHotEndMs) ? 3 : 0;
        } else {
            // ext: the earlier 3-step heat ladder with the 6 s held floor (the floor's
            // sight / contact producers are gated above, so heat alone drives it here).
            byHeat = g_heat >= 12.0f ? 3 : g_heat >= 6.0f ? 2 : g_heat >= 2.0f ? 1 : 0;
        }

        int want = g_tier;
        if (byHeat > g_tier && !::net::player_status::LocalDowned() && !g_dead) {
            want = byHeat;                       // ext: up at once, the frame it crosses
        } else if (g_tier >= 1 && g_tier <= 3 && now >= g_holdUntil) {
            if (NrRules()) {
                // ext: the fight is over 0.9 s after the last alerted enemy, with a minimum
                // 6.5 s combat dwell (t2r-music-dwell-old.txt removes the dwell).
                const bool dwellOk = engine::flag::Present("t2r-music-dwell-old.txt")
                    || now - g_tierAt >= kDwellMs;
                if (byHeat == 0 && dwellOk) {
                    if (!g_calmSince)
                        g_calmSince = now;
                    // ext then plays the decay + search bed before returning to silence;
                    // with no playlists ported the tier just returns to 0 here.
                    want = 0;
                } else
                    g_calmSince = 0;
            } else {
                // ext (old rules): calm 4 s (contact lost), ease one tier after 5 s quiet.
                const bool quiet = !g_evtMs || now - g_evtMs > 3000;
                if (g_heat < 1.0f) {
                    if (!g_calmSince)
                        g_calmSince = now;
                    if (now - g_calmSince > kCalmDownMs && now - g_tierAt > 5000)
                        want = 0;
                } else {
                    g_calmSince = 0;
                    if (quiet && now - g_tierAt > 5000) {
                        const float th = g_tier == 1 ? 2.0f : g_tier == 2 ? 6.0f : 12.0f;
                        if (g_heat < th * 0.75f)
                            want = g_tier - 1;
                    }
                }
            }
        }
        if (g_heat >= 1.0f && !NrRules())
            g_calmSince = 0;

        if (want != g_tier) {
            const int old = g_tier;
            // ext: the grid-quantised cut (pend_at / MusicBoundary) is a MUSIC-PLAYBACK
            // concern -- with no playlists ported there is no loop to wait on, so the tier
            // moves at once. Documented deviation.
            g_tier = want;
            g_tierAt = now;
            g_calmSince = 0;
            TO1_LOG("[MUSIC-TIER] tier %d -> %d (%s; heat %.1f, last aware %llu ms ago, %s)\n",
                old, want, g_cause,
                g_heat, g_awareMs ? static_cast<unsigned long long>(now - g_awareMs) : 0ULL,
                g_awareWhy);
        }
    }

    int Tier()   { return g_tier; }
    bool Dead()  { return g_dead; }

    void Reset()
    {
        g_heat = 0.0f;
        g_evtMs = g_awareMs = g_contactMs = g_hitRxMs = g_holdUntil = 0;
        g_tierAt = g_lastMs = g_calmSince = g_logMs = 0;
        g_tier = 0;
        g_dead = false;
        g_inMatch = false;
        g_wasFiring = g_wasMelee = g_wasPaired = false;
        g_lastHp = -1.0f;
        std::strcpy(g_cause, "?");
        std::strcpy(g_awareWhy, "?");
    }

}
