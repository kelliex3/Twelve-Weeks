// ported from tools/ext/alert_sting.c (crossbow bolt HUD: xbhud_tick). All rules
// live in crossbow-hud-core.hpp; the engine / audio / net seams stay here.

#include "crossbow-hud.hpp"

#include "crossbow-hud-pcm.hpp"

#include "boot.hpp"
#include "engine/pcm.hpp"
#include "log.hpp"
#include "net/game/player-status.hpp"

#include <windows.h>

#include <cstdint>

namespace net::audio::crossbow_hud {

    namespace {

        namespace core = net::audio::crossbow_hud::core;
        namespace sink = net::audio::crossbow_hud::pcm;

        core::State g_state;
        engine::pcm::Voice g_pulse, g_one;

        bool ExtOwns() noexcept { return boot::ExtLoaded(); }

        void PlayOne(sink::Wave wave, float gain, engine::pcm::Voice& voice,
                     const char* what)
        {
            (void)what;                            // log-only in debug builds
            engine::pcm::Cut(voice, 0);          // ext als_v2_play replaces the one-shot
            voice = sink::Play(wave, gain);
            TO1_LOG("[XBOWHUD] %s (slot %d) -> %s\n", what, g_state.slot,
                    engine::pcm::Live(voice) ? "playing" : "mixer off / voices busy");
        }

    } // namespace

    void NoteBolt(int slot, uint64_t hitMs)
    {
        if (ExtOwns())
            return;
        core::NoteBolt(g_state, slot, hitMs);
    }

    void NoteClear(bool pulledOut)
    {
        if (ExtOwns())
            return;
        core::NoteClear(g_state, pulledOut);
    }

    void Tick()
    {
        if (ExtOwns())
            return;
        // No private data: no voice, no hit_seen / next_pulse consumption and no
        // playback log. Wire feeds are still accepted above.
        if constexpr (!sink::kHave)
            return;

        const uint64_t now = GetTickCount64();
        const core::Actions a = core::Tick(g_state, now,
                                           net::player_status::MatchLive(),
                                           sink::PulsePeriodMs());
        if (a.impact)
            PlayOne(sink::Wave::kImpact, 0.8f, g_one, "impact");
        if (a.pulse) {
            engine::pcm::Cut(g_pulse, 0);
            g_pulse = sink::Play(sink::Wave::kPulse, 0.55f);
        }
        if (a.fadePulse)
            engine::pcm::Cut(g_pulse, 200);
        if (a.pull)
            PlayOne(sink::Wave::kClear, 0.8f, g_one, "pulled out");
    }

    void Reset()
    {
        core::Reset(g_state);
        engine::pcm::Cut(g_pulse, 0);
        engine::pcm::Cut(g_one, 0);
        engine::pcm::Invalidate(g_pulse);
        engine::pcm::Invalidate(g_one);
    }

    bool Active() noexcept
    {
        return !ExtOwns() && core::Active(g_state, net::player_status::MatchLive());
    }

} // namespace net::audio::crossbow_hud
