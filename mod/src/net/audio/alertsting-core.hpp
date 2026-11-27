#pragma once

// Pure alert-sting state machine (no engine, audio, net or clock calls). The native
// owner in alertsting.cpp supplies now / match / tier / PCM results; the rules here
// follow tools/ext/alert_sting.c exactly:
//   * Combat() stamps the threat clock BEFORE every gate;
//   * the "out of the quiet" gate is a fresh calm clock (tier 0 within 1500 ms) plus
//     no earlier threat inside ALS_QUIET_MS;
//   * the play is delayed ALS_DELAY_MS and Tick() rechecks only dead / downed;
//   * a sting updates the gap clock only when the PCM voice actually started.
// Tests: mod/tests/alertsting_core_test.cpp.

#include <cstdint>

namespace net::audio::alertsting::core {

    inline constexpr uint64_t kQuietMs     = 15000;  // ext ALS_QUIET_MS
    inline constexpr uint64_t kGapMs       = 6000;   // ext ALS_GAP_MS
    inline constexpr uint64_t kDelayMs     = 100;    // ext ALS_DELAY_MS
    inline constexpr uint64_t kCalmFreshMs = 1500;   // ext's "calm until at most 1.5 s ago"
    inline constexpr float    kCombatLevel = 0.8f;   // ext AlertstingTick
    inline constexpr float    kBackShivLevel = 1.0f; // ext shiv_sting.c

    struct State {
        uint64_t    last   = 0;   // last successful play (gap clock)
        uint64_t    calm   = 0;   // last frame the combat music tier was 0
        uint64_t    threat = 0;   // last enemy-caused input (hit / near miss)
        uint64_t    due    = 0;   // delayed play deadline
        const char* dueWhy = nullptr;
    };

    inline bool CalmFresh(const State& s, uint64_t now) noexcept
    {
        return s.calm != 0 && now >= s.calm && now - s.calm <= kCalmFreshMs;
    }

    // returns true when a delayed play was armed (ext AlertstingCombat).
    inline bool Combat(State& s, uint64_t now, bool live, bool dead,
                       bool downed, const char* why) noexcept
    {
        const uint64_t prev = s.threat;
        s.threat = now;                       // before ALL gates, as in the ext
        if (!live || dead || downed)
            return false;
        if (!CalmFresh(s, now))
            return false;
        if (prev && now - prev < kQuietMs)
            return false;
        s.due = now + kDelayMs;
        s.dueWhy = why;
        return true;
    }

    struct Due {
        bool        fire = false;
        float       level = kCombatLevel;
        const char* why = nullptr;
    };

    inline Due Tick(State& s, uint64_t now, bool tierCalm,
                    bool dead, bool downed) noexcept
    {
        if (tierCalm)
            s.calm = now;                     // ext AlertstingTick's tier-0 clock
        Due out;
        if (!s.due || now < s.due)
            return out;
        s.due = 0;
        if (dead || downed)
            return out;                       // the only recheck ext does at play time
        out.fire = true;
        out.why = s.dueWhy ? s.dueWhy : "shot at";
        s.dueWhy = nullptr;
        return out;
    }

    enum class PlayGate { kOk, kNotLive, kGap, kSilent };

    inline PlayGate CheckPlay(const State& s, uint64_t now,
                              bool live, float gain) noexcept
    {
        if (!live)
            return PlayGate::kNotLive;
        if (s.last && now - s.last < kGapMs)
            return PlayGate::kGap;
        if (gain <= 0.0f)
            return PlayGate::kSilent;
        return PlayGate::kOk;
    }

    inline void CommitPlay(State& s, uint64_t now) noexcept { s.last = now; }

    inline void Reset(State& s) noexcept { s = State{}; }

} // namespace net::audio::alertsting::core
