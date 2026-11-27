#pragma once

// Pure downed-music rules (no engine, audio, net or window headers). The native
// sole owner in downed-music.cpp supplies the clock, the downed state, PCM
// results and volume; everything decided here follows tools/ext/downed_music.c:
//   * the bleed fraction f is slewed toward the caller's fraction (that clock
//     already has the revive pause subtracted), and a >0.06 jump opens a 1.5 s
//     "quick" window where the next stage change is beat-synced instead of
//     bar-synced;
//   * the music starts 300 ms into a down with a 250 ms fade, stage changes
//     crossfade at a bar line (150 ms equal power) or at a beat after a hit
//     (100 ms), and the loop's appended tail is taken over with a 100 ms
//     linear crossfade at the loop point;
//   * bad gates cut in the native priority order: match left 400 ms, executed
//     300 ms, death 400 ms (or an ending tail on the next beat), revived 800 ms;
//   * the combat music is held every running frame and for kHoldMs after a cut;
//   * the death tail holds for its own length plus kHoldMs, and the death
//     sting is ducked by the tail's own 80 ms peak down to 15 %.
// The Input's downToken is the native g_down_ms (0 while up, otherwise the
// down's millisecond stamp): a change arms one start per down, and the start
// delay is measured from that stamp. Tests: mod/tests/downed_music_core_test.cpp.

#include <cmath>
#include <cstdint>

namespace net::audio::downed_music::core {

    inline constexpr float    kLevel        = 1.0f;
    inline constexpr uint64_t kStartMs      = 300;
    inline constexpr uint64_t kStartFadeMs  = 250;
    inline constexpr uint64_t kXfMs         = 150;
    inline constexpr uint64_t kXfHitMs      = 100;
    inline constexpr uint64_t kWrapMs       = 100;
    inline constexpr uint64_t kQuickMs      = 1500;
    inline constexpr uint64_t kHoldMs       = 1500;
    inline constexpr float    kLook         = 0.05f;
    inline constexpr float    kBarS         = 2.0f;
    inline constexpr float    kBeatS        = 0.5f;

    inline constexpr int      kStageCount   = 4;
    inline constexpr float    kStageF[kStageCount] = { 0.0f, 0.30f, 0.52f, 0.76f };

    inline constexpr float    kSmoothS      = 0.08f;
    inline constexpr float    kHitStep      = 0.06f;
    inline constexpr float    kHalfGuardS   = 0.02f;
    inline constexpr float    kPhaseGuardS  = 0.04f;
    inline constexpr float    kDuckRef      = 0.30f;
    inline constexpr float    kDuckDepth    = 0.85f;
    inline constexpr float    kTailNearS    = 0.03f; // ext's "rem > 0.03" tail guard

    // Additional ext numbers (not in the table above, kept named for the cuts).
    inline constexpr int      kMatchFadeMs  = 400;
    inline constexpr int      kExecFadeMs   = 300;
    inline constexpr int      kDeathFadeMs  = 400;
    inline constexpr int      kReviveFadeMs = 800;
    inline constexpr int      kTailFadeMs   = 300;
    inline constexpr int      kEndStage     = -1;  // the one-shot is not one of kStageF

    // ext dnm_db: piecewise dBFS-RMS path, flat outside [0,1].
    inline constexpr float kDbF[7] = { 0.0f, 0.30f, 0.52f, 0.70f, 0.80f, 0.90f, 1.0f };
    inline constexpr float kDbV[7] = { -27.0f, -23.0f, -18.5f, -14.5f, -11.5f, -10.5f, -10.5f };

    inline float Db(float f) noexcept
    {
        if (f <= kDbF[0])
            return kDbV[0];
        for (int i = 1; i < 7; ++i) {
            if (f <= kDbF[i]) {
                const float t = (f - kDbF[i - 1]) / (kDbF[i] - kDbF[i - 1]);
                return kDbV[i - 1] + (kDbV[i] - kDbV[i - 1]) * t;
            }
        }
        return kDbV[6];
    }

