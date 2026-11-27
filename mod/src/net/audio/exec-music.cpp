// ported from tools/ext/exec_music.c. All timing / hold decisions live in
// exec-music-core.hpp; the engine, PCM and music-hold seams stay in this owner.

#include "exec-music.hpp"

#include "exec-music-pcm.hpp"

#include "boot.hpp"
#include "engine/pcm.hpp"
#include "log.hpp"
#include "music-hold.hpp"

#include <cstdint>

namespace net::audio::exec_music {

    namespace {

        namespace sink = net::audio::exec_music::pcm;

        core::State g_state;
        engine::pcm::Voice g_l, g_r;       // live cue pair (Cut clears the handles)
        engine::pcm::Voice g_fadeL, g_fadeR; // copies kept while the ext's fade rings
        bool g_noDataLogged = false;

        bool ExtOwns() noexcept { return boot::ExtLoaded(); }

        // ext exmus_live: g_mix_state == 1 and either slot still on with OUR src.
        // engine::pcm::Live validates both the slot and the source, and the fade
        // copies keep the old src visible for the 300 ms the mixer is fading it.
        bool CueLive() noexcept
        {
            return engine::pcm::Live(g_l) || engine::pcm::Live(g_r)
                || engine::pcm::Live(g_fadeL) || engine::pcm::Live(g_fadeR);
        }

        void ForgetAll() noexcept
        {
            engine::pcm::Invalidate(g_l);
            engine::pcm::Invalidate(g_r);
            engine::pcm::Invalidate(g_fadeL);
            engine::pcm::Invalidate(g_fadeR);
        }

        void HardCutAll() noexcept
        {
            engine::pcm::Cut(g_l, 0);
            engine::pcm::Cut(g_r, 0);
            engine::pcm::Cut(g_fadeL, 0);
            engine::pcm::Cut(g_fadeR, 0);
            ForgetAll();
        }

        // The ext kept a/b and their src through a fade so its live check saw the
        // ringing voices; native Cut() clears handles, so keep a copy first.
        void KeepFading() noexcept
        {
            g_fadeL = g_l;
            g_fadeR = g_r;
        }

        void LogNoDataOnce()
        {
            if (g_noDataLogged)
                return;
            g_noDataLogged = true;
            TO1_LOG("exec music: no private PCM data (TO1_EXEC_MUSIC_PCM off) - cue stays silent\n");
        }

        void ApplyFade(const core::FadeOutput& f, const char* why)
        {
            if (!f.acted)
                return;
            music_hold::Hold(f.holdUntil);
            KeepFading();
            engine::pcm::Cut(g_l, static_cast<int>(core::kFadeMs));
            engine::pcm::Cut(g_r, static_cast<int>(core::kFadeMs));
            engine::pcm::Invalidate(g_l);
            engine::pcm::Invalidate(g_r);
            (void)why;                         // log-only when TO1_DEBUG is off
            (void)f.elapsedMs;
            TO1_LOG("exec music: %s - fade-out %u ms (%llu ms into it)\n",
                    why ? why : "?", static_cast<unsigned>(core::kFadeMs),
                    static_cast<unsigned long long>(f.elapsedMs));
        }

        // ext execmus_play: one cue at a time, a raw hard-panned pair started at
        // seek * rate, music volume already folded into the gain by the core.
        bool StartCue(const core::Output& out)
        {
            if (out.cutOld)
                HardCutAll();                  // old pair (or ringing fade) is cut first
            ForgetAll();                       // ext a = b = -1 before the play attempt

            if (out.noVolume) {
                TO1_LOG("exec music: cue %d skipped (options music volume 0)\n", out.cue);
                return false;
            }
            if constexpr (!sink::kHave) {
                LogNoDataOnce();
                return false;
            }
            const engine::pcm::Sample* l = sink::Left(out.cue);
            const engine::pcm::Sample* r = sink::Right(out.cue);
            const int frames = sink::Frames();
            const int rate = sink::Rate();
            if (!l || !r || frames < 2 || rate <= 0) {
                LogNoDataOnce();
                return false;
            }
            if (out.mixerOff) {
                TO1_LOG("exec music: mixer off\n");
                return false;
            }

            const double startFrame = core::StartFrame(out.seek, rate);
            engine::pcm::Voice nl, nr;
            const bool ok = engine::pcm::PlayStereoRawAt(l, r, frames,
                                                         out.gain, out.gain, rate,
                                                         startFrame, nl, nr);
            if (!ok) {
                TO1_LOG("exec music: mixer off\n");
                return false;
            }
            g_l = nl;
            g_r = nr;
            TO1_LOG("exec music: victim cue %d (beg-for-life set) gain %.2f - %s\n",
                    out.cue, static_cast<double>(out.gain), "playing");
            return true;
        }

    } // namespace

