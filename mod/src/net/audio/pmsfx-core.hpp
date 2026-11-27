#pragma once

// Pure post-match score-sound rules (tools/ext/pmsfx.c plus its call sites in
// t2r-ext.c's pm_tick): the enter / count / count-land / stamp / next-page cues,
// the count duration from the awarded parts, and the one-shot guards that keep
// the rolling count loop from starting twice. No engine, PCM, GUI, sidecar or
// private-data header is included here; the native owner in pmsfx.cpp supplies
// the page edges and the guarded PCM sink. Tests: mod/tests/pmsfx_core_test.cpp.

#include <cstdint>

namespace net::audio::pmsfx::core {

    enum class CueKind : int { kNone = 0, kPlay, kCountStart, kCountCut };

    enum class Kind : int { kEnter = 0, kCount, kEnd, kStamp, kPage, kCountKinds = 5 };

    // ext PMS_* gains: the ripped waves were peak-normalised, so the old
    // 0.35-0.45 levels were far above the game's own bus.
    inline constexpr float kGains[5] = { 0.16f, 0.13f, 0.15f, 0.18f, 0.16f };
    inline constexpr float kCountGain = 0.13f;

    inline constexpr int kCntAt = 2200;      // ext PM_CNT_AT
    inline constexpr int kCountMinMs = 100;  // ext's 4800 samples at 48 kHz
    inline constexpr int kTailFadeMs = 60;   // ext's 2880 samples at 48 kHz

    struct Cue {
        CueKind kind = CueKind::kNone;
        int     cue  = 0;     // Kind for kPlay, unused otherwise
        int     ms   = 0;     // kCountStart duration / kCountCut fade
    };

    struct Result {
        Cue   cues[4];
        int   count = 0;

        void Add(CueKind kind, int cue, int ms) noexcept
        {
            if (count < 4) {
                cues[count] = { kind, cue, ms };
                ++count;
            }
        }
        void Play(Kind kind) noexcept { Add(CueKind::kPlay, static_cast<int>(kind), 0); }
    };

    struct State {
        bool armed = false;         // a score page is up
        bool countStarted = false;  // rolling loop started once
        bool skipped = false;       // the select skip jumped past the count
        bool finished = false;      // the count landed / the total was stamped
    };

    inline void Reset(State& s) noexcept { s = State{}; }

    // ext cdur = clamp(parts * 15, 700, 1800).
    inline int CountDur(int parts) noexcept
    {
        int ms = parts * 15;
        if (ms < 700) ms = 700;
        if (ms > 1800) ms = 1800;
        return ms;
    }

    // ext pms_count_start: ms of loop plus a 100 ms lead, clamped into the wave
    // and never shorter than 100 ms. The owner copies that many frames.
    inline int CountSamples(int ms, int rate, int maxFrames) noexcept
    {
        if (ms < 0) ms = 0;
        int n = ms * (rate / 1000) + rate / 10;
        if (n > maxFrames) n = maxFrames;
        if (n < rate / 10) n = rate / 10;
        return n;
    }

    // ext's linear tail fade, k = min(n, rate * 60 / 1000) frames.
    inline int TailFrames(int n, int rate) noexcept
    {
        int k = rate * kTailFadeMs / 1000;
        if (k < 1) k = 1;
        return n < k ? n : k;
    }

    inline void Enter(State& s, Result& out) noexcept
    {
        Reset(s);
        s.armed = true;
        out.Play(Kind::kEnter);
    }

    // ext pms_count_cut(50) on the select skip: jump to the finished state and
    // fade whatever loop is playing.
    inline void Skip(State& s, Result& out) noexcept
    {
        if (!s.armed || s.finished || s.skipped)
            return;
        s.skipped = true;
        out.Add(CueKind::kCountCut, 0, 50);
    }

    // ext's once-only count loop start, between PM_CNT_AT and PM_CNT_AT+cdur.
    inline void Tick(State& s, Result& out, bool ready, int64_t elapsedMs,
                     int countDurMs) noexcept
    {
        if (!s.armed || s.countStarted || s.skipped || !ready)
            return;
        if (elapsedMs < kCntAt || elapsedMs >= kCntAt + countDurMs)
            return;
        s.countStarted = true;
        out.Add(CueKind::kCountStart, 0, countDurMs);
    }

    // ext pms_count_cut(60) + PMSK_END + PMSK_STAMP when the count lands.
    inline void Finish(State& s, Result& out) noexcept
    {
        if (!s.armed || s.finished)
            return;
        s.finished = true;
        out.Add(CueKind::kCountCut, 0, 60);
        out.Play(Kind::kEnd);
        out.Play(Kind::kStamp);
    }

    inline void Page(Result& out) noexcept { out.Play(Kind::kPage); }

} // namespace net::audio::pmsfx::core
