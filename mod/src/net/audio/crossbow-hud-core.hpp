#pragma once

// Pure core for the crossbow bolt HUD sounds from tools/ext/alert_sting.c (169-200).
// No engine, audio, match or clock calls live here: the owner supplies the clock and
// match gate and performs the PCM starts. The rules are the ext's xbhud_tick:
//   * on each new hit id while the bolt is in us during play, replay the impact
//     one-shot and arm the tonal pulse 900 ms later;
//   * repeat the pulse back to back at XBP_N * 1000 / XBP_RATE ms (a data-derived
//     period the caller computes, so this core stays independent of private PCM);
//   * on the transition off (or out of play) fade the pulse over 200 ms and, only
//     when the bolt was pulled out by us, play the pull-out one-shot;
//   * hit_seen / next_pulse / was / the pulled-out flag all live here.
//
// The native feed is NoteBolt(slot, hitMs) / NoteClear(pulledOut) from the crossbow
// bolt wire (MSG_XBOLT 0x6E, wired in net-replication.cpp). The native owner
// (crossbow-hud.cpp) is the sole owner of this sub-feature when the ext is absent;
// boot::ExtLoaded() yields it while the ext is loaded. Tests:
// mod/tests/stun_audio_core_test.cpp.

#include <cstdint>

namespace net::audio::crossbow_hud::core {

    inline constexpr uint64_t kPulseLeadMs = 900;   // ext: pulse under the impact tail

    struct State {
        bool     on         = false;   // ext g_xb.on (a bolt is in us)
        int      slot       = -1;      // producer slot, for logs only
        uint64_t hitMs      = 0;       // ext g_xb.hit_ms (new id = new impact)
        bool     pulledOut  = false;   // ext g_xb_out_pull
        bool     was        = false;   // ext static was
        uint64_t hitSeen    = 0;       // ext static hit_seen
        uint64_t nextPulse  = 0;       // ext static next_pulse
    };

    inline void NoteBolt(State& s, int slot, uint64_t hitMs) noexcept
    {
        s.on = true;
        s.slot = slot;
        s.hitMs = hitMs;
    }

    inline void NoteClear(State& s, bool pulledOut) noexcept
    {
        s.on = false;
        s.pulledOut = pulledOut;
    }

    // ext: (uint64_t)XBP_N * 1000 / XBP_RATE, integer division.
    inline uint64_t PulsePeriodMs(int frames, int rate) noexcept
    {
        return rate > 0 && frames > 0
             ? static_cast<uint64_t>(frames) * 1000u / static_cast<uint64_t>(rate)
             : 0;
    }

    struct Actions {
        bool impact = false;   // play XBI at 0.8 gain
        bool pulse = false;    // play XBP at 0.55 gain
        bool pull = false;     // play XBC at 0.8 gain
        bool fadePulse = false;// Cut(pulse, 200)
    };

    inline Actions Tick(State& s, uint64_t now, bool matchLive,
                        uint64_t pulsePeriodMs) noexcept
    {
        Actions out;
        const bool on = s.on && matchLive;   // ext on = g_xb.on && match_substate()==6

        if (on && (!s.was || s.hitMs != s.hitSeen)) {
            s.hitSeen = s.hitMs;
            s.nextPulse = now + kPulseLeadMs;
            out.impact = true;
        }
        if (on && now >= s.nextPulse) {
            s.nextPulse = now + pulsePeriodMs;
            out.pulse = true;
        }
        if (!on && s.was) {
            out.fadePulse = true;
            if (s.pulledOut && matchLive)
                out.pull = true;             // ext: only the transition off consumes it
        }
        s.pulledOut = false;                 // ext clears g_xb_out_pull there
        s.was = on;
        return out;
    }

    inline bool Active(const State& s, bool matchLive) noexcept
    {
        return s.on && matchLive;
    }

    inline void Reset(State& s) noexcept { s = State{}; }

} // namespace net::audio::crossbow_hud::core