    // ext dnm_stage_gain: rms-match the stage to the level path, never above vol.
    inline float StageGain(int stage, float f, float rmsDb, float vol) noexcept
    {
        if (stage < 0 || stage >= kStageCount)
            return 0.0f;
        const float g = std::pow(10.0f, (Db(f) - rmsDb) / 20.0f);
        return (g > 1.0f ? 1.0f : g) * vol;
    }

    // ext dnm_stage_of, with an optional caller stage count (the private data asserts 4).
    inline int StageOf(float f, int stages = kStageCount) noexcept
    {
        if (stages < 1)
            return 0;
        if (stages > kStageCount)
            stages = kStageCount;
        int s = 0;
        for (int i = 1; i < stages; ++i)
            if (f >= kStageF[i])
                s = i;
        return s;
    }

    // ext's exponential follow of f: snap at >= 80 ms, else linear in dt.
    inline float SlewFraction(float f, float tgt, float dt) noexcept
    {
        const float step = dt >= kSmoothS ? 1.0f : (dt > 0.0f ? dt / kSmoothS : 0.0f);
        return f + (tgt - f) * step;
    }

    // ext's hit check (strictly greater than 0.06).
    inline bool HitJump(float tgt, float prev) noexcept
    {
        return tgt - prev > kHitStep;
    }

    // ext dnm_duck_tick: 1 = normal, 0.15 = fully ducked.
    inline float Duck(float level) noexcept
    {
        float x = level > 0.0f ? level / kDuckRef : 0.0f;
        if (x > 1.0f)
            x = 1.0f;
        const float d = 1.0f - kDuckDepth * x;
        return d < 1.0f - kDuckDepth ? 1.0f - kDuckDepth : d;
    }

    struct State {
        uint64_t downToken  = 0;       // ext g_down_ms
        uint64_t firstMs    = 0;       // ext g_dnm.first_t0 (music clock for re-starts)
        uint64_t xfT0       = 0;       // ext g_dnm.xf_t0
        uint64_t fadeT0     = 0;       // log-only start of the current cut fade
        uint64_t quickUntil = 0;       // ext g_dnm.quick_until
        uint64_t fT         = 0;       // ext g_dnm.f_t (last fraction sample)

        int      running    = 0;       // -1 armed, 0 off, 1 playing
        int      stage      = 0;       // ext g_dnm.cs
        int      oldStage   = -1;      // ext g_dnm.os
        bool     xf         = false;   // a crossfade is running
        bool     xfLinear   = false;   // loop wrap (identical audio) vs equal power
        int      xfMs       = 0;
        bool     fading     = false;   // a timed cut fade is running
        int      fadeMs     = 0;
        int      switches   = 0;

        float    f          = 0.0f;    // slewed bleed fraction
        float    tgtPrev    = 0.0f;    // previous caller fraction (hit detector)

        bool     dying      = false;  // ext g_dnm.dying: the ending tail is pending
        uint64_t tailAt     = 0;       // ext g_dnm.tail_at
        uint64_t tailT0     = 0;       // ext g_dnm.tail_t0

        bool     duckCapture = false; // ext g_dnm.duck_cap (death-sting baseline held)
        float    duckGl      = 1.0f;   // ext g_dnm.duck_gl
        float    duckGr      = 1.0f;   // ext g_dnm.duck_gr
    };

