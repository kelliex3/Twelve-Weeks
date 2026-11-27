#pragma once

// Pure execution-victim music rules (tools/ext/exec_music.c), with no engine,
// audio, net or window headers and no clock: the native owner in exec-music.cpp
// supplies nowMs, the in-play state, cue / mixer liveness, the music volume and
// the combat-music hold. Kept exactly:
//   * the hit second of each of five cues lands on the death screen (83 + 70 ms
//     after the move starts), with a phase-matched mid-cue seek on late joins;
//   * a due pending cue is consumed before the match check, so it never starts
//     outside the playing state, and a fade / abort cancels it permanently;
//   * one cue at a time, a hard 12500 ms cap, a 300 ms fade, and a 1500 ms
//     resume hold on fade / end / match-left;
//   * raw target-gain capture / restore for the 0.35 duck under the plea
//     (ext exec_beg.c's eb_music_duck).
// Tests: mod/tests/exec_music_core_test.cpp.

#include <cstdint>

namespace net::audio::exec_music::core {

    inline constexpr float    kLevel        = 0.85f;  // ext EXMUS_LEVEL
    inline constexpr uint64_t kCapMs        = 12500;  // ext EXMUS_CAP_MS
    inline constexpr uint64_t kFadeMs       = 300;    // ext EXMUS_FADE_MS
    inline constexpr uint64_t kResumeMs     = 1500;   // ext EXMUS_RESUME_MS
    inline constexpr uint64_t kDeathDelayMs = 83 + 70;// death_ui delay + cut
    inline constexpr int      kCues         = 5;      // ext EXMUS_CUES
    inline constexpr float    kHit[kCues]   = { 2.44f, 2.35f, 2.31f, 2.35f, 2.39f };
    static_assert(sizeof kHit / sizeof kHit[0] == kCues, "kHit: one timing per cue");

    inline int ClampPick(int pick) noexcept
    {
        return pick >= 0 && pick < kCues ? pick : 0;
    }

    // ext: land = kill_s + (83 + 70) / 1000.0; lead = land - EXMUS_HIT[pick].
    inline double Lead(float killS, int pick) noexcept
    {
        return static_cast<double>(killS)
             + static_cast<double>(kDeathDelayMs) / 1000.0
             - static_cast<double>(kHit[ClampPick(pick)]);
    }

    // ext execmus_play: g_mixv[].pos = seek_s * EXMUS_RATE.
    inline double StartFrame(double seek, int rate) noexcept
    {
        return rate > 0 ? seek * static_cast<double>(rate) : 0.0;
    }

    // ---- duck (pure part of exec_beg.c's eb_music_duck) -------------------------

    struct Duck {
        bool  captured = false;
        float value    = 1.0f;                       // last request (1 = normal)
        float base[2][2] = { { 1.0f, 0.0f }, { 0.0f, 1.0f } }; // raw target gains per voice
    };

    inline void ClearDuck(Duck& d) noexcept { d = Duck{}; }

    struct DuckOutput {
        bool  apply = false;
        float gains[2][2] = {};                      // target raw gains for both voices
    };

    // On the first non-unity request capture each live voice's current RAW target
    // gains, then scale both channels. On 1.0 restore the captured base and clear
    // the capture. With no live voice the call is a no-op and forgets any capture.
    // The owner checks cue liveness / fading and calls RawGains / SetTargetGainRaw.
    inline DuckOutput DuckStep(Duck& d, float duck,
                               const float raw[2][2], const bool live[2]) noexcept
    {
        DuckOutput out;
        if (duck < 0.0f) duck = 0.0f;
        if (duck > 1.0f) duck = 1.0f;
        d.value = duck;
        if (!live[0] && !live[1]) {
            ClearDuck(d);
            return out;
        }
        if (!d.captured && duck < 1.0f) {
            for (int k = 0; k < 2; ++k)
                for (int c = 0; c < 2; ++c)
                    if (live[k])
                        d.base[k][c] = raw[k][c];
            d.captured = true;
        }
        if (!d.captured)
            return out;
        for (int k = 0; k < 2; ++k) {
            if (!live[k])
                continue;
            for (int c = 0; c < 2; ++c)
                out.gains[k][c] = d.base[k][c] * duck;
        }
        out.apply = true;
        if (duck >= 1.0f)
            ClearDuck(d);
        return out;
    }

    // ---- pending cue / voice state ---------------------------------------------

    struct State {
        uint64_t pendingAt  = 0;      // ext g_exmus_pend.at (0 = none)
        uint64_t t0         = 0;      // ext g_exmus.t0, a successful cue's start
        int      pick       = -1;     // pending / current cue, from the host's draw
        double   seek       = 0.0;    // ext g_exmus_pend.seek, seconds into the cue
        bool     pendingCut = false;  // ext g_exmus_pend.cut, decided at Start
        bool     haveVoice  = false;  // ext g_exmus.a >= 0 (a pair was started)
        bool     fading     = false;  // ext g_exmus.fading
        Duck     duck;
    };

    struct Input {
        uint64_t nowMs     = 0;
        bool     inPlay    = false;  // ext match_substate() == 6
        bool     cueLive   = false;  // owner: engine::pcm::Live on the cue pair
        bool     mixerLive = true;   // owner: ext g_mix_state == 1
        float    volume    = 1.0f;   // engine::pcm::MusicUserVolume()
        uint64_t holdUntil = 0;      // music_hold::HoldUntil(), read at Start
    };