    void Start(uint64_t nowMs, float killS, int pick)
    {
        if (ExtOwns())
            return;
        const uint64_t hold = core::Start(g_state, nowMs, killS, pick,
                                          music_hold::HoldUntil());
        music_hold::Hold(hold);
        const double lead = core::Lead(killS, g_state.pick);
        (void)lead;                            // log-only when TO1_DEBUG is off
        TO1_LOG("exec music: cue %d hit %.2f s on the death screen: starts in %.2f s "
                "(kill %.2f s)%s\n",
                g_state.pick, static_cast<double>(core::kHit[g_state.pick]),
                lead > 0.0 ? lead : 0.0, static_cast<double>(killS),
                lead < 0.0 ? " - joined mid-cue" : "");
    }

    void Tick(uint64_t nowMs, bool inPlayState)
    {
        if (ExtOwns())
            return;

        core::Input in;
        in.nowMs     = nowMs;
        in.inPlay    = inPlayState;
        in.cueLive   = CueLive();
        in.volume    = engine::pcm::MusicUserVolume();
        // ext pcm_play_voice starts the mixer on the cue's first frame. Read the
        // same engine::pcm state here, only when a cue is actually due and loud
        // enough to play; engine::pcm::Live() below folds g_mix_state into the
        // cue-live answer on every other frame. A failed start is reported by
        // the core's mixerOff / a failed PlayStereoRawAt.
        const bool due = inPlayState && g_state.pendingAt != 0
                      && nowMs >= g_state.pendingAt;
        in.mixerLive = sink::kHave
                    && (!due || in.volume <= 0.0f || engine::pcm::Start());
        in.holdUntil = music_hold::HoldUntil();

        const core::Output out = core::Tick(g_state, in);
        if (out.startCue) {
            if (out.musicCut)
                (void)music_hold::Play("music-playlist-rogue-player-death",
                                       "execution victim: combat music cut on the cue's first sound");
            if (out.hold)
                music_hold::Hold(out.holdUntil);
            core::CommitStart(g_state, nowMs, StartCue(out));
            return;                            // ext tick's `if (a < 0) return`
        }
        if (out.cutCue) {
            core::FadeOutput f;
            f.acted     = true;
            f.holdUntil = out.holdUntil;
            f.elapsedMs = out.elapsedMs;
            ApplyFade(f, out.why);
            return;
        }
        if (out.stopped) {
            if (out.hold)
                music_hold::Hold(out.holdUntil);
            HardCutAll();                      // the mixer let it finish
            return;
        }
        if (out.hold)
            music_hold::Hold(out.holdUntil);
    }

    void Fade(uint64_t nowMs, const char* why)
    {
        if (ExtOwns())
            return;
        ApplyFade(core::Fade(g_state, nowMs), why);
    }

    void SetDuck(float duck) noexcept
    {
        if (ExtOwns())
            return;
        // ext eb_music_duck: no duck while fading, or once the cue is gone.
        if (!g_state.haveVoice || g_state.fading) {
            core::ClearDuck(g_state.duck);
            return;
        }
        const bool live[2] = { engine::pcm::Live(g_l), engine::pcm::Live(g_r) };
        if (!live[0] && !live[1]) {
            core::ClearDuck(g_state.duck);
            return;
        }
        float raw[2][2] = {};
        (void)engine::pcm::RawGains(g_l, raw[0][0], raw[0][1]);
        (void)engine::pcm::RawGains(g_r, raw[1][0], raw[1][1]);
        const core::DuckOutput out = core::DuckStep(g_state.duck, duck, raw, live);
        if (!out.apply)
            return;
        if (live[0])
            engine::pcm::SetTargetGainRaw(g_l, out.gains[0][0], out.gains[0][1]);
        if (live[1])
            engine::pcm::SetTargetGainRaw(g_r, out.gains[1][0], out.gains[1][1]);
        const float gain = kLevel * engine::pcm::MusicUserVolume()
                         * (g_state.duck.value < 1.0f ? g_state.duck.value : 1.0f);
        (void)gain;                            // log-only when TO1_DEBUG is off
        TO1_LOG("exec music: %s (gain %.2f)\n",
                g_state.duck.value < 1.0f ? "ducked under the plea" : "back up",
                static_cast<double>(gain));
    }

    bool Pending(uint64_t nowMs) noexcept
    {
        if (ExtOwns())
            return false;
        return g_state.pendingAt != 0 && nowMs >= g_state.pendingAt;
    }

    bool Playing() noexcept
    {
        if (ExtOwns())
            return false;
        return g_state.haveVoice;
    }

    bool Fading() noexcept
    {
        if (ExtOwns())
            return false;
        return g_state.fading;
    }

    uint64_t PendingAt() noexcept
    {
        if (ExtOwns())
            return 0;
        return g_state.pendingAt;
    }

    uint64_t StartedAt() noexcept
    {
        if (ExtOwns())
            return 0;
        return g_state.t0;
    }

    float Duck() noexcept
    {
        if (ExtOwns())
            return 1.0f;
        return g_state.duck.value;
    }

    int Cue() noexcept
    {
        if (ExtOwns())
            return -1;
        return g_state.pick;
    }

    void Reset()
    {
        if (ExtOwns())
            return;
        core::Reset(g_state);
        HardCutAll();
        TO1_LOG("exec music: reset\n");
    }

} // namespace net::audio::exec_music
