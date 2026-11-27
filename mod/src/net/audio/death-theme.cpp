// ported from tools/ext/death_theme.c (the death drone). All decisions live in
// death-theme-core.hpp; every engine / PCM / match read stays in this owner.

#include "death-theme.hpp"

#include "death-theme-pcm.hpp"

#include "boot.hpp"
#include "engine/flag.hpp"
#include "engine/pcm.hpp"
#include "log.hpp"
#include "net/match/match-liveness.hpp"

#include <windows.h>

#include <cstdint>

namespace net::audio::death_theme {

    namespace {

        namespace core = net::audio::death_theme::core;
        namespace sink = net::audio::death_theme::pcm;

        // Native Cut() forgets its handles, but the ext kept its slot indices and
        // left them "on" until a timed fade ended. The separate fadeUntil window
        // preserves that behaviour: no new copy starts during the fade.
        struct Instance {
            engine::pcm::Voice l;
            engine::pcm::Voice r;
            uint64_t           fadeUntil = 0;
        };

        core::State g_state;
        Instance    g_instances[core::kMaxInst];
        uint64_t    g_rng = 0;           // owner-owned ext t2r_rand state (clock here)
        bool        g_noDataLogged = false;

        bool ExtOwns() noexcept { return boot::ExtLoaded(); }

        uint32_t NextRng() noexcept
        {
            return core::NextRand(g_rng, GetTickCount64());
        }

        void ClearInstances() noexcept
        {
            for (Instance& in : g_instances) {
                engine::pcm::Invalidate(in.l);
                engine::pcm::Invalidate(in.r);
                in.fadeUntil = 0;
            }
            for (uint64_t& t : g_state.t0)
                t = 0;
        }

        bool SlotLive(int i, uint64_t now) noexcept
        {
            const Instance& in = g_instances[i];
            return in.fadeUntil > now || engine::pcm::Live(in.l) || engine::pcm::Live(in.r);
        }

        bool AnyLive(uint64_t now) noexcept
        {
            for (int i = 0; i < core::kMaxInst; ++i)
                if (SlotLive(i, now))
                    return true;
            return false;
        }

        void LogNoDataOnce()
        {
            if (g_noDataLogged)
                return;
            g_noDataLogged = true;
            TO1_LOG("death drone: no private PCM data (TO1_DEATH_DRONE_PCM off) - no voice starts\n");
        }

        // The ext skips its own two source pointers in the foreground scan. All
        // copies use the same wave, so at most two distinct pointers are ignored.
        int CollectIgnore(uint64_t now,
                          const engine::pcm::Sample* out[2 * core::kMaxInst]) noexcept
        {
            int n = 0;
            for (int i = 0; i < core::kMaxInst; ++i) {
                if (!SlotLive(i, now))
                    continue;
                const engine::pcm::Sample* channels[2] = {
                    sink::Left(g_state.cue), sink::Right(g_state.cue)
                };
                for (const engine::pcm::Sample* p : channels) {
                    if (!p)
                        continue;
                    bool seen = false;
                    for (int j = 0; j < n; ++j)
                        if (out[j] == p) { seen = true; break; }
                    if (!seen)
                        out[n++] = p;
                }
            }
            return n;
        }

        // ext dth_spawn: first free slot, music volume, hard-panned raw pair.
        bool Spawn(uint64_t now)
        {
            int slot = -1;
            for (int i = 0; i < core::kMaxInst; ++i) {
                if (!SlotLive(i, now)) { slot = i; break; }
            }
            if (slot < 0) {
                TO1_LOG("death drone: NOT played (voices busy)\n");
                return false;
            }

            const float g = g_state.lvl * engine::pcm::MusicUserVolume();
            engine::pcm::Voice l, r;
            if (!engine::pcm::PlayStereoRaw(sink::Left(g_state.cue), sink::Right(g_state.cue),
                                            sink::Frames(), g, sink::Rate(), l, r)) {
                TO1_LOG("death drone: mixer off\n");
                return false;
            }

            // Tick never spawns inside a fade, so no copy needs ext's fadeN=1 start.
            Instance& in = g_instances[slot];
            in.l = l;
            in.r = r;
            in.fadeUntil = 0;
            g_state.t0[slot] = now;
            ++g_state.count;
            if (g_state.count == 1)
                g_state.firstT0 = now;
            TO1_LOG("death drone: %s copy %d (%.0f s, gain %.3f, %llu ms after the death edge)",
                    sink::Name(g_state.cue), g_state.count,
                    static_cast<double>(sink::Frames()) / static_cast<double>(sink::Rate()),
                    static_cast<double>(g), static_cast<unsigned long long>(now - g_state.armT0));
            return true;
        }

    } // namespace

    void Arm(int stingMs, bool execCue)
    {
        if (ExtOwns())
            return;
        // ext dth_off: both flags are read (bitwise OR in C), only here.
        const bool noDrone = engine::flag::Present("t2r-no-death-drone.txt", 120);
        const bool noTheme = engine::flag::Present("t2r-no-death-theme.txt", 120);
        if (noDrone || noTheme)
            return;
        if constexpr (!sink::kHave) {
            LogNoDataOnce();
            return;
        }
        if (sink::WaveCount() <= 0 || sink::Frames() <= 0 || sink::Rate() <= 0
            || !sink::Left(0) || !sink::Right(0)) {
            LogNoDataOnce();
            return;
        }

        const uint64_t now = GetTickCount64();
        if (AnyLive(now))
            Cut(0, "new death");
        ClearInstances();
        core::Arm(g_state, now, sink::WaveCount(), NextRng());

        TO1_LOG("death drone: armed - %s starts now %s (%d ms sting), ducked under whatever plays",
                sink::Name(g_state.cue),
                execCue ? "under the execution victim cue" : "under the death sting", stingMs);
        (void)stingMs;                 // log-only when TO1_DEBUG is off
        (void)execCue;
    }