    struct Input {
        uint64_t nowMs      = 0;
        uint64_t downToken  = 0;       // net::match::downed::DownMs()
        bool     downed     = false;
        bool     dying      = false;   // native downed dying latch (or hp <= 0.5 below)
        bool     exec       = false;   // role 2 / paired move / exec latch
        bool     inPlay     = false;   // ext match_substate() == 6
        float    hp         = 1.0f;    // 1 when unavailable
        float    fraction   = 0.0f;    // ext tgt: BleedFraction(), pause already subtracted
        float    volume     = 0.0f;    // ext DNM_LEVEL * exmus_user_volume()
        double   rate       = 44100.0; // PCM source rate
        int      stages     = kStageCount;
        float    rmsDb[kStageCount]    = {};  // ext DM_RMS_DB
        int      loopFrames[kStageCount] = {}; // ext DM_LEN per stage (0 = unknown)

        bool     haveVoice  = false;   // current stage pair still live
        int      voiceStage = -1;      // stage the current pair was started with
        double   voicePos   = -1.0;    // engine::pcm::Position(left), -1 = stale
        bool     oldHaveVoice = false; // outgoing pair still live
        int      oldVoiceStage = -1;
        bool     mixerLive  = true;    // ext g_mix_state == 1

        bool     tailLive   = false;   // ending-tail pair still live
        float    tailGain   = 0.0f;    // tail left channel's current target gain
        float    tailPeak   = 0.0f;    // peak of the next 80 ms of the tail wave (0..1)
        int      tailFrames = 0;       // ext DM_END_N

        bool     off        = false;   // t2r-no-downed-music.txt: no new start/switch
    };

    struct Output {
        bool     start       = false; // first voice pair of a down
        bool     switchStage = false; // crossfade to a new pair (stage change or wrap)
        bool     cut         = false; // fade/stop the stage pairs
        bool     tail        = false; // start the ending tail
        bool     hold        = false; // push the combat-music hold
        bool     stopTail    = false; // the tail has finished: forget its handles
        bool     killOld     = false; // stop the outgoing pair now (xfade done / replaced)
        bool     hit         = false; // this frame's fraction jumped

        int      stage       = -1;    // new stage, or kEndStage for tail
        int      oldStage    = -1;
        double   offset      = 0.0;   // source-frame offset for start / switch / tail
        int      fadeMs      = 0;
        bool     linear      = false;
        const char* why      = nullptr;

        float    gains[2]    = { 0.0f, 0.0f };    // current pair L, R
        float    oldGains[2] = { 0.0f, 0.0f };    // outgoing pair L, R
        float    duck        = 1.0f;              // 1 = normal, 0.15 = fully ducked
        bool     duckCapture = false;             // call death_sting::SetDuck this frame
        uint64_t holdUntil   = 0;
        uint64_t tailAt      = 0;                 // scheduled beat (log only)
    };

    namespace detail {

        inline int LoopFramesFor(const Input& in, int stage) noexcept
        {
            return stage >= 0 && stage < kStageCount ? in.loopFrames[stage] : 0;
        }

        inline float RmsFor(const Input& in, int stage) noexcept
        {
            return stage >= 0 && stage < kStageCount ? in.rmsDb[stage] : 0.0f;
        }

        inline double Mod(double x, double m) noexcept
        {
            if (!(m > 0.0))
                return 0.0;
            const double r = std::fmod(x, m);
            return r < 0.0 ? r + m : r;
        }

        // ext dnm_live_pair / dnm_live: either the playing or the outgoing pair.
        inline bool PairLive(const State& s, const Input& in) noexcept
        {
            if (!in.mixerLive)
                return false;
            if (in.haveVoice && in.voiceStage == s.stage && in.voicePos >= 0.0)
                return true;
            return s.xf && in.oldHaveVoice && in.oldVoiceStage == s.oldStage;
        }

        inline uint64_t TailHoldMs(const Input& in) noexcept
        {
            if (in.rate <= 0.0 || in.tailFrames <= 0)
                return kHoldMs;
            const double ms = static_cast<double>(in.tailFrames) * 1000.0 / in.rate;
            return static_cast<uint64_t>(ms < 0.0 ? 0.0 : ms) + kHoldMs;
        }

