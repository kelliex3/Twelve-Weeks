// ported from tools/ext/alert_sting.c (the alert sting; the stun tone / crossbow HUD / stun
// cough sub-features are separate and not ported). Pure rules: alertsting-core.hpp.

#include "alertsting.hpp"

#include "alertsting-pcm.hpp"

#include "boot.hpp"
#include "engine/flag.hpp"
#include "engine/pcm.hpp"

#include "music-tier.hpp"
#include "net/game/player-status.hpp"

#include "log.hpp"

#include <windows.h>

#include <cstdint>

namespace net::audio::alertsting {

    namespace {

        namespace core = net::audio::alertsting::core;
        namespace sink = net::audio::alertsting::pcm;

        core::State g_state;

        bool ExtOwns() noexcept { return boot::ExtLoaded(); }

    }

    void Play(float level, const char* why)
    {
        if (ExtOwns() || engine::flag::Present("t2r-no-alert-sting.txt"))
            return;
        const char* tag = why ? why : "?";
        (void)tag;                             // log-only when TO1_DEBUG is off
        const uint64_t now = GetTickCount64();
        const float gain = level * engine::pcm::MusicUserVolume();
        const core::PlayGate gate = core::CheckPlay(g_state, now,
                                                    net::player_status::MatchLive(), gain);
        if (gate == core::PlayGate::kNotLive)
            return;
        if (gate == core::PlayGate::kGap) {
            TO1_LOG("[ALERTSTING] %s ignored - played %llu ms ago\n", tag,
                    static_cast<unsigned long long>(now - g_state.last));
            return;
        }
        if (gate == core::PlayGate::kSilent)
            return;                             // ext: g <= 0 -> nothing to play

        engine::pcm::Voice l, r;
        if (!sink::Play(gain, l, r)) {
            TO1_LOG("[ALERTSTING] %s - %s\n", tag,
                    sink::kHave ? "mixer off / voices busy" : "data not built (TO1_ALERT_STING_PCM off)");
            return;
        }
        core::CommitPlay(g_state, now);           // ext only stamps the gap on a started voice
        TO1_LOG("[ALERTSTING] %s - playing (%.2f s, gain %.2f)\n", tag,
                sink::Seconds(), static_cast<double>(gain));
    }

    void Combat(const char* why)
    {
        if (ExtOwns())
            return;
        const uint64_t now = GetTickCount64();
        core::Combat(g_state, now, net::player_status::MatchLive(),
                     music_tier::Dead(), net::player_status::LocalDowned(), why);
    }

    void Tick()
    {
        if (ExtOwns())
            return;
        // music_tier::Tick runs first in boot::FrameTick, so this is the same frame's tier.
        const uint64_t now = GetTickCount64();
        const core::Due due = core::Tick(g_state, now, music_tier::Tier() == 0,
                                         music_tier::Dead(),
                                         net::player_status::LocalDowned());
        if (due.fire)
            Play(due.level, due.why);
        // (the ext also ticks the stun tone, the crossbow HUD and the stun cough here; those
        // are separate features and are not ported)
    }

    void Reset()
    {
        core::Reset(g_state);
    }

} // namespace net::audio::alertsting
