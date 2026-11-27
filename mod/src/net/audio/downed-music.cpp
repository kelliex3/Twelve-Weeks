// Ported from tools/ext/downed_music.c. All rules live in downed-music-core.hpp;
// the engine / audio / net seams stay here. No new network message.

#include "downed-music.hpp"

#include "downed-music-pcm.hpp"

#include "boot.hpp"
#include "engine/flag.hpp"
#include "engine/local-player.hpp"
#include "engine/pcm.hpp"
#include "log.hpp"
#include "music-hold.hpp"
#include "net/anim/paired.hpp"
#include "net/game/player-status.hpp"
#include "net/match/downed-state.hpp"
#include "net/ui/death-sting.hpp"

#include <windows.h>

#include <cstdint>

namespace net::audio::downed_music {

    namespace {

        namespace core = net::audio::downed_music::core;
        namespace sink = net::audio::downed_music::pcm;

        constexpr char kOffFlag[] = "t2r-no-downed-music.txt";

        core::State g_state;
        engine::pcm::Voice g_curL, g_curR;    // playing stage pair
        engine::pcm::Voice g_oldL, g_oldR;    // pair fading out
        engine::pcm::Voice g_fadeL, g_fadeR;  // copies kept only while a cut fade rings
        engine::pcm::Voice g_fadeOldL, g_fadeOldR;
        engine::pcm::Voice g_tailL, g_tailR;  // ending tail pair
        int      g_curStage = -1;
        int      g_oldStage = -1;
        uint64_t g_execUntil = 0;

        bool ExtOwns() noexcept { return boot::ExtLoaded(); }
        bool Off() { return engine::flag::Present(kOffFlag, 120); }

        bool PairLive(const engine::pcm::Voice& l, const engine::pcm::Voice& r) noexcept
        {
            return engine::pcm::Live(l) && engine::pcm::Live(r);
        }

        void CutPair(engine::pcm::Voice& l, engine::pcm::Voice& r, int ms)
        {
            engine::pcm::Cut(l, ms);
            engine::pcm::Cut(r, ms);
            engine::pcm::Invalidate(l);
            engine::pcm::Invalidate(r);
        }

        bool StartStage(int stage, double offset, float gain,
                        engine::pcm::Voice& l, engine::pcm::Voice& r)
        {
            if constexpr (!sink::kHave)
                return false;
            if (stage < 0 || stage >= core::kStageCount)
                return false;
            const engine::pcm::Sample* sl = sink::StageL(stage);
            const engine::pcm::Sample* sr = sink::StageR(stage);
            const int frames = sink::StageVoiceFrames(stage);
            if (!sl || !sr || frames < 2)
                return false;
            // Native starts both hard-panned voices at zero gain; the caller
            // ramps the target gains on the next frame's crossfade step.
            return engine::pcm::PlayStereoRawAt(sl, sr, frames, gain, gain,
                                                sink::Rate(), offset, l, r);
        }

        bool StartTail(float gain)
        {
            if constexpr (!sink::kHave)
                return false;
            const engine::pcm::Sample* l = sink::EndL();
            const engine::pcm::Sample* r = sink::EndR();
            const int frames = sink::EndFrames();
            if (!l || !r || frames < 2 || gain <= 0.0f)
                return false;
            return engine::pcm::PlayStereoRawAt(l, r, frames, gain, gain,
                                                sink::Rate(), 0.0, g_tailL, g_tailR);
        }

        // ext dnm_gain: only the left voice's L target and the right voice's R
        // target are driven; the other two stay at the hard-pan zero they started with.
        void SetPairGains(engine::pcm::Voice& l, engine::pcm::Voice& r,
                          const float gains[2])
        {
            engine::pcm::SetTargetGainRaw(l, gains[0], 0.0f);
            engine::pcm::SetTargetGainRaw(r, 0.0f, gains[1]);
        }

        // ext dnm_duck_tick's 80 ms next-window peak of the ending tail.
        float TailPeak(double pos)
        {
            if constexpr (!sink::kHave)
                return 0.0f;
            const int frames = sink::EndFrames();
            const int rate = sink::Rate();
            const engine::pcm::Sample* wave = sink::EndL();
            if (!wave || frames < 2 || rate <= 0)
                return 0.0f;
            int p0 = pos > 0.0 && pos < static_cast<double>(frames)
                   ? static_cast<int>(pos) : 0;
            if (p0 < 0)
                p0 = 0;
            int p1 = p0 + rate / 12;
            if (p1 > frames)
                p1 = frames;
            int peak = 0;
            for (int i = p0; i < p1; i += 4) {
                int x = wave[i];
                if (x < 0)
                    x = -x;
                if (x > peak)
                    peak = x;
            }
            return static_cast<float>(peak) / 32768.0f;
        }

        // engine::pcm::Cut clears the caller's handle but leaves the slot fading
        // until it ends. Keep a copy so the core can still see the native
        // dnm_live() window before its fade-completion reset.
        void KeepFading(const engine::pcm::Voice& l, const engine::pcm::Voice& r,
                        bool outgoing)
        {
            if (outgoing) {
                g_fadeOldL = l;
                g_fadeOldR = r;
            } else {
                g_fadeL = l;
                g_fadeR = r;
            }
        }

        void ClearAll()
        {
            CutPair(g_curL, g_curR, 0);
            CutPair(g_oldL, g_oldR, 0);
            CutPair(g_fadeL, g_fadeR, 0);
            CutPair(g_fadeOldL, g_fadeOldR, 0);
            CutPair(g_tailL, g_tailR, 0);
            g_curStage = g_oldStage = -1;
            g_execUntil = 0;
        }

    } // namespace

