#pragma once

// Pure core for the stun concussion tone from tools/ext/alert_sting.c (lines 95-167).
// No engine, audio, match, clock or wire calls live here: the native owner in
// stun-tone.cpp supplies the clock, the match / downed / dying gates, the local
// root and the PCM result, and the rules are exactly the ext's:
//   * the bed is built once from two stereo sources: the ear-split impact under the
//     ~410 Hz tinnitus ring, 0.12 s in, raised-cosine fade over the last 1.5 s,
//     ring faded in with smoothstep from 0.30 s over 0.40 s, impact tail cut over
//     0.40 s, sum clamped to +/-32000 (ext STT_* / stuntone_build);
//   * a burst inside radius + 0.5 m starts an UNCONFIRMED voice; the same voice is
//     not restarted inside 1500 ms (ext stuntone_burst);
//   * stunned() confirms that voice without replaying it, or starts a confirmed
//     voice when no burst was seen (ext stuntone_stunned);
//   * unconfirmed after >700 ms fades 150 ms, the match leaving play fades 100 ms,
//     a confirmed voice >900 ms with no stun fx fades 250 ms, and a dead handle is
//     simply invalidated (ext stuntone_tick, in that order).
//
// The builder assumes both private waves share one rate (ext ST_RATE == SR_RATE);
// the owner passes the impact rate. Samples past a source's declared frame count
// read as zero, which keeps a shorter private wave safe without changing the ext
// result when its waves are long enough. The native owner (stun-tone.cpp) is the
// sole owner of this sub-feature when the ext is absent; boot::ExtLoaded() yields
// it while the ext is loaded. Tests: mod/tests/stun_audio_core_test.cpp.

#include <cmath>
#include <cstdint>
#include <vector>

namespace net::audio::stun_tone::core {

    using Sample = int16_t;                 // engine::pcm::Sample

    inline constexpr float kBedSeconds       = 3.45f;   // ext STT_LEN_S
    inline constexpr float kIntroSeconds     = 0.12f;   // ext: ring-in / level ramp
    inline constexpr float kFadeSeconds      = 1.50f;   // ext fo: raised-cosine tail
    inline constexpr float kRingAtSeconds    = 0.30f;   // ext STT_RING_AT
    inline constexpr float kRingInSeconds    = 0.40f;   // ext STT_RING_IN
    inline constexpr float kImpactTailSeconds = 0.40f;  // ext's impact hf fade
    inline constexpr float kClamp            = 32000.0f;
    inline constexpr float kPi               = 3.14159265f;

    inline constexpr uint64_t kObserveMs     = 1500;    // burst / confirm reuse window
    inline constexpr uint64_t kConfirmByMs   = 700;     // fade a burst that did not stun
    inline constexpr uint64_t kStunCheckMs   = 900;     // start looking for a live stun fx
    inline constexpr int kFadeNotStunnedMs   = 150;
    inline constexpr int kFadeNotLiveMs      = 100;
    inline constexpr int kFadeStunOverMs     = 250;

    struct State {
        uint64_t t0        = 0;       // ext g_stt.t0
        bool     confirmed = false;   // ext g_stt.confirmed
        bool     started   = false;   // ext g_stt.v.a >= 0 (a voice handle was taken)
    };

    inline int BedFrames(int rate) noexcept
    {
        return rate > 0 ? static_cast<int>(kBedSeconds * static_cast<float>(rate)) : 0;
    }

