#pragma once

// Pure death-drone rules (tools/ext/death_theme.c), with no engine, audio, net or
// window headers and no clock: the native owner in death-theme.cpp supplies now,
// the match state, the PCM results and the RNG draw. Kept exactly:
//   * a bed picked at random per death, never the same wave twice in a row;
//   * arm -> immediate first copy, then overlapping copies at (length - crossfade);
//   * a squared sidechain duck with a 60 ms attack / 700 ms release;
//   * a single edge-triggered 600 ms fade-out, at most three live copies;
//   * the final countdown drop-before-first-spawn rule.
// The ext's t2r_rand is seeded from GetTickCount64; that clock stays in the owner.
// Tests: mod/tests/death_theme_core_test.cpp.

#include <cmath>
#include <cstdint>

namespace net::audio::death_theme::core {

    inline constexpr float    kBed       = 0.30f;  // ext DTH_BED
    inline constexpr float    kFloor     = 0.02f;  // ext DTH_FLOOR
    inline constexpr float    kDuckFull  = 0.25f;  // ext DTH_DUCK_FULL
    inline constexpr float    kDownS     = 0.06f;  // ext DTH_DOWN_S (duck attack)
    inline constexpr float    kUpS       = 0.70f;  // ext DTH_UP_S (duck release)
    inline constexpr uint64_t kFadeMs    = 600;    // ext DTH_FADE_MS
    inline constexpr int      kMaxInst   = 3;      // ext DTH_MAX_INST

    struct State {
        bool      armed   = false;   // ext g_dth.armed
        bool      fading  = false;   // ext g_dth.fading (one fade-out per death)
        int       count   = 0;       // successful copy starts since this arm
        int       cue     = -1;      // -1 before the first pick
        int       prev    = -1;      // ext's static `prev`, outlives dth_reset
        uint64_t  armT0   = 0;       // ext g_dth.arm_t0
        uint64_t  firstT0 = 0;       // ext g_dth.first_t0 (first copy's start)
        uint64_t  tickT   = 0;       // ext g_dth.tick_t (duck filter clock)
        float     lvl     = 0.0f;    // smoothed bed level
        float     fg      = 0.0f;    // last foreground peak
        uint64_t  t0[kMaxInst]{};   // ext g_dth.t0[], start of each copy
    };

    inline void Reset(State& s) noexcept { s = State{}; }

    // The ext's shared "xorshift32" (t2r_rand), exactly as near-miss-core: a
    // 64-bit state with the 13/7/17 triple, clock-seeded on first use by the
    // OWNER (GetTickCount64 stays there). One draw feeds Arm / PickCue.
    inline uint32_t NextRand(uint64_t& state, uint64_t nowMs) noexcept
    {
        uint64_t x = state;
        if (!x)
            x = (nowMs * 0x9E3779B97F4A7C15ULL) | 1ULL;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        state = x;
        return static_cast<uint32_t>(x >> 33);
    }

    // ext dth_arm's pool: rand % (count - (prev >= 0)), skipping prev once known.
    inline int PickCue(State& s, int waveCount, uint32_t rand) noexcept
    {
        int cue = 0;
        if (waveCount > 1) {
            cue = static_cast<int>(rand % static_cast<uint32_t>(waveCount - (s.prev >= 0 ? 1 : 0)));
            if (s.prev >= 0 && cue >= s.prev)
                ++cue;
        }
        s.cue = cue;
        s.prev = cue;                 // no immediate repeat across deaths
        return cue;
    }

    // ext dth_arm after its hard cut: reset instances / timers, keep the no-repeat
    // history, pick the bed and start at the floor with the foreground ducked.
    inline int Arm(State& s, uint64_t now, int waveCount, uint32_t rand) noexcept
    {
        const int prev = s.prev;
        Reset(s);
        s.prev = prev;
        const int cue = PickCue(s, waveCount, rand);
        s.armed = true;
        s.armT0 = now;
        s.lvl = kFloor;
        s.fg = 1.0f;
        return cue;
    }

    // ext dth_tick: floor + (bed - floor) * d^2, d = clamp(1 - fg / duck full).
    inline float DuckTarget(float fg) noexcept
    {
        float d = 1.0f - fg / kDuckFull;
        if (d < 0.0f) d = 0.0f;
        if (d > 1.0f) d = 1.0f;
        return kFloor + (kBed - kFloor) * d * d;
    }

    // Fast down, slow up, one exponential filter step; dt == 0 holds the level.
    inline float ApproachLevel(float lvl, float target, float dt) noexcept
    {
        const float tau = target < lvl ? kDownS : kUpS;
        const float a = dt > 0.0f ? 1.0f - std::exp(-dt / tau) : 0.0f;
        return lvl + (target - lvl) * a;
    }

    // ext's overlap test: now - latest start >= wave length - baked crossfade.
    inline bool LoopDue(uint64_t now, uint64_t latestT0, int frames,
                        int rate, int crossfadeMs) noexcept
    {
        if (frames <= 0 || rate <= 0)
            return false;
        const uint64_t period = static_cast<uint64_t>(frames) * 1000u
                              / static_cast<uint64_t>(rate);
        const uint64_t fade = crossfadeMs > 0 ? static_cast<uint64_t>(crossfadeMs) : 0u;
        const uint64_t threshold = period > fade ? period - fade : 0u;
        return now >= latestT0 && now - latestT0 >= threshold;
    }

    // Newest copy start across the three slots (0 = unused), like ext's `last` scan.
    inline uint64_t LatestT0(const State& s) noexcept
    {
        uint64_t newest = 0;
        for (uint64_t t : s.t0)
            if (t > newest) newest = t;
        return newest;
    }

    // ext dth_tick's cut priority: match state, screen phase, then the final fade.
    enum class CutReason {
        kNone, kNotInPlay, kDeathScreenOff, kRespawned, kMatchEnded, kFinalFade
    };

    inline CutReason ShouldCut(int phase, bool final, bool inPlay) noexcept
    {
        if (!inPlay)              return CutReason::kNotInPlay;
        if (phase == 0)           return CutReason::kDeathScreenOff;
        if (phase == 2)           return CutReason::kRespawned;
        if (phase >= 3)           return phase == 3 ? CutReason::kMatchEnded
                                                   : CutReason::kDeathScreenOff;
        if (final)                return CutReason::kFinalFade;
        return CutReason::kNone;
    }

    // The ext's log strings, so the owner's "death drone:" lines keep the wording.
    inline const char* Why(CutReason reason) noexcept
    {
        switch (reason) {
        case CutReason::kNotInPlay:    return "match left the playing state";
        case CutReason::kDeathScreenOff: return "death screen off";
        case CutReason::kRespawned:    return "respawned";
        case CutReason::kMatchEnded:   return "match ended";
        case CutReason::kFinalFade:    return "countdown fade to black";
        case CutReason::kNone:         return "none";
        }
        return "?";
    }

    // ext: if the respawn fade arrives before the first copy, drop the armed cue
    // instead of fading anything (the drone never actually played).
    inline bool ShouldDropFinalBeforeFirst(bool armed, int count) noexcept
    {
        return armed && count == 0;
    }

} // namespace net::audio::death_theme::core