    void Tick(uint64_t nowMs, int phase, bool final)
    {
        if (ExtOwns())
            return;
        if constexpr (!sink::kHave)
            return;
        if (!g_state.armed && !AnyLive(nowMs)) {
            if (g_state.count || g_state.fading) {
                ClearInstances();      // the ext's dth_reset once the fade is over
                g_state.fading = false;
                g_state.count = 0;
            }
            return;
        }

        const core::CutReason reason =
            core::ShouldCut(phase, final, net::match::liveness::Live());
        // ext's final-before-first rule must run before the fade-out: nothing
        // played yet, so there is nothing to fade.
        if (reason == core::CutReason::kFinalFade
            && core::ShouldDropFinalBeforeFirst(g_state.armed, g_state.count)) {
            g_state.armed = false;
            TO1_LOG("death drone: dropped (the respawn fade came first)\n");
            return;
        }
        if (reason != core::CutReason::kNone) {
            Cut(static_cast<int>(core::kFadeMs), core::Why(reason));
            return;
        }
        if (g_state.fading) {
            bool window = false;
            for (int i = 0; i < core::kMaxInst; ++i)
                if (g_instances[i].fadeUntil > nowMs) window = true;
            if (!window) {             // the ext's voices have left the mixer
                ClearInstances();
                g_state.fading = false;
                g_state.count = 0;
            }
            return;                    // one fade per death, no further copies
        }

        if (g_state.armed) {
            if (g_state.count == 0) {
                if (!Spawn(nowMs)) {
                    if (nowMs - g_state.armT0 > 3000)
                        g_state.armed = false;   // ext's 3 s fail-arm
                    return;
                }
            } else if (core::LoopDue(nowMs, core::LatestT0(g_state),
                                     sink::Frames(), sink::Rate(), sink::CrossfadeMs())) {
                Spawn(nowMs);          // overlapping copies, at most kMaxInst
            }
        }

        const engine::pcm::Sample* ignore[2 * core::kMaxInst] = {};
        const int ignoreCount = CollectIgnore(nowMs, ignore);
        const float fg = engine::pcm::ForegroundPeak(ignore, ignoreCount);
        const float dt = g_state.tickT && nowMs > g_state.tickT
                       ? static_cast<float>(nowMs - g_state.tickT) / 1000.0f : 0.0f;
        g_state.tickT = nowMs;
        g_state.lvl = core::ApproachLevel(g_state.lvl, core::DuckTarget(fg), dt);
        if ((fg > 0.02f) != (g_state.fg > 0.02f))
            TO1_LOG("death drone: %s (foreground %.2f, level %.3f, %llu ms in)",
                    fg > 0.02f ? "ducked under a cue" : "foreground clear - rising to the bed",
                    static_cast<double>(fg), static_cast<double>(g_state.lvl),
                    static_cast<unsigned long long>(nowMs - g_state.armT0));
        g_state.fg = fg;

        const float g = g_state.lvl * engine::pcm::MusicUserVolume();
        for (int i = 0; i < core::kMaxInst; ++i) {
            Instance& in = g_instances[i];
            if (!SlotLive(i, nowMs))
                continue;
            engine::pcm::SetTargetGainRaw(in.l, g, 0.0f);
            engine::pcm::SetTargetGainRaw(in.r, 0.0f, g);
        }
    }

    void Cut(int fadeMs, const char* why)
    {
        if (ExtOwns())
            return;
        g_state.armed = false;
        (void)why;                     // log-only when TO1_DEBUG is off
        const uint64_t now = GetTickCount64();
        if (!AnyLive(now)) {
            ClearInstances();
            g_state.fading = false;
            g_state.count = 0;
            return;
        }
        if (g_state.fading && fadeMs > 0)
            return;                    // edge-triggered: callers re-ask every frame

        g_state.fading = fadeMs > 0;
        for (int i = 0; i < core::kMaxInst; ++i) {
            Instance& in = g_instances[i];
            if (in.fadeUntil > now)
                continue;              // ext: a voice already fading is left alone
            engine::pcm::Cut(in.l, fadeMs);
            engine::pcm::Cut(in.r, fadeMs);
            in.fadeUntil = fadeMs > 0 ? now + static_cast<uint64_t>(fadeMs) : 0;
        }
        TO1_LOG("death drone: %s - fade-out %d ms (%llu ms into it)", why ? why : "?", fadeMs,
                static_cast<unsigned long long>(now - g_state.firstT0));
        if (fadeMs <= 0) {
            ClearInstances();
            g_state.fading = false;
            g_state.count = 0;
        }
    }

    void Reset()
    {
        if (ExtOwns())
            return;
        ClearInstances();
        core::Reset(g_state);
        // g_rng / g_noDataLogged are immutable session helpers, like the ext's
        // t2r_rand seed and its log-once latch.
    }

    bool Active() noexcept
    {
        if (ExtOwns())
            return false;
        return g_state.armed || AnyLive(GetTickCount64());
    }

} // namespace net::audio::death_theme