    // Both output vectors are owned by the caller (the owner builds once and keeps them).
    // `impact` and `ring` are packed stereo: channel 1 starts frames[0] samples later.
    inline void BuildBed(std::vector<Sample>& outL, std::vector<Sample>& outR,
                         const Sample* impact, int impactFrames,
                         const Sample* ring, int ringFrames, int rate)
    {
        const int n = BedFrames(rate);
        outL.assign(n, 0);
        outR.assign(n, 0);
        if (n <= 0 || rate <= 0)
            return;

        std::vector<Sample>* out[2] = { &outL, &outR };
        const int ringAt = static_cast<int>(kRingAtSeconds * static_cast<float>(rate));
        const float ringInDiv = kRingInSeconds * static_cast<float>(rate);
        const int ringInInt = static_cast<int>(ringInDiv);
        const float T = static_cast<float>(n) / static_cast<float>(rate);

        for (int c = 0; c < 2; ++c) {
            const Sample* hit = impact ? impact + (c ? impactFrames : 0) : nullptr;
            const Sample* rr = ring ? ring + (c ? ringFrames : 0) : nullptr;
            for (int i = 0; i < n; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(rate);
                float env = t < kIntroSeconds ? t / kIntroSeconds : 1.0f;
                if (t > T - kFadeSeconds) {
                    const float u = (t - (T - kFadeSeconds)) / kFadeSeconds;
                    env *= 0.5f * (1.0f + std::cos(kPi * u));
                }

                const int ri = i - ringAt;
                const float rin = ri <= 0 ? 0.0f
                                : ri >= ringInInt ? 1.0f
                                : static_cast<float>(ri) / ringInDiv;
                const float rs = (rr && ri >= 0 && ri < ringFrames)
                               ? static_cast<float>(rr[ri]) * rin * rin * (3.0f - 2.0f * rin)
                               : 0.0f;

                const float hf = t < T - kImpactTailSeconds
                               ? 1.0f : (T - t) / kImpactTailSeconds;
                const float hitSample = (hit && i < impactFrames)
                                      ? static_cast<float>(hit[i]) : 0.0f;
                float x = rs * env + hitSample * hf;
                if (x > kClamp) x = kClamp;
                if (x < -kClamp) x = -kClamp;
                (*out[c])[i] = static_cast<Sample>(x);
            }
        }
    }

    // ext stuntone_burst's distance: the bomb's Y is compared with our root + 1 m.
    inline float BurstDistance(const float at[3], const float localRoot[3]) noexcept
    {
        const float dx = at[0] - localRoot[0];
        const float dy = at[1] + 0.0f - (localRoot[1] + 1.0f);
        const float dz = at[2] - localRoot[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    inline bool BurstInRange(const float at[3], const float localRoot[3],
                             float radius) noexcept
    {
        return BurstDistance(at, localRoot) <= radius + 0.5f;
    }

    inline bool VoiceUsable(const State& s, uint64_t now, bool voiceLive) noexcept
    {
        return s.started && voiceLive && now >= s.t0 && now - s.t0 < kObserveMs;
    }

    inline void Start(State& s, uint64_t now, bool confirmed) noexcept
    {
        s.t0 = now;
        s.confirmed = confirmed;
        s.started = true;
    }

    // ext stuntone_stunned: a live burst voice is confirmed, not replayed.
    inline bool CanConfirm(const State& s, uint64_t now, bool voiceLive) noexcept
    {
        return VoiceUsable(s, now, voiceLive);
    }

    inline void Confirm(State& s) noexcept { s.confirmed = true; }

    enum class Fade {
        kNone,
        kNotStunned,   // fade 150 ms
        kNotLive,      // fade 100 ms
        kStunOver,     // fade 250 ms
        kInvalidate    // the handle is already gone; forget it
    };

    // Checks run in the ext's order; a fade retires the state because the owner
    // clears the voice handle with Cut() (also when Cut is a no-op on a dead voice).
    inline Fade Tick(State& s, uint64_t now, bool voiceLive,
                     bool matchLive, bool stunFxLive) noexcept
    {
        if (!s.started)
            return Fade::kNone;
        if (!s.confirmed && now >= s.t0 && now - s.t0 > kConfirmByMs) {
            s.started = false;
            return Fade::kNotStunned;
        }
        if (!matchLive) {
            s.started = false;
            return Fade::kNotLive;
        }
        if (s.confirmed && now >= s.t0 && now - s.t0 > kStunCheckMs && !stunFxLive) {
            s.started = false;
            return Fade::kStunOver;
        }
        if (!voiceLive) {
            s.started = false;
            return Fade::kInvalidate;
        }
        return Fade::kNone;
    }

    inline void Reset(State& s) noexcept { s = State{}; }

} // namespace net::audio::stun_tone::core
