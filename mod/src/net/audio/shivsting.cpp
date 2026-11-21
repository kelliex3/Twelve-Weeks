// ported from tools/ext/shiv_sting.c

#include "shivsting.hpp"

#include "alertsting.hpp"
#include "music-hold.hpp"

#include "gameplay/stun-lock.hpp"
#include "engine/flag.hpp"
#include "engine/pcm.hpp"

#include "net/game/player-status.hpp"

#include "log.hpp"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <random>

#ifdef T2R_DATA_SHIV_STING
#include "shiv_sting.h"     // generated (private): shs_blob, SHS_OFF, SHS_LEN, SHS_RATES, SHS_K, SHS_NAME
#endif
#ifdef T2R_DATA_SHIV_DRUMS
#include "shiv_drums.h"     // generated (private): shd_blob, SHD_OFF, SHD_LEN, SHD_RATE, SHD_K
#endif

namespace net::audio::shivsting {

    namespace {

        constexpr float    kLevel    = 0.9f;     // ext SHS_LEVEL
        constexpr uint64_t kDedupeMs = 2500;     // ext SHS_DEDUPE_MS
        constexpr float    kDrumLevel = 0.75f;   // ext SHD_LEVEL
        constexpr int      kDrumFadeMs = 350;    // ext SHD_FADE_MS
        constexpr uint64_t kDrumHoldMs = 4000;   // ext SHD_HOLD_MS

        struct State {
            engine::pcm::Voice a, b;
            int      cue  = 0;
            uint64_t t0   = 0;
            uint64_t last = 0;
        };
        State g_shs;   // shock sting
        State g_shd;   // struggle drums

        bool Live(const State& s)
        {
            return engine::pcm::Live(s.a) || engine::pcm::Live(s.b);
        }

        void Forget(State& s)
        {
            engine::pcm::Invalidate(s.a);
            engine::pcm::Invalidate(s.b);
            s.a = {};
            s.b = {};
        }

        uint32_t Rand()
        {
            static std::mt19937 rng{ std::random_device{}() };
            return rng();
        }

        // One take of a pool at random, never the same take twice in a row (ext's Shs / Shd pick).
        int Pick(int k, int& prev)
        {
            if (k <= 1)
                return 0;
            int p = static_cast<int>(Rand() % static_cast<uint32_t>(k));
            if (p == prev)
                p = (p + 1 + static_cast<int>(Rand() % static_cast<uint32_t>(k - 1))) % k;
            prev = p;
            return p;
        }

        void DrumTick()
        {
            if (g_shd.a.slot < 0 && g_shd.b.slot < 0)
                return;
            if (!Live(g_shd)) {
                Forget(g_shd);
                return;
            }
            if (!net::player_status::MatchLive())
                ShivMusicCut(60, "match left the playing state");
        }

    }

    void ShivMusicStart(const char* name)
    {
        if (engine::flag::Present("t2r-no-shiv-music.txt"))
            return;
        const uint64_t now = GetTickCount64();
        if (g_shd.last && now - g_shd.last < kDedupeMs)
            return;
        if (!net::player_status::MatchLive()) {
            TO1_LOG("[SHIVSTING] %s - music skipped, not in a running match\n", name ? name : "?");
            return;
        }
        g_shd.last = now;

        // The ext cuts the combat music with the game's rogue-player-death playlist and holds it
        // cut through the death screen. MusicPlay is a sink; the hold state is native.
        const uint64_t previous = music_hold::Current();
        music_hold::Play("music-playlist-rogue-player-death", "shiv victim: combat music out");
        music_hold::Hold(now + kDrumHoldMs);       // g_msHoldUntil == g_msResumeAt
        music_hold::NowatchUntil(now + kDrumHoldMs);

        const float g = kDrumLevel * engine::pcm::MusicUserVolume();
        TO1_LOG("[SHIVSTING] %s - combat music cut (was %llx), held %llu ms\n",
                name ? name : "?", static_cast<unsigned long long>(previous),
                static_cast<unsigned long long>(kDrumHoldMs));
        if (g <= 0.0f)
            return;

#ifndef T2R_DATA_SHIV_DRUMS
        TO1_LOG("[SHIVSTING] %s - drums data not built (T2R_DATA_SHIV_DRUMS off)\n", name ? name : "?");
        (void)g;
        return;
#else
        if (Live(g_shd))
            ShivMusicCut(0, "new move");
        Forget(g_shd);
        static int prev = -1;
        const int pick = Pick(SHD_K, prev);
        const engine::pcm::Sample* base = shd_blob + SHD_OFF[pick];
        const int n = SHD_LEN[pick];
        if (engine::pcm::PlayStereoRaw(base, base + n, n, g, SHD_RATE, g_shd.a, g_shd.b)) {
            g_shd.cue = pick;
            g_shd.t0 = now;
            TO1_LOG("[SHIVSTING] %s - drums playing (%.2f s, gain %.2f)\n", name ? name : "?",
                    n / static_cast<double>(SHD_RATE), static_cast<double>(g));
        } else {
            TO1_LOG("[SHIVSTING] %s - drums NOT played (voices busy / mixer off)\n", name ? name : "?");
        }
#endif
    }