    struct Output {
        bool     startCue     = false;  // ext execmus_play this tick
        bool     cutOld       = false;  // hard-cut the previous pair first
        bool     clearVoices  = false;  // ext a = b = -1
        bool     musicCut     = false;  // pendingCut: cut combat music on first sound
        bool     hold         = false;  // push the music hold / resume
        uint64_t holdUntil    = 0;
        bool     cutCue       = false;  // fade the live pair
        int      fadeMs       = 0;
        bool     stopped      = false;  // the mixer let the cue finish: forget + resume
        int      cue          = -1;
        double   seek         = 0.0;
        float    gain         = 0.0f;   // ext EXMUS_LEVEL * exmus_user_volume()
        bool     noVolume     = false;  // options music volume 0
        bool     mixerOff     = false;  // ext g_mix_state != 1
        const char* why       = nullptr;
        uint64_t elapsedMs    = 0;      // log-only: now - t0 at the fade
    };

    struct FadeOutput {
        bool     acted     = false;    // false = pending cancelled, nothing live
        uint64_t holdUntil = 0;
        uint64_t elapsedMs = 0;
    };

    inline void Reset(State& s) noexcept { s = State{}; }

    // ext execmus_start: place the pick's hit second on the death screen and
    // decide the combat-music cut NOW (the hold set here would block the check at
    // play time). Returns the new hold / resume deadline.
    inline uint64_t Start(State& s, uint64_t nowMs, float killS, int pick,
                          uint64_t holdUntil) noexcept
    {
        const int p = ClampPick(pick);
        const double lead = Lead(killS, p);
        const uint64_t delay = lead > 0.0 ? static_cast<uint64_t>(lead * 1000.0) : 0ULL;
        s.pick       = p;
        s.pendingAt  = nowMs + delay;
        s.seek       = lead < 0.0 ? -lead : 0.0;
        s.pendingCut = nowMs >= holdUntil;   // ext: NOT music_hold::BlocksDown(now)
        return s.pendingAt + kCapMs + kResumeMs;
    }

    // The owner reports whether the PCM pair actually started; ext set a/b = -1
    // again and left t0 at 0 when the volume was 0, the mixer was off or voices
    // were busy.
    inline void CommitStart(State& s, uint64_t nowMs, bool started) noexcept
    {
        s.fading    = false;
        s.haveVoice = started;
        if (started)
            s.t0 = nowMs;                     // ext sets t0 only for a started pair
        else
            ClearDuck(s.duck);
    }

    // ext execmus_fade: cancel any pending cue first; only a live, unfaded cue
    // gets the hold and the 300 ms fade.
    inline FadeOutput Fade(State& s, uint64_t nowMs) noexcept
    {
        FadeOutput out;
        s.pendingAt = 0;
        ClearDuck(s.duck);
        if (!s.haveVoice || s.fading)
            return out;
        out.holdUntil = nowMs + kFadeMs + kResumeMs;
        out.elapsedMs = nowMs >= s.t0 ? nowMs - s.t0 : 0;
        out.acted = true;
        s.fading = true;
        return out;
    }

    // ext execmus_tick's exact order: due pending start (in play only), then the
    // live check with its resume hold, then cap before match-left.
    inline Output Tick(State& s, const Input& in) noexcept
    {
        Output out;
        const uint64_t now = in.nowMs;

        if (s.pendingAt != 0 && now >= s.pendingAt) {
            s.pendingAt = 0;                 // consumed even when not in play
            if (in.inPlay) {
                out.startCue    = true;
                out.cue         = s.pick;
                out.seek        = s.seek;
                out.gain        = in.volume > 0.0f ? kLevel * in.volume : 0.0f;
                out.noVolume    = !(in.volume > 0.0f);
                out.mixerOff    = !in.mixerLive;
                out.musicCut    = s.pendingCut;
                out.cutOld      = in.cueLive;
                out.clearVoices = true;
                out.hold        = true;
                out.holdUntil   = now + kCapMs + kResumeMs;
                s.haveVoice = false;         // ext a = b = -1 before the play attempt
                s.fading    = false;         // t0 stays until a new cue actually plays
                ClearDuck(s.duck);
                return out;                  // ext tick's `if (a < 0) return`
            }
        }

        if (!s.haveVoice)
            return out;

        if (!in.mixerLive || !in.cueLive) {
            s.haveVoice = false;             // ext a = b = -1; t0 is log history
            ClearDuck(s.duck);
            out.stopped     = true;
            out.clearVoices = true;
            out.hold        = true;
            out.holdUntil   = now + kResumeMs;
            return out;
        }

        if (now >= s.t0 && now - s.t0 >= kCapMs) {
            const FadeOutput f = Fade(s, now);
            if (f.acted) {
                out.cutCue    = true;
                out.fadeMs    = static_cast<int>(kFadeMs);
                out.hold      = true;
                out.holdUntil = f.holdUntil;
                out.why       = "cap reached";
                out.elapsedMs = f.elapsedMs;
            }
        } else if (!in.inPlay) {
            const FadeOutput f = Fade(s, now);
            if (f.acted) {
                out.cutCue    = true;
                out.fadeMs    = static_cast<int>(kFadeMs);
                out.hold      = true;
                out.holdUntil = f.holdUntil;
                out.why       = "match left the playing state";
                out.elapsedMs = f.elapsedMs;
            }
        }
        return out;
    }

} // namespace net::audio::exec_music::core