        // ext dnm_reset: forget the voice handles / fades, keep the down clock.
        inline void ResetVoices(State& s) noexcept
        {
            s.oldStage = -1;
            s.running = 0;
            s.xf = false;
            s.fading = false;
            s.fadeT0 = 0;
            s.fadeMs = 0;
        }

        // ext dnm_switch: phase-match the new pair against the playing voice (or
        // the music clock when nothing is live) and start it at zero gain.
        inline void BeginSwitch(State& s, const Input& in, Output& out, int stage,
                                int ms, bool linear, const char* why,
                                bool first) noexcept
        {
            if (s.xf) {
                out.killOld = true;          // a still-running fade is cut at once
                s.xf = false;
            }
            const int prevStage = s.stage;
            double p = (!first && in.haveVoice && in.voiceStage == prevStage)
                     ? in.voicePos : -1.0;
            if (p < 0.0) {
                p = (s.firstMs && in.nowMs >= s.firstMs)
                  ? static_cast<double>(in.nowMs - s.firstMs) / 1000.0 * in.rate
                  : 0.0;
            } else {
                const int curLoop = LoopFramesFor(in, prevStage);
                if (curLoop > 0 && p >= curLoop)
                    p -= curLoop;            // phase inside the loop of the playing stage
            }
            const int newLoop = LoopFramesFor(in, stage);
            double off = newLoop > 0 ? Mod(p, static_cast<double>(newLoop))
                                     : (p > 0.0 ? p : 0.0);

            s.oldStage = first ? -1 : prevStage;
            s.stage = stage;
            s.xf = true;
            s.xfLinear = linear;
            s.xfT0 = in.nowMs;
            s.xfMs = ms;
            ++s.switches;

            out.start = first;
            out.switchStage = true;
            out.stage = stage;
            out.oldStage = s.oldStage;
            out.offset = off;
            out.fadeMs = ms;
            out.linear = linear;
            out.why = why;
            out.gains[0] = out.gains[1] = 0.0f;  // the caller ramps it
        }

        // ext dnm_cut, with its "fading already running" no-op and hold update.
        inline void Cut(State& s, const Input& in, Output& out, int fadeMs,
                        const char* why) noexcept
        {
            s.running = 0;
            if (!PairLive(s, in)) {
                ResetVoices(s);
                return;
            }
            if (s.fading && fadeMs > 0)
                return;
            s.fading = fadeMs > 0;
            s.fadeT0 = in.nowMs;
            s.fadeMs = fadeMs;
            out.cut = true;
            out.fadeMs = fadeMs;
            out.why = why;
            out.hold = true;
            out.holdUntil = in.nowMs + static_cast<uint64_t>(fadeMs < 0 ? 0 : fadeMs)
                          + kHoldMs;
            if (fadeMs <= 0)
                ResetVoices(s);
        }

        inline void DuckTick(State& s, const Input& in, Output& out) noexcept
        {
            if (!in.mixerLive)
                return;                     // ext's early g_mix_state != 1 return
            if (in.tailLive) {
                const float level = (in.tailGain > 0.0f ? in.tailGain : 0.0f)
                                  * (in.tailPeak > 0.0f ? in.tailPeak : 0.0f);
                out.duck = Duck(level);
                out.duckCapture = true;
                s.duckCapture = true;
                return;
            }
            if (s.duckCapture) {
                // Native makes one last pass with duck = 1 to restore the
                // captured death-sting gains, then releases the capture.
                out.duck = 1.0f;
                out.duckCapture = true;
                out.stopTail = true;
                s.duckCapture = false;
                s.tailT0 = 0;
            }
        }

        inline void DueTail(State& s, const Input& in, Output& out) noexcept
        {
            if (!s.dying || !s.tailAt || in.nowMs < s.tailAt)
                return;
            s.tailAt = 0;
            out.tail = true;
            out.stage = kEndStage;
            out.gains[0] = out.gains[1] = kLevel * in.volume;
            Cut(s, in, out, kTailFadeMs, "death - the tail takes over");
            const uint64_t hold = in.nowMs + TailHoldMs(in);
            out.hold = true;
            out.holdUntil = hold;            // the tail rings longer than the stage fade
            if (in.inPlay && in.volume > 0.0f && in.mixerLive)
                s.tailT0 = in.nowMs;
        }