    void Start(const char* name)
    {
        if (engine::flag::Present("t2r-no-shiv-sting.txt"))
            return;
        const uint64_t now = GetTickCount64();
        if (g_shs.last && now - g_shs.last < kDedupeMs) {
            TO1_LOG("[SHIVSTING] %s ignored - the same move already stung %llu ms ago\n",
                name ? name : "?", static_cast<unsigned long long>(now - g_shs.last));
            return;
        }
        if (!net::player_status::MatchLive()) {
            TO1_LOG("[SHIVSTING] %s skipped - not in a running match\n", name ? name : "?");
            return;
        }
        g_shs.last = now;

        // The stealth-notification alert is prominent only for a back shiv on a victim that was
        // NOT stunned (ext: !(g_stun_until && g_t60 < g_stun_until)).
        if (name && strstr(name, "stealth-back") != nullptr) {
            if (!gameplay::stun_lock::Active(now))
                net::audio::alertsting::Play(1.0f, "back shiv victim (not stunned)");
            else
                TO1_LOG("[ALERTSTING] back shiv on a stunned victim - none\n");
        }
        // The ext cuts our stun lock the moment the shiv half begins.
        gameplay::stun_lock::Cut(now);

#ifndef T2R_DATA_SHIV_STING
        TO1_LOG("[SHIVSTING] %s - data not built (T2R_DATA_SHIV_STING off)\n", name ? name : "?");
        return;
#else
        const float g = kLevel * engine::pcm::MusicUserVolume();
        if (g <= 0.0f) {
            TO1_LOG("[SHIVSTING] %s skipped (options music volume 0)\n", name ? name : "?");
            return;
        }
        if (Live(g_shs))
            Cut(0, "new sting");
        Forget(g_shs);

        static int prev = -1;
        const int cue = Pick(SHS_K, prev);
        const int n = SHS_LEN[cue], rate = SHS_RATES[cue];
        const engine::pcm::Sample* l = shs_blob + SHS_OFF[cue];
        const engine::pcm::Sample* r = l + n;
        if (engine::pcm::PlayStereoRaw(l, r, n, g, rate, g_shs.a, g_shs.b)) {
            g_shs.cue = cue;
            g_shs.t0 = now;
            TO1_LOG("[SHIVSTING] %s - playing cue %s (%.2f s, gain %.2f)\n",
                name ? name : "?", SHS_NAME[cue], n / static_cast<double>(rate),
                static_cast<double>(g));
        } else {
            TO1_LOG("[SHIVSTING] %s - NOT played (voices busy / mixer off)\n", name ? name : "?");
        }
#endif
    }

    void Cut(int fadeMs, const char* why)
    {
        // ext ShivstingCut ducks the drums under the death sting with at least SHD_FADE_MS.
        ShivMusicCut(fadeMs > 0 && fadeMs < kDrumFadeMs ? kDrumFadeMs : fadeMs, why);
        const bool live = Live(g_shs);
        engine::pcm::Cut(g_shs.a, fadeMs);
        engine::pcm::Cut(g_shs.b, fadeMs);
        if (live)
            TO1_LOG("[SHIVSTING] cut (%s) %llu ms in\n", why ? why : "?",
                static_cast<unsigned long long>(GetTickCount64() - g_shs.t0));
    }

    void ShivMusicCut(int fadeMs, const char* why)
    {
        const bool live = Live(g_shd);
        engine::pcm::Cut(g_shd.a, fadeMs);
        engine::pcm::Cut(g_shd.b, fadeMs);
        if (live)
            TO1_LOG("[SHIVSTING] drums faded %d ms (%s) %llu ms in\n", fadeMs,
                why ? why : "?", static_cast<unsigned long long>(GetTickCount64() - g_shd.t0));
        Forget(g_shd);
    }

    int RemainingMs()
    {
#ifdef T2R_DATA_SHIV_STING
        if (!Live(g_shs))
            return 0;
        const int len = static_cast<int>(SHS_LEN[g_shs.cue] * 1000LL / SHS_RATES[g_shs.cue]);
        const int el = static_cast<int>(GetTickCount64() - g_shs.t0);
        return len > el ? len - el : 0;
#else
        return 0;
#endif
    }

    void Tick()
    {
        DrumTick();
        if (g_shs.a.slot < 0 && g_shs.b.slot < 0)
            return;
        if (!Live(g_shs)) {
            Forget(g_shs);
            return;
        }
        if (!net::player_status::MatchLive())
            Cut(60, "match left the playing state");
    }

    void Reset()
    {
        Forget(g_shs);
        Forget(g_shd);
        g_shs = {};
        g_shd = {};
    }

} // namespace net::audio::shivsting