    void Tick(uint64_t nowMs)
    {
        if (ExtOwns())
            return;

        const bool downed = net::match::downed::IsDown();
        const bool dying = net::match::downed::LocalDying();
        const bool execNow = net::anim::paired::Get().role == 2
                          || net::player_status::LocalInPairedMove();
        if (execNow)
            g_execUntil = nowMs + core::kHoldMs;  // ext g_exec_until / g_special_exec_until
        const bool exec = execNow || nowMs < g_execUntil;

        float hp = engine::local_player::Health();
        if (hp < 0.0f)
            hp = 1.0f;                       // no player object: "unavailable"

        core::Input in;
        in.nowMs       = nowMs;
        in.downToken   = net::match::downed::DownMs();
        in.downed      = downed;
        in.dying       = dying;
        in.exec        = exec;
        in.inPlay      = net::player_status::MatchLive();
        in.hp          = hp;
        in.fraction    = net::match::downed::BleedFraction();
        in.volume      = sink::kHave ? engine::pcm::MusicUserVolume() : 0.0f;
        in.rate        = static_cast<double>(sink::Rate());
        in.stages      = core::kStageCount;
        for (int s = 0; s < core::kStageCount; ++s) {
            in.rmsDb[s] = sink::RmsDb(s);
            in.loopFrames[s] = sink::StageLoopFrames(s);
        }
        const bool curLive = PairLive(g_curL, g_curR);
        const bool fadeCur = PairLive(g_fadeL, g_fadeR);
        const bool fadeOut = PairLive(g_fadeOldL, g_fadeOldR);
        in.haveVoice   = curLive || fadeCur;
        in.voiceStage  = curLive ? g_curStage : g_state.stage;
        in.voicePos    = curLive ? engine::pcm::Position(g_curL)
                       : fadeCur ? engine::pcm::Position(g_fadeL) : -1.0;
        in.oldHaveVoice = PairLive(g_oldL, g_oldR) || fadeOut;
        in.oldVoiceStage = PairLive(g_oldL, g_oldR) ? g_oldStage : g_state.oldStage;
        in.mixerLive   = sink::kHave;
        in.tailLive    = PairLive(g_tailL, g_tailR);
        if (in.tailLive) {
            float gl = 0.0f, gr = 0.0f;
            if (engine::pcm::RawGains(g_tailL, gl, gr))
                in.tailGain = gl;
            in.tailPeak = TailPeak(engine::pcm::Position(g_tailL));
        }
        in.tailFrames  = sink::EndFrames();
        in.off         = Off();

        const core::Output out = core::Tick(g_state, in);

        if (out.cut) {
            if (out.fadeMs > 0) {
                KeepFading(g_curL, g_curR, false);
                KeepFading(g_oldL, g_oldR, true);
            }
            CutPair(g_curL, g_curR, out.fadeMs);
            CutPair(g_oldL, g_oldR, out.fadeMs);
            g_curStage = g_oldStage = -1;
        } else if (out.killOld) {
            CutPair(g_oldL, g_oldR, 0);
            g_oldStage = -1;
        }

        if ((out.start || out.switchStage) && !out.cut) {
            const bool carry = !out.start && in.haveVoice && g_curStage >= 0
                            && PairLive(g_curL, g_curR);
            if (carry) {
                g_oldL = g_curL;
                g_oldR = g_curR;
                g_oldStage = g_curStage;
            } else {
                CutPair(g_oldL, g_oldR, 0);
                g_oldStage = -1;
            }
            const bool ok = StartStage(out.stage, out.offset, 0.0f, g_curL, g_curR);
            g_curStage = ok ? out.stage : -1;
            if (!ok) {
                TO1_LOG("downed music: NOT played (voices busy / no private data)\n");
            } else {
                TO1_LOG("downed music: %s - stage %d at %.2f s (xfade %u ms%s)\n",
                        out.why ? out.why : "?", out.stage,
                        static_cast<double>(out.offset) / in.rate,
                        static_cast<unsigned>(out.fadeMs < 0 ? 0 : out.fadeMs),
                        out.linear ? ", loop wrap" : "");
            }
        } else if (!out.cut && !out.start && !out.switchStage) {
            SetPairGains(g_curL, g_curR, out.gains);
            if (PairLive(g_oldL, g_oldR))
                SetPairGains(g_oldL, g_oldR, out.oldGains);
        }

        if (out.tail) {
            const bool ok = StartTail(in.volume > 0.0f && in.inPlay
                                      ? core::kLevel * in.volume : 0.0f);
            TO1_LOG("downed music: ending tail (%.1f s) %s\n",
                    in.rate > 0.0 ? static_cast<double>(in.tailFrames) / in.rate : 0.0,
                    ok ? "playing" : "NOT played");
            (void)ok;
        }
        if (out.cut)
            TO1_LOG("downed music: %s - fade-out %d ms\n",
                    out.why ? out.why : "?", out.fadeMs);
        if (out.hit && !out.cut && !out.start && !out.switchStage)
            TO1_LOG("downed music: bleed time cut (%s)\n", out.why ? out.why : "?");
        if (out.hold)
            music_hold::Hold(out.holdUntil);
        if (out.duckCapture)
            net::ui::death_sting::SetDuck(out.duck);
        if (out.stopTail)
            CutPair(g_tailL, g_tailR, 0);

        (void)out.why;                       // log-only in release builds
        (void)out.tailAt;
    }

    void Reset()
    {
        if (ExtOwns())
            return;
        core::Reset(g_state);
        ClearAll();
        net::ui::death_sting::SetDuck(1.0f);
        TO1_LOG("downed music: reset\n");
    }

    bool Active() noexcept
    {
        if (ExtOwns())
            return false;
        return core::Active(g_state, GetTickCount64());
    }

} // namespace net::audio::downed_music