        inline void BadGate(State& s, const Input& in, Output& out) noexcept
        {
            const bool bad = !in.downed || in.dying || in.exec || !in.inPlay;
            if (!bad || s.fading || s.dying)
                return;
            if (s.running == 0 && !PairLive(s, in))
                return;
            if (!in.inPlay) {
                Cut(s, in, out, kMatchFadeMs, "match left the playing state");
                return;
            }
            if (in.exec) {
                Cut(s, in, out, kExecFadeMs, "executed - the execution music takes over");
                return;
            }
            if (in.dying || in.hp <= 0.5f) {
                const bool have = in.haveVoice && in.voiceStage == s.stage;
                const double p = have ? in.voicePos : -1.0;
                const float vol = kLevel * in.volume;
                if (have && p >= 0.0 && vol > 0.0f && in.mixerLive && in.rate > 0.0) {
                    // Ending tail on the NEXT BEAT of the playing stage. The
                    // 20 ms guard keeps a tail scheduled for "now" from firing
                    // before the beat actually lands.
                    const double period = kBeatS * in.rate;
                    const double rem = (period - Mod(p, period)) / in.rate;
                    s.dying = true;
                    s.tailAt = in.nowMs + static_cast<uint64_t>(
                        rem > kTailNearS ? (rem - kHalfGuardS) * 1000.0 : 0.0);
                    out.tailAt = s.tailAt;
                    out.why = "dead - ending tail on the next beat";
                    return;
                }
                Cut(s, in, out, kDeathFadeMs, "dead");
                return;
            }
            Cut(s, in, out, kReviveFadeMs, "revived / up");
        }

    } // namespace detail

