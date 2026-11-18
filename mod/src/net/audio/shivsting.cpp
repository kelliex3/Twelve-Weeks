// ported from tools/ext/shiv_sting.c

#include "shivsting.hpp"

#include "alertsting.hpp"

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

namespace net::audio::shivsting {

    namespace {

        constexpr float    kLevel    = 0.9f;     // ext SHS_LEVEL
        constexpr uint64_t kDedupeMs = 2500;     // ext SHS_DEDUPE_MS

        struct State {
            engine::pcm::Voice a, b;
            int      cue  = 0;
            uint64_t t0   = 0;
            uint64_t last = 0;
        };
        State g_shs;

        bool Live()
        {
            return engine::pcm::Live(g_shs.a) || engine::pcm::Live(g_shs.b);
        }

        uint32_t Rand()
        {
            static std::mt19937 rng{ std::random_device{}() };
            return rng();
        }

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

        // The stealth-notification alert as a prominent sting on top, ONLY for a shiv from
        // behind on a victim that was not stunned (stun is not ported: the gate is absent).
        if (name && strstr(name, "stealth-back"))
            net::audio::alertsting::Play(1.0f, "back shiv victim (not stunned)");

#ifndef T2R_DATA_SHIV_STING
        TO1_LOG("[SHIVSTING] %s - data not built (T2R_DATA_SHIV_STING off)\n", name ? name : "?");
        return;
#else
        const float g = kLevel * engine::pcm::MusicUserVolume();
        if (g <= 0.0f) {
            TO1_LOG("[SHIVSTING] %s skipped (options music volume 0)\n", name ? name : "?");
            return;
        }
        if (Live())
            Cut(0, "new sting");
        g_shs.a = {};
        g_shs.b = {};

        // The pool (candidates): random, never the same cue twice in a row.
        static int prev = -1;
        int cue = SHS_K > 1 ? static_cast<int>(Rand() % static_cast<uint32_t>(SHS_K - (prev >= 0))) : 0;
        if (SHS_K > 1 && prev >= 0 && cue >= prev)
            cue++;
        prev = cue;

        const int n = SHS_LEN[cue], rate = SHS_RATES[cue];
        const engine::pcm::Sample* l = shs_blob + SHS_OFF[cue];
        const engine::pcm::Sample* r = l + n;
        if (engine::pcm::PlayStereoRaw(l, r, n, g, rate, g_shs.a, g_shs.b)) {
            g_shs.cue = cue;
            g_shs.t0 = now;
            TO1_LOG("[SHIVSTING] %s - playing cue %s (%.2f s, gain %.2f)\n",
                name ? name : "?", SHS_NAME[cue], n / static_cast<double>(rate), static_cast<double>(g));
        } else {
            TO1_LOG("[SHIVSTING] %s - NOT played (voices busy / mixer off)\n", name ? name : "?");
        }
#endif
    }

    void Cut(int fadeMs, const char* why)
    {
        (void)why;      // only the (debug-only) log uses it
        const bool live = Live();
        engine::pcm::Cut(g_shs.a, fadeMs);
        engine::pcm::Cut(g_shs.b, fadeMs);
        if (live)
            TO1_LOG("[SHIVSTING] cut (%s) %llu ms in\n", why ? why : "?",
                static_cast<unsigned long long>(GetTickCount64() - g_shs.t0));
    }

    int RemainingMs()
    {
#ifdef T2R_DATA_SHIV_STING
        if (!Live())
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
        if (g_shs.a.slot < 0 && g_shs.b.slot < 0)
            return;
        if (!Live()) {
            g_shs.a = {};
            g_shs.b = {};
            return;
        }
        if (!net::player_status::MatchLive())
            Cut(60, "match left the playing state");
    }

}