    inline Output Tick(State& s, const Input& in) noexcept
    {
        Output out;
        // Native order: duck, delayed tail, bad gates, fade completion, new
        // down, then the fraction / stage machinery.
        detail::DuckTick(s, in, out);
        detail::DueTail(s, in, out);
        detail::BadGate(s, in, out);
        if (s.fading && !detail::PairLive(s, in))
            detail::ResetVoices(s);

        if (!in.downed || !in.downToken) {
            s.downToken = 0;
            return out;
        }
        if (s.downToken != in.downToken) {
            s.downToken = in.downToken;
            s.running = -1;                  // armed: one start per down
            s.f = 0.0f;
            s.fT = in.nowMs;
            s.tgtPrev = 0.0f;
            s.quickUntil = 0;
            s.fading = false;
            s.xf = false;
            s.xfT0 = 0;
            s.dying = false;
            s.tailAt = 0;
            s.tailT0 = 0;
        }

        const bool bad = !in.downed || in.dying || in.exec || !in.inPlay;
        if (bad || in.off || s.running == 0 || s.fading)
            return out;

        float tgt = in.fraction < 0.0f ? 0.0f : (in.fraction > 1.0f ? 1.0f : in.fraction);
        const float dt = s.fT && in.nowMs >= s.fT
                       ? static_cast<float>(in.nowMs - s.fT) / 1000.0f : 0.0f;
        s.fT = in.nowMs;
        if (HitJump(tgt, s.tgtPrev)) {
            s.quickUntil = in.nowMs + kQuickMs;
            out.hit = true;
            out.why = "bleed time cut";
        }
        s.tgtPrev = tgt;
        s.f = SlewFraction(s.f, tgt, dt);

        const float vol = kLevel * in.volume;
        if (s.running == -1) {
            if (in.nowMs < in.downToken || in.nowMs - in.downToken < kStartMs)
                return out;
            if (vol <= 0.0f || !in.mixerLive) {
                s.running = 0;
                out.why = "skipped (options music volume 0 / mixer off)";
                return out;
            }
            s.f = tgt;                       // joined late: begin at the real fraction
            s.firstMs = in.nowMs;
            s.xf = false;                    // nothing is playing yet
            detail::BeginSwitch(s, in, out, StageOf(tgt, in.stages),
                                static_cast<int>(kStartFadeMs), false, "start", true);
            s.running = 1;
            return out;
        }

        out.hold = true;                     // keep the combat music cut every frame
        out.holdUntil = in.nowMs + kHoldMs;

        if (s.xf) {
            const float denom = s.xfMs > 0 ? static_cast<float>(s.xfMs) : 1.0f;
            float t = in.nowMs >= s.xfT0
                    ? static_cast<float>(in.nowMs - s.xfT0) / denom : 0.0f;
            if (t < 0.0f)
                t = 0.0f;
            const float gi = StageGain(s.stage, s.f, detail::RmsFor(in, s.stage), vol);
            const float go = (s.oldStage >= 0 && in.oldHaveVoice)
                           ? StageGain(s.oldStage, s.f, detail::RmsFor(in, s.oldStage), vol)
                           : 0.0f;
            if (t >= 1.0f) {
                out.killOld = true;          // the outgoing pair is done
                s.xf = false;
                s.oldStage = -1;
                out.gains[0] = out.gains[1] = gi;
            } else {
                const float ci = s.xfLinear ? t : std::sin(t * 1.5707963f);
                const float co = s.xfLinear ? 1.0f - t : std::cos(t * 1.5707963f);
                out.gains[0] = out.gains[1] = gi * ci;
                out.oldGains[0] = out.oldGains[1] = go * co;
            }
            return out;
        }

        out.gains[0] = out.gains[1] =
            StageGain(s.stage, s.f, detail::RmsFor(in, s.stage), vol);

        const bool have = in.haveVoice && in.voiceStage == s.stage && in.voicePos >= 0.0;
        if (!have) {                         // the voice ended / its slot was taken
            detail::BeginSwitch(s, in, out, StageOf(s.f, in.stages),
                                static_cast<int>(kStartFadeMs), false,
                                "voice ended", true);
            return out;
        }
        const int loop = detail::LoopFramesFor(in, s.stage);
        if (loop > 0 && in.voicePos >= loop) {
            detail::BeginSwitch(s, in, out, s.stage, static_cast<int>(kWrapMs),
                                true, "loop", false);
            return out;
        }
        if (in.rate <= 0.0)
            return out;

        const bool quick = in.nowMs < s.quickUntil;
        int want = StageOf(tgt + (quick ? 0.0f : kLook), in.stages);
        if (!quick && want > s.stage + 1)
            want = s.stage + 1;              // at most one stage per bar line
        if (want != s.stage) {
            const int ms = quick ? static_cast<int>(kXfHitMs)
                                 : static_cast<int>(kXfMs);
            const double period = (quick ? kBeatS : kBarS) * in.rate;
            const double phase = detail::Mod(in.voicePos, period);
            const double rem = (period - phase) / in.rate;
            const double half = static_cast<double>(ms) / 2000.0 + kHalfGuardS;
            if (rem <= half || phase / in.rate <= kPhaseGuardS) {
                detail::BeginSwitch(s, in, out, want, ms, false,
                                    quick ? "stage change (beat-synced)"
                                          : "stage change (bar-synced)", false);
            }
        }
        return out;
    }

    inline void Reset(State& s) noexcept { s = State{}; }

    inline bool Active(const State& s, uint64_t now) noexcept
    {
        if (s.duckCapture || s.fading || s.dying || (s.tailAt && s.tailAt >= now))
            return true;
        if (s.downToken && s.running == -1)
            return true;
        return s.running == 1;
    }

} // namespace net::audio::downed_music::core
